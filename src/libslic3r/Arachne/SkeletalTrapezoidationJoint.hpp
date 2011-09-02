//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef SKELETAL_TRAPEZOIDATION_JOINT_H
#define SKELETAL_TRAPEZOIDATION_JOINT_H

#include <memory> // smart pointers

#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r::Arachne
{

class SkeletalTrapezoidationJoint
{
    using Beading = BeadingStrategy::Beading;
public:
    struct BeadingPropagation
    {
        Beading beading;
        coord_t dist_to_bottom_source;
        coord_t dist_from_top_source;
        bool is_upward_propagated_only;
        BeadingPropagation(const Beading& beading)
            : beading(beading)
            , dist_to_bottom_source(0)
            , dist_from_top_source(0)
            , is_upward_propagated_only(false)
        {}
    };

    coord_t distance_to_boundary;
    coord_t bead_count;
    float transition_ratio; //! The distance near the skeleton to leave free because this joint is in the middle of a transition, as a fraction of the inner bead width of the bead at the higher transition.
    SkeletalTrapezoidationJoint()
    : distance_to_boundary(-1)
    , bead_count(-1)
    , transition_ratio(0)
    {}

    bool hasBeading() const
    {
        return beading.use_count() > 0;
    }
    void setBeading(std::shared_ptr<BeadingPropagation> storage)
    {
        beading = storage;
    }
    std::shared_ptr<BeadingPropagation> getBeading()
    {
        return beading.lock();
    }

private:

    std::weak_ptr<BeadingPropagation> beading;
};

} // namespace Slic3r::Arachne
#endif // SKELETAL_TRAPEZOIDATION_JOINT_H
