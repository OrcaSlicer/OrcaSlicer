//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef SKELETAL_TRAPEZOIDATION_EDGE_H
#define SKELETAL_TRAPEZOIDATION_EDGE_H

#include <memory> // smart pointers
#include <list>
#include <vector>

#include "utils/ExtrusionJunction.hpp"

namespace Slic3r::Arachne
{

class SkeletalTrapezoidationEdge
{
private:
    enum class Central { UNKNOWN = -1, NO, YES };

public:
    /*!
     * Representing the location along an edge where the anchor position of a transition should be placed.
     */
    struct TransitionMiddle
    {
        coord_t pos; // Position along edge as measure from edge.from.p
        int lower_bead_count;
        coord_t feature_radius; // The feature radius at which this transition is placed
        TransitionMiddle(coord_t pos, int lower_bead_count, coord_t feature_radius)
            : pos(pos), lower_bead_count(lower_bead_count)
            , feature_radius(feature_radius)
        {}
    };

    /*!
     * Represents the location along an edge where the lower or upper end of a transition should be placed.
     */
    struct TransitionEnd
    {
        coord_t pos; // Position along edge as measure from edge.from.p, where the edge is always the half edge oriented from lower to higher R
        int lower_bead_count;
        bool is_lower_end; // Whether this is the ed of the transition with lower bead count
        TransitionEnd(coord_t pos, int lower_bead_count, bool is_lower_end)
            : pos(pos), lower_bead_count(lower_bead_count), is_lower_end(is_lower_end)
        {}
    };

    enum class EdgeType
    {
        NORMAL = 0, // from voronoi diagram
        EXTRA_VD = 1, // introduced to voronoi diagram in order to make the gMAT
        TRANSITION_END = 2 // introduced to voronoi diagram in order to make the gMAT
    };
    EdgeType type;

    SkeletalTrapezoidationEdge() : SkeletalTrapezoidationEdge(EdgeType::NORMAL) {}
    SkeletalTrapezoidationEdge(const EdgeType &type) : type(type), is_central(Central::UNKNOWN) {}

    bool isCentral() const
    {
        assert(is_central != Central::UNKNOWN);
        return is_central == Central::YES;
    }
    void setIsCentral(bool b)
    {
        is_central = b ? Central::YES : Central::NO;
    }
    bool centralIsSet() const
    {
        return is_central != Central::UNKNOWN;
    }

    bool hasTransitions(bool ignore_empty = false) const
    {
        return transitions.use_count() > 0 && (ignore_empty || ! transitions.lock()->empty());
    }
    void setTransitions(std::shared_ptr<std::list<TransitionMiddle>> storage)
    {
        transitions = storage;
    }
    std::shared_ptr<std::list<TransitionMiddle>> getTransitions()
    {
        return transitions.lock();
    }

    bool hasTransitionEnds(bool ignore_empty = false) const
    {
        return transition_ends.use_count() > 0 && (ignore_empty || ! transition_ends.lock()->empty());
    }
    void setTransitionEnds(std::shared_ptr<std::list<TransitionEnd>> storage)
    {
        transition_ends = storage;
    }
    std::shared_ptr<std::list<TransitionEnd>> getTransitionEnds()
    {
        return transition_ends.lock();
    }

    bool hasExtrusionJunctions(bool ignore_empty = false) const
    {
        return extrusion_junctions.use_count() > 0 && (ignore_empty || ! extrusion_junctions.lock()->empty());
    }
    void setExtrusionJunctions(std::shared_ptr<LineJunctions> storage)
    {
        extrusion_junctions = storage;
    }
    std::shared_ptr<LineJunctions> getExtrusionJunctions()
    {
        return extrusion_junctions.lock();
    }

private:
    Central is_central; //! whether the edge is significant; whether the source segments have a sharp angle; -1 is unknown

    std::weak_ptr<std::list<TransitionMiddle>> transitions;
    std::weak_ptr<std::list<TransitionEnd>> transition_ends;
    std::weak_ptr<LineJunctions> extrusion_junctions;
};

} // namespace Slic3r::Arachne
#endif // SKELETAL_TRAPEZOIDATION_EDGE_H
