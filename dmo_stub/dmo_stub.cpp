#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <intrin.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "MinHook.h"

/*
 * Spice -k hook (IIDX / Whisky+Wine).
 *
 * BMSoundLib2017 CDmoSoundFxDriver CoCreateInstance()'s DirectSound FX DMOs
 * (Distortion, Chorus, Echo, ...). Wine often returns CLASS_E_CLASSNOTREG;
 * the game still calls IMediaObject::AllocateStreamingResources on a NULL
 * pointer and AVs.
 *
 * Hook ole32!CoCreateInstance: when creation fails for a DSFX CLSID (or,
 * optionally, any failure whose return address is inside bm2dx.dll), return
 * a no-op stub that implements IUnknown + IMediaObject + FX Set/GetAllParameters.
 *
 * Load:  -k dll_mods\dmo_stub\dmo_stub_64bit.dll
 * Keep dmo_stub_64bit.ini next to the DLL.
 */

#ifdef _WIN64
#define INI_NAME L"dmo_stub_64bit.ini"
#define LOG_NAME L"dmo_stub_64bit.log"
#else
#error dmo_stub is 64-bit only
#endif

enum {
    MODE_DSFX = 0,       /* stub failed known DSFX CLSIDs only */
    MODE_BM2DX_FAIL = 1, /* stub any CoCreate fail returning into bm2dx */
    MODE_DSFX_ALWAYS = 2, /* always stub DSFX even if native succeeds */
    MODE_IMO_FAIL = 3    /* stub REGDB_E_CLASSNOTREG when riid is IMediaObject (default) */
};

#ifndef REGDB_E_CLASSNOTREG
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154L)
#endif

#ifndef DMO_E_NO_MORE_ITEMS
#define DMO_E_NO_MORE_ITEMS ((HRESULT)0x80040200L)
#endif

typedef HRESULT(WINAPI *CoCreateInstance_fn)(
    REFCLSID rclsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *ppv);

static HMODULE g_self;
static CoCreateInstance_fn g_orig_cci;
static volatile LONG g_ready;
static int g_log_enabled = 1;
static int g_enabled = 1;
static int g_mode = MODE_DSFX;
static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;
static volatile LONG g_stub_count;
static volatile LONG g_pass_count;
static volatile LONG g_fail_passthru;
static volatile LONG g_cs_ready;
static volatile LONG g_hooks_live;

/* IIDX embeds DSFX CLSIDs that often differ from MSDN GUID_DSFX_* Data4. */
static const CLSID kDsfxClsids[] = {
    /* Chorus (MS / IIDX) */
    {0xEFE6629C, 0x81F7, 0x4281, {0xBD, 0x91, 0xC9, 0xD6, 0x04, 0xA9, 0x5A, 0xF6}},
    /* Compressor (IIDX Data4[5]=0xC3) */
    {0xEF011F79, 0x4000, 0x406D, {0x87, 0xAF, 0xBF, 0xFB, 0x3F, 0xC3, 0x9D, 0x57}},
    {0xEF011F79, 0x4000, 0x406D, {0x87, 0xAF, 0xBF, 0xFB, 0x3F, 0xCD, 0x9D, 0x57}},
    /* Distortion */
    {0xEF114C90, 0xCD1D, 0x484E, {0x96, 0xE5, 0x09, 0xCF, 0xAF, 0x91, 0x2A, 0x21}},
    /* Echo — IIDX (sub_18048C590), not MSDN A655-339785D71367 */
    {0xEF3E932C, 0xD40B, 0x4F51, {0x8C, 0xCF, 0x3F, 0x98, 0xF1, 0xB2, 0x9D, 0x5D}},
    {0xEF3E932C, 0xD40B, 0x4F51, {0xA6, 0x55, 0x33, 0x97, 0x85, 0xD7, 0x13, 0x67}},
    /* Flanger — IIDX EFCA3D92... (sub_18048C670), plus MSDN */
    {0xEFCA3D92, 0xDFD8, 0x4672, {0xA6, 0x03, 0x74, 0x20, 0x89, 0x4B, 0xAD, 0x98}},
    {0xEFCAE6E4, 0xD3B0, 0x11D2, {0x95, 0xA5, 0x00, 0xC0, 0x4F, 0x8E, 0xE8, 0x4E}},
    /* Gargle — IIDX Data3=4B91 (sub_18048C750), plus MSDN */
    {0xDAFD8210, 0x5711, 0x4B91, {0x9F, 0xE3, 0xF7, 0x5B, 0x7A, 0xE2, 0x79, 0xBF}},
    {0xDAFD8210, 0x5711, 0x4B65, {0x85, 0x9D, 0xE9, 0xB7, 0x7C, 0xB7, 0x50, 0x8F}},
    /* ParamEq */
    {0xEF985E71, 0xD5C7, 0x42D4, {0xBA, 0x4D, 0x2D, 0x07, 0x3E, 0x2E, 0x96, 0xF4}},
    /* I3DL2 / Waves Reverb */
    {0x120CED89, 0x3BF4, 0x4173, {0xA1, 0x32, 0x3C, 0xB4, 0x06, 0xCF, 0x32, 0x31}},
    {0x87FC0268, 0x9A55, 0x4360, {0x95, 0xAA, 0x00, 0x4A, 0x1D, 0x9D, 0xE2, 0x6C}},
};

