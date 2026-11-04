#pragma once

#include <cstddef>

namespace Slic3r {

// Returns the current process resident set size (RSS) in bytes, or 0 if unavailable.
size_t current_process_rss_bytes();

// Returns the peak process resident set size in bytes, or 0 if unavailable.
size_t peak_process_rss_bytes();

// Releases unused/cached heap memory back to the operating system:
// - glibc: malloc_trim(0)
// - tbbmalloc: scalable_allocation_command(TBBMALLOC_CLEAN_ALL_BUFFERS, nullptr)
// - Windows: SetProcessWorkingSetSize / EmptyWorkingSet
// - macOS: malloc_zone_pressure_relief
// Returns true if memory trimming was executed.
bool trim_process_memory();

// Initializes allocator parameters (e.g. glibc malloc thresholds) to prevent excessive heap fragmentation.
void initialize_memory_allocator();

} // namespace Slic3r
