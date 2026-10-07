#include "WinPipe.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <vector>
#include <filesystem>

namespace Slic3r::BambuBridge {

std::wstring environment(const wchar_t* name)
{
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    DWORD count = GetEnvironmentVariableW(name, value.data(), size);
    if (!count || count >= size) throw win_error("GetEnvironmentVariable");
    value.resize(count);
    return value;
}

std::wstring utf16(const std::string& text)
{
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw win_error("MultiByteToWideChar");
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count))
        throw win_error("MultiByteToWideChar");
    return result;
}

HelperProcess launch_helper(const std::wstring& executable)
{
    if (!std::filesystem::path(executable).is_absolute() || executable.find(L'"') != std::wstring::npos)
        throw std::runtime_error("ORCA_BAMBU_HELPER must be an absolute executable path");
    unsigned char random[16];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Cannot generate Bambu pipe name");
    std::wstring pipe_name = L"\\\\.\\pipe\\OrcaBambu-";
    const wchar_t* hex = L"0123456789abcdef";
    for (auto byte : random) { pipe_name += hex[byte >> 4]; pipe_name += hex[byte & 15]; }

    HANDLE token_handle = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle)) throw win_error("OpenProcessToken");
    Handle token(token_handle);
    DWORD token_size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_size);
    std::vector<unsigned char> token_data(token_size);
    if (!GetTokenInformation(token.get(), TokenUser, token_data.data(), token_size, &token_size))
        throw win_error("GetTokenInformation");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid, &sid))
        throw win_error("ConvertSidToStringSid");
    const std::wstring descriptor = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr))
        throw win_error("ConvertStringSecurityDescriptor");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
    HANDLE pipe_handle = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         1, 65536, 65536, 0, &attributes);
    LocalFree(security);
    if (pipe_handle == INVALID_HANDLE_VALUE) throw win_error("CreateNamedPipe");
    Handle pending_pipe(pipe_handle);

    HelperProcess child;
    child.job = std::make_unique<Handle>(CreateJobObjectW(nullptr, nullptr));
    if (!child.job->get()) throw win_error("CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child.job->get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw win_error("SetInformationJobObject");
    std::wstring command = L"\"" + executable + L"\" --pipe \"" + pipe_name + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
        throw win_error("CreateProcess");
    child.process = std::make_unique<Handle>(process.hProcess);
    Handle thread(process.hThread);
    if (!AssignProcessToJobObject(child.job->get(), process.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        SetLastError(error);
        throw win_error("AssignProcessToJobObject");
    }
    if (ResumeThread(thread.get()) == DWORD(-1)) throw win_error("ResumeThread");

    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get()) throw win_error("CreateEvent");
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    BOOL connected = ConnectNamedPipe(pipe_handle, &overlapped);
    DWORD error = GetLastError();
    if (!connected && error == ERROR_IO_PENDING) {
        HANDLE waits[] = {event.get(), process.hProcess};
        if (WaitForMultipleObjects(2, waits, FALSE, 10000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_handle, &overlapped);
            DWORD unused;
            GetOverlappedResult(pipe_handle, &overlapped, &unused, TRUE);
            throw std::runtime_error("Bambu helper exited or failed to connect within 10 seconds");
        }
        DWORD unused;
        if (!GetOverlappedResult(pipe_handle, &overlapped, &unused, FALSE)) throw win_error("ConnectNamedPipe");
    } else if (!connected && error != ERROR_PIPE_CONNECTED) {
        SetLastError(error);
        throw win_error("ConnectNamedPipe");
    }
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe_handle, &client_pid) || client_pid != process.dwProcessId)
        throw std::runtime_error("Unexpected Bambu named-pipe client");
    // Duplicate ownership only after all failure paths are covered by pending_pipe's RAII.
    HANDLE owned_pipe = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), pipe_handle, GetCurrentProcess(), &owned_pipe, 0, FALSE, DUPLICATE_SAME_ACCESS))
        throw win_error("DuplicateHandle");
    child.pipe = std::make_shared<WinPipe>(owned_pipe);
    return child;
}

std::shared_ptr<WinPipe> connect_to_parent(const std::wstring& pipe_name)
{
    if (pipe_name.find(L"\\\\.\\pipe\\OrcaBambu-") != 0) throw std::runtime_error("Invalid Bambu pipe name");
    HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw win_error("Open Bambu pipe");
    return std::make_shared<WinPipe>(pipe);
}

} // namespace Slic3r::BambuBridge
