// McpTools.hpp reaches wx's MSW headers through GLCanvas3D.hpp after OCCT has hidden the USER and GDI
// declarations they need; the Windows headers first keep them, as in test_plugin_host_api.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/FilamentMapPanel.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpEndpoint.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/PythonScriptsDialog.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/PreferenceOps.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/calib.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

bool has(const std::vector<std::string>& keys, const std::string& key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// What SettingsFactory::get_options() returns for an FFF printer: region keys for a part or
// modifier, and object keys as well for an object.
std::vector<std::string> fff_options(bool is_part)
{
    std::vector<std::string> keys = PrintRegionConfig().keys();
    if (!is_part) {
        const std::vector<std::string> object_keys = PrintObjectConfig().keys();
        keys.insert(keys.end(), object_keys.begin(), object_keys.end());
    }
    return keys;
}

// A 20 mm cube object with process wall_loops 2 and infill 15 %.
struct Scene
{
    Model              model;
    ModelObject*       object = nullptr;
    DynamicPrintConfig process;

    Scene()
    {
        object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        object->add_instance();
        process.set_key_value("wall_loops", new ConfigOptionInt(2));
        process.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));
    }

    ModelVolume* add(ModelVolumeType type)
    {
        return object->add_volume(make_cube(5., 5., 5.), type);
    }
};

// Process presets: a system one, user presets "Mine" (selected) and "Other", and one kept inside the project.
struct Processes
{
    PresetCollection presets{Preset::TYPE_PRINT, Preset::print_options(), static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults())};

    Processes()
    {
        add("0.20mm Standard").is_system = true;
        add("Mine");
        add("Other");
        add("In Project").is_project_embedded = true;
        presets.select_preset_by_name("Mine", true);
    }

    Preset& add(const std::string& name)
    {
        return presets.load_preset(std::string(), name, DynamicPrintConfig(presets.default_preset().config), /*select=*/false);
    }
};

} // namespace

TEST_CASE("the object scope takes the settings the object list offers an object, and its filament", "[Mcp]")
{
    const std::vector<std::string> one = model_scope_options(fff_options(false), false, 1);
    CHECK(has(one, "wall_loops"));
    CHECK(has(one, "enable_support"));
    CHECK(has(one, "fuzzy_skin"));
    CHECK(has(one, "extruder"));
    // Keys a person cannot add to an object, accepted while the scope took Preset::print_options().
    REQUIRE(has(Preset::print_options(), "print_extruder_id"));
    REQUIRE(has(Preset::print_options(), "print_sequence"));
    CHECK_FALSE(has(one, "print_extruder_id"));
    CHECK_FALSE(has(one, "print_sequence"));
    // Per-feature filaments are offered once there is a second filament.
    CHECK_FALSE(has(one, "outer_wall_filament_id"));
    CHECK(has(model_scope_options(fff_options(false), false, 2), "outer_wall_filament_id"));
    CHECK(has(one, "support_filament"));
}

TEST_CASE("the volume scope takes the settings the object list offers a part or modifier", "[Mcp]")
{
    const std::vector<std::string> part = model_scope_options(fff_options(true), true, 2);
    CHECK(has(part, "wall_loops"));
    CHECK(has(part, "sparse_infill_density"));
    CHECK(has(part, "outer_wall_filament_id"));
    CHECK(has(part, "extruder"));
    // Object-wide settings stay on the object.
    CHECK_FALSE(has(part, "layer_height"));
    CHECK_FALSE(has(part, "enable_support"));
    CHECK_FALSE(has(part, "print_extruder_id"));
}

TEST_CASE("an object's effective value is its override, else the process's", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);

    json e = effective_setting("wall_loops", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "4");
    CHECK(e["source"] == "object");
    CHECK_FALSE(e.contains("parts"));

    // The process's value is the global one, as the Process panel's Global row names it.
    e = effective_setting("sparse_infill_density", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "15%");
    CHECK(e["source"] == "global");

    // A key the process does not carry takes the option's default.
    e = effective_setting("enable_support", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "0");
    CHECK(e["source"] == "default");
}

TEST_CASE("a modifier's override applies only inside its own volume", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);
    ModelVolume* modifier = s.add(ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));

    // For the object: the process value, with the modifier's listed apart.
    json e = effective_setting("sparse_infill_density", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "15%");
    CHECK(e["source"] == "global");
    REQUIRE(e["parts"].size() == 1);
    CHECK(e["parts"][0]["part"] == 1);
    CHECK(e["parts"][0]["value"] == "40%");

    // Inside the modifier: its own value, and the object's or process's for the rest.
    e = effective_setting("sparse_infill_density", s.process, *s.object, modifier);
    CHECK(e["effective"] == "40%");
    CHECK(e["source"] == "part");
    CHECK_FALSE(e.contains("parts"));
    e = effective_setting("wall_loops", s.process, *s.object, modifier);
    CHECK(e["effective"] == "4");
    CHECK(e["source"] == "object");

    // Volumes that print nothing of their own are not listed.
    s.add(ModelVolumeType::NEGATIVE_VOLUME)->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(90));
    s.add(ModelVolumeType::SUPPORT_BLOCKER)->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(90));
    CHECK(effective_setting("sparse_infill_density", s.process, *s.object, nullptr)["parts"].size() == 1);
}

TEST_CASE("a filament of 0 is no filament of its own", "[Mcp]")
{
    Scene s;
    json e = effective_setting("extruder", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "1");
    CHECK(e["source"] == "default");

    s.object->config.set("extruder", 2);
    ModelVolume* part     = s.add(ModelVolumeType::MODEL_PART);
    ModelVolume* modifier = s.add(ModelVolumeType::PARAMETER_MODIFIER);
    part->config.set("extruder", 3);
    modifier->config.set("extruder", 0);

    e = effective_setting("extruder", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "2");
    CHECK(e["source"] == "object");
    REQUIRE(e["parts"].size() == 1);
    CHECK(e["parts"][0]["part"] == 1);
    CHECK(e["parts"][0]["value"] == "3");

    e = effective_setting("extruder", s.process, *s.object, part);
    CHECK(e["effective"] == "3");
    CHECK(e["source"] == "part");
    // A modifier's filament of 0 takes the filament of the part it overlaps, the object's where that part has none.
    e = effective_setting("extruder", s.process, *s.object, modifier);
    CHECK(e["effective"].is_null());
    CHECK(e["source"] == "overlapped_part");
    CHECK(e["object_effective"] == "2");
    CHECK(e["object_source"] == "object");
    CHECK(e["parts"] == json::array({{{"part", 1}, {"value", "3"}}}));
}

TEST_CASE("inside a modifier that sets none of a key, the value of the part it overlaps applies", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);
    s.object->volumes.front()->config.set("wall_loops", 5);
    ModelVolume* modifier = s.add(ModelVolumeType::PARAMETER_MODIFIER);

    // The part's 5 prints inside the modifier where it overlaps that part, the object's 4 where it overlaps another
    // part: no value is the one it prints with.
    json e = effective_setting("wall_loops", s.process, *s.object, modifier);
    CHECK(e["effective"].is_null());
    CHECK(e["source"] == "overlapped_part");
    CHECK(e["object_effective"] == "4");
    CHECK(e["object_source"] == "object");
    CHECK(e["parts"] == json::array({{{"part", 0}, {"value", "5"}}}));
    CHECK_FALSE(e.contains("height_ranges"));

    // A modifier before it that sets the key is a region it can overlap too; one after it is not.
    ModelVolume* later = s.add(ModelVolumeType::PARAMETER_MODIFIER);
    later->config.set("wall_loops", 8);
    CHECK(effective_setting("wall_loops", s.process, *s.object, modifier)["parts"] == json::array({{{"part", 0}, {"value", "5"}}}));
    e = effective_setting("sparse_infill_density", s.process, *s.object, later);
    CHECK(e["effective"] == "15%");
    CHECK(e["source"] == "global");
    modifier->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    e = effective_setting("sparse_infill_density", s.process, *s.object, later);
    CHECK(e["effective"].is_null());
    CHECK(e["object_effective"] == "15%");
    CHECK(e["object_source"] == "global");
    CHECK(e["parts"] == json::array({{{"part", 1}, {"value", "40%"}}}));

    // Where no part sets the key, the object's value is the one the modifier prints with.
    e = effective_setting("enable_support", s.process, *s.object, modifier);
    CHECK(e["effective"] == "0");
    CHECK(e["source"] == "default");
    CHECK_FALSE(e.contains("parts"));
    CHECK_FALSE(e.contains("object_effective"));
}

TEST_CASE("a painted modifier before a modifier that sets none of a key is a region it can overlap", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);
    ModelVolume* painted = s.add(ModelVolumeType::PAINTED_MODIFIER);
    painted->config.set("wall_loops", 7);
    ModelVolume* modifier = s.add(ModelVolumeType::PARAMETER_MODIFIER);

    // The slicer chains the mesh modifier's region onto the painted modifier's as onto a part's: 7 where it overlaps it.
    const json e = effective_setting("wall_loops", s.process, *s.object, modifier);
    CHECK(e["effective"].is_null());
    CHECK(e["source"] == "overlapped_part");
    CHECK(e["object_effective"] == "4");
    CHECK(e["object_source"] == "object");
    CHECK(e["parts"] == json::array({{{"part", 1}, {"value", "7"}}}));
}

TEST_CASE("inside a painted modifier that sets none of a key, the value of the part it overlaps applies", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);
    s.object->volumes.front()->config.set("wall_loops", 5);
    ModelVolume*       painted = s.add(ModelVolumeType::PAINTED_MODIFIER);
    DynamicPrintConfig lower;
    lower.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    lower.set_key_value("wall_loops", new ConfigOptionInt(6));
    s.object->layer_config_ranges[{0., 5.}].assign_config(lower);

    // As inside a mesh modifier: the part's value, and the range's between its heights.
    json e = effective_setting("wall_loops", s.process, *s.object, painted);
    CHECK(e["effective"].is_null());
    CHECK(e["source"] == "overlapped_part");
    CHECK(e["object_effective"] == "4");
    CHECK(e["object_source"] == "object");
    CHECK(e["parts"] == json::array({{{"part", 0}, {"value", "5"}}}));
    CHECK(e["height_ranges"] == json::array({{{"min_z", 0.}, {"max_z", 5.}, {"value", "6"}}}));

    // One that sets the key holds its own value through the range.
    painted->config.set("wall_loops", 7);
    e = effective_setting("wall_loops", s.process, *s.object, painted);
    CHECK(e["effective"] == "7");
    CHECK(e["source"] == "part");
    CHECK_FALSE(e.contains("height_ranges"));
}

TEST_CASE("a painted modifier that sets a key is listed among an object's overriding volumes", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);
    ModelVolume* painted = s.add(ModelVolumeType::PAINTED_MODIFIER);
    painted->config.set("wall_loops", 7);
    DynamicPrintConfig lower;
    lower.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    lower.set_key_value("wall_loops", new ConfigOptionInt(6));
    s.object->layer_config_ranges[{0., 5.}].assign_config(lower);
    const json painted_only = json::array({{{"part", 1}, {"value", "7"}}});

    // For the object, and inside a height range that sets the key, where a modifier's own value still applies.
    json e = effective_setting("wall_loops", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "4");
    CHECK(e["parts"] == painted_only);
    e = effective_setting("wall_loops", s.process, *s.object, nullptr, &s.object->layer_config_ranges.begin()->second.get());
    CHECK(e["effective"] == "6");
    CHECK(e["source"] == "height_range");
    CHECK(e["parts"] == painted_only);
}

