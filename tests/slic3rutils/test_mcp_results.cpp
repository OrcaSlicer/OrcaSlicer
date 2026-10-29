// The GUI headers reach wx's MSW headers after OCCT has hidden the USER and GDI declarations they
// need; the Windows headers first keep them, as in test_plugin_host_api.cpp.
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

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/GCodeStatistics.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libvgcode/include/Types.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Mcp/CanvasTarget.hpp"
#include "slic3r/GUI/Mcp/JobRegistry.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpScript.hpp"
#include "slic3r/GUI/Mcp/McpService.hpp"
#include "slic3r/GUI/Mcp/McpServicePtr.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/ModalTracker.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"
#include "slic3r/GUI/Mcp/UiInvoker.hpp"
#include "slic3r/GUI/Mcp/UiRefs.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/SceneRaycaster.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/dataview.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/frame.h>
#include <wx/gdicmn.h>
#include <wx/grid.h>
#include <wx/init.h>
#include <wx/menu.h>
#include <wx/modalhook.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/popupwin.h>
#include <wx/utils.h>
#include <wx/variant.h>
#include <wx/vector.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;

namespace {

// ImGui frames drawn without OpenGL, as in test_imgui_wrapper.cpp.
struct ImGuiFrames
{
    ImGuiWrapper imgui;

    ImGuiFrames()
    {
        imgui.set_display_size(800.f, 600.f);
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    template<class Draw> void frame(Draw draw)
    {
        ImGui::NewFrame();
        draw();
        ImGui::EndFrame();
    }
};

void window(const char* name, const ImVec2& pos, const ImVec2& size, ImGuiWindowFlags flags = 0)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(name, nullptr, flags | ImGuiWindowFlags_NoResize);
    ImGui::Text("text");
}

} // namespace

TEST_CASE("a click goes to the frontmost ImGui window that takes mouse input at its pixel", "[Mcp]")
{
    ImGuiFrames frames;
    // A gizmo panel with a child, a notification over its corner, and a full-canvas overlay drawn
    // last, in front of both, that takes no mouse input.
    const auto draw = [] {
        window("Cut##gizmo", ImVec2(100.f, 100.f), ImVec2(200.f, 200.f));
        ImGui::BeginChild("body", ImVec2(100.f, 100.f));
        ImGui::Text("body");
        ImGui::EndChild();
        ImGui::End();
        window("!!Ntfctn1", ImVec2(250.f, 250.f), ImVec2(200.f, 100.f));
        ImGui::End();
        window("overlay", ImVec2(0.f, 0.f), ImVec2(800.f, 600.f), ImGuiWindowFlags_NoInputs);
        ImGui::End();
    };
    frames.frame(draw);
    frames.frame(draw);

    const ImGuiWindow* in_panel = imgui_window_at(ImVec2(280.f, 120.f));
    REQUIRE(in_panel != nullptr);
    CHECK(std::string(in_panel->Name) == "Cut##gizmo");
    // The child is the window there; its panel is what owns it.
    const ImGuiWindow* in_child = imgui_window_at(ImVec2(150.f, 180.f));
    REQUIRE(in_child != nullptr);
    CHECK((in_child->Flags & ImGuiWindowFlags_ChildWindow) != 0);
    CHECK(in_child->RootWindow == in_panel);
    const ImGuiWindow* over_corner = imgui_window_at(ImVec2(275.f, 275.f));
    REQUIRE(over_corner != nullptr);
    CHECK(std::string(over_corner->Name) == "!!Ntfctn1");
    // Under the overlay alone, and beside everything: the scene.
    CHECK(imgui_window_at(ImVec2(500.f, 500.f)) == nullptr);
    CHECK(imgui_window_at(ImVec2(50.f, 50.f)) == nullptr);

    // A window the last frame no longer drew is gone.
    frames.frame([] {
        window("Cut##gizmo", ImVec2(100.f, 100.f), ImVec2(200.f, 200.f));
        ImGui::End();
    });
    CHECK(std::string(imgui_window_at(ImVec2(275.f, 275.f))->Name) == "Cut##gizmo");
    CHECK(imgui_window_at(ImVec2(400.f, 300.f)) == nullptr);
}

TEST_CASE("what covers a click is named by its title, or a notification by its text", "[Mcp]")
{
    std::vector<ImGuiWrapper::RecordedText> texts(2);
    texts[0].window = "Cut##gizmo";
    texts[0].text   = "Plane";
    texts[1].window = "!!Ntfctn3/##body_0A1B2C3D";
    texts[1].text   = "Exported successfully\nOpen folder";
    CHECK(imgui_window_description("Cut##gizmo", texts) == "the \"Cut\" window");
    CHECK(imgui_window_description("!!Ntfctn3", texts) == "the notification \"Exported successfully\"");
    CHECK(imgui_window_description("!!Ntfctn4", texts) == "a notification");
    CHECK(imgui_window_description("##overlay", texts) == "an ImGui window");

    texts[1].text = std::string(70, 'x');
    CHECK(imgui_window_description("!!Ntfctn3", texts) == "the notification \"" + std::string(57, 'x') + "...\"");
}

TEST_CASE("a modifier or negative part is a surface target, and see-through for other targets", "[Mcp]")
{
    // A 10 mm part, a negative part standing 5 mm out of its top, and a modifier above that.
    const auto mesh = std::make_shared<const TriangleMesh>(its_make_cube(10., 10., 10.));
    GLVolume   part, negative, modifier;
    for (GLVolume* v : {&part, &negative, &modifier})
        v->mesh_raycaster = std::make_unique<MeshRaycaster>(mesh);
    part.composite_id     = GLVolume::CompositeID(0, 0, 0);
    negative.composite_id = GLVolume::CompositeID(0, 1, 0);
    modifier.composite_id = GLVolume::CompositeID(0, 2, 0);
    negative.is_modifier = modifier.is_modifier = true;
    negative.set_volume_offset(Vec3d(2.5, 2.5, 5.));
    modifier.set_volume_offset(Vec3d(2.5, 2.5, 20.));
    const std::vector<GLVolume*> volumes{&part, &negative, &modifier};
    const auto                   no_gizmo = [](const GLVolume*) { return false; };

    const std::vector<SurfaceMesh> for_negative = surface_meshes(volumes, &negative, no_gizmo);
    REQUIRE(for_negative.size() == 2);
    CHECK_FALSE(for_negative[0].target);
    CHECK(for_negative[1].target);
    CHECK(for_negative[1].name == "object 0 instance 0 part 1");
    // The negative part's top, looked at from above, through the modifier.
    const std::pair<Vec3d, Vec3d> down(Vec3d(7.5, 7.5, 100.), -Vec3d::UnitZ());
    CHECK(check_surface(Vec3d(7.5, 7.5, 15.), down, for_negative, nullptr, {}, 0.1).empty());

    // For the part, both are see-through.
    const std::vector<SurfaceMesh> for_part = surface_meshes(volumes, &part, no_gizmo);
    REQUIRE(for_part.size() == 1);
    CHECK(for_part[0].target);
    CHECK(check_surface(Vec3d(7.5, 7.5, 10.), down, for_part, nullptr, {}, 0.1).empty());

    // An inactive part is hidden unless the open gizmo draws it.
    part.is_active = false;
    CHECK(surface_meshes(volumes, &negative, no_gizmo).size() == 1);
    CHECK(surface_meshes(volumes, &negative, [&](const GLVolume* v) { return v == &part; }).size() == 2);
}

TEST_CASE("a toolbar item reads the enabled state its callback gives now, not the last idle event's", "[Mcp]")
{
    GLToolbar           toolbar(GLToolbar::Normal, "test");
    toolbar.set_enabled(true); // as the canvas does; a disabled toolbar finds no items
    bool                can_edit = false;
    GLToolbarItem::Data data;
    data.name              = "layersediting";
    data.enabling_callback = [&can_edit] { return can_edit; };
    REQUIRE(toolbar.add_item(data));
    toolbar.update_items_state(); // the last idle event, before the selection
    REQUIRE(toolbar.get_item("layersediting")->is_disabled());

    can_edit     = true; // `select` picked an object
    bool changed = false;
    CHECK_FALSE(toolbar_items(toolbar, &changed).front()->is_disabled());
    CHECK(changed);
    toolbar_items(toolbar, &changed);
    CHECK_FALSE(changed);
}

TEST_CASE("an auto-answer rule presses a wx dialog's button, and answers a native one unseen", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    // A GUI app for the dialogs; wxInitializer uses one set before it, and deletes it.
    if (wxTheApp == nullptr)
        wxApp::SetInstance(new wxApp);
    wxInitializer init;
    REQUIRE(init.IsOk());
    {
        ModalTracker tracker;
        tracker.push_rules({make_rule("unsaved", "Transfer"), make_rule("discard", "no")});

        // The button's handler does the dialog's work, which ShowModal() returning its id would skip.
        wxDialog dialog(nullptr, wxID_ANY, "Unsaved changes");
        int      transfers = 0;
        auto*    transfer  = new wxButton(&dialog, wxID_ANY, "Transfer");
        transfer->Bind(wxEVT_BUTTON, [&transfers](wxCommandEvent&) { ++transfers; });
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE); // it opens
        CHECK(tracker.top() == &dialog);
        CHECK(transfers == 0);
        wxTheApp->ProcessPendingEvents(); // what its modal loop does first
        CHECK(transfers == 1);
        wxModalDialogHook::CallExit(&dialog);
        CHECK(tracker.top() == nullptr);
        CHECK(tracker.rules_json()[0]["uses"] == 1);

        // A modal opened above it before its loop ran leaves it to dialog_respond.
        wxDialog above(nullptr, wxID_ANY, "Other");
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        CHECK(wxModalDialogHook::CallEnter(&above) == wxID_NONE);
        wxTheApp->ProcessPendingEvents();
        CHECK(transfers == 1);
        wxModalDialogHook::CallExit(&above);
        wxModalDialogHook::CallExit(&dialog);

        // A native dialog has no handlers: ShowModal() returns the id without showing it.
        wxMessageDialog native(nullptr, "Discard the changes?", "Changes", wxYES_NO);
        CHECK(wxModalDialogHook::CallEnter(&native) == wxID_NO);
        CHECK(tracker.top() == nullptr);
    }
}

