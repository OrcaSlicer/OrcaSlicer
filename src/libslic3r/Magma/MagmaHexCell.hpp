#ifndef slic3r_Magma_MagmaHexCell_hpp_
#define slic3r_Magma_MagmaHexCell_hpp_

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
// Pure-hexagon lattice — every cell is a hexagon, paired into U-tubes
// ============================================================================
//
// A regular hexagonal (honeycomb) tiling of pointy-top hexagons: each hex has a
// VERTICAL left and right edge and four slanted edges, and borders 6 neighbours.
// Unlike tri-hex there are no vents — adjacent hexes pair into a 2-cell U-tube
// like triangle/rectilinear, with a window cut in their shared edge.
//
// The hexagon math is the same as the tri-hex HUB (the hub IS a hexagon), so the
// open area / opening / inscribed radius / sizing are reused; only the things that
// were hub<->vent specific differ here: neighbour distance is hex<->hex (= spacing),
// the window feeds the paired hex (not a vent), and the vertex overlap is the
// degree-3 honeycomb junction (three walls at 120°) rather than tri-hex's degree-4.
//
// CellId carries (q, r, 0, 0): axial hex coordinates; c and kind are unused.
//
// NOTE: the toolpath (FillMagmaHoneycomb, reusing the honeycomb zigzag) draws the
// vertical edges DOUBLED — each is traced by both horizontally-adjacent columns —
// so on the vertical plane the wall is two beads thick. HexLattice's constructor DOES
// fold that anisotropy in (see m_sx / m_edge / m_vtop / m_row below), so the printed
// open hexagon is regular at flat-to-flat = interior. Injection volume is measured from
// the real deposited cavity, so no ×2 window correction exists or is needed.
// (The earlier TODO here claimed the opposite and predated the lattice work.)

// hex edge length e from cell spacing s (center-to-center = flat-to-flat = e*sqrt3):
//   s = e*sqrt3  ->  e = s/sqrt3 = s*INV_SQRT3.   Same relation as the tri-hex hub.
inline double hex_edge_length(double cell_spacing) { return cell_spacing * INV_SQRT3; }

// ============================================================================
// HexGeometry — MagmaGeometry impl for a pure-hexagon cell
// ============================================================================
struct HexagonGeometry final : public MagmaGeometry
{
    double edge_length(double spacing) const override { return hex_edge_length(spacing); }

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

    // Largest circle inside the open hex = inscribed circle = open apothem = interior/2.
    // Regular hexagon: inscribed circle radius == apothem == interior/2.
    double inscribed_radius(double interior_width, double) const override { return interior_width * 0.5; }

    // Hex <-> hex centres are one full spacing apart (flat-to-flat = center-to-center).
    double neighbor_centroid_distance(double spacing) const override { return spacing; }

    // Honeycomb has degree-3 vertices (line ENDS meet at 120deg, not crossings) -> no
    // line-crossing overlap. Return 0. (The doubled VERTICAL walls are a separate effect,
    // already captured by the measured cavity footprint, not a vertex overlap.)
    double vertex_overlap_excess_area(double /*line_width*/) const override {
        return 0.0;
    }

    // Geometric window height: window flow cross-section = the paired hex's open tube
    // cross-section (a U-tube feeds the partner hex, same size). height = hex open area /
    // open shared-edge length. Written area/edge to parallel triangle/square.
    double auto_window_height(double interior_width, double line_width) const override {
        double spacing = interior_width + line_width;
        // Insetting a regular hexagon by lw/2 shortens each EDGE by lw/sqrt3, not by lw:
        // apothem a' = a - lw/2 and e = 2a/sqrt3, so e' = e - lw/sqrt3. Subtracting the full
        // line width made the open edge 0.42*lw too short, and since the flow-area term is
        // large for hex this cap always binds -- so every honeycomb auto window was ~8%
        // undersized. Written as hex_edge_length(spacing - lw) because that is literally
        // HexLattice's own m_edge, which keeps the geometry and the lattice in agreement.
        double open_edge = hex_edge_length(spacing - line_width);
        double area      = inset_open_area(spacing, line_width);
        return open_edge > 0.0 ? area / open_edge : 0.1;
    }