TEST_CASE("a height range's value applies over its parts' own, and inside a modifier that sets none", "[Mcp]")
{
    Scene s;
    s.object->config.set("wall_loops", 4);
    ModelVolume* part = s.object->volumes.front();
    part->config.set("wall_loops", 5);
    ModelVolume*       modifier = s.add(ModelVolumeType::PARAMETER_MODIFIER);
    DynamicPrintConfig lower;
    lower.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    lower.set_key_value("wall_loops", new ConfigOptionInt(6));
    s.object->layer_config_ranges[{0., 5.}].assign_config(lower);
    const json ranges = json::array({{{"min_z", 0.}, {"max_z", 5.}, {"value", "6"}}});

    // The object's and the part's own values, with the range's between its heights.
    json e = effective_setting("wall_loops", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "4");
    CHECK(e["source"] == "object");
    CHECK(e["height_ranges"] == ranges);
    e = effective_setting("wall_loops", s.process, *s.object, part);
    CHECK(e["effective"] == "5");
    CHECK(e["source"] == "part");
    CHECK(e["height_ranges"] == ranges);

    // A modifier that sets none takes what it covers: the part's value, and the range's between its heights (its region
    // inherits the part's region, which the range is applied to); one that sets the key holds its own value through
    // the range.
    e = effective_setting("wall_loops", s.process, *s.object, modifier);
    CHECK(e["effective"].is_null());
    CHECK(e["source"] == "overlapped_part");
    CHECK(e["object_effective"] == "4");
    CHECK(e["parts"] == json::array({{{"part", 0}, {"value", "5"}}}));
    CHECK(e["height_ranges"] == ranges);
    modifier->config.set("wall_loops", 7);
    e = effective_setting("wall_loops", s.process, *s.object, modifier);
    CHECK(e["effective"] == "7");
    CHECK(e["source"] == "part");
    CHECK_FALSE(e.contains("height_ranges"));

    // The range itself: its value, over the part's, which it replaces; the modifier's still applies inside it.
    e = effective_setting("wall_loops", s.process, *s.object, nullptr, &s.object->layer_config_ranges.begin()->second.get());
    CHECK(e["effective"] == "6");
    CHECK(e["source"] == "height_range");
    CHECK(e["parts"] == json::array({{{"part", 1}, {"value", "7"}}}));
    CHECK_FALSE(e.contains("height_ranges"));
}

TEST_CASE("preset_save refuses the names the Save preset dialog refuses", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    Processes p;
    CHECK_THROWS_WITH(save_preset_inside_project(p.presets, "a:b", json::object()), ContainsSubstring("illegal characters"));
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "", json::object()), ToolFailure);
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "Mine ", json::object()), ToolFailure);
    // A system preset is never overwritten.
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "0.20mm Standard", json{{"overwrite", true}}), ToolFailure);
}

TEST_CASE("preset_save replaces another preset only with overwrite, and saves the selected one again without", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    Processes p;
    CHECK_THROWS_WITH(save_preset_inside_project(p.presets, "Other", json::object()), ContainsSubstring("overwrite"));
    CHECK_FALSE(save_preset_inside_project(p.presets, "Other", json{{"overwrite", true}}));
    // The selected preset's own name is the tab's plain Save.
    CHECK_FALSE(save_preset_inside_project(p.presets, "Mine", json::object()));
}

TEST_CASE("preset_save keeps a preset where the Save preset dialog would", "[Mcp]")
{
    Processes p;
    // A new name: where the edited preset is, unless inside_project says otherwise.
    CHECK_FALSE(save_preset_inside_project(p.presets, "New", json::object()));
    CHECK(save_preset_inside_project(p.presets, "New", json{{"inside_project", true}}));
    p.presets.select_preset_by_name("In Project", true);
    CHECK(save_preset_inside_project(p.presets, "New", json::object()));
    CHECK_FALSE(save_preset_inside_project(p.presets, "New", json{{"inside_project", false}}));
    // An existing name: where that preset is, as the dialog locks its choice.
    CHECK(save_preset_inside_project(p.presets, "In Project", json::object()));
    CHECK_FALSE(save_preset_inside_project(p.presets, "Mine", json{{"overwrite", true}}));
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "Mine", json{{"overwrite", true}, {"inside_project", true}}), ToolFailure);
}

namespace {

// An X1C process: its one extruder has a Standard and a High Flow variant.
DynamicPrintConfig x1c_process()
{
    DynamicPrintConfig c;
    c.set_deserialize_strict("print_extruder_id", "1,1");
    c.set_deserialize_strict("print_extruder_variant", "\"Direct Drive Standard\";\"Direct Drive High Flow\"");
    c.set_deserialize_strict("outer_wall_speed", "200,350");
    c.set_deserialize_strict("inner_wall_speed", "300");
    c.set_deserialize_strict("wall_loops", "2");
    return c;
}

const std::vector<int> one_extruder{etDirectDrive};
const std::vector<int> standard{nvtStandard};

} // namespace

TEST_CASE("an enum's meta lists the labels the GUI shows, in the order of its values", "[Mcp]")
{
    const json meta = option_meta(*print_config_def.get("fuzzy_skin"));
    REQUIRE(meta["labels"].size() == meta["values"].size());
    CHECK(meta["values"][1] == "external");
    CHECK(meta["labels"][1] == "Contour");
    CHECK_FALSE(option_meta(*print_config_def.get("wall_loops")).contains("labels"));
}

TEST_CASE("a setting's meta describes it with its whole tooltip", "[Mcp]")
{
    const ConfigOptionDef& def = *print_config_def.get("seam_slope_type");
    REQUIRE_FALSE(def.tooltip.empty());
    CHECK(option_meta(def)["description"] == def.tooltip);
}

TEST_CASE("settings_list_pages groups a type's settings by page and group, in alphabetical order", "[Mcp]")
{
    const std::map<std::string, std::string> places{{"wall_loops", "Strength > Walls"},
                                                    {"top_shell_layers", "Strength > Top/bottom shells"},
                                                    {"brim_type", "Others > Brim"},
                                                    {"brim_width", "Others > Brim"},
                                                    {"skirt_loops", "Others > Skirt"},
                                                    {"seam_position", "Quality > Seam"}};
    // The keys in the order the type lists them, one with no row on a page; seam_position the type does not list.
    const json pages = settings_pages(places, {"wall_loops", "brim_width", "brim_type", "top_shell_layers", "skirt_loops", "no_row"});
    const json expected{{"pages", json::array({
                             {{"page", "Others"},
                              {"groups", json::array({{{"group", "Brim"}, {"settings", 2}}, {{"group", "Skirt"}, {"settings", 1}}})}},
                             {{"page", "Strength"},
                              {"groups", json::array({{{"group", "Top/bottom shells"}, {"settings", 1}},
                                                      {{"group", "Walls"}, {"settings", 1}}})}},
                         })}};
    CHECK(pages == expected);
    CHECK(settings_pages(places, {}) == json{{"pages", json::array()}});
}

TEST_CASE("a per-variant value names its entries and the one the nozzle prints with", "[Mcp]")
{
    const DynamicPrintConfig process = x1c_process();
    json                     e       = variant_entries("outer_wall_speed", process, one_extruder, standard);
    CHECK(e["variants"] == json::array({"Direct Drive Standard", "Direct Drive High Flow"}));
    CHECK(e["active"] == json::array({0}));
    // A High Flow nozzle prints with the second entry.
    CHECK(variant_entries("outer_wall_speed", process, one_extruder, {nvtHighFlow})["active"] == json::array({1}));
    // One entry, or not a per-variant key: nothing to choose.
    CHECK(variant_entries("inner_wall_speed", process, one_extruder, standard).is_null());
    CHECK(variant_entries("wall_loops", process, one_extruder, standard).is_null());

    // Two extruders, each with both variants; the second has a High Flow nozzle.
    DynamicPrintConfig dual;
    dual.set_deserialize_strict("print_extruder_id", "1,1,2,2");
    dual.set_deserialize_strict("print_extruder_variant",
                                "\"Direct Drive Standard\";\"Direct Drive High Flow\";\"Direct Drive Standard\";\"Direct Drive High Flow\"");
    dual.set_deserialize_strict("outer_wall_speed", "200,350,200,350");
    e = variant_entries("outer_wall_speed", dual, {etDirectDrive, etDirectDrive}, {nvtStandard, nvtHighFlow});
    CHECK(e["variants"][3] == "extruder 2: Direct Drive High Flow");
    CHECK(e["active"] == json::array({0, 3}));
}

TEST_CASE("the printer's machine limits and per-extruder values name their entries", "[Mcp]")
{
    DynamicPrintConfig printer;
    printer.set_deserialize_strict("printer_extruder_id", "1");
    printer.set_deserialize_strict("printer_extruder_variant", "\"Direct Drive Standard\"");
    printer.set_deserialize_strict("machine_max_speed_x", "500,200");
    printer.set_deserialize_strict("nozzle_diameter", "0.4,0.6");
    json e = variant_entries("machine_max_speed_x", printer, one_extruder, standard);
    CHECK(e["variants"] == json::array({"Direct Drive Standard (normal mode)", "Direct Drive Standard (silent mode)"}));
    CHECK(e["active"] == json::array({0}));
    e = variant_entries("nozzle_diameter", printer, {etDirectDrive, etDirectDrive}, {nvtStandard, nvtStandard});
    CHECK(e["variants"] == json::array({"extruder 1", "extruder 2"}));
    CHECK_FALSE(e.contains("active"));
}

TEST_CASE("settings_set writes a single value to the per-variant entries in use, or one entry by index", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const DynamicPrintConfig process = x1c_process();

    // Behaviour change 7. Before, the scalar was written as it was, and the vector lost its other variant.
    DynamicPrintConfig collapsed = process;
    collapsed.set_deserialize_strict("outer_wall_speed", to_config_string(json(250), "outer_wall_speed"));
    REQUIRE(collapsed.option<ConfigOptionFloats>("outer_wall_speed")->size() == 1);
    // Now it goes to the entry in use, as typing into the tab's field does; the other variant keeps its value.
    CHECK(preset_setting_value("outer_wall_speed", json(250), process, one_extruder, standard) == "250,350");
    CHECK(preset_setting_value("outer_wall_speed", "250", process, one_extruder, standard) == "250,350");
    CHECK(preset_setting_value("outer_wall_speed", json(250), process, one_extruder, {nvtHighFlow}) == "200,250");
    // Another number of entries is still refused, naming the entries and the one in use; an array
    // with one value is such a number, not a single value.
    CHECK_THROWS_WITH(preset_setting_value("outer_wall_speed", json::array({250}), process, one_extruder, standard),
                      ContainsSubstring("holds 2 values"));
    try {
        preset_setting_value("outer_wall_speed", json::array({1, 2, 3}), process, one_extruder, standard);
        FAIL("three entries were accepted");
    } catch (const ToolFailure& e) {
        CHECK_THAT(e.what(), ContainsSubstring("0 = Direct Drive Standard (in use), 1 = Direct Drive High Flow"));
        CHECK_THAT(e.what(), ContainsSubstring("\"index\""));
        CHECK(e.details["active"] == json::array({0}));
    }

    // Every entry, or one by index.
    CHECK(preset_setting_value("outer_wall_speed", json::array({220, 360}), process, one_extruder, standard) == "220,360");
    CHECK(preset_setting_value("outer_wall_speed", {{"index", 1}, {"value", 250}}, process, one_extruder, standard) == "200,250");
    CHECK(preset_setting_value("outer_wall_speed", {{"index", 0}, {"value", "210"}}, process, one_extruder, standard) == "210,350");
    CHECK_THROWS_WITH(preset_setting_value("outer_wall_speed", {{"index", 2}, {"value", 250}}, process, one_extruder, standard),
                      ContainsSubstring("has 2 entries"));
    CHECK_THROWS_WITH(preset_setting_value("outer_wall_speed", {{"index", 1}, {"value", "250,260"}}, process, one_extruder, standard),
                      ContainsSubstring("takes one value"));
    CHECK_THROWS_AS(preset_setting_value("outer_wall_speed", {{"index", 1}}, process, one_extruder, standard), ToolFailure);
    CHECK_THROWS_AS(preset_setting_value("wall_loops", {{"index", 0}, {"value", 3}}, process, one_extruder, standard), ToolFailure);

    // Values with one entry, and settings that are not per variant, take a single value as before.
    CHECK(preset_setting_value("inner_wall_speed", json(280), process, one_extruder, standard) == "280");
    CHECK(preset_setting_value("wall_loops", json(3), process, one_extruder, standard) == "3");
}

