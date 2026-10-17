#include "VoronoiRelaxation.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiPointCloud.hpp"
#include "../BoundingBox.hpp"
#include "../Point.hpp"
#include "../libslic3r.h"
#include <CGAL/Delaunay_triangulation_3.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Triangulation_vertex_base_with_info_3.h>
#include <CGAL/Triangulation_cell_base_3.h>
#include <CGAL/Triangulation_data_structure_3.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {
namespace {
bool height_order(const Vec3d &a, const Vec3d &b)
{
    if (a.z() != b.z()) return a.z() < b.z();
    if (a.y() != b.y()) return a.y() < b.y();
    return a.x() < b.x();
}
} // namespace

RelaxedPointCloud::RelaxedPointCloud(std::shared_ptr<const PointCloudProvider> source, const BoundingBoxf3 &bounds,
                                   double site_spacing, int iterations, const std::function<void(double)> &progress)
    : m_source(std::move(source))
{
    if (iterations < 0 || !std::isfinite(site_spacing) || site_spacing <= 0.)
        throw std::invalid_argument("Invalid Voronoi relaxation settings");
    const auto report = [&](double fraction) { if (progress) progress(fraction); };
    report(0.);
    if (const auto extent = m_source->extent()) {
        if (extent->defined)
            m_sites = m_source->points_in(BoundingBoxf3(extent->min - Vec3d::Constant(EPSILON),
                                                       extent->max + Vec3d::Constant(EPSILON)), site_spacing);
    } else if (bounds.defined) {
        // Reuse the ingestion proof with a slab spanning the whole object.
        // Exterior neighbours participate; model outlines do not clip sites.
        ActivePointCloud active(m_source);
        const BoundingBox xy(Point::new_scale(bounds.min.x(), bounds.min.y()), Point::new_scale(bounds.max.x(), bounds.max.y()));
        const double z = 0.5 * (bounds.min.z() + bounds.max.z());
        active.update(xy, z, 0.5 * (bounds.max.z() - bounds.min.z()), site_spacing,
            [&](const auto &points) { report(0.); return section(points, xy, z).covering_radius; });
        m_sites = active.sites();
    }
    std::sort(m_sites.begin(), m_sites.end(), height_order);
    m_sites.erase(std::unique(m_sites.begin(), m_sites.end()), m_sites.end());
    report(0.1);

    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Vertex = CGAL::Triangulation_vertex_base_with_info_3<size_t, Kernel>;
    using Cells = CGAL::Triangulation_cell_base_3<Kernel>;
    using Triangulation = CGAL::Delaunay_triangulation_3<Kernel, CGAL::Triangulation_data_structure_3<Vertex, Cells>>;
    for (int iteration = 0; iteration < iterations && !m_sites.empty(); ++iteration) {
        const auto stage = [&](double fraction) { report(0.1 + 0.9 * (iteration + fraction) / iterations); };
        Triangulation mesh;
        constexpr size_t batch = 4096;
        for (size_t first = 0; first < m_sites.size(); first += batch) {
            std::vector<std::pair<Kernel::Point_3, size_t>> points;
            const size_t last = std::min(first + batch, m_sites.size());
            for (size_t i = first; i < last; ++i)
                points.emplace_back(Kernel::Point_3(m_sites[i].x(), m_sites[i].y(), m_sites[i].z()), i);
            mesh.insert(points.begin(), points.end());
            stage(0.6 * double(last) / m_sites.size());
        }
        if (mesh.dimension() < 3) break;
        const auto lengths = m_source->local_spacing(m_sites, site_spacing);
        if (lengths.size() != m_sites.size() || std::any_of(lengths.begin(), lengths.end(),
            [](double s) { return !std::isfinite(s) || s <= 0.; }))
            throw std::invalid_argument("Invalid Voronoi local spacing");
        std::vector<bool> fixed(m_sites.size(), false);
        size_t visited = 0;
        // Hold the finite constellation's outer boundary rather than allowing
        // attractive springs to shrink the entire field towards the object.
        for (auto cell = mesh.all_cells_begin(); cell != mesh.all_cells_end(); ++cell) {
            if (mesh.is_infinite(cell))
                for (int i = 0; i < 4; ++i)
                    if (!mesh.is_infinite(cell->vertex(i))) fixed[cell->vertex(i)->info()] = true;
            if (++visited % 8192 == 0) stage(0.65);
        }
        std::vector<Vec3d> forces(m_sites.size(), Vec3d::Zero());
        std::vector<size_t> degree(m_sites.size(), 0);
        for (auto edge = mesh.finite_edges_begin(); edge != mesh.finite_edges_end(); ++edge) {
            const size_t a = edge->first->vertex(edge->second)->info(), b = edge->first->vertex(edge->third)->info();
            const Vec3d delta = m_sites[b] - m_sites[a];
            const double length = delta.norm();
            if (length > EPSILON) {
                const double target = std::sqrt(lengths[a]) * std::sqrt(lengths[b]);
                const Vec3d force = delta * ((length - target) / length);
                forces[a] += force;
                forces[b] -= force;
                ++degree[a];
                ++degree[b];
            }
            if (++visited % 8192 == 0) stage(0.7);
        }
        // Simultaneous damped steps do not depend on vertex traversal order.
        for (size_t i = 0; i < m_sites.size(); ++i) {
            if (fixed[i] || degree[i] == 0) continue;
            Vec3d step = forces[i] * (0.2 / degree[i]);
            const double length = step.norm(), limit = 0.1 * site_spacing;
            if (length > limit) step *= limit / length;
            m_sites[i] += step;
        }
        stage(1.);
    }
    std::sort(m_sites.begin(), m_sites.end(), height_order);
    for (const auto &point : m_sites) m_bounds.merge(point);
    report(1.);
}

std::vector<Vec3d> RelaxedPointCloud::points_in(const BoundingBoxf3 &box, double) const
{
    std::vector<Vec3d> result;
    if (!box.defined) return result;
    const auto first = std::lower_bound(m_sites.begin(), m_sites.end(), box.min.z(), [](const Vec3d &p, double z) { return p.z() < z; });
    const auto last = std::lower_bound(first, m_sites.end(), box.max.z(), [](const Vec3d &p, double z) { return p.z() < z; });
    for (auto it = first; it != last; ++it)
        if ((it->array() >= box.min.array()).all() && (it->array() < box.max.array()).all()) result.push_back(*it);
    return result;
}
} // namespace Slic3r::Voronoi