    // Inverse of opening_diameter(): opening = 2*interior/sqrt3  ->  interior = opening * sqrt3 / 2.
    double interior_for_opening(double opening, double /*line_width*/) const override {
        return opening > 0.0 ? std::max(0.1, opening * SQRT3 * 0.5) : 0.1;
    }

    int max_neighbors() const override { return 6; }
};

// Shared pure-hex geometry instance (stateless).
inline const MagmaGeometry& hexagon_geometry() {
    static const HexagonGeometry s_geom;
    return s_geom;
}

// ============================================================================
// HexLattice — MagmaLattice impl for a pointy-top hexagonal tiling
// ============================================================================
// Axial coords (q, r) packed as CellId(a=q, b=r, 0, 0). Pointy-top: centre at
//   x = s*(q + r/2) + ox,   y = s*(sqrt3/2)*r + oy      (s = cell_spacing)
// so a row (r fixed) steps by s horizontally and each row is offset by s/2 and
// raised by s*sqrt3/2. The six corners sit at 30/90/150/210/270/330 deg, radius
// e = s/sqrt3; the 30<->330 (right) and 150<->210 (left) corner pairs share the
// same x, giving the vertical left/right edges at x = centre.x +/- s/2.
class HexLattice : public MagmaLattice {
public:
    HexLattice() = default;
    // The open tube is built as a regular hexagon whose flat-to-flat is the user's INTERIOR
    // width (= cell_spacing − line_width), so it prints at exactly the requested size. The walls
    // are then added OUTSIDE that open hexagon: the toolpath draws the VERTICAL edges doubled
    // (two beads, total 2·lw → the two neighbouring opens sit 2·lw apart, so the X pitch is
    // interior + 2·lw) and the SLANTS single (one bead → its lw/2 offset lifts the apex by
    // lw/√3). m_edge is therefore the OPEN edge e = interior/√3, m_sx = interior + 2·lw, and the
    // printed open hexagon comes out regular at f2f = interior.
    //   - X pitch          : interior + 2·lw                                     -> m_sx
    //   - open edge         : e = interior/√3                                     -> m_edge
    //   - top/bottom vertex : e + lw/√3   (the single slant's lw/2 lifts the apex) -> m_vtop
    //   - row spacing       : 1.5·e + lw/√3  (keeps the tiling)                    -> m_row
    // (Previously m_edge/m_sx were based on cell_spacing, which left the open one bead too wide.)
    explicit HexLattice(double cell_spacing, double offset_x = 0.0, double offset_y = 0.0,
                        double line_width = 0.0)
        : m_cell_spacing(cell_spacing)
        , m_edge(hex_edge_length(std::max(0.0, cell_spacing - std::max(0.0, line_width))))  // e = interior/√3
        , m_sx(cell_spacing + std::max(0.0, line_width))                                    // interior + 2·lw
        , m_vtop(m_edge + std::max(0.0, line_width) * INV_SQRT3)
        , m_row(1.5 * m_edge + std::max(0.0, line_width) * INV_SQRT3)
        , m_offset_x(offset_x)
        , m_offset_y(offset_y)
    {}

    // ---- topology: 6 edge-sharing neighbours (pointy-top axial) ----
    std::vector<CellId> neighbors(const CellId &c) const override {
        const int q = c.a, r = c.b;
        return { CellId(q+1, r,   0), CellId(q+1, r-1, 0), CellId(q,   r-1, 0),
                 CellId(q-1, r,   0), CellId(q-1, r+1, 0), CellId(q,   r+1, 0) };
    }
    bool is_up(const CellId &) const override { return false; }  // hexes have no parity
    int  max_neighbors() const override { return 6; }

    // ---- cell geometry ----
    Vec2d cell_center(const CellId &c) const override { return center(c.a, c.b); }

