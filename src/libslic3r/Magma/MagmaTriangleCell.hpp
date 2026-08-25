#ifndef slic3r_Magma_MagmaTriangleCell_hpp_
#define slic3r_Magma_MagmaTriangleCell_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../BoundingBox.hpp"
#include "MagmaGeometry.hpp"
#include "MagmaCell.hpp"
#include "MagmaLattice.hpp"

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <functional>
#include <utility>

namespace Slic3r {
namespace magma {

// ============================================================================
// Triangle Grid Constants
// ============================================================================

constexpr double SQRT3 = 1.7320508075688772935;
constexpr double INV_SQRT3 = 0.57735026918962576451;

// ============================================================================
// Triangle Grid Geometry Helpers
// ============================================================================

// Triangle side length (edge length) from cell spacing
// cell_spacing = distance between parallel lines
// side_length = cell_spacing * 2 / sqrt(3)
inline double triangle_side_length(double cell_spacing) {
    return cell_spacing * 2.0 * INV_SQRT3;
}

// Cell spacing from interior width and line width
// cell_spacing = interior_width + line_width (center-to-center distance)
inline double cell_spacing_from_geometry(double interior_width, double line_width) {
    return interior_width + line_width;
}

// Interior area of a triangle cell after insetting by half the line width.
// Infill beads are centered on triangle edges, so each edge eats line_width/2
// into the interior. This is the actual open tube cross-section in mm².
// Used for: obstruction checks, window height auto-calculation, volume computation.
inline double inset_triangle_area(double cell_spacing, double line_width) {
    double side = triangle_side_length(cell_spacing);
    double inset_side = side - line_width * SQRT3;
    return (inset_side > 0) ? (SQRT3 / 4.0) * inset_side * inset_side : 0.0;
}

// ============================================================================
// Injection seal geometry (single source of truth: injection G-code + validation)
// ============================================================================
// The nozzle seals a tube opening when its flat tip -- and the cone above it,
// once pressed down -- covers the opening. These helpers are shared by the
// injection G-code that *applies* the slam/plunge and by Print::validate() that
// *predicts* whether it will seal, so the two can never drift apart.

constexpr double MAGMA_DEG2RAD           = 0.017453292519943295;
constexpr double MAGMA_SLAM_CLAMP        = 3.5;  // absolute sanity ceiling on total depth (mm)

// Above which cone-diameter-to-cell-pitch ratio the nozzle is judged to be crushing the
// NEIGHBOURING cells rather than only the one it is sealing.
//
// MEASURED, not derived. A sweep at flat 1.70 / line width 0.60 / plunge 0.10 was clean up to a
// ratio of 1.129 and showed the lattice visibly disrupted from 1.139, so the threshold sits
// between them. Do not tighten it without print evidence: at 1.10 it fires on configurations
// that demonstrably print well.
//
// Evaluated at SEAL depth, NOT at seal + plunge. That assumption used to run the other way, and
// a print settled it: a plate sweeping plunge 0.10 -> 0.60 at a fixed tube reached 135% of pitch
// at full depth with no lattice disruption at all, and the 0.30 cell looked the best of the six.
// Depth reached while injecting does not do the damage that depth reached before it does -- the
// cone descends into a cell that is filling with hot plastic, not into cold neighbours.
//
// The 1.135 value itself predates that plate and is INHERITED, not re-measured against this
// definition: it came from a sweep whose geometry the calibration record does not preserve well
// enough to recompute. A tube-width sweep is what re-derives it. See CALIBRATION.md.
constexpr double MAGMA_PITCH_WARN_RATIO   = 1.135;
// Default seal press, in mm. Exposed as magma_seal_press; this is only
// the default. It used to be a hardcoded DIAMETRAL 0.1, i.e. 0.05 radial, which is what this
// preserves. See corner_grip() for why it is the load-bearing number in the seal.
// Default seal PRESS: how far the nozzle descends past the depth at which it first touches the
// cell, in mm. 0.0866 is the old hardcoded 0.1mm DIAMETRAL margin expressed as a depth
// (0.1 / (2 tan30)), so the default behaviour is unchanged.
constexpr double MAGMA_SEAL_PRESS         = 0.0866;
// Clearance added to the auto crater-iron start margin, beyond the r_flat needed to put the
// nozzle flat outside the crater mouth. Covers the displaced material that rode up the bevel
// and piled into a rim just outside the mouth.
constexpr double MAGMA_IRON_RIM_CLEARANCE = 0.3;
// Crater-iron speed used when neither an explicit value nor an ironing speed is configured.
// Must NOT fall back to travel speed -- see MagmaInjection.cpp.
constexpr double MAGMA_IRON_SPEED_FALLBACK = 40.0;  // mm/s
// Auto crater-iron radial step per spiral turn, as a fraction of the nozzle flat. The flat
// is also the width of the bevel doing the plowing, so the step must scale with it.
constexpr double MAGMA_IRON_STEP_FRACTION = 0.5;

// Conservative flat-from-bore estimate, used ONLY as the "start here" value in the
// unmeasured-flat error -- Print::validate() blocks slicing before an unset flat can reach
// any real geometry, so nothing is ever sized from this.
//
// Calibrated from the one flat actually measured: a classic E3D 0.6mm nozzle is 1.7mm, i.e.
// 2.83x bore. Biased BELOW that on purpose. The error direction is not symmetric -- an
// estimate that reads high sizes tubes the nozzle cannot seal, which leaks injected plastic
// across the part, while one that reads low just gives smaller tubes. So the user starts
// under the real value and walks up.
constexpr double MAGMA_FLAT_BORE_MULTIPLE = 2.5;
inline double resolve_nozzle_flat(double configured_flat, double nozzle_diameter) {
    return configured_flat > 0.0 ? configured_flat
                                 : MAGMA_FLAT_BORE_MULTIPLE * nozzle_diameter;
}

// Depth the cone must descend so it widens from `flat` to cover `opening_dia`.
inline double seal_depth_for_opening(double opening_dia, double flat, double cone_half_angle_deg) {
    double tan_t = std::tan(cone_half_angle_deg * MAGMA_DEG2RAD);
    double gap = opening_dia - flat;
    return (gap > 0.0 && tan_t > 1e-6) ? gap / (2.0 * tan_t) : 0.0;
}

// Opening diameter the cone covers when pressed `depth` below the flat contact.
inline double cone_coverage_at_depth(double depth, double flat, double cone_half_angle_deg) {
    return flat + 2.0 * depth * std::tan(cone_half_angle_deg * MAGMA_DEG2RAD);
}

// Largest tube opening an immersion budget allows: how wide the cone has grown by the
// time it has descended `max_immersion` into the tube, less the seal margin. Auto tube
// sizing feeds this to MagmaGeometry::interior_for_opening(), so the tube comes out as
// large as the budget permits instead of as large as some fixed formula happens to give.
// max_immersion == 0 yields opening = flat - margin, i.e. the nozzle seats on the rim
// and never enters the tube at all.
// Largest opening a given seal depth can cover, with `seal_press` of that depth spent pressing
// rather than reaching. The inverse of auto_seal_depth(); kept for readouts and calibration.
inline double max_opening_for_immersion(double flat, double cone_half_angle_deg,
                                        double seal_depth, double seal_press) {
    double tan_t = std::tan(cone_half_angle_deg * MAGMA_DEG2RAD);
    return std::max(0.1, flat + 2.0 * std::max(0.0, seal_depth - std::max(0.0, seal_press)) * tan_t);
}

// Diameter of the nozzle cone at a given depth below first contact with the print surface.
inline double cone_diameter_at(double depth, double flat, double cone_half_angle_deg) {
    return flat + 2.0 * std::max(0.0, depth) * std::tan(cone_half_angle_deg * MAGMA_DEG2RAD);
}

// How hard the corners are gripped at the END of the injection.
//
// This is the quantity that actually holds the seal shut, and until it was written down
// nothing in the code computed it. At the sealing depth the cone has jammed a long way into
// the EDGE MIDPOINTS of the cell but only `press * tan(theta)` past the CORNERS -- and the
// corner is where the seal is made, because it is the last part of the opening to be covered.
// The plunge then adds `plunge * tan(theta)` of further radial grip as it descends.
//
// Note it does not depend on the tube size or the seal depth at all: corner grip is set by
// the press and the plunge, and by nothing else.
inline double corner_grip(double seal_press, double plunge_depth, double cone_half_angle_deg) {
    return (std::max(0.0, seal_press) + std::max(0.0, plunge_depth))
         * std::tan(cone_half_angle_deg * MAGMA_DEG2RAD);
}

// Seal depth: how deep the nozzle must sit for the cone to cover the whole cell opening,
// then pressed `seal_press` further. One fast Z move gets it here,
// before any filament flows; the plunge then carries it deeper DURING the fill.
//
// Cell SHAPE enters through `opening_dia`, which each MagmaGeometry reports as its cell's
// CIRCUMSCRIBED circle -- the corners are the last thing the widening cone reaches, so they
// are what the depth has to be solved for.
//
// This is not a free choice. Given the nozzle, seal depth and cell size are the same number in
// two units: solve for one and the other follows. The tube width is the setting because it is
// the thing the part's strength depends on; this depth is the price the hardware charges for
// it, and it is reported rather than chosen.
inline double auto_seal_depth(double opening_dia, double flat, double cone_half_angle_deg,
                              double seal_press) {
    // First contact: the depth at which the widening cone reaches the cell's furthest corner.
    // Zero when the flat already spans the opening -- then the nozzle touches at the surface.
    // Everything shallower than this is free descent inside the tube, touching nothing.
    const double first_contact = seal_depth_for_opening(opening_dia, flat, cone_half_angle_deg);
    // Then press. Covered is not the same as sealed: at first contact the nozzle is resting,
    // not gripping. This is the ONE quantity that decides how hard -- and it is a depth, so it
    // reads the same whether the cone is biting into a corner or the flat is bearing on a rim.
    // No regime switch, because there is only one thing being measured.
    return first_contact + std::max(0.0, seal_press);
}

// The plunge is ADDITIVE: it descends further during the injection, on top of the seal depth.
//
// It used to be carved out of a shared "total immersion" budget, which meant raising the plunge
// silently shrank the tube -- the sizing subtracted it while the emitter added it back at print
// time. Only the absolute sanity clamp bounds the pair now; whether the total is sensible is a
// question about the LATTICE PITCH, which Print::validate() checks and reports rather than
// quietly reshaping the settings to satisfy.
inline double clamp_plunge_depth(double seal_depth, double plunge_depth) {
    return std::min(std::max(0.0, plunge_depth),
                    std::max(0.0, MAGMA_SLAM_CLAMP - std::max(0.0, seal_depth)));
}

// ============================================================================
// TriangleGeometry — MagmaGeometry impl for the equilateral triangle lattice
// ============================================================================
// Single source of truth for the triangle pattern's shape formulas. Every value
// here matches the formula it replaces in MagmaTubeMap/Print so triangle output
// stays byte-identical; new patterns provide their own MagmaGeometry impl.
struct TriangleGeometry final : public MagmaGeometry
{
    double edge_length(double spacing) const override { return triangle_side_length(spacing); }

