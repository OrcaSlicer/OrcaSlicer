#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "../ClipperUtils.hpp"
#include "../Surface.hpp"

#include "FillScales.hpp"

namespace Slic3r {

namespace {

// One visible arc of a scale, in units of the distance between neighbouring lines.
struct MotifArc
{
    double radius;
    double begin;
    double end;
};

// Angular interval of the circle of radius r about the origin that lies inside the disc of radius
// disc_r centred at c. Returns false if the circle is not touched at all.
bool hidden_span(double r, const Vec2d &c, double disc_r, double &begin, double &end)
{
    const double d = c.norm();
    if (d < EPSILON || d >= r + disc_r)
        return false;
    const double cos_half = (r * r + d * d - disc_r * disc_r) / (2. * r * d);
    if (cos_half >= 1.)
        return false;
    const double half = cos_half <= -1. ? M_PI : std::acos(cos_half);
    const double mid  = std::atan2(c.y(), c.x());
    begin = mid - half;
    end   = mid + half;
    return true;
}

// The arcs of one scale, in units of the distance between neighbouring lines, in ascending radius.
//
// Density falls out of the construction rather than needing a constant: the visible parts of the
// discs tile the plane, so the visible angular extent phi(r) satisfies integral of phi(r) * r dr
// over [0, R] = the area of one lattice cell. Arcs spaced one unit apart therefore have a total
// length of one cell area per unit, i.e. filled area / path length = the distance between
// neighbouring lines, exactly as required. Radii sit at k + 1/2 to make that a midpoint rule.
std::vector<MotifArc> build_motif(size_t arcs)
{
    const double radius  = double(arcs);
    const double pitch_x = std::sqrt(3.) * radius;
    const double pitch_y = 0.5 * radius;

    // Every disc in a lower row close enough to hide part of this one. Discs in the same row need
    // no tie-break: at these proportions their overlap is covered by the disc in front of both.
    std::vector<Vec2d> occluders;
    for (int j = -1; double(-j) * pitch_y < 2. * radius; -- j)
        for (int i = -2; i <= 2; ++ i) {
            const Vec2d c(double(i) * pitch_x + (j % 2 != 0 ? 0.5 * pitch_x : 0.), double(j) * pitch_y);
            if (c.norm() < 2. * radius)
                occluders.emplace_back(c);
        }

    std::vector<MotifArc> motif;
    std::vector<std::pair<double, double>> hidden;
    for (size_t k = 0; k < arcs; ++ k) {
        const double r = double(k) + 0.5;
        hidden.clear();
        bool covered = false;
        for (const Vec2d &c : occluders) {
            double begin, end;
            if (! hidden_span(r, c, radius, begin, end))
                continue;
            if (end - begin >= 2. * M_PI) {
                covered = true;
                break;
            }
            const double b = std::fmod(std::fmod(begin, 2. * M_PI) + 2. * M_PI, 2. * M_PI);
            const double e = std::fmod(std::fmod(end,   2. * M_PI) + 2. * M_PI, 2. * M_PI);
            if (e < b) {
                hidden.emplace_back(b, 2. * M_PI);
                hidden.emplace_back(0., e);
            } else
                hidden.emplace_back(b, e);
        }
        if (covered)
            continue;
        // Sweep the complement. The visible arc is symmetric about +Y and spans less than 120
        // degrees, so it never straddles the 0/2pi seam and always comes out as a single piece.
        std::sort(hidden.begin(), hidden.end());
        double cursor = 0.;
        for (const std::pair<double, double> &span : hidden) {
            if (span.first > cursor)
                motif.push_back({ r, cursor, span.first });
            cursor = std::max(cursor, span.second);
        }
        if (cursor < 2. * M_PI)
            motif.push_back({ r, cursor, 2. * M_PI });
    }
    return motif;
}

// True if the arc turns counter-clockwise about its own centre. A fragment clipped down to a
// straight line has no direction to speak of and reports false.
bool sweeps_ccw(const Polyline &pl)
{
    if (pl.points.size() < 3)
        return false;
    const Vec2d a = pl.points.front().cast<double>();
    const Vec2d b = pl.points[pl.points.size() / 2].cast<double>();
    const Vec2d c = pl.points.back().cast<double>();
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()) > 0.;
}

// Discretize one arc, keeping the chord deviation below tolerance.
Polyline arc_polyline(const Point &center, double radius, double begin, double end, double tolerance)
{
    const double step     = 2. * std::acos(std::max(-1., 1. - tolerance / radius));
    const size_t segments = std::max<size_t>(2, size_t(std::ceil((end - begin) / step)));
    Polyline pl;
    pl.points.reserve(segments + 1);
    for (size_t i = 0; i <= segments; ++ i) {
        const double a = begin + (end - begin) * double(i) / double(segments);
        pl.points.emplace_back(center.x() + coord_t(std::cos(a) * radius),
                               center.y() + coord_t(std::sin(a) * radius));
    }
    return pl;
}

} // namespace

