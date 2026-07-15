#pragma once

#include <pybind11/pybind11.h>

#include <string>

namespace Slic3r {

// Binds the `orca.host.ui` submodule: native message boxes, progress dialogs,
// and interactive HTML windows for plugins. All calls run on the main/UI thread
// (marshaled from the plugin worker thread) and the host owns every window.
class PluginHostUi
{
public:
    static void RegisterBindings(pybind11::module_& host);

    // Scope used by the slicing dispatcher to reject UI calls from pipeline hooks. Blocking on the
    // UI event loop from a slicing worker can deadlock when the UI is waiting for that worker.
    class PipelineHookScope
    {
    public:
        PipelineHookScope();
        ~PipelineHookScope();
        PipelineHookScope(const PipelineHookScope&)            = delete;
        PipelineHookScope& operator=(const PipelineHookScope&) = delete;
    };

    static bool is_pipeline_hook_context();

    // Lifecycle hook: close and tear down every UI window owned by a plugin. PluginManager invokes
    // this after plugin teardown and also for bulk unload during application shutdown.
    static void close_windows_for_plugin(const std::string& plugin_key);
};

} // namespace Slic3r
