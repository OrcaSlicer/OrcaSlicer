#pragma once

#include <pybind11/pybind11.h>

#include <string>

// Internal to plugin/host/: the per-domain registrars of the `orca.host`
// surface, one per translation unit, called by PluginHost::RegisterBindings.
namespace Slic3r::host_bindings {

void register_geometry(pybind11::module_& host); // PluginHostGeometry.cpp
void register_mesh(pybind11::module_& host);     // PluginHostMesh.cpp
void register_presets(pybind11::module_& host);  // PluginHostPresets.cpp
void register_model(pybind11::module_& host);    // PluginHostModel.cpp
void register_app(pybind11::module_& host);      // PluginHostApp.cpp
void register_slicing(pybind11::module_& host);  // PluginHostSlicing.cpp
void register_plugin(pybind11::module_& host);  // PluginHost.cpp

// A filament slot colour as the sidebar colour picker stores it: "#RRGGBB" or "#RRGGBBAA",
// returned upper-case with an opaque "FF" alpha dropped. Anything else raises
// std::invalid_argument. Defined in PluginHostPresets.cpp.
std::string normalize_slot_colour(const std::string& value);
} // namespace Slic3r::host_bindings
