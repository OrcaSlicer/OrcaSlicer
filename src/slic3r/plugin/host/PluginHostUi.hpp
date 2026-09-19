#pragma once

#include <pybind11/pybind11.h>

#include <functional>
#include <string>
#include <utility>

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
    // Native-only identity captured while a plugin capability is audited. It is
    // intentionally not a Python type and has no caller-supplied constructor.
    // Deferred UI callbacks use it to restore exactly their registration context.
    class DeferredCallbackAuditContext
    {
    public:
        std::string plugin_key() const { return m_plugin_key; }

    private:
        friend class PluginHostUi;

        DeferredCallbackAuditContext(std::string plugin_key, std::string capability_name)
            : m_plugin_key(std::move(plugin_key))
            , m_capability_name(std::move(capability_name))
        {}

        std::string m_plugin_key;
        std::string m_capability_name;
    };

    // Capture only from the host's current audited context. Neither Python
    // callback arguments nor UI payloads contribute to this identity.
    static DeferredCallbackAuditContext capture_deferred_callback_audit_context();

    // Run a native deferred callback with its captured context. The existing
    // ScopedPluginAuditContext restores any outer thread-local context on every
    // return and exception path.
    static void invoke_deferred_callback(
        const DeferredCallbackAuditContext& context,
        const std::function<void()>& callback);

    static void RegisterBindings(pybind11::module_& host);

    // Lifecycle hook: close and tear down every UI window owned by a plugin. PluginManager invokes
    // this after plugin teardown and also for bulk unload during application shutdown.
    static void close_windows_for_plugin(const std::string& plugin_key);
};

} // namespace Slic3r
