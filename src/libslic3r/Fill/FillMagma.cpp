#include "FillMagma.hpp"
#include "FillBase.hpp"
#include "../ClipperUtils.hpp"
#include "../Polyline.hpp"
#include "../Magma/MagmaTubeMap.hpp"
#include "../Magma/MagmaSpiralOffset.hpp"

#include <cmath>
#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <boost/log/trivial.hpp>

namespace Slic3r {

// ============================================================================
// Infill Direction — fixed angle, world-origin reference
// ============================================================================

std::pair<float, Point> FillMagmaTriangle::_infill_direction(const Surface *surface) const
{
    float out_angle = 0.f;
    if (surface->bridge_angle >= 0)
        out_angle = float(surface->bridge_angle);
    return std::make_pair(out_angle, Point(0, 0));
}

// ============================================================================
// Helpers
// ============================================================================

// Create a 2-point polyline from world coordinates (mm) for X, scaled Y.
static Polyline make_horiz_segment(double x0_mm, double x1_mm, coord_t y_scaled)
{
    Polyline pl;
    pl.points.push_back(Point(coord_t(scale_(x0_mm)), y_scaled));
    pl.points.push_back(Point(coord_t(scale_(x1_mm)), y_scaled));
    return pl;
}

// Subtract sorted, merged gap intervals from a range [lo, hi].
// Emits kept segments to `out` via the callback.
template<typename EmitFn>
// `eps` is the shortest run worth emitting, IN THE CALLER'S UNITS. It used to be a
// hardcoded 0.01, which is 0.01mm for the world-space callers but 1% of the whole line for
// the parametric one below -- so on a part more than ~100 rows tall the wall stub between two
// windows on the same line was silently deleted and the two windows merged into a single
// opening, cross-connecting two U-tubes. Pass it explicitly so the unit is never in doubt.
static void subtract_gaps(double lo, double hi,
                          const std::vector<std::pair<double, double>> &gaps,
                          double eps,
                          EmitFn emit)
{
    double cursor = lo;
    for (const auto &gap : gaps) {
        double gl = std::max(gap.first, lo);
        double gr = std::min(gap.second, hi);
        if (gr <= gl)
            continue;
        if (gl > cursor + eps)
            emit(cursor, gl);
        cursor = std::max(cursor, gr);
    }
    if (hi > cursor + eps)
        emit(cursor, hi);
}

// Split a line segment p0→p1 by removing Y intervals where gaps exist.
// Gaps are in world-space Y coordinates. The line must have non-zero Y span.
static void split_line_by_y_gaps(
    const Vec2d &p0, const Vec2d &p1,
    const std::vector<std::pair<double, double>> &y_gaps,
    Polylines &out)
{
    double dy = p1.y() - p0.y();
    if (std::abs(dy) < 1e-6) {
        // Degenerate — no Y span, can't split by Y
        Polyline pl;
        pl.points.push_back(Point(scale_(p0.x()), scale_(p0.y())));
        pl.points.push_back(Point(scale_(p1.x()), scale_(p1.y())));
        out.push_back(std::move(pl));
        return;
    }

    double y_lo = std::min(p0.y(), p1.y());
    double y_hi = std::max(p0.y(), p1.y());

    // Convert Y intervals to parametric t values along p0→p1
    // P(t) = p0 + t*(p1 - p0), t ∈ [0, 1]
    // t = (y - p0.y) / dy
    auto y_to_t = [&](double y) { return (y - p0.y()) / dy; };
    auto t_to_point = [&](double t) -> Vec2d {
        return Vec2d(p0.x() + t * (p1.x() - p0.x()),
                     p0.y() + t * dy);
    };

    // Collect gap t-intervals
    std::vector<std::pair<double, double>> t_gaps;
    for (const auto &gap : y_gaps) {
        double gl = std::max(gap.first, y_lo);
        double gr = std::min(gap.second, y_hi);
        if (gr <= gl)
            continue;
        double t0 = y_to_t(gl);
        double t1 = y_to_t(gr);
        if (t0 > t1) std::swap(t0, t1);
        t_gaps.push_back({t0, t1});
    }

    if (t_gaps.empty()) {
        Polyline pl;
        pl.points.push_back(Point(scale_(p0.x()), scale_(p0.y())));
        pl.points.push_back(Point(scale_(p1.x()), scale_(p1.y())));
        out.push_back(std::move(pl));
        return;
    }

    std::sort(t_gaps.begin(), t_gaps.end());

    // t is normalised over the whole line, so express the 0.01mm world threshold in t.
    const double seg_len_mm = (p1 - p0).norm();
    const double t_eps      = seg_len_mm > 1e-9 ? (0.01 / seg_len_mm) : 0.0;
    subtract_gaps(0.0, 1.0, t_gaps, t_eps, [&](double t_lo, double t_hi) {
        Vec2d a = t_to_point(t_lo);
        Vec2d b = t_to_point(t_hi);
        Polyline pl;
        pl.points.push_back(Point(scale_(a.x()), scale_(a.y())));
        pl.points.push_back(Point(scale_(b.x()), scale_(b.y())));
        out.push_back(std::move(pl));
    });
}

// ============================================================================
// Per-shape window-cut machinery
//
// The tube map is shape-agnostic: it only knows which (cell_a, cell_b) pairs
// have an open window on a given layer (window_open_at). Each per-shape toolpath
// computes its own line-interruption geometry from that pair list. Triangle and
// square shapes interrupt different line families, so each owns its own cut code.
// ============================================================================

// Merge overlapping/adjacent intervals in a sorted vector.
static void merge_intervals(std::vector<std::pair<double, double>> &intervals)
{
    if (intervals.size() <= 1)
        return;
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<double, double>> merged;
    merged.push_back(intervals[0]);
    for (size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].first <= merged.back().second + 0.01)
            merged.back().second = std::max(merged.back().second, intervals[i].second);
        else
            merged.push_back(intervals[i]);
    }
    intervals = std::move(merged);
}

