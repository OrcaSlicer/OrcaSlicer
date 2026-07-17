#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Line.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../Surface.hpp"

#include "FillNormalizedLines.hpp"
#include "FillRectilinear.hpp"

namespace Slic3r {

// Falls back to FillRectilinear when the boundary has no favorable curvature
// to follow (e.g. a plain rectangular bridge).
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

// A point sampled along a boundary stretch that curves toward the fill area,
// together with the direction the fill area lies in at that point.
struct NormalOrigin
{
    Point  point;
    Vec2d  normal;
    size_t ring_index;
};

// Minimum turn angle (radians, expressed via sin) for a vertex to count as
// curving toward the fill area; filters out noise from near-straight edges.
static const double kMinTurnAngle = 0.0175; // ~1 degree

// Whether the boundary bulges into the fill area at `cur` (true along the
// whole loop of a circular hole, or at an inward notch of the outer
// contour). The contour (CCW) and holes (CW) of an ExPolygon both keep the
// filled area to the left of the direction of travel, so this test is the
// same regardless of which ring it is applied to.
static bool turns_toward_fill(const Point &prev, const Point &cur, const Point &next)
{
    Vec2d v1 = (cur - prev).cast<double>();
    Vec2d v2 = (next - cur).cast<double>();
    double l1 = v1.norm(), l2 = v2.norm();
    if (l1 < SCALED_EPSILON || l2 < SCALED_EPSILON)
        return false;
    return cross2(v1, v2) < -kMinTurnAngle * l1 * l2;
}

// Samples points roughly `spacing` apart along `points`, each with the local
// left-hand normal (pointing into the fill area). When `closed` the chain
// wraps back to its first point (used for a ring that curves toward the
// fill area along its entire length); otherwise the two end points are
// treated as context only, so sampling stays clear of them.
static void sample_chain(const Points &points, double spacing, bool closed, size_t ring_index, std::vector<NormalOrigin> &out)
{
    size_t n = points.size();
    if (n < 2 || spacing <= 0.)
        return;
    size_t segment_count = closed ? n : n - 1;

    std::vector<double> seg_len(segment_count);
    std::vector<Vec2d>  seg_dir(segment_count);
    double total_len = 0.;
    for (size_t seg = 0; seg < segment_count; ++seg) {
        Vec2d d = (points[(seg + 1) % n] - points[seg]).cast<double>();
        double len = d.norm();
        seg_len[seg] = len;
        seg_dir[seg] = len > SCALED_EPSILON ? Vec2d(d / len) : Vec2d(0., 0.);
        total_len += len;
    }
    if (total_len < SCALED_EPSILON)
        return;

    // Stay clear of the padding vertices at the ends of an open chain.
    double start = closed ? 0. : spacing * 0.5;
    double end   = closed ? total_len : total_len - spacing * 0.5;

    double acc = 0.;
    size_t seg = 0;
    for (double d = start; d < end; d += spacing) {
        while (seg + 1 < segment_count && acc + seg_len[seg] < d) {
            acc += seg_len[seg];
            ++seg;
        }
        double local = d - acc;
        const Point &a   = points[seg];
        const Vec2d &dir = seg_dir[seg];
        Point p(a.x() + coord_t(dir.x() * local), a.y() + coord_t(dir.y() * local));
        out.push_back({ p, Vec2d(-dir.y(), dir.x()), ring_index });
    }
}

// Walks `ring` once, grouping vertices that curve toward the fill area into
// maximal runs, and samples normal origins along each run (padded with one
// straight neighbor on either side for tangent continuity).
static void collect_normal_origins(const Polygon &ring, double spacing, size_t ring_index, std::vector<NormalOrigin> &out)
{
    size_t n = ring.points.size();
    if (n < 3)
        return;

    std::vector<bool> concave(n);
    for (size_t i = 0; i < n; ++i)
        concave[i] = turns_toward_fill(ring.points[(i + n - 1) % n], ring.points[i], ring.points[(i + 1) % n]);

    if (std::all_of(concave.begin(), concave.end(), [](bool b) { return b; })) {
        // The whole ring curves toward the fill area (e.g. a circular hole) -
        // sample around the full loop.
        sample_chain(ring.points, spacing, true, ring_index, out);
        return;
    }

    // Rotate the starting index to a non-concave vertex so runs of concave
    // vertices never need to wrap around the array end.
    size_t zero = 0;
    while (concave[zero])
        ++zero;

    for (size_t idx = 0; idx < n; ) {
        if (!concave[(zero + idx) % n]) {
            ++idx;
            continue;
        }
        size_t run_start = idx;
        size_t run_end   = idx;
        while (run_end + 1 < n && concave[(zero + run_end + 1) % n])
            ++run_end;

        Points chain;
        chain.push_back(ring.points[(zero + run_start + n - 1) % n]);
        for (size_t k = run_start; k <= run_end; ++k)
            chain.push_back(ring.points[(zero + k) % n]);
        chain.push_back(ring.points[(zero + run_end + 1) % n]);
        sample_chain(chain, spacing, false, ring_index, out);

        idx = run_end + 1;
    }
}

// Finds the nearest crossing of a ray cast from `origin` along `direction`
// against any edge of any ring in `rings` other than `skip_ring_index` (the
// ring the ray started from). The local normal is only an approximation of
// the true radial direction on a coarsely-tessellated curve, so without this
// exclusion the ray can clip back into its own ring at a moderate distance
// instead of reaching the opposite boundary, producing short stub lines.
static bool nearest_ray_intersection(
    const Point &origin, const Vec2d &direction, double ray_len, double min_distance,
    const std::vector<const Polygon *> &rings, size_t skip_ring_index, Point *out)
{
    Point far_point(
        origin.x() + coord_t(direction.x() * ray_len),
        origin.y() + coord_t(direction.y() * ray_len));
    Line ray(origin, far_point);

    bool   found     = false;
    double best_dist = std::numeric_limits<double>::max();
    Point  best_pt;
    for (size_t ri = 0; ri < rings.size(); ++ri) {
        if (ri == skip_ring_index)
            continue;
        for (const Line &edge : rings[ri]->lines()) {
            Point hit;
            if (ray.intersection(edge, &hit)) {
                double dist = (hit - origin).cast<double>().norm();
                if (dist >= min_distance && dist < best_dist) {
                    best_dist = dist;
                    best_pt   = hit;
                    found     = true;
                }
            }
        }
    }
    if (found)
        *out = best_pt;
    return found;
}

void FillNormalizedLines::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    coord_t min_spacing = scale_(this->spacing);
    coord_t distance    = coord_t(min_spacing / params.density);

