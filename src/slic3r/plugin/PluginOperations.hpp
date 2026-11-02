#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pybind11 { class object; }

namespace Slic3r {

// An operation a plugin offers to agents and scripts (D4): declared with orca.register_operation() while
// the plugin loads, offered only once the person has granted it, and gone when the plugin unloads. Agents
// reach it through api_describe and api_call, scripts through orca.host.tools.call, as
// "plugin.<plugin key>.<name>"; it is never in tools/list.
struct PluginOperation
{
    std::string    plugin_key;
    std::string    name;        // as the plugin declared it
    std::string    description;
    nlohmann::json schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    // Whether it changes the project or files: a changing operation runs under the mutate guard and is
    // journaled; one that only reads is neither.
    bool           changes = true;
    // The plugin's Python callable, taking the args dict and returning a JSON value. Released under the GIL.
    std::shared_ptr<pybind11::object> handler;

    std::string full_name() const { return "plugin." + plugin_key + "." + name; }
};

class PluginOperations
{
public:
    static PluginOperations& instance();

    // Keeps the operations a loading plugin declared and the person granted, replacing any it kept from
    // an earlier attempt; activate() offers them once the plugin has loaded.
    void stage(const std::string& plugin_key, std::vector<PluginOperation> operations);
    void activate(const std::string& plugin_key);
    // Drops what a load that failed or was cancelled staged; operations already offered stay.
    void discard_staged(const std::string& plugin_key);
    // Offers `op`, replacing the plugin's operation of the same name.
    void add(PluginOperation op);
    // Withdraws every operation of the plugin `plugin_key`, offered or staged (it unloaded).
    void remove_plugin(const std::string& plugin_key);
    // The operation called `full_name` ("plugin.<key>.<name>"), if offered.
    std::optional<PluginOperation> find(const std::string& full_name) const;
    std::vector<PluginOperation>   all() const;

    // Runs `op` with `args` on the calling thread, under the plugin's audit identity and grants, and returns
    // its JSON result. Throws std::runtime_error when `op` is no longer offered (its plugin unloaded since
    // find() returned it, or loaded again with new code), with the formatted traceback when it raises or
    // returns something that is not JSON, or when Python is unavailable. One already running when its plugin
    // unloads runs to its end; its own tool calls are refused from the unload on.
    nlohmann::json invoke(const PluginOperation& op, const nlohmann::json& args) const;

private:
    PluginOperations() = default;

    mutable std::mutex           m_mutex;
    std::vector<PluginOperation> m_operations;
    std::vector<PluginOperation> m_staged;
};

} // namespace Slic3r
