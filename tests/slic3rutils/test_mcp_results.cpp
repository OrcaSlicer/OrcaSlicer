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
#include <catch2/matchers/catch_matchers_string.hpp>

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
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/Mcp/McpReplay.hpp"
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
#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/arrstr.h>
#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/filedlg.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/gdicmn.h>
#include <wx/grid.h>
#include <wx/init.h>
#include <wx/listbase.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/modalhook.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/popupwin.h>
#include <wx/radiobut.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <wx/validate.h>
#include <wx/variant.h>
#include <wx/vector.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
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

    can_edit     = true; // `select_objects` picked an object
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

        // A modal opened above it before its loop ran leaves it to answer_dialog.
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

namespace {

// What ShowModal() returns for a picker a path rule can serve: OK where GetPath() then reads the path SetPath() gave
// it (Windows, macOS), and cancel on GTK, whose GetPath() reads the GTK chooser, which has no path until it shows.
#if defined(__WXMSW__) || defined(__WXOSX__)
constexpr int kPathAnswer = wxID_OK;
#else
constexpr int kPathAnswer = wxID_CANCEL;
#endif

void write_file(const std::string& path) { boost::nowide::ofstream(path.c_str()) << "<svg/>"; }

} // namespace

TEST_CASE("a rule with a path answers a file picker with it, unseen", "[Mcp]")
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
        ScopedTemporaryDir dir("orca-mcp-picker");
        const std::string  a_svg = path_to_utf8(dir.path() / "a.svg");
        write_file(a_svg);
        ModalTracker tracker;
        tracker.push_rules({make_rule("Import SVG", "ok", a_svg)});

        wxFileDialog dlg(nullptr, "Import SVG", "", "", "SVG files (*.svg)|*.svg", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        CHECK(wxModalDialogHook::CallEnter(&dlg) == kPathAnswer);
        if (kPathAnswer == wxID_OK)
            CHECK(dlg.GetPath() == wxString::FromUTF8(a_svg));
        CHECK(tracker.top() == nullptr);
        CHECK(tracker.rules_json()[0]["uses"] == 1);
        CHECK(tracker.rules_json()[0]["path"] == a_svg);
    }
}

TEST_CASE("a save picker takes a new file, and an existing one only with overwrite", "[Mcp]")
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
        ScopedTemporaryDir dir("orca-mcp-picker");
        const std::string  out = path_to_utf8(dir.path() / "out.3mf");
        ModalTracker       tracker;
        // The answer, and the path an OK returns.
        const auto save = [&](bool overwrite) {
            const int    layer = tracker.push_rules({make_rule("Save project", "ok", out, overwrite)});
            wxFileDialog dlg(nullptr, "Save project", "", "", "3MF files (*.3mf)|*.3mf", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            const int    answer = wxModalDialogHook::CallEnter(&dlg);
            const wxString path = answer == wxID_OK ? dlg.GetPath() : wxString();
            tracker.pop_rules(layer);
            return std::make_pair(answer, path);
        };

        // A file that doesn't exist yet.
        const auto fresh = save(false);
        CHECK(fresh.first == kPathAnswer);
        if (kPathAnswer == wxID_OK)
            CHECK(fresh.second == wxString::FromUTF8(out));

        // Once it exists, only a rule with overwrite replaces it.
        write_file(out);
        CHECK(save(false).first == wxID_CANCEL);
        const auto replaced = save(true);
        CHECK(replaced.first == kPathAnswer);
        if (kPathAnswer == wxID_OK)
            CHECK(replaced.second == wxString::FromUTF8(out));
        CHECK(tracker.top() == nullptr);
    }
}

TEST_CASE("a folder picker takes an existing folder", "[Mcp]")
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
        ScopedTemporaryDir dir("orca-mcp-picker");
        const std::string  folder = path_to_utf8(dir.path());
        ModalTracker       tracker;
        tracker.push_rules({make_rule("Choose a folder", "ok", folder)});

        wxDirDialog dlg(nullptr, "Choose a folder", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        CHECK(wxModalDialogHook::CallEnter(&dlg) == kPathAnswer);
        if (kPathAnswer == wxID_OK)
            CHECK(dlg.GetPath() == wxString::FromUTF8(folder));
        CHECK(tracker.top() == nullptr);
        CHECK(tracker.rules_json()[0]["uses"] == 1);
    }
}

TEST_CASE("a picker the path can't serve is cancelled unseen", "[Mcp]")
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
        ScopedTemporaryDir dir("orca-mcp-picker");
        const std::string  a_svg   = path_to_utf8(dir.path() / "a.svg");
        const std::string  missing = path_to_utf8(dir.path() / "missing.svg");
        write_file(a_svg);
        ModalTracker tracker;
        tracker.push_rules({make_rule("Open missing", "ok", missing), make_rule("Choose a folder", "ok", a_svg),
                            make_rule("Open several", "ok", a_svg), make_rule("Choose folders", "ok", path_to_utf8(dir.path()))});

        // An open picker needs an existing file.
        wxFileDialog open_missing(nullptr, "Open missing", "", "", "SVG files (*.svg)|*.svg", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        CHECK(wxModalDialogHook::CallEnter(&open_missing) == wxID_CANCEL);
        // A folder picker needs a folder.
        wxDirDialog folder(nullptr, "Choose a folder");
        CHECK(wxModalDialogHook::CallEnter(&folder) == wxID_CANCEL);
        // A multi-select picker reads its own list of paths, not the one a rule sets.
        wxFileDialog several(nullptr, "Open several", "", "", "SVG files (*.svg)|*.svg", wxFD_OPEN | wxFD_MULTIPLE);
        CHECK(wxModalDialogHook::CallEnter(&several) == wxID_CANCEL);
        wxDirDialog folders(nullptr, "Choose folders", "", wxDD_DEFAULT_STYLE | wxDD_MULTIPLE);
        CHECK(wxModalDialogHook::CallEnter(&folders) == wxID_CANCEL);
        // None of them showed.
        CHECK(tracker.top() == nullptr);
    }
}

TEST_CASE("a rule with a path answers pickers only", "[Mcp]")
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
        ScopedTemporaryDir dir("orca-mcp-picker");
        const std::string  a_svg = path_to_utf8(dir.path() / "a.svg");
        write_file(a_svg);
        ModalTracker tracker;
        tracker.push_rules({make_rule(".*", "ok", a_svg)});

        // A wx dialog that matches opens, and its OK is not pressed.
        wxDialog dialog(nullptr, wxID_ANY, "Import SVG");
        int      presses = 0;
        auto*    ok      = new wxButton(&dialog, wxID_OK, "OK");
        ok->Bind(wxEVT_BUTTON, [&presses](wxCommandEvent&) { ++presses; });
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        wxTheApp->ProcessPendingEvents();
        CHECK(presses == 0);
        wxModalDialogHook::CallExit(&dialog);

        // A message box that matches shows for the person.
        wxMessageDialog box(nullptr, "Import the file?", "Import SVG", wxOK | wxCANCEL);
        CHECK(wxModalDialogHook::CallEnter(&box) == wxID_NONE);
        wxModalDialogHook::CallExit(&box);
        CHECK(tracker.rules_json()[0]["uses"] == 0);
    }
}

TEST_CASE("a rule without a path still answers a picker only with cancel", "[Mcp]")
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
        ModalTracker tracker;
        const int    layer = tracker.push_rules({make_rule("Import SVG", "ok")});

        // OK without a path would return no file, so the picker shows for the person.
        wxFileDialog dlg(nullptr, "Import SVG", "", "", "SVG files (*.svg)|*.svg", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        CHECK(wxModalDialogHook::CallEnter(&dlg) == wxID_NONE);
        CHECK(tracker.top() == &dlg);
        wxModalDialogHook::CallExit(&dlg);
        tracker.pop_rules(layer);

        tracker.push_rules({make_rule("Import SVG", "cancel")});
        CHECK(wxModalDialogHook::CallEnter(&dlg) == wxID_CANCEL);
        CHECK(tracker.top() == nullptr);
    }
}

TEST_CASE("a rule's path must be absolute and comes with ok", "[Mcp]")
{
    const std::string abs = path_to_utf8(boost::filesystem::temp_directory_path() / "a.svg");
    CHECK_THROWS_AS(make_rule("x", "ok", "rel/a.svg"), ToolFailure);
    CHECK_THROWS_AS(make_rule("x", "cancel", abs), ToolFailure);
    CHECK_NOTHROW(make_rule("x", "OK", abs));
    CHECK(make_rule("x", "ok", abs, true).overwrite);
}

