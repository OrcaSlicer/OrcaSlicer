//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef OUTER_WALL_INSET_BEADING_STRATEGY_H
#define OUTER_WALL_INSET_BEADING_STRATEGY_H

#include <string>

#include "BeadingStrategy.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{
    /*
     * This is a meta strategy that allows for the outer wall to be inset towards the inside of the model. 
     */
    class OuterWallInsetBeadingStrategy : public BeadingStrategy
    {
    public:
        OuterWallInsetBeadingStrategy(coord_t outer_wall_offset, BeadingStrategyPtr parent);
         
        ~OuterWallInsetBeadingStrategy() override = default;

        Beading compute(coord_t thickness, coord_t bead_count) const override;
        
        coord_t getOptimalThickness(coord_t bead_count) const override;
        coord_t getTransitionThickness(coord_t lower_bead_count) const override;
        coord_t getOptimalBeadCount(coord_t thickness) const override;
        coord_t getTransitioningLength(coord_t lower_bead_count) const override;
        
        std::string toString() const override;
        
    private:
        BeadingStrategyPtr parent;
        coord_t outer_wall_offset;
    };
} // namespace Slic3r::Arachne
#endif // OUTER_WALL_INSET_BEADING_STRATEGY_H
