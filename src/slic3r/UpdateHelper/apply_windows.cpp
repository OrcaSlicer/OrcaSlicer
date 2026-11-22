#include "slic3r/UpdateHelper/InstallPlatform.hpp"
#include <libslic3r/UpdateHandoff.hpp>
#include <slic3r/UpdateHelper/InstallApply.hpp>

#if defined(_WIN32)

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
    #include <softpub.h>
    #include <wincrypt.h>
    #include <wintrust.h>
    #include <tlhelp32.h>

    #if defined(_MSC_VER)
        #pragma comment(lib, "crypt32.lib")
        #pragma comment(lib, "wintrust.lib")
    #endif

    #include <wchar.h>

    #include <chrono>
    #include <cstdio>
    #include <cwchar>
    #include <string>
    #include <thread>
    #include <vector>

    #include <boost/filesystem.hpp>
    #include <boost/log/trivial.hpp>
    #include <boost/nowide/convert.hpp>

#endif

namespace Slic3r {
namespace Updater {

#if defined(_WIN32)

namespace {

namespace fs = boost::filesystem;

// Handoff paths travel as UTF-8; boost::filesystem on Windows takes UTF-16.
fs::path to_path(const std::string& utf8)
{
    return fs::path(boost::nowide::widen(utf8));
}

// The display name of the certificate that signed the embedded PKCS#7 message.
// Empty when the message or the certificate cannot be read.
std::wstring signer_display_name(HCERTSTORE store, HCRYPTMSG message)
{
    DWORD signer_size = 0;
    if (!::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signer_size) || signer_size == 0)
        return std::wstring();

    std::vector<BYTE> buffer(signer_size);
    if (!::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &signer_size))
        return std::wstring();
    const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(buffer.data());

    // CERT_FIND_SUBJECT_CERT keys on the issuer and serial number of the signer.
    CERT_INFO cert_info;
    ::ZeroMemory(&cert_info, sizeof(cert_info));
    cert_info.Issuer       = signer->Issuer;
    cert_info.SerialNumber = signer->SerialNumber;

    PCCERT_CONTEXT cert = ::CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                       CERT_FIND_SUBJECT_CERT, &cert_info, nullptr);
    if (cert == nullptr)
        return std::wstring();

    const DWORD size = ::CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
    std::wstring name;
    if (size > 1) {
        name.resize(static_cast<size_t>(size) - 1);
        if (::CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), size) == 0)
            name.clear();
    }
    ::CertFreeCertificateContext(cert);
    return name;
}

// Checks that the downloaded installer carries a valid Authenticode signature
// by the pinned publisher. An empty ORCA_UPDATER_PUBLISHER accepts any trusted
// signer with a warning, which is what a local build wants.
bool verify_signature(const std::string& artifact_path, std::string& error)
{
    const std::wstring file = boost::nowide::widen(artifact_path);
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
    trust_data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust_data.dwUnionChoice       = WTD_CHOICE_FILE;
    trust_data.pFile               = &file_info;
    trust_data.dwStateAction       = WTD_STATEACTION_VERIFY;

    GUID       action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = ::WinVerifyTrust(nullptr, &action, &trust_data);

    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(nullptr, &action, &trust_data);

    // A revocation server that cannot be reached is not a bad installer; the
    // signature and chain were already verified above.
    const bool revocation_unreachable = status == CRYPT_E_REVOCATION_OFFLINE || status == CERT_E_REVOCATION_FAILURE;
    if (status != ERROR_SUCCESS && !revocation_unreachable) {
        char buffer[16] = { 0 };
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(status));
        error = std::string("The installer's digital signature could not be verified (status ") + buffer +
                "), so it was not run.";
        return false;
    }

    // WinVerifyTrust accepts any trusted signer, so the signer identity is read
    // from the embedded PKCS#7 message and compared with the pinned publisher.
    const std::wstring expected = boost::nowide::widen(ORCA_UPDATER_PUBLISHER);
    if (expected.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "The update publisher is not pinned in this build; any trusted signer is accepted.";
        return true;
    }

    HCERTSTORE   store    = nullptr;
    HCRYPTMSG    message  = nullptr;
    DWORD        encoding = 0;
    DWORD        content  = 0;
    DWORD        format   = 0;
    std::wstring actual;
    if (::CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                           CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format, &store, &message, nullptr))
        actual = signer_display_name(store, message);
    if (message != nullptr)
        ::CryptMsgClose(message);
    if (store != nullptr)
        ::CertCloseStore(store, 0);

    if (actual.empty()) {
        error = "The installer's signature could not be read, so it was not run.";
        return false;
    }
    if (_wcsicmp(actual.c_str(), expected.c_str()) != 0) {
        error = "The update is signed by \"" + boost::nowide::narrow(actual) + "\", not by " + ORCA_UPDATER_PUBLISHER +
                ", so it was not run.";
        return false;
    }
    return true;
}

