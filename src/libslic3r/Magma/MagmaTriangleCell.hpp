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
constexpr double MAGMA_SLAM_CLAMP        = 3.5;  // max single z-slam depth (mm)
constexpr double MAGMA_SLAM_PLUNGE_CLAMP = 4.0;  // max slam + plunge depth (mm)
constexpr double MAGMA_SEAL_MARGIN       = 0.1;  // min opening coverage to predict a seal (mm)
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

// Single resolver for the nozzle tip flat (magma_nozzle_outer_diameter). The user
// should measure it (field tooltip says so); when left at 0 ("auto") we estimate
// it from the bore. A typical brass nozzle's flat shoulder is roughly 3x the bore,
// and using the same multiple here and in calculate_auto_interior_width keeps
// full-auto mode self-consistent (opening and flat track each other, so it still
// seals). This is the ONE place the fallback lives, so injection G-code and
// Print::validate always agree; the seal check warns if the estimate can't seal.
constexpr double MAGMA_FLAT_BORE_MULTIPLE = 3.0;
inline double resolve_nozzle_flat(double configured_flat, double nozzle_diameter) {
    return configured_flat > 0.0 ? configured_flat
                                 : MAGMA_FLAT_BORE_MULTIPLE * nozzle_diameter;
}

// Has the user actually measured the nozzle flat, or are we estimating it?
inline bool magma_flat_measured(double configured_flat) { return configured_flat > 0.0; }

// Immersion budget actually permitted. Letting the nozzle enter a tube requires knowing the
// flat: immersion sizes the tube to be WIDER than the flat, so if the flat is an estimate
// that runs high, the tube comes out wider than the nozzle can seal and the injection leaks
// out around it. An unmeasured flat therefore forces seating mode -- the tube is sized to
// fit under the estimated flat and the nozzle never descends into it. The failure then
// degrades to "tubes smaller than they could be" instead of "injection leaks".
inline double effective_max_immersion(double configured_flat, double max_immersion) {
    return magma_flat_measured(configured_flat) ? std::max(0.0, max_immersion) : 0.0;
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
inline double max_opening_for_immersion(double flat, double cone_half_angle_deg, double max_immersion) {
    double tan_t = std::tan(cone_half_angle_deg * MAGMA_DEG2RAD);
    return std::max(0.1, flat + 2.0 * std::max(0.0, max_immersion) * tan_t - MAGMA_SEAL_MARGIN);
}

// Auto Z-slam depth.
//
// The nozzle first touches the opening rim at seal_depth_for_opening(opening, flat);
// everything shallower than that is free descent *inside* the tube, touching nothing.
// Because this solves for opening + MAGMA_SEAL_MARGIN, the mechanical interference past
// first contact is always MAGMA_SEAL_MARGIN / (2*tan θ) -- the opening and the flat
// cancel, so it is identical for every tube size and every nozzle (0.0866mm at 30°).
//
// So interference is NOT what varies between a clean tube and a deformed one. What
// varies is how deep the hot nozzle travels inside the tube before it seals: two test
// prints differing only in that (0.54mm clean, 1.08mm deformed) had byte-identical
// interference. `max_immersion` is therefore the budget that matters, and it caps the
// depth here. In auto tube mode the opening was sized against the same budget so the cap
// never binds; in manual mode it binds and Print::validate() reports the shortfall
// rather than letting the nozzle drive arbitrarily deep.
//
// `press` is the positive-contact press applied when the flat already covers the opening
// and no descent is needed (magma_auto_slam_press).
inline double auto_slam_depth(double opening_dia, double flat, double cone_half_angle_deg,
                              double max_immersion, double press) {
    press = std::max(0.0, press);
    double needed = seal_depth_for_opening(opening_dia + MAGMA_SEAL_MARGIN, flat, cone_half_angle_deg);
    double budget = std::max(press, std::max(0.0, max_immersion));
    return std::min(std::min(std::max(press, needed), budget), MAGMA_SLAM_CLAMP);
}

// Plunge depth clamped so slam + plunge stays within the total intrusion clamp.
inline double clamp_plunge_depth(double slam_depth, double plunge_depth) {
    return std::min(std::max(0.0, plunge_depth), std::max(0.0, MAGMA_SLAM_PLUNGE_CLAMP - slam_depth));
}

// Auto-calculate interior width from nozzle diameter (fallback: 3× bore).
// Used only when no nozzle flat has been measured; see resolve_nozzle_flat().
double calculate_auto_interior_width(double nozzle_diameter);

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

    double inscribed_radius(double interior_width) const override { return interior_width * 0.5; }

    // Centroid-to-neighbor-centroid distance = circumradius = side/sqrt3.
    double neighbor_centroid_distance(double spacing) const override {
        return triangle_side_length(spacing) / SQRT3;
    }

    double interlock_radius(double spacing) const override { return spacing * 0.5; }

    // 3 line families crossing at 60deg per vertex: (3*sqrt3/4) * w^2. (Only subtracted from
    // injection volume when the overlap flow correction is off — see MagmaGeometry.hpp.)
    double vertex_overlap_excess_area(double line_width) const override {
        return (3.0 * SQRT3 / 4.0) * line_width * line_width;
    }

    double line_overlap_excess_fraction(double spacing, double line_width) const override {
        return spacing > 0.0 ? 3.0 * line_width / (4.0 * spacing) : 0.0;
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
    int cells_per_pair() const override { return 2; }
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
