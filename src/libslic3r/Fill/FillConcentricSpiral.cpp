#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"

#include "FillConcentricSpiral.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

// Index of the corner the spiral should start at. Every following loop is split at the point nearest
// the end of the one before it, so this choice propagates inwards and decides where the whole spiral
// hands over from ring to ring. A tight corner is the worst place for it: there the next ring
// retreats along the bisector by spacing/sin(angle), so the spiral has to strike out several spacings
// to reach it instead of stepping across to a ring running parallel one spacing away.
//
// A right angle is taken first when the loop has one. It clips cleanly, since the trimming below
// scales with 1/sin(angle) and so is at its shortest and least sensitive there, and it holds its
// shape as the loop is offset inwards, which keeps the handover in the same place ring after ring.
// Failing that the widest corner is the flattest stretch on offer, which is the next best handover.
// A straight point is no corner at all and only turns up as an artefact of the offsetting, so it is
// skipped.
static int find_spiral_start_corner(const Polygon& loop)
{
    const size_t n = loop.points.size();
    if (n < 3)
        return 0;

    // cos(85 deg): a corner within five degrees of square counts as a right angle.
    static const double right_angle_cos = 0.08716;
    // cos(179 deg): anything flatter than this counts as a straight point rather than a corner.
    static const double straight_cos = -0.99985;

    // Only convex corners qualify. A reflex corner spans the same angle between its two edges but
    // bulges the other way, so the next ring in steps away from it along the bisector instead of
    // hugging it, and starting there hands over across a long diagonal on every single ring. Loops
    // arrive counter-clockwise, in which case a convex corner turns left, but check the winding
    // rather than trust it. A closed loop always has at least one convex corner.
    const double convex_turn = loop.is_counter_clockwise() ? 1.0 : -1.0;

    double best_right_cos = right_angle_cos;
    int    best_right     = -1;
    double best_wide_cos  = 1.0;
    int    best_wide      = -1;
    for (size_t i = 0; i < n; ++i) {
        const Point& p_prev = loop.points[(i - 1 + n) % n];
        const Point& p      = loop.points[i];
        const Point& p_next = loop.points[(i + 1) % n];

        Vec2d  e_in  = (p - p_prev).cast<double>();
        Vec2d  e_out = (p_next - p).cast<double>();
        double len1  = e_in.norm();
        double len2  = e_out.norm();
        if (len1 < 1e-6 || len2 < 1e-6)
            continue;
        if (convex_turn * (e_in.x() * e_out.y() - e_in.y() * e_out.x()) <= 0.0)
            continue;

        // Cosine of the angle the two edges span at the corner: 1 at a spike, 0 square, -1 straight.
        double cos_val = -e_in.dot(e_out) / (len1 * len2);
        if (std::abs(cos_val) < best_right_cos) {
            best_right_cos = std::abs(cos_val);
            best_right     = int(i);
        }
        if (cos_val > straight_cos && cos_val < best_wide_cos) {
            best_wide_cos = cos_val;
            best_wide     = int(i);
        }
    }
    if (best_right >= 0)
        return best_right;
    // A loop smooth enough to have no corner at all, a circle say, hands over equally well anywhere.
    return best_wide < 0 ? 0 : best_wide;
}

// Length to trim off the end of a loop so that it does not overlap the start of the next one.
// The theoretical gap is distance/sin(alpha), alpha being the angle between the last segment of the
// loop and the first segment of the next one.
static double loop_clip_length(const Polyline& loop_path, const double gap)
{
    const Point& p_prev = loop_path.points[loop_path.points.size() - 2];
    const Point& p_last = loop_path.points.back();
    const Point& p_next = loop_path.points[1];
    Vec2d        v1     = (p_last - p_prev).cast<double>();
    Vec2d        v2     = (p_next - p_last).cast<double>();
    if (v1.norm() < 1e-6 || v2.norm() < 1e-6)
        return gap;

    double alpha = std::atan2(std::abs(v1.x() * v2.y() - v1.y() * v2.x()), v1.dot(v2));
    // Outside 45deg < alpha < 120deg the 1/sin(alpha) term would clip far too much, so fall back to the plain gap.
    return (alpha > M_PI / 4 && alpha < 2 * M_PI / 3) ? gap / std::sin(alpha) : gap;
}

