#include "VoronoiRouting.hpp"
#include "../ShortestPath.hpp"
#include <algorithm>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {
std::vector<PathGroup> order_groups(std::vector<PathGroup> groups, const Point &start_near)
{
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                               [](const PathGroup &group) { return group.paths.empty(); }), groups.end());
    std::vector<PathEndpoints> endpoints;
    endpoints.reserve(groups.size());
    for (const PathGroup &group : groups)
        endpoints.push_back({group.paths.front().first_point(), group.paths.back().last_point(), group.reversible});
    std::vector<PathGroup> ordered;
    ordered.reserve(groups.size());
    for (const auto &[index, reverse] : chain_path_endpoints(endpoints, &start_near)) {
        PathGroup &group = groups[index];
        if (reverse) {
            std::reverse(group.paths.begin(), group.paths.end());
            for (Polyline &path : group.paths) path.reverse();
        }
        ordered.push_back(std::move(group));
    }
    return ordered;
}

} // namespace Slic3r::Voronoi
