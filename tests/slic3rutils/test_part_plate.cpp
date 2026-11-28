// PartPlate.hpp reaches wx/wxprec.h, which needs the Windows headers first.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    // Match the GUI precompiled header: wx/msw/wrapcctl.h needs HDITEM from CommCtrl.h.
    #include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/PartPlate.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
DynamicPrintConfig nozzle_config(std::vector<double> nozzle_diameters,
                                 std::vector<int>    filament_map,
                                 std::vector<int>    physical_extruder_map)
{
    DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::move(nozzle_diameters)));
    config.set_key_value("filament_map", new ConfigOptionInts(std::move(filament_map)));
    config.set_key_value("physical_extruder_map", new ConfigOptionInts(std::move(physical_extruder_map)));
    return config;
}

// Print settings for three filaments with no print-wide feature filament, brim, support or raft.
DynamicPrintConfig config_with_object_filaments_only()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings(3, "#FFFFFF"));
    for (const char* key : {"outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                            "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"})
        config.set(key, 0);
    config.set("enable_support", false);
    config.set("raft_layers", 0);
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    return config;
}

struct ObjectOnPlate
{
    Model        model;
    PartPlate    plate{nullptr, Vec3d::Zero(), 200, 200, 200., nullptr, &model, true, ptFFF};
    ModelObject* object{model.add_object()};
    ModelVolume* part{object->add_volume(make_cube(10., 10., 10.), ModelVolumeType::MODEL_PART, false)};

    ObjectOnPlate()
    {
        object->config.set("extruder", 1);
        object->add_instance()->set_offset(Vec3d(95., 95., 0.));
        plate.set_shape({{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}}, {}, {}, {}, Vec2d::Zero(), 0.f, 0.f);
        plate.add_instance(0, 0, false);
    }
};
} // namespace

TEST_CASE("A multi-nozzle printer without its own physical extruder map puts each filament on its own nozzle", "[PartPlate]")
{
    // Four nozzles, filament n on nozzle n, and the single-entry physical_extruder_map default.
    const DynamicPrintConfig config = nozzle_config({0.4, 0.4, 0.4, 0.4}, {1, 2, 3, 4}, {0});
    const PartPlate          plate;

    const int filament_id = GENERATE(1, 2, 3, 4);
    CHECK(plate.get_physical_extruder_by_filament_id(config, filament_id) == filament_id - 1);
}

TEST_CASE("A physical extruder map the profile sets decides the physical nozzle", "[PartPlate]")
{
    // Two nozzles whose physical order is the reverse of the logical one, as on a two-nozzle Bambu printer.
    const DynamicPrintConfig config = nozzle_config({0.4, 0.4}, {1, 2}, {1, 0});
    const PartPlate          plate;

    CHECK(plate.get_physical_extruder_by_filament_id(config, 1) == 1);
    CHECK(plate.get_physical_extruder_by_filament_id(config, 2) == 0);
}

TEST_CASE("A filament with no extruder to map to has no physical nozzle", "[PartPlate]")
{
    // Filament 2 is mapped to a fifth extruder the printer does not have; filament 0 and 3 do not exist.
    const DynamicPrintConfig config = nozzle_config({0.4, 0.4, 0.4, 0.4}, {1, 5}, {0});
    const PartPlate          plate;

    CHECK(plate.get_physical_extruder_by_filament_id(config, 2) == -1);
    CHECK(plate.get_physical_extruder_by_filament_id(config, 0) == -1);
    CHECK(plate.get_physical_extruder_by_filament_id(config, 3) == -1);
}

TEST_CASE("A feature filament a part or layer range sets is a filament the plate uses", "[PartPlate]")
{
    const std::string key = GENERATE("outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                                     "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id");
    const bool        in_layer_range = GENERATE(false, true);
    ObjectOnPlate      on_plate;
    if (in_layer_range)
        on_plate.object->layer_config_ranges[{2., 5.}].set(key, 2);
    else
        on_plate.part->config.set(key, 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1, 2});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1, 2});
}

TEST_CASE("The filament a layer range prints with is a filament the plate uses", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    on_plate.object->layer_config_ranges[{2., 5.}].set("extruder", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1, 2});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1, 2});
}

