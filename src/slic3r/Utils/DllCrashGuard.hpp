#ifndef SLIC3R_DLL_CRASH_GUARD_HPP
#define SLIC3R_DLL_CRASH_GUARD_HPP

// DllCrashGuard: Defensive wrapper for calls into external DLLs (e.g. libbambu_networking)
// that may crash with SIGSEGV/access violations due to bugs in the DLL.
//
// Two layers of protection:
// 1. DllSafeCall: wraps individual DLL calls with signal/SEH protection + try-catch
// 2. install_dll_crash_handler(): global signal handler that catches crashes on DLL-owned
//    threads (e.g. MQTTAsync_sendThread) and prevents process termination.

#include <atomic>
#include <functional>
#include <string>

#include <boost/log/trivial.hpp>

namespace Slic3r {

// Global flag: set to true when the networking DLL has crashed on one of its own threads.
// Check this flag after networking operations to detect async DLL failures.
extern std::atomic<bool> g_networking_dll_crashed;

// Returns a human-readable description of the last DLL crash (empty if none).
std::string get_dll_crash_message();

// Install a global signal/exception handler that catches SIGSEGV in DLL-owned threads.
// Call once during application startup. Thread-safe.
void install_dll_crash_handler();

// Execute a callable with crash protection. Returns the callable's return value on success,
// or `fallback` if a crash (SIGSEGV/access violation) or C++ exception was caught.
//
// Usage:
//   int result = dll_safe_call([&]() {
//       return func(agent, params);
//   }, -1, "start_print");
//
template<typename Fn, typename R = std::invoke_result_t<Fn>>
R dll_safe_call(Fn&& fn, R fallback, const char* context = "unknown");

// Non-template declaration for the platform-specific implementation
int dll_safe_call_impl(std::function<int()> fn, int fallback, const char* context);

// Template implementation that delegates to the platform-specific impl for int return type
template<typename Fn, typename R>
R dll_safe_call(Fn&& fn, R fallback, const char* context)
{
    // For int return type, use the platform-specific implementation with signal protection
    if constexpr (std::is_same_v<R, int>) {
        return dll_safe_call_impl(std::function<int()>(std::forward<Fn>(fn)), fallback, context);
    } else {
        // For other return types, use try-catch only (no signal protection)
        try {
            return fn();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: C++ exception in " << context << ": " << e.what();
            return fallback;
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: unknown exception in " << context;
            return fallback;
        }
    }
}

// Void-returning variant
template<typename Fn>
void dll_safe_call_void(Fn&& fn, const char* context = "unknown")
{
    try {
        fn();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: C++ exception in " << context << ": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "DllCrashGuard: unknown exception in " << context;
    }
}

} // namespace Slic3r

#endif // SLIC3R_DLL_CRASH_GUARD_HPP
