/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef KDTREE_KDTREE_H
#define KDTREE_KDTREE_H

#include "mcut/internal/cdt/utils.h"
#include "mcut/internal/math.h"

#include <cassert>
#include <limits>

namespace kdt {

struct NodeSplitDirection {
    enum Enum {
        X,
        Y,
    };
};

/// Simple tree structure with alternating half splitting nodes
/// @details Simple tree structure
///          - Tree to incrementally add points to the structure.
///          - Get the nearest point to a given input.
///          - Does not check for duplicates, expect unique points.
/// @tparam TCoordType type used for storing point coordinate.
/// @tparam NumVerticesInLeaf The number of points per leaf.
/// @tparam InitialStackDepth initial size of stack depth for nearest query.
/// Should be at least 1.
/// @tparam StackDepthIncrement increment of stack depth for nearest query when
/// stack depth is reached.
template <
    typename TCoordType,
    size_t NumVerticesInLeaf,
    size_t InitialStackDepth,
    size_t StackDepthIncrement>
class kdtree_t_ {
public:
    typedef TCoordType coord_type;
    typedef vec2_<coord_type> point_type;
    typedef std::pair<point_type, std::uint32_t> value_type;
    typedef std::vector<std::uint32_t> point_data_vec;
    typedef point_data_vec::const_iterator pd_cit;
    typedef std::array<std::uint32_t, 2> children_type;

    /// Stores kd-tree node data
    struct Node {
        children_type children; ///< two children if not leaf; {0,0} if leaf
        point_data_vec data; ///< points' data if leaf
        /// Create empty leaf
        Node()
        {
            setChildren(0, 0);
            data.reserve(NumVerticesInLeaf);
        }
        /// Children setter for convenience
        void setChildren(const std::uint32_t c1, const std::uint32_t c2)
        {
            children[0] = c1;
            children[1] = c2;
        }
        /// Check if node is a leaf (has no valid children)
        bool isLeaf() const
        {
            return children[0] == children[1];
        }
    };

    /// Default constructor
    kdtree_t_()
        : m_rootDir(NodeSplitDirection::X)
        , m_min(point_type::make(
              -std::numeric_limits<coord_type>::max(),
              -std::numeric_limits<coord_type>::max()))
        , m_max(point_type::make(
              std::numeric_limits<coord_type>::max(),
              std::numeric_limits<coord_type>::max()))
        , m_isRootBoxInitialized(false)
        , m_tasksStack(InitialStackDepth, NearestTask())
    {
        m_root = addNewNode();
    }

    /// Constructor with bounding box known in advance
    kdtree_t_(const point_type& min, const point_type& max)
        : m_rootDir(NodeSplitDirection::X)
        , m_min(min)
        , m_max(max)
        , m_isRootBoxInitialized(true)
        , m_tasksStack(InitialStackDepth, NearestTask())
    {
        m_root = addNewNode();
    }

    /// Insert a point into kd-tree
    /// @note external point-buffer is used to reduce kd-tree's memory footprint
    /// @param iPoint index of point in external point-buffer
    /// @param points external point-buffer
    void
    insert(const std::uint32_t& iPoint, const std::vector<point_type>& points)
    {
        // if point is outside root, extend tree by adding new roots
        const point_type& pos = points[iPoint];
        while (!isInsideBox(pos, m_min, m_max)) {
            extendTree(pos);
        }
        // now insert the point into the tree
        std::uint32_t node = m_root;
        point_type min = m_min;
        point_type max = m_max;
        NodeSplitDirection::Enum dir = m_rootDir;

        // below: initialized only to suppress warnings
        NodeSplitDirection::Enum newDir(NodeSplitDirection::X);
        coord_type mid(0);
        point_type newMin, newMax;
        while (true) {
            if (m_nodes[node].isLeaf()) {
                // add point if capacity is not reached
                point_data_vec& pd = m_nodes[node].data;
                if (pd.size() < NumVerticesInLeaf) {
                    pd.push_back(iPoint);
                    return;
                }
                // initialize bbox first time the root capacity is reached
                if (!m_isRootBoxInitialized) {
                    initializeRootBox(points);
                    min = m_min;
                    max = m_max;
                }
                // split a full leaf node
                calcSplitInfo(min, max, dir, mid, newDir, newMin, newMax);
                const std::uint32_t c1 = addNewNode(), c2 = addNewNode();
                Node& n = m_nodes[node];
                n.setChildren(c1, c2);
                point_data_vec& c1data = m_nodes[c1].data;
                point_data_vec& c2data = m_nodes[c2].data;
                // move node's points to children
                for (pd_cit it = n.data.begin(); it != n.data.end(); ++it) {
                    whichChild(points[*it], mid, dir) == 0
                        ? c1data.push_back(*it)
                        : c2data.push_back(*it);
                }
                n.data = point_data_vec();
            } else {
                calcSplitInfo(min, max, dir, mid, newDir, newMin, newMax);
            }
            // add the point to a child
            const std::size_t iChild = whichChild(points[iPoint], mid, dir);
            iChild == 0 ? max = newMax : min = newMin;
            node = m_nodes[node].children[iChild];
            dir = newDir;
        }
    }

