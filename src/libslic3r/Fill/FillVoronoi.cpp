#include "FillVoronoi.hpp"
#include "FillRectilinear.hpp"
#include "VoronoiRouting.hpp"
#include "../Surface.hpp"
#include "../ClipperUtils.hpp"
#include <algorithm>
#include <cmath>

namespace Slic3r {

void FillVoronoi::_fill_surface_single(const FillParams &params, unsigned int,
                                      const std::pair<float, Point> &, ExPolygon expolygon,
                                      Polylines &polylines_out)
{
    const double height = params.flow.height();
    const double reference_height = print_object_config ? print_object_config->layer_height.value : height;
    Voronoi::Layer layer = m_infill.fill_layer(bounding_box, expolygon, spacing, params.density, z,
                                             params.flow.width(), height, reference_height, params.flow.spacing(), layer_id);
    const Point start = polylines_out.empty() ? expolygon.contour.first_point() : polylines_out.back().last_point();
    for (Voronoi::PathGroup &group : fill_paths(std::move(layer), params, start))
        append(polylines_out, std::move(group.paths));
}

std::vector<Voronoi::PathGroup> FillVoronoi::fill_paths(Voronoi::Layer layer, const FillParams &params,
                                                     const Point &start_near) const
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
    return Voronoi::order_groups(std::move(groups), start_near);
}

Polylines FillVoronoi::fill_band(const Voronoi::Band &band, const FillParams &params) const
{
    const double pitch = params.flow.spacing();
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
    filler.layer_id = layer_id;
    filler.z = z;
    filler.fixed_angle = true;
    filler.spacing = pitch;
    // An exactly zero inner inset means "no inner contour" to Rectilinear.
    filler.overlap = 0.5 * pitch - EPSILON;
    FillParams solid = params;
    solid.density = 1.f;
    solid.multiline = 1;
    solid.dont_adjust = true;
    // Use Orca's contour-aware joins and monotonic sweep to alternate rows
    // without jumping back toward the advancing side of the floor.
    solid.anchor_length = solid.anchor_length_max = 1000.f;
    solid.monotonic = true;
    // Span along the wall section, with ends toward neighboring cell walls.
    filler.angle = float(std::atan2(band.supported_side.y(), band.supported_side.x()) + M_PI / 2.);
    Surface surface(stInternal, band.polygon);
    Polylines lines = intersection_pl(filler.fill_surface(&surface, solid), band.polygon);
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

} // namespace Slic3r
