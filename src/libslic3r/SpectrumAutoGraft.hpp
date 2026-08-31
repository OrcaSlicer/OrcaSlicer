#pragma once

#include "PrintConfig.hpp"

#include <string>
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

} // namespace Slic3r