// True when this user can write inside the install root. A per-user tree is
// writable and a protected tree is not, whatever the uninstall record says.
// The probe file goes away with its handle.
bool install_root_writable(const std::wstring& root)
{
    std::wstring probe = root;
    if (!probe.empty() && probe.back() != L'\\')
        probe += L'\\';
    probe += L".orca-write-test-" + std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId()));

    const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    ::CloseHandle(file);
    return true;
}

// One string value from the machine hive. The view selects the 64-bit or the
// 32-bit registry: installers built before this helper were 32-bit and wrote
// under WOW6432Node.
std::wstring read_machine_value(const std::wstring& key_path, const wchar_t* name, REGSAM view)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS)
        return std::wstring();

    std::wstring value;
    DWORD        type = 0;
    DWORD        size = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t)) {
        value.resize(size / sizeof(wchar_t));
        if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size) == ERROR_SUCCESS) {
            while (!value.empty() && value.back() == L'\0')
                value.pop_back();
            // The installer expands this type before it reads the record, so
            // comparing the raw value here would name a different tree.
            if (type == REG_EXPAND_SZ && !value.empty()) {
                const DWORD needed = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
                if (needed > 1) {
                    std::wstring expanded(needed, L'\0');
                    const DWORD  written = ::ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
                    if (written > 1 && written <= needed) {
                        expanded.resize(written - 1);
                        value = expanded;
                    }
                }
            }
        } else {
            value.clear();
        }
    }
    ::RegCloseKey(key);
    return value;
}

// Trailing separators are not part of the tree: "C:\Apps\OrcaSlicer\" and
// "C:\Apps\OrcaSlicer" name one directory.
std::wstring without_trailing_separators(std::wstring path)
{
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

// Whole directories, case-insensitively: "C:\Apps\Orca" is not "C:\Apps\OrcaSlicer".
bool same_directory(const std::wstring& left, const std::wstring& right)
{
    const std::wstring a = without_trailing_separators(left);
    const std::wstring b = without_trailing_separators(right);
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// An UninstallString is the path of the uninstaller, quoted by this installer
// and by some older ones, so the tree is what remains once the quotes and the
// file name are removed.
std::wstring uninstall_string_root(const std::wstring& value)
{
    std::wstring path = value;
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);

    const std::wstring suffix = L"\\uninstall.exe";
    if (path.size() > suffix.size() && _wcsicmp(path.c_str() + (path.size() - suffix.size()), suffix.c_str()) == 0)
        path.resize(path.size() - suffix.size());
    return path;
}

// The tree the machine hive records, resolved exactly as the installer resolves it:
// the 64-bit view first, then WOW6432Node; InstallLocation, then the CPack-era legacy
// value, then UninstallString. Empty when nothing is recorded. The record is what makes
// an install machine-wide; it does not say the tree is protected, because an
// administrator can install into a folder this user can write.
std::wstring machine_install_root()
{
    // The key names packaging/windows/orca-slicer.nsi writes, and the one the
    // CPack-era installer wrote its tree into instead.
    const std::wstring uninstall_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OrcaSlicer";
    const std::wstring legacy_key    = L"Software\\SoftFever\\OrcaSlicer";
    const REGSAM       views[]       = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

    for (const REGSAM view : views) {
        std::wstring root = read_machine_value(uninstall_key, L"InstallLocation", view);
        if (!root.empty())
            return root;
        // The CPack-era installer wrote only this default value.
        root = read_machine_value(legacy_key, nullptr, view);
        if (!root.empty())
            return root;
        const std::wstring uninstall = read_machine_value(uninstall_key, L"UninstallString", view);
        if (!uninstall.empty())
            return uninstall_string_root(uninstall);
    }
    return std::wstring();
}

// True when a record in either view names this tree, in any of the three forms. The
// installer only updates the tree its own resolution names, so a tree recorded in the
// other view is refused rather than updated by the wrong rule.
bool machine_record_names(const std::wstring& root)
{
    if (root.empty())
        return false;

    const std::wstring uninstall_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OrcaSlicer";
    const std::wstring legacy_key    = L"Software\\SoftFever\\OrcaSlicer";
    const REGSAM       views[]       = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

    for (const REGSAM view : views) {
        if (same_directory(root, read_machine_value(uninstall_key, L"InstallLocation", view)))
            return true;
        if (same_directory(root, read_machine_value(legacy_key, nullptr, view)))
            return true;
        const std::wstring uninstall = read_machine_value(uninstall_key, L"UninstallString", view);
        if (!uninstall.empty() && same_directory(root, uninstall_string_root(uninstall)))
            return true;
    }
    return false;
}

// %LOCALAPPDATA%\Programs\OrcaSlicer: the tree a first double-click install gets, and the
// one the installer cannot tell from an explicit /D= while a machine record exists.
std::wstring per_user_directory()
{
    const DWORD size = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0)
        return std::wstring();

    std::wstring base(size, L'\0');
    const DWORD  written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), size);
    if (written == 0 || written >= size)
        return std::wstring();
    base.resize(written);
    return base + L"\\Programs\\OrcaSlicer";
}

