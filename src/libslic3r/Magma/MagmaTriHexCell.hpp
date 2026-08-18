#ifndef slic3r_Magma_MagmaTriHexCell_hpp_
#define slic3r_Magma_MagmaTriHexCell_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../BoundingBox.hpp"
#include "MagmaGeometry.hpp"
#include "MagmaLattice.hpp"
#include "MagmaCell.hpp"
#include "MagmaTriangleCell.hpp"   // SQRT3, INV_SQRT3

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <utility>

namespace Slic3r {
namespace magma {

// ============================================================================
// Tri-hex (trihexagonal) lattice — hexagon hubs + triangle vents
// ============================================================================
//
// See DESIGN-TRIHEX.md. The trihexagonal tiling is the *rectified* triangular
// tiling: hexagon cells at the vertices of an underlying triangular grid (edge g)
// and up/down triangle cells at the medial (edge-midpoint) triangles of that
// grid's faces. It is bipartite — a hex borders only triangles (6), a triangle
// borders only hexes (3) — which is what makes the hub<->vent solver edges fall
// out for free. CellId carries (i, j, 0, kind) with kind = HEX / TRI_UP / TRI_DOWN.
//
// Hexes are the injection HUBS; triangles are the VENTS. The hub geometry below
// drives the seal/auto-sizing; per-cell open AREA (for presence + volume) is taken
// from cell_corners by the tube map, so no per-kind area method is needed here.

// Cell kinds packed into CellId.kind.
enum TriHexKind : uint8_t { THK_HEX = 0, THK_TRI_UP = 1, THK_TRI_DOWN = 2 };

// Minimum AUTO window height (mm). The vent-matched window (see auto_window_height) can
// come out sub-layer for small vents; floor it so the gap is a robust, reliably-printed
// opening (~a few layers) instead of a single skipped wall segment. Flooring up is always
// flow-safe — the vent-matched height is the minimum that avoids constriction, so a taller
// window can't constrict; it only costs a little min-tube height. The manual "Window
// height" setting (magma_window_height > 0) overrides the auto calc entirely, so this
// floor only governs the auto default.
static constexpr double MAGMA_TRIHEX_MIN_WINDOW_MM = 1.0;

// ----------------------------------------------------------------------------
// Sizing relation (cell_spacing s = interior_width + line_width):
//   interior_width  = hex open flat-to-flat (inscribed) width
//   hex apothem (wall centreline)      = s/2          (open apothem = interior/2)
//   hex circumradius = trihex edge e   = s/sqrt3
//   underlying triangular grid edge g  = 2e           (vertex-to-vertex)
//   vertex row y-step                  = g*sqrt3/2 = s
// ----------------------------------------------------------------------------
inline double trihex_edge_length(double cell_spacing) { return cell_spacing * INV_SQRT3; }   // e

// ============================================================================
// HexGeometry — MagmaGeometry impl, formulas for the HUB (hexagon) cell
// ============================================================================
// All formulas are derived from the trihexagonal (Kagome) geometry: the seal-critical
// opening / area / inscribed-radius / neighbour-distance from the hexagon hub, the
// window height from the hub's open cross-section, and the overlap correction from the
// Kagome line topology (see vertex_overlap_excess_area).
struct HexGeometry final : public MagmaGeometry
{
    double edge_length(double spacing) const override { return trihex_edge_length(spacing); }

    // Open hexagon area. Open apothem a' = s/2 - lw/2 = (s - lw)/2 = interior/2.
    // Regular hexagon area from apothem a:  2*sqrt3*a^2.
    double inset_open_area(double spacing, double line_width) const override {
        double a = (spacing - line_width) * 0.5;          // open apothem
        return a > 0.0 ? 2.0 * SQRT3 * a * a : 0.0;
    }

    // Seal opening = circumscribed circle of the open hexagon = 2 * open circumradius,
    // open circumradius = open_apothem / (sqrt3/2) = (s-lw)/sqrt3.
    double opening_diameter(double spacing, double line_width) const override {
        double s = spacing - line_width;
        return s > 0.0 ? 2.0 * s * INV_SQRT3 : 0.0;
    }

    // Largest circle inside the open hub = inscribed circle = open apothem = interior/2.
    // Hub is a regular hexagon: inscribed radius == apothem == interior/2.
    double inscribed_radius(double interior_width, double) const override { return interior_width * 0.5; }

    // Hub (hex centre = grid vertex) to vent (triangle centroid) centre distance:
    // vertex-to-incident-triangle-centroid = g/sqrt3 = 2e/sqrt3 = (2/3) * spacing.
    double neighbor_centroid_distance(double spacing) const override { return (2.0 / 3.0) * spacing; }

    // Trihex (Kagome) degree-4 vertices: TWO families cross at 60deg, double-depositing a
    // rhombus 2 w^2/sqrt3 per vertex; compute charges it by corner count (hub 6 -> 1.5,
    // vent 3 -> 0.75).
    double vertex_overlap_excess_area(double line_width) const override {
        return 2.0 * INV_SQRT3 * line_width * line_width;
    }

