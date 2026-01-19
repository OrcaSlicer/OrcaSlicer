//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef SKELETAL_TRAPEZOIDATION_H
#define SKELETAL_TRAPEZOIDATION_H

#include <boost/polygon/voronoi.hpp>
#include <ankerl/unordered_dense.h>
#include <memory> // smart pointers
#include <utility> // pair
#include <list>
#include <vector>

#include "utils/HalfEdgeGraph.hpp"
#include "utils/PolygonsSegmentIndex.hpp"
#include "utils/ExtrusionJunction.hpp"
#include "utils/ExtrusionLine.hpp"
#include "SkeletalTrapezoidationEdge.hpp"
#include "SkeletalTrapezoidationJoint.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"
#include "SkeletalTrapezoidationGraph.hpp"
#include "../Geometry/Voronoi.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

//#define ARACHNE_DEBUG
//#define ARACHNE_DEBUG_VORONOI

namespace Slic3r::Arachne {

using VD = Slic3r::Geometry::VoronoiDiagram;

/*!
 * Main class of the dynamic beading strategies.
 *
 * The input polygon region is decomposed into trapezoids and represented as a half-edge data-structure.
 *
 * We determine which edges are 'central' accordinding to the transitioning_angle of the beading strategy,
 * and determine the bead count for these central regions and apply them outward when generating toolpaths. [oversimplified]
 *
 * The method can be visually explained as generating the 3D union of cones surface on the outline polygons,
 * and changing the heights along central regions of that surface so that they are flat.
 * For more info, please consult the paper "A framework for adaptive width control of dense contour-parallel toolpaths in fused
deposition modeling" by Kuipers et al.
 * This visual explanation aid explains the use of "upward", "lower" etc,
 * i.e. the radial distance and/or the bead count are used as heights of this visualization, there is no coordinate called 'Z'.
 *
 * TODO: split this class into two:
 * 1. Class for generating the decomposition and aux functions for performing updates
 * 2. Class for editing the structure for our purposes.
 */
class SkeletalTrapezoidation
{
    using graph_t = SkeletalTrapezoidationGraph;
    using edge_t = STHalfEdge;
    using node_t = STHalfEdgeNode;
    using Beading = BeadingStrategy::Beading;
    using BeadingPropagation = SkeletalTrapezoidationJoint::BeadingPropagation;
    using TransitionMiddle = SkeletalTrapezoidationEdge::TransitionMiddle;
    using TransitionEnd = SkeletalTrapezoidationEdge::TransitionEnd;

    template<typename T>
    using ptr_vector_t = std::vector<std::shared_ptr<T>>;

    double  transitioning_angle; //!< How pointy a region should be before we apply the method. Equals 180* - limit_bisector_angle
    coord_t discretization_step_size; //!< approximate size of segments when parabolic VD edges get discretized (and vertex-vertex edges)
    coord_t transition_filter_dist; //!< Filter transition mids (i.e. anchors) closer together than this
    coord_t allowed_filter_deviation; //!< The allowed line width deviation induced by filtering
    coord_t beading_propagation_transition_dist; //!< When there are different beadings propagated from below and from above, use this transitioning distance
    //!< Filter areas marked as 'central' smaller than this
    inline coord_t central_filter_dist() { return scaled<coord_t>(0.02); }
    //!< Generic arithmatic inaccuracy. Only used to determine whether a transition really needs to insert an extra edge.
    inline coord_t snap_dist() { return scaled<coord_t>(0.02); }

    /*!
     * The strategy to use to fill a certain shape with lines.
     *
     * Various BeadingStrategies are available that differ in which lines get to
     * print at their optimal width, where the play is being compensated, and
     * how the joints are handled where we transition to different numbers of
     * lines.
     */
    const BeadingStrategy& beading_strategy;

public:
    using Segment  = PolygonsSegmentIndex;
    using NodeSet  = ankerl::unordered_dense::set<node_t*>;

    /*!
     * Construct a new trapezoidation problem to solve.
     * \param polys The shapes to fill with walls.
     * \param beading_strategy The strategy to use to fill these shapes.
     * \param transitioning_angle Where we transition to a different number of
     * walls, how steep should this transition be? A lower angle means that the
     * transition will be longer.
     * \param discretization_step_size Since g-code can't represent smooth
     * transitions in line width, the line width must change with discretized
     * steps. This indicates how long the line segments between those steps will
     * be.
     * \param transition_filter_dist The minimum length of transitions.
     * Transitions shorter than this will be considered for dissolution.
     * \param beading_propagation_transition_dist When there are different
     * beadings propagated from below and from above, use this transitioning
     * distance.
     */
    SkeletalTrapezoidation(const Polygons& polys,
                           const BeadingStrategy& beading_strategy,
                           double transitioning_angle
    , coord_t discretization_step_size
    , coord_t transition_filter_dist
    , coord_t allowed_filter_deviation
    , coord_t beading_propagation_transition_dist);

