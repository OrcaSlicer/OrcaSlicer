//Copyright (c) 2022 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef WIDENING_BEADING_STRATEGY_H
#define WIDENING_BEADING_STRATEGY_H

#include <string>
#include <vector>

#include "BeadingStrategy.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{

/*!
 * This is a meta-strategy that can be applied on any other beading strategy. If
 * the part is thinner than a single line, this strategy adjusts the part so
 * that it becomes the minimum thickness of one line.
 *
 * This way, tiny pieces that are smaller than a single line will still be
 * printed.
 */
class WideningBeadingStrategy : public BeadingStrategy
{
public:
    /*!
     * Takes responsibility for deleting \param parent
     */
    WideningBeadingStrategy(BeadingStrategyPtr parent, coord_t min_input_width, coord_t min_output_width);

    ~WideningBeadingStrategy() override = default;

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalThickness(coord_t bead_count) const override;
    coord_t getTransitionThickness(coord_t lower_bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    coord_t getTransitioningLength(coord_t lower_bead_count) const override;
    float getTransitionAnchorPos(coord_t lower_bead_count) const override;
    std::vector<coord_t> getNonlinearThicknesses(coord_t lower_bead_count) const override;
    std::string toString() const override;

protected:
    BeadingStrategyPtr parent;
    const coord_t min_input_width;
    const coord_t min_output_width;
};

} // namespace Slic3r::Arachne
#endif // WIDENING_BEADING_STRATEGY_H
