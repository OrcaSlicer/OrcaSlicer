//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef LIGHTNING_TREE_NODE_H
#define LIGHTNING_TREE_NODE_H

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "../../EdgeGrid.hpp"
#include "../../Polygon.hpp"
#include "SVG.hpp"

//#define LIGHTNING_TREE_NODE_DEBUG_OUTPUT

namespace Slic3r::FillLightning
{

inline coord_t locator_cell_size() { return scaled<coord_t>(4.); }

class Node;

using NodeSPtr = std::shared_ptr<Node>;

// NOTE: As written, this struct will only be valid for a single layer, will have to be updated for the next.
// NOTE: Reasons for implementing this with some separate closures:
//       - keep clear deliniation during development
//       - possibility of multiple distance field strategies

/*!
 * A single vertex of a Lightning Tree, the structure that determines the paths
 * to be printed to form Lightning Infill.
 *
 * In essence these vertices are just a position linked to other positions in
 * 2D. The nodes have a hierarchical structure of parents and children, forming
 * a tree. The class also has some helper functions specific to Lightning Infill
 * e.g. to straighten the paths around this node.
 */
class Node : public std::enable_shared_from_this<Node>
{
public:
    // Workaround for private/protected constructors and 'make_shared': https://stackoverflow.com/a/27832765
    template<typename ...Arg> NodeSPtr static create(Arg&&...arg)
    {
        struct EnableMakeShared : public Node
        {
            explicit EnableMakeShared(Arg&&...arg) : Node(std::forward<Arg>(arg)...) {}
        };
        return std::make_shared<EnableMakeShared>(std::forward<Arg>(arg)...);
    }

    /*!
     * Get the position on this layer that this node represents, a vertex of the
     * path to print.
     * \return The position that this node represents.
     */
    const Point& getLocation() const { return m_p; }

    /*!
     * Change the position on this layer that the node represents.
     * \param p The position that the node needs to represent.
     */
    void setLocation(const Point& p) { m_p = p; }

    /*!
     * Construct a new ``Node`` instance and add it as a child of
     * this node.
     * \param p The location of the new node.
     * \return A shared pointer to the new node.
     */
    NodeSPtr addChild(const Point& p);

    /*!
     * Add an existing ``Node`` as a child of this node.
     * \param new_child The node that must be added as a child.
     * \return Always returns \p new_child.
     */
    NodeSPtr addChild(NodeSPtr& new_child);

    /*!
     * Propagate this node's sub-tree to the next layer.
     *
     * Creates a copy of this tree, realign it to the new layer boundaries
     * \p next_outlines and reduce (i.e. prune and straighten) it. A copy of
     * this node and all of its descendant nodes will be added to the
     * \p next_trees vector.
     * \param next_trees A collection of tree nodes to use for the next layer.
     * \param next_outlines The shape of the layer below, to make sure that the
     * tree stays within the bounds of the infill area.
     * \param prune_distance The maximum distance that a leaf node may be moved
     * such that it still supports the current node.
     * \param smooth_magnitude The maximum distance that a line may be shifted
     * to straighten the tree's paths, such that it still supports the current
     * paths.
     * \param max_remove_colinear_dist The maximum distance of a line-segment
     * from which straightening may remove a colinear point.
     */
    void propagateToNextLayer
    (
        std::vector<NodeSPtr>& next_trees,
        const Polygons& next_outlines,
        const EdgeGrid::Grid& outline_locator,
        coord_t prune_distance,
        coord_t smooth_magnitude,
        coord_t max_remove_colinear_dist
    ) const;

    /*!
     * Executes a given function for every line segment in this node's sub-tree.
     *
     * The function takes two `Point` arguments. These arguments will be filled
     * in with the higher-order node (closer to the root) first, and the
     * downtree node (closer to the leaves) as the second argument. The segment
     * from this node's parent to this node itself is not included.
     * The order in which the segments are visited is depth-first.
     * \param visitor A function to execute for every branch in the node's sub-
     * tree.
     */
    void visitBranches(const std::function<void(const Point&, const Point&)>& visitor) const;

