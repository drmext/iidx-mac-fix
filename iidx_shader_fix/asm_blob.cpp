#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Offline helper: assemble IIDX vs.1.1 / ps.1.1 sources with Microsoft
 * d3dx9_43!D3DXAssembleShader and write shaders/iidx_vs11.cso + iidx_ps11.cso
 * for gen_bytecode.py / build64.bat.
 *
 * Build/run: build_asm.bat  (or cl asm_blob.cpp && asm_blob.exe)
 */

struct ID3DXBuffer : public IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize(void) = 0;
};

typedef HRESULT(WINAPI *AsmFn)(
    LPCSTR, UINT, const void *, const void *, DWORD, ID3DXBuffer **, ID3DXBuffer **);

/* Embedded fallbacks (exact game strings from bm2dx) if .asm files are missing. */
static const char kFallbackVs[] =
    "vs.1.1\t\t//Shader version 1.1\t\t\n"
    "dcl_position\t\tv0;\t\t\t\t\t\n"
    "dcl_color\t\t\tv1;\t\t\t\t\t\n"
    "dcl_texcoord0\t\tv2;\t\t\t\t\t\n"
    "m4x4\toPos,\t\tv0, c0\t\t\t\t\n"
    "mul\toD0,\t\tv1, c4\t\t\t\t\n"
    "mov\toT0.xy,\t\tv2\t\t\t\t\t\n";

static const char kFallbackPs[] =
    "ps.1.1\t\t\t//Shader version 1.2\t\t\n"
    "tex\tt0\t\t\t\t\t\t\t\t\t\n"
    "tex\tt1\t\t\t\t\t\t\t\t\t\n"
    "mad\tr1, v0, t0, c0 \t\t\t\t\t\t\n"
    "lrp    r0, r1.a, r1, t1\t\t\t\t\t\n"
    "mov    r0.a, r1.a\t\t\t\t\t\t\t\n";

static int dirname_of(const wchar_t *path, wchar_t *out, size_t out_n)
{
    wchar_t tmp[MAX_PATH];
    wchar_t *slash;
    if (!path || !out || out_n == 0)
        return 0;
    wcsncpy_s(tmp, path, _TRUNCATE);
    slash = wcsrchr(tmp, L'\\');
    if (!slash)
        slash = wcsrchr(tmp, L'/');
    if (!slash)
        return 0;
    *slash = 0;
    wcsncpy_s(out, out_n, tmp, _TRUNCATE);
    return out[0] != 0;
}

static HMODULE load_d3dx(const wchar_t *exe_dir)
{
    static const wchar_t *const names[] = {
        L"d3dx9_43.dll",
        L"d3dx9_42.dll",
        L"d3dx9_39.dll",
    };
    wchar_t path[MAX_PATH];
    wchar_t contents[MAX_PATH];
    size_t i;

    /* 1) Next to asm_blob.exe */
    if (exe_dir && exe_dir[0]) {
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            HMODULE m;
            _snwprintf_s(path, _TRUNCATE, L"%s\\%s", exe_dir, names[i]);
            m = LoadLibraryW(path);
            if (m) {
                wprintf(L"loaded %s\n", path);
                return m;
            }
        }
        /* 2) contents/ = two levels up from exe_dir. */
        _snwprintf_s(contents, _TRUNCATE, L"%s\\..\\..", exe_dir);
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            HMODULE m;
            _snwprintf_s(path, _TRUNCATE, L"%s\\%s", contents, names[i]);
            m = LoadLibraryW(path);
            if (m) {
                wprintf(L"loaded %s\n", path);
                return m;
            }
        }
    }

    /* 3) System / PATH search. */
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        HMODULE m = LoadLibraryW(names[i]);
        if (m) {
            wprintf(L"loaded %s (search path)\n", names[i]);
            return m;
        }
    }
    return NULL;
}

