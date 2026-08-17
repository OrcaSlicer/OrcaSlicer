#ifndef slic3r_Magma_MagmaPatterns_hpp_
#define slic3r_Magma_MagmaPatterns_hpp_

#include "../PrintConfig.hpp"        // InfillPattern, ipMagmaTriangle
#include "MagmaGeometry.hpp"
#include "MagmaLattice.hpp"
#include "MagmaTriangleCell.hpp"     // triangle_geometry(), TriangleLattice
#include "MagmaRectilinearCell.hpp"  // square_geometry(), RectilinearLattice
#include "MagmaTriHexCell.hpp"       // trihex_geometry(), TriHexLattice
#include "MagmaHexCell.hpp"          // hex_geometry(), HexLattice

#include <memory>

namespace Slic3r {
namespace magma {

// ============================================================================
// MagmaPatterns — registry-lite pattern → geometry/lattice selectors
// ============================================================================
//
// One place that maps an InfillPattern onto its MagmaGeometry (scalar shape
// formulas) and constructs its MagmaLattice (topology / coordinates). The tube
// map, spiral-offset builder, and Print::validate() route through these so the
// pipeline picks the *pattern's* shape rather than hardcoding triangle. Adding a
// second Magma pattern means adding a case here (plus its Geometry/Lattice impl)
// rather than editing every call site.
//
// Today only ipMagmaTriangle exists; both selectors default to the triangle
// implementation so behavior is byte-identical to the previous hardcoding.

// Shape formula strategy for a pattern (shared, stateless instance).
inline const MagmaGeometry& magma_geometry_for(InfillPattern p)
{
    switch (p) {
    case ipMagmaRectilinear:
        return square_geometry();
    case ipMagmaTriHex:
        return trihex_geometry();
    case ipMagmaHoneycomb:
        return hexagon_geometry();
    case ipMagmaTriangle:
    default:
        return triangle_geometry();
    }
}

// Build the topology/coordinate lattice for a pattern with the given cell
// spacing and spiral offset.
inline std::unique_ptr<MagmaLattice> make_magma_lattice(
    InfillPattern p, double cell_spacing, double offset_x, double offset_y, double line_width = 0.0)
{
    switch (p) {
    case ipMagmaRectilinear:
        return std::make_unique<RectilinearLattice>(cell_spacing, offset_x, offset_y);
    case ipMagmaTriHex:
        return std::make_unique<TriHexLattice>(cell_spacing, offset_x, offset_y);
    case ipMagmaHoneycomb:
        return std::make_unique<HexLattice>(cell_spacing, offset_x, offset_y, line_width);
    case ipMagmaTriangle:
    default:
        return std::make_unique<TriangleLattice>(cell_spacing, offset_x, offset_y);
    }
}

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaPatterns_hpp_
