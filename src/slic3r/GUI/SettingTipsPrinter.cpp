#include "SettingTips.hpp"

#include <string>

namespace Slic3r::GUI {

// Pictures for the tooltips of the printer settings, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/printer (scripts/gizmo_tips/settings_printer.py). See SettingTips.hpp.
void add_printer_setting_tips(SettingTips &tips)
{
    const auto pic = [](const char *name) { return std::string("settings/printer/") + name; };

    // Bed
    tips["printable_area"].media = {{pic("printable_area.png"), {}}};
    // The Origin of the rectangular bed in the bed shape dialog (BedShapeDialog.cpp), not a preset key.
    tips["rect_origin"].media              = {{pic("bed_origin.png"), {}}};
    tips["bed_exclude_area"].media         = {{pic("bed_exclude_area.png"), {}}};
    tips["adaptive_bed_mesh_margin"].media = {{pic("adaptive_bed_mesh_margin.png"), {}}};

    // By-object printing
    tips["extruder_clearance_radius"].media        = {{pic("extruder_clearance_radius.png"), {}}};
    tips["extruder_clearance_height_to_rod"].media = {{pic("extruder_clearance_height_to_rod.png"), {}}};
    tips["extruder_clearance_height_to_lid"].media = {{pic("extruder_clearance_height_to_lid.png"), {}}};
    tips["nozzle_height"].media                    = {{pic("nozzle_height.png"), {}}};

    // Z-hop
    tips["z_hop_types"].values = {
        {"Auto Lift", {pic("z_hop_auto.gif"), {}}},
        {"Normal Lift", {pic("z_hop_normal.gif"), {}}},
        {"Slope Lift", {pic("z_hop_slope.gif"), {}}},
        {"Spiral Lift", {pic("z_hop_spiral.gif"), {}}},
    };
    tips["travel_slope"].media       = {{pic("travel_slope.png"), {}}};
    tips["retract_lift_above"].media = {{pic("z_hop_range.png"), {}}};
    tips["retract_lift_below"].media = {{pic("z_hop_range.png"), {}}};
    tips["retract_lift_enforce"].values = {
        {"All Surfaces", {pic("retract_lift_all.png"), {}}},
        {"Top Only", {pic("retract_lift_top.png"), {}}},
        {"Bottom Only", {pic("retract_lift_bottom.png"), {}}},
        {"Top and Bottom", {pic("retract_lift_top_bottom.png"), {}}},
    };

    // Retraction
    tips["wipe_distance"].media         = {{pic("wipe_distance.gif"), {}}};
    tips["retract_restart_extra"].media = {{pic("retract_restart_extra.png"), {}}};
}

} // namespace Slic3r::GUI
