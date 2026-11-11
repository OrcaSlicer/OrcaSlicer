#include "SettingTips.hpp"

#include <initializer_list>
#include <string>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

namespace {

std::string file(const std::string &name) { return "settings/process/" + name; }

// One picture, with no caption: what the setting acts on.
std::vector<TipMedia> one(const std::string &name) { return {{file(name), {}}}; }

// The picture of one kind of line, highlighted in the same part (shared by the Process pages).
std::vector<TipMedia> feature(const std::string &name) { return one("feature_" + name + ".png"); }

// Two values side by side.
std::vector<TipMedia> two(const std::string &a, const wxString &caption_a, const std::string &b, const wxString &caption_b)
{
    return {{file(a), caption_a}, {file(b), caption_b}};
}

std::vector<TipMedia> off_on(const std::string &off, const std::string &on) { return two(off, _L("Off"), on, _L("On")); }

std::vector<TipMedia> low_high(const std::string &low, const std::string &high)
{
    return two(low, _L("Low"), high, _L("High"));
}

} // namespace

// Pictures for the settings of the Process tab's Quality page, drawn by scripts/gen_gizmo_tips.py
// (scripts/gizmo_tips/settings_process_quality.py) from G-code that OrcaSlicer sliced for each value. The
// settings that already had pictures (seams, wall order, ironing type) keep those of SettingTipsProcess.cpp.
// See SettingTips.hpp.
void add_process_quality_setting_tips(SettingTips &tips)
{
    // Layer height.
    tips["layer_height"].media = two("layer_height_low.png", _L("0.1 mm"), "layer_height_high.png", _L("0.3 mm"));
    tips["initial_layer_print_height"].media = low_high("first_layer_height_low.png", "first_layer_height_high.png");

    // Line width: each width shows the lines it sets.
    tips["line_width"].media = two("line_width_narrow.png", _L("Narrow"), "line_width_wide.png", _L("Wide"));
    tips["initial_layer_line_width"].media         = feature("first_layer");
    tips["outer_wall_line_width"].media            = feature("outer_wall");
    tips["inner_wall_line_width"].media            = feature("inner_wall");
    tips["top_surface_line_width"].media           = feature("top_surface");
    tips["sparse_infill_line_width"].media         = feature("sparse_infill");
    tips["internal_solid_infill_line_width"].media = feature("internal_solid_infill");
    tips["support_line_width"].media               = feature("support");
    tips["bridge_line_width"].media                = feature("bridge");

    // Seam.
    tips["staggered_inner_seams"].media = off_on("staggered_inner_seams_off.png", "staggered_inner_seams_on.png");
    tips["seam_gap"].media              = low_high("seam_gap_low.png", "seam_gap_high.png");
    tips["seam_slope_type"].values      = {{"none", {file("scarf_none.png"), {}}},
                                           {"external", {file("scarf_external.png"), {}}},
                                           {"all", {file("scarf_all.png"), {}}}};
    // The scarf in section, for the settings that tune it.
    for (const char *key : {"seam_slope_conditional", "scarf_angle_threshold", "scarf_overhang_threshold", "scarf_joint_speed",
                            "scarf_joint_flow_ratio", "seam_slope_inner_walls"})
        tips[key].media = one("scarf_section.png");
    tips["seam_slope_start_height"].media = low_high("scarf_section.png", "scarf_start_high.png");
    tips["seam_slope_entire_loop"].media  = off_on("scarf_section.png", "scarf_entire_loop.png");
    // TRN Captions of two pictures of a scarf joint seam: a short ramp and a long one.
    tips["seam_slope_min_length"].media = two("scarf_length_short.png", _L("Short"), "scarf_length_long.png", _L("Long"));
    tips["seam_slope_steps"].media      = low_high("scarf_steps_few.png", "scarf_section.png");
    // The wipe at the end of the outer wall, in ACCENT.
    tips["role_based_wipe_speed"].media     = one("wipe_on_loops_off.png");
    tips["wipe_speed"].media                = one("wipe_on_loops_off.png");
    tips["wipe_on_loops"].media             = off_on("wipe_on_loops_off.png", "wipe_on_loops_on.png");
    tips["wipe_inward"].media               = off_on("wipe_inward_off.png", "wipe_inward_on.png");
    tips["wipe_inward_distance"].media      = one("wipe_inward_on.png");
    tips["wipe_before_external_loop"].media = off_on("wipe_before_external_off.png", "wipe_before_external_on.png");

    // Precision.
    tips["resolution"].media         = low_high("resolution_fine.png", "resolution_coarse.png");
    tips["enable_arc_fitting"].values = {{"disabled", {file("arc_fitting_disabled.png"), {}}},
                                         {"enabled", {file("arc_fitting_enabled.png"), {}}},
                                         {"sparse_infill", {file("arc_fitting_sparse_infill.png"), {}}}};
    // TRN Captions of two pictures of a hole or an outline moved by a negative and by a positive value.
    tips["xy_hole_compensation"].media    = two("xy_hole_negative.png", _L("Negative"), "xy_hole_positive.png", _L("Positive"));
    tips["xy_contour_compensation"].media = two("xy_contour_negative.png", _L("Negative"), "xy_contour_positive.png",
                                                 _L("Positive"));
    tips["elefant_foot_compensation"].media        = off_on("elephant_foot_off.png", "elephant_foot_on.png");
    tips["elefant_foot_compensation_layers"].media = low_high("elephant_foot_on.png", "elephant_foot_layers.png");
    tips["hole_to_polyhole"].media                 = off_on("polyhole_off.png", "polyhole_on.png");
    tips["hole_to_polyhole_threshold"].media       = one("polyhole_on.png");
    tips["hole_to_polyhole_max_edges"].media       = one("polyhole_on.png");
    tips["hole_to_polyhole_twisted"].media         = off_on("polyhole_on.png", "polyhole_twisted.png");

    // Ironing.
    tips["ironing_flow"].media    = feature("ironing");
    tips["ironing_spacing"].media = low_high("ironing_spacing_low.png", "ironing_spacing_high.png");
    tips["ironing_inset"].media   = low_high("ironing_inset_low.png", "ironing_inset_high.png");
    // TRN Captions of two pictures of ironing lines running along the top surface's lines and across them.
    tips["ironing_angle"].media = two("ironing_angle_0.png", _L("Along"), "ironing_angle_90.png", _L("Across"));

    // Z contouring.
    tips["zaa_enabled"].media                   = off_on("zaa_off.png", "zaa_on.png");
    tips["zaa_minimize_perimeter_height"].media = one("zaa_on.png");
    tips["zaa_min_z"].media                     = one("zaa_on.png");

    // Wall generator: Arachne's walls on a narrowing wedge, for the settings that tune them.
    for (const char *key : {"wall_transition_angle", "wall_transition_filter_deviation", "wall_transition_length",
                            "wall_distribution_count", "initial_layer_min_bead_width", "min_bead_width", "min_feature_size"})
        tips[key].media = one("wall_generator_arachne.png");
    tips["wall_maximum_resolution"].media = low_high("resolution_fine.png", "resolution_coarse.png");

    // Walls and surfaces.
    tips["is_infill_first"].media  = off_on("infill_first_off.gif", "infill_first_on.gif");
    tips["print_flow_ratio"].media = low_high("flow_low.png", "flow_high.png");
    tips["top_solid_infill_flow_ratio"].media      = feature("top_surface");
    tips["bottom_solid_infill_flow_ratio"].media   = feature("bottom_surface");
    tips["first_layer_flow_ratio"].media           = feature("first_layer");
    tips["outer_wall_flow_ratio"].media            = feature("outer_wall");
    tips["inner_wall_flow_ratio"].media            = feature("inner_wall");
    tips["overhang_flow_ratio"].media              = feature("overhang");
    tips["sparse_infill_flow_ratio"].media         = feature("sparse_infill");
    tips["internal_solid_infill_flow_ratio"].media = feature("internal_solid_infill");
    tips["gap_fill_flow_ratio"].media              = feature("gap_fill");
    tips["support_flow_ratio"].media               = feature("support");
    tips["support_interface_flow_ratio"].media     = feature("support_interface");
    tips["only_one_wall_first_layer"].media = off_on("one_wall_first_layer_off.png", "one_wall_first_layer_on.png");
    tips["only_one_wall_top"].media         = off_on("one_wall_top_off.png", "one_wall_top_on.png");
    tips["reduce_crossing_wall"].media      = off_on("avoid_crossing_walls_off.png", "avoid_crossing_walls_on.png");
    tips["max_travel_detour_distance"].media = one("avoid_crossing_walls_on.png");
    tips["small_area_infill_flow_compensation"].media       = one("small_area_flow_model.png");
    tips["small_area_infill_flow_compensation_model"].media = one("small_area_flow_model.png");

    // Bridging.
    tips["bridge_flow"].media             = feature("bridge");
    tips["internal_bridge_flow"].media    = feature("internal_bridge");
    tips["bridge_density"].media          = low_high("bridge_density_low.png", "bridge_density_high.png");
    tips["internal_bridge_density"].media = low_high("internal_bridge_density_low.png", "internal_bridge_density_high.png");
    tips["thick_internal_bridges"].media  = off_on("thick_internal_bridges_off.png", "thick_internal_bridges_on.png");
    // The layer over an external bridge. "Internal bridge only" leaves that layer as it is, so it has no picture.
    tips["enable_extra_bridge_layer"].values = {{"disabled", {file("extra_bridge_layer_off.png"), {}}},
                                                {"external_bridge_only", {file("extra_bridge_layer_on.png"), {}}},
                                                {"apply_to_all", {file("extra_bridge_layer_on.png"), {}}}};

    // Overhangs.
    tips["detect_overhang_wall"].media          = feature("overhang");
    tips["make_overhang_printable_angle"].media = low_high("overhang_printable_angle_low.png", "overhang_printable_angle_high.png");

    // The settings below are drawn by scripts/gizmo_tips/settings_process_extra.py.
    tips["enable_mixed_color_sublayer"].media = off_on("mixed_color_sublayer_off.png", "mixed_color_sublayer_on.png");
    tips["min_width_top_surface"].media = two("one_wall_threshold_0.png", wxString::FromUTF8("0 ") + _L("mm"),
                                              "one_wall_threshold_5.png", wxString::FromUTF8("5 ") + _L("mm"));
    tips["precise_outer_wall"].media    = off_on("precise_outer_wall_0.png", "precise_outer_wall_1.png");
    tips["precise_z_height"].media      = off_on("precise_z_height_0.png", "precise_z_height_1.png");
    tips["slice_closing_radius"].media  = two("slice_closing_radius_0_049.png", wxString::FromUTF8("0.049 ") + _L("mm"),
                                              "slice_closing_radius_0_2.png", wxString::FromUTF8("0.2 ") + _L("mm"));
    tips["elefant_foot_layers_density"].media = two("ef_density_100.png", wxString::FromUTF8("100%"), "ef_density_50.png",
                                                    wxString::FromUTF8("50%"));
    tips["ironing_angle_fixed"].media   = off_on("ironing_angle_fixed_0.png", "ironing_angle_fixed_1.png");
    tips["wall_maximum_deviation"].media = low_high("resolution_fine.png", "resolution_coarse.png");
    tips["zaa_dont_alternate_fill_direction"].media = off_on("zaa_dont_alternate_0.png", "zaa_dont_alternate_1.png");
    tips["dont_filter_internal_bridges"].values = {{"disabled", {file("internal_bridge_filter_disabled.png"), {}}},
                                                   {"limited", {file("internal_bridge_filter_limited.png"), {}}},
                                                   {"nofilter", {file("internal_bridge_filter_nofilter.png"), {}}}};
    tips["make_overhang_printable_hole_size"].media = two("overhang_hole_size_0.png", wxString::FromUTF8("0 ") + _L("mm²"),
                                                          "overhang_hole_size_50.png", wxString::FromUTF8("50 ") + _L("mm²"));
    // Overhang reversal: the loops printed the other way round on even layers, on a cap that flares out.
    tips["overhang_reverse"].media               = off_on("overhang_reverse_off.png", "overhang_reverse_on.png");
    tips["overhang_reverse_internal_only"].media = off_on("overhang_reverse_on.png", "overhang_reverse_internal.png");
    tips["overhang_reverse_threshold"].media     = two("overhang_reverse_thr0.png", wxString::FromUTF8("0"),
                                                       "overhang_reverse_on.png", wxString::FromUTF8("50%"));
    tips["unsupported_wall_last"].media = off_on("unsupported_wall_last_0.gif", "unsupported_wall_last_1.gif");
}

} // namespace Slic3r::GUI
