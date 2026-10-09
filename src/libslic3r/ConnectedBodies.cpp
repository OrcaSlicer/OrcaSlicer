#include "ConnectedBodies.hpp"

#include "AABBTreeIndirect.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

std::vector<std::vector<size_t>> connected_bodies(const std::vector<const ExPolygons *> &layers, size_t &count,
                                                  const std::function<void()> &throw_if_canceled)
{
    // Union-find over the islands of all layers, numbered layer after layer.
    std::vector<size_t> first(layers.size() + 1, 0);
    for (size_t l = 0; l < layers.size(); ++l)
        first[l + 1] = first[l] + layers[l]->size();
    std::vector<size_t> parent(first.back());
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    const auto find = [&parent](size_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };

    std::vector<std::vector<BoundingBox>> boxes(layers.size());
    for (size_t l = 0; l < layers.size(); ++l)
        for (const ExPolygon &island : *layers[l])
            boxes[l].emplace_back(get_extents(island));
    for (size_t l = 0; l + 1 < layers.size(); ++l) {
        if (throw_if_canceled)
            throw_if_canceled();
        // Index the smaller of the two layers, so that a fragmented layer is not scanned island by island.
        size_t a_layer = l;
        size_t b_layer = l + 1;
        if (layers[a_layer]->size() < layers[b_layer]->size())
            std::swap(a_layer, b_layer);
        if (layers[b_layer]->empty())
            continue;
        using IslandTree = AABBTreeIndirect::Tree<2, coord_t>;
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> wrappers;
        wrappers.reserve(boxes[b_layer].size());
        for (size_t b = 0; b < boxes[b_layer].size(); ++b)
            wrappers.emplace_back(b, boxes[b_layer][b]);
        IslandTree tree;
        tree.build_modify_input(wrappers);
        for (size_t a = 0; a < boxes[a_layer].size(); ++a) {
            const IslandTree::BoundingBox query(boxes[a_layer][a].min, boxes[a_layer][a].max);
            AABBTreeIndirect::traverse(
                tree, [&query](const IslandTree::Node &node) { return node.bbox.intersects(query); },
                [&](const IslandTree::Node &node) {
                    // The tree's boxes are widened by an epsilon, and islands already joined need no clipping.
                    const size_t b = node.idx;
                    if (boxes[a_layer][a].overlap(boxes[b_layer][b]) && find(first[a_layer] + a) != find(first[b_layer] + b) &&
                        !intersection_ex((*layers[a_layer])[a], (*layers[b_layer])[b]).empty())
                        parent[find(first[a_layer] + a)] = find(first[b_layer] + b);
                    return true;
                });
        }
    }

    std::vector<size_t>              body(parent.size(), std::numeric_limits<size_t>::max());
    std::vector<std::vector<size_t>> out(layers.size());
    count = 0;
    for (size_t l = 0; l < layers.size(); ++l)
        for (size_t i = 0; i < layers[l]->size(); ++i) {
            size_t &b = body[find(first[l] + i)];
            if (b == std::numeric_limits<size_t>::max())
                b = count++;
            out[l].emplace_back(b);
        }
    return out;
}

IslandLocator::IslandLocator(const ExPolygons &islands, coord_t margin) : m_islands(&islands), m_alone(islands.size(), true)
{
    m_boxes.reserve(islands.size());
    for (const ExPolygon &island : islands)
        m_boxes.emplace_back(get_extents(island).inflated(margin));
    // Sweep the boxes along x, so that only those reaching each other are compared.
    std::vector<size_t> order(m_boxes.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [this](size_t l, size_t r) { return m_boxes[l].min.x() < m_boxes[r].min.x(); });
    for (size_t a = 0; a < order.size(); ++a)
        for (size_t b = a + 1; b < order.size() && m_boxes[order[b]].min.x() <= m_boxes[order[a]].max.x(); ++b)
            if (m_boxes[order[a]].overlap(m_boxes[order[b]]))
                m_alone[order[a]] = m_alone[order[b]] = false;
}

