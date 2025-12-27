#include "JumpPointSearch.hpp"
#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Point.hpp"
#include "libslic3r/AStar.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <oneapi/tbb/scalable_allocator.h>

//#define DEBUG_FILES
#ifdef DEBUG_FILES
#include "libslic3r/SVG.hpp"
#endif

namespace Slic3r {

// execute fn for each pixel on the line. If fn returns false, terminate the iteration
template<typename PointFn> void dda(coord_t x0, coord_t y0, coord_t x1, coord_t y1, const PointFn &fn)
{
    coord_t dx    = abs(x1 - x0);
    coord_t dy    = abs(y1 - y0);
    coord_t x     = x0;
    coord_t y     = y0;
    coord_t n     = 1 + dx + dy;
    coord_t x_inc = (x1 > x0) ? 1 : -1;
    coord_t y_inc = (y1 > y0) ? 1 : -1;
    coord_t error = dx - dy;
    dx *= 2;
    dy *= 2;

    for (; n > 0; --n) {
        if (!fn(x, y)) return;

        if (error > 0) {
            x += x_inc;
            error -= dy;
        } else {
            y += y_inc;
            error += dx;
        }
    }
}

// will draw the line twice, second time with and offset of 1 in the direction of normal
// may call the fn on the same coordiantes multiple times!
template<typename PointFn> void double_dda_with_offset(coord_t x0, coord_t y0, coord_t x1, coord_t y1, const PointFn &fn)
{
    Vec2d normal       = Point{y1 - y0, x1 - x0}.cast<double>().normalized();
    normal.x()         = ceil(normal.x());
    normal.y()         = ceil(normal.y());
    Point start_offset = Point(x0, y0) + (normal).cast<coord_t>();
    Point end_offset   = Point(x1, y1) + (normal).cast<coord_t>();

    dda(x0, y0, x1, y1, fn);
    dda(start_offset.x(), start_offset.y(), end_offset.x(), end_offset.y(), fn);
}

template<typename CellPositionType, typename CellQueryFn> class JPSTracer
{
public:
    // Use incoming_dir [0,0] for starting points, so that all directions are checked from that point
    struct Node
    {
        CellPositionType position;
        CellPositionType incoming_dir;
    };

    JPSTracer(CellPositionType target, CellQueryFn is_passable) : target(target), is_passable(is_passable) {}

private:
    CellPositionType target;
    CellQueryFn      is_passable; // should return boolean whether the cell is passable or not

    CellPositionType find_jump_point(CellPositionType start, CellPositionType forward_dir) const
    {
        CellPositionType next = start + forward_dir;
        while (next != target && is_passable(next) && !(is_jump_point(next, forward_dir))) { next = next + forward_dir; }

        if (is_passable(next)) {
            return next;
        } else {
            return start;
        }
    }

    bool is_jump_point(CellPositionType pos, CellPositionType forward_dir) const
    {
        if (abs(forward_dir.x()) + abs(forward_dir.y()) == 2) {
            // diagonal
            CellPositionType horizontal_check_dir = CellPositionType{forward_dir.x(), 0};
            CellPositionType vertical_check_dir   = CellPositionType{0, forward_dir.y()};

            if (!is_passable(pos - horizontal_check_dir) && is_passable(pos + forward_dir - 2 * horizontal_check_dir)) { return true; }

            if (!is_passable(pos - vertical_check_dir) && is_passable(pos + forward_dir - 2 * vertical_check_dir)) { return true; }

            if (find_jump_point(pos, horizontal_check_dir) != pos) { return true; }

            if (find_jump_point(pos, vertical_check_dir) != pos) { return true; }

            return false;
        } else { // horizontal or vertical
            CellPositionType side_dir = CellPositionType(forward_dir.y(), forward_dir.x());

            if (!is_passable(pos + side_dir) && is_passable(pos + forward_dir + side_dir)) { return true; }

            if (!is_passable(pos - side_dir) && is_passable(pos + forward_dir - side_dir)) { return true; }

            return false;
        }
    }

public:
    template<class Fn> void foreach_reachable(const Node &from, Fn &&fn) const
    {
        const CellPositionType &      pos         = from.position;
        const CellPositionType &      forward_dir = from.incoming_dir;
        std::vector<CellPositionType> dirs_to_check{};

        if (abs(forward_dir.x()) + abs(forward_dir.y()) == 0) { // special case for starting point
            dirs_to_check = all_directions;
        } else if (abs(forward_dir.x()) + abs(forward_dir.y()) == 2) {
            // diagonal
            CellPositionType horizontal_check_dir = CellPositionType{forward_dir.x(), 0};
            CellPositionType vertical_check_dir   = CellPositionType{0, forward_dir.y()};

            if (!is_passable(pos - horizontal_check_dir) && is_passable(pos + forward_dir - 2 * horizontal_check_dir)) {
                dirs_to_check.push_back(forward_dir - 2 * horizontal_check_dir);
            }

            if (!is_passable(pos - vertical_check_dir) && is_passable(pos + forward_dir - 2 * vertical_check_dir)) {
                dirs_to_check.push_back(forward_dir - 2 * vertical_check_dir);
            }

            dirs_to_check.push_back(horizontal_check_dir);
            dirs_to_check.push_back(vertical_check_dir);
            dirs_to_check.push_back(forward_dir);

        } else { // horizontal or vertical
            CellPositionType side_dir = CellPositionType(forward_dir.y(), forward_dir.x());

            if (!is_passable(pos + side_dir) && is_passable(pos + forward_dir + side_dir)) { dirs_to_check.push_back(forward_dir + side_dir); }

            if (!is_passable(pos - side_dir) && is_passable(pos + forward_dir - side_dir)) { dirs_to_check.push_back(forward_dir - side_dir); }
            dirs_to_check.push_back(forward_dir);
        }

        for (const CellPositionType &dir : dirs_to_check) {
            CellPositionType jp = find_jump_point(pos, dir);
            if (jp != pos) fn(Node{jp, dir});
        }
    }

    float distance(Node a, Node b) const { return (a.position - b.position).template cast<double>().norm(); }

    float goal_heuristic(Node n) const { return n.position == target ? -1.f : (target - n.position).template cast<double>().norm(); }

    size_t unique_id(Node n) const { return (static_cast<size_t>(uint16_t(n.position.x())) << 16) + static_cast<size_t>(uint16_t(n.position.y())); }

    const std::vector<CellPositionType> all_directions{{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
};

void JPSPathFinder::clear()
{
    inpassable.clear();
    max_search_box.max = Pixel(std::numeric_limits<coord_t>::min(), std::numeric_limits<coord_t>::min());
    max_search_box.min = Pixel(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max());
    add_obstacles(bed_shape);
}

void JPSPathFinder::add_obstacles(const Lines &obstacles)
{
    auto store_obstacle = [&](coord_t x, coord_t y) {
        max_search_box.max.x() = std::max(max_search_box.max.x(), x);
        max_search_box.max.y() = std::max(max_search_box.max.y(), y);
        max_search_box.min.x() = std::min(max_search_box.min.x(), x);
        max_search_box.min.y() = std::min(max_search_box.min.y(), y);
        inpassable.insert(Pixel{x, y});
        return true;
    };

    for (const Line &l : obstacles) {
        Pixel start = pixelize(l.a);
        Pixel end   = pixelize(l.b);
        double_dda_with_offset(start.x(), start.y(), end.x(), end.y(), store_obstacle);
    }
}

Polyline JPSPathFinder::find_path(const Point &p0, const Point &p1)
{
    Pixel start = pixelize(p0);
    Pixel end   = pixelize(p1);
    if (inpassable.empty() || (start - end).cast<float>().norm() < 3.0) { return Polyline{p0, p1}; }

    if (inpassable.find(start) != inpassable.end()) {
        dda(start.x(), start.y(), end.x(), end.y(), [&](coord_t x, coord_t y) {
            if (inpassable.find(Pixel(x, y)) == inpassable.end() || start == end) { // new start not found yet, and xy passable
                start = Pixel(x, y);
                return false;
            }
            return true;
        });
    }

    if (inpassable.find(end) != inpassable.end()) {
        dda(end.x(), end.y(), start.x(), start.y(), [&](coord_t x, coord_t y) {
            if (inpassable.find(Pixel(x, y)) == inpassable.end() || start == end) { // new start not found yet, and xy passable
                end = Pixel(x, y);
                return false;
            }
            return true;
        });
    }

    BoundingBox search_box = max_search_box;
    search_box.max -= Pixel(1, 1);
    search_box.min += Pixel(1, 1);

    BoundingBox bounding_square(Points{start, end});
    bounding_square.max += Pixel(5, 5);
    bounding_square.min -= Pixel(5, 5);
    coord_t bounding_square_size = 2 * std::max(bounding_square.size().x(), bounding_square.size().y());
    bounding_square.max.x() += (bounding_square_size - bounding_square.size().x()) / 2;
    bounding_square.min.x() -= (bounding_square_size - bounding_square.size().x()) / 2;
    bounding_square.max.y() += (bounding_square_size - bounding_square.size().y()) / 2;
    bounding_square.min.y() -= (bounding_square_size - bounding_square.size().y()) / 2;

    // Intersection - limit the search box to a square area around the start and end, to fasten the path searching
    search_box.max = search_box.max.cwiseMin(bounding_square.max);
    search_box.min = search_box.min.cwiseMax(bounding_square.min);

    auto cell_query = [&](Pixel pixel) { return search_box.contains(pixel) && (pixel == start || pixel == end || inpassable.find(pixel) == inpassable.end()); };

    JPSTracer<Pixel, decltype(cell_query)> tracer(end, cell_query);
    using QNode = astar::QNode<JPSTracer<Pixel, decltype(cell_query)>>;

    std::unordered_map<size_t, QNode>          astar_cache{};
    std::vector<Pixel, PointsAllocator<Pixel>> out_path;
    std::vector<decltype(tracer)::Node>        out_nodes;

    if (!astar::search_route(tracer, {start, {0, 0}}, std::back_inserter(out_nodes), astar_cache)) {
        // path not found - just reconstruct the best path from astar cache.
        // Note that astar_cache is NOT empty - at least the starting point should always be there
        auto                coordiante_func = [&astar_cache](size_t idx, size_t dim) { return float(astar_cache[idx].node.position[dim]); };
        std::vector<size_t> keys;
        keys.reserve(astar_cache.size());
        for (const auto &pair : astar_cache) { keys.push_back(pair.first); }
        KDTreeIndirect<2, float, decltype(coordiante_func)> kd_tree(coordiante_func, keys);
        size_t                                              closest_qnode = find_closest_point(kd_tree, end.cast<float>());

        out_path.push_back(end);
        while (closest_qnode != astar::Unassigned) {
            out_path.push_back(astar_cache[closest_qnode].node.position);
            closest_qnode = astar_cache[closest_qnode].parent;
        }
    } else {
        for (const auto &node : out_nodes) { out_path.push_back(node.position); }
        out_path.push_back(start);
    }

#ifdef DEBUG_FILES
    auto scaled_points = [](const Points &ps) {
        Points r;
        for (const Point &p : ps) { r.push_back(Point::new_scale(p.x(), p.y())); }
        return r;
    };
    auto          scaled_point = [](const Point &p) { return Point::new_scale(p.x(), p.y()); };
    ::Slic3r::SVG svg(debug_out_path(("path_jps" + std::to_string(print_z) + "_" + std::to_string(rand() % 1000)).c_str()).c_str(),
                      BoundingBox(scaled_point(search_box.min), scaled_point(search_box.max)));
    for (const auto &p : inpassable) { svg.draw(scaled_point(p), "black", scale_(0.4)); }
    for (const auto &qn : astar_cache) { svg.draw(scaled_point(qn.second.node.position), "blue", scale_(0.3)); }
    svg.draw(Polyline(scaled_points(out_path)), "yellow", scale_(0.25));
    svg.draw(scaled_point(end), "purple", scale_(0.4));
    svg.draw(scaled_point(start), "green", scale_(0.4));
#endif

    std::vector<Pixel, PointsAllocator<Pixel>> tmp_path;
    tmp_path.reserve(out_path.size());
    // Some path found, reverse and remove points that do not change direction
    std::reverse(out_path.begin(), out_path.end());
    {
        tmp_path.push_back(out_path.front()); // first point
        for (size_t i = 1; i < out_path.size() - 1; i++) {
            if ((out_path[i] - out_path[i - 1]).cast<float>().normalized() != (out_path[i + 1] - out_path[i]).cast<float>().normalized()) { tmp_path.push_back(out_path[i]); }
        }
        tmp_path.push_back(out_path.back()); // last_point
        out_path = tmp_path;
    }

#ifdef DEBUG_FILES
    svg.draw(Polyline(scaled_points(out_path)), "orange", scale_(0.20));
#endif

    tmp_path.clear();
    // remove redundant jump points - there are points that change direction but are not needed - this inefficiency arises from the
    // usage of grid search The removal alg tries to find the longest Px Px+k path without obstacles. If Px Px+k+1 is blocked, it will
    // insert the Px+k point to result and continue search from Px+k
    {
        tmp_path.push_back(out_path.front()); // first point
        size_t index_of_last_stored_point = 0;
        for (size_t i = 1; i < out_path.size(); i++) {
            if (i - index_of_last_stored_point < 2) continue;
            bool passable       = true;
            auto store_obstacle = [&](coord_t x, coord_t y) {
                if (Pixel(x, y) != start && Pixel(x, y) != end && inpassable.find(Pixel(x, y)) != inpassable.end()) {
                    passable = false;
                    return false;
                }
                return true;
            };
            dda(tmp_path.back().x(), tmp_path.back().y(), out_path[i].x(), out_path[i].y(), store_obstacle);
            if (!passable) {
                tmp_path.push_back(out_path[i - 1]);
                index_of_last_stored_point = i - 1;
            }
        }
        tmp_path.push_back(out_path.back()); // last_point
        out_path = tmp_path;
    }

#ifdef DEBUG_FILES
    svg.draw(Polyline(scaled_points(out_path)), "red", scale_(0.15));
    svg.Close();
#endif

    // before returing the path, transform it from pixels back to points.
    // Also replace the first and last pixel by input points so that result path patches input params exactly.
    for (Pixel &p : out_path) { p = unpixelize(p); }
    out_path.front() = p0;
    out_path.back()  = p1;

    return Polyline(out_path);
}

} // namespace Slic3r