// Which edge two adjacent triangle cells share.
enum class SharedEdge { Horizontal, Col60, Diag120 };

// Determine shared edge type between two adjacent cells.
// Cells differ in exactly one coordinate (a, b, or c).
inline SharedEdge shared_edge(const magma::CellId &a, const magma::CellId &b) {
    if (a.a != b.a) return SharedEdge::Col60;       // differ in a → 60° edge
    if (a.b != b.b) return SharedEdge::Horizontal;   // differ in b → horizontal
    return SharedEdge::Diag120;                       // differ in c → 120° edge
}

// World-space line-interruption intervals for a layer, organized by line family.
// Each map key is the line index (row, column, or diagonal); each value is a
// sorted, merged list of world-coordinate intervals where lines are interrupted.
struct MagmaWindowCuts {
    std::map<int, std::vector<std::pair<double, double>>> horiz, col60, diag120, vert;
};

// Triangle window cuts: relocated verbatim from MagmaTubeMap's
// compute_window_gaps_for_layer. Same arithmetic, byte-identical output.
static MagmaWindowCuts triangle_window_cuts(const magma::MagmaTubeMap &tm, int layer,
                                            const magma::MagmaLattice &lat)
{
    MagmaWindowCuts cuts;

    // For each pair with an open window on this layer, determine the shared
    // edge between cell_a and cell_b and add a gap on the correct line family.
    for (const auto &pair : tm.u_tube_pairs()) {
        if (!tm.window_open_at(pair, layer))
            continue;

        SharedEdge edge = shared_edge(pair.cell_a, pair.cell_b);

        switch (edge) {
        case SharedEdge::Horizontal: {
            const magma::CellId &up = lat.is_up(pair.cell_a) ? pair.cell_a : pair.cell_b;
            int row = up.b;
            Vec2d v0 = lat.to_world(up.a, row);
            Vec2d v1 = lat.to_world(up.a + 1, row);
            double x_lo = std::min(v0.x(), v1.x());
            double x_hi = std::max(v0.x(), v1.x());
            cuts.horiz[row].push_back({x_lo, x_hi});
            break;
        }
        case SharedEdge::Col60: {
            int col = std::max(pair.cell_a.a, pair.cell_b.a);
            int row = std::min(pair.cell_a.b, pair.cell_b.b);
            Vec2d v0 = lat.to_world(col, row);
            Vec2d v1 = lat.to_world(col, row + 1);
            double y_lo = std::min(v0.y(), v1.y());
            double y_hi = std::max(v0.y(), v1.y());
            cuts.col60[col].push_back({y_lo, y_hi});
            break;
        }
        case SharedEdge::Diag120: {
            const magma::CellId &up = lat.is_up(pair.cell_a) ? pair.cell_a : pair.cell_b;
            int diag = up.a + up.b + 1;
            Vec2d v0 = lat.to_world(up.a + 1, up.b);
            Vec2d v1 = lat.to_world(up.a, up.b + 1);
            double y_lo = std::min(v0.y(), v1.y());
            double y_hi = std::max(v0.y(), v1.y());
            cuts.diag120[diag].push_back({y_lo, y_hi});
            break;
        }
        }
    }

    // Sort and merge intervals per line index
    for (auto &[key, intervals] : cuts.horiz)
        merge_intervals(intervals);
    for (auto &[key, intervals] : cuts.col60)
        merge_intervals(intervals);
    for (auto &[key, intervals] : cuts.diag120)
        merge_intervals(intervals);

    return cuts;
}

