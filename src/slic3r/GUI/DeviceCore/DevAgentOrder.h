#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

struct DevAgentListEntry
{
    size_t index;  // position in the input list
    bool   usable; // belongs to the live agent, so the current preset can drive it
};

// Orders printers for a list that mixes several printer agents: the live agent's printers first,
// then each other registered agent in registration order, then printers of unknown agents. The
// order inside one agent is kept. Only the live agent's printers are usable.
inline std::vector<DevAgentListEntry> order_printers_by_agent(const std::vector<std::string>& agent_ids,
                                                              const std::string&              live_agent_id,
                                                              const std::vector<std::string>& registration_order)
{
    auto rank = [&](const std::string& id) -> size_t {
        if (id == live_agent_id)
            return 0;
        auto it = std::find(registration_order.begin(), registration_order.end(), id);
        return 1 + static_cast<size_t>(it - registration_order.begin());
    };

    std::vector<DevAgentListEntry> result;
    result.reserve(agent_ids.size());
    for (size_t i = 0; i < agent_ids.size(); ++i)
        result.push_back({i, agent_ids[i] == live_agent_id});
    std::stable_sort(result.begin(), result.end(), [&](const DevAgentListEntry& a, const DevAgentListEntry& b) {
        return rank(agent_ids[a.index]) < rank(agent_ids[b.index]);
    });
    return result;
}

} // namespace Slic3r