    double inset_open_area(double spacing, double line_width) const override {
        return inset_triangle_area(spacing, line_width);
    }

    // Circumscribed circle of the inset triangle: 2*(side - lw*sqrt3)/sqrt3.
    double opening_diameter(double spacing, double line_width) const override {
        double inset_side = triangle_side_length(spacing) - line_width * SQRT3;
        return inset_side > 0.0 ? 2.0 * inset_side / SQRT3 : 0.0;
    }

    // Equilateral triangle. With spacing = interior + lw as the altitude, the inset side is
    // (2*interior - lw)/sqrt3, so the inradius is inset_side/(2*sqrt3) = (2*interior - lw)/6.
    // Returning interior/2 is the answer for a SQUARE; here it reported a bore ~59% too large,
    // contradicting the 50%-of-flat figure the validator quotes for triangle.
    double inscribed_radius(double interior_width, double line_width) const override {
        return std::max(0.0, (2.0 * interior_width - line_width) / 3.0) * 0.5;
    }

    // Centroid-to-neighbor-centroid distance = circumradius = side/sqrt3.
    double neighbor_centroid_distance(double spacing) const override {
        return triangle_side_length(spacing) / SQRT3;
    }

    // 3 line families crossing at 60deg per vertex: (3*sqrt3/4) * w^2.
    double vertex_overlap_excess_area(double line_width) const override {
        return (3.0 * SQRT3 / 4.0) * line_width * line_width;
    }

