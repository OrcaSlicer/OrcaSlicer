#ifndef slic3r_PythonPluginBridge_hpp_
#define slic3r_PythonPluginBridge_hpp_

#include <memory>
#include <string>
#include <vector>

#include "PythonPluginInterface.hpp"

namespace Slic3r {

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
        const std::string& plugin_key, std::string& error);

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
