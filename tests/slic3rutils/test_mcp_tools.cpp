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
#include "slic3r/GUI/Mcp/WindowControl.hpp"
#include "slic3r/GUI/Camera.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"

#include <nlohmann/json.hpp>
#include <wx/gdicmn.h>
#include <wx/types.h>

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

TEST_CASE("search_python_api's reference covers the orca package with constructors and named instances", "[Mcp][Python]")
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
    // No required parameter follows one with a default, which Python could not declare; the handler may
    // still be passed by keyword alone.
    REQUIRE(by_name.count("orca.register_operation") == 1);
    CHECK(by_name["orca.register_operation"]["signature"].get<std::string>().find("schema: object = None, handler: object = None") !=
          std::string::npos);

    CHECK(rank_python_api(reference, "mesh normals")[0]["name"] == "orca.host.TriangleMesh.face_normals");
}

TEST_CASE("every tool of the catalog has the lane the concept gives it", "[Mcp]")
{
    for (const char* name : {"get_app_state", "wait", "cancel_job", "get_log", "run_replay", "export_replay", "answer_dialog",
                             "auto_answer_dialogs", "run_script", "search_python_api"})
        CHECK(tool_lane(name) == ToolLane::Core);
    for (const char* name : {"list_windows", "set_window", "list_widgets", "click", "set_widget_value", "press_keys", "mouse_gesture",
                             "pick_gizmo_file", "run_javascript", "set_view", "screenshot", "start_recording", "stop_recording",
                             "encode_recording", "search_actions", "run_action", "select_objects"})
        CHECK(tool_lane(name) == ToolLane::Ui);
    for (const char* name : {"set_settings", "slice", "generate_calibration_test", "list_cad_tools", "describe_cad_tool",
                             "call_cad_tool", "call_tool", "search_tools", "get_slice_stats", "plugin.example.measure"})
        CHECK(tool_lane(name) == ToolLane::Api);
}

TEST_CASE("a registered tool's description starts with its lane, once", "[Mcp]")
{
    ToolRegistry registry;
    ToolDef      def;
    def.name        = "click";
    def.description = "Click a widget.";
    def.read_only   = false;
    registry.add(def);
    const ToolDef* added = registry.find("click");
    REQUIRE(added != nullptr);
    CHECK(added->lane == ToolLane::Ui);
    CHECK(added->description == "[ui] Click a widget.");
    CHECK_FALSE(added->read_only);

    // A def copied from the registry and added again keeps one tag.
    registry.add(*added);
    CHECK(registry.find("click")->description == "[ui] Click a widget.");
}

TEST_CASE("set_window refuses a request the window can't take, before changing anything", "[Mcp]")
{
    WindowAbilities main;   main.main = true;
    WindowAbilities modal;  modal.modal = true; modal.can_maximize = true; modal.can_minimize = true;
    WindowAbilities fixed;  // a modeless window whose title bar offers neither
    WindowAbilities wayland; wayland.main = true; wayland.positions_known = false;

    CHECK(window_request_refusal(json::object(), main).find("size, position, state or raise") != std::string::npos);
    CHECK(window_request_refusal({{"state", "maximized"}, {"size", {800, 600}}}, main).find("maximized") != std::string::npos);
    CHECK(window_request_refusal({{"state", "minimized"}, {"position", {0, 0}}}, main).find("minimized") != std::string::npos);
    CHECK(window_request_refusal({{"state", "maximized"}}, main).empty());
    CHECK(window_request_refusal({{"state", "minimized"}}, main).empty());
    CHECK(window_request_refusal({{"state", "minimized"}}, modal).find("modal") != std::string::npos);
    CHECK(window_request_refusal({{"state", "maximized"}}, modal).empty());
    CHECK(window_request_refusal({{"state", "maximized"}}, fixed).find("maximize") != std::string::npos);
    CHECK(window_request_refusal({{"state", "normal"}}, fixed).empty());
    CHECK(window_request_refusal({{"size", {0, 600}}}, main).find("size") != std::string::npos);
    CHECK(window_request_refusal({{"size", {800}}}, main).find("[width, height]") != std::string::npos);
    CHECK(window_request_refusal({{"position", {0, 0}}}, wayland).find("X11") != std::string::npos);
    CHECK(window_request_refusal({{"position", {0, 0}}}, wayland).find("GDK_BACKEND=x11") != std::string::npos);
    CHECK(window_request_refusal({{"size", {800, 600}}, {"state", "maximized"}}, wayland).find("maximized") != std::string::npos);
    CHECK(window_request_refusal({{"state", "minimized"}}, wayland).empty());
    CHECK(window_request_refusal({{"raise", true}}, fixed).empty());
}

