#pragma once

#include "slic3r/GUI/CAD/CadFaceSelection.hpp"

#include <optional>
#include <string>

namespace pybind11 { class module_; }

namespace Slic3r {

class PluginHostCad
{
public:
    static void RegisterBindings(pybind11::module_& host);
    // Native GUI producer; called only on the main thread, including deselection and teardown.
    static void selection_changed(std::optional<GUI::CadFaceSelection> face);
    // Synchronous and thread-safe: discard queued deliveries before unloading Python code.
    static void close_for_plugin(const std::string& plugin_key);
};

} // namespace Slic3r
