//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "OuterWallInsetBeadingStrategy.hpp"

#include <algorithm>
#include <utility>

#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne
{
OuterWallInsetBeadingStrategy::OuterWallInsetBeadingStrategy(coord_t outer_wall_offset, BeadingStrategyPtr parent)
    : BeadingStrategy(*parent), parent(std::move(parent)), outer_wall_offset(outer_wall_offset)
{
    name = "OuterWallOfsetBeadingStrategy";
}

coord_t OuterWallInsetBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return parent->getOptimalThickness(bead_count);
}

coord_t OuterWallInsetBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    return parent->getTransitionThickness(lower_bead_count);
}

coord_t OuterWallInsetBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    return parent->getOptimalBeadCount(thickness);
}

coord_t OuterWallInsetBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent->getTransitioningLength(lower_bead_count);
}

std::string OuterWallInsetBeadingStrategy::toString() const
{
    return std::string("OuterWallOfsetBeadingStrategy+") + parent->toString();
}

BeadingStrategy::Beading OuterWallInsetBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    Beading ret = parent->compute(thickness, bead_count);

    // Actual count and thickness as represented by extant walls. Don't count any potential zero-width 'signaling' walls.
    bead_count = std::count_if(ret.bead_widths.begin(), ret.bead_widths.end(), [](const coord_t width) { return width > 0; });

    // No need to apply any inset if there is just a single wall.
    if (bead_count < 2)
    {
        return ret;
    }

    // Actually move the outer wall inside. Ensure that the outer wall never goes beyond the middle line.
    ret.toolpath_locations[0] = std::min(ret.toolpath_locations[0] + outer_wall_offset, thickness / 2);
    return ret;
}

} // namespace Slic3r::Arachne
