#include "PluginHost.hpp"
#include "PluginHostBindings.hpp"
#include "PluginHostUi.hpp"

namespace Slic3r {

void PluginHost::RegisterBindings(pybind11::module_& module)
{
    auto host = module.def_submodule("host", "Host application API");

    // Value types first so the docstring signatures of later registrars
    // resolve to the bound Python names.
    host_bindings::register_geometry(host);
    host_bindings::register_mesh(host);
    host_bindings::register_presets(host);
    host_bindings::register_model(host);
    host_bindings::register_app(host);

    // UI: native dialogs and interactive HTML windows for plugins.
    PluginHostUi::RegisterBindings(host);

    // Slicing print-graph data model (Print, Layer, Surface, ...).
    host_bindings::register_slicing(host);
}

} // namespace Slic3r
