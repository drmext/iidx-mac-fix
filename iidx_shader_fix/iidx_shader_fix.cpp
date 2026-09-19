#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "shaders_bytecode.h"

/*
 * Spice -k hook for Beatmania IIDX on Wine (LDJ-2025091200).
 *
 * 1) Wine's D3DXCompileShader / AssembleShader fail on AFP MRT/text shaders
 *    embedded in bm2dx.dll. On known (src_len, profile, entry) matches, return
 *    fxc-precompiled bytecode from shaders_bytecode.h.
 * 2) CreateTexture with levels=0 on large non-RT atlases (8192 font atlas)
 *    builds a mip chain Wine fails to update — force levels=1. UpdateTexture
 *    falls back to D3DXLoadSurfaceFromSurface on failure.
 *
 * Ship Microsoft d3dx9_43.dll next to the game (Wine's builtin is flaky).
 * No bm2dx RVA hooks — portable across builds that share the same HLSL keys.
 */

#define INI_NAME L"iidx_shader_fix_64bit.ini"
#define LOG_NAME L"iidx_shader_fix_64bit.log"

#ifndef D3DX_DEFAULT
#define D3DX_DEFAULT ((UINT)-1)
#endif

/* Minimal ID3DXBuffer (d3dx9shader.h) — avoid linking d3dx9.lib */
MIDL_INTERFACE("8BA5FB08-5195-40e2-AC58-0D989C3A0102")
ID3DXBuffer : public IUnknown
{
public:
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize(void) = 0;
};

typedef struct _D3DXMACRO {
    LPCSTR Name;
    LPCSTR Definition;
} D3DXMACRO;

typedef interface ID3DXInclude ID3DXInclude;
typedef interface ID3DXConstantTable ID3DXConstantTable;

typedef HRESULT(WINAPI *D3DXCompileShader_t)(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable);

typedef HRESULT(WINAPI *D3DXGetShaderConstantTable_t)(
    CONST DWORD *pFunction, ID3DXConstantTable **ppConstantTable);

typedef HRESULT(WINAPI *D3DXAssembleShader_t)(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs);

typedef HRESULT(WINAPI *D3DXCreateTextureFromFileInMemoryEx_t)(
    void *pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    UINT Width,
    UINT Height,
    UINT MipLevels,
    DWORD Usage,
    DWORD Format,
    DWORD Pool,
    DWORD Filter,
    DWORD MipFilter,
    DWORD ColorKey,
    void *pSrcInfo,
    void *pPalette,
    void **ppTexture);

typedef HRESULT(WINAPI *D3DXLoadSurfaceFromSurface_t)(
    void *pDestSurface, const void *pDestPalette, const RECT *pDestRect,
    void *pSrcSurface, const void *pSrcPalette, const RECT *pSrcRect,
    DWORD Filter, DWORD ColorKey);

#define D3D9_VT_CREATETEXTURE 23
#define D3D9_VT_UPDATETEXTURE 30
#define D3DUSAGE_RENDERTARGET 0x00000001L
#define TEX9_VT_GETSURFACELEVEL 18
#define D3D9EX_VT_CREATEDEVICEEX 20

typedef HRESULT(STDMETHODCALLTYPE *CreateTexture_fn)(
    void *self, UINT width, UINT height, UINT levels, DWORD usage, DWORD format,
    DWORD pool, void **out, void *shared);
typedef HRESULT(STDMETHODCALLTYPE *TexGetSurfaceLevel_fn)(
    void *tex, UINT level, void **out_surf);
typedef HRESULT(STDMETHODCALLTYPE *UpdateTexture_fn)(
    void *self, void *src_tex, void *dst_tex);
typedef HRESULT(STDMETHODCALLTYPE *CreateDeviceEx_fn)(
    void *self, UINT adapter, DWORD type, HWND hwnd, DWORD flags,
    void *pp, void *fs, void **out_dev);
typedef HRESULT(WINAPI *Direct3DCreate9Ex_fn)(UINT sdk, void **out);

static HMODULE g_self;
static int g_log_enabled = 1;
static int g_log_verbose = 0;
static int g_enabled = 1;
static int g_fallback = 1;
static int g_force_fxc = 1;

