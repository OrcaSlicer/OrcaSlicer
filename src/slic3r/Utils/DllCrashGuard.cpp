#include "DllCrashGuard.hpp"

#include <mutex>
#include <cstring>

#include <boost/log/trivial.hpp>

#if defined(_MSC_VER) || defined(_WIN32)
    #define DLL_CRASH_GUARD_WINDOWS 1
    #include <windows.h>
#else
    #define DLL_CRASH_GUARD_POSIX 1
    #include <signal.h>
    #include <setjmp.h>
    #include <pthread.h>
    #include <unistd.h>
#endif

namespace Slic3r {

// ============================================================================
// Global state
// ============================================================================

std::atomic<bool> g_networking_dll_crashed{false};

static std::mutex  s_crash_msg_mutex;
static std::string s_crash_message;
static std::once_flag s_handler_installed;

std::string get_dll_crash_message()
{
    std::lock_guard<std::mutex> lock(s_crash_msg_mutex);
    return s_crash_message;
}

static void set_crash_message(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(s_crash_msg_mutex);
    s_crash_message = msg;
}

// ============================================================================
// POSIX implementation (macOS / Linux)
// ============================================================================

#if DLL_CRASH_GUARD_POSIX

// Thread-local jump buffer for recovering from SIGSEGV in DLL calls we initiate.
// When dll_safe_call_impl sets this, our SIGSEGV handler will longjmp back instead of crashing.
static thread_local sigjmp_buf* tl_recovery_point = nullptr;

// Previous SIGSEGV handler, so we can chain if the crash isn't from our DLL call.
static struct sigaction s_old_sigsegv_action;

static void sigsegv_handler(int sig, siginfo_t* info, void* ucontext)
{
    // Case 1: We're inside a dll_safe_call — recover via longjmp
    if (tl_recovery_point) {
        siglongjmp(*tl_recovery_point, 1);
        // unreachable
    }

    // Case 2: Crash on a DLL-owned thread (e.g. MQTTAsync_sendThread).
    // We can't recover this thread, but we can flag the crash and try to
    // terminate just this thread instead of the whole process.
    //
    // Note: pthread_exit is not async-signal-safe, but in practice it works
    // on macOS and Linux for this use case. The alternative is process death.

    // Write a minimal message (write() is async-signal-safe)
    const char crash_msg[] = "DllCrashGuard: SIGSEGV caught on DLL thread, terminating thread.\n";
    (void)write(STDERR_FILENO, crash_msg, sizeof(crash_msg) - 1);

    g_networking_dll_crashed.store(true, std::memory_order_release);

    // Attempt to terminate just this thread.
    // This is a best-effort recovery — it may not always work cleanly.
    pthread_exit(nullptr);

    // If pthread_exit somehow returns (shouldn't happen), fall through to the
    // previous handler or default behavior.
    if (s_old_sigsegv_action.sa_flags & SA_SIGINFO) {
        if (s_old_sigsegv_action.sa_sigaction)
            s_old_sigsegv_action.sa_sigaction(sig, info, ucontext);
    } else {
        if (s_old_sigsegv_action.sa_handler != SIG_DFL &&
            s_old_sigsegv_action.sa_handler != SIG_IGN)
        {
            s_old_sigsegv_action.sa_handler(sig);
        } else {
            // Re-raise with default handler
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }
}

void install_dll_crash_handler()
{
    std::call_once(s_handler_installed, []() {
        // Use sigaltstack so our handler has its own stack
        // (in case the crash corrupted the thread's stack)
        static char alt_stack[SIGSTKSZ * 2];
        stack_t ss;
        ss.ss_sp = alt_stack;
        ss.ss_size = sizeof(alt_stack);
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = sigsegv_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGSEGV, &sa, &s_old_sigsegv_action) != 0) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: failed to install SIGSEGV handler";
        } else {
            BOOST_LOG_TRIVIAL(info) << "DllCrashGuard: SIGSEGV handler installed for DLL crash protection";
        }

        // Also handle SIGBUS (common on macOS for similar memory access issues)
        struct sigaction sa_bus;
        memset(&sa_bus, 0, sizeof(sa_bus));
        sa_bus.sa_sigaction = sigsegv_handler;
        sa_bus.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa_bus.sa_mask);
        sigaction(SIGBUS, &sa_bus, nullptr);
    });
}