// The helper's own directory. It is never the install tree, which the old
// build's uninstaller removes while the installer runs.
std::wstring current_directory()
{
    const DWORD size = ::GetCurrentDirectoryW(0, nullptr);
    if (size == 0)
        return std::wstring();

    std::wstring directory(size, L'\0');
    const DWORD  written = ::GetCurrentDirectoryW(size, directory.data());
    if (written == 0 || written >= size)
        return std::wstring();
    directory.resize(written);
    return directory;
}

enum class InstallerResult { Done, Failed, StillRunning };

// Waits for the installer to exit, and takes ownership of its handle. A timeout is
// not a failure: the installer is still writing the tree, and stopping it here would
// leave that tree half written with nothing to finish it.
InstallerResult wait_for_installer(HANDLE process, std::string& error)
{
    const DWORD waited = ::WaitForSingleObject(process, 30u * 60u * 1000u);
    if (waited == WAIT_TIMEOUT) {
        ::CloseHandle(process);
        BOOST_LOG_TRIVIAL(warning) << "The installer is still running after its timeout; leaving it to finish.";
        return InstallerResult::StillRunning;
    }

    DWORD      exit_code = 1;
    const BOOL got_exit  = ::GetExitCodeProcess(process, &exit_code);
    ::CloseHandle(process);

    if (waited != WAIT_OBJECT_0) {
        error = "The installer could not be followed to its end.";
        return InstallerResult::Failed;
    }
    if (!got_exit || exit_code != 0) {
        if (got_exit && exit_code == 2) {
            // The installer's own code for "nothing was written": the tree was in use or the
            // stage could not be created. Its silent message never reaches the user.
            error = "The installer made no changes: another OrcaSlicer window kept the folder in use, "
                    "or the new files could not be written.";
        } else {
            error = "The installer reported a failure (exit code " +
                    std::to_string(static_cast<long long>(got_exit ? exit_code : 1)) + ").";
        }
        return InstallerResult::Failed;
    }
    return InstallerResult::Done;
}