    std::vector<const Polygon *> rings;
    rings.push_back(&expolygon.contour);
    for (const Polygon &h : expolygon.holes)
        rings.push_back(&h);

    std::vector<NormalOrigin> origins;
    for (size_t ri = 0; ri < rings.size(); ++ri)
        collect_normal_origins(*rings[ri], double(distance), ri, origins);

    if (origins.empty()) {
        fill_surface_fallback(*this, params, direction, expolygon, polylines_out);
        return;
    }

    const double ray_len          = 2. * this->bounding_box.radius() + double(min_spacing);
    const double min_hit_distance = double(SCALED_EPSILON) * 10.;

    for (const NormalOrigin &o : origins) {
        Point hit;
        if (!nearest_ray_intersection(o.point, o.normal, ray_len, min_hit_distance, rings, o.ring_index, &hit))
            continue;
        Polyline pl;
        pl.points = { o.point, hit };
        polylines_out.emplace_back(std::move(pl));
    }

    if (polylines_out.empty())
        fill_surface_fallback(*this, params, direction, expolygon, polylines_out);
}

void FillNormalizedLines::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    // Bridge lines generated this way are only meaningful as plain
    // (non-variable-width) lines.
    Polylines polylines;
    this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
    append(thick_polylines_out, to_thick_polylines(std::move(polylines), scale_(this->spacing)));
}

} // namespace Slic3r
