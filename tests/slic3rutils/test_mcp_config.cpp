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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

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
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
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

    e = effective_setting("sparse_infill_density", s.process, *s.object, nullptr);
    CHECK(e["effective"] == "15%");
    CHECK(e["source"] == "process");

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
    CHECK(e["source"] == "process");
    REQUIRE(e["parts"].size() == 1);
    CHECK(e["parts"][0]["volume"] == 1);
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
    CHECK(e["parts"][0]["volume"] == 1);
    CHECK(e["parts"][0]["value"] == "3");

    e = effective_setting("extruder", s.process, *s.object, part);
    CHECK(e["effective"] == "3");
    CHECK(e["source"] == "part");
    e = effective_setting("extruder", s.process, *s.object, modifier);
    CHECK(e["effective"] == "2");
    CHECK(e["source"] == "object");
}

TEST_CASE("save_preset refuses the names the Save preset dialog refuses", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    Processes p;
    CHECK_THROWS_WITH(save_preset_inside_project(p.presets, "a:b", json::object()), ContainsSubstring("illegal characters"));
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "", json::object()), ToolFailure);
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "Mine ", json::object()), ToolFailure);
    // A system preset is never overwritten.
    CHECK_THROWS_AS(save_preset_inside_project(p.presets, "0.20mm Standard", json{{"overwrite", true}}), ToolFailure);
}

TEST_CASE("save_preset replaces another preset only with overwrite, and saves the selected one again without", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    Processes p;
    CHECK_THROWS_WITH(save_preset_inside_project(p.presets, "Other", json::object()), ContainsSubstring("overwrite"));
    CHECK_FALSE(save_preset_inside_project(p.presets, "Other", json{{"overwrite", true}}));
    // The selected preset's own name is the tab's plain Save.
    CHECK_FALSE(save_preset_inside_project(p.presets, "Mine", json::object()));
}

TEST_CASE("save_preset keeps a preset where the Save preset dialog would", "[Mcp]")
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

TEST_CASE("set_settings writes a single value to the per-variant entries in use, or one entry by index", "[Mcp]")
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

TEST_CASE("delete_preset refuses the presets the tab does not delete", "[Mcp]")
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

    // One slot by index, through set_settings' entry write.
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

TEST_CASE("the preferences scope reads the keys it is given, as stored, and lists none unasked", "[Mcp]")
{
    using Catch::Matchers::ContainsSubstring;
    AppConfig config;
    config.set("auto_calculate_flush", "disabled");
    config.set("region", "");
    config.set("presets", "filament", "Bambu PLA Basic"); // another section than the preferences'
    const json out = preferences_settings(config, json{{"keys", {"auto_calculate_flush", "region", "filament", "no_such_preference"}}});
    REQUIRE(out["total"] == 2);
    CHECK(out["items"][0] == json{{"key", "auto_calculate_flush"}, {"value", "disabled"}});
    // A key stored empty is known, with its empty value.
    CHECK(out["items"][1] == json{{"key", "region"}, {"value", ""}});
    CHECK(out["unknown_keys"] == json{"filament", "no_such_preference"});
    CHECK_THAT(out["note"].get<std::string>(), ContainsSubstring("never changed"));
    const json known = preferences_settings(config, json{{"keys", {"region"}}});
    CHECK_FALSE(known.contains("unknown_keys"));
    CHECK_FALSE(known.contains("note"));

    // The app section also holds account and device state, so nothing is listed without keys.
    CHECK_THROWS_WITH(preferences_settings(config, json::object()), ContainsSubstring("keys"));
    CHECK_THROWS_AS(preferences_settings(config, json{{"query", "flush"}}), ToolFailure);
    CHECK_THROWS_AS(preferences_settings(config, json{{"keys", {"region"}}, {"compare", ":saved"}}), ToolFailure);
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

TEST_CASE("calibrate arguments map to the dialog's calibration modes", "[Mcp]")
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

TEST_CASE("omitted calibrate parameters take the dialog defaults", "[Mcp]")
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

TEST_CASE("calibrate refuses arguments its test does not take", "[Mcp]")
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
    CHECK_NOTHROW(calib_args({{"test", "retraction"}, {"discard_changes", true}, {"changes", "keep"}}));
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

TEST_CASE("calibrate clamps temperatures to 155-500 C and rounds them down to a multiple of 5, as the dialog's fields do", "[Mcp]")
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

TEST_CASE("calibrate records the parameters a test ran with, defaults included", "[Mcp]")
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
