#pragma once

#include "slic3r/GUI/CAD/CadShapeSelection.hpp"

#include <optional>
#include <string>

namespace pybind11 { class module_; }

namespace Slic3r {

void register_cad_geometry(pybind11::module_& cad);

class PluginHostCad
{
public:
    // Construct the selection caches before PluginManager so they outlive its plugin entries.
    static void initialize();
    static void RegisterBindings(pybind11::module_& host);
    // Native GUI producer; called only on the main thread, including deselection and teardown.
    static void selection_changed(std::optional<GUI::CadShapeSelection> selection);
    // Synchronous and thread-safe: discard queued deliveries before unloading Python code.
    static void close_for_plugin(const std::string& plugin_key);
};

} // namespace Slic3r
