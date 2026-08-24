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
    // True when nothing was measured and nozzle_flat is resolve_nozzle_flat's bore-scaled
    // guess. Print::validate refuses to slice in that state, so it can only be reached by the
    // GUI readout -- which would otherwise print tube sizes derived from a guess as though
    // they were derived from a nozzle. Every consumer of nozzle_flat has to be able to say so.
    bool   nozzle_flat_is_estimate = false;
    double cone_half_angle_deg = 0.0;
    double seal_press          = 0.0;  // depth pressed past first contact, before injection

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

    // --- derived depths, NOMINAL ---
    // Computed from the nominal opening above. The G-code path recomputes these per tube
    // from that tube's own clipped opening, so a tube whose top was clipped narrow seals
    // deeper than this. Use these for warnings and readouts, never to emit a move.
    //
    // The nozzle reaches seal_depth in one fast Z move before anything is extruded, then
    // sinks the further plunge_depth ACROSS the injection, arriving at total_depth() as the
    // last of the filament goes in and holding there through the dwell.
    double seal_depth   = 0.0;  // derived from the tube width -- not a free choice
    double plunge_depth = 0.0;  // additive, during the injection

    // What holds the seal shut: (press + plunge) * tan(theta). Press and plunge are the same
    // kind of thing -- depth past first contact -- one applied before the injection and one
    // during it, so they add. The
    // corner is the last part of the opening the cone covers and therefore the least pressed,
    // so this is the smallest contact in the system and the one that fails first.
    double grip         = 0.0;
    // Cone diameter at full depth. Compared against the CELL PITCH, not against the tube: past
    // roughly MAGMA_PITCH_WARN_RATIO the nozzle is crushing the neighbouring cells.
    double cone_at_full = 0.0;

    // What the user asked for, before the budget clamped it. Kept so the readout and
    // Print::validate can say a value was reduced instead of quietly showing a different
    // number than the one that was typed.
    double plunge_requested = 0.0;
    bool   plunge_clamped() const { return plunge_requested > plunge_depth + 1e-9; }

    // Ratio the grid-disruption warning fires on.
    double pitch_ratio() const {
        const double pitch = cell_spacing;
        return pitch > 0.0 ? cone_at_full / pitch : 0.0;
    }

    // Total depth below the print surface the nozzle reaches at the end of injection.
    // A REPORTED consequence now, not a budget that was spent: the seal depth is whatever the
    // chosen tube costs, and the plunge is added on top.
    double total_depth() const { return seal_depth + plunge_depth; }

    // Opening the cone actually covers at that depth. Compare against opening_diameter
    // to decide whether this nozzle can seal this tube at all.
    double covered_diameter() const;
    bool   seals() const { return covered_diameter() >= opening_diameter; }
};

// Resolve everything above from the three configs that own the inputs. Returns false when
// the effective pattern is not a Magma pattern, in which case `out` is untouched.
//
// `region` supplies the pattern and tube width; `object` supplies the nozzle flat, cone,
// immersion budget and minimum seal depth; `print` supplies the nozzle diameters.
bool resolve_magma(const PrintRegionConfig &region,
                   const PrintObjectConfig &object,
                   const PrintConfig       &print,
                   MagmaResolved           &out);

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaResolved_hpp_