TEST_CASE("a plate summary lists every filament slot by use, and its totals count the flush", "[Mcp]")
{
    // Slots 1 and 3 of three print; slot 1 also prints the support. At 1.25 g/cm3, 1000 mm3 weigh 1.25 g.
    GCodeProcessorResult result;
    result.filaments_count    = 3;
    result.filament_diameters = {1.75f, 1.75f, 1.75f};
    result.filament_densities = {1.25f, 1.25f, 1.25f};
    result.filament_costs     = {40.f, 40.f, 40.f};
    PrintEstimatedStatistics& stats       = result.print_statistics;
    stats.model_volumes_per_extruder      = {{0, 1000.}, {2, 400.}};
    stats.support_volumes_per_extruder    = {{0, 80.}};
    stats.wipe_tower_volumes_per_extruder = {{0, 120.}, {2, 40.}};
    stats.flush_per_filament              = {{0, 200.}, {2, 160.}};
    stats.total_volumes_per_extruder      = {{0, 1400.}, {2, 600.}};
    // The processor's per-role sums leave the flush out: 1640 mm3, 2.05 g.
    stats.used_filaments_per_role = {{erExternalPerimeter, {0.58, 1.75}}, {erSupportMaterial, {0.03, 0.1}}, {erWipeTower, {0.07, 0.2}}};
    for (PrintEstimatedStatistics::Mode& mode : stats.modes)
        mode.reset(); // PrintEstimatedStatistics::reset() resets copies
    stats.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time  = 3725.4;
    stats.modes[size_t(PrintEstimatedStatistics::ETimeMode::Stealth)].time = 4102.;
    stats.total_filament_changes                                          = 3;
    // Two layers, at 0.2 and 0.4 mm.
    for (const auto& [layer, z, filament] : std::vector<std::tuple<unsigned int, float, unsigned char>>{{0, 0.2f, 0}, {0, 0.2f, 2}, {1, 0.4f, 0}, {1, 0.4f, 2}}) {
        GCodeProcessorResult::MoveVertex move;
        move.type           = EMoveType::Extrude;
        move.extrusion_role = erExternalPerimeter;
        move.extruder_id    = filament;
        move.layer_id       = layer;
        move.position       = Vec3f(0.f, 0.f, z);
        move.delta_extruder = 1.f;
        move.time[0]        = 1.f;
        result.moves.push_back(move);
    }
    using Catch::Matchers::WithinAbs;

    SECTION("a printer with a silent mode, on a single nozzle")
    {
        const nlohmann::json summary = plate_summary(nlohmann::json::parse(gcode_statistics_json(result)), result);
        CHECK(summary["time_s"] == 3725);
        CHECK(summary["silent_time_s"] == 4102);
        CHECK_FALSE(summary.contains("extruder_changes"));
        CHECK(summary["layers"] == 2);
        CHECK_THAT(summary["max_z"].get<double>(), WithinAbs(0.4, 1e-6));
        CHECK(summary["filament_changes"] == 3);
        CHECK_THAT(summary["filament_cost"].get<double>(), WithinAbs(0.1, 1e-9));

        // 2000 mm3 with the flush: 2.5 g, the per-role sums and the 0.45 g of flush.
        CHECK_THAT(summary["filament_g"].get<double>(), WithinAbs(2.5, 1e-9));
        double role_grams = 0.;
        for (const auto& [role, used] : stats.used_filaments_per_role)
            role_grams += used.second;
        CHECK_THAT(summary["filament_g"].get<double>(), WithinAbs(role_grams + 0.45, 1e-9));
        // 2000 mm3 of 1.75 mm filament are 0.83 m.
        CHECK_THAT(summary["filament_m"].get<double>(), WithinAbs(0.83, 1e-9));

        const nlohmann::json& filaments = summary["filaments"];
        REQUIRE(filaments.size() == 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(filaments[i]["slot"] == i + 1);
        CHECK_THAT(filaments[0]["g"].get<double>(), WithinAbs(1.75, 1e-9));
        CHECK_THAT(filaments[0]["model_g"].get<double>(), WithinAbs(1.25, 1e-9));
        CHECK_THAT(filaments[0]["support_g"].get<double>(), WithinAbs(0.1, 1e-9));
        CHECK_THAT(filaments[0]["tower_g"].get<double>(), WithinAbs(0.15, 1e-9));
        CHECK_THAT(filaments[0]["flush_g"].get<double>(), WithinAbs(0.25, 1e-9));
        CHECK_THAT(filaments[0]["m"].get<double>(), WithinAbs(0.58, 1e-9));
        // An unused slot reads 0 in every use.
        for (const char* key : {"m", "g", "model_g", "support_g", "flush_g", "tower_g"})
            CHECK(filaments[1][key].get<double>() == 0.);
        CHECK_THAT(filaments[2]["g"].get<double>(), WithinAbs(0.75, 1e-9));
        CHECK_THAT(filaments[2]["flush_g"].get<double>(), WithinAbs(0.2, 1e-9));
        CHECK_THAT(filaments[2]["tower_g"].get<double>(), WithinAbs(0.05, 1e-9));
    }

    SECTION("a printer without a silent mode, with two nozzles")
    {
        stats.modes[size_t(PrintEstimatedStatistics::ETimeMode::Stealth)].time = 0.;
        stats.total_extruder_changes                                          = 2;
        const nlohmann::json summary = plate_summary(nlohmann::json::parse(gcode_statistics_json(result)), result);
        CHECK_FALSE(summary.contains("silent_time_s"));
        CHECK(summary["extruder_changes"] == 2);
    }

    SECTION("a slot past the project's count, as a G-code file's header may give")
    {
        result.filaments_count = 1;
        const nlohmann::json summary = plate_summary(nlohmann::json::parse(gcode_statistics_json(result)), result);
        REQUIRE(summary["filaments"].size() == 3);
        CHECK(summary["filaments"][2]["slot"] == 3);
        CHECK_THAT(summary["filaments"][2]["g"].get<double>(), WithinAbs(0.75, 1e-9));
    }
}

namespace {

// Three layers of moves: an object's first two at 0.2 and 0.4 mm, then the first of the next object of a
// by-object print, back at 0.2 mm. Each has a 2 s outer wall and a 1 s travel; the first also 3 s of
// sparse infill. 20 s in all, as retractions, Z moves and the prepare time count too.
void fill_preview_result(GCodeProcessorResult& result)
{
    result.filaments_count    = 1;
    result.filament_diameters = {1.75f};
    result.filament_densities = {1.25f};
    result.filament_costs     = {20.f};
    PrintEstimatedStatistics& stats = result.print_statistics;
    for (PrintEstimatedStatistics::Mode& mode : stats.modes)
        mode.reset();
    stats.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time         = 20.;
    stats.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].prepare_time = 2.f;
    stats.total_travel_distance                                                   = 123.44f;
    const auto add = [&result](unsigned int layer, float z, EMoveType type, ExtrusionRole role, float seconds) {
        GCodeProcessorResult::MoveVertex move;
        move.type           = type;
        move.extrusion_role = role;
        move.layer_id       = layer;
        move.position       = Vec3f(0.f, 0.f, z);
        move.delta_extruder = type == EMoveType::Extrude ? 10.f : 0.f;
        move.time[0]        = seconds;
        result.moves.push_back(move);
    };
    for (const auto& [layer, z] : std::vector<std::pair<unsigned int, float>>{{0, 0.2f}, {1, 0.4f}, {2, 0.2f}}) {
        add(layer, z, EMoveType::Extrude, erExternalPerimeter, 2.f);
        if (layer == 0)
            add(layer, z, EMoveType::Extrude, erInternalInfill, 3.f);
        add(layer, z + 0.4f, EMoveType::Travel, erNone, 1.f);
    }
}

} // namespace