int dll_safe_call_impl(std::function<int()> fn, int fallback, const char* context)
{
    // Layer 1: Signal protection (SIGSEGV / SIGBUS)
    sigjmp_buf jmpbuf;
    sigjmp_buf* old_recovery = tl_recovery_point;
    tl_recovery_point = &jmpbuf;

    int result = fallback;

    if (sigsetjmp(jmpbuf, 1) == 0) {
        // Normal execution path
        try {
            // Layer 2: C++ exception protection
            result = fn();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: C++ exception in " << context << ": " << e.what();
            set_crash_message(std::string("Exception in ") + context + ": " + e.what());
            result = fallback;
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: unknown C++ exception in " << context;
            set_crash_message(std::string("Unknown exception in ") + context);
            result = fallback;
        }
    } else {
        // Recovered from SIGSEGV/SIGBUS via siglongjmp
        BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: RECOVERED from crash (SIGSEGV/SIGBUS) in " << context
                                 << " — the networking DLL has a bug. Returning error code.";
        set_crash_message(std::string("Crash recovered in ") + context +
                         ": the Bambu networking library crashed (SIGSEGV). "
                         "This is a bug in the networking plugin.");
        g_networking_dll_crashed.store(true, std::memory_order_release);
        result = fallback;
    }

    tl_recovery_point = old_recovery;
    return result;
}

#endif // DLL_CRASH_GUARD_POSIX

// ============================================================================
// Windows implementation
// ============================================================================

#if DLL_CRASH_GUARD_WINDOWS

// Check if a faulting address belongs to the Bambu networking DLL.
static bool is_bambu_dll_address(void* addr)
{
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)addr,
        &hmod);
    if (!hmod)
        return false;

    char modname[MAX_PATH] = {};
    GetModuleFileNameA(hmod, modname, sizeof(modname));
    return (strstr(modname, "bambu_networking") ||
            strstr(modname, "BambuSource") ||
            strstr(modname, "bambu_network"));
}

// Vectored Exception Handler: called BEFORE frame-based handlers on ALL threads.
// If the crash is inside the Bambu networking DLL, we flag it and redirect the
// crashing thread to ExitThread() so only that thread dies, not the whole process.
static LONG WINAPI dll_vectored_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    if (!is_bambu_dll_address(ep->ExceptionRecord->ExceptionAddress))
        return EXCEPTION_CONTINUE_SEARCH;

    // The crash is inside the Bambu DLL. Flag it and terminate only this thread.
    g_networking_dll_crashed.store(true, std::memory_order_release);

    char modname[MAX_PATH] = {};
    {
        HMODULE hmod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ep->ExceptionRecord->ExceptionAddress,
            &hmod);
        if (hmod)
            GetModuleFileNameA(hmod, modname, sizeof(modname));
    }

    BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: Access violation in DLL module: " << modname
                             << " at address " << ep->ExceptionRecord->ExceptionAddress
                             << " — terminating DLL thread to prevent app crash.";
    set_crash_message(std::string("Access violation in DLL: ") + modname);

    // Redirect execution to ExitThread(1) so only this thread terminates.
    // We overwrite the instruction pointer in the exception context.
#ifdef _M_X64
    ep->ContextRecord->Rip = (DWORD64)ExitThread;
    ep->ContextRecord->Rcx = 1;  // ExitThread argument (exit code)
#elif defined(_M_ARM64)
    ep->ContextRecord->Pc = (DWORD64)ExitThread;
    ep->ContextRecord->X0 = 1;
#else
    // x86 fallback: push exit code on stack and jump to ExitThread
    ep->ContextRecord->Esp -= sizeof(DWORD);
    *(DWORD*)ep->ContextRecord->Esp = 1;
    ep->ContextRecord->Eip = (DWORD)ExitThread;
#endif

    return EXCEPTION_CONTINUE_EXECUTION;
}

void install_dll_crash_handler()
{
    std::call_once(s_handler_installed, []() {
        // Use a Vectored Exception Handler (first-chance) so we intercept
        // crashes on DLL-owned threads before any frame-based handler runs.
        // The '1' means "add to front of the VEH chain".
        AddVectoredExceptionHandler(1, dll_vectored_handler);
        BOOST_LOG_TRIVIAL(info) << "DllCrashGuard: Vectored exception handler installed for DLL crash protection";
    });
}

int dll_safe_call_impl(std::function<int()> fn, int fallback, const char* context)
{
    // Layer 1: SEH protection — catches access violations from DLL calls we initiate.
    // Layer 2: C++ exception protection.
    int result = fallback;
    __try {
        try {
            result = fn();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: C++ exception in " << context << ": " << e.what();
            set_crash_message(std::string("Exception in ") + context + ": " + e.what());
            result = fallback;
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: unknown C++ exception in " << context;
            set_crash_message(std::string("Unknown exception in ") + context);
            result = fallback;
        }
    } __except (
        GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH
    ) {
        BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: RECOVERED from access violation (SEH) in " << context
                                 << " — the networking DLL has a bug. Returning error code.";
        set_crash_message(std::string("Crash recovered in ") + context +
                         ": the Bambu networking library crashed (access violation). "
                         "This is a bug in the networking plugin.");
        g_networking_dll_crashed.store(true, std::memory_order_release);
        result = fallback;
    }
    return result;
}

#endif // DLL_CRASH_GUARD_WINDOWS

} // namespace Slic3r
