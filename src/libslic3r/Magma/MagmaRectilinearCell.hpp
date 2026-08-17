#ifndef slic3r_Magma_MagmaRectilinearCell_hpp_
#define slic3r_Magma_MagmaRectilinearCell_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../BoundingBox.hpp"
#include "MagmaGeometry.hpp"
#include "MagmaLattice.hpp"
#include "MagmaCell.hpp"

#include <vector>
#include <cmath>
#include <algorithm>
#include <utility>

namespace Slic3r {
namespace magma {

// ============================================================================
// Square (rectilinear) grid geometry
// ============================================================================
//
// A square lattice: two perpendicular line families spaced `cell_spacing` apart,
// so each cell is a square of side = cell_spacing. Walls are single shared beads
// centered on the grid lines, so the open interior is inset by line_width/2 on
// each of the four sides -> inset (open) square side = spacing - line_width.
//
// Compared with the triangle grid this seals better (the circumscribed-circle /
// inscribed-circle ratio is sqrt(2) ~= 1.41 vs the triangle's 2.0) and prints
// faster (2 straight line families instead of 3). All formulas below are the
// square analogues of TriangleGeometry; they need slice-verification when the
// square tube map / injection are wired (tasks #24/#25).

constexpr double SQRT2 = 1.4142135623730951;

struct SquareGeometry final : public MagmaGeometry
{
    // The line spacing IS the square's side.
    double edge_length(double spacing) const override { return spacing; }

    // Inset open square: side = spacing - line_width (lw/2 off each of 2 opposite
    // walls). Area = side^2.
    double inset_open_area(double spacing, double line_width) const override {
        double s = spacing - line_width;
        return s > 0.0 ? s * s : 0.0;
    }

    // Seal opening = circumscribed circle of the inset square (covers all four
    // corners) = inset_side * sqrt(2).
    double opening_diameter(double spacing, double line_width) const override {
        double s = spacing - line_width;
        return s > 0.0 ? s * SQRT2 : 0.0;
    }

    // Largest circle that fits inside the open tube.
    double inscribed_radius(double interior_width) const override { return interior_width * 0.5; }

    // Edge-sharing neighbor centers are one full spacing apart.
    double neighbor_centroid_distance(double spacing) const override { return spacing; }

    double interlock_radius(double spacing) const override { return spacing * 0.5; }

    // 2 line families crossing at 90deg: each crossing double-deposits an lw x lw square,
    // ~1 crossing per cell -> lw^2. (Only subtracted when the overlap flow correction is off.)
    double vertex_overlap_excess_area(double line_width) const override {
        return line_width * line_width;
    }

    // Per cell ~2*spacing*lw of wall is deposited and lw^2 is doubled at the
    // crossing -> excess fraction = lw / (2*spacing).
    double line_overlap_excess_fraction(double spacing, double line_width) const override {
        return spacing > 0.0 ? line_width / (2.0 * spacing) : 0.0;
    }

    // Geometric window height: window flow cross-section = tube open cross-section.
    // Window opening edge length = inset side (s - lw); tube area = (s - lw)^2.
    // height = area / edge = (s-lw)^2 / (s-lw) = (s-lw) == interior_width.
    // Written as area/edge to parallel the triangle so both shapes make the
    // window flow area equal the tube's open cross-section.
    double auto_window_height(double interior_width, double line_width) const override {
        double spacing = interior_width + line_width;
        double inset = spacing - line_width;
        return inset > 0.0 ? (inset * inset) / inset : 0.1;
    }

    // Largest interior whose circumscribed opening fits the nozzle flat:
    // interior * sqrt(2) <= od  ->  interior = od / sqrt(2).
    double auto_interior_width_from_od(double nozzle_od, double /*line_width*/) const override {
        return nozzle_od > 0.0 ? std::max(0.1, nozzle_od / SQRT2) : 0.1;
    }

    int max_neighbors() const override { return 4; }
    int cells_per_pair() const override { return 2; }
};

// Shared square-geometry instance (stateless).
inline const MagmaGeometry& square_geometry() {
    static const SquareGeometry s_geom;
    return s_geom;
}

// ============================================================================
// RectilinearLattice — MagmaLattice impl for the square grid
// ============================================================================
//
// CellId uses (a, b) = integer (column, row); c and kind are unused (0). World
// mapping is a plain axis-aligned scale + spiral offset (no skew). Each cell is
// the unit square [a, a+1] x [b, b+1] in lattice space.
class RectilinearLattice : public MagmaLattice {
public:
    RectilinearLattice() = default;
    explicit RectilinearLattice(double cell_spacing, double offset_x = 0.0, double offset_y = 0.0)
        : m_cell_spacing(cell_spacing), m_offset_x(offset_x), m_offset_y(offset_y) {}

    // ---- topology ----
    std::vector<CellId> neighbors(const CellId &c) const override {
        return { CellId(c.a - 1, c.b, 0), CellId(c.a + 1, c.b, 0),
                 CellId(c.a, c.b - 1, 0), CellId(c.a, c.b + 1, 0) };
    }
    bool is_up(const CellId &) const override { return false; }  // squares have no parity
    int  max_neighbors() const override { return 4; }

    // ---- cell geometry ----
    CellId cell_at(double px, double py) const override {
        auto [lx, ly] = to_lattice(px, py);
        return CellId(int(std::floor(lx)), int(std::floor(ly)), 0);
    }
    std::vector<Vec2d> cell_corners(const CellId &c) const override {
        return { to_world(c.a,     c.b),
                 to_world(c.a + 1, c.b),
                 to_world(c.a + 1, c.b + 1),
                 to_world(c.a,     c.b + 1) };
    }
    Vec2d cell_center(const CellId &c) const override {
        return to_world(c.a + 0.5, c.b + 0.5);
    }
    std::vector<CellId> enumerate_cells(const BoundingBox &bbox) const override {
        std::vector<CellId> cells;
        double min_x = unscale<double>(bbox.min.x());
        double min_y = unscale<double>(bbox.min.y());
        double max_x = unscale<double>(bbox.max.x());
        double max_y = unscale<double>(bbox.max.y());
        auto [lx0, ly0] = to_lattice(min_x, min_y);
        auto [lx1, ly1] = to_lattice(max_x, max_y);
        int i0 = int(std::floor(std::min(lx0, lx1))) - 1;
        int i1 = int(std::ceil (std::max(lx0, lx1))) + 1;
        int j0 = int(std::floor(std::min(ly0, ly1))) - 1;
        int j1 = int(std::ceil (std::max(ly0, ly1))) + 1;
        cells.reserve(size_t(std::max(0, i1 - i0 + 1)) * size_t(std::max(0, j1 - j0 + 1)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                cells.emplace_back(i, j, 0);
        return cells;
    }

    // ---- coordinate transforms (axis-aligned, no skew) ----
    Vec2d to_world(double lx, double ly) const override {
        return Vec2d(lx * m_cell_spacing + m_offset_x,
                     ly * m_cell_spacing + m_offset_y);
    }
    std::pair<double, double> to_lattice(double px, double py) const override {
        return { (px - m_offset_x) / m_cell_spacing,
                 (py - m_offset_y) / m_cell_spacing };
    }

    // ---- accessors ----
    double cell_spacing() const override { return m_cell_spacing; }
    double edge_length()  const override { return m_cell_spacing; }  // square side = spacing
    double offset_x()     const override { return m_offset_x; }
    double offset_y()     const override { return m_offset_y; }

private:
    double m_cell_spacing = 0.0;
    double m_offset_x = 0.0;
    double m_offset_y = 0.0;
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaRectilinearCell_hpp_