TEST_CASE("A filament an object, part or layer range sets past the last filament slot is filament 1 on the plate", "[PartPlate]")
{
    enum class Where { Object, Part, PaintedPart, LayerRange };
    const auto [where, key, expected] = GENERATE(table<Where, std::string, std::vector<int>>({
        {Where::Object, "outer_wall_filament_id", {1, 2}},
        {Where::Part, "outer_wall_filament_id", {1, 2}},
        {Where::LayerRange, "outer_wall_filament_id", {1, 2}},
        {Where::Object, "extruder", {1}},
        {Where::Part, "extruder", {1}},
        {Where::PaintedPart, "extruder", {1, 3}},
        {Where::LayerRange, "extruder", {1, 2}},
    }));
    ObjectOnPlate on_plate;
    on_plate.object->config.set("extruder", 2);
    if (where == Where::Object)
        on_plate.object->config.set(key, 4);
    else if (where == Where::LayerRange)
        on_plate.object->layer_config_ranges[{2., 5.}].set(key, 4);
    else
        on_plate.part->config.set(key, 4);
    if (where == Where::PaintedPart) {
        TriangleSelector selector(on_plate.part->mesh());
        selector.set_facet(0, EnforcerBlockerType::Extruder3);
        REQUIRE(on_plate.part->mmu_segmentation_facets.set(selector));
    }
    DynamicPrintConfig config       = config_with_object_filaments_only();
    DynamicPrintConfig print_config = config;
    print_config.erase("filament_colour");

    CHECK(on_plate.plate.get_extruders(false, print_config, config) == expected);
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == expected);
}

TEST_CASE("A print-wide filament past the last filament slot resolves as the app's preset reset or the slicer's clamp does", "[PartPlate]")
{
    const auto [key, reset_by_app, clamped_by_slicer] = GENERATE(table<std::string, std::vector<int>, std::vector<int>>({
        {"outer_wall_filament_id", {2}, {1, 2}},
        {"support_filament", {2, 3}, {1, 2}},
        {"brim_filament_id", {2}, {2}},
    }));
    ObjectOnPlate on_plate;
    on_plate.object->config.set("extruder", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();
    config.set(key, 4);
    config.set("enable_support", key == "support_filament");
    if (key == "brim_filament_id")
        config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    DynamicPrintConfig print_config = config;
    print_config.erase("filament_colour");

    CHECK(on_plate.plate.get_extruders(false, print_config, config, true, true) == reset_by_app);
    CHECK(on_plate.plate.get_extruders(false, config, config) == clamped_by_slicer);
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == clamped_by_slicer);
}

TEST_CASE("A volume's feature filament is a filament the plate uses only when the volume makes print regions", "[PartPlate]")
{
    const auto [type, expected] = GENERATE(table<ModelVolumeType, std::vector<int>>({
        {ModelVolumeType::PARAMETER_MODIFIER, {1, 2}},
        {ModelVolumeType::PAINTED_MODIFIER, {1, 2}},
        {ModelVolumeType::NEGATIVE_VOLUME, {1}},
        {ModelVolumeType::SUPPORT_BLOCKER, {1}},
        {ModelVolumeType::SUPPORT_ENFORCER, {1}},
        {ModelVolumeType::PRECISE_SEAM_CENTER, {1}},
    }));
    ObjectOnPlate on_plate;
    on_plate.object->add_volume(make_cube(5., 5., 5.), type, false)->config.set("outer_wall_filament_id", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == expected);
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == expected);
}

TEST_CASE("A mixed filament a part sets for its outer wall stands for its components on the plate", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    on_plate.part->config.set("outer_wall_filament_id", 3);
    DynamicPrintConfig config = config_with_object_filaments_only();
    // Filament 3 mixes filaments 1 and 2.
    config.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false, true}));
    config.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", "", "1,2"}));

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1, 2});
    CHECK(on_plate.plate.get_extruders(false, config, config, false) == std::vector<int>{1, 3});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1, 2});
}

TEST_CASE("A feature filament a part sets for a feature it does not print is not a filament the plate uses", "[PartPlate]")
{
    const auto [key, setting, value] = GENERATE(table<std::string, std::string, int>({
        {"top_surface_filament_id", "top_shell_layers", 0},
        {"bottom_surface_filament_id", "bottom_shell_layers", 0},
        {"inner_wall_filament_id", "wall_loops", 1},
        {"sparse_infill_filament_id", "sparse_infill_density", 0},
    }));
    ObjectOnPlate on_plate;
    on_plate.part->config.set(key, 2);
    if (setting == "sparse_infill_density")
        on_plate.part->config.set_key_value(setting, new ConfigOptionPercent(value));
    else
        on_plate.part->config.set(setting, value);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1});
}

