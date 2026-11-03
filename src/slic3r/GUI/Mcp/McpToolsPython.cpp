// run_python and python_api: Python scripts against orca.host, the plugin API, and the orca package's
// reference.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "PythonScriptsDialog.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonScriptRunner.hpp"
#include "slic3r/plugin/host/PluginHostTools.hpp"

#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace py = pybind11;

namespace {

// A run's orca.host.tools calls, as child calls of the run_python job, so each has its tool's guards,
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

json rank_python_api(const json& reference, const std::string& query)
{
    const std::vector<std::string>              words = query_words(query);
    std::vector<std::pair<std::pair<int, int>, std::size_t>> scored;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const json&               e     = reference[i];
        const std::pair<int, int> score = match_score(words, e["name"].get<std::string>(),
                                                      e["signature"].get<std::string>() + '\n' + e["doc"].get<std::string>());
        if (score.first > 0)
            scored.emplace_back(score, i);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    json out = json::array();
    for (const auto& [score, i] : scored)
        out.push_back(reference[i]);
    return out;
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

void register_python_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    ToolDef run = mutate_tool(
        "run_python", L("Run Python code"),
        "Run Python code inside OrcaSlicer as its main program (__name__ == \"__main__\") against orca.host, the whole plugin "
        "API (python_api searches it): read objects, meshes and settings, compute, use orca.host.ui and the preset "
        "API, and call these same tools, with their guards and undo, through orca.host.tools.call(name, args), from the code "
        "and from the threads and pool tasks it starts while the run lasts. Mesh and matrix arrays need numpy, which "
        "python_api says is installed or not; a file run's PEP 723 block can install it. The code runs on a worker "
        "thread, where calls documented as UI thread only raise RuntimeError. The run ends once its code and the non-daemon "
        "threads and pool tasks it started are done; its windows close with it. `args` holds the args you pass; set `result` "
        "to a JSON value to return it; print() output comes back as output. Returns {status: finished | raised | cancelled | "
        "timed_out, result, output, traceback?, message?}; a script that raised changed what it changed before. Python runs "
        "with OrcaSlicer's full access to the computer, so a person must allow Python scripts in Preferences first. Recorded "
        "by export_script as one step with its code, which a replay runs again against the project as it is then.",
        object_schema({{"code", prop("string", "Python source, run as a module body.")},
                       {"args", {{"type", "object"}, {"description", "Bound to the name `args` in the code."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600},
                                      {"description", "The run is stopped as timed_out after this many seconds (default 300)."}}},
                       {"filename", prop("string", "The name tracebacks show for the code (default <run_python>). An existing file's "
                                                   "path makes it a file run, as `python file.py` runs it: __file__, sys.argv, imports "
                                                   "from its folder and its PEP 723 dependencies; any other name only names it.")}},
                      {"code"}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string& plugin = ctx.caller_plugin();
            std::string        identity;
            if (!plugin.empty()) {
                // A plugin's script is the plugin's own code: its identity and grants, no setting.
                identity = plugin;
            } else {
                identity = service.person_run(ctx.lineage()) ? kScriptIdentityPerson : kScriptIdentityAgent;
                const bool allowed = ctx.observe_ui([] { return json(python_scripts_allowed(*wxGetApp().app_config)); }).get<bool>();
                if (!allowed)
                    throw ToolFailure("Python scripts are off. A person can allow them in Preferences > AI features > Python "
                                      "scripts; no tool can.");
            }

            // A run owns what its calls leave behind (auto-answer rules, a recording, a held press) as a JSON
            // script does: it all ends with the run.
            const std::string job = ctx.job_id();
            // A run may wait for a person in its window: `wait {for: "idle"}` counts only the calls it makes.
            service.jobs().set_idle(job);
            service.begin_script_scope(job);
            struct ScopeEnd
            {
                McpService& service;
                std::string job;
                ~ScopeEnd() { service.end_script_scope(job); }
            } scope_end{service, job};

            RunBridge        bridge(ctx);
            ScopedToolBridge bound(bridge);
            PythonRunRequest request;
            request.code      = arg_string(args, "code");
            request.args      = args.value("args", json::object());
            request.identity  = identity;
            request.filename  = arg_string(args, "filename", "<run_python>");
            // 0, below the schema's minimum, is passed only in process, by the Run Script dialog, whose Stop ends a run:
            // every external path validates the arguments. It means no time limit.
            request.budget    = std::chrono::seconds(arg_int(args, "timeout_s", 300));
            request.cancelled = [&ctx] { return ctx.cancelled(); };
            // A stop or time-out cancels the run's tool calls in flight too, so one that waits returns at once.
            request.stop_calls = [&service, job] { service.request_stop(job); };
            const PythonRunOutcome out = run_python_code(request);
            if (out.status == "unavailable")
                throw ToolFailure(out.message);

            json data{{"status", out.status}, {"result", out.result}, {"output", out.output}};
            if (!out.traceback.empty())
                data["traceback"] = out.traceback;
            if (!out.message.empty())
                data["message"] = out.message;
            // A run that did not finish is reported, not failed, as run_script reports a failed step: the
            // journal keeps it, and a replay of it fails again (D7).
            return ToolResult::ok(std::move(data));
        });
    // cancel {job} stops a run: its code is interrupted at its next bytecode.
    run.cancellable = true;
    reg.add(run);

    reg.add(inspect_tool(
        "python_api", L("Search the Python API"),
        "Search orca, the Python API of run_python scripts and plugins, in the running build: orca.host (the project's "
        "model and meshes, presets and their editors, slicing data, orca.host.ui windows and dialogs, orca.host.tools "
        "calling these tools) and the classes, events and pages plugins build on. Each entry has its kind, signature (a "
        "class's constructors) and documentation. A query ranks them, best match first; without one, the whole package is "
        "listed with each entry's first sentence. Page with offset. numpy says whether numpy is installed, which mesh and "
        "matrix arrays need.",
        object_schema({{"query", prop("string", "A few words, such as \"mesh normals\" or \"preset value\".")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "At most this many (default 20)."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext&) {
            if (!PythonInterpreter::instance().is_initialized())
                throw ToolFailure("Python is not available in this OrcaSlicer");
            PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
            if (!lease)
                throw ToolFailure("Python is shutting down");
            const json        reference = python_api_reference();
            const std::string query     = arg_string(args, "query");
            json              items     = json::array();
            if (query.empty()) {
                for (const json& e : reference)
                    items.push_back({{"name", e["name"]}, {"kind", e["kind"]}, {"signature", e["signature"]},
                                     {"summary", first_sentence(e["doc"].get<std::string>())}});
            } else
                items = rank_python_api(reference, query);
            json out     = page(items, args, 20);
            out["numpy"] = python_has_numpy();
            return ToolResult::ok(std::move(out));
        }));
}

}}} // namespace Slic3r::GUI::Mcp