    // Geometric window height: window cross-section (inset_side x height) equals
    // the tube's open cross-section (inset triangle area). Byte-identical to the
    // formula previously used for the triangle's auto window height.
    double auto_window_height(double interior_width, double line_width) const override {
        double spacing = cell_spacing_from_geometry(interior_width, line_width);
        double side = triangle_side_length(spacing);
        double inset_side = side - line_width * SQRT3;
        return inset_side > 0.0 ? inset_triangle_area(spacing, line_width) / inset_side : 0.1;
    }

    // Inverse of opening_diameter(). Substituting side = (interior+lw)*2/sqrt3 into
    // opening = 2*(side - lw*sqrt3)/sqrt3 reduces to  opening = (4*interior - 2*lw)/3,
    // hence interior = (3*opening + 2*lw)/4.
    double interior_for_opening(double opening, double line_width) const override {
        return opening > 0.0 ? std::max(0.1, 0.75 * opening + 0.5 * line_width) : 0.1;
    }

    int max_neighbors() const override { return 3; }
};

// Shared triangle-geometry instance (stateless).
inline const MagmaGeometry& triangle_geometry() {
    static const TriangleGeometry s_geom;
    return s_geom;
}

// ============================================================================
// Triangle Cell Coordinate System
// ============================================================================

// Triangle cell coordinate system using (a, b, c) integer coordinates
//
// The triangular grid uses three axes at 60° angles, creating a dual coordinate system:
// - Up triangles: a + b + c == 2 (pointing up: △)
// - Down triangles: a + b + c == 1 (pointing down: ▽)
//
// This coordinate system allows:
// - Efficient neighbor lookups
// - Consistent cell identification across layers
//
// Geometry:
// - cell_spacing: distance between parallel lines (center-to-center)
// - Triangle side length = cell_spacing * 2 / sqrt(3)
// - Triangle height (altitude) = cell_spacing
//
// Transitional aliases onto the generic CellId / CellIdHash (Phase 0b de-typing).
// The triangle's (a, b, c) coordinates map directly onto CellId's first three
// fields (kind unused). The cell's old is_up()/neighbors() methods now live on
// TriangleLattice below — topology belongs to the lattice, not the cell id.
using TriangleCell     = CellId;
using TriangleCellHash = CellIdHash;

// ============================================================================
// TriangleLattice - Unified Coordinate System for Triangle Grid
// ============================================================================
//
// The triangle grid uses a skewed coordinate system where:
// - Rows are spaced cell_spacing apart vertically
// - Columns are spaced edge_length apart, but shift by edge_length/2 per row
//
// Lattice coordinates (lx, ly) map to world coordinates (px, py) via:
//   py = ly * cell_spacing
//   px = lx * edge_length + ly * edge_length * 0.5
//
// This class bundles cell_spacing and spiral offset together, providing
// all coordinate transformations needed for cell detection and window placement.

class TriangleLattice : public MagmaLattice {
public:
    TriangleLattice() : m_cell_spacing(0), m_edge_length(0), m_offset_x(0), m_offset_y(0) {}

