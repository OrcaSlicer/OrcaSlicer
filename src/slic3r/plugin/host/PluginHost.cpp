#include "PluginHost.hpp"
#include "PluginHostBindings.hpp"
#include "PluginHostUi.hpp"
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginManager.hpp>

#include <stdexcept>

namespace Slic3r {

namespace host_bindings {
void register_plugin(pybind11::module_& host)
{
    auto plugin_host = host.def_submodule("plugin", "Plugin host API");

    plugin_host.def(
        "storage",
        []() -> std::string {
            const std::string plugin_key = PluginAuditManager::instance().current_plugin();
            if (plugin_key.empty())
                throw std::runtime_error("plugin.storage() must be called from a plugin callback");

            return PluginManager::instance().get_storage_dir(plugin_key);
        },
        "Return the installed folder of the current plugin.");
}
} // namespace host_bindings

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
    host_bindings::register_plugin(host);

    // UI: native dialogs and interactive HTML windows for plugins.
    PluginHostUi::RegisterBindings(host);

    // Slicing print-graph data model (Print, Layer, Surface, ...).
    host_bindings::register_slicing(host);
}

} // namespace Slic3r
