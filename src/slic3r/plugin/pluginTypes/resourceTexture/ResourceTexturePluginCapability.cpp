#include "ResourceTexturePluginCapability.hpp"
#include "ResourceTexturePluginCapabilityTrampoline.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <boost/log/trivial.hpp>

#include <memory>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>

namespace py = pybind11;

namespace Slic3r {

void ResourceTexturePluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.resource_texture bindings";

    auto resource_texture = module.def_submodule("resource_texture", "Plugin resource texture API");

    py::class_<ResourceTexturePluginCapability, PluginCapabilityInterface,
               PyResourceTexturePluginCapabilityTrampoline,
               std::shared_ptr<ResourceTexturePluginCapability>>(resource_texture,
                                                                 "ResourceTexturePluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &ResourceTexturePluginCapability::get_type)
        .def("get_texture_dir", &ResourceTexturePluginCapability::get_texture_dir);
}

} // namespace Slic3r
