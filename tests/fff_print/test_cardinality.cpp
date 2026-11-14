// A 2-extruder object sliced through the real Print::apply -> process path. Print::apply runs
// enforce_cardinality, which sizes every per-N array (per-extruder printable height, machine limits,
// filament_map, ...) to its declared cardinality, so the multi-extruder slice completes.
#include <catch2/catch_all.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Config.hpp>
#include <libslic3r/Point.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PrintConfigCardinality.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("enforce_cardinality sizes per-N arrays so a multi-extruder object slices", "[Cardinality]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    config.set_key_value("gcode_flavor",               new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_line_width",   new ConfigOptionFloatOrPercent(0, false));
    config.set_key_value("z_hop",                       new ConfigOptionFloats({0}));
    // Sequential print so each object uses its own extruder without a wipe tower.
    config.set_key_value("print_sequence",             new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

    // Clear every custom G-code block so the end-of-export placeholder-def check
    // (ORCA_CHECK_GCODE_PLACEHOLDERS) has nothing to complain about. This isolates
    // the per-extruder sizing behaviour from unrelated placeholder validation.
    config.set_key_value("machine_start_gcode",        new ConfigOptionString(""));
    config.set_key_value("machine_end_gcode",          new ConfigOptionString(""));
    config.set_key_value("before_layer_change_gcode",  new ConfigOptionString(""));
    config.set_key_value("layer_change_gcode",         new ConfigOptionString(""));
    config.set_key_value("change_filament_gcode",      new ConfigOptionString(""));
    config.set_key_value("time_lapse_gcode",           new ConfigOptionString(""));
    config.set_key_value("printing_by_object_gcode",   new ConfigOptionString(""));
    config.set_key_value("filament_start_gcode",       new ConfigOptionStrings({"", ""}));
    config.set_key_value("filament_end_gcode",         new ConfigOptionStrings({"", ""}));

    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 0, 0, 0}));

    // Stride-2 (normal, silent) machine limits per extruder.
    config.set_key_value("machine_max_acceleration_x",          new ConfigOptionFloats({500, 0, 1000, 0}));
    config.set_key_value("machine_max_acceleration_y",          new ConfigOptionFloats({700, 0, 1100, 0}));
    config.set_key_value("machine_max_acceleration_z",          new ConfigOptionFloats({100, 0, 300, 0}));
    config.set_key_value("machine_max_acceleration_e",          new ConfigOptionFloats({5000, 0, 8000, 0}));
    config.set_key_value("machine_max_acceleration_extruding",  new ConfigOptionFloats({1200, 0, 2200, 0}));
    config.set_key_value("machine_max_acceleration_retracting", new ConfigOptionFloats({1400, 0, 2400, 0}));
    config.set_key_value("machine_max_acceleration_travel",     new ConfigOptionFloats({1600, 0, 2600, 0}));
    config.set_key_value("machine_max_speed_x",  new ConfigOptionFloats({100, 0, 200, 0}));
    config.set_key_value("machine_max_speed_y",  new ConfigOptionFloats({110, 0, 210, 0}));
    config.set_key_value("machine_max_speed_z",  new ConfigOptionFloats({10, 0, 30, 0}));
    config.set_key_value("machine_max_speed_e",  new ConfigOptionFloats({50, 0, 80, 0}));
    config.set_key_value("machine_max_jerk_x",   new ConfigOptionFloats({8, 0, 12, 0}));
    config.set_key_value("machine_max_jerk_y",   new ConfigOptionFloats({9, 0, 13, 0}));
    config.set_key_value("machine_max_jerk_z",   new ConfigOptionFloats({0.4, 0, 0.6, 0}));
    config.set_key_value("machine_max_jerk_e",   new ConfigOptionFloats({5, 0, 10, 0}));
    config.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats({0.02, 0, 0.05, 0}));

    Model model;
    auto *obj1 = model.add_object();
    obj1->add_volume(cube(20));
    obj1->add_instance();

    auto *obj2 = model.add_object();
    obj2->add_volume(cube(20));
    obj2->add_instance();
    obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

    Print print;
    arrange_objects(model, InfiniteBed{}, ArrangeParams{scaled(min_object_distance(config))});
    for (auto *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    // apply() runs enforce_cardinality, sizing every per-N array to its cardinality; the slice then completes.
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 2);
    REQUIRE(print.objects().front()->layer_count() > 0);
}

