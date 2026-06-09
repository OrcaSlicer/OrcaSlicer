#include "PluginVerifyRedirect.hpp"

#ifdef _WIN32

#include <windows.h>
#include <wincrypt.h>
#include <intrin.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MinHook.h"

namespace {

// The proprietary network plugin runs TWO Authenticode checks: it verifies the
// studio DLL that loaded it AND the host .exe of the process. Neither of OUR
// files (OrcaSlicer.dll / orca-slicer.exe) is Bambu-signed, so for BOTH we
// intercept the plugin's path queries (GetModuleFileName / CryptQueryObject) and
// hand back the genuine on-disk Bambu file instead. The plugin then runs
// WinVerifyTrust on the genuine path and passes. This lets us launch OrcaSlicer's
// own unsigned exe + dll with no Bambu-named files staged anywhere.
//
// Resolved at install():
//   g_our_dll / g_our_exe        -> our forked, unsigned files (this process)
//   g_genuine_dll / g_genuine_exe -> the genuine Bambu files we redirect to
wchar_t g_our_dll[MAX_PATH] = {0};
wchar_t g_our_exe[MAX_PATH] = {0};
wchar_t g_genuine_dll_w[MAX_PATH] = {0};
char    g_genuine_dll_a[MAX_PATH] = {0};
wchar_t g_genuine_exe_w[MAX_PATH] = {0};
char    g_genuine_exe_a[MAX_PATH] = {0};

typedef DWORD (WINAPI *fn_gmfw)(HMODULE, LPWSTR, DWORD);
typedef DWORD (WINAPI *fn_gmfa)(HMODULE, LPSTR, DWORD);
typedef BOOL  (WINAPI *fn_cqo)(DWORD, const void*, DWORD, DWORD, DWORD,
                               DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);

fn_gmfw o_gmfw = nullptr;
fn_gmfa o_gmfa = nullptr;
fn_cqo  o_cqo  = nullptr;

// Is the return address inside the proprietary network plugin? (resolved lazily,
// since the plugin loads after we install.) Always uses the trampoline to avoid
// re-entering our own hook.
bool caller_is_plugin(void* ra)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ra, &h) || !h || !o_gmfw)
        return false;
    wchar_t p[MAX_PATH] = {0};
    o_gmfw(h, p, MAX_PATH);
    return wcsstr(p, L"bambu_networking") != nullptr || wcsstr(p, L"BambuSource") != nullptr;
}

// Given a path the plugin just queried, return the genuine file to substitute
// (our dll -> genuine dll, our exe -> genuine exe), or nullptr to leave as-is.
const wchar_t* redirect_w(const wchar_t* p)
{
    if (!p) return nullptr;
    if (g_our_dll[0] && _wcsicmp(p, g_our_dll) == 0 && g_genuine_dll_w[0]) return g_genuine_dll_w;
    if (g_our_exe[0] && _wcsicmp(p, g_our_exe) == 0 && g_genuine_exe_w[0]) return g_genuine_exe_w;
    return nullptr;
}
const char* redirect_a(const char* p)
{
    if (!p) return nullptr;
    wchar_t w[MAX_PATH] = {0};
    MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH);
    if (g_our_dll[0] && _wcsicmp(w, g_our_dll) == 0 && g_genuine_dll_a[0]) return g_genuine_dll_a;
    if (g_our_exe[0] && _wcsicmp(w, g_our_exe) == 0 && g_genuine_exe_a[0]) return g_genuine_exe_a;
    return nullptr;
}

DWORD WINAPI hk_gmfw(HMODULE hMod, LPWSTR fn, DWORD sz)
{
    void* ra = _ReturnAddress();
    DWORD r = o_gmfw(hMod, fn, sz);
    if (fn && r && caller_is_plugin(ra)) {
        const wchar_t* t = redirect_w(fn);
        if (t) { size_t n = wcslen(t); if (sz > n) { wcscpy_s(fn, sz, t); return (DWORD)n; } }
    }
    return r;
}

DWORD WINAPI hk_gmfa(HMODULE hMod, LPSTR fn, DWORD sz)
{
    void* ra = _ReturnAddress();
    DWORD r = o_gmfa(hMod, fn, sz);
    if (fn && r && caller_is_plugin(ra)) {
        const char* t = redirect_a(fn);
        if (t) { size_t n = strlen(t); if (sz > n) { strcpy_s(fn, sz, t); return (DWORD)n; } }
    }
    return r;
}

BOOL WINAPI hk_cqo(DWORD ot, const void* obj, DWORD a, DWORD b, DWORD c,
                   DWORD* d, DWORD* e, DWORD* f, HCERTSTORE* st, HCRYPTMSG* msg, const void** ctx)
{
    const void* use = obj;
    if (ot == CERT_QUERY_OBJECT_FILE && obj && caller_is_plugin(_ReturnAddress())) {
        const wchar_t* t = redirect_w((const wchar_t*)obj);
        if (t) use = t;
    }
    return o_cqo(ot, use, a, b, c, d, e, f, st, msg, ctx);
}

// Fill out[] (wide) + outa[] (ansi) from an env var, falling back to a default,
// only if the resolved file actually exists. Returns true if a usable path set.
bool resolve_genuine(const char* env, const char* fallback, wchar_t* out_w, char* out_a)
{
    const char* v = std::getenv(env);
    std::string p = (v && *v) ? v : fallback;
    wchar_t w[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, MAX_PATH);
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) return false;
    wcscpy_s(out_w, MAX_PATH, w);
    strncpy_s(out_a, MAX_PATH, p.c_str(), _TRUNCATE);
    return true;
}

} // namespace

namespace Slic3r {

void install_plugin_verify_redirect()
{
    static bool done = false;
    if (done) return;
    done = true;

    // Resolve the genuine Bambu files we redirect the plugin's checks to. The
    // exe redirect is what lets us run our own (Orca-branded, unsigned) launcher
    // instead of a Bambu-signed host. If a genuine file is missing its redirect
    // simply stays disabled.
    bool have_dll = resolve_genuine("BAMBU_BRIDGE_GENUINE_DLL",
                                    "C:\\Program Files\\Bambu Studio\\BambuStudio.dll",
                                    g_genuine_dll_w, g_genuine_dll_a);
    bool have_exe = resolve_genuine("BAMBU_BRIDGE_GENUINE_EXE",
                                    "C:\\Program Files\\Bambu Studio\\bambu-studio.exe",
                                    g_genuine_exe_w, g_genuine_exe_a);
    if (!have_dll && !have_exe) return;  // nothing to redirect to

    // our own dll path (the module this function lives in) and our exe path
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install_plugin_verify_redirect, &self);
    GetModuleFileNameW(self, g_our_dll, MAX_PATH);
    GetModuleFileNameW(nullptr, g_our_exe, MAX_PATH);

    if (MH_Initialize() != MH_OK) return;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE c32 = GetModuleHandleW(L"crypt32.dll");
    if (!c32) c32 = LoadLibraryW(L"crypt32.dll");
    if (k32) {
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleFileNameW"), (void*)hk_gmfw, (void**)&o_gmfw);
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleFileNameA"), (void*)hk_gmfa, (void**)&o_gmfa);
    }
    if (c32)
        MH_CreateHook((void*)GetProcAddress(c32, "CryptQueryObject"), (void*)hk_cqo, (void**)&o_cqo);
    MH_EnableHook(MH_ALL_HOOKS);
}

} // namespace Slic3r

#else  // !_WIN32

namespace Slic3r { void install_plugin_verify_redirect() {} }

#endif
