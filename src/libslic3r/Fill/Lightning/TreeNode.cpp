//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "TreeNode.hpp"

#include "../../Geometry.hpp"

namespace Slic3r::FillLightning {

coord_t Node::getWeightedDistance(const Point& unsupported_location, const coord_t& supporting_radius) const
{
    constexpr coord_t min_valence_for_boost = 0;
    constexpr coord_t max_valence_for_boost = 4;
    constexpr coord_t valence_boost_multiplier = 4;

    const size_t valence = (!m_is_root) + m_children.size();
    const coord_t valence_boost = (min_valence_for_boost < valence && valence < max_valence_for_boost) ? valence_boost_multiplier * supporting_radius : 0;
    const auto dist_here = coord_t((getLocation() - unsupported_location).cast<double>().norm());
    return dist_here - valence_boost;
}

bool Node::hasOffspring(const NodeSPtr& to_be_checked) const
{
    if (to_be_checked == shared_from_this())
        return true;

    for (auto& child_ptr : m_children)
        if (child_ptr->hasOffspring(to_be_checked)) 
            return true;

    return false;
}

NodeSPtr Node::addChild(const Point& child_loc)
{
    assert(m_p != child_loc);
    NodeSPtr child = Node::create(child_loc);
    return addChild(child);
}

NodeSPtr Node::addChild(NodeSPtr& new_child)
{
    assert(new_child != shared_from_this());
    //assert(p != new_child->p); // NOTE: No problem for now. Issue to solve later. Maybe even afetr final. Low prio.
    m_children.push_back(new_child);
    new_child->m_parent = shared_from_this();
    new_child->m_is_root = false;
    return new_child;
}

void Node::propagateToNextLayer(
    std::vector<NodeSPtr>& next_trees,
    const Polygons& next_outlines,
    const EdgeGrid::Grid& outline_locator,
    const coord_t prune_distance,
    const coord_t smooth_magnitude,
    const coord_t max_remove_colinear_dist) const
{
    auto tree_below = deepCopy();
    tree_below->prune(prune_distance);
    tree_below->straighten(smooth_magnitude, max_remove_colinear_dist);
    if (tree_below->realign(next_outlines, outline_locator, next_trees))
        next_trees.push_back(tree_below);
}

// NOTE: Depth-first, as currently implemented.
//       Skips the root (because that has no root itself), but all initial nodes will have the root point anyway.
void Node::visitBranches(const std::function<void(const Point&, const Point&)>& visitor) const
{
    for (const auto& node : m_children) {
        assert(node->m_parent.lock() == shared_from_this());
        visitor(m_p, node->m_p);
        node->visitBranches(visitor);
    }
}

// NOTE: Depth-first, as currently implemented.
void Node::visitNodes(const std::function<void(NodeSPtr)>& visitor)
{
    visitor(shared_from_this());
    for (const auto& node : m_children) {
        assert(node->m_parent.lock() == shared_from_this());
        node->visitNodes(visitor);
    }
}

Node::Node(const Point& p, const std::optional<Point>& last_grounding_location /*= std::nullopt*/) : 
    m_is_root(true), m_p(p), m_last_grounding_location(last_grounding_location)
{}

NodeSPtr Node::deepCopy() const
{
    NodeSPtr local_root = Node::create(m_p);
    local_root->m_is_root = m_is_root;
    if (m_is_root)
    {
        local_root->m_last_grounding_location = m_last_grounding_location.value_or(m_p);
    }
    local_root->m_children.reserve(m_children.size());
    for (const auto& node : m_children)
    {
        NodeSPtr child = node->deepCopy();
        child->m_parent = local_root;
        local_root->m_children.push_back(child);
    }
    return local_root;
}

void Node::reroot(const NodeSPtr &new_parent)
{
    if (! m_is_root) {
        auto old_parent = m_parent.lock();
        old_parent->reroot(shared_from_this());
        m_children.push_back(old_parent);
    }

    if (new_parent) {
        m_children.erase(std::remove(m_children.begin(), m_children.end(), new_parent), m_children.end());
        m_is_root = false;
        m_parent = new_parent;
    } else {
        m_is_root = true;
        m_parent.reset();
    }
}

NodeSPtr Node::closestNode(const Point& loc)
{
    NodeSPtr result = shared_from_this();
    auto closest_dist2 = coord_t((m_p - loc).cast<double>().norm());

    for (const auto& child : m_children) {
        NodeSPtr candidate_node = child->closestNode(loc);
        const auto child_dist2 = coord_t((candidate_node->m_p - loc).cast<double>().norm());
        if (child_dist2 < closest_dist2) {
            closest_dist2 = child_dist2;
            result = candidate_node;
        }
    }

    return result;
}

bool inside(const Polygons &polygons, const Point &p)
{
    int poly_count_inside = 0;
    for (const Polygon &poly : polygons) {
        const int is_inside_this_poly = ClipperLib::PointInPolygon(p, poly.points);
        if (is_inside_this_poly == -1)
            return true;
        poly_count_inside += is_inside_this_poly;
    }
    return (poly_count_inside % 2) == 1;
}

bool lineSegmentPolygonsIntersection(const Point& a, const Point& b, const EdgeGrid::Grid& outline_locator, Point& result, const coord_t within_max_dist)
{
    struct Visitor {
        bool operator()(coord_t iy, coord_t ix) {
            // Called with a row and colum of the grid cell, which is intersected by a line.
            auto cell_data_range = grid.cell_data_range(iy, ix);
            for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
                // End points of the line segment and their vector.
                auto segment = grid.segment(*it_contour_and_segment);
                if (Vec2d ip; Geometry::segment_segment_intersection(segment.first.cast<double>(), segment.second.cast<double>(), this->line_a, this->line_b, ip))
                    if (double d = (this->intersection_pt - this->line_b).squaredNorm(); d < d2min) {
                        this->d2min = d;
                        this->intersection_pt = ip;
                    }
            }
            // Continue traversing the grid along the edge.
            return true;
        }

        const EdgeGrid::Grid& grid;
        Vec2d                 line_a;
        Vec2d                 line_b;
        Vec2d                 intersection_pt;
        double                d2min { std::numeric_limits<double>::max() };
    } visitor { outline_locator, a.cast<double>(), b.cast<double>() };

