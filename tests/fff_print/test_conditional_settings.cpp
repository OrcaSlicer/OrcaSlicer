#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "catch2/generators/catch_generators.hpp"
#include "catch2/matchers/catch_matchers_string.hpp"
#include "catch2/catch_message.hpp"
#include "libslic3r/ConditionalSettings.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "test_utils.hpp"
#include "test_helpers.hpp"
#include <libslic3r/Config.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/GCodeWriter.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/CustomGCode.hpp>
#include <libslic3r/Semver.hpp>
#include <sstream>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {
DynamicPrintConfig conditional_config()
{
    PresetBundle presets;
    presets.printers.get_edited_preset().set_num_extruders(2);
    presets.set_num_filaments(2);
    presets.project_config.option<ConfigOptionInts>("filament_map")->values = {1, 2};
    auto config = presets.full_config(false);
    config.set_deserialize_strict({{"enable_prime_tower", "0"}, {"z_hop", "0,0"},
        {"filament_z_hop", "0,0"}, {"enable_support", "0"}, {"skirt_loops", "0"},
        {"brim_type", "no_brim"}, {"wall_loops", "2"}, {"layer_height", "0.2"}});
    return config;
}

void rules(DynamicPrintConfig &config, std::initializer_list<ConditionalSettingRule> rows)
{
    std::vector<std::string> serialized;
    for (const auto &row : rows) serialized.push_back(row.serialize());
    config.set_key_value("conditional_settings", new ConfigOptionStrings(serialized));
}

Model model_with_colors(bool multi_color)
{
    Model model;
    auto *object = model.add_object();
    object->add_volume(make_cube(10., 10., 1.));
    object->add_instance();
    if (multi_color) {
        auto *volume = object->add_volume(make_cube(10., 10., 1.));
        volume->set_offset(Vec3d(15., 0., 0.));
        volume->config.set_key_value("extruder", new ConfigOptionInt(2));
    }
    return model;
}
}

TEST_CASE("Conditional settings default empty and preserve legacy configurations", "[ConditionalSettings]")
{
    auto config = conditional_config();
    REQUIRE(config.option<ConfigOptionStrings>("conditional_settings")->values.empty());
    const auto original = config;
    apply_conditional_settings(config, 2);
    REQUIRE(config.diff(original).empty());
    config.erase("conditional_settings");
    const auto legacy = config;
    apply_conditional_settings(config, 2);
    REQUIRE(config.diff(legacy).empty());
    REQUIRE(validate_conditional_settings(config).empty());
}

TEST_CASE("Inactive rules preserve first apply normalization of the base profile", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_deserialize_strict("enable_prime_tower", "1");
    const auto model = model_with_colors(false);
    Print baseline;
    baseline.set_status_silent();
    baseline.apply(model, config);
    ConditionalSettingRule rule{"multi_color", "wall_loops", "4"};
    rule.enabled = GENERATE(true, false);
    rules(config, {rule});
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.config().enable_prime_tower.value == baseline.config().enable_prime_tower.value);
    REQUIRE(print.config().independent_support_layer_height.value ==
            baseline.config().independent_support_layer_height.value);
    REQUIRE(print.default_region_config().wall_loops.value == baseline.default_region_config().wall_loops.value);
}

TEST_CASE("Completed slices retain conditional settings after filament mapping", "[ConditionalSettings][RuleSlicePersistence]")
{
    const bool multi_color = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    auto config = conditional_config();
    config.set_deserialize_strict("filament_retraction_length", "0.2,0.3");
    ConditionalSettingRule rule;
    rule.enabled = enabled;
    rule.conditions = {{"multi_color", "eq", "1"}};
    rule.effects = {{"z_hop", "0.6"}, {"retraction_length", "0.8"}};
    rules(config, {rule});
    auto model = model_with_colors(multi_color);
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    print.apply(model, config);
    const auto expected = print.full_print_config();
    const auto retraction = print.config().retraction_length.serialize();
    REQUIRE_FALSE(Test::gcode(print).empty());
    REQUIRE(print.finished());
    for (const auto *key : {"conditional_settings_matches", "z_hop", "filament_z_hop",
                            "retraction_length", "filament_retraction_length"}) {
        INFO(key);
        const auto *actual = print.full_print_config().option(key);
        CHECK(actual != nullptr);
        if (actual) CHECK(actual->serialize() == expected.option(key)->serialize());
    }
    CHECK(print.config().retraction_length.serialize() == retraction);
    // Mirror the GUI's readback of the engine-selected maps before refreshing settings.
    config.set_key_value("filament_map", new ConfigOptionInts(print.get_filament_maps()));
    config.set_key_value("filament_volume_map", new ConfigOptionInts(print.get_filament_volume_maps()));
    config.set_key_value("filament_nozzle_map", new ConfigOptionInts(print.get_filament_nozzle_maps()));
    CHECK(print.apply(model, config) == Print::APPLY_STATUS_UNCHANGED);
    CHECK(print.finished());
}

TEST_CASE("Blank conditional rules preserve empty selections until completed", "[ConditionalSettings]")
{
    ConditionalSettingRule rule;
    rule.conditions = {{"", "eq", ""}};
    rule.effects = {{"", ""}};
    const auto loaded = ConditionalSettingRule::deserialize(rule.serialize());
    REQUIRE(loaded.predicates().front().setting.empty());
    REQUIRE(loaded.predicates().front().value.empty());
    REQUIRE(loaded.actions().front().target.empty());
    REQUIRE(loaded.actions().front().value.empty());
    REQUIRE(validate_conditional_setting(loaded) == "Choose an If setting.");
    rule.enabled = false;
    REQUIRE(validate_conditional_setting(rule).empty());
}

