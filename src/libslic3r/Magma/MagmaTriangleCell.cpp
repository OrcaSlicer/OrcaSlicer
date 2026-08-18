#include "MagmaTriangleCell.hpp"

#include <cmath>
#include <algorithm>

namespace Slic3r {
namespace magma {

// Triangle grid geometry:
// - Lines are spaced cell_spacing apart
// - Triangle side length s = cell_spacing * 2 / sqrt(3)
// - Triangle height (altitude) h = cell_spacing
//
// Coordinate system basis vectors (for converting (a,b,c) to (x,y)):
// - a axis: (1, 0) scaled by s/2
// - b axis: (cos(60°), sin(60°)) = (0.5, sqrt(3)/2) scaled by s/2
// - c axis: (-cos(60°), sin(60°)) = (-0.5, sqrt(3)/2) scaled by s/2
//
// For a given cell_spacing:
// s = cell_spacing * 2 / sqrt(3)
// basis_a = (s/2, 0) = (cell_spacing / sqrt(3), 0)
// basis_b = (s/4, s*sqrt(3)/4) = (cell_spacing / (2*sqrt(3)), cell_spacing/2)
// basis_c = (-s/4, s*sqrt(3)/4) = (-cell_spacing / (2*sqrt(3)), cell_spacing/2)

// ============================================================================
// Geometry Helper Implementations
// ============================================================================

double calculate_auto_interior_width(double nozzle_diameter)
{
    // Fallback when nozzle outer diameter is not specified: a conservative
    // bore-relative default. Same multiple as resolve_nozzle_flat() so full-auto
    // mode (no measured flat) keeps the tube opening and the flat in step.
    return nozzle_diameter * MAGMA_FLAT_BORE_MULTIPLE;
}

// The triangle-specific inverse of opening_diameter() now lives on TriangleGeometry as
// interior_for_opening(). It used to live here as a free function called directly by
// MagmaTubeMap, which meant every pattern -- rectilinear and hex included -- was sized
// with the triangle formula. Sizing must go through the geometry.

// Auto window height now lives on each MagmaGeometry impl
// (TriangleGeometry::auto_window_height / SquareGeometry::auto_window_height),
// so the formula matches the cell shape. WindowSpec::from_config dispatches to
// geom.auto_window_height(); the triangle version is byte-identical to the old
// calculate_auto_window_height_mm that lived here.

WindowSpec WindowSpec::from_config(
    const MagmaGeometry& geom,
    float config_window_height_mm,
    float interior_width,
    float line_width,
    float layer_height)
{
    WindowSpec spec;

    // Window height: auto-calculate if 0, otherwise use the config value as-is.
    // Auto adds one layer height above the geometric value so the window
    // reliably spans a full printed layer despite layer-registration accuracy.
    if (config_window_height_mm <= 0) {
        // AUTO height = the smaller of the flow-area sizing (geom.auto_window_height)
        // and the window's own WIDTH (open shared edge). A window taller than it is
        // wide lets injected plastic loop straight across it near the top instead of
        // being forced down to the tube bottom and up the partner — so cap at width.
        // Applies to every Magma pattern, since this is the one AUTO dispatch site.
        const double spacing      = double(interior_width) + double(line_width);
        const double window_width = geom.edge_length(spacing) - double(line_width);
        double       h            = geom.auto_window_height(interior_width, line_width);
        if (window_width > 0.0)
            h = std::min(h, window_width);
        spec.window_height_mm = float(h) + std::max(0.0f, layer_height);
    } else {
        spec.window_height_mm = config_window_height_mm;
    }

    return spec;
}


// ============================================================================
// TriangleLattice Method Implementations
// ============================================================================

Vec2d TriangleLattice::cell_center(const TriangleCell& cell) const
{
    // Centroid of the triangle within the lattice parallelogram cell (a, b).
    // Up triangle vertices: lattice (a,b), (a+1,b), (a,b+1) → centroid at (a+1/3, b+1/3)
    // Down triangle vertices: lattice (a+1,b), (a,b+1), (a+1,b+1) → centroid at (a+2/3, b+2/3)
    if (is_up(cell)) {
        return to_world(cell.a + 1.0 / 3.0, cell.b + 1.0 / 3.0);
    } else {
        return to_world(cell.a + 2.0 / 3.0, cell.b + 2.0 / 3.0);
    }
}

std::vector<Vec2d> TriangleLattice::cell_corners(const TriangleCell& cell) const
{
    // Use exact lattice vertex positions for correct geometry.
    // Up triangle at (a,b): vertices at lattice (a,b), (a+1,b), (a,b+1)
    // Down triangle at (a,b): vertices at lattice (a+1,b), (a,b+1), (a+1,b+1)
    if (is_up(cell)) {
        return { to_world(cell.a, cell.b),
                 to_world(cell.a + 1, cell.b),
                 to_world(cell.a, cell.b + 1) };
    } else {
        return { to_world(cell.a + 1, cell.b),
                 to_world(cell.a, cell.b + 1),
                 to_world(cell.a + 1, cell.b + 1) };
    }
}

std::vector<TriangleCell> TriangleLattice::enumerate_cells(const BoundingBox& bbox) const
{
    std::vector<TriangleCell> cells;

    double min_x = unscale<double>(bbox.min.x());
    double min_y = unscale<double>(bbox.min.y());
    double max_x = unscale<double>(bbox.max.x());
    double max_y = unscale<double>(bbox.max.y());

    // Row range is unskewed (ly = y / cell_spacing), so direct computation works
    int row_min = static_cast<int>(std::floor((min_y - m_offset_y) / m_cell_spacing)) - 1;
    int row_max = static_cast<int>(std::ceil((max_y - m_offset_y) / m_cell_spacing)) + 1;

    for (int row = row_min; row <= row_max; ++row) {
        // Column range depends on row due to lattice skew (lx = (x - ly*edge*0.5) / edge)
        double row_y = row * m_cell_spacing + m_offset_y;
        auto [lx_lo, _1] = to_lattice(min_x, row_y);
        auto [lx_hi, _2] = to_lattice(max_x, row_y);
        int col_min = static_cast<int>(std::floor(std::min(lx_lo, lx_hi))) - 1;
        int col_max = static_cast<int>(std::ceil(std::max(lx_lo, lx_hi))) + 1;

        for (int col = col_min; col <= col_max; ++col) {
            cells.emplace_back(col, row, 2 - col - row);  // up triangle
            cells.emplace_back(col, row, 1 - col - row);  // down triangle
        }
    }
    return cells;
}

} // namespace magma
} // namespace Slic3r
