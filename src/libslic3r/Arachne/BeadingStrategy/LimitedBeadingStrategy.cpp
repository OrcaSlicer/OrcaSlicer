//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include <boost/log/trivial.hpp>
#include <cassert>
#include <utility>
#include <cstddef>

#include "LimitedBeadingStrategy.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne
{

std::string LimitedBeadingStrategy::toString() const
{
    return std::string("LimitedBeadingStrategy+") + parent->toString();
}

coord_t LimitedBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent->getTransitioningLength(lower_bead_count);
}

float LimitedBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent->getTransitionAnchorPos(lower_bead_count);
}

LimitedBeadingStrategy::LimitedBeadingStrategy(const coord_t max_bead_count, BeadingStrategyPtr parent)
    : BeadingStrategy(*parent)
    , max_bead_count(max_bead_count)
    , parent(std::move(parent))
{
    if (max_bead_count % 2 == 1)
    {
        BOOST_LOG_TRIVIAL(warning) << "LimitedBeadingStrategy with odd bead count is odd indeed!";
    }
}

LimitedBeadingStrategy::Beading LimitedBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    if (bead_count <= max_bead_count)
    {
        Beading ret = parent->compute(thickness, bead_count);
        bead_count = ret.toolpath_locations.size();

        if (bead_count % 2 == 0 && bead_count == max_bead_count)
        {
            const coord_t innermost_toolpath_location = ret.toolpath_locations[max_bead_count / 2 - 1];
            const coord_t innermost_toolpath_width = ret.bead_widths[max_bead_count / 2 - 1];
            ret.toolpath_locations.insert(ret.toolpath_locations.begin() + max_bead_count / 2, innermost_toolpath_location + innermost_toolpath_width / 2);
            ret.bead_widths.insert(ret.bead_widths.begin() + max_bead_count / 2, 0);
        }
        return ret;
    }
    assert(bead_count == max_bead_count + 1);
    if(bead_count != max_bead_count + 1)
    {
        BOOST_LOG_TRIVIAL(warning) << "Too many beads! " << bead_count << " != " << max_bead_count + 1;
    }

    coord_t optimal_thickness = parent->getOptimalThickness(max_bead_count);
    Beading ret = parent->compute(optimal_thickness, max_bead_count);
    bead_count = ret.toolpath_locations.size();
    ret.left_over += thickness - ret.total_thickness;
    ret.total_thickness = thickness;
    
    // Enforce symmetry
    if (bead_count % 2 == 1) {
        ret.toolpath_locations[bead_count / 2] = thickness / 2;
        ret.bead_widths[bead_count / 2] = thickness - optimal_thickness;
    }
    for (coord_t bead_idx = 0; bead_idx < (bead_count + 1) / 2; bead_idx++)
        ret.toolpath_locations[bead_count - 1 - bead_idx] = thickness - ret.toolpath_locations[bead_idx];

    //Create a "fake" inner wall with 0 width to indicate the edge of the walled area.
    //This wall can then be used by other structures to e.g. fill the infill area adjacent to the variable-width walls.
    coord_t innermost_toolpath_location = ret.toolpath_locations[max_bead_count / 2 - 1];
    coord_t innermost_toolpath_width = ret.bead_widths[max_bead_count / 2 - 1];
    ret.toolpath_locations.insert(ret.toolpath_locations.begin() + max_bead_count / 2, innermost_toolpath_location + innermost_toolpath_width / 2);
    ret.bead_widths.insert(ret.bead_widths.begin() + max_bead_count / 2, 0);

    //Symmetry on both sides. Symmetry is guaranteed since this code is stopped early if the bead_count <= max_bead_count, and never reaches this point then.
    const size_t opposite_bead = bead_count - (max_bead_count / 2 - 1);
    innermost_toolpath_location = ret.toolpath_locations[opposite_bead];
    innermost_toolpath_width = ret.bead_widths[opposite_bead];
    ret.toolpath_locations.insert(ret.toolpath_locations.begin() + opposite_bead, innermost_toolpath_location - innermost_toolpath_width / 2);
    ret.bead_widths.insert(ret.bead_widths.begin() + opposite_bead, 0);

    return ret;
}

coord_t LimitedBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    if (bead_count <= max_bead_count)
        return parent->getOptimalThickness(bead_count);
    assert(false);
    return scaled<coord_t>(1000.); // 1 meter (Cura was returning 10 meter)
}

coord_t LimitedBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    if (lower_bead_count < max_bead_count)
        return parent->getTransitionThickness(lower_bead_count);

    if (lower_bead_count == max_bead_count)
        return parent->getOptimalThickness(lower_bead_count + 1) - scaled<coord_t>(0.01);

    assert(false);
    return scaled<coord_t>(900.); // 0.9 meter;
}

coord_t LimitedBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    coord_t parent_bead_count = parent->getOptimalBeadCount(thickness);
    if (parent_bead_count <= max_bead_count) {
        return parent->getOptimalBeadCount(thickness);
    } else if (parent_bead_count == max_bead_count + 1) {
        if (thickness < parent->getOptimalThickness(max_bead_count + 1) - scaled<coord_t>(0.01))
            return max_bead_count;
        else 
            return max_bead_count + 1;
    }
    else return max_bead_count + 1;
}

} // namespace Slic3r::Arachne
