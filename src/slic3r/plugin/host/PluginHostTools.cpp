#include "PluginHostTools.hpp"

#include "PluginHostBindings.hpp"

#include <slic3r/GUI/CommandCatalog.hpp>
#include <slic3r/GUI/NativeCommands.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginJsonPy.hpp>
#include <slic3r/plugin/PythonRunScope.hpp>

#include <nlohmann/json.hpp>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <wx/app.h>
#include <wx/thread.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace py = pybind11;
using nlohmann::json;

namespace {

thread_local ToolBridge* tl_bridge      = nullptr;
thread_local bool        tl_tool_caller = false;

std::mutex       g_caller_mutex;
PluginToolCaller g_plugin_caller;

PluginToolCaller plugin_tool_caller()
{
    std::lock_guard<std::mutex> lock(g_caller_mutex);
    return g_plugin_caller;
}

// The tool error class, defined in Python so it carries `details`.
constexpr const char* kToolErrorSource = R"PY(
class ToolError(Exception):
    """A tool call that ended in an error result. str() is its message; details holds the result's data."""
    def __init__(self, message, details=None):
        super().__init__(message)
        self.details = details
)PY";

// Raises orca.host.tools.ToolError with `message` and `details`.
[[noreturn]] void raise_tool_error(py::module_& tools, const std::string& message, const json& details)
{
    py::object error = tools.attr("ToolError")(message, json_to_py(details));
    PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(error.ptr())), error.ptr());
    throw py::error_already_set();
}

// What orca.host.tools.list() gives: the process catalog's commands in the order they were added, then those its
// sources hold now.
json list_commands()
{
    const GUI::CommandCatalog& catalog = GUI::NativeCommands::instance();
    json                       out     = json::array();
    const auto add = [&out](const GUI::NativeCommand& c) {
        out.push_back({{"name", c.key},
                       {"title", c.title},
                       {"group", c.group},
                       {"description", c.description},
                       {"read_only", c.read_only},
                       {"listed", c.listed}});
    };
    for (const GUI::NativeCommand& c : catalog.commands())
        add(c);
    for (const std::shared_ptr<const GUI::NativeCommand>& c : catalog.source_commands())
        add(*c);
    return out;
}

json call_tool(py::module_& tools, const std::string& name, const py::object& args)
{
    const json  arguments = args.is_none() ? json::object() : py_to_json(args);
    if (!arguments.is_object())
        throw py::type_error("orca.host.tools.call takes its arguments as a dict");
    ToolBridge*             bridge   = current_tool_bridge();
    const std::string       identity = PluginAuditManager::instance().current_plugin();
    const PythonRunBinding& binding  = current_python_run();
    try {
        // A tool call waits for the UI thread, so a window callback, which runs there, can't make one. wx reports
        // every thread as its main one until it has started, hence the app check.
        if (binding.run && (binding.kind == PythonRunThread::Callback || (wxTheApp != nullptr && wxIsMainThread())))
            throw std::runtime_error("A script's window callbacks run on the UI thread and call no tools; hand the message to the "
                                     "script's own code, which can");
        // Any thread of a run calls through the run, while the run lasts.
        if (binding.run && (binding.run->has_tools() || binding.run->tools_closed())) {
            std::shared_ptr<PythonRunScope> run = binding.run;
            py::gil_scoped_release          release;
            return run->call_tool(name, arguments);
        }
        if (bridge != nullptr) {
            py::gil_scoped_release release;
            return bridge->call(name, arguments);
        }
        if (is_script_identity(identity))
            throw std::runtime_error("A script calls tools only while its run lasts");
        if (identity.empty())
            throw std::runtime_error("orca.host.tools needs a plugin or a script: call it from a Script capability or a script's own code");
        // A plugin's call waits for the UI thread, so it comes only from a thread nothing there waits for.
        if (!tool_caller_thread())
            throw std::runtime_error("A plugin calls orca.host.tools from a Script worker or a thread it starts, never from the UI "
                                     "thread, a slicing hook or another plugin callback");
        const PluginToolCaller caller = plugin_tool_caller();
        if (!caller)
            throw std::runtime_error("Tools are not available yet: OrcaSlicer is still starting");
        py::gil_scoped_release release;
        return caller(identity, name, arguments);
    } catch (const ToolCallError& e) {
        raise_tool_error(tools, e.what(), e.details);
    }
}

} // namespace

ToolBridge* current_tool_bridge() { return tl_bridge; }

bool tool_caller_thread() { return tl_tool_caller; }

