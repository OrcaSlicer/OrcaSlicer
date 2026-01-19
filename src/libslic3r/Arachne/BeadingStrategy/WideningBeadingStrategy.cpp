//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "WideningBeadingStrategy.hpp"

#include <algorithm>
#include <utility>

#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne
{

WideningBeadingStrategy::WideningBeadingStrategy(BeadingStrategyPtr parent, const coord_t min_input_width, const coord_t min_output_width)
    : BeadingStrategy(*parent)
    , parent(std::move(parent))
    , min_input_width(min_input_width)
    , min_output_width(min_output_width)
{
}

std::string WideningBeadingStrategy::toString() const
{
    return std::string("Widening+") + parent->toString();
}

WideningBeadingStrategy::Beading WideningBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    if (thickness < optimal_width) {
        Beading ret;
        ret.total_thickness = thickness;
        if (thickness >= min_input_width) {
            ret.bead_widths.emplace_back(std::max(thickness, min_output_width));
            ret.toolpath_locations.emplace_back(thickness / 2);
            ret.left_over = 0;
        } else
            ret.left_over = thickness;

        return ret;
    } else
        return parent->compute(thickness, bead_count);
}

coord_t WideningBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return parent->getOptimalThickness(bead_count);
}

coord_t WideningBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    if (lower_bead_count == 0)
        return min_input_width;
    else
        return parent->getTransitionThickness(lower_bead_count);
}

coord_t WideningBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < min_input_width)
        return 0;
    coord_t ret = parent->getOptimalBeadCount(thickness);
    if (thickness >= min_input_width && ret < 1)
        return 1;
    return ret;
}

coord_t WideningBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const 
{
    return parent->getTransitioningLength(lower_bead_count);
}

float WideningBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent->getTransitionAnchorPos(lower_bead_count);
}

std::vector<coord_t> WideningBeadingStrategy::getNonlinearThicknesses(coord_t lower_bead_count) const
{
    std::vector<coord_t> ret;
    ret.emplace_back(min_output_width);
    std::vector<coord_t> pret = parent->getNonlinearThicknesses(lower_bead_count);
    ret.insert(ret.end(), pret.begin(), pret.end());
    return ret;
}

} // namespace Slic3r::Arachne
