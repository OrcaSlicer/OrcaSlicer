#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace Slic3r { namespace Bench {

// Every query reports the current process, and answers 0 if the platform call fails.

// Resident memory held right now.
std::uint64_t current_rss_bytes();

// Highest resident memory since process start.
std::uint64_t peak_rss_bytes();

// User plus system CPU time consumed.
std::uint64_t process_cpu_ns();

// Every query reports this machine, falling back to something coarser when the platform does not
// say, and to "unknown" only when nothing coarser exists.

// Name of this machine.
std::string host_name();

// Operating system and version.
std::string os_description();

// Processor model, or the architecture where the model is not published.
std::string cpu_model();

// Logical processors on this machine, which is not necessarily how many threads TBB runs.
unsigned logical_cores();

// The step in which process_cpu_ns() advances each thread's time, and zero where it counts finely
// enough not to matter.
std::chrono::nanoseconds cpu_time_step();

// Turns on escape sequences in the terminal standard output writes to, which a Windows console starts
// without, and says whether standard output is a terminal that shows them.
bool enable_terminal_escapes();

// Whether standard error is a terminal, where a progress line can redraw itself in place.
bool error_is_terminal();

}} // namespace Slic3r::Bench