TEST_CASE("a paused rule layer answers nothing until resumed", "[Mcp]")
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
        ModalTracker tracker;
        const int    layer = tracker.push_rules({make_rule("unsaved", "Transfer")});
        tracker.set_paused(layer, true);

        wxDialog dialog(nullptr, wxID_ANY, "Unsaved changes");
        int      transfers = 0;
        auto*    transfer  = new wxButton(&dialog, wxID_ANY, "Transfer");
        transfer->Bind(wxEVT_BUTTON, [&transfers](wxCommandEvent&) { ++transfers; });
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        wxTheApp->ProcessPendingEvents();
        CHECK(transfers == 0);
        CHECK(tracker.top() == &dialog); // it stays open for the person
        wxModalDialogHook::CallExit(&dialog);
        CHECK(tracker.rules_json()[0]["uses"] == 0);
        CHECK(tracker.rules_json()[0].value("paused", false));
        CHECK_FALSE(tracker.rule_in_force(layer, "unsaved", "Transfer"));

        tracker.set_paused(layer, false);
        CHECK(tracker.rule_in_force(layer, "unsaved", "Transfer"));
        CHECK_FALSE(tracker.rules_json()[0].value("paused", false));
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        wxTheApp->ProcessPendingEvents();
        CHECK(transfers == 1);
        wxModalDialogHook::CallExit(&dialog);
        CHECK(tracker.rules_json()[0]["uses"] == 1);
    }
}

TEST_CASE("an answer queued for a dialog is dropped when its rule is paused before it runs", "[Mcp]")
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
        ModalTracker tracker;
        const int    layer = tracker.push_rules({make_rule("unsaved", "Transfer")});

        wxDialog dialog(nullptr, wxID_ANY, "Unsaved changes");
        int      transfers = 0;
        auto*    transfer  = new wxButton(&dialog, wxID_ANY, "Transfer");
        transfer->Bind(wxEVT_BUTTON, [&transfers](wxCommandEvent&) { ++transfers; });
        // The press is queued for the dialog's modal loop; a Run Script work ends and pauses its rules before it runs.
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        tracker.set_paused(layer, true);
        wxTheApp->ProcessPendingEvents();
        CHECK(transfers == 0);
        CHECK(tracker.top() == &dialog);
        wxModalDialogHook::CallExit(&dialog);
        CHECK(tracker.rules_json()[0]["uses"] == 0);
    }
}

TEST_CASE("a rule layer pushed paused answers nothing from the start", "[Mcp]")
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
        // A Run Script session's thread registers a rule between works: the layer is paused as it is created, so it is
        // never in force before the session's next work resumes it.
        ModalTracker tracker;
        const int    layer = tracker.push_rules({make_rule("unsaved", "Transfer")}, true);
        CHECK(tracker.rules_json()[0].value("paused", false));
        CHECK_FALSE(tracker.rule_in_force(layer, "unsaved", "Transfer"));

        wxDialog dialog(nullptr, wxID_ANY, "Unsaved changes");
        int      transfers = 0;
        auto*    transfer  = new wxButton(&dialog, wxID_ANY, "Transfer");
        transfer->Bind(wxEVT_BUTTON, [&transfers](wxCommandEvent&) { ++transfers; });
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        wxTheApp->ProcessPendingEvents();
        CHECK(transfers == 0);
        wxModalDialogHook::CallExit(&dialog);

        tracker.set_paused(layer, false);
        CHECK(tracker.rule_in_force(layer, "unsaved", "Transfer"));
    }
}

TEST_CASE("a dialog hidden from automation is neither tracked nor auto-answered", "[Mcp]")
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
        ModalTracker tracker;
        tracker.push_rules({make_rule("Python", "ok")});

        wxDialog hidden(nullptr, wxID_ANY, "Python scripts", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE,
                        kHiddenFromAutomation);
        int pressed = 0;
        auto* ok    = new wxButton(&hidden, wxID_OK, "OK");
        ok->Bind(wxEVT_BUTTON, [&pressed](wxCommandEvent&) { ++pressed; });
        CHECK(wxModalDialogHook::CallEnter(&hidden) == wxID_NONE);
        CHECK(tracker.top() == nullptr);
        CHECK(tracker.snapshot().empty());
        wxTheApp->ProcessPendingEvents();
        CHECK(pressed == 0);
        wxModalDialogHook::CallExit(&hidden);

        // A dialog of the same title without the name is tracked, and its rule answers it.
        wxDialog plain(nullptr, wxID_ANY, "Python scripts");
        CHECK(wxModalDialogHook::CallEnter(&plain) == wxID_NONE);
        CHECK(tracker.top() == &plain);
        wxModalDialogHook::CallExit(&plain);
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

TEST_CASE("set_view's legend keys name one feature type or option each, as get_slice_stats lists them", "[Mcp]")
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

