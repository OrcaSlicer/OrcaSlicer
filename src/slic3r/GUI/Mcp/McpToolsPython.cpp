// run_python and python_api: Python scripts against orca.host, the plugin API, and its reference.

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

#include <chrono>
#include <string>
#include <utility>

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

// The reference python_api searches: every function, class and method under orca.host with its
// signature and documentation, from the running build's bindings.
constexpr const char* kReferenceSource = R"PY(
import inspect, types

def _first_line(doc):
    return doc.strip().splitlines()[0] if doc and doc.strip() else ""

def _entry(name, obj, kind):
    doc = inspect.getdoc(obj) or ""
    try:
        signature = name.rsplit(".", 1)[-1] + str(inspect.signature(obj))
    except (TypeError, ValueError):
        # pybind11 functions start their docstring with the signature.
        signature = _first_line(doc)
        if signature and "(" in signature:
            doc = doc[len(signature):].strip()
        else:
            signature = name.rsplit(".", 1)[-1]
    return {"name": name, "kind": kind, "signature": signature, "doc": doc}

def reference(host):
    out = []
    seen = set()
    def walk(module, prefix):
        if id(module) in seen:
            return
        seen.add(id(module))
        out.append(_entry(prefix.rstrip("."), module, "module"))
        for name, value in sorted(vars(module).items()):
            if name.startswith("_"):
                continue
            if isinstance(value, types.ModuleType):
                if value.__name__.startswith(host.__name__):
                    walk(value, prefix + name + ".")
            elif inspect.isclass(value):
                out.append(_entry(prefix + name, value, "class"))
                for member, attr in sorted(vars(value).items()):
                    if member.startswith("_"):
                        continue
                    kind = "property" if isinstance(attr, property) else "method" if callable(attr) else None
                    if kind:
                        out.append(_entry(prefix + name + "." + member, attr, kind))
            elif callable(value):
                out.append(_entry(prefix + name, value, "function"))
    walk(host, "orca.host.")
    return out

def search(host, query, limit):
    entries = reference(host)
    words = [w for w in query.lower().replace(".", " ").replace("_", " ").split() if w]
    if not words:
        return len(entries), [{"name": e["name"], "kind": e["kind"], "signature": e["signature"]} for e in entries[:limit]]
    scored = []
    for e in entries:
        name = e["name"].lower().replace("_", " ").replace(".", " ")
        doc = e["doc"].lower()
        hits = sum(1 for w in words if w in name or w in doc)
        if hits == 0:
            continue
        in_name = sum(1 for w in words if w in name)
        scored.append((-hits, -in_name, e["name"], e))
    scored.sort(key=lambda t: t[:3])
    return len(scored), [t[3] for t in scored[:limit]]
)PY";

} // namespace

void register_python_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    ToolDef run = mutate_tool(
        "run_python", L("Run Python code"),
        "Run Python code inside OrcaSlicer against orca.host, the plugin API (python_api searches it): read objects, meshes "
        "(numpy) and settings, compute, and change the project through orca.host.tools.call(name, args), which calls these "
        "same tools with their guards and undo. The code is a module body: `args` holds the args you pass, set `result` to "
        "a JSON value to return it; print() output comes back as output. Returns {status: finished | raised | cancelled | "
        "timed_out, result, output, traceback?, message?}; a script that raised changed what it changed before. Python runs "
        "with OrcaSlicer's full access to the computer, so a person must allow Python scripts in Preferences first. Recorded "
        "by export_script as one step with its code, which a replay runs again against the project as it is then.",
        object_schema({{"code", prop("string", "Python source, run as a module body.")},
                       {"args", {{"type", "object"}, {"description", "Bound to the name `args` in the code."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600},
                                      {"description", "The run is stopped as timed_out after this many seconds (default 300)."}}},
                       {"filename", prop("string", "The name tracebacks show for the code, such as its file's (default <run_python>).")}},
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
            request.budget    = std::chrono::seconds(arg_int(args, "timeout_s", 300));
            request.cancelled = [&ctx] { return ctx.cancelled(); };
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
        "Search orca.host, the plugin API run_python scripts use, in the running build: modules, classes, functions, methods "
        "and properties with their signatures and documentation, best match first. Without query, lists the names.",
        object_schema({{"query", prop("string", "A few words, such as \"mesh normals\" or \"preset value\".")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "At most this many (default 20)."}}}}),
        [](const json& args, ToolContext&) {
            if (!PythonInterpreter::instance().is_initialized())
                throw ToolFailure("Python is not available in this OrcaSlicer");
            PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease();
            if (!lease)
                throw ToolFailure("Python is shutting down");
            py::gil_scoped_acquire gil;
            try {
                py::dict scope;
                py::exec(kReferenceSource, scope);
                py::object host  = py::module_::import("orca").attr("host");
                py::tuple  found = scope["search"](host, arg_string(args, "query"), arg_int(args, "limit", 20));
                json       items = json::array();
                for (const auto item : found[1].cast<py::list>()) {
                    json entry = json::object();
                    for (const auto field : item.cast<py::dict>())
                        entry[field.first.cast<std::string>()] = field.second.cast<std::string>();
                    items.push_back(std::move(entry));
                }
                return ToolResult::ok({{"total", found[0].cast<int>()}, {"items", std::move(items)}});
            } catch (py::error_already_set& e) {
                throw ToolFailure(std::string("Cannot read the Python API: ") + e.what());
            }
        }));
}

}}} // namespace Slic3r::GUI::Mcp
