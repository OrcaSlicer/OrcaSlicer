#include "FillVoronoi.hpp"
#include "FillBase.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "FillRectilinear.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Point.hpp"
#include "VoronoiInfill.hpp"
#include "VoronoiInfillCgal.hpp"
#include "VoronoiRouting.hpp"
#include "../Surface.hpp"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"
#include "../Line.hpp"
#include "libslic3r/libslic3r.h"
#include <cstddef>
#include <limits>
#include <algorithm>
#include <cmath>
#include <math.h>
#include <utility>
#include <vector>

namespace Slic3r {

Polylines FillVoronoi::fill_surface(const Surface *surface, const FillParams &params)
{
    m_surface_boundary = surface->expolygon;
    return Fill::fill_surface(surface, params);
}

void FillVoronoi::_fill_surface_single(const FillParams &params, unsigned int,
                                      const std::pair<float, Point> &, ExPolygon expolygon,
                                      Polylines &polylines_out)
{
    if (!(params.density > 0.)) return;
    const double height = params.flow.height();
    const double reference_height = print_object_config ? print_object_config->layer_height.value : height;
    VoronoiInfill::GeometryParams geometry;
    geometry.extrusion_spacing = spacing;
    geometry.density = params.density;
    geometry.z = z;
    geometry.width = params.flow.width();
    geometry.height = height;
    geometry.reference_height = reference_height;
    geometry.row_spacing = params.flow.spacing();
    geometry.wall_count = params.multiline;
    geometry.point_spacing = point_spacing;
    Voronoi::Layer layer = m_infill->fill_geometry(bounding_box, expolygon, geometry);
    layer.walls = m_infill->route_walls(std::move(layer.walls), layer_id);
    multiline_fill(layer.walls, params, float(params.flow.spacing()));
    if (params.multiline > 1)
        layer.walls = intersection_pl(std::move(layer.walls), expolygon);
    ExPolygons excluded;
    // Exclude floor interiors, preserving walls exactly on their boundaries.
    for (const auto &band : layer.bands) append(excluded, offset_ex(band.polygon, -1.f));
    if (!excluded.empty()) layer.walls = diff_pl(layer.walls, excluded);
    if (!params.dont_connect() && !layer.walls.empty()) {
        Polylines connected;
        connect_infill(std::move(layer.walls), to_polygons(expolygon),
                       m_surface_boundary.contour.bounding_box(), connected, params.flow.spacing(), params);
        layer.walls = std::move(connected);
    }
    const Point start = polylines_out.empty() ? expolygon.contour.first_point() : polylines_out.back().last_point();
    auto groups = fill_paths(std::move(layer), params);
    for (Voronoi::PathGroup &group : Voronoi::order_groups(std::move(groups), start))
        append(polylines_out, std::move(group.paths));
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

Polylines FillVoronoi::fill_band(const Voronoi::Band &band, FillParams params) const
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
    filler.layer_id = layer_id;
    filler.z = z;
    filler.fixed_angle = true;
    filler.spacing = params.flow.spacing();
    // Zero inner inset means "no inner contour" to Rectilinear.
    filler.overlap = 0.5 * filler.spacing - EPSILON;
    params.density = 1.f;
    params.multiline = 1;
    params.dont_adjust = true;
    constexpr float floor_anchor_length_mm = 1000.f;
    params.anchor_length = params.anchor_length_max = floor_anchor_length_mm;
    params.monotonic = true;
    const Surface surface(stInternal, band.polygon);
    Polylines lines = intersection_pl(filler.fill_surface(&surface, params), band.polygon);
    if (m_floor_support && !m_floor_support->empty()) {
        const double reach = scale_(params.flow.width());
        ExPolygons support = intersection_ex(*m_floor_support, offset_ex(band.polygon, float(reach)));
        const Polygons support_polygons = to_polygons(support);
        ExPolygons allowed = support;
        if (!m_surface_boundary.empty()) allowed.push_back(m_surface_boundary);
        allowed = union_ex(allowed);
        for (Polyline &path : lines) {
            const Points original = path.points;
            for (size_t i = 1; i < original.size(); ++i) {
                const Vec2d delta = original[i].cast<double>() - original[i - 1].cast<double>();
                const double length = delta.norm();
                // Rotating integer endpoints can add a few fixed-point units of error.
                if (length < SCALED_EPSILON || std::abs(band.supported_side.dot(delta)) > 3.) continue;
                const Vec2d direction = delta / length;
                for (size_t end : {i - 1, i}) {
                    if (expolygons_contain(support, original[end])) continue;
                    const Vec2d outward = direction * (end == i ? 1. : -1.);
                    const Vec2d xy = original[end].cast<double>() + reach * outward;
                    const Line ray(original[end], Point(coord_t(std::round(xy.x())), coord_t(std::round(xy.y()))));
                    double best = std::numeric_limits<double>::infinity();
                    for (const Line &span : intersection_ln(ray, support_polygons)) {
                        const double distance = outward.dot((span.midpoint().cast<double>() - original[end].cast<double>()).eval());
                        if (distance > 0. && distance < best) best = distance;
                    }
                    if (!std::isfinite(best)) continue;
                    const Vec2d target = original[end].cast<double>() + best * outward;
                    const Point anchor(coord_t(std::round(target.x())), coord_t(std::round(target.y())));
                    if (!m_surface_boundary.empty() && !diff_pl(Polylines{Polyline(Points{original[end], anchor})}, allowed).empty()) continue;
                    path.points[end] = anchor;
                }
            }
        }
    }
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
