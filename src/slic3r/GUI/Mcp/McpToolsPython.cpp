// script_run, script_list_api and script_describe_api: Python scripts against orca.host, the plugin API, and the orca
// package's reference, read one module or class at a time.

#include "JobRegistry.hpp"
#include "McpArguments.hpp"
#include "McpCore.hpp"
#include "McpPaths.hpp"
#include "McpPersonRun.hpp"
#include "AutomationRuntime.hpp"
#include "McpTools.hpp"

#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonScriptRunner.hpp"
#include "slic3r/plugin/PythonSession.hpp"
#include "slic3r/plugin/host/PluginHostTools.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace py = pybind11;

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

void register_python_tools(CommandCatalog& catalog)
{
    NativeCommand run = mutate_tool(
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
    run.cancellable = true;
    // A script reaches outside the app: files, programs and the network.
    run.open_world = true;
    catalog.add(run);

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
}

}}} // namespace Slic3r::GUI::Mcp
