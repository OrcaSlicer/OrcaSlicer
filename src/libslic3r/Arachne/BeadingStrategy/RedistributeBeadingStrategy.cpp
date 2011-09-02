//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "RedistributeBeadingStrategy.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne
{

RedistributeBeadingStrategy::RedistributeBeadingStrategy(const coord_t      optimal_width_outer,
                                                         const double       minimum_variable_line_ratio,
                                                         BeadingStrategyPtr parent)
    : BeadingStrategy(*parent)
    , parent(std::move(parent))
    , optimal_width_outer(optimal_width_outer)
    , minimum_variable_line_ratio(minimum_variable_line_ratio)
{
    name = "RedistributeBeadingStrategy";
}

coord_t RedistributeBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    const coord_t inner_bead_count = std::max(static_cast<coord_t>(0), bead_count - 2);
    const coord_t outer_bead_count = bead_count - inner_bead_count;
    return parent->getOptimalThickness(inner_bead_count) + optimal_width_outer * outer_bead_count;
}

coord_t RedistributeBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    switch (lower_bead_count) {
    case 0: return minimum_variable_line_ratio * optimal_width_outer;
    case 1: return (1.0 + parent->getSplitMiddleThreshold()) * optimal_width_outer;
    default: return parent->getTransitionThickness(lower_bead_count - 2) + 2 * optimal_width_outer;
    }
}

coord_t RedistributeBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < minimum_variable_line_ratio * optimal_width_outer)
        return 0;
    if (thickness <= 2 * optimal_width_outer)
        return thickness > (1.0 + parent->getSplitMiddleThreshold()) * optimal_width_outer ? 2 : 1;
    return parent->getOptimalBeadCount(thickness - 2 * optimal_width_outer) + 2;
}

coord_t RedistributeBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent->getTransitioningLength(lower_bead_count);
}

float RedistributeBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent->getTransitionAnchorPos(lower_bead_count);
}

std::string RedistributeBeadingStrategy::toString() const
{
    return std::string("RedistributeBeadingStrategy+") + parent->toString();
}

BeadingStrategy::Beading RedistributeBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    Beading ret;

    // Take care of all situations in which no lines are actually produced:
    if (bead_count == 0 || thickness < minimum_variable_line_ratio * optimal_width_outer) {
        ret.left_over       = thickness;
        ret.total_thickness = thickness;
        return ret;
    }

    // Compute the beadings of the inner walls, if any:
    const coord_t inner_bead_count = bead_count - 2;
    const coord_t inner_thickness  = thickness - 2 * optimal_width_outer;
    if (inner_bead_count > 0 && inner_thickness > 0) {
        ret = parent->compute(inner_thickness, inner_bead_count);
        for (auto &toolpath_location : ret.toolpath_locations) toolpath_location += optimal_width_outer;
    }

    // Insert the outer wall(s) around the previously computed inner wall(s), which may be empty:
    const coord_t actual_outer_thickness = bead_count > 2 ? std::min(thickness / 2, optimal_width_outer) : thickness / bead_count;
    ret.bead_widths.insert(ret.bead_widths.begin(), actual_outer_thickness);
    ret.toolpath_locations.insert(ret.toolpath_locations.begin(), actual_outer_thickness / 2);
    if (bead_count > 1) {
        ret.bead_widths.push_back(actual_outer_thickness);
        ret.toolpath_locations.push_back(thickness - actual_outer_thickness / 2);
    }

    // Ensure correct total and left over thickness.
    ret.total_thickness = thickness;
    ret.left_over       = thickness - std::accumulate(ret.bead_widths.cbegin(), ret.bead_widths.cend(), static_cast<coord_t>(0));
    return ret;
}

} // namespace Slic3r::Arachne
