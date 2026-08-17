#ifndef slic3r_Magma_MagmaSpiralOffset_hpp_
#define slic3r_Magma_MagmaSpiralOffset_hpp_

#include "MagmaTriangleCell.hpp"
#include "MagmaLattice.hpp"
#include "../PrintConfig.hpp"   // InfillPattern

#include <memory>

namespace Slic3r {
namespace magma {

// Pre-computed spiral parameters, derived once per object from config values.
struct SpiralParams {
    bool   enabled = true;
    float  spiral_radius = 0.f;      // cell_spacing / 2
    float  angle_per_layer = 0.f;    // max_displacement / spiral_radius
};

// Compute spiral parameters once per object from config values.
// interior_width: effective interior width (mm)
// line_width: extrusion line width (mm)
// layer_height: nominal layer height (mm), used to cap helix angle
// enabled: whether spiral interlock is enabled
SpiralParams compute_spiral_params(float interior_width, float line_width, float layer_height, bool enabled);

// Compute per-layer (x,y) offset in mm.
Vec2d compute_spiral_offset(const SpiralParams &params, int layer_id);

// Build the pattern's lattice for a specific layer with spiral offset applied.
// Returns through the MagmaLattice interface so callers stay pattern-agnostic.
std::shared_ptr<MagmaLattice> lattice_for_layer(
    InfillPattern pattern, double cell_spacing, const SpiralParams &params, int layer_id,
    double line_width = 0.0);

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaSpiralOffset_hpp_