static const IID IID_IMediaObject_Local =
    {0xd8ad0f58, 0x5494, 0x4102, {0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4}};

struct StubFx;
struct StubDmo {
    const void *imo_vtbl;
    LONG refs;
    CLSID clsid;
    StubFx *fx;
};

struct StubFx {
    const void *fx_vtbl;
    StubDmo *parent;
};

static void log_msg(const char *fmt, ...);
static void fmt_guid(char *buf, size_t n, const GUID *g);

static int guid_eq(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static int is_dsfx_clsid(REFCLSID clsid)
{
    size_t i;
    for (i = 0; i < sizeof(kDsfxClsids) / sizeof(kDsfxClsids[0]); i++) {
        if (guid_eq(&kDsfxClsids[i], &clsid))
            return 1;
    }
    return 0;
}

/* IIDX often mangles Data4; Data1 alone still identifies the DSFX family. */
static int looks_like_dsfx(REFCLSID clsid)
{
    switch (clsid.Data1) {
    case 0xEFE6629C: /* Chorus */
    case 0xEF011F79: /* Compressor */
    case 0xEF114C90: /* Distortion */
    case 0xEF3E932C: /* Echo */
    case 0xEFCA3D92: /* Flanger (IIDX) */
    case 0xEFCAE6E4: /* Flanger (MS) */
    case 0xDAFD8210: /* Gargle */
    case 0xEF985E71: /* ParamEq */
    case 0x120CED89: /* I3DL2 */
    case 0x87FC0268: /* Waves Reverb */
        return 1;
    default:
        return is_dsfx_clsid(clsid);
    }
}

static int return_addr_in_module(void *ret, const wchar_t *modname)
{
    HMODULE mod;
    MODULEINFO mi;
    BYTE *base;
    SIZE_T size;

    mod = GetModuleHandleW(modname);
    if (!mod || !ret)
        return 0;
    memset(&mi, 0, sizeof(mi));
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
        return 0;
    base = (BYTE *)mi.lpBaseOfDll;
    size = mi.SizeOfImage;
    return (BYTE *)ret >= base && (BYTE *)ret < base + size;
}

static HRESULT STDMETHODCALLTYPE stub_qi(StubDmo *self, REFIID riid, void **ppv);
static ULONG STDMETHODCALLTYPE stub_addref(StubDmo *self);
static ULONG STDMETHODCALLTYPE stub_release(StubDmo *self);

static HRESULT STDMETHODCALLTYPE fx_qi(StubFx *self, REFIID riid, void **ppv)
{
    if (!self || !self->parent)
        return E_POINTER;
    return stub_qi(self->parent, riid, ppv);
}

static ULONG STDMETHODCALLTYPE fx_addref(StubFx *self)
{
    if (!self || !self->parent)
        return 0;
    return stub_addref(self->parent);
}

static ULONG STDMETHODCALLTYPE fx_release(StubFx *self)
{
    if (!self || !self->parent)
        return 0;
    return stub_release(self->parent);
}

static HRESULT STDMETHODCALLTYPE fx_set_all(StubFx *self, void *params)
{
    (void)self;
    (void)params;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fx_get_all(StubFx *self, void *params)
{
    /* No-op: DSFX GetAllParameters struct sizes differ; never memset a fixed size. */
    (void)self;
    (void)params;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_get_stream_count(StubDmo *self, DWORD *in_c, DWORD *out_c)
{
    (void)self;
    if (in_c)
        *in_c = 1;
    if (out_c)
        *out_c = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_ok0(StubDmo *self)
{
    (void)self;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_ok1(StubDmo *self, DWORD a)
{
    (void)self;
    (void)a;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_ok2(StubDmo *self, DWORD a, void *b)
{
    (void)self;
    (void)a;
    (void)b;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_get_type(StubDmo *self, DWORD stream, DWORD type_idx, void *mt)
{
    (void)self;
    (void)stream;
    (void)type_idx;
    (void)mt;
    return DMO_E_NO_MORE_ITEMS;
}

static HRESULT STDMETHODCALLTYPE imo_set_type(StubDmo *self, DWORD stream, const void *mt, DWORD flags)
{
    (void)self;
    (void)stream;
    (void)mt;
    (void)flags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_get_input_status(StubDmo *self, DWORD stream, DWORD *flags)
{
    (void)self;
    (void)stream;
    if (flags)
        *flags = 0x1; /* DMO_INPUT_STATUSF_ACCEPT_DATA */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_process_input(
    StubDmo *self, DWORD stream, void *buf, DWORD flags, LONGLONG ts, LONGLONG tl)
{
    (void)self;
    (void)stream;
    (void)buf;
    (void)flags;
    (void)ts;
    (void)tl;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE imo_process_output(
    StubDmo *self, DWORD flags, DWORD count, void *out, DWORD *status)
{
    (void)self;
    (void)flags;
    (void)count;
    (void)out;
    if (status)
        *status = 0;
    return S_OK;
}

/* IUnknown + IMediaObject; slot 18 (+0x90) = AllocateStreamingResources. */
static const void *g_imo_vtbl[] = {
    (void *)stub_qi,
    (void *)stub_addref,
    (void *)stub_release,
    (void *)imo_get_stream_count,
    (void *)imo_ok2,
    (void *)imo_ok2,
    (void *)imo_get_type,
    (void *)imo_get_type,
    (void *)imo_set_type,
    (void *)imo_set_type,
    (void *)imo_ok2,
    (void *)imo_ok2,
    (void *)imo_ok2,
    (void *)imo_ok2,
    (void *)imo_ok2,
    (void *)imo_ok1,
    (void *)imo_ok0,
    (void *)imo_ok1,
    (void *)imo_ok0, /* AllocateStreamingResources */
    (void *)imo_ok0,
    (void *)imo_get_input_status,
    (void *)imo_process_input,
    (void *)imo_process_output,
    (void *)imo_ok1,
};

static const void *g_fx_vtbl[] = {
    (void *)fx_qi,
    (void *)fx_addref,
    (void *)fx_release,
    (void *)fx_set_all,
    (void *)fx_get_all,
};

static ULONG STDMETHODCALLTYPE stub_addref(StubDmo *self)
{
    if (!self)
        return 0;
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE stub_release(StubDmo *self)
{
    LONG n;
    if (!self)
        return 0;
    n = InterlockedDecrement(&self->refs);
    if (n == 0) {
        if (self->fx)
            HeapFree(GetProcessHeap(), 0, self->fx);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE stub_qi(StubDmo *self, REFIID riid, void **ppv)
{
    if (!self || !ppv)
        return E_POINTER;
    *ppv = NULL;
    if (guid_eq(&riid, &IID_IUnknown) || guid_eq(&riid, &IID_IMediaObject_Local)) {
        *ppv = self;
        stub_addref(self);
        return S_OK;
    }
    /* FX face allocated eagerly in create_stub (avoids QI race / leak). */
    if (!self->fx)
        return E_OUTOFMEMORY;
    *ppv = self->fx;
    stub_addref(self);
    return S_OK;
}

static HRESULT create_stub(REFCLSID clsid, REFIID riid, LPVOID *ppv)
{
    StubDmo *s;
    StubFx *fx;
    HRESULT hr;

    if (!ppv)
        return E_POINTER;

    s = (StubDmo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StubDmo));
    if (!s)
        return E_OUTOFMEMORY;
    fx = (StubFx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StubFx));
    if (!fx) {
        HeapFree(GetProcessHeap(), 0, s);
        return E_OUTOFMEMORY;
    }
    fx->fx_vtbl = g_fx_vtbl;
    fx->parent = s;

    s->imo_vtbl = g_imo_vtbl;
    s->refs = 1;
    s->clsid = clsid;
    s->fx = fx;

    hr = stub_qi(s, riid, ppv);
    stub_release(s);
    if (SUCCEEDED(hr))
        InterlockedIncrement(&g_stub_count);
    return hr;
}

static void fmt_guid(char *buf, size_t n, const GUID *g)
{
    _snprintf_s(buf, n, _TRUNCATE,
                "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                g->Data1, g->Data2, g->Data3,
                g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
                g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static int should_stub_fail(HRESULT native_hr, int dsfx, int from_bm2dx, int want_imo)
{
    if (g_mode == MODE_DSFX)
        return dsfx;
    if (g_mode == MODE_BM2DX_FAIL)
        return from_bm2dx;
    if (g_mode == MODE_DSFX_ALWAYS)
        return dsfx;
    if (g_mode == MODE_IMO_FAIL) {
        /* Ctor CoCreate uses IID_IUnknown; later QI asks for IMediaObject.
         * Stub CLASSNOTREG for DSFX family, or when IMediaObject was requested. */
        if (native_hr == REGDB_E_CLASSNOTREG && (dsfx || want_imo))
            return 1;
        return dsfx;
    }
    return 0;
}

static HRESULT WINAPI hook_CoCreateInstance(
    REFCLSID rclsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *ppv)
{
    HRESULT hr;
    int dsfx;
    int from_bm2dx;
    int want_imo;
    void *ret = _ReturnAddress();
    char gbuf[64];
    char ibuf[64];

    if (!g_orig_cci)
        return E_UNEXPECTED;
    hr = g_orig_cci(rclsid, outer, ctx, riid, ppv);
    if (!g_ready || !g_enabled)
        return hr;

    /* Aggregated creates must not be replaced with a non-aggregated stub. */
    if (outer != NULL)
        return hr;

    dsfx = looks_like_dsfx(rclsid);
    from_bm2dx = return_addr_in_module(ret, L"bm2dx.dll");
    want_imo = guid_eq(&riid, &IID_IMediaObject_Local);
    fmt_guid(gbuf, sizeof(gbuf), &rclsid);
    fmt_guid(ibuf, sizeof(ibuf), &riid);

    if (SUCCEEDED(hr)) {
        InterlockedIncrement(&g_pass_count);
        if (dsfx && g_mode == MODE_DSFX_ALWAYS && ppv) {
            LPVOID stub_ppv = NULL;
            HRESULT shr = create_stub(rclsid, riid, &stub_ppv);
            if (SUCCEEDED(shr) && stub_ppv) {
                if (*ppv)
                    ((IUnknown *)(*ppv))->Release();
                *ppv = stub_ppv;
                hr = S_OK;
            } else {
                if (stub_ppv)
                    ((IUnknown *)stub_ppv)->Release();
                /* Keep native *ppv on stub failure. */
            }
            log_msg("DSFX_ALWAYS stub clsid=%s hr=0x%08X kept_native=%d",
                    gbuf, (unsigned)shr, SUCCEEDED(shr) ? 0 : 1);
            return hr;
        }
        if (dsfx)
            log_msg("native OK clsid=%s", gbuf);
        return hr;
    }

    if (!should_stub_fail(hr, dsfx, from_bm2dx, want_imo)) {
        InterlockedIncrement(&g_fail_passthru);
        log_msg("PASS_FAIL clsid=%s iid=%s hr=0x%08X dsfx=%d imo=%d bm2dx=%d mode=%d",
                gbuf, ibuf, (unsigned)hr, dsfx, want_imo, from_bm2dx, g_mode);
        return hr;
    }

    if (!ppv)
        return hr;

    {
        HRESULT shr = create_stub(rclsid, riid, ppv);
        log_msg("STUB clsid=%s iid=%s native_hr=0x%08X stub_hr=0x%08X dsfx=%d imo=%d bm2dx=%d mode=%d",
                gbuf, ibuf, (unsigned)hr, (unsigned)shr, dsfx, want_imo, from_bm2dx, g_mode);
        return SUCCEEDED(shr) ? S_OK : hr;
    }
}

static void log_msg(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;
    if (!g_log_enabled || !g_cs_ready)
        return;
    EnterCriticalSection(&g_log_cs);
    if (!g_log) {
        LeaveCriticalSection(&g_log_cs);
        return;
    }
    GetLocalTime(&st);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static void init_paths(void)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir[0] = 0;
    if (n == 0 || n >= MAX_PATH)
        return;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    *slash = 0;
    wcsncpy_s(g_dir, path, _TRUNCATE);
}

static void open_log(void)
{
    wchar_t log_path[MAX_PATH];
    if (!g_log_enabled || !g_dir[0])
        return;
    _snwprintf_s(log_path, _TRUNCATE, L"%s\\" LOG_NAME, g_dir);
    _wfopen_s(&g_log, log_path, L"a");
}

static void ini_path(wchar_t *ini, size_t n)
{
    _snwprintf_s(ini, n, _TRUNCATE, L"%s\\" INI_NAME, g_dir);
}

static int ini_int(const wchar_t *key, int def)
{
    wchar_t ini[MAX_PATH];
    if (!g_dir[0])
        return def;
    ini_path(ini, MAX_PATH);
    return (int)GetPrivateProfileIntW(L"dmo_stub", key, def, ini);
}

static void load_ini(void)
{
    wchar_t ini[MAX_PATH];
    wchar_t buf[64];
    g_log_enabled = ini_int(L"log", 1);
    g_enabled = ini_int(L"enabled", 1);
    g_mode = MODE_IMO_FAIL;
    if (!g_dir[0])
        return;
    ini_path(ini, MAX_PATH);
    GetPrivateProfileStringW(L"dmo_stub", L"mode", L"imo_fail", buf, 64, ini);
    if (_wcsicmp(buf, L"bm2dx_fail") == 0)
        g_mode = MODE_BM2DX_FAIL;
    else if (_wcsicmp(buf, L"dsfx_always") == 0)
        g_mode = MODE_DSFX_ALWAYS;
    else if (_wcsicmp(buf, L"dsfx") == 0)
        g_mode = MODE_DSFX;
    else
        g_mode = MODE_IMO_FAIL;
}

static void shutdown_hooks(void)
{
    InterlockedExchange(&g_ready, 0);
    if (InterlockedExchange(&g_hooks_live, 0)) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_orig_cci = NULL;
    }
    if (g_cs_ready) {
        EnterCriticalSection(&g_log_cs);
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
        LeaveCriticalSection(&g_log_cs);
        DeleteCriticalSection(&g_log_cs);
        InterlockedExchange(&g_cs_ready, 0);
    }
}

static DWORD WINAPI init_thread(LPVOID)
{
    MH_STATUS st;
    HMODULE ole;
    FARPROC p;

    InitializeCriticalSection(&g_log_cs);
    InterlockedExchange(&g_cs_ready, 1);
    init_paths();
    load_ini();
    open_log();

    log_msg("dmo_stub starting enabled=%d mode=%d (DMO-only)", g_enabled, g_mode);

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    ole = LoadLibraryW(L"ole32.dll");
    if (!ole) {
        log_msg("LoadLibrary ole32 failed");
        return 1;
    }
    p = GetProcAddress(ole, "CoCreateInstance");
    if (!p) {
        log_msg("GetProcAddress CoCreateInstance failed");
        return 1;
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }

    st = MH_CreateHook((LPVOID)p, (LPVOID)hook_CoCreateInstance, (LPVOID *)&g_orig_cci);
    if (st != MH_OK) {
        log_msg("MH_CreateHook CoCreateInstance: %s", MH_StatusToString(st));
        MH_Uninitialize();
        return 1;
    }

    /* Ready before enable so the first hooked calls can stub. */
    InterlockedExchange(&g_ready, 1);
    InterlockedExchange(&g_hooks_live, 1);

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        InterlockedExchange(&g_ready, 0);
        InterlockedExchange(&g_hooks_live, 0);
        MH_Uninitialize();
        g_orig_cci = NULL;
        return 1;
    }

    log_msg("hooks enabled (ole32!CoCreateInstance)");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        {
            HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
            if (t)
                CloseHandle(t);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        shutdown_hooks();
    }
    return TRUE;
}
