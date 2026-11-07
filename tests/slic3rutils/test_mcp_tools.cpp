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

#include "python_test_support.hpp"

#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>
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

TEST_CASE("a search matches query words at the start of a text's words, CamelCase parts included", "[Mcp]")
{
    // Short words drop out once the query has a longer one.
    CHECK(query_words("Add a Brim to it") == std::vector<std::string>{"add", "brim"});
    CHECK(query_words("z") == std::vector<std::string>{"z"});

    const std::vector<std::string> words = query_words("mesh normals");
    // "mesh" starts the second part of TriangleMesh and "normals" a word of the name: both in the name.
    CHECK(match_score(words, "orca.host.TriangleMesh.face_normals", "Per-triangle normals.") == std::pair<int, int>{2, 2});
    // A word only the text has counts, but not as a name match.
    CHECK(match_score(words, "orca.host.ModelVolume.mesh", "Face normals are not cached.") == std::pair<int, int>{2, 1});
    CHECK(match_score(words, "orca.host.Model", "The project's objects.") == std::pair<int, int>{0, 0});
    // A word is matched at the start of a word, never inside one.
    CHECK(match_score(query_words("range"), "arrange", "") == std::pair<int, int>{0, 0});
    CHECK(match_score(query_words("retract"), "retraction_length", "") == std::pair<int, int>{1, 1});
}

TEST_CASE("a listing summary is a documentation's first sentence on one line", "[Mcp]")
{
    CHECK(first_sentence("Return the mesh.\nIt is a copy.") == "Return the mesh.");
    CHECK(first_sentence("A box, e.g. the bed's. More.") == "A box, e.g. the bed's.");
    CHECK(first_sentence("Version 2.5 of the\nformat") == "Version 2.5 of the format");
    CHECK(first_sentence("") == "");
}

TEST_CASE("python_api's reference covers the orca package with constructors and named instances", "[Mcp][Python]")
{
    (void) import_orca_module();
    const json                  reference = python_api_reference();
    std::map<std::string, json> by_name;
    for (const json& entry : reference)
        by_name[entry["name"].get<std::string>()] = entry;

    // The plugin classes and pages beside orca.host.
    CHECK(by_name.count("orca.pages") == 1);
    CHECK(by_name.count("orca.script.ScriptPluginCapabilityBase") == 1);
    // A class's signature is its constructors, and its documentation goes on with what they say.
    REQUIRE(by_name.count("orca.host.Point") == 1);
    CHECK(by_name["orca.host.Point"]["signature"].get<std::string>().rfind("Point(x: ", 0) == 0);
    REQUIRE(by_name.count("orca.host.ExPolygon") == 1);
    CHECK(by_name["orca.host.ExPolygon"]["doc"].get<std::string>().find("Orientation is normalized") != std::string::npos);
    // A constant, which has no docstring, points at the documentation that says what it means.
    REQUIRE(by_name.count("orca.host.ui.PD_CAN_ABORT") == 1);
    const json& abort = by_name["orca.host.ui.PD_CAN_ABORT"];
    CHECK(abort["kind"] == "constant");
    CHECK(abort["signature"].get<std::string>().rfind("PD_CAN_ABORT = ", 0) == 0);
    CHECK(abort["doc"].get<std::string>().find("orca.host.ui.create_progress_dialog") != std::string::npos);
    // An instance a module offers by name.
    REQUIRE(by_name.count("orca.host.preset.process") == 1);
    CHECK(by_name["orca.host.preset.process"]["kind"] == "attribute");
    // An enum's values are in its documentation, also where a module exports them.
    CHECK(by_name.count("orca.host.PresetType.Print") == 0);
    CHECK(by_name.count("orca.Analysis") == 0);
    // pybind11's signature lines are the signature, not documentation.
    REQUIRE(by_name.count("orca.host.TriangleMesh.face_normals") == 1);
    const json& normals = by_name["orca.host.TriangleMesh.face_normals"];
    CHECK(normals["signature"].get<std::string>().rfind("face_normals(self)", 0) == 0);
    CHECK(normals["doc"].get<std::string>().find("face_normals(") == std::string::npos);
    // An overloaded method's signature is its overloads, not pybind11's name(*args, **kwargs) before them.
    REQUIRE(by_name.count("orca.host.preset.PresetEditor.begin_update") == 1);
    const std::string overloads = by_name["orca.host.preset.PresetEditor.begin_update"]["signature"];
    CHECK(overloads.find("*args") == std::string::npos);
    CHECK(overloads.find("slot_number") != std::string::npos);
    // A static method has its own signature and documentation, not the staticmethod type's.
    REQUIRE(by_name.count("orca.ExecutionResult.success") == 1);
    const json& success = by_name["orca.ExecutionResult.success"];
    CHECK(success["signature"].get<std::string>().rfind("success(message: str", 0) == 0);
    CHECK(success["doc"].get<std::string>().find("Success result") != std::string::npos);

    CHECK(rank_python_api(reference, "mesh normals")[0]["name"] == "orca.host.TriangleMesh.face_normals");
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