namespace {

// Two filaments on two direct-drive extruders, each extruder with a Standard and a High Flow variant. The variant
// options are laid out unexpanded, as the preset bundle hands them to Print::apply: two columns per extruder for the
// printer and process options, two per filament for the filament options.
DynamicPrintConfig two_extruder_variant_config()
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "layer_height",               0.2      },
        { "initial_layer_print_height", 0.2      },
        { "skirt_loops",                0        } });
    config.set_num_extruders(2);
    config.set_deserialize_strict({
        { "single_extruder_multi_material", false },
        { "nozzle_diameter",                "0.4,0.4" },
        { "extruder_type",                  "Direct Drive,Direct Drive" },
        { "nozzle_volume_type",             "Standard,Standard" },
        { "extruder_printable_area",        "0x0,200x0,200x200,0x200#0x0,200x0,200x200,0x200" },
        { "extruder_printable_height",      "100,100" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2" } });
    size_flush_to_nozzles(config);

    const std::vector<std::string> variants = { "Direct Drive Standard", "Direct Drive High Flow" };
    const auto columns = [&variants, &config](int owners, const char *variant_key, const char *id_key) {
        std::vector<std::string> names;
        std::vector<int>         ids;
        for (int owner = 1; owner <= owners; ++owner)
            for (const std::string &variant : variants) {
                names.push_back(variant);
                ids.push_back(owner);
            }
        config.option<ConfigOptionStrings>(variant_key, true)->values = names;
        config.option<ConfigOptionInts>(id_key, true)->values         = ids;
    };
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = std::vector<std::string>(2, variants[0] + "," + variants[1]);
    columns(2, "printer_extruder_variant", "printer_extruder_id");
    columns(2, "print_extruder_variant", "print_extruder_id");
    columns(2, "filament_extruder_variant", "filament_self_index");

    // Every per-N option at the length its cardinality declares, so a case cuts one option to empty and nothing else.
    const AxisCounts n = axis_counts(config);
    for (const auto &[key, def] : print_config_def.options)
        if (def.default_value && def.default_value->is_vector() && expected_size(cardinality_of(key), n) != 0) {
            config.option(key, true);
            resize_to_cardinality(config, key, n);
        }
    return config;
}

// One 5 mm cube per extruder.
Model two_extruder_model()
{
    Model model;
    for (int extruder = 1; extruder <= 2; ++extruder) {
        TriangleMesh mesh = make_cube(5., 5., 5.);
        mesh.translate(80.f + 15.f * static_cast<float>(extruder), 150.f, 0.f);
        ModelObject *object = model.add_object();
        object->add_volume(std::move(mesh));
        object->add_instance();
        object->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    }
    return model;
}

struct EmptyCase
{
    const char *key;
    size_t      size_after_apply;
};

} // namespace

TEST_CASE("Print::apply sizes an empty variant-expansion input instead of crashing in the expansion", "[Cardinality]")
{
    // Each key is one the expansion reads through get_at() before anything has sized it: a per-extruder-variant,
    // a dual (normal + silent) per-extruder-variant and a per-process-variant option, the extruder and nozzle
    // volume types, the variant list and the variant id and name columns. One section per key, so `-c "<key>"` runs
    // one alone.
    const std::vector<EmptyCase> cases = {
        { "retraction_length", 2 }, { "machine_max_acceleration_x", 4 }, { "outer_wall_speed", 2 },
        { "extruder_type", 2 }, { "nozzle_volume_type", 2 }, { "extruder_variant_list", 2 },
        { "printer_extruder_variant", 2 }, { "printer_extruder_id", 2 }, { "print_extruder_variant", 2 },
        { "print_extruder_id", 2 },
    };
    for (const EmptyCase &c : cases) {
        DYNAMIC_SECTION(c.key) {
            DynamicPrintConfig config = two_extruder_variant_config();
            auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(c.key));
            REQUIRE(opt != nullptr);
            REQUIRE(opt->size() >= 2);
            opt->resize(0);

            Print print;
            print.apply(two_extruder_model(), config);

            const auto *applied = dynamic_cast<const ConfigOptionVectorBase *>(print.full_print_config().option(c.key));
            REQUIRE(applied != nullptr);
            CHECK(applied->size() == c.size_after_apply);
        }
    }
}

TEST_CASE("size_empty_expansion_inputs leaves a config with no empty option unchanged", "[Cardinality]")
{
    const DynamicPrintConfig before = two_extruder_variant_config();
    DynamicPrintConfig       after  = before;
    after.size_empty_expansion_inputs();
    CHECK(after == before);
}