TEST_CASE("Conditional setting selections read current values and filament overrides", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_deserialize_strict({{"wall_loops", "7"}, {"layer_height", "0.12"},
        {"sparse_infill_density", "17%"}, {"is_infill_first", "1"}, {"wall_sequence", "outer wall/inner wall"},
        {"nozzle_diameter", "0.4,0.6"}, {"retraction_length", "0.6,0.9"},
        {"filament_retraction_length", "nil,1.3"}, {"printer_settings_id", "Current printer"}});
    ConditionalSettingsContext context;
    context.used_filaments = {0, 1};
    REQUIRE(conditional_setting_value(config, context, "wall_loops", 0) == "7");
    REQUIRE(conditional_setting_value(config, context, "layer_height", 0) == "0.12");
    REQUIRE(conditional_setting_value(config, context, "sparse_infill_density", 0) == "17%");
    REQUIRE(conditional_setting_value(config, context, "is_infill_first", 0) == "1");
    REQUIRE(conditional_setting_value(config, context, "enable_support", 0) == "0");
    REQUIRE(conditional_setting_value(config, context, "wall_sequence", 0) == "outer wall/inner wall");
    REQUIRE(conditional_setting_value(config, context, "printer_settings_id", 0) == "Current printer");
    REQUIRE(conditional_setting_value(config, context, "nozzle_diameter", 1) == "0.6");
    REQUIRE(conditional_setting_value(config, context, "retraction_length", 0) == "0.6");
    REQUIRE(conditional_setting_value(config, context, "retraction_length", 1) == "1.3");
    REQUIRE(conditional_setting_value(config, context, "multi_color", 0) == "1");
    config.set_key_value("wall_loops", new ConfigOptionInt(4));
    REQUIRE(conditional_setting_value(config, context, "wall_loops", 0) == "4");
    config.erase("wall_loops");
    REQUIRE(conditional_setting_value(config, context, "wall_loops", 0) == conditional_setting_default("wall_loops"));
}

TEST_CASE("Conditional selection context can exclude other plates and instances", "[ConditionalSettings]")
{
    auto config = conditional_config();
    auto model = model_with_colors(false);
    model.objects.front()->add_instance();
    auto *other = model.add_object();
    other->add_volume(make_cube(10., 10., 8.))->config.set_key_value("extruder", new ConfigOptionInt(2));
    other->config.set_key_value("enable_support", new ConfigOptionBool(true));
    other->add_instance();
    const auto all = conditional_settings_context(model, config);
    REQUIRE(all.object_count == 3);
    REQUIRE_THAT(all.model_height, Catch::Matchers::WithinAbs(8., 1e-6));
    REQUIRE(all.used_filaments == std::vector<unsigned int>{0, 1});
    REQUIRE(all.has_support);
    const auto plate = conditional_settings_context(model, config,
        [](size_t object, size_t instance) { return object == 0 && instance == 0; });
    REQUIRE(plate.object_count == 1);
    REQUIRE_THAT(plate.model_height, Catch::Matchers::WithinAbs(1., 1e-6));
    REQUIRE(plate.used_filaments == std::vector<unsigned int>{0});
    REQUIRE_FALSE(plate.has_support);
    REQUIRE(conditional_setting_value(config, plate, "multi_color", 0) == "0");
    REQUIRE(conditional_setting_value(config, plate, "object_count", 0) == "1");
    const auto empty = conditional_settings_context(model, config, [](size_t, size_t) { return false; });
    REQUIRE(empty.object_count == 0);
    REQUIRE(empty.used_filaments.empty());
}

TEST_CASE("Conditional settings use original conditions and last matching values", "[ConditionalSettings]")
{
    auto base = conditional_config();
    rules(base, {{"multi_color", "enable_support", "1"}, {"enable_support", "wall_loops", "5"},
        {"multi_color", "z_hop", "0.6"}, {"multi_color", "z_hop", "0.8"}});
    auto single = base;
    apply_conditional_settings(single, 1);
    REQUIRE(single.diff(base).empty());
    auto multi = base;
    apply_conditional_settings(multi, 2);
    REQUIRE(multi.opt_bool("enable_support"));
    REQUIRE(multi.opt_int("wall_loops") == 2);
    REQUIRE(multi.option<ConfigOptionFloats>("z_hop")->values == std::vector<double>{0.8, 0.8});
    REQUIRE(multi.option<ConfigOptionFloatsNullable>("filament_z_hop")->values == std::vector<double>{0.8, 0.8});
    REQUIRE(base.option<ConfigOptionFloats>("z_hop")->values == std::vector<double>{0., 0.});
    REQUIRE(base.opt_bool("enable_support") == false);
}

TEST_CASE("Conditional rule validation rejects malformed versions and unsafe values", "[ConditionalSettings]")
{
    const ConditionalSettingRule invalid = GENERATE(
        ConditionalSettingRule{"unknown", "z_hop", "0.4"},
        ConditionalSettingRule{"multi_color", "machine_start_gcode", "text"},
        ConditionalSettingRule{"multi_color", "conditional_settings", "text"},
        ConditionalSettingRule{"multi_color", "z_hop", "-1"},
        ConditionalSettingRule{"multi_color", "z_hop", "6"},
        ConditionalSettingRule{"multi_color", "z_hop", "0.4garbage"},
        ConditionalSettingRule{"multi_color", "z_hop", "nan"},
        ConditionalSettingRule{"multi_color", "z_hop", "1,2"},
        ConditionalSettingRule{"multi_color", "z_hop", "40%"},
        ConditionalSettingRule{"multi_color", "layer_height", "0"},
        ConditionalSettingRule{"multi_color", "initial_layer_print_height", "0"},
        ConditionalSettingRule{"multi_color", "bridge_flow", "0"},
        ConditionalSettingRule{"multi_color", "wall_loops", "2.5"},
        ConditionalSettingRule{"multi_color", "wall_loops", "2e2"},
        ConditionalSettingRule{"multi_color", "wall_loops", "4294967296"},
        ConditionalSettingRule{"multi_color", "enable_support", "yes"},
        ConditionalSettingRule{"multi_color", "wall_generator", "unknown"});
    REQUIRE_FALSE(validate_conditional_setting(invalid).empty());
    auto config = conditional_config();
    rules(config, {invalid});
    REQUIRE_FALSE(validate_conditional_settings(config).empty());
    REQUIRE(config.validate(false).count("conditional_settings") == 1);
    REQUIRE_THROWS_AS(apply_conditional_settings(config, 1), ConfigurationError);
    REQUIRE_THROWS_AS(ConditionalSettingRule::deserialize("2\tmulti_color\tz_hop\t0.4"), ConfigurationError);
    REQUIRE_THROWS_AS(ConditionalSettingRule::deserialize("1\tmulti_color\tz_hop"), ConfigurationError);
}

