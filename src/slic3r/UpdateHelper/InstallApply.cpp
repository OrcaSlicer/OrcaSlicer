#include "slic3r/UpdateHelper/InstallApply.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cerrno>
#include <cstring>
#include <libslic3r/UpdateHandoff.hpp>
#include <string>
#include <system_error>

#include <boost/filesystem.hpp>

#include "slic3r/UpdateHelper/InstallPlatform.hpp"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <vector>
#else
    #include <spawn.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <vector>
    extern char** environ;
#endif

namespace Slic3r {
namespace Updater {

const char* stage_key(Stage stage)
{
    switch (stage) {
    case Stage::Idle:        return "idle";
    case Stage::Downloading: return "downloading";
    case Stage::Verifying:   return "verifying";
    case Stage::Installing:  return "installing";
    case Stage::Launching:   return "launching";
    case Stage::Failed:      return "failed";
    case Stage::Done:        return "done";
    }
    return "unknown";
}

bool apply_update(const UpdateHandoff& handoff,
                  const std::string&   artifact_path,
                  ProgressSink&        sink,
                  std::string&         error)
{
    if (handoff.flavour == "windows_installer" || handoff.flavour == "windows_portable")
        return apply_windows_install(handoff, artifact_path, sink, error);
    if (handoff.flavour == "macos_bundle")
        return apply_macos_install(handoff, artifact_path, sink, error);
    if (handoff.flavour == "linux_appimage")
        return apply_linux_install(handoff, artifact_path, sink, error);
    error = "This installation cannot update itself.";
    return false;
}

void remove_path_quietly(const std::string& path)
{
    if (path.empty())
        return;
    try {
        boost::system::error_code ec;
        boost::filesystem::remove_all(boost::filesystem::path(path), ec);
    } catch (...) {
        // Best effort: the caller is finishing an install, a leftover file must
        // not turn that into a failure.
    }
}

#if defined(_WIN32)

namespace {

std::wstring widen_utf8(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
    out.resize(static_cast<size_t>(needed) - 1);
    return out;
}

} // namespace

#endif

bool launch_detached(const std::string& exe_path, std::string& error)
{
    if (exe_path.empty()) {
        error = "There is no program to start.";
        return false;
    }

#if defined(_WIN32)
    const std::wstring exe = widen_utf8(exe_path);
    if (exe.empty()) {
        error = "The program path is not valid.";
        return false;
    }

    std::wstring cmdline = L"\"" + exe + L"\"";
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0');

    std::wstring work_dir;
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0)
        work_dir = exe.substr(0, slash);

    STARTUPINFOW startup;
    ::ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    ::ZeroMemory(&process, sizeof(process));

    const BOOL started = ::CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                                          DETACHED_PROCESS, nullptr, work_dir.empty() ? nullptr : work_dir.c_str(),
                                          &startup, &process);
    if (!started) {
        error = "The new version could not be started (error " + std::to_string(static_cast<long long>(::GetLastError())) + ").";
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#else
    // posix_spawn reports a failed exec in the child back through its return
    // value, so callers can roll back instead of believing a build started.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe_path.c_str()));
    argv.push_back(nullptr);

    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    pid_t     pid = 0;
    const int rc  = ::posix_spawn(&pid, exe_path.c_str(), nullptr, &attr, argv.data(), environ);
    ::posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = std::string("The new version could not be started: ") + std::strerror(rc) + ".";
        return false;
    }
    return true;
#endif
}

} // namespace Updater
} // namespace Slic3r