    /*!
     * A skeletal graph through the polygons that we need to fill with beads.
     *
     * The skeletal graph represents the medial axes through each part of the
     * polygons, and the lines from these medial axes towards each vertex of the
     * polygons. The graph can be used to see what the width is of a polygon in
     * each place and where the width transitions.
     */
    graph_t graph;

    /*!
     * Generate the paths that the printer must extrude, to print the outlines
     * in the input polygons.
     * \param filter_outermost_central_edges Some edges are "central" but still
     * touch the outside of the polygon. If enabled, don't treat these as
     * "central" but as if it's a obtuse corner. As a result, sharp corners will
     * no longer end in a single line but will just loop.
     */
    void generateToolpaths(std::vector<VariableWidthLines> &generated_toolpaths, bool filter_outermost_central_edges = false);

#ifdef ARACHNE_DEBUG
    Polygons outline;
#endif

protected:
    /*!
     * Auxiliary for referencing one transition along an edge which may contain multiple transitions
     */
    struct TransitionMidRef
    {
        edge_t* edge;
        std::list<TransitionMiddle>::iterator transition_it;
        TransitionMidRef(edge_t* edge, std::list<TransitionMiddle>::iterator transition_it)
            : edge(edge)
            , transition_it(transition_it)
        {}
    };

    /*!
     * Compute the skeletal trapezoidation decomposition of the input shape.
     *
     * Compute the Voronoi Diagram (VD) and transfer all inside edges into our half-edge (HE) datastructure.
     *
     * The algorithm is currently a bit overcomplicated, because the discretization of parabolic edges is performed at the same time as all edges are being transfered,
     * which means that there is no one-to-one mapping from VD edges to HE edges.
     * Instead we map from a VD edge to the last HE edge.
     * This could be cimplified by recording the edges which should be discretized and discretizing the mafterwards.
     *
     * Another complication arises because the VD uses floating logic, which can result in zero-length segments after rounding to integers.
     * We therefore collapse edges and their whole cells afterwards.
     */
    void constructFromPolygons(const Polygons& polys);

    /*!
     * mapping each voronoi VD edge to the corresponding halfedge HE edge
     * In case the result segment is discretized, we map the VD edge to the *last* HE edge
     */
    ankerl::unordered_dense::map<const VD::edge_type *, edge_t *> vd_edge_to_he_edge;
    ankerl::unordered_dense::map<const VD::vertex_type *, node_t *> vd_node_to_he_node;
    node_t &makeNode(const VD::vertex_type &vd_node, Point p); //!< Get the node which the VD node maps to, or create a new mapping if there wasn't any yet.

    /*!
     * (Eventual) returned 'polylines per index' result (from generateToolpaths):
     */
    std::vector<VariableWidthLines> *p_generated_toolpaths;

    /*!
     * Transfer an edge from the VD to the HE and perform discretization of parabolic edges (and vertex-vertex edges)
     * \p prev_edge serves as input and output. May be null as input.
     */
    void transferEdge(const Point &from, const Point &to, const VD::edge_type &vd_edge, edge_t *&prev_edge, const Point &start_source_point, const Point &end_source_point, const std::vector<Segment> &segments);

    /*!
     * Discretize a Voronoi edge that represents the medial axis of a vertex-
     * line region or vertex-vertex region into small segments that can be
     * considered to have a straight medial axis and a linear line width
     * transition.
     *
     * The medial axis between a point and a line is a parabola. The rest of the
     * algorithm doesn't want to have to deal with parabola, so this discretises
     * the parabola into straight line segments. This is necessary if there is a
     * sharp inner corner (acts as a point) that comes close to a straight edge.
     *
     * The medial axis between a point and a point is a straight line segment.
     * However the distance from the medial axis to either of those points draws
     * a parabola as you go along the medial axis. That means that the resulting
     * line width along the medial axis would not be linearly increasing or
     * linearly decreasing, but needs to take the shape of a parabola. Instead,
     * we'll break this edge up into tiny line segments that can approximate the
     * parabola with tiny linear increases or decreases in line width.
     * \param segment The variable-width Voronoi edge to discretize.
     * \param points All vertices of the original Polygons to fill with beads.
     * \param segments All line segments of the original Polygons to fill with
     * beads.
     * \return A number of coordinates along the edge where the edge is broken
     * up into discrete pieces.
     */
    Points discretize(const VD::edge_type& segment, const std::vector<Segment>& segments);