    // Geometric window height. A window is a gap in ONE hub<->vent shared wall, and it
    // only ever feeds that VENT — the hub is filled directly by injection from the top,
    // not through any window. So the window opening (open shared-edge × height) should
    // match the VENT's open cross-section, NOT the hub's. Sizing to the (much larger) hub
    // would make the window — and hence the minimum tube height — needlessly tall while
    // relieving no real constriction. Vent = equilateral triangle of side e (the trihex
    // edge), inset by lw/2 → side (e - lw·√3), area (√3/4)·side².
    double auto_window_height(double interior_width, double line_width) const override {
        double spacing   = interior_width + line_width;
        double e         = trihex_edge_length(spacing);    // hub<->vent shared edge (full)
        double open_edge = e - line_width;                 // inset shared-edge length
        double vent_side = e - line_width * SQRT3;          // inset vent triangle side
        double vent_area = vent_side > 0.0 ? (SQRT3 / 4.0) * vent_side * vent_side : 0.0;
        double vent_matched = open_edge > 0.0 ? vent_area / open_edge : 0.1;
        return std::max(MAGMA_TRIHEX_MIN_WINDOW_MM, vent_matched);
    }

    // Inverse of opening_diameter(): opening = 2*interior/sqrt3  ->  interior = opening * sqrt3 / 2.
    double interior_for_opening(double opening, double /*line_width*/) const override {
        return opening > 0.0 ? std::max(0.1, opening * SQRT3 * 0.5) : 0.1;
    }

    int max_neighbors() const override { return 6; }   // hub has 6 (vent has 3); report the max
                                                       // extra legs are added by the vent sweep
};

// Shared tri-hex (hub) geometry instance (stateless).
inline const MagmaGeometry& trihex_geometry() {
    static const HexGeometry s_geom;
    return s_geom;
}

// ============================================================================
// TriHexLattice — MagmaLattice impl for the trihexagonal tiling
// ============================================================================
class TriHexLattice : public MagmaLattice {
public:
    TriHexLattice() = default;
    explicit TriHexLattice(double cell_spacing, double offset_x = 0.0, double offset_y = 0.0)
        : m_cell_spacing(cell_spacing)
        , m_edge(trihex_edge_length(cell_spacing))
        , m_offset_x(offset_x)
        , m_offset_y(offset_y)
    {}

    // ---- topology (bipartite hub<->vent) ----
    std::vector<CellId> neighbors(const CellId &c) const override {
        const int i = c.a, j = c.b;
        switch (c.kind) {
        case THK_HEX:
            return { {i,   j,   0, THK_TRI_UP},   {i-1, j,   0, THK_TRI_UP},   {i,   j-1, 0, THK_TRI_UP},
                     {i-1, j,   0, THK_TRI_DOWN}, {i,   j-1, 0, THK_TRI_DOWN}, {i-1, j-1, 0, THK_TRI_DOWN} };
        case THK_TRI_UP:    // up-tri(i,j) vertices V(i,j), V(i+1,j), V(i,j+1)
            return { {i,   j,   0, THK_HEX}, {i+1, j,   0, THK_HEX}, {i,   j+1, 0, THK_HEX} };
        case THK_TRI_DOWN:  // down-tri(i,j) vertices V(i+1,j), V(i,j+1), V(i+1,j+1)
        default:
            return { {i+1, j,   0, THK_HEX}, {i,   j+1, 0, THK_HEX}, {i+1, j+1, 0, THK_HEX} };
        }
    }

    bool is_up(const CellId &c) const override { return c.kind == THK_TRI_UP; }
    int  max_neighbors() const override { return 6; }

    // ---- cell geometry ----
    Vec2d cell_center(const CellId &c) const override {
        switch (c.kind) {
        case THK_HEX:
            return vertex(c.a, c.b);
        case THK_TRI_UP:
            return centroid3(vertex(c.a, c.b), vertex(c.a + 1, c.b), vertex(c.a, c.b + 1));
        case THK_TRI_DOWN:
        default:
            return centroid3(vertex(c.a + 1, c.b), vertex(c.a, c.b + 1), vertex(c.a + 1, c.b + 1));
        }
    }

    std::vector<Vec2d> cell_corners(const CellId &c) const override {
        if (c.kind == THK_HEX) {
            // 6 edge-midpoints to the grid-neighbours, CCW from 0deg.
            const int i = c.a, j = c.b;
            Vec2d v = vertex(i, j);
            const std::array<std::pair<int,int>,6> nb = {{
                {i+1, j}, {i, j+1}, {i-1, j+1}, {i-1, j}, {i, j-1}, {i+1, j-1} }};
            std::vector<Vec2d> out;
            out.reserve(6);
            for (auto &n : nb) out.push_back(0.5 * (v + vertex(n.first, n.second)));
            return out;
        }
        // Triangle vent = medial triangle (edge midpoints) of the underlying face.
        Vec2d A, B, C;
        if (c.kind == THK_TRI_UP) {
            A = vertex(c.a, c.b); B = vertex(c.a + 1, c.b); C = vertex(c.a, c.b + 1);
        } else {
            A = vertex(c.a + 1, c.b); B = vertex(c.a, c.b + 1); C = vertex(c.a + 1, c.b + 1);
        }
        return { 0.5 * (A + B), 0.5 * (B + C), 0.5 * (C + A) };
    }