// Square window cuts: each open pair shares either a vertical edge (cells differ
// in column) or a horizontal edge (cells differ in row).
static MagmaWindowCuts square_window_cuts(const magma::MagmaTubeMap &tm, int layer,
                                          const magma::MagmaLattice &lat)
{
    MagmaWindowCuts cuts;

    for (const auto &pair : tm.u_tube_pairs()) {
        if (!tm.window_open_at(pair, layer))
            continue;

        const auto &ca = pair.cell_a, &cb = pair.cell_b;
        if (cb.a != ca.a) {
            int col = std::max(ca.a, cb.a);
            int row = ca.b;
            Vec2d v0 = lat.to_world(col, row), v1 = lat.to_world(col, row + 1);
            cuts.vert[col].push_back({std::min(v0.y(), v1.y()), std::max(v0.y(), v1.y())});
        } else {
            int row = std::max(ca.b, cb.b);
            int col = ca.a;
            Vec2d v0 = lat.to_world(col, row), v1 = lat.to_world(col + 1, row);
            cuts.horiz[row].push_back({std::min(v0.x(), v1.x()), std::max(v0.x(), v1.x())});
        }
    }

    for (auto &[key, intervals] : cuts.horiz)
        merge_intervals(intervals);
    for (auto &[key, intervals] : cuts.vert)
        merge_intervals(intervals);

    return cuts;
}

// ============================================================================
// Main Fill — Direct Lattice Generation
// ============================================================================