static Polylines generate_concentric_spiral_polylines(const FillParams& params,
                                                      const Polygons&   loops,
                                                      const coord_t     distance,
                                                      const ExPolygon&  original_expoly)
{
    Polylines output;
    Polyline  spiral;
    Point     current_pos(0, 0);
    // Index into loops of the innermost loop appended to the spiral currently being built.
    int innermost_loop = -1;

    // Whether the spiral can run straight from one point to the other without leaving the material.
    // Neighbouring rings sit one spacing apart, so a hop that short cannot leave it and needs no
    // check, which covers all but a few of the loops. The rest are tested against the surface.
    const double free_hop = 1.5 * double(distance);
    auto reachable = [&](const Point& from, const Point& to) {
        return from.distance_to(to) <= free_hop || original_expoly.contains(Line(from, to));
    };

    // The centre point plugs the pin hole left in the middle of an island, it is not meant to
    // traverse it, so it is only worth adding when the innermost loop has shrunk to about a ring.
    const double max_center_stub = 2.0 * double(distance);

    // Emit the spiral built so far as one path and start over on a fresh island.
    auto flush_spiral = [&]() {
        if (spiral.empty())
            return;
        // Run into the middle of the innermost loop so the island's centre is filled instead of
        // being left as a pin hole. Only if that point sits inside the loop and is reachable,
        // otherwise the stub would run over material that is already extruded, or off the surface.
        if (innermost_loop >= 0) {
            const Point centroid = loops[innermost_loop].centroid();
            if (centroid != spiral.last_point() && spiral.last_point().distance_to(centroid) <= max_center_stub &&
                loops[innermost_loop].contains(centroid) && reachable(spiral.last_point(), centroid))
                spiral.points.push_back(centroid);
        }
        output.emplace_back(std::move(spiral));
        spiral.clear();
        innermost_loop = -1;
        current_pos    = Point(0, 0);
    };

    for (size_t i = 0; i < loops.size(); ++i) {
        const Polygon& loop = loops[i];
        if (loop.points.empty())
            continue;

        // split_at_index() duplicates the split point at both ends, so a usable loop has at least 3 points.
        Polyline loop_path(loop.split_at_index(spiral.empty() ? find_spiral_start_corner(loop) : current_pos.nearest_point_index(loop.points)));
        if (loop_path.size() < 3)
            continue;

        // Island jumping: union_pt_chained_outside_in() walks the nesting tree of the loops depth
        // first, so the next loop continues the current spiral exactly when it lies inside the one
        // just laid down. Distance cannot stand in for that test: at a sharp corner the next ring
        // retreats along the bisector by spacing/sin(angle), which leaves it several spacings away
        // while still being the very next ring in, and the spiral would break off at every spike.
        const bool same_island = innermost_loop >= 0 && loops[innermost_loop].contains(loop_path.points.front());
        if (!spiral.empty() && (!same_island || !reachable(spiral.last_point(), loop_path.points.front()))) {
            flush_spiral();
            loop_path = loop.split_at_index(find_spiral_start_corner(loop));
            if (loop_path.size() < 3)
                continue;
        }

        // Clip the end of the loop to leave room for the run into the next one. The last loop of the
        // surface has no successor, so it only gives up half of the gap.
        loop_path.clip_end(loop_clip_length(loop_path, (i + 1 == loops.size() ? 0.5 : 1.0) * double(distance)));
        // clip_end() empties the path when the loop is shorter than the clipping length, which happens
        // on the degenerate slivers that offsetting leaves behind. Such a loop carries no extrusion.
        if (loop_path.size() < 2)
            continue;

        if (spiral.empty())
            spiral = std::move(loop_path);
        else
            spiral.append(std::move(loop_path));

        innermost_loop = int(i);
        current_pos    = spiral.last_point();
    }

    flush_spiral();

    // An outward fill order runs every spiral from its centre to its outer edge, innermost island first.
    if (params.fill_order == SurfaceFillOrder::Outward) {
        for (Polyline& path : output)
            path.reverse();
        std::reverse(output.begin(), output.end());
    }

    return output;
}

void FillConcentricSpiral::_fill_surface_single(const FillParams& params,
                                                unsigned int thickness_layers,
                                                const std::pair<float, Point>& direction,
                                                ExPolygon expolygon,
                                                Polylines& polylines_out)
{
    BoundingBox bounding_box = expolygon.contour.bounding_box();

    coord_t min_spacing = scale_(this->spacing) * params.multiline;
    coord_t distance    = coord_t(min_spacing / params.density);

    if (params.density > 0.9999f && !params.dont_adjust) {
        distance      = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * (params.multiline - 1) * this->spacing)));

    Polygons loops = to_polygons(contracted);

    ExPolygons last{std::move(contracted)};
    while (!last.empty()) {
        last = offset2_ex(last, -(distance + min_spacing / 2), +min_spacing / 2);
        append(loops, to_polygons(last));
    }

    loops = union_pt_chained_outside_in(loops);

    // Generate the concentric spiral polylines.
    Polylines spiral_result = generate_concentric_spiral_polylines(params, loops, distance, expolygon);

    // Apply multiline offset if needed.
    multiline_fill(spiral_result, params, spacing);
    append(polylines_out, spiral_result);
}

} // namespace Slic3r
