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

} // namespace Slic3r::host_bindings
