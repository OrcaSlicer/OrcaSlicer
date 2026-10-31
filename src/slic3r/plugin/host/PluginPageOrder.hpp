#pragma once

#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

inline std::optional<std::vector<PluginCapabilityId>> parse_plugin_page_order(const std::string& raw)
{
    std::vector<PluginCapabilityId> order;
    if (raw.empty())
        return order;

    const nlohmann::json entries = nlohmann::json::parse(raw, nullptr, false);
    if (!entries.is_array())
        return std::nullopt;

    for (const nlohmann::json& entry : entries) {
        if (!entry.is_object())
            continue;

        const auto plugin_key = entry.find("plugin_key");
        const auto name = entry.find("name");
        if (plugin_key == entry.end() || !plugin_key->is_string() || name == entry.end() || !name->is_string())
            continue;

        PluginCapabilityId id{PluginCapabilityType::Pages, name->get<std::string>(), plugin_key->get<std::string>()};
        if (!id.empty() && std::find(order.begin(), order.end(), id) == order.end())
            order.push_back(id);
    }
    return order;
}

inline std::string serialize_plugin_page_order(const std::vector<PluginCapabilityId>& order)
{
    nlohmann::json entries = nlohmann::json::array();
    for (const PluginCapabilityId& id : order)
        entries.push_back({{"plugin_key", id.plugin_key}, {"name", id.name}});
    return entries.dump();
}

// Pages may register in any order. Missing saved pages retain their position when
// they return; newly installed pages follow the saved pages in registration order.
inline void order_plugin_pages(std::vector<PluginCapabilityId>& active, const std::vector<PluginCapabilityId>& preferred)
{
    std::stable_sort(active.begin(), active.end(), [&preferred](const PluginCapabilityId& lhs, const PluginCapabilityId& rhs) {
        return std::find(preferred.begin(), preferred.end(), lhs) < std::find(preferred.begin(), preferred.end(), rhs);
    });
}

inline bool move_plugin_page(std::vector<PluginCapabilityId>& order,
                             const PluginCapabilityId& source,
                             const PluginCapabilityId& target,
                             bool before)
{
    const auto source_it = std::find(order.begin(), order.end(), source);
    const auto target_it = std::find(order.begin(), order.end(), target);
    if (source_it == order.end() || target_it == order.end() || source == target)
        return false;

    const auto source_index = static_cast<std::size_t>(source_it - order.begin());
    auto destination = static_cast<std::size_t>(target_it - order.begin()) + (before ? 0 : 1);
    if (source_index < destination)
        --destination;
    if (source_index == destination)
        return false;

    const PluginCapabilityId moved = *source_it;
    order.erase(source_it);
    order.insert(order.begin() + destination, moved);
    return true;
}

} // namespace Slic3r
