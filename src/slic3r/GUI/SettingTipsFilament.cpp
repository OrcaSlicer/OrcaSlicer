#include "SettingTips.hpp"

#include <string>
#include <vector>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IllustratedTipPopup.hpp"

namespace Slic3r::GUI {

// Pictures for the tooltips of the filament settings, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/filament. See SettingTips.hpp.
void add_filament_setting_tips(SettingTips &tips)
{
    const std::string dir = "settings/filament/";
    const auto one = [&dir](const char *file) { return std::vector<TipMedia>{{dir + file, {}}}; };
    // The pictures of one kind of line highlighted in a printed part, drawn for the Process settings.
    const auto feature = [](const char *name) { return TipMedia{std::string("settings/process/feature_") + name + ".png", {}}; };

    // Basic information.
    tips["filament_soluble"].media    = {feature("support_interface")};
    tips["filament_is_support"].media = {feature("support")};
    tips["filament_diameter"].media   = {{dir + "diameter_175.png", _L("1.75 mm")}, {dir + "diameter_285.png", _L("2.85 mm")}};
    for (const char *key : {"nozzle_temperature_range_low", "nozzle_temperature_range_high"})
        tips[key].media = one("temperature_range.png");
    tips["idle_temperature"].media       = one("idle_temperature.png");
    tips["filament_change_length"].media = {feature("prime_tower")};

    // Flow ratio and pressure advance.
    tips["filament_flow_ratio"].media = {{dir + "flow_ratio_low.png", _L("Too low")},
                                         {dir + "flow_ratio_right.png", _L("Calibrated")},
                                         {dir + "flow_ratio_high.png", _L("Too high")}};
    for (const char *key : {"pressure_advance", "enable_pressure_advance"})
        tips[key].media = {{dir + "pressure_advance_low.png", _L("Too low")},
                           {dir + "pressure_advance_right.png", _L("Calibrated")},
                           {dir + "pressure_advance_high.png", _L("Too high")}};
    // Pressure advance against the flow, measured at two accelerations, and the curves fitted to it.
    for (const char *key : {"adaptive_pressure_advance", "adaptive_pressure_advance_model", "adaptive_pressure_advance_overhangs"})
        tips[key].media = one("adaptive_pa.png");
    tips["adaptive_pressure_advance_bridges"].media = {feature("bridge")};

    // Temperatures: the chamber heating up to its target, printing from the minimal one; the first layer
    // and the layers above it, for the nozzle and every bed type.
    for (const char *key : {"activate_chamber_temp_control", "chamber_temperature", "chamber_minimal_temperature"})
        tips[key].media = one("chamber_heatup.png");
    for (const char *key : {"nozzle_temperature_initial_layer", "supertack_plate_temp_initial_layer", "cool_plate_temp_initial_layer",
                            "textured_cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer", "hot_plate_temp_initial_layer",
                            "textured_plate_temp_initial_layer"})
        tips[key].media = one("layers_first.png");
    for (const char *key : {"nozzle_temperature", "supertack_plate_temp", "cool_plate_temp", "textured_cool_plate_temp", "eng_plate_temp",
                            "hot_plate_temp", "textured_plate_temp"})
        tips[key].media = one("layers_other.png");

    // What one second of extrusion prints at the same volumetric speed.
    for (const char *key : {"filament_max_volumetric_speed", "filament_adaptive_volumetric_speed"})
        tips[key].media = {{dir + "volumetric_thin.png", _L("Thin line, printed faster")},
                           {dir + "volumetric_thick.png", _L("Thick line, printed slower")}};

    // The part cooling fan against the layer time (GCode/CoolingBuffer.cpp).
    for (const char *key : {"fan_min_speed", "fan_max_speed", "fan_cooling_layer_time", "slow_down_layer_time"})
        tips[key].media = one("fan_curve.png");
    // And the print speed: a quick layer is slowed down to take the layer time, but not below the minimum speed.
    tips["slow_down_layer_time"].media.push_back({dir + "slow_down.png", {}});
    tips["slow_down_for_layer_cooling"].media = one("slow_down.png");
    tips["slow_down_min_speed"].media         = one("slow_down_min.png");
    tips["dont_slow_down_outer_wall"].media   = {feature("outer_wall")};
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
    // The fan speeds of particular kinds of line.
    for (const char *key : {"enable_overhang_bridge_fan", "overhang_fan_speed"})
        tips[key].media = {feature("overhang"), feature("bridge")};
    tips["internal_bridge_fan_speed"].media            = {feature("internal_bridge")};
    tips["support_material_interface_fan_speed"].media = {feature("support_interface")};
    tips["ironing_fan_speed"].media                    = {feature("ironing")};

    tips["additional_cooling_fan_speed"].media = one("aux_fan.png");
    // The exhaust fan, during the print and once it has finished; the captions are the lines' labels.
    tips["activate_air_filtration"].media = {{dir + "exhaust_during.png", _L("During print")},
                                             {dir + "exhaust_after.png", _L("Complete print")}};
    for (const char *key : {"activate_air_filtration_during_print", "during_print_exhaust_fan_speed"})
        tips[key].media = one("exhaust_during.png");
    for (const char *key : {"activate_air_filtration_on_completion", "complete_print_exhaust_fan_speed"})
        tips[key].media = one("exhaust_after.png");

    // The wipe tower: the purge, ramming on it, and its interface layer, where one material is printed on another.
    for (const char *key : {"filament_minimal_purge_on_wipe_tower", "filament_multitool_ramming", "filament_multitool_ramming_volume",
                            "filament_multitool_ramming_flow"})
        tips[key].media = {feature("prime_tower")};
    for (const char *key : {"filament_tower_interface_pre_extrusion_dist", "filament_tower_interface_pre_extrusion_length",
                            "filament_tower_ironing_area", "filament_tower_interface_purge_volume", "filament_tower_interface_print_temp"})
        tips[key].media = one("tower_interface.png");
    // The filament pulled up out of the hot zone before its extruder is parked.
    for (const char *key : {"long_retractions_when_ec", "retraction_distances_when_ec"})
        tips[key].media = one("ec_long_retraction.gif");

    // The filament change on a single extruder multi-material printer (GCode/WipeTower2.cpp): ramming,
    // unloading, cooling moves, and stamping between the cooling moves.
    for (const char *key : {"filament_ramming_parameters", "filament_unloading_speed", "filament_unloading_speed_start",
                            "filament_cooling_moves", "filament_cooling_initial_speed", "filament_cooling_final_speed"})
        tips[key].media = one("toolchange_unload.gif");
    tips["filament_toolchange_delay"].media = one("toolchange_unload.gif");
    // Loading the next filament: slow at first, fast, then slow again into the nozzle (toolchange_Load()).
    for (const char *key : {"filament_loading_speed_start", "filament_loading_speed"})
        tips[key].media = one("toolchange_load.gif");
    for (const char *key : {"filament_stamping_distance", "filament_stamping_loading_speed"})
        tips[key].media = one("cooling_stamping.gif");

    tips["filament_shrink"].media                   = one("shrink_xy.gif");
    tips["filament_shrinkage_compensation_z"].media = one("shrink_z.gif");
}

} // namespace Slic3r::GUI
