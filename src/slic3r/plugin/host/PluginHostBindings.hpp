#pragma once

#include <pybind11/pybind11.h>

// Internal to plugin/host/: the per-domain registrars of the `orca.host`
// surface, one per translation unit, called by PluginHost::RegisterBindings.
namespace Slic3r::host_bindings {

void register_geometry(pybind11::module_& host); // PluginHostGeometry.cpp
void register_mesh(pybind11::module_& host);     // PluginHostMesh.cpp
void register_presets(pybind11::module_& host);  // PluginHostPresets.cpp
void register_model(pybind11::module_& host);    // PluginHostModel.cpp
void register_app(pybind11::module_& host);      // PluginHostApp.cpp
void register_slicing(pybind11::module_& host);  // PluginHostSlicing.cpp
void register_isolated_slicing(pybind11::module_& host); // PluginHostIsolatedSlicing.cpp
void register_persistent_mutation(pybind11::module_& host); // PluginHostPersistentMutation.cpp
void register_feature_contracts(pybind11::module_& host); // PluginHostFeatureContracts.cpp
#if defined(SLIC3R_ENABLE_PLUGIN_HOST_TEST_API)
void register_test_support(pybind11::module_& host); // PluginHostTestSupport.cpp
#endif
void register_plugin(pybind11::module_& host);  // PluginHost.cpp
} // namespace Slic3r::host_bindings