TEST_CASE("Conditional settings support typed process values and profile serialization", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {{"multi_color", "wall_loops", "4"}, {"multi_color", "sparse_infill_density", "25%"},
        {"multi_color", "wall_generator", "classic"}, {"multi_color", "spiral_mode_max_xy_smoothing", "50%"}});
    REQUIRE(validate_conditional_settings(config).empty());
    DynamicPrintConfig reloaded;
    reloaded.set_deserialize_strict("conditional_settings", config.option("conditional_settings")->serialize());
    REQUIRE(reloaded.option("conditional_settings")->serialize() == config.option("conditional_settings")->serialize());
    apply_conditional_settings(config, 2);
    REQUIRE(config.opt_int("wall_loops") == 4);
    REQUIRE_THAT(config.option<ConfigOptionPercent>("sparse_infill_density")->value, Catch::Matchers::WithinAbs(25., 1e-6));
    REQUIRE(config.option("wall_generator")->serialize() == "classic");
    REQUIRE(config.option<ConfigOptionFloatOrPercent>("spiral_mode_max_xy_smoothing")->percent);
}

TEST_CASE("False boolean conditions retain custom base values", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_deserialize_strict("z_hop", "0.2,0.3");
    rules(config, {{"enable_support", "z_hop", "0.8"}});
    auto disabled = config;
    apply_conditional_settings(disabled, 2);
    REQUIRE(disabled.option<ConfigOptionFloats>("z_hop")->values == std::vector<double>{0.2, 0.3});
    config.set_deserialize_strict("enable_support", "1");
    apply_conditional_settings(config, 1);
    REQUIRE(config.option<ConfigOptionFloats>("z_hop")->values == std::vector<double>{0.8, 0.8});
}

TEST_CASE("Print apply evaluates current model colors on first apply and restores base values", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {{"multi_color", "z_hop", "0.6"}, {"multi_color", "wall_loops", "4"}});
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    auto model = model_with_colors(true);
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    REQUIRE(print.default_region_config().wall_loops.value == 4);
    GCodeWriter writer;
    writer.apply_print_config(print.config());
    writer.set_extruders({0, 1});
    writer.set_extruder(0);
    writer.travel_to_z(10.);
    REQUIRE_THAT(writer.eager_lift(LiftType::NormalLift), Catch::Matchers::ContainsSubstring("Z10.6"));
    REQUIRE(print.apply(model, config) == Print::APPLY_STATUS_UNCHANGED);
    model.objects.front()->volumes.back()->config.set_key_value("extruder", new ConfigOptionInt(1));
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE(print.default_region_config().wall_loops.value == 2);
    model.objects.front()->volumes.back()->config.set_key_value("extruder", new ConfigOptionInt(2));
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    rules(config, {});
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Conditional settings consider support, height ranges and current plate tool changes", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {{"multi_color", "z_hop", "0.5"}});
    auto model = model_with_colors(false);
    auto check = [&] {
        Print print;
        print.is_BBL_printer() = false;
        print.set_plate_origin(Vec3d::Zero());
        print.set_status_silent();
        print.apply(model, config);
        REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.5, 1e-6));
    };
    SECTION("Support material") {
        config.set_deserialize_strict({{"enable_support", "1"}, {"support_filament", "2"}});
        check();
    }
    SECTION("Height range") {
        ModelConfig range;
        range.set_key_value("extruder", new ConfigOptionInt(2));
        model.objects.front()->layer_config_ranges[{0.4, 0.8}] = range;
        check();
    }
    SECTION("Tool change on current plate") {
        CustomGCode::Item change;
        change.type = CustomGCode::Type::ToolChange;
        change.extruder = 2;
        change.print_z = 0.4;
        model.plates_custom_gcodes[model.curr_plate_index].gcodes.push_back(change);
        check();
    }
}

TEST_CASE("Invalid conditional rows block slicing through normal validation", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("conditional_settings", new ConfigOptionStrings{"unsupported rule"});
    auto model = model_with_colors(true);
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    REQUIRE_NOTHROW(print.apply(model, config));
    REQUIRE_THAT(print.validate().string, Catch::Matchers::ContainsSubstring("If / Then rule 1"));
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Empty and false conditional rules preserve existing G-code moves", "[ConditionalSettings]")
{
    auto config = conditional_config();
    const auto baseline = Test::slice({make_cube(10., 10., 1.)}, config);
    rules(config, {{"multi_color", "z_hop", "0.6"}, {"multi_color", "wall_loops", "5"}});
    const auto unchanged = Test::slice({make_cube(10., 10., 1.)}, config);
    auto moves = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line, result;
        while (std::getline(input, line))
            if (line.rfind("G1 ", 0) == 0) result += line + '\n';
        return result;
    };
    REQUIRE_FALSE(moves(baseline).empty());
    REQUIRE(moves(baseline) == moves(unchanged));
}

