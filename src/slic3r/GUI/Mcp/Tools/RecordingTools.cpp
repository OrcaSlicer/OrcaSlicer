// The place Recordings, replays and scripts. window_screenshot, what the agent looks at, and window_save_screenshot,
// the person's Take Screenshot. recording_start, recording_stop and recording_encode: a video of what OrcaSlicer
// shows, optionally captioned with the current tool call and its pointer highlighted, and its encode to MP4.
// replay_run and replay_export: deterministic replays of tool calls, and turning a session into one. script_run,
// script_list_api and script_describe_api: Python scripts against orca.host, the plugin API, and the orca package's
// reference, read one module or class at a time.

#include "slic3r/GUI/Mcp/AutomationRuntime.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/McpPersonRun.hpp"
#include "slic3r/GUI/Mcp/McpRecorder.hpp"
#include "slic3r/GUI/Mcp/McpReplay.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/ModalTracker.hpp"
#include "slic3r/GUI/Mcp/RecordingSettings.hpp"
#include "slic3r/GUI/Mcp/ScreenCapture.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"
#include "slic3r/GUI/Mcp/VideoEncode.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/plugin/host/PluginHostTools.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonScriptRunner.hpp"
#include "slic3r/plugin/PythonSession.hpp"

#include <wx/bookctrl.h>
#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <ios>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;
namespace py = pybind11;

namespace {

std::string base64(const std::string& bytes)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(bytes.size()), '\0');
    out.resize(b64::encode(out.data(), bytes.data(), bytes.size()));
    return out;
}

std::string read_file(const fs::path& path)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    if (!in)
        throw ToolFailure("Cannot read " + path_to_utf8(path));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& text)
{
    boost::nowide::ofstream out(path_to_utf8(path).c_str(), std::ios::binary | std::ios::trunc);
    out << text;
    if (!out)
        throw ToolFailure("Cannot write " + path_to_utf8(path));
}

std::string timestamp()
{
    const std::time_t t = std::time(nullptr);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

// An invalid replay is a tool error flagged `invalid`, which --run turns into exit code 2.
ToolResult invalid(const std::string& message) { return ToolResult::error("Invalid replay: " + message, {{"invalid", true}}); }

} // namespace

json run_replay_job(AutomationRuntime& runtime, ToolContext& ctx, const Replay& replay, ReplayContext& sctx, const ReplayRunOptions& options)
{
    boost::system::error_code ec;
    fs::create_directories(sctx.out_dir, ec);
    if (ec)
        throw ToolFailure("Cannot create " + path_to_utf8(sctx.out_dir) + ": " + ec.message());

    const std::string job = ctx.job_id();
    runtime.begin_run_scope(job);
    struct ScopeEnd
    {
        AutomationRuntime& runtime;
        std::string job;
        bool        recording_waited = false; // the run below waited for the replay's recording already
        ~ScopeEnd() { runtime.end_run_scope(job, !recording_waited); }
    } scope_end{runtime, job};

    const auto stopped = [&ctx] { return ctx.cancelled(); };

    ReplayHost host;
    host.call     = [&ctx](const std::string& tool, const json& a) { return ctx.call_child(tool, a); };
    host.wait_job = [&runtime, &stopped](const std::string& id, int timeout_s) {
        return runtime.jobs().wait(id, std::chrono::seconds(timeout_s), stopped);
    };
    host.cancelled     = stopped;
    host.step_started  = options.step_started;
    host.step_finished = options.step_finished;
    host.open_dialogs  = [&runtime] { return runtime.ui().open_dialogs(); };

    json result = run_replay(replay, sctx, host, options.only_step);
    // A recording the replay started ends with it, its file closed before the result is written; one still
    // being written after the wait is named, since an encode of it would fail.
    std::optional<std::string> unfinished = runtime.recorder().end_owned_by(job, "run_ended", true);
    scope_end.recording_waited            = true;
    if (unfinished)
        result["unfinished_recording"] = *unfinished;
    result["out_dir"] = path_to_utf8(sctx.out_dir);
    if (options.write_result) {
        const fs::path file = sctx.out_dir / "result.json";
        write_file(file, result.dump(2));
        result["result_file"] = path_to_utf8(file);
    }
    return result;
}

