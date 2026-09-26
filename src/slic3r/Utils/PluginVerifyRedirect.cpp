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

// The proprietary network plugin runs Authenticode checks on the host .exe and the
// studio DLL that loaded it. Neither of OUR files (orca-slicer.exe / OrcaSlicer.dll)
// is Bambu-signed, so we MinHook the plugin's lookups and feed it the genuine,
// Bambu-signed files instead:
//   * GetModuleFileName / CryptQueryObject on our exe/dll -> the genuine path, so
//     WinVerifyTrust runs against a real Bambu-signed binary and passes.
//   * GetModuleHandle("BambuStudio.dll")                  -> OUR module, so the plugin's
//     name-based studio lookup resolves to us even though our DLL is OrcaSlicer.dll.
// The genuine files are NOT shipped; we locate them in the installed Bambu Studio via
// its uninstall registry entry (env vars override for development).
//
// Resolved at install():
//   g_our_dll / g_our_exe         -> our unsigned files (this process)
//   g_genuine_dll / g_genuine_exe -> the genuine Bambu files we redirect to
//   g_self                        -> our studio DLL's own module handle
wchar_t g_our_dll[MAX_PATH] = {0};
wchar_t g_our_exe[MAX_PATH] = {0};
wchar_t g_genuine_dll_w[MAX_PATH] = {0};
char    g_genuine_dll_a[MAX_PATH] = {0};
wchar_t g_genuine_exe_w[MAX_PATH] = {0};
char    g_genuine_exe_a[MAX_PATH] = {0};
HMODULE g_self = nullptr;

// Signatures of the Win32 APIs we hook, used to call through to the originals.
typedef DWORD   (WINAPI *fn_gmfw)(HMODULE, LPWSTR, DWORD);
typedef DWORD   (WINAPI *fn_gmfa)(HMODULE, LPSTR, DWORD);
typedef BOOL    (WINAPI *fn_cqo)(DWORD, const void*, DWORD, DWORD, DWORD,
                                 DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);
typedef HMODULE (WINAPI *fn_gmhw)(LPCWSTR);
typedef HMODULE (WINAPI *fn_gmha)(LPCSTR);

// Trampolines to the original, un-hooked APIs, filled in by MH_CreateHook.
fn_gmfw o_gmfw = nullptr;
fn_gmfa o_gmfa = nullptr;
fn_cqo  o_cqo  = nullptr;
fn_gmhw o_gmhw = nullptr;
fn_gmha o_gmha = nullptr;

// Is the return address inside the proprietary network plugin? (resolved lazily, since the
// plugin loads after we install.) Always uses the trampoline to avoid re-entering our hook.
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

// Does this name refer to the genuine studio module name "BambuStudio.dll"?
bool wants_studio_w(const wchar_t* n)
{
    if (!n) return false;
    const wchar_t* b = wcsrchr(n, L'\\'); b = b ? b + 1 : n;
    return _wcsicmp(b, L"BambuStudio.dll") == 0 || _wcsicmp(b, L"BambuStudio") == 0;
}
bool wants_studio_a(const char* n)
{
    if (!n) return false;
    const char* b = strrchr(n, '\\'); b = b ? b + 1 : n;
    return _stricmp(b, "BambuStudio.dll") == 0 || _stricmp(b, "BambuStudio") == 0;
}

// Given a path the plugin queried, return the genuine file to substitute (our dll -> genuine
// dll, our exe -> genuine exe), or nullptr to leave as-is.
const wchar_t* redirect_w(const wchar_t* p)
{
    if (!p) return nullptr;
    if (g_our_dll[0] && _wcsicmp(p, g_our_dll) == 0 && g_genuine_dll_w[0]) return g_genuine_dll_w;
    if (g_our_exe[0] && _wcsicmp(p, g_our_exe) == 0 && g_genuine_exe_w[0]) return g_genuine_exe_w;
    return nullptr;
}
// ANSI counterpart of redirect_w(): widen the queried path, then return its genuine one.
const char* redirect_a(const char* p)
{
    if (!p) return nullptr;
    wchar_t w[MAX_PATH] = {0};
    MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH);
    if (g_our_dll[0] && _wcsicmp(w, g_our_dll) == 0 && g_genuine_dll_a[0]) return g_genuine_dll_a;
    if (g_our_exe[0] && _wcsicmp(w, g_our_exe) == 0 && g_genuine_exe_a[0]) return g_genuine_exe_a;
    return nullptr;
}

// Hook for GetModuleFileNameW: when the plugin queries one of our module paths, overwrite the
// returned buffer with the genuine path so its WinVerifyTrust check sees a Bambu-signed file.
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

// Hook for GetModuleFileNameA: ANSI counterpart of hk_gmfw.
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

// Hook for CryptQueryObject: when the plugin verifies a file object that is one of ours,
// substitute the genuine file path before the call reaches crypt32.
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