TEST_CASE("Conditional colors include painting and exclude unprintable objects", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {{"multi_color", "z_hop", "0.7"}});
    auto model = model_with_colors(false);
    auto *volume = model.objects.front()->volumes.front();
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.7, 1e-6));
    volume->mmu_segmentation_facets.reset();
    auto *unused = model.add_object();
    unused->add_volume(make_cube(10., 10., 1.))->config.set_key_value("extruder", new ConfigOptionInt(2));
    unused->add_instance()->printable = false;
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Conditional triggers count an enabled later copy while excluding disabled copies", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {{"multi_color", "z_hop", "0.7"}});
    auto model = model_with_colors(false);
    auto *other = model.add_object();
    other->add_volume(make_cube(10., 10., 1.))->config.set_key_value("extruder", new ConfigOptionInt(2));
    other->add_instance()->printable = false;
    auto *later = other->add_instance();
    later->printable = false;
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    later->printable = true;
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.7, 1e-6));
    REQUIRE(conditional_settings_context(model, config).used_filaments == std::vector<unsigned int>{0, 1});
    later->printable = false;
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Conditional colors expand mixed slots into their printable filament components", "[ConditionalSettings]")
{
    PresetBundle presets;
    presets.printers.get_edited_preset().set_num_extruders(2);
    presets.set_num_filaments(3);
    presets.project_config.option<ConfigOptionInts>("filament_map")->values = {1, 2, 1};
    auto config = presets.full_config(false);
    config.set_deserialize_strict({{"filament_is_mixed", "0,0,1"},
        {"filament_mixed_components", ";;1,2"}, {"z_hop", "0,0"}});
    rules(config, {{"multi_color", "z_hop", "0.7"}});
    auto model = model_with_colors(false);
    model.objects.front()->volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(3));
    const auto context = conditional_settings_context(model, config);
    REQUIRE(context.used_filaments == std::vector<unsigned int>{0, 1});
    REQUIRE(conditional_setting_value(config, context, "multi_color", 0) == "1");
    REQUIRE(conditional_setting_value(config, context, "color_count", 0) == "2");
    const auto original = config;
    apply_conditional_settings(config, original, context, ConditionalSettingsStage::All);
    REQUIRE_THAT(config.option<ConfigOptionFloats>("z_hop")->values.front(), Catch::Matchers::WithinAbs(0.7, 1e-6));
}

TEST_CASE("Projects round trip conditional rules without overwriting base settings", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {{"multi_color", "z_hop", "0.6"}, {"enable_support", "wall_loops", "4"}});
    auto model = model_with_colors(true);
    ScopedTemporaryFile file(".3mf");
    StoreParams params;
    const std::string path = file.string();
    params.path = path.c_str();
    params.model = &model;
    params.config = &config;
    params.strategy = SaveStrategy::SplitModel | SaveStrategy::Silence | SaveStrategy::ShareMesh;
    REQUIRE(store_bbs_3mf(params));
    Model reloaded;
    DynamicPrintConfig loaded;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool is_bbl = false, is_orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(path.c_str(), &loaded, &substitutions, &reloaded, &plates, &presets,
        &is_bbl, &is_orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    release_PlateData_list(plates);
    for (auto *preset : presets) delete preset;
    REQUIRE(loaded.option("conditional_settings")->serialize() == config.option("conditional_settings")->serialize());
    REQUIRE_THAT(loaded.option<ConfigOptionFloats>("z_hop")->values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    apply_conditional_settings(loaded, 2);
    REQUIRE_THAT(loaded.option<ConfigOptionFloats>("z_hop")->values.front(), Catch::Matchers::WithinAbs(0.6, 1e-6));
}

namespace {
ConditionalSettingRule comparison_rule(std::string target, std::string value,
    std::initializer_list<ConditionalSettingCondition> conditions, bool legacy_scope = false, bool match_all = true)
{
    ConditionalSettingRule rule;
    rule.target = std::move(target);
    rule.value = std::move(value);
    rule.conditions = conditions;
    (void)legacy_scope;
    rule.match_all = match_all;
    return rule;
}
}

TEST_CASE("Expanded conditional rows preserve typed comparisons and reject invalid schemas", "[ConditionalSettings]")
{
    auto rule = comparison_rule("retraction_length", "0.8",
        {{"filament_type", "eq", "PETG"}, {"nozzle_diameter", "ge", "0.4"}}, true);
    const auto decoded = ConditionalSettingRule::deserialize(rule.serialize());
    REQUIRE(decoded.serialize() == rule.serialize());
    REQUIRE(decoded.name.empty());
    REQUIRE(decoded.conditions.size() == 2);
    REQUIRE(validate_conditional_setting(decoded).empty());
    REQUIRE(ConditionalSettingRule::deserialize("1\tmulti_color\tz_hop\t0.4").serialize() == "1\tmulti_color\tz_hop\t0.4");
    for (const auto &row : {
        R"({"version":3,"conditions":[{}]})",
        R"({"version":4294967298,"enabled":true,"per_filament":false,"match_all":true,"conditions":[{"setting":"multi_color","comparison":"eq","value":"1"}],"target":"z_hop","value":"0.4"})",
        R"({"version":2,"enabled":"true","per_filament":false,"match_all":true,"conditions":[{}],"target":"z_hop","value":"0.4"})",
        R"({"version":2,"enabled":true,"per_filament":false,"match_all":true,"conditions":[],"target":"z_hop","value":"0.4"})"}) {
        REQUIRE_THROWS_AS(ConditionalSettingRule::deserialize(row), ConfigurationError);
    }
    rule.conditions.front().comparison = "gt";
    REQUIRE_FALSE(validate_conditional_setting(rule).empty());
    rule.conditions.front() = {"wall_loops", "eq", "2e2"};
    REQUIRE_FALSE(validate_conditional_setting(rule).empty());
    rule.conditions.front() = {"nozzle_diameter", "ge", "nan"};
    REQUIRE_FALSE(validate_conditional_setting(rule).empty());
    rule.conditions.front() = {"nozzle_diameter", "ge", "0.4garbage"};
    REQUIRE_FALSE(validate_conditional_setting(rule).empty());
    rule.target = "machine_start_gcode";
    REQUIRE_FALSE(validate_conditional_setting(rule).empty());
    rule.enabled = false;
    REQUIRE(validate_conditional_setting(rule).empty());
    REQUIRE_FALSE(ConditionalSettingRule::deserialize(rule.serialize()).enabled);
}

TEST_CASE("Expanded conditions compare numbers enums strings and AND OR using the original configuration", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    rules(config, {
        comparison_rule("travel_speed", "120", {{"layer_height", "le", "0.2"}, {"print_sequence", "eq", "by layer"}}),
        comparison_rule("wall_loops", "4", {{"color_count", "gt", "1"}, {"filament_type", "contains", "PET"}}),
        comparison_rule("enable_support", "1", {{"filament_type", "eq", "ABS"}, {"nozzle_diameter", "ge", "0.4"}}, false, false),
        comparison_rule("sparse_infill_density", "42%", {{"enable_support", "eq", "1"}})
    });
    REQUIRE(validate_conditional_settings(config).empty());
    apply_conditional_settings(config, 2);
    REQUIRE_THAT(config.opt_float_nullable("travel_speed", 0), Catch::Matchers::WithinAbs(120., 1e-6));
    REQUIRE(config.opt_int("wall_loops") == 4);
    REQUIRE(config.opt_bool("enable_support"));
    REQUIRE(config.option<ConfigOptionPercent>("sparse_infill_density")->value != 42.);
}

TEST_CASE("Whole plate negative material conditions exclude any used matching material", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    rules(config, {comparison_rule("wall_loops", "5", {{"filament_type", "ne", "PETG"}})});
    auto both = config;
    apply_conditional_settings(both, 2);
    REQUIRE(both.opt_int("wall_loops") == 2);
    apply_conditional_settings(config, 1);
    REQUIRE(config.opt_int("wall_loops") == 5);
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "unused PETG"});
    rules(config, {comparison_rule("wall_loops", "6", {{"filament_type", "not_contains", "PET"}})});
    apply_conditional_settings(config, 1);
    REQUIRE(config.opt_int("wall_loops") == 6);
}