void FillScales::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     /* thickness_layers */,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // Distance between neighbouring lines. The scale radius is m_arcs of them, so lowering the
    // surface density opens the lines up and grows the scales with them.
    const double  distance = double(scale_(this->spacing)) / double(params.density);
    const coord_t radius   = coord_t(distance * double(m_arcs));
    const coord_t pitch_x  = coord_t(distance * double(m_arcs) * std::sqrt(3.));
    const coord_t pitch_y  = radius / 2;

    BoundingBox bounding_box = expolygon.contour.bounding_box();
    {
        // Rotate the bounding box according to the infill direction; the arcs are generated
        // axis-aligned and rotated back below.
        Polygon bb_polygon = bounding_box.polygon();
        bb_polygon.rotate(direction.first);
        bounding_box = bb_polygon.bounding_box();
    }
    // A scale reaches one radius away from its centre, so centres just outside still contribute.
    bounding_box.offset(radius);
    // Rows alternate their x offset, so the lattice repeats every two rows. Aligning it to a world
    // grid keeps the pattern in register across layers and across separate fill surfaces.
    const Point origin = align_to_grid(bounding_box.min, Point(pitch_x, 2 * pitch_y));

    const std::vector<MotifArc> motif     = build_motif(m_arcs);
    // Keep a floor under the tolerance: a resolution of zero would discretize the arcs to death.
    const double                tolerance = std::max(scaled<double>(params.resolution), scaled<double>(0.001));

    // One arc of the motif, repeated across the whole lattice.
    auto append_pass = [&](const MotifArc &arc, Polylines &out) {
        for (coord_t y = origin.y(); y <= bounding_box.max.y(); y += pitch_y) {
            const bool    odd_row = ((y - origin.y()) / pitch_y) & 1;
            const coord_t first_x = origin.x() + (odd_row ? pitch_x / 2 : 0);
            for (coord_t x = first_x; x <= bounding_box.max.x(); x += pitch_x)
                out.emplace_back(arc_polyline(Point(x, y), arc.radius * distance, arc.begin, arc.end, tolerance));
        }
    };

    if (params.fill_order == SurfaceFillOrder::Default) {
        Polylines all_polylines;
        for (const MotifArc &arc : motif)
            append_pass(arc, all_polylines);
        for (Polyline &pl : all_polylines)
            pl.rotate(-direction.first);
        all_polylines = intersection_pl(std::move(all_polylines), expolygon);
        chain_or_connect_infill(std::move(all_polylines), expolygon, polylines_out, this->spacing, params);
        return;
    }

    // A forced fill order sweeps the whole surface once per arc, so every scale is at the same
    // depth at any moment and flow variation shows up as a change across the surface rather than as
    // a difference between neighbouring scales. Inward starts on the outermost arcs. This costs a
    // travel move between scales for every arc, which is why it is not the default.
    //
    // The clip has to happen per pass: intersection_pl returns a Clipper PolyTree, whose order does
    // not track the input, so sorting a single clipped batch by radius afterwards is not possible.
    const bool outermost_first = params.fill_order == SurfaceFillOrder::Inward;
    for (size_t i = 0; i < motif.size(); ++ i) {
        // build_motif() emits in ascending radius.
        const MotifArc &arc = motif[outermost_first ? motif.size() - 1 - i : i];
        Polylines pass;
        append_pass(arc, pass);
        // The motif arc sweeps counter-clockwise, so it runs right to left - against the order the
        // lattice is walked in, which would put a travel the length of an arc between every pair of
        // neighbouring scales. Flip it to match.
        for (Polyline &pl : pass) {
            pl.reverse();
            pl.rotate(-direction.first);
        }
        pass = intersection_pl(std::move(pass), expolygon);
        // Deliberately not chained: chain_polylines() reverses paths to shorten travel, and Clipper
        // is free to hand an open path back either way round. Every scale has to be drawn in the
        // same direction, otherwise the start-of-extrusion blob lands at the left of one scale and
        // the right of its neighbour, which is the artefact a forced order exists to prevent.
        for (Polyline &pl : pass)
            if (sweeps_ccw(pl))
                pl.reverse();
        append(polylines_out, std::move(pass));
    }
}

} // namespace Slic3r
