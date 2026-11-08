// Automation action tests. Owner: work package S4 (object settings). Tag: [actions].
//
// The pure rules behind object.getSettings / object.setSettings / part.* / layer.*: which keys may
// be overridden at which level, value parsing and range checks, and height-range placement. The
// descriptors themselves are checked by test_action_catalog.cpp (ids, schema dialect, examples).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/Actions/ObjectSettingsRules.hpp"
#include "slic3r/GUI/WebAppBridge.hpp"
#include "slic3r/GUI/WebAppSettings.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Actions::ObjectSettings;

namespace {
bool has(const std::vector<std::string>& v, const std::string& k) { return std::find(v.begin(), v.end(), k) != v.end(); }
} // namespace

TEST_CASE("Object settings: the keys each level offers", "[actions][objectsettings]")
{
    const auto& object = level_keys(Level::Object);
    const auto& part   = level_keys(Level::Part);
    const auto& range  = level_keys(Level::LayerRange);

    CHECK(std::is_sorted(object.begin(), object.end()));
    // Region options everywhere; object options on objects only; a range adds its own two.
    for (const char* k : {"wall_loops", "sparse_infill_density", "top_shell_layers", "outer_wall_speed"}) {
        CHECK(has(object, k));
        CHECK(has(part, k));
        CHECK(has(range, k));
    }
    for (const char* k : {"enable_support", "brim_type", "flush_into_infill", "flush_into_objects", "flush_into_support", "layer_height"}) {
        CHECK(has(object, k));
        CHECK_FALSE(has(part, k));
    }
    CHECK(has(range, "layer_height"));
    CHECK(has(range, "extruder"));
    CHECK_FALSE(has(range, "enable_support"));
    // A solid part can carry its own build direction (a directed part); a range cannot.
    for (const char* k : {"layer_rotation_b", "layer_rotation_c"}) {
        CHECK(has(object, k));
        CHECK(has(part, k));
        CHECK_FALSE(has(range, k));
        CHECK(check_key(k, Level::Part, 1, true).ok);
        CHECK_FALSE(check_key(k, Level::LayerRange, 1, true).ok);
    }
    // Printer and filament options are nowhere.
    for (const char* k : {"nozzle_diameter", "nozzle_temperature", "bed_temperature_difference", "machine_start_gcode"}) {
        CHECK_FALSE(has(object, k));
        CHECK_FALSE(has(part, k));
    }
}

TEST_CASE("Object settings: per-key verdicts", "[actions][objectsettings]")
{
    SECTION("ordinary per-object options are allowed")
    {
        CHECK(check_key("wall_loops", Level::Object, 1, true).ok);
        CHECK(check_key("sparse_infill_density", Level::Part, 1, true).ok);
        CHECK(check_key("sparse_infill_pattern", Level::LayerRange, 1, true).ok);
        CHECK(check_key("enable_support", Level::Object, 1, true).ok);
        CHECK(check_key("flush_into_infill", Level::Object, 1, true).ok);
        CHECK(check_key("layer_height", Level::Object, 1, true).ok);
    }
    SECTION("unknown option: 1008")
    {
        const KeyVerdict v = check_key("no_such_option_xyz", Level::Object, 1, true);
        CHECK_FALSE(v.ok);
        CHECK(v.code == WAB_NOT_FOUND);
    }
    SECTION("deny-listed options: 1003, whatever the level")
    {
        for (const char* k : {"post_process", "print_host", "printhost_apikey", "inherits", "print_settings_id"}) {
            const KeyVerdict v = check_key(k, Level::Object, 4, true);
            CHECK_FALSE(v.ok);
            CHECK(v.code == WAB_FORBIDDEN_ORIGIN);
            CHECK_FALSE(v.why.empty());
        }
    }
    SECTION("custom G-code: a printer preset option, never per object, but writable on the preset")
    {
        // feat/bridge-gcode-write: the side panel converts a preset to the firmware it runs
        const KeyVerdict v = check_key("machine_start_gcode", Level::Object, 1, true);
        CHECK_FALSE(v.ok);
        std::string why;
        CHECK(WebAppSettings::key_writable("machine_start_gcode", why));
        CHECK(WebAppSettings::key_writable("machine_end_gcode", why));
        CHECK_FALSE(WebAppSettings::key_writable("post_process", why));
        CHECK_FALSE(why.empty());
    }
    SECTION("object-wide options on a part or range: 1002 with a hint")
    {
        for (Level level : {Level::Part, Level::LayerRange}) {
            const KeyVerdict v = check_key("enable_support", level, 1, true);
            CHECK_FALSE(v.ok);
            CHECK(v.code == WAB_BAD_PARAMS);
            CHECK(v.why.find("object-wide") != std::string::npos);
        }
    }
    SECTION("printer or filament options: 1002")
    {
        const KeyVerdict v = check_key("nozzle_temperature", Level::Object, 1, true);
        CHECK_FALSE(v.ok);
        CHECK(v.code == WAB_BAD_PARAMS);
    }
    SECTION("the filament is not a setting on objects and parts, but is one on ranges")
    {
        CHECK_FALSE(check_key("extruder", Level::Object, 4, true).ok);
        CHECK_FALSE(check_key("extruder", Level::Part, 4, true).ok);
        CHECK(check_key("extruder", Level::LayerRange, 1, true).ok);
        CHECK(check_key("layer_height", Level::LayerRange, 1, true).ok);
    }
    SECTION("reading is looser than writing only for the deny lists")
    {
        CHECK(check_key("wall_loops", Level::Object, 1, false).ok);
        CHECK(check_key("print_host", Level::Object, 1, false).code == WAB_FORBIDDEN_ORIGIN);
    }
}

