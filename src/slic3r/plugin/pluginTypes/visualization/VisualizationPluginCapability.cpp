#include "VisualizationPluginCapability.hpp"
#include "VisualizationPluginCapabilityTrampoline.hpp"

#include <boost/log/trivial.hpp>
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Slic3r {

void VisualizationPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.visualization bindings";

    auto visualization = module.def_submodule("visualization", "Visualization plugin API");

    py::class_<VisualizationContext, PluginContext>(visualization, "VisualizationContext")
        .def(py::init<>())
        .def_readwrite("orca_version", &VisualizationContext::orca_version)
        .def_readwrite("scene_id", &VisualizationContext::scene_id)
        .def_readwrite("plate_index", &VisualizationContext::plate_index)
        .def_readwrite("geometry_path", &VisualizationContext::geometry_path)
        .def_readwrite("metadata_json", &VisualizationContext::metadata_json);

    py::class_<VisualizationPluginCapability, PluginCapabilityInterface, PyVisualizationPluginCapabilityTrampoline,
               std::shared_ptr<VisualizationPluginCapability>>(visualization, "VisualizationPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &VisualizationPluginCapability::get_type)
        .def("open", &VisualizationPluginCapability::open)
        .def("update", &VisualizationPluginCapability::update)
        .def("close", &VisualizationPluginCapability::close);
}

} // namespace Slic3r
