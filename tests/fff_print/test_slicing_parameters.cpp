#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

using Catch::Matchers::WithinAbs;
using namespace Slic3r;

namespace {

constexpr double EPS = 1e-9;

// Two extruders with distinct layer height limits, so the column that was read shows in the
// result: extruder 1 allows 0.04 to 0.14 mm, extruder 2 allows 0.08 to 0.28 mm.
struct TwoExtruderSlicingConfig
{
    PrintConfig       print_config;
    PrintObjectConfig object_config;

    TwoExtruderSlicingConfig()
    {
        print_config.nozzle_diameter.values            = {0.2, 0.4};
        print_config.min_layer_height.values           = {0.04, 0.08};
        print_config.max_layer_height.values           = {0.14, 0.28};
        print_config.filament_map.values               = {1, 2};
        print_config.filament_map_mode.value           = fmmManual;
        print_config.initial_layer_print_height.value  = 0.10;
        object_config.layer_height.value               = 0.12;
    }

    // filament_ids are zero based, as PrintObject::object_extruders() returns them.
    SlicingParameters create(const std::vector<unsigned int> &filament_ids) const
    {
        return SlicingParameters::create_from_config(print_config, object_config, 10.0, filament_ids, Vec3d::Ones());
    }
};

} // namespace

TEST_CASE("Layer height limits follow a manual filament map", "[SlicingParameters]")
{
    TwoExtruderSlicingConfig cfg;
    cfg.print_config.filament_map_mode.value = GENERATE(fmmManual, fmmNozzleManual);

    const SlicingParameters identity = cfg.create({0});
    CHECK_THAT(identity.min_layer_height, WithinAbs(0.04, EPS));
    CHECK_THAT(identity.max_layer_height, WithinAbs(0.14, EPS));

    // Only the map changes: filament 1 now prints on extruder 2.
    cfg.print_config.filament_map.values = {2, 1};
    const SlicingParameters remapped = cfg.create({0});
    CHECK_THAT(remapped.min_layer_height, WithinAbs(0.08, EPS));
    CHECK_THAT(remapped.max_layer_height, WithinAbs(0.28, EPS));
}

TEST_CASE("Layer height limits of several filaments keep the window they share", "[SlicingParameters]")
{
    TwoExtruderSlicingConfig cfg;
    cfg.print_config.filament_map.values = {1, 1, 2};

    // Filament 2 is on extruder 1 (max 0.14), filament 3 on extruder 2 (min 0.08).
    const SlicingParameters params = cfg.create({1, 2});
    CHECK_THAT(params.min_layer_height, WithinAbs(0.08, EPS));
    CHECK_THAT(params.max_layer_height, WithinAbs(0.14, EPS));
}

TEST_CASE("Support layer height limits follow a manual filament map", "[SlicingParameters][Support]")
{
    TwoExtruderSlicingConfig cfg;
    cfg.object_config.enable_support.value             = true;
    cfg.object_config.support_filament.value           = 1; // one based
    cfg.object_config.support_interface_filament.value = 1;

    cfg.print_config.filament_map.values = {1, 1};
    const SlicingParameters identity = cfg.create({1});
    CHECK_THAT(identity.min_layer_height, WithinAbs(0.04, EPS));
    CHECK_THAT(identity.max_suport_layer_height, WithinAbs(0.14, EPS));

    cfg.print_config.filament_map.values = {2, 1};
    const SlicingParameters remapped = cfg.create({1});
    CHECK_THAT(remapped.min_layer_height, WithinAbs(0.08, EPS));
    CHECK_THAT(remapped.max_suport_layer_height, WithinAbs(0.28, EPS));
}

TEST_CASE("Automatic filament map modes keep the per-filament limit lookup", "[SlicingParameters]")
{
    TwoExtruderSlicingConfig cfg;
    cfg.print_config.filament_map_mode.value = GENERATE(fmmAutoForFlush, fmmAutoForMatch, fmmDefault);
    // The map is decided after slicing in these modes, so a map in the config is not read.
    cfg.print_config.filament_map.values = {2, 2};

    const SlicingParameters params = cfg.create({1});
    CHECK(params.valid);
    CHECK_THAT(params.min_layer_height, WithinAbs(0.04, EPS));
    CHECK_THAT(params.max_layer_height, WithinAbs(0.14, EPS));
}

TEST_CASE("Changing the filament map or its mode re-slices the object", "[SlicingParameters]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values   = {0.4, 0.4};
    config.option<ConfigOptionFloats>("min_layer_height", true)->values  = {0.04, 0.08};
    config.option<ConfigOptionFloats>("max_layer_height", true)->values  = {0.14, 0.28};
    config.option<ConfigOptionFloats>("filament_diameter", true)->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour", true)->values  = {"#FF0000", "#00FF00"};
    config.option<ConfigOptionInts>("filament_map", true)->values        = {1, 2};
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));

    Model model;
    model.add_object("cube", "", make_cube(10, 10, 10))->add_instance()->set_offset(Vec3d(100., 100., 0.));

    Print print;
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().front()->is_step_done(posSlice));

    const std::string changed_key = GENERATE(as<std::string>{}, "filament_map", "filament_map_mode");
    if (changed_key == "filament_map")
        config.option<ConfigOptionInts>("filament_map", true)->values = {2, 2};
    else
        config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmAutoForFlush));

    REQUIRE(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK_FALSE(print.objects().front()->is_step_done(posSlice));
}
