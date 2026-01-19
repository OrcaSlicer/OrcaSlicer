// KD tree built upon external data set, referencing the external data by integer indices.

#ifndef slic3r_KDTreeIndirect_hpp_
#define slic3r_KDTreeIndirect_hpp_

#include <algorithm>
#include <limits>
#include <vector>

#include "Utils.hpp" // for next_highest_power_of_2()

namespace Slic3r {

enum class VisitorReturnMask : unsigned int {
    CONTINUE_LEFT  = 1,
    CONTINUE_RIGHT = 2,
    STOP           = 4,
};

// KD tree for N-dimensional closest point search.
template<size_t ANumDimensions, typename ACoordType, typename ACoordinateFn>
class KDTreeIndirect
{
public:
    static constexpr size_t NumDimensions = ANumDimensions;
    using					CoordinateFn  = ACoordinateFn;
    using					CoordType     = ACoordType;
    // Following could be static constexpr size_t, but that would not link in C++11
    enum : size_t {
        npos = size_t(-1)
    };

    KDTreeIndirect(CoordinateFn coordinate) : coordinate(coordinate) {}
    KDTreeIndirect(CoordinateFn coordinate, std::vector<size_t> indices) : coordinate(coordinate) { this->build(indices); }
    KDTreeIndirect(CoordinateFn coordinate, size_t num_indices) : coordinate(coordinate) { this->build(num_indices); }
    KDTreeIndirect(KDTreeIndirect &&rhs) : m_nodes(std::move(rhs.m_nodes)), coordinate(std::move(rhs.coordinate)) {}
    KDTreeIndirect& operator=(KDTreeIndirect &&rhs) { m_nodes = std::move(rhs.m_nodes); coordinate = std::move(rhs.coordinate); return *this; }
    void clear() { m_nodes.clear(); }

    void build(size_t num_indices)
    {
        std::vector<size_t> indices;
        indices.reserve(num_indices);
        for (size_t i = 0; i < num_indices; ++ i)
            indices.emplace_back(i);
        this->build(indices);
    }

    void build(std::vector<size_t> &indices)
    {
        if (indices.empty())
            clear();
        else {
            // Allocate enough memory for a full binary tree.
            m_nodes.assign(next_highest_power_of_2(indices.size() + 1), npos);
            build_recursive(indices, 0, 0, 0, indices.size() - 1);
        }
        indices.clear();
    }

    template<typename CoordType>
    unsigned int descent_mask(const CoordType &point_coord, const CoordType &search_radius, size_t idx, size_t dimension) const
    {
        CoordType dist = point_coord - this->coordinate(idx, dimension);
        return (dist * dist < search_radius + CoordType(EPSILON)) ?
                                                                    // The plane intersects a hypersphere centered at point_coord of search_radius.
                   ((unsigned int)(VisitorReturnMask::CONTINUE_LEFT) | (unsigned int)(VisitorReturnMask::CONTINUE_RIGHT)) :
                   // The plane does not intersect the hypersphere.
                   (dist > CoordType(0)) ? (unsigned int)(VisitorReturnMask::CONTINUE_RIGHT) : (unsigned int)(VisitorReturnMask::CONTINUE_LEFT);
    }

       // Visitor is supposed to return a bit mask of VisitorReturnMask.
    template<typename Visitor>
    void visit(Visitor &visitor) const
    {
        visit_recursive(0, 0, visitor);
    }

    CoordinateFn coordinate;

private:
    // Build a balanced tree by splitting the input sequence by an axis aligned plane at a dimension.
    void build_recursive(std::vector<size_t> &input, size_t node, const size_t dimension, const size_t left, const size_t right)
    {
        if (left > right)
            return;

        assert(node < m_nodes.size());

        if (left == right) {
            // Insert a node into the balanced tree.
            m_nodes[node] = input[left];
            return;
        }

           // Partition the input to left / right pieces of the same length to produce a balanced tree.
        size_t center = (left + right) / 2;
        partition_input(input, dimension, left, right, center);
        // Insert a node into the tree.
        m_nodes[node] = input[center];
        // Build up the left / right subtrees.
        size_t next_dimension = dimension;
        if (++ next_dimension == NumDimensions)
            next_dimension = 0;
        if (center > left)
            build_recursive(input, node * 2 + 1, next_dimension, left, center - 1);
        build_recursive(input, node * 2 + 2, next_dimension, center + 1, right);
    }