    /*!
     * For VD cells associated with an input polygon vertex, we need to separate the node at the end and start of the cell into two
     * That way we can reach both the quad_start and the quad_end from the [incident_edge] of the two new nodes
     * Otherwise if node.incident_edge = quad_start you couldnt reach quad_end.twin by normal iteration (i.e. it = it.twin.next)
     */
    void separatePointyQuadEndNodes();


    // ^ init | v transitioning

    void updateIsCentral(); // Update the "is_central" flag for each edge based on the transitioning_angle

    /*!
     * Filter out small central areas.
     *
     * Only used to get rid of small edges which get marked as central because
     * of rounding errors because the region is so small.
     */
    void filterCentral(coord_t max_length);

    /*!
     * Filter central areas connected to starting_edge recursively.
     * \return Whether we should unmark this section marked as central, on the
     * way back out of the recursion.
     */
    bool filterCentral(edge_t* starting_edge, coord_t traveled_dist, coord_t max_length);

    /*!
     * Unmark the outermost edges directly connected to the outline, as not
     * being central.
     *
     * Only used to emulate some related literature.
     *
     * The paper shows that this function is bad for the stability of the framework.
     */
    void filterOuterCentral();

    /*!
     * Set bead count in central regions based on the optimal_bead_count of the
     * beading strategy.
     */
    void updateBeadCount();

    /*!
     * Add central regions and set bead counts where there is an end of the
     * central area and when traveling upward we get to another region with the
     * same bead count.
     */
    void filterNoncentralRegions();

    /*!
     * Add central regions and set bead counts for a particular edge and all of
     * its adjacent edges.
     *
     * Recursive subroutine for \ref filterNoncentralRegions().
     * \return Whether to set the bead count on the way back
     */
    bool filterNoncentralRegions(edge_t* to_edge, coord_t bead_count, coord_t traveled_dist, coord_t max_dist);

    /*!
     * Generate middle points of all transitions on edges.
     *
     * The transition middle points are saved in the graph itself. They are also
     * returned via the output parameter.
     * \param[out] edge_transitions A list of transitions that were generated.
     */
    void generateTransitionMids(ptr_vector_t<std::list<TransitionMiddle>>& edge_transitions);

    /*!
     * Removes some transition middle points.
     *
     * Transitions can be removed if there are multiple intersecting transitions
     * that are too close together. If transitions have opposite effects, both
     * are removed.
     */
    void filterTransitionMids();

    /*!
     * Merge transitions that are too close together.
     * \param edge_to_start Edge pointing to the node from which to start
     * traveling in all directions except along \p edge_to_start .
     * \param origin_transition The transition for which we are checking nearby
     * transitions.
     * \param traveled_dist The distance traveled before we came to
     * \p edge_to_start.to .
     * \param going_up Whether we are traveling in the upward direction as seen
     * from the \p origin_transition. If this doesn't align with the direction
     * according to the R diff on a consecutive edge we know there was a local
     * optimum.
     * \return Whether the origin transition should be dissolved.
     */
    std::list<TransitionMidRef> dissolveNearbyTransitions(edge_t* edge_to_start, TransitionMiddle& origin_transition, coord_t traveled_dist, coord_t max_dist, bool going_up);

    /*!
     * Spread a certain bead count over a region in the graph.
     * \param edge_to_start One edge of the region to spread the bead count in.
     * \param from_bead_count All edges with this bead count will be changed.
     * \param to_bead_count The new bead count for those edges.
     */
    void dissolveBeadCountRegion(edge_t* edge_to_start, coord_t from_bead_count, coord_t to_bead_count);

    /*!
     * Change the bead count if the given edge is at the end of a central
     * region.
     *
     * This is necessary to provide a transitioning bead count to the edges of a
     * central region to transition more smoothly from a high bead count in the
     * central region to a lower bead count at the edge.
     * \param edge_to_start One edge from a zone that needs to be filtered.
     * \param traveled_dist The distance along the edges we've traveled so far.
     * \param max_distance Don't filter beyond this range.
     * \param replacing_bead_count The new bead count for this region.
     * \return ``true`` if the bead count of this edge was changed.
     */
    bool filterEndOfCentralTransition(edge_t* edge_to_start, coord_t traveled_dist, coord_t max_dist, coord_t replacing_bead_count);

    /*!
     * Generate the endpoints of all transitions for all edges in the graph.
     * \param[out] edge_transition_ends The resulting transition endpoints.
     */
    void generateAllTransitionEnds(ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends);