static char *read_file_or_fallback(const wchar_t *path, const char *fallback)
{
    FILE *f = NULL;
    long sz;
    char *buf;
    if (_wfopen_s(&f, path, L"rb") == 0 && f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            sz = ftell(f);
            if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
                buf = (char *)malloc((size_t)sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)sz, f);
                    buf[n] = 0;
                    fclose(f);
                    wprintf(L"read %s (%u bytes)\n", path, (unsigned)n);
                    return buf;
                }
            }
        }
        fclose(f);
    }
    printf("using embedded fallback for missing/unreadable asm\n");
    buf = (char *)malloc(strlen(fallback) + 1);
    if (!buf)
        return NULL;
    memcpy(buf, fallback, strlen(fallback) + 1);
    return buf;
}

static int dump_asm(AsmFn asmfn, const char *name, const char *src, const wchar_t *out_path)
{
    ID3DXBuffer *out = NULL, *err = NULL;
    HRESULT hr;
    if (!src)
        return 1;
    hr = asmfn(src, (UINT)strlen(src), NULL, NULL, 0, &out, &err);
    printf("%s hr=0x%08X size=%u\n", name, (unsigned)hr, out ? out->GetBufferSize() : 0);
    if (FAILED(hr) && err && err->GetBufferPointer())
        printf("  err: %s\n", (char *)err->GetBufferPointer());
    if (SUCCEEDED(hr) && out) {
        FILE *f = NULL;
        if (_wfopen_s(&f, out_path, L"wb") == 0 && f) {
            fwrite(out->GetBufferPointer(), 1, out->GetBufferSize(), f);
            fclose(f);
            wprintf(L"  wrote %s (%u bytes)\n", out_path, out->GetBufferSize());
        } else {
            wprintf(L"  failed to write %s\n", out_path);
            hr = E_FAIL;
        }
    }
    if (out)
        out->Release();
    if (err)
        err->Release();
    return FAILED(hr);
}

int main(void)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t vs_asm[MAX_PATH], ps_asm[MAX_PATH];
    wchar_t vs_cso[MAX_PATH], ps_cso[MAX_PATH];
    HMODULE m;
    AsmFn asmfn;
    char *vs = NULL;
    char *ps = NULL;
    int rc = 0;

    exe_dir[0] = 0;
    if (GetModuleFileNameW(NULL, exe_path, MAX_PATH))
        dirname_of(exe_path, exe_dir, MAX_PATH);

    m = load_d3dx(exe_dir);
    if (!m) {
        printf("failed to load d3dx9_43.dll (place Microsoft DLL next to asm_blob.exe "
               "or in contents/)\n");
        return 1;
    }
    asmfn = (AsmFn)GetProcAddress(m, "D3DXAssembleShader");
    if (!asmfn) {
        printf("no D3DXAssembleShader export\n");
        return 1;
    }

    if (exe_dir[0]) {
        _snwprintf_s(vs_asm, _TRUNCATE, L"%s\\shaders\\iidx_vs11.asm", exe_dir);
        _snwprintf_s(ps_asm, _TRUNCATE, L"%s\\shaders\\iidx_ps11.asm", exe_dir);
        _snwprintf_s(vs_cso, _TRUNCATE, L"%s\\shaders\\iidx_vs11.cso", exe_dir);
        _snwprintf_s(ps_cso, _TRUNCATE, L"%s\\shaders\\iidx_ps11.cso", exe_dir);
    } else {
        wcscpy_s(vs_asm, L"shaders\\iidx_vs11.asm");
        wcscpy_s(ps_asm, L"shaders\\iidx_ps11.asm");
        wcscpy_s(vs_cso, L"shaders\\iidx_vs11.cso");
        wcscpy_s(ps_cso, L"shaders\\iidx_ps11.cso");
    }

    vs = read_file_or_fallback(vs_asm, kFallbackVs);
    ps = read_file_or_fallback(ps_asm, kFallbackPs);
    if (!vs || !ps) {
        printf("OOM reading sources\n");
        free(vs);
        free(ps);
        return 1;
    }

    rc |= dump_asm(asmfn, "VS", vs, vs_cso);
    rc |= dump_asm(asmfn, "PS", ps, ps_cso);

    free(vs);
    free(ps);
    return rc;
}
