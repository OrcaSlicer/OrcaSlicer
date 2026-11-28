#pragma once

// The command line's run: the replay or Python script that --run gives.
namespace Slic3r { namespace GUI { namespace Mcp {

class AutomationRuntime;
struct McpLaunchOptions;

// Exit code of the command line's run (0 passed, 1 failed, 2 invalid), -1 when none ran. Read by GUI_Run after
// the event loop ends; wx ignores OnExit()'s return value.
int launch_exit_code();
// Registers the run of `options.run_file` for when the runtime is ready, and makes `options.run_out`, when given,
// the session's output folder (set_run_output_dir). UI thread.
void start_launch_run(AutomationRuntime& runtime, const McpLaunchOptions& options);

}}} // namespace Slic3r::GUI::Mcp
