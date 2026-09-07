#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"

#include "FillSpiralInset.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

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

// The chaining below drives two kinds of loop: the plain offset polygons of the classic path, and
// Arachne's variable width walls. These are the only four steps that differ between them. Widths run
// two per segment, so every point added or removed takes a pair with it.
static Polyline open_loop(const Polygon& loop, int start_index) { return loop.split_at_index(start_index); }

static ThickPolyline open_loop(const Arachne::ExtrusionLine& loop, int start_index)
{
    ThickPolyline path = Arachne::to_thick_polyline(loop);
    // start_at_index() rotates a closed path, and wants it closed with a matching width at both ends.
    if (path.points.front() != path.points.back()) {
        const coordf_t w_first = path.width.front(), w_last = path.width.back();
        path.points.emplace_back(path.points.front());
        path.width.emplace_back(w_last);
        path.width.emplace_back(w_first);
    }
    path.start_at_index(start_index);
    return path;
}

static void clip_path_end(Polyline& path, double distance) { path.clip_end(distance); }

static void clip_path_end(ThickPolyline& path, double distance)
{
    // Polyline::clip_end() knows nothing about the widths, so walk back trimming the two together.
    while (distance > 0 && path.points.size() >= 2) {
        const Point    last  = path.points.back();
        const coordf_t w_end = path.width.back();
        path.points.pop_back();
        path.width.pop_back();
        const coordf_t w_start = path.width.back();
        path.width.pop_back();

        const Vec2d  v   = (path.points.back() - last).cast<double>();
        const double len = v.norm();
        if (len > distance) {
            const double t = distance / len;
            path.points.emplace_back((last.cast<double>() + v * t).cast<coord_t>());
            path.width.emplace_back(w_start);
            path.width.emplace_back(w_start + (w_end - w_start) * (1.0 - t));
            return;
        }
        distance -= len;
    }
    path.clear();
}

static void append_path(Polyline& dst, Polyline&& src) { dst.append(std::move(src)); }

static void append_path(ThickPolyline& dst, ThickPolyline&& src)
{
    if (dst.empty()) {
        dst = std::move(src);
        return;
    }
    if (dst.points.back() == src.points.front()) {
        // Carrying straight on from the same point, so there is no run across to give a width to.
        src.points.erase(src.points.begin());
        src.width.erase(src.width.begin(), src.width.begin() + 2);
    } else {
        // The run across to the next loop tapers between the two ends it joins.
        const coordf_t w_from = dst.width.back(), w_to = src.width.front();
        dst.width.emplace_back(w_from);
        dst.width.emplace_back(w_to);
    }
    append(dst.points, std::move(src.points));
    append(dst.width, std::move(src.width));
}

// The classic loops all carry the same width, so the innermost one of an island can still ring an
// unfilled pin hole, which the spiral plugs by running into the middle. Arachne's walls widen to take
// up whatever is left over, so there is nothing there to plug and the stub would only double back
// over the wall that just filled it.
static bool leaves_a_centre_hole(const Polygon&) { return true; }
static bool leaves_a_centre_hole(const Arachne::ExtrusionLine&) { return false; }

static void append_path_point(Polyline& path, const Point& point) { path.points.emplace_back(point); }

static void append_path_point(ThickPolyline& path, const Point& point)
{
    const coordf_t w = path.width.back();
    path.points.emplace_back(point);
    path.width.emplace_back(w);
    path.width.emplace_back(w);
}

