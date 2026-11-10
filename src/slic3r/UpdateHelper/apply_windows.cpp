#include "slic3r/UpdateHelper/InstallPlatform.hpp"

#if defined(_WIN32)

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
    #include <softpub.h>
    #include <wincrypt.h>
    #include <wintrust.h>

    #if defined(_MSC_VER)
        #pragma comment(lib, "wintrust.lib")
    #endif

    #include <cstdio>
    #include <string>
    #include <system_error>
    #include <vector>

    #include <boost/filesystem.hpp>

    #include "libslic3r/UpdateHandoff.hpp"

#endif

namespace Slic3r {
namespace Updater {

#if defined(_WIN32)

namespace {

namespace fs = boost::filesystem;

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

std::string narrow_utf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    out.resize(static_cast<size_t>(needed) - 1);
    return out;
}

// Handoff paths travel as UTF-8; boost::filesystem on Windows takes UTF-16.
fs::path to_path(const std::string& utf8)
{
    return fs::path(widen_utf8(utf8));
}

bool run_and_wait(const std::wstring& exe, const std::wstring& args, int timeout_seconds, DWORD& exit_code, std::string& error)
{
    std::wstring cmdline = args.empty() ? exe : exe + L" " + args;
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup;
    ::ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    ::ZeroMemory(&process, sizeof(process));

    if (!::CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &process)) {
        error = "Could not run " + narrow_utf8(exe) + " (error " + std::to_string(static_cast<long long>(::GetLastError())) + ").";
        return false;
    }

    const DWORD waited = ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout_seconds) * 1000u);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 1);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        error = "A step of the update took too long and was stopped.";
        return false;
    }
    if (!::GetExitCodeProcess(process.hProcess, &exit_code)) {
        exit_code = 1;
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        error = "A step of the update ended without a result.";
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

bool extract_zip(const std::string& zip_path, const std::string& destination, std::string& error)
{
    // tar.exe (bsdtar) ships with Windows 10 and later; no COM shell setup or
    // archive library needed, and it handles long paths and UTF-8 names.
    const std::wstring args = L"-xf \"" + widen_utf8(zip_path) + L"\" -C \"" + widen_utf8(destination) + L"\"";
    DWORD              exit_code = 1;
    if (!run_and_wait(L"tar.exe", args, 1800, exit_code, error))
        return false;
    if (exit_code != 0) {
        error = "The downloaded archive could not be unpacked (tar exit code " + std::to_string(static_cast<long long>(exit_code)) + ").";
        return false;
    }
    return true;
}

// Checks that the downloaded installer carries a valid Authenticode signature.
// Publisher pinning needs the expected certificate subject from CI and is a
// follow-up; this only proves the file is intact and signed by someone trusted.
bool verify_signature(const std::string& artifact_path, std::string& error)
{
    const std::wstring file = widen_utf8(artifact_path);
    if (file.empty()) {
        error = "The installer path is not valid.";
        return false;
    }

    WINTRUST_FILE_INFO file_info;
    ::ZeroMemory(&file_info, sizeof(file_info));
    file_info.cbStruct      = sizeof(file_info);
    file_info.pcwszFilePath = file.c_str();

    WINTRUST_DATA trust_data;
    ::ZeroMemory(&trust_data, sizeof(trust_data));
    trust_data.cbStruct            = sizeof(trust_data);
    trust_data.dwUIChoice          = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice       = WTD_CHOICE_FILE;
    trust_data.pFile               = &file_info;
    trust_data.dwStateAction       = WTD_STATEACTION_VERIFY;

    GUID       action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = ::WinVerifyTrust(nullptr, &action, &trust_data);

    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(nullptr, &action, &trust_data);

    if (status != ERROR_SUCCESS) {
        char buffer[16] = { 0 };
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(status));
        error = std::string("The installer's digital signature could not be verified (status ") + buffer +
                "), so it was not run.";
        return false;
    }
    return true;
}

