#include <cmath>
#include <limits>

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Line.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../Surface.hpp"

#include "FillRadial.hpp"
#include "FillRectilinear.hpp"

namespace Slic3r {

// Falls back to FillRectilinear when there is no inner loop to radiate from
// (radial spokes only make sense for an annular-ish surface).
static void fill_surface_fallback(
    Fill                            &self,
    const FillParams                &params,
    const std::pair<float, Point>   &direction,
    const ExPolygon                 &expolygon,
    Polylines                       &polylines_out)
{
    FillRectilinear fallback;
    fallback.layer_id                = self.layer_id;
    fallback.z                       = self.z;
    fallback.spacing                 = self.spacing;
    fallback.overlap                 = self.overlap;
    fallback.angle                   = direction.first;
    fallback.fixed_angle             = self.fixed_angle;
    fallback.link_max_length         = self.link_max_length;
    fallback.loop_clipping           = self.loop_clipping;
    fallback.bounding_box            = self.bounding_box;
    fallback.print_config            = self.print_config;
    fallback.print_object_config     = self.print_object_config;
    fallback.no_overlap_expolygons   = self.no_overlap_expolygons;
    fallback.dont_alternate_fill_direction = self.dont_alternate_fill_direction;

    Surface surface(stBottomBridge, expolygon);
    surface.bridge_angle = direction.first;
    append(polylines_out, fallback.fill_surface(&surface, params));
}

// Finds where a ray from `center` through `far_point` crosses `polygon`'s
// boundary nearest to `center`. A non-convex polygon can cross the ray more
// than once; v1 targets roughly circular loops, so nearest is good enough.
static bool nearest_ray_polygon_intersection(
    const Point &center, const Point &far_point, const Polygon &polygon, double min_distance, Point *out)
{
    Line ray(center, far_point);
    bool found = false;
    double best_dist = std::numeric_limits<double>::max();
    Point  best_pt;
    for (const Line &edge : polygon.lines()) {
        Point hit;
        if (ray.intersection(edge, &hit)) {
            double dist = (hit - center).cast<double>().norm();
            if (dist >= min_distance && dist < best_dist) {
                best_dist = dist;
                best_pt   = hit;
                found     = true;
            }
        }
    }
    if (found)
        *out = best_pt;
    return found;
}

void FillRadial::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    if (expolygon.holes.empty()) {
        fill_surface_fallback(*this, params, direction, expolygon, polylines_out);
        return;
    }

    // v1: only the largest hole is treated as the inner loop to radiate from.
    const Polygon *hole = &expolygon.holes.front();
    for (const Polygon &h : expolygon.holes)
        if (std::abs(h.area()) > std::abs(hole->area()))
            hole = &h;

    const Point center = hole->centroid();

    double avg_outer_radius = 0.;
    for (const Point &pt : expolygon.contour.points)
        avg_outer_radius += (pt - center).cast<double>().norm();
    avg_outer_radius /= double(expolygon.contour.points.size());
    if (avg_outer_radius <= 0.) {
        fill_surface_fallback(*this, params, direction, expolygon, polylines_out);
        return;
    }

    coord_t min_spacing = scale_(this->spacing);
    coord_t distance    = coord_t(min_spacing / params.density);

    double angle_step = double(distance) / avg_outer_radius;
    if (!(angle_step > 0.) || !std::isfinite(angle_step))
        angle_step = M_PI / 4.;
    int ray_count = std::max(3, int(std::round(2. * M_PI / angle_step)));

    const double ray_len = 2. * this->bounding_box.radius() + double(min_spacing);

    for (int i = 0; i < ray_count; ++i) {
        double theta = 2. * M_PI * double(i) / double(ray_count);
        Point  far_point(
            center.x() + coord_t(std::cos(theta) * ray_len),
            center.y() + coord_t(std::sin(theta) * ray_len));

        Point inner_pt, outer_pt;
        if (!nearest_ray_polygon_intersection(center, far_point, *hole, 0., &inner_pt))
            continue;
        double inner_dist = (inner_pt - center).cast<double>().norm();
        if (!nearest_ray_polygon_intersection(center, far_point, expolygon.contour, inner_dist, &outer_pt))
            continue;

        Polyline pl;
        pl.points = { inner_pt, outer_pt };
        polylines_out.emplace_back(std::move(pl));
    }
}

void FillRadial::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    // Radial bridging is only meaningful as plain (non-variable-width) lines.
    Polylines polylines;
    this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
    append(thick_polylines_out, to_thick_polylines(std::move(polylines), scale_(this->spacing)));
}

} // namespace Slic3r