TEST_CASE("Material and nozzle conditions trigger plate-wide printer overrides", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    config.set_deserialize_strict("nozzle_diameter", "0.6,0.4");
    config.set_deserialize_strict("filament_retraction_length", "0.2,0.3");
    auto rule = comparison_rule("retraction_length", "0.8",
        {{"filament_type", "eq", "PETG"}, {"nozzle_diameter", "eq", "0.6"}}, true);
    rules(config, {rule});
    auto unchanged = config;
    apply_conditional_settings(unchanged, 2);
    REQUIRE(unchanged.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{0.8, 0.8});
    config.set_deserialize_strict("filament_map", "2,1");
    apply_conditional_settings(config, 2);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{0.8, 0.8});
    REQUIRE(config.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{0.8, 0.8});
}

TEST_CASE("Conditional printer targets handle booleans enums percentages and machine limits", "[ConditionalSettings]")
{
    auto config = conditional_config();
    rules(config, {
        comparison_rule("wipe", "1", {{"multi_color", "eq", "1"}}),
        comparison_rule("retract_when_changing_layer", "0", {{"multi_color", "eq", "1"}}),
        comparison_rule("z_hop_types", "Normal Lift", {{"multi_color", "eq", "1"}}),
        comparison_rule("retract_before_wipe", "50%", {{"multi_color", "eq", "1"}}),
        comparison_rule("retract_length_toolchange", "1.2", {{"multi_color", "eq", "1"}}),
        comparison_rule("machine_max_speed_z", "15", {{"multi_color", "eq", "1"}})
    });
    INFO(validate_conditional_settings(config));
    REQUIRE(validate_conditional_settings(config).empty());
    apply_conditional_settings(config, 2);
    REQUIRE(config.option("wipe")->serialize() == "1,1");
    REQUIRE(config.option("filament_wipe")->serialize() == "1,1");
    REQUIRE(config.option("filament_retract_when_changing_layer")->serialize() == "0,0");
    REQUIRE(config.option("z_hop_types")->serialize() == "Normal Lift,Normal Lift");
    REQUIRE(config.option("filament_z_hop_types")->serialize() == "Normal Lift,Normal Lift");
    REQUIRE(config.option("filament_retract_before_wipe")->serialize() == "50%,50%");
    REQUIRE(config.option<ConfigOptionFloats>("retract_length_toolchange")->values == std::vector<double>{1.2, 1.2});
    REQUIRE_THAT(config.option<ConfigOptionFloats>("machine_max_speed_z")->values.front(), Catch::Matchers::WithinAbs(15., 1e-6));
}

