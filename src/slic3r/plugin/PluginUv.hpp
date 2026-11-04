#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace Slic3r::plugin_uv {

// Run the bundled uv with `args` (everything after the executable), capturing its stderr.
// Blocks the calling thread until the child exits or `timeout` elapses.
//
// `error` is set to an actionable message on failure: a missing bundled uv, a failed wait, a
// timeout, a non-zero exit status, or the exception thrown while spawning the child. Arguments are
// passed as an array; nothing is routed through a shell.
bool run(const std::vector<std::string>& args, std::chrono::seconds timeout, std::string& error);

} // namespace Slic3r::plugin_uv
