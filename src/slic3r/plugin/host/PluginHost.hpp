#pragma once

#include <pybind11/pybind11.h>

namespace Slic3r {

// Entry point of the `orca.host` Python API surface. Each domain of the
// surface (geometry, mesh, presets, model, app access, ui, slicing graph)
// lives in its own translation unit in this directory; RegisterBindings
// creates the submodule and runs the per-domain registrars.
class PluginHost
{
public:
    static void RegisterBindings(pybind11::module_& module);
};

} // namespace Slic3r