void FillMagmaTriangle::_fill_surface_single(
    const FillParams &params,
    unsigned int       thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon          expolygon,
    Polylines         &polylines_out)
{
    if (!this->tube_map) {
        BOOST_LOG_TRIVIAL(error) << "FillMagmaTriangle: null tube_map on layer " << this->layer_id;
        return;
    }

    // Disable anchors for Magma infill: zone shells provide the bonding
    // surface, and anchors fill boundary cell interiors causing bad injections.
    FillParams no_anchor_params = params;
    no_anchor_params.anchor_length     = 0.f;
    no_anchor_params.anchor_length_max = 0.f;

    const double cs   = this->tube_map->cell_spacing();
    const int    layer = static_cast<int>(this->layer_id);

    // Use cached lattice with spiral offset for this layer
    const magma::MagmaLattice &lattice = this->tube_map->lattice_at(layer);

    const double edge  = lattice.edge_length();
    const double off_x = lattice.offset_x();
    const double off_y = lattice.offset_y();

    // Compute this shape's window cuts from the tube map's pair list.
    MagmaWindowCuts gaps = triangle_window_cuts(*this->tube_map, layer, lattice);

    // ---- Bounding box → lattice ranges ----

    BoundingBox bbox = expolygon.contour.bounding_box();
    double x_min = unscale<double>(bbox.min.x()) - edge;
    double x_max = unscale<double>(bbox.max.x()) + edge;
    double y_min = unscale<double>(bbox.min.y()) - cs;
    double y_max = unscale<double>(bbox.max.y()) + cs;

    // Row range (horizontal lines, constant b)
    int row_min = static_cast<int>(std::floor((y_min - off_y) / cs));
    int row_max = static_cast<int>(std::ceil((y_max - off_y) / cs));

    // Column range (60° lines, constant a)
    // to_world(a, b).x = a*edge + b*edge/2 + off_x
    // Conservative: use extreme b values that push x furthest
    int col_min = static_cast<int>(std::floor(
        (x_min - off_x - std::max(row_min, row_max) * edge * 0.5) / edge)) - 1;
    int col_max = static_cast<int>(std::ceil(
        (x_max - off_x - std::min(row_min, row_max) * edge * 0.5) / edge)) + 1;

    // Diagonal range (120° lines, constant s = a + b)
    int diag_min = col_min + row_min - 1;
    int diag_max = col_max + row_max + 1;

    // ---- Generate lines per direction, clip, and connect ----
    //
    // Each direction is generated, clipped per-line (so same-line fragments
    // stay adjacent), then passed through connect_infill for boundary
    // anchoring and perimeter-arc connections.  Processing per-direction
    // preserves the sweep order between directions while letting
    // connect_infill optimise routing within each parallel family.

    auto clip_lines = [&](Polylines &raw) -> Polylines {
        Polylines clipped;
        for (Polyline &pl : raw) {
            Polylines frags = intersection_pl(Polylines{std::move(pl)}, expolygon);
            append(clipped, std::move(frags));
        }
        return clipped;
    };

    // --- Horizontal lines (one per row b) ---
    {
        Polylines raw;
        for (int b = row_min; b <= row_max; ++b) {
            coord_t y_s = coord_t(scale_(b * cs + off_y));
            auto it = gaps.horiz.find(b);
            if (it == gaps.horiz.end()) {
                raw.push_back(make_horiz_segment(x_min, x_max, y_s));
            } else {
                subtract_gaps(x_min, x_max, it->second, 0.01, [&](double lo, double hi) {
                    raw.push_back(make_horiz_segment(lo, hi, y_s));
                });
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }

    // --- 60° lines (one per column a) ---
    {
        Polylines raw;
        for (int a = col_min; a <= col_max; ++a) {
            Vec2d p0 = lattice.to_world(a, row_min - 1);
            Vec2d p1 = lattice.to_world(a, row_max + 1);
            auto it = gaps.col60.find(a);
            if (it == gaps.col60.end()) {
                Polyline pl;
                pl.points.push_back(Point(scale_(p0.x()), scale_(p0.y())));
                pl.points.push_back(Point(scale_(p1.x()), scale_(p1.y())));
                raw.push_back(std::move(pl));
            } else {
                split_line_by_y_gaps(p0, p1, it->second, raw);
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }

    // --- 120° lines (one per diagonal s = a + b) ---
    {
        Polylines raw;
        for (int s = diag_min; s <= diag_max; ++s) {
            Vec2d p0 = lattice.to_world(s - (row_min - 1), row_min - 1);
            Vec2d p1 = lattice.to_world(s - (row_max + 1), row_max + 1);
            auto it = gaps.diag120.find(s);
            if (it == gaps.diag120.end()) {
                Polyline pl;
                pl.points.push_back(Point(scale_(p0.x()), scale_(p0.y())));
                pl.points.push_back(Point(scale_(p1.x()), scale_(p1.y())));
                raw.push_back(std::move(pl));
            } else {
                split_line_by_y_gaps(p0, p1, it->second, raw);
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }
}

// ============================================================================
// FillMagmaRectilinear — square grid (2 perpendicular single-wall families)
// ============================================================================

std::pair<float, Point> FillMagmaRectilinear::_infill_direction(const Surface *surface) const
{
    float out_angle = 0.f;
    if (surface->bridge_angle >= 0)
        out_angle = float(surface->bridge_angle);
    return std::make_pair(out_angle, Point(0, 0));
}

void FillMagmaRectilinear::_fill_surface_single(
    const FillParams &params,
    unsigned int /*thickness_layers*/,
    const std::pair<float, Point> & /*direction*/,
    ExPolygon          expolygon,
    Polylines         &polylines_out)
{
    if (!this->tube_map) {
        BOOST_LOG_TRIVIAL(error) << "FillMagmaRectilinear: null tube_map on layer " << this->layer_id;
        return;
    }

    FillParams no_anchor_params = params;
    no_anchor_params.anchor_length     = 0.f;
    no_anchor_params.anchor_length_max = 0.f;

    const double cs    = this->tube_map->cell_spacing();
    const int    layer = static_cast<int>(this->layer_id);

    // Cached lattice with this layer's spiral offset.
    const magma::MagmaLattice &lattice = this->tube_map->lattice_at(layer);
    const double off_x = lattice.offset_x();
    const double off_y = lattice.offset_y();

    // Compute this shape's window cuts from the tube map's pair list.
    MagmaWindowCuts gaps = square_window_cuts(*this->tube_map, layer, lattice);

    BoundingBox bbox = expolygon.contour.bounding_box();
    double x_min = unscale<double>(bbox.min.x()) - cs;
    double x_max = unscale<double>(bbox.max.x()) + cs;
    double y_min = unscale<double>(bbox.min.y()) - cs;
    double y_max = unscale<double>(bbox.max.y()) + cs;

    // Square grid: lines spaced cs apart in both axes (no skew).
    int row_min = static_cast<int>(std::floor((y_min - off_y) / cs));
    int row_max = static_cast<int>(std::ceil ((y_max - off_y) / cs));
    int col_min = static_cast<int>(std::floor((x_min - off_x) / cs));
    int col_max = static_cast<int>(std::ceil ((x_max - off_x) / cs));

    auto clip_lines = [&](Polylines &raw) -> Polylines {
        Polylines clipped;
        for (Polyline &pl : raw) {
            Polylines frags = intersection_pl(Polylines{std::move(pl)}, expolygon);
            append(clipped, std::move(frags));
        }
        return clipped;
    };

    // --- Horizontal lines (one per row b): y = b*cs + off_y ---
    {
        Polylines raw;
        for (int b = row_min; b <= row_max; ++b) {
            coord_t y_s = coord_t(scale_(b * cs + off_y));
            auto it = gaps.horiz.find(b);
            if (it == gaps.horiz.end()) {
                raw.push_back(make_horiz_segment(x_min, x_max, y_s));
            } else {
                subtract_gaps(x_min, x_max, it->second, 0.01, [&](double lo, double hi) {
                    raw.push_back(make_horiz_segment(lo, hi, y_s));
                });
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }

    // --- Vertical lines (one per column a): x = a*cs + off_x ---
    {
        Polylines raw;
        for (int a = col_min; a <= col_max; ++a) {
            coord_t x_s = coord_t(scale_(a * cs + off_x));
            auto it = gaps.vert.find(a);
            if (it == gaps.vert.end()) {
                Polyline pl;
                pl.points.push_back(Point(x_s, coord_t(scale_(y_min))));
                pl.points.push_back(Point(x_s, coord_t(scale_(y_max))));
                raw.push_back(std::move(pl));
            } else {
                subtract_gaps(y_min, y_max, it->second, 0.01, [&](double lo, double hi) {
                    Polyline pl;
                    pl.points.push_back(Point(x_s, coord_t(scale_(lo))));
                    pl.points.push_back(Point(x_s, coord_t(scale_(hi))));
                    raw.push_back(std::move(pl));
                });
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }
}

// ============================================================================
// FillMagmaTriHex — trihexagonal (hex hubs + up/down triangle vents)
// ============================================================================
//
// The trihexagonal walls are three straight line families at 0/60/120 degrees — the SAME
// generator as FillMagmaTriangle, but at HALF-integer lattice indices. The trihex tiling is
// the rectified triangular grid, so its walls are the triangle pattern's three families shifted
// by 1/2 in each index: horizontal at ly = k+0.5, the 60 deg family at lx = m+0.5, the 120 deg
// family at lx+ly = n+0.5. That half-shift is exactly what opens the hub hexagons: no family
// line passes through a hub vertex (integer ly/lx) or a vent centroid (thirds), so both stay
// hollow, while every wall lies on exactly one family. Each open hub<->vent window is one wall
// segment on one family; trihex_window_cuts classifies it and cuts the gap, then we generate
// full lines per family, clip per line, and chain each family — long sweeps in three directions.

// Tri-hex window cuts: classify each open hub<->vent shared edge into its line family and record
// the interruption interval (x-interval for the horizontal family, y-interval for 60/120), the
// trihexagonal analogue of triangle_window_cuts.
static MagmaWindowCuts trihex_window_cuts(const magma::MagmaTubeMap &tm, int layer,
                                          const magma::MagmaLattice &lat)
{
    MagmaWindowCuts cuts;
    auto add_edge = [&](const Vec2d &P0, const Vec2d &P1) {
        auto [lx0, ly0] = lat.to_lattice(P0.x(), P0.y());
        auto [lx1, ly1] = lat.to_lattice(P1.x(), P1.y());
        if (std::abs(ly0 - ly1) < 1e-3) {                       // horizontal family (const ly)
            int k = int(std::lround(0.5 * (ly0 + ly1) - 0.5));
            cuts.horiz[k].push_back({ std::min(P0.x(), P1.x()), std::max(P0.x(), P1.x()) });
        } else if (std::abs(lx0 - lx1) < 1e-3) {                // 60 deg family (const lx)
            int mcol = int(std::lround(0.5 * (lx0 + lx1) - 0.5));
            cuts.col60[mcol].push_back({ std::min(P0.y(), P1.y()), std::max(P0.y(), P1.y()) });
        } else {                                                // 120 deg family (const lx+ly)
            int n = int(std::lround(0.5 * ((lx0 + ly0) + (lx1 + ly1)) - 0.5));
            cuts.diag120[n].push_back({ std::min(P0.y(), P1.y()), std::max(P0.y(), P1.y()) });
        }
    };
    for (const auto &pair : tm.u_tube_pairs()) {
        if (!tm.window_open_at(pair, layer))
            continue;
        const std::vector<Vec2d> hub = lat.cell_corners(pair.cell_a);
        auto cut_leg = [&](const magma::CellId &leg) {
            const std::vector<Vec2d> lc = lat.cell_corners(leg);
            // A hub and a vent share exactly one wall = the two corners common to both rings.
            std::vector<Vec2d> shared;
            for (const Vec2d &h : hub)
                for (const Vec2d &l : lc)
                    if ((h - l).squaredNorm() < 1e-6) { shared.push_back(h); break; }
            if (shared.size() >= 2)
                add_edge(shared[0], shared[1]);
        };
        cut_leg(pair.cell_b);
        for (const magma::CellId &ev : pair.extra_vents)
            cut_leg(ev);
    }
    for (auto &kv : cuts.horiz)   merge_intervals(kv.second);
    for (auto &kv : cuts.col60)   merge_intervals(kv.second);
    for (auto &kv : cuts.diag120) merge_intervals(kv.second);
    return cuts;
}

std::pair<float, Point> FillMagmaTriHex::_infill_direction(const Surface *surface) const
{
    float out_angle = 0.f;
    if (surface->bridge_angle >= 0)
        out_angle = float(surface->bridge_angle);
    return std::make_pair(out_angle, Point(0, 0));
}

void FillMagmaTriHex::_fill_surface_single(
    const FillParams &params,
    unsigned int /*thickness_layers*/,
    const std::pair<float, Point> & /*direction*/,
    ExPolygon          expolygon,
    Polylines         &polylines_out)
{
    if (!this->tube_map) {
        BOOST_LOG_TRIVIAL(error) << "FillMagmaTriHex: null tube_map on layer " << this->layer_id;
        return;
    }

    FillParams no_anchor_params = params;
    no_anchor_params.anchor_length     = 0.f;
    no_anchor_params.anchor_length_max = 0.f;

    const double s     = this->tube_map->cell_spacing();
    const int    layer = static_cast<int>(this->layer_id);
    const magma::MagmaLattice &lattice = this->tube_map->lattice_at(layer);
    const double off_y = lattice.offset_y();

    MagmaWindowCuts gaps = trihex_window_cuts(*this->tube_map, layer, lattice);

    // Bounding box -> half-integer lattice index ranges for the 3 families. The lattice is
    // skewed, so map all four bbox corners and take the spanning (ly, lx, lx+ly) box.
    BoundingBox bbox = expolygon.contour.bounding_box();
    const double x_min = unscale<double>(bbox.min.x()) - s, x_max = unscale<double>(bbox.max.x()) + s;
    const double y_min = unscale<double>(bbox.min.y()) - s, y_max = unscale<double>(bbox.max.y()) + s;
    double lxv[4], lyv[4];
    const double cx[4] = { x_min, x_max, x_min, x_max };
    const double cy[4] = { y_min, y_min, y_max, y_max };
    for (int i = 0; i < 4; ++i) { auto p = lattice.to_lattice(cx[i], cy[i]); lxv[i] = p.first; lyv[i] = p.second; }
    const double ly_min = *std::min_element(lyv, lyv + 4), ly_max = *std::max_element(lyv, lyv + 4);
    const double lx_min = *std::min_element(lxv, lxv + 4), lx_max = *std::max_element(lxv, lxv + 4);
    double n_min = lxv[0] + lyv[0], n_max = n_min;
    for (int i = 1; i < 4; ++i) { double n = lxv[i] + lyv[i]; n_min = std::min(n_min, n); n_max = std::max(n_max, n); }

    const int row_min  = int(std::floor(ly_min - 0.5)) - 1, row_max  = int(std::ceil(ly_max - 0.5)) + 1;
    const int col_min  = int(std::floor(lx_min - 0.5)) - 1, col_max  = int(std::ceil(lx_max - 0.5)) + 1;
    const int diag_min = int(std::floor(n_min  - 0.5)) - 1, diag_max = int(std::ceil(n_max  - 0.5)) + 1;
    // 60/120 deg lines run between these ly extremes (a touch past the bbox).
    const double ly_lo = ly_min - 1.0, ly_hi = ly_max + 1.0;

    auto clip_lines = [&](Polylines &raw) -> Polylines {
        Polylines clipped;
        for (Polyline &pl : raw) {
            Polylines frags = intersection_pl(Polylines{ std::move(pl) }, expolygon);
            append(clipped, std::move(frags));
        }
        return clipped;
    };

    // --- Horizontal family: y = (k + 0.5)*s + off_y ---
    {
        Polylines raw;
        for (int k = row_min; k <= row_max; ++k) {
            coord_t y_s = coord_t(scale_((k + 0.5) * s + off_y));
            auto it = gaps.horiz.find(k);
            if (it == gaps.horiz.end())
                raw.push_back(make_horiz_segment(x_min, x_max, y_s));
            else
                subtract_gaps(x_min, x_max, it->second, 0.01, [&](double lo, double hi) {
                    raw.push_back(make_horiz_segment(lo, hi, y_s));
                });
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }

    // --- 60 deg family: one line per column lx = m + 0.5 ---
    {
        Polylines raw;
        for (int mcol = col_min; mcol <= col_max; ++mcol) {
            Vec2d p0 = lattice.to_world(mcol + 0.5, ly_lo);
            Vec2d p1 = lattice.to_world(mcol + 0.5, ly_hi);
            auto it = gaps.col60.find(mcol);
            if (it == gaps.col60.end()) {
                Polyline pl;
                pl.points.push_back(Point(scale_(p0.x()), scale_(p0.y())));
                pl.points.push_back(Point(scale_(p1.x()), scale_(p1.y())));
                raw.push_back(std::move(pl));
            } else {
                split_line_by_y_gaps(p0, p1, it->second, raw);
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }

    // --- 120 deg family: one line per diagonal lx + ly = n + 0.5 ---
    {
        Polylines raw;
        for (int n = diag_min; n <= diag_max; ++n) {
            Vec2d p0 = lattice.to_world((n + 0.5) - ly_lo, ly_lo);
            Vec2d p1 = lattice.to_world((n + 0.5) - ly_hi, ly_hi);
            auto it = gaps.diag120.find(n);
            if (it == gaps.diag120.end()) {
                Polyline pl;
                pl.points.push_back(Point(scale_(p0.x()), scale_(p0.y())));
                pl.points.push_back(Point(scale_(p1.x()), scale_(p1.y())));
                raw.push_back(std::move(pl));
            } else {
                split_line_by_y_gaps(p0, p1, it->second, raw);
            }
        }
        Polylines clipped = clip_lines(raw);
        if (!clipped.empty())
            chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
    }
}

// ============================================================================
// FillMagmaHoneycomb — pure honeycomb (verification: deduped hex outlines)
// ============================================================================

std::pair<float, Point> FillMagmaHoneycomb::_infill_direction(const Surface *surface) const
{
    float out_angle = 0.f;
    if (surface->bridge_angle >= 0)
        out_angle = float(surface->bridge_angle);
    return std::make_pair(out_angle, Point(0, 0));
}

void FillMagmaHoneycomb::_fill_surface_single(
    const FillParams &params,
    unsigned int /*thickness_layers*/,
    const std::pair<float, Point> & /*direction*/,
    ExPolygon          expolygon,
    Polylines         &polylines_out)
{
    if (!this->tube_map) {
        BOOST_LOG_TRIVIAL(error) << "FillMagmaHoneycomb: null tube_map on layer " << this->layer_id;
        return;
    }

    FillParams no_anchor_params = params;
    no_anchor_params.anchor_length     = 0.f;
    no_anchor_params.anchor_length_max = 0.f;

    const int    layer = static_cast<int>(this->layer_id);
    const double s     = this->tube_map->cell_spacing();
    const double iw    = std::max(0.0, double(this->tube_map->interior_width()));  // open f2f (user spec)
    const double lw    = std::max(0.0, s - iw);          // line width = cell_spacing - interior
    // Build the toolpath from the OPEN hexagon (edge e = interior/√3) so the printed tube is the
    // requested interior width; the doubled vertical walls + single slants are added on top,
    // matching the lattice (MagmaHexCell.hpp) so windows/injection land on the drawn walls.
    const double e     = iw * magma::INV_SQRT3;          // OPEN hex edge = interior/√3
    const double x_off = std::min(lw * 0.5, s * 0.2);    // half-gap of the doubled vertical walls
    const magma::MagmaLattice &lattice = this->tube_map->lattice_at(layer);
    const double ox = lattice.offset_x(), oy = lattice.offset_y();

    BoundingBox bbox = expolygon.contour.bounding_box();
    const double x0 = unscale<double>(bbox.min.x()) - s, x1 = unscale<double>(bbox.max.x()) + s;
    const double y0 = unscale<double>(bbox.min.y()) - s, y1 = unscale<double>(bbox.max.y()) + s;

    // Fast continuous honeycomb sweep (Orca-style), matching the lattice. The vertical edges
    // fall on lanes x = ox + k*(m_sx/2), m_sx = interior + 2*lw; the vertices and rows are
    // extended in Y by lw/sqrt3 so the open hexagon is regular (see MagmaHexCell.hpp). One
    // continuous zigzag per lane PAIR oscillates between lanes k and k+1: a vertical (length e),
    // a slant up to the next lane's vertical (whose bottom == the hexagon vertex), its vertical,
    // a slant back — period 2*row in y, phased at oy + row*(k-1). Each lane's verticals are swept
    // by both neighbouring pairs (doubled vertical walls); slants stay single. Fast, low-travel.
    const double row   = 1.5 * e + lw * magma::INV_SQRT3;   // lattice row spacing (Y)
    const double half  = (iw + 2.0 * lw) * 0.5;             // lane pitch = m_sx/2 = (interior + 2lw)/2
    const int    k_min = int(std::floor((x0 - ox) / half)) - 1;
    const int    k_max = int(std::ceil ((x1 - ox) / half)) + 1;
    Polylines raw;
    for (int k = k_min; k <= k_max; ++k) {
        const double xL  = ox + double(k)     * half + x_off;
        const double xR  = ox + double(k + 1) * half - x_off;
        const double phi = oy + row * double(k - 1);
        const int j_min = int(std::floor((y0 - phi) / (2.0 * row))) - 1;
        const int j_max = int(std::ceil ((y1 - phi) / (2.0 * row))) + 1;
        Polyline pl;
        for (int j = j_min; j <= j_max; ++j) {
            const double b = phi + 2.0 * row * double(j);
            pl.points.push_back(Point(scale_(xL), scale_(b - e * 0.5)));
            pl.points.push_back(Point(scale_(xL), scale_(b + e * 0.5)));
            pl.points.push_back(Point(scale_(xR), scale_(b + row - e * 0.5)));
            pl.points.push_back(Point(scale_(xR), scale_(b + row + e * 0.5)));
        }
        if (pl.points.size() >= 2) raw.push_back(std::move(pl));
    }

    // Windows: subtract each open pair's shared wall (a rectangle over the shared edge,
    // wide enough to span both doubled verticals, shortened by a bead so the corners stay).
    Polygons window_cuts;
    for (const magma::UTubePair &pr : this->tube_map->u_tube_pairs()) {
        if (!this->tube_map->window_open_at(pr, layer)) continue;
        std::vector<Vec2d> ca = lattice.cell_corners(pr.cell_a);
        std::vector<Vec2d> cb = lattice.cell_corners(pr.cell_b);
        Vec2d shared[2]; int ns = 0;
        for (const Vec2d &p : ca) {
            for (const Vec2d &q : cb)
                if ((p - q).squaredNorm() < 1e-6) { if (ns < 2) shared[ns++] = p; break; }
            if (ns == 2) break;
        }
        if (ns < 2) continue;
        Vec2d d = shared[1] - shared[0];
        double len = d.norm();
        if (len < 1e-9) continue;
        d /= len;
        const Vec2d  n(-d.y(), d.x());
        const Vec2d  M  = 0.5 * (shared[0] + shared[1]);
        const double hl = len * 0.5;                       // span the full flat edge of the hex side
        const double hw = x_off + lw;                      // half cut width (covers both walls)
        const Vec2d  r0 = M - d * hl - n * hw, r1 = M + d * hl - n * hw;
        const Vec2d  r2 = M + d * hl + n * hw, r3 = M - d * hl + n * hw;
        Polygon rect;
        rect.points = { Point(scale_(r0.x()), scale_(r0.y())), Point(scale_(r1.x()), scale_(r1.y())),
                        Point(scale_(r2.x()), scale_(r2.y())), Point(scale_(r3.x()), scale_(r3.y())) };
        window_cuts.push_back(std::move(rect));
    }
    if (!window_cuts.empty())
        raw = diff_pl(raw, window_cuts);

    Polylines clipped;
    for (Polyline &pl : raw)
        append(clipped, intersection_pl(Polylines{ std::move(pl) }, expolygon));
    if (!clipped.empty())
        chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
}

} // namespace Slic3r
