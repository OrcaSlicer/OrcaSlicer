// Why?
#define _WIN32_WINNT 0x0601
// The standard Windows includes.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wchar.h>



#ifdef SLIC3R_GUI
extern "C"
{
    // Let the NVIDIA and AMD know we want to use their graphics card
    // on a dual graphics card system.
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif /* SLIC3R_GUI */

#include <stdlib.h>
#include <stdio.h>

#ifdef SLIC3R_GUI
    #include <GL/GL.h>
#endif /* SLIC3R_GUI */

#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "comctl32.lib")

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

#include <stdio.h>

#ifdef SLIC3R_GUI
class OpenGLVersionCheck
{
public:
    std::string version;
    std::string glsl_version;
    std::string vendor;
    std::string renderer;

    HINSTANCE   hOpenGL = nullptr;
    bool 		success = false;

    bool load_opengl_dll()
    {
        MSG      msg     = {0};
        WNDCLASS wc      = {0};
        wc.lpfnWndProc   = OpenGLVersionCheck::supports_opengl2_wndproc;
        wc.hInstance     = (HINSTANCE)GetModuleHandle(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND);
        wc.lpszClassName = L"OrcaSlicer_opengl_version_check";
        wc.style = CS_OWNDC;
        if (RegisterClass(&wc)) {
            HWND hwnd = CreateWindowW(wc.lpszClassName, L"OrcaSlicer_opengl_version_check", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, 0, 0, wc.hInstance, (LPVOID)this);
            if (hwnd) {
                message_pump_exit = false;
                while (GetMessage(&msg, NULL, 0, 0 ) > 0 && ! message_pump_exit)
                    DispatchMessage(&msg);
            }
        }
        return this->success;
    }

    void unload_opengl_dll()
    {
        if (this->hOpenGL) {
            BOOL released = FreeLibrary(this->hOpenGL);
            if (released)
                printf("System OpenGL library released\n");
            else
                printf("System OpenGL library NOT released\n");
            this->hOpenGL = nullptr;
        }
    }

    bool is_version_greater_or_equal_to(unsigned int major, unsigned int minor) const
    {
        // printf("is_version_greater_or_equal_to, version: %s\n", version.c_str());
        std::vector<std::string> tokens;
        boost::split(tokens, version, boost::is_any_of(" "), boost::token_compress_on);
        if (tokens.empty())
            return false;

        std::vector<std::string> numbers;
        boost::split(numbers, tokens[0], boost::is_any_of("."), boost::token_compress_on);

        unsigned int gl_major = 0;
        unsigned int gl_minor = 0;
        if (numbers.size() > 0)
            gl_major = ::atoi(numbers[0].c_str());
        if (numbers.size() > 1)
            gl_minor = ::atoi(numbers[1].c_str());
        // printf("Major: %d, minor: %d\n", gl_major, gl_minor);
        if (gl_major < major)
            return false;
        else if (gl_major > major)
            return true;
        else
            return gl_minor >= minor;
    }

protected:
    static bool message_pump_exit;

    void check(HWND hWnd)
    {
        hOpenGL = LoadLibraryExW(L"opengl32.dll", nullptr, 0);
        if (hOpenGL == nullptr) {
            printf("Failed loading the system opengl32.dll\n");
            return;
        }

        typedef HGLRC 		(WINAPI *Func_wglCreateContext)(HDC);
        typedef BOOL 		(WINAPI *Func_wglMakeCurrent  )(HDC, HGLRC);
        typedef BOOL     	(WINAPI *Func_wglDeleteContext)(HGLRC);
        typedef GLubyte* 	(WINAPI *Func_glGetString     )(GLenum);

        Func_wglCreateContext 	wglCreateContext = (Func_wglCreateContext)GetProcAddress(hOpenGL, "wglCreateContext");
        Func_wglMakeCurrent 	wglMakeCurrent 	 = (Func_wglMakeCurrent)  GetProcAddress(hOpenGL, "wglMakeCurrent");
        Func_wglDeleteContext 	wglDeleteContext = (Func_wglDeleteContext)GetProcAddress(hOpenGL, "wglDeleteContext");
        Func_glGetString 		glGetString 	 = (Func_glGetString)	  GetProcAddress(hOpenGL, "glGetString");

        if (wglCreateContext == nullptr || wglMakeCurrent == nullptr || wglDeleteContext == nullptr || glGetString == nullptr) {
            printf("Failed loading the system opengl32.dll: The library is invalid.\n");
            return;
        }

        PIXELFORMATDESCRIPTOR pfd =
        {
            sizeof(PIXELFORMATDESCRIPTOR),
            1,
            PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
            PFD_TYPE_RGBA,            	// The kind of framebuffer. RGBA or palette.
            32,                        	// Color depth of the framebuffer.
            0, 0, 0, 0, 0, 0,
            0,
            0,
            0,
            0, 0, 0, 0,
            24,                        	// Number of bits for the depthbuffer
            8,                        	// Number of bits for the stencilbuffer
            0,                        	// Number of Aux buffers in the framebuffer.
            PFD_MAIN_PLANE,
            0,
            0, 0, 0
        };

        HDC ourWindowHandleToDeviceContext = ::GetDC(hWnd);
        // Gdi32.dll
        int letWindowsChooseThisPixelFormat = ::ChoosePixelFormat(ourWindowHandleToDeviceContext, &pfd);
        // Gdi32.dll
        SetPixelFormat(ourWindowHandleToDeviceContext, letWindowsChooseThisPixelFormat, &pfd);
        // Opengl32.dll
        HGLRC glcontext = wglCreateContext(ourWindowHandleToDeviceContext);
        wglMakeCurrent(ourWindowHandleToDeviceContext, glcontext);
        // Opengl32.dll
        const char *data = (const char*)glGetString(GL_VERSION);
        if (data != nullptr)
            this->version = data;
        // printf("check -version: %s\n", version.c_str());
        data = (const char*)glGetString(0x8B8C); // GL_SHADING_LANGUAGE_VERSION
        if (data != nullptr)
            this->glsl_version = data;
        data = (const char*)glGetString(GL_VENDOR);
        if (data != nullptr)
            this->vendor = data;
        data = (const char*)glGetString(GL_RENDERER);
        if (data != nullptr)
            this->renderer = data;
        // Opengl32.dll
        wglDeleteContext(glcontext);
        ::ReleaseDC(hWnd, ourWindowHandleToDeviceContext);
        this->success = true;
    }

    static LRESULT CALLBACK supports_opengl2_wndproc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch(message)
        {
        case WM_CREATE:
        {
            CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            OpenGLVersionCheck *ogl_data = reinterpret_cast<OpenGLVersionCheck*>(pCreate->lpCreateParams);
            ogl_data->check(hWnd);
            DestroyWindow(hWnd);
            return 0;
        }
        case WM_NCDESTROY:
            message_pump_exit = true;
            return 0;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
};

bool OpenGLVersionCheck::message_pump_exit = false;
#endif /* SLIC3R_GUI */

extern "C" {
    typedef int (__stdcall *Slic3rMainFunc)(int argc, wchar_t **argv);
    Slic3rMainFunc orcaslicer_main = nullptr;
}

// ---- Genuine-host staging (defense-in-depth backup for the installer) --------------
// The installer normally stages a genuine Bambu-signed bambu-studio.exe + BambuStudio.dll
// next to OrcaSlicer at install time (elevated). As a backup, the launcher self-heals on
// startup: if the host is missing but Bambu Studio is installed, copy the genuine files
// in. That also lights up Bambu mode if Bambu Studio is installed AFTER OrcaSlicer.
enum class StageResult { Ok, NoBambu, WriteFailed };

// Locate an installed Bambu Studio via the registry. On success fills genuine_exe with
// its bambu-studio.exe and genuine_dir with its install directory (trailing backslash).
static bool find_bambu_studio(wchar_t* genuine_exe, wchar_t* genuine_dir)
{
    genuine_exe[0] = 0; genuine_dir[0] = 0;
    HKEY hk = nullptr;
    bool found = false;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Bambu Studio",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        wchar_t icon[MAX_PATH + 1] = { 0 };
        DWORD cb = sizeof(icon) - sizeof(wchar_t), type = 0;
        if (::RegQueryValueExW(hk, L"DisplayIcon", nullptr, &type, (LPBYTE)icon, &cb) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            wchar_t* comma = wcsrchr(icon, L',');               // strip ",<iconIndex>"
            wchar_t* slash = wcsrchr(icon, L'\\');
            if (comma && (!slash || comma > slash)) *comma = 0;
            if (slash && ::GetFileAttributesW(icon) != INVALID_FILE_ATTRIBUTES) {
                wcscpy(genuine_exe, icon);                      // the genuine bambu-studio.exe
                slash[1] = 0;                                   // truncate to the install dir
                wcscpy(genuine_dir, icon);
                found = true;
            }
        }
        ::RegCloseKey(hk);
    }
    return found;
}

// Copy the genuine host + DLL into `dir`, and make sure dir\BambuStudio.dll exists (a
// vanilla install may have shipped it renamed to OrcaSlicer.dll). Idempotent.
static StageResult stage_genuine_files(const wchar_t* dir)
{
    wchar_t host_exe[MAX_PATH + 1] = { 0 };
    wcscpy(host_exe, dir); wcscat(host_exe, L"bambu-studio.exe");

    wchar_t genuine_exe[MAX_PATH + 1] = { 0 }, genuine_dir[MAX_PATH + 1] = { 0 };
    bool bambu = find_bambu_studio(genuine_exe, genuine_dir);
    if (bambu) {
        ::CopyFileW(genuine_exe, host_exe, FALSE);
        // Stash the genuine BambuStudio.dll as BambuStudioOriginal.dll so the plugin's
        // signature check stays satisfiable even if Bambu Studio is later uninstalled.
        wchar_t gdll[MAX_PATH + 1] = { 0 }; wcscpy(gdll, genuine_dir); wcscat(gdll, L"BambuStudio.dll");
        wchar_t odll[MAX_PATH + 1] = { 0 }; wcscpy(odll, dir);         wcscat(odll, L"BambuStudioOriginal.dll");
        if (::GetFileAttributesW(gdll) != INVALID_FILE_ATTRIBUTES
            && ::GetFileAttributesW(odll) == INVALID_FILE_ATTRIBUTES)
            ::CopyFileW(gdll, odll, FALSE);
    }
    // The genuine host loads its studio dll by the fixed name BambuStudio.dll.
    if (::GetFileAttributesW(host_exe) != INVALID_FILE_ATTRIBUTES) {
        wchar_t studio_dll[MAX_PATH + 1] = { 0 }; wcscpy(studio_dll, dir); wcscat(studio_dll, L"BambuStudio.dll");
        if (::GetFileAttributesW(studio_dll) == INVALID_FILE_ATTRIBUTES) {
            wchar_t orca_dll[MAX_PATH + 1] = { 0 }; wcscpy(orca_dll, dir); wcscat(orca_dll, L"OrcaSlicer.dll");
            if (::GetFileAttributesW(orca_dll) != INVALID_FILE_ATTRIBUTES)
                ::CopyFileW(orca_dll, studio_dll, FALSE);
        }
    }
    if (::GetFileAttributesW(host_exe) != INVALID_FILE_ATTRIBUTES) return StageResult::Ok;
    return bambu ? StageResult::WriteFailed : StageResult::NoBambu;
}

// Re-launch ourselves elevated (UAC consent) to run only the staging, so it can write a
// protected install dir (Program Files). Returns true if the elevated pass ran.
static bool elevate_and_stage(const wchar_t* self_exe)
{
    SHELLEXECUTEINFOW sei; ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";                                       // triggers the UAC prompt
    sei.lpFile = self_exe;
    sei.lpParameters = L"--stage-genuine";
    sei.nShow  = SW_HIDE;
    if (::ShellExecuteExW(&sei) && sei.hProcess) {
        ::WaitForSingleObject(sei.hProcess, 120000);
        ::CloseHandle(sei.hProcess);
        return true;
    }
    return false;                                                // user declined UAC, or failed
}

// Per-user "don't offer the bridge again" flag (HKCU\Software\OrcaSlicer\SkipBambuBridge).
static bool bridge_skip_forever()
{
    DWORD val = 0, cb = sizeof(val), type = 0;
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\OrcaSlicer", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        ::RegQueryValueExW(hk, L"SkipBambuBridge", nullptr, &type, (LPBYTE)&val, &cb);
        ::RegCloseKey(hk);
    }
    return type == REG_DWORD && val != 0;
}
static void set_bridge_skip_forever()
{
    HKEY hk = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\OrcaSlicer", 0, nullptr, 0,
            KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        DWORD val = 1;
        ::RegSetValueExW(hk, L"SkipBambuBridge", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        ::RegCloseKey(hk);
    }
}

