#include "SettingTips.hpp"

#include <initializer_list>
#include <string>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

// Pictures for the tooltips of the printer settings, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/printer (scripts/gizmo_tips/settings_printer.py, settings_printer_more.py and
// settings_printer_anim.py). See SettingTips.hpp.
void add_printer_setting_tips(SettingTips &tips)
{
    const auto pic = [](const std::string &name) { return "settings/printer/" + name; };
    const auto one = [&](std::initializer_list<const char *> keys, const char *file) {
        for (const char *key : keys)
            tips[key].media = {{pic(file), {}}};
    };

    // Printable space
    one({"printable_area"}, "printable_area.png");
    // The Origin of the rectangular bed in the bed shape dialog (BedShapeDialog.cpp), not a preset key.
    one({"rect_origin"}, "bed_origin.png");
    one({"parallel_printheads_count"}, "parallel_printheads.png");
    one({"bed_exclude_area"}, "bed_exclude_area.png");
    one({"bed_exclude_volumes", "extruder_bed_exclude_volumes"}, "bed_exclude_volumes.png");
    one({"printable_height", "extruder_printable_height"}, "printable_height.png");
    tips["best_object_pos"].media = {{pic("best_object_pos_centre.png"), wxString("0.5, 0.5")},
                                     {pic("best_object_pos_back.png"), wxString("0.5, 0.75")}};
    tips["z_offset"].media = {{pic("z_offset_up.png"), _L("Positive")}, {pic("z_offset_down.png"), _L("Negative")}};
    one({"preferred_orientation"}, "preferred_orientation.png");

    // Advanced
    tips["printer_structure"].values = {
        {"corexy", {pic("structure_corexy.png"), {}}},
        // Hbot moves the same parts as CoreXY; only the belts run differently.
        {"hbot", {pic("structure_corexy.png"), {}}},
        {"i3", {pic("structure_i3.png"), {}}},
        {"delta", {pic("structure_delta.png"), {}}},
    };
    one({"build_plate_tilt_x"}, "plate_tilt_x.png");
    one({"build_plate_tilt_y"}, "plate_tilt_y.png");

    // Belt printer
    one({"belt_printer", "belt_slice_rotation_angle", "belt_slice_rotation"}, "belt_printer.png");
    one({"belt_printer_infinite_y"}, "belt_infinite_y.png");

    // Cooling fan
    one({"fan_speedup_time", "fan_speedup_overhangs"}, "fan_speedup_time.png");
    one({"fan_kickstart"}, "fan_kickstart.png");
    one({"part_cooling_fan_min_pwm"}, "fan_min_pwm.png");

    // Extruder clearance (by-object printing)
    one({"extruder_clearance_radius"}, "extruder_clearance_radius.png");
    one({"extruder_clearance_dist_to_rod"}, "extruder_clearance_dist_to_rod.png");
    one({"extruder_clearance_height_to_rod"}, "extruder_clearance_height_to_rod.png");
    one({"extruder_clearance_height_to_lid"}, "extruder_clearance_height_to_lid.png");
    one({"nozzle_height"}, "nozzle_height.png");

    // Adaptive bed mesh
    one({"bed_mesh_min"}, "bed_mesh_min.png");
    one({"bed_mesh_max"}, "bed_mesh_max.png");
    tips["bed_mesh_probe_distance"].media = {{pic("probe_distance_large.png"), _L("Large")},
                                             {pic("probe_distance_small.png"), _L("Small")}};
    one({"adaptive_bed_mesh_margin"}, "adaptive_bed_mesh_margin.png");

    // Accessory
    one({"auxiliary_fan"}, "aux_fan_left.png");
    tips["fan_direction"].values = {
        {"left", {pic("aux_fan_left.png"), {}}},
        {"right", {pic("aux_fan_right.png"), {}}},
        {"both", {pic("aux_fan_both.png"), {}}},
    };

    // Motion ability
    one({"resonance_avoidance", "min_resonance_avoidance_speed", "max_resonance_avoidance_speed"}, "resonance_avoidance.png");
    one({"input_shaping_emit", "input_shaping_type", "input_shaping_freq_x", "input_shaping_freq_y", "input_shaping_damp_x",
         "input_shaping_damp_y"},
        "input_shaping.png");
    // Each machine limit: the axis it caps, beside what it caps on the move's speed over time.
    for (const std::string axis : {"x", "y", "z", "e"}) {
        const std::string axis_pic = pic("axis_" + axis + ".png");
        tips["machine_max_speed_" + axis].media        = {{axis_pic, {}}, {pic("limit_speed.png"), {}}};
        tips["machine_max_acceleration_" + axis].media = {{axis_pic, {}}, {pic("limit_accel.png"), {}}};
        tips["machine_max_jerk_" + axis].media         = {{axis_pic, {}}, {pic("limit_jerk.png"), {}}};
    }
    one({"machine_max_acceleration_extruding", "machine_max_acceleration_travel"}, "limit_accel.png");
    tips["machine_max_acceleration_retracting"].media = {{pic("axis_e.png"), {}}, {pic("limit_accel.png"), {}}};
    one({"machine_max_junction_deviation"}, "junction_deviation.png");

    // Multimaterial
    one({"single_extruder_multi_material"}, "single_extruder_multi_material.png");
    // The prime tower, drawn for the Process tab's Speed page.
    for (const char *key : {"purge_in_prime_tower", "tool_change_on_wipe_tower", "wait_for_temp_on_wipe_tower"})
        tips[key].media = {{"settings/process/feature_prime_tower.png", {}}};
    one({"enable_filament_ramming"}, "filament_ramming.png");
    one({"cooling_tube_retraction"}, "cooling_tube_position.png");
    one({"cooling_tube_length"}, "cooling_tube_length.png");
    one({"parking_pos_retraction"}, "parking_position.png");
    one({"extra_loading_move"}, "extra_loading_move.png");
    one({"is_imex"}, "imex_idex.png");
    for (const char *key : {"imex_gantry_count", "imex_tools_per_gantry"})
        tips[key].media = {{pic("imex_idex.png"), wxString("IDEX")}, {pic("imex_iqex.png"), wxString("IQEX")}};
    tips["imex_tool_layout"].values = {
        {"front-left", {pic("imex_t0_front_left.png"), {}}},
        {"front-right", {pic("imex_t0_front_right.png"), {}}},
        {"rear-left", {pic("imex_t0_rear_left.png"), {}}},
        {"rear-right", {pic("imex_t0_rear_right.png"), {}}},
    };
    one({"imex_nozzle_clearance_x"}, "imex_clearance_x.png");
    one({"imex_nozzle_clearance_y"}, "imex_clearance_y.png");
    one({"imex_carriage_margin"}, "imex_carriage_margin.png");

    // Extruder: basic information
    tips["nozzle_diameter"].media = {{pic("nozzle_diameter_small.png"), _L("Small")},
                                     {pic("nozzle_diameter_large.png"), _L("Large")}};
    one({"nozzle_volume"}, "nozzle_volume.png");
    one({"extruder_printable_area"}, "extruder_printable_area.png");
    one({"min_layer_height"}, "layer_height_min.png");
    one({"max_layer_height"}, "layer_height_max.png");
    one({"extruder_offset"}, "extruder_offset.png");

    // Retraction
    one({"retraction_length", "retract_length_toolchange"}, "retraction_length.gif");
    one({"retract_restart_extra", "retract_restart_extra_toolchange"}, "retract_restart_extra.gif");
    one({"retraction_speed"}, "retraction_speed.gif");
    one({"deretraction_speed"}, "deretraction_speed.gif");
    one({"retraction_minimum_travel"}, "retraction_minimum_travel.png");
    one({"retract_when_changing_layer"}, "retract_when_changing_layer.png");
    one({"wipe", "wipe_distance"}, "wipe_distance.gif");
    one({"retract_before_wipe"}, "retract_before_wipe.png");
    one({"retract_after_wipe"}, "retract_after_wipe.png");
    one({"long_retractions_when_cut", "retraction_distances_when_cut"}, "retraction_when_cut.gif");

    // Z-hop
    tips["z_hop_types"].values = {
        {"Auto Lift", {pic("z_hop_auto.gif"), {}}},
        {"Normal Lift", {pic("z_hop_normal.gif"), {}}},
        {"Slope Lift", {pic("z_hop_slope.gif"), {}}},
        {"Spiral Lift", {pic("z_hop_spiral.gif"), {}}},
    };
    one({"z_hop"}, "z_hop.png");
    one({"travel_slope"}, "travel_slope.png");
    one({"retract_lift_above", "retract_lift_below"}, "z_hop_range.png");
    tips["retract_lift_enforce"].values = {
        {"All Surfaces", {pic("retract_lift_all.png"), {}}},
        {"Top Only", {pic("retract_lift_top.png"), {}}},
        {"Bottom Only", {pic("retract_lift_bottom.png"), {}}},
        {"Top and Bottom", {pic("retract_lift_top_bottom.png"), {}}},
    };
}

} // namespace Slic3r::GUI
