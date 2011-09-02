// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.
#include <numeric>
#include <algorithm>
#include <vector>
#include <cassert>

#include "DistributedBeadingStrategy.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne
{

DistributedBeadingStrategy::DistributedBeadingStrategy(const coord_t optimal_width,
                                                       const coord_t default_transition_length,
                                                       const double  transitioning_angle,
                                                       const double  wall_split_middle_threshold,
                                                       const double  wall_add_middle_threshold,
                                                       const int     distribution_radius)
    : BeadingStrategy(optimal_width, wall_split_middle_threshold, wall_add_middle_threshold, default_transition_length, transitioning_angle)
{
    if(distribution_radius >= 2)
        one_over_distribution_radius_squared = 1.0f / (distribution_radius - 1) * 1.0f / (distribution_radius - 1);
    else
        one_over_distribution_radius_squared = 1.0f / 1 * 1.0f / 1;
    name = "DistributedBeadingStrategy";
}

DistributedBeadingStrategy::Beading DistributedBeadingStrategy::compute(const coord_t thickness, const coord_t bead_count) const
{
    Beading ret;

    ret.total_thickness = thickness;
    if (bead_count > 2) {
        const coord_t to_be_divided = thickness - bead_count * optimal_width;
        const float middle = static_cast<float>(bead_count - 1) / 2;

        const auto getWeight = [middle, this](coord_t bead_idx) {
            const float dev_from_middle = bead_idx - middle;
            return std::max(0.0f, 1.0f - one_over_distribution_radius_squared * dev_from_middle * dev_from_middle);
        };

        std::vector<float> weights;
        weights.resize(bead_count);
        for (coord_t bead_idx = 0; bead_idx < bead_count; bead_idx++)
            weights[bead_idx] = getWeight(bead_idx);

        const float total_weight      = std::accumulate(weights.cbegin(), weights.cend(), 0.f);
        coord_t     accumulated_width = 0;
        for (coord_t bead_idx = 0; bead_idx < bead_count; bead_idx++) {
            const float   weight_fraction          = weights[bead_idx] / total_weight;
            const coord_t splitup_left_over_weight = to_be_divided * weight_fraction;
            const coord_t width                    = (bead_idx == bead_count - 1) ? thickness - accumulated_width : optimal_width + splitup_left_over_weight;

            // Be aware that toolpath_locations is computed by dividing the width by 2, so toolpath_locations
            // could be off by 1 because of rounding errors.
            if (bead_idx == 0)
                ret.toolpath_locations.emplace_back(width / 2);
            else
                ret.toolpath_locations.emplace_back(ret.toolpath_locations.back() + (ret.bead_widths.back() + width) / 2);
            ret.bead_widths.emplace_back(width);
            accumulated_width += width;
        }
        ret.left_over = 0;
        assert((accumulated_width + ret.left_over) == thickness);
    } else if (bead_count == 2) {
        const coord_t outer_width = thickness / 2;
        ret.bead_widths.emplace_back(outer_width);
        ret.bead_widths.emplace_back(outer_width);
        ret.toolpath_locations.emplace_back(outer_width / 2);
        ret.toolpath_locations.emplace_back(thickness - outer_width / 2);
        ret.left_over = 0;
    } else if (bead_count == 1) {
        const coord_t outer_width = thickness;
        ret.bead_widths.emplace_back(outer_width);
        ret.toolpath_locations.emplace_back(outer_width / 2);
        ret.left_over = 0;
    } else {
        ret.left_over = thickness;
    }

    assert(([&ret = std::as_const(ret), thickness]() -> bool {
        coord_t total_bead_width = 0;
        for (const coord_t &bead_width : ret.bead_widths)
            total_bead_width += bead_width;
        return (total_bead_width + ret.left_over) == thickness;
    }()));

    return ret;
}

coord_t DistributedBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    const coord_t naive_count        = thickness / optimal_width;               // How many lines we can fit in for sure.
    const coord_t remainder          = thickness - naive_count * optimal_width; // Space left after fitting that many lines.
    const coord_t minimum_line_width = optimal_width * (naive_count % 2 == 1 ? wall_split_middle_threshold : wall_add_middle_threshold);
    return naive_count + (remainder >= minimum_line_width); // If there's enough space, fit an extra one.
}

} // namespace Slic3r::Arachne
