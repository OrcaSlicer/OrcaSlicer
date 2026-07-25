#include "ScriptPluginCapability.hpp"

#include "ScriptPluginCapabilityTrampoline.hpp"

#include <boost/log/trivial.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace Slic3r {
void ScriptPluginCapability::RegisterBindings(pybind11::module_& module, pybind11::enum_<PluginCapabilityType>& pluginTypes)
{
    (void) pluginTypes;
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.script bindings";

    auto script = module.def_submodule("script", "Script Plugins API");

    py::class_<ScriptPluginCapability, PluginCapabilityInterface, PyScriptPluginCapabilityTrampoline, std::shared_ptr<ScriptPluginCapability>>(script, "ScriptPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &ScriptPluginCapability::get_type)
        .def("execute", &ScriptPluginCapability::execute);
}
} // namespace Slic3r
