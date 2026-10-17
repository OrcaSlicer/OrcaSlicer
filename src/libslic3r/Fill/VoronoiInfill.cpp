#include "VoronoiInfill.hpp"

#include "../ClipperUtils.hpp"
#include "../AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiRouting.hpp"
#include <cstddef>
#include <memory>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "../I18N.hpp"
#include <utility>
#include "../Polygon.hpp"
#include "libslic3r/libslic3r.h"
#include "VoronoiPointCloud.hpp"

namespace Slic3r {
Polylines VoronoiInfill::route_walls(Polylines walls, size_t layer_id) const
{
    if (walls.empty()) return walls;
    const Voronoi::WallGraph graph = Voronoi::build_wall_graph(walls);
    return Voronoi::trace_trails(graph,
        Voronoi::pair_junctions(graph, m_section.junctions, layer_id), layer_id);
}

Voronoi::Layer VoronoiInfill::fill_geometry(const BoundingBox &object_bounds, const ExPolygon &region,
                                          const GeometryParams &params)
{
    auto [spacing, density, z, width, height, reference_height, row_spacing, wall_count, point_spacing] = params;
    if (row_spacing < 0.)
        row_spacing = spacing;
    if (reference_height < 0.)
        reference_height = height;
    if (region.contour.empty() || density == 0.) return {};
    if (wall_count < 1) throw std::invalid_argument(_u8L("Invalid Voronoi wall count"));
    if (!(density > 0.) || !(spacing > 0.) || !std::isfinite(density) || !std::isfinite(spacing) ||
        !std::isfinite(z) || !(width > 0.) || !std::isfinite(width) || !(height >= 0.) ||
        !std::isfinite(height) || !std::isfinite(reference_height) || !(row_spacing > 0.) ||
        !std::isfinite(row_spacing) || !std::isfinite(point_spacing) || point_spacing < 0.)
        throw std::invalid_argument(_u8L("Invalid Voronoi fill parameters"));
    const BoundingBox bounds = empty(object_bounds) ? region.contour.bounding_box() : object_bounds;
    if (empty(bounds))
        return {};
    const double site_spacing = point_spacing > 0. ? point_spacing : m_cloud.spacing(spacing * wall_count, reference_height, density);
    if (!std::isfinite(site_spacing) || !(site_spacing >= SCALING_FACTOR))
        throw std::invalid_argument(_u8L("Invalid Voronoi point spacing"));
    // Outward rounding can move the face beyond z +/- height by less than
    // row_spacing * slope_length; shallow faces have slope_length < 2*h/width.
    const double margin = height * (1. + 2. * row_spacing / width);
    if (!std::isfinite(margin)) throw std::invalid_argument(_u8L("Invalid Voronoi floor margin"));

    if (m_site_spacing != site_spacing || m_bounds.min != bounds.min || m_bounds.max != bounds.max) {
        m_bounds = bounds;
        m_site_spacing = site_spacing;
        m_layer_z = std::numeric_limits<double>::quiet_NaN();
    }
    if (m_layer_z != z || m_layer_width != width || m_layer_height != height || m_row_spacing != row_spacing || m_wall_count != wall_count) {
        if (!m_cloud.update(bounds, z, margin, site_spacing, [&, z = params.z](const std::vector<Vec3d> &sites) {
                m_section = Voronoi::section(sites, bounds, z);
                return m_section.covering_radius;
            }, [this](double cutoff) { m_geometry.remove_below(cutoff); })) return {};
        // Coverage uses the cheap planar diagram. Retire points before updating
        // the persistent 3D network, avoiding a larger provisional triangulation.
        m_bands.clear();
        if (height > 0.) {
            if (m_geometry_revision != m_cloud.revision()) {
                m_geometry.update(m_cloud.sites());
                m_geometry_revision = m_cloud.revision();
            }
            m_bands = m_geometry.shallow_bands(bounds, z, width, height, row_spacing);
        }
        m_layer_z = z;
        m_layer_width = width;
        m_layer_height = height;
        m_row_spacing = row_spacing;
        m_wall_count = wall_count;
    }
    Voronoi::Layer result;
    std::vector<ExPolygons> footprints;
    std::vector<AABBTreeIndirect::BoundingBoxWrapper> boxes;
    const double expansion = 0.5 * (wall_count - 1) * row_spacing;
    for (const Voronoi::Band &band : m_bands) {
        const ExPolygons footprint = expansion > 0. ? offset_ex(band.polygon, float(scale_(expansion))) : ExPolygons{band.polygon};
        footprints.push_back(intersection_ex(footprint, ExPolygons{region}));
        if (!footprints.back().empty()) boxes.emplace_back(footprints.size() - 1, get_extents(footprints.back()));
    }
    using BandTree = AABBTreeIndirect::Tree<2, coord_t>;
    BandTree tree;
    tree.build_modify_input(boxes);
    for (size_t i = 0; i < m_bands.size(); ++i) {
        if (footprints[i].empty()) continue;
        const BoundingBox box = get_extents(footprints[i]);
        ExPolygons covered;
        AABBTreeIndirect::traverse(tree, AABBTreeIndirect::intersecting(BandTree::BoundingBox(box.min, box.max)),
            [&](const BandTree::Node &node) {
                if (node.idx < i) append(covered, footprints[node.idx]);
                return true;
            });
        for (ExPolygon &polygon : diff_ex(footprints[i], covered))
            result.bands.push_back({std::move(polygon), m_bands[i].supported_side, m_bands[i].row_origin + expansion});
    }
    // Shrink floor interiors by one fixed-point unit to retain coincident
    // boundary walls; Clipper handles those lines differently by direction.
    ExPolygons wall_mask;
    for (const Voronoi::Band &band : result.bands)
        append(wall_mask, offset_ex(band.polygon, -1.f));
    result.walls = diff_pl(intersection_pl(m_section.walls, region), wall_mask);
    return result;
}

} // namespace Slic3r
