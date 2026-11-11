#include "SettingTips.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

namespace {

// A name without an extension is a still, <name>.png; an animation is named with its .gif.
std::string picture_file(const std::string &name)
{
    return "settings/process/" + name + (name.find('.') == std::string::npos ? ".png" : "");
}

// Two values of a setting side by side, captioned: {picture, caption} under settings/process. A caption that
// is a number, an angle or a percentage is not translated; one in millimetres is, as some languages write
// the unit differently.
std::vector<TipMedia> compare(const std::string &a, const wxString &caption_a, const std::string &b, const wxString &caption_b)
{
    return {{picture_file(a), caption_a}, {picture_file(b), caption_b}};
}

std::vector<TipMedia> off_on(const std::string &off, const std::string &on) { return compare(off, _L("Off"), on, _L("On")); }

// One picture alone, with no caption.
std::vector<TipMedia> one(const std::string &name) { return {{picture_file(name), {}}}; }

// One picture per value, captioned with the value's label: {enum value, picture under settings/process}.
std::map<std::string, TipMedia> per_value(const std::vector<std::pair<std::string, std::string>> &files)
{
    std::map<std::string, TipMedia> values;
    for (const auto &[value, name] : files)
        values[value] = {picture_file(name), {}};
    return values;
}

} // namespace