TEST_CASE("compare lists the settings two presets differ in, entry by entry for filaments", "[Mcp]")
{
    Preset left(Preset::TYPE_PRINT, "Left");
    left.config = x1c_process();
    left.config.set_deserialize_strict("print_settings_id", "Left");
    Preset right = left;
    right.name   = "Right";
    right.config.set_deserialize_strict("print_settings_id", "Right");
    right.config.set_deserialize_strict("wall_loops", "3");
    right.config.set_deserialize_strict("outer_wall_speed", "200,300");

    const std::vector<std::string> keys{"wall_loops", "outer_wall_speed", "print_settings_id"};
    json items = preset_diff(left, right, keys);
    // A process compares whole values, as the Compare presets dialog does; the bookkeeping id is left out.
    REQUIRE(items.size() == 2);
    CHECK(items[0] == json({{"key", "outer_wall_speed"}, {"value", "200,350"}, {"other", "200,300"}}));
    CHECK(items[1] == json({{"key", "wall_loops"}, {"value", "2"}, {"other", "3"}}));
    // Only the keys asked for.
    CHECK(preset_diff(left, right, {"wall_loops"}).size() == 1);
    CHECK(preset_diff(left, left, keys).empty());

    // Filament vectors are compared per entry.
    Preset pla(Preset::TYPE_FILAMENT, "PLA");
    pla.config.set_deserialize_strict("filament_extruder_variant", "\"Direct Drive Standard\";\"Direct Drive High Flow\"");
    pla.config.set_deserialize_strict("nozzle_temperature", "220,220");
    Preset hot  = pla;
    hot.name    = "Hot PLA";
    hot.config.set_deserialize_strict("nozzle_temperature", "220,230");
    items = preset_diff(pla, hot, {"nozzle_temperature"});
    REQUIRE(items.size() == 1);
    CHECK(items[0] == json({{"key", "nozzle_temperature"}, {"index", 1}, {"value", "220"}, {"other", "230"}}));
}

TEST_CASE("changed lists every setting a call changed, the app's own adjustments included", "[Mcp]")
{
    DynamicPrintConfig before;
    before.set_deserialize_strict("spiral_mode", "0");
    before.set_deserialize_strict("wall_loops", "3");
    before.set_deserialize_strict("top_shell_layers", "5");
    before.set_deserialize_strict("sparse_infill_density", "15%");
    // Spiral vase confirmed: the key asked for, and those the spiral mode dialog adjusts.
    DynamicPrintConfig after = before;
    after.set_deserialize_strict("spiral_mode", "1");
    after.set_deserialize_strict("wall_loops", "1");
    after.set_deserialize_strict("top_shell_layers", "0");
    after.set_deserialize_strict("sparse_infill_density", "15%");
    CHECK(changed_settings(before, after) == json({{"spiral_mode", "1"}, {"top_shell_layers", "0"}, {"wall_loops", "1"}}));
    CHECK(changed_settings(after, after).empty());
    // An override removed or added.
    DynamicPrintConfig fewer = before;
    fewer.erase("wall_loops");
    CHECK(changed_settings(before, fewer) == json({{"wall_loops", nullptr}}));
    CHECK(changed_settings(fewer, before) == json({{"wall_loops", "3"}}));
}

TEST_CASE("an unsaved change is discarded when the edited preset no longer holds its value", "[Mcp]")
{
    // Unsaved before a preset switch: four walls, and the High Flow entry of the outer wall speed.
    const json changes{{"wall_loops", "4"}, {"outer_wall_speed#1", "300"}};
    // Discarded: the preset edited afterwards has other values.
    const DynamicPrintConfig process = x1c_process();
    CHECK(discarded_changes(changes, process) == json::array({"outer_wall_speed", "wall_loops"}));
    // Transferred: the preset edited afterwards holds them, whatever else differs.
    DynamicPrintConfig transferred = process;
    transferred.set_deserialize_strict("wall_loops", "4");
    transferred.set_deserialize_strict("outer_wall_speed", "180,300");
    CHECK(discarded_changes(changes, transferred).empty());
    // An entry the new preset does not have is gone.
    transferred.set_deserialize_strict("outer_wall_speed", "300");
    CHECK(discarded_changes(changes, transferred) == json::array({"outer_wall_speed"}));
}

namespace {

// Adds the preset `name` to `presets`, inheriting from `inherits` and made for `printers` alone when given.
void add_preset(PresetCollection& presets, const std::string& name, const std::string& inherits = {},
                const std::vector<std::string>& printers = {})
{
    Preset& preset = presets.load_preset(std::string(), name, DynamicPrintConfig(presets.default_preset().config), /*select=*/false);
    if (!inherits.empty())
        preset.inherits() = inherits;
    if (!printers.empty())
        preset.config.option<ConfigOptionStrings>("compatible_printers", true)->values = printers;
}

} // namespace

TEST_CASE("preset_delete refuses the presets the tab does not delete", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    Processes p;
    add_preset(p.presets, "Mine Child", "Mine");
    add_preset(p.presets, "From Bundle");
    p.presets.find_preset("From Bundle", false, true)->bundle_id = "a bundle";
    p.presets.select_preset_by_name("Mine", true);
    const PresetCollection filaments{Preset::TYPE_FILAMENT, Preset::filament_options(),
                                     static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults())};

    // User presets and presets inside the project, with nothing deleted along with them.
    CHECK(check_preset_deletable(p.presets, "Other", false, p.presets, filaments).empty());
    CHECK(check_preset_deletable(p.presets, "In Project", false, p.presets, filaments).empty());
    CHECK(check_preset_deletable(p.presets, "Mine Child", false, p.presets, filaments).empty());

    CHECK_THROWS_WITH(check_preset_deletable(p.presets, "0.20mm Standard", false, p.presets, filaments), ContainsSubstring("system preset"));
    CHECK_THROWS_WITH(check_preset_deletable(p.presets, "From Bundle", false, p.presets, filaments), ContainsSubstring("preset bundle"));
    try {
        check_preset_deletable(p.presets, "Other process", false, p.presets, filaments);
        FAIL("an unknown name was accepted");
    } catch (const ToolFailure& e) {
        CHECK(e.details["similar"].front() == "Other");
    }
    // A base preset that another inherits from, the selected one included, as Tab::delete_preset() refuses it.
    try {
        check_preset_deletable(p.presets, "Mine", true, p.presets, filaments);
        FAIL("a base preset with a child was accepted");
    } catch (const ToolFailure& e) {
        CHECK_THAT(e.what(), ContainsSubstring("inherit from it"));
        CHECK(e.details["inherited_by"] == json::array({"Mine Child"}));
    }
}

TEST_CASE("a custom printer's own presets are deleted with it only when asked", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const auto& defaults = static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults());
    PresetCollection printers{Preset::TYPE_PRINTER, Preset::printer_options(), defaults, "Default Printer"};
    PresetCollection prints{Preset::TYPE_PRINT, Preset::print_options(), defaults};
    PresetCollection filaments{Preset::TYPE_FILAMENT, Preset::filament_options(), defaults};
    add_preset(printers, "X1C");
    printers.find_preset("X1C", false, true)->is_system = true;
    add_preset(printers, "My Printer");
    add_preset(printers, "My X1C", "X1C");
    add_preset(prints, "For My Printer", {}, {"My Printer"});
    add_preset(prints, "Shared", {}, {"My Printer", "X1C"});
    add_preset(filaments, "My PLA", {}, {"My Printer"});

    // Counted as the GUI's Delete confirmation counts them, and refused without delete_attached.
    const json attached{{"process", json::array({"For My Printer"})}, {"filament", json::array({"My PLA"})}};
    try {
        check_preset_deletable(printers, "My Printer", false, prints, filaments);
        FAIL("a custom printer with attached presets was accepted");
    } catch (const ToolFailure& e) {
        CHECK_THAT(e.what(), ContainsSubstring("(process: 1, filament: 1)"));
        CHECK(e.details["attached"] == attached);
    }
    CHECK(check_preset_deletable(printers, "My Printer", true, prints, filaments) == attached);
    // A printer that inherits a system printer takes nothing with it, as in the GUI.
    CHECK(check_preset_deletable(printers, "My X1C", false, prints, filaments).empty());
    CHECK_THROWS_WITH(check_preset_deletable(printers, "X1C", true, prints, filaments), ContainsSubstring("system preset"));
}

TEST_CASE("the project scope takes one colour per slot and leaves a mixed slot's colour to its components", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const std::vector<std::string>   current{"#FFFFFF", "#000000", "#7F7F7F"};
    const std::vector<unsigned char> mixed{0, 0, 1}; // slot 3 mixes slots 1 and 2

    // Stored as the colour pickers store them; unchanged entries stay as they are, in any case.
    CHECK(check_filament_colours({"#ff0000", "#000000", "#7f7f7f"}, current, mixed) ==
          std::vector<std::string>{"#FF0000", "#000000", "#7F7F7F"});
    CHECK_THROWS_WITH(check_filament_colours({"#FF0000", "#000000"}, current, mixed), ContainsSubstring("one colour per filament slot: 3"));
    CHECK_THROWS_WITH(check_filament_colours({"red", "#000000", "#7F7F7F"}, current, mixed), ContainsSubstring("is not #RRGGBB"));
    CHECK_THROWS_AS(check_filament_colours({"#FF00001", "#000000", "#7F7F7F"}, current, mixed), ToolFailure);
    CHECK_THROWS_WITH(check_filament_colours({"#FFFFFF", "#000000", "#FF0000"}, current, mixed), ContainsSubstring("Slot 3 is a mixed filament"));

    // One slot by index, through settings_set's entry write.
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings(current));
    CHECK(preset_setting_value("filament_colour", {{"index", 1}, {"value", "#00FF00"}}, project, {}, {}) == "#FFFFFF;#00FF00;#7F7F7F");
}

TEST_CASE("flush_multiplier takes one value per nozzle within the Flushing volumes dialog's range", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    CHECK(check_flush_multipliers({0.7}, 1) == std::vector<double>{0.7});
    CHECK(check_flush_multipliers({0.7}, 2) == std::vector<double>{0.7, 0.7});
    CHECK(check_flush_multipliers({0.5, 1.5}, 2) == std::vector<double>{0.5, 1.5});
    CHECK(check_flush_multipliers({0., 3.}, 2) == std::vector<double>{0., 3.});
    CHECK_THROWS_WITH(check_flush_multipliers({0.5, 1.5, 1.}, 2), ContainsSubstring("one value per nozzle (2)"));
    CHECK_THROWS_WITH(check_flush_multipliers({3.5}, 1), ContainsSubstring("outside 0..3"));
    CHECK_THROWS_AS(check_flush_multipliers({-0.1}, 1), ToolFailure);
}

TEST_CASE("flush_volumes_matrix is slots x slots per nozzle in config order, limited as the dialog limits it", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    // Two physical slots and a mixed third, one nozzle: rows are "from", columns "to".
    const std::vector<double>        current{0, 280, 0, 320, 0, 0, 0, 0, 0};
    const std::vector<unsigned char> mixed{0, 0, 1};
    const std::vector<double>        multiplier{0.3};

    std::vector<double> matrix = current;
    matrix[1]                  = 400; // from slot 1 to slot 2
    CHECK(check_flush_matrix(matrix, 3, multiplier, current, mixed) == matrix);
    CHECK_THROWS_WITH(check_flush_matrix({0, 400, 320, 0}, 3, multiplier, current, mixed), ContainsSubstring("takes 9 values: 3x3"));
    // Two nozzles take a matrix each, one after the other.
    CHECK_THROWS_WITH(check_flush_matrix(matrix, 3, {1., 1.}, current, mixed), ContainsSubstring("takes 18 values"));

    std::vector<double> bad = matrix;
    bad[3]                  = -1;
    CHECK_THROWS_WITH(check_flush_matrix(bad, 3, multiplier, current, mixed), ContainsSubstring("(nozzle 1, from slot 2 to slot 1) is negative"));
    bad    = matrix;
    bad[4] = 50;
    CHECK_THROWS_WITH(check_flush_matrix(bad, 3, multiplier, current, mixed), ContainsSubstring("must be 0"));
    // A mixed slot's row and column stay as they are.
    bad    = matrix;
    bad[2] = 100;
    CHECK_THROWS_WITH(check_flush_matrix(bad, 3, multiplier, current, mixed), ContainsSubstring("belongs to a mixed filament"));

    // The dialog keeps volume x multiplier within g_max_flush_volume (20000 mm3 in Orca).
    std::vector<double> huge = matrix;
    huge[1]                  = 100000;
    CHECK(check_flush_matrix(huge, 3, {2.}, current, mixed)[1] == 10000.);
    CHECK(check_flush_matrix(huge, 3, {0.}, current, mixed)[1] == 100000.);
}

