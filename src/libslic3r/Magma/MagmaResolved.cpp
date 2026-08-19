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

    // Every nozzle-derived number below must come from the extruder that actually prints the
    // lattice. With dual infill that is the ZONE filament, not the sparse infill one: the lattice
    // lives in the outer zone. Sizing it against sparse infill's nozzle described a tool that does
    // not print it on any mixed-nozzle machine -- the same plan-versus-emit split as the injection
    // extruder, one layer up.
    //
    // These ids are 1-based with 0 = "Default"; PrintApply resolves 0 to a real id before slicing,
    // and the max() covers the GUI readout, which resolves against an unresolved preset.
    const int lattice_filament = region.dual_infill_enabled.value
                                     ? region.dual_infill_outer_filament.value
                                     : region.sparse_infill_filament_id.value;
    r.sparse_extruder = std::max(0, lattice_filament - 1);
    r.nozzle_diameter = print.nozzle_diameter.get_at(r.sparse_extruder);

    r.line_width = region.sparse_infill_line_width.get_abs_value(r.nozzle_diameter);
    if (r.line_width <= 0.0)
        // 0 means "auto" in OrcaSlicer. Resolve it the way the slicer does. Standing in the
        // nozzle diameter here (as two of the three callers used to) is a different and
        // larger number, so the tube it describes is not the tube that gets printed.
        r.line_width = Flow::auto_extrusion_width(frInfill, float(r.nozzle_diameter));

    // Indexed by the extruder each number describes. This is the whole point of the option being
    // per-extruder: the lattice and the injection can be different tools with different tips.
    r.nozzle_flat_is_estimate = object.magma_nozzle_outer_diameter.get_at(r.sparse_extruder) <= 0.0;
    r.nozzle_flat         = resolve_nozzle_flat(object.magma_nozzle_outer_diameter.get_at(r.sparse_extruder),
                                                r.nozzle_diameter);

    const int inj_filament = object.magma_injection_filament.value;
    r.injection_extruder   = inj_filament > 0 ? inj_filament - 1 : r.sparse_extruder;
    r.injection_nozzle_diameter = print.nozzle_diameter.get_at(r.injection_extruder);
    r.injection_nozzle_flat     = resolve_nozzle_flat(
        object.magma_nozzle_outer_diameter.get_at(r.injection_extruder), r.injection_nozzle_diameter);
    // The cone belongs to the tool that does the sealing, which is the injection extruder.
    r.cone_half_angle_deg = object.magma_nozzle_cone_half_angle.get_at(r.injection_extruder);
    r.max_immersion       = object.magma_max_immersion.value;
    r.min_seal_depth      = object.magma_auto_slam_press.value;

    // Auto sizing must leave room for the plunge, or the seal consumes the whole budget and
    // the plunge clamps to zero (see effective_interior_width). Resolved before sizing and
    // reused for the clamp below, so the depth the tube was sized around and the depth the
    // nozzle is asked for are the same number rather than two readings of one option.
    r.plunge_requested = object.magma_injection_plunge.value
                             ? std::max(0.0, object.magma_injection_plunge_depth.value)
                             : 0.0;
    r.interior_width   = effective_interior_width(*r.geometry,
                                                  object.magma_tube_width_mode.value,
                                                  object.magma_interior_width.value,
                                                  r.nozzle_flat, r.line_width,
                                                  r.cone_half_angle_deg, r.max_immersion,
                                                  r.plunge_requested);
    r.cell_spacing     = cell_spacing_from_geometry(r.interior_width, r.line_width);
    r.opening_diameter = r.geometry->opening_diameter(r.cell_spacing, r.line_width);
    r.bore_diameter    = 2.0 * r.geometry->inscribed_radius(r.interior_width, r.line_width);

    // The one budget both depths live under. auto_seal_depth and clamp_plunge_depth are both
    // handed this same value, which is what makes total_depth() <= max_immersion hold without
    // any call site here clamping again.
    r.budget           = immersion_budget(r.max_immersion);
    r.seal_depth       = auto_seal_depth(r.opening_diameter, r.nozzle_flat,
                                         r.cone_half_angle_deg, r.budget, r.min_seal_depth);
    r.plunge_depth     = clamp_plunge_depth(r.seal_depth, r.plunge_requested, r.budget);

    out = r;
    return true;
}

} // namespace magma
} // namespace Slic3r