// Hooks for GetModuleHandleW/A: the plugin locates the studio module by the hardcoded name
// "BambuStudio.dll". Hand back our own module instead -- the plugin then caches it and uses
// it for the control-command signing path.
HMODULE WINAPI hk_gmhw(LPCWSTR name)
{
    if (g_self && wants_studio_w(name) && caller_is_plugin(_ReturnAddress())) return g_self;
    return o_gmhw(name);
}
HMODULE WINAPI hk_gmha(LPCSTR name)
{
    if (g_self && wants_studio_a(name) && caller_is_plugin(_ReturnAddress())) return g_self;
    return o_gmha(name);
}

// Record `path` in out_w (wide) + out_a (ansi) if it exists on disk. Returns true on success.
bool set_genuine(const wchar_t* path, wchar_t* out_w, char* out_a)
{
    if (!path || !path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    wcscpy_s(out_w, MAX_PATH, path);
    WideCharToMultiByte(CP_ACP, 0, path, -1, out_a, MAX_PATH, nullptr, nullptr);
    return true;
}

// Dev override: take a genuine path from an environment variable, if the file exists.
bool genuine_from_env(const char* env, wchar_t* out_w, char* out_a)
{
    const char* v = std::getenv(env);
    if (!v || !*v) return false;
    wchar_t w[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, v, -1, w, MAX_PATH);
    return set_genuine(w, out_w, out_a);
}

// Find the installed Bambu Studio directory (with trailing backslash) from its uninstall
// registry entry's DisplayIcon (which points at bambu-studio.exe). Returns false if Bambu
// Studio is not installed.
bool bambu_studio_dir(wchar_t* dir, size_t n)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Bambu Studio",
            0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return false;
    wchar_t icon[MAX_PATH] = {0};
    DWORD sz = sizeof(icon), type = 0;
    LONG rc = RegQueryValueExW(k, L"DisplayIcon", nullptr, &type, (LPBYTE)icon, &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || !icon[0]) return false;
    wchar_t* slash = wcsrchr(icon, L'\\');
    if (!slash) return false;
    slash[1] = 0;                       // keep the trailing backslash
    wcsncpy_s(dir, n, icon, _TRUNCATE);
    return true;
}

// Resolve a genuine file: env override first, then <bambu studio dir>\<leaf>.
bool resolve_genuine(const char* env, const wchar_t* bsdir, const wchar_t* leaf,
                     wchar_t* out_w, char* out_a)
{
    if (genuine_from_env(env, out_w, out_a)) return true;
    if (bsdir && bsdir[0]) {
        wchar_t p[MAX_PATH] = {0};
        wcscpy_s(p, MAX_PATH, bsdir);
        wcsncat_s(p, MAX_PATH, leaf, _TRUNCATE);
        return set_genuine(p, out_w, out_a);
    }
    return false;
}

} // namespace

namespace Slic3r {

// Entry point (idempotent): resolve our unsigned files + the genuine Bambu files (from the
// registry / env), then install the hooks that feed the plugin the genuine paths and our
// own module handle.
void install_plugin_verify_redirect()
{
    static bool done = false;
    if (done) return;
    done = true;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install_plugin_verify_redirect, &self);
    g_self = self;
    GetModuleFileNameW(self, g_our_dll, MAX_PATH);
    GetModuleFileNameW(nullptr, g_our_exe, MAX_PATH);

    // Genuine, Bambu-signed files the plugin's Authenticode checks are redirected to. We do
    // NOT ship copies -- locate them in the installed Bambu Studio via the registry (env vars
    // override for development). Bambu Studio must be installed for the network plugin to work.
    wchar_t bsdir[MAX_PATH] = {0};
    bambu_studio_dir(bsdir, MAX_PATH);
    bool have_dll = resolve_genuine("BAMBU_BRIDGE_GENUINE_DLL", bsdir, L"BambuStudio.dll",
                                    g_genuine_dll_w, g_genuine_dll_a);
    bool have_exe = resolve_genuine("BAMBU_BRIDGE_GENUINE_EXE", bsdir, L"bambu-studio.exe",
                                    g_genuine_exe_w, g_genuine_exe_a);
    if (!have_dll && !have_exe) return;  // no genuine files found -> nothing to redirect

    if (MH_Initialize() != MH_OK) return;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE c32 = GetModuleHandleW(L"crypt32.dll");
    if (!c32) c32 = LoadLibraryW(L"crypt32.dll");
    if (k32) {
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleFileNameW"), (void*)hk_gmfw, (void**)&o_gmfw);
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleFileNameA"), (void*)hk_gmfa, (void**)&o_gmfa);
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleHandleW"),   (void*)hk_gmhw, (void**)&o_gmhw);
        MH_CreateHook((void*)GetProcAddress(k32, "GetModuleHandleA"),   (void*)hk_gmha, (void**)&o_gmha);
    }
    if (c32)
        MH_CreateHook((void*)GetProcAddress(c32, "CryptQueryObject"), (void*)hk_cqo, (void**)&o_cqo);
    MH_EnableHook(MH_ALL_HOOKS);
}

} // namespace Slic3r

#else  // !_WIN32

// Non-Windows build: there is no Authenticode gate to satisfy, so the entry point is a no-op.
namespace Slic3r { void install_plugin_verify_redirect() {} }

#endif
