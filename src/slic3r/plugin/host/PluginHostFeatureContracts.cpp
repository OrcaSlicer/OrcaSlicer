#include "PluginHostFeatureContracts.hpp"
#include "PluginHostBindings.hpp"

#include <pybind11/stl.h>

#include <algorithm>
#include <mutex>

namespace py = pybind11;

namespace Slic3r {

namespace {

std::mutex& feature_contracts_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<PluginHostFeatureContract>& advertised_feature_contracts()
{
    static std::vector<PluginHostFeatureContract> contracts;
    return contracts;
}

} // namespace

void advertise_plugin_host_feature_contract(
    std::string feature_id, uint32_t major_version, uint32_t minor_version)
{
    std::lock_guard<std::mutex> lock(feature_contracts_mutex());
    auto& contracts = advertised_feature_contracts();
    const auto existing = std::find_if(
        contracts.begin(), contracts.end(), [&feature_id](const PluginHostFeatureContract& contract) {
            return contract.feature_id == feature_id;
        });
    if (existing == contracts.end()) {
        contracts.push_back({ std::move(feature_id), major_version, minor_version });
        std::sort(contracts.begin(), contracts.end(), [](const PluginHostFeatureContract& left,
                                                         const PluginHostFeatureContract& right) {
            return left.feature_id < right.feature_id;
        });
    }
}

std::vector<PluginHostFeatureContract> plugin_host_feature_contracts()
{
    std::lock_guard<std::mutex> lock(feature_contracts_mutex());
    return advertised_feature_contracts();
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