    CellId cell_at(double px, double py) const override {
        // Locate the underlying triangle + barycentric coords, then decide hex vs
        // medial-triangle: if any barycentric coord > 0.5 the point is in that
        // vertex's hexagon; otherwise it's in the central (medial) triangle.
        auto [lx, ly] = to_lattice(px, py);
        int col = int(std::floor(lx)), row = int(std::floor(ly));
        double fx = lx - col, fy = ly - row;
        if (fx + fy < 1.0) {
            // up-tri(col,row): bary (1-fx-fy, fx, fy) for V(col,row),V(col+1,row),V(col,row+1)
            double w0 = 1.0 - fx - fy, w1 = fx, w2 = fy;
            if (w0 > 0.5) return { col,   row,   0, THK_HEX };
            if (w1 > 0.5) return { col+1, row,   0, THK_HEX };
            if (w2 > 0.5) return { col,   row+1, 0, THK_HEX };
            return { col, row, 0, THK_TRI_UP };
        } else {
            // down-tri(col,row): bary for V(col+1,row),V(col,row+1),V(col+1,row+1)
            double w0 = 1.0 - fy, w1 = 1.0 - fx, w2 = fx + fy - 1.0;
            if (w0 > 0.5) return { col+1, row,   0, THK_HEX };
            if (w1 > 0.5) return { col,   row+1, 0, THK_HEX };
            if (w2 > 0.5) return { col+1, row+1, 0, THK_HEX };
            return { col, row, 0, THK_TRI_DOWN };
        }
    }

    std::vector<CellId> enumerate_cells(const BoundingBox &bbox) const override {
        double min_x = unscale<double>(bbox.min.x()), min_y = unscale<double>(bbox.min.y());
        double max_x = unscale<double>(bbox.max.x()), max_y = unscale<double>(bbox.max.y());
        // Map the four bbox corners to lattice space and take the spanning (i,j) box.
        double lxs[4], lys[4];
        const double xs[4] = { min_x, max_x, min_x, max_x };
        const double ys[4] = { min_y, min_y, max_y, max_y };
        for (int k = 0; k < 4; ++k) { auto p = to_lattice(xs[k], ys[k]); lxs[k] = p.first; lys[k] = p.second; }
        int i0 = int(std::floor(*std::min_element(lxs, lxs + 4))) - 1;
        int i1 = int(std::ceil (*std::max_element(lxs, lxs + 4))) + 1;
        int j0 = int(std::floor(*std::min_element(lys, lys + 4))) - 1;
        int j1 = int(std::ceil (*std::max_element(lys, lys + 4))) + 1;
        std::vector<CellId> cells;
        cells.reserve(size_t(std::max(0, i1 - i0 + 1)) * size_t(std::max(0, j1 - j0 + 1)) * 3);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                cells.emplace_back(i, j, 0, THK_HEX);
                cells.emplace_back(i, j, 0, THK_TRI_UP);
                cells.emplace_back(i, j, 0, THK_TRI_DOWN);
            }
        return cells;
    }

    // ---- coordinate transforms (underlying triangular grid, edge g = 2e) ----
    // Basis: u1 = (g, 0), u2 = (g/2, g*sqrt3/2) = (e, s). Vertex(i,j) = i*u1 + j*u2.
    //   g = 2e = 2*s*INV_SQRT3,  g*sqrt3/2 = s.
    Vec2d to_world(double lx, double ly) const override {
        double g = 2.0 * m_edge;
        return Vec2d(lx * g + ly * (g * 0.5) + m_offset_x,
                     ly * m_cell_spacing + m_offset_y);
    }
    std::pair<double, double> to_lattice(double px, double py) const override {
        double g = 2.0 * m_edge;
        double ly = (py - m_offset_y) / m_cell_spacing;
        double lx = ((px - m_offset_x) - ly * (g * 0.5)) / g;
        return { lx, ly };
    }

    // ---- accessors ----
    double cell_spacing() const override { return m_cell_spacing; }
    double edge_length()  const override { return m_edge; }
    double offset_x()     const override { return m_offset_x; }
    double offset_y()     const override { return m_offset_y; }

private:
    Vec2d vertex(int i, int j) const { return to_world(double(i), double(j)); }
    static Vec2d centroid3(const Vec2d &a, const Vec2d &b, const Vec2d &c) {
        return (a + b + c) / 3.0;
    }

    double m_cell_spacing = 0.0;
    double m_edge = 0.0;       // trihex wall length e = spacing/sqrt3
    double m_offset_x = 0.0;
    double m_offset_y = 0.0;
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaTriHexCell_hpp_
