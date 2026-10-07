#pragma once

#ifndef _WIN32
#error Bambu bridge transport requires Windows
#endif

#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>

namespace Slic3r::BambuBridge {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : m_value(value) {}
    ~Handle() { if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return m_value; }
private:
    HANDLE m_value;
};

inline std::runtime_error win_error(const char* operation)
{ return std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ")"); }

class WinPipe {
public:
    explicit WinPipe(HANDLE handle) : m_handle(handle) {}
    ~WinPipe() { close(); }
    void read(void* buffer, std::size_t size) { transfer(buffer, size, false); }
    void write(void* buffer, std::size_t size) { transfer(buffer, size, true); }
    void close()
    {
        std::lock_guard<std::mutex> lock(m_start_mutex);
        m_closed = true;
        CancelIoEx(m_handle.get(), nullptr);
    }
private:
    void transfer(void* buffer, std::size_t size, bool write)
    {
        auto* bytes = static_cast<unsigned char*>(buffer);
        while (size) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.get()) throw win_error("CreateEvent");
            OVERLAPPED overlapped{};
            overlapped.hEvent = event.get();
            DWORD transferred = 0;
            BOOL completed;
            DWORD error;
            {
                // CancelIoEx cannot miss an operation that starts after close().
                std::lock_guard<std::mutex> lock(m_start_mutex);
                if (m_closed) throw std::runtime_error("Bambu pipe closed");
                completed = write ? WriteFile(m_handle.get(), bytes, static_cast<DWORD>(size), &transferred, &overlapped)
                                  : ReadFile(m_handle.get(), bytes, static_cast<DWORD>(size), &transferred, &overlapped);
                error = GetLastError();
            }
            if (!completed) {
                if (error != ERROR_IO_PENDING) { SetLastError(error); throw win_error(write ? "WriteFile" : "ReadFile"); }
                const DWORD waited = WaitForSingleObject(event.get(), write ? 30000 : INFINITE);
                if (waited != WAIT_OBJECT_0) {
                    CancelIoEx(m_handle.get(), &overlapped);
                    // OVERLAPPED and its event must stay alive until cancellation completes.
                    GetOverlappedResult(m_handle.get(), &overlapped, &transferred, TRUE);
                    throw std::runtime_error("Bambu pipe write timed out");
                }
                if (!GetOverlappedResult(m_handle.get(), &overlapped, &transferred, FALSE)) throw win_error("Pipe transfer");
            }
            if (!transferred) throw std::runtime_error("Bambu pipe reached EOF");
            bytes += transferred;
            size -= transferred;
        }
    }
    Handle m_handle;
    std::mutex m_start_mutex;
    bool m_closed = false;
};

std::wstring environment(const wchar_t* name);
std::wstring utf16(const std::string& text);

// Own the child through a kill-on-close job. All networking remains in that child.
struct HelperProcess {
    std::shared_ptr<WinPipe> pipe;
    std::unique_ptr<Handle> job;
    std::unique_ptr<Handle> process;
};
HelperProcess launch_helper(const std::wstring& executable);
std::shared_ptr<WinPipe> connect_to_parent(const std::wstring& pipe_name);

} // namespace Slic3r::BambuBridge
