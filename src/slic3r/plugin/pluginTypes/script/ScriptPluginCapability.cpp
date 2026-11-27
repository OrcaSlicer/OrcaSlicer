#include "ScriptPluginCapability.hpp"

#include "ScriptPluginCapabilityTrampoline.hpp"

#include <boost/log/trivial.hpp>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "slic3r/plugin/PythonPluginInterface.hpp"

namespace py = pybind11;

namespace Slic3r {
void ScriptPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.script bindings";

    auto script = module.def_submodule("script", "Script Plugins API");

    py::class_<ScriptPluginCapability, PluginCapabilityInterface, PyScriptPluginCapabilityTrampoline,
               std::shared_ptr<ScriptPluginCapability>>(
        script, "ScriptPluginCapabilityBase",
        "Base for a script capability: an action the user runs from the Plugins dialog or the speed dial.")
        .def(py::init<>())
        .def("get_type", &ScriptPluginCapability::get_type, "Return PluginType.Script.")
        .def("execute", &ScriptPluginCapability::execute,
             "Required. Called with no arguments when the user runs the script, on a worker thread rather than\n"
             "the UI thread; it may be an async def. Return an ExecutionResult: Success and Skipped show its\n"
             "message, or a default text, to the user, and the Plugins dialog reports RecoverableError and\n"
             "FatalError as errors with the message. A raised exception is reported as an error.");
}
} // namespace Slic3r
