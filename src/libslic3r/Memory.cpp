#include "Memory.hpp"

#include <cstddef>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include <oneapi/tbb/scalable_allocator.h>

#ifdef WIN32
    #include <windows.h>
    #include <psapi.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/resource.h>
    #ifdef __APPLE__
        #include <mach/mach.h>
        #include <malloc/malloc.h>
    #endif
    #ifdef __linux__
        #include <malloc.h>
    #endif
#endif

namespace Slic3r {

size_t current_process_rss_bytes()
{
#ifdef WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return (size_t)pmc.WorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS)
        return (size_t)info.resident_size;
    return 0;
#elif defined(__linux__)
    size_t tSize = 0, resident = 0, share = 0;
    std::ifstream buffer("/proc/self/statm");
    if (buffer && (buffer >> tSize >> resident >> share)) {
        size_t page_size = (size_t)sysconf(_SC_PAGE_SIZE);
        return resident * page_size;
    }
    return 0;
#else
    return 0;
#endif
}

size_t peak_process_rss_bytes()
{
    size_t current = current_process_rss_bytes();
    size_t peak = 0;
#ifdef WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        peak = (size_t)pmc.PeakWorkingSetSize;
#elif defined(__linux__) || defined(__APPLE__)
    rusage memory_info;
    if (getrusage(RUSAGE_SELF, &memory_info) == 0) {
        peak = (size_t)memory_info.ru_maxrss;
#ifdef __linux__
        peak *= 1024;
#endif
    }
#endif
    return std::max(peak, current);
}

bool trim_process_memory()
{
    bool trimmed = false;

    if (scalable_allocation_command(TBBMALLOC_CLEAN_ALL_BUFFERS, nullptr) == TBBMALLOC_OK)
        trimmed = true;

#if defined(__linux__) && defined(__GLIBC__)
    if (malloc_trim(0) == 1)
        trimmed = true;
#endif

#ifdef WIN32
    HANDLE hProcess = GetCurrentProcess();
    if (SetProcessWorkingSetSize(hProcess, (SIZE_T)-1, (SIZE_T)-1))
        trimmed = true;
    else if (EmptyWorkingSet(hProcess))
        trimmed = true;
#endif

#ifdef __APPLE__
    malloc_zone_pressure_relief(nullptr, 0);
    trimmed = true;
#endif

    return trimmed;
}

void initialize_memory_allocator()
{
#if defined(__linux__) && defined(__GLIBC__)
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
}

} // namespace Slic3r