TEST_CASE("a plate's filament orders list every slot once, and its layer ranges start at layer 2 without overlapping", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    CHECK(filament_order(json::array({2, 1}), 2) == std::vector<int>{2, 1});
    CHECK_THROWS_WITH(filament_order(json::array({1, 1}), 2), ContainsSubstring("every filament slot 1..2 once"));
    CHECK_THROWS_AS(filament_order(json::array({1}), 2), ToolFailure);
    CHECK_THROWS_AS(filament_order(json::array({1, 2, 3}), 2), ToolFailure);
    CHECK_THROWS_AS(filament_order("2,1", 2), ToolFailure);

    // Sorted by layer as the dialog sorts its rows; a range without `to` runs to the last layer ("End").
    const auto ranges = layer_sequences(json::parse(R"([{"from": 21, "order": [1, 2]}, {"from": 2, "to": 20, "order": [2, 1]}])"), 2);
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].first == std::pair<int, int>{2, 20});
    CHECK(ranges[0].second == std::vector<int>{2, 1});
    CHECK(ranges[1].first == std::pair<int, int>{21, INT_MAX - 1});
    CHECK(layer_sequences_json(ranges) == json::parse(R"([{"from": 2, "to": 20, "order": [2, 1]}, {"from": 21, "order": [1, 2]}])"));
    // The plate tab's own open end, INT_MAX, reads the same.
    CHECK(layer_sequences_json({{{2, INT_MAX}, {1, 2}}}) == json::parse(R"([{"from": 2, "order": [1, 2]}])"));

    CHECK_THROWS_WITH(layer_sequences(json::parse(R"([{"from": 1, "order": [1, 2]}])"), 2), ContainsSubstring("start at layer 2"));
    CHECK_THROWS_WITH(layer_sequences(json::parse(R"([{"from": 5, "to": 4, "order": [1, 2]}])"), 2), ContainsSubstring("ends before it starts"));
    CHECK_THROWS_WITH(layer_sequences(json::parse(R"([{"from": 2, "order": [1, 2]}, {"from": 10, "to": 20, "order": [2, 1]}])"), 2),
                      ContainsSubstring("overlap"));
    CHECK_THROWS_WITH(layer_sequences(json::parse(R"([{"from": 2, "to": 10, "order": [1, 2]}, {"from": 10, "order": [2, 1]}])"), 2),
                      ContainsSubstring("overlap"));
    CHECK_THROWS_AS(layer_sequences(json::parse(R"([{"from": 2, "order": [1]}])"), 2), ToolFailure);
    CHECK_THROWS_AS(layer_sequences(json::parse(R"([{"from": 2, "until": 5, "order": [1, 2]}])"), 2), ToolFailure);
    CHECK_THROWS_AS(layer_sequences(json::array(), 2), ToolFailure);
}

TEST_CASE("a plate's layers are numbered as the Preview's layer slider numbers them", "[Mcp]")
{
    GCodeProcessorResult result;
    const auto           move = [&result](EMoveType type, ExtrusionRole role, float z, unsigned int layer, float print_z = 0.f) {
        GCodeProcessorResult::MoveVertex v;
        v.type           = type;
        v.extrusion_role = role;
        v.position       = Vec3f(10.f, 10.f, z);
        v.layer_id       = layer;
        v.print_z        = print_z;
        result.moves.push_back(v);
    };
    move(EMoveType::Extrude, erExternalPerimeter, 9.f, 0); // the first move, which the viewer does not load
    move(EMoveType::Extrude, erExternalPerimeter, 0.2f, 0);
    move(EMoveType::Travel, erNone, 0.6f, 0);  // a z hop
    move(EMoveType::Extrude, erCustom, 0.5f, 0); // custom G-code's extrusion
    move(EMoveType::Extrude, erPerimeter, 0.4f, 1);
    move(EMoveType::Extrude, erPerimeter, 0.6f, 3); // layer ids need not be consecutive
    std::vector<double> zs = gcode_layer_zs(result);
    REQUIRE(zs.size() == 3);
    CHECK(zs[0] == double(0.2f));
    CHECK(zs[1] == double(0.4f));
    CHECK(zs[2] == double(0.6f));

    // A belt print labels its layers with their print Z.
    for (std::size_t i = 0; i < result.moves.size(); ++i)
        result.moves[i].print_z = 1.f + float(result.moves[i].layer_id);
    result.belt_tilt_angle = 45.f;
    CHECK(gcode_layer_zs(result) == std::vector<double>{1., 2., 4.});
}

TEST_CASE("a plate's layer G-codes read back by height, on the layer the Preview slider shows them", "[Mcp]")
{
    CustomGCode::Info codes;
    codes.gcodes.push_back({double(5.2f), CustomGCode::PausePrint, 1, "", ""});
    codes.gcodes.push_back({double(0.6f), CustomGCode::ToolChange, 2, "#00FF00", ""});
    codes.gcodes.push_back({10.2, CustomGCode::Custom, 1, "", "M104 S215"});
    std::vector<double> zs;
    for (int i = 1; i <= 30; ++i)
        zs.push_back(double(0.2f * float(i))); // 0.2 mm layers up to 6 mm, as the viewer's floats
    const json out = layer_gcode_json(codes, zs);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == json{{"z", 0.6}, {"layer", 3}, {"type", "change_filament"}, {"filament", 2}, {"color", "#00FF00"}});
    CHECK(out[1] == json{{"z", 5.2}, {"layer", 26}, {"type", "pause"}});
    // Above the top layer, so on no layer of this slice.
    CHECK(out[2] == json{{"z", 10.2}, {"type", "custom"}, {"gcode", "M104 S215"}});
    // Without an up-to-date slice there are no layer numbers.
    CHECK_FALSE(layer_gcode_json(codes, {})[0].contains("layer"));
}

namespace {

// Three rows of the Preferences dialog's table, as preferences_get lists them: a checkbox, a choice and a number.
std::vector<Ops::Preference> preference_table()
{
    Ops::Preference arrange;
    arrange.key      = "auto_arrange";
    arrange.tab      = "behaviour";
    arrange.section  = "Prepare";
    arrange.label    = "Auto arrange plate after cloning";
    arrange.fallback = "false";
    Ops::Preference flush = arrange;
    flush.key             = "auto_calculate_flush";
    flush.label           = "Auto flush after changing...";
    flush.kind            = Ops::PreferenceKind::Choice;
    flush.choices         = {{"all", "All"}, {"color change", "Color"}, {"disabled", "None"}};
    flush.fallback        = "all";
    Ops::Preference fps   = arrange;
    fps.key               = "opengl_fps_cap";
    fps.tab               = "graphics";
    fps.section           = "FPS";
    fps.label             = "FPS cap";
    fps.kind              = Ops::PreferenceKind::Number;
    fps.min               = 0;
    fps.max               = 240;
    fps.fallback          = "";
    fps.refused           = "%1%: a reason a call may not write it.";
    return {arrange, flush, fps};
}

} // namespace

TEST_CASE("preferences_get lists the dialog's preferences, typed, and reads a named key outside them as stored", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const std::vector<Ops::Preference> table = preference_table();
    AppConfig                          config;
    config.set("auto_calculate_flush", "disabled");
    config.set("opengl_fps_cap", "60");
    config.set("region", "");
    config.set("presets", "filament", "Bambu PLA Basic"); // another section than the preferences'
    config.erase("app", "auto_arrange");

    // With no argument, every row of the table, in its order, with its value typed by its kind.
    const json all = preferences_settings(config, table, json::object());
    REQUIRE(all["items"].size() == 3);
    CHECK(all["total"] == 3);
    // Nothing stored: the value the dialog shows, the checkbox unchecked.
    CHECK(all["items"][0] == json{{"key", "auto_arrange"}, {"tab", "behaviour"}, {"section", "Prepare"},
                                  {"label", "Auto arrange plate after cloning"}, {"kind", "checkbox"}, {"value", false},
                                  {"stored", false}});
    CHECK(all["items"][1]["value"] == "disabled");
    CHECK(all["items"][1]["kind"] == "choice");
    CHECK(all["items"][1]["choices"] == json::array({{{"value", "all"}, {"label", "All"}},
                                                     {{"value", "color change"}, {"label", "Color"}},
                                                     {{"value", "disabled"}, {"label", "None"}}}));
    CHECK_FALSE(all["items"][1].contains("stored"));
    CHECK(all["items"][2]["value"] == 60);
    CHECK(all["items"][2]["kind"] == "number");
    CHECK(all["items"][2]["min"] == 0);
    CHECK(all["items"][2]["max"] == 240);
    // A row a call may not write says why.
    CHECK(all["items"][2]["refused"] == "opengl_fps_cap: a reason a call may not write it.");
    CHECK_FALSE(all.contains("unknown_keys"));

    // tab and keys narrow the list together.
    const json graphics = preferences_settings(config, table, json{{"tab", "graphics"}});
    REQUIRE(graphics["items"].size() == 1);
    CHECK(graphics["items"][0]["key"] == "opengl_fps_cap");
    CHECK(preferences_settings(config, table, json{{"tab", "behaviour"}})["items"].size() == 2);
    CHECK(preferences_settings(config, table, json{{"tab", "graphics"}, {"keys", {"auto_arrange"}}})["items"].empty());

    // Named keys: those of the table as listed, another key of the app section as stored, and the others unknown.
    const json named =
        preferences_settings(config, table, json{{"keys", {"region", "auto_calculate_flush", "filament", "no_such_preference"}}});
    REQUIRE(named["items"].size() == 2);
    CHECK(named["items"][0]["key"] == "auto_calculate_flush");
    CHECK(named["items"][0]["kind"] == "choice");
    // A key stored empty is known, with its empty value.
    CHECK(named["items"][1] == json{{"key", "region"}, {"value", ""}});
    CHECK(named["unknown_keys"] == json{"filament", "no_such_preference"});
    CHECK_THAT(named["note"].get<std::string>(), ContainsSubstring("not stored"));
    const json known = preferences_settings(config, table, json{{"keys", {"region"}}});
    CHECK_FALSE(known.contains("unknown_keys"));
    CHECK_FALSE(known.contains("note"));
}

TEST_CASE("preferences_get gives what the dialog shows where that is not the stored value, and marks a value read at the next start",
          "[Mcp]")
{
    std::vector<Ops::Preference> table = preference_table();
    table[0].restart_needed            = true;
    AppConfig config;
    config.set("opengl_fps_cap", "60");
    config.erase("app", "auto_arrange");

    // The dialog shows a value of its own for a key: it is the value, typed by the kind, and stored still says whether
    // the configuration holds one.
    const json shown = preferences_settings(config, table, json::object(), {{"auto_arrange", "true"}, {"opengl_fps_cap", "14100"}});
    REQUIRE(shown["items"].size() == 3);
    CHECK(shown["items"][0]["value"] == true);
    CHECK(shown["items"][0]["stored"] == false);
    CHECK(shown["items"][2]["value"] == 14100);
    CHECK_FALSE(shown["items"][2].contains("stored"));
    // Without one, the stored value.
    CHECK(preferences_settings(config, table, json::object())["items"][2]["value"] == 60);

    // A row read at the next start says so; the others say nothing of it.
    CHECK(shown["items"][0]["restart_needed"] == true);
    CHECK_FALSE(shown["items"][1].contains("restart_needed"));
}

namespace {

// The values of an enum property of `command`'s schema, as a set.
std::set<std::string> enum_of(const NativeCommand& command, const char* property)
{
    std::set<std::string> values;
    for (const json& v : command.input_schema.at("properties").at(property).at("enum"))
        values.insert(v.get<std::string>());
    return values;
}

// A call's context that fails the case when the call reaches its UI closure.
class NoUiContext final : public ToolContext
{
public:
    json run_ui(const std::function<json()>& /*fn*/) override
    {
        FAIL("the call reached its UI closure");
        return json();
    }
    json observe_ui(const std::function<json()>& /*fn*/) override
    {
        FAIL("the call reached its UI closure");
        return json();
    }
    const std::string&    job_id() const override { return m_job; }
    bool                  cancelled() const override { return false; }
    ToolResult            call_child(const std::string& /*tool*/, const json& /*args*/) override { return ToolResult::error("no child calls"); }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(json /*args*/) override {}
    void                  begin_operation() override {}

private:
    std::string m_job;
};

// A call's context that runs its UI closures in place, for a check a call makes there before it reads the app.
class InPlaceContext final : public ToolContext
{
public:
    json                  run_ui(const std::function<json()>& fn) override { return fn(); }
    json                  observe_ui(const std::function<json()>& fn) override { return fn(); }
    const std::string&    job_id() const override { return m_job; }
    bool                  cancelled() const override { return false; }
    ToolResult            call_child(const std::string& /*tool*/, const json& /*args*/) override
    {
        return ToolResult::error("no child calls");
    }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(json /*args*/) override {}
    void                  begin_operation() override {}

private:
    std::string m_job;
};

} // namespace

