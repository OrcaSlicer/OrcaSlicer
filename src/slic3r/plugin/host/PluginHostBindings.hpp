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
void register_plugin(pybind11::module_& host);  // PluginHost.cpp
void register_tools(pybind11::module_& host);   // PluginHostTools.cpp

// Refuses a person's or an agent's script (a script audit identity) with a RuntimeError naming
// orca.host.<function>. The functions that change state outside the MCP tools (the staged preset update,
// bundle reload, the 3MF copy, plugin UI, plugin storage) call it first: a script reads through orca.host
// and changes the project only through orca.host.tools, with each tool's guards and undo. The
// "orca.host functions are classified" test lists which functions are which.
void require_plugin_caller(const char* function);
} // namespace Slic3r::host_bindings