TEST_CASE("a click that only listed a menu is dropped from the replay when the next call picks an item of it", "[Mcp]")
{
    const nlohmann::json click{{"action", "click"}, {"button", "right"}, {"at", {{"object", 0}, {"spot", "top"}}}};
    nlohmann::json       pick = click;
    pick["popup_item"]        = "Add Modifier>Cylinder";
    const ToolResult listed   = ToolResult::ok({{"popup_menu", {{"items", {"Add Modifier>Cylinder"}}}}});
    const ToolResult ran      = ToolResult::ok({{"ran", "Add Modifier>Cylinder"}});
    auto steps = [](Journal& journal) { return journal.to_replay("t")["steps"]; };

    Journal journal;
    journal.end(journal.begin("mouse_gesture", click), listed);
    journal.end(journal.begin("mouse_gesture", pick), ran);
    REQUIRE(steps(journal).size() == 1);
    CHECK(steps(journal)[0]["args"] == pick);

    // A failed pick in between does not count; the listing click then stays, as it changed the selection.
    Journal kept;
    kept.end(kept.begin("mouse_gesture", click), listed);
    kept.end(kept.begin("mouse_gesture", pick), ToolResult::error("The menu has no item"));
    CHECK(steps(kept).size() == 1);
    CHECK(steps(kept)[0]["args"] == click);
    // Nor does a pick after another call, or one on another target.
    Journal other;
    other.end(other.begin("mouse_gesture", click), listed);
    other.end(other.begin("select_objects", {{"objects", {0}}}), ToolResult::ok());
    other.end(other.begin("mouse_gesture", pick), ran);
    CHECK(steps(other).size() == 3);
    nlohmann::json elsewhere = pick;
    elsewhere["at"]["spot"]  = "front";
    Journal moved;
    moved.end(moved.begin("mouse_gesture", click), listed);
    moved.end(moved.begin("mouse_gesture", elsewhere), ran);
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
    invoker.run(GuardClass::Mutate, "click", [&] {
        invoker.run(GuardClass::Inspect, "get_app_state", [] { return nlohmann::json(); });
        return nlohmann::json();
    });
    CHECK(seen ==
          std::vector<std::pair<std::string, bool>>{{"click", true}, {"get_app_state", true}, {"get_app_state", false}, {"click", false}});
    seen.clear();
    // A closure that throws still ends; one refused before it runs never starts.
    CHECK_THROWS_AS(invoker.run(GuardClass::Mutate, "press_keys", []() -> nlohmann::json { throw ToolFailure("no"); }), ToolFailure);
    CHECK(seen == std::vector<std::pair<std::string, bool>>{{"press_keys", true}, {"press_keys", false}});
    seen.clear();
    modal = true;
    CHECK_THROWS_AS(invoker.run(GuardClass::Mutate, "select_objects", [] { return nlohmann::json(); }), ToolFailure);
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

        inspector.ui_activity("click", true);
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
        inspector.ui_activity("click", false);

        // A menu from a call that armed nothing is closed unseen, until the first idle after the call.
        inspector.ui_activity("extrude", true);
        inspector.ui_activity("extrude", false);
        CHECK(popup_menu(frame, menu));
        const nlohmann::json closed = inspector.closed_menus();
        REQUIRE(closed.size() == 2);
        CHECK(closed[1]["tool"] == "extrude");
        CHECK(closed[1]["items"].size() == 7);
        wxTheApp->ProcessPendingEvents();
        wxIdleEvent idle;
        wxTheApp->ProcessEvent(idle);
        CHECK_FALSE(inspector.on_menu(menu, frame));

        // Past a second a person's menu shows even when no idle came, as while events keep queuing.
        inspector.ui_activity("extrude", true);
        inspector.ui_activity("extrude", false);
        wxMilliSleep(1100);
        CHECK_FALSE(inspector.on_menu(menu, frame));

        // Only menus of the event loop the call ran in are taken: a person's right click in a dialog's
        // loop shows its menu.
        inspector.ui_activity("click", true);
        {
            wxEventLoop          dialog_loop;
            wxEventLoopActivator inside(&dialog_loop);
            CHECK_FALSE(inspector.on_menu(menu, frame));
        }
        CHECK(inspector.on_menu(menu, frame));
        // get_app_state keeps the last three.
        CHECK(inspector.on_menu(menu, frame));
        CHECK(inspector.closed_menus().size() == 3);
        CHECK(inspector.closed_menus()[2]["tool"] == "click");
        inspector.ui_activity("click", false);
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
    // A checkbox that shows neither state.
    UiNode mixed;
    mixed.ref     = "w5";
    mixed.kind    = "toggle";
    mixed.label   = "Detach";
    mixed.checked = 2;
    CHECK(format_snapshot({mixed}, 0) == "[w5] toggle \"Detach\" [mixed]\n");
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

TEST_CASE("a list control's rows are listed with their columns and picked as a click picks them", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxFrame frame(nullptr, wxID_ANY, "hosts", wxDefaultPosition, wxSize(640, 520));
        auto*   hosts = new wxListView(&frame, wxID_ANY, wxPoint(0, 0), wxSize(400, 300));
        hosts->AppendColumn("Address");
        hosts->AppendColumn("Hostname");
        hosts->InsertItem(0, "192.168.1.10");
        hosts->SetItem(0, 1, "x1c");
        hosts->InsertItem(1, "192.168.1.11");
        hosts->SetItem(1, 1, "p1s");
        frame.Show();

        CHECK(kind_of(hosts) == "list");
        CHECK(is_row_list(hosts));
        CHECK(list_columns(hosts) == std::vector<std::string>{"Address", "Hostname"});
        const std::vector<ListRow> rows = list_rows(hosts);
        REQUIRE(rows.size() == 2);
        CHECK(rows[1].path == std::vector<std::string>{"192.168.1.11"});
        CHECK(rows[1].value == "p1s");
        CHECK_FALSE(rows[1].selected);
        CHECK(list_rows(hosts, 1).size() == 1);

        // The list tells its owner of the pick, as a person's click makes it do.
        long selected = -1, activated = -1;
        hosts->Bind(wxEVT_LIST_ITEM_SELECTED, [&selected](wxListEvent& e) { selected = e.GetIndex(); });
        hosts->Bind(wxEVT_LIST_ITEM_ACTIVATED, [&activated](wxListEvent& e) { activated = e.GetIndex(); });
        const nlohmann::json out = click_row(*hosts, row_of(*hosts, {"192.168.1.11"}), "", ClickOptions{});
        CHECK(out["selected"] == nlohmann::json{"192.168.1.11"});
        CHECK(selected == 1);
        CHECK(activated == -1);
        ClickOptions twice;
        twice.double_click = true;
        click_row(*hosts, row_of(*hosts, {"192.168.1.10"}), "", twice);
        CHECK(selected == 0);
        CHECK(activated == 0);
        CHECK(hosts->GetSelectedItemCount() == 1);
        CHECK_THROWS_AS(input_row(*hosts, row_of(*hosts, {"192.168.1.10"}), "", true), ToolFailure);
    }
}

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("a modal dialog's message reads its fields, choices, toggles and lists as they show", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxDialog dialog(nullptr, wxID_ANY, "Save preset");
        auto*    top = new wxBoxSizer(wxVERTICAL);
        auto     row = [&](std::initializer_list<wxWindow*> windows) {
            auto* line = new wxBoxSizer(wxHORIZONTAL);
            for (wxWindow* w : windows)
                line->Add(w, 0, wxALL, 4);
            top->Add(line);
        };
        auto stacked = [&](wxWindow* w) {
            top->Add(w, 0, wxALL, 4);
            return w;
        };

        // A field beside its label, and one created before the label above it.
        row({new wxStaticText(&dialog, wxID_ANY, "Name"), new wxTextCtrl(&dialog, wxID_ANY, "Generic PLA", wxDefaultPosition, wxSize(160, -1))});
        auto* diameter = new wxTextCtrl(&dialog, wxID_ANY, "0.4");
        stacked(new wxStaticText(&dialog, wxID_ANY, "Nozzle diameter"));
        stacked(diameter);
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "line one\nline two", wxDefaultPosition, wxSize(200, 60), wxTE_MULTILINE | wxTE_READONLY));

        // Secrets: by style, by the dialog's name for the field, by the setting the field edits.
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "hunter2", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD));
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "12345678"))->SetName(kSecretField);
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "abcdef"))->SetToolTip("parameter name: printhost_apikey");
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "user:pw@printer.local"))->SetToolTip("parameter name: print_host");
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "http://me:pw@host/api?token=x"));
        stacked(new wxTextCtrl(&dialog, wxID_ANY, "http://bob:pw2@host "));
        stacked(new wxTextCtrl(&dialog, wxID_ANY, " carol:pw3@printer.local"))->SetToolTip("parameter name: print_host_webui");

        auto* remember = new wxCheckBox(&dialog, wxID_ANY, "Don't show again");
        remember->SetValue(true);
        stacked(remember);
        auto* detach = new wxCheckBox(&dialog, wxID_ANY, "Detach", wxDefaultPosition, wxDefaultSize, wxCHK_3STATE);
        detach->Set3StateValue(wxCHK_UNDETERMINED);
        stacked(detach);
        // Toggles with no label of their own, as Orca's CheckBox and RadioBox are; two of them made
        // before their labels.
        const wxBitmap mark(16, 16);
        auto*          choice_toggle = new wxBitmapToggleButton(&dialog, wxID_ANY, mark);
        choice_toggle->SetValue(true);
        row({choice_toggle, new wxStaticText(&dialog, wxID_ANY, "Remember my choice")});
        auto* plate_a = new wxBitmapToggleButton(&dialog, wxID_ANY, mark);
        auto* plate_b = new wxBitmapToggleButton(&dialog, wxID_ANY, mark);
        plate_b->SetValue(true);
        auto* label_a = new wxStaticText(&dialog, wxID_ANY, "Plate A");
        auto* label_b = new wxStaticText(&dialog, wxID_ANY, "Plate B");
        row({plate_a, label_a});
        row({plate_b, label_b});

        auto* user    = new wxRadioButton(&dialog, wxID_ANY, "User preset", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        auto* project = new wxRadioButton(&dialog, wxID_ANY, "Project preset");
        project->SetValue(true);
        row({user, project});
        const wxString choices[] = {"A", "B"};
        auto*          choice    = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, choices);
        choice->SetSelection(1);
        stacked(choice);
        auto* copies = new wxSpinCtrl(&dialog, wxID_ANY);
        copies->SetValue(3);
        stacked(copies);
        auto* progress = new wxGauge(&dialog, wxID_ANY, 200);
        progress->SetValue(90);
        stacked(progress);
        auto* disabled = new wxTextCtrl(&dialog, wxID_ANY, "0.000");
        disabled->Disable();
        stacked(disabled);

        auto* box   = new wxStaticBox(&dialog, wxID_ANY, "Settings");
        auto* boxed = new wxStaticBoxSizer(box, wxVERTICAL);
        boxed->Add(new wxStaticText(box, wxID_ANY, "Inside the box"));
        top->Add(boxed);

        auto* book    = new wxNotebook(&dialog, wxID_ANY);
        auto* general = new wxPanel(book);
        new wxStaticText(general, wxID_ANY, "General page");
        auto* advanced = new wxPanel(book);
        new wxStaticText(advanced, wxID_ANY, "Advanced page");
        book->AddPage(general, "General");
        book->AddPage(advanced, "Advanced", true);
        stacked(book);

        const wxString filaments[] = {"PLA", "PETG"};
        auto*          checks      = new wxCheckListBox(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, filaments);
        checks->Check(0);
        stacked(checks);
        auto* hosts = new wxListView(&dialog, wxID_ANY, wxDefaultPosition, wxSize(300, 150));
        hosts->AppendColumn("Address");
        hosts->AppendColumn("Hostname");
        for (int i = 0; i < 30; ++i) {
            hosts->InsertItem(i, "192.168.1." + std::to_string(i));
            hosts->SetItem(i, 1, "printer" + std::to_string(i));
        }
        stacked(hosts);

        auto* ok = new wxButton(&dialog, wxID_OK, "OK");
        ok->Disable();
        row({ok, new wxButton(&dialog, wxID_CANCEL, "Cancel")});
        dialog.SetSizerAndFit(top);

        // A dialog of its own is described on its own.
        wxDialog nested(&dialog, wxID_ANY, "Nested");
        new wxStaticText(&nested, wxID_ANY, "Nested text");
        nested.Show();

        const nlohmann::json        described = describe_dialog(&dialog);
        const std::string message   = described["message"].get<std::string>();
        INFO(message);
        CHECK(has(message, "Name | [Generic PLA]"));
        CHECK(has(message, "Nozzle diameter\n[0.4]"));
        CHECK(has(message, "line one\nline two"));
        CHECK(has(message, "[******]"));
        CHECK_FALSE(has(message, "hunter2"));
        CHECK_FALSE(has(message, "12345678"));
        CHECK_FALSE(has(message, "abcdef"));
        CHECK(has(message, "[REDACTED:REDACTED@printer.local]"));
        CHECK_FALSE(has(message, ":pw@"));
        CHECK_FALSE(has(message, "token=x"));
        CHECK_FALSE(has(message, "pw2"));
        CHECK_FALSE(has(message, "pw3"));
        CHECK(has(message, "[http://REDACTED:REDACTED@host ]"));
        CHECK(has(message, "[x] Don't show again"));
        CHECK(has(message, "[-] Detach"));
        CHECK(has(message, "[x] Remember my choice"));
        CHECK(has(message, "[ ] Plate A"));
        CHECK(has(message, "[x] Plate B"));
        CHECK(has(message, "( ) User preset | (*) Project preset"));
        CHECK(has(message, "[B ▾]"));
        CHECK(has(message, "[3]"));
        CHECK(has(message, "[45%]"));
        CHECK(has(message, "[0.000] (disabled)"));
        CHECK_FALSE(has(message, "[Generic PLA] (disabled)"));
        CHECK(has(message, "Settings\nInside the box"));
        CHECK(has(message, "( ) General | (*) Advanced\nAdvanced page"));
        CHECK_FALSE(has(message, "General page"));
        CHECK(has(message, "- [x] PLA\n- [ ] PETG"));
        CHECK(has(message, "- 192.168.1.0 | printer0"));
        CHECK(has(message, "- 192.168.1.24 | printer24\n- … more rows"));
        CHECK_FALSE(has(message, "printer25"));
        CHECK_FALSE(has(message, "Nested text"));
        CHECK(described["buttons"] ==
              nlohmann::json::array({{{"id", wxID_OK}, {"label", "OK"}, {"enabled", false}}, {{"id", wxID_CANCEL}, {"label", "Cancel"}}}));

        // Under another modal the dialog is disabled as a whole, which leaves its controls as they read.
        dialog.Disable();
        const std::string under = describe_dialog(&dialog)["message"].get<std::string>();
        CHECK(has(under, "Name | [Generic PLA]"));
        CHECK_FALSE(has(under, "[Generic PLA] (disabled)"));
        dialog.Enable();
        nested.Hide();
    }
}