TEST_CASE("Object settings: the allowed list agrees with check_key", "[actions][objectsettings]")
{
    for (Level level : {Level::Object, Level::Part, Level::LayerRange}) {
        const std::vector<std::string> keys = allowed_keys(level, 2);
        CHECK(!keys.empty());
        for (const std::string& k : keys)
            CHECK(check_key(k, level, 2, true).ok);
    }
    const std::vector<std::string> infill = allowed_keys(Level::Part, 1, "INFILL");
    CHECK(has(infill, "sparse_infill_density"));
    CHECK_FALSE(has(infill, "wall_loops"));
    const std::vector<std::string> support = allowed_keys(Level::Object, 1, "", "Support");
    CHECK(has(support, "enable_support"));
    CHECK_FALSE(has(support, "wall_loops"));
    CHECK(allowed_keys(Level::Object, 1).size() > allowed_keys(Level::Part, 1).size());
}

TEST_CASE("Object settings: values are parsed and range-checked", "[actions][objectsettings]")
{
    DynamicPrintConfig cfg;
    CHECK(set_value(cfg, "wall_loops", "4").empty());
    CHECK(cfg.opt_int("wall_loops") == 4);
    CHECK(set_value(cfg, "sparse_infill_density", "35%").empty());
    CHECK(cfg.opt_serialize("sparse_infill_density") == "35%");
    CHECK(set_value(cfg, "sparse_infill_pattern", "gyroid").empty());
    CHECK(set_value(cfg, "enable_support", "1").empty());
    CHECK(cfg.opt_bool("enable_support"));

    SECTION("an unparseable value leaves the config as it was")
    {
        CHECK_FALSE(set_value(cfg, "wall_loops", "lots").empty());
        CHECK(cfg.opt_int("wall_loops") == 4);
        CHECK_FALSE(set_value(cfg, "sparse_infill_pattern", "no_such_pattern").empty());
        CHECK(cfg.opt_serialize("sparse_infill_pattern") == "gyroid");
    }
    SECTION("an out-of-range value is refused, and a new key is not left behind")
    {
        const std::string why = set_value(cfg, "wall_loops", "-3");
        CHECK(why.find("out of range") != std::string::npos);
        CHECK(cfg.opt_int("wall_loops") == 4);
        DynamicPrintConfig empty;
        CHECK_FALSE(set_value(empty, "top_shell_layers", "-1").empty());
        CHECK_FALSE(empty.has("top_shell_layers"));
    }
}

TEST_CASE("Object settings: range layer heights follow the printer's limits", "[actions][objectsettings]")
{
    CHECK(check_layer_height(0.2, 0.08, 0.32, 0.4).empty());
    CHECK_FALSE(check_layer_height(0.05, 0.08, 0.32, 0.4).empty());
    CHECK_FALSE(check_layer_height(0.4, 0.08, 0.32, 0.4).empty());
    CHECK_FALSE(check_layer_height(0., 0.08, 0.32, 0.4).empty());
    // max 0 = 75 % of the nozzle
    CHECK(check_layer_height(0.3, 0.08, 0., 0.4).empty());
    CHECK_FALSE(check_layer_height(0.31, 0.08, 0., 0.4).empty());
}

TEST_CASE("Object settings: height range placement", "[actions][objectsettings]")
{
    t_layer_config_ranges ranges;
    CHECK(check_range(0., 2., 20., ranges).empty());
    ranges[{0., 2.}];
    ranges[{10., 15.}];

    CHECK(check_range(2., 5., 20., ranges).empty());     // touching is fine
    CHECK(check_range(15., 30., 20., ranges).empty());   // may run past the top
    CHECK_FALSE(check_range(1., 3., 20., ranges).empty()); // overlaps 0-2
    CHECK_FALSE(check_range(5., 12., 20., ranges).empty()); // overlaps 10-15
    CHECK_FALSE(check_range(3., 3., 20., ranges).empty()); // empty
    CHECK_FALSE(check_range(5., 4., 20., ranges).empty()); // inverted
    CHECK_FALSE(check_range(-1., 1., 20., ranges).empty());
    CHECK_FALSE(check_range(20., 25., 20., ranges).empty()); // starts at the top

    // Moving a range may overlap its own old place.
    const t_layer_height_range old{10., 15.};
    CHECK(check_range(9., 14., 20., ranges, 0.01, &old).empty());
    CHECK_FALSE(check_range(1., 14., 20., ranges, 0.01, &old).empty());

    CHECK(find_range(ranges, 10.00005, 15.) != ranges.end());
    CHECK(find_range(ranges, 10.1, 15.) == ranges.end());
}
