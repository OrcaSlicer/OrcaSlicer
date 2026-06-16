#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"

#include "test_data.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

ModelConfig make_spiral_range_config()
{
    DynamicPrintConfig dcfg;
    dcfg.set_deserialize_strict({
        { "range_spiral_mode", true },
        { "layer_height", 0.2 },
        { "wall_loops", 1 },
        { "top_shell_layers", 0 },
        { "sparse_infill_density", 0 },
        { "bottom_shell_layers", 0 },
    });
    ModelConfig cfg;
    cfg.assign_config(dcfg);
    return cfg;
}

void add_height_range(ModelObject &obj, double z_min, double z_max, const ModelConfig &cfg)
{
    obj.layer_config_ranges[{z_min, z_max}] = cfg;
}

DynamicPrintConfig base_spiral_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", 0.2 },
        { "first_layer_height", 0.2 },
        { "wall_loops", 1 },
        { "top_shell_layers", 0 },
        { "sparse_infill_density", 0 },
        { "spiral_mode", false },
    });
    return config;
}

DynamicPrintConfig merged_spiral_test_config(const DynamicPrintConfig &config_in)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    return config;
}

// Same as init_print() but without arrange_objects(). Orca's arrange path leaves
// bed_idx at -1 for a lone cube with InfiniteBed{}; single-object tests only need ensure_on_bed().
void setup_spiral_print(Print &print, Model &model, const DynamicPrintConfig &config_in, bool validate = true)
{
    const DynamicPrintConfig config = merged_spiral_test_config(config_in);

    ModelObject *object = model.add_object();
    object->name += "object.stl";
    TriangleMesh cube = mesh(TestMesh::cube_20x20x20);
    object->add_volume(std::move(cube));
    object->add_instance();

    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    print.apply(model, config);
    if (validate)
        print.validate();
    print.set_status_silent();
}

void apply_print_config(Print &print, Model &model, const DynamicPrintConfig &config_in)
{
    print.apply(model, merged_spiral_test_config(config_in));
}

} // namespace

TEST_CASE("layer_z_in_height_range boundaries", "[SpiralVase][unit]")
{
    const t_layer_height_range range{ 5.0, 10.0 };

    REQUIRE(layer_z_in_height_range(5.0, range));
    REQUIRE(layer_z_in_height_range(9.99, range));
    REQUIRE_FALSE(layer_z_in_height_range(10.0, range));
    REQUIRE_FALSE(layer_z_in_height_range(4.99, range));
    REQUIRE_FALSE(layer_z_in_height_range(10.0 - EPSILON, range));
    REQUIRE(layer_z_in_height_range(5.0 + EPSILON, range));
}

TEST_CASE("dynamic_config_range_spiral_mode", "[SpiralVase][unit]")
{
    REQUIRE_FALSE(dynamic_config_range_spiral_mode(nullptr));

    DynamicPrintConfig empty;
    REQUIRE_FALSE(dynamic_config_range_spiral_mode(&empty));

    DynamicPrintConfig off;
    off.set_deserialize_strict({ { "range_spiral_mode", false } });
    REQUIRE_FALSE(dynamic_config_range_spiral_mode(&off));

    DynamicPrintConfig on;
    on.set_deserialize_strict({ { "range_spiral_mode", true } });
    REQUIRE(dynamic_config_range_spiral_mode(&on));
}

TEST_CASE("Height range range_spiral_mode region config", "[SpiralVase][unit]")
{
    PrintRegionConfig config;
    REQUIRE_FALSE(config.range_spiral_mode);
    config.range_spiral_mode.value = true;
    config.wall_loops.value            = 1;
    config.top_shell_layers.value      = 0;
    config.sparse_infill_density.value = 0;
    config.bottom_shell_layers.value   = 0;
    REQUIRE(config.range_spiral_mode);
    REQUIRE(config.range_spiral_mode_smooth == false);
    REQUIRE(config.wall_loops == 1);
    REQUIRE(config.top_shell_layers == 0);
    REQUIRE(config.sparse_infill_density.value == 0.f);
    REQUIRE(config.bottom_shell_layers == 0);
}