    /// Query kd-tree for a nearest neighbor point
    /// @note external point-buffer is used to reduce kd-tree's memory footprint
    /// @param point query point position
    /// @param points external point-buffer
    value_type nearest(
        const point_type& point,
        const std::vector<point_type>& points) const
    {
        value_type out;
        int iTask = -1;
        coord_type minDistSq = std::numeric_limits<coord_type>::max();
        m_tasksStack[++iTask] = NearestTask(m_root, m_min, m_max, m_rootDir, minDistSq);
        while (iTask != -1) {
            const NearestTask t = m_tasksStack[iTask--];
            if (t.distSq > minDistSq)
                continue;
            const Node& n = m_nodes[t.node];
            if (n.isLeaf()) {
                for (pd_cit it = n.data.begin(); it != n.data.end(); ++it) {
                    const point_type& p = points[*it];
                    const coord_type distSq = cdt::get_square_distance(point, p);
                    if (distSq < minDistSq) {
                        minDistSq = distSq;
                        out.first = p;
                        out.second = *it;
                    }
                }
            } else {
                coord_type mid(0);
                NodeSplitDirection::Enum newDir;
                point_type newMin, newMax;
                calcSplitInfo(t.min, t.max, t.dir, mid, newDir, newMin, newMax);

                const coord_type distToMid = t.dir == NodeSplitDirection::X
                    ? (point.x() - mid)
                    : (point.y() - mid);
                const coord_type toMidSq = distToMid * distToMid;

                const std::size_t iChild = whichChild(point, mid, t.dir);
                if (iTask + 2 >= static_cast<int>(m_tasksStack.size())) {
                    m_tasksStack.resize(
                        m_tasksStack.size() + StackDepthIncrement);
                }
                // node containing point should end up on top of the stack
                if (iChild == 0) {
                    m_tasksStack[++iTask] = NearestTask(
                        n.children[1], newMin, t.max, newDir, toMidSq);
                    m_tasksStack[++iTask] = NearestTask(
                        n.children[0], t.min, newMax, newDir, toMidSq);
                } else {
                    m_tasksStack[++iTask] = NearestTask(
                        n.children[0], t.min, newMax, newDir, toMidSq);
                    m_tasksStack[++iTask] = NearestTask(
                        n.children[1], newMin, t.max, newDir, toMidSq);
                }
            }
        }
        return out;
    }

private:
    /// Add a new node and return it's index in nodes buffer
    std::uint32_t addNewNode()
    {
        const std::uint32_t newNodeIndex = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.push_back(Node());
        return newNodeIndex;
    }

    /// Test which child point belongs to after the split
    /// @returns 0 if first child, 1 if second child
    std::size_t whichChild(
        const point_type& point,
        const coord_type& split,
        const NodeSplitDirection::Enum dir) const
    {
        return static_cast<size_t>(
            dir == NodeSplitDirection::X ? point.x() > split : point.y() > split);
    }

    /// Calculate split location, direction, and children boxes
    static void calcSplitInfo(
        const point_type& min,
        const point_type& max,
        const NodeSplitDirection::Enum dir,
        coord_type& midOut,
        NodeSplitDirection::Enum& newDirOut,
        point_type& newMinOut,
        point_type& newMaxOut)
    {
        newMaxOut = max;
        newMinOut = min;
        switch (dir) {
        case NodeSplitDirection::X:
            midOut = (min.x() + max.x()) / coord_type(2);
            newDirOut = NodeSplitDirection::Y;
            newMinOut.x() = midOut;
            newMaxOut.x() = midOut;
            return;
        case NodeSplitDirection::Y:
            midOut = (min.y() + max.y()) / coord_type(2);
            newDirOut = NodeSplitDirection::X;
            newMinOut.y() = midOut;
            newMaxOut.y() = midOut;
            return;
        }
    }

    /// Test if point is inside a box
    static bool isInsideBox(
        const point_type& p,
        const point_type& min,
        const point_type& max)
    {
        return p.x() >= min.x() && p.x() <= max.x() && p.y() >= min.y() && p.y() <= max.y();
    }