TEST_CASE("preview features carry their share of the print time, and a travel row", "[Mcp]")
{
    using Catch::Matchers::WithinAbs;
    GCodeProcessorResult result;
    fill_preview_result(result);
    const nlohmann::json features = preview_features(nlohmann::json::parse(gcode_statistics_json(result)));

    REQUIRE(features.size() == 3);
    CHECK_THAT(features["Outer wall"]["time_s"].get<double>(), WithinAbs(6., 1e-9));
    CHECK_THAT(features["Outer wall"]["percent"].get<double>(), WithinAbs(30., 1e-9));
    CHECK_THAT(features["Sparse infill"]["time_s"].get<double>(), WithinAbs(3., 1e-9));
    CHECK_THAT(features["Sparse infill"]["percent"].get<double>(), WithinAbs(15., 1e-9));
    // Each 10 mm of 1.75 mm filament at 1.25 g/cm3 weighs 0.03 g.
    CHECK_THAT(features["Outer wall"]["g"].get<double>(), WithinAbs(0.09, 1e-9));
    CHECK_THAT(features["Sparse infill"]["g"].get<double>(), WithinAbs(0.03, 1e-9));
    CHECK_THAT(features["Travel"]["time_s"].get<double>(), WithinAbs(3., 1e-9));
    CHECK_THAT(features["Travel"]["mm"].get<double>(), WithinAbs(123.4, 1e-9));
    CHECK_FALSE(features["Travel"].contains("g"));

    // A share is the row's time over the print time; the rows leave out what is neither extrusion nor travel.
    double shares = 0.;
    for (const auto& row : features.items()) {
        CHECK_THAT(row.value()["percent"].get<double>(), WithinAbs(row.value()["time_s"].get<double>() / 20. * 100., 0.05));
        shares += row.value()["percent"].get<double>();
    }
    CHECK(shares <= 100.);
    CHECK_THAT(shares, WithinAbs(60., 1e-9));
}

TEST_CASE("preview layers count from 1, with each height above the layer below", "[Mcp]")
{
    using Catch::Matchers::WithinAbs;
    GCodeProcessorResult result;
    fill_preview_result(result);
    const nlohmann::json statistics = nlohmann::json::parse(gcode_statistics_json(result));

    const nlohmann::json layers = preview_layers(statistics, false);
    REQUIRE(layers.size() == 3);
    for (size_t i = 0; i < 3; ++i)
        CHECK(layers[i]["layer"] == i + 1);
    CHECK_THAT(layers[0]["z"].get<double>(), WithinAbs(0.2, 1e-6));
    CHECK_THAT(layers[0]["height"].get<double>(), WithinAbs(0.2, 1e-9));
    CHECK_THAT(layers[1]["height"].get<double>(), WithinAbs(0.2, 1e-9));
    // The next object of a by-object print starts again from the bed.
    CHECK_THAT(layers[2]["z"].get<double>(), WithinAbs(0.2, 1e-6));
    CHECK_THAT(layers[2]["height"].get<double>(), WithinAbs(0.2, 1e-9));
    // A layer takes the time of all its moves, travel included.
    CHECK_THAT(layers[0]["time_s"].get<double>(), WithinAbs(6., 1e-9));
    CHECK_THAT(layers[1]["time_s"].get<double>(), WithinAbs(3., 1e-9));
    CHECK_THAT(layers[0]["g"].get<double>(), WithinAbs(0.06, 1e-3));
    CHECK_FALSE(layers[0].contains("features"));

    SECTION("with each feature type's seconds")
    {
        const nlohmann::json with = preview_layers(statistics, true);
        CHECK(with[0]["features"] == nlohmann::json{{"Outer wall", 2.}, {"Sparse infill", 3.}});
        CHECK(with[1]["features"] == nlohmann::json{{"Outer wall", 2.}});
    }

    SECTION("paged")
    {
        const nlohmann::json first = page(layers, {{"limit", 2}});
        CHECK(first["total"] == 3);
        CHECK(first["items"].size() == 2);
        CHECK(first["next_offset"] == 2);
        const nlohmann::json rest = page(layers, {{"limit", 2}, {"offset", 2}});
        REQUIRE(rest["items"].size() == 1);
        CHECK(rest["items"][0]["layer"] == 3);
        CHECK_FALSE(rest.contains("next_offset"));
    }
}

TEST_CASE("a preview object carries its list_objects index only when its name is unique", "[Mcp]")
{
    using Catch::Matchers::WithinAbs;
    GCodeProcessorResult result;
    fill_preview_result(result);
    // Two instances of a cube, with 0.2 g of support under the first, and a benchy. Masses are in mg.
    const auto mass = [](double milligrams) {
        GCodeProcessorResult::ObjectMass object;
        GCodeProcessorResult::ObjectMass::Sum sum;
        sum.mass = milligrams;
        object.printed_up_to_layer.push_back(sum);
        return object;
    };
    for (const auto& [name, instance, milligrams] : std::vector<std::tuple<std::string, int, double>>{{"Cube", 0, 1500.}, {"Cube", 1, 1500.}, {"Benchy", 0, 12340.}}) {
        result.object_masses.push_back(mass(milligrams));
        result.object_masses.back().name     = name;
        result.object_masses.back().instance = instance;
    }
    result.support_masses.push_back(mass(200.));
    const nlohmann::json statistics = nlohmann::json::parse(gcode_statistics_json(result));

    const nlohmann::json objects = preview_objects(statistics, {"Benchy", "Cube"});
    REQUIRE(objects.size() == 3);
    CHECK(objects[0]["name"] == "Cube");
    CHECK(objects[0]["instance"] == 0);
    CHECK(objects[0]["object"] == 1);
    CHECK_THAT(objects[0]["g"].get<double>(), WithinAbs(1.5, 1e-9));
    CHECK_THAT(objects[0]["support_g"].get<double>(), WithinAbs(0.2, 1e-9));
    CHECK(objects[1]["instance"] == 1);
    CHECK(objects[1]["object"] == 1);
    CHECK(objects[1]["support_g"].get<double>() == 0.);
    CHECK(objects[2]["object"] == 0);
    CHECK_THAT(objects[2]["g"].get<double>(), WithinAbs(12.34, 1e-9));

    // Two objects named Cube, and none named Benchy: neither name says which object it is.
    const nlohmann::json ambiguous = preview_objects(statistics, {"Cube", "Cube"});
    for (const nlohmann::json& object : ambiguous)
        CHECK_FALSE(object.contains("object"));
}

TEST_CASE("set_view's legend keys name one feature type or option each, as get_preview lists them", "[Mcp]")
{
    // A feature type goes by the English name of its ;FEATURE: tag, which names no other role.
    std::set<std::string> features;
    for (std::size_t i = 0; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i) {
        const auto        role = libvgcode::EGCodeExtrusionRole(i);
        const std::string name = legend_feature_key(role);
        CAPTURE(i, name);
        CHECK_FALSE(name.empty());
        CHECK(libvgcode::convert(ExtrusionEntity::string_to_role(name)) == role);
        features.insert(name);
    }
    CHECK(features.size() == libvgcode::GCODE_EXTRUSION_ROLES_COUNT);

    // Every option the legend can list has a key of its own, which is no feature type's name.
    std::set<std::string> keys;
    for (const libvgcode::EOptionType type :
         {libvgcode::EOptionType::Travels, libvgcode::EOptionType::Wipes, libvgcode::EOptionType::Retractions,
          libvgcode::EOptionType::Unretractions, libvgcode::EOptionType::Seams, libvgcode::EOptionType::ToolChanges,
          libvgcode::EOptionType::ColorChanges, libvgcode::EOptionType::PausePrints, libvgcode::EOptionType::CustomGCodes}) {
        const char* key = legend_option_key(type);
        REQUIRE(key != nullptr);
        CHECK(features.count(key) == 0);
        keys.insert(key);
    }
    CHECK(keys.size() == 9);
    CHECK(std::string(legend_option_key(libvgcode::EOptionType::Travels)) == "travel");
    CHECK(std::string(legend_option_key(libvgcode::EOptionType::ToolChanges)) == "filament_changes");
}

TEST_CASE("a height picks the first layer whose top is at or above it, as the slider places a custom G-code", "[Mcp]")
{
    // The tops of four 0.2 mm layers, as the Preview gives the layer slider.
    IMSlider layers(0, 3, 0, 3);
    layers.SetSliderValues({0.2, 0.4, 0.6, 0.8});
    CHECK(layers.get_tick_from_value(0.0, true) == 0);
    CHECK(layers.get_tick_from_value(0.4, true) == 1);
    // A height a hair above a top, from float arithmetic, is still that layer.
    CHECK(layers.get_tick_from_value(0.1 + 0.2 + 0.1, true) == 1);
    // Between two tops: the layer that covers that height.
    CHECK(layers.get_tick_from_value(0.45, true) == 2);
    CHECK(layers.get_tick_from_value(0.8, true) == 3);
    // Above the top layer: no layer, which set_view refuses.
    CHECK(layers.get_tick_from_value(0.81, true) == -1);
}

namespace {

// A popup menu as the inspector walks it: a submenu's entry before its items.
std::vector<MenuEntry> sample_menu()
{
    auto entry = [](std::string label, int parent, bool submenu = false) {
        MenuEntry e;
        e.label   = std::move(label);
        e.parent  = parent;
        e.submenu = submenu;
        return e;
    };
    std::vector<MenuEntry> entries{entry("Add Part", -1, true),       entry("Cube", 0),         entry("Load...", 0),
                                   entry("Change Filament", -1, true), entry("Default", 3),      entry("Generic PLA (current)", 3),
                                   entry("Generic PLA", 3),            entry("Printable", -1),   entry("Fix Model", -1),
                                   entry("Delete", -1)};
    for (int i = 0; i < int(entries.size()); ++i)
        entries[i].id = 100 + i;
    entries[7].checkable = entries[7].checked = true;
    entries[8].enabled                        = false;
    name_menu_entries(entries, " (current)");
    return entries;
}

const MenuEntry* find(const std::vector<MenuEntry>& entries, const std::string& path, std::string& err)
{
    nlohmann::json details;
    return find_menu_entry(entries, path, " (current)", err, details);
}

// Runs each posted closure at once, as the UI thread would.
class InlineDispatcher final : public UiDispatcher
{
public:
    void post(std::function<void()> fn) override { fn(); }
};

} // namespace

