#pragma once

#include "../pluginTypes/visualization/VisualizationPluginCapability.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Slic3r {

class PluginManager;

class PluginVisualizations
{
public:
    using ChangeToken = size_t;

    static PluginVisualizations& instance();

    void install_callbacks(PluginManager& manager);

    std::vector<std::shared_ptr<VisualizationPluginCapability>> capabilities() const;

    ExecutionResult open(const std::shared_ptr<VisualizationPluginCapability>& capability, VisualizationContext& ctx);
    ExecutionResult update(const PluginCapabilityId& id, VisualizationContext& ctx);
    void            close(const PluginCapabilityId& id);
    void            close_plugin(const std::string& plugin_key);
    void            close_all();
    bool            is_active(const PluginCapabilityId& id) const;

    ChangeToken subscribe(std::function<void()> callback);
    void        unsubscribe(ChangeToken token);

private:
    struct Session
    {
        std::shared_ptr<VisualizationPluginCapability> capability;
        uint64_t                                       scene_id{0};
    };

    void notify_change();

    mutable std::mutex                         m_mutex;
    std::map<PluginCapabilityId, Session>      m_sessions;
    std::map<ChangeToken, std::function<void()>> m_callbacks;
    ChangeToken                                m_next_token{1};
};

} // namespace Slic3r