    /*!
     * Execute a given function for every node in this node's sub-tree.
     *
     * The visitor function takes a node as input. This node is not const, so
     * this can be used to change the tree.
     * Nodes are visited in depth-first order. This node itself is visited as
     * well (pre-order).
     * \param visitor A function to execute for every node in this node's sub-
     * tree.
     */
    void visitNodes(const std::function<void(NodeSPtr)>& visitor);

    /*!
     * Get a weighted distance from an unsupported point to this node (given the current supporting radius).
     *
     * When attaching a unsupported location to a node, not all nodes have the same priority.
     * (Eucludian) closer nodes are prioritised, but that's not the whole story.
     * For instance, we give some nodes a 'valence boost' depending on the nr. of branches.
     * \param unsupported_location The (unsuppported) location of which the weighted distance needs to be calculated.
     * \param supporting_radius The maximum distance which can be bridged without (infill) supporting it.
     * \return The weighted distance.
     */
    coord_t getWeightedDistance(const Point& unsupported_location, const coord_t& supporting_radius) const;

    /*!
     * Returns whether this node is the root of a lightning tree. It is the root
     * if it has no parents.
     * \return ``true`` if this node is the root (no parents) or ``false`` if it
     * is a child node of some other node.
     */
    bool isRoot() const { return m_is_root; }

    /*!
     * Reverse the parent-child relationship all the way to the root, from this node onward.
     * This has the effect of 're-rooting' the tree at the current node if no immediate parent is given as argument.
     * That is, the current node will become the root, it's (former) parent if any, will become one of it's children.
     * This is then recursively bubbled up until it reaches the (former) root, which then will become a leaf.
     * \param new_parent The (new) parent-node of the root, useful for recursing or immediately attaching the node to another tree.
     */
    void reroot(const NodeSPtr &new_parent = nullptr);

    /*!
     * Retrieves the closest node to the specified location.
     * \param loc The specified location.
     * \result The branch that starts at the position closest to the location within this tree.
     */
    NodeSPtr closestNode(const Point& loc);

    /*!
     * Returns whether the given tree node is a descendant of this node.
     *
     * If this node itself is given, it is also considered to be a descendant.
     * \param to_be_checked A node to find out whether it is a descendant of
     * this node.
     * \return ``true`` if the given node is a descendant or this node itself,
     * or ``false`` if it is not in the sub-tree.
     */
    bool hasOffspring(const NodeSPtr& to_be_checked) const;

    Node() = delete; // Don't allow empty contruction

protected:
    /*!
     * Construct a new node, either for insertion in a tree or as root.
     * \param p The physical location in the 2D layer that this node represents.
     * Connecting other nodes to this node indicates that a line segment should
     * be drawn between those two physical positions.
     */
    explicit Node(const Point& p, const std::optional<Point>& last_grounding_location = std::nullopt);

    /*!
     * Copy this node and its entire sub-tree.
     * \return The equivalent of this node in the copy (the root of the new sub-
     * tree).
     */
    NodeSPtr deepCopy() const;

    /*! Reconnect trees from the layer above to the new outlines of the lower layer.
     * \return Wether or not the root is kept (false is no, true is yes).
     */
    bool realign(const Polygons& outlines, const EdgeGrid::Grid& outline_locator, std::vector<NodeSPtr>& rerooted_parts);

    struct RectilinearJunction
    {
        coord_t total_recti_dist; //!< rectilinear distance along the tree from the last junction above to the junction below
        Point junction_loc; //!< junction location below
    };

    /*!
     * Smoothen the tree to make it a bit more printable, while still supporting
     * the trees above.
     * \param magnitude The maximum allowed distance to move the node.
     * \param max_remove_colinear_dist Maximum distance of the (compound) line-segment from which a co-linear point may be removed.
     */
    void straighten(coord_t magnitude, coord_t max_remove_colinear_dist);