TEST_CASE("a menu item is found by its label path, case and trailing dots aside", "[Mcp]")
{
    const std::vector<MenuEntry> entries = sample_menu();
    std::string                  err;
    const MenuEntry*             e = find(entries, "add part>cube", err);
    REQUIRE(e != nullptr);
    CHECK(e->path == "Add Part>Cube");
    e = find(entries, "Add Part>Load", err);
    REQUIRE(e != nullptr);
    CHECK(e->id == 102);
    CHECK(find(entries, "Add Part > Cube", err) != nullptr);
    CHECK(find(entries, "Add Part>Sphere", err) == nullptr);
    CHECK(err == "The menu has no item \"Add Part>Sphere\"");
    CHECK(find(entries, "Cube", err) == nullptr);
}

TEST_CASE("a repeated label is numbered #2, and the item in use is matched without its marker", "[Mcp]")
{
    const std::vector<MenuEntry> entries = sample_menu();
    CHECK(entries[5].path == "Change Filament>Generic PLA (current)");
    CHECK(entries[6].path == "Change Filament>Generic PLA #2");
    std::string err;
    // A path recorded while another slot was in use still names the same items.
    CHECK(find(entries, "Change Filament>Generic PLA", err) == &entries[5]);
    CHECK(find(entries, "Change Filament>Generic PLA (current)", err) == &entries[5]);
    CHECK(find(entries, "Change Filament>Generic PLA #2", err) == &entries[6]);
    CHECK(find(entries, "change filament>generic pla (current) #2", err) == &entries[6]);
    CHECK(find(entries, "Change Filament>Generic PLA #3", err) == nullptr);
    // A label of its own that ends in " #2" is found by itself.
    std::vector<MenuEntry> own(2);
    own[0].label = "Plate #2";
    own[1].label = "Plate";
    name_menu_entries(own, " (current)");
    CHECK(find(own, "Plate #2", err) == &own[0]);
}

TEST_CASE("a submenu path is refused and the error names its items", "[Mcp]")
{
    const std::vector<MenuEntry> entries = sample_menu();
    std::string                  err;
    nlohmann::json               details;
    CHECK(find_menu_entry(entries, "Add Part", " (current)", err, details) == nullptr);
    CHECK(err == "\"Add Part\" is a submenu; name one of its items");
    CHECK(details["items"] == nlohmann::json({"Add Part>Cube", "Add Part>Load..."}));
}

TEST_CASE("a disabled item is reported as disabled, not run", "[Mcp]")
{
    const std::vector<MenuEntry> entries = sample_menu();
    std::string                  err;
    nlohmann::json               details;
    CHECK(find_menu_entry(entries, "Fix Model", " (current)", err, details) == nullptr);
    CHECK(err == "\"Fix Model\" is disabled here");
    CHECK(details["disabled"] == nlohmann::json({"Fix Model"}));
    // Under a disabled submenu every item is disabled.
    std::vector<MenuEntry> nested(2);
    nested[0].label   = "More";
    nested[0].submenu = true;
    nested[0].enabled = false;
    nested[1].label   = "Export";
    nested[1].parent  = 0;
    nested[1].enabled = false; // as the inspector's walk sets it
    name_menu_entries(nested, " (current)");
    CHECK(find_menu_entry(nested, "More>Export", " (current)", err, details) == nullptr);
}

TEST_CASE("a menu listing names disabled and checked items apart from the item paths", "[Mcp]")
{
    const nlohmann::json listing = menu_listing(sample_menu());
    CHECK(listing["items"] == nlohmann::json({"Add Part>Cube", "Add Part>Load...", "Change Filament>Default",
                                              "Change Filament>Generic PLA (current)", "Change Filament>Generic PLA #2", "Printable",
                                              "Fix Model", "Delete"}));
    CHECK(listing["disabled"] == nlohmann::json({"Fix Model"}));
    CHECK(listing["checked"] == nlohmann::json({"Printable"}));
    // Without any, the lists are left out.
    std::vector<MenuEntry> plain(1);
    plain[0].label = "Delete";
    name_menu_entries(plain, " (current)");
    CHECK(menu_listing(plain) == nlohmann::json({{"items", {"Delete"}}}));
}

TEST_CASE("a click that only listed a menu is dropped from the script when the next call picks an item of it", "[Mcp]")
{
    const nlohmann::json click{{"action", "click"}, {"button", "right"}, {"at", {{"object", 0}, {"spot", "top"}}}};
    nlohmann::json       pick = click;
    pick["popup_item"]        = "Add Modifier>Cylinder";
    const ToolResult listed   = ToolResult::ok({{"popup_menu", {{"items", {"Add Modifier>Cylinder"}}}}});
    const ToolResult ran      = ToolResult::ok({{"ran", "Add Modifier>Cylinder"}});
    auto steps = [](Journal& journal) { return journal.to_script("t")["steps"]; };

    Journal journal;
    journal.end(journal.begin("ui_mouse", click), listed);
    journal.end(journal.begin("ui_mouse", pick), ran);
    REQUIRE(steps(journal).size() == 1);
    CHECK(steps(journal)[0]["args"] == pick);

    // A failed pick in between does not count; the listing click then stays, as it changed the selection.
    Journal kept;
    kept.end(kept.begin("ui_mouse", click), listed);
    kept.end(kept.begin("ui_mouse", pick), ToolResult::error("The menu has no item"));
    CHECK(steps(kept).size() == 1);
    CHECK(steps(kept)[0]["args"] == click);
    // Nor does a pick after another call, or one on another target.
    Journal other;
    other.end(other.begin("ui_mouse", click), listed);
    other.end(other.begin("select", {{"objects", {0}}}), ToolResult::ok());
    other.end(other.begin("ui_mouse", pick), ran);
    CHECK(steps(other).size() == 3);
    nlohmann::json elsewhere = pick;
    elsewhere["at"]["spot"]  = "front";
    Journal moved;
    moved.end(moved.begin("ui_mouse", click), listed);
    moved.end(moved.begin("ui_mouse", elsewhere), ran);
    CHECK(steps(moved).size() == 2);
}

TEST_CASE("the UI activity hook brackets each closure that runs, not one its guard refuses", "[Mcp]")
{
    InlineDispatcher                          dispatcher;
    bool                                      modal = false;
    std::vector<std::pair<std::string, bool>> seen;
    UiInvoker invoker(
        dispatcher, [&] { GuardState g; g.modal = modal; return g; }, {}, {},
        [&seen](const std::string& tool, bool running) { seen.emplace_back(tool, running); });
    invoker.run(GuardClass::Mutate, "ui_click", [&] {
        invoker.run(GuardClass::Inspect, "get_state", [] { return nlohmann::json(); });
        return nlohmann::json();
    });
    CHECK(seen == std::vector<std::pair<std::string, bool>>{{"ui_click", true}, {"get_state", true}, {"get_state", false}, {"ui_click", false}});
    seen.clear();
    // A closure that throws still ends; one refused before it runs never starts.
    CHECK_THROWS_AS(invoker.run(GuardClass::Mutate, "ui_key", []() -> nlohmann::json { throw ToolFailure("no"); }), ToolFailure);
    CHECK(seen == std::vector<std::pair<std::string, bool>>{{"ui_key", true}, {"ui_key", false}});
    seen.clear();
    modal = true;
    CHECK_THROWS_AS(invoker.run(GuardClass::Mutate, "select", [] { return nlohmann::json(); }), ToolFailure);
    CHECK(seen.empty());
}

