#include "SettingTips.hpp"

#include <string>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IllustratedTipPopup.hpp"

namespace Slic3r::GUI {

namespace {

// One picture under settings/process, without a caption.
std::vector<TipMedia> picture(const std::string &name)
{
    return {{"settings/process/" + name + ".png", {}}};
}

// A low and a high value of the setting side by side: <name>_low.png and <name>_high.png.
std::vector<TipMedia> low_high(const std::string &name)
{
    return {{"settings/process/" + name + "_low.png", _L("Low")}, {"settings/process/" + name + "_high.png", _L("High")}};
}

} // namespace

// Pictures for the settings of the Process tab's Speed page, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/process (scripts/gizmo_tips/settings_process_speed.py). See SettingTips.hpp.
//
// A setting that changes one kind of line shows the picture of that line: feature_<name>.png is one test
// part, sliced by OrcaSlicer, with that kind of line in the accent colour. The other pages' settings that
// change one kind of line use the same pictures.
void add_process_speed_setting_tips(SettingTips &tips)
{
    // First layer.
    tips["initial_layer_speed"].media = picture("speed_first_layer_walls");
    tips["initial_layer_infill_speed"].media = picture("speed_first_layer_infill");
    tips["initial_layer_travel_speed"].media = picture("speed_first_layer_travel");
    tips["slow_down_layers"].media = picture("speed_slow_down_layers");

    // Other layers.
    tips["outer_wall_speed"].media = picture("feature_outer_wall");
    tips["inner_wall_speed"].media = picture("feature_inner_wall");
    tips["small_perimeter_speed"].media = picture("speed_small_perimeter_high");
    tips["small_perimeter_threshold"].media = low_high("speed_small_perimeter");
    tips["sparse_infill_speed"].media = picture("feature_sparse_infill");
    tips["internal_solid_infill_speed"].media = picture("feature_internal_solid_infill");
    tips["top_surface_speed"].media = picture("feature_top_surface");
    tips["gap_infill_speed"].media = picture("feature_gap_fill");
    tips["ironing_speed"].media = picture("feature_ironing");
    tips["support_speed"].media = picture("feature_support");
    tips["support_interface_speed"].media = picture("feature_support_interface");
    tips["small_support_perimeter_speed"].media = picture("speed_small_support_high");
    tips["small_support_perimeter_threshold"].media = low_high("speed_small_support");

    // Overhangs: a wall in section, each layer hanging further out and printed slower.
    tips["enable_overhang_speed"].media = picture("speed_overhang");
    tips["slowdown_for_curled_perimeters"].media = picture("speed_curled_perimeters");
    tips["overhang_1_4_speed"].media = picture("speed_overhang_1");
    tips["overhang_2_4_speed"].media = picture("speed_overhang_2");
    tips["overhang_3_4_speed"].media = picture("speed_overhang_3");
    tips["overhang_4_4_speed"].media = picture("speed_overhang_4");
    tips["bridge_speed"].media = picture("feature_bridge");
    tips["internal_bridge_speed"].media = picture("feature_internal_bridge");

    // Travel.
    tips["travel_speed"].media = picture("feature_travel");

    // Acceleration: the nozzle's speed over one move, and the lines each acceleration is for.
    tips["default_acceleration"].media = low_high("speed_accel");
    tips["outer_wall_acceleration"].media = picture("feature_outer_wall");
    tips["inner_wall_acceleration"].media = picture("feature_inner_wall");
    tips["bridge_acceleration"].media = picture("speed_bridges");
    tips["sparse_infill_acceleration"].media = picture("feature_sparse_infill");
    tips["internal_solid_infill_acceleration"].media = picture("feature_internal_solid_infill");
    tips["initial_layer_acceleration"].media = picture("feature_first_layer");
    tips["initial_layer_travel_acceleration"].media = picture("speed_first_layer_travel");
    tips["top_surface_acceleration"].media = picture("feature_top_surface");
    tips["travel_acceleration"].media = picture("feature_travel");
    tips["accel_to_decel_enable"].media = picture("speed_accel_to_decel_low");
    tips["accel_to_decel_factor"].media = low_high("speed_accel_to_decel");

    // Junction deviation and jerk: how much the nozzle slows down at a corner.
    tips["default_junction_deviation"].media = low_high("speed_jerk");
    tips["default_jerk"].media = low_high("speed_jerk");
    tips["outer_wall_jerk"].media = picture("feature_outer_wall");
    tips["inner_wall_jerk"].media = picture("feature_inner_wall");
    tips["infill_jerk"].media = picture("speed_infill");
    tips["top_surface_jerk"].media = picture("feature_top_surface");
    tips["initial_layer_jerk"].media = picture("feature_first_layer");
    tips["initial_layer_travel_jerk"].media = picture("speed_first_layer_travel");
    tips["travel_jerk"].media = picture("feature_travel");

    // Advanced: extrusion rate smoothing, the flow ramping from one feature's to the next one's.
    tips["max_volumetric_extrusion_rate_slope"].media = low_high("speed_ers");
    tips["max_volumetric_extrusion_rate_slope_segment_length"].media = low_high("speed_ers_segment");
    tips["extrusion_rate_smoothing_external_perimeter_only"].media = {
        {"settings/process/feature_outer_wall.png", _L("Outer wall")},
        {"settings/process/feature_overhang.png", _L("Overhang wall")}};
}

} // namespace Slic3r::GUI
