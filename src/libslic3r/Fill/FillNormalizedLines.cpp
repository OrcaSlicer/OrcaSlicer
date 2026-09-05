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
    // fallback.fill_surface() re-derives its own fill angle via
    // Fill::_infill_direction(), which unconditionally adds another 90
    // degrees on top of whatever angle it's given - but `direction` here is
    // already the result of one such call, made by our own caller. Pre-
    // subtract that same 90 degrees so the two additions cancel out and this
    // ends up using the same angle every other pattern would for this
    // bridge, instead of coming out rotated an extra 90 degrees.
    float compensated_angle = direction.first - float(M_PI / 2.);
    if (compensated_angle < 0.f)
        compensated_angle += float(2. * M_PI);

    FillRectilinear fallback;
    fallback.layer_id                = self.layer_id;
    fallback.z                       = self.z;
    fallback.spacing                 = self.spacing;
    fallback.overlap                 = self.overlap;
    fallback.angle                   = compensated_angle;
    fallback.fixed_angle             = self.fixed_angle;
    fallback.link_max_length         = self.link_max_length;
    fallback.loop_clipping           = self.loop_clipping;
    fallback.bounding_box            = self.bounding_box;
    fallback.print_config            = self.print_config;
    fallback.print_object_config     = self.print_object_config;
    fallback.no_overlap_expolygons   = self.no_overlap_expolygons;
    fallback.dont_alternate_fill_direction = self.dont_alternate_fill_direction;

    Surface surface(stBottomBridge, expolygon);
    surface.bridge_angle = compensated_angle;
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

// A boundary stretch that curves toward the fill area, pre-processed for
// fast interpolation of the point and left-hand normal at any arc-length
// position along it. When `closed` the chain wraps back to its first point
// (a ring that curves toward the fill area along its entire length);
// otherwise its first and last points are padding, kept only for the
// tangent they give neighboring samples.
struct Chain
{
    Points               points;
    bool                 closed = false;
    size_t               ring_index = 0;
    std::vector<double>  seg_len;
    std::vector<Vec2d>   seg_dir;    // per-edge direction, used to place the sampled point
    std::vector<Vec2d>   vertex_dir; // smoothed per-vertex direction, used for the ray itself
    double               total_len = 0.;
};

static Chain make_chain(Points points, bool closed, size_t ring_index)
{
    Chain c;
    c.points     = std::move(points);
    c.closed     = closed;
    c.ring_index = ring_index;
    size_t n = c.points.size();
    size_t segment_count = closed ? n : (n == 0 ? 0 : n - 1);
    c.seg_len.resize(segment_count);
    c.seg_dir.resize(segment_count);
    for (size_t seg = 0; seg < segment_count; ++seg) {
        Vec2d d = (c.points[(seg + 1) % n] - c.points[seg]).cast<double>();
        double len = d.norm();
        c.seg_len[seg] = len;
        c.seg_dir[seg] = len > SCALED_EPSILON ? Vec2d(d / len) : Vec2d(0., 0.);
        c.total_len += len;
    }

    // A polygon's per-edge direction is constant along each edge, with a hard
    // jump at every vertex - fine for placing points, but a real problem for
    // the ray direction: on a coarsely-tessellated curve that jump, magnified
    // by a long ray, produces a fixed gap between neighboring lines that no
    // amount of bisection between the same two constant directions can close.
    // Smooth it into a per-vertex tangent (averaging the two adjacent edges)
    // and interpolate that instead, approximating the true smooth curve.
    c.vertex_dir.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2d dir(0., 0.);
        if (closed) {
            dir = c.seg_dir[(i + segment_count - 1) % segment_count] + c.seg_dir[i % segment_count];
        } else if (i == 0) {
            dir = c.seg_dir[0];
        } else if (i + 1 == n) {
            dir = c.seg_dir[segment_count - 1];
        } else {
            dir = c.seg_dir[i - 1] + c.seg_dir[i];
        }
        // dir is a sum of up to two unit vectors, so its own magnitude lives
        // in roughly (0, 2] - compare against a plain small epsilon, not
        // SCALED_EPSILON (which is sized for scaled coordinate distances).
        double len = dir.norm();
        c.vertex_dir[i] = len > EPSILON ? Vec2d(dir / len) : Vec2d(0., 0.);
    }
    return c;
}