TEST_CASE("the settings tools take a type and a target", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const NativeCommand* get  = NativeCommands::find("settings_get");
    const NativeCommand* set  = NativeCommands::find("settings_set");
    const NativeCommand* list = NativeCommands::find("settings_list");
    REQUIRE(get != nullptr);
    REQUIRE(set != nullptr);
    REQUIRE(list != nullptr);
    for (const NativeCommand* c : {get, set}) {
        INFO(c->key);
        CHECK(enum_of(*c, "type") == std::set<std::string>{"printer", "process", "filament"});
        CHECK(enum_of(*c, "target") == std::set<std::string>{"global", "object", "part", "height_range"});
        CHECK_FALSE(c->input_schema["properties"].contains("scope"));
        for (const char* range : {"min_z", "max_z"})
            CHECK(c->input_schema["properties"].contains(range));
    }
    json scoped{{"scope", "process"}, {"keys", {"layer_height"}}};
    CHECK_THAT(argument_error(*get, scoped), ContainsSubstring("unknown argument scope"));

    // A type's settings, whole or one page of them: a preset's, or those an object or a region takes.
    CHECK(list->input_schema["required"] == json::array({"type"}));
    CHECK(enum_of(*list, "type") == std::set<std::string>{"printer", "process", "filament", "object", "region"});
    for (const char* absent : {"target", "keys", "compare", "scope"}) {
        INFO(absent);
        CHECK_FALSE(list->input_schema["properties"].contains(absent));
    }
    for (const char* present : {"where", "preset", "filament_slot", "include_meta", "limit", "offset"}) {
        INFO(present);
        CHECK(list->input_schema["properties"].contains(present));
    }

    // The refusals the arguments alone decide.
    const json nothing{{"type", "process"}};
    const json no_target{{"type", "filament"}, {"target", "object"}, {"object", 0}, {"keys", {"wall_loops"}}};
    // Nothing named is worded for the tool called: what settings_get reads by, what settings_set writes.
    const std::string read_nothing  = settings_request_error(nothing);
    const std::string write_nothing = settings_request_error(nothing, true);
    CHECK_THAT(read_nothing, ContainsSubstring("Nothing is named"));
    CHECK_THAT(read_nothing, ContainsSubstring("keys or compare"));
    CHECK_THAT(read_nothing, ContainsSubstring("settings_list_pages"));
    CHECK_THAT(write_nothing, ContainsSubstring("Nothing is named"));
    CHECK_THAT(write_nothing, ContainsSubstring("values"));
    CHECK_THAT(write_nothing, !ContainsSubstring("query"));
    CHECK_THAT(settings_request_error(no_target), ContainsSubstring("no target object"));
    CHECK_THAT(settings_request_error(no_target, true), ContainsSubstring("no target object"));
    CHECK(settings_request_error({{"target", "object"}, {"object", 0}}).empty());
    CHECK(settings_request_error({{"target", "object"}, {"object", 0}, {"values", {{"wall_loops", 3}}}}, true).empty());
    CHECK_THAT(settings_request_error({{"target", "part"}, {"part", 0}}), ContainsSubstring("object"));
    // With no object, or no part for the target part, the selected one.
    CHECK(settings_request_error({{"target", "object"}}).empty());
    CHECK(settings_request_error({{"target", "part"}}).empty());
    CHECK(settings_request_error({{"target", "height_range"}, {"min_z", 0}, {"max_z", 5}}).empty());
    // Both tools refuse them before their UI closure.
    for (const NativeCommand* c : {get, set})
        for (const json& args : {nothing, no_target}) {
            INFO(c->key << " " << args.dump());
            NoUiContext ctx;
            CHECK_THROWS_WITH(c->run(args, ctx), settings_request_error(args, c == set));
        }
}

TEST_CASE("the settings tools' object, part and height range go with the target they name", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const NativeCommand* get = NativeCommands::find("settings_get");
    const NativeCommand* set = NativeCommands::find("settings_set");
    REQUIRE(get != nullptr);
    REQUIRE(set != nullptr);
    // Each refused before anything is read, worded for what is wrong.
    const std::vector<std::pair<json, std::string>> refused{
        {{{"object", 0}, {"keys", {"wall_loops"}}}, "give target object, part or height_range"},
        {{{"target", "object"}, {"object", 0}, {"part", 1}, {"keys", {"wall_loops"}}}, "part goes with target part"},
        {{{"target", "height_range"}, {"object", 0}, {"min_z", 0}}, "target height_range needs min_z and max_z"},
        {{{"target", "part"}, {"object", 0}, {"min_z", 0}, {"max_z", 5}}, "min_z and max_z go with target height_range"},
        {{{"target", "part"}, {"object", 0}, {"keys", {"wall_loops"}}}, "target part names its part by object and part"}};
    for (const auto& [args, message] : refused) {
        INFO(args.dump());
        CHECK_THAT(settings_request_error(args), ContainsSubstring(message));
        json written = args;
        written.erase("keys");
        written["values"] = {{"wall_loops", 3}};
        CHECK_THAT(settings_request_error(written, true), ContainsSubstring(message));
        for (const NativeCommand* c : {get, set}) {
            const json call = c == set ? written : args;
            json       given = call;
            CHECK(argument_error(*c, given).empty());
            NoUiContext ctx;
            CHECK_THROWS_WITH(c->run(call, ctx), ContainsSubstring(message));
        }
    }
}

TEST_CASE("filament_install names filaments by names, or by a vendor and a filament type together", "[Mcp]")
{
    const NativeCommand* install = NativeCommands::find("filament_install");
    REQUIRE(install != nullptr);
    // Each passes the schema, and the command refuses it before it reads the app.
    const std::vector<std::pair<json, std::string>> refused{
        {json::object(), "Give names, or vendor and filament_type"},
        {{{"names", {"Bambu PLA Basic"}}, {"vendor", "Bambu Lab"}, {"filament_type", "PLA"}},
         "Give names, or vendor and filament_type, not both"},
        {{{"names", {"Bambu PLA Basic"}}, {"filament_type", "PLA"}}, "Give names, or vendor and filament_type, not both"},
        {{{"vendor", "Bambu Lab"}}, "vendor and filament_type go together"},
        {{{"filament_type", "PLA"}}, "vendor and filament_type go together"}};
    for (const auto& [args, message] : refused) {
        INFO(args.dump());
        json given = args;
        CHECK(argument_error(*install, given).empty());
        NoUiContext ctx;
        CHECK_THROWS_WITH(install->run(args, ctx), message);
    }
}

TEST_CASE("preferences_set needs a preference to write", "[Mcp]")
{
    const NativeCommand* write = NativeCommands::find("preferences_set");
    REQUIRE(write != nullptr);
    // It passes the schema, and the command refuses it before it reads the app.
    json given = {{"values", json::object()}};
    CHECK(argument_error(*write, given).empty());
    NoUiContext ctx;
    CHECK_THROWS_WITH(write->run(json{{"values", json::object()}}, ctx),
                      "values names no preference: give {key: value}, e.g. {\"auto_arrange\": false}");
}

TEST_CASE("plugin_enable refuses an empty capability before it reads the app", "[Mcp]")
{
    const NativeCommand* enable = NativeCommands::find("plugin_enable");
    REQUIRE(enable != nullptr);
    CHECK(enable->input_schema.at("properties").at("capability").at("minLength") == 1);
    // An empty name is no capability, and not the plugin itself either, which enabled false would unload.
    const json empty{{"plugin", "my_plugin"}, {"capability", ""}, {"enabled", false}};
    NoUiContext ctx;
    CHECK_THROWS_WITH(enable->run(empty, ctx),
                      "capability is empty: name one of the plugin's capabilities, or leave capability out to turn the plugin "
                      "itself on or off");
}

TEST_CASE("a target takes the settings its panel offers", "[Mcp]")
{
    using Targets = std::vector<std::string>;
    CHECK(targets_taking("layer_height", 1) == Targets{"object", "height_range"});
    CHECK(targets_taking("wall_loops", 1) == Targets{"object", "part", "height_range"});
    CHECK(targets_taking("skirt_loops", 1).empty());
    // Support is an object's alone, and a feature's filament is offered once there is a second filament.
    CHECK(targets_taking("enable_support", 1) == Targets{"object"});
    CHECK(targets_taking("sparse_infill_filament_id", 1).empty());
    CHECK(targets_taking("sparse_infill_filament_id", 2) == Targets{"object", "part", "height_range"});

    const std::vector<std::string> object = target_options("object", 1);
    const std::vector<std::string> part   = target_options("part", 1);
    const std::vector<std::string> range  = target_options("height_range", 1);
    CHECK(has(part, "wall_loops"));
    CHECK_FALSE(has(part, "layer_height"));
    // A part takes what an object takes, and a height range what a part takes and the layer height.
    for (const std::string& key : part) {
        INFO(key);
        CHECK(has(object, key));
        CHECK(has(range, key));
    }
    CHECK(has(range, "layer_height"));
    CHECK(range.size() == part.size() + 1);
    // The filament each row's filament column sets.
    for (const std::vector<std::string>* keys : {&object, &part, &range})
        CHECK(has(*keys, "extruder"));
    CHECK(target_options("global", 1).empty());
}

TEST_CASE("a setting a target does not take is refused with where it can be set", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const std::string layer_height = untaken_setting_error("layer_height", "part", 1);
    CHECK_THAT(layer_height, ContainsSubstring("layer_height is set on an object or a height range, not on a part"));
    CHECK_THAT(untaken_setting_error("enable_support", "height_range", 1),
               ContainsSubstring("enable_support is set on an object, not on a height range"));
    // A process setting no target takes is the process's alone.
    const std::string skirt = untaken_setting_error("skirt_loops", "object", 1);
    CHECK_THAT(skirt, ContainsSubstring("skirt_loops is set globally only"));
    CHECK_THAT(skirt, !ContainsSubstring("height range"));
    for (const char* target : {"object", "part", "height_range"}) {
        INFO(target);
        CHECK(untaken_setting_error("wall_loops", target, 1).empty());
    }
}

TEST_CASE("listing a type returns it whole unless it carries meta", "[Mcp]")
{
    const NativeCommand* list = NativeCommands::find("settings_list");
    REQUIRE(list != nullptr);
    CHECK(enum_of(*list, "type") == std::set<std::string>{"process", "printer", "filament", "object", "region"});
    // The process settings of an X1 Carbon, as many as the app lists.
    json items = json::array();
    for (int i = 0; i < 395; ++i)
        items.push_back({{"key", "setting_" + std::to_string(i)}, {"value", "0"}});
    const json whole = settings_list_page(items, json::object());
    CHECK(whole["total"] == 395);
    CHECK(whole["items"].size() == 395);
    CHECK_FALSE(whole.contains("next_offset"));
    // With meta, which where carries too, 100 a page.
    for (const json& args : {json{{"include_meta", true}}, json{{"where", "Strength"}}}) {
        INFO(args.dump());
        const json meta = settings_list_page(items, args);
        CHECK(meta["items"].size() == 100);
        CHECK(meta["next_offset"] == 100);
    }
    CHECK(settings_list_page(items, {{"where", "Strength"}, {"include_meta", false}})["items"].size() == 395);
    const json fifty = settings_list_page(items, {{"limit", 50}});
    CHECK(fifty["items"].size() == 50);
    CHECK(fifty["next_offset"] == 50);
    CHECK(settings_list_page(items, {{"offset", 350}})["items"].size() == 45);
}

TEST_CASE("the preferences, a plate's settings and the project's settings are commands of their own", "[Mcp]")
{
    const NativeCommand* preferences = NativeCommands::find("preferences_get");
    REQUIRE(preferences != nullptr);
    CHECK(preferences->listed);
    // It takes keys and tab, and needs none: with none it lists the Preferences dialog's preferences.
    for (const char* arg : {"keys", "tab"}) {
        INFO(arg);
        CHECK(preferences->input_schema["properties"].contains(arg));
    }
    CHECK_FALSE(preferences->input_schema["properties"].contains("query"));
    CHECK(preferences->input_schema.value("required", json::array()).empty());
    const NativeCommand* write = NativeCommands::find("preferences_set");
    REQUIRE(write != nullptr);
    CHECK(write->listed);
    CHECK(write->input_schema["required"] == json::array({"values"}));
    for (const char* key : {"plate_get_settings", "plate_set_settings", "project_get_settings", "project_set_settings"}) {
        INFO(key);
        const NativeCommand* c = NativeCommands::find(key);
        REQUIRE(c != nullptr);
        CHECK(c->listed);
    }
    for (const char* key : {"plate_set_settings", "project_set_settings"}) {
        INFO(key);
        CHECK(NativeCommands::find(key)->input_schema["required"] == json::array({"values"}));
    }
    for (const char* key : {"preferences_get", "plate_get_settings", "plate_set_settings", "project_get_settings", "project_set_settings"})
        for (const char* absent : {"type", "target", "scope"}) {
            INFO(key << " " << absent);
            CHECK_FALSE(NativeCommands::find(key)->input_schema["properties"].contains(absent));
        }
}