TEST_CASE("a dialog's cells read in rows, and the cells side by side in a row join", "[Mcp]")
{
    auto cell = [](int x, int y, int w, int h, const std::string& text, bool toggle = false) {
        MessageCell c;
        c.rect   = wxRect(x, y, w, h);
        c.text   = text;
        c.toggle = toggle;
        return c;
    };
    // Radio boxes made before their labels each take the label on their row.
    std::vector<MessageCell> out = layout_cells({cell(0, 0, 16, 16, "(*)", true), cell(0, 30, 16, 16, "( )", true),
                                                 cell(20, 0, 100, 16, "Textured PEI"), cell(20, 30, 100, 16, "Smooth PEI")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "(*) Textured PEI");
    CHECK_FALSE(out[0].toggle);
    CHECK(out[1].text == "( ) Smooth PEI");
    // A table's header and rows, each cell in a panel of its own, read a row to a line.
    out = layout_cells({cell(0, 0, 100, 20, "Settings"), cell(100, 0, 60, 20, "Old Value"), cell(160, 0, 60, 20, "New Value"),
                        cell(0, 20, 100, 20, "Layer height"), cell(100, 20, 60, 20, "0.2"), cell(160, 20, 60, 20, "0.28")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "Settings | Old Value | New Value");
    CHECK(out[1].text == "Layer height | 0.2 | 0.28");
    // A label's trailing spaces don't widen the separator.
    out = layout_cells({cell(0, 0, 80, 20, "Start PA: "), cell(100, 0, 60, 20, "[0]")});
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "Start PA: | [0]");
    // A label above its field reads first, whichever was made first.
    out = layout_cells({cell(0, 20, 80, 20, "[0.4]"), cell(0, 0, 80, 16, "Nozzle diameter")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "Nozzle diameter");
    CHECK(out[1].text == "[0.4]");
    // A toggle takes a label of several lines; such a label joins nothing else.
    out = layout_cells({cell(0, 0, 16, 16, "[x]", true), cell(20, 0, 200, 40, "Long\nlabel"), cell(230, 0, 50, 16, "after")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "[x] Long\nlabel");
    CHECK(out[1].text == "after");
    // A toggle's suffix follows its label; two toggles side by side don't take each other.
    MessageCell disabled = cell(0, 0, 16, 16, "[ ]", true);
    disabled.suffix      = " (disabled)";
    out = layout_cells({disabled, cell(20, 0, 16, 16, "[x]", true), cell(40, 0, 80, 16, "Time-lapse")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].toggle);
    CHECK(out[1].text == "[x] Time-lapse");
    // A tall block beside a column of label and field rows leaves those rows as they are.
    out = layout_cells({cell(0, 0, 100, 200, "About\nthese"), cell(120, 10, 60, 20, "Width"), cell(200, 10, 60, 20, "[200]"),
                        cell(120, 40, 60, 20, "Depth"), cell(200, 40, 60, 20, "[250]")});
    REQUIRE(out.size() == 3);
    CHECK(out[0].text == "About\nthese");
    CHECK(out[1].text == "Width | [200]");
    CHECK(out[2].text == "Depth | [250]");
    // A checkbox with its own label is no label for the unlabelled one before it.
    MessageCell detach = cell(20, 0, 100, 16, "[x] Detach");
    detach.marked      = true;
    out = layout_cells({cell(0, 0, 16, 16, "[ ]", true), detach});
    REQUIRE(out.size() == 2);
    CHECK(out[0].toggle);
    CHECK(out[1].text == "[x] Detach");
    // A checkbox and its label, once joined, are no label for a second checkbox after them.
    out = layout_cells({cell(0, 0, 16, 16, "[ ]", true), cell(20, 0, 60, 16, "Detach"), cell(90, 0, 16, 16, "[x]", true)});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "[ ] Detach");
    CHECK(out[0].marked);
    CHECK(out[1].toggle);
    // Beside a one-line tall cell, the band is the shorter cell's height: a field at its top shares
    // its row, one a field's height lower does not.
    out = layout_cells({cell(160, 10, 80, 20, "[5]"), cell(0, 0, 150, 200, "Preview")});
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "Preview | [5]");
    out = layout_cells({cell(160, 20, 80, 20, "[5]"), cell(0, 0, 150, 200, "Preview")});
    REQUIRE(out.size() == 2);
    out = layout_cells({cell(160, 30, 80, 20, "[5]"), cell(0, 0, 150, 200, "Preview")});
    REQUIRE(out.size() == 2);
    // A field below a ten-line block's first line keeps its own row, however tall the field.
    out = layout_cells({cell(0, 0, 150, 200, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10"), cell(160, 20, 80, 40, "[5]")});
    REQUIRE(out.size() == 2);
    CHECK(out[1].text == "[5]");
    // Rows closer together than the tall block's first lines stay rows of their own too.
    out = layout_cells({cell(0, 0, 100, 200, "About\nthese"), cell(120, 0, 60, 20, "Width"), cell(200, 0, 60, 20, "[200]"),
                        cell(120, 25, 60, 20, "Depth"), cell(200, 25, 60, 20, "[250]")});
    REQUIRE(out.size() == 3);
    CHECK(out[1].text == "Width | [200]");
    CHECK(out[2].text == "Depth | [250]");
    // A field beside a wrapped label's first line shares its row, so the two read left to right, also
    // with a top margin of up to half its own height.
    out = layout_cells({cell(160, 10, 80, 20, "[5]"), cell(0, 0, 150, 60, "Long\nwrapped label")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "Long\nwrapped label");
    CHECK(out[1].text == "[5]");
    out = layout_cells({cell(160, 0, 80, 20, "[5]"), cell(0, 0, 150, 60, "Long\nwrapped label")});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "Long\nwrapped label");
    CHECK(out[1].text == "[5]");
    // A checkbox at a wrapped label's first line, or centred on it, takes it; one at its third line doesn't.
    out = layout_cells({cell(0, 0, 16, 16, "[x]", true), cell(20, 0, 200, 60, "A label\nwrapped over\nthree lines")});
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "[x] A label\nwrapped over\nthree lines");
    out = layout_cells({cell(0, 22, 16, 16, "[x]", true), cell(20, 0, 200, 60, "A label\nwrapped over\nthree lines")});
    REQUIRE(out.size() == 1);
    out = layout_cells({cell(0, 42, 16, 16, "[x]", true), cell(20, 0, 200, 60, "A label\nwrapped over\nthree lines")});
    REQUIRE(out.size() == 2);
    CHECK(out[1].toggle);
    // A switch centred on a title and its hint takes the title, as RecordingDialog lays them out.
    out = layout_cells({cell(0, 0, 200, 19, "Highlight the pointer"), cell(0, 21, 300, 16, "A ring follows the pointer."),
                        cell(400, 8, 40, 22, "[ ]", true)});
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "Highlight the pointer | [ ]");
    CHECK(out[1].text == "A ring follows the pointer.");
    // A toggle with nothing to its right takes the label to its left, as Preferences lays out its rows.
    out = layout_cells({cell(0, 0, 150, 20, "Show splash screen"), cell(200, 2, 16, 16, "[x]", true)});
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "Show splash screen | [x]");
    CHECK_FALSE(out[0].toggle);
    // Cells not laid out yet overlap at the origin: they keep their order, and none joins another.
    out = layout_cells({cell(0, 0, 80, 20, "one"), cell(0, 0, 80, 20, "two"), cell(0, 0, 16, 16, "[x]", true)});
    REQUIRE(out.size() == 3);
    CHECK(out[0].text == "one");
    CHECK(out[1].text == "two");
    CHECK(out[2].toggle);

    CHECK(selector_line({"Open as project", "Import geometry only"}, 1) == "( ) Open as project | (*) Import geometry only");
    CHECK(selector_line({"Left Nozzle", "Right Nozzle"}, -1) == "( ) Left Nozzle | ( ) Right Nozzle");
    CHECK(is_secret_key("printhost_password"));
    CHECK(is_secret_key("printhost_apikey"));
    CHECK_FALSE(is_secret_key("print_host"));
    CHECK_FALSE(is_secret_key("layer_height"));
}

TEST_CASE("a disabled or hidden button is neither pressed nor ends its dialog, and a rule naming it is not counted", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxDialog dialog(nullptr, wxID_ANY, "Save preset");
        int      presses = 0;
        auto*    ok      = new wxButton(&dialog, wxID_OK, "OK");
        ok->Bind(wxEVT_BUTTON, [&presses](wxCommandEvent&) { ++presses; });
        ok->Disable();
        CHECK(refused_press(&dialog, wxID_OK) == DialogPress::Disabled);
        CHECK_FALSE(has_dialog_button(&dialog, wxID_OK));
        CHECK(press_dialog_button(&dialog, wxID_OK) == DialogPress::Disabled);
        CHECK(presses == 0);
        CHECK(dialog.GetReturnCode() != wxID_OK);

        // Named by a rule, it is not pressed once the dialog's loop runs, and the rule is not counted.
        {
            ModalTracker tracker;
            tracker.push_rules({make_rule("Save preset", "ok")});
            CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
            wxTheApp->ProcessPendingEvents();
            CHECK(presses == 0);
            CHECK(tracker.rules_json()[0]["uses"] == 0);
            wxModalDialogHook::CallExit(&dialog);
        }

        ok->Enable();
        CHECK_FALSE(refused_press(&dialog, wxID_OK).has_value());
        CHECK(press_dialog_button(&dialog, wxID_OK) == DialogPress::Pressed);
        CHECK(presses == 1);

        // A wizard's Finish on a page it does not show yet is not pressed, nor found by its label; its
        // Cancel there ends the dialog as a dialog without one ends.
        wxDialog wizard(nullptr, wxID_ANY, "Wizard");
        auto*    pages = new wxSimplebook(&wizard);
        auto*    first = new wxPanel(pages);
        new wxStaticText(first, wxID_ANY, "Step 1");
        auto* last     = new wxPanel(pages);
        int   finishes = 0;
        auto* finish   = new wxButton(last, wxID_OK, "Finish");
        finish->Bind(wxEVT_BUTTON, [&finishes](wxCommandEvent&) { ++finishes; });
        new wxButton(last, wxID_CANCEL, "Cancel");
        pages->AddPage(first, "");
        pages->AddPage(last, "");
        pages->SetSelection(0);
        CHECK(refused_press(&wizard, wxID_OK) == DialogPress::Hidden);
        CHECK(dialog_button_by_label(&wizard, "Finish") == wxID_NONE);
        CHECK(press_dialog_button(&wizard, wxID_OK) == DialogPress::Hidden);
        CHECK(finishes == 0);
        CHECK_FALSE(refused_press(&wizard, wxID_CANCEL).has_value());
        CHECK(describe_dialog(&wizard)["buttons"].empty());
        pages->SetSelection(1);
        CHECK(dialog_button_by_label(&wizard, "Finish") == wxID_OK);
        CHECK(press_dialog_button(&wizard, wxID_OK) == DialogPress::Pressed);
        CHECK(finishes == 1);
        pages->SetSelection(0);
        CHECK(press_dialog_button(&wizard, wxID_CANCEL) == DialogPress::Ended);
        CHECK(wizard.GetReturnCode() == wxID_CANCEL);

        // A popup's or a nested dialog's buttons are not the dialog's.
        wxDialog host(nullptr, wxID_ANY, "Host");
        auto*    popup  = new wxPopupWindow(&host);
        int      popped = 0;
        auto*    popup_cancel = new wxButton(popup, wxID_CANCEL, "Cancel");
        popup_cancel->Bind(wxEVT_BUTTON, [&popped](wxCommandEvent&) { ++popped; });
        popup->Show();
        wxDialog nested(&host, wxID_ANY, "Nested");
        new wxButton(&nested, wxID_APPLY, "Nested apply");
        nested.Show();
        CHECK(dialog_button_by_label(&host, "Cancel") == wxID_NONE);
        CHECK(dialog_button_by_label(&host, "Nested apply") == wxID_NONE);
        CHECK_FALSE(has_dialog_button(&host, wxID_CANCEL));
        CHECK_FALSE(refused_press(&host, wxID_APPLY).has_value());
        CHECK(describe_dialog(&host)["buttons"].empty());
        CHECK(popped == 0);
        popup->Hide();
        nested.Hide();

        // A message box that is not shown can't be pressed.
        wxMessageDialog box(nullptr, "Continue?", "Question", wxOK | wxCANCEL);
        CHECK_FALSE(refused_press(&box, wxID_OK).has_value());
#if defined(_WIN32) || defined(__WXGTK__)
        CHECK(press_dialog_button(&box, wxID_OK) == DialogPress::Failed);
#endif
    }
}