bool IslandLocator::holds(size_t island, const Point &point, bool strict) const
{
    return m_boxes[island].contains(point) && ((m_alone[island] && !strict) || (*m_islands)[island].contains(point));
}

std::pair<int, double> IslandLocator::find(const Point &point, bool strict) const
{
    int    nearest  = -1;
    double distance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < m_boxes.size(); ++i)
        if (m_boxes[i].contains(point)) {
            if ((m_alone[i] && !strict) || (*m_islands)[i].contains(point))
                return { int(i), 0. };
            if (const double d = ((*m_islands)[i].point_projection(point) - point).cast<double>().squaredNorm(); d < distance) {
                distance = d;
                nearest  = int(i);
            }
        }
    return { nearest, distance };
}

// The area of polygons and their first and second moments of area, which holes, running clockwise, subtract.
struct AreaMoments
{
    double area{ 0. };
    Vec2d  first{ Vec2d::Zero() };
    // Of x^2, y^2 and xy.
    Vec3d second{ Vec3d::Zero() };

    void add(const Polygon &polygon)
    {
        if (polygon.points.size() < 3)
            return;
        Vec2d p1 = unscaled(polygon.points.back());
        for (const Point &point : polygon.points) {
            const Vec2d  p2 = unscaled(point);
            const double a  = cross2(p1, p2);
            area += a / 2.;
            first += a / 6. * (p1 + p2);
            second += a / 12. *
                      Vec3d(p1.x() * p1.x() + p1.x() * p2.x() + p2.x() * p2.x(), p1.y() * p1.y() + p1.y() * p2.y() + p2.y() * p2.y(),
                            p1.x() * p1.y() + p2.x() * p2.y() + 0.5 * (p1.x() * p2.y() + p2.x() * p1.y()));
            p1 = p2;
        }
    }
};

// Mass, volume and the first and second moments of mass about the origin.
struct Moments
{
    double   mass{ 0. };
    double   volume{ 0. };
    Vec3d    first{ Vec3d::Zero() };
    Matrix3d second{ Matrix3d::Zero() };

    void add(const Moments &other)
    {
        mass += other.mass;
        volume += other.volume;
        first += other.first;
        second += other.second;
    }
};

BoundingBoxf3 SolidBody::bounding_box(const Transform3d &trafo) const
{
    BoundingBoxf3 box;
    for (const Point &point : hull.points)
        for (const double z : { z_min, z_max })
            box.merge(trafo * Vec3d(unscaled(point.x()), unscaled(point.y()), z));
    return box;
}