namespace {

// A run's orca.host.tools calls, as child calls of the script_run job, so each has its tool's guards,
// admission and undo, and a cancel of the run reaches them.
class RunBridge final : public ToolBridge
{
public:
    explicit RunBridge(ToolContext& ctx) : m_ctx(ctx) {}
    json call(const std::string& tool, const json& args) override
    {
        ToolResult r = m_ctx.call_child(tool, args);
        if (r.is_error)
            throw ToolCallError(r.message, r.data);
        return r.data;
    }

private:
    ToolContext& m_ctx;
};

// The Run Script panel's session's orca.host.tools calls: child calls under a lineage node of the session's job, so a
// stop cancels every call in flight, inline ones (app_wait) included, without stopping the session, and the next work's
// calls run under a fresh node.
class SessionBridge final : public ToolBridge
{
public:
    SessionBridge(AutomationRuntime& runtime, const std::string& session_job)
        : m_runtime(runtime), m_node(runtime.jobs(), "script_run", session_job)
    {}
    json call(const std::string& tool, const json& args) override
    {
        ToolResult r = m_runtime.core().call_child_named(tool, args, m_node.id());
        if (r.is_error)
            throw ToolCallError(r.message, r.data);
        return r.data;
    }
    // A stop or time-out, from the session's watcher: the node's calls are cancelled, and its operations, as the
    // job_cancel cancels them.
    void stop_calls()
    {
        const std::string node = m_node.id();
        m_stopped              = true;
        m_runtime.jobs().request_stop(node);
        // Found again on the UI thread: the runtime may be gone by the time the closure runs.
        m_runtime.post_ui([node] {
            if (AutomationRuntime* runtime = wxGetApp().automation_if_started())
                runtime->cancel_operations_of(node);
        });
    }
    // As a work starts: after a stop its calls get a node no stop was asked for.
    void renew_after_stop()
    {
        if (m_stopped.exchange(false))
            m_node.renew();
    }

private:
    AutomationRuntime& m_runtime;
    LineageNode        m_node;
    std::atomic<bool>  m_stopped{false};
};

// python_api_reference()'s walk over the orca package.
constexpr const char* kReferenceSource = R"PY(

import inspect, re, types

# A signature line pybind11 starts a docstring with: "name(...) -> T", numbered after "Overloaded function.",
# and without a name for a property's getter.

_SIGNATURE = re.compile(r"^(?:\d+\. )?(\w*)\(.*\)(?: -> .+)?$")

_SELF = re.compile(r"\bself: [\w.]+")
# The types of a module's constants, such as orca.host.ui.PD_CAN_ABORT.

_CONSTANTS = (bool, int, float, str)

def _split(names, doc):
    """The signature lines of a docstring whose function is called one of names, and the rest of it."""
    signatures, text = [], []
    for line in (doc or "").splitlines():
        stripped = line.strip()
        match = _SIGNATURE.match(stripped)
        if match and match.group(1) in names:
            signatures.append(_SELF.sub("self", re.sub(r"^\d+\. ", "", stripped)))
        elif stripped != "Overloaded function.":
            text.append(line)
    # An overloaded function's first line, name(*args, **kwargs), stands for the numbered overloads after it.
    if len(signatures) > 1:
        signatures = [s for s in signatures if not s.endswith("(*args, **kwargs)")]
    return signatures, "\n".join(text).strip()

def _entry(name, obj, kind):
    short = name.rsplit(".", 1)[-1]
    if kind == "class":
        # A class's own docstring, not one inherited from a base, then what its constructors say of their
        # arguments; the constructors' signatures are the class's.
        init = vars(obj).get("__init__")
        signatures, made = _split(("__init__",), init.__doc__ if init is not None else None)
        signatures = [short + s[len("__init__"):].replace("(self, ", "(").replace("(self)", "()").replace(" -> None", "")
                      for s in signatures]
        doc = inspect.cleandoc(obj.__doc__ or "")
        if made and made != doc:
            doc = (doc + "\n\n" + made).strip()
        return {"name": name, "kind": kind, "signature": " | ".join(signatures) or short, "doc": doc}
    if kind == "property":
        getter = obj.fget.__doc__ if obj.fget is not None else ""
        returns = re.match(r"^\w*\(.*\) -> (.+)$", (getter or "").strip().split("\n")[0])
        _, doc = _split((short, ""), inspect.getdoc(obj))
        return {"name": name, "kind": kind, "signature": short + (": " + returns.group(1) if returns else ""), "doc": doc}
    signatures, doc = _split((short, ""), inspect.getdoc(obj))
    if not signatures:
        try:
            signatures = [short + str(inspect.signature(obj))]
        except (TypeError, ValueError):
            signatures = [short]
    return {"name": name, "kind": kind, "signature": " | ".join(signatures), "doc": doc}

def reference(package):
    out, seen, constants = [], set(), []
    def walk(module, prefix):
        if id(module) in seen:
            return
        seen.add(id(module))
        out.append(_entry(prefix.rstrip("."), module, "module"))
        for name, value in sorted(vars(module).items()):
            if name.startswith("_"):
                continue
            full = prefix + name
            if isinstance(value, types.ModuleType):
                if value.__name__.startswith(package.__name__):
                    walk(value, full + ".")
            elif inspect.isclass(value):
                out.append(_entry(full, value, "class"))
                # An enum lists its values in its docstring; name and value are every enum's.
                values = getattr(value, "__members__", None)
                for member, attr in sorted(vars(value).items()):
                    if member.startswith("_") or (values is not None and (member in values or member in ("name", "value"))):
                        continue
                    # A class holds a static method as a staticmethod object, whose documentation is the
                    # staticmethod type's; pybind11 documented the function inside it.
                    if isinstance(attr, (staticmethod, classmethod)):
                        attr = attr.__func__
                    kind = "property" if isinstance(attr, property) else "method" if callable(attr) else None
                    if kind:
                        out.append(_entry(full + "." + member, attr, kind))
            elif callable(value):
                out.append(_entry(full, value, "function"))
            elif isinstance(value, _CONSTANTS):
                constants.append(len(out))
                out.append({"name": full, "kind": "constant", "signature": name + " = " + repr(value), "doc": ""})
            elif type(value).__module__.startswith(package.__name__) and not hasattr(type(value), "__members__"):
                # An instance the package offers by name, such as orca.host.preset.process; enum values
                # exported into a module are their enum's.
                kind_of = type(value)
                out.append({"name": full, "kind": "attribute", "signature": name + ": " + kind_of.__module__ + "." + kind_of.__name__,
                            "doc": inspect.cleandoc(kind_of.__doc__ or "")})
    walk(package, package.__name__ + ".")
    # A constant has no docstring of its own: the documentation of what takes it says what it means.
    for i in constants:
        word = re.compile(r"\b" + re.escape(out[i]["name"].rsplit(".", 1)[-1]) + r"\b")
        users = [e["name"] for e in out if e["kind"] != "constant" and word.search(e["doc"])]
        if users:
            out[i]["doc"] = "See " + ", ".join(users) + "."
    return out
)PY";

} // namespace

json python_api_reference()
{
    py::gil_scoped_acquire gil;
    try {
        py::dict scope;
        py::exec(kReferenceSource, scope);
        json entries = json::array();
        for (const auto item : scope["reference"](py::module_::import("orca")).cast<py::list>()) {
            json entry = json::object();
            for (const auto field : item.cast<py::dict>())
                entry[field.first.cast<std::string>()] = field.second.cast<std::string>();
            entries.push_back(std::move(entry));
        }
        return entries;
    } catch (py::error_already_set& e) {
        throw ToolFailure(std::string("Cannot read the Python API: ") + e.what());
    }
}

json python_api_children(const json& reference, const std::string& within)
{
    const auto is_scope = [&within](const json& e) {
        return e["name"] == within && (e["kind"] == "module" || e["kind"] == "class");
    };
    if (std::none_of(reference.begin(), reference.end(), is_scope)) {
        json        modules = json::array();
        std::string names;
        for (const json& e : reference)
            if (e["kind"] == "module") {
                modules.push_back(e["name"]);
                names += (names.empty() ? "" : ", ") + e["name"].get<std::string>();
            }
        throw ToolFailure(within + " is no module or class of the orca package; its modules are " + names,
                          {{"code", "invalid_argument"}, {"modules", std::move(modules)}});
    }
    // An entry is directly inside `within` when its name is `within`, a dot and one more part.
    const std::string prefix = within + '.';
    json              items  = json::array();
    for (const json& e : reference) {
        const std::string& name = e["name"].get_ref<const std::string&>();
        if (boost::algorithm::starts_with(name, prefix) && name.find('.', prefix.size()) == std::string::npos)
            items.push_back({{"name", name}, {"kind", e["kind"]}, {"signature", e["signature"]},
                             {"summary", first_sentence(e["doc"].get<std::string>())}});
    }
    return items;
}

json python_api_entry(const json& reference, const std::string& name)
{
    std::vector<std::pair<std::size_t, std::size_t>> scored; // {distance, index}, the reference's order among equals
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const std::string& candidate = reference[i]["name"].get_ref<const std::string&>();
        if (candidate == name)
            return reference[i];
        scored.emplace_back(edit_distance(name, candidate), i);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    json        closest = json::array();
    std::string names;
    for (std::size_t i = 0; i < scored.size() && closest.size() < 3; ++i) {
        closest.push_back(reference[scored[i].second]["name"]);
        names += (names.empty() ? "" : ", ") + closest.back().get<std::string>();
    }
    throw ToolFailure("No entry " + name + " in the orca package; the closest are " + names,
                      {{"code", "invalid_argument"}, {"did_you_mean", std::move(closest)}});
}

bool python_has_numpy()
{
    py::gil_scoped_acquire gil;
    try {
        return !py::module_::import("importlib.util").attr("find_spec")("numpy").is_none();
    } catch (py::error_already_set&) {
        return false;
    }
}

ToolResult run_python_session_job(AutomationRuntime& runtime, ToolContext& ctx, PythonSession& session, const std::string& filename)
{
    const std::string job = ctx.job_id();
    // The session waits for the person between works: `app_wait {for: "idle"}` counts only the calls it makes.
    runtime.jobs().set_idle(job);
    // What its calls leave behind is the session's, as a run's is; its rules act only while a work runs.
    runtime.begin_run_scope(job);
    runtime.set_run_scope_paused(job, true);
    struct ScopeEnd
    {
        AutomationRuntime& runtime;
        std::string job;
        ~ScopeEnd() { runtime.end_run_scope(job); }
    } scope_end{runtime, job};

    SessionBridge        bridge(runtime, job);
    ScopedToolBridge     bound(bridge);
    PythonSessionRequest request;
    request.identity     = kScriptIdentityPerson;
    request.filename     = filename;
    request.cancelled    = [&ctx] { return ctx.cancelled(); };
    request.stop_calls   = [&bridge] { bridge.stop_calls(); };
    request.work_started = [&runtime, &bridge, job] {
        bridge.renew_after_stop();
        runtime.set_run_scope_paused(job, false);
    };
    request.work_ended = [&runtime, job] {
        runtime.set_run_scope_paused(job, true);
        runtime.release_run_presses(job);
    };
    host_python_session(request, session);
    return ToolResult::ok({{"status", "ended"}});
}