TEST_CASE("a childless window is a control by the label it shows, not by a default name", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxDialog dialog(nullptr, wxID_ANY, "Kinds");
        // Windows shows a bare panel's name as its text.
        auto* bare = new wxPanel(&dialog);
        bare->SetLabel("panel");
        CHECK(kind_of(bare).empty());
        auto* titled = new wxWindow(&dialog, wxID_ANY);
        titled->SetLabel("Settings");
        CHECK(kind_of(titled) == "control");
        // A label the window is also named after is still its label.
        auto* named = new wxWindow(&dialog, wxID_ANY);
        named->SetName("Advanced");
        named->SetLabel("Advanced");
        CHECK(kind_of(named) == "control");
        CHECK(meaningful_name(named) == "Advanced");
        CHECK(meaningful_name(bare).empty());
        // Every class's default name, a gauge's and a tree's too.
        auto* gauge = new wxGauge(&dialog, wxID_ANY, 100);
        CHECK(kind_of(gauge) != "control");
        auto* tree_named = new wxWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, "treeCtrl");
        tree_named->SetLabel("treeCtrl");
        CHECK(kind_of(tree_named).empty());
        // A section title that happens to be another class's default name is still a title.
        auto* section = new wxWindow(&dialog, wxID_ANY);
        section->SetLabel("Text");
        CHECK(kind_of(section) == "control");
    }
}