TEST_CASE("a plate's settings are those of its Plate Settings and filament grouping dialogs, and its name and lock are none of them",
          "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const std::vector<std::string> keys = plate_setting_keys();
    CHECK(keys == std::vector<std::string>{"curr_bed_type", "print_sequence", "spiral_mode", "first_layer_print_sequence",
                                           "other_layers_print_sequence", "filament_map_mode", "filament_map"});
    CHECK_FALSE(has(keys, "name"));
    CHECK_FALSE(has(keys, "locked"));

    // A key a plate's settings lack is refused before the plate is read, whatever the other values.
    const NativeCommand* set = NativeCommands::find("plate_set_settings");
    REQUIRE(set != nullptr);
    for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{{"name", "x"}, {"locked", true}}) {
        INFO(key);
        const json     args{{"plate", 0}, {"values", {{"print_sequence", "by object"}, {key, value}}}};
        InPlaceContext ctx;
        CHECK_THROWS_WITH(set->run(args, ctx), ContainsSubstring("Unknown setting \"" + key + "\" for the plate settings"));
    }
}

TEST_CASE("preset_list lists the presets the selectors do not offer only when asked", "[Mcp]")
{
    const NativeCommand* list = NativeCommands::find("preset_list");
    REQUIRE(list != nullptr);
    CHECK(list->input_schema["properties"].contains("include_uninstalled"));
    CHECK_FALSE(list->input_schema["properties"].contains("include_hidden"));
}

TEST_CASE("a plate's filament map gives each filament an extruder the printer has, in the manual grouping alone", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    // Three filaments on a printer with two extruders.
    CHECK(filament_map_error("Manual", {1, 2, 2}, 3, 2).empty());
    CHECK_THAT(filament_map_error("Manual", {1, 2}, 3, 2), ContainsSubstring("3 filaments"));
    CHECK_THAT(filament_map_error("Manual", {1, 2, 2, 1}, 3, 2), ContainsSubstring("3 filaments"));
    CHECK_THAT(filament_map_error("Manual", {1, 3, 2}, 3, 2), ContainsSubstring("extruder 3"));
    CHECK_THAT(filament_map_error("Manual", {0, 1, 2}, 3, 2), ContainsSubstring("extruder 0"));
    // The automatic modes group the filaments themselves when the plate slices, and a plate following the
    // project's grouping ("") has no map of its own.
    CHECK_THAT(filament_map_error("Auto For Flush", {1, 2, 2}, 3, 2), ContainsSubstring("Manual"));
    CHECK_THAT(filament_map_error("", {1, 2, 2}, 3, 2), ContainsSubstring("Manual"));
}

TEST_CASE("a manual filament map needs a nozzle of each volume type its used extruders print with", "[Mcp]")
{
    // A left extruder with one Standard nozzle and a right one that can hold six, with six Standard nozzles.
    NozzleInventory inventory{{1, 6}, {nvtStandard, nvtHighFlow}, {{{nvtStandard, 1}}, {{nvtStandard, 6}}}};
    REQUIRE(has_multi_nozzle_extruder(inventory.max_nozzle_counts));

    // The right extruder prints High Flow and has no such nozzle: refused once a printed filament goes to it.
    std::optional<NozzleShortage> shortage = manual_map_nozzle_shortage(inventory, {1, 2}, {1, 2}, {0, 1});
    REQUIRE(shortage.has_value());
    CHECK(shortage->extruder == 1);
    CHECK(shortage->volume_type == nvtHighFlow);
    CHECK_FALSE(manual_map_nozzle_shortage(inventory, {1, 2}, {1, 1}, {0, 0}).has_value());
    // An extruder that only a filament the plate does not print goes to is not used.
    CHECK_FALSE(manual_map_nozzle_shortage(inventory, {1}, {1, 2}, {0, 1}).has_value());
    // The left extruder's own type is what it needs, whatever the volume map says of its filaments.
    inventory.volume_types = {nvtHighFlow, nvtStandard};
    shortage               = manual_map_nozzle_shortage(inventory, {1, 2}, {1, 2}, {0, 0});
    REQUIRE(shortage.has_value());
    CHECK(shortage->extruder == 0);
    CHECK(shortage->volume_type == nvtHighFlow);

    // A Hybrid extruder needs a nozzle of each type the volume map gives its filaments.
    inventory.volume_types = {nvtStandard, nvtHybrid};
    CHECK_FALSE(manual_map_nozzle_shortage(inventory, {1, 2, 3}, {1, 2, 2}, {0, 0, 0}).has_value());
    shortage = manual_map_nozzle_shortage(inventory, {1, 2, 3}, {1, 2, 2}, {0, 0, 1});
    REQUIRE(shortage.has_value());
    CHECK(shortage->extruder == 1);
    CHECK(shortage->volume_type == nvtHighFlow);
    // With High Flow nozzles alone, its Standard filament is the one refused, and Standard is named before High Flow.
    inventory.nozzle_counts[1] = {{nvtHighFlow, 6}};
    CHECK_FALSE(manual_map_nozzle_shortage(inventory, {1, 2}, {1, 2}, {0, 1}).has_value());
    shortage = manual_map_nozzle_shortage(inventory, {1, 2, 3}, {1, 2, 2}, {0, 0, 1});
    REQUIRE(shortage.has_value());
    CHECK(shortage->extruder == 1);
    CHECK(shortage->volume_type == nvtStandard);
    inventory.nozzle_counts[1] = {};
    shortage                   = manual_map_nozzle_shortage(inventory, {1, 2, 3}, {1, 2, 2}, {0, 1, 0});
    REQUIRE(shortage.has_value());
    CHECK(shortage->volume_type == nvtStandard);

    // A printer with no multi-nozzle extruder (a nil count is none) keeps no inventory to check.
    for (const std::vector<int>& counts : {std::vector<int>{1, 1}, std::vector<int>{1, ConfigOptionIntsNullable::nil_value()}}) {
        inventory.max_nozzle_counts = counts;
        CHECK_FALSE(has_multi_nozzle_extruder(inventory.max_nozzle_counts));
        CHECK_FALSE(manual_map_nozzle_shortage(inventory, {1, 2, 3}, {1, 2, 2}, {0, 1, 0}).has_value());
    }
}

TEST_CASE("a preset list is filtered by vendor, filament type, printer and source", "[Mcp]")
{
    const NativeCommand* list = NativeCommands::find("preset_list");
    REQUIRE(list != nullptr);
    for (const char* filter : {"vendor", "filament_type", "printer", "source"}) {
        INFO(filter);
        CHECK(list->input_schema["properties"].contains(filter));
    }
    CHECK(enum_of(*list, "source") == std::set<std::string>{"system", "user", "project"});

    // Filaments as preset_list builds their items: two system ones of a vendor, a user one and one kept in the project
    // of another, a system one incompatible with the selected printer and one a person has not installed.
    const json items = json::array({
        {{"name", "Bambu PLA Basic @BBL X1C"}, {"system", true}, {"user", false}, {"vendor", "Bambu Lab"}, {"filament_type", "PLA"}},
        {{"name", "Bambu PETG HF @BBL X1C"}, {"system", true}, {"user", false}, {"vendor", "Bambu Lab"}, {"filament_type", "PETG"}},
        {{"name", "My PLA"}, {"system", false}, {"user", true}, {"vendor", "Generic"}, {"filament_type", "PLA"}, {"alias", "Mine"}},
        {{"name", "Kept PLA"}, {"system", false}, {"user", true}, {"project", true}, {"vendor", "Generic"}, {"filament_type", "PLA"}},
        {{"name", "Bambu PLA Basic @BBL X1C 0.2 nozzle"}, {"system", true}, {"user", false}, {"vendor", "Bambu Lab"},
         {"filament_type", "PLA"}, {"compatible", false}},
        {{"name", "Bambu PLA Silk @BBL X1C"}, {"system", true}, {"user", false}, {"vendor", "Bambu Lab"}, {"filament_type", "PLA"},
         {"installed", false}},
    });
    const auto names = [](const json& listed) {
        std::vector<std::string> out;
        for (const json& item : listed)
            out.push_back(item["name"].get<std::string>());
        return out;
    };
    using Names = std::vector<std::string>;
    const std::function<bool(const json&)> selected_printer;

    // Unfiltered, the compatible and installed ones; a query matches a name or an alias.
    CHECK(names(filter_preset_items(items, json::object(), selected_printer)) ==
          Names{"Bambu PLA Basic @BBL X1C", "Bambu PETG HF @BBL X1C", "My PLA", "Kept PLA"});
    CHECK(names(filter_preset_items(items, {{"query", "mine"}}, selected_printer)) == Names{"My PLA"});
    // A vendor and a filament type, in either case.
    CHECK(names(filter_preset_items(items, {{"vendor", "bambu lab"}, {"filament_type", "pla"}}, selected_printer)) ==
          Names{"Bambu PLA Basic @BBL X1C"});
    CHECK(names(filter_preset_items(items, {{"vendor", "Bambu Lab"}, {"filament_type", "PLA"}, {"include_uninstalled", true}},
                                    selected_printer)) == Names{"Bambu PLA Basic @BBL X1C", "Bambu PLA Silk @BBL X1C"});
    CHECK(names(filter_preset_items(items, {{"vendor", "Polymaker"}}, selected_printer)).empty());
    // Where a preset comes from: a preset kept in the project is no user preset.
    CHECK(names(filter_preset_items(items, {{"source", "system"}}, selected_printer)) ==
          Names{"Bambu PLA Basic @BBL X1C", "Bambu PETG HF @BBL X1C"});
    CHECK(names(filter_preset_items(items, {{"source", "user"}}, selected_printer)) == Names{"My PLA"});
    CHECK(names(filter_preset_items(items, {{"source", "project"}}, selected_printer)) == Names{"Kept PLA"});

    // Another printer: what fits it decides, in place of the flag for the selected printer, and the items say so.
    const std::function<bool(const json&)> fits_nozzle_02 = [](const json& item) {
        const std::string name = item["name"];
        return name.find("0.2 nozzle") != std::string::npos || name == "My PLA";
    };
    const json other = filter_preset_items(items, {{"printer", "Bambu Lab X1 Carbon 0.2 nozzle"}}, fits_nozzle_02);
    CHECK(names(other) == Names{"My PLA", "Bambu PLA Basic @BBL X1C 0.2 nozzle"});
    CHECK_FALSE(other[1].contains("compatible"));
    const json every = filter_preset_items(items, {{"printer", "Bambu Lab X1 Carbon 0.2 nozzle"}, {"include_incompatible", true}},
                                           fits_nozzle_02);
    REQUIRE(every.size() == 5);
    CHECK(every[0]["name"] == "Bambu PLA Basic @BBL X1C");
    CHECK(every[0]["compatible"] == false);

    // A filter goes with the presets it can name: a printer for the filaments and processes, a filament type for the
    // filaments; refused before the app is read.
    for (const json& args : {json{{"type", "printer"}, {"printer", "Bambu Lab X1 Carbon 0.2 nozzle"}},
                             json{{"type", "process"}, {"filament_type", "PLA"}}}) {
        INFO(args.dump());
        NoUiContext ctx;
        CHECK_THROWS_AS(list->run(args, ctx), ToolFailure);
    }
}