    // Construct a lattice with given cell spacing and optional spiral offset
    explicit TriangleLattice(double cell_spacing, double offset_x = 0.0, double offset_y = 0.0)
        : m_cell_spacing(cell_spacing)
        , m_edge_length(triangle_side_length(cell_spacing))
        , m_offset_x(offset_x)
        , m_offset_y(offset_y)
    {}

    // Accessors
    double cell_spacing() const override { return m_cell_spacing; }
    double edge_length() const override { return m_edge_length; }
    double offset_x() const override { return m_offset_x; }
    double offset_y() const override { return m_offset_y; }

    // ========================================================================
    // Topology
    // ========================================================================

    // Edge-sharing neighbor count for the triangle grid.
    int max_neighbors() const override { return 3; }

    // Check if this is an upward-pointing triangle (△). Moved verbatim from the
    // old TriangleCell::is_up(): up triangles have a + b + c == 2.
    bool is_up(const TriangleCell& cell) const override {
        return (cell.a + cell.b + cell.c) == 2;
    }

    // Adjacent cells sharing an edge. Moved verbatim from the old
    // TriangleCell::neighbors().
    // Up triangle neighbors: decrement one coordinate by 1 → down triangles (sum=1).
    // Down triangle neighbors: increment one coordinate by 1 → up triangles (sum=2).
    std::vector<TriangleCell> neighbors(const TriangleCell& cell) const override {
        const int a = cell.a, b = cell.b, c = cell.c;
        if (is_up(cell))
            // Up (sum=2) neighbors are down triangles (sum=1)
            return {{ {a-1,b,c}, {a,b-1,c}, {a,b,c-1} }};
        else
            // Down (sum=1) neighbors are up triangles (sum=2)
            return {{ {a+1,b,c}, {a,b+1,c}, {a,b,c+1} }};
    }

