#ifndef slic3r_GUI_LlmAgent_hpp_
#define slic3r_GUI_LlmAgent_hpp_

// Runs a local agent CLI (phase 1: Claude Code) against this app's own MCP socket. The CLI is the user's own install and
// keeps its own login; Preferences > LLM / MCP only holds where it is, an optional model and extra arguments.
// docs/CAD/SPEC-llm-panel.md

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

// Full path of the agent CLI: the Preferences path if set, else `claude` on PATH. Empty when it is not installed.
std::string llm_agent_executable();

// Run `prompt` through the agent with the Orca-CAD MCP tools attached. Runs on a worker thread; `done(ok, text)` is called
// on the UI thread with the agent's reply (ok) or the reason it failed (!ok). Only one run at a time: a second call while
// one is in flight is refused through `done`.
void llm_agent_run(const std::string& prompt, std::function<void(bool ok, const std::string& text)> done);

}} // namespace Slic3r::GUI

#endif
