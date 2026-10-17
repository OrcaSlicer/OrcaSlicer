#include "VoronoiInfill.hpp"

#include "../ClipperUtils.hpp"
#include "VoronoiInfillCgal.hpp"
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include "../Polygon.hpp"

namespace Slic3r {
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
    const double site_spacing = m_provider->spacing(spacing * wall_count, reference_height, density);
    if (!std::isfinite(site_spacing) || !(site_spacing >= SCALING_FACTOR)) return {};
    // Outward rounding can move the face beyond z +/- height by less than
    // row_spacing * slope_length; shallow faces have slope_length < 2*h/width.
    const double margin = height * (1. + 2. * row_spacing / width);
    if (!std::isfinite(margin)) return {};

    const BoundingBoxf3 domain(Vec3d(unscale<double>(bounds.min.x()), unscale<double>(bounds.min.y()), std::min(z, m_z_range.x()) - margin),
                               Vec3d(unscale<double>(bounds.max.x()), unscale<double>(bounds.max.y()), std::max(z, m_z_range.y()) + margin));
    if (m_site_spacing != site_spacing || !m_domain.defined || m_domain.min != domain.min || m_domain.max != domain.max) {
        m_sites = Voronoi::complete_cloud(*m_provider, domain, site_spacing);
        if (m_sites.empty()) { m_domain.reset(); return {}; }
        m_geometry = height > 0. ? std::make_shared<Voronoi::CellGeometry>(m_sites) : nullptr;
        m_domain = domain;
        m_site_spacing = site_spacing;
        m_layer_z = std::numeric_limits<double>::quiet_NaN();
    }
    if (m_layer_z != z || m_layer_width != width || m_layer_height != height || m_row_spacing != row_spacing || m_wall_count != wall_count) {
        m_section = Voronoi::section(m_sites, bounds, z);
        m_bands = height > 0. ? m_geometry->shallow_bands(bounds, z, width, height, row_spacing) : std::vector<Voronoi::Band>{};
        m_layer_z = z;
        m_layer_width = width;
        m_layer_height = height;
        m_row_spacing = row_spacing;
        m_wall_count = wall_count;
    }
    Voronoi::Layer result;
    ExPolygons covered;
    for (const Voronoi::Band &band : m_bands) {
        const double expansion = 0.5 * (wall_count - 1) * row_spacing;
        const ExPolygons footprint = expansion > 0. ? offset_ex(band.polygon, float(scale_(expansion))) : ExPolygons{band.polygon};
        ExPolygons clipped = intersection_ex(footprint, ExPolygons{region});
        for (ExPolygon &polygon : diff_ex(clipped, covered))
            result.bands.push_back({std::move(polygon), band.supported_side, band.row_origin + expansion});
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
    result.walls.erase(std::remove_if(result.walls.begin(), result.walls.end(),
        [](const Polyline &wall) { return wall.length() <= scale_(0.000008); }), result.walls.end());
    return result;
}

} // namespace Slic3r
