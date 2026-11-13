#include "ResourcePackPluginCapability.hpp"
#include "ResourcePackPluginCapabilityTrampoline.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <boost/log/trivial.hpp>

#include <memory>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>

namespace py = pybind11;

namespace Slic3r {

void ResourcePackPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.resource_pack bindings";

    auto resource_pack = module.def_submodule("resource_pack", "Plugin resource pack API");

    py::class_<ResourcePackPluginCapability, PluginCapabilityInterface, PyResourcePackPluginCapabilityTrampoline,
               std::shared_ptr<ResourcePackPluginCapability>>(resource_pack, "ResourcePackPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &ResourcePackPluginCapability::get_type)
        .def("get_resource_dir", &ResourcePackPluginCapability::get_resource_dir)
        .def("get_resource_kinds", &ResourcePackPluginCapability::get_resource_kinds);
}

} // namespace Slic3r