    outline_locator.visit_cells_intersecting_line(a, b, visitor);
    if (visitor.d2min < double(within_max_dist) * double(within_max_dist)) {
        result = Point(visitor.intersection_pt);
        return true;
    }
    return false;
}

bool Node::realign(const Polygons& outlines, const EdgeGrid::Grid& outline_locator, std::vector<NodeSPtr>& rerooted_parts)
{
    if (outlines.empty())
        return false;

    if (inside(outlines, m_p)) {
        // Only keep children that have an unbroken connection to here, realign will put the rest in rerooted parts due to recursion:
        Point coll;
        bool reground_me = false;
        m_children.erase(std::remove_if(m_children.begin(), m_children.end(), [&](const NodeSPtr &child) {
            bool connect_branch = child->realign(outlines, outline_locator, rerooted_parts);
            // Find an intersection of the line segment from p to child->p, at maximum outline_locator.resolution() * 2 distance from p.
            if (connect_branch && lineSegmentPolygonsIntersection(child->m_p, m_p, outline_locator, coll, outline_locator.resolution() * 2)) {
                child->m_last_grounding_location.reset();
                child->m_parent.reset();
                child->m_is_root = true;
                rerooted_parts.push_back(child);
                reground_me = true;
                connect_branch = false;
            }
            return ! connect_branch;
        }), m_children.end());
        if (reground_me)
            m_last_grounding_location.reset();
        return true;
    }

    // 'Lift' any decendants out of this tree:
    for (auto& child : m_children)
        if (child->realign(outlines, outline_locator, rerooted_parts)) {
            child->m_last_grounding_location = m_p;
            child->m_parent.reset();
            child->m_is_root = true;
            rerooted_parts.push_back(child);
        }

    m_children.clear();
    return false;
}

void Node::straighten(const coord_t magnitude, const coord_t max_remove_colinear_dist)
{
    straighten(magnitude, m_p, 0, int64_t(max_remove_colinear_dist) * int64_t(max_remove_colinear_dist));
}

