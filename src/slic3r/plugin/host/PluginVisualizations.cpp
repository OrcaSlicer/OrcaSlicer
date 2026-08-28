#include "PluginVisualizations.hpp"

#include "../PluginManager.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace {

ExecutionResult recoverable_error(const PluginCapabilityId& id, const char* operation, const std::exception& error)
{
    BOOST_LOG_TRIVIAL(error) << "Visualization capability " << id.plugin_key << "/" << id.name << " failed to " << operation << ": "
                             << error.what();
    return ExecutionResult::failure(PluginResult::RecoverableError, error.what());
}

} // namespace

PluginVisualizations& PluginVisualizations::instance()
{
    static PluginVisualizations visualizations;
    return visualizations;
}

void PluginVisualizations::install_callbacks(PluginManager& manager)
{
    manager.subscribe_on_capability_load_callback([this](const PluginCapabilityId& id) {
        if (id.type == PluginCapabilityType::Visualization)
            notify_change();
    });
    manager.subscribe_on_capability_unload_callback([this](const PluginCapabilityId& id) {
        if (id.type == PluginCapabilityType::Visualization) {
            close(id);
            notify_change();
        }
    });
    manager.subscribe_on_unload_callback([this](const std::string& plugin_key) {
        close_plugin(plugin_key);
        notify_change();
    });
}

std::vector<std::shared_ptr<VisualizationPluginCapability>> PluginVisualizations::capabilities() const
{
    std::vector<std::shared_ptr<VisualizationPluginCapability>> result;
    for (const auto& capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Visualization))
        if (auto visualization = std::dynamic_pointer_cast<VisualizationPluginCapability>(capability))
            result.push_back(std::move(visualization));
    return result;
}

ExecutionResult PluginVisualizations::open(const std::shared_ptr<VisualizationPluginCapability>& capability, VisualizationContext& ctx)
{
    if (!capability || !capability->is_enabled())
        return ExecutionResult::skipped("Visualization capability is unavailable");

    const PluginCapabilityId id = capability->identity();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.count(id) != 0)
            return ExecutionResult::skipped("Visualization session is already open");
    }

    ExecutionResult result;
    try {
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        result = capability->open(ctx);
    } catch (const std::exception& error) {
        return recoverable_error(id, "open", error);
    }

    if (result.status == PluginResult::Success) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.emplace(id, Session{capability, ctx.scene_id});
    }
    return result;
}

ExecutionResult PluginVisualizations::update(const PluginCapabilityId& id, VisualizationContext& ctx)
{
    std::shared_ptr<VisualizationPluginCapability> capability;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_sessions.find(id);
        if (it == m_sessions.end())
            return ExecutionResult::skipped("Visualization session is not open");
        if (it->second.scene_id == ctx.scene_id)
            return ExecutionResult::skipped("Visualization scene is unchanged");
        capability = it->second.capability;
    }

    ExecutionResult result;
    try {
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        result = capability->update(ctx);
    } catch (const std::exception& error) {
        return recoverable_error(id, "update", error);
    }

    if (result.status == PluginResult::Success) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_sessions.find(id);
        if (it != m_sessions.end() && it->second.capability == capability)
            it->second.scene_id = ctx.scene_id;
    }
    return result;
}

void PluginVisualizations::close(const PluginCapabilityId& id)
{
    std::shared_ptr<VisualizationPluginCapability> capability;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_sessions.find(id);
        if (it == m_sessions.end())
            return;
        capability = std::move(it->second.capability);
        m_sessions.erase(it);
    }

    try {
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        capability->close();
    } catch (const std::exception& error) {
        recoverable_error(id, "close", error);
    }
}

void PluginVisualizations::close_plugin(const std::string& plugin_key)
{
    std::vector<PluginCapabilityId> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_sessions)
            if (entry.first.plugin_key == plugin_key)
                sessions.push_back(entry.first);
    }
    for (const PluginCapabilityId& id : sessions)
        close(id);
}

void PluginVisualizations::close_all()
{
    std::vector<PluginCapabilityId> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_sessions)
            sessions.push_back(entry.first);
    }
    for (const PluginCapabilityId& id : sessions)
        close(id);
}

bool PluginVisualizations::is_active(const PluginCapabilityId& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.count(id) != 0;
}

PluginVisualizations::ChangeToken PluginVisualizations::subscribe(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const ChangeToken           token = m_next_token++;
    m_callbacks.emplace(token, std::move(callback));
    return token;
}

void PluginVisualizations::unsubscribe(ChangeToken token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.erase(token);
}

void PluginVisualizations::notify_change()
{
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_callbacks)
            callbacks.push_back(entry.second);
    }
    for (const auto& callback : callbacks)
        callback();
}

} // namespace Slic3r
