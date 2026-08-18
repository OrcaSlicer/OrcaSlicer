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

    // Radius of the largest circle that fits inside the open tube -- the usable bore, as
    // opposed to opening_diameter()'s circumscribed circle that the nozzle must cover.
    // Takes line_width because a triangle's inset reduces the inradius by more than half a
    // bead: interior/2 is right for square and hex and wrong only for triangle, which is
    // exactly the near-miss that shipped.
    virtual double inscribed_radius(double interior_width, double line_width) const = 0;

    // Center-to-center distance to an edge-sharing neighbor cell (the injection
    // crater-iron neighbor-clearance "D").
    virtual double neighbor_centroid_distance(double spacing) const = 0;

    // Excess area (mm^2) deposited per cell where infill lines cross at vertices. Lines always
    // print at full width, so this material is always present and is always subtracted from the
    // injection volume.
    virtual double vertex_overlap_excess_area(double line_width) const = 0;

    // Geometric window height so the window's flow cross-section equals the tube's
    // open cross-section area (mm). Both args in mm; spacing is derived internally.
    virtual double auto_window_height(double interior_width, double line_width) const = 0;

    // Inverse of opening_diameter(): the interior width whose seal opening is exactly
    // `opening`. Auto tube sizing feeds this the largest opening the injection immersion
    // budget allows, so the tube comes out as big as the deformation budget permits.
    // NOTE: this MUST be reached through the geometry, not a shape-specific free function --
    // sizing every pattern with the triangle formula silently mis-sizes rect and hex.
    virtual double interior_for_opening(double opening, double line_width) const = 0;

    // Topology constants.
    virtual int max_neighbors() const = 0;   // edge-sharing neighbor count (3/4/6)
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaGeometry_hpp_
