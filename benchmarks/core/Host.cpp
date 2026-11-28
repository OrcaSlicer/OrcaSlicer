#include "Host.hpp"
#include "Text.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

// Every #ifdef in benchmarks/ belongs in this file, keeping the rest portable.
#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
#else
    #include <sys/resource.h>
    #include <sys/utsname.h>
    #include <unistd.h>
    #ifdef __APPLE__
        #include <mach/mach.h>
        #include <sys/sysctl.h>
    #else
        #include <fstream>
        #include <string>
    #endif
#endif

namespace Slic3r { namespace Bench {

namespace {

// What a query returns when nothing coarser is available either, so a reader can tell it from a
// field nobody set.
constexpr const char* unknown = "unknown";

#ifdef _WIN32
// The processor family, the coarser answer when the registry has no model name.
std::string native_architecture()
{
    SYSTEM_INFO info {};
    ::GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return unknown;
    }
}
#else
std::string uname_description()
{
    utsname info {};
    if (::uname(&info) != 0)
        return unknown;
    return std::string(info.sysname) + " " + info.release;
}

std::string uname_machine()
{
    utsname info {};
    return ::uname(&info) == 0 ? info.machine : unknown;
}
#endif

} // namespace

std::uint64_t current_rss_bytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info   info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<std::uint64_t>(info.resident_size);
    return 0;
#else
    // statm reports page counts; field 2 is the resident set.
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total    = 0;
    std::uint64_t resident = 0;
    if (statm >> total >> resident)
        return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGE_SIZE));
    return 0;
#endif
}

std::uint64_t peak_rss_bytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
    return 0;
#else
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
    // ru_maxrss is bytes on macOS but kilobytes on Linux.
    #ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
    #else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
    #endif
#endif
}

std::uint64_t process_cpu_ns()
{
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (! ::GetProcessTimes(::GetCurrentProcess(), &creation, &exited, &kernel, &user))
        return 0;
    // FILETIME counts 100 ns ticks.
    const auto to_ns = [](const FILETIME& ft) {
        ULARGE_INTEGER v;
        v.LowPart  = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return static_cast<std::uint64_t>(v.QuadPart) * 100;
    };
    return to_ns(kernel) + to_ns(user);
#else
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
    const auto to_ns = [](const timeval& tv) {
        return static_cast<std::uint64_t>(tv.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(tv.tv_usec) * 1000ull;
    };
    return to_ns(usage.ru_utime) + to_ns(usage.ru_stime);
#endif
}

std::string host_name()
{
#ifdef _WIN32
    char  name[MAX_COMPUTERNAME_LENGTH + 1] {};
    DWORD size = sizeof(name);
    if (::GetComputerNameA(name, &size))
        return std::string(name, size);
    return unknown;
#else
    char name[256] {};
    if (::gethostname(name, sizeof(name) - 1) == 0)
        return name;
    return unknown;
#endif
}

std::string os_description()
{
#ifdef _WIN32
    // Since Windows 8.1, GetVersionEx reports whatever version the application manifest
    // targets.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW info {};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtl_get_version && rtl_get_version(&info) == 0)
            return "Windows " + std::to_string(info.dwMajorVersion) + "." +
                   std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
    }
    return "Windows";
#else
    #ifdef __APPLE__
    char   version[64] {};
    size_t size = sizeof(version);
    if (::sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0)
        return std::string("macOS ") + version;
    #endif
    return uname_description();
#endif
}

std::string cpu_model()
{
#ifdef _WIN32
    char  model[256] {};
    DWORD size = sizeof(model);
    const bool found = ::RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                      "ProcessorNameString", RRF_RT_REG_SZ, nullptr, model, &size) == ERROR_SUCCESS;
    const std::string name = found ? trimmed(model) : std::string();
    return name.empty() ? native_architecture() : name;
#elif defined(__APPLE__)
    char   brand[256] {};
    size_t size = sizeof(brand);
    const bool found = ::sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0;
    const std::string name = found ? trimmed(brand) : std::string();
    return name.empty() ? uname_machine() : name;
#else
    // "model name" is missing on ARM, where the architecture from uname is the fallback.
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string   line;
    while (std::getline(cpuinfo, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.compare(0, 10, "model name") != 0)
            continue;
        const std::string name = trimmed(line.substr(colon + 1));
        if (!name.empty())
            return name;
    }
    return uname_machine();
#endif
}

unsigned logical_cores()
{
    // hardware_concurrency() may return 0 when it cannot tell, and a machine has at least one
    // core.
    const unsigned reported = std::thread::hardware_concurrency();
    return reported == 0 ? 1 : reported;
}

std::chrono::nanoseconds cpu_time_step()
{
#ifdef _WIN32
    // GetProcessTimes charges each thread's time at the clock interrupt, every 15.625 ms by default.
    return std::chrono::microseconds(15625);
#elif defined(__linux__)
    // getrusage() counts another running thread's time only at the scheduler tick, every 4 ms at the
    // usual CONFIG_HZ of 250, which user space cannot read.
    return std::chrono::milliseconds(4);
#else
    return std::chrono::nanoseconds::zero();
#endif
}

bool enable_terminal_escapes()
{
#ifdef _WIN32
    // GetConsoleMode fails for a file or a pipe, which is also what mintty gives a Windows program.
    const HANDLE out  = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD        mode = 0;
    if (!GetConsoleMode(out, &mode))
        return false;
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 || SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(STDOUT_FILENO) == 1;
#endif
}

bool error_is_terminal()
{
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
#else
    return isatty(STDERR_FILENO) == 1;
#endif
}

}} // namespace Slic3r::Bench