bool run_installer(const std::string& artifact_path, const std::string& install_root, std::string& error)
{
    // Never hand an unverified executable to the administrator prompt.
    if (!verify_signature(artifact_path, error))
        return false;

    // /S silent, /D= the install directory. NSIS requires /D= to be last and
    // unquoted, and it must not have a trailing backslash.
    std::string root = install_root;
    while (root.size() > 3 && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();

    const std::wstring parameters = L"/S /D=" + widen_utf8(root);
    const std::wstring file       = widen_utf8(artifact_path);
    const std::wstring directory  = widen_utf8(root);

    SHELLEXECUTEINFOW info;
    ::ZeroMemory(&info, sizeof(info));
    info.cbSize       = sizeof(info);
    info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb       = L"runas";
    info.lpFile       = file.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory  = directory.c_str();
    info.nShow        = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info)) {
        const DWORD last = ::GetLastError();
        if (last == ERROR_CANCELLED)
            error = "The update was cancelled at the administrator prompt.";
        else
            error = "The installer could not be started (error " + std::to_string(static_cast<long long>(last)) + ").";
        return false;
    }
    if (info.hProcess == nullptr) {
        error = "The installer started but could not be followed, so the update is unconfirmed.";
        return false;
    }

    const DWORD waited = ::WaitForSingleObject(info.hProcess, 30u * 60u * 1000u);
    DWORD       exit_code = 1;
    const BOOL  got_exit = ::GetExitCodeProcess(info.hProcess, &exit_code);
    ::CloseHandle(info.hProcess);

    if (waited != WAIT_OBJECT_0) {
        error = "The installer took too long and the update is unconfirmed.";
        return false;
    }
    if (!got_exit || exit_code != 0) {
        error = "The installer reported a failure (exit code " + std::to_string(static_cast<long long>(got_exit ? exit_code : 1)) + ").";
        return false;
    }
    return true;
}

} // namespace

