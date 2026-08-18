#ifndef slic3r_Magma_MagmaResolved_hpp_
#define slic3r_Magma_MagmaResolved_hpp_

#include "../PrintConfig.hpp"
#include "MagmaGeometry.hpp"

namespace Slic3r {
namespace magma {

// ============================================================================
// MagmaResolved — every value derived from config, resolved exactly once
// ============================================================================
//
// Three places need the same set of derived numbers: MagmaTubeMap::build (which
// prints the lattice), Print::validate (which warns about it), and PresetHints
// (which shows the user what they are about to get). Each used to walk the same
// derivation itself, and they drifted -- the nozzle to measure from, the fallback
// when the infill line width is "auto", which geometry the sizing goes through.
// The formulas were always shared; the INPUTS were not, which is how a warning
// ends up evaluated against a tube that is never printed.
//
// So the derivation lives here, once, and those three consume the result. Adding
// a derived quantity means adding a field, not another copy of the chain.
struct MagmaResolved
{
    // --- what the lattice is ---
    InfillPattern        pattern   = ipMagmaRectilinear;
    const MagmaGeometry *geometry  = nullptr;

    // --- inputs, after resolution ---
    int    sparse_extruder     = 0;    // 0-based index of the extruder printing the lattice
    double nozzle_diameter     = 0.0;  // that extruder's bore
    double line_width          = 0.0;  // deposited bead width ("auto" already resolved)
    double nozzle_flat         = 0.0;  // measured tip flat (or the bore-scaled stand-in)
    double cone_half_angle_deg = 0.0;
    double max_immersion       = 0.0;
    double slam_press          = 0.0;

    // The INJECTION extruder is not necessarily the one that prints the lattice: with a
    // dedicated injection filament it is a different tool with its own nozzle, and the seal
    // is made by that nozzle. Anything about sealing uses these; anything about printing the
    // lattice uses the sparse_* fields above. Conflating them seals against the wrong tip.
    int    injection_extruder       = 0;    // 0-based; falls back to sparse_extruder
    double injection_nozzle_diameter = 0.0;
    double injection_nozzle_flat     = 0.0;

    // --- derived geometry ---
    double interior_width   = 0.0;  // open face-to-face width of one cell
    double cell_spacing     = 0.0;  // centre-to-centre line spacing
    double opening_diameter = 0.0;  // circle the nozzle must cover to seal
    double bore_diameter    = 0.0;  // largest circle that fits inside the tube

    // --- derived seal depths, NOMINAL ---
    // Computed from the nominal opening above. The G-code path recomputes these per tube
    // from that tube's own clipped opening, so a tube whose top was clipped narrow seals
    // deeper than this. Use these for warnings and readouts, never to emit a move.
    double immersion_budget = 0.0;  // the cap both depths are held under
    double slam_depth       = 0.0;
    double plunge_depth     = 0.0;

    // Total depth below the print surface the nozzle reaches at the end of injection.
    double total_depth() const { return slam_depth + plunge_depth; }

    // Opening the cone actually covers at that depth. Compare against opening_diameter
    // to decide whether this nozzle can seal this tube at all.
    double covered_diameter() const;
    bool   seals() const { return covered_diameter() >= opening_diameter; }
};

// Resolve everything above from the three configs that own the inputs. Returns false when
// the effective pattern is not a Magma pattern, in which case `out` is untouched.
//
// `region` supplies the pattern, tube width, nozzle flat and cone; `object` supplies the
// immersion budget and slam press; `print` supplies the nozzle diameters.
bool resolve_magma(const PrintRegionConfig &region,
                   const PrintObjectConfig &object,
                   const PrintConfig       &print,
                   MagmaResolved           &out);

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaResolved_hpp_