// The final spelling of a directory, with the \\?\ prefix removed, or empty when it
// cannot be read. A junction, a shortcut or a substituted drive resolves to the path
// it points at, which is not the path that was asked for.
std::wstring resolved_directory(const std::wstring& directory)
{
    const HANDLE handle = ::CreateFileW(directory.c_str(), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::wstring();

    std::wstring  buffer(MAX_PATH, L'\0');
    DWORD         length = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (length >= buffer.size()) {
        buffer.resize(length);
        length = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    }
    ::CloseHandle(handle);
    if (length == 0 || length >= buffer.size())
        return std::wstring();

    buffer.resize(length);
    if (buffer.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        return L"\\\\" + buffer.substr(8);
    if (buffer.compare(0, 4, L"\\\\?\\") == 0)
        return buffer.substr(4);
    return buffer;
}

// A tree a machine record names is updated through an elevated installer even when this
// user can write to it. The uninstaller the administrator then runs from that tree is one
// this user could have replaced, and RMDir /r and File /r follow junctions: check that the
// tree is the one that was asked for and that its uninstaller carries the pinned publisher.
bool check_writable_machine_tree(const std::wstring& directory, std::string& error)
{
    const DWORD attributes = ::GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "The installation folder could not be read, so the update was not run.";
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "The installation folder is a link to another folder, so the update was not run.";
        return false;
    }

    const std::wstring resolved = resolved_directory(directory);
    if (resolved.empty()) {
        error = "The final path of the installation folder could not be read, so the update was not run.";
        return false;
    }
    if (!same_directory(resolved, directory)) {
        error = "The installation folder is not where it is recorded (" + boost::nowide::narrow(resolved) +
                "), so the update was not run.";
        return false;
    }

    const std::string uninstaller = boost::nowide::narrow(directory) + "\\uninstall.exe";
    boost::system::error_code ec;
    if (!fs::is_regular_file(to_path(uninstaller), ec)) {
        error = "The uninstaller of the all-users install is missing, so the update was not run.";
        return false;
    }
    return verify_signature(uninstaller, error);
}

InstallerResult run_installer(const std::string& artifact_path, const std::string& install_root, std::string& error)
{
    // Never hand an unverified executable to the administrator prompt.
    if (!verify_signature(artifact_path, error))
        return InstallerResult::Failed;

    // /S silent, /D= the install directory. NSIS requires /D= to be last and
    // unquoted, and it must not have a trailing backslash.
    std::string root = install_root;
    while (root.size() > 3 && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();

    const std::wstring parameters        = L"/S /D=" + boost::nowide::widen(root);
    const std::wstring file              = boost::nowide::widen(artifact_path);
    const std::wstring directory         = boost::nowide::widen(root);
    const std::wstring working_directory = current_directory();
    const std::wstring machine_root      = machine_install_root();
    const bool         recorded_machine  = same_directory(directory, machine_root);
    const bool         tree_writable     = install_root_writable(directory);

    // A per-user copy at the default path cannot be told from a first install once a machine
    // record exists: the installer would retarget the recorded tree, the helper would record
    // this one. Refuse instead of writing a tree the caller did not ask for.
    if (!machine_root.empty() && !recorded_machine && same_directory(directory, per_user_directory())) {
        error = "OrcaSlicer is installed for all users in " + boost::nowide::narrow(machine_root) +
                ". Update that copy, or uninstall it first.";
        return InstallerResult::Failed;
    }

    // A tree recorded only in the other registry view is a machine install this build's
    // installer would treat as per-user, writing the wrong hive.
    if (!recorded_machine && machine_record_names(directory)) {
        error = "This copy is recorded as an all-users install that this version cannot update in place. "
                "Uninstall OrcaSlicer, then install this version again.";
        return InstallerResult::Failed;
    }

    // The elevated branch below runs the tree's own uninstaller as the administrator.
    // A tree this user can write cannot be trusted with that executable.
    if (recorded_machine && tree_writable && !check_writable_machine_tree(directory, error))
        return InstallerResult::Failed;

    // The installer never raises UAC for itself, so the scope is decided here.
    // A tree a machine record names is machine-wide even when this user can
    // write to it: the entry in HKLM needs an administrator to rewrite. A tree
    // this user cannot write to needs the token whatever the record says.
    if (!recorded_machine && tree_writable) {
        STARTUPINFOW startup;
        ::ZeroMemory(&startup, sizeof(startup));
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process;
        ::ZeroMemory(&process, sizeof(process));

        std::wstring command = L'"' + file + L"\" " + parameters;
        // The old build's uninstaller removes the install tree, so it must not be the
        // working directory: the helper's own folder is used instead.
        if (!::CreateProcessW(file.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                              working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process)) {
            const DWORD last = ::GetLastError();
            error = "The installer could not be started (error " + std::to_string(static_cast<long long>(last)) + ").";
            return InstallerResult::Failed;
        }
        ::CloseHandle(process.hThread);
        return wait_for_installer(process.hProcess, error);
    }

    SHELLEXECUTEINFOW info;
    ::ZeroMemory(&info, sizeof(info));
    info.cbSize       = sizeof(info);
    info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb       = L"runas";
    info.lpFile       = file.c_str();
    info.lpParameters = parameters.c_str();
    // Same working directory as the unelevated branch, for the same reason.
    info.lpDirectory  = working_directory.empty() ? nullptr : working_directory.c_str();
    info.nShow        = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info)) {
        const DWORD last = ::GetLastError();
        if (last == ERROR_CANCELLED)
            error = "The update was cancelled at the administrator prompt.";
        else
            error = "The installer could not be started (error " + std::to_string(static_cast<long long>(last)) + ").";
        return InstallerResult::Failed;
    }
    if (info.hProcess == nullptr) {
        error = "The installer started but could not be followed, so the update is unconfirmed.";
        return InstallerResult::Failed;
    }

    return wait_for_installer(info.hProcess, error);
}