TEST_CASE("An empty filament_self_index still gives each filament the values of its own variant column", "[Cardinality]")
{
    // A regression guard, not a fail-before test: it passes without the sizing too. An empty index must stay empty
    // through the expansion, which then finds each filament's column from the variant list's own order; filling it
    // with the schema default would assign every column to filament 1 and filament 2 would read filament 1's column.
    DynamicPrintConfig config = two_extruder_variant_config();
    // Columns are filament 1 Standard, filament 1 High Flow, filament 2 Standard, filament 2 High Flow.
    config.option<ConfigOptionFloats>("filament_flow_ratio", true)->values = { 0.90, 0.91, 0.95, 0.96 };
    config.option<ConfigOptionInts>("filament_self_index", true)->values.clear();

    Print print;
    print.apply(two_extruder_model(), config);

    const auto *flow_ratio = print.full_print_config().option<ConfigOptionFloats>("filament_flow_ratio");
    REQUIRE(flow_ratio != nullptr);
    REQUIRE(flow_ratio->values.size() == 2);
    CHECK_THAT(flow_ratio->values[0], Catch::Matchers::WithinAbs(0.90, 1e-9));
    CHECK_THAT(flow_ratio->values[1], Catch::Matchers::WithinAbs(0.95, 1e-9));
}

TEST_CASE("A filament_self_index shorter than the variant list does not stop a multi-extruder slice", "[Cardinality]")
{
    // The filament variant list names four columns, and the list that says which filament owns each is shorter.
    DynamicPrintConfig config = two_extruder_variant_config();
    REQUIRE(config.option<ConfigOptionStrings>("filament_extruder_variant")->values.size() == 4);
    std::vector<int> &self_index = config.option<ConfigOptionInts>("filament_self_index", true)->values;
    REQUIRE(self_index.size() == 4);

    SECTION("empty") { self_index.clear(); }
    SECTION("one column short") { self_index.pop_back(); }

    Print print;
    print.apply(two_extruder_model(), config);
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 2);
    CHECK(print.objects().front()->layer_count() > 0);
}

namespace {

// Two 20 mm cubes on two filaments of one nozzle, with a prime tower between the filament changes.
DynamicPrintConfig tower_config()
{
    return multifilament_config(2, {
        { "enable_prime_tower", "1"        },
        { "wipe_tower_x",       "50"       },
        { "wipe_tower_y",       "50"       },
        { "layer_change_gcode", "G92 E0\n" } });
}

const std::vector<std::vector<ConfigBase::SetDeserializeItem>> two_cube_extruders{ { { "extruder", "1" } }, { { "extruder", "2" } } };

} // namespace

TEST_CASE("An empty flush_multiplier is sized at apply so the export does not divide by its length", "[Cardinality]")
{
    DynamicPrintConfig config = tower_config();
    config.option<ConfigOptionFloats>("flush_multiplier", true)->values.clear();

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &two_cube_extruders);
    const std::string gcode_text = gcode(print);

    CHECK_FALSE(gcode_text.empty());
    CHECK(print.full_print_config().option<ConfigOptionFloats>("flush_multiplier")->values.size() == 1);
}

TEST_CASE("An empty flush_volumes_matrix is sized at apply so the wipe tower slices", "[Cardinality]")
{
    DynamicPrintConfig config = tower_config();
    config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values.clear();

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &two_cube_extruders);
    REQUIRE(print.has_wipe_tower());
    const std::string gcode_text = gcode(print);

    CHECK_FALSE(gcode_text.empty());
    CHECK(print.full_print_config().option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 4);
}

TEST_CASE("Print::apply sizes the flush matrix to the nozzles and filaments the config declares", "[Cardinality]")
{
    // Two nozzles and three filaments need three 3 x 3 blocks' worth of cells per nozzle; the schema default has 16.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "filament_diameter", "1.75,1.75,1.75" } });
    REQUIRE(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);

    SECTION("filament_colour agrees with filament_diameter") {
        config.set_deserialize_strict({ { "filament_colour", "#FF0000;#00FF00;#0000FF" } });
    }
    SECTION("filament_colour is shorter than filament_diameter") {
        config.set_deserialize_strict({ { "filament_colour", "#FF0000;#00FF00" } });
    }

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance();
    Print print;
    print.apply(model, config);

    const DynamicPrintConfig &applied = print.full_print_config();
    CHECK(applied.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 18);
    CHECK(applied.option<ConfigOptionFloats>("flush_multiplier")->values.size() == 2);
    CHECK(applied.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
}

TEST_CASE("Print::apply keeps the flush volumes of a single-filament config as they were", "[Cardinality]")
{
    // A command-line slice of one filament carries the 16-cell schema default; it purges nothing and stays as is.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance();
    Print print;
    print.apply(model, config);

    CHECK(print.full_print_config().option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);
}
