#include "VoronoiInfill.hpp"

#include "../ClipperUtils.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiRouting.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
Polylines VoronoiInfill::fill(const BoundingBox &object_bounds, const ExPolygon &region,
                            double spacing, double density, double z)
{
    Polylines walls = fill_layer(object_bounds, region, spacing, density, z, spacing, 0.).walls;
    return walls.empty() ? walls : Voronoi::order_trails(std::move(walls), region.contour.points.front());
}

Voronoi::Layer VoronoiInfill::fill_layer(const BoundingBox &object_bounds, const ExPolygon &region,
                                       double spacing, double density, double z, double width, double height,
                                       double reference_height, double row_spacing, size_t layer_id)
{
    Voronoi::Layer result = fill_geometry(object_bounds, region, spacing, density, z, width, height,
                                         reference_height, row_spacing);
    if (result.walls.empty())
        return result;
    const Voronoi::WallGraph graph = Voronoi::build_wall_graph(result.walls);
    const Voronoi::Connections connections = Voronoi::pair_junctions(graph, m_section.junctions, layer_id);
    result.walls = Voronoi::trace_trails(graph, connections, layer_id);
    return result;
}

Voronoi::Layer VoronoiInfill::fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                          double spacing, double density, double z, double width, double height,
                                          double reference_height, double row_spacing)
{
    if (row_spacing < 0.)
        row_spacing = spacing;
    if (reference_height < 0.)
        reference_height = height;
    if (!(density > 0.) || !(spacing > 0.) || !std::isfinite(density) || !std::isfinite(spacing) ||
        !std::isfinite(z) || !(width > 0.) || !std::isfinite(width) || !(height >= 0.) ||
        !std::isfinite(height) || !std::isfinite(reference_height) || !(row_spacing > 0.) ||
        !std::isfinite(row_spacing) || region.contour.empty())
        return {};
    const BoundingBox bounds = empty(object_bounds) ? region.contour.bounding_box() : object_bounds;
    if (empty(bounds))
        return {};
    const double site_spacing = m_cloud.spacing(spacing, reference_height, density);
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
    if (m_layer_z != z || m_layer_width != width || m_layer_height != height || m_row_spacing != row_spacing) {
        if (!m_cloud.update(bounds, z, margin, site_spacing, [&](const std::vector<Vec3d> &sites) {
                m_section = Voronoi::section(sites, bounds, z);
                return m_section.covering_radius;
            })) return {};
        m_bands = height > 0. ? Voronoi::shallow_bands(m_cloud.sites(), bounds, z, width, height, row_spacing) : std::vector<Voronoi::Band>{};
        m_layer_z = z;
        m_layer_width = width;
        m_layer_height = height;
        m_row_spacing = row_spacing;
    }
    Voronoi::Layer result;
    ExPolygons covered;
    for (const Voronoi::Band &band : m_bands) {
        ExPolygons clipped = intersection_ex(ExPolygons{band.polygon}, ExPolygons{region});
        for (ExPolygon &polygon : diff_ex(clipped, covered))
            result.bands.push_back({std::move(polygon), band.supported_side, band.row_origin});
        append(covered, std::move(clipped));
    }
    // Subtract floor interiors, preserving walls shared by floor faces. Clipper
    // otherwise treats coincident boundary lines differently by direction, and
    // floor rows can leave a gap at those boundaries. One coordinate unit is
    // enough to disambiguate clipping; tiny resulting fragments collapse below.
    ExPolygons wall_mask;
    for (const Voronoi::Band &band : result.bands)
        append(wall_mask, offset_ex(band.polygon, -1.f));
    result.walls = diff_pl(intersection_pl(m_section.walls, region), wall_mask);
    return result;
}

} // namespace Slic3r
