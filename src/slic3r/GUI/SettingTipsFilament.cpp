#include "SettingTips.hpp"

#include <string>
#include <vector>

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

// Pictures for the tooltips of the filament settings, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/filament. See SettingTips.hpp.
void add_filament_setting_tips(SettingTips &tips)
{
    const std::string dir = "settings/filament/";
    const auto one = [&dir](const char *file) { return std::vector<TipMedia>{{dir + file, {}}}; };

    // The part cooling fan against the layer time (GCode/CoolingBuffer.cpp).
    for (const char *key : {"fan_min_speed", "fan_max_speed", "fan_cooling_layer_time", "slow_down_layer_time"})
        tips[key].media = one("fan_curve.png");
    tips["reduce_fan_stop_start_freq"].media = {{dir + "fan_curve.png", _L("Off")}, {dir + "fan_curve_always_on.png", _L("On")}};
    // The fan over the first layers.
    tips["close_fan_the_first_x_layers"].media = one("fan_ramp.png");
    tips["full_fan_speed_layer"].media         = one("fan_ramp.png");
    tips["initial_layer_fan_speed"].media      = one("fan_first_layer.png");

    // Keyed by the option's enum_values.
    tips["overhang_fan_threshold"].values = {{"0%", {dir + "overhang_fan_threshold_0.png", {}}},
                                             {"10%", {dir + "overhang_fan_threshold_10.png", {}}},
                                             {"25%", {dir + "overhang_fan_threshold_25.png", {}}},
                                             {"50%", {dir + "overhang_fan_threshold_50.png", {}}},
                                             {"75%", {dir + "overhang_fan_threshold_75.png", {}}},
                                             {"95%", {dir + "overhang_fan_threshold_95.png", {}}}};

    // The filament change on a single extruder multi-material printer (GCode/WipeTower2.cpp): ramming,
    // unloading, cooling moves, and stamping between the cooling moves.
    for (const char *key : {"filament_ramming_parameters", "filament_unloading_speed", "filament_unloading_speed_start",
                            "filament_cooling_moves", "filament_cooling_initial_speed", "filament_cooling_final_speed"})
        tips[key].media = one("toolchange_unload.gif");
    for (const char *key : {"filament_stamping_distance", "filament_stamping_loading_speed"})
        tips[key].media = one("cooling_stamping.gif");

    tips["filament_shrink"].media                   = one("shrink_xy.gif");
    tips["filament_shrinkage_compensation_z"].media = one("shrink_z.gif");

    tips["pressure_advance"].media = {{dir + "pressure_advance_low.png", _L("Too low")},
                                      {dir + "pressure_advance_right.png", _L("Calibrated")},
                                      {dir + "pressure_advance_high.png", _L("Too high")}};
}

} // namespace Slic3r::GUI