std::shared_ptr<PersonRun> start_python_session(AutomationRuntime& runtime, std::shared_ptr<PythonSession> session,
                                                const boost::filesystem::path& file, PersonRun::Events events)
{
    const NativeCommand* script_run = NativeCommands::instance().find("script_run");
    if (script_run == nullptr || runtime.stopping() || !session)
        return nullptr;
    auto run      = std::make_shared<PersonRun>(runtime, std::move(events));
    auto hosted   = std::make_shared<std::atomic<bool>>(false);
    auto filename = file.empty() ? std::string("<script_run>") : path_to_utf8(file);
    // script_run's properties, outside the session journal, as a person's run.
    NativeCommand def = *script_run;
    def.journal       = false;
    def.run           = [&runtime, run, session, hosted, filename](const json&, ToolContext& ctx) {
        run->set_job(ctx.job_id());
        hosted->store(true);
        // A handler that refuses never hosts the session: it ends here all the same.
        struct Ended
        {
            PythonSession& session;
            ~Ended()
            {
                if (!session.ended())
                    session.host_returned();
            }
        } ended{*session};
        return run_python_session_job(runtime, ctx, *session, filename);
    };

    FollowedCall call;
    call.command = std::move(def);
    call.origin  = {{"source", "run_script"}};
    call.person  = true;
    call.ended   = [&runtime, run, session, hosted](const std::optional<ToolResult>& r) {
        run->mark_ended();
        // Refused before its handler ran: the session was never hosted.
        if (r && !hosted->load())
            session->host_returned();
        if (!r || runtime.stopping() || !r->is_error)
            return;
        run->post_finished(json{{"error", r->message}}, json());
    };
    call.stop = [run] { run->stop(); };
    if (!runtime.follow(std::move(call)))
        return nullptr;
    return run;
}

namespace {

// The recording tools only read the app, so they run behind dialogs and during operations, but each
// is a job: ownership is read from the job's ancestry, and an encode outlives the reply budget.
NativeCommand record_tool(std::string name, std::string title, std::string description, json schema, CommandFn handler)
{
    NativeCommand d = NativeCommands::long_command(std::move(name), std::move(title), {}, std::move(schema), std::move(handler));
    d.description   = std::move(description);
    d.listed        = true;
    d.guard         = GuardClass::Inspect;
    d.journal       = true;
    return d;
}

json int_prop(int minimum, int maximum, const std::string& description)
{
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}

std::string read_text(const fs::path& path)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    std::ostringstream      ss;
    ss << in.rdbuf();
    return ss.str();
}

void remove_quietly(const fs::path& path)
{
    boost::system::error_code ec;
    fs::remove(path, ec);
}

int process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return int(getpid());
#endif
}

std::vector<std::string> encode_preset_keys()
{
    std::vector<std::string> keys;
    for (const EncodePreset& p : encode_presets())
        keys.emplace_back(p.key);
    return keys;
}

} // namespace

ToolResult start_encode(AutomationRuntime& runtime, const json& args, const std::string& stop_job)
{
    const NativeCommand* encode = NativeCommands::instance().find("recording_encode");
    if (encode == nullptr || runtime.stopping())
        return ToolResult::error("OrcaSlicer is shutting down");
    NativeCommand def = *encode;
    def.journal   = false;
    def.budget_ms = 0; // the envelope at once; the result comes from app_wait
    return runtime.core().call(def, args, {{"source", "agent"}, {"under", stop_job}});
}