Node::RectilinearJunction Node::straighten(
    const coord_t magnitude,
    const Point& junction_above,
    const coord_t accumulated_dist,
    const int64_t max_remove_colinear_dist2)
{
    constexpr coord_t junction_magnitude_factor_numerator = 3;
    constexpr coord_t junction_magnitude_factor_denominator = 4;

    const coord_t junction_magnitude = magnitude * junction_magnitude_factor_numerator / junction_magnitude_factor_denominator;
    if (m_children.size() == 1)
    {
        auto child_p = m_children.front();
        auto child_dist = coord_t((m_p - child_p->m_p).cast<double>().norm());
        RectilinearJunction junction_below = child_p->straighten(magnitude, junction_above, accumulated_dist + child_dist, max_remove_colinear_dist2);
        coord_t total_dist_to_junction_below = junction_below.total_recti_dist;
        const Point& a = junction_above;
        Point        b = junction_below.junction_loc;
        if (a != b) // should always be true!
        {
            Point ab = b - a;
            Point destination = (a.cast<int64_t>() + ab.cast<int64_t>() * int64_t(accumulated_dist) / std::max(int64_t(1), int64_t(total_dist_to_junction_below))).cast<coord_t>();
            if ((destination - m_p).cast<int64_t>().squaredNorm() <= int64_t(magnitude) * int64_t(magnitude))
                m_p = destination;
            else
                m_p += ((destination - m_p).cast<double>().normalized() * magnitude).cast<coord_t>();
        }
        { // remove nodes on linear segments
            constexpr coord_t close_enough = 10;

            child_p = m_children.front(); //recursive call to straighten might have removed the child
            const NodeSPtr& parent_node = m_parent.lock();
            if (parent_node &&
                (child_p->m_p - parent_node->m_p).cast<int64_t>().squaredNorm() < max_remove_colinear_dist2 &&
                Line::distance_to_squared(m_p, parent_node->m_p, child_p->m_p) < close_enough * close_enough) {
                child_p->m_parent = m_parent;
                for (auto& sibling : parent_node->m_children)
                { // find this node among siblings
                    if (sibling == shared_from_this())
                    {
                        sibling = child_p; // replace this node by child
                        break;
                    }
                }
            }
        }
        return junction_below;
    }
    else
    {
        constexpr coord_t weight = 1000;
        Point junction_moving_dir = ((junction_above - m_p).cast<double>().normalized() * weight).cast<coord_t>();
        bool prevent_junction_moving = false;
        for (auto& child_p : m_children)
        {
            const auto child_dist = coord_t((m_p - child_p->m_p).cast<double>().norm());
            RectilinearJunction below = child_p->straighten(magnitude, m_p, child_dist, max_remove_colinear_dist2);

            junction_moving_dir += ((below.junction_loc - m_p).cast<double>().normalized() * weight).cast<coord_t>();
            if (below.total_recti_dist < magnitude) // TODO: make configurable?
            {
                prevent_junction_moving = true; // prevent flipflopping in branches due to straightening and junctoin moving clashing
            }
        }
        if (junction_moving_dir != Point(0, 0) && ! m_children.empty() && ! m_is_root && ! prevent_junction_moving)
        {
            auto junction_moving_dir_len = coord_t(junction_moving_dir.norm());
            if (junction_moving_dir_len > junction_magnitude)
            {
                junction_moving_dir = junction_moving_dir * junction_magnitude / junction_moving_dir_len;
            }
            m_p += junction_moving_dir;
        }
        return RectilinearJunction{ accumulated_dist, m_p };
    }
}

