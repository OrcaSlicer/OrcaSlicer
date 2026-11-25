#include <catch2/catch_test_macros.hpp>

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "../fff_print/test_helpers.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/TickCode.hpp"

#include <algorithm>
#include <vector>

using namespace Slic3r;

namespace {
// Exercise the same commands dispatched by the ImGui menu, without an OpenGL context.
struct Slider : GUI::IMSlider {
    using IMSlider::IMSlider;
    using IMSlider::add_custom_gcode;
    using IMSlider::add_code_as_tick;
    using IMSlider::delete_tick;
};
}

TEST_CASE("Preview layer edits stay with a copy through reordering and reslicing", "[InstanceLayerEvents][Preview]")
{
    Model model;
    Print print;
    const auto config = Test::multifilament_config(2, {{"print_sequence", "by object"}});
    Test::init_print({Test::cube(3)}, print, model, config);
    ModelObject *object = model.objects.front();
    object->name = "Cube";
    ModelInstance *a = object->instances.front();
    ModelInstance *b = object->add_instance();
    b->layer_events.gcodes = {{0.39, CustomGCode::Custom, 1, {}, "M117 B"},
                              {0.4, CustomGCode::PausePrint, 1, {}, {}},
                              {4.0, CustomGCode::Custom, 1, {}, "M117 above"}};
    GCodeProcessorResult result;
    result.instance_layer_ranges = {{a->id().id, 0, 1, {0.2, 0.4}}, {b->id().id, 2, 1, {0.2, 0.4}}};
    Slider slider(0, 3, 0, 3);
    slider.SetDrawMode(true);
    slider.SetExtruderColors({"#FFFFFF", "#123456"});
    slider.SetTickProject(model.id().id, 0);
    slider.SetSliderValues({0.2, 0.4, 0.2, 0.4});
    auto refresh = [&] { slider.SetSequentialTicks(model, print, result, {0, 1, 2, 3}, true); };
    refresh();
    CHECK(slider.instance_events().count(3) == 2);
    CHECK(slider.find_instance_layer({b->id().id, 4.0}) == -1);

    slider.SetHigherValue(1);
    slider.add_code_as_tick(CustomGCode::PausePrint);
    REQUIRE(slider.is_need_post_tick_event());
    const auto pause = slider.tick_change();
    CHECK(pause.model_id == model.id().id);
    CHECK(pause.instance_id == a->id().id);
    REQUIRE(pause.after);
    CHECK(pause.after->print_z == 0.4);

    // The wx event may be delivered after the user moves the slider or changes the order.
    std::swap(object->instances.front(), object->instances.back());
    std::swap(result.instance_layer_ranges[0].instance_id, result.instance_layer_ranges[1].instance_id);
    refresh();
    REQUIRE(pause.apply_to_instance(a->layer_events));
    REQUIRE(a->layer_events.gcodes.size() == 1);
    refresh();
    CHECK(slider.find_instance_layer({a->id().id, 0.4}) == 3);
    CHECK(slider.find_instance_layer({b->id().id, 0.4}) == 1);
    CHECK(slider.instance_events().count(3) == 1);

    slider.SetHigherValue(1);
    slider.add_custom_gcode("M117 edited B");
    const auto edit = slider.tick_change();
    REQUIRE(edit.instance_id == b->id().id);
    REQUIRE(edit.apply_to_instance(b->layer_events));
    CHECK(b->layer_events.gcodes.front().print_z == 0.39);
    CHECK(b->layer_events.gcodes.front().extra == "M117 edited B");
    CHECK_FALSE(edit.apply_to_instance(b->layer_events));
    // An edit and deletion can follow immediately while the pending slice is still invalid.
    slider.add_custom_gcode("M117 revised B");
    REQUIRE(slider.tick_change().before == edit.after);
    REQUIRE(slider.tick_change().apply_to_instance(b->layer_events));
    slider.delete_tick({1, CustomGCode::Custom});
    REQUIRE(slider.tick_change().apply_to_instance(b->layer_events));
    REQUIRE(slider.instance_events().count(1) == 1);
    CHECK(slider.instance_events().find(1)->second.type == CustomGCode::PausePrint);
    CHECK(b->layer_events.gcodes.back().extra == "M117 above");
    CHECK(a->layer_events.gcodes.front().type == CustomGCode::PausePrint);

    slider.SetHigherValue(0);
    slider.add_code_as_tick(CustomGCode::ToolChange, 2);
    const auto filament = slider.tick_change();
    REQUIRE(filament.instance_id == b->id().id);
    REQUIRE(filament.apply_to_instance(b->layer_events));
    CHECK(b->layer_events.mode == CustomGCode::MultiAsSingle);
    CHECK(b->layer_events.gcodes.front().extruder == 2);
    CHECK(a->layer_events.gcodes.size() == 1);

    // A file preview has chronological events, but no layer ranges from the slice.
    result.instance_layer_ranges.clear();
    refresh();
    CHECK(slider.instance_layer(1).instance_id == 0);
    slider.reset_post_tick_event();
    slider.add_custom_gcode("M117 no target");
    CHECK_FALSE(slider.is_need_post_tick_event());

    result.custom_gcode_per_print_z = {{0.4, CustomGCode::PausePrint, 1, {}, {}}};
    result.custom_gcode_layer_ids = {3};
    slider.SetSequentialTicks(model, print, result, {0, 1, 2, 3}, false);
    REQUIRE(slider.instance_events().size() == 1);
    CHECK(slider.instance_events().begin()->first == 3);
    CHECK(slider.instance_layer(3).instance_id == 0);

    // A travel layer inserted by a script sits between two editable layers of the same copy.
    result.instance_layer_ranges = {{a->id().id, 0, 1, {0.2}}, {a->id().id, 2, 2, {0.4}}, {b->id().id, 3, 1, {0.2}}};
    result.custom_gcode_layer_ids = {1};
    refresh();
    CHECK(slider.find_instance_layer({a->id().id, 0.39}) == 2);
    CHECK(slider.instance_layer(2).local_layer == 2);
    REQUIRE(slider.instance_events().count(1) == 1);
    CHECK(slider.instance_layer(1).instance_id == 0);
    slider.SetHigherValue(1);
    slider.reset_post_tick_event();
    slider.add_custom_gcode("M117 file-only layer");
    CHECK_FALSE(slider.is_need_post_tick_event());
    slider.SetDrawMode(false);
    CHECK(slider.instance_events().empty());
}
