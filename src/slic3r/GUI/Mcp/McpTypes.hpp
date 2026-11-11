#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The name of a top-level window the MCP GUI tools never see: list_widgets and locators skip it,
// run_javascript never picks its web views, keys sent without a target never go to it, window screenshots leave it
// out, and as a modal dialog it is not tracked, so the dialog tools and auto-answer rules never reach
// it. Action Replay carries it, so a replay it runs can't act on it, and so does the window that
// changes the Python scripts setting, so no agent, replay or script can turn it on.
inline constexpr char kHiddenFromAutomation[] = "OrcaHiddenFromMcp";

// The name of a text field that holds a secret its dialog shows in plain text, such as a printer's
// access code: a modal dialog's description writes its value as [******] (describe_dialog()). A field
// with password style, or a setting field for a password or key, needs no name.
inline constexpr char kSecretField[] = "OrcaSecretForMcp";

// What a tool may do while the app is inside another call's nested event loop or behind a modal
// dialog. Inspect tools only read; Recover tools act on the topmost modal dialog to unblock the
// app; everything else is Mutate and is refused in both situations.
enum class GuardClass { Inspect, Recover, Mutate };

// Which way a tool acts, for the agent choosing one: Core tools run the session and recover from
// dialogs, Api tools act on project data by index and name with JSON in and out, Ui tools drive
// the GUI itself (refs, pixels, GUI commands). Every caller reaches every tool; the lane is a hint,
// shown at the start of each description and ordering tools/list and the server instructions.
enum class ToolLane { Core, Api, Ui };

// A tool's outcome in protocol-neutral form. `data` is the JSON the model receives as text (and a
// script evaluates `expect` against); `content` carries extra MCP content blocks such as images.
struct ToolResult
{
    nlohmann::json              data = nlohmann::json::object();
    std::vector<nlohmann::json> content;
    bool                        is_error = false;
    std::string                 message;

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

// The services a handler gets for one call. Handlers run on a request or job thread, never on the
// UI thread; everything that touches wx goes through run_ui().
class ToolContext
{
public:
    virtual ~ToolContext() = default;
    // Runs `fn` on the UI thread under the tool's guard class and returns its value. Throws
    // ToolFailure when a guard refuses the call, the UI does not pick it up in time, or the app is
    // closing.
    virtual nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) = 0;
    // Runs `fn` on the UI thread under the inspect guard, whatever the tool's class: for watching
    // an operation the call has started, which a dialog or another call's nested loop must not stop.
    virtual nlohmann::json observe_ui(const std::function<nlohmann::json()>& fn) = 0;
    // Id of the job this call runs as; empty for inline (inspect/recover) calls.
    virtual const std::string& job_id() const = 0;
    // True once the service is stopping, or a stop was asked for this call's job or one of its
    // ancestors (a replay or script the user stopped); long-running handlers poll it between steps.
    virtual bool cancelled() const = 0;
    // Calls another tool as a child of this call (replay steps and a script's calls). Returns what a client would get:
    // the result, or a job envelope when the child outlives its reply budget.
    virtual ToolResult call_child(const std::string& tool, const nlohmann::json& args) = 0;
    // This call's job and its ancestors, which `wait {for:"idle"}` must not wait for.
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

using ToolHandler = std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)>;