TEST_CASE("range_spiral keys in print_options", "[SpiralVase][unit]")
{
    const std::vector<std::string> &opts = Preset::print_options();
    auto has_opt = [&opts](const char *key) {
        return std::find(opts.begin(), opts.end(), key) != opts.end();
    };
    REQUIRE(has_opt("range_spiral_mode"));
    REQUIRE(has_opt("range_spiral_mode_smooth"));
    REQUIRE(has_opt("range_spiral_max_xy_smoothing"));
    REQUIRE(has_opt("range_spiral_starting_flow_ratio"));
    REQUIRE(has_opt("range_spiral_finishing_flow_ratio"));
}

TEST_CASE("has_spiral_mode without height ranges", "[SpiralVase][validate]")
{
    Model    model;
    Print    print;
    const DynamicPrintConfig config = base_spiral_test_config();
    setup_spiral_print(print, model, config);
    REQUIRE_FALSE(print.has_spiral_mode());
}

TEST_CASE("has_spiral_mode with one spiral height range", "[SpiralVase][validate]")
{
    Model    model;
    Print    print;
    const DynamicPrintConfig config = base_spiral_test_config();
    setup_spiral_print(print, model, config, /*validate*/ false);
    add_height_range(*model.objects.front(), 5.0, 10.0, make_spiral_range_config());
    apply_print_config(print, model, config);
    REQUIRE(print.has_spiral_mode());
}

TEST_CASE("validate allows two non-overlapping spiral height ranges", "[SpiralVase][validate]")
{
    Model    model;
    Print    print;
    const DynamicPrintConfig config = base_spiral_test_config();
    setup_spiral_print(print, model, config, /*validate*/ false);
    ModelObject &object = *model.objects.front();
    const ModelConfig spiral_cfg = make_spiral_range_config();
    add_height_range(object, 5.0, 10.0, spiral_cfg);
    add_height_range(object, 12.0, 17.0, spiral_cfg);
    apply_print_config(print, model, config);

    const StringObjectException err = print.validate();
    REQUIRE(err.string.empty());
}

TEST_CASE("has_spiral_mode with global spiral_mode", "[SpiralVase][validate]")
{
    Model    model;
    Print    print;
    DynamicPrintConfig config = base_spiral_test_config();
    config.set_deserialize_strict({ { "spiral_mode", true } });
    setup_spiral_print(print, model, config);
    REQUIRE(print.has_spiral_mode());
}

TEST_CASE("range spiral vase slicing on height bands", "[SpiralVase][slice]")
{
    Model    model;
    Print    print;
    const DynamicPrintConfig config = base_spiral_test_config();
    setup_spiral_print(print, model, config);
    ModelObject &object = *model.objects.front();
    const ModelConfig spiral_cfg = make_spiral_range_config();
    add_height_range(object, 5.0, 10.0, spiral_cfg);
    add_height_range(object, 12.0, 17.0, spiral_cfg);
    apply_print_config(print, model, config);
    print.process();

    const PrintObject &print_object = *print.objects().front();
    REQUIRE_FALSE(print_object.layers().empty());

    const Layer *in_band  = nullptr;
    const Layer *out_band = nullptr;
    for (const Layer *layer : print_object.layers()) {
        if (layer_z_in_height_range(layer->print_z, { 5.0, 10.0 }) && in_band == nullptr)
            in_band = layer;
        if (layer->print_z > 10.5 && layer->print_z < 11.5 && out_band == nullptr)
            out_band = layer;
    }
    REQUIRE(in_band != nullptr);
    REQUIRE(out_band != nullptr);
    REQUIRE(in_band->any_spiral_vase_active());
    REQUIRE_FALSE(out_band->any_spiral_vase_active());

    bool checked_spiral_geometry = false;
    for (const LayerRegion *layerm : in_band->regions()) {
        if (layerm->slices.empty())
            continue;
        REQUIRE(layerm->perimeters.items_count() == 1);
        REQUIRE(layerm->fills.items_count() == 0);
        checked_spiral_geometry = true;
    }
    REQUIRE(checked_spiral_geometry);
}
