#pragma once

#include "../Polyline.hpp"
#include <vector>

namespace Slic3r::Voronoi {

struct PathGroup {
    Polylines paths;
    bool reversible { false };
};
// Wall trails may reverse; floor groups retain their supported-side-first sweep.
std::vector<PathGroup> order_groups(std::vector<PathGroup> groups, const Point &start_near);

} // namespace Slic3r::Voronoi
