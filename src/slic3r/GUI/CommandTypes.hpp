#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The types a command of the catalog (CommandCatalog.hpp) is called with and answers in, whichever front end
// calls it: a Speed Dial action, an MCP tool or a plugin. No wx here: this header is part of the library
// libslic3r_automation, which is built without wx.
namespace Slic3r { namespace GUI {

// Defined in ActionRegistry.hpp, which brings in wx: a ToolResult only holds one through a shared_ptr, which
// the code that makes one (NativeCommands.cpp) completes.
struct AppActionRunResult;
// Defined in CommandCatalog.hpp, which includes this header.
struct NativeCommand;

namespace Mcp {
class AutomationRuntime;
}

// What a tool may do while the app is inside another call's nested event loop or behind a modal
// dialog. Inspect tools only read; Recover tools act on the topmost modal dialog to unblock the
// app; everything else is Mutate and is refused in both situations.
enum class GuardClass { Inspect, Recover, Mutate };

// A tool's outcome in protocol-neutral form. `data` is the JSON the model receives as text (and a
// script evaluates `expect` against); `content` carries extra MCP content blocks such as images.
struct ToolResult
{
    nlohmann::json              data = nlohmann::json::object();
    std::vector<nlohmann::json> content;
    bool                        is_error = false;
    std::string                 message;
    // What a person's pick of the command shows, in the UI's language (NativeCommands::notify_result());
    // null for a result that shows nothing.
    std::shared_ptr<const AppActionRunResult> notice;

    static ToolResult ok(nlohmann::json data = nlohmann::json::object())
    {
        ToolResult r;
        r.data = std::move(data);
        return r;
    }
    static ToolResult error(std::string message, nlohmann::json details = nlohmann::json())
    {
        ToolResult r;
        r.is_error = true;
        r.message  = std::move(message);
        r.data     = std::move(details);
        return r;
    }
};

// Thrown by handlers (or by ToolContext on their behalf) to end a call with an isError result.
struct ToolFailure : std::runtime_error
{
    nlohmann::json details;
    explicit ToolFailure(const std::string& message, nlohmann::json details = nlohmann::json())
        : std::runtime_error(message), details(std::move(details))
    {}
};

// The services a handler gets for one call. A client's call, a replay's and a script's run on a request or
// job thread, never on the UI thread, and everything that touches wx goes through run_ui(); a person's pick
// runs on the UI thread (person()).
class ToolContext
{
public:
    virtual ~ToolContext() = default;
    // True for a person's pick from the Speed Dial or a menu: the call is on the UI thread and has no job.
    virtual bool person() const { return false; }
    // The automation runtime the call runs under: what a command reads the jobs, the journal, recordings and the UI
    // inspector from, so that the catalog is built without a runtime.
    // Throws when the call has none: std::logic_error here, ToolFailure where one cannot be started.
    virtual Mcp::AutomationRuntime& runtime() { throw std::logic_error("This call runs without the automation runtime"); }
    // Starts command `key` with `args` as a job and returns at once, as the reply to a long command that a
    // person picks. Throws ToolFailure where this call cannot start a job.
    virtual ToolResult submit(const std::string& key, const nlohmann::json& /*args*/)
    {
        throw ToolFailure("This call cannot start " + key + " as a job");
    }
    // Runs `fn` on the UI thread under the tool's guard class and returns its value. Throws
    // ToolFailure when a guard refuses the call, the UI does not pick it up in time, or the app is
    // closing.
    virtual nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) = 0;
    // Runs `fn` on the UI thread under the inspect guard, whatever the tool's class: for watching
    // an operation the call has started, which a dialog or another call's nested loop must not stop.
    virtual nlohmann::json observe_ui(const std::function<nlohmann::json()>& fn) = 0;
    // Id of the job this call runs as; empty for inline (inspect/recover) calls.
    virtual const std::string& job_id() const = 0;
    // True once the runtime is stopping, or a stop was asked for this call's job or one of its
    // ancestors (a replay or script the user stopped); long-running handlers poll it between steps.
    virtual bool cancelled() const = 0;
    // Calls another tool as a child of this call (replay steps and a script's calls). Returns what a client would get:
    // the result, or a job envelope when the child outlives its reply budget.
    virtual ToolResult call_child(const std::string& tool, const nlohmann::json& args) = 0;
    // This call's job and its ancestors, which `app_wait {for:"idle"}` must not wait for.
    virtual std::set<std::string> lineage() const = 0;
    // Replaces the arguments the session journal records for this call (refs become locators).
    virtual void set_journal_args(nlohmann::json args) = 0;
    // Marks this job as a slice/export operation: until it finishes, other mutate calls are refused
    // so nothing changes the project under it. Call it on the UI thread before starting the work, at
    // the latest in the closure that starts it; throws ToolFailure when another operation already runs.
    // No-op for inline calls.
    virtual void begin_operation() = 0;
    // Says the call works in the open modal dialog whose serial is `dialog` (ModalTracker::serial()):
    // while that dialog is the topmost, a call outliving its soft budget is still running, not
    // blocked on a dialog. The web page paths of the GUI input tools say it once their page is in the
    // topmost dialog, since a page can take longer than a widget to answer. Any thread; no-op for
    // inline calls.
    virtual void works_in_dialog(std::uint64_t /*dialog*/) {}
    // The plugin whose code made this call, directly or through a call it made (a step of its script);
    // empty for a client's call, a replay, and a person's or an agent's script.
    virtual const std::string& caller_plugin() const
    {
        static const std::string none;
        return none;
    }
};

// Executes a command for the protocol layer: the runtime's core adds jobs, budgets, guards and the
// journal; tests call the command's function directly. `origin` says who made the call, for the core's
// observers (Mcp::CallObserver::call_started); null when nothing says.
class ToolCaller
{
public:
    virtual ~ToolCaller() = default;
    virtual ToolResult call(const NativeCommand& command, const nlohmann::json& args, const nlohmann::json& origin = nlohmann::json()) = 0;
};

}} // namespace Slic3r::GUI
