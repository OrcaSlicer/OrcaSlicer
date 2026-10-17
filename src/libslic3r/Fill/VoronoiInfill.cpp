#include "VoronoiInfill.hpp"

#include "../ClipperUtils.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiRouting.hpp"
#include "../KDTreeIndirect.hpp"
#include <cstddef>
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <boost/log/trivial.hpp>
#include <utility>
#include "../Polygon.hpp"

namespace Slic3r {
Polylines VoronoiInfill::route_walls(Polylines walls, size_t layer_id, const Points &boundary_nodes) const
{
    if (walls.empty()) return walls;
    std::vector<std::array<Vec3d, 2>> boundary_sites;
    const auto &sites = m_cloud.sites();
    if (!boundary_nodes.empty() && sites.size() >= 2) {
        auto coordinate = [&sites](size_t i, size_t axis) { return sites[i][axis]; };
        KDTreeIndirect<3, double, decltype(coordinate)> tree(coordinate, sites.size());
        for (const Point &point : boundary_nodes) {
            const Vec3d position(unscale<double>(point.x()), unscale<double>(point.y()), m_layer_z);
            const auto nearest = find_closest_points<2>(tree, position);
            boundary_sites.push_back({sites[nearest[0]], sites[nearest[1]]});
        }
    }
    const Voronoi::WallGraph graph = Voronoi::build_wall_graph(walls);
    return Voronoi::trace_trails(graph,
        Voronoi::pair_junctions(graph, m_section.junctions, layer_id, boundary_nodes, boundary_sites), layer_id);
}

Voronoi::Layer VoronoiInfill::fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                          double spacing, double density, double z, double width, double height,
                                          double reference_height, double row_spacing, int wall_count)
{
    if (row_spacing < 0.)
        row_spacing = spacing;
    if (reference_height < 0.)
        reference_height = height;
    if (wall_count < 1) return {};
    if (!(density > 0.) || !(spacing > 0.) || !std::isfinite(density) || !std::isfinite(spacing) ||
        !std::isfinite(z) || !(width > 0.) || !std::isfinite(width) || !(height >= 0.) ||
        !std::isfinite(height) || !std::isfinite(reference_height) || !(row_spacing > 0.) ||
        !std::isfinite(row_spacing) || region.contour.empty())
        return {};
    const BoundingBox bounds = empty(object_bounds) ? region.contour.bounding_box() : object_bounds;
    if (empty(bounds))
        return {};
    const double site_spacing = m_cloud.spacing(spacing * wall_count, reference_height, density);
    if (!std::isfinite(site_spacing) || !(site_spacing >= SCALING_FACTOR)) return {};
    // Outward rounding can move the face beyond z +/- height by less than
    // row_spacing * slope_length; shallow faces have slope_length < 2*h/width.
    const double margin = height * (1. + 2. * row_spacing / width);
    if (!std::isfinite(margin)) return {};

    if (m_site_spacing != site_spacing || m_bounds.min != bounds.min || m_bounds.max != bounds.max) {
        m_bounds = bounds;
        m_site_spacing = site_spacing;
        m_layer_z = std::numeric_limits<double>::quiet_NaN();
    }
    if (m_layer_z != z || m_layer_width != width || m_layer_height != height || m_row_spacing != row_spacing || m_wall_count != wall_count) {
        if (!m_cloud.update(bounds, z, margin, site_spacing, [&](const std::vector<Vec3d> &sites) {
                m_section = Voronoi::section(sites, bounds, z);
                return m_section.covering_radius;
            })) return {};
        // Coverage uses the cheap planar diagram. Retire points before updating
        // the persistent 3D network, avoiding a larger provisional triangulation.
        m_bands.clear();
        if (height > 0.) {
            m_geometry.update(m_cloud.sites());
            m_bands = m_geometry.shallow_bands(bounds, z, width, height, row_spacing);
        }
        m_layer_z = z;
        m_layer_width = width;
        m_layer_height = height;
        m_row_spacing = row_spacing;
        m_wall_count = wall_count;
    }
    const auto retained = [&](const Points &points) {
        std::vector<Vec3d> positions;
        positions.reserve(points.size());
        for (const Point &point : points)
            positions.emplace_back(unscale<double>(point.x()), unscale<double>(point.y()), z);
        std::vector<bool> keep;
        for (double value : m_provider->relative_density(positions)) keep.push_back(value >= 0.);
        return keep;
    };
    Polylines walls = intersection_pl(m_section.walls, region);
    if (m_provider->has_cutoff()) {
        const auto graph = Voronoi::build_wall_graph(walls);
        Points points;
        for (const auto &vertex : graph.vertices) points.push_back(vertex.point);
        const auto keep = retained(points);
        walls.clear();
        for (const auto &edge : graph.edges)
            if (keep[edge[0]] && keep[edge[1]])
                walls.emplace_back(Points{points[edge[0]], points[edge[1]]});
    }
    Voronoi::Layer result;
    ExPolygons covered, wall_mask;
    for (const Voronoi::Band &band : m_bands) {
        const double expansion = 0.5 * (wall_count - 1) * row_spacing;
        const ExPolygons footprint = expansion > 0. ? offset_ex(band.polygon, float(scale_(expansion))) : ExPolygons{band.polygon};
        ExPolygons clipped = intersection_ex(footprint, ExPolygons{region});
        for (ExPolygon &polygon : diff_ex(clipped, covered)) {
            // Also mask rejected floors so they cannot reappear as unsupported lines.
            append(wall_mask, offset_ex(polygon, -1.f));
            if (m_provider->has_cutoff()) {
                const auto keep = retained(polygon.contour.points);
                if (!std::all_of(keep.begin(), keep.end(), [](bool value) { return value; })) continue;
            }
            result.bands.push_back({std::move(polygon), band.supported_side, band.row_origin + expansion});
        }
        append(covered, std::move(clipped));
    }
    // Subtract floor interiors, preserving walls shared by floor faces. Clipper
    // otherwise treats coincident boundary lines differently by direction, and
    // floor rows can leave a gap at those boundaries. One coordinate unit is
    // enough to disambiguate clipping; tiny resulting fragments collapse below.
    result.walls = diff_pl(walls, wall_mask);
    BOOST_LOG_TRIVIAL(warning) << "Voronoi diagnostic: geometry z=" << z << " density=" << density
        << " spacing=" << spacing << " site_spacing=" << site_spacing << " sites=" << m_cloud.sites().size()
        << " section_walls=" << m_section.walls.size() << " floors=" << result.bands.size()
        << " clipped_walls=" << result.walls.size() << " bounds=" << bounds.min.transpose() << ';' << bounds.max.transpose();
    return result;
}

} // namespace Slic3r