TEST_CASE("set_window's reply lists the fields the window changed, with a note on why", "[Mcp]")
{
    const json     sized{{"size", {800, 600}}};
    WindowSettling taken;
    taken.held  = true;
    taken.drawn = true;
    const auto note = [](const json& reply) { return reply.value("note", std::string()); };

    json reply{{"size", {800, 600}}, {"position", {10, 20}}, {"state", "normal"}};
    add_window_notes(sized, taken, reply);
    CHECK_FALSE(reply.contains("adjusted"));
    CHECK_FALSE(reply.contains("note"));

    // The window manager gave another size.
    reply = {{"size", {900, 600}}, {"state", "normal"}};
    add_window_notes(sized, taken, reply);
    CHECK(reply["adjusted"] == json::array({"size"}));
    CHECK(note(reply) == "the window manager adjusted the request");

    // A size limit held it.
    WindowSettling limited = taken;
    limited.limited        = true;
    reply                  = {{"size", {900, 600}}, {"state", "normal"}};
    add_window_notes(sized, limited, reply);
    CHECK(reply["adjusted"] == json::array({"size"}));
    CHECK(note(reply) == "size is held to the window's minimum or maximum size");

    // The window never held still in the state asked for.
    reply = {{"size", {800, 600}}, {"state", "maximized"}};
    add_window_notes({{"state", "normal"}}, WindowSettling(), reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply).find("the window manager did not apply the request") != std::string::npos);

    // The window held still, but its 3D view never drew at its size: the note says so, not the window manager.
    WindowSettling undrawn;
    undrawn.held    = true;
    undrawn.undrawn = "OrcaSlicer's window is not being drawn";
    reply           = {{"size", {800, 600}}, {"state", "normal"}};
    add_window_notes(sized, undrawn, reply);
    CHECK_FALSE(reply.contains("adjusted"));
    CHECK(note(reply).find("the 3D view did not draw") != std::string::npos);
    CHECK(note(reply).find(undrawn.undrawn) != std::string::npos);
    CHECK(note(reply).find("window manager") == std::string::npos);

    // The wait ran out: the note gives how long it lasted.
    WindowSettling slow;
    slow.seconds = 5;
    reply        = {{"size", {800, 600}}, {"state", "normal"}};
    add_window_notes({{"state", "minimized"}}, slow, reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply) == "the window manager did not apply the request within 5 s");

    // A Wayland compositor that refused to activate a minimized window: the note says so and how the person
    // brings it back, not that the window manager was slow.
    WindowSettling refused;
    refused.activation = true;
    reply              = {{"size", {800, 600}}, {"state", "minimized"}};
    add_window_notes({{"state", "normal"}}, refused, reply);
    CHECK(reply["adjusted"] == json::array({"state"}));
    CHECK(note(reply).find("the compositor did not bring the window back") != std::string::npos);
    CHECK(note(reply).find("taskbar") != std::string::npos);
    CHECK(note(reply).find("window manager") == std::string::npos);

    // An activation that brought the window back needs no note.
    WindowSettling activated = taken;
    activated.activation     = true;
    reply                    = {{"size", {800, 600}}, {"state", "normal"}};
    add_window_notes({{"state", "normal"}}, activated, reply);
    CHECK_FALSE(reply.contains("note"));
}

TEST_CASE("set_window sizes a window by what it shows, held to its limits", "[Mcp]")
{
    bool limited = true;
    // No invisible margins and no limits: the size asked for.
    CHECK(frame_size_for({800, 600}, {0, 0}, wxDefaultSize, wxDefaultSize, limited) == wxSize(800, 600));
    CHECK_FALSE(limited);
    // The margins of a client-side decorated window on Wayland go on top, so it shows the size asked for.
    CHECK(frame_size_for({800, 600}, {90, 90}, wxDefaultSize, wxDefaultSize, limited) == wxSize(890, 690));
    CHECK_FALSE(limited);
    // A minimum holds it, in wx's sizes, which count the margins: 836 x 539 shows 746 x 449.
    CHECK(frame_size_for({700, 400}, {90, 90}, wxSize(836, 539), wxDefaultSize, limited) == wxSize(836, 539));
    CHECK(limited);
    CHECK(frame_size_for({746, 449}, {90, 90}, wxSize(836, 539), wxDefaultSize, limited) == wxSize(836, 539));
    CHECK_FALSE(limited);
    // So does a maximum, on one side alone.
    CHECK(frame_size_for({2000, 600}, {0, 0}, wxDefaultSize, wxSize(1600, wxDefaultCoord), limited) == wxSize(1600, 600));
    CHECK(limited);
}

