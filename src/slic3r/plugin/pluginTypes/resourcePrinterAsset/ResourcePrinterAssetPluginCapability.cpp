#include "ResourcePrinterAssetPluginCapability.hpp"
#include "ResourcePrinterAssetPluginCapabilityTrampoline.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <boost/log/trivial.hpp>

#include <memory>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>

namespace py = pybind11;

namespace Slic3r {

void ResourcePrinterAssetPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.resource_printer_asset bindings";

    auto printer_asset = module.def_submodule("resource_printer_asset", "Plugin resource printer asset API");

    py::class_<ResourcePrinterAssetPluginCapability, PluginCapabilityInterface, PyResourcePrinterAssetPluginCapabilityTrampoline,
               std::shared_ptr<ResourcePrinterAssetPluginCapability>>(printer_asset, "ResourcePrinterAssetPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &ResourcePrinterAssetPluginCapability::get_type)
        .def("get_asset_dir", &ResourcePrinterAssetPluginCapability::get_asset_dir);
}

} // namespace Slic3r