TEST_CASE("a menu opened while a call acts on the UI is listed, run by its path or closed unseen, never shown", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    if (wxTheApp == nullptr)
        wxApp::SetInstance(new wxApp);
    wxInitializer init;
    REQUIRE(init.IsOk());
    {
        McpServicePtr service(new McpService());
        UiInspector   inspector(*service);
        inspector.capture_menus(true);

        // A menu as MenuFactory builds one: handlers on the menu, item states from wxEVT_UPDATE_UI
        // handlers on the window the menu pops up from.
        wxFrame frame(nullptr, wxID_ANY, "menus");
        wxMenu  menu;
        auto*   add = new wxMenu;
        add->Append(1001, "Cube");
        add->Append(1002, "Cylinder");
        menu.AppendSubMenu(add, "Add &Part");
        auto* filaments = new wxMenu;
        filaments->Append(1003, "Generic PLA");
        filaments->Append(1004, "Generic PLA");
        menu.AppendSubMenu(filaments, "Change Filament");
        menu.AppendCheckItem(1005, "Printable");
        menu.Check(1005, true);
        menu.Append(1006, "Fix Model");
        menu.Append(1007, "Delete\tDel");
        std::vector<int> ran;
        int              printable = -1;
        menu.Bind(wxEVT_MENU, [&ran, &printable](wxCommandEvent& e) {
            ran.push_back(e.GetId());
            if (e.GetId() == 1005)
                printable = e.GetInt();
        });
        frame.Bind(wxEVT_UPDATE_UI, [](wxUpdateUIEvent& e) { e.Enable(false); }, 1006);

        // A person's menu, with no call acting on the UI, is shown, not taken.
        CHECK_FALSE(inspector.on_menu(menu, frame));

        inspector.ui_activity("ui_click", true);
        int token = inspector.arm_menu("");
        CHECK(popup_menu(frame, menu));
        nlohmann::json out = inspector.disarm_menu(token);
        CHECK(out["popup_menu"]["items"] == nlohmann::json({"Add Part>Cube", "Add Part>Cylinder", "Change Filament>Generic PLA",
                                                            "Change Filament>Generic PLA #2", "Printable", "Fix Model", "Delete"}));
        CHECK(out["popup_menu"]["disabled"] == nlohmann::json({"Fix Model"}));
        CHECK(out["popup_menu"]["checked"] == nlohmann::json({"Printable"}));
        CHECK(ran.empty());

        // Run by path: the submenu's item, through the menu's handler; a check item toggles first.
        token = inspector.arm_menu("change filament>generic pla #2");
        CHECK(popup_menu(frame, menu));
        CHECK(inspector.disarm_menu(token) == nlohmann::json({{"ran", "Change Filament>Generic PLA #2"}}));
        CHECK(ran == std::vector<int>{1004});
        token = inspector.arm_menu("Printable");
        CHECK(popup_menu(frame, menu));
        inspector.disarm_menu(token);
        CHECK(printable == 0);
        CHECK_FALSE(menu.IsChecked(1005));

        // Items it can't run, and a menu that never opened.
        for (const char* path : {"Add Part", "Fix Model", "Sphere"}) {
            token = inspector.arm_menu(path);
            CHECK(popup_menu(frame, menu));
            CHECK_THROWS_AS(inspector.disarm_menu(token), ToolFailure);
        }
        token = inspector.arm_menu("Delete");
        CHECK_THROWS_AS(inspector.disarm_menu(token), ToolFailure);
        CHECK(ran == std::vector<int>{1004, 1005});

        // A second menu in one call is closed unseen and counted.
        token = inspector.arm_menu("");
        CHECK(popup_menu(frame, menu));
        CHECK(popup_menu(frame, menu));
        CHECK(inspector.disarm_menu(token)["menus_closed"] == 1);
        CHECK(inspector.closed_menus().size() == 1);
        inspector.ui_activity("ui_click", false);

        // A menu from a call that armed nothing is closed unseen, until the first idle after the call.
        inspector.ui_activity("cad_call", true);
        inspector.ui_activity("cad_call", false);
        CHECK(popup_menu(frame, menu));
        const nlohmann::json closed = inspector.closed_menus();
        REQUIRE(closed.size() == 2);
        CHECK(closed[1]["tool"] == "cad_call");
        CHECK(closed[1]["items"].size() == 7);
        wxTheApp->ProcessPendingEvents();
        wxIdleEvent idle;
        wxTheApp->ProcessEvent(idle);
        CHECK_FALSE(inspector.on_menu(menu, frame));

        // Past a second a person's menu shows even when no idle came, as while events keep queuing.
        inspector.ui_activity("cad_call", true);
        inspector.ui_activity("cad_call", false);
        wxMilliSleep(1100);
        CHECK_FALSE(inspector.on_menu(menu, frame));

        // Only menus of the event loop the call ran in are taken: a person's right click in a dialog's
        // loop shows its menu.
        inspector.ui_activity("ui_click", true);
        {
            wxEventLoop          dialog_loop;
            wxEventLoopActivator inside(&dialog_loop);
            CHECK_FALSE(inspector.on_menu(menu, frame));
        }
        CHECK(inspector.on_menu(menu, frame));
        // get_state keeps the last three.
        CHECK(inspector.on_menu(menu, frame));
        CHECK(inspector.closed_menus().size() == 3);
        CHECK(inspector.closed_menus()[2]["tool"] == "ui_click");
        inspector.ui_activity("ui_click", false);
        inspector.capture_menus(false);
    }
}

TEST_CASE("a row locator matches the last segments of a row's path", "[Mcp]")
{
    const std::vector<std::string> path{"Plate 1", "Cube", "Cylinder"};
    CHECK(row_segments("Plate 1>Cube>Cylinder") == path);
    CHECK(row_segments(nlohmann::json{"Cube", "A>B"}) == std::vector<std::string>{"Cube", "A>B"});
    CHECK_THROWS_AS(row_segments(""), ToolFailure);
    CHECK_THROWS_AS(row_segments(3), ToolFailure);

    CHECK(row_matches(path, {"Cylinder"}, ""));
    CHECK(row_matches(path, {"Cube", "Cylinder"}, ""));
    CHECK(row_matches(path, path, ""));
    CHECK_FALSE(row_matches(path, {"Plate 1", "Cylinder"}, ""));
    CHECK_FALSE(row_matches(path, {"Cube"}, ""));
    CHECK_FALSE(row_matches(path, {"Plate 0", "Plate 1", "Cube", "Cylinder"}, ""));
    CHECK_FALSE(row_matches(path, {}, ""));
    // The item in use may be named without the marker its list appends.
    CHECK(row_matches({"Generic PLA (current)"}, {"Generic PLA"}, " (current)"));
    CHECK(row_matches({"Generic PLA (current)"}, {"Generic PLA (current)"}, " (current)"));
    CHECK_FALSE(row_matches({"Generic PLA (current)"}, {"Generic"}, " (current)"));

    // The journal writes a path whose label holds '>' as an array.
    CHECK(row_locator(path) == "Plate 1>Cube>Cylinder");
    CHECK(row_locator({"Cube", "A>B"}) == nlohmann::json{"Cube", "A>B"});
}

TEST_CASE("snapshot text shows a row's value, check state and note", "[Mcp]")
{
    UiNode list;
    list.ref  = "w1";
    list.kind = "list";
    list.cls  = "ObjectList";
    list.note = "columns: Name | Filament";
    UiNode cube;
    cube.ref   = "r2";
    cube.kind  = "row";
    cube.label = "Cube";
    cube.value = "1";
    cube.note  = "object 0, selected";
    cube.depth = 1;
    UiNode part   = cube;
    part.ref      = "r3";
    part.label    = "Cylinder";
    part.value    = "";
    part.checked  = 1;
    part.note     = "object 0 part 1, modifier";
    part.depth    = 2;
    UiNode option = part;
    option.ref    = "r4";
    option.label  = "Brim";
    option.enabled = false;
    option.note   = "";
    CHECK(format_snapshot({list, cube, part, option}, 0) == "[w1] list (ObjectList) [columns: Name | Filament]\n"
                                                           "  [r2] row \"Cube\" = \"1\" [object 0, selected]\n"
                                                           "    [r3] row \"Cylinder\" [checked] [object 0 part 1, modifier]\n"
                                                           "    [r4] row \"Brim\" [checked] [disabled]\n");
}

namespace {

// A GUI app for list controls; wxInitializer uses one set before it, and deletes it. Members start
// in order, so the plain wxApp is in place before wx starts: without one, wx would create the
// GUI_App that IMPLEMENT_APP names, whose destructor shuts down singletons later tests use.
struct GuiApp
{
    static bool install()
    {
        if (wxTheApp == nullptr)
            wxApp::SetInstance(new wxApp);
        return true;
    }
    bool          installed = install();
    wxInitializer init;
};

ListRow row_of(wxWindow& list, const std::vector<std::string>& path, int twin = 0)
{
    for (ListRow& row : list_rows(&list))
        if (row.path == path && row.twin == twin)
            return row;
    FAIL("no row " << nlohmann::json(path).dump());
    return {};
}

} // namespace

