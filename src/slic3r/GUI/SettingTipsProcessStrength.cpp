#include "SettingTips.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

namespace {

const std::string DIR = "settings/process/";

// A caption that is a value as typed, a number with its own unit sign: "25%", "30°", "8#2".
wxString value(const char* text) { return wxString::FromUTF8(text); }

// A length: the number and the translated unit.
wxString mm(const char* number) { return wxString::FromUTF8(number) + " " + _L("mm"); }

// Pictures side by side, each with its caption: {file under settings/process, caption}.
std::vector<TipMedia> side_by_side(const std::vector<std::pair<std::string, wxString>>& pictures)
{
    std::vector<TipMedia> media;
    for (const auto& [file, caption] : pictures)
        media.push_back({DIR + file, caption});
    return media;
}

// The same setting off and on.
std::vector<TipMedia> off_on(const std::string& name)
{ return side_by_side({{name + "_off.png", _L("Off")}, {name + "_on.png", _L("On")}}); }

// One picture per value, captioned with the value's label.
std::map<std::string, TipMedia> per_value(const std::vector<std::pair<std::string, std::string>>& files)
{
    std::map<std::string, TipMedia> values;
    for (const auto& [key, file] : files)
        values[key] = {DIR + file, {}};
    return values;
}

} // namespace