    /// Extend a tree by creating new root with old root and a new node as
    /// children
    void extendTree(const point_type& point)
    {
        const std::uint32_t newRoot = addNewNode();
        const std::uint32_t newLeaf = addNewNode();
        switch (m_rootDir) {
        case NodeSplitDirection::X:
            m_rootDir = NodeSplitDirection::Y;
            point.y() < m_min.y() ? m_nodes[newRoot].setChildren(newLeaf, m_root)
                                  : m_nodes[newRoot].setChildren(m_root, newLeaf);
            if (point.y() < m_min.y())
                m_min.y() -= m_max.y() - m_min.y();
            else if (point.y() > m_max.y())
                m_max.y() += m_max.y() - m_min.y();
            break;
        case NodeSplitDirection::Y:
            m_rootDir = NodeSplitDirection::X;
            point.x() < m_min.x() ? m_nodes[newRoot].setChildren(newLeaf, m_root)
                                  : m_nodes[newRoot].setChildren(m_root, newLeaf);
            if (point.x() < m_min.x())
                m_min.x() -= m_max.x() - m_min.x();
            else if (point.x() > m_max.x())
                m_max.x() += m_max.x() - m_min.x();
            break;
        }
        m_root = newRoot;
    }

    /// Calculate root's box enclosing all root points
    void initializeRootBox(const std::vector<point_type>& points)
    {
        const point_data_vec& data = m_nodes[m_root].data;
        m_min = points[data.front()];
        m_max = m_min;
        for (pd_cit it = data.begin(); it != data.end(); ++it) {
            const point_type& p = points[*it];
            m_min = point_type::make(
                std::min(m_min.x(), p.x()), std::min(m_min.y(), p.y()));
            m_max = point_type::make(
                std::max(m_max.x(), p.x()), std::max(m_max.y(), p.y()));
        }
        // Make sure bounding box does not have a zero size by adding padding:
        // zero-size bounding box cannot be extended properly
        const TCoordType padding(1);
        if (m_min.x() == m_max.x()) {
            m_min.x() -= padding;
            m_max.x() += padding;
        }
        if (m_min.y() == m_max.y()) {
            m_min.y() -= padding;
            m_max.y() += padding;
        }
        m_isRootBoxInitialized = true;
    }

private:
    std::uint32_t m_root;
    std::vector<Node> m_nodes;
    NodeSplitDirection::Enum m_rootDir;
    point_type m_min;
    point_type m_max;
    bool m_isRootBoxInitialized;

    // used for nearest query
    struct NearestTask {
        std::uint32_t node;
        point_type min, max;
        NodeSplitDirection::Enum dir;
        coord_type distSq;
        NearestTask()
        {
        }
        NearestTask(
            const std::uint32_t node,
            const point_type& min,
            const point_type& max,
            const NodeSplitDirection::Enum dir,
            const coord_type distSq)
            : node(node)
            , min(min)
            , max(max)
            , dir(dir)
            , distSq(distSq)
        {
        }
    };
    // allocated in class (not in the 'nearest' method) for better performance
    mutable std::vector<NearestTask> m_tasksStack;
};

} // namespace kdt

namespace cdt {

/// KD-tree holding points
template <
    typename TCoordType,
    size_t NumVerticesInLeaf = 32,
    size_t InitialStackDepth = 32,
    size_t StackDepthIncrement = 32>
class locator_kdtree_t {
public:
    /// Initialize KD-tree with points
    void initialize(const std::vector<vec2_<TCoordType>>& points)
    {
        vec2_<TCoordType> min = points.front();
        vec2_<TCoordType> max = min;

        for (typename std::vector<vec2_<TCoordType>>::const_iterator it = points.begin();
             it != points.end();
             ++it) {

            min = vec2_<TCoordType>::make(std::min(min.x(), it->x()), std::min(min.y(), it->y()));
            max = vec2_<TCoordType>::make(std::max(max.x(), it->x()), std::max(max.y(), it->y()));

        }

        m_tree = kdtree_t(min, max);

        for (std::uint32_t i = 0; i < (std::uint32_t)points.size(); ++i) {
            m_tree.insert(i, points);
        }
    }
    /// Add point to KD-tree
    void add_point(const std::uint32_t i, const std::vector<vec2_<TCoordType>>& points)
    {
        m_tree.insert(i, points);
    }

    /// Find nearest point using R-tree
    std::uint32_t nearPoint(
        const vec2_<TCoordType>& pos,
        const std::vector<vec2_<TCoordType>>& points) const
    {
        return m_tree.nearest(pos, points).second;
    }

private:
    typedef kdt::kdtree_t_<TCoordType, NumVerticesInLeaf, InitialStackDepth, StackDepthIncrement> kdtree_t;

    kdtree_t m_tree;
};

} // namespace cdt

#endif // header guard