       // Partition the input m_nodes <left, right> at "k" and "dimension" using the QuickSelect method:
       // https://en.wikipedia.org/wiki/Quickselect
       // Items left of the k'th item are lower than the k'th item in the "dimension",
       // items right of the k'th item are higher than the k'th item in the "dimension",
    void partition_input(std::vector<size_t> &input, const size_t dimension, size_t left, size_t right, const size_t k) const
    {
        while (left < right) {
            size_t center = (left + right) / 2;
            CoordType pivot;
            {
                // Bubble sort the input[left], input[center], input[right], so that a median of the three values
                // will end up in input[center].
                CoordType left_value   = this->coordinate(input[left],   dimension);
                CoordType center_value = this->coordinate(input[center], dimension);
                CoordType right_value  = this->coordinate(input[right],  dimension);
                if (left_value > center_value) {
                    std::swap(input[left], input[center]);
                    std::swap(left_value,  center_value);
                }
                if (left_value > right_value) {
                    std::swap(input[left], input[right]);
                    right_value = left_value;
                }
                if (center_value > right_value) {
                    std::swap(input[center], input[right]);
                    center_value = right_value;
                }
                pivot = center_value;
            }
            if (right <= left + 2)
                // The <left, right> interval is already sorted.
                break;
            size_t i = left;
            size_t j = right - 1;
            std::swap(input[center], input[j]);
            // Partition the set based on the pivot.
            for (;;) {
                // Skip left points that are already at correct positions.
                // Search will certainly stop at position (right - 1), which stores the pivot.
                while (this->coordinate(input[++ i], dimension) < pivot) ;
                // Skip right points that are already at correct positions.
                while (this->coordinate(input[-- j], dimension) > pivot && i < j) ;
                if (i >= j)
                    break;
                std::swap(input[i], input[j]);
            }
            // Restore pivot to the center of the sequence.
            std::swap(input[i], input[right - 1]);
            // Which side the kth element is in?
            if (k < i)
                right = i - 1;
            else if (k == i)
                // Sequence is partitioned, kth element is at its place.
                break;
            else
                left = i + 1;
        }
    }

    template<typename Visitor>
    void visit_recursive(size_t node, size_t dimension, Visitor &visitor) const
    {
        assert(! m_nodes.empty());
        if (node >= m_nodes.size() || m_nodes[node] == npos)
            return;

           // Left / right child node index.
        size_t left  = node * 2 + 1;
        size_t right = left + 1;
        unsigned int mask = visitor(m_nodes[node], dimension);
        if ((mask & (unsigned int)VisitorReturnMask::STOP) == 0) {
            size_t next_dimension = (++ dimension == NumDimensions) ? 0 : dimension;
            if (mask & (unsigned int)VisitorReturnMask::CONTINUE_LEFT)
                visit_recursive(left,  next_dimension, visitor);
            if (mask & (unsigned int)VisitorReturnMask::CONTINUE_RIGHT)
                visit_recursive(right, next_dimension, visitor);
        }
    }