// True when a process other than this one runs an image under install_root.
// Such a process keeps files locked, and the silent NSIS installer would
// uninstall first and then fail halfway.
bool other_instance_running(const std::string& install_root)
{
    std::wstring prefix = boost::nowide::widen(install_root);
    for (wchar_t& ch : prefix)
        if (ch == L'/')
            ch = L'\\';
    while (!prefix.empty() && prefix.back() == L'\\')
        prefix.pop_back();
    if (prefix.empty())
        return false;
    prefix += L'\\';

    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    const DWORD self = ::GetCurrentProcessId();
    bool        found = false;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self)
            continue;
        const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr)
            continue;
        std::wstring image(32768, L'\0');
        DWORD        size = static_cast<DWORD>(image.size());
        if (::QueryFullProcessImageNameW(process, 0, image.data(), &size)) {
            image.resize(size);
            found = image.size() >= prefix.size() && _wcsnicmp(image.c_str(), prefix.c_str(), prefix.size()) == 0;
        }
        ::CloseHandle(process);
    }
    ::CloseHandle(snapshot);
    return found;
}

} // namespace

bool apply_windows_install(const UpdateHandoff& handoff, const std::string& artifact_path, ProgressSink& sink, std::string& error, bool launch)
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

    if (other_instance_running(install_root)) {
        sink.say(Stage::Installing, 0, "Waiting for other OrcaSlicer windows to close");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (other_instance_running(install_root)) {
            if (sink.cancelled && sink.cancelled()) {
                error = "cancelled";
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "Another OrcaSlicer window is still open. Close all OrcaSlicer windows, then try the update again.";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

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
    const InstallerResult installed = run_installer(artifact_path, install_root, error);
    if (installed == InstallerResult::StillRunning) {
        // Neither installed nor failed: the installer is still writing the tree. Reported
        // through the sink so the helper exits 0, records nothing, and starts nothing.
        remove_pending_install();
        error = "The installer is still running. Start OrcaSlicer after it finishes.";
        sink.say(Stage::Failed, 100, error);
        return false;
    }
    if (installed == InstallerResult::Failed) {
        remove_pending_install();
        // The installer removes the previous version before it writes the new
        // one. When it stopped in between, no application is left to start and
        // the installer's own message cannot name the consequence.
        if (!fs::is_regular_file(to_path(install_root + "\\orca-slicer.exe"), ec))
            error += " The previous version was removed and the new one could not be written, so OrcaSlicer has to be "
                     "installed again from the release page.";
        return false;
    }

    if (launch)
        sink.say(Stage::Launching, 90, "Starting " + label);

    // Recorded before the launch: a crash while the new build starts still
    // leaves the application knowing what it is running.
    InstallRecord record;
    record.digest  = handoff.digest;
    record.channel = handoff.channel;
    record.version = handoff.version;
    record.target  = install_root;
    if (!write_install_record(record))
        BOOST_LOG_TRIVIAL(warning) << "Could not record the installed build " << handoff.version;

    const std::string exe_path = handoff.exe_path.empty() ? install_root + "\\orca-slicer.exe" : handoff.exe_path;
    if (launch) {
        if (sink.release_lock)
            sink.release_lock();
        if (!launch_detached(exe_path, error)) {
            // The tree is in place and recorded; only starting it failed. Keep both records
            // so the build the user starts knows what it is, and report the start, not the
            // update, as what went wrong.
            sink.say(Stage::Failed, 100, "OrcaSlicer was updated but could not be started. Start it again.");
            return false;
        }
    }
    return true;
}

#else

bool apply_windows_install(const UpdateHandoff&, const std::string&, ProgressSink&, std::string& error, bool)
{
    error = "Windows installs are not supported on this platform.";
    return false;
}

#endif

} // namespace Updater
} // namespace Slic3r