bool apply_windows_install(const UpdateHandoff& handoff, const std::string& artifact_path, ProgressSink& sink, std::string& error)
{
    if (sink.cancelled && sink.cancelled()) {
        error = "cancelled";
        return false;
    }

    const std::string label = handoff.version_label.empty() ? std::string("the new version") : handoff.version_label;
    sink.say(Stage::Installing, 0, "Installing " + label);

    const std::string install_root = handoff.install_root;
    if (install_root.empty()) {
        error = "The installation folder is not known, so the update cannot be installed.";
        return false;
    }

    boost::system::error_code ec;
    if (!fs::is_directory(to_path(install_root), ec)) {
        error = "The installed application was not found at " + install_root + ".";
        return false;
    }
    if (!fs::is_regular_file(to_path(artifact_path), ec)) {
        error = "The downloaded file is missing, so the update cannot be installed.";
        return false;
    }

    if (handoff.flavour == "windows_installer") {
        // No backup folder here on purpose: the installer removes and rewrites
        // the whole tree itself, and a pre-rename would make its uninstall
        // step fail. The pending record still marks the install as in flight.
        PendingInstall pending;
        pending.target  = install_root;
        pending.digest  = handoff.digest;
        pending.channel = handoff.channel;
        pending.version = handoff.version;
        if (!write_pending_install(pending)) {
            error = "The update could not be recorded, so nothing was changed.";
            return false;
        }

        sink.say(Stage::Installing, 30, "Running the installer");
        if (!run_installer(artifact_path, install_root, error)) {
            remove_pending_install();
            return false;
        }

        sink.say(Stage::Launching, 90, "Starting " + label);

        // Recorded before the launch: a crash while the new build starts still
        // leaves the application knowing what it is running.
        InstallRecord record;
        record.digest  = handoff.digest;
        record.channel = handoff.channel;
        record.version = handoff.version;
        write_install_record(record);

        const std::string exe_path = handoff.exe_path.empty() ? install_root + "\\OrcaSlicer.exe" : handoff.exe_path;
        if (!launch_detached(exe_path, error)) {
            remove_path_quietly(updater_install_record_path());
            remove_pending_install();
            error += " The installer finished, but the new version did not start.";
            return false;
        }
        return true;
    }

    // Portable build: a zip that unpacks beside the install folder, with the
    // portable user data kept inside that folder.
    const std::string pid        = std::to_string(static_cast<long long>(::GetCurrentProcessId()));
    const std::string staged     = install_root + ".staged-" + pid;
    const std::string backup     = install_root + ".backup-" + pid;
    const std::string data_dir   = install_root + "\\data_dir";
    // A sibling of both trees, so `data_dir` can leave the install folder
    // before anything is renamed or deleted there.
    const std::string data_aside = install_root + ".data-" + pid;
    const bool        has_data   = fs::is_directory(to_path(data_dir), ec);

    remove_path_quietly(staged);
    if (fs::exists(to_path(data_aside), ec)) {
        error = "The update could not be prepared: " + data_aside + " already exists.";
        return false;
    }
    fs::create_directories(to_path(staged), ec);
    if (!fs::is_directory(to_path(staged), ec)) {
        error = "The update could not be prepared: " + staged + " could not be created.";
        return false;
    }

    sink.say(Stage::Installing, 30, "Unpacking " + label);
    if (!extract_zip(artifact_path, staged, error)) {
        remove_path_quietly(staged);
        return false;
    }

    // Written before anything moves: the application restores the backup on its
    // next start if we die between "old moved aside" and "new in place". The
    // aside path is recorded so recovery can find the user's data even when the
    // process dies after moving it out of the tree.
    PendingInstall pending;
    pending.backup     = backup;
    pending.target     = install_root;
    pending.digest     = handoff.digest;
    pending.channel    = handoff.channel;
    pending.version    = handoff.version;
    pending.data_aside = has_data ? data_aside : std::string();
    if (!write_pending_install(pending)) {
        remove_path_quietly(staged);
        error = "The update could not be recorded, so nothing was changed.";
        return false;
    }

    // `data_dir` is at its original place, at `data_aside`, or inside the new
    // tree. Nothing that may hold it is deleted before it has been moved back.
    bool data_aside_ready = false;
    bool data_placed      = false;
    bool moved_old        = false;
    bool placed_new       = false;

    // Restores the previous tree and always puts the user's data back into it.
    // Returns true only when nothing is left to recover, which is the only case
    // where the pending record may be deleted. On failure `problem` says what is
    // still out of place and no tree that may hold the data has been touched.
    auto rollback = [&](std::string& problem) -> bool {
        boost::system::error_code ignore;

        if (data_placed) {
            // Pull the user's data out of the new tree before removing it.
            ignore.clear();
            fs::rename(to_path(data_dir), to_path(data_aside), ignore);
            if (ignore) {
                problem = "Your portable settings are still inside " + install_root + " and could not be moved aside (" + ignore.message() + ")";
                return false; // the new tree holds the data, so it is not deleted
            }
            data_placed      = false;
            data_aside_ready = true;
        }

        if (placed_new) {
            ignore.clear();
            fs::remove_all(to_path(install_root), ignore);
            if (ignore) {
                problem = "The new version could not be removed from " + install_root + " (" + ignore.message() + ")";
                return false;
            }
            placed_new = false;
        }

        if (moved_old) {
            ignore.clear();
            fs::rename(to_path(backup), to_path(install_root), ignore);
            if (ignore) {
                problem = "The previous version is still at " + backup + " and could not be moved back (" + ignore.message() + ")";
                return false;
            }
            moved_old = false;
        }

        if (data_aside_ready) {
            if (!fs::is_directory(to_path(install_root), ignore)) {
                problem = "Your portable settings are still at " + data_aside + " because the installation folder is missing";
                return false;
            }
            ignore.clear();
            fs::rename(to_path(data_aside), to_path(data_dir), ignore);
            if (ignore) {
                problem = "Your portable settings are still at " + data_aside + " and could not be moved back (" + ignore.message() + ")";
                return false;
            }
            data_aside_ready = false;
        }

        // Everything is back: only now is there nothing left to recover.
        remove_path_quietly(staged);
        remove_path_quietly(updater_install_record_path());
        remove_pending_install();
        return true;
    };

    // Rolls back and keeps the pending record when anything is still out of
    // place, so the application can retry on its next start.
    auto fail = [&](std::string message) -> bool {
        std::string problem;
        if (!rollback(problem))
            message += " " + problem + ", so the update was left recorded for the next start.";
        error = message;
        return false;
    };

    if (has_data) {
        fs::rename(to_path(data_dir), to_path(data_aside), ec);
        if (ec)
            return fail("Your portable settings could not be moved aside: " + ec.message() + ".");
        data_aside_ready = true;
    }

    sink.say(Stage::Installing, 70, "Replacing the installation folder");

    ec.clear();
    fs::rename(to_path(install_root), to_path(backup), ec);
    if (ec)
        return fail("The current installation could not be moved aside: " + ec.message() + ".");
    moved_old = true;

    ec.clear();
    fs::rename(to_path(staged), to_path(install_root), ec);
    if (ec)
        return fail("The new version could not be put in place: " + ec.message() + ".");
    placed_new = true;

    if (data_aside_ready) {
        ec.clear();
        fs::rename(to_path(data_aside), to_path(data_dir), ec);
        if (ec)
            return fail("Your portable settings could not be moved into the new version: " + ec.message() + ".");
        data_aside_ready = false;
        data_placed      = true;
    }

    sink.say(Stage::Launching, 90, "Starting " + label);

    // Recorded before the launch: a crash while the new build starts still
    // leaves the application knowing what it is running.
    InstallRecord record;
    record.digest  = handoff.digest;
    record.channel = handoff.channel;
    record.version = handoff.version;
    write_install_record(record);

    const std::string exe_path = handoff.exe_path.empty() ? install_root + "\\OrcaSlicer.exe" : handoff.exe_path;
    if (!launch_detached(exe_path, error)) {
        std::string problem;
        if (rollback(problem))
            error += " The previous version was put back.";
        else
            error += " " + problem + ", so the update was left recorded for the next start.";
        return false;
    }

    // Success: the backup and the pending record stay behind on purpose; the
    // application cleans both up on its next start.
    return true;
}

#else

bool apply_windows_install(const UpdateHandoff&, const std::string&, ProgressSink&, std::string& error)
{
    error = "Windows installs are not supported on this platform.";
    return false;
}

#endif

} // namespace Updater
} // namespace Slic3r