    std::vector<size_t> m_nodes;
};

// Find a closest point using Euclidian metrics.
// Returns npos if not found.
template<size_t K,
         typename PointType,
         typename FilterFn,
         size_t D,
         typename CoordT,
         typename CoordFn>
std::array<size_t, K> find_closest_points(
    const KDTreeIndirect<D, CoordT, CoordFn> &kdtree,
    const PointType                          &point,
    FilterFn                                  filter)
{
    using Tree = KDTreeIndirect<D, CoordT, CoordFn>;

    struct Visitor
    {
        const Tree      &kdtree;
        const PointType &point;
        const FilterFn   filter;

        std::array<std::pair<size_t, CoordT>, K> results;

        Visitor(const Tree &kdtree, const PointType &point, FilterFn filter)
            : kdtree(kdtree), point(point), filter(filter)
        {
            results.fill(std::make_pair(Tree::npos,
                                        std::numeric_limits<CoordT>::max()));
        }
        unsigned int operator()(size_t idx, size_t dimension)
        {
            if (this->filter(idx)) {
                auto dist = CoordT(0);
                for (size_t i = 0; i < D; ++i) {
                    CoordT d = point[i] - kdtree.coordinate(idx, i);
                    dist += d * d;
                }

                auto res = std::make_pair(idx, dist);
                auto it  = std::lower_bound(results.begin(), results.end(),
                                            res, [](auto &r1, auto &r2) {
                                               return r1.second < r2.second;
                                            });

                if (it != results.end()) {
                    std::rotate(it, std::prev(results.end()), results.end());
                    *it = res;
                }
            }
            return kdtree.descent_mask(point[dimension],
                                       results.front().second, idx,
                                       dimension);
        }
    } visitor(kdtree, point, filter);

    kdtree.visit(visitor);
    std::array<size_t, K> ret;
    for (size_t i = 0; i < K; i++) ret[i] = visitor.results[i].first;

    return ret;
}

template<size_t K, typename PointType, size_t D, typename CoordT, typename CoordFn>
std::array<size_t, K> find_closest_points(
    const KDTreeIndirect<D, CoordT, CoordFn> &kdtree, const PointType &point)
{
    return find_closest_points<K>(kdtree, point, [](size_t) { return true; });
}

template<typename PointType,
         typename FilterFn,
         size_t D,
         typename CoordT,
         typename CoordFn>
size_t find_closest_point(const KDTreeIndirect<D, CoordT, CoordFn> &kdtree,
                          const PointType                          &point,
                          FilterFn                                  filter)
{
    return find_closest_points<1>(kdtree, point, filter)[0];
}

template<typename KDTreeIndirectType, typename PointType>
size_t find_closest_point(const KDTreeIndirectType& kdtree, const PointType& point)
{
    return find_closest_point(kdtree, point, [](size_t) { return true; });
}

// Find nearby points (spherical neighbourhood) using Euclidian metrics.
template<typename KDTreeIndirectType, typename PointType, typename FilterFn>
std::vector<size_t> find_nearby_points(const KDTreeIndirectType &kdtree, const PointType &center,
                                       const typename KDTreeIndirectType::CoordType& max_distance, FilterFn filter)
{
    using CoordType = typename KDTreeIndirectType::CoordType;

    struct Visitor {
        const KDTreeIndirectType &kdtree;
        const PointType center;
        const CoordType max_distance_squared;
        const FilterFn filter;
        std::vector<size_t> result;

        Visitor(const KDTreeIndirectType &kdtree, const PointType& center, const CoordType &max_distance,
                FilterFn filter) :
            kdtree(kdtree), center(center), max_distance_squared(max_distance*max_distance), filter(filter) {
        }
        unsigned int operator()(size_t idx, size_t dimension) {
            if (this->filter(idx)) {
                auto dist = CoordType(0);
                for (size_t i = 0; i < KDTreeIndirectType::NumDimensions; ++i) {
                    CoordType d = center[i] - kdtree.coordinate(idx, i);
                    dist += d * d;
                }
                if (dist < max_distance_squared) {
                    result.push_back(idx);
                }
            }
            return kdtree.descent_mask(center[dimension], max_distance_squared, idx, dimension);
        }
    } visitor(kdtree, center, max_distance, filter);

    kdtree.visit(visitor);
    return visitor.result;
}

template<typename KDTreeIndirectType, typename PointType>
std::vector<size_t> find_nearby_points(const KDTreeIndirectType &kdtree, const PointType &center,
                                       const typename KDTreeIndirectType::CoordType& max_distance)
{
    return find_nearby_points(kdtree, center, max_distance, [](size_t) {
        return true;
    });
}

// Find nearby points (spherical neighbourhood) using Euclidian metrics.
template<typename KDTreeIndirectType, typename PointType, typename FilterFn>
std::vector<size_t> find_nearby_points(const KDTreeIndirectType &kdtree,
                                       const PointType          &bb_min,
                                       const PointType          &bb_max,
                                       FilterFn                  filter)
{
    struct Visitor {
        const KDTreeIndirectType &kdtree;
        const PointType &bb_min, &bb_max;
        const FilterFn filter;
        std::vector<size_t> result;

        Visitor(const KDTreeIndirectType &kdtree, const PointType& bbmin, const PointType& bbmax,
                FilterFn filter) :
            kdtree(kdtree), bb_min{bbmin}, bb_max{bbmax}, filter(filter) {
        }
        unsigned int operator()(size_t idx, size_t dimension) {
            unsigned int ret =
                static_cast<unsigned int>(VisitorReturnMask::CONTINUE_LEFT) |
                static_cast<unsigned int>(VisitorReturnMask::CONTINUE_RIGHT);

            if (this->filter(idx)) {
                PointType p;
                bool contains = true;
                for (size_t i = 0; i < KDTreeIndirectType::NumDimensions; ++i) {
                    p(i) = kdtree.coordinate(idx, i);
                    contains = contains && bb_min(i) <= p(i) && p(i) <= bb_max(i);
                }

                if (p(dimension) < bb_min(dimension))
                    ret = static_cast<unsigned int>(VisitorReturnMask::CONTINUE_RIGHT);
                if (p(dimension) > bb_max(dimension))
                    ret = static_cast<unsigned int>(VisitorReturnMask::CONTINUE_LEFT);

                if (contains)
                    result.emplace_back(idx);
            }

            return ret;
        }
    } visitor(kdtree, bb_min, bb_max, filter);

    kdtree.visit(visitor);
    return visitor.result;
}

} // namespace Slic3r

#endif /* slic3r_KDTreeIndirect_hpp_ */