TEST_CASE("Expanded Print apply uses plate dimensions and reports disabled matched and unmatched rules", "[ConditionalSettings]")
{
    auto config = conditional_config();
    auto disabled = comparison_rule("wall_loops", "bad", {{"unknown", "eq", "bad"}});
    disabled.enabled = false;
    rules(config, {
        comparison_rule("travel_speed", "140", {{"model_height", "ge", "1"}, {"object_count", "eq", "2"}}),
        comparison_rule("wall_loops", "5", {{"has_support", "eq", "1"}}),
        disabled
    });
    auto model = model_with_colors(false);
    model.objects.front()->add_instance()->set_offset(Vec3d(20., 0., 0.));
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE_THAT(print.config().travel_speed.get_at(0), Catch::Matchers::WithinAbs(140., 1e-6));
    REQUIRE(print.default_region_config().wall_loops.value == 2);
    const auto *results = print.full_print_config().option<ConfigOptionStrings>("conditional_settings_matches");
    REQUIRE(results);
    REQUIRE(results->size() == 3);
    REQUIRE_THAT(results->values[0], Catch::Matchers::ContainsSubstring(R"("matched":true)"));
    REQUIRE_THAT(results->values[1], Catch::Matchers::ContainsSubstring(R"("matched":false)"));
    REQUIRE_THAT(results->values[2], Catch::Matchers::ContainsSubstring(R"("enabled":false)"));
    REQUIRE(print.apply(model, config) == Print::APPLY_STATUS_UNCHANGED);
    model.objects.front()->instances.back()->printable = false;
    print.apply(model, config);
    REQUIRE(print.config().travel_speed.get_at(0) != 140.);
    config.set_deserialize_strict("enable_support", "1");
    print.apply(model, config);
    REQUIRE(print.default_region_config().wall_loops.value == 5);
}

TEST_CASE("Material conditions change retraction plate-wide and restore all original values when disabled", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    config.set_deserialize_strict("filament_retraction_length", "0.2,0.3");
    config.set_deserialize_strict("filament_wipe", "0,0");
    auto length = comparison_rule("retraction_length", "0.8", {{"filament_type", "eq", "PETG"}}, true);
    auto wipe = comparison_rule("wipe", "1", {{"filament_type", "eq", "PETG"}}, true);
    auto hop_type = comparison_rule("z_hop_types", "Normal Lift", {{"filament_type", "eq", "PETG"}}, true);
    rules(config, {length, wipe, hop_type});
    auto model = model_with_colors(true);
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.config().retraction_length.values == std::vector<double>{0.8, 0.8});
    REQUIRE(print.config().wipe.serialize() == "1,1");
    REQUIRE(print.config().z_hop_types.values.back() == int(ZHopType::zhtNormal));
    REQUIRE(print.apply(model, config) == Print::APPLY_STATUS_UNCHANGED);
    length.enabled = false;
    wipe.enabled = false;
    hop_type.enabled = false;
    rules(config, {length, wipe, hop_type});
    print.apply(model, config);
    REQUIRE(print.config().retraction_length.values == std::vector<double>{0.2, 0.3});
    REQUIRE(print.config().wipe.serialize() == "0,0");
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{0.2, 0.3});
}

TEST_CASE("Rule order overrides the whole plate for each matching condition", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    rules(config, {
        comparison_rule("retraction_length", "0.5", {{"multi_color", "eq", "1"}}),
        comparison_rule("retraction_length", "0.9", {{"filament_type", "eq", "PETG"}}, true),
        comparison_rule("retraction_length", "0.7", {{"filament_type", "eq", "ABS"}}, true)
    });
    apply_conditional_settings(config, 2);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{0.9, 0.9});
}