    // ========================================================================
    // Coordinate Transformations
    // ========================================================================

    // Convert lattice coordinates to world coordinates
    Vec2d to_world(double lx, double ly) const override {
        return Vec2d(
            lx * m_edge_length + ly * m_edge_length * 0.5 + m_offset_x,
            ly * m_cell_spacing + m_offset_y
        );
    }

    // Convert world coordinates to lattice coordinates
    // Returns (lattice_x, lattice_y) in the unshifted reference frame
    std::pair<double, double> to_lattice(double px, double py) const override {
        double adjusted_x = px - m_offset_x;
        double adjusted_y = py - m_offset_y;
        double ly = adjusted_y / m_cell_spacing;
        double lx = (adjusted_x - ly * m_edge_length * 0.5) / m_edge_length;
        return {lx, ly};
    }

    // ========================================================================
    // Cell Operations
    // ========================================================================

    // Get the triangle cell containing a world point
    TriangleCell cell_at(double px, double py) const override {
        auto [lx, ly] = to_lattice(px, py);

        int col = static_cast<int>(std::floor(lx));
        int row = static_cast<int>(std::floor(ly));

        double fx = lx - col;
        double fy = ly - row;

        // fx + fy < 1 means UP triangle, >= 1 means DOWN triangle
        bool cell_is_up = (fx + fy) < 1.0;

        int c = cell_is_up ? (2 - col - row) : (1 - col - row);
        return TriangleCell(col, row, c);
    }

    // ========================================================================
    // Cell Geometry Operations
    // ========================================================================

    // Get the 3 corners of a cell in world coordinates
    std::vector<Vec2d> cell_corners(const TriangleCell& cell) const override;

    // Get the center of a cell in world coordinates
    Vec2d cell_center(const TriangleCell& cell) const override;

    // Enumerate all unique cells within a bounding box.
    // Iterates lattice (col, row) coordinates covering the bbox,
    // generates both up and down triangles for each position.
    std::vector<TriangleCell> enumerate_cells(const BoundingBox& bbox) const override;

private:
    double m_cell_spacing;
    double m_edge_length;
    double m_offset_x;
    double m_offset_y;
};

// Window specification for U-tube formation.
// Windows are gaps in the shared walls between paired cells that create
// connected U-tubes. Boundary dodge (see MagmaTubeMap::m_dodge_distance)
// prevents horizontal weak planes by staggering tube boundaries.
//
// All boundaries are checked in mm (via window_end_z on each UTubePair),
// so variable layer height works correctly.
struct WindowSpec {
    double window_height_mm = 0.4;     // Gap height in mm

    // Default constructor with defaults
    WindowSpec() = default;

    // Construct from config values with auto-calculation support.
    // config_window_height_mm = 0 means auto-calculate from tube geometry.
    static WindowSpec from_config(
        const MagmaGeometry& geom,          // per-shape auto window height formula
        float config_window_height_mm,      // 0 = auto-calculate
        float interior_width,               // for auto window height calc
        float line_width,                   // for auto window height calc
        float layer_height                  // auto adds 1 layer above the calculated height
    );

};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaTriangleCell_hpp_
