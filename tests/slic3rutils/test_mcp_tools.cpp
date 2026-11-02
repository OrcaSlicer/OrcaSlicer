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

#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

TEST_CASE("setting values serialize the way their options read them back", "[Mcp]")
{
    CHECK(to_config_string(json(3)) == "3");
    CHECK(to_config_string(json(0.2)) == "0.2");
    CHECK(to_config_string(json(true)) == "1");
    CHECK(to_config_string(json("15%")) == "15%");
    CHECK(to_config_string(json::array({0.4, 0.6}), "nozzle_diameter") == "0.4,0.6");

    // A list of strings whose entries contain the separators survives the round trip.
    const std::vector<std::string> scripts{"/usr/bin/fix --mode=a,b", "second; with a semicolon"};
    Slic3r::ConfigOptionStrings    parsed;
    parsed.deserialize(to_config_string(json(scripts), "post_process"));
    CHECK(parsed.values == scripts);

    // Points as [x, y] pairs, or already in OrcaSlicer's "XxY" form.
    const json area = json::array({json::array({0, 0}), json::array({220.5, 0}), "220x220", json::array({0, 220})});
    Slic3r::ConfigOptionPoints points;
    REQUIRE(points.deserialize(to_config_string(area, "printable_area")));
    REQUIRE(points.values.size() == 4);
    CHECK(points.values[1] == Slic3r::Vec2d(220.5, 0));
    CHECK(points.values[2] == Slic3r::Vec2d(220, 220));
    CHECK(to_config_string(json::array({10, 20}), "bed_mesh_min") == "10x20");
    // A string is already in OrcaSlicer's format, list options included.
    CHECK(to_config_string(json("a.py;b.py"), "post_process") == "a.py;b.py");

    CHECK_THROWS_AS(to_config_string(json::object()), ToolFailure);
}

TEST_CASE("every tool of the catalog has the lane the concept gives it", "[Mcp]")
{
    for (const char* name : {"get_state", "wait", "cancel", "get_log", "run_script", "export_script", "dialog_respond",
                             "dialog_auto_answer", "run_python", "python_api"})
        CHECK(tool_lane(name) == ToolLane::Core);
    for (const char* name : {"ui_snapshot", "ui_click", "ui_input", "ui_key", "ui_mouse", "gizmo_file", "webview_eval", "set_view",
                             "screenshot", "start_recording", "stop_recording", "encode_recording", "list_actions", "run_action",
                             "select"})
        CHECK(tool_lane(name) == ToolLane::Ui);
    for (const char* name : {"set_settings", "slice", "cad_call", "api_call", "api_describe", "plugin.example.measure"})
        CHECK(tool_lane(name) == ToolLane::Api);
}

TEST_CASE("a registered tool's description starts with its lane, once", "[Mcp]")
{
    ToolRegistry registry;
    ToolDef      def;
    def.name        = "ui_click";
    def.description = "Click a widget.";
    def.read_only   = false;
    registry.add(def);
    const ToolDef* added = registry.find("ui_click");
    REQUIRE(added != nullptr);
    CHECK(added->lane == ToolLane::Ui);
    CHECK(added->description == "[ui] Click a widget.");
    CHECK_FALSE(added->read_only);

    // A def copied from the registry and added again keeps one tag.
    registry.add(*added);
    CHECK(registry.find("ui_click")->description == "[ui] Click a widget.");
}

TEST_CASE("a call naming an object's id is refused once its index names another object", "[Mcp]")
{
    Slic3r::Model model;
    Slic3r::ModelObject* first  = model.add_object();
    Slic3r::ModelObject* second = model.add_object();
    first->name                 = "bracket";
    second->name                = "lid";
    Slic3r::ModelVolume* part   = first->add_volume(Slic3r::TriangleMesh());

    CHECK_NOTHROW(check_expected_ids(*first, nullptr, json{{"object", 0}, {"object_id", first->id().id}}));
    CHECK_NOTHROW(check_expected_ids(*first, part, json{{"object", 0}, {"volume_id", part->id().id}}));
    // Without expected ids the index alone decides, as before.
    CHECK_NOTHROW(check_expected_ids(*second, nullptr, json{{"object", 0}}));
    try {
        check_expected_ids(*second, nullptr, json{{"object", 0}, {"object_id", first->id().id}});
        FAIL("the call was not refused");
    } catch (const ToolFailure& f) {
        CHECK(std::string(f.what()).find("lid") != std::string::npos);
        CHECK(f.details["id"] == second->id().id);
    }
    CHECK_THROWS_AS(check_expected_ids(*first, part, json{{"volume_id", part->id().id + 1000}}), ToolFailure);
}

TEST_CASE("the journal records indices without expected ids, except a Python step's arguments", "[Mcp]")
{
    CHECK(without_expected_ids("set_settings", json{{"object", 0}, {"object_id", 7}, {"volume_id", 8}}) == json{{"object", 0}});
    const json script{{"script", {{"orca_script", 1},
                                  {"steps", {{{"tool", "transform"}, {"args", {{"object", 1}, {"object_id", 9}}}},
                                             {{"tool", "run_python"}, {"args", {{"code", "x"}, {"args", {{"object_id", 9}}}}}}}}}}};
    const json recorded = without_expected_ids("run_script", script);
    CHECK(recorded["script"]["steps"][0]["args"] == json{{"object", 1}});
    CHECK(recorded["script"]["steps"][1]["args"]["args"]["object_id"] == 9);
    CHECK(without_expected_ids("run_python", json{{"code", "x"}, {"args", {{"object_id", 3}}}})["args"]["object_id"] == 3);
}

TEST_CASE("the journal records an inline script given as JSON text as its object, without expected ids", "[Mcp]")
{
    const json as_text{{"script", "// moves the lid\n"
                                  R"({"orca_script": 1, "steps": [{"tool": "transform", "args": {"object": 1, "object_id": 9}}]})"}};
    const json recorded = without_expected_ids("run_script", as_text);
    REQUIRE(recorded["script"].is_object());
    CHECK(recorded["script"]["steps"][0]["args"] == json{{"object", 1}});
    // Text that doesn't parse is the call's error; it is recorded as given.
    CHECK(without_expected_ids("run_script", json{{"script", "{\"orca_script\": 1,"}}) == json{{"script", "{\"orca_script\": 1,"}});
}

TEST_CASE("the journal strips expected ids through api_call and only from the tools that take them", "[Mcp]")
{
    CHECK(without_expected_ids("api_call", json{{"tool", "transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}) ==
          json{{"tool", "transform"}, {"args", {{"object", 1}}}});
    // A plugin operation's own fields of the same names are its arguments, recorded as given.
    CHECK(without_expected_ids("plugin.measure.area", json{{"object_id", 4}}) == json{{"object_id", 4}});
    CHECK(without_expected_ids("api_call", json{{"tool", "plugin.measure.area"}, {"args", {{"object_id", 4}}}})["args"]["object_id"] == 4);
    CHECK(without_expected_ids("select", json{{"objects", {0, 1}}, {"object_ids", {5, 6}}}) == json{{"objects", {0, 1}}});
    CHECK(without_expected_ids("select", json{{"instances", {{0, 1}}}, {"instance_object_ids", {5}}}) == json{{"instances", {{0, 1}}}});
}