ScopedToolCallerThread::ScopedToolCallerThread() : m_previous(tl_tool_caller) { tl_tool_caller = true; }

ScopedToolCallerThread::~ScopedToolCallerThread() { tl_tool_caller = m_previous; }

ScopedToolBridge::ScopedToolBridge(ToolBridge& bridge) : m_previous(tl_bridge) { tl_bridge = &bridge; }

ScopedToolBridge::~ScopedToolBridge() { tl_bridge = m_previous; }

void set_plugin_tool_caller(PluginToolCaller caller)
{
    std::lock_guard<std::mutex> lock(g_caller_mutex);
    g_plugin_caller = std::move(caller);
}

namespace host_bindings {

void register_tools(py::module_& host)
{
    py::module_ tools = host.def_submodule("tools", "Calls the MCP tools from Python: the same tools, arguments and results "
                                                    "an agent gets, with each tool's guards, admission and undo; list() and "
                                                    "describe() say what there is to call.");
    py::exec(kToolErrorSource, tools.attr("__dict__"));

    tools.def(
        "call",
        [tools](const std::string& name, const py::object& args) mutable { return json_to_py(call_tool(tools, name, args)); },
        py::arg("name"), py::arg("args") = py::none(),
        "Call the MCP tool `name` with `args` (a dict) and return its result as a dict. A call that outlives its "
        "reply budget returns {status: \"running\" | \"blocked_by_dialog\", job}, as it does for an agent; follow it "
        "with call(\"wait\", {\"for\": \"job\", \"job\": ...}). An error result raises ToolError, whose details "
        "hold the result's data. A person's or an agent's script calls tools from its own code and from the threads and "
        "pool tasks it starts, while its run lasts, but not from its window callbacks; a plugin calls from a "
        "Script worker, never the UI thread.");
    tools.def(
        "run_replay",
        [tools](const py::object& replay, const py::object& vars) mutable {
            py::dict args;
            args["replay"] = replay;
            if (!vars.is_none())
                args["vars"] = vars;
            return json_to_py(call_tool(tools, "run_replay", args));
        },
        py::arg("replay"), py::arg("vars") = py::none(),
        "Run a replay ({orca_replay: 1, steps: [...]}) as the run_replay tool does, and return its result. "
        "`replay` is the replay as a dict or as its JSON text, which may hold comments as a replay file does; "
        "`vars` override the replay's vars. For a replay file, call(\"run_replay\", {\"path\": ...}).");
    // Both only read the catalog, which is sealed and read on any thread, so neither needs a run or a plugin. They read
    // it without the GIL, as call() waits without it; no thread holds the plugin operations' lock while it waits for the GIL.
    tools.def(
        "list",
        [] {
            json commands;
            {
                py::gil_scoped_release release;
                commands = list_commands();
            }
            return json_to_py(commands);
        },
        "Every command call() reaches, as a list of {name, title, group, description, read_only, listed}: the tools, the "
        "Speed Dial's commands and the Design tab's (with the CAD feature on), in the order the app adds them, then the "
        "operations the loaded plugins offer (plugin.<plugin>.<name>). title and description are in English; listed says "
        "whether an agent's tool list shows it, read_only that it changes nothing. Any thread.");
    tools.def(
        "describe",
        [tools](const std::string& name) mutable {
            json        entry;
            std::string unknown; // the error for a name the catalog does not hold
            json        close;
            {
                py::gil_scoped_release     release;
                const GUI::CommandCatalog& catalog = GUI::NativeCommands::instance();
                if (const std::shared_ptr<const GUI::NativeCommand> command = catalog.lookup(name)) {
                    entry = GUI::command_reference(*command);
                } else {
                    unknown = catalog.unknown_command(name);
                    close   = catalog.similar(name);
                }
            }
            if (!unknown.empty())
                raise_tool_error(tools, unknown, {{"did_you_mean", close}});
            return json_to_py(entry);
        },
        py::arg("name"),
        "The command `name` as a dict: key, title, group, description, input_schema (the JSON Schema of its "
        "arguments), examples when it has some ([{title, calls: [{tool, args}]}]), listed, in_palette (a person can "
        "pick it in the Speed Dial), the access labels read_only, destructive, idempotent and open_world, guard "
        "(inspect, recover or mutate: what it may do behind a dialog), completion and operations. An unknown name "
        "raises ToolError, whose details hold did_you_mean, the closest names. Any thread.");
}

} // namespace host_bindings

} // namespace Slic3r