TEST_CASE("the printer type of settings_set takes the extruder count, and each extruder's nozzle flow type and diameter", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    // The Extruders field of the Printer settings: 1 or more, up to the app's limit, and greyed out while single
    // extruder multi-material is on.
    CHECK(extruders_count_value(json(2), false) == 2);
    CHECK(extruders_count_value(json("3"), false) == 3);
    CHECK_THROWS_WITH(extruders_count_value(json(0), false), ContainsSubstring("1 to 64"));
    CHECK_THROWS_WITH(extruders_count_value(json(65), false), ContainsSubstring("1 to 64"));
    CHECK_THROWS_AS(extruders_count_value(json(1.5), false), ToolFailure);
    CHECK_THROWS_WITH(extruders_count_value(json(2), true), ContainsSubstring("single extruder multimaterial"));
    // It is set alone, on the printer, before the app is read: the extruders it adds take their settings in a
    // following call.
    const NativeCommand* set = NativeCommands::find("settings_set");
    REQUIRE(set != nullptr);
    for (const json& args : {json{{"type", "printer"}, {"values", {{"extruders_count", 2}, {"nozzle_diameter", "0.4,0.4"}}}},
                             json{{"type", "printer"}, {"values", {{"extruders_count", 2}}}, {"reset_all", true}},
                             json{{"values", {{"extruders_count", 2}}}}}) {
        INFO(args.dump());
        CHECK_THAT(settings_request_error(args, true), ContainsSubstring("extruders_count"));
        NoUiContext ctx;
        CHECK_THROWS_WITH(set->run(args, ctx), settings_request_error(args, true));
    }
    CHECK(settings_request_error({{"type", "printer"}, {"values", {{"extruders_count", 2}}}}, true).empty());

    // A nozzle flow type per extruder, as each extruder's card in the sidebar picks it.
    DynamicPrintConfig printer;
    printer.set_deserialize_strict("nozzle_diameter", "0.4,0.4");
    printer.set_deserialize_strict("nozzle_volume_type", "Standard,Standard");
    const std::vector<int> two_extruders{etDirectDrive, etDirectDrive};
    const std::vector<int> standard_nozzles{nvtStandard, nvtStandard};
    CHECK(variant_entries("nozzle_volume_type", printer, two_extruders, standard_nozzles)["variants"] ==
          json::array({"extruder 1", "extruder 2"}));
    CHECK(preset_setting_value("nozzle_volume_type", {{"index", 1}, {"value", "High Flow"}}, printer, two_extruders,
                               standard_nozzles) == "Standard,High Flow");
    CHECK_THROWS_WITH(preset_setting_value("nozzle_volume_type", "High Flow", printer, two_extruders, standard_nozzles),
                      ContainsSubstring("holds 2 values"));

    // A diameter one of the printer model's variants has switches to that variant, as the sidebar's nozzle diameter
    // does; any other is typed into the Printer settings.
    const std::vector<std::string> variants{"0.2", "0.4", "0.6", "0.8"};
    CHECK(nozzle_diameter_variant({0.6}, variants, "0.4") == "0.6");
    CHECK(nozzle_diameter_variant({0.6, 0.6}, variants, "0.4") == "0.6");
    CHECK(nozzle_diameter_variant({0.4, 0.6}, variants, "0.4").empty());
    CHECK(nozzle_diameter_variant({0.5}, variants, "0.4").empty());
    CHECK(nozzle_diameter_variant({0.4}, variants, "0.4").empty());
    CHECK(nozzle_diameter_variant({0.25}, {"0.25", "0.4"}, "0.4") == "0.25");
    // A named variant shares its diameter with the plain one, which the diameter switches back to.
    CHECK(nozzle_diameter_variant({0.4}, {"0.4", "0.4 High Flow"}, "0.4 High Flow") == "0.4");
}

TEST_CASE("the run's flag lets scripts run unrestricted without changing the setting", "[Mcp]")
{
    // The flag is process-wide: it goes back off however the case ends.
    struct RunFlagOff
    {
        ~RunFlagOff() { set_python_scripts_for_run(false); }
    } run_flag_off;

    AppConfig config;
    config.set_bool("python_scripts_unrestricted", false);
    set_python_scripts_for_run(true);
    CHECK(python_scripts_set_for_run());
    CHECK(python_scripts_unrestricted(config));
    CHECK_FALSE(python_scripts_setting(config));
    CHECK_FALSE(config.get_bool("python_scripts_unrestricted"));

    set_python_scripts_for_run(false);
    CHECK_FALSE(python_scripts_unrestricted(config));
    config.set_bool("python_scripts_unrestricted", true);
    CHECK(python_scripts_unrestricted(config));
    CHECK(python_scripts_setting(config));

    // Another key, whatever its name, is not the setting.
    AppConfig other;
    other.set_bool("python_scripts_unrestricted", false);
    other.set_bool("mcp_python_unrestricted", true);
    CHECK_FALSE(python_scripts_unrestricted(other));
    CHECK_FALSE(python_scripts_setting(other));
}

TEST_CASE("the endpoint's port is the option's, then the setting's, then the default", "[Mcp]")
{
    CHECK(endpoint_port(13700, "13800") == 13700);
    CHECK(endpoint_port(0, "13800") == 13800);
    CHECK(endpoint_port(0, "") == 13619);
    CHECK(endpoint_port(0, "abc") == 13619);
    CHECK(endpoint_port(0, "0") == 13619);
    CHECK(endpoint_port(0, "70000") == 13619);
    // An option out of range gives the default, not the setting.
    CHECK(endpoint_port(70000, "13800") == 13619);
}

namespace {

// A printer for the calibration tests: its firmware, extruder type and junction deviation (0: classic jerk).
DynamicPrintConfig calib_printer(GCodeFlavor flavor = gcfKlipper, ExtruderType extruder = etDirectDrive, double junction_deviation = 0.)
{
    DynamicPrintConfig printer = DynamicPrintConfig::full_print_config();
    printer.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(flavor));
    printer.set_key_value("extruder_type", new ConfigOptionEnumsGeneric{int(extruder)});
    printer.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{junction_deviation});
    printer.set_key_value("belt_printer", new ConfigOptionBool(false));
    return printer;
}

Calib_Params calib_args(const json& args, const DynamicPrintConfig& printer = calib_printer())
{
    std::vector<std::string> warnings;
    return calib_params_from_args(args, printer, warnings);
}

} // namespace

TEST_CASE("calibration_generate_test arguments map to the dialog's calibration modes", "[Mcp]")
{
    for (const auto& [args, mode] : std::initializer_list<std::pair<json, CalibMode>>{
             {{{"test", "temperature"}}, CalibMode::Calib_Temp_Tower},
             {{{"test", "max_volumetric"}}, CalibMode::Calib_Vol_speed_Tower},
             {{{"test", "pressure_advance"}}, CalibMode::Calib_PA_Tower},
             {{{"test", "pressure_advance"}, {"method", "line"}}, CalibMode::Calib_PA_Line},
             {{{"test", "pressure_advance"}, {"method", "pattern"}}, CalibMode::Calib_PA_Pattern},
             {{{"test", "flow_ratio"}, {"method", "pass2"}}, CalibMode::Calib_Flow_Rate},
             {{{"test", "retraction"}}, CalibMode::Calib_Retraction_tower},
             {{{"test", "vfa"}}, CalibMode::Calib_VFA_Tower},
             {{{"test", "input_shaping_freq"}}, CalibMode::Calib_Input_shaping_freq},
             {{{"test", "input_shaping_damp"}}, CalibMode::Calib_Input_shaping_damp},
             {{{"test", "cornering"}}, CalibMode::Calib_Cornering}})
        CHECK(calib_args(args).mode == mode);
    CHECK_THROWS_AS(calib_args({{"test", "pressure_advance"}, {"method", "yolo"}}), ToolFailure);
    CHECK_THROWS_AS(calib_args({{"test", "flow_ratio"}, {"method", "tower"}}), ToolFailure);
}

TEST_CASE("omitted calibration_generate_test parameters take the dialog defaults", "[Mcp]")
{
    const Calib_Params temp = calib_args({{"test", "temperature"}});
    CHECK((temp.start == 230 && temp.end == 190 && temp.nozzle_based_resize));
    const Calib_Params tower = calib_args({{"test", "pressure_advance"}});
    CHECK((tower.start == 0 && tower.end == 0.1 && tower.step == 0.002 && !tower.print_numbers));
    CHECK(calib_args({{"test", "pressure_advance"}, {"method", "line"}}).print_numbers);
    const Calib_Params pattern = calib_args({{"test", "pressure_advance"}, {"method", "pattern"}});
    CHECK((pattern.end == 0.08 && pattern.step == 0.005 && pattern.accelerations.empty() && pattern.speeds.empty()));
    // The dialog's Extruder type radio only switches these defaults; the printer's extruder type picks them here.
    const DynamicPrintConfig bowden = calib_printer(gcfKlipper, etBowden);
    const Calib_Params       bowden_tower = calib_args({{"test", "pressure_advance"}}, bowden);
    CHECK((bowden_tower.end == 1.0 && bowden_tower.step == 0.02));
    CHECK(calib_args({{"test", "pressure_advance"}, {"method", "pattern"}}, bowden).step == 0.05);
    const Calib_Params vol = calib_args({{"test", "max_volumetric"}});
    CHECK((vol.start == 5 && vol.end == 20 && vol.step == 0.5));
    const Calib_Params retraction = calib_args({{"test", "retraction"}});
    CHECK((retraction.start == 0 && retraction.end == 2 && retraction.step == 0.1));
    const Calib_Params vfa = calib_args({{"test", "vfa"}});
    CHECK((vfa.start == 40 && vfa.end == 200 && vfa.step == 10));
    const Calib_Params freq = calib_args({{"test", "input_shaping_freq"}});
    CHECK((freq.freqStartX == 15 && freq.freqEndX == 110 && freq.start == 0.15 && freq.test_model == 0));
    CHECK(freq.shaper_type == "Default"); // the first of the radio's types on Klipper
    const Calib_Params damp = calib_args({{"test", "input_shaping_damp"}});
    CHECK((damp.freqStartX == 30 && damp.freqStartY == 30 && damp.start == 0 && damp.end == 0.4));
    // Cornering is junction deviation on Marlin 2 with it set, classic jerk otherwise.
    const Calib_Params jerk = calib_args({{"test", "cornering"}}, calib_printer(gcfMarlinFirmware));
    CHECK((jerk.start == 1 && jerk.end == 15));
    const Calib_Params jd = calib_args({{"test", "cornering"}}, calib_printer(gcfMarlinFirmware, etDirectDrive, 0.1));
    CHECK((jd.start == 0 && jd.end == 0.25));
    CHECK(calib_args({{"test", "cornering"}}, calib_printer(gcfKlipper, etDirectDrive, 0.1)).end == 15);
}

TEST_CASE("calibration_generate_test refuses arguments its test does not take", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(calib_args({{"test", "pressure_advance"}, {"speeds", {100}}}),
                      ContainsSubstring("speeds does not apply to pressure_advance with method tower"));
    CHECK_THROWS_WITH(calib_args({{"test", "temperature"}, {"step", 5}}), ContainsSubstring("step does not apply to temperature"));
    CHECK_THROWS_AS(calib_args({{"test", "cornering"}, {"step", 0.01}}), ToolFailure);
    CHECK_THROWS_AS(calib_args({{"test", "vfa"}, {"damping", 0.1}}), ToolFailure);
    CHECK_THROWS_AS(calib_args({{"test", "pressure_advance"}, {"method", "line"}, {"accelerations", {1000}}}), ToolFailure);
    CHECK_THROWS_AS(calib_args({{"test", "flow_ratio"}, {"start", 1}}), ToolFailure);
    // The model picker of the temperature dialog shows on belt printers only.
    CHECK_THROWS_WITH(calib_args({{"test", "temperature"}, {"test_model", "overhang"}}), ContainsSubstring("belt printers"));
    CHECK_THROWS_AS(calib_args({{"test", "input_shaping_freq"}, {"test_model", "scv_v2"}}), ToolFailure);
    CHECK(calib_args({{"test", "cornering"}, {"test_model", "scv_v2"}}).test_model == 2);
    // The firmware's input shaper types, as the dialog's radio lists them.
    CHECK_THROWS_WITH(calib_args({{"test", "input_shaping_freq"}, {"shaper_type", "EI3"}}),
                      ContainsSubstring("klipper firmware: Default, ZV, MZV, ZVD, EI, 2HUMP_EI, 3HUMP_EI"));
    CHECK(calib_args({{"test", "input_shaping_freq"}, {"shaper_type", "EI3"}}, calib_printer(gcfRepRapFirmware)).shaper_type == "EI3");
    CHECK(calib_args({{"test", "input_shaping_damp"}}, calib_printer(gcfMarlinFirmware)).shaper_type == "ZV");
    // The values the call needs besides the test's own are not refused.
    CHECK_NOTHROW(calib_args({{"test", "retraction"}, {"discard_changes", true}, {"preset_changes", "keep"}}));
    // Every test prints with one filament, so each takes the slot.
    CHECK_NOTHROW(calib_args({{"test", "flow_ratio"}, {"filament_slot", 2}}));
    CHECK_NOTHROW(calib_args({{"test", "cornering"}, {"filament_slot", 2}}));
}