std::vector<SolidBody> solid_bodies(const std::vector<MeshInPlace> &solids, const std::vector<double> &densities,
                                    const std::vector<MeshInPlace> &negatives, size_t slabs)
{
    assert(densities.size() == solids.size());
    double z_min = std::numeric_limits<double>::max();
    double z_max = std::numeric_limits<double>::lowest();
    for (const auto &[mesh, trafo] : solids)
        for (const stl_vertex &v : mesh->vertices) {
            const double z = (trafo * v.cast<double>()).z();
            z_min          = std::min(z_min, z);
            z_max          = std::max(z_max, z);
        }
    if (z_min >= z_max || slabs == 0)
        return {};

    // Each slab sliced at its middle.
    const double       thickness = (z_max - z_min) / double(slabs);
    std::vector<float> zs(slabs);
    for (size_t k = 0; k < slabs; ++k)
        zs[k] = float(z_min + (double(k) + 0.5) * thickness);

    MeshSlicingParamsEx params;
    const auto          slice = [&zs, &params](const MeshInPlace &mesh) {
        params.trafo = mesh.second;
        return slice_mesh_ex(*mesh.first, zs, params);
    };
    std::vector<std::vector<ExPolygons>> slices;
    for (const MeshInPlace &solid : solids)
        slices.emplace_back(slice(solid));
    std::vector<ExPolygons> cut(slabs);
    for (const MeshInPlace &negative : negatives) {
        std::vector<ExPolygons> slices_negative = slice(negative);
        for (size_t k = 0; k < slabs; ++k)
            append(cut[k], std::move(slices_negative[k]));
    }

    // The islands of each slab, and the moments of what each solid prints of them with its density.
    const bool uniform = std::all_of(densities.begin(), densities.end(), [&densities](double d) { return d == densities.front(); });
    std::vector<ExPolygons>           islands(slabs);
    std::vector<std::vector<Moments>> moments(slabs);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, slabs), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t k = range.begin(); k < range.end(); ++k) {
            ExPolygons all;
            for (const std::vector<ExPolygons> &solid : slices)
                append(all, solid[k]);
            islands[k] = diff_ex(union_ex(all), cut[k]);
            moments[k].assign(islands[k].size(), {});
            const double z   = zs[k];
            const auto   add = [&](const ExPolygon &region, double density, size_t island) {
                AreaMoments area;
                area.add(region.contour);
                for (const Polygon &hole : region.holes)
                    area.add(hole);
                if (area.area <= 0.)
                    return;
                // A prism of the slab's thickness.
                Matrix3d second;
                second << area.second.x(), area.second.z(), area.first.x() * z, area.second.z(), area.second.y(), area.first.y() * z,
                    area.first.x() * z, area.first.y() * z, area.area * (z * z + thickness * thickness / 12.);
                moments[k][island].add({ density * area.area * thickness, area.area * thickness,
                                         density * thickness * Vec3d(area.first.x(), area.first.y(), area.area * z), density * thickness * second });
            };
            if (uniform) {
                for (size_t j = 0; j < islands[k].size(); ++j)
                    add(islands[k][j], densities.front(), j);
                continue;
            }
            // A later solid prints where it overlaps an earlier one, and each region it prints lies in one island.
            const IslandLocator locator(islands[k], 10);
            ExPolygons          later = cut[k];
            for (size_t i = solids.size(); i-- > 0;)
                if (!slices[i][k].empty()) {
                    for (const ExPolygon &region : diff_ex(slices[i][k], later))
                        if (const int island = locator.find(region.contour.points.front()).first; island >= 0)
                            add(region, densities[i], size_t(island));
                    later = union_ex(later, slices[i][k]);
                }
        }
    });

    std::vector<const ExPolygons *> layers;
    layers.reserve(slabs);
    for (const ExPolygons &layer : islands)
        layers.emplace_back(&layer);
    size_t                                 count  = 0;
    const std::vector<std::vector<size_t>> bodies = connected_bodies(layers, count);
    std::vector<Moments>                   sums(count);
    std::vector<Points>                    outlines(count);
    std::vector<SolidBody>                 out(count);
    for (SolidBody &body : out) {
        body.z_min = std::numeric_limits<double>::max();
        body.z_max = std::numeric_limits<double>::lowest();
    }
    for (size_t k = 0; k < slabs; ++k)
        for (size_t j = 0; j < islands[k].size(); ++j) {
            const size_t body = bodies[k][j];
            sums[body].add(moments[k][j]);
            append(outlines[body], islands[k][j].contour.points);
            out[body].z_min = std::min(out[body].z_min, zs[k] - 0.5 * thickness);
            out[body].z_max = std::max(out[body].z_max, zs[k] + 0.5 * thickness);
        }
    for (size_t body = 0; body < count; ++body)
        if (const Moments &sum = sums[body]; sum.mass > 0.) {
            const Vec3d     center = sum.first / sum.mass;
            MassProperties &solid  = out[body];
            solid                  = { sum.mass, sum.volume, center, sum.second / sum.mass - center * center.transpose() };
            out[body].hull         = Geometry::convex_hull(std::move(outlines[body]));
        }
    return out;
}

} // namespace Slic3r