TEST_CASE("camera_state reads the camera set_view leaves", "[Mcp]")
{
    using Slic3r::Vec3d;
    using Slic3r::GUI::Camera;
    Camera camera;
    camera.set_viewport(0, 0, 800, 600);
    camera.set_scene_box(Slic3r::BoundingBoxf3(Vec3d(-100, -100, 0), Vec3d(100, 100, 100)));
    camera.set_type(Camera::EType::Ortho);
    camera.look_at(Vec3d(10, 20, 500), Vec3d(10, 20, 0), Vec3d::UnitY()); // straight down, +Y up the screen
    camera.set_zoom(2.0);
    const json s = camera_state(camera, 1.5);
    CHECK(s["type"] == "ortho");
    CHECK(s["target"] == json{10.0, 20.0, 0.0});
    CHECK(s["zoom"] == 2.0);
    CHECK(s["click_px_per_mm"] == 1.3333);
    CHECK(s["elevation"] == 90);
    CHECK(s["azimuth"] == -90);
    camera.look_at(Vec3d(10, -480, 0), Vec3d(10, 20, 0), Vec3d::UnitZ()); // from the front
    CHECK(camera_state(camera, 1.0)["elevation"] == 0);
    CHECK(camera_state(camera, 1.0)["azimuth"] == -90);
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
    CHECK(without_expected_ids("set_height_ranges", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    CHECK(without_expected_ids("set_variable_layer_height", json{{"object", 0}, {"object_id", 7}}) == json{{"object", 0}});
    const json replay{{"replay", {{"orca_replay", 1},
                                  {"steps", {{{"tool", "transform"}, {"args", {{"object", 1}, {"object_id", 9}}}},
                                             {{"tool", "run_script"}, {"args", {{"code", "x"}, {"args", {{"object_id", 9}}}}}}}}}}};
    const json recorded = without_expected_ids("run_replay", replay);
    CHECK(recorded["replay"]["steps"][0]["args"] == json{{"object", 1}});
    CHECK(recorded["replay"]["steps"][1]["args"]["args"]["object_id"] == 9);
    CHECK(without_expected_ids("run_script", json{{"code", "x"}, {"args", {{"object_id", 3}}}})["args"]["object_id"] == 3);
}

TEST_CASE("the journal records an inline replay given as JSON text as its object, without expected ids", "[Mcp]")
{
    const json as_text{{"replay", "// moves the lid\n"
                                  R"({"orca_replay": 1, "steps": [{"tool": "transform", "args": {"object": 1, "object_id": 9}}]})"}};
    const json recorded = without_expected_ids("run_replay", as_text);
    REQUIRE(recorded["replay"].is_object());
    CHECK(recorded["replay"]["steps"][0]["args"] == json{{"object", 1}});
    // Text that doesn't parse is the call's error; it is recorded as given.
    CHECK(without_expected_ids("run_replay", json{{"replay", "{\"orca_replay\": 1,"}}) == json{{"replay", "{\"orca_replay\": 1,"}});
}

TEST_CASE("the journal strips expected ids through call_tool and only from the tools that take them", "[Mcp]")
{
    CHECK(without_expected_ids("call_tool", json{{"tool", "transform"}, {"args", {{"object", 1}, {"object_id", 9}}}}) ==
          json{{"tool", "transform"}, {"args", {{"object", 1}}}});
    // A plugin operation's own fields of the same names are its arguments, recorded as given.
    CHECK(without_expected_ids("plugin.measure.area", json{{"object_id", 4}}) == json{{"object_id", 4}});
    CHECK(without_expected_ids("call_tool", json{{"tool", "plugin.measure.area"}, {"args", {{"object_id", 4}}}})["args"]["object_id"] == 4);
    CHECK(without_expected_ids("select_objects", json{{"objects", {0, 1}}, {"object_ids", {5, 6}}}) == json{{"objects", {0, 1}}});
    CHECK(without_expected_ids("select_objects", json{{"instances", {{0, 1}}}, {"instance_object_ids", {5}}}) == json{{"instances", {{0, 1}}}});
}