// Interpolates the point at arc-length `d` along `chain`, together with the
// left-hand normal (pointing into the fill area) of the smoothed tangent
// there, so the ray direction varies continuously instead of jumping at
// every polygon vertex.
static NormalOrigin sample_at(const Chain &chain, double d)
{
    size_t segment_count = chain.seg_len.size();
    double acc = 0.;
    size_t seg = 0;
    while (seg + 1 < segment_count && acc + chain.seg_len[seg] < d) {
        acc += chain.seg_len[seg];
        ++seg;
    }
    double       local = d - acc;
    const Point &a     = chain.points[seg];
    const Vec2d &dir   = chain.seg_dir[seg];
    Point p(a.x() + coord_t(dir.x() * local), a.y() + coord_t(dir.y() * local));

    double t = chain.seg_len[seg] > SCALED_EPSILON ? local / chain.seg_len[seg] : 0.;
    size_t next_vertex = (seg + 1) % chain.vertex_dir.size();
    Vec2d  smooth_dir  = (1. - t) * chain.vertex_dir[seg] + t * chain.vertex_dir[next_vertex];
    double smooth_len  = smooth_dir.norm();
    if (smooth_len > EPSILON)
        smooth_dir /= smooth_len;
    else
        smooth_dir = dir;

    return { p, Vec2d(-smooth_dir.y(), smooth_dir.x()), chain.ring_index };
}

// The arc-length midpoint between `d0` and `d1` along `chain`, wrapping
// through the closing segment of a closed chain when `d1` lies before `d0`.
static double chain_midpoint(const Chain &chain, double d0, double d1)
{
    if (chain.closed && d1 < d0)
        d1 += chain.total_len;
    double dm = 0.5 * (d0 + d1);
    if (chain.closed && dm >= chain.total_len)
        dm -= chain.total_len;
    return dm;
}

