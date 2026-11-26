#pragma once

#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <functional>
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

    static void run_on_ui_thread(std::function<void()> fn);

    // Lifecycle hook: close and tear down every UI window owned by a plugin. PluginManager invokes
    // this after plugin teardown and also for bulk unload during application shutdown.
    static void close_windows_for_plugin(const std::string& plugin_key);

    // From any thread: on the UI thread, ends `owner`'s message boxes and modal windows, which a stopped session's code
    // may wait in, and leaves its modeless windows, dock panels and callbacks open.
    static void end_run_modals(const std::string& owner);

    // From a script run's thread, with the GIL held: tears down what the run owns (`owner`, its ui_owner()) on the UI
    // thread and returns once that is done, so none of its callbacks runs after. Does nothing while the app closes.
    static void close_run_windows(const std::string& owner);

    // The handler create_window() and create_dock_panel() give a page for its on_message and on_submit: calls `fn`
    // with the page's data under the GIL. Made in a script's run, it runs inside that run, which waits for it, and does
    // nothing once the run's UI has closed.
    static std::function<void(const nlohmann::json&)> message_callback(pybind11::object fn);
    // The same for a callable that takes no arguments (on_close, a notification's link): true unless it returned a
    // false value.
    static std::function<bool()> event_callback(pybind11::object fn);
};

} // namespace Slic3r
