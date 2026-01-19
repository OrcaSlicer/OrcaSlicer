//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef LIMITED_BEADING_STRATEGY_H
#define LIMITED_BEADING_STRATEGY_H

#include <string>

#include "BeadingStrategy.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{

/*!
 * This is a meta-strategy that can be applied on top of any other beading
 * strategy, which limits the thickness of the walls to the thickness that the
 * lines can reasonably print.
 *
 * The width of the wall is limited to the maximum number of contours times the
 * maximum width of each of these contours.
 *
 * If the width of the wall gets limited, this strategy outputs one additional
 * bead with 0 width. This bead is used to denote the limits of the walled area.
 * Other structures can then use this border to align their structures to, such
 * as to create correctly overlapping infill or skin, or to align the infill
 * pattern to any extra infill walls.
 */
class LimitedBeadingStrategy : public BeadingStrategy
{
public:
    LimitedBeadingStrategy(coord_t max_bead_count, BeadingStrategyPtr parent);
    
    ~LimitedBeadingStrategy() override = default;
    
    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalThickness(coord_t bead_count) const override;
    coord_t getTransitionThickness(coord_t lower_bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    std::string toString() const override;
    
    coord_t getTransitioningLength(coord_t lower_bead_count) const override;
    
    float getTransitionAnchorPos(coord_t lower_bead_count) const override;

protected:
    const coord_t max_bead_count;
    const BeadingStrategyPtr parent;
};

} // namespace Slic3r::Arachne
#endif // LIMITED_DISTRIBUTED_BEADING_STRATEGY_H
