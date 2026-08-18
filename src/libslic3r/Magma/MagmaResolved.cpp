#include "MagmaResolved.hpp"

#include <algorithm>

#include "../Flow.hpp"
#include "MagmaPatterns.hpp"
#include "MagmaTriangleCell.hpp"
#include "MagmaTubeMap.hpp"

namespace Slic3r {
namespace magma {

double MagmaResolved::covered_diameter() const
{
    return cone_coverage_at_depth(this->total_depth(), nozzle_flat, cone_half_angle_deg);
}

bool resolve_magma(const PrintRegionConfig &region,
                   const PrintObjectConfig &object,
                   const PrintConfig       &print,
                   MagmaResolved           &out)
{
    const InfillPattern pattern = magma_effective_pattern(region);
    if (! is_magma_pattern(pattern))
        return false;

    MagmaResolved r;
    r.pattern  = pattern;
    r.geometry = &magma_geometry_for(pattern);

    // The lattice is printed by the SPARSE INFILL extruder, so every nozzle-derived number
    // below must come from that extruder. Reading nozzle_diameter[0] instead describes a
    // nozzle that does not print the lattice on any mixed-nozzle machine.
    r.sparse_extruder = std::max(0, region.sparse_infill_filament.value - 1);
    r.nozzle_diameter = print.nozzle_diameter.get_at(r.sparse_extruder);

    r.line_width = region.sparse_infill_line_width.get_abs_value(r.nozzle_diameter);
    if (r.line_width <= 0.0)
        // 0 means "auto" in OrcaSlicer. Resolve it the way the slicer does. Standing in the
        // nozzle diameter here (as two of the three callers used to) is a different and
        // larger number, so the tube it describes is not the tube that gets printed.
        r.line_width = Flow::auto_extrusion_width(frInfill, float(r.nozzle_diameter));

    r.nozzle_flat         = resolve_nozzle_flat(object.magma_nozzle_outer_diameter.value,
                                                r.nozzle_diameter);

    const int inj_filament = object.magma_injection_filament.value;
    r.injection_extruder   = inj_filament > 0 ? inj_filament - 1 : r.sparse_extruder;
    r.injection_nozzle_diameter = print.nozzle_diameter.get_at(r.injection_extruder);
    r.injection_nozzle_flat     = resolve_nozzle_flat(object.magma_nozzle_outer_diameter.value,
                                                      r.injection_nozzle_diameter);
    r.cone_half_angle_deg = object.magma_nozzle_cone_half_angle.value;
    r.max_immersion       = object.magma_max_immersion.value;
    r.slam_press          = object.magma_auto_slam_press.value;

    // Auto sizing must leave room for the plunge, or the seal consumes the whole budget and
    // the plunge clamps to zero (see effective_interior_width).
    const double plunge_reserve = object.magma_injection_plunge.value
                                      ? std::max(0.0, object.magma_injection_plunge_depth.value)
                                      : 0.0;
    r.interior_width   = effective_interior_width(*r.geometry,
                                                  object.magma_tube_width_mode.value,
                                                  object.magma_interior_width.value,
                                                  r.nozzle_flat, r.line_width,
                                                  r.cone_half_angle_deg, r.max_immersion,
                                                  plunge_reserve);
    r.cell_spacing     = cell_spacing_from_geometry(r.interior_width, r.line_width);
    r.opening_diameter = r.geometry->opening_diameter(r.cell_spacing, r.line_width);
    r.bore_diameter    = 2.0 * r.geometry->inscribed_radius(r.interior_width, r.line_width);

    // The budget both depths live under. slam_press participates because a press is applied
    // even when the flat already covers the opening and no descent is geometrically needed.
    r.immersion_budget = immersion_budget_for(r.slam_press, r.max_immersion);
    // The user's offset is added before the clamp, exactly as MagmaInjection does it, so a
    // positive offset in Auto tube mode is absorbed by the budget rather than deepening the
    // seal -- and the readout says the same thing the G-code will do.
    r.slam_depth       = std::min(r.immersion_budget,
                                  std::max(0.0, auto_slam_depth(r.opening_diameter, r.nozzle_flat,
                                                                r.cone_half_angle_deg,
                                                                r.max_immersion, r.slam_press)
                                                + object.magma_injection_z_slam_offset.value));
    r.plunge_depth     = object.magma_injection_plunge.value
                             ? clamp_plunge_depth(r.slam_depth,
                                                  object.magma_injection_plunge_depth.value,
                                                  r.immersion_budget)
                             : 0.0;

    out = r;
    return true;
}

} // namespace magma
} // namespace Slic3r
