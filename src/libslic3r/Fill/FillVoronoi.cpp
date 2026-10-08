#include "FillVoronoi.hpp"
#include "FillRectilinear.hpp"
#include "VoronoiRouting.hpp"
#include "../Surface.hpp"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace Slic3r {

void FillVoronoi::_fill_surface_single(const FillParams &params, unsigned int,
                                      const std::pair<float, Point> &, ExPolygon expolygon,
                                      Polylines &polylines_out)
{
    const double height = params.flow.height();
    const double reference_height = print_object_config ? print_object_config->layer_height.value : height;
    Voronoi::Layer layer = m_infill.fill_layer(bounding_box, expolygon, spacing, params.density, z,
                                             params.flow.width(), height, reference_height, params.flow.spacing(), layer_id);
    if (m_hull_threshold > 0.) {
        const std::array<double, 4> key {z, height, params.flow.width(), m_hull_threshold};
        if (key != m_hull_key || bounding_box.min != m_hull_bounds.min || bounding_box.max != m_hull_bounds.max) {
            m_hull = Voronoi::hull_section(*m_provider, bounding_box, z - 0.5 * height, height,
                                         params.flow.width(), m_hull_threshold);
            m_hull_key = key;
            m_hull_bounds = bounding_box;
        }
        ExPolygons excluded = m_hull.cavity;
        append(excluded, m_hull.skin);
        layer.walls = diff_pl(layer.walls, excluded);
        std::vector<Voronoi::Band> bands;
        for (const auto &band : layer.bands)
            for (auto &polygon : diff_ex(ExPolygons{band.polygon}, excluded))
                bands.push_back({std::move(polygon), band.supported_side, band.row_origin});
        layer.bands = std::move(bands);
    }
    const Point start = polylines_out.empty() ? expolygon.contour.first_point() : polylines_out.back().last_point();
    auto groups = fill_paths(std::move(layer), params);
    if (m_hull_threshold > 0. && !m_hull.skin.empty())
        append(groups, fill_hull(m_hull, expolygon, params));
    for (Voronoi::PathGroup &group : Voronoi::order_groups(std::move(groups), start))
        append(polylines_out, std::move(group.paths));
}

std::vector<Voronoi::PathGroup> FillVoronoi::fill_hull(const Voronoi::HullSection &hull, const ExPolygon &region, const FillParams &params) const
{
    const double width = params.flow.width();
    const auto centers = offset_ex(hull.cavity, float(scale_(0.5 * width)));
    std::vector<Voronoi::PathGroup> groups;
    for (const auto &polygon : to_polygons(centers)) {
        Polyline loop(polygon.points);
        loop.points.push_back(loop.points.front());
        for (auto &path : intersection_pl(Polylines{std::move(loop)}, region))
            groups.push_back({Polylines{std::move(path)}, true});
    }
    // The contour supplies one continuous bead for the vertical wall. Orca's
    // solid infill fills the additional swept area on slopes and end caps.
    const auto covered = diff_ex(offset_ex(centers, float(scale_(0.5 * width))),
                                 offset_ex(centers, -float(scale_(0.5 * width))));
    const auto extra = intersection_ex(diff_ex(hull.skin, covered), ExPolygons{region});
    FillAlignedRectilinear filler;
    filler.set_bounding_box(bounding_box);
    filler.angle = layer_id % 2 == 0 ? 0.f : float(M_PI / 2.);
    for (const auto &polygon : extra) {
        auto paths = solid_fill(filler, polygon, params);
        if (!paths.empty()) groups.push_back({std::move(paths), false});
    }
    return groups;
}

std::vector<Voronoi::PathGroup> FillVoronoi::fill_paths(Voronoi::Layer layer, const FillParams &params) const
{
    std::vector<Voronoi::PathGroup> groups;
    groups.reserve(layer.walls.size() + layer.bands.size());
    for (Polyline &wall : layer.walls) {
        Polylines paths;
        paths.push_back(std::move(wall));
        groups.push_back({std::move(paths), true});
    }
    for (const Voronoi::Band &band : layer.bands) {
        Polylines paths = fill_band(band, params);
        if (!paths.empty()) groups.push_back({std::move(paths), false});
    }
    return groups;
}

Polylines FillVoronoi::fill_band(const Voronoi::Band &band, const FillParams &params) const
{
    FillAlignedRectilinear filler;
    // Set the grid phase from the face's supported strip boundary, retaining
    // it through model clipping and subtraction of neighboring floor bands.
    if (std::isfinite(band.row_origin)) {
        const Vec2d anchor = band.supported_side * band.row_origin / SCALING_FACTOR;
        const Point point(anchor.x(), anchor.y());
        filler.set_bounding_box(BoundingBox(point - Point(1, 1), point + Point(1, 1)));
    } else {
        filler.set_bounding_box(bounding_box);
    }
    // Span along the wall section, with ends toward neighboring cell walls.
    filler.angle = float(std::atan2(band.supported_side.y(), band.supported_side.x()) + M_PI / 2.);
    Polylines lines = solid_fill(filler, band.polygon, params, true);
    for (Polyline &line : lines)
        if (band.supported_side.dot((line.first_point().cast<double>() - line.last_point().cast<double>()).eval()) < 0.)
            line.reverse();
    const auto position = [&band](const Polyline &line) {
        return band.supported_side.dot(line.first_point().cast<double>());
    };
    std::stable_sort(lines.begin(), lines.end(), [&](const Polyline &a, const Polyline &b) {
        return position(a) > position(b);
    });
    return lines;
}

Polylines FillVoronoi::solid_fill(FillAlignedRectilinear &filler, const ExPolygon &polygon,
                                FillParams params, bool monotonic) const
{
    filler.layer_id = layer_id;
    filler.z = z;
    filler.fixed_angle = true;
    filler.spacing = params.flow.spacing();
    // Zero inner inset means "no inner contour" to Rectilinear.
    filler.overlap = 0.5 * filler.spacing - EPSILON;
    params.density = 1.f;
    params.multiline = 1;
    params.dont_adjust = true;
    params.anchor_length = params.anchor_length_max = 1000.f;
    if (monotonic) params.monotonic = true;
    const Surface surface(stInternal, polygon);
    return intersection_pl(filler.fill_surface(&surface, params), polygon);
}

} // namespace Slic3r