// Pictures for the settings of the Process tab's Strength page, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/process (scripts/gizmo_tips/settings_process_strength.py) from G-code that
// OrcaSlicer sliced for each value. The patterns' own pictures are in SettingTipsProcess.cpp. See
// SettingTips.hpp.
void add_process_strength_setting_tips(SettingTips& tips)
{
    // Walls.
    tips["wall_loops"].media = side_by_side(
        {{"wall_loops_1.png", value("1")}, {"wall_loops_3.png", value("3")}, {"wall_loops_5.png", value("5")}});
    tips["alternate_extra_wall"].media = off_on("alternate_extra_wall");
    tips["detect_thin_wall"].media     = off_on("detect_thin_wall");

    // Top and bottom shells, cut through. At the default 0.2 mm layer height 2 and 5 layers are 0.4 and 1 mm.
    tips["top_shell_layers"].media             = side_by_side({{"top_shell_2.png", value("2")}, {"top_shell_5.png", value("5")}});
    tips["top_shell_thickness"].media          = side_by_side({{"top_shell_2.png", mm("0.4")}, {"top_shell_5.png", mm("1")}});
    tips["bottom_shell_layers"].media          = side_by_side({{"bottom_shell_2.png", value("2")}, {"bottom_shell_5.png", value("5")}});
    tips["bottom_shell_thickness"].media       = side_by_side({{"bottom_shell_2.png", mm("0.4")}, {"bottom_shell_5.png", mm("1")}});
    tips["top_color_penetration_layers"].media = side_by_side(
        {{"top_paint_penetration_2.png", value("2")}, {"top_paint_penetration_8.png", value("8")}});
    tips["bottom_color_penetration_layers"].media = side_by_side(
        {{"bottom_paint_penetration_2.png", value("2")}, {"bottom_paint_penetration_8.png", value("8")}});
    tips["top_surface_density"].media    = side_by_side({{"top_surface_density_100.png", value("100%")},
                                                         {"top_surface_density_50.png", value("50%")},
                                                         {"top_surface_density_25.png", value("25%")}});
    tips["bottom_surface_density"].media = side_by_side({{"bottom_surface_density_100.png", value("100%")},
                                                         {"bottom_surface_density_50.png", value("50%")},
                                                         {"bottom_surface_density_25.png", value("25%")}});
    // Concentric top surfaces being printed; the bottom ones are filled the same way.
    const auto fill_order                    = per_value({{"default", "surface_fill_order_default.gif"},
                                                          {"outward", "surface_fill_order_outward.gif"},
                                                          {"inward", "surface_fill_order_inward.gif"}});
    tips["top_surface_fill_order"].values    = fill_order;
    tips["bottom_surface_fill_order"].values = fill_order;
    tips["top_layer_direction"].media        = side_by_side(
        {{"top_layer_direction_0.png", value("0°")}, {"top_layer_direction_30.png", value("30°")}});
    tips["bottom_layer_direction"].media = side_by_side(
        {{"bottom_layer_direction_0.png", value("0°")}, {"bottom_layer_direction_30.png", value("30°")}});
    // A plate with a peg standing on it and a rib across it, outlined in white.
    tips["top_surface_expansion"].media = side_by_side(
        {{"top_surface_expansion_0.png", mm("0")}, {"top_surface_expansion_inward_and_outward.png", mm("2")}});
    tips["top_surface_expansion_margin"].media = side_by_side(
        {{"top_surface_expansion_inward_and_outward.png", mm("0")}, {"top_surface_expansion_margin.png", mm("1.5")}});
    tips["top_surface_expansion_direction"].values = per_value({{"inward_and_outward", "top_surface_expansion_inward_and_outward.png"},
                                                                {"inward", "top_surface_expansion_inward.png"},
                                                                {"outward", "top_surface_expansion_outward.png"}});
    tips["center_of_surface_pattern"].values       = per_value({{"each_surface", "center_of_surface_pattern_each_surface.png"},
                                                                {"each_model", "center_of_surface_pattern_each_model.png"},
                                                                {"each_assembly", "center_of_surface_pattern_each_assembly.png"}});
    tips["top_bottom_infill_wall_overlap"].media   = side_by_side(
        {{"top_bottom_infill_wall_overlap_0.png", value("0%")}, {"top_bottom_infill_wall_overlap_50.png", value("50%")}});

    // Sparse infill.
    tips["sparse_infill_density"].media = side_by_side({{"sparse_infill_density_10.png", value("10%")},
                                                        {"sparse_infill_density_25.png", value("25%")},
                                                        {"sparse_infill_density_50.png", value("50%")}});
    tips["fill_multiline"].media        = side_by_side(
        {{"fill_multiline_1.png", value("1")}, {"fill_multiline_2.png", value("2")}, {"fill_multiline_3.png", value("3")}});
    tips["adaptive_infill_mode"].values            = per_value({{"disabled", "adaptive_infill_mode_disabled.png"},
                                                                {"distance_warp", "adaptive_infill_mode_distance_warp.png"},
                                                                {"smooth_blend", "adaptive_infill_mode_smooth_blend.png"},
                                                                {"stepped_shells", "adaptive_infill_mode_stepped_shells.png"},
                                                                {"lobes", "adaptive_infill_mode_lobes.png"},
                                                                {"normal_z", "adaptive_infill_mode_normal_z.png"},
                                                                {"normal_y", "adaptive_infill_mode_normal_y.png"},
                                                                {"normal_x", "adaptive_infill_mode_normal_x.png"}});
    tips["adaptive_infill_interior_density"].media = side_by_side(
        {{"adaptive_infill_interior_density_5.png", value("5%")}, {"adaptive_infill_interior_density_20.png", value("20%")}});
    tips["adaptive_infill_gradient"].values   = per_value({{"linear", "adaptive_infill_gradient_linear.png"},
                                                           {"quadratic", "adaptive_infill_gradient_quadratic.png"},
                                                           {"exponential", "adaptive_infill_gradient_exponential.png"}});
    tips["gyroid_optimized"].media            = off_on("gyroid_optimized");
    tips["sparse_infill_smooth_factor"].media = side_by_side(
        {{"sparse_infill_smooth_factor_0.png", value("0%")}, {"sparse_infill_smooth_factor_100.png", value("100%")}});
    tips["infill_direction"].media = side_by_side({{"infill_direction_0.png", value("0°")}, {"infill_direction_30.png", value("30°")}});
    tips["sparse_infill_rotate_template"].media = side_by_side(
        {{"infill_rotate_template_none.png", _L("Off")}, {"infill_rotate_template_template.png", value("0,30,60,90,120,150")}});
    // Locked Zag: the skin along the walls and the skeleton inside it.
    tips["skin_infill_density"].media = side_by_side(
        {{"lockedzag_default.png", value("25%")}, {"lockedzag_skin_density.png", value("50%")}});
    tips["skeleton_infill_density"].media = side_by_side(
        {{"lockedzag_default.png", value("25%")}, {"lockedzag_skeleton_density.png", value("50%")}});
    tips["skin_infill_depth"].media      = side_by_side({{"lockedzag_default.png", mm("2")}, {"lockedzag_skin_depth.png", mm("5")}});
    tips["skin_infill_line_width"].media = side_by_side(
        {{"lockedzag_default.png", value("100%")}, {"lockedzag_skin_width.png", value("160%")}});
    tips["skeleton_infill_line_width"].media = side_by_side(
        {{"lockedzag_default.png", value("100%")}, {"lockedzag_skeleton_width.png", value("160%")}});
    tips["infill_complete_top"].media = off_on("infill_complete_top");
    tips["infill_shift_step"].media   = side_by_side({{"infill_shift_step_0.png", mm("0")}, {"infill_shift_step_1_5.png", mm("1.5")}});
    // Lateral Lattice and Lateral Honeycomb, cut through.
    tips["lateral_lattice_angle_1"].media = side_by_side(
        {{"lateral_lattice_default.png", value("-45°")}, {"lateral_lattice_angle_1.png", value("-15°")}});
    tips["lateral_lattice_angle_2"].media = side_by_side(
        {{"lateral_lattice_default.png", value("45°")}, {"lateral_lattice_angle_2.png", value("15°")}});
    tips["infill_overhang_angle"].media = side_by_side(
        {{"infill_overhang_angle_60.png", value("60°")}, {"infill_overhang_angle_30.png", value("30°")}});
    // Lightning under a cube's top, sliced whole.
    tips["lightning_overhang_angle"].media = side_by_side(
        {{"lightning_default.png", value("45°")}, {"lightning_overhang.png", value("70°")}});
    tips["lightning_prune_angle"].media = side_by_side({{"lightning_default.png", value("45°")}, {"lightning_prune.png", value("75°")}});
    tips["lightning_straightening_angle"].media = side_by_side(
        {{"lightning_default.png", value("45°")}, {"lightning_straightening.png", value("10°")}});
    tips["infill_anchor_max"].media = side_by_side(
        {{"infill_anchor_max_none.png", _L("0 (Simple connect)")}, {"infill_anchor_max_default.png", mm("20")}});
    tips["infill_wall_overlap"].media = side_by_side(
        {{"infill_wall_overlap_0.png", value("0%")}, {"infill_wall_overlap_60.png", value("60%")}});
    tips["separated_infills"].media = off_on("separated_infills");

    // Solid infill and gaps. A point of a star, its top layer above a layer from its middle.
    tips["solid_infill_direction"].media = side_by_side(
        {{"solid_infill_direction_0.png", value("0°")}, {"solid_infill_direction_30.png", value("30°")}});
    tips["gap_fill_target"].values = per_value(
        {{"everywhere", "gap_fill_everywhere.png"}, {"topbottom", "gap_fill_topbottom.png"}, {"nowhere", "gap_fill_nowhere.png"}});
    tips["filter_out_gap_fill"].media = side_by_side({{"gap_fill_everywhere.png", mm("0")}, {"gap_fill_filtered.png", mm("2")}});

    // Advanced.
    tips["align_infill_direction_to_model"].media = off_on("align_infill_direction_to_model");
    tips["extra_solid_infills"].media             = side_by_side(
        {{"extra_solid_infills_none.png", _L("Off")}, {"extra_solid_infills_8_2.png", value("8#2")}});
    // Two piers and a deck across them: the bridge's own direction is 90 degrees.
    tips["bridge_angle"].media          = side_by_side({{"bridge_angle_0.png", value("0°")}, {"bridge_angle_30.png", value("30°")}});
    tips["relative_bridge_angle"].media = side_by_side({{"bridge_angle_30.png", _L("Off")}, {"bridge_angle_relative.png", _L("On")}});
    tips["internal_bridge_angle"].media = side_by_side(
        {{"internal_bridge_angle_0.png", value("0°")}, {"internal_bridge_angle_45.png", value("45°")}});
    tips["minimum_sparse_infill_area"].media = side_by_side(
        {{"minimum_sparse_infill_area_0.png", value("0 ") + _L("mm²")}, {"minimum_sparse_infill_area_15.png", value("15 ") + _L("mm²")}});
    tips["infill_combination"].media = side_by_side({{"infill_combination_off.png", _L("Off")}, {"infill_combination_100.png", _L("On")}});
    tips["infill_combination_max_layer_height"].media = side_by_side(
        {{"infill_combination_100.png", value("100%")}, {"infill_combination_50.png", value("50%")}});
    tips["detect_narrow_internal_solid_infill"].media = off_on("detect_narrow_internal_solid_infill");
    tips["ensure_vertical_shell_thickness"].values = per_value({{"none", "ensure_vertical_shell_none.png"},
                                                                {"ensure_critical_only", "ensure_vertical_shell_ensure_critical_only.png"},
                                                                {"ensure_moderate", "ensure_vertical_shell_ensure_moderate.png"},
                                                                {"ensure_all", "ensure_vertical_shell_ensure_all.png"}});

    // The settings below are drawn by scripts/gizmo_tips/settings_process_extra.py.
    tips["infill_anchor"].media = side_by_side({{"infill_anchor_0.png", value("0")}, {"infill_anchor_400.png", value("400%")}});
    tips["infill_lock_depth"].media = side_by_side({{"infill_lock_depth_0.png", mm("0")}, {"infill_lock_depth_2.png", mm("2")}});
    tips["solid_infill_rotate_template"].media = side_by_side(
        {{"solid_infill_rotate_template_none.png", _L("Off")}, {"solid_infill_rotate_template_template.png", value("0,30,60,90,120,150")}});
    tips["symmetric_infill_y_axis"].media = side_by_side(
        {{"symmetric_infill_y_axis_0.png", _L("Off")}, {"symmetric_infill_y_axis_1.png", _L("On")}});
}

} // namespace Slic3r::GUI
