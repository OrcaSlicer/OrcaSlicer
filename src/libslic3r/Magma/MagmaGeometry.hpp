#ifndef slic3r_Magma_MagmaGeometry_hpp_
#define slic3r_Magma_MagmaGeometry_hpp_

namespace Slic3r {
namespace magma {

// ============================================================================
// MagmaGeometry — per-cell-shape geometry strategy
// ============================================================================
//
// Each Magma infill pattern (triangle, rectilinear, tri-hex, ...) supplies one
// implementation of this interface. The tube map, injection G-code, and
// Print::validate() consume it so every shape-specific formula lives in exactly
// ONE place -- which is what lets warnings stay in lockstep with the g-code
// (they call the same functions) and lets a new pattern drop in by providing a
// handful of numbers rather than editing twenty call sites.
//
// Conventions:
//  - "spacing"      = center-to-center distance between adjacent parallel infill
//                     lines (== interior_width + line_width for the triangle grid).
//  - "line_width"   = the *effective* (deposited) bead width unless the caller
//                     passes a nominal width deliberately (e.g. pre-build estimates).
//  - All lengths in mm, areas in mm^2, volumes in mm^3. No scaled coordinates.
//
// This interface holds only pure shape formulas + topology constants. Coordinate
// transforms (cell positions, neighbors, corners) belong to MagmaLattice; the
// nozzle seal math (z-slam/plunge/coverage) stays shape-agnostic and consumes
// opening_diameter()/neighbor_centroid_distance() as scalar inputs.
struct MagmaGeometry
{
    virtual ~MagmaGeometry() = default;

    // Edge (side) length of one cell from the line spacing.
    virtual double edge_length(double spacing) const = 0;

    // Open tube cross-section after insetting each wall by half the line width (mm^2).
    virtual double inset_open_area(double spacing, double line_width) const = 0;

    // Diameter of the circle that must be covered to seal the tube top
    // (circumscribed circle of the inset opening). Feeds the z-slam/seal math.
    virtual double opening_diameter(double spacing, double line_width) const = 0;

    // Radius of the largest circle that fits inside the open tube (flow check).
    virtual double inscribed_radius(double interior_width) const = 0;

    // Center-to-center distance to an edge-sharing neighbor cell (the injection
    // crater-iron neighbor-clearance "D").
    virtual double neighbor_centroid_distance(double spacing) const = 0;

    // Spiral-interlock radius: how far a tube's swept footprint may shift before
    // touching a neighbor.
    virtual double interlock_radius(double spacing) const = 0;

    // Excess area (mm^2) deposited per cell where infill lines cross at vertices. Subtracted
    // from injection volume ONLY when the overlap flow correction is OFF — when it's on, the
    // reduced flow already removed this material, so subtracting again would double-count.
    virtual double vertex_overlap_excess_area(double line_width) const = 0;

    // Fraction of deposited line material doubled at line crossings.
    virtual double line_overlap_excess_fraction(double spacing, double line_width) const = 0;

    // Geometric window height so the window's flow cross-section equals the tube's
    // open cross-section area (mm). Both args in mm; spacing is derived internally.
    virtual double auto_window_height(double interior_width, double line_width) const = 0;

    // Largest tube interior width whose opening still fits within the nozzle flat.
    virtual double auto_interior_width_from_od(double nozzle_od, double line_width) const = 0;

    // Topology constants.
    virtual int max_neighbors() const = 0;   // edge-sharing neighbor count (3/4/6)
    virtual int cells_per_pair() const = 0;   // cells joined into one injected U-tube
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaGeometry_hpp_