struct ToolDef
{
    std::string    name;
    std::string    title;
    std::string    description;
    nlohmann::json input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    // ToolRegistry::add sets it from tool_lane().
    ToolLane       lane        = ToolLane::Api;
    // The tool's access label, sent as MCP's annotations. Clients act on them: Codex asks a person before every call
    // of a destructive tool, and Claude Code runs read-only calls in parallel and retries idempotent ones. read_only:
    // it changes nothing in the app or the project; screenshot is, since its PNG replaces no file without overwrite,
    // and export_replay, which can clear the journal, is not. destructive: every call deletes what a person made, with
    // no undo: delete_preset. A tool that can lose work only when an argument asks for it is not: new_project,
    // generate_calibration_test and open_files drop unsaved changes only with discard_changes, an output replaces a file only with
    // overwrite, and encode_recording deletes its source only with delete_source. Nor is a tool that runs what its
    // arguments name (click and the other input tools, run_action, call_tool, run_replay, run_script), though what it runs may be. The
    // client sees both in the arguments, and the label would ask on every call. open_world: it can reach outside
    // OrcaSlicer (a printer, the cloud account, the network, or anything the computer allows), including through what
    // it runs.
    bool           read_only   = false;
    bool           destructive = false;
    bool           idempotent  = false;
    bool           open_world  = false;
    GuardClass     guard       = GuardClass::Mutate;
    // Pure observation: runs on the request thread and is never a job. Everything that can activate
    // UI runs as a job instead, because activating UI can open a modal that blocks the call.
    bool           inline_call = false;
    // May run while an MCP slice/export operation holds the project (cancel_job).
    bool           allowed_during_operation = false;
    // `cancel_job {job}` stops a running call of this tool that is not an operation (encode_recording): the
    // handler sees ToolContext::cancelled() and ends.
    bool           cancellable = false;
    // Recorded by the session journal for export_replay. Observation tools turn this off, except
    // wait and screenshot, which a replay needs.
    bool           journal   = true;
    // How long a call may run before the reply returns a job envelope and the call continues.
    int            budget_ms = 45000;
    ToolHandler    handler;
};

// The lane of the tool `name`: the session and recovery tools and Python are Core, the GUI-driving
// tools Ui, everything else (the typed tools, call_tool, plugin operations) Api.
ToolLane tool_lane(const std::string& name);
// "core", "api" or "ui": the tag that starts each tool's description.
const char* lane_name(ToolLane lane);

class ToolRegistry
{
public:
    // Adds or replaces a tool, setting its lane from tool_lane() and starting its description with the
    // lane's tag in brackets.
    void add(ToolDef def);
    const ToolDef* find(const std::string& name) const;
    const std::vector<ToolDef>& tools() const { return m_tools; }
    // Resolves names the table does not hold (plugin operations), which tools/list never shows. Set once,
    // before any call.
    void set_dynamic(std::function<std::shared_ptr<const ToolDef>(const std::string&)> resolver) { m_dynamic = std::move(resolver); }
    // A registered tool, else one the resolver gives, or nullptr. The pointer keeps a resolved tool alive
    // through its call.
    std::shared_ptr<const ToolDef> lookup(const std::string& name) const;
    // Up to `limit` registered names closest to `name`, for "did you mean" errors.
    std::vector<std::string> similar(const std::string& name, std::size_t limit = 3) const;

private:
    std::vector<ToolDef>                                                     m_tools;
    std::function<std::shared_ptr<const ToolDef>(const std::string&)>        m_dynamic;
};

// For a call of call_tool ({tool, args}): replaces `def` and `args` with its target's, normalized and
// validated, so the call runs and is journaled as the target's own. Returns an error message, or "" (also
// for any other tool, which it leaves as it is).
std::string resolve_api_call(const ToolRegistry& registry, std::shared_ptr<const ToolDef>& def, nlohmann::json& args);

// Executes a tool for the protocol layer: the service implementation adds jobs, budgets, guards
// and the journal; tests call the handler directly.
class ToolCaller
{
public:
    virtual ~ToolCaller() = default;
    virtual ToolResult call(const ToolDef& def, const nlohmann::json& args) = 0;
};

// Validates `args` against the subset of JSON Schema the tool catalog uses (object properties with
// type, enum, minimum/maximum, required, additionalProperties:false, array items). Returns an empty
// string when valid, else a message naming the offending property.
std::string validate_arguments(const nlohmann::json& schema, const nlohmann::json& args);

// Rewrites, before validation, what agents often send in place of what the schema asks for: an
// argument under a name the tool does not take but that stands for one it does, which the call leaves
// out and the value fits (ref for target, key for keys, text for value, action for id or button, object for objects,
// plate for plates, filter for query, settings for values, target for root), a value written in
// another JSON form (a number, boolean, array or object as a string, a single item for an array),
// a value outside its argument's enum that only one omitted argument's enum takes (set_view's view
// "front" as camera), and the x and y, or width and height, that list_windows reports, both given, as
// the position or size array of a tool that takes that array, of integers, and neither part. Anything
// that still does not fit is left for validate_arguments to report.
void normalize_arguments(const nlohmann::json& schema, nlohmann::json& args);

}}} // namespace Slic3r::GUI::Mcp
