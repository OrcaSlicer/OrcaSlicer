#pragma once

#include "libslic3r/Config.hpp"

namespace Slic3r {

class PresetBundle;

// physical_extruder_map has one entry per LOGICAL extruder -- the index space of
// nozzle_diameter -- with each value a physical extruder index. It is NOT derivable from
// printer_extruder_id, which is indexed by variant slot (one entry per extruder+variant pair):
// an X1 Carbon has one nozzle and printer_extruder_id {1,1}, an H2D two nozzles and {1,1,2,2,2}.
//
// The setting belongs to every printer, not only IMEX ones: the single-element PrintConfig default
// is what any multi-nozzle profile that never sets it (a Snapmaker U1, say) carries, so reading it
// raw by logical extruder runs past its end. GUI and device-side readers go through these helpers;
// the non-IMEX G-code paths read the raw value on purpose (see the is_imex gate in PrintApply.cpp).
//
// A profile has authored a map only when its length matches nozzle_count; otherwise the identity
// is returned, the default upstream's printer sync starts from before it applies an authored map.
ConfigOptionInts effective_physical_extruder_map(const ConfigOptionInts* explicit_pem, int nozzle_count);

// GUI overload: resolves the effective pem from a live PresetBundle using the project_config
// → printer preset fallback. Use this instead of open-coding the lookup at ghost-color,
// tooltip, click-gate and cache-key call sites so they all agree with what the slicer sees.
ConfigOptionInts effective_physical_extruder_map(const PresetBundle& pb);

} // namespace Slic3r