    /*!
     * Also set the rest values at nodes in between the transition ends
     */
    void applyTransitions(ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends);

    /*!
     * Create extra edges along all edges, where it needs to transition from one
     * bead count to another.
     *
     * For example, if an edge of the graph goes from a bead count of 6 to a
     * bead count of 1, it needs to generate 5 places where the beads around
     * this line transition to a lower bead count. These are the "ribs". They
     * reach from the edge to the border of the polygon. Where the beads hit
     * those ribs the beads know to make a transition.
     */
    void generateTransitioningRibs();

    /*!
     * Generate the endpoints of a specific transition midpoint.
     * \param edge The edge to create transitions on.
     * \param mid_R The radius of the transition middle point.
     * \param transition_lower_bead_count The bead count at the lower end of the
     * transition.
     * \param[out] edge_transition_ends A list of endpoints to add the new
     * endpoints to.
     */
    void generateTransitionEnds(edge_t& edge, coord_t mid_R, coord_t transition_lower_bead_count, ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends);

    /*!
     * Compute a single endpoint of a transition.
     * \param edge The edge to generate the endpoint for.
     * \param start_pos The position where the transition starts.
     * \param end_pos The position where the transition ends on the other side.
     * \param transition_half_length The distance to the transition middle
     * point.
     * \param start_rest The gap between the start of the transition and the
     * starting endpoint, as ratio of the inner bead width at the high end of
     * the transition.
     * \param end_rest The gap between the end of the transition and the ending
     * endpoint, as ratio of the inner bead width at the high end of the
     * transition.
     * \param transition_lower_bead_count The bead count at the lower end of the
     * transition.
     * \param[out] edge_transition_ends The list to put the resulting endpoints
     * in.
     * \return Whether the given edge is going downward (i.e. towards a thinner
     * region of the polygon).
     */
    bool generateTransitionEnd(edge_t& edge, coord_t start_pos, coord_t end_pos, coord_t transition_half_length, double start_rest, double end_rest, coord_t transition_lower_bead_count, ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends);

    /*!
     * Determines whether an edge is going downwards or upwards in the graph.
     *
     * An edge is said to go "downwards" if it's going towards a narrower part
     * of the polygon. The notion of "downwards" comes from the conical
     * representation of the graph, where the polygon is filled with a cone of
     * maximum radius.
     *
     * This function works by recursively checking adjacent edges until the edge
     * is reached.
     * \param outgoing The edge to check.
     * \param traveled_dist The distance traversed so far.
     * \param transition_half_length The radius of the transition width.
     * \param lower_bead_count The bead count at the lower end of the edge.
     * \return ``true`` if this edge is going down, or ``false`` if it's going
     * up.
     */
    bool isGoingDown(edge_t* outgoing, coord_t traveled_dist, coord_t transition_half_length, coord_t lower_bead_count) const;

    /*!
     * Determines whether this edge marks the end of the central region.
     * \param edge The edge to check.
     * \return ``true`` if this edge goes from a central region to a non-central
     * region, or ``false`` in every other case (central to central, non-central
     * to non-central, non-central to central, or end-of-the-line).
     */
    bool isEndOfCentral(const edge_t& edge) const;

    /*!
     * Create extra ribs in the graph where the graph contains a parabolic arc
     * or a straight between two inner corners.
     *
     * There might be transitions there as the beads go through a narrow
     * bottleneck in the polygon.
     */
    void generateExtraRibs();

    // ^ transitioning ^

    // v toolpath generation v

    /*!
     * \param[out] segments the generated segments
     */
    void generateSegments();

    /*!
     * From a quad (a group of linked edges in one cell of the Voronoi), find
     * the edge pointing to the node that is furthest away from the border of the polygon.
     * \param quad_start_edge The first edge of the quad.
     * \return The edge of the quad that is furthest away from the border.
     */
    edge_t* getQuadMaxRedgeTo(edge_t* quad_start_edge);

    /*!
     * Propagate beading information from nodes that are closer to the edge
     * (low radius R) to nodes that are farther from the edge (high R).
     *
     * only propagate from nodes with beading info upward to nodes without beading info
     *
     * Edges are sorted by their radius, so that we can do a depth-first walk
     * without employing a recursive algorithm.
     *
     * In upward propagated beadings we store the distance traveled, so that we can merge these beadings with the downward propagated beadings in \ref propagateBeadingsDownward(.)
     *
     * \param upward_quad_mids all upward halfedges of the inner skeletal edges (not directly connected to the outline) sorted on their highest [distance_to_boundary]. Higher dist first.
     */
    void propagateBeadingsUpward(std::vector<edge_t*>& upward_quad_mids, ptr_vector_t<BeadingPropagation>& node_beadings);

