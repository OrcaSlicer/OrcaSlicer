#include "OuterWallNodes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r {

namespace {

// Two outer walls are connected when a stretch of the lower one at least this long lies right below the upper one.
const double connected_length = scale_(0.01);

struct Contour
{
    const ExtrusionEntity *entity = nullptr;
    Points                 pts;
    // Width of the extrusion at each point, scaled.
    std::vector<double>    widths;
    bool                   is_loop = false;
    BoundingBox            bbox;
};

void append_path(const ExtrusionPath &path, Contour &contour)
{
    for (const Point3 &pt : path.polyline.points) {
        const Point p = pt.to_point();
        // The paths of a loop share their end points.
        if (! contour.pts.empty() && contour.pts.back() == p)
            continue;
        contour.pts.emplace_back(p);
        contour.widths.emplace_back(scale_(std::max(double(path.width), 0.2)));
    }
}

void collect_contours(const ExtrusionEntity &entity, std::vector<Contour> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_contours(*child, out);
        return;
    }
    if (entity.role() != erExternalPerimeter)
        return;

    Contour contour;
    contour.entity = &entity;
    if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            append_path(path, contour);
        contour.is_loop = true;
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            append_path(path, contour);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        append_path(*path, contour);
    }
    if (contour.pts.size() < 2)
        return;
    contour.bbox = get_extents(contour.pts);
    // The walls of two layers are never exactly on top of each other.
    contour.bbox.offset(*std::max_element(contour.widths.begin(), contour.widths.end()));
    out.emplace_back(std::move(contour));
}

// Signed distance of the points of the lower contour from the polyline of the upper one.
class ContourDistance
{
public:
    explicit ContourDistance(const Contour &contour)
    {
        const size_t n = contour.pts.size();
        const size_t segments = contour.is_loop ? n : n - 1;
        m_lines.reserve(segments);
        for (size_t i = 0; i < segments; ++ i)
            m_lines.emplace_back(contour.pts[i].cast<double>(), contour.pts[(i + 1) % n].cast<double>());
        m_tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(m_lines);
    }

    double distance(const Point &pt) const
    {
        size_t hit_idx = 0;
        Vec2d  hit_pt  = Vec2d::Zero();
        const double sqr = AABBTreeLines::squared_distance_to_indexed_lines(m_lines, m_tree, pt.cast<double>(), hit_idx, hit_pt);
        return sqr < 0. ? std::numeric_limits<double>::max() : std::sqrt(sqr);
    }

private:
    std::vector<Linef>                m_lines;
    AABBTreeIndirect::Tree<2, double> m_tree;
};

// True if a stretch of the lower contour at least connected_length long lies within its own extrusion width of the upper contour.
bool is_connected(const ContourDistance &upper, const Contour &lower)
{
    const size_t n = lower.pts.size();
    const size_t segments = lower.is_loop ? n : n - 1;
    double run = 0.;
    // Walk a loop twice, so that a stretch around the start point is found as well.
    const size_t steps = lower.is_loop ? 2 * segments : segments;
    for (size_t step = 0; step < steps; ++ step) {
        const size_t i = step % segments;
        const size_t j = (i + 1) % n;
        if (upper.distance(lower.pts[i]) < lower.widths[i] && upper.distance(lower.pts[j]) < lower.widths[j]) {
            run += (lower.pts[j] - lower.pts[i]).cast<double>().norm();
            if (run >= connected_length)
                return true;
        } else
            run = 0.;
    }
    return false;
}

size_t find_root(std::vector<size_t> &parent, size_t i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

} // namespace

OuterWallNodes OuterWallNodes::build(const Print &print)
{
    OuterWallNodes result;
    int            next_id = 0;

    for (const PrintObject *object : print.objects()) {
        // Contours of every layer.
        std::vector<std::vector<Contour>> layers;
        layers.reserve(object->layers().size());
        for (const Layer *layer : object->layers()) {
            std::vector<Contour> contours;
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->perimeters.entities)
                    collect_contours(*entity, contours);
            layers.emplace_back(std::move(contours));
        }

        // Index of the first contour of every layer in the union-find structure.
        std::vector<size_t> first(layers.size() + 1, 0);
        for (size_t i = 0; i < layers.size(); ++ i)
            first[i + 1] = first[i] + layers[i].size();
        std::vector<size_t> parent(first.back());
        std::iota(parent.begin(), parent.end(), size_t(0));

        for (size_t layer_idx = 1; layer_idx < layers.size(); ++ layer_idx) {
            for (size_t upper_idx = 0; upper_idx < layers[layer_idx].size(); ++ upper_idx) {
                const Contour &upper = layers[layer_idx][upper_idx];
                ContourDistance distance(upper);
                for (size_t lower_idx = 0; lower_idx < layers[layer_idx - 1].size(); ++ lower_idx) {
                    const Contour &lower = layers[layer_idx - 1][lower_idx];
                    if (! upper.bbox.overlap(lower.bbox) || ! is_connected(distance, lower))
                        continue;
                    const size_t a = find_root(parent, first[layer_idx] + upper_idx);
                    const size_t b = find_root(parent, first[layer_idx - 1] + lower_idx);
                    parent[std::max(a, b)] = std::min(a, b);
                }
            }
        }

        // Renumber the chains, the ids are unique over all the objects.
        std::vector<int> id_of_root(parent.size(), -1);
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++ layer_idx)
            for (size_t i = 0; i < layers[layer_idx].size(); ++ i) {
                const size_t root = find_root(parent, first[layer_idx] + i);
                if (id_of_root[root] < 0)
                    id_of_root[root] = next_id ++;
                result.m_node_of_entity[layers[layer_idx][i].entity] = id_of_root[root];
            }
    }
    return result;
}

} // namespace Slic3r
