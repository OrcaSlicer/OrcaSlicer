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
               std::shared_ptr<ResourceTexturePluginCapability>>(
        resource_texture, "ResourceTexturePluginCapabilityBase",
        "Base for a resource texture capability: a pack of height-map images offered in the texture picker of\n"
        "the texture displacement tool, listed under the capability's name.")
        .def(py::init<>())
        .def("get_type", &ResourceTexturePluginCapability::get_type, "Return PluginType.ResourceTexture.")
        .def("get_texture_dir", &ResourceTexturePluginCapability::get_texture_dir,
             "Required. Return the path of the folder holding the pack's images. Called once when the plugin\n"
             "loads. The folder must be inside the plugin's own folder; its .png, .jpg, .jpeg and .bmp files are\n"
             "used, without subfolders and at most 64 of them. An empty string, a folder that fails these\n"
             "checks or a raised exception leaves the pack without textures.");
}

} // namespace Slic3r
