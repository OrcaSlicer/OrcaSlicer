#ifndef slic3r_GUI_LlmAgent_hpp_
#define slic3r_GUI_LlmAgent_hpp_

// Runs a local agent CLI (phase 1-2: Claude Code) against this app's own MCP socket. The CLI is the user's own install and
// keeps its own login; Preferences > LLM / MCP only holds where it is, an optional model and extra arguments.
// docs/CAD/SPEC-llm-panel.md

#include <functional>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

// Full path of the agent CLI: the Preferences path if set, else `claude` on PATH or where its installer puts it. Empty when absent.
std::string llm_agent_executable();

// One thing the agent did while it worked, in the order it did it.
struct LlmEvent {
    enum Kind { Text, Tool, Result } kind = Text;
    std::string text;         // Text: the agent's words. Tool: "name  {arguments}". Result: the final reply.
    std::string session_id;   // Result only: pass it to the next llm_agent_start to continue the conversation
    bool        error = false;
};

class LlmAgentRun {
public:
    virtual ~LlmAgentRun() = default;
    virtual void cancel() = 0;   // kills the agent process; `done` then reports "stopped"
};

// Start one agent turn. Runs on a worker thread; `on_event` and `done` are called on the UI thread, `done` exactly once
// (ok=false carries the reason). `session_id` continues an earlier turn ("" starts a conversation). Only one run at a
// time: a second call while one is in flight is refused through `done` and returns null.
std::shared_ptr<LlmAgentRun> llm_agent_start(const std::string& prompt, const std::string& session_id,
                                             std::function<void(const LlmEvent&)> on_event,
                                             std::function<void(bool ok, const std::string& err)> done);

// Convenience for the Preferences Test button: one turn, only the final reply.
void llm_agent_run(const std::string& prompt, std::function<void(bool ok, const std::string& text)> done);

}} // namespace Slic3r::GUI

#endif
