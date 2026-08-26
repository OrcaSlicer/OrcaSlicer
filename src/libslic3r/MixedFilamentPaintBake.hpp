#pragma once

#include "Color.hpp"
#include "TriangleSelector.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class ModelVolume;

struct SpectrumPaintBakePlan
{
    bool                      valid = false;
    std::string               mixed_filament_definitions; // ';' rows; empty if all Physical
    EnforcerBlockerStateMap   slot_map{};                 // identity except mapped source IDs
    size_t                    mix_count             = 0;
    size_t                    physical_mapped_count = 0;
    std::string               error;                      // if !valid
};

SpectrumPaintBakePlan plan_spectrum_paint_bake(
    const std::vector<std::string> &source_hexes, // spectrum_source_filament_colour
    const std::vector<ColorRGB>    &physicals);   // live filament_colour; first 4 used

// Deserialize (keep IDs 5–8), remap_triangle_state, FacetsAnnotation::set.
bool apply_spectrum_paint_bake(ModelVolume &vol, const EnforcerBlockerStateMap &slot_map);

} // namespace Slic3r
