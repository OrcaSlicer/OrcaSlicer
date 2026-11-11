#include "SettingTips.hpp"

#include <map>
#include <string>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IllustratedTipPopup.hpp"

namespace Slic3r::GUI {

namespace {

TipMedia pic(const std::string &file, const wxString &caption = {}) { return {"settings/process/" + file, caption}; }

// A caption that is a value as typed, unit included ("30°", "10%").
wxString raw(const char *text) { return wxString::FromUTF8(text); }

wxString mm(const char *number) { return wxString::FromUTF8(number) + " " + _L("mm"); }

// The same setting at two values, side by side: support_<name>_<a>.png and support_<name>_<b>.png.
std::vector<TipMedia> compare(const std::string &name, const std::string &a, const wxString &caption_a, const std::string &b,
                              const wxString &caption_b)
{
    return {pic("support_" + name + "_" + a + ".png", caption_a), pic("support_" + name + "_" + b + ".png", caption_b)};
}

} // namespace

// Pictures for the settings of the Process tab's Support page, drawn by scripts/gen_gizmo_tips.py
// (scripts/gizmo_tips/settings_process_support.py) from G-code that OrcaSlicer sliced at each value. The side
// views are a cut through a cap on a stem and the support under it: the part grey, the gap or the layers the
// setting changes in the accent colour. See SettingTips.hpp.
void add_process_support_setting_tips(SettingTips &tips)
{
    // Support. The type, the style and the patterns have one picture per value in SettingTipsProcess.cpp.
    tips["enable_support"].media = {pic("support_enable_0.png", _L("Off")), pic("support_style_grid.png", _L("On"))};
    tips["support_threshold_angle"].media    = compare("threshold", "30", raw("30°"), "60", raw("60°"));
    tips["support_threshold_overlap"].media  = compare("overlap", "10", raw("10%"), "90", raw("90%"));
    tips["raft_first_layer_density"].media   = compare("first_density", "30", raw("30%"), "90", raw("90%"));
    tips["raft_first_layer_expansion"].media = compare("first_expansion", "0", mm("0"), "3", mm("3"));
    tips["support_on_build_plate_only"].media = compare("on_plate", "0", _L("Off"), "1", _L("On"));

    // Raft.
    tips["raft_layers"].media           = compare("raft_layers", "0", raw("0"), "3", raw("3"));
    tips["raft_contact_distance"].media = compare("raft_contact", "0_1", mm("0.1"), "0_3", mm("0.3"));
    tips["raft_expansion"].media        = compare("raft_expansion", "0", mm("0"), "3", mm("3"));

    // Filament for supports: the pictures of the line types, shared with the Speed page.
    tips["support_filament"].media           = {pic("feature_support.png")};
    tips["support_interface_filament"].media = {pic("feature_support_interface.png")};

    // Support ironing. Its patterns are those of ironing a top surface.
    tips["support_ironing"].media         = compare("ironing", "0", _L("Off"), "1", _L("On"));
    tips["support_ironing_pattern"].values = {{"rectilinear", pic("ironing_pattern_rectilinear.png")},
                                              {"concentric", pic("ironing_pattern_concentric.png")}};
    tips["support_ironing_spacing"].media = compare("ironing_spacing", "0_1", mm("0.1"), "0_3", mm("0.3"));

    // Advanced: the gaps and interfaces, seen from the side.
    tips["support_top_z_distance"].media           = compare("top_z", "0_1", mm("0.1"), "0_3", mm("0.3"));
    tips["support_bottom_z_distance"].media        = compare("bottom_z", "0_1", mm("0.1"), "0_5", mm("0.5"));
    tips["support_interface_top_layers"].media     = compare("top_layers", "0", raw("0"), "3", raw("3"));
    tips["support_interface_bottom_layers"].media  = compare("bottom_layers", "0", raw("0"), "2", raw("2"));
    tips["support_expansion"].media                = compare("expansion", "0", mm("0"), "1_5", mm("1.5"));
    tips["support_object_xy_distance"].media       = compare("xy_distance", "0_35", mm("0.35"), "1_5", mm("1.5"));
    tips["support_object_first_layer_gap"].media   = compare("first_gap", "0_2", mm("0.2"), "1_5", mm("1.5"));
    // ... and the patterns, from above.
    tips["tree_support_wall_count"].media          = compare("wall_count", "1", raw("1"), "2", raw("2"));
    tips["support_base_pattern_spacing"].media     = compare("base_spacing", "1", mm("1"), "4", mm("4"));
    tips["support_angle"].media                    = compare("angle", "0", raw("0°"), "45", raw("45°"));
    tips["support_interface_spacing"].media        = compare("interface_spacing", "0", mm("0"), "1", mm("1"));
    tips["support_bottom_interface_spacing"].media = compare("bottom_spacing", "0", mm("0"), "1", mm("1"));
    tips["bridge_no_support"].media                = compare("bridge_no_support", "0", _L("Off"), "1", _L("On"));

    // Tree supports. The tips from above, at the top of the support; the branches in the plater's view.
    tips["tree_support_tip_diameter"].media            = compare("tip_diameter", "0_8", mm("0.8"), "2", mm("2"));
    tips["tree_support_branch_distance"].media         = compare("branch_distance", "2", mm("2"), "6", mm("6"));
    tips["tree_support_branch_distance_organic"].media = compare("branch_distance_organic", "1", mm("1"), "4", mm("4"));
    tips["tree_support_top_rate"].media                = compare("top_rate", "10", raw("10%"), "35", raw("35%"));
    tips["tree_support_branch_diameter"].media         = compare("branch_diameter", "2", mm("2"), "6", mm("6"));
    tips["tree_support_branch_diameter_organic"].media = compare("branch_diameter_organic", "2", mm("2"), "5", mm("5"));
    tips["tree_support_branch_diameter_angle"].media   = compare("diameter_angle", "0", raw("0°"), "10", raw("10°"));
    tips["tree_support_branch_angle_organic"].media    = compare("branch_angle_organic", "20", raw("20°"), "55", raw("55°"));
    tips["tree_support_angle_slow"].media              = compare("angle_slow", "10", raw("10°"), "35", raw("35°"));
    tips["tree_support_brim_width"].media              = compare("tree_brim", "0", mm("0"), "5", mm("5"));
}

} // namespace Slic3r::GUI