// Consent dialog shown BEFORE the UAC prompt. 1 = Yes (stage), 2 = Skip for now,
// 3 = Skip forever.
static int ask_stage_bridge()
{
    TASKDIALOGCONFIG cfg; ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize  = sizeof(cfg);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    cfg.pszWindowTitle     = L"OrcaSlicer";
    cfg.pszMainIcon        = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"Add / Heal Full Bambu Network Bridge";
    cfg.pszContent =
        L"Bambu Studio is installed on this PC. OrcaSlicer can copy its genuine network "
        L"component so you can connect to Bambu Lab printers (cloud and LAN) directly. "
        L"Windows will ask for administrator approval.";
    static const TASKDIALOG_BUTTON buttons[] = {
        { 100, L"Yes\nAdd the Bambu network bridge (requires administrator)" },
        { 101, L"Skip For Now\nContinue without it; ask again next time" },
        { 102, L"Skip Forever\nContinue without it; don't ask again" },
    };
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    int pressed = 0;
    if (SUCCEEDED(::TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) {
        if (pressed == 100) return 1;
        if (pressed == 102) return 3;
        return 2;                          // Skip For Now, or closed via X / Esc
    }
    // Fallback if the rich dialog is unavailable.
    int m = ::MessageBoxW(nullptr,
        L"Add the full Bambu network bridge so you can use Bambu Lab printers?\n"
        L"This needs administrator approval.\n\n"
        L"Yes = Add now\nNo = Skip for now\nCancel = Skip forever (don't ask again)",
        L"OrcaSlicer", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (m == IDYES)    return 1;
    if (m == IDCANCEL) return 3;
    return 2;
}
// ------------------------------------------------------------------------------------

extern "C" {
#ifdef SLIC3R_WRAPPER_NOCONSOLE
int APIENTRY wWinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, PWSTR /* lpCmdLine */, int /* nCmdShow */)
{
    int 	  argc;
    wchar_t **argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
#else
int wmain(int argc, wchar_t **argv)
{
#endif
    // Allow the asserts to open message box, such message box allows to ignore the assert and continue with the application.
    // Without this call, the seemingly same message box is being opened by the abort() function, but that is too late and
    // the application will be killed even if "Ignore" button is pressed.
    _set_error_mode(_OUT_TO_MSGBOX);

    std::vector<wchar_t*> argv_extended;
    argv_extended.emplace_back(argv[0]);

#ifdef SLIC3R_WRAPPER_GCODEVIEWER
    wchar_t gcodeviewer_param[] = L"--gcodeviewer";
    argv_extended.emplace_back(gcodeviewer_param);
#endif /* SLIC3R_WRAPPER_GCODEVIEWER */

#ifdef SLIC3R_GUI
    // Here one may push some additional parameters based on the wrapper type.
    bool force_mesa = false;
#endif /* SLIC3R_GUI */
    bool stage_only = false;                 // internal: elevated genuine-host staging pass
    for (int i = 1; i < argc; ++ i) {
        if (wcscmp(argv[i], L"--stage-genuine") == 0) { stage_only = true; continue; }  // not forwarded
#ifdef SLIC3R_GUI
        if (wcscmp(argv[i], L"--sw-renderer") == 0)
            force_mesa = true;
        else if (wcscmp(argv[i], L"--no-sw-renderer") == 0)
            force_mesa = false;
#endif /* SLIC3R_GUI */
        argv_extended.emplace_back(argv[i]);
    }
    argv_extended.emplace_back(nullptr);

#ifdef SLIC3R_GUI
    OpenGLVersionCheck opengl_version_check;
    bool load_mesa = false;
    if (!stage_only)                          // the staging pass does no GUI work
        load_mesa =
            // Forced from the command line.
            force_mesa ||
            // Try to load the default OpenGL driver and test its context version.
            ! opengl_version_check.load_opengl_dll() || ! opengl_version_check.is_version_greater_or_equal_to(2, 0);
#endif /* SLIC3R_GUI */

    wchar_t path_to_exe[MAX_PATH + 1] = { 0 };
    ::GetModuleFileNameW(nullptr, path_to_exe, MAX_PATH);
    wchar_t self_exe[MAX_PATH + 1] = { 0 };
    wcscpy(self_exe, path_to_exe);                  // full path to this exe, for re-launch
    wchar_t drive[_MAX_DRIVE];
    wchar_t dir[_MAX_DIR];
    wchar_t fname[_MAX_FNAME];
    wchar_t ext[_MAX_EXT];
    _wsplitpath(path_to_exe, drive, dir, fname, ext);
    _wmakepath(path_to_exe, drive, dir, nullptr, nullptr);

    wchar_t path_to_python[MAX_PATH + 1] = { 0 };
    wcscpy(path_to_python, path_to_exe);
    wcscat(path_to_python, L"python");
    DWORD python_attrs = GetFileAttributesW(path_to_python);
    if (python_attrs != INVALID_FILE_ATTRIBUTES && (python_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        SetDllDirectoryW(path_to_python);
    }

    // Elevated entry point: a UAC-elevated copy of ourselves (launched by the self-heal
    // below) runs ONLY the staging and exits, so it can write into a protected install
    // dir (Program Files) on behalf of the normal-user launch.
    if (stage_only) {
        stage_genuine_files(path_to_exe);
        return 0;
    }

    // ---- Run under a genuine, Bambu-signed host --------------------------------
    // orca-slicer.exe is unsigned. The proprietary Bambu network plugin verifies the
    // HOST process's Authenticode signature when it loads and deliberately crashes an
    // unsigned host (Themida anti-tamper). So rather than load BambuStudio.dll in this
    // process, re-launch under a genuine Bambu-signed host: a copy of bambu-studio.exe
    // sitting in OUR folder. That genuine host loads OUR BambuStudio.dll (same folder)
    // and calls bambustu_main -> our app, and our resources resolve from our folder
    // because the running exe (program_location) is here. The installer drops the copy
    // in; as a fallback we copy it on first run from the Bambu Studio install found in
    // the registry. Guard on our own name so the host copy never recurses.
    if (_wcsicmp(fname, L"bambu-studio") != 0) {
        wchar_t host_exe[MAX_PATH + 1] = { 0 };
        wcscpy(host_exe, path_to_exe);
        wcscat(host_exe, L"bambu-studio.exe");

        if (::GetFileAttributesW(host_exe) == INVALID_FILE_ATTRIBUTES && !bridge_skip_forever()) {
            // Self-heal: copy the genuine host + DLL out of an installed Bambu Studio.
            // Works without elevation for a writable (e.g. portable) install.
            StageResult r = stage_genuine_files(path_to_exe);
            if (r == StageResult::WriteFailed) {
                // Bambu Studio is installed but our dir (Program Files) isn't writable by a
                // normal user. Ask the user BEFORE prompting for UAC. This is also the path
                // that lights up Bambu mode when Bambu Studio is installed AFTER OrcaSlicer.
                int choice = ask_stage_bridge();        // 1=Yes  2=skip now  3=skip forever
                if (choice == 1)
                    elevate_and_stage(self_exe);        // UAC consent -> elevated staging
                else if (choice == 3)
                    set_bridge_skip_forever();          // remember "don't ask again"
                // choice == 2 (skip for now) -> ask again on the next launch
            }
            // StageResult::NoBambu -> no Bambu Studio installed; fall through to vanilla.
        }

        if (::GetFileAttributesW(host_exe) != INVALID_FILE_ATTRIBUTES) {
            // The genuine host loads its studio dll by the fixed name BambuStudio.dll;
            // ensure it exists (a vanilla install may ship it as OrcaSlicer.dll).
            {
                wchar_t studio_dll[MAX_PATH + 1] = { 0 };
                wcscpy(studio_dll, path_to_exe); wcscat(studio_dll, L"BambuStudio.dll");
                if (::GetFileAttributesW(studio_dll) == INVALID_FILE_ATTRIBUTES) {
                    wchar_t orca_dll[MAX_PATH + 1] = { 0 };
                    wcscpy(orca_dll, path_to_exe); wcscat(orca_dll, L"OrcaSlicer.dll");
                    if (::GetFileAttributesW(orca_dll) != INVALID_FILE_ATTRIBUTES)
                        ::CopyFileW(orca_dll, studio_dll, FALSE);
                }
            }
            std::wstring cmd = L"\"";
            cmd += host_exe;
            cmd += L"\"";
            for (int i = 1; i < argc; ++i) { cmd += L" \""; cmd += argv[i]; cmd += L"\""; }
            std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
            cmdbuf.push_back(L'\0');
            // Launch the genuine host with its PARENT re-pointed to the shell
            // (explorer.exe), so the plugin's Themida anti-tamper never sees our
            // unsigned orca-slicer.exe as the parent process (which makes it crash).
            HANDLE hParent = nullptr;
            DWORD shellPid = 0;
            ::GetWindowThreadProcessId(::GetShellWindow(), &shellPid);
            if (shellPid)
                hParent = ::OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shellPid);

            PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
            BOOL launched = FALSE;

            if (hParent) {
                STARTUPINFOEXW six; ZeroMemory(&six, sizeof(six));
                six.StartupInfo.cb = sizeof(six);
                SIZE_T attrSize = 0;
                ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
                six.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)
                    ::HeapAlloc(::GetProcessHeap(), 0, attrSize);
                if (six.lpAttributeList
                    && ::InitializeProcThreadAttributeList(six.lpAttributeList, 1, 0, &attrSize)
                    && ::UpdateProcThreadAttribute(six.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &hParent, sizeof(hParent),
                            nullptr, nullptr)) {
                    launched = ::CreateProcessW(host_exe, cmdbuf.data(), nullptr, nullptr, FALSE,
                                                EXTENDED_STARTUPINFO_PRESENT, nullptr, path_to_exe,
                                                &six.StartupInfo, &pi);
                }
                if (six.lpAttributeList) {
                    ::DeleteProcThreadAttributeList(six.lpAttributeList);
                    ::HeapFree(::GetProcessHeap(), 0, six.lpAttributeList);
                }
                ::CloseHandle(hParent);
            }

            if (!launched) {   // fallback: plain launch if re-parenting was unavailable
                STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
                launched = ::CreateProcessW(host_exe, cmdbuf.data(), nullptr, nullptr, FALSE,
                                            0, nullptr, path_to_exe, &si, &pi);
            }

            if (launched) {
                ::CloseHandle(pi.hThread);
                ::CloseHandle(pi.hProcess);
                return 0;   // the genuine host (parented to explorer) now runs our app
            }
        }
        // No Bambu Studio host present (or fork failed): fall through and load the
        // studio dll in-process -> plain OrcaSlicer ("vanilla mode", no Bambu network).
    }
    // ---------------------------------------------------------------------------


#ifdef SLIC3R_GUI
// https://wiki.qt.io/Cross_compiling_Mesa_for_Windows
// http://download.qt.io/development_releases/prebuilt/llvmpipe/windows/
    if (load_mesa) {
        opengl_version_check.unload_opengl_dll();
        wchar_t path_to_mesa[MAX_PATH + 1] = { 0 };
        wcscpy(path_to_mesa, path_to_exe);
        wcscat(path_to_mesa, L"mesa\\opengl32.dll");
        printf("Loading MESA OpenGL library: %S\n", path_to_mesa);
        HINSTANCE hInstance_OpenGL = LoadLibraryExW(path_to_mesa, nullptr, 0);
        if (hInstance_OpenGL == nullptr) {
            printf("MESA OpenGL library was not loaded\n");
        } else
            printf("MESA OpenGL library was loaded successfully\n");
    }
#endif /* SLIC3R_GUI */


    // The studio DLL ships as BambuStudio.dll (the genuine name) so the network
    // plugin's name-based studio lookups — used on the control-command signing
    // path — resolve to our module. Its signature is handled by the redirect.
    wchar_t path_to_slic3r[MAX_PATH + 1] = { 0 };
    wcscpy(path_to_slic3r, path_to_exe);
    wcscat(path_to_slic3r, L"OrcaSlicer.dll");
//	printf("Loading Slic3r library: %S\n", path_to_slic3r);
    HINSTANCE hInstance_Slic3r = LoadLibraryExW(path_to_slic3r, nullptr, 0);
    if (hInstance_Slic3r == nullptr) {
        // a Bambu install ships the studio dll as BambuStudio.dll -- try that name too.
        wcscpy(path_to_slic3r, path_to_exe); wcscat(path_to_slic3r, L"BambuStudio.dll");
        hInstance_Slic3r = LoadLibraryExW(path_to_slic3r, nullptr, 0);
    }
    if (hInstance_Slic3r == nullptr) {
        printf("BambuStudio.dll was not loaded, error=%d\n", GetLastError());
        return -1;
    }

    // resolve function address here — match BambuStudio's entry name (bambustu_main)
    orcaslicer_main = (Slic3rMainFunc)GetProcAddress(hInstance_Slic3r,
#ifdef _WIN64
        // there is just a single calling conversion, therefore no mangling of the function name.
        "bambustu_main"
#else	// stdcall calling convention declaration
        "_bambustu_main@8"
#endif
        );
    if (orcaslicer_main == nullptr) {
        printf("could not locate the function bambustu_main in BambuStudio.dll\n");
        return -1;
    }
    // argc minus the trailing nullptr of the argv
    return orcaslicer_main((int)argv_extended.size() - 1, argv_extended.data());
}
}
