// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef DISTRIBUTED_BEADING_STRATEGY_H
#define DISTRIBUTED_BEADING_STRATEGY_H

#include "BeadingStrategy.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{

/*!
 * This beading strategy chooses a wall count that would make the line width
 * deviate the least from the optimal line width, and then distributes the lines
 * evenly among the thickness available.
 */
class DistributedBeadingStrategy : public BeadingStrategy
{
protected:
    float one_over_distribution_radius_squared; // (1 / distribution_radius)^2

public:
    /*!
    * \param distribution_radius the radius (in number of beads) over which to distribute the discrepancy between the feature size and the optimal thickness
    */
    DistributedBeadingStrategy(coord_t optimal_width,
                               coord_t default_transition_length,
                               double  transitioning_angle,
                               double  wall_split_middle_threshold,
                               double  wall_add_middle_threshold,
                               int     distribution_radius);

    ~DistributedBeadingStrategy() override = default;

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
};

} // namespace Slic3r::Arachne
#endif // DISTRIBUTED_BEADING_STRATEGY_H
