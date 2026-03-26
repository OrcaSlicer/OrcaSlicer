#include "ProcessSettingsMerger.hpp"

#include <algorithm>

namespace Slic3r {

namespace {

const std::vector<std::string>& safe_option_keys()
{
    static const std::vector<std::string> keys = {
        "layer_height", "initial_layer_print_height",
        "line_width", "initial_layer_line_width", "inner_wall_line_width", "outer_wall_line_width", "sparse_infill_line_width",
        "internal_solid_infill_line_width", "skin_infill_line_width", "skeleton_infill_line_width", "top_surface_line_width", "support_line_width",
        "wall_loops", "alternate_extra_wall", "top_shell_layers", "top_shell_thickness", "top_surface_density",
        "bottom_surface_density", "bottom_shell_layers", "bottom_shell_thickness", "extra_perimeters_on_overhangs",
        "ensure_vertical_shell_thickness", "detect_thin_wall", "detect_overhang_wall", "wall_direction", "wall_sequence",
        "sparse_infill_density", "sparse_infill_pattern", "fill_multiline", "top_surface_pattern", "bottom_surface_pattern",
        "infill_direction", "solid_infill_direction", "extra_solid_infills", "minimum_sparse_infill_area", "internal_solid_infill_pattern",
        "gap_fill_target", "infill_wall_overlap", "top_bottom_infill_wall_overlap", "skin_infill_density", "skin_infill_depth",
        "skeleton_infill_density", "infill_overhang_angle",
        "enable_support", "support_type", "support_threshold_angle", "support_threshold_overlap", "enforce_support_layers",
        "support_base_pattern", "support_base_pattern_spacing", "support_expansion", "support_style", "independent_support_layer_height",
        "support_angle", "support_interface_top_layers", "support_interface_bottom_layers", "support_interface_pattern",
        "support_interface_spacing", "support_interface_loop_pattern", "support_top_z_distance", "support_bottom_z_distance",
        "support_on_build_plate_only", "support_critical_regions_only", "support_remove_small_overhang", "bridge_no_support",
        "tree_support_branch_angle", "tree_support_angle_slow", "tree_support_wall_count", "tree_support_top_rate",
        "tree_support_branch_distance", "tree_support_tip_diameter", "tree_support_branch_diameter", "tree_support_branch_diameter_angle",
        "skirt_type", "skirt_loops", "min_skirt_length", "skirt_distance", "skirt_height", "draft_shield", "single_loop_draft_shield",
        "brim_width", "brim_object_gap", "brim_use_efc_outline", "brim_type", "brim_ears_max_angle", "brim_ears_detection_length",
        "raft_layers", "raft_first_layer_density", "raft_first_layer_expansion", "raft_contact_distance", "raft_expansion"
    };
    return keys;
}

const std::vector<std::string>& conditional_option_keys()
{
    static const std::vector<std::string> keys = {
        "retract_before_wipe", "retraction_length", "retract_length_toolchange", "retract_lift_above", "retract_lift_below",
        "retract_lift_enforce", "retract_restart_extra", "retract_restart_extra_toolchange", "wipe", "wipe_distance", "wipe_speed",
        "wipe_on_loops", "wipe_before_external_loop", "role_based_wipe_speed",
        "enable_overhang_bridge_fan", "slow_down_for_layer_cooling", "dont_slow_down_outer_wall", "fan_max_speed", "fan_min_speed",
        "slow_down_min_speed", "slow_down_layer_time", "slow_down_layers", "internal_bridge_fan_speed",
        "bridge_flow", "internal_bridge_flow", "bridge_speed", "internal_bridge_speed", "bridge_angle", "internal_bridge_angle",
        "thick_bridges", "thick_internal_bridges", "dont_filter_internal_bridges", "enable_extra_bridge_layer", "max_bridge_length"
    };
    return keys;
}

const std::vector<std::string>& hardware_specific_option_keys()
{
    static const std::vector<std::string> keys = {
        "inner_wall_speed", "outer_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed",
        "support_speed", "support_interface_speed", "gap_infill_speed", "travel_speed", "travel_speed_z", "initial_layer_speed",
        "initial_layer_infill_speed", "small_perimeter_speed", "small_perimeter_threshold",
        "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed",
        "outer_wall_acceleration", "initial_layer_acceleration", "top_surface_acceleration", "default_acceleration",
        "travel_acceleration", "inner_wall_acceleration",
        "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "infill_jerk", "top_surface_jerk", "initial_layer_jerk",
        "travel_jerk", "default_junction_deviation",
        "print_flow_ratio", "max_volumetric_extrusion_rate_slope", "max_volumetric_extrusion_rate_slope_segment_length",
        "extrusion_rate_smoothing_external_perimeter_only",
        "hot_plate_temp", "hot_plate_temp_initial_layer", "nozzle_temperature_initial_layer", "nozzle_temperature",
        "standby_temperature_delta", "preheat_time", "preheat_steps", "idle_temperature", "chamber_temperature",
        "bed_temperature_formula", "bbl_bed_temperature_gcode",
        "enable_pressure_advance", "pressure_advance", "adaptive_pressure_advance", "adaptive_pressure_advance_overhangs",
        "adaptive_pressure_advance_model", "adaptive_pressure_advance_bridges"
    };
    return keys;
}

std::vector<std::string> make_unique(std::vector<std::string> keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

} // namespace

ProcessSettingsMerger::Categories ProcessSettingsMerger::all_categories()
{
    return { ProcessSettingsCategory::Safe, ProcessSettingsCategory::Conditional, ProcessSettingsCategory::HardwareSpecific };
}

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::option_keys_for_categories(const Categories &categories)
{
    std::vector<std::string> keys;
    for (const ProcessSettingsCategory category : categories) {
        switch (category) {
        case ProcessSettingsCategory::Safe:
            keys.insert(keys.end(), safe_option_keys().begin(), safe_option_keys().end());
            break;
        case ProcessSettingsCategory::Conditional:
            keys.insert(keys.end(), conditional_option_keys().begin(), conditional_option_keys().end());
            break;
        case ProcessSettingsCategory::HardwareSpecific:
            keys.insert(keys.end(), hardware_specific_option_keys().begin(), hardware_specific_option_keys().end());
            break;
        }
    }

    return make_unique(std::move(keys));
}

DynamicPrintConfig ProcessSettingsMerger::merge_settings(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const Categories &categories)
{
    return merge_settings(old_settings, new_defaults, option_keys_for_categories(categories));
}

DynamicPrintConfig ProcessSettingsMerger::merge_settings(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const OptionKeys &option_keys)
{
    DynamicPrintConfig merged = new_defaults;
    const std::vector<std::string> keys = make_unique(option_keys);
    if (!keys.empty())
        merged.apply_only(old_settings, keys, true);
    return merged;
}

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::diff_keys(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const Categories &categories)
{
    return diff_keys(old_settings, new_defaults, option_keys_for_categories(categories));
}

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::diff_keys(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const OptionKeys &option_keys)
{
    DynamicPrintConfig merged = merge_settings(old_settings, new_defaults, option_keys);
    return merged.diff(new_defaults);
}

bool ProcessSettingsMerger::has_transferable_settings(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const Categories &categories)
{
    return !diff_keys(old_settings, new_defaults, categories).empty();
}

} // namespace Slic3r