TEST_CASE("a rule answers a message box only with a button the box has", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        ModalTracker tracker;
        tracker.push_rules({make_rule("Discard", "ok")});
        wxMessageDialog yes_no(nullptr, "Discard the changes?", "Changes", wxYES_NO);
        // A Yes/No box has no OK: it opens for an answer instead.
        CHECK(wxModalDialogHook::CallEnter(&yes_no) == wxID_NONE);
        CHECK(tracker.top() == &yes_no);
        CHECK(tracker.rules_json()[0]["uses"] == 0);
        wxModalDialogHook::CallExit(&yes_no);
        tracker.push_rules({make_rule("Discard", "no")});
        CHECK(wxModalDialogHook::CallEnter(&yes_no) == wxID_NO);
        CHECK(tracker.top() == nullptr);
        // Cancel answers a box with a Cancel button, not an OK-only one.
        tracker.push_rules({make_rule("Saved", "cancel")});
        wxMessageDialog ok_only(nullptr, "Saved.", "Info", wxOK);
        CHECK(wxModalDialogHook::CallEnter(&ok_only) == wxID_NONE);
        wxModalDialogHook::CallExit(&ok_only);
        wxMessageDialog ok_cancel(nullptr, "Saved. Open it?", "Info", wxOK | wxCANCEL);
        CHECK(wxModalDialogHook::CallEnter(&ok_cancel) == wxID_CANCEL);
    }
}

TEST_CASE("a long label is cut where a character starts, and a toggle's tooltip name stays whole", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        std::string tip;
        for (int i = 0; i < 31; ++i)
            tip += "\xF0\x9F\x98\x80";
        wxDialog dialog(nullptr, wxID_ANY, "Overlay");
        auto*    toggle = new wxBitmapToggleButton(&dialog, wxID_ANY, wxBitmap(16, 16));
        toggle->SetToolTip(wxString::FromUTF8(tip));
        // list_widgets's label is cut at 120 bytes, where a character starts.
        const std::string label = label_of(toggle);
        CHECK(label.size() < tip.size());
        CHECK(label.substr(label.size() - 3) == "...");
        CHECK((label.size() - 3) % 4 == 0);
        CHECK_NOTHROW(nlohmann::json(label).dump());
        // The message names the toggle by its whole tooltip.
        const nlohmann::json described = describe_dialog(&dialog);
        CHECK_NOTHROW(described.dump());
        CHECK(has(described["message"].get<std::string>(), "[ ] " + tip));
    }
}

namespace {

// A label that can't be read, as a widget in a bad state might fail.
class FailingLabel final : public wxStaticText
{
public:
    explicit FailingLabel(wxWindow* parent) : wxStaticText(parent, wxID_ANY, "label") {}
    wxString GetLabel() const override { throw std::runtime_error("unreadable"); }
};

} // namespace

TEST_CASE("a toggle left without a label is named by its own tooltip, not by its neighbour's label", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        // Two switches and one label: the first takes it, the second its tooltip.
        wxDialog dialog(nullptr, wxID_ANY, "Switches");
        auto*    row    = new wxBoxSizer(wxHORIZONTAL);
        auto*    first  = new wxBitmapToggleButton(&dialog, wxID_ANY, wxBitmap(16, 16));
        auto*    label  = new wxStaticText(&dialog, wxID_ANY, "Detach");
        auto*    second = new wxBitmapToggleButton(&dialog, wxID_ANY, wxBitmap(16, 16));
        second->SetToolTip("Keep the original");
        row->Add(first, 0, wxALL, 4);
        row->Add(label, 0, wxALL, 4);
        row->Add(second, 0, wxALL, 4);
        dialog.SetSizerAndFit(row);
        const std::string message = describe_dialog(&dialog)["message"].get<std::string>();
        INFO(message);
        CHECK(has(message, "[ ] Detach"));
        CHECK(has(message, "[ ] Keep the original"));
        CHECK(message.find("Detach") == message.rfind("Detach"));

        // A setting's toggle whose setting has no label beside a wrapped label it can't take: named by
        // its window name, not by that label.
        wxDialog setting(nullptr, wxID_ANY, "Setting");
        auto*    line   = new wxBoxSizer(wxHORIZONTAL);
        auto*    text   = new wxStaticText(&setting, wxID_ANY, "Wrapped\nneighbour");
        auto*    toggle = new wxBitmapToggleButton(&setting, wxID_ANY, wxBitmap(16, 16), wxDefaultPosition, wxDefaultSize, 0,
                                                   wxDefaultValidator, "unlisted_toggle");
        toggle->SetToolTip("parameter name: not_a_setting");
        line->Add(text, 0, wxALL, 4);
        line->Add(toggle, 0, wxALL, 4);
        setting.SetSizerAndFit(line);
        const std::string read = describe_dialog(&setting)["message"].get<std::string>();
        INFO(read);
        CHECK(has(read, "[ ] (unlisted_toggle)"));
        CHECK(read.find("neighbour") == read.rfind("neighbour"));
    }
}

TEST_CASE("a button is found by the label its dialog's description shows", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        // "&&" is a literal "&": the description shows "Save & close", and that label finds it.
        wxDialog dialog(nullptr, wxID_ANY, "Labels");
        new wxButton(&dialog, 5200, "Save && close");
        new wxButton(&dialog, 5201, "&Discard");
        const nlohmann::json buttons = describe_dialog(&dialog)["buttons"];
        REQUIRE(buttons.size() == 2);
        CHECK(buttons[0]["label"] == "Save & close");
        CHECK(buttons[1]["label"] == "Discard");
        CHECK(dialog_button_by_label(&dialog, "Save & close") == 5200);
        CHECK(dialog_button_by_label(&dialog, "discard") == 5201);
    }
}

TEST_CASE("a notebook's tabs are no label for a toggle beside them", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        wxDialog dialog(nullptr, wxID_ANY, "Tabs");
        auto*    row    = new wxBoxSizer(wxHORIZONTAL);
        auto*    toggle = new wxBitmapToggleButton(&dialog, wxID_ANY, wxBitmap(16, 16));
        auto*    book   = new wxNotebook(&dialog, wxID_ANY);
        book->AddPage(new wxPanel(book), "General");
        book->AddPage(new wxPanel(book), "Advanced", true);
        row->Add(toggle, 0, wxALL, 4);
        row->Add(book, 0, wxALL, 4);
        dialog.SetSizerAndFit(row);
        const std::string message = describe_dialog(&dialog)["message"].get<std::string>();
        INFO(message);
        CHECK(has(message, "( ) General | (*) Advanced"));
        CHECK_FALSE(has(message, "[ ] ( ) General"));
    }
}

TEST_CASE("a dialog that can't be described is still listed, by its title", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        ModalTracker tracker;
        tracker.push_rules({make_rule("^Broken$", "cancel")});
        wxDialog dialog(nullptr, wxID_ANY, "Broken");
        new FailingLabel(&dialog);
        int cancels = 0;
        auto* cancel = new wxButton(&dialog, wxID_CANCEL, "Cancel");
        cancel->Bind(wxEVT_BUTTON, [&cancels](wxCommandEvent&) { ++cancels; });
        CHECK_THROWS(describe_dialog(&dialog));
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        CHECK(tracker.any_open());
        const nlohmann::json snap = tracker.snapshot();
        REQUIRE(snap.size() == 1);
        CHECK(snap[0]["title"] == "Broken");
        // Its rule matches it by its title alone, and presses its button.
        wxTheApp->ProcessPendingEvents();
        CHECK(cancels == 1);
        wxModalDialogHook::CallExit(&dialog);
        CHECK_FALSE(tracker.any_open());
    }
}

