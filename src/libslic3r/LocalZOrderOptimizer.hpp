#ifndef slic3r_LocalZOrderOptimizer_hpp_
#define slic3r_LocalZOrderOptimizer_hpp_

#include <algorithm>
#include <numeric>
#include <vector>

namespace Slic3r {
namespace LocalZOrderOptimizer {

inline bool bucket_contains_extruder(const std::vector<unsigned int> &extruders, int extruder_id)
{
    return extruder_id >= 0 &&
           std::find(extruders.begin(), extruders.end(), static_cast<unsigned int>(extruder_id)) != extruders.end();
}

inline std::vector<unsigned int> order_bucket_extruders(std::vector<unsigned int> extruders,
                                                        int                       current_extruder,
                                                        int                       preferred_last_extruder = -1)
{
    extruders.erase(std::unique(extruders.begin(), extruders.end()), extruders.end());
    if (extruders.empty())
        return extruders;

    if (current_extruder >= 0) {
        auto current_it = std::find(extruders.begin(), extruders.end(), static_cast<unsigned int>(current_extruder));
        if (current_it != extruders.end())
            std::rotate(extruders.begin(), current_it, extruders.end());
    }

    if (preferred_last_extruder >= 0 && extruders.size() > 1 && static_cast<int>(extruders.front()) != preferred_last_extruder) {
        auto preferred_it = std::find(extruders.begin() + 1, extruders.end(), static_cast<unsigned int>(preferred_last_extruder));
        if (preferred_it != extruders.end())
            std::rotate(preferred_it, preferred_it + 1, extruders.end());
    }

    return extruders;
}

inline std::vector<size_t> order_pass_group(const std::vector<std::vector<unsigned int>> &group_extruders, int current_extruder)
{
    std::vector<size_t> remaining(group_extruders.size());
    std::iota(remaining.begin(), remaining.end(), size_t(0));

    std::vector<size_t> ordered;
    ordered.reserve(group_extruders.size());

    int active_extruder = current_extruder;
    while (!remaining.empty()) {
        auto next_it = std::find_if(remaining.begin(), remaining.end(), [&](size_t idx) {
            return bucket_contains_extruder(group_extruders[idx], active_extruder);
        });
        if (next_it == remaining.end())
            next_it = remaining.begin();

        const size_t next_idx = *next_it;
        ordered.push_back(next_idx);

        const std::vector<unsigned int> ordered_bucket = order_bucket_extruders(group_extruders[next_idx], active_extruder);
        if (!ordered_bucket.empty())
            active_extruder = static_cast<int>(ordered_bucket.back());

        remaining.erase(next_it);
    }

    return ordered;
}

} // namespace LocalZOrderOptimizer
} // namespace Slic3r

#endif