TEST_CASE("rows of trees, lists, checklists and tables are listed with their cells, checks and selection", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxFrame frame(nullptr, wxID_ANY, "rows", wxDefaultPosition, wxSize(640, 520));

        // A tree, with two objects of one name on a plate: twins, told apart by their order.
        auto*                tree  = new wxDataViewTreeCtrl(&frame, wxID_ANY, wxPoint(0, 0), wxSize(300, 200));
        const wxDataViewItem plate = tree->AppendContainer(wxDataViewItem(), "Plate 1");
        const wxDataViewItem cube  = tree->AppendContainer(plate, "Cube");
        tree->AppendItem(cube, "Cylinder");
        tree->AppendItem(plate, "Cube");
        const std::vector<ListRow> tree_rows = list_rows(tree);
        REQUIRE(tree_rows.size() == 4);
        CHECK(tree_rows[2].path == std::vector<std::string>{"Plate 1", "Cube", "Cylinder"});
        CHECK(tree_rows[2].depth == 2);
        CHECK_FALSE(tree_rows[2].shown); // its branches are collapsed
        CHECK(tree_rows[3].path == std::vector<std::string>{"Plate 1", "Cube"});
        CHECK(tree_rows[3].twin == 1);
        CHECK(tree_rows[3].checked == -1);

        // Labels drawn from markup, as the compare presets tree draws them outside macOS, read as they
        // show; a label that only starts with "<" is kept.
        auto*                marked  = new wxDataViewTreeCtrl(&frame, wxID_ANY, wxPoint(0, 450), wxSize(300, 60));
        const wxDataViewItem quality = marked->AppendContainer(wxDataViewItem(), "<b><span color=\"#363636\">Quality</span></b>");
        marked->AppendItem(quality, "<span color=\"#363636\">Walls &amp; surfaces</span>");
        marked->AppendItem(wxDataViewItem(), "<untitled");
        const std::vector<ListRow> marked_rows = list_rows(marked);
        REQUIRE(marked_rows.size() == 3);
        CHECK(marked_rows[1].path == std::vector<std::string>{"Quality", "Walls & surfaces"});
        CHECK(marked_rows[2].path == std::vector<std::string>{"<untitled"});

        // A list with a checkbox column first: the first text cell is the label, the others the value.
        auto* options = new wxDataViewListCtrl(&frame, wxID_ANY, wxPoint(300, 0), wxSize(300, 200));
        options->AppendToggleColumn("Use");
        options->AppendTextColumn("Option");
        options->AppendTextColumn("Value");
        wxVector<wxVariant> values;
        values.push_back(wxVariant(true));
        values.push_back(wxVariant("Layer height"));
        values.push_back(wxVariant("0.2 | 0.12"));
        options->AppendItem(values);
        const std::vector<ListRow> option_rows = list_rows(options);
        REQUIRE(option_rows.size() == 1);
        CHECK(option_rows[0].path == std::vector<std::string>{"Layer height"});
        CHECK(option_rows[0].value == "0.2 | 0.12");
        CHECK(option_rows[0].checked == 1);
        CHECK(list_columns(options) == std::vector<std::string>{"Use", "Option", "Value"});

        wxArrayString names;
        names.Add("Wall loops");
        names.Add("Sparse infill density");
        auto* checks = new wxCheckListBox(&frame, wxID_ANY, wxPoint(0, 200), wxSize(300, 100), names);
        checks->Check(1);
        const std::vector<ListRow> check_rows = list_rows(checks);
        REQUIRE(check_rows.size() == 2);
        CHECK(check_rows[0].checked == 0);
        CHECK(check_rows[1].checked == 1);
        CHECK(check_rows[1].index == 1);

        // A table with column labels; one that draws its titles as its first row, as the object
        // settings table does, is read the same way.
        auto* table = new wxGrid(&frame, wxID_ANY, wxPoint(300, 200), wxSize(300, 150));
        table->CreateGrid(2, 2);
        table->SetColLabelValue(0, "Name");
        table->SetColLabelValue(1, "Walls");
        table->SetCellValue(0, 0, "Cube");
        table->SetCellValue(0, 1, "2");
        table->SetCellValue(1, 0, "Cylinder");
        table->SetCellValue(1, 1, "3");
        auto* titled = new wxGrid(&frame, wxID_ANY, wxPoint(0, 300), wxSize(300, 150));
        titled->CreateGrid(2, 3);
        titled->HideColLabels();
        titled->HideRowLabels();
        titled->SetCellValue(0, 0, "Plate");
        titled->SetCellValue(0, 2, "Name");
        titled->SetCellValue(1, 0, "1");
        titled->SetCellValue(1, 1, "unseen");
        titled->SetCellValue(1, 2, "Cube");
        for (wxGrid* grid : {table, titled}) {
            const std::vector<ListRow> rows = list_rows(grid);
            REQUIRE_FALSE(rows.empty());
            CHECK(rows[0].path == std::vector<std::string>{"Cube"});
        }
        CHECK(list_rows(table)[1].value == "Cylinder | 3");
        CHECK(list_rows(titled).size() == 1);
        CHECK(list_rows(titled)[0].value == "1 | Cube");
        CHECK(list_columns(titled) == std::vector<std::string>{"Plate", "Name"});

        // Row locators look through every shown list: a label two rows have needs an index.
        frame.Show();
        CHECK(find_rows({&frame}, {"Cube"}).size() == 4); // two tree rows and a row of each table
        CHECK(find_rows({&frame}, {"Plate 1", "Cube"}).size() == 2);
        const auto cylinder = find_rows({&frame}, {"Cube", "Cylinder"});
        REQUIRE(cylinder.size() == 1);
        CHECK(cylinder[0].first == tree);
        CHECK(find_rows({&frame}, {"Sparse infill density"}).at(0).first == checks);
    }
}

TEST_CASE("a click on a row selects it as a person's does and tells its owner once", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxFrame              frame(nullptr, wxID_ANY, "clicks", wxDefaultPosition, wxSize(640, 520));
        auto*                tree  = new wxDataViewTreeCtrl(&frame, wxID_ANY, wxPoint(0, 0), wxSize(300, 300), wxDV_MULTIPLE);
        const wxDataViewItem plate = tree->AppendContainer(wxDataViewItem(), "Plate 1");
        const wxDataViewItem cube  = tree->AppendContainer(plate, "Cube");
        const wxDataViewItem part  = tree->AppendItem(cube, "Cylinder");
        const wxDataViewItem other = tree->AppendItem(plate, "Cube");
        frame.Show();
        // GTK lays the rows out once the shown frame has been through the event loop; a click needs
        // their places, as the app's lists have long had theirs.
        for (int i = 0; i < 100; ++i) {
            wxTheApp->Yield(true);
            if (!tree->GetItemRect(plate).IsEmpty())
                break;
            wxMilliSleep(10);
        }

        std::vector<wxDataViewItem> changed;
        wxDataViewItem              menu, activated;
        tree->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [&changed](wxDataViewEvent& e) { changed.push_back(e.GetItem()); });
        tree->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [&menu](wxDataViewEvent& e) { menu = e.GetItem(); });
        tree->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [&activated](wxDataViewEvent& e) { activated = e.GetItem(); });

        // A click opens the branches above the row and selects it alone, with one notice.
        ClickOptions         left;
        const nlohmann::json out = click_row(*tree, row_of(*tree, {"Plate 1", "Cube", "Cylinder"}), "", left);
        CHECK(out["selected"] == nlohmann::json{"Plate 1>Cube>Cylinder"});
        CHECK(tree->IsExpanded(cube));
        REQUIRE(changed.size() == 1);
        CHECK(changed[0] == part);
        CHECK_FALSE(SyntheticInput::is_mouse_held());

        // Ctrl adds a row and takes it out again.
        ClickOptions ctrl;
        ctrl.modifiers = {"ctrl"};
        CHECK(click_row(*tree, row_of(*tree, {"Plate 1", "Cube"}, 1), "", ctrl)["selected"].size() == 2);
        CHECK(changed.size() == 2);
        CHECK(changed[1] == other);
        CHECK(click_row(*tree, row_of(*tree, {"Plate 1", "Cube"}, 1), "", ctrl)["selected"] == nlohmann::json{"Plate 1>Cube>Cylinder"});

        // A right click on a selected row keeps the selection and asks for its menu; on another row it
        // selects that row first.
        ClickOptions right;
        right.button = "right";
        click_row(*tree, row_of(*tree, {"Plate 1", "Cube", "Cylinder"}), "", right);
        CHECK(menu == part);
        CHECK(changed.size() == 3);
        click_row(*tree, row_of(*tree, {"Plate 1"}), "", right);
        CHECK(menu == plate);
        CHECK(changed.size() == 4);
        CHECK(tree->IsSelected(plate));

        ClickOptions twice;
        twice.double_click = true;
        click_row(*tree, row_of(*tree, {"Plate 1", "Cube"}), "", twice);
        CHECK(activated == cube);

        ClickOptions middle;
        middle.button = "middle";
        CHECK_THROWS_AS(click_row(*tree, row_of(*tree, {"Plate 1"}), "", middle), ToolFailure);
        CHECK_THROWS_AS(click_row(*tree, row_of(*tree, {"Plate 1"}), "Nope", left), ToolFailure);
        CHECK_THROWS_AS(input_row(*tree, row_of(*tree, {"Plate 1"}), "", true), ToolFailure);

        // A checkbox cell is set as the toggle renderer sets it, which tells the owner.
        auto* options = new wxDataViewListCtrl(&frame, wxID_ANY, wxPoint(300, 0), wxSize(300, 200));
        options->AppendToggleColumn("Use");
        options->AppendTextColumn("Option");
        wxVector<wxVariant> values;
        values.push_back(wxVariant(true));
        values.push_back(wxVariant("Layer height"));
        options->AppendItem(values);
        int value_changes = 0;
        options->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [&value_changes](wxDataViewEvent&) { ++value_changes; });
        CHECK(input_row(*options, row_of(*options, {"Layer height"}), "", false)["checked"] == false);
        CHECK_FALSE(options->GetToggleValue(0, 0));
        CHECK(value_changes == 1);
        CHECK(input_row(*options, row_of(*options, {"Layer height"}), "Use", "false")["checked"] == false);
        CHECK(value_changes == 1);
        CHECK_THROWS_AS(input_row(*options, row_of(*options, {"Layer height"}), "Option", true), ToolFailure);

        // A checklist row: ticked with its event; clicked, selected with the list box's.
        wxArrayString names;
        names.Add("Wall loops");
        names.Add("Sparse infill density");
        auto* checks = new wxCheckListBox(&frame, wxID_ANY, wxPoint(0, 300), wxSize(300, 100), names);
        int   ticks = 0, picks = 0;
        checks->Bind(wxEVT_CHECKLISTBOX, [&ticks](wxCommandEvent& e) { ticks += e.GetInt() + 1; });
        checks->Bind(wxEVT_LISTBOX, [&picks](wxCommandEvent&) { ++picks; });
        CHECK(input_row(*checks, row_of(*checks, {"Sparse infill density"}), "", true)["checked"] == true);
        CHECK(checks->IsChecked(1));
        CHECK(ticks == 2);
        input_row(*checks, row_of(*checks, {"Sparse infill density"}), "", true);
        CHECK(ticks == 2);
        CHECK(click_row(*checks, row_of(*checks, {"Wall loops"}), "", left)["selected"] == nlohmann::json{"Wall loops"});
        CHECK(picks == 1);
        CHECK_THROWS_AS(click_row(*checks, row_of(*checks, {"Wall loops"}), "Option", left), ToolFailure);

        // A table row's cell is made current, as a click makes it; the table is read-only.
        auto* table = new wxGrid(&frame, wxID_ANY, wxPoint(300, 200), wxSize(300, 150));
        table->CreateGrid(2, 2);
        table->SetColLabelValue(0, "Name");
        table->SetColLabelValue(1, "Walls");
        table->SetCellValue(1, 0, "Cylinder");
        int selects = 0;
        table->Bind(wxEVT_GRID_SELECT_CELL, [&selects](wxGridEvent& e) {
            ++selects;
            e.Skip();
        });
        CHECK(click_row(*table, row_of(*table, {"Cylinder"}), "walls", left)["column"] == "Walls");
        CHECK(table->GetGridCursorRow() == 1);
        CHECK(table->GetGridCursorCol() == 1);
        CHECK(selects == 1);
        CHECK_THROWS_AS(input_row(*table, row_of(*table, {"Cylinder"}), "", true), ToolFailure);
        CHECK_THROWS_AS(click_row(*table, row_of(*table, {"Cylinder"}), "", right), ToolFailure);
    }
}