// Walks `ring` once, grouping vertices that curve toward the fill area into
// maximal runs, and builds a Chain for each run (padded with one straight
// neighbor on either side for tangent continuity).
static void collect_chains(const Polygon &ring, size_t ring_index, std::vector<Chain> &out)
{
    size_t n = ring.points.size();
    if (n < 3)
        return;

    std::vector<bool> concave(n);
    for (size_t i = 0; i < n; ++i)
        concave[i] = turns_toward_fill(ring.points[(i + n - 1) % n], ring.points[i], ring.points[(i + 1) % n]);

    std::vector<double> edge_len(n);
    double total_edge_len = 0.;
    for (size_t i = 0; i < n; ++i) {
        edge_len[i] = (ring.points[(i + 1) % n] - ring.points[i]).cast<double>().norm();
        total_edge_len += edge_len[i];
    }

    // A real STL mesh's polygon approximation of a curve often has a seam
    // (where the mesh wraps around) spanning a few vertices that aren't quite
    // convex, which would otherwise break an almost-fully-curved ring (e.g. a
    // circular hole) into a separate, short run - and an open run's endpoints
    // sit right next to this one's, since the seam it's stepping around is
    // narrow, producing two independent, crowded lines instead of one. Bridge
    // over any non-concave run that's a small fraction of the ring's whole
    // perimeter, so this scales with the ring's own size instead of an
    // unrelated setting like the print's line spacing; a real flat or
    // convex-away *feature* (as opposed to seam noise) is expected to make up
    // a much larger share of the boundary than that.
    const double kMaxSeamFraction = 0.15;
    const double max_seam_len     = total_edge_len * kMaxSeamFraction;
    bool any_concave = std::any_of(concave.begin(), concave.end(), [](bool b) { return b; });
    bool all_concave  = std::all_of(concave.begin(), concave.end(), [](bool b) { return b; });
    if (any_concave && !all_concave) {
        std::vector<bool> bridged = concave;
        for (size_t i = 0; i < n; ++i) {
            if (concave[i] || !concave[(i + n - 1) % n])
                continue; // not the start of a false-run
            double gap_len = 0.;
            size_t j       = i;
            size_t steps   = 0;
            while (steps < n && !concave[j]) {
                gap_len += edge_len[j];
                ++steps;
                j = (j + 1) % n;
            }
            if (gap_len <= max_seam_len) {
                size_t k = i;
                for (size_t s = 0; s < steps; ++s) {
                    bridged[k] = true;
                    k          = (k + 1) % n;
                }
            }
        }
        concave    = std::move(bridged);
        all_concave = std::all_of(concave.begin(), concave.end(), [](bool b) { return b; });
    }

    if (all_concave) {
        // The whole ring curves toward the fill area (e.g. a circular hole).
        out.push_back(make_chain(ring.points, true, ring_index));
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

        Points chain_points;
        chain_points.push_back(ring.points[(zero + run_start + n - 1) % n]);
        for (size_t k = run_start; k <= run_end; ++k)
            chain_points.push_back(ring.points[(zero + k) % n]);
        chain_points.push_back(ring.points[(zero + run_end + 1) % n]);
        out.push_back(make_chain(std::move(chain_points), false, ring_index));

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

// Casts a line from arc-length position `d` along `chain` and, if it hits
// the opposite boundary, appends it to `polylines_out`. Reports the far
// endpoint back to the caller so adjacent lines can be checked for gaps.
static bool emit_line(
    const Chain &chain, double d, const std::vector<const Polygon *> &rings,
    double ray_len, double min_hit_distance, Polylines &polylines_out, Point *hit_out)
{
    NormalOrigin o = sample_at(chain, d);
    Point        hit;
    if (!nearest_ray_intersection(o.point, o.normal, ray_len, min_hit_distance, rings, chain.ring_index, &hit))
        return false;
    Polyline pl;
    pl.points = { o.point, hit };
    polylines_out.emplace_back(std::move(pl));
    *hit_out = hit;
    return true;
}

// Lines cast from evenly-spaced origins on a curved boundary diverge as they
// travel toward the opposite side (an outer contour far from a small hole,
// for instance), so evenly spacing the origins is not enough to keep the far
// ends within `target_spacing` of each other. This adds extra lines,
// bisecting the gap between (d0, hit0) and (d1, hit1) until their far ends
// are close enough, or a recursion depth limit is hit.
static void refine_gap(
    const Chain &chain, double d0, const Point &hit0, bool valid0, double d1, const Point &hit1, bool valid1,
    int depth, const std::vector<const Polygon *> &rings, double ray_len, double min_hit_distance,
    double target_spacing, Polylines &polylines_out)
{
    static const int kMaxDepth = 6;
    if (depth >= kMaxDepth || !valid0 || !valid1)
        return;
    if ((hit1 - hit0).cast<double>().norm() <= target_spacing * 1.5)
        return;

    double dm = chain_midpoint(chain, d0, d1);
    Point  hitm;
    bool   validm = emit_line(chain, dm, rings, ray_len, min_hit_distance, polylines_out, &hitm);

    refine_gap(chain, d0, hit0, valid0, dm, hitm, validm, depth + 1, rings, ray_len, min_hit_distance, target_spacing, polylines_out);
    refine_gap(chain, dm, hitm, validm, d1, hit1, valid1, depth + 1, rings, ray_len, min_hit_distance, target_spacing, polylines_out);
}

void FillNormalizedLines::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    coord_t min_spacing          = scale_(this->spacing);
    coord_t distance             = coord_t(min_spacing / params.density);
    const double target_spacing = double(distance);

    std::vector<const Polygon *> rings;
    rings.push_back(&expolygon.contour);
    for (const Polygon &h : expolygon.holes)
        rings.push_back(&h);

    std::vector<Chain> chains;
    for (size_t ri = 0; ri < rings.size(); ++ri)
        collect_chains(*rings[ri], ri, chains);

    if (chains.empty()) {
        fill_surface_fallback(*this, params, direction, expolygon, polylines_out);
        return;
    }

    const double ray_len          = 2. * this->bounding_box.radius() + double(min_spacing);
    const double min_hit_distance = double(SCALED_EPSILON) * 10.;

    for (const Chain &chain : chains) {
        if (chain.total_len < SCALED_EPSILON)
            continue;

        // Initial, evenly-spaced samples along the chain, right out to its end
        // points: the tangent there is a smoothed average of neighboring
        // edges (see make_chain), so it stays well-defined even exactly on a
        // padding vertex - leaving any clearance here would just reopen the
        // gap this chain's own padding was meant to prevent.
        std::vector<double> ds;
        for (double d = 0.; d < chain.total_len; d += target_spacing)
            ds.push_back(d);
        if (!chain.closed)
            ds.push_back(chain.total_len);
        if (ds.empty())
            ds.push_back(chain.closed ? 0. : chain.total_len * 0.5);

        // An open chain's own two end points can still end up close together
        // in real space (e.g. a wide mesh seam that wasn't merged into a
        // closed loop above): drop trailing samples that crowd the first one
        // rather than emitting two nearly-coincident lines there.
        if (!chain.closed) {
            Point first_origin = sample_at(chain, ds.front()).point;
            while (ds.size() > 1) {
                Point last_origin = sample_at(chain, ds.back()).point;
                if ((last_origin - first_origin).cast<double>().norm() >= target_spacing * 0.5)
                    break;
                ds.pop_back();
            }
        }

        std::vector<Point> hits(ds.size());
        std::vector<bool>  valids(ds.size());
        for (size_t i = 0; i < ds.size(); ++i)
            valids[i] = emit_line(chain, ds[i], rings, ray_len, min_hit_distance, polylines_out, &hits[i]);

        if (ds.size() < 2)
            continue;
        size_t pair_count = chain.closed ? ds.size() : ds.size() - 1;
        for (size_t i = 0; i < pair_count; ++i) {
            size_t j = (i + 1) % ds.size();
            refine_gap(chain, ds[i], hits[i], valids[i], ds[j], hits[j], valids[j], 0,
                       rings, ray_len, min_hit_distance, target_spacing, polylines_out);
        }
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
