#pragma once

#include <pybind11/pybind11.h>

#include <string>

namespace Slic3r {

// Binds the `orca.host.ui` submodule: native message boxes, progress dialogs,
// and interactive HTML windows/dialogs for plugins. All calls run on the main/UI
// thread (marshaled from the plugin worker thread) and the host owns every window.
//
// Not safe to call from a slicing pipeline hook (SlicingPipelinePluginCapability):
// that hook runs on the slicing worker thread, which the UI thread can itself be
// blocked waiting on, so marshaling a UI call from there can deadlock. Plugin
// authors must not call orca.host.ui.* from pipeline hooks.
class PluginHostUi
{
public:
    static void RegisterBindings(pybind11::module_& host);

    // Lifecycle hook: close and tear down every UI window owned by a plugin. PluginManager invokes
    // this after plugin teardown and also for bulk unload during application shutdown.
    static void close_windows_for_plugin(const std::string& plugin_key);
};

} // namespace Slic3r