TEST_CASE("a widget in a popup belongs to the window the popup opened from, which lists the popup after it", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxFrame frame(nullptr, wxID_ANY, "popups");
        auto*   panel  = new wxPanel(&frame);
        auto*   popup  = new wxPopupTransientWindow(panel);
        auto*   button = new wxButton(popup, wxID_ANY, "Export plate sliced file");
        CHECK(owning_window(button) == &frame);
        CHECK(owning_window(popup) == &frame);
        CHECK(owning_window(panel) == &frame);
        frame.Show();
        CHECK(popups_of(&frame).empty()); // not shown
        popup->Show();
        CHECK(popups_of(&frame) == std::vector<wxWindow*>{popup});
        popup->Hide();
    }
}

TEST_CASE("a plate icon name maps to its picking action and back", "[Mcp]")
{
    const std::vector<std::string> names{"plate", "delete", "orient", "arrange", "lock",
                                         "settings", "name", "move_to_front", "filament_map", "imex_mode"};
    REQUIRE(int(names.size()) == kPlateIconCount);
    for (int action = 0; action < kPlateIconCount; ++action) {
        CAPTURE(action);
        CHECK(plate_icon_name(action) == names[action]);
        CHECK(plate_icon_action(names[action]) == action);
    }
    CHECK(plate_icon_action("Lock") == -1);
    CHECK(plate_icon_action("") == -1);
    CHECK(plate_icon_name(-1).empty());
    CHECK(plate_icon_name(kPlateIconCount).empty());
    // The actions PartPlate names, and the count its picking ids are spaced by.
    CHECK(kPlateIconCount == int(PartPlate::GRABBER_COUNT));
    CHECK(plate_icon_action("name") == int(PartPlate::PLATE_NAME_HOVER_ID));
    CHECK(plate_icon_action("filament_map") == int(PartPlate::PLATE_FILAMENT_MAP_ID));
    CHECK(plate_icon_action("imex_mode") == int(PartPlate::PLATE_IMEX_MODE_ID));
}

TEST_CASE("a bed picking id decodes to its plate and icon", "[Mcp]")
{
    CHECK(plate_icon_of(0) == std::pair<int, int>{0, 0});
    CHECK(plate_icon_of(4) == std::pair<int, int>{0, plate_icon_action("lock")});
    CHECK(plate_icon_of(23) == std::pair<int, int>{2, plate_icon_action("arrange")});
    // What PartPlate::picking_id_component() registers, as the scene raycaster hands it back.
    SceneRaycaster      scene;
    const MeshRaycaster mesh(std::make_shared<const TriangleMesh>(its_make_cube(1., 1., 1.)));
    for (int plate = 0; plate < int(GUI::PartPlateList::MAX_PLATES_COUNT); ++plate)
        for (int action = 0; action < kPlateIconCount; ++action)
            scene.add_raycaster(SceneRaycaster::EType::Bed, plate * int(PartPlate::GRABBER_COUNT) + action, mesh, Transform3d::Identity());
    const auto* items = scene.get_raycasters(SceneRaycaster::EType::Bed);
    REQUIRE(items != nullptr);
    REQUIRE(int(items->size()) == int(GUI::PartPlateList::MAX_PLATES_COUNT) * kPlateIconCount);
    for (std::size_t i = 0; i < items->size(); ++i) {
        const int plate = int(i) / kPlateIconCount, action = int(i) % kPlateIconCount;
        CHECK(plate_icon_of(SceneRaycaster::decode_id(SceneRaycaster::EType::Bed, (*items)[i]->get_id())) ==
              std::pair<int, int>{plate, action});
    }
}

namespace {

// A core whose tools need no UI thread: `build` outlives its reply budget until `release` is set,
// then returns `build_result` (a slow CAD rebuild); `edit` succeeds at once; `wait` hands back a
// job's result, or reports it still running, as the real tool does.
struct JobJournal
{
    InlineDispatcher            ui;
    UiInvoker                   invoker{ui, [] { return GuardState{}; }};
    JobRegistry                 jobs;
    Journal                     journal;
    ToolRegistry                registry;
    std::atomic<bool>           release{false};
    ToolResult                  build_result;
    std::atomic<int>            builds{0};
    std::atomic<int>            edits{0};
    std::unique_ptr<McpCore>    core;

    JobJournal()
    {
        ToolDef build;
        build.name      = "build";
        build.budget_ms = 100;
        build.handler   = [this](const nlohmann::json&, ToolContext&) {
            ++builds;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return build_result;
        };
        registry.add(build);
        ToolDef edit;
        edit.name    = "edit";
        edit.handler = [this](const nlohmann::json&, ToolContext&) {
            ++edits;
            return ToolResult::ok({{"edited", true}});
        };
        registry.add(edit);
        ToolDef wait;
        wait.name        = "wait";
        wait.guard       = GuardClass::Inspect;
        wait.inline_call = true;
        wait.handler     = [this](const nlohmann::json& args, ToolContext&) {
            const std::string job = args["job"].get<std::string>();
            if (auto r = jobs.wait(job, std::chrono::milliseconds(args.value("timeout_ms", 5000))))
                return *r;
            return ToolResult::ok({{"status", "running"}, {"job", job}, {"note", "Still running; wait again."}});
        };
        registry.add(wait);
        core = std::make_unique<McpCore>(registry, invoker, jobs, &journal);
    }
    ~JobJournal()
    {
        release = true;
        invoker.shutdown();
        jobs.wake_all();
        core->shutdown();
    }

    ToolResult call(const std::string& tool, const nlohmann::json& args = nlohmann::json::object())
    {
        return core->call(*registry.find(tool), args);
    }
    // Starts `build`, which replies with its running job; returns the job.
    std::string start_build()
    {
        const ToolResult started = call("build", {{"feature", "thread"}});
        REQUIRE_FALSE(started.is_error);
        REQUIRE(started.data["status"] == "running");
        return started.data["job"].get<std::string>();
    }
};

} // namespace

TEST_CASE("a mutation whose job fails after its reply is dropped from the script, which replays the rest", "[Mcp]")
{
    JobJournal h;
    h.build_result          = ToolResult::error("The thread could not be built");
    const std::string job   = h.start_build();
    h.release               = true;
    const ToolResult failed = h.call("wait", {{"for", "job"}, {"job", job}});
    CHECK(failed.is_error);
    CHECK_FALSE(h.call("edit", {{"value", 3}}).is_error);

    nlohmann::json running;
    const nlohmann::json script = h.journal.to_script("t", &running);
    REQUIRE(script["steps"].size() == 1);
    CHECK(script["steps"][0]["tool"] == "edit");
    CHECK(script["steps"][0]["args"] == nlohmann::json{{"value", 3}});
    CHECK(running.empty());

    // The replay runs the edit alone, as a run_script job's children.
    std::string                 err;
    const std::optional<Script> parsed = parse_script(script.dump(), err);
    REQUIRE(parsed.has_value());
    const std::string parent = h.jobs.create("run_script", "");
    ScriptContext     sctx;
    ScriptHost        host;
    host.call     = [&](const std::string& tool, const nlohmann::json& args) { return h.core->call_child(*h.registry.find(tool), args, parent); };
    host.wait_job = [&](const std::string& id, int timeout_s) { return h.jobs.wait(id, std::chrono::seconds(timeout_s)); };
    const nlohmann::json replay = run_script(*parsed, sctx, host);
    h.jobs.finish(parent, ToolResult::ok());
    CHECK(replay["passed"] == true);
    CHECK(h.builds.load() == 1);
    CHECK(h.edits.load() == 2);
    // A script's own steps are not journaled.
    CHECK(h.journal.to_script("t")["steps"].size() == 1);
}