    /*!
     * propagate beading info from higher R nodes to lower R nodes
     *
     * merge with upward propagated beadings if they are encountered
     *
     * don't transfer to nodes which lie on the outline polygon
     *
     * edges are sorted so that we can do a depth-first walk without employing a recursive algorithm
     *
     * \param upward_quad_mids all upward halfedges of the inner skeletal edges (not directly connected to the outline) sorted on their highest [distance_to_boundary]. Higher dist first.
     */
    void propagateBeadingsDownward(std::vector<edge_t*>& upward_quad_mids, ptr_vector_t<BeadingPropagation>& node_beadings);

    /*!
     * Subroutine of \ref propagateBeadingsDownward(std::vector<edge_t*>&, ptr_vector_t<BeadingPropagation>&)
     */
    void propagateBeadingsDownward(edge_t* edge_to_peak, ptr_vector_t<BeadingPropagation>& node_beadings);

    /*!
     * Find a beading in between two other beadings.
     *
     * This creates a new beading. With this we can find the coordinates of the
     * endpoints of the actual line segments to draw.
     *
     * The parameters \p left and \p right are not actually always left or right
     * but just arbitrary directions to visually indicate the difference.
     * \param left One of the beadings to interpolate between.
     * \param ratio_left_to_whole The position within the two beadings to sample
     * an interpolation. Should be a ratio between 0 and 1.
     * \param right One of the beadings to interpolate between.
     * \param switching_radius The bead radius at which we switch from the left
     * beading to the merged beading, if the beadings have a different number of
     * beads.
     * \return The beading at the interpolated location.
     */
    Beading interpolate(const Beading& left, double ratio_left_to_whole, const Beading& right, coord_t switching_radius) const;

    /*!
     * Subroutine of \ref interpolate(const Beading&, Ratio, const Beading&, coord_t)
     *
     * This creates a new Beading between two beadings, assuming that both have
     * the same number of beads.
     * \param left One of the beadings to interpolate between.
     * \param ratio_left_to_whole The position within the two beadings to sample
     * an interpolation. Should be a ratio between 0 and 1.
     * \param right One of the beadings to interpolate between.
     * \return The beading at the interpolated location.
     */
    Beading interpolate(const Beading& left, double ratio_left_to_whole, const Beading& right) const;

    /*!
     * Get the beading at a certain node of the skeletal graph, or create one if
     * it doesn't have one yet.
     *
     * This is a lazy get.
     * \param node The node to get the beading from.
     * \param node_beadings A list of all beadings for nodes.
     * \return The beading of that node.
     */
    std::shared_ptr<BeadingPropagation> getOrCreateBeading(node_t* node, ptr_vector_t<BeadingPropagation>& node_beadings);

    /*!
     * In case we cannot find the beading of a node, get a beading from the
     * nearest node.
     * \param node The node to attempt to get a beading from. The actual node
     * that the returned beading is from may be a different, nearby node.
     * \param max_dist The maximum distance to search for.
     * \return A beading for the node, or ``nullptr`` if there is no node nearby
     * with a beading.
     */
    std::shared_ptr<BeadingPropagation> getNearestBeading(node_t* node, coord_t max_dist);

    /*!
     * generate junctions for each bone
     * \param edge_to_junctions junctions ordered high R to low R
     */
    void generateJunctions(ptr_vector_t<BeadingPropagation>& node_beadings, ptr_vector_t<LineJunctions>& edge_junctions);

    /*!
     * Add a new toolpath segment, defined between two extrusion-juntions.
     *
     * \param from The junction from which to add a segment.
     * \param to The junction to which to add a segment.
     * \param is_odd Whether this segment is an odd gap filler along the middle of the skeleton.
     * \param force_new_path Whether to prevent adding this path to an existing path which ends in \p from
     * \param from_is_3way Whether the \p from junction is a splitting junction where two normal wall lines and a gap filler line come together.
     * \param to_is_3way Whether the \p to junction is a splitting junction where two normal wall lines and a gap filler line come together.
     */
    void addToolpathSegment(const ExtrusionJunction& from, const ExtrusionJunction& to, bool is_odd, bool force_new_path, bool from_is_3way, bool to_is_3way);

    /*!
     * connect junctions in each quad
     */
    void connectJunctions(ptr_vector_t<LineJunctions>& edge_junctions);

    /*!
     * Genrate small segments for local maxima where the beading would only result in a single bead
     */
    void generateLocalMaximaSingleBeads();
};

} // namespace Slic3r::Arachne
#endif // VORONOI_QUADRILATERALIZATION_H