TEST_CASE("A filament a layer range above the object sets is not a filament the plate uses", "[PartPlate]")
{
    const std::string key = GENERATE("extruder", "sparse_infill_filament_id");
    ObjectOnPlate     on_plate;
    on_plate.object->layer_config_ranges[{20., 30.}].set(key, 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1});
}

TEST_CASE("A filament a modifier sets is a filament the plate uses only when the modifier overlaps a part", "[PartPlate]")
{
    const std::string key = GENERATE("extruder", "sparse_infill_filament_id");
    const auto [offset, expected] = GENERATE(table<Vec3d, std::vector<int>>({
        {Vec3d(2., 2., 2.), {1, 2}},
        {Vec3d(20., 20., 2.), {1}},
    }));
    ObjectOnPlate on_plate;
    ModelVolume*  modifier = on_plate.object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER, false);
    modifier->set_offset(offset);
    modifier->config.set(key, 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == expected);
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == expected);
}

TEST_CASE("The object's filament is not a filament the plate uses when its only part prints every feature with another", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    for (const char* key : {"outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                            "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"})
        on_plate.part->config.set(key, 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{2});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{2});
}

TEST_CASE("The plate's filaments without support list a feature filament a part sets and leave out the support and brim filaments", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    on_plate.part->config.set("sparse_infill_filament_id", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();
    config.set("enable_support", true);
    config.set("support_filament", 3);
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    config.set("brim_width", 5.);
    config.set("brim_filament_id", 3);

    CHECK(on_plate.plate.get_extruders_without_support(false, config, config) == std::vector<int>{1, 2});
    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1, 2, 3});
}

TEST_CASE("The object's filament is not a filament the plate uses when a layer range covering the object sets another", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    on_plate.object->layer_config_ranges[{0., 10.}].set("extruder", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{2});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{2});
}

TEST_CASE("A layer range over the part of a sinking object below the bed is not a filament the plate uses", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    // The 10 mm cube sinks 3 mm, so it prints up to 7 mm.
    on_plate.object->instances.front()->set_offset(Vec3d(95., 95., -3.));
    on_plate.object->layer_config_ranges[{8., 10.}].set("sparse_infill_filament_id", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1});
}

TEST_CASE("A layer range above a rotated part's top is not a filament the plate uses", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    on_plate.object->add_volume(make_sphere(10.), ModelVolumeType::MODEL_PART, false);
    on_plate.object->delete_volume(0);
    // The sphere prints up to 20 mm, while its mesh box turned 45 degrees reaches 24 mm.
    on_plate.object->instances.front()->set_offset(Vec3d(95., 95., 10.));
    on_plate.object->instances.front()->set_rotation(Vec3d(PI / 4., 0., 0.));
    on_plate.object->layer_config_ranges[{21., 24.}].set("sparse_infill_filament_id", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1});
}

TEST_CASE("The plate's filaments without support leave out an outer wall filament that only the brim prints with", "[PartPlate]")
{
    ObjectOnPlate on_plate;
    on_plate.part->config.set("wall_loops", 0);
    on_plate.part->config.set("outer_wall_filament_id", 2);
    DynamicPrintConfig config = config_with_object_filaments_only();
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    config.set("brim_width", 5.);

    CHECK(on_plate.plate.get_extruders_without_support(false, config, config) == std::vector<int>{1});
}

TEST_CASE("A brim filament is a filament the plate uses only when the brim prints", "[PartPlate]")
{
    const auto [brim_width, raft_layers, expected] = GENERATE(table<double, int, std::vector<int>>({
        {5., 0, {1, 2}},
        {0., 0, {1}},
        {5., 2, {1}},
    }));
    ObjectOnPlate      on_plate;
    DynamicPrintConfig config = config_with_object_filaments_only();
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    config.set("brim_width", brim_width);
    config.set("brim_filament_id", 2);
    config.set("raft_layers", raft_layers);

    CHECK(on_plate.plate.get_extruders(false, config, config) == expected);
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == expected);
}

TEST_CASE("The support filament is a filament the plate uses when support is enforced for the first layers", "[PartPlate]")
{
    ObjectOnPlate      on_plate;
    DynamicPrintConfig config = config_with_object_filaments_only();
    config.set("enforce_support_layers", 3);
    config.set("support_filament", 3);

    CHECK(on_plate.plate.get_extruders(false, config, config) == std::vector<int>{1, 3});
    CHECK(on_plate.plate.get_extruders_under_cli(false, config) == std::vector<int>{1, 3});
}