    /*! Recursive part of \ref straighten(.)
     * \param junction_above The last seen junction with multiple children above
     * \param accumulated_dist The distance along the tree from the last seen junction to this node
     * \param max_remove_colinear_dist2 Maximum distance _squared_ of the (compound) line-segment from which a co-linear point may be removed.
     * \return the total distance along the tree from the last junction above to the first next junction below and the location of the next junction below
     */
    RectilinearJunction straighten(coord_t magnitude, const Point& junction_above, coord_t accumulated_dist, int64_t max_remove_colinear_dist2);

    /*! Prune the tree from the extremeties (leaf-nodes) until the pruning distance is reached.
     * \return The distance that has been pruned. If less than \p distance, then the whole tree was puned away.
     */
    coord_t prune(const coord_t& distance);

public:
    /*!
     * Convert the tree into polylines
     * 
     * At each junction one line is chosen at random to continue
     * 
     * The lines start at a leaf and end in a junction
     * 
     * \param output all branches in this tree connected into polylines
     */
    void convertToPolylines(Polylines &output, coord_t line_overlap) const;

    /*! If this was ever a direct child of the root, it'll have a previous grounding location.
     *
     * This needs to be known when roots are reconnected, so that the last (higher) layer is supported by the next one.
     */
    const std::optional<Point>& getLastGroundingLocation() const { return m_last_grounding_location; }

    void draw_tree(SVG& svg) { for (auto& child : m_children) { svg.draw(Line(m_p, child->getLocation()), "yellow"); child->draw_tree(svg); } }

protected:
    /*!
     * Convert the tree into polylines
     * 
     * At each junction one line is chosen at random to continue
     * 
     * The lines start at a leaf and end in a junction
     * 
     * \param long_line a reference to a polyline in \p output which to continue building on in the recursion
     * \param output all branches in this tree connected into polylines
     */
    void convertToPolylines(size_t long_line_idx, Polylines &output) const;

    void removeJunctionOverlap(Polylines &polylines, coord_t line_overlap) const;

    bool m_is_root;
    Point m_p;
    std::weak_ptr<Node> m_parent;
    std::vector<NodeSPtr> m_children;

    std::optional<Point> m_last_grounding_location;  //<! The last known grounding location, see 'getLastGroundingLocation()'.

    friend BoundingBox get_extents(const NodeSPtr &root_node);
    friend BoundingBox get_extents(const std::vector<NodeSPtr> &tree_roots);

#ifdef LIGHTNING_TREE_NODE_DEBUG_OUTPUT
    friend void export_to_svg(const NodeSPtr &root_node, Slic3r::SVG &svg);
    friend void export_to_svg(const std::string &path, const Polygons &contour, const std::vector<NodeSPtr> &root_nodes);
#endif /* LIGHTNING_TREE_NODE_DEBUG_OUTPUT */
};

bool inside(const Polygons &polygons, const Point &p);
bool lineSegmentPolygonsIntersection(const Point& a, const Point& b, const EdgeGrid::Grid& outline_locator, Point& result, coord_t within_max_dist);

inline BoundingBox get_extents(const NodeSPtr &root_node)
{
    BoundingBox bbox;
    for (const NodeSPtr &children : root_node->m_children)
        bbox.merge(get_extents(children));
    bbox.merge(root_node->getLocation());
    return bbox;
}

inline BoundingBox get_extents(const std::vector<NodeSPtr> &tree_roots)
{
    BoundingBox bbox;
    for (const NodeSPtr &root_node : tree_roots)
        bbox.merge(get_extents(root_node));
    return bbox;
}

#ifdef LIGHTNING_TREE_NODE_DEBUG_OUTPUT
void export_to_svg(const NodeSPtr &root_node, SVG &svg);
void export_to_svg(const std::string &path, const Polygons &contour, const std::vector<NodeSPtr> &root_nodes);
#endif /* LIGHTNING_TREE_NODE_DEBUG_OUTPUT */

} // namespace Slic3r::FillLightning

#endif // LIGHTNING_TREE_NODE_H