// Chain the loops of one surface into as few continuous spirals as its shape allows. The loops arrive
// ordered outside in, depth first, each paired with its outline in loop_outlines; every decision here
// is made on those outlines, so the two kinds of loop take exactly the same route.
template<class LoopType, class PathType>
static std::vector<PathType> generate_spiral_insets(const FillParams&                   params,
                                                         const std::vector<const LoopType*>& loops,
                                                         const Polygons&                     loop_outlines,
                                                         const coord_t                       distance,
                                                         const ExPolygon&                    original_expoly)
{
    std::vector<PathType> output;
    PathType              spiral;
    Point                 current_pos(0, 0);
    // Index into loops of the innermost loop appended to the spiral currently being built.
    int innermost_loop = -1;

    // Whether the spiral can run straight from one point to the other. The run across is extruded,
    // not travelled, so it has to be a genuine step over to the ring alongside:
    //  - up to a ring spacing and a half it cannot leave the material, and needs no check at all,
    //    which covers all but a few of the loops;
    //  - beyond that it is tested against the surface, which catches the points that are close in a
    //    straight line but separated by a hole or a notch;
    //  - past four spacings it is refused outright. A handover does stretch at a corner, where the
    //    next ring retreats along the bisector by spacing/sin(angle), but four spacings is already a
    //    fifteen degree wedge, and down a wedge that tight the run across would trace the bisector,
    //    which is where the tail is filled from anyway. Anything longer is a traverse across the
    //    surface that prints over what it crosses. Breaking the spiral leaves the G-code to travel it.
    const double free_hop = 1.5 * double(distance);
    const double max_hop  = 4.0 * double(distance);
    auto reachable = [&](const Point& from, const Point& to) {
        const double hop = from.distance_to(to);
        if (hop > max_hop)
            return false;
        return hop <= free_hop || original_expoly.contains(Line(from, to));
    };

    // The centre point plugs the pin hole left in the middle of an island, it is not meant to
    // traverse it, so it is only worth adding when the innermost loop has shrunk to about a ring.
    const double max_center_stub = 2.0 * double(distance);

    // Emit the spiral built so far as one path and start over on a fresh island.
    auto flush_spiral = [&]() {
        if (spiral.empty())
            return;
        // Run into the middle of the innermost loop so the island's centre is filled instead of being
        // left as a pin hole. Only where there is a hole to fill: the loop has to still enclose open
        // space once its own bead is accounted for, or the stub just runs back over that bead. And
        // the point has to sit inside the loop and be reachable, or it runs off across the surface.
        if (innermost_loop >= 0 && leaves_a_centre_hole(*loops[innermost_loop])) {
            const Polygon& innermost = loop_outlines[innermost_loop];
            const Point    centroid  = innermost.centroid();
            if (!offset(innermost, -float(0.5 * double(distance))).empty() && centroid != spiral.last_point() &&
                spiral.last_point().distance_to(centroid) <= max_center_stub && innermost.contains(centroid) &&
                reachable(spiral.last_point(), centroid))
                append_path_point(spiral, centroid);
        }
        output.emplace_back(std::move(spiral));
        spiral.clear();
        innermost_loop = -1;
        current_pos    = Point(0, 0);
    };

    for (size_t i = 0; i < loops.size(); ++i) {
        const Polygon& outline = loop_outlines[i];
        if (outline.points.empty())
            continue;

        // The loop is opened into a path with the split point repeated at both ends, so a usable one
        // has at least 3 points. Both kinds of loop share the outline's indices, hence its start point.
        PathType loop_path = open_loop(*loops[i], spiral.empty() ? find_spiral_start_corner(outline) :
                                                                  current_pos.nearest_point_index(outline.points));
        if (loop_path.size() < 3)
            continue;

        // Island jumping: the loops are ordered by their nesting, depth first, so the next one
        // continues the current spiral exactly when it lies inside the one just laid down. Distance
        // cannot stand in for that test: at a sharp corner the next ring retreats along the bisector
        // by spacing/sin(angle), which leaves it several spacings away while still being the very
        // next ring in, and the spiral would break off at every spike.
        const bool same_island = innermost_loop >= 0 && loop_outlines[innermost_loop].contains(loop_path.points.front());
        if (!spiral.empty() && (!same_island || !reachable(spiral.last_point(), loop_path.points.front()))) {
            flush_spiral();
            loop_path = open_loop(*loops[i], find_spiral_start_corner(outline));
            if (loop_path.size() < 3)
                continue;
        }

        // Clip the end of the loop to leave room for the run into the next one. The last loop of the
        // surface has no successor, so it only gives up half of the gap.
        clip_path_end(loop_path, loop_clip_length(loop_path, (i + 1 == loops.size() ? 0.5 : 1.0) * double(distance)));
        // Clipping empties the path when the loop is shorter than the clipping length, which happens
        // on the degenerate slivers that offsetting leaves behind. Such a loop carries no extrusion.
        if (loop_path.size() < 2)
            continue;

        append_path(spiral, std::move(loop_path));
        innermost_loop = int(i);
        current_pos    = spiral.last_point();
    }

    flush_spiral();

    // An outward fill order runs every spiral from its centre to its outer edge, innermost island first.
    if (params.fill_order != SurfaceFillOrder::Inward) {
        for (PathType& path : output)
            path.reverse();
        std::reverse(output.begin(), output.end());
    }

    return output;
}