TEST_CASE("Conditional project serialization preserves disabled named rules and multiple effects", "[ConditionalSettings]")
{
    auto config = conditional_config();
    auto scoped = comparison_rule("retraction_speed", "35",
        {{"filament_settings_id", "contains", "PETG"}, {"toolhead", "eq", "2"}}, true);
    if (GENERATE(false, true)) scoped.effects = {{"retraction_speed", "35"}, {"wipe", "1"}};
    scoped.name = "PETG travel settings";
    auto disabled = scoped;
    disabled.enabled = false;
    rules(config, {scoped, disabled});
    auto model = model_with_colors(true);
    ScopedTemporaryFile file(".3mf");
    StoreParams params;
    const std::string path = file.string();
    params.path = path.c_str();
    params.model = &model;
    params.config = &config;
    params.strategy = SaveStrategy::SplitModel | SaveStrategy::Silence | SaveStrategy::ShareMesh;
    REQUIRE(store_bbs_3mf(params));
    Model reloaded;
    DynamicPrintConfig loaded;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool is_bbl = false, is_orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(path.c_str(), &loaded, &substitutions, &reloaded, &plates, &presets,
        &is_bbl, &is_orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    release_PlateData_list(plates);
    for (auto *preset : presets) delete preset;
    REQUIRE(loaded.option("conditional_settings")->serialize() == config.option("conditional_settings")->serialize());
    REQUIRE(validate_conditional_settings(loaded).empty());
    REQUIRE_FALSE(loaded.has("conditional_settings_matches"));
    REQUIRE(loaded.option("retraction_speed")->serialize() == config.option("retraction_speed")->serialize());
}

TEST_CASE("Conditional numeric comparisons preserve percentage units and reject unsafe printer values", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_deserialize_strict("spiral_mode_max_xy_smoothing", "50%");
    rules(config, {comparison_rule("wall_loops", "4", {{"spiral_mode_max_xy_smoothing", "eq", "50"}})});
    apply_conditional_settings(config, 1);
    REQUIRE(config.opt_int("wall_loops") == 2);
    rules(config, {comparison_rule("wall_loops", "4", {{"spiral_mode_max_xy_smoothing", "ge", "50%"}})});
    apply_conditional_settings(config, 1);
    REQUIRE(config.opt_int("wall_loops") == 4);
    REQUIRE_FALSE(validate_conditional_setting(comparison_rule("retraction_length", "-0.4", {{"multi_color", "eq", "1"}})).empty());
    REQUIRE_FALSE(validate_conditional_setting(comparison_rule("retraction_speed", "-20", {{"multi_color", "eq", "1"}})).empty());
    REQUIRE_FALSE(validate_conditional_setting(comparison_rule("retract_before_wipe", "120%", {{"multi_color", "eq", "1"}})).empty());
    REQUIRE(validate_conditional_setting(comparison_rule("retract_restart_extra", "-0.1", {{"multi_color", "eq", "1"}})).empty());
}

TEST_CASE("A condition can change Z-hop and wall order together and restore both", "[ConditionalSettings]")
{
    auto config = conditional_config();
    auto rule = comparison_rule("", "", {{"multi_color", "eq", "1"}});
    rule.effects = {{"z_hop", "0.6"}, {"wall_sequence", "outer wall/inner wall"}};
    rules(config, {rule});
    REQUIRE(validate_conditional_settings(config).empty());
    const auto decoded = ConditionalSettingRule::deserialize(rule.serialize());
    REQUIRE(decoded.effects.size() == 2);
    REQUIRE(decoded.actions().back().target == "wall_sequence");
    REQUIRE(decoded.serialize() == rule.serialize());
    REQUIRE_THAT(rule.serialize(), Catch::Matchers::ContainsSubstring(R"("version":4)"));
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    auto model = model_with_colors(true);
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    REQUIRE(print.default_region_config().wall_sequence.value == WallSequence::OuterInner);
    REQUIRE(print.apply(model, config) == Print::APPLY_STATUS_UNCHANGED);
    const auto *report = print.full_print_config().option<ConfigOptionStrings>("conditional_settings_matches");
    REQUIRE(report->size() == 1);
    REQUIRE_THAT(report->values.front(), Catch::Matchers::ContainsSubstring(R"("effects":[)"));
    REQUIRE_THAT(report->values.front(), Catch::Matchers::ContainsSubstring(R"("target":"wall_sequence")"));
    model.objects.front()->volumes.back()->config.set_key_value("extruder", new ConfigOptionInt(1));
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE(print.default_region_config().wall_sequence.value == WallSequence::InnerOuter);
    model.objects.front()->volumes.back()->config.set_key_value("extruder", new ConfigOptionInt(2));
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    rule.enabled = false;
    rules(config, {rule});
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE(print.default_region_config().wall_sequence.value == WallSequence::InnerOuter);
    REQUIRE_THAT(config.option<ConfigOptionFloats>("z_hop")->values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE(config.option("wall_sequence")->serialize() == "inner wall/outer wall");
}

TEST_CASE("Legacy conditional rows acquire one effect without changing their serialized data", "[ConditionalSettings]")
{
    const auto legacy = ConditionalSettingRule::deserialize("1\tmulti_color\tz_hop\t0.4");
    REQUIRE(legacy.effects.empty());
    REQUIRE(legacy.actions().size() == 1);
    REQUIRE(legacy.actions().front().target == "z_hop");
    REQUIRE(legacy.serialize() == "1\tmulti_color\tz_hop\t0.4");
    auto v2 = comparison_rule("z_hop", "0.5", {{"color_count", "gt", "1"}});
    REQUIRE(ConditionalSettingRule::deserialize(v2.serialize()).serialize() == v2.serialize());
    REQUIRE(ConditionalSettingRule::deserialize(v2.serialize()).actions().front().value == "0.5");
    auto edited = v2;
    edited.effects = edited.actions();
    edited.effects.push_back({"wall_sequence", "outer wall/inner wall"});
    REQUIRE(ConditionalSettingRule::deserialize(edited.serialize()).effects.size() == 2);
    auto config = conditional_config();
    rules(config, {legacy, v2, edited});
    apply_conditional_settings(config, 2);
    REQUIRE_THAT(config.option<ConfigOptionFloats>("z_hop")->values.front(), Catch::Matchers::WithinAbs(0.5, 1e-6));
    REQUIRE(config.option("wall_sequence")->serialize() == "outer wall/inner wall");
}

TEST_CASE("All effects are validated before any conditional setting is applied", "[ConditionalSettings]")
{
    auto rule = comparison_rule("", "", {{"multi_color", "eq", "1"}});
    rule.effects = {{"z_hop", "0.6"}, {"wall_sequence", "invalid order"}};
    REQUIRE_THAT(validate_conditional_setting(rule), Catch::Matchers::ContainsSubstring("Effect 2"));
    auto config = conditional_config();
    rules(config, {rule});
    const auto original = config;
    REQUIRE_THROWS_AS(apply_conditional_settings(config, 2), ConfigurationError);
    REQUIRE(config.diff(original).empty());
    rule.effects.back() = {"", ""};
    REQUIRE_THAT(validate_conditional_setting(rule), Catch::Matchers::ContainsSubstring("Choose a setting"));
    rule.effects.back() = {"machine_start_gcode", "G28"};
    REQUIRE_FALSE(validate_conditional_setting(rule).empty());
    rule.enabled = false;
    REQUIRE(validate_conditional_setting(rule).empty());
    rules(config, {rule});
    apply_conditional_settings(config, 2);
    REQUIRE_THAT(config.option<ConfigOptionFloats>("z_hop")->values.front(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE_FALSE(ConditionalSettingRule::deserialize(rule.serialize()).enabled);
    const auto malformed = GENERATE(
        R"({"version":5,"conditions":[{}]})",
        R"({"version":3,"enabled":true,"per_filament":false,"match_all":true,"conditions":[{"setting":"multi_color","comparison":"eq","value":"1"}],"effects":[]})",
        R"({"version":3,"enabled":true,"per_filament":false,"match_all":true,"conditions":[{"setting":"multi_color","comparison":"eq","value":"1"}],"effects":[{"target":"z_hop","value":0.6}]})");
    REQUIRE_THROWS_AS(ConditionalSettingRule::deserialize(malformed), ConfigurationError);
}

TEST_CASE("Multiple effects retain frozen predicates and precedence for each setting", "[ConditionalSettings]")
{
    auto config = conditional_config();
    auto rule = comparison_rule("", "", {{"multi_color", "eq", "1"}});
    rule.effects = {{"enable_support", "1"}, {"z_hop", "0.4"}, {"wall_loops", "4"}, {"z_hop", "0.6"}};
    rules(config, {rule, comparison_rule("travel_speed", "42", {{"enable_support", "eq", "1"}}),
        comparison_rule("wall_loops", "5", {{"multi_color", "eq", "1"}})});
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    auto model = model_with_colors(true);
    print.apply(model, config);
    REQUIRE_THAT(print.config().z_hop.values.front(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    REQUIRE(print.default_object_config().enable_support.value);
    REQUIRE(print.default_region_config().wall_loops.value == 5);
    REQUIRE(print.config().travel_speed.get_at(0) != 42.);
    REQUIRE(print.apply(model, config) == Print::APPLY_STATUS_UNCHANGED);
}

TEST_CASE("A material condition changes multiple settings plate-wide", "[ConditionalSettings]")
{
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    config.set_deserialize_strict("filament_retraction_length", "0.2,0.3");
    config.set_deserialize_strict("filament_wipe", "0,0");
    auto rule = comparison_rule("", "", {{"filament_type", "eq", "PETG"}}, true);
    rule.effects = {{"retraction_length", "0.8"}, {"wipe", "1"}, {"retract_before_wipe", "50%"}};
    rules(config, {rule});
    REQUIRE(validate_conditional_settings(config).empty());
    Print print;
    print.is_BBL_printer() = false;
    print.set_plate_origin(Vec3d::Zero());
    print.set_status_silent();
    auto model = model_with_colors(true);
    print.apply(model, config);
    REQUIRE(print.config().retraction_length.values == std::vector<double>{0.8, 0.8});
    REQUIRE(print.config().wipe.serialize() == "1,1");
    REQUIRE_THAT(print.config().retract_before_wipe.values.back(), Catch::Matchers::WithinAbs(50., 1e-6));
    rule.effects.push_back({"wall_sequence", "outer wall/inner wall"});
    REQUIRE(validate_conditional_setting(rule).empty());
    rule.effects.pop_back();
    rule.enabled = false;
    rules(config, {rule});
    print.apply(model, config);
    REQUIRE(print.config().retraction_length.values == std::vector<double>{0.2, 0.3});
    REQUIRE(print.config().wipe.serialize() == "0,0");
}

TEST_CASE("Old filament-scoped rule files now apply every effect to the whole plate", "[ConditionalSettings]")
{
    const std::string row = GENERATE(
        R"({"version":2,"enabled":true,"per_filament":true,"match_all":true,"conditions":[{"setting":"filament_type","comparison":"eq","value":"PETG"}],"target":"retraction_length","value":"0.9"})",
        R"({"version":3,"enabled":true,"per_filament":true,"match_all":true,"conditions":[{"setting":"filament_type","comparison":"eq","value":"PETG"}],"effects":[{"target":"retraction_length","value":"0.9"},{"target":"wipe","value":"1"}]})");
    auto rule = ConditionalSettingRule::deserialize(row);
    REQUIRE(validate_conditional_setting(rule).empty());
    REQUIRE_THAT(rule.serialize(), Catch::Matchers::ContainsSubstring(R"("version":4)"));
    REQUIRE_THAT(rule.serialize(), !Catch::Matchers::ContainsSubstring("per_filament"));
    auto config = conditional_config();
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PETG"});
    config.set_deserialize_strict("filament_retraction_length", "0.2,0.3");
    config.set_key_value("conditional_settings", new ConfigOptionStrings{row});
    apply_conditional_settings(config, 2);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{0.9, 0.9});
    REQUIRE(config.option<ConfigOptionStrings>("conditional_settings")->values.front() == row);
}

TEST_CASE("Named rules preserve their name enable state conditions and effects", "[ConditionalSettings]")
{
    auto rule = comparison_rule("", "", {{"multi_color", "eq", "1"}, {"enable_support", "eq", "0"}});
    rule.name = "Multicolor \"walls\" + Z-hop";
    rule.match_all = false;
    rule.enabled = false;
    rule.effects = {{"z_hop", "0.4"}, {"wall_loops", "4"}};
    const auto decoded = ConditionalSettingRule::deserialize(rule.serialize());
    REQUIRE(decoded.name == rule.name);
    REQUIRE_FALSE(decoded.enabled);
    REQUIRE_FALSE(decoded.match_all);
    REQUIRE(decoded.conditions.size() == 2);
    REQUIRE(decoded.effects.size() == 2);
    REQUIRE(decoded.serialize() == rule.serialize());
    auto config = conditional_config();
    rules(config, {decoded});
    const auto original = config;
    apply_conditional_settings(config, 2);
    REQUIRE(config.diff(original).empty());
}

TEST_CASE("Clickable AND OR and Off conditions retain their logical meaning", "[ConditionalSettings]")
{
    auto config = conditional_config();
    auto rule = comparison_rule("wall_loops", "4",
        {{"multi_color", "eq", "1"}, {"enable_support", GENERATE("eq", "ne"), "0"}});
    if (rule.conditions.back().comparison == "ne") rule.conditions.back().value = "1";
    rules(config, {rule});
    auto all = config;
    apply_conditional_settings(all, 1);
    REQUIRE(all.opt_int("wall_loops") == 2);
    rule.match_all = false;
    rules(config, {rule});
    auto any = config;
    apply_conditional_settings(any, 1);
    REQUIRE(any.opt_int("wall_loops") == 4);
    rule.enabled = false;
    rules(config, {rule});
    auto off = config;
    apply_conditional_settings(off, 2);
    REQUIRE(off.opt_int("wall_loops") == 2);
}
