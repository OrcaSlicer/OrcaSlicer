#include "PluginHostFeatureContracts.hpp"
#include "PluginHostBindings.hpp"

#include <pybind11/stl.h>

namespace py = pybind11;

namespace Slic3r {

std::vector<PluginHostFeatureContract> plugin_host_feature_contracts()
{
    return {
        { "isolated_fff_simulation", 1, 0 },
        { "process_preset_transaction", 1, 0 },
        { "printer_preset_transaction", 1, 0 },
        { "filament_slot_transaction", 1, 0 },
    };
}

} // namespace Slic3r

namespace Slic3r::host_bindings {

void register_feature_contracts(py::module_& host)
{
    py::class_<PluginHostFeatureContract>(host, "FeatureContract")
        .def_readonly("feature_id", &PluginHostFeatureContract::feature_id)
        .def_readonly("major_version", &PluginHostFeatureContract::major_version)
        .def_readonly("minor_version", &PluginHostFeatureContract::minor_version);

    host.def("feature_contracts", &plugin_host_feature_contracts,
             "Return copied semantic contracts for host features compiled into this build.");
}

} // namespace Slic3r::host_bindings