    std::vector<Vec2d> cell_corners(const CellId &c) const override {
        const Vec2d ctr = center(c.a, c.b);
        const double hx = m_sx * 0.5;              // half X pitch = interior/2 + lw
        const double hy = m_edge * 0.5;            // half open edge length
        // CCW from the upper-right corner (30deg): the 30<->330 pair forms the right
        // vertical edge, 150<->210 the left vertical edge.
        return { Vec2d(ctr.x() + hx, ctr.y() + hy),   //  30 deg (upper right)
                 Vec2d(ctr.x(),      ctr.y() + m_vtop),//  90 deg (top vertex, extended)
                 Vec2d(ctr.x() - hx, ctr.y() + hy),   // 150 deg (upper left)
                 Vec2d(ctr.x() - hx, ctr.y() - hy),   // 210 deg (lower left)
                 Vec2d(ctr.x(),      ctr.y() - m_vtop),// 270 deg (bottom vertex, extended)
                 Vec2d(ctr.x() + hx, ctr.y() - hy) }; // 330 deg (lower right)
    }

    CellId cell_at(double px, double py) const override {
        auto [fq, fr] = to_lattice(px, py);
        return axial_round(fq, fr);
    }

    std::vector<CellId> enumerate_cells(const BoundingBox &bbox) const override {
        double min_x = unscale<double>(bbox.min.x()), min_y = unscale<double>(bbox.min.y());
        double max_x = unscale<double>(bbox.max.x()), max_y = unscale<double>(bbox.max.y());
        double fq[4], fr[4];
        const double xs[4] = { min_x, max_x, min_x, max_x };
        const double ys[4] = { min_y, min_y, max_y, max_y };
        for (int k = 0; k < 4; ++k) { auto p = to_lattice(xs[k], ys[k]); fq[k] = p.first; fr[k] = p.second; }
        int q0 = int(std::floor(*std::min_element(fq, fq + 4))) - 1;
        int q1 = int(std::ceil (*std::max_element(fq, fq + 4))) + 1;
        int r0 = int(std::floor(*std::min_element(fr, fr + 4))) - 1;
        int r1 = int(std::ceil (*std::max_element(fr, fr + 4))) + 1;
        std::vector<CellId> cells;
        cells.reserve(size_t(std::max(0, q1 - q0 + 1)) * size_t(std::max(0, r1 - r0 + 1)));
        for (int r = r0; r <= r1; ++r)
            for (int q = q0; q <= q1; ++q)
                cells.emplace_back(q, r, 0);
        return cells;
    }

    // ---- coordinate transforms (pointy-top axial) ----
    Vec2d to_world(double lq, double lr) const override {
        return Vec2d(m_sx * (lq + lr * 0.5) + m_offset_x,   // X: stretched flat-to-flat
                     m_row * lr + m_offset_y);              // Y: extended row spacing
    }
    std::pair<double, double> to_lattice(double px, double py) const override {
        double r = (py - m_offset_y) / m_row;
        double q = ((px - m_offset_x) / m_sx) - r * 0.5;
        return { q, r };
    }

    // ---- accessors ----
    double cell_spacing() const override { return m_cell_spacing; }
    double edge_length()  const override { return m_edge; }
    double offset_x()     const override { return m_offset_x; }
    double offset_y()     const override { return m_offset_y; }

private:
    Vec2d center(int q, int r) const { return to_world(double(q), double(r)); }

    // Round fractional axial (q, r) to the nearest hex via cube rounding.
    static CellId axial_round(double q, double r) {
        double x = q, z = r, y = -x - z;
        double rx = std::round(x), ry = std::round(y), rz = std::round(z);
        double dx = std::abs(rx - x), dy = std::abs(ry - y), dz = std::abs(rz - z);
        if (dx > dy && dx > dz)      rx = -ry - rz;
        else if (dy > dz)            ry = -rx - rz;
        else                         rz = -rx - ry;
        return CellId(int(rx), int(rz), 0);
    }

    double m_cell_spacing = 0.0;
    double m_edge = 0.0;        // OPEN hex edge e = interior/sqrt3 (interior = spacing - line_width)
    double m_sx   = 0.0;        // X pitch = interior + 2*line_width (open f2f + doubled wall)
    double m_vtop = 0.0;        // top/bottom vertex height = e + line_width/sqrt3
    double m_row  = 0.0;        // row spacing (Y) = 1.5*e + line_width/sqrt3
    double m_offset_x = 0.0;
    double m_offset_y = 0.0;
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaHexCell_hpp_
