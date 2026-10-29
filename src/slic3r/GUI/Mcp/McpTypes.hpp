#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The name of a top-level window the MCP GUI tools never see: ui_snapshot and locators skip it, web
// view scripts never pick it, keys sent without a target never go to it, and window screenshots
// leave it out. The script runner dialog carries it, so a script it runs can't act on it.
inline constexpr char kHiddenFromAutomation[] = "OrcaHiddenFromMcp";

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
    // ancestors (a script run the user stopped); long-running handlers poll it between steps.
    virtual bool cancelled() const = 0;
    // Calls another tool as a child of this call (script steps). Returns what a client would get:
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
};

using ToolHandler = std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)>;

struct ToolDef
{
    std::string    name;
    std::string    title;
    std::string    description;
    nlohmann::json input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    bool           read_only   = false;
    bool           destructive = false;
    bool           idempotent  = false;
    GuardClass     guard       = GuardClass::Mutate;
    // Pure observation: runs on the request thread and is never a job. Everything that can activate
    // UI runs as a job instead, because activating UI can open a modal that blocks the call.
    bool           inline_call = false;
    // May run while an MCP slice/export operation holds the project (cancel).
    bool           allowed_during_operation = false;
    // Recorded by the session journal for export_script. Observation tools turn this off, except
    // wait and screenshot, which a replay needs.
    bool           journal   = true;
    // How long a call may run before the reply returns a job envelope and the call continues.
    int            budget_ms = 45000;
    ToolHandler    handler;
};

class ToolRegistry
{
public:
    void add(ToolDef def);
    const ToolDef* find(const std::string& name) const;
    const std::vector<ToolDef>& tools() const { return m_tools; }
    // Up to `limit` registered names closest to `name`, for "did you mean" errors.
    std::vector<std::string> similar(const std::string& name, std::size_t limit = 3) const;

private:
    std::vector<ToolDef> m_tools;
};

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

}}} // namespace Slic3r::GUI::Mcp