// Prune the tree from the extremeties (leaf-nodes) until the pruning distance is reached.
coord_t Node::prune(const coord_t& pruning_distance)
{
    if (pruning_distance <= 0)
        return 0;

    coord_t max_distance_pruned = 0;
    for (auto child_it = m_children.begin(); child_it != m_children.end(); ) {
        auto& child = *child_it;
        coord_t dist_pruned_child = child->prune(pruning_distance);
        if (dist_pruned_child >= pruning_distance)
        { // pruning is finished for child; dont modify further
            max_distance_pruned = std::max(max_distance_pruned, dist_pruned_child);
            ++child_it;
        } else {
            const Point a = getLocation();
            const Point b = child->getLocation();
            const Point ba = a - b;
            const auto ab_len = coord_t(ba.cast<double>().norm());
            if (dist_pruned_child + ab_len <= pruning_distance) { 
                // we're still in the process of pruning
                assert(child->m_children.empty() && "when pruning away a node all it's children must already have been pruned away");
                max_distance_pruned = std::max(max_distance_pruned, dist_pruned_child + ab_len);
                child_it = m_children.erase(child_it);
            } else {
                // pruning stops in between this node and the child
                const Point n = b + (ba.cast<double>().normalized() * (pruning_distance - dist_pruned_child)).cast<coord_t>();
                assert(std::abs((n - b).cast<double>().norm() + dist_pruned_child - pruning_distance) < 10 && "total pruned distance must be equal to the pruning_distance");
                max_distance_pruned = std::max(max_distance_pruned, pruning_distance);
                child->setLocation(n);
                ++child_it;
            }
        }
    }

    return max_distance_pruned;
}

void Node::convertToPolylines(Polylines &output, const coord_t line_overlap) const
{
    Polylines result;
    result.emplace_back();
    convertToPolylines(0, result);
    removeJunctionOverlap(result, line_overlap);
    append(output, std::move(result));
}

void Node::convertToPolylines(size_t long_line_idx, Polylines &output) const
{
    if (m_children.empty()) {
        output[long_line_idx].points.push_back(m_p);
        return;
    }
    size_t first_child_idx = rand() % m_children.size();
    m_children[first_child_idx]->convertToPolylines(long_line_idx, output);
    output[long_line_idx].points.push_back(m_p);

    for (size_t idx_offset = 1; idx_offset < m_children.size(); idx_offset++) {
        size_t child_idx = (first_child_idx + idx_offset) % m_children.size();
        const Node& child = *m_children[child_idx];
        output.emplace_back();
        size_t child_line_idx = output.size() - 1;
        child.convertToPolylines(child_line_idx, output);
        output[child_line_idx].points.emplace_back(m_p);
    }
}

void Node::removeJunctionOverlap(Polylines &result_lines, const coord_t line_overlap) const
{
    const coord_t reduction    = line_overlap;
    size_t        res_line_idx = 0;
    while (res_line_idx < result_lines.size()) {
        Polyline &polyline = result_lines[res_line_idx];
        if (polyline.size() <= 1) {
            polyline = std::move(result_lines.back());
            result_lines.pop_back();
            continue;
        }

        coord_t to_be_reduced = reduction;
        Point a = polyline.back();
        for (int point_idx = int(polyline.size()) - 2; point_idx >= 0; point_idx--) {
            const Point b = polyline.points[point_idx];
            const Point ab = b - a;
            const auto ab_len = coord_t(ab.cast<double>().norm());
            if (ab_len >= to_be_reduced) {
                polyline.points.back() = a + (ab.cast<double>() * (double(to_be_reduced) / ab_len)).cast<coord_t>();
                break;
            } else {
                to_be_reduced -= ab_len;
                polyline.points.pop_back();
            }
            a = b;
        }

        if (polyline.size() <= 1) {
            polyline = std::move(result_lines.back());
            result_lines.pop_back();
        } else
            ++ res_line_idx;
    }
}

#ifdef LIGHTNING_TREE_NODE_DEBUG_OUTPUT
void export_to_svg(const NodeSPtr &root_node, SVG &svg)
{
    for (const NodeSPtr &children : root_node->m_children) {
        svg.draw(Line(root_node->getLocation(), children->getLocation()), "red");
        export_to_svg(children, svg);
    }
}

void export_to_svg(const std::string &path, const Polygons &contour, const std::vector<NodeSPtr> &root_nodes) {
    BoundingBox bbox = get_extents(contour);

    bbox.offset(SCALED_EPSILON);
    SVG svg(path, bbox);
    svg.draw_outline(contour, "blue");

    for (const NodeSPtr &root_node: root_nodes) {
        for (const NodeSPtr &children: root_node->m_children) {
            svg.draw(Line(root_node->getLocation(), children->getLocation()), "red");
            export_to_svg(children, svg);
        }
    }
}
#endif /* LIGHTNING_TREE_NODE_DEBUG_OUTPUT */

} // namespace Slic3r::FillLightning