void FillSpiralInset::_fill_surface_single(const FillParams& params,
                                                unsigned int thickness_layers,
                                                const std::pair<float, Point>& direction,
                                                ExPolygon expolygon,
                                                Polylines& polylines_out)
{
    BoundingBox bounding_box = expolygon.contour.bounding_box();

    coord_t min_spacing = scale_(this->spacing);
    coord_t distance    = coord_t(min_spacing / params.density);

    if (params.density > 0.9999f && !params.dont_adjust) {
        distance      = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    Polygons loops = to_polygons(expolygon);

    ExPolygons last{std::move(expolygon)};
    while (!last.empty()) {
        last = offset2_ex(last, -(distance + min_spacing / 2), +min_spacing / 2);
        append(loops, to_polygons(last));
    }

    // Orders the loops outside in, depth first, which is the order the chaining below expects.
    loops = union_pt_chained_outside_in(loops);

    std::vector<const Polygon*> loop_refs;
    loop_refs.reserve(loops.size());
    for (const Polygon& loop : loops)
        loop_refs.emplace_back(&loop);

    Polylines spiral_result = generate_spiral_insets<Polygon, Polyline>(params, loop_refs, loops, distance, expolygon);

    append(polylines_out, spiral_result);
}

void FillSpiralInset::_fill_surface_single(const FillParams& params,
                                                unsigned int thickness_layers,
                                                const std::pair<float, Point>& direction,
                                                ExPolygon expolygon,
                                                ThickPolylines& thick_polylines_out)
{
    assert(params.use_arachne);
    assert(this->print_config != nullptr && this->print_object_config != nullptr);

    // Only a solid surface is worth the variable width walls; a sparse one falls back to plain loops.
    if (params.density <= 0.9999f || params.dont_adjust) {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), scaled<coord_t>(this->spacing)));
        return;
    }

    // no rotation is supported for this infill pattern
    Point   bbox_size   = expolygon.contour.bounding_box().size();
    coord_t min_spacing = scaled<coord_t>(this->spacing);

    coord_t  loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
    Polygons polygons    = offset(expolygon, float(min_spacing) / 2.f);

    double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
    Arachne::WallToolPathsParams input_params;
    input_params.min_bead_width                = 0.85 * min_nozzle_diameter;
    input_params.min_feature_size              = 0.25 * min_nozzle_diameter;
    input_params.wall_transition_length        = 1.0 * min_nozzle_diameter;
    input_params.wall_transition_angle         = 10;
    input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
    input_params.wall_distribution_count       = 1;

    Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);
    std::vector<Arachne::VariableWidthLines> walls_by_inset = wallToolPaths.getToolPaths();

    // Open walls are the thin features Arachne fits between the closed ones. They cannot join a
    // spiral, so they go out as they are; leaving them behind is what would put the gaps back.
    std::vector<const Arachne::ExtrusionLine*> walls;
    Polygons                                   wall_outlines;
    ThickPolylines                             open_walls;
    for (const Arachne::VariableWidthLines& inset : walls_by_inset)
        for (const Arachne::ExtrusionLine& wall : inset) {
            if (wall.empty())
                continue;
            if (wall.is_closed) {
                walls.emplace_back(&wall);
                wall_outlines.emplace_back(wall.toPolygon());
            } else {
                open_walls.emplace_back(Arachne::to_thick_polyline(wall));
            }
        }

    // Arachne hands the walls back grouped by inset, which is not their nesting: around a hole the
    // wall of a given inset lies inside the wall of that same inset around the contour. Nest them by
    // containment instead, so the spiral follows one island all the way in before starting the next,
    // the same order union_pt_chained_outside_in gives the classic path above.
    const size_t      wall_count = walls.size();
    std::vector<int>  nesting_depth(wall_count, 0), parent(wall_count, -1);
    std::vector<char> inside(wall_count * wall_count, 0);
    for (size_t i = 0; i < wall_count; ++i)
        for (size_t j = 0; j < wall_count; ++j)
            if (i != j && wall_outlines[j].contains(walls[i]->junctions.front().p)) {
                inside[i * wall_count + j] = 1;
                ++nesting_depth[i];
            }
    // The innermost of the walls containing this one, which is the deepest of them, is its parent.
    for (size_t i = 0; i < wall_count; ++i)
        for (size_t j = 0; j < wall_count; ++j)
            if (inside[i * wall_count + j] && (parent[i] < 0 || nesting_depth[parent[i]] < nesting_depth[j]))
                parent[i] = int(j);

    std::vector<const Arachne::ExtrusionLine*> ordered;
    Polygons                                   outlines;
    ordered.reserve(wall_count);
    outlines.reserve(wall_count);
    std::function<void(int)> descend = [&](int idx) {
        ordered.emplace_back(walls[idx]);
        outlines.emplace_back(wall_outlines[idx]);
        for (size_t k = 0; k < wall_count; ++k)
            if (parent[k] == idx)
                descend(int(k));
    };
    for (size_t i = 0; i < wall_count; ++i)
        if (parent[i] < 0)
            descend(int(i));

    ThickPolylines spiral_result =
        generate_spiral_insets<Arachne::ExtrusionLine, ThickPolyline>(params, ordered, outlines, min_spacing, expolygon);

    append(thick_polylines_out, std::move(spiral_result));
    append(thick_polylines_out, std::move(open_walls));
}

} // namespace Slic3r
