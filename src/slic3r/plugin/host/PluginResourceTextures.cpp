#include "PluginResourceTextures.hpp"

#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/pluginTypes/resourceTexture/ResourceTexturePluginCapability.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

PluginResourceTextures &PluginResourceTextures::instance()
{
    static PluginResourceTextures registry;
    return registry;
}

std::optional<ResourceTextureEntry> PluginResourceTextures::build_entry(const PluginCapabilityId &id) const
{
    // Query by id and re-check state; the manager copies the shared_ptr and releases its lock.
    auto capability = PluginManager::instance().get_plugin_capability(id, /*only_enabled=*/false);
    if (!capability || !capability->is_enabled() || capability->type() != PluginCapabilityType::ResourceTexture)
        return std::nullopt;

    auto texture_capability = std::dynamic_pointer_cast<ResourceTexturePluginCapability>(capability);
    if (!texture_capability)
        return std::nullopt;

    // An empty directory means resolution failed at load, so the pack contributes nothing.
    const std::string &texture_dir = texture_capability->texture_dir();
    if (texture_dir.empty())
        return std::nullopt;

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor_for_capability(capability->name(),
                                                                            PluginCapabilityType::ResourceTexture,
                                                                            descriptor))
        return std::nullopt;

    ResourceTextureEntry entry;
    entry.id           = capability->identity();
    entry.plugin_key   = entry.id.plugin_key;
    entry.display_name = capability->name();
    entry.texture_dir  = texture_dir;
    entry.plugin_root  = descriptor.plugin_root;
    return entry;
}

void PluginResourceTextures::on_plugin_register(const std::string &plugin_key)
{
    for (const auto &capability : PluginManager::instance().get_plugin_capabilities(plugin_key, PluginCapabilityType::ResourceTexture)) {
        if (capability)
            on_cap_register(capability->identity());
    }
}

void PluginResourceTextures::on_plugin_deregister(const std::string &plugin_key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->first.plugin_key == plugin_key)
            it = m_entries.erase(it);
        else
            ++it;
    }
}

void PluginResourceTextures::on_cap_register(const PluginCapabilityId &id)
{
    std::optional<ResourceTextureEntry> entry = build_entry(id);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (entry)
        m_entries[entry->id] = std::move(*entry);
}

void PluginResourceTextures::on_cap_deregister(const PluginCapabilityId &id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(id);
}

void PluginResourceTextures::initial_sweep()
{
    std::map<PluginCapabilityId, ResourceTextureEntry> entries;
    for (const auto &capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::ResourceTexture)) {
        if (!capability)
            continue;
        std::optional<ResourceTextureEntry> entry = build_entry(capability->identity());
        if (entry)
            entries[entry->id] = std::move(*entry);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries = std::move(entries);
}

std::vector<ResourceTextureEntry> PluginResourceTextures::entries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<ResourceTextureEntry> result;
    result.reserve(m_entries.size());
    for (const auto &entry : m_entries)
        result.push_back(entry.second);
    return result;
}

} // namespace Slic3r