TEST_CASE("an export while a mutation's job runs leaves the call out and names it, and keeps it once the job is done", "[Mcp]")
{
    JobJournal h;
    h.build_result        = ToolResult::ok({{"built", true}});
    const std::string job = h.start_build();
    CHECK_FALSE(h.call("edit", {{"value", 1}}).is_error);
    // A wait that times out names the job too, but it is not the wait's own.
    const ToolResult still = h.call("wait", {{"for", "job"}, {"job", job}, {"timeout_ms", 0}});
    CHECK(still.data["status"] == "running");

    nlohmann::json running;
    nlohmann::json steps = h.journal.to_script("t", &running)["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "edit");
    CHECK(running == nlohmann::json::array({{{"tool", "build"}, {"job", job}}}));

    h.release = true;
    CHECK(h.call("wait", {{"for", "job"}, {"job", job}}).data["built"] == true);
    steps = h.journal.to_script("t", &running)["steps"];
    CHECK(running.empty());
    REQUIRE(steps.size() == 4);
    CHECK(steps[0]["tool"] == "build");
    CHECK(steps[0]["await"] == false);
    CHECK(steps[0]["save_as"] == "job1");
    CHECK(steps[1]["tool"] == "edit");
    CHECK(steps[2]["args"]["job"] == "${jobs.job1}");
    CHECK_FALSE(steps[2].contains("save_as"));
    CHECK(steps[3]["args"]["job"] == "${jobs.job1}");
}

TEST_CASE("a job that waits on a dialog stays in the script, and a call yet to reply is left out", "[Mcp]")
{
    Journal        journal;
    const uint64_t open = journal.begin("run_action", {{"id", "orca_command:open_preferences:orca"}});
    journal.job_started(open, "j1");
    journal.end(open, ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j1"}, {"tool", "run_action"}}));
    const uint64_t answer = journal.begin("dialog_respond", {{"button", "cancel"}});
    journal.job_started(answer, "j2");
    journal.job_ended(answer, ToolResult::ok());
    journal.end(answer, ToolResult::ok());
    const uint64_t slice = journal.begin("slice", nlohmann::json::object());
    journal.job_started(slice, "j3");
    journal.begin("screenshot", {{"target", "canvas"}});

    nlohmann::json running;
    nlohmann::json steps = journal.to_script("t", &running)["steps"];
    REQUIRE(steps.size() == 2);
    CHECK(steps[0]["save_as"] == "job1");
    CHECK(steps[1]["tool"] == "dialog_respond");
    CHECK(running == nlohmann::json::array({{{"tool", "slice"}, {"job", "j3"}}, {{"tool", "screenshot"}}}));

    // The dialog's answer follows it in the script, so it stays when its job then fails.
    journal.job_ended(open, ToolResult::error("Cancelled"));
    // A job that ends before its reply is recorded is in the script at once.
    journal.job_ended(slice, ToolResult::ok());
    steps = journal.to_script("t", &running)["steps"];
    REQUIRE(steps.size() == 3);
    CHECK(steps[0]["tool"] == "run_action");
    CHECK(steps[2]["tool"] == "slice");
    CHECK_FALSE(steps[2].contains("await"));
    CHECK(running == nlohmann::json::array({{{"tool", "screenshot"}}}));
}

TEST_CASE("a job that was running when a dialog was answered stays in the script before the answer", "[Mcp]")
{
    // A script past its reply budget opens Preferences, which a later call answers.
    Journal        journal;
    const uint64_t script = journal.begin("run_script", {{"path", "s.json"}});
    journal.job_started(script, "j1");
    journal.end(script, ToolResult::ok({{"status", "running"}, {"job", "j1"}, {"tool", "run_script"}}));
    // A call still within its reply budget, whose dialog is answered before it replies.
    const uint64_t open = journal.begin("run_action", {{"id", "orca_command:open_preferences:orca"}});
    journal.job_started(open, "j2");
    const uint64_t slice = journal.begin("slice", nlohmann::json::object());
    journal.job_started(slice, "j3");
    journal.end(slice, ToolResult::ok({{"status", "running"}, {"job", "j3"}, {"tool", "slice"}}));
    journal.job_ended(slice, ToolResult::error("Cancelled"));
    const uint64_t answer = journal.begin("dialog_respond", {{"button", "close"}}, true);
    journal.job_started(answer, "j4");
    journal.job_ended(answer, ToolResult::ok());
    journal.end(answer, ToolResult::ok());
    // Input with no dialog open ties nothing to the jobs then running.
    const uint64_t click = journal.begin("ui_click", {{"target", "w1"}});
    journal.job_started(click, "j5");
    journal.job_ended(click, ToolResult::ok());
    journal.end(click, ToolResult::ok());
    const uint64_t later = journal.begin("run_action", {{"id", "obj_arrange"}});
    journal.job_started(later, "j6");
    journal.end(later, ToolResult::ok({{"status", "running"}, {"job", "j6"}, {"tool", "run_action"}}));
    journal.begin("ui_click", {{"target", "w2"}});

    nlohmann::json running;
    nlohmann::json steps = journal.to_script("t", &running)["steps"];
    REQUIRE(steps.size() == 5);
    CHECK(steps[0]["tool"] == "run_script");
    CHECK(steps[0]["await"] == false);
    CHECK(steps[1]["tool"] == "run_action");
    CHECK(steps[1]["await"] == false);
    // The answer waits for its dialog, which the replayed job may open later than its reply.
    CHECK(steps[2] == nlohmann::json({{"tool", "wait"}, {"args", {{"for", "dialog"}, {"timeout_s", 50}}}, {"expect", {{"/satisfied", {{"eq", true}}}}}}));
    CHECK(steps[3]["tool"] == "dialog_respond");
    CHECK(steps[4]["tool"] == "ui_click");
    CHECK(running == nlohmann::json::array({{{"tool", "run_action"}, {"job", "j6"}}, {{"tool", "ui_click"}}}));

    // They stay whatever their jobs' results: the answer after them needs a dialog one of them opens.
    journal.job_ended(script, ToolResult::error("Step 4 failed"));
    journal.job_ended(open, ToolResult::ok());
    journal.end(open, ToolResult::ok({{"ran", "orca_command:open_preferences:orca"}}));
    steps = journal.to_script("t")["steps"];
    REQUIRE(steps.size() == 5);
    CHECK(steps[0]["tool"] == "run_script");
    CHECK(steps[1]["await"] == false);
}

TEST_CASE("an exported answer to a dialog a job opens after its reply replays once the dialog is open", "[Mcp]")
{
    // A script replies running, opens Preferences later, and is answered while it keeps running.
    Journal        journal;
    const uint64_t script = journal.begin("run_script", {{"path", "s.json"}});
    journal.job_started(script, "j1");
    journal.end(script, ToolResult::ok({{"status", "running"}, {"job", "j1"}, {"tool", "run_script"}}));
    const uint64_t answer = journal.begin("dialog_respond", {{"button", "close"}}, true);
    journal.job_started(answer, "j2");
    journal.job_ended(answer, ToolResult::ok());
    journal.end(answer, ToolResult::ok());
    const uint64_t waited = journal.begin("wait", {{"for", "job"}, {"job", "j1"}});
    journal.end(waited, ToolResult::ok());
    journal.job_ended(script, ToolResult::ok());

    std::string                 err;
    const std::optional<Script> parsed = parse_script(journal.to_script("t").dump(), err);
    REQUIRE(parsed.has_value());
    // The replayed script opens its dialog 300 ms after its reply; an answer before that is refused.
    std::atomic<bool> dialog{false};
    std::thread       opener;
    std::vector<std::string> calls;
    ScriptHost        host;
    host.call = [&](const std::string& tool, const nlohmann::json& args) {
        calls.push_back(tool);
        if (tool == "run_script") {
            opener = std::thread([&dialog] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                dialog = true;
            });
            return ToolResult::ok({{"status", "running"}, {"job", "r1"}, {"tool", "run_script"}});
        }
        if (tool == "wait" && args["for"] == "dialog") {
            for (int i = 0; i < 100 && !dialog; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return ToolResult::ok({{"satisfied", dialog.load()}});
        }
        if (tool == "dialog_respond") {
            if (!dialog)
                return ToolResult::error("No modal dialog is open");
            dialog = false;
            return ToolResult::ok({{"pressed", "close"}});
        }
        CHECK(args["job"] == "r1");
        return ToolResult::ok({{"done", true}});
    };
    host.wait_job = [](const std::string&, int) { return std::optional<ToolResult>(ToolResult::ok()); };
    ScriptContext        sctx;
    const nlohmann::json replay = run_script(*parsed, sctx, host);
    opener.join();
    CHECK(replay["passed"] == true);
    CHECK(calls == std::vector<std::string>{"run_script", "wait", "dialog_respond", "wait"});
}

TEST_CASE("an upload's row reaches 100 percent only once the host has answered", "[Mcp][PrintHost]")
{
    const std::string buffer;
    auto              percent = [&](size_t ulnow, size_t ultotal) {
        return PrintHostJobQueue::sent_percent(Http::Progress(0, 0, ultotal, ulnow, buffer));
    };
    CHECK(percent(0, 0) == 0);
    CHECK(percent(0, 4096) == 0);
    CHECK(percent(2048, 4096) == 50);
    CHECK(percent(4095, 4096) == 99);
    // Every byte sent, the host's answer still to come: a 500 would turn the row into an error.
    CHECK(percent(4096, 4096) == 99);
}

TEST_CASE("the send command is a Slice & Export command that takes no param", "[Mcp][SpeedDial]")
{
    const NativeCommand* send = NativeCommands::find("send_gcode");
    REQUIRE(send != nullptr);
    CHECK(send->title == "Send G-code to printer host");
    CHECK(send->group == "Slice & Export");
    CHECK(send->param.type == CommandParamType::None);
    CHECK(send->completion == CommandCompletion::None);
    CHECK(send->description.find("get_state.uploads") != std::string::npos);
}
