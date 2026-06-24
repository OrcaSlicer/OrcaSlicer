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
static void subtract_gaps(double lo, double hi,
                          const std::vector<std::pair<double, double>> &gaps,
                          EmitFn emit)
{
    double cursor = lo;
    for (const auto &gap : gaps) {
        double gl = std::max(gap.first, lo);
        double gr = std::min(gap.second, hi);
        if (gr <= gl)
            continue;
        if (gl > cursor + 0.01)
            emit(cursor, gl);
        cursor = std::max(cursor, gr);
    }
    if (hi > cursor + 0.01)
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

    subtract_gaps(0.0, 1.0, t_gaps, [&](double t_lo, double t_hi) {
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
                subtract_gaps(x_min, x_max, it->second, [&](double lo, double hi) {
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
                subtract_gaps(x_min, x_max, it->second, [&](double lo, double hi) {
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
                subtract_gaps(y_min, y_max, it->second, [&](double lo, double hi) {
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
// The trihexagonal walls are the tiling's edges. Hexagons break any analytic line
// family into segments, so instead of generating line families we emit the walls
// directly from the lattice cell corners and deduplicate each shared edge (every
// edge is shared by exactly one hex and one triangle). A window is the shared edge
// of an open hub<->vent pair on this layer, which we drop. chain_or_connect_infill
// then stitches the collinear segments back into runs.

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

    const double cs    = this->tube_map->cell_spacing();
    const int    layer = static_cast<int>(this->layer_id);
    const magma::MagmaLattice &lattice = this->tube_map->lattice_at(layer);

    // Canonical key for an undirected edge, rounded to ~1um so the same wall
    // computed from each of its two adjacent cells dedups exactly.
    auto qd = [](double mm) -> coord_t { return coord_t(std::llround(mm * 1e6 / 1000.0)) * 1000; };
    auto ekey = [&](const Vec2d &a, const Vec2d &b) -> std::array<coord_t, 4> {
        coord_t ax = qd(a.x()), ay = qd(a.y()), bx = qd(b.x()), by = qd(b.y());
        if (std::tie(ax, ay) > std::tie(bx, by)) { std::swap(ax, bx); std::swap(ay, by); }
        return { ax, ay, bx, by };
    };
    auto cell_edge_keys = [&](const std::vector<Vec2d> &c, std::set<std::array<coord_t, 4>> &out) {
        const size_t n = c.size();
        for (size_t i = 0; i < n; ++i) out.insert(ekey(c[i], c[(i + 1) % n]));
    };

    // Window gaps: the shared edge between the hub (cell_a) and EACH of its legs — the
    // primary vent (cell_b) plus any extra manifold legs — for every open pair on this
    // layer. extra_vents is empty for triangle/square, so this reduces to the single
    // hub<->vent edge there.
    std::set<std::array<coord_t, 4>> window_edges;
    for (const auto &pair : this->tube_map->u_tube_pairs()) {
        if (!this->tube_map->window_open_at(pair, layer))
            continue;
        std::set<std::array<coord_t, 4>> hub_edges;
        cell_edge_keys(lattice.cell_corners(pair.cell_a), hub_edges);
        auto cut_shared = [&](const magma::CellId &leg) {
            std::vector<Vec2d> cl = lattice.cell_corners(leg);
            for (size_t i = 0; i < cl.size(); ++i) {
                auto k = ekey(cl[i], cl[(i + 1) % cl.size()]);
                if (hub_edges.count(k)) window_edges.insert(k);
            }
        };
        cut_shared(pair.cell_b);
        for (const magma::CellId &ev : pair.extra_vents)
            cut_shared(ev);
    }

    // Enumerate cells over the expanded region; emit each unique wall edge once.
    BoundingBox bbox = expolygon.contour.bounding_box();
    coord_t m = coord_t(scale_(cs));
    bbox.min -= Point(m, m);
    bbox.max += Point(m, m);
    std::vector<magma::CellId> cells = lattice.enumerate_cells(bbox);

    std::set<std::array<coord_t, 4>> seen;
    Polylines raw;
    for (const magma::CellId &cell : cells) {
        std::vector<Vec2d> corners = lattice.cell_corners(cell);
        const size_t n = corners.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2d &a = corners[i];
            const Vec2d &b = corners[(i + 1) % n];
            auto k = ekey(a, b);
            if (!seen.insert(k).second)    // already emitted from the adjacent cell
                continue;
            if (window_edges.count(k))     // open window — leave the gap
                continue;
            Polyline pl;
            pl.points.push_back(Point(scale_(a.x()), scale_(a.y())));
            pl.points.push_back(Point(scale_(b.x()), scale_(b.y())));
            raw.push_back(std::move(pl));
        }
    }

    // Clip to the fill region and connect. The edges are independent cell walls (no per-line
    // sweep order to preserve, unlike the triangle/square line families), so clip the whole
    // batch in one Clipper pass instead of one call per edge — there are many edges per layer.
    Polylines clipped = intersection_pl(raw, expolygon);
    if (!clipped.empty())
        chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, no_anchor_params);
}

} // namespace Slic3r