void register_recording_tools(CommandCatalog& catalog)
{
    NativeCommand screenshot = inspect_tool(
        "window_screenshot", L("Take a screenshot"),
        "Capture what OrcaSlicer shows and return it as an image, downscaled to `max_dim` on the long edge (default 1280), "
        "plus the full-size PNG written to `save_path` (or the session's output folder). `target: \"main\"` (the main "
        "window), \"canvas\" (the visible 3D view exactly as rendered: scene, toolpaths, toolbars, ImGui panels; on the "
        "Design tab its view), "
        "\"dialog\" (the topmost modal dialog), or a ref: a window's from `window_list` (one that is not modal, say), or a "
        "widget's from `widget_list`, cropped to that widget (a page element's ref captures its whole web view). The result "
        "says when parts may differ from the "
        "screen (approximate). Point the view first with `view_set`.",
        object_schema({{"target", {{"type", json::array({"string", "object"})},
                                    {"description", "main (default), canvas, dialog, or a widget's ref or locator from `widget_list`."}}},
                       {"max_dim", {{"type", "integer"}, {"minimum", 256}, {"maximum", 2000}, {"description", "Long edge of the returned image."}}},
                       {"save_path", prop("string", "Absolute .png path for the full-size image.")},
                       {"overwrite", prop("boolean", "Replace an existing `save_path`.")},
                       {"include_image", prop("boolean", "Return the image itself (default true); false returns only the path.")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const json target_arg = args.value("target", json("main"));
            const std::string target = target_arg.is_string() ? target_arg.get<std::string>() : std::string();
            json              locator;
            fs::path          path;
            if (args.contains("save_path")) {
                std::string err;
                auto        p = resolve_output_path(arg_string(args, "save_path"), arg_bool(args, "overwrite", false), err);
                if (!p)
                    throw ToolFailure(err);
                path = *p;
            } else {
                path = timestamped_path(output_dir(), "screenshot", ".png");
            }
            const int max_dim = std::clamp(arg_int(args, "max_dim", 1280), 256, 2000);
            json      info;
            wxImage   full;
            if (target != "main" && target != "canvas" && target != "dialog")
                // Scrolled into view first, and painted before the capture below runs.
                ctx.run_ui([&] {
                    UiTarget t = runtime.inspector().resolve(target_arg);
                    runtime.inspector().reveal(t);
                    return json();
                });
            // A canvas that comes back blank has not drawn yet; it does within moments.
            bool blank = false;
            for (int attempt = 0;; ++attempt) {
                ctx.run_ui([&] {
                    std::string             err;
                    std::optional<Capture>  cap;
                    if (target == "canvas") {
                        ui_plater(); // refuses while OrcaSlicer starts or closes
                        GLCanvas3D* canvas = shown_canvas();
                        if (canvas == nullptr)
                            throw ToolFailure("No 3D view is shown; switch to one with view_set tab");
                        cap = capture_canvas(*canvas, err);
                    } else if (target == "dialog") {
                        wxDialog* top = runtime.modals().top();
                        if (top == nullptr)
                            throw ToolFailure(no_modal_dialog(runtime.modals()));
                        cap = capture_window(*top, err);
                    } else if (target != "main") {
                        // A widget: its top-level window, cropped to the widget unless it is that window.
                        const UiTarget t = runtime.inspector().resolve(target_arg);
                        locator          = runtime.inspector().locator_for(t);
                        // A page element is captured as its web view.
                        wxWindow*      w = t.window.get();
                        if ((t.kind != UiTarget::Kind::Window && t.kind != UiTarget::Kind::WebElement) || w == nullptr)
                            throw ToolFailure("target must be main, canvas, dialog or the ref of a widget");
                        if (t.kind == UiTarget::Kind::WebElement) {
                            // A replay captures the same web view, by its own locator: a page element's
                            // has none the window_screenshot's target takes.
                            UiTarget view;
                            view.kind    = UiTarget::Kind::Window;
                            view.window  = w;
                            view.locator = {{"class", "webview"}};
                            if (const std::string within = within_of(w); !within.empty())
                                view.locator["within"] = within;
                            locator = runtime.inspector().locator_for(view);
                        }
                        if (t.page >= 0) {
                            // A tab: its page, which only the selected tab shows.
                            auto* book = dynamic_cast<wxBookCtrlBase*>(w);
                            if (book == nullptr || t.page >= int(book->GetPageCount()))
                                throw ToolFailure("The tab is gone; call widget_list again");
                            if (book->GetSelection() != t.page)
                                throw ToolFailure("The tab's page is not shown; click the tab (widget_click) first");
                            w = book->GetPage(size_t(t.page));
                        }
                        if (GLCanvas3D* canvas = runtime.inspector().canvas_of(&t); canvas != nullptr && canvas->get_wxglcanvas() == w) {
                            cap = capture_canvas(*canvas, err);
                        } else {
                            wxWindow* top = wxGetTopLevelParent(w);
                            // A popup is a native window of its own, which its window's capture draws in.
                            if (dynamic_cast<wxPopupWindow*>(top) != nullptr)
                                throw ToolFailure("The widget is in a popup; a screenshot of its window shows the popup, and widget_list "
                                                  "{target: \"popup\"} lists it");
                            auto* tlw = dynamic_cast<wxTopLevelWindow*>(top);
                            if (tlw == nullptr)
                                throw ToolFailure("The widget has no window");
                            tlw->Update();
                            cap = capture_window(*tlw, err);
                            if (cap && w != tlw) {
                                // The capture is in physical pixels, like client_offset_in().
                                const wxPoint at    = client_offset_in(*w, *tlw);
                                const double  scale = tlw->GetContentScaleFactor();
                                const wxSize  size(int(std::lround(w->GetSize().x * scale)), int(std::lround(w->GetSize().y * scale)));
                                const wxRect  r = wxRect(at, size).Intersect(wxRect(cap->image.GetSize()));
                                if (r.IsEmpty())
                                    throw ToolFailure("The widget is outside its window's visible area");
                                cap->image = cap->image.GetSubImage(r);
                            }
                        }
                    } else {
                        wxTopLevelWindow* frame = wxGetApp().mainframe;
                        if (frame == nullptr)
                            throw ToolFailure("The main window is not up yet");
                        cap = capture_window(*frame, err);
                    }
                    if (!cap)
                        throw ToolFailure(err);
                    full = cap->image;
                    info = {{"source", target.empty() ? json(target_arg) : json(target)}, {"width", full.GetWidth()}, {"height", full.GetHeight()}};
                    if (cap->approximate)
                        info["approximate"] = true;
                    if (!cap->note.empty())
                        info["note"] = cap->note;
                    blank = cap->blank_canvas;
                    return json();
                });
                if (!blank || attempt >= 10 || ctx.cancelled())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            if (blank) {
                const std::string undrawn = undrawn_window(ctx);
                info["approximate"]       = true;
                info["note"] = info.value("note", std::string()) + (info.contains("note") ? "; " : "") + "the 3D view came back blank" +
                               (undrawn.empty() ? ", as if not drawn yet" : ": " + undrawn);
            }
            if (target_arg.is_string() && !locator.is_null()) {
                // A replay needs the locator; refs mean nothing in another session.
                json recorded      = args;
                recorded["target"] = locator;
                ctx.set_journal_args(std::move(recorded));
            }
            // Encoding and scaling run off the UI thread.
            if (!full.SaveFile(from_path(path), wxBITMAP_TYPE_PNG))
                throw ToolFailure("Could not write " + path_to_utf8(path));
            info["path"]   = path_to_utf8(path);
            ToolResult out = ToolResult::ok(info);
            if (arg_bool(args, "include_image", true)) {
                const wxSize  size  = fit_within(full.GetSize(), max_dim);
                const wxImage scaled = size == full.GetSize() ? full : full.Scale(size.x, size.y, wxIMAGE_QUALITY_HIGH);
                out.data["image_width"]  = size.x;
                out.data["image_height"] = size.y;
                out.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", base64(encode_png(scaled))}});
            }
            return out;
        });
    screenshot.journal = true; // a replay takes the same screenshots
    catalog.add(screenshot);

    const std::string saved = "Save a PNG of the main window in the folder Start Recording saves in (the person's Downloads unless "
                              "they chose another) and show the person a notification naming it; the reply has its path. Agents use "
                              "the window_screenshot tool, which returns the image.";
    NativeCommand save_screenshot = menu_command("window_save_screenshot", L("Take Screenshot"), saved, object_schema({}),
                                                json::array({NativeCommands::action("take_screenshot", L("Take Screenshot"),
                                                                                    L("Commands"), "screenshot", json::object(), saved)}),
                                                [](const json&) { return MenuItems::take_screenshot(); });
    // A menu item with no argument form: tool_list lists it and tool_call runs it, the tool list does not show it.
    save_screenshot.listed = false;
    catalog.add(std::move(save_screenshot));

    NativeCommand run = mutate_tool(
        "replay_run", L("Run a replay"),
        std::string("Replay a list of tool calls in order and report each step: `{\"orca_replay_version\": \"") +
            REPLAY_VERSION +
            "\", name, vars, steps: [{tool, args, expect, optional, await, save_as, timeout_s}]}`, JSON with comments. "
            "Strings may use `${replay_dir}`, `${out_dir}`, `${env:NAME}`, `${vars.NAME}` and `${jobs.NAME}`. A step that "
            "returns a job is awaited unless `await: false`, which stores the job under `save_as` so later steps can answer "
            "the dialog it opened. `expect` maps a JSON pointer into the step's result to matchers (`eq` `ne` `gt` `ge` `lt` "
            "`le` `contains` `regex` `exists` `empty`); the first failed step or expectation stops the run unless the step is "
            "`optional`. Writes `result.json` to `out_dir` (default: a new folder `replay-<timestamp>` in "
            "`orca_action_replay` in the temp folder, or in the `--outputdir` folder of a `--run` launch). Reply: `passed`, "
            "`steps`, `failed_step`, `out_dir` and `result_file`. `dialog_auto_answer` rules a replay registers end with it. "
            "`replay_export` turns this session into one.",
        object_schema({{"path", prop("string", "Absolute path of a replay file.")},
                       {"replay", {{"type", json::array({"object", "string"})},
                                   {"description", "The replay itself, as an object or as its JSON text, instead of `path`."}}},
                       {"out_dir", prop("string", "Absolute directory for result.json and screenshots; created if missing.")},
                       {"vars", {{"type", "object"}, {"description", "Values that override the replay's vars."}}}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const bool has_path = args.contains("path"), has_replay = args.contains("replay");
            if (has_path == has_replay)
                return invalid("give exactly one of path and replay");

            std::optional<Replay> replay;
            ReplayContext         sctx;
            std::string           err;
            if (has_path) {
                const auto path = resolve_input_path(arg_string(args, "path"), err);
                if (!path)
                    return invalid(err);
                std::string text;
                try {
                    text = read_file(*path);
                } catch (const ToolFailure& f) {
                    return invalid(f.what());
                }
                replay          = parse_replay(text, err);
                sctx.replay_dir = path->parent_path();
            } else
                replay = parse_replay(inline_replay_text(args["replay"]), err);
            if (!replay)
                return invalid(err);
            for (const ReplayStep& step : replay->steps)
                if (NativeCommands::instance().lookup(step.tool) == nullptr)
                    return invalid("unknown tool " + step.tool);
            if (args.contains("vars"))
                for (const auto& [key, value] : args["vars"].items())
                    replay->vars[key] = value;

            sctx.out_dir = args.contains("out_dir") ? path_from_utf8(arg_string(args, "out_dir")) : replay_output_dir();
            if (!sctx.out_dir.is_absolute())
                return invalid("out_dir must be an absolute path");
            if (!has_path)
                sctx.replay_dir = sctx.out_dir;
            json result = run_replay_job(runtime, ctx, *replay, sctx, ReplayRunOptions{});
            return ToolResult::ok(std::move(result));
        });
    // Its steps call any tool, script_run included.
    run.open_world = true;
    catalog.add(run);

    NativeCommand exp = inspect_tool(
        "replay_export", L("Export this session as a replay"),
        "Turn the calls this session made into a replay that `replay_run` runs: mutating calls, `app_wait`, `window_screenshot` and the "
        "recording tools, in order, with refs replaced by locators, job replies replayed with await:false, and `window_screenshot` "
        "and recording paths moved under `${out_dir}`. "
        "Calls that failed, at once or as a job later, are left out (a `script_run` that raised or timed out replied, so it "
        "stays); so is a call whose job is still running (unless it "
        "waits on a dialog), which `left_out_running` lists as `{tool, job}`. "
        "Reply: the replay, which with path is also written there. clear:true then forgets the calls made so far.",
        object_schema({{"name", prop("string", "Replay name.")},
                       {"path", prop("string", "Absolute .json path to write.")},
                       {"overwrite", prop("boolean", "Replace an existing file.")},
                       {"clear", prop("boolean", "Forget the calls recorded so far once exported.")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            if (!ctx.caller_plugin().empty())
                throw ToolFailure("replay_export exports the agent's session, which a plugin's calls are not part of");
            json running;
            json replay = runtime.journal().to_replay(arg_string(args, "name", "session " + timestamp()), &running);
            json out{{"steps", replay["steps"].size()}};
            if (!running.empty())
                out["left_out_running"] = std::move(running);
            if (args.contains("path")) {
                std::string err;
                const auto  path = resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err);
                if (!path)
                    throw ToolFailure(err);
                write_file(*path, replay.dump(2));
                out["path"] = path_to_utf8(*path);
            }
            if (arg_bool(args, "clear", false))
                runtime.journal().clear();
            out["replay"] = std::move(replay);
            return ToolResult::ok(std::move(out));
        });
    exp.read_only = false;
    // clear empties the journal, so a second call exports less.
    exp.idempotent = false;
    catalog.add(exp);

    NativeCommand script = mutate_tool(
        "script_run", L("Run a Python script"),
        "Run Python code inside OrcaSlicer as its main program (__name__ == \"__main__\") against `orca.host`, the whole plugin "
        "API (`script_list_api` lists it): read objects, meshes and settings, compute, use `orca.host.ui` and the preset "
        "API, and call these same tools, with their guards and undo, through `orca.host.tools.call`(name, args). Mesh and "
        "matrix arrays need numpy, which "
        "`script_list_api` says is installed or not; a file run's PEP 723 block can install it. The code runs on a worker "
        "thread, where calls documented as UI thread only raise RuntimeError. The run ends once its code and the non-daemon "
        "threads and pool tasks it started are done; its windows close with it. `args` holds the args you pass; set `result` "
        "to a JSON value to return it; print() output comes back as output. Reply: `{status: finished | raised | cancelled | "
        "timed_out, result, output, traceback?, message?}`; a script that raised changed what it changed before. The code's "
        "own file access is held to OrcaSlicer's folders (its data folder, and read only its resources folder and, for a "
        "file run, the file's folder), and it starts no programs or threads and reaches no network: such an access raises "
        "PermissionError, unless a person has let Python scripts run unrestricted in Preferences "
        "(`app_state.python.scripts_unrestricted`), when the threads and pool tasks it starts call the tools too while "
        "the run lasts; the tools it calls work as they do for you. Recorded "
        "by `replay_export` as one step with its code, which a replay runs again against the project as it is then.",
        object_schema({{"code", prop("string", "Python source, run as a module body.")},
                       {"args", {{"type", "object"}, {"description", "Bound to the name `args` in the code."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600},
                                      {"description", "The run is stopped as `timed_out` after this many seconds (default 300)."}}},
                       {"filename", prop("string", "The name tracebacks show for the code (default <script_run>). An existing file's "
                                                   "path makes it a file run, as `python file.py` runs it: __file__, sys.argv, imports "
                                                   "from its folder and its PEP 723 dependencies; any other name only names it. "
                                                   "What runs is still `code`: pass the file's text there.")}},
                      {"code"}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            // A plugin's script is the plugin's own code: its identity and grants.
            const std::string& plugin   = ctx.caller_plugin();
            const std::string  identity = !plugin.empty()                          ? plugin :
                                          runtime.jobs().person_in(ctx.lineage()) ? kScriptIdentityPerson :
                                                                                    kScriptIdentityAgent;

            // A run owns what its calls leave behind (auto-answer rules, a recording, a held press) as a JSON
            // script does: it all ends with the run.
            const std::string job = ctx.job_id();
            // A run may wait for a person in its window: `app_wait {for: "idle"}` counts only the calls it makes.
            runtime.jobs().set_idle(job);
            runtime.begin_run_scope(job);
            struct ScopeEnd
            {
                AutomationRuntime& runtime;
                std::string job;
                ~ScopeEnd() { runtime.end_run_scope(job); }
            } scope_end{runtime, job};

            RunBridge        bridge(ctx);
            ScopedToolBridge bound(bridge);
            PythonRunRequest request;
            request.code      = arg_string(args, "code");
            request.args      = args.value("args", json::object());
            request.identity  = identity;
            request.filename  = arg_string(args, "filename", "<script_run>");
            // 0, below the schema's minimum, is passed only in process, by the Run Script dialog, whose Stop ends a run:
            // every external path validates the arguments. It means no time limit.
            request.budget    = std::chrono::seconds(arg_int(args, "timeout_s", 300));
            request.cancelled = [&ctx] { return ctx.cancelled(); };
            // A stop or time-out cancels the run's tool calls in flight too, so one that waits returns at once.
            request.stop_calls = [&runtime, job] { runtime.jobs().request_stop(job); };
            const PythonRunOutcome out = run_python_code(request);
            if (out.status == "unavailable")
                throw ToolFailure(out.message);

            json data{{"status", out.status}, {"result", out.result}, {"output", out.output}};
            if (!out.traceback.empty())
                data["traceback"] = out.traceback;
            if (!out.message.empty())
                data["message"] = out.message;
            // A run that did not finish is reported, not failed, as replay_run reports a failed step: the
            // journal keeps it, and a replay of it fails again (D7).
            return ToolResult::ok(std::move(data));
        });
    // job_cancel {job} stops a run: its code is interrupted at its next bytecode.
    script.cancellable = true;
    // A script reaches outside the app: files, programs and the network.
    script.open_world = true;
    catalog.add(script);

    // Both read the package as Python runs it now, holding its runtime for the read as script_run does.
    const auto while_python_runs = [](const auto& read) {
        if (!PythonInterpreter::instance().is_initialized())
            throw ToolFailure("Python is not available in this OrcaSlicer");
        PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
        if (!lease)
            throw ToolFailure("Python is shutting down");
        return read();
    };
    catalog.add(inspect_tool(
        "script_list_api", L("List the Python API"),
        "List `orca`, the Python API of `script_run` scripts and plugins, in the running build, one module or class at a time: "
        "`orca.host` (the project's model and meshes, presets and their editors, slicing data, `orca.host.ui` windows and "
        "dialogs, `orca.host.tools` calling these tools) and the classes, events and pages plugins build on. It lists what is "
        "directly inside `within`, a module or a class (default `orca`): each entry's `name`, `kind`, `signature` (a "
        "class's constructors) and the first sentence of its documentation. A module or class it lists is listed with "
        "`within` set "
        "to its name, and `script_describe_api {name}` gives an entry's whole documentation. Page with `offset`. `numpy` says "
        "whether numpy is installed, which mesh and matrix arrays need.",
        object_schema({{"within", {{"type", "string"}, {"default", kPythonApiRoot},
                                   {"description", "A module's or class's dotted name, e.g. \"orca.host\" or "
                                                   "\"orca.host.TriangleMesh\"."}}},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "At most this many (default 20)."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [while_python_runs](const json& args, ToolContext&) {
            return ToolResult::ok(while_python_runs([&args] {
                json out     = page(python_api_children(python_api_reference(), arg_string(args, "within", kPythonApiRoot)), args, 20);
                out["numpy"] = python_has_numpy();
                return out;
            }));
        }));
    catalog.add(inspect_tool(
        "script_describe_api", L("Describe a Python API entry"),
        "Read one entry of `orca`, the Python API of `script_run` scripts and plugins, by its dotted name as `script_list_api` "
        "lists it (`orca.host.TriangleMesh.face_normals`): its `kind`, its `signature` (a class's constructors) and its whole "
        "documentation. A name no entry has is refused with the closest names.",
        object_schema({{"name", prop("string", "An entry's dotted name, as `script_list_api` lists it.")}}, {"name"}),
        [while_python_runs](const json& args, ToolContext&) {
            return ToolResult::ok(
                while_python_runs([&args] { return python_api_entry(python_api_reference(), arg_string(args, "name")); }));
        }));

    // The Run Script panel's menu item, which has no argument form.
    const std::string panel = "Show or hide the Run Script panel, where a person writes and runs Python scripts cell by cell. Agents "
                              "run Python with the script_run tool instead.";
    NativeCommand toggle_panel = menu_command("script_toggle_panel", L("Run Script"), panel, object_schema({}),
                                             json::array({NativeCommands::action("toggle_run_script_panel", L("Run Script"),
                                                                                 L("Commands"), "media_play", json::object(), panel)}),
                                             [](const json&) { return MenuItems::toggle_run_script_panel(); });
    // A menu item with no argument form: tool_list lists it and tool_call runs it, the tool list does not show it.
    toggle_panel.listed = false;
    catalog.add(std::move(toggle_panel));

    // The Speed Dial's Start Recording is its action: with no arguments it opens the action's dialog.
    NativeCommand start = record_tool(
        "recording_start", L("Start Recording"),
        "Record a video of OrcaSlicer until `recording_stop`: MJPEG frames with their real timestamps in a Matroska (.mkv) "
        "file. Captions show the tool call acting on the app at the bottom (secret setting values never show); pointer "
        "highlights the pointer, its clicks rippling. Capture takes at most about a third of the UI thread, so the frame "
        "rate drops where capturing is slow. One recording at a time: a replay's or script's ends with it, a client's when "
        "the server is turned off. With encode, the `recording_stop` that reports it also writes an MP4 beside it and "
        "deletes the .mkv unless `keep_raw_video_file`; `recording_encode` encodes any recording later. With no arguments it "
        "opens the Start Recording dialog (`blocked_by_dialog`), whose controls are named after their settings for "
        "`widget_set_value`; its \"Start recording\" starts the person's recording, which neither a replay's or script's end "
        "nor the server turning off ends, and the dialog remembers the settings. `silent: true` starts one at once with the "
        "defaults.",
        object_schema({{"target", enum_prop({"main", "canvas"}, "main (default): the main window and its dialogs; canvas: the 3D view shown.")},
                       {"fps", int_prop(1, 30, "Frames per second at most (default 10).")},
                       {"max_dim", int_prop(320, 3840, "Long edge of the video in pixels (default 1920).")},
                       {"jpeg_quality", int_prop(30, 95, "JPEG quality of the frames (default 80).")},
                       {"max_seconds", int_prop(1, 3600, "The recording ends on its own after this long (default 3600).")},
                       {"captions", prop("boolean", "Caption the tool call acting on the app (default false).")},
                       {"pointer", prop("boolean", "Highlight the pointer and show its clicks (default false).")},
                       {"encode", prop("boolean", "Encode it to an MP4 once stopped; needs an ffmpeg (default false).")},
                       {"encode_preset", enum_prop(encode_preset_keys(), "With encode: the settings the fields below change "
                                                                         "(default balanced: h265, crf 28, medium, main).")},
                       {"codec", enum_prop({"h265", "h264"}, "With encode: the codec (default the preset's).")},
                       {"crf", int_prop(0, 51, "With encode: constant rate factor, lower is better and larger (default the preset's).")},
                       {"speed", enum_prop(encoder_speeds(), "With encode: slower makes a smaller file (default the preset's).")},
                       {"profile", enum_prop({"high", "main", "baseline", "main10"},
                                             "With encode: h264 high, main or baseline; h265 main or main10 (default the preset's, else "
                                             "the codec's first).")},
                       {"keep_raw_video_file", prop("boolean", "With encode: keep the .mkv beside the MP4 (default false).")},
                       {"save_path", prop("string", "Absolute .mkv path; default the session's output folder.")},
                       {"overwrite", prop("boolean", "Replace an existing `save_path`, and with encode its MP4.")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            RecordingOptions o;
            o.target      = arg_string(args, "target", "main");
            o.fps         = arg_int(args, "fps", 10);
            o.max_dim     = arg_int(args, "max_dim", 1920);
            o.quality     = arg_int(args, "jpeg_quality", 80);
            o.max_seconds = arg_int(args, "max_seconds", 3600);
            o.captions    = arg_bool(args, "captions", false);
            o.pointer     = arg_bool(args, "pointer", false);
            // The encode with the Start Recording dialog's defaults, the Balanced preset.
            RecordingSettings settings;
            if (const std::string err = apply_recording_overrides(settings, args); !err.empty())
                throw ToolFailure(err);
            if (std::string err; settings.encode && !find_ffmpeg(err))
                throw ToolFailure("Cannot encode: " + err);
            if (args.contains("save_path")) {
                const std::string path = arg_string(args, "save_path");
                if (!boost::algorithm::iends_with(path, ".mkv"))
                    throw ToolFailure("save_path must end with .mkv: the recording is Matroska");
                std::string err;
                auto        p = resolve_output_path(path, arg_bool(args, "overwrite", false), err);
                if (!p)
                    throw ToolFailure(err);
                o.path = *p;
            } else {
                o.path = timestamped_path(output_dir(), "recording", ".mkv", {".mp4"});
            }
            if (settings.encode) {
                o.encode = encode_arguments(settings, path_to_utf8(o.path));
                if (arg_bool(args, "overwrite", false))
                    o.encode["overwrite"] = true;
            }
            // Like auto-answer rules: a run's recording ends with the run, a client's with the endpoint.
            const std::vector<std::string> ancestry = runtime.jobs().ancestry(ctx.job_id());
            o.owner                                 = runtime.run_owner(ancestry);
            // A call of a run that has ended (its scope gone before this ran): not a client's call.
            if (o.owner.empty() && runtime.recorder().owner_ended_in(ancestry))
                throw ToolFailure("The replay or script that started the recording has ended");
            // Reserved here, off the UI thread: comparing paths may touch the disk. start() keeps the
            // reservation for the recording or releases it.
            const std::shared_ptr<RecordingFiles>& files = runtime.recorder().files();
            const uint64_t                         owner = files->new_owner();
            if (const std::string why = files->reserve(owner, {{o.path, RecordingFiles::Use::Recording}}); !why.empty())
                throw ToolFailure("Cannot record there: " + why);
            bool handed_over = false;
            struct Release
            {
                RecordingFiles& files;
                uint64_t        owner;
                const bool&     handed_over;
                ~Release()
                {
                    if (!handed_over)
                        files.release(owner);
                }
            } release{*files, owner, handed_over};
            json out = ctx.run_ui([&] {
                ui_plater(); // refuses while OrcaSlicer starts or closes
                // On the UI thread, as close_client_scope() runs: a client's start queued while the endpoint
                // closed must not begin a recording nothing would end.
                if (o.owner.empty() && !runtime.client_scope_open())
                    throw ToolFailure("The MCP server was turned off; the recording was not started");
                handed_over = true; // start() owns the reservation from here, success or not
                return runtime.recorder().start(o, owner);
            });
            if (settings.encode)
                out["encode"] = {{"codec", settings.codec},
                                 {"crf", settings.crf},
                                 {"speed", settings.speed},
                                 {"profile", settings.profile},
                                 {"keep_raw_video_file", settings.keep_raw_video_file}};
            // The limit it runs under, which a call that leaves it out did not choose.
            out["max_seconds"] = o.max_seconds;
            // The path used, so a replay records into its own output folder under the same name and its
            // encode finds it there.
            json recorded         = args;
            recorded["save_path"] = out["path"];
            ctx.set_journal_args(std::move(recorded));
            return ToolResult::ok(out);
        });
    start.actions = json::array({NativeCommands::action("start_recording", L("Start Recording"), L("Commands"), "recording_start",
                                                        json::object(),
                                                        "Runs recording_start with no arguments, as Start Recording does: the Start "
                                                        "Recording dialog, whose Start recording starts the person's recording, "
                                                        "which neither the end of a replay or script nor the server turning off ends.")});
    catalog.add(NativeCommands::with_person_form(std::move(start), [](const json&) { return NativeCommands::start_person_recording(); }));

    // The Speed Dial's Stop Recording is its action: with no arguments it stops as the action does.
    NativeCommand stop = record_tool("recording_stop", L("Stop Recording"),
                        "Stop the running recording. Call it with `silent: true` for the report: it waits for the file and "
                        "returns it with its seconds, frames captured and "
                        "written, frames dropped when encoding fell behind, failed when they could not be encoded, or "
                        "skipped when the window could not be captured, "
                        "the size, the mean UI-thread time a capture took (`capture_ms`), and why it ended. A recording that ended on its own (`max_seconds`, the replay or script that started it ended, a "
                        "write error) is reported once by the next `recording_stop`. For a recording started with encode, it then "
                        "starts the MP4 encode and returns it under encode: a job, whose `app_wait {for: \"job\"}` returns "
                        "`recording_encode`'s result, whose `raw_video_file_deleted` says whether the .mkv was deleted. "
                        "Called with no arguments, it stops "
                        "the recording as Stop Recording does, whoever started it: without waiting for its "
                        "file, it shows the person a notification naming it and replies with its path; the next "
                        "`recording_stop` with `silent: true` reports it as ended `stopped_in_app`.",
                        object_schema({}), [](const json&, ToolContext& ctx) {
                            AutomationRuntime& runtime = ctx.runtime();
                            // The last state too, even if the timer has not ticked since the last call. Without the
                            // UI thread (a native dialog holds it), the recording ends with its last frame.
                            try {
                                ctx.run_ui([&] {
                                    runtime.recorder().capture_now();
                                    return json();
                                });
                            } catch (const ToolFailure&) {
                            }
                            json encode;
                            json report = runtime.recorder().stop_and_report(std::chrono::seconds(60), encode);
                            if (report.is_null())
                                throw ToolFailure("No recording is running");
                            if (!encode.is_null()) {
                                const ToolResult r = start_encode(runtime, encode, ctx.job_id());
                                report["encode"]   = r.is_error ? json{{"error", r.message}} : r.data;
                            }
                            return ToolResult::ok(report);
                        });
    stop.actions = json::array({NativeCommands::action("stop_recording", L("Stop Recording"), L("Commands"), "recording_stop",
                                                       json::object(),
                                                       "Runs recording_stop with no arguments, as Stop Recording does: it stops the "
                                                       "recording, whoever started it, and shows the person a notification naming its "
                                                       "file.")});
    // The person's stop opens nothing: like the silent one, it stops a recording behind a dialog.
    stop = NativeCommands::with_person_form(std::move(stop), [](const json&) { return NativeCommands::stop_recording_in_app(); },
                                            /*person_form_behind_dialogs=*/true);
    // It has no other argument, and neither form asks: silent picks the form that waits and reports.
    stop.input_schema["properties"]["silent"]["description"] =
        "true waits for the file and returns the report, and the encode job when the recording has one; a call with no "
        "arguments stops as Stop Recording does.";
    catalog.add(stop);

    NativeCommand encode = record_tool(
        "recording_encode", L("Encode a recording"),
        "Encode a recording into an H.265 (or H.264) MP4 with the system ffmpeg (ORCA_FFMPEG, else ffmpeg on PATH; it "
        "needs libx265 or libx264), keeping its timing. Takes this session's last finished recording unless path names "
        "one, and writes beside it unless `save_path` says where. A long encode returns a job: `app_wait` waits for it, or `job_cancel {job}` "
        "stops it. Once the MP4 is written, it deletes the .mkv if this session recorded it, unless `keep_raw_video_file`; "
        "any other .mkv is kept. `raw_video_file_deleted` in the reply says which.",
        object_schema({{"path", prop("string", "Absolute .mkv of a finished recording; default this session's last.")},
                       {"save_path", prop("string", "Absolute .mp4 path; default the recording's, with .mp4.")},
                       {"overwrite", prop("boolean", "Replace an existing `save_path`.")},
                       {"codec", enum_prop({"h265", "h264"}, "Default h265.")},
                       {"crf", int_prop(0, 51, "Constant rate factor, lower is better and larger (default 28 for h265, 23 for h264).")},
                       {"speed", enum_prop(encoder_speeds(), "The encoder's preset: slower makes a smaller file of the same quality (default medium).")},
                       {"profile", enum_prop({"high", "main", "baseline", "main10"},
                                             "h264: high, main or baseline; h265: main, or main10 for 10 bits per sample. Default the "
                                             "encoder's (high, main).")},
                       {"keep_raw_video_file", prop("boolean", "Keep the .mkv beside the MP4 (default false: a recording this "
                                                               "session made is deleted once the MP4 is written).")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const std::shared_ptr<RecordingFiles>& files = runtime.recorder().files();
            std::string                            err;
            fs::path                               source;
            if (args.contains("path")) {
                auto p = resolve_input_path(arg_string(args, "path"), err);
                if (!p)
                    throw ToolFailure(err);
                source = *p;
            } else if (auto last = files->last_finished()) {
                source = *last;
            } else {
                throw ToolFailure("No recording of this session has finished; give path");
            }
            if (!boost::algorithm::iends_with(path_to_utf8(source), ".mkv"))
                throw ToolFailure("path must be a .mkv recording");
            if (boost::system::error_code ec; !fs::is_regular_file(source, ec))
                throw ToolFailure("No such recording: " + path_to_utf8(source));

            const bool overwrite = arg_bool(args, "overwrite", false);
            fs::path   dest;
            if (args.contains("save_path")) {
                auto p = resolve_output_path(arg_string(args, "save_path"), overwrite, err);
                if (!p)
                    throw ToolFailure(err);
                dest = *p;
            } else {
                dest = source;
                dest.replace_extension(".mp4");
                if (boost::system::error_code ec; fs::exists(dest, ec) && !overwrite)
                    throw ToolFailure(path_to_utf8(dest) + " exists; set overwrite or give save_path");
            }
            if (RecordingFiles::same_file(source, dest))
                throw ToolFailure("save_path is the recording itself");
            // The .mkv goes once the MP4 is written unless it is to be kept, and only a recording this session made:
            // any other .mkv is the person's own file, so it stays.
            const bool keep_asked = arg_bool(args, "keep_raw_video_file", false);
            const bool ours       = files->is_finished(source);
            const bool remove     = !keep_asked && ours;

            const std::string codec   = arg_string(args, "codec", "h265");
            const std::string encoder = encoder_for(codec);
            const int         crf     = arg_int(args, "crf", codec == "h265" ? 28 : 23);
            const std::string profile = arg_string(args, "profile", "");
            if (const std::vector<std::string> profiles = codec_profiles(codec);
                !profile.empty() && std::find(profiles.begin(), profiles.end(), profile) == profiles.end())
                throw ToolFailure(codec + " has no " + profile + " profile");
            const auto        ffmpeg  = find_ffmpeg(err);
            if (!ffmpeg)
                throw ToolFailure(err);

            const uint64_t owner = files->new_owner();
            if (const std::string why = files->reserve(owner, {{source, remove ? RecordingFiles::Use::ExclusiveSource : RecordingFiles::Use::Source},
                                                               {dest, RecordingFiles::Use::Destination}});
                !why.empty())
                throw ToolFailure("Cannot encode now: " + why);
            // Every file this call makes is its own, so failing or a cancel removes only those.
            const std::string tag   = "." + std::to_string(process_id()) + "-" + std::to_string(owner);
            const fs::path    part  = dest.parent_path() / (dest.filename().native() + path_from_utf8(tag + ".part").native());
            const fs::path    log   = dest.parent_path() / (dest.filename().native() + path_from_utf8(tag + ".log").native());
            const fs::path    probe = dest.parent_path() / (dest.filename().native() + path_from_utf8(tag + ".probe").native());
            struct Cleanup
            {
                RecordingFiles&       files;
                uint64_t              owner;
                std::vector<fs::path> paths;
                bool                  release = true;
                ~Cleanup()
                {
                    for (const fs::path& p : paths)
                        remove_quietly(p);
                    if (release)
                        files.release(owner);
                }
            } cleanup{*files, owner, {part, log, probe}};
            const auto cancelled = [&ctx] { return ctx.cancelled(); };

            // What this ffmpeg is: the options that keep the timing depend on its version, and the encoder
            // must be built in.
            auto listing = [&](const char* what) {
                const ProcessOutcome r = run_process(*ffmpeg, {"-hide_banner", what}, probe, {}, cancelled, std::chrono::seconds(20));
                if (ctx.cancelled())
                    throw ToolFailure("The encode was cancelled");
                if (!r.started || r.timed_out || r.exit_code != 0)
                    throw ToolFailure("Cannot run " + path_to_utf8(*ffmpeg) + " " + what);
                return read_text(probe);
            };
            const FfmpegVersion version = parse_ffmpeg_version(listing("-version"));
            if (parse_encoders(listing("-encoders")).count(encoder) == 0)
                throw ToolFailure(path_to_utf8(*ffmpeg) + " has no " + encoder +
                                  " encoder. Install an ffmpeg built with it (Homebrew, winget Gyan.FFmpeg, Debian/Ubuntu ffmpeg) or "
                                  "point ORCA_FFMPEG at one");

            EncodeSpec spec;
            spec.input  = path_to_utf8(source);
            spec.output = path_to_utf8(part);
            spec.codec   = codec;
            spec.crf     = crf;
            spec.speed   = arg_string(args, "speed", "medium");
            spec.profile = profile;
            ProcessOutcome run;
            // A build whose version is unknown may refuse the current spelling of the timing options: the next.
            const std::vector<EncodeTiming> timings = timing_candidates(version);
            for (std::size_t i = 0; i < timings.size(); ++i) {
                spec.timing = timings[i];
                run         = run_process(*ffmpeg, encode_args(spec), {}, log, cancelled, std::chrono::hours(4));
                const bool refused = run.started && run.gone && !run.cancelled && !run.timed_out && run.exit_code != 0 &&
                                     refused_timing_option(file_tail(log, 6));
                if (!refused || i + 1 == timings.size())
                    break;
            }
            if (!run.gone) {
                // It may still read the source and write its files: they are left for it, and the source and
                // destination stay reserved for the session, so no later encode deletes or writes them under it.
                cleanup.paths   = {probe};
                cleanup.release = false;
                throw ToolFailure("ffmpeg did not exit after it was stopped; " + path_to_utf8(part) + " and " + path_to_utf8(log) +
                                  " are left, and " + path_to_utf8(source) + " stays in use until OrcaSlicer restarts");
            }
            if (run.cancelled)
                throw ToolFailure("The encode was cancelled");
            if (!run.started)
                throw ToolFailure("Cannot run " + path_to_utf8(*ffmpeg));
            if (run.timed_out || run.exit_code != 0) {
                const std::string tail = file_tail(log, 6);
                throw ToolFailure(run.timed_out ? "ffmpeg did not finish within 4 hours" :
                                                  "ffmpeg failed" + (tail.empty() ? std::string() : ":\n" + tail));
            }
            const double seconds = mp4_duration(part).value_or(0.);

            boost::system::error_code ec;
            if (!overwrite && fs::exists(dest, ec))
                throw ToolFailure(path_to_utf8(dest) + " appeared while encoding; set overwrite to replace it");
            fs::rename(part, dest, ec); // replaces an existing file on every platform
            if (ec)
                throw ToolFailure("Cannot write " + path_to_utf8(dest) + ": " + ec.message());
            if (remove && fs::remove(source, ec))
                files->forget(source);
            const uintmax_t size = fs::file_size(dest, ec);
            json            out{{"path", path_to_utf8(dest)},
                                {"source", path_to_utf8(source)},
                                {"codec", codec},
                                {"encoder", encoder},
                                {"seconds", std::round(seconds * 10.) / 10.},
                                {"size_bytes", ec ? 0 : size}};
            out["raw_video_file_deleted"] = remove && !fs::exists(source, ec);
            if (!keep_asked && !ours)
                out["note"] = "The .mkv was kept: an encode deletes only a recording this session made";
            return ToolResult::ok(out);
        });
    encode.cancellable = true;
    catalog.add(encode);
}

}}} // namespace Slic3r::GUI::Mcp
