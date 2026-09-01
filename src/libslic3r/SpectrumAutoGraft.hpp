#pragma once

#include "PrintConfig.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// True iff printer_model == "WonderMaker ZR Ultra S" or printer_settings_id contains
// "WonderMaker ZR Ultra". Name gate only — missing nozzle_diameter / SEMM does not reject.
bool spectrum_is_zr_ultra_s_dest(const DynamicPrintConfig &cfg);

// True iff filament_colour exists, 1 <= size <= 4, and !spectrum_is_zr_ultra_s_dest(cfg).
bool spectrum_should_auto_graft_leq4(const DynamicPrintConfig &cfg);

// True iff this 3mf load is a full project open (File→Open / drag-drop OpenProject),
// not LoadConfig-only and not backup Restore. Bools only — no LoadStrategy here.
bool spectrum_auto_graft_load_is_project_open(bool load_model, bool load_config, bool is_restore);

// Resize colour to 4, then copy/pad from dest[0..3]. dest empty → false, no write.
// dest size 1–3 pads with last hex; dest ≥4 copies first four.
bool spectrum_restore_dest_filament_colours(std::vector<std::string>       &colour,
                                           const std::vector<std::string> &dest);

// Empty types → ""; else types[min(i, size-1)] (pad last like hex restore).
std::string spectrum_filament_type_at(const std::vector<std::string> &types, size_t i);

// Skip session hex restore when the loaded mix string is Snapmaker FullSpec
// custom-entry grammar — keep file filament_colour (M/Y/C/W). Thin wrapper
// around spectrum_mix_looks_like_snapmaker_custom_entries.
bool spectrum_keep_imported_filament_colours(const std::string &mixed_filament_definitions);

// Exact type ==. Empty wanted → session_name. Prefer session_name if that pair
// matches wanted; else first pair with type==wanted; else "".
std::string spectrum_pick_filament_name_for_type(
    const std::string                                       &wanted,
    const std::string                                       &session_name,
    const std::vector<std::pair<std::string, std::string>> &name_and_type);

} // namespace Slic3r
