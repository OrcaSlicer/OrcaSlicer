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

namespace Slic3r { namespace GUI {

// Defined in ActionRegistry.hpp, which brings in wx: a ToolResult only holds one through a shared_ptr, which
// the code that makes one (NativeCommands.cpp) completes.
struct AppActionRunResult;
// Defined in CommandCatalog.hpp, which includes this header.
struct NativeCommand;
class CommandCatalog;

namespace Mcp {

class McpService;

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
    // The MCP service the call runs under: what a command that reaches the automation runtime (jobs, the
    // journal, recordings, the UI inspector) reads it from, so that the catalog is built without a service.
    // Throws when the call has none: std::logic_error here, ToolFailure where one cannot be started.
    virtual McpService& service() { throw std::logic_error("This call runs without an MCP service"); }
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

// For a call of call_tool ({tool, args}): replaces `command` and `args` with its target's, normalized and
// validated, so the call runs and is journaled as the target's own. Returns an error message, or "" (also
// for any other command, which it leaves as it is).
std::string resolve_call(const CommandCatalog& catalog, std::shared_ptr<const NativeCommand>& command, nlohmann::json& args);

// Executes a command for the protocol layer: the service implementation adds jobs, budgets, guards
// and the journal; tests call the command's function directly.
class ToolCaller
{
public:
    virtual ~ToolCaller() = default;
    virtual ToolResult call(const NativeCommand& command, const nlohmann::json& args) = 0;
};

// ---- arguments and name matching (McpArguments.cpp) ------------------------------------------

// Validates `args` against the subset of JSON Schema the tool catalog uses (object properties with
// type, enum, minimum/maximum, required, additionalProperties:false, array items, and the
// "if": {"minProperties": 1}, "then": {"required": [...]} of with_silent()). Returns an empty string when
// valid, else a message naming the offending property.
std::string validate_arguments(const nlohmann::json& schema, const nlohmann::json& args);

// Whether `args` asks for a command's person's form: an object with no members, or null. A command that has
// a person's form and a silent one does the person's then (it asks, as a person's pick does), and the
// silent one when it is given any argument.
bool person_form(const nlohmann::json& args);
// `args` without `silent`, which only chooses the silent form.
nlohmann::json silent_args(nlohmann::json args);
// The schema of a two-form command from its silent form's `schema`: it gains the property `silent`, which
// takes only true and asks for the silent form with its defaults, and a top-level `required` list moves to
// "if": {"minProperties": 1}, "then": {"required": [...]}, since the call with no arguments, the person's
// form, needs none of them.
nlohmann::json with_silent(nlohmann::json schema);

// Readies a call's arguments for `command`, as every caller of a command does: normalizes `args` in place
// (normalize_arguments()) and returns why validate_arguments() refuses them; empty when they are valid.
std::string argument_error(const NativeCommand& command, nlohmann::json& args);

// Rewrites, before validation, what agents often send in place of what the schema asks for: an
// argument under a name the tool does not take but that stands for one it does, which the call leaves
// out and the value fits (ref for target, key for keys, text for value, action for id or button, object for objects,
// plate for plates, filter for query, settings for values, path for save_path, index for feature, params or
// values for fields), a value written in
// another JSON form (a number, boolean, array or object as a string, a single item for an array),
// a value outside its argument's enum that only one omitted argument's enum takes (set_view's canvas
// "front" as camera), and the x and y, or width and height, that list_windows reports, both given, as
// the position or size array of a tool that takes that array, of integers, and neither part. Anything
// that still does not fit is left for validate_arguments to report.
void normalize_arguments(const nlohmann::json& schema, nlohmann::json& args);

// The Levenshtein distance between `a` and `b`: the fewest insertions, deletions and substitutions of one
// character that turn one into the other. A "did you mean" ranks the names a mistyped one may stand for by it,
// each lookup with its own threshold.
std::size_t edit_distance(const std::string& a, const std::string& b);

// The words of a search query: lower case, split at anything but a letter or a digit. "to" or "of"
// starts a word of most help texts, so words of fewer than three letters are left out when the query
// has longer ones.
std::vector<std::string> query_words(const std::string& query);

// How well an entry matches the words of query_words(): {words found in `name` or `text`, of them those
// found in `name`}, {0, 0} when none is. A word is found where a word of the text starts with it, so
// "retract" finds "retraction"; the text's words are split at underscores, dots and the like and also
// at a capital in a CamelCase name, so "mesh" finds TriangleMesh.
std::pair<int, int> match_score(const std::vector<std::string>& words, const std::string& name, const std::string& text);

// The first sentence of `text` on one line, for a listing: up to the first full stop followed by a space
// or the end, "e.g." and "i.e." aside; the whole text when it has none.
std::string first_sentence(const std::string& text);

}}} // namespace Slic3r::GUI::Mcp
