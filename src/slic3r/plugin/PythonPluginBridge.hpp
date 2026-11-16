#ifndef slic3r_PythonPluginBridge_hpp_
#define slic3r_PythonPluginBridge_hpp_

#include <memory>
#include <string>
#include <vector>

namespace Slic3r { class PluginCapabilityInterface; }
namespace Slic3r { class PluginCapabilityInterface; }
namespace Slic3r { class PluginCapabilityInterface; }
namespace Slic3r { class PluginCapabilityInterface; }


namespace Slic3r {

// Install the thread-attribution shim: wraps `_thread.start_new_thread` (and the reference
// cached by an already-imported `threading` module) so a thread spawned from inside a plugin
// call runs with that call's plugin key/capability audit scope. The captured identity is held
// in a private C++ wrapper; nothing is exposed to Python that could set it.
//
// Call with the GIL held, after PluginAuditManager::install_hook() and before the interpreter
// releases the GIL. Safe to call more than once per interpreter.
void install_python_thread_audit_shim();

// One materialized capability returned from finalize_plugin_capture: the C++ instance
// and its resolved get_name() (cached while the GIL was held).
struct CapturedCapability
{
    std::shared_ptr<PluginCapabilityInterface> instance;
    std::string name;
};

class PythonPluginBridge
{
public:
    static PythonPluginBridge& instance();

    // Mark the beginning of a plugin registration capture for the provided key (usually file path).
    void begin_plugin_capture(const std::string& plugin_key);

    // Finalize capture: the plugin class was recorded by the @orca.plugin decorator during
    // import; run register_capabilities() (which registers each capability class), then
    // instantiate every registered capability and cache its get_name().
    // Returns one CapturedCapability per capability, or an empty vector on failure
    // (error message populated).
    std::vector<CapturedCapability> finalize_plugin_capture(
        const std::string& capture_key, const std::string& plugin_key, std::string& error);

    // Clear any pending registrations for the key. Safe to call when import fails.
    void cancel_plugin_capture(const std::string& plugin_key);

    // Clear all pending registrations before interpreter shutdown.
    void clear_pending_captures();

private:
    PythonPluginBridge() = default;
    PythonPluginBridge(const PythonPluginBridge&) = delete;
    PythonPluginBridge& operator=(const PythonPluginBridge&) = delete;
};

} // namespace Slic3r

#endif /* slic3r_PythonPluginBridge_hpp_ */