static CRITICAL_SECTION g_log_cs;
static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static D3DXCompileShader_t g_orig_compile;
static D3DXGetShaderConstantTable_t g_get_ctable;
static D3DXAssembleShader_t g_orig_assemble;
static D3DXCreateTextureFromFileInMemoryEx_t g_orig_tex_mem;
static CreateTexture_fn g_orig_create_tex;
static UpdateTexture_fn g_orig_update_tex;
static CreateDeviceEx_fn g_orig_create_device_ex;
static D3DXLoadSurfaceFromSurface_t g_d3dx_load_surf;
static volatile LONG g_d3d_hooks_once;
static volatile LONG g_create_tex_logs;
static volatile LONG g_update_tex_logs;
static volatile LONG g_cs_ready;
static volatile LONG g_hooks_live;

/* g_d3d_hooks_once: 0=idle, 1=in progress, 2=done */
enum { D3D_HOOK_IDLE = 0, D3D_HOOK_BUSY = 1, D3D_HOOK_DONE = 2 };

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
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir[0] = 0;
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    *slash = 0;
    wcsncpy_s(g_dir, path, _TRUNCATE);
}

static void open_log(void)
{
    if (!g_log_enabled || !g_dir[0])
        return;
    wchar_t log_path[MAX_PATH];
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
    return (int)GetPrivateProfileIntW(L"iidx_shader_fix", key, def, ini);
}

static const IidxShaderBlob *find_blob(LPCSTR src, UINT src_len, LPCSTR entry, LPCSTR profile)
{
    int i;
    (void)src;
    if (!entry || !profile)
        return NULL;
    for (i = 0; i < IIDX_SHADER_BLOB_COUNT; i++) {
        if (g_iidx_shader_blobs[i].src_len == src_len &&
            strcmp(g_iidx_shader_blobs[i].profile, profile) == 0 &&
            strcmp(g_iidx_shader_blobs[i].entry, entry) == 0)
            return &g_iidx_shader_blobs[i];
    }
    return NULL;
}

/* ---- SEH wrappers (keep C++ dtors out of these frames) ---- */

