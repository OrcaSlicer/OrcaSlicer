//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef REDISTRIBUTE_DISTRIBUTED_BEADING_STRATEGY_H
#define REDISTRIBUTE_DISTRIBUTED_BEADING_STRATEGY_H

#include <string>

#include "BeadingStrategy.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{
    /*!
     * A meta-beading-strategy that takes outer and inner wall widths into account.
     *
     * The outer wall will try to keep a constant width by only applying the beading strategy on the inner walls. This
     * ensures that this outer wall doesn't react to changes happening to inner walls. It will limit print artifacts on
     * the surface of the print. Although this strategy technically deviates from the original philosophy of the paper.
     * It will generally results in better prints because of a smoother motion and less variation in extrusion width in
     * the outer walls.
     *
     * If the thickness of the model is less then two times the optimal outer wall width and once the minimum inner wall
     * width it will keep the minimum inner wall at a minimum constant and vary the outer wall widths symmetrical. Until
     * The thickness of the model is that of at least twice the optimal outer wall width it will then use two
     * symmetrical outer walls only. Until it transitions into a single outer wall. These last scenario's are always
     * symmetrical in nature, disregarding the user specified strategy.
     */
    class RedistributeBeadingStrategy : public BeadingStrategy
    {
    public:
        /*!
         * /param optimal_width_outer         Outer wall width, guaranteed to be the actual (save rounding errors) at a
         *                                    bead count if the parent strategies' optimum bead width is a weighted
         *                                    average of the outer and inner walls at that bead count.
         * /param minimum_variable_line_ratio Minimum factor that the variable line might deviate from the optimal width.
         */
        RedistributeBeadingStrategy(coord_t optimal_width_outer, double minimum_variable_line_ratio, BeadingStrategyPtr parent);

        ~RedistributeBeadingStrategy() override = default;

        Beading compute(coord_t thickness, coord_t bead_count) const override;

        coord_t getOptimalThickness(coord_t bead_count) const override;
        coord_t getTransitionThickness(coord_t lower_bead_count) const override;
        coord_t getOptimalBeadCount(coord_t thickness) const override;
        coord_t getTransitioningLength(coord_t lower_bead_count) const override;
        float getTransitionAnchorPos(coord_t lower_bead_count) const override;

        std::string toString() const override;

    protected:
        BeadingStrategyPtr parent;
        coord_t optimal_width_outer;
        double minimum_variable_line_ratio;
    };

} // namespace Slic3r::Arachne
#endif // INWARD_DISTRIBUTED_BEADING_STRATEGY_H
