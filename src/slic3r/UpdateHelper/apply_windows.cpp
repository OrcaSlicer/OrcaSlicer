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

    #include <chrono>
    #include <cstdio>
    #include <cwchar>
    #include <string>
    #include <thread>
    #include <vector>

    #include <boost/filesystem.hpp>
    #include <boost/log/trivial.hpp>

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
    const std::wstring expected = widen_utf8(ORCA_UPDATER_PUBLISHER);
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
        error = "The update is signed by \"" + narrow_utf8(actual) + "\", not by " + ORCA_UPDATER_PUBLISHER +
                ", so it was not run.";
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

// True when a process other than this one runs an image under install_root.
// Such a process keeps files locked, and the silent NSIS installer would
// uninstall first and then fail halfway.
bool other_instance_running(const std::string& install_root)
{
    std::wstring prefix = widen_utf8(install_root);
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
    record.target  = install_root;
    if (!write_install_record(record))
        BOOST_LOG_TRIVIAL(warning) << "Could not record the installed build " << handoff.version;

    const std::string exe_path = handoff.exe_path.empty() ? install_root + "\\orca-slicer.exe" : handoff.exe_path;
    if (!launch_detached(exe_path, error)) {
        remove_path_quietly(updater_install_record_path());
        remove_pending_install();
        error += " The installer finished, but the new version did not start.";
        return false;
    }
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
