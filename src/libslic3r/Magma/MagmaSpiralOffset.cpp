#include "MagmaSpiralOffset.hpp"
#include "MagmaPatterns.hpp"

#include <cmath>
#include <algorithm>

namespace Slic3r {
namespace magma {

SpiralParams compute_spiral_params(float interior_width, float line_width, float layer_height, bool enabled)
{
    SpiralParams params;
    params.enabled = enabled;

    if (!enabled) {
        params.spiral_radius = 0.f;
        params.angle_per_layer = 0.f;
        return params;
    }

    // Constraint 1: Printability - 40% line overlap between layers
    constexpr float target_line_overlap = 0.40f;
    const float max_disp_line = (1.0f - target_line_overlap) * line_width;

    // Constraint 2: Tube continuity - 75% tube area overlap between layers
    constexpr float target_tube_overlap = 0.75f;
    const float max_disp_tube = (1.0f - target_tube_overlap) * interior_width;

    // Constraint 3: Maximum helix angle for injection flow and thin sections.
    // At low layer heights the per-layer geometric constraints allow the same
    // horizontal displacement as at thick layers, but over much less vertical
    // distance, producing a steep helix that resists injection flow and takes
    // more volumetric space (harder to fit tubes in thin sections).  Cap so
    // the helix angle never exceeds ~27° (tan(27°) ≈ 0.5).
    constexpr float MAX_HELIX_TAN = 0.5f;   // tan(~27°)
    const float max_disp_helix = MAX_HELIX_TAN * layer_height;

    // Use the most restrictive constraint
    const float max_displacement = std::min({max_disp_line, max_disp_tube, max_disp_helix});

    // Interlock constraint: swept circles of adjacent tubes should touch
    const float cell_spacing = static_cast<float>(cell_spacing_from_geometry(interior_width, line_width));
    params.spiral_radius = cell_spacing / 2.0f;

    // Per-layer angle follows from radius and displacement constraints
    params.angle_per_layer = max_displacement / params.spiral_radius;

    return params;
}

Vec2d compute_spiral_offset(const SpiralParams &params, int layer_id)
{
    if (!params.enabled)
        return Vec2d(0.0, 0.0);

    const double layer_angle = double(layer_id) * double(params.angle_per_layer);
    return Vec2d(
        params.spiral_radius * std::cos(layer_angle),
        params.spiral_radius * std::sin(layer_angle)
    );
}

std::shared_ptr<MagmaLattice> lattice_for_layer(
    InfillPattern pattern, double cell_spacing, const SpiralParams &params, int layer_id,
    double line_width)
{
    Vec2d offset = compute_spiral_offset(params, layer_id);
    return make_magma_lattice(pattern, cell_spacing, offset.x(), offset.y(), line_width);
}

} // namespace magma
} // namespace Slic3r
