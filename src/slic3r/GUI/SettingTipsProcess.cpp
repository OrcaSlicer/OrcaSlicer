#include "SettingTips.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IllustratedTipPopup.hpp"

namespace Slic3r::GUI {

namespace {

// One picture per value, captioned with the value's label: {enum value, file under settings/process}.
std::map<std::string, TipMedia> per_value(const std::vector<std::pair<std::string, std::string>> &files)
{
    std::map<std::string, TipMedia> values;
    for (const auto &[value, file] : files)
        values[value] = {"settings/process/" + file, {}};
    return values;
}

// The same setting off and on, side by side.
std::vector<TipMedia> off_on(const std::string &name)
{
    return {{"settings/process/" + name + "_off.png", _L("Off")}, {"settings/process/" + name + "_on.png", _L("On")}};
}

} // namespace

// Pictures for the tooltips of the process settings, drawn by scripts/gen_gizmo_tips.py into
// resources/images/settings/process (scripts/gizmo_tips/settings_process.py) from G-code that OrcaSlicer
// sliced for each value. See SettingTips.hpp.
void add_process_setting_tips(SettingTips &tips)
{
    // Quality: walls and seams.
    tips["seam_position"].values = per_value({{"nearest", "seam_nearest.png"},
                                              {"aligned", "seam_aligned.png"},
                                              {"aligned_back", "seam_aligned_back.png"},
                                              {"back", "seam_back.png"},
                                              {"random", "seam_random.png"}});
    tips["wall_sequence"].values = per_value({{"inner wall/outer wall", "wall_sequence_inner_outer.gif"},
                                              {"outer wall/inner wall", "wall_sequence_outer_inner.gif"},
                                              {"inner-outer-inner wall", "wall_sequence_inner_outer_inner.gif"}});
    tips["wall_direction"].values = per_value({{"ccw", "wall_direction_ccw.gif"}, {"cw", "wall_direction_cw.gif"}});
    tips["wall_generator"].values = per_value({{"classic", "wall_generator_classic.png"},
                                               {"arachne", "wall_generator_arachne.png"}});
    tips["extra_perimeters_on_overhangs"].media = off_on("extra_perimeters_on_overhangs");
    tips["make_overhang_printable"].media = off_on("overhang_printable");
    tips["thick_bridges"].media = off_on("thick_bridges");
    tips["counterbore_hole_bridging"].values = per_value({{"none", "counterbore_none.png"},
                                                          {"partiallybridge", "counterbore_partiallybridge.png"},
                                                          {"sacrificiallayer", "counterbore_sacrificiallayer.png"}});

    // Ironing.
    tips["ironing_type"].values = per_value({{"no ironing", "ironing_none.png"},
                                             {"top", "ironing_top.png"},
                                             {"topmost", "ironing_topmost.png"},
                                             {"solid", "ironing_solid.png"}});
    tips["ironing_pattern"].values = per_value({{"rectilinear", "ironing_pattern_rectilinear.png"},
                                                {"concentric", "ironing_pattern_concentric.png"}});

    // Fuzzy skin. "Painted only" roughens nothing until it is painted, so it shows the paint tool.
    tips["fuzzy_skin"].values = per_value({{"external", "fuzzy_skin_external.png"},
                                           {"hole", "fuzzy_skin_hole.png"},
                                           {"all", "fuzzy_skin_all.png"},
                                           {"disabled_fuzzy", "fuzzy_skin_disabled_fuzzy.png"}});
    tips["fuzzy_skin"].values["none"] = {"gizmo_toolbar/fuzzy_skin_painting.gif", {}};
    tips["fuzzy_skin_noise_type"].values = per_value({{"classic", "fuzzy_noise_classic.png"},
                                                      {"perlin", "fuzzy_noise_perlin.png"},
                                                      {"billow", "fuzzy_noise_billow.png"},
                                                      {"ridgedmulti", "fuzzy_noise_ridgedmulti.png"},
                                                      {"voronoi", "fuzzy_noise_voronoi.png"},
                                                      {"ripple", "fuzzy_noise_ripple.png"}});

    // Strength: infill patterns. Top, bottom and internal solid infill share one list of patterns.
    tips["sparse_infill_pattern"].values = per_value({{"rectilinear", "sparse_rectilinear.png"},
                                                      {"alignedrectilinear", "sparse_alignedrectilinear.png"},
                                                      {"zigzag", "sparse_zigzag.png"},
                                                      {"crosszag", "sparse_crosszag.png"},
                                                      {"lockedzag", "sparse_lockedzag.png"},
                                                      {"line", "sparse_line.png"},
                                                      {"grid", "sparse_grid.png"},
                                                      {"triangles", "sparse_triangles.png"},
                                                      {"tri-hexagon", "sparse_tri_hexagon.png"},
                                                      {"cubic", "sparse_cubic.png"},
                                                      {"adaptivecubic", "sparse_adaptivecubic.png"},
                                                      {"quartercubic", "sparse_quartercubic.png"},
                                                      {"supportcubic", "sparse_supportcubic.png"},
                                                      {"lightning", "sparse_lightning.png"},
                                                      {"honeycomb", "sparse_honeycomb.png"},
                                                      {"3dhoneycomb", "sparse_3dhoneycomb.png"},
                                                      {"lateral-honeycomb", "sparse_lateral_honeycomb.png"},
                                                      {"lateral-lattice", "sparse_lateral_lattice.png"},
                                                      {"crosshatch", "sparse_crosshatch.png"},
                                                      {"tpmsd", "sparse_tpmsd.png"},
                                                      {"tpmsfk", "sparse_tpmsfk.png"},
                                                      {"gyroid", "sparse_gyroid.png"},
                                                      {"concentric", "sparse_concentric.png"},
                                                      {"hilbertcurve", "sparse_hilbertcurve.png"},
                                                      {"archimedeanchords", "sparse_archimedeanchords.png"},
                                                      {"octagramspiral", "sparse_octagramspiral.png"}});
    const auto surface = per_value({{"monotonic", "surface_monotonic.gif"},
                                    {"monotonicline", "surface_monotonicline.gif"},
                                    {"rectilinear", "surface_rectilinear.gif"},
                                    {"alignedrectilinear", "surface_alignedrectilinear.gif"},
                                    {"concentric", "surface_concentric.gif"},
                                    {"spiralinset", "surface_spiralinset.gif"},
                                    {"hilbertcurve", "surface_hilbertcurve.gif"},
                                    {"archimedeanchords", "surface_archimedeanchords.gif"},
                                    {"octagramspiral", "surface_octagramspiral.gif"}});
    tips["top_surface_pattern"].values           = surface;
    tips["bottom_surface_pattern"].values        = surface;
    tips["internal_solid_infill_pattern"].values = surface;

    // Support. Manual supports and the Default style depend on what is painted or on the support type,
    // so they have no picture of their own.
    tips["support_type"].values = per_value({{"normal(auto)", "support_style_grid.png"}, {"tree(auto)", "support_style_organic.png"}});
    tips["support_style"].values = per_value({{"grid", "support_style_grid.png"},
                                              {"snug", "support_style_snug.png"},
                                              {"organic", "support_style_organic.png"},
                                              {"tree_slim", "support_style_tree_slim.png"},
                                              {"tree_strong", "support_style_tree_slim.png"},
                                              {"tree_hybrid", "support_style_tree_hybrid.png"}});
    tips["support_base_pattern"].values = per_value({{"rectilinear", "support_base_rectilinear.png"},
                                                     {"rectilinear-grid", "support_base_rectilinear_grid.png"},
                                                     {"honeycomb", "support_base_honeycomb.png"},
                                                     {"lightning", "support_base_lightning.png"},
                                                     {"hollow", "support_base_hollow.png"}});
    tips["support_interface_pattern"].values = per_value({{"auto", "support_interface_auto.png"},
                                                          {"rectilinear", "support_interface_rectilinear.png"},
                                                          {"concentric", "support_interface_concentric.png"},
                                                          {"spiralinset", "support_interface_spiralinset.png"},
                                                          {"rectilinear_interlaced", "support_interface_rectilinear_interlaced.png"},
                                                          {"grid", "support_interface_grid.png"}});

    // Skirt and brim.
    tips["brim_type"].values = per_value({{"outer_only", "brim_outer_only.png"},
                                          {"inner_only", "brim_inner_only.png"},
                                          {"outer_and_inner", "brim_outer_and_inner.png"},
                                          {"no_brim", "brim_no_brim.png"}});
    tips["brim_type"].values["brim_ears"] = {"gizmo_toolbar/brim_ears.gif", {}};
    tips["draft_shield"].values = per_value({{"disabled", "draft_shield_disabled.png"}, {"enabled", "draft_shield_enabled.png"}});

    // The pages' own files, so they can be filled independently.
    add_process_quality_setting_tips(tips);
    add_process_strength_setting_tips(tips);
    add_process_speed_setting_tips(tips);
    add_process_support_setting_tips(tips);
    add_process_others_setting_tips(tips);
}

} // namespace Slic3r::GUI