static HRESULT safe_orig_compile(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable)
{
    HRESULT hr = E_FAIL;
    if (!g_orig_compile)
        return E_FAIL;
    if (ppShader)
        *ppShader = NULL;
    if (ppErrorMsgs)
        *ppErrorMsgs = NULL;
    if (ppConstantTable)
        *ppConstantTable = NULL;
    __try {
        hr = g_orig_compile(
            pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags,
            ppShader, ppErrorMsgs, ppConstantTable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("D3DXCompileShader exception code=0x%08X", (unsigned)GetExceptionCode());
        if (ppShader && *ppShader) {
            (*ppShader)->Release();
            *ppShader = NULL;
        }
        if (ppErrorMsgs && *ppErrorMsgs) {
            (*ppErrorMsgs)->Release();
            *ppErrorMsgs = NULL;
        }
        if (ppConstantTable && *ppConstantTable) {
            ((IUnknown *)*ppConstantTable)->Release();
            *ppConstantTable = NULL;
        }
        hr = E_FAIL;
    }
    return hr;
}

static HRESULT safe_orig_assemble(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs)
{
    HRESULT hr = E_FAIL;
    if (!g_orig_assemble)
        return E_FAIL;
    if (ppShader)
        *ppShader = NULL;
    if (ppErrorMsgs)
        *ppErrorMsgs = NULL;
    __try {
        hr = g_orig_assemble(pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("D3DXAssembleShader exception code=0x%08X", (unsigned)GetExceptionCode());
        if (ppShader && *ppShader) {
            (*ppShader)->Release();
            *ppShader = NULL;
        }
        if (ppErrorMsgs && *ppErrorMsgs) {
            (*ppErrorMsgs)->Release();
            *ppErrorMsgs = NULL;
        }
        hr = E_FAIL;
    }
    return hr;
}

static HRESULT safe_get_ctable(CONST DWORD *pFunction, ID3DXConstantTable **ppConstantTable)
{
    HRESULT hr = E_FAIL;
    if (!g_get_ctable || !ppConstantTable)
        return E_FAIL;
    *ppConstantTable = NULL;
    __try {
        hr = g_get_ctable(pFunction, ppConstantTable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_msg("D3DXGetShaderConstantTable exception code=0x%08X", (unsigned)GetExceptionCode());
        *ppConstantTable = NULL;
        hr = E_FAIL;
    }
    return hr;
}

/* ---- synthetic ID3DXBuffer ---- */

class SynthBuffer : public ID3DXBuffer {
    LONG m_refs;
    uint8_t *m_data;
    DWORD m_size;

public:
    SynthBuffer(const uint8_t *data, DWORD size)
        : m_refs(1), m_data(NULL), m_size(size)
    {
        m_data = (uint8_t *)malloc(size ? size : 1);
        if (m_data && size)
            memcpy(m_data, data, size);
    }

    ~SynthBuffer()
    {
        free(m_data);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv)
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ID3DXBuffer)) {
            *ppv = static_cast<ID3DXBuffer *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef(void)
    {
        return (ULONG)InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release(void)
    {
        LONG n = InterlockedDecrement(&m_refs);
        if (n == 0)
            delete this;
        return (ULONG)n;
    }

    LPVOID STDMETHODCALLTYPE GetBufferPointer(void)
    {
        return m_data;
    }

    DWORD STDMETHODCALLTYPE GetBufferSize(void)
    {
        return m_size;
    }
};

static HRESULT make_synth_buffer(const IidxShaderBlob *blob, ID3DXBuffer **out)
{
    SynthBuffer *buf;
    if (!out || !blob || !blob->data || !blob->size)
        return E_FAIL;
    buf = new (std::nothrow) SynthBuffer(blob->data, blob->size);
    if (!buf || !buf->GetBufferPointer()) {
        delete buf;
        return E_OUTOFMEMORY;
    }
    *out = buf;
    return S_OK;
}

static HRESULT apply_blob(
    const IidxShaderBlob *blob,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable,
    const char *tag)
{
    HRESULT syn;
    ID3DXBuffer *wine_shader = NULL;
    ID3DXBuffer *wine_err = NULL;
    ID3DXConstantTable *wine_ct = NULL;
    ID3DXBuffer *new_shader = NULL;

    if (ppShader) {
        wine_shader = *ppShader;
        *ppShader = NULL;
    }
    if (ppErrorMsgs) {
        wine_err = *ppErrorMsgs;
        *ppErrorMsgs = NULL;
    }
    if (ppConstantTable) {
        wine_ct = *ppConstantTable;
        *ppConstantTable = NULL;
    }

    syn = make_synth_buffer(blob, &new_shader);
    if (FAILED(syn) || !new_shader) {
        if (ppShader)
            *ppShader = wine_shader;
        if (ppErrorMsgs)
            *ppErrorMsgs = wine_err;
        if (ppConstantTable)
            *ppConstantTable = wine_ct;
        log_msg("  %s FAIL name=%s syn=0x%08X", tag, blob->name, (unsigned)syn);
        return FAILED(syn) ? syn : E_FAIL;
    }

    if (wine_shader)
        wine_shader->Release();
    if (wine_err)
        wine_err->Release();
    if (wine_ct)
        ((IUnknown *)wine_ct)->Release();

    if (ppShader)
        *ppShader = new_shader;
    else
        new_shader->Release();

    if (ppConstantTable && g_get_ctable && ppShader && *ppShader) {
        HRESULT ct = safe_get_ctable(
            (CONST DWORD *)(*ppShader)->GetBufferPointer(), ppConstantTable);
        if (g_log_verbose || FAILED(ct)) {
            log_msg(
                "  %s OK name=%s cso=%u ctab=0x%08X",
                tag, blob->name, (unsigned)blob->size, (unsigned)ct);
        }
    } else if (g_log_verbose) {
        log_msg("  %s OK name=%s cso=%u", tag, blob->name, (unsigned)blob->size);
    }
    return S_OK;
}

static HRESULT WINAPI detour_compile(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    LPCSTR pFunctionName,
    LPCSTR pProfile,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs,
    ID3DXConstantTable **ppConstantTable)
{
    HRESULT wine_hr;
    HRESULT hr;
    const IidxShaderBlob *blob;

    hr = safe_orig_compile(
        pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags,
        ppShader, ppErrorMsgs, ppConstantTable);
    wine_hr = hr;

    blob = find_blob(pSrcData, SrcDataLen, pFunctionName, pProfile);
    if (g_log_verbose || (blob && g_fallback) || FAILED(hr)) {
        log_msg(
            "D3DXCompileShader profile=%s entry=%s src_len=%u hr=0x%08X blob=%s",
            pProfile ? pProfile : "(null)",
            pFunctionName ? pFunctionName : "(null)",
            (unsigned)SrcDataLen,
            (unsigned)hr,
            blob ? blob->name : "-");
    }

    if (g_fallback && blob && ppShader) {
        if (FAILED(hr)) {
            if (SUCCEEDED(apply_blob(blob, ppShader, ppErrorMsgs, ppConstantTable, "fallback")))
                hr = S_OK;
        } else if (g_force_fxc) {
            /* apply_blob stashes/restores wine outs; releases them only on success. */
            if (SUCCEEDED(apply_blob(blob, ppShader, ppErrorMsgs, ppConstantTable, "force_fxc")))
                hr = S_OK;
        }
    }

    (void)wine_hr;
    return hr;
}

/* ---- D3DXAssembleShader (boot-text VS/PS 1.1) ---- */

static const AsmShaderBlob *find_asm_blob(LPCSTR src, UINT src_len)
{
    size_t i;
    (void)src;
    for (i = 0; i < ASM_BLOB_COUNT; i++) {
        if (g_asm_blobs[i].src_len == src_len)
            return &g_asm_blobs[i];
    }
    return NULL;
}

static HRESULT WINAPI detour_assemble(
    LPCSTR pSrcData,
    UINT SrcDataLen,
    CONST D3DXMACRO *pDefines,
    ID3DXInclude *pInclude,
    DWORD Flags,
    ID3DXBuffer **ppShader,
    ID3DXBuffer **ppErrorMsgs)
{
    HRESULT hr;
    const AsmShaderBlob *blob;
    IidxShaderBlob syn;

    if (!g_orig_assemble)
        return E_FAIL;
    hr = safe_orig_assemble(pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs);
    blob = find_asm_blob(pSrcData, SrcDataLen);

    if (g_log_verbose || (blob && FAILED(hr))) {
        log_msg(
            "D3DXAssembleShader src_len=%u hr=0x%08X blob=%s",
            (unsigned)SrcDataLen, (unsigned)hr, blob ? blob->tag : "-");
    }

    if (g_fallback && blob && ppShader && FAILED(hr)) {
        syn.key = 0;
        syn.src_len = blob->src_len;
        syn.profile = "";
        syn.entry = "";
        syn.name = blob->tag;
        syn.data = blob->data;
        syn.size = blob->size;
        if (SUCCEEDED(apply_blob(&syn, ppShader, ppErrorMsgs, NULL, "asm_fallback")))
            hr = S_OK;
    }
    /* Do NOT force-replace a successful Wine assemble: CreateVertexShader
     * HRESULT is ignored by the game; a bad replacement silently kills boot text. */

    return hr;
}

/* ---- D3D9 CreateTexture / UpdateTexture ---- */

static HRESULT STDMETHODCALLTYPE detour_create_tex(
    void *self, UINT width, UINT height, UINT levels, DWORD usage, DWORD format,
    DWORD pool, void **out, void *shared)
{
    UINT levels_in = levels;
    int levels_forced = 0;
    HRESULT hr;
    if (!g_orig_create_tex)
        return E_FAIL;
    /* 8192 atlas is created as SYSTEMMEM+DEFAULT pair (UpdateTexture).
     * levels=0 builds a full mip chain Wine often fails to update — force 1. */
    if ((usage & D3DUSAGE_RENDERTARGET) == 0 &&
        (width >= 2048u || height >= 2048u) && levels == 0) {
        levels = 1;
        levels_forced = 1;
    }
    hr = g_orig_create_tex(self, width, height, levels, usage, format, pool, out, shared);
    if (levels_forced || FAILED(hr)) {
        LONG n = InterlockedIncrement(&g_create_tex_logs);
        if (n <= 24) {
            log_msg("CreateTexture %ux%u lv=%u->%u usage=0x%X fmt=0x%X pool=%u "
                    "lv_forced=%d hr=0x%08X",
                    width, height, levels_in, levels, (unsigned)usage,
                    (unsigned)format, (unsigned)pool, levels_forced, (unsigned)hr);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE detour_update_tex(void *self, void *src_tex, void *dst_tex)
{
    HRESULT hr;
    if (!g_orig_update_tex)
        return E_FAIL;
    hr = g_orig_update_tex(self, src_tex, dst_tex);
    if (FAILED(hr) && src_tex && dst_tex && g_d3dx_load_surf) {
        void **sv = *(void ***)src_tex;
        void **dv = *(void ***)dst_tex;
        void *ss = NULL, *ds = NULL;
        HRESULT hrs = E_FAIL, hrd = E_FAIL;
        if (sv && dv && sv[TEX9_VT_GETSURFACELEVEL] && dv[TEX9_VT_GETSURFACELEVEL]) {
            hrs = ((TexGetSurfaceLevel_fn)sv[TEX9_VT_GETSURFACELEVEL])(src_tex, 0, &ss);
            hrd = ((TexGetSurfaceLevel_fn)dv[TEX9_VT_GETSURFACELEVEL])(dst_tex, 0, &ds);
        }
        if (SUCCEEDED(hrs) && SUCCEEDED(hrd) && ss && ds) {
            HRESULT hr2 = g_d3dx_load_surf(ds, NULL, NULL, ss, NULL, NULL, 1 /*D3DX_FILTER_NONE*/, 0);
            log_msg("UpdateTexture hr=0x%08X -> D3DXLoadSurfaceFromSurface hr=0x%08X",
                    (unsigned)hr, (unsigned)hr2);
            if (SUCCEEDED(hr2))
                hr = hr2;
        }
        if (ss)
            ((IUnknown *)ss)->Release();
        if (ds)
            ((IUnknown *)ds)->Release();
    }
    if (FAILED(hr)) {
        LONG n = InterlockedIncrement(&g_update_tex_logs);
        if (n <= 16) {
            log_msg("UpdateTexture src=%p dst=%p hr=0x%08X", src_tex, dst_tex,
                    (unsigned)hr);
        }
    }
    return hr;
}

static void try_hook_d3d_device(void *pDevice)
{
    void **vtbl;
    MH_STATUS st_ct = MH_ERROR_NOT_CREATED;
    MH_STATUS st_ut = MH_ERROR_NOT_CREATED;
    MH_STATUS st_en;
    int created = 0;

    if (!pDevice)
        return;
    if (InterlockedCompareExchange(&g_d3d_hooks_once, D3D_HOOK_BUSY, D3D_HOOK_IDLE) != D3D_HOOK_IDLE)
        return;

    vtbl = *(void ***)pDevice;
    if (!vtbl) {
        InterlockedExchange(&g_d3d_hooks_once, D3D_HOOK_IDLE);
        return;
    }

    st_ct = MH_CreateHook(vtbl[D3D9_VT_CREATETEXTURE], (LPVOID)detour_create_tex,
                          (LPVOID *)&g_orig_create_tex);
    if (st_ct == MH_OK) {
        created = 1;
        log_msg("hooked IDirect3DDevice9::CreateTexture @ %p", vtbl[D3D9_VT_CREATETEXTURE]);
    } else {
        log_msg("MH_CreateHook CreateTexture: %s", MH_StatusToString(st_ct));
    }

    st_ut = MH_CreateHook(vtbl[D3D9_VT_UPDATETEXTURE], (LPVOID)detour_update_tex,
                          (LPVOID *)&g_orig_update_tex);
    if (st_ut == MH_OK) {
        created = 1;
        log_msg("hooked IDirect3DDevice9::UpdateTexture @ %p", vtbl[D3D9_VT_UPDATETEXTURE]);
    } else {
        log_msg("MH_CreateHook UpdateTexture: %s", MH_StatusToString(st_ut));
    }

    if (!created) {
        InterlockedExchange(&g_d3d_hooks_once, D3D_HOOK_IDLE);
        return;
    }

    st_en = MH_EnableHook(MH_ALL_HOOKS);
    if (st_en != MH_OK) {
        log_msg("MH_EnableHook (d3d device): %s", MH_StatusToString(st_en));
        if (st_ct == MH_OK)
            MH_RemoveHook(vtbl[D3D9_VT_CREATETEXTURE]);
        if (st_ut == MH_OK)
            MH_RemoveHook(vtbl[D3D9_VT_UPDATETEXTURE]);
        g_orig_create_tex = NULL;
        g_orig_update_tex = NULL;
        InterlockedExchange(&g_d3d_hooks_once, D3D_HOOK_IDLE);
        return;
    }

    InterlockedExchange(&g_d3d_hooks_once, D3D_HOOK_DONE);
    log_msg("d3d9 device hooks: CreateTexture+UpdateTexture");
}

static HRESULT STDMETHODCALLTYPE detour_create_device_ex(
    void *self, UINT adapter, DWORD type, HWND hwnd, DWORD flags,
    void *pp, void *fs, void **out_dev)
{
    HRESULT hr;
    if (!g_orig_create_device_ex)
        return E_FAIL;
    hr = g_orig_create_device_ex(self, adapter, type, hwnd, flags, pp, fs, out_dev);
    if (SUCCEEDED(hr) && out_dev && *out_dev)
        try_hook_d3d_device(*out_dev);
    return hr;
}

/* Backup path to install device hooks if CreateDeviceEx probe missed. */
static HRESULT WINAPI detour_tex_mem(
    void *pDevice,
    LPCVOID pSrcData,
    UINT SrcDataSize,
    UINT Width,
    UINT Height,
    UINT MipLevels,
    DWORD Usage,
    DWORD Format,
    DWORD Pool,
    DWORD Filter,
    DWORD MipFilter,
    DWORD ColorKey,
    void *pSrcInfo,
    void *pPalette,
    void **ppTexture)
{
    HRESULT hr;
    if (!g_orig_tex_mem)
        return E_FAIL;
    hr = g_orig_tex_mem(
        pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool,
        Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
    try_hook_d3d_device(pDevice);
    return hr;
}

static FARPROC get_export(const wchar_t *mod, const char *name)
{
    HMODULE h = GetModuleHandleW(mod);
    if (!h)
        h = LoadLibraryW(mod);
    if (!h)
        return NULL;
    return GetProcAddress(h, name);
}

static int module_directory(HMODULE mod, wchar_t *out, size_t out_n)
{
    wchar_t path[MAX_PATH];
    DWORD n;
    wchar_t *slash;
    if (!out || out_n == 0)
        return 0;
    out[0] = 0;
    if (!mod)
        return 0;
    n = GetModuleFileNameW(mod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return 0;
    *slash = 0;
    wcsncpy_s(out, out_n, path, _TRUNCATE);
    return out[0] != 0;
}

static HMODULE load_d3dx_from_dir(const wchar_t *dir)
{
    static const wchar_t *const names[] = {
        L"d3dx9_43.dll",
        L"d3dx9_42.dll",
        L"d3dx9_39.dll",
    };
    int i;
    if (!dir || !dir[0])
        return NULL;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        wchar_t path[MAX_PATH];
        HMODULE h;
        _snwprintf_s(path, _TRUNCATE, L"%s\\%s", dir, names[i]);
        h = LoadLibraryW(path);
        if (h)
            return h;
    }
    return NULL;
}

/*
 * Prefer an already-mapped d3dx, else load from hook / exe / bm2dx dirs
 * (official redistributable beside the game), and only then system search.
 * Wine's builtin system32 d3dx9_43 can crash under MinHook + fxc CT paths.
 */
static HMODULE find_or_load_d3dx(void)
{
    static const wchar_t *const names[] = {
        L"d3dx9_43.dll",
        L"d3dx9_42.dll",
        L"d3dx9_39.dll",
    };
    int i;
    wchar_t exe_dir[MAX_PATH];
    wchar_t iidx_dir[MAX_PATH];
    HMODULE bm2dx;

    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        HMODULE h = GetModuleHandleW(names[i]);
        if (h)
            return h;
    }

    {
        HMODULE h = load_d3dx_from_dir(g_dir);
        if (h)
            return h;
    }

    if (module_directory(GetModuleHandleW(NULL), exe_dir, MAX_PATH)) {
        HMODULE h = load_d3dx_from_dir(exe_dir);
        if (h)
            return h;
    }

    bm2dx = GetModuleHandleW(L"bm2dx.dll");
    if (module_directory(bm2dx, iidx_dir, MAX_PATH)) {
        HMODULE h = load_d3dx_from_dir(iidx_dir);
        if (h)
            return h;
    }

    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        HMODULE h = LoadLibraryW(names[i]);
        if (h)
            return h;
    }
    return NULL;
}

static int wait_modules(void)
{
    const DWORD timeout_ms = 180000;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        if (!GetModuleHandleW(L"bm2dx.dll")) {
            Sleep(50);
            continue;
        }
        if (find_or_load_d3dx())
            return 1;
        Sleep(50);
    }
    return 0;
}

static void install_hooks_cleanup(void)
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_orig_compile = NULL;
    g_orig_assemble = NULL;
    g_get_ctable = NULL;
    g_orig_tex_mem = NULL;
    g_orig_create_device_ex = NULL;
    g_orig_create_tex = NULL;
    g_orig_update_tex = NULL;
    g_d3dx_load_surf = NULL;
    InterlockedExchange(&g_hooks_live, 0);
    InterlockedExchange(&g_d3d_hooks_once, D3D_HOOK_IDLE);
}

static void shutdown_hooks(void)
{
    if (InterlockedExchange(&g_hooks_live, 0)) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_orig_compile = NULL;
        g_orig_assemble = NULL;
        g_get_ctable = NULL;
        g_orig_tex_mem = NULL;
        g_orig_create_device_ex = NULL;
        g_orig_create_tex = NULL;
        g_orig_update_tex = NULL;
        g_d3dx_load_surf = NULL;
        InterlockedExchange(&g_d3d_hooks_once, D3D_HOOK_IDLE);
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

static int install_hooks(void)
{
    MH_STATUS st;
    FARPROC p;
    const wchar_t *d3dx_name = L"d3dx9_43.dll";

    if (!GetModuleHandleW(L"d3dx9_43.dll")) {
        if (GetModuleHandleW(L"d3dx9_42.dll"))
            d3dx_name = L"d3dx9_42.dll";
        else if (GetModuleHandleW(L"d3dx9_39.dll"))
            d3dx_name = L"d3dx9_39.dll";
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        log_msg("MH_Initialize: %s", MH_StatusToString(st));
        return 1;
    }

    p = get_export(d3dx_name, "D3DXCompileShader");
    if (!p) {
        log_msg("D3DXCompileShader not found in %ls", d3dx_name);
        install_hooks_cleanup();
        return 1;
    }
    st = MH_CreateHook((LPVOID)p, (LPVOID)detour_compile, (LPVOID *)&g_orig_compile);
    if (st != MH_OK) {
        log_msg("MH_CreateHook D3DXCompileShader: %s", MH_StatusToString(st));
        install_hooks_cleanup();
        return 1;
    }
    log_msg("hooked %ls!D3DXCompileShader @ %p", d3dx_name, (void *)p);

    g_get_ctable = (D3DXGetShaderConstantTable_t)get_export(d3dx_name, "D3DXGetShaderConstantTable");
    if (!g_get_ctable)
        log_msg("D3DXGetShaderConstantTable missing");

    p = get_export(d3dx_name, "D3DXAssembleShader");
    if (p) {
        st = MH_CreateHook((LPVOID)p, (LPVOID)detour_assemble, (LPVOID *)&g_orig_assemble);
        if (st == MH_OK)
            log_msg("hooked %ls!D3DXAssembleShader @ %p", d3dx_name, (void *)p);
        else
            log_msg("MH_CreateHook D3DXAssembleShader: %s", MH_StatusToString(st));
    } else {
        log_msg("D3DXAssembleShader not found in %ls", d3dx_name);
    }

    p = get_export(d3dx_name, "D3DXCreateTextureFromFileInMemoryEx");
    if (p) {
        st = MH_CreateHook((LPVOID)p, (LPVOID)detour_tex_mem, (LPVOID *)&g_orig_tex_mem);
        if (st == MH_OK)
            log_msg("hooked %ls!D3DXCreateTextureFromFileInMemoryEx (device-hook backup) @ %p",
                    d3dx_name, (void *)p);
        else
            log_msg("MH_CreateHook D3DXCreateTextureFromFileInMemoryEx: %s", MH_StatusToString(st));
    }

    p = get_export(d3dx_name, "D3DXLoadSurfaceFromSurface");
    if (p) {
        g_d3dx_load_surf = (D3DXLoadSurfaceFromSurface_t)p;
        log_msg("resolved %ls!D3DXLoadSurfaceFromSurface @ %p", d3dx_name, (void *)p);
    } else {
        g_d3dx_load_surf = NULL;
        log_msg("D3DXLoadSurfaceFromSurface not found (UpdateTexture fallback disabled)");
    }

    /* Discover wine CreateDeviceEx via a temporary D3D9Ex (avoids fighting spice IAT). */
    {
        HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        Direct3DCreate9Ex_fn create9ex = NULL;
        void *d3d = NULL;
        if (d3d9)
            create9ex = (Direct3DCreate9Ex_fn)GetProcAddress(d3d9, "Direct3DCreate9Ex");
        if (create9ex && SUCCEEDED(create9ex(32, &d3d)) && d3d) {
            void **vtbl = *(void ***)d3d;
            if (vtbl && vtbl[D3D9EX_VT_CREATEDEVICEEX]) {
                st = MH_CreateHook(vtbl[D3D9EX_VT_CREATEDEVICEEX],
                                   (LPVOID)detour_create_device_ex,
                                   (LPVOID *)&g_orig_create_device_ex);
                if (st == MH_OK)
                    log_msg("hooked d3d9 CreateDeviceEx impl @ %p", vtbl[D3D9EX_VT_CREATEDEVICEEX]);
                else
                    log_msg("MH_CreateHook CreateDeviceEx impl: %s", MH_StatusToString(st));
            }
            ((IUnknown *)d3d)->Release();
        } else {
            log_msg("Direct3DCreate9Ex probe failed; will try device hook via D3DX texture path");
        }
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_msg("MH_EnableHook: %s", MH_StatusToString(st));
        install_hooks_cleanup();
        return 1;
    }
    InterlockedExchange(&g_hooks_live, 1);
    log_msg("hooks enabled (d3dx=%ls tex_mem_backup=%d)",
            d3dx_name, g_orig_tex_mem ? 1 : 0);
    return 0;
}

static DWORD WINAPI init_thread(LPVOID)
{
    InitializeCriticalSection(&g_log_cs);
    InterlockedExchange(&g_cs_ready, 1);
    init_paths();
    g_log_enabled = ini_int(L"log", 1);
    g_log_verbose = ini_int(L"log_verbose", 0);
    g_enabled = ini_int(L"enabled", 1);
    g_fallback = ini_int(L"fallback", 1);
    g_force_fxc = ini_int(L"force_fxc", 1);
    open_log();

    log_msg(
        "iidx_shader_fix starting (enabled=%d fallback=%d force_fxc=%d log_verbose=%d blobs=%d)",
        g_enabled, g_fallback, g_force_fxc, g_log_verbose, IIDX_SHADER_BLOB_COUNT);

    if (!wait_modules()) {
        log_msg("timeout waiting for bm2dx.dll / d3dx9");
        return 1;
    }
    {
        HMODULE d3dx = find_or_load_d3dx();
        wchar_t d3dx_path[MAX_PATH];
        if (d3dx && GetModuleFileNameW(d3dx, d3dx_path, MAX_PATH))
            log_msg("d3dx module %p path=%ls", (void *)d3dx, d3dx_path);
    }

    if (!g_enabled) {
        log_msg("disabled by ini");
        return 0;
    }

    if (install_hooks() != 0)
        return 1;

    log_msg("hooks enabled");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        shutdown_hooks();
    }
    return TRUE;
}