TEST_CASE("set_widget_value false is refused on a selected radio button, which selecting another unselects", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        McpServicePtr service(new McpService());
        UiInspector   inspector(*service);
        wxFrame       frame(nullptr, wxID_ANY, "radios");
        auto*         first  = new wxRadioButton(&frame, wxID_ANY, "First", wxPoint(0, 0), wxDefaultSize, wxRB_GROUP);
        auto*         second = new wxRadioButton(&frame, wxID_ANY, "Second", wxPoint(0, 30));
        first->SetValue(true);
        UiTarget target;
        target.kind   = UiTarget::Kind::Window;
        target.window = first;
        CHECK_THROWS_AS(inspector.input(target, false, false), ToolFailure);
        CHECK(first->GetValue());
        // One already unselected stays so.
        target.window = second;
        CHECK(inspector.input(target, false, false)["checked"] == false);
        CHECK(inspector.input(target, true, false)["checked"] == true);
        CHECK(second->GetValue());
    }
}

TEST_CASE("a dialog's message is cut where a character starts", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        // 3000 bytes of a 3-byte character: the 2000-byte cut falls inside one.
        std::string long_text;
        for (int i = 0; i < 1000; ++i)
            long_text += "\xE4\xB8\xAD";
        wxDialog dialog(nullptr, wxID_ANY, "Long");
        new wxStaticText(&dialog, wxID_ANY, wxString::FromUTF8(long_text));
        std::string       text;
        const nlohmann::json        described = describe_dialog(&dialog, &text);
        const std::string message   = described["message"].get<std::string>();
        REQUIRE(message.size() > 3);
        CHECK(message.substr(message.size() - 3) == "...");
        CHECK((message.size() - 3) % 3 == 0);
        CHECK_NOTHROW(described.dump());
        CHECK((text.size() - 3) % 3 == 0);
        CHECK_NOTHROW(nlohmann::json(text).dump());
    }
}

TEST_CASE("the snapshot read on the UI thread shows a dialog as it is now", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        ModalTracker tracker;
        wxDialog     dialog(nullptr, wxID_ANY, "Bind printer");
        auto*        name = new wxStaticText(&dialog, wxID_ANY, "");
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        // What a dialog fills in once it shows.
        name->SetLabel("Bambu Lab X1C");
        CHECK(tracker.snapshot()[0]["message"] == "Bambu Lab X1C");
        wxModalDialogHook::CallExit(&dialog);
        CHECK(tracker.snapshot().empty());
    }
}