// Pictures for the settings of the Process tab's Multimaterial and Others pages, drawn by
// scripts/gen_gizmo_tips.py (scripts/gizmo_tips/settings_process_others.py), most of them from G-code that
// OrcaSlicer sliced for each value. A setting that changes one kind of line shows the picture of that line,
// feature_<line>.png, shared with the other pages. See SettingTips.hpp.
//
// brim_type, draft_shield, fuzzy_skin and fuzzy_skin_noise_type have their pictures in SettingTipsProcess.cpp.
void add_process_others_setting_tips(SettingTips &tips)
{
    // Multimaterial: the prime tower.
    tips["enable_prime_tower"].media = off_on("prime_tower_off", "prime_tower_on");
    tips["prime_tower_skip_points"].media = off_on("tower_skip_0", "tower_skip_1");
    tips["prime_tower_width"].media = compare("tower_width_12", _L("12 mm"), "tower_width_30", _L("30 mm"));
    tips["prime_tower_brim_width"].media = compare("tower_brim_0", _L("0 mm"), "tower_brim_4", _L("4 mm"));
    // The two kinds of tower each have their own setting for the spacing of the purge lines.
    tips["prime_tower_infill_gap"].media   = compare("tower_spacing_100", wxString("100%"), "tower_spacing_200", wxString("200%"));
    tips["wipe_tower_extra_spacing"].media = tips["prime_tower_infill_gap"].media;
    tips["wipe_tower_rotation_angle"].media = compare("tower_rotation_0", wxString::FromUTF8("0°"), "tower_rotation_30", wxString::FromUTF8("30°"));
    tips["wipe_tower_bridging"].media = compare("tower_bridging_4", _L("4 mm"), "tower_bridging_16", _L("16 mm"));
    tips["wipe_tower_extra_flow"].media = one("feature_prime_tower");
    tips["wipe_tower_max_purge_speed"].media = one("feature_prime_tower");
    tips["wipe_tower_wall_type"].values = per_value({{"rectangle", "tower_wall_rectangle"},
                                                     {"cone", "tower_wall_cone"},
                                                     {"rib", "tower_wall_rib"}});
    tips["wipe_tower_cone_angle"].media = compare("tower_wall_cone", wxString::FromUTF8("30°"), "tower_cone_60", wxString::FromUTF8("60°"));
    tips["wipe_tower_extra_rib_length"].media = compare("tower_rib_length_0", _L("0 mm"), "tower_rib_length_12", _L("12 mm"));
    tips["wipe_tower_rib_width"].media = compare("tower_rib_width_4", _L("4 mm"), "tower_rib_width_14", _L("14 mm"));
    tips["wipe_tower_fillet_wall"].media = off_on("tower_fillet_0", "tower_fillet_1");
    tips["wipe_tower_no_sparse_layers"].media = off_on("tower_sparse_0", "tower_sparse_1");
    tips["wipe_tower_sparse_layers_combination"].media = off_on("tower_combine_0", "tower_combine_1");

    // Multimaterial: the filament each kind of line is printed with.
    tips["outer_wall_filament_id"].media     = one("feature_outer_wall");
    tips["inner_wall_filament_id"].media     = one("feature_inner_wall");
    tips["sparse_infill_filament_id"].media  = one("feature_sparse_infill");
    tips["internal_solid_filament_id"].media = one("feature_internal_solid_infill");
    tips["top_surface_filament_id"].media    = one("feature_top_surface");
    tips["bottom_surface_filament_id"].media = one("feature_bottom_surface");
    tips["wipe_tower_filament"].media        = one("feature_prime_tower");

    // Multimaterial: ooze prevention, the two filaments' temperatures through three filament changes.
    tips["ooze_prevention"].media = off_on("ooze_off", "ooze_on");
    tips["standby_temperature_delta"].media = compare("ooze_delta_small", _L("Small"), "ooze_on", _L("Large"));
    tips["preheat_time"].media = compare("ooze_preheat_short", _L("Short"), "ooze_preheat_long", _L("Long"));

    // Multimaterial: flush options.
    tips["flush_into_infill"].media = off_on("flush_none", "flush_infill");
    tips["flush_into_objects"].media = one("flush_objects");

    // Multimaterial: advanced.
    tips["interlocking_beam"].media = off_on("interlock_beam_0", "interlock_beam_1");
    tips["toolchange_ordering"].values = per_value({{"default", "toolchange_default.gif"}, {"cyclic", "toolchange_cyclic.gif"}});
    tips["toolchange_cyclic_order"].media = one("toolchange_cyclic.gif");
    tips["toolchange_cyclic_first_layer"].media = off_on("toolchange_first_layer_off.gif", "toolchange_cyclic.gif");
    tips["interface_shells"].media = off_on("interface_shells_0", "interface_shells_1");
    tips["interlocking_beam_width"].media = compare("interlock_width_0_5", _L("0.5 mm"), "interlock_width_1_6", _L("1.6 mm"));
    tips["interlocking_orientation"].media = compare("interlock_orient_0", wxString::FromUTF8("0°"), "interlock_orient_45", wxString::FromUTF8("45°"));
    tips["interlocking_beam_layer_count"].media = compare("interlock_layers_1", wxString("1"), "interlock_layers_4", wxString("4"));
    tips["interlocking_depth"].media = compare("interlock_depth_1", wxString("1"), "interlock_depth_4", wxString("4"));
    tips["interlocking_boundary_avoidance"].media = compare("interlock_avoid_0", wxString("0"), "interlock_avoid_3", wxString("3"));

    // Others: skirt.
    tips["skirt_loops"].media = compare("skirt_loops_1", wxString("1"), "skirt_loops_3", wxString("3"));
    tips["skirt_type"].values = per_value({{"combined", "skirt_type_combined"}, {"perobject", "skirt_type_perobject"}});
    tips["min_skirt_length"].media = compare("skirt_min_length_0", _L("0 mm"), "skirt_min_length_6", _L("6 mm"));
    tips["skirt_distance"].media = compare("skirt_distance_1", _L("1 mm"), "skirt_distance_8", _L("8 mm"));
    tips["skirt_start_angle"].media = compare("skirt_start_m135", wxString::FromUTF8("-135°"), "skirt_start_45", wxString::FromUTF8("45°"));
    tips["skirt_speed"].media = one("feature_skirt");
    tips["skirt_height"].media = compare("skirt_height_1", wxString("1"), "skirt_height_5", wxString("5"));
    tips["single_loop_draft_shield"].media = off_on("skirt_single_loop_0", "skirt_single_loop_1");

    // Others: brim.
    tips["brim_width"].media = compare("brim_width_2", _L("2 mm"), "brim_width_8", _L("8 mm"));
    tips["brim_object_gap"].media = compare("brim_gap_0", _L("0 mm"), "brim_gap_1", _L("1 mm"));
    tips["brim_flow_ratio"].media = one("feature_brim");
    tips["brim_use_efc_outline"].media = off_on("brim_efc_0", "brim_efc_1");
    tips["combine_brims"].media = off_on("combine_brims_0", "combine_brims_1");
    tips["brim_ears_max_angle"].media = compare("brim_ears_angle_60", wxString::FromUTF8("60°"), "brim_ears_angle_130", wxString::FromUTF8("130°"));

    // Others: special modes. Best of and Snake take the same way as Default on the pictures' small layout,
    // so they have no picture.
    tips["slicing_mode"].values = per_value({{"regular", "slicing_mode_regular"},
                                             {"even_odd", "slicing_mode_even_odd"},
                                             {"close_holes", "slicing_mode_close_holes"}});
    tips["print_sequence"].values = per_value({{"by layer", "print_sequence_by_layer.gif"}, {"by object", "print_sequence_by_object.gif"}});
    tips["print_order"].values = per_value({{"default", "print_order_default.gif"}, {"as_obj_list", "print_order_as_obj_list.gif"}});
    tips["spiral_mode"].media = off_on("spiral_0", "spiral_1");
    tips["spiral_starting_flow_ratio"].media = compare("spiral_start_0", wxString("0"), "spiral_start_half", wxString("0.5"));
    tips["spiral_finishing_flow_ratio"].media = compare("spiral_finish_0", wxString("0"), "spiral_finish_half", wxString("0.5"));

    // Others: fuzzy skin.
    tips["fuzzy_skin_mode"].values = per_value({{"displacement", "fuzzy_mode_displacement"},
                                                {"extrusion", "fuzzy_mode_extrusion"},
                                                {"combined", "fuzzy_mode_combined"}});
    tips["fuzzy_skin_point_distance"].media = compare("fuzzy_point_distance_0_2", _L("0.2 mm"), "fuzzy_point_distance_1_2", _L("1.2 mm"));
    tips["fuzzy_skin_thickness"].media = compare("fuzzy_thickness_0_15", _L("0.15 mm"), "fuzzy_thickness_0_6", _L("0.6 mm"));
    tips["fuzzy_skin_scale"].media = compare("fuzzy_scale_0_4", _L("0.4 mm"), "fuzzy_scale_3", _L("3 mm"));
    tips["fuzzy_skin_octaves"].media = compare("fuzzy_octaves_1", wxString("1"), "fuzzy_octaves_8", wxString("8"));
    tips["fuzzy_skin_persistence"].media = compare("fuzzy_persistence_0_15", wxString("0.15"), "fuzzy_persistence_0_9", wxString("0.9"));
    tips["fuzzy_skin_ripples_per_layer"].media = compare("fuzzy_ripples_6", wxString("6"), "fuzzy_ripples_30", wxString("30"));
    tips["fuzzy_skin_ripple_offset"].media = compare("fuzzy_ripple_offset_0", wxString("0%"), "fuzzy_ripple_offset_50", wxString("50%"));
    tips["fuzzy_skin_layers_between_ripple_offset"].media = compare("fuzzy_ripple_layers_1", wxString("1"), "fuzzy_ripple_layers_5", wxString("5"));
    tips["fuzzy_skin_first_layer"].media = off_on("fuzzy_first_layer_0", "fuzzy_first_layer_1");

    // The settings below are drawn by scripts/gizmo_tips/settings_process_extra.py. The belt printers' and the
    // painted colors' settings, and the temperatures at the tower, are diagrams; the rest is sliced.
    tips["prime_volume"].media = compare("prime_volume_15", wxString::FromUTF8("15 ") + _L("mm³"), "prime_volume_90",
                                         wxString::FromUTF8("90 ") + _L("mm³"));
    tips["single_extruder_multi_material_priming"].media = off_on("multi_material_priming_0", "multi_material_priming_1");
    tips["enable_tower_interface_features"].media = off_on("tower_interface_off", "tower_interface_on");
    tips["enable_tower_interface_cooldown_during_tower"].media = off_on("tower_interface_on", "tower_interface_cooldown");
    tips["belt_purge_tower_width"].media = compare("belt_purge_tower_narrow", _L("Narrow"), "belt_purge_tower_wide", _L("Wide"));
    tips["mmu_segmented_region_max_width"].media = compare("mmu_region_width_0", wxString("0"), "mmu_region_width_3", _L("3 mm"));
    tips["mmu_segmented_region_interlocking_depth"].media = off_on("mmu_region_interlock_0", "mmu_region_interlock_on");
    tips["brim_ears_detection_length"].media = compare("brim_ears_detection_0", wxString("0"), "brim_ears_detection_1_5", _L("1.5 mm"));
    tips["leading_brim_length"].media = compare("belt_brim_plain", _L("0 mm"), "belt_brim_leading", _L("8 mm"));
    tips["extra_brim_width"].media = compare("belt_brim_plain", _L("0 mm"), "belt_brim_extra", _L("4 mm"));
    tips["exclude_object"].media = one("exclude_object_tolerance_0");
    tips["exclude_object_tolerance"].media = compare("exclude_object_tolerance_0", _L("0 mm"), "exclude_object_tolerance_1", _L("1 mm"));
    tips["reduce_infill_retraction"].media = off_on("reduce_infill_retraction_0", "reduce_infill_retraction_1");
    tips["timelapse_type"].values = per_value({{"0", "timelapse_traditional.gif"}, {"1", "timelapse_smooth.gif"}});
}

} // namespace Slic3r::GUI