TEST_CASE("the input shaping Y range defaults to the X range, and RepRapFirmware takes one range", "[Mcp]")
{
    const Calib_Params freq = calib_args({{"test", "input_shaping_freq"}, {"start", 20}, {"end", 80}});
    CHECK((freq.freqStartY == 20 && freq.freqEndY == 80));
    const Calib_Params y = calib_args({{"test", "input_shaping_freq"}, {"start", 20}, {"end", 80}, {"start_y", 30}, {"end_y", 90}});
    CHECK((y.freqStartX == 20 && y.freqStartY == 30 && y.freqEndY == 90));
    CHECK(calib_args({{"test", "input_shaping_damp"}, {"freq_x", 48}}).freqStartY == 48);
    const DynamicPrintConfig rrf = calib_printer(gcfRepRapFirmware);
    CHECK_THROWS_AS(calib_args({{"test", "input_shaping_freq"}, {"start_y", 30}}, rrf), ToolFailure);
    CHECK_THROWS_AS(calib_args({{"test", "input_shaping_damp"}, {"freq_y", 30}}, rrf), ToolFailure);
    // The dialog checks only the X range there; a Y range from another firmware's defaults does not count.
    Calib_Params p = calib_default_params(CalibMode::Calib_Input_shaping_freq, rrf);
    p.freqStartY   = 600;
    CHECK(check_calib_params(p, rrf) == CalibParamsError::None);
    CHECK(check_calib_params(p, calib_printer()) == CalibParamsError::Range);
}

TEST_CASE("calibration_generate_test clamps temperatures to 155-500 C, rounded down to 5 C steps, as the dialog does", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    std::vector<std::string> warnings;
    const Calib_Params       p = calib_params_from_args({{"test", "temperature"}, {"start", 252}, {"end", 232}}, calib_printer(), warnings);
    CHECK((p.start == 250 && p.end == 230));
    REQUIRE(warnings.size() == 2);
    CHECK_THAT(warnings[0], ContainsSubstring("rounded down to 250"));
    warnings.clear();
    const Calib_Params clamped = calib_params_from_args({{"test", "temperature"}, {"start", 600}, {"end", 150}}, calib_printer(), warnings);
    CHECK((clamped.start == 500 && clamped.end == 155));
    REQUIRE(warnings.size() == 2);
    // The dialog's message box says 170 but clamps to 155.
    CHECK_THAT(warnings[0], ContainsSubstring("155-500 °C; clamped to 500"));
    CHECK_THAT(warnings[1], ContainsSubstring("clamped to 155"));
    CHECK_THROWS_WITH(calib_args({{"test", "temperature"}, {"start", 230}, {"end", 230}}), ContainsSubstring("start at least end + 5"));
}

TEST_CASE("the calibration dialogs' rules refuse what the dialogs refuse", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    const DynamicPrintConfig printer = calib_printer();
    const auto check = [&printer](CalibMode mode, double start, double end, double step) {
        Calib_Params p = calib_default_params(mode, printer);
        p.start        = start;
        p.end          = end;
        p.step         = step;
        return check_calib_params(p, printer);
    };
    using E = CalibParamsError;
    CHECK(check(CalibMode::Calib_Temp_Tower, 500, 155, 0) == E::None);
    CHECK(check(CalibMode::Calib_Temp_Tower, 505, 230, 0) == E::Range);
    CHECK(check(CalibMode::Calib_Temp_Tower, 230, 150, 0) == E::Range);
    CHECK(check(CalibMode::Calib_Temp_Tower, 230, 226, 0) == E::Range);
    CHECK(check(CalibMode::Calib_Vol_speed_Tower, 0, 20, 0.5) == E::Range);
    CHECK(check(CalibMode::Calib_Vol_speed_Tower, 5, 5.4, 0.5) == E::Range);
    CHECK(check(CalibMode::Calib_Vol_speed_Tower, 5, 5.5, 0.5) == E::None);
    CHECK(check(CalibMode::Calib_PA_Tower, -0.01, 0.1, 0.002) == E::Range);
    CHECK(check(CalibMode::Calib_PA_Tower, 0, 0.1, 0.0009) == E::Range); // "PA step: >= 0.001"
    CHECK(check(CalibMode::Calib_PA_Tower, 0, 0.1, 0.002) == E::None);
    CHECK(check(CalibMode::Calib_Retraction_tower, 0, 2, 0) == E::Range);
    CHECK(check(CalibMode::Calib_Retraction_tower, 0, 2, 0.1) == E::None);
    CHECK(check(CalibMode::Calib_VFA_Tower, 10, 200, 10) == E::Range);
    CHECK(check(CalibMode::Calib_VFA_Tower, 11, 200, 10) == E::None);
    CHECK(check(CalibMode::Calib_Input_shaping_freq, 1, 0, 0) == E::Damping); // damping 1 is not below 1
    CHECK(check(CalibMode::Calib_Input_shaping_damp, 0.3, 0.2, 0) == E::Damping);
    CHECK(check(CalibMode::Calib_Input_shaping_damp, 0, 1, 0) == E::None);

    Calib_Params pattern = calib_default_params(CalibMode::Calib_PA_Pattern, printer);
    pattern.accelerations = {1000, 2000};
    pattern.speeds        = {100, 1000};
    CHECK(check_calib_params(pattern, printer) == E::AccelBelowSpeed);
    pattern.speeds = {100, 200};
    CHECK(check_calib_params(pattern, printer) == E::None);
    DynamicPrintConfig belt = printer;
    belt.set_key_value("belt_printer", new ConfigOptionBool(true));
    CHECK(check_calib_params(pattern, belt) == E::BeltMethod);
    CHECK(check_calib_params(calib_default_params(CalibMode::Calib_PA_Tower, belt), belt) == E::None);

    // Cornering takes junction deviation up to 0.3 mm on Marlin 2 with it set, jerk up to 100 mm/s otherwise, and
    // warns above 0.25 mm or 20 mm/s.
    const DynamicPrintConfig marlin_jd = calib_printer(gcfMarlinFirmware, etDirectDrive, 0.1);
    CHECK((cornering_end_limits(marlin_jd) == std::make_pair(0.3, 0.25)));
    CHECK((cornering_end_limits(printer) == std::make_pair(100., 20.)));
    CHECK_THROWS_WITH(calib_args({{"test", "cornering"}, {"end", 0.31}}, marlin_jd), ContainsSubstring("<= 0.3 mm (junction deviation)"));
    std::vector<std::string> warnings;
    calib_params_from_args({{"test", "cornering"}, {"end", 0.28}}, marlin_jd, warnings);
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings[0], ContainsSubstring("layer shifts"));
    CHECK_NOTHROW(calib_args({{"test", "cornering"}, {"end", 90}}));

    // The tool states the rule with the values given.
    CHECK_THROWS_WITH(calib_args({{"test", "pressure_advance"}, {"start", 0.01}, {"end", 0.05}, {"step", 0.051}}),
                      ContainsSubstring("needs start >= 0, step >= 0.001 and end >= start + step; got start 0.01, end 0.05, step 0.051"));
    CHECK_THROWS_WITH(calib_args({{"test", "pressure_advance"}, {"method", "pattern"}, {"accelerations", {100}}, {"speeds", {200}}}),
                      ContainsSubstring("greater than every speed"));
    CHECK_THROWS_WITH(calib_args({{"test", "pressure_advance"}, {"method", "line"}}, belt), ContainsSubstring("use method tower"));
}

TEST_CASE("VFA's auto-adjust lowers the layer height, else the end speed, to stay within the volumetric limit", "[Mcp]")
{
    DynamicPrintConfig printer = calib_printer();
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
    DynamicPrintConfig print = DynamicPrintConfig::full_print_config();
    print.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    DynamicPrintConfig filament = DynamicPrintConfig::full_print_config();
    filament.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.});
    // At 0.42 mm wide, 12 mm³/s allows about 159 mm/s at 0.2 mm, 195 at 0.16 and 254 at 0.12.
    const std::vector<double> candidates{0.12, 0.16, 0.2, 0.28};
    Calib_Params p = calib_default_params(CalibMode::Calib_VFA_Tower, printer); // 40 to 200 in steps of 10

    VfaSpeedFit fit = fit_vfa_speed(p, printer, print, filament, true, candidates);
    CHECK(fit.kind == VfaSpeedFit::ReducedLayerHeight);
    CHECK(fit.layer_height == 0.12); // the largest that reaches 200, never above the default 0.2
    CHECK((fit.default_lh == 0.2 && fit.speed_limit > 155 && fit.speed_limit < 165));

    p.end = 400;
    fit   = fit_vfa_speed(p, printer, print, filament, true, candidates);
    CHECK(fit.kind == VfaSpeedFit::LoweredEnd);
    CHECK((fit.layer_height == 0.12 && fit.end == 250)); // a step multiple the smallest one reaches

    CHECK(fit_vfa_speed(p, printer, print, filament, false, candidates).kind == VfaSpeedFit::Clamped);
    CHECK(fit_vfa_speed(p, printer, print, filament, true, {}).kind == VfaSpeedFit::Clamped);
    p.nozzle_based_resize = false; // the layer height of an unscaled model stays the process's
    CHECK(fit_vfa_speed(p, printer, print, filament, true, candidates).kind == VfaSpeedFit::Clamped);
    p.end = 150;
    CHECK(fit_vfa_speed(p, printer, print, filament, true, candidates).kind == VfaSpeedFit::Fits);
}

TEST_CASE("calibration_generate_test records the parameters a test ran with, defaults included", "[Mcp]")
{
    const DynamicPrintConfig printer = calib_printer();
    const json tower = {{"test", "pressure_advance"}, {"end", 0.06}};
    CHECK(calib_params_json(tower, calib_args(tower), printer) == json{{"method", "tower"}, {"start", 0.}, {"end", 0.06}, {"step", 0.002}});
    const json line = {{"test", "pressure_advance"}, {"method", "line"}, {"print_numbers", false}};
    CHECK(calib_params_json(line, calib_args(line), printer)["print_numbers"] == false);
    const json pattern = {{"test", "pressure_advance"}, {"method", "pattern"}, {"accelerations", {1000, 2000}}, {"speeds", {50}}};
    const json recorded = calib_params_json(pattern, calib_args(pattern), printer);
    CHECK((recorded["accelerations"] == json{1000., 2000.} && recorded["speeds"] == json::array({50.})));
    const json flow = {{"test", "flow_ratio"}};
    CHECK(calib_params_json(flow, calib_args(flow), printer) == json{{"method", "yolo"}, {"top_surface_pattern", "archimedeanchords"}});
    const json freq = {{"test", "input_shaping_freq"}, {"start", 20}};
    CHECK(calib_params_json(freq, calib_args(freq), printer) ==
          json{{"start", 20.}, {"end", 110.}, {"start_y", 20.}, {"end_y", 110.}, {"damping", 0.15}, {"shaper_type", "Default"},
               {"test_model", "ringing_tower"}});
    const json rrf_damp = {{"test", "input_shaping_damp"}, {"freq_x", 40}};
    const DynamicPrintConfig rrf = calib_printer(gcfRepRapFirmware);
    CHECK_FALSE(calib_params_json(rrf_damp, calib_args(rrf_damp, rrf), rrf).contains("freq_y"));
    const json temp = {{"test", "temperature"}, {"start", 252}};
    CHECK(calib_params_json(temp, calib_args(temp), printer) == json{{"start", 250.}, {"end", 190.}, {"auto_scale", true}});
    const json vfa = {{"test", "vfa"}, {"auto_adjust", false}};
    CHECK(calib_params_json(vfa, calib_args(vfa), printer)["auto_adjust"] == false);
}

TEST_CASE("filament_sync_ams takes the sync dialog's choice, map or overwrite, and called with none opens the dialog", "[Mcp]")
{
    const NativeCommand* sync = NativeCommands::find("filament_sync_ams");
    REQUIRE(sync != nullptr);
    std::set<std::string> modes;
    for (const json& mode : sync->input_schema.at("properties").at("mode").at("enum"))
        modes.insert(mode.get<std::string>());
    CHECK(modes == std::set<std::string>{"map", "overwrite"});
    for (const char* mode : {"map", "overwrite"}) {
        INFO(mode);
        json args{{"mode", mode}};
        CHECK(argument_error(*sync, args).empty());
        CHECK_FALSE(Mcp::person_form(*sync, args));
    }
    json other{{"mode", "merge"}};
    CHECK_THAT(argument_error(*sync, other), Catch::Matchers::ContainsSubstring("mode"));
    // Its Speed Dial action's call, with no arguments, is the person's form, which opens the sync dialog.
    CHECK(Mcp::person_form(*sync, json::object()));
}