TEST_CASE("a rule finds the dialog's own text past what its message holds", "[Mcp]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    GuiApp app;
    REQUIRE(app.init.IsOk());
    {
        // A long G-code and a long list before the label fill the message; the label is still the
        // dialog's text, as it always was.
        wxDialog dialog(nullptr, wxID_ANY, "Edit Custom G-code");
        auto*    sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxTextCtrl(&dialog, wxID_ANY, std::string(3000, 'G'), wxDefaultPosition, wxSize(300, 100), wxTE_MULTILINE));
        wxArrayString placeholders;
        for (int i = 0; i < 100; ++i)
            placeholders.Add("placeholder_" + std::to_string(i));
        sizer->Add(new wxListBox(&dialog, wxID_ANY, wxDefaultPosition, wxSize(300, 100), placeholders));
        sizer->Add(new wxStaticText(&dialog, wxID_ANY, "Select placeholder"));
        int   cancels = 0;
        auto* cancel  = new wxButton(&dialog, wxID_CANCEL, "Cancel");
        cancel->Bind(wxEVT_BUTTON, [&cancels](wxCommandEvent&) { ++cancels; });
        sizer->Add(cancel);
        dialog.SetSizerAndFit(sizer);

        std::string text;
        const nlohmann::json  described = describe_dialog(&dialog, &text);
        CHECK_FALSE(has(described["message"].get<std::string>(), "Select placeholder"));
        CHECK(has(text, "Select placeholder"));
        // Its buttons are listed all the same.
        CHECK(described["buttons"] == nlohmann::json::array({{{"id", wxID_CANCEL}, {"label", "Cancel"}}}));

        ModalTracker tracker;
        tracker.push_rules({make_rule("Select placeholder", "cancel")});
        CHECK(wxModalDialogHook::CallEnter(&dialog) == wxID_NONE);
        wxTheApp->ProcessPendingEvents();
        CHECK(cancels == 1);
        CHECK(tracker.rules_json()[0]["uses"] == 1);
        wxModalDialogHook::CallExit(&dialog);
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
    CommandCatalog              registry;
    std::atomic<bool>           release{false};
    ToolResult                  build_result;
    std::atomic<int>            builds{0};
    std::atomic<int>            edits{0};
    std::unique_ptr<McpCore>    core;

    JobJournal()
    {
        NativeCommand build;
        build.key       = "build";
        build.budget_ms = 100;
        build.run       = [this](const nlohmann::json&, ToolContext&) {
            ++builds;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return build_result;
        };
        registry.add(build);
        NativeCommand edit;
        edit.key     = "edit";
        edit.run     = [this](const nlohmann::json&, ToolContext&) {
            ++edits;
            return ToolResult::ok({{"edited", true}});
        };
        registry.add(edit);
        NativeCommand wait;
        wait.key         = "wait";
        wait.guard       = GuardClass::Inspect;
        wait.inline_call = true;
        wait.run         = [this](const nlohmann::json& args, ToolContext&) {
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

TEST_CASE("a mutation whose job fails after its reply is dropped from the replay, which replays the rest", "[Mcp]")
{
    JobJournal h;
    h.build_result          = ToolResult::error("The thread could not be built");
    const std::string job   = h.start_build();
    h.release               = true;
    const ToolResult failed = h.call("wait", {{"for", "job"}, {"job", job}});
    CHECK(failed.is_error);
    CHECK_FALSE(h.call("edit", {{"value", 3}}).is_error);

    nlohmann::json running;
    const nlohmann::json exported = h.journal.to_replay("t", &running);
    REQUIRE(exported["steps"].size() == 1);
    CHECK(exported["steps"][0]["tool"] == "edit");
    CHECK(exported["steps"][0]["args"] == nlohmann::json{{"value", 3}});
    CHECK(running.empty());

    // The replay runs the edit alone, as a run_replay job's children.
    std::string                 err;
    const std::optional<Replay> parsed = parse_replay(exported.dump(), err);
    REQUIRE(parsed.has_value());
    const std::string parent = h.jobs.create("run_replay", "");
    ReplayContext     sctx;
    ReplayHost        host;
    host.call     = [&](const std::string& tool, const nlohmann::json& args) { return h.core->call_child(*h.registry.find(tool), args, parent); };
    host.wait_job = [&](const std::string& id, int timeout_s) { return h.jobs.wait(id, std::chrono::seconds(timeout_s)); };
    const nlohmann::json replay = run_replay(*parsed, sctx, host);
    h.jobs.finish(parent, ToolResult::ok());
    CHECK(replay["passed"] == true);
    CHECK(h.builds.load() == 1);
    CHECK(h.edits.load() == 2);
    // A replay's own steps are not journaled.
    CHECK(h.journal.to_replay("t")["steps"].size() == 1);
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
    nlohmann::json steps = h.journal.to_replay("t", &running)["steps"];
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["tool"] == "edit");
    CHECK(running == nlohmann::json::array({{{"tool", "build"}, {"job", job}}}));

    h.release = true;
    CHECK(h.call("wait", {{"for", "job"}, {"job", job}}).data["built"] == true);
    steps = h.journal.to_replay("t", &running)["steps"];
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

TEST_CASE("a job that waits on a dialog stays in the replay, and a call yet to reply is left out", "[Mcp]")
{
    Journal        journal;
    const uint64_t open = journal.begin("run_action", {{"id", "orca_command:open_preferences:orca"}});
    journal.job_started(open, "j1");
    journal.end(open, ToolResult::ok({{"status", "blocked_by_dialog"}, {"job", "j1"}, {"tool", "run_action"}}));
    const uint64_t answer = journal.begin("answer_dialog", {{"button", "cancel"}});
    journal.job_started(answer, "j2");
    journal.job_ended(answer, ToolResult::ok());
    journal.end(answer, ToolResult::ok());
    const uint64_t slice = journal.begin("slice", nlohmann::json::object());
    journal.job_started(slice, "j3");
    journal.begin("screenshot", {{"target", "canvas"}});

    nlohmann::json running;
    nlohmann::json steps = journal.to_replay("t", &running)["steps"];
    REQUIRE(steps.size() == 2);
    CHECK(steps[0]["save_as"] == "job1");
    CHECK(steps[1]["tool"] == "answer_dialog");
    CHECK(running == nlohmann::json::array({{{"tool", "slice"}, {"job", "j3"}}, {{"tool", "screenshot"}}}));

    // The dialog's answer follows it in the replay, so it stays when its job then fails.
    journal.job_ended(open, ToolResult::error("Cancelled"));
    // A job that ends before its reply is recorded is in the replay at once.
    journal.job_ended(slice, ToolResult::ok());
    steps = journal.to_replay("t", &running)["steps"];
    REQUIRE(steps.size() == 3);
    CHECK(steps[0]["tool"] == "run_action");
    CHECK(steps[2]["tool"] == "slice");
    CHECK_FALSE(steps[2].contains("await"));
    CHECK(running == nlohmann::json::array({{{"tool", "screenshot"}}}));
}

TEST_CASE("a job that was running when a dialog was answered stays in the replay before the answer", "[Mcp]")
{
    // A replay past its reply budget opens Preferences, which a later call answers.
    Journal        journal;
    const uint64_t replay_call = journal.begin("run_replay", {{"path", "s.json"}});
    journal.job_started(replay_call, "j1");
    journal.end(replay_call, ToolResult::ok({{"status", "running"}, {"job", "j1"}, {"tool", "run_replay"}}));
    // A call still within its reply budget, whose dialog is answered before it replies.
    const uint64_t open = journal.begin("run_action", {{"id", "orca_command:open_preferences:orca"}});
    journal.job_started(open, "j2");
    const uint64_t slice = journal.begin("slice", nlohmann::json::object());
    journal.job_started(slice, "j3");
    journal.end(slice, ToolResult::ok({{"status", "running"}, {"job", "j3"}, {"tool", "slice"}}));
    journal.job_ended(slice, ToolResult::error("Cancelled"));
    const uint64_t answer = journal.begin("answer_dialog", {{"button", "close"}}, true);
    journal.job_started(answer, "j4");
    journal.job_ended(answer, ToolResult::ok());
    journal.end(answer, ToolResult::ok());
    // Input with no dialog open ties nothing to the jobs then running.
    const uint64_t click = journal.begin("click", {{"target", "w1"}});
    journal.job_started(click, "j5");
    journal.job_ended(click, ToolResult::ok());
    journal.end(click, ToolResult::ok());
    const uint64_t later = journal.begin("run_action", {{"id", "obj_arrange"}});
    journal.job_started(later, "j6");
    journal.end(later, ToolResult::ok({{"status", "running"}, {"job", "j6"}, {"tool", "run_action"}}));
    journal.begin("click", {{"target", "w2"}});

    nlohmann::json running;
    nlohmann::json steps = journal.to_replay("t", &running)["steps"];
    REQUIRE(steps.size() == 5);
    CHECK(steps[0]["tool"] == "run_replay");
    CHECK(steps[0]["await"] == false);
    CHECK(steps[1]["tool"] == "run_action");
    CHECK(steps[1]["await"] == false);
    // The answer waits for its dialog, which the replayed job may open later than its reply.
    CHECK(steps[2] == nlohmann::json({{"tool", "wait"}, {"args", {{"for", "dialog"}, {"timeout_s", 50}}}, {"expect", {{"/satisfied", {{"eq", true}}}}}}));
    CHECK(steps[3]["tool"] == "answer_dialog");
    CHECK(steps[4]["tool"] == "click");
    CHECK(running == nlohmann::json::array({{{"tool", "run_action"}, {"job", "j6"}}, {{"tool", "click"}}}));

    // They stay whatever their jobs' results: the answer after them needs a dialog one of them opens.
    journal.job_ended(replay_call, ToolResult::error("Step 4 failed"));
    journal.job_ended(open, ToolResult::ok());
    journal.end(open, ToolResult::ok({{"ran", "orca_command:open_preferences:orca"}}));
    steps = journal.to_replay("t")["steps"];
    REQUIRE(steps.size() == 5);
    CHECK(steps[0]["tool"] == "run_replay");
    CHECK(steps[1]["await"] == false);
}

TEST_CASE("an exported answer to a dialog a job opens after its reply replays once the dialog is open", "[Mcp]")
{
    // A replay replies running, opens Preferences later, and is answered while it keeps running.
    Journal        journal;
    const uint64_t replay_call = journal.begin("run_replay", {{"path", "s.json"}});
    journal.job_started(replay_call, "j1");
    journal.end(replay_call, ToolResult::ok({{"status", "running"}, {"job", "j1"}, {"tool", "run_replay"}}));
    const uint64_t answer = journal.begin("answer_dialog", {{"button", "close"}}, true);
    journal.job_started(answer, "j2");
    journal.job_ended(answer, ToolResult::ok());
    journal.end(answer, ToolResult::ok());
    const uint64_t waited = journal.begin("wait", {{"for", "job"}, {"job", "j1"}});
    journal.end(waited, ToolResult::ok());
    journal.job_ended(replay_call, ToolResult::ok());

    std::string                 err;
    const std::optional<Replay> parsed = parse_replay(journal.to_replay("t").dump(), err);
    REQUIRE(parsed.has_value());
    // The replay opens its dialog 300 ms after its reply; an answer before that is refused.
    std::atomic<bool> dialog{false};
    std::thread       opener;
    std::vector<std::string> calls;
    ReplayHost        host;
    host.call = [&](const std::string& tool, const nlohmann::json& args) {
        calls.push_back(tool);
        if (tool == "run_replay") {
            opener = std::thread([&dialog] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                dialog = true;
            });
            return ToolResult::ok({{"status", "running"}, {"job", "r1"}, {"tool", "run_replay"}});
        }
        if (tool == "wait" && args["for"] == "dialog") {
            for (int i = 0; i < 100 && !dialog; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return ToolResult::ok({{"satisfied", dialog.load()}});
        }
        if (tool == "answer_dialog") {
            if (!dialog)
                return ToolResult::error("No modal dialog is open");
            dialog = false;
            return ToolResult::ok({{"pressed", "close"}});
        }
        CHECK(args["job"] == "r1");
        return ToolResult::ok({{"done", true}});
    };
    host.wait_job = [](const std::string&, int) { return std::optional<ToolResult>(ToolResult::ok()); };
    ReplayContext        sctx;
    const nlohmann::json replay = run_replay(*parsed, sctx, host);
    opener.join();
    CHECK(replay["passed"] == true);
    CHECK(calls == std::vector<std::string>{"run_replay", "wait", "answer_dialog", "wait"});
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

TEST_CASE("the send command is a Slice & Export command that takes no arguments", "[Mcp][SpeedDial]")
{
    const NativeCommand* send = NativeCommands::find("send_gcode");
    REQUIRE(send != nullptr);
    CHECK(send->title == "Send G-code to printer host");
    CHECK(send->group == "Slice & Export");
    CHECK(send->input_schema.at("properties").empty());
    CHECK(send->completion == CommandCompletion::None);
    CHECK(send->description.find("get_app_state.uploads") != std::string::npos);
}

TEST_CASE("run_action with a param is refused by name", "[Mcp][SpeedDial]")
{
    const NativeCommand* run_action = NativeCommands::find("run_action");
    REQUIRE(run_action != nullptr);
    nlohmann::json args{{"id", "x"}, {"param", "1"}};
    CHECK_THAT(argument_error(*run_action, args), Catch::Matchers::ContainsSubstring("param"));
    nlohmann::json given{{"id", "x"}, {"args", {{"percent", 50}}}};
    CHECK(argument_error(*run_action, given).empty());
}
