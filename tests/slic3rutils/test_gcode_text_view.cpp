// The preview UI is linked by slic3rutils_tests.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <algorithm>
#include <limits>
#include <cstddef>
#include <vector>
#include <string>
#include <ios>

#include <boost/nowide/fstream.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <wx/slider.h>

#include "test_utils.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/IMSlider.hpp"

using Slic3r::GUI::GCodeTextView;

TEST_CASE("G-code text bounds include the first and last source lines", "[GCodeTextView]")
{
    const uint64_t count = GENERATE(uint64_t(1), uint64_t(3), uint64_t(100));
    GCodeTextView view;
    view.update(count, 20, 1);
    CHECK(view.first_line() == 1);
    CHECK(view.last_line() == std::min(count, uint64_t(20)));
    view.scroll(std::numeric_limits<int>::max());
    CHECK(view.last_line() == count);
    view.scroll(std::numeric_limits<int>::min());
    CHECK(view.first_line() == 1);
}

TEST_CASE("Empty and zero-height G-code viewports have no readable row", "[GCodeTextView]")
{
    GCodeTextView view;
    view.update(0, 20, 0);
    CHECK(view.last_line() == 0);
    view.scroll(3);
    CHECK(view.first_line() == 1);
    view.update(100, 0, 100);
    CHECK(view.last_line() == 0);
    CHECK(view.first_line() == 1);
}

TEST_CASE("G-code browsing does not follow path updates until explicitly resumed", "[GCodeTextView]")
{
    GCodeTextView view;
    view.update(100, 10, 50);
    CHECK(view.first_line() == 45);
    view.scroll(3);
    CHECK_FALSE(view.following());
    view.update(100, 10, 80);
    CHECK(view.first_line() == 48);
    view.resume_following();
    view.update(100, 10, 80);
    CHECK(view.first_line() == 75);
}

TEST_CASE("G-code browsing survives resize and resets for another result", "[GCodeTextView]")
{
    GCodeTextView view;
    view.update(100, 10, 1);
    view.scroll_to(85);
    view.update(100, 12, 1);
    CHECK(view.first_line() == 85);
    view.update(100, 30, 1);
    CHECK(view.first_line() == 71);
    CHECK(view.last_line() == 100);
    view.reset();
    view.update(3, 30, 3);
    CHECK(view.following());
    CHECK(view.first_line() == 1);
    CHECK(view.last_line() == 3);
}

TEST_CASE("Programmatic preview slider updates do not count as user input", "[GCodeTextView]")
{
    Slic3r::GUI::IMSlider slider(0, 9, 0, 9, wxSL_HORIZONTAL);
    slider.SetHigherValue(5);
    CHECK(slider.is_dirty());
    CHECK_FALSE(slider.consume_user_interaction());
    slider.SetSelectionSpan(0, 4);
    CHECK_FALSE(slider.consume_user_interaction());
    slider.switch_one_layer_mode();
    CHECK(slider.consume_user_interaction());
    CHECK_FALSE(slider.consume_user_interaction());
}

TEST_CASE("G-code navigation resolves source gaps and keeps complete command groups", "[GCodeTextView][GCodeNavigation]")
{
    const std::vector<uint32_t> ids{10, 10, 20, 20, 20, 30};
    const auto [requested, expected_line, first, last] = GENERATE(
        table<uint32_t, uint32_t, size_t, size_t>({
            {1, 10, 0, 1}, {10, 10, 0, 1}, {19, 10, 0, 1},
            {20, 20, 2, 4}, {21, 20, 2, 4}, {1000, 30, 5, 5}}));
    const auto target = Slic3r::GUI::find_gcode_navigation_target(requested, ids.size(),
        [&](size_t i) { return ids[i]; }, true);
    REQUIRE(target.has_value());
    CHECK(target->line_id == expected_line);
    CHECK(target->first_vertex == first);
    CHECK(target->last_vertex == last);
}

TEST_CASE("G-code navigation rejects missing source identities", "[GCodeTextView][GCodeNavigation]")
{
    const auto zero = [](size_t) { return uint32_t(0); };
    CHECK_FALSE(Slic3r::GUI::find_gcode_navigation_target(1, 0, zero, true));
    CHECK_FALSE(Slic3r::GUI::find_gcode_navigation_target(0, 3, zero, true));
    CHECK_FALSE(Slic3r::GUI::find_gcode_navigation_target(1, 3, zero, true));
    const std::vector<uint32_t> ids{0, 0, 10, 10};
    const auto target = Slic3r::GUI::find_gcode_navigation_target(1, ids.size(),
        [&](size_t i) { return ids[i]; }, true);
    REQUIRE(target.has_value());
    CHECK(target->first_vertex == 2);
    CHECK(target->last_vertex == 3);
}

TEST_CASE("G-code navigation falls back safely for nonmonotonic source identities", "[GCodeTextView][GCodeNavigation]")
{
    const std::vector<uint32_t> ids{10, 10, 30, 20, 20, 30};
    const auto target = Slic3r::GUI::find_gcode_navigation_target(25, ids.size(),
        [&](size_t i) { return ids[i]; }, false);
    REQUIRE(target.has_value());
    CHECK(target->line_id == 20);
    CHECK(target->first_vertex == 3);
    CHECK(target->last_vertex == 4);
    const auto leading = Slic3r::GUI::find_gcode_navigation_target(1, ids.size(),
        [&](size_t i) { return ids[i]; }, false);
    REQUIRE(leading.has_value());
    CHECK(leading->line_id == 10);
}

TEST_CASE("Ordered G-code navigation does not scan the whole file", "[GCodeTextView][GCodeNavigation]")
{
    size_t reads = 0;
    const auto target = Slic3r::GUI::find_gcode_navigation_target(12345, 1000000000,
        [&](size_t i) { ++reads; return uint32_t(i / 2 + 1); }, true);
    REQUIRE(target.has_value());
    CHECK(target->line_id == 12345);
    CHECK(target->last_vertex == 24689);
    CHECK(reads < 80);
}

TEST_CASE("G-code command categories include their geometry and event types only", "[GCodeTextView][GCodeNavigation]")
{
    std::vector<libvgcode::PathVertex> vertices(4);
    vertices[0].type = vertices[1].type = libvgcode::EMoveType::Extrude;
    vertices[0].role = vertices[1].role = libvgcode::EGCodeExtrusionRole::Perimeter;
    vertices[2].type = libvgcode::EMoveType::Seam;
    vertices[3].type = libvgcode::EMoveType::Travel;
    const auto categories = Slic3r::GUI::gcode_navigation_categories({10, 0, 2},
        [&](size_t i) -> const libvgcode::PathVertex& { return vertices[i]; });
    CHECK(categories.roles[size_t(libvgcode::EGCodeExtrusionRole::Perimeter)]);
    CHECK_FALSE(categories.roles[size_t(libvgcode::EGCodeExtrusionRole::InternalInfill)]);
    CHECK(categories.options[size_t(libvgcode::EOptionType::Seams)]);
    CHECK_FALSE(categories.options[size_t(libvgcode::EOptionType::Travels)]);
    CHECK_FALSE(categories.options[size_t(libvgcode::EOptionType::Retractions)]);
}

TEST_CASE("Rebuilt move slider mappings retain vertex values rather than old tick indices", "[GCodeTextView][GCodeNavigation]")
{
    Slic3r::GUI::IMSlider slider(0, 2, 0, 2, wxSL_HORIZONTAL);
    slider.SetSliderValues({11, 21, 31});
    REQUIRE(slider.GetTickFromValue(21) == 1);
    slider.SetSliderValues({1, 6, 11, 21, 31});
    slider.SetMaxValue(4);
    CHECK(slider.GetTickFromValue(21) == 3);
    slider.SetLowerValue(slider.GetTickFromValue(21));
    slider.SetHigherValue(slider.GetTickFromValue(11));
    CHECK(slider.GetLowerValue() == 2);
    CHECK(slider.GetHigherValue() == 2);
    CHECK_FALSE(slider.consume_user_interaction());
}

TEST_CASE("G-code navigation enables global categories idempotently without changing others", "[GCodeTextView][GCodeNavigation]")
{
    libvgcode::Viewer viewer;
    const auto role = libvgcode::EGCodeExtrusionRole::Perimeter;
    const auto option = libvgcode::EOptionType::Travels;
    if (viewer.is_extrusion_role_visible(role))
        viewer.toggle_extrusion_role_visibility(role);
    if (viewer.is_option_visible(option))
        viewer.toggle_option_visibility(option);
    const bool other_role = viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole::InternalInfill);
    const bool other_option = viewer.is_option_visible(libvgcode::EOptionType::Retractions);
    Slic3r::GUI::GCodeNavigationCategories categories;
    categories.roles[size_t(role)] = true;
    categories.options[size_t(option)] = true;
    Slic3r::GUI::enable_gcode_navigation_categories(viewer, categories);
    Slic3r::GUI::enable_gcode_navigation_categories(viewer, categories);
    CHECK(viewer.is_extrusion_role_visible(role));
    CHECK(viewer.is_option_visible(option));
    CHECK(viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole::InternalInfill) == other_role);
    CHECK(viewer.is_option_visible(libvgcode::EOptionType::Retractions) == other_option);
    viewer.toggle_option_visibility(option);
    CHECK_FALSE(viewer.is_option_visible(option));
}

namespace {

// Exercise the real mapped-file text widget and its ImGui hit testing without OpenGL or a wx app.
struct TextFrames
{
    ScopedTemporaryFile file{".gcode"};
    Slic3r::GUI::ImGuiWrapper imgui;
    Slic3r::GUI::GCodeViewer::SequentialView::GCodeWindow window;
    std::vector<size_t> line_ends;

    explicit TextFrames(size_t count = 100)
    {
        imgui.set_display_size(800.f, 600.f);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        boost::nowide::ofstream output(file.string(), std::ios::binary);
        size_t offset = 0;
        for (size_t i = 1; i <= count; ++i) {
            const std::string line = "G1 X" + std::to_string(i) + " ; source row\n";
            output << line;
            offset += line.size();
            line_ends.push_back(offset);
        }
        output.close();
        window.load_gcode(file.string(), line_ends);
        frame();
        frame();
    }

    void frame(uint64_t current = 50)
    {
        ImGui::NewFrame();
        window.render(imgui, 20.f, 580.f, 750.f, current);
        ImGui::EndFrame();
    }

    ImVec2 row_point(size_t row) const
    {
        const ImGuiWindow* w = ImGui::FindWindowByName("G-code");
        const auto [first, last] = window.visible_lines();
        const float count = float(last - first + 1);
        const float text_height = ImGui::CalcTextSize("0").y;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float first_y = w->Pos.y + 0.5f * (560.f - count * text_height - (count - 1.f) * style.ItemSpacing.y);
        return ImVec2(w->Pos.x + style.WindowPadding.x + 4.f,
            first_y + float(row) * (text_height + style.ItemSpacing.y) + 0.5f * text_height);
    }
};

} // namespace

TEST_CASE("G-code row clicks emit one source request without moving the viewport", "[GCodeTextView][GCodeTextWidget]")
{
    TextFrames f;
    const auto before = f.window.visible_lines();
    ImGui::GetIO().MousePos = f.row_point(2);
    ImGui::GetIO().MouseDown[0] = true;
    f.frame();
    CHECK_FALSE(f.window.take_requested_line());
    ImGui::GetIO().MouseDown[0] = false;
    f.frame();
    const auto request = f.window.take_requested_line();
    REQUIRE(request.has_value());
    CHECK(*request == before.first + 2);
    CHECK(f.window.selected_line_id() == *request);
    CHECK_FALSE(f.window.take_requested_line());
    f.frame(80);
    CHECK(f.window.visible_lines() == before);
    f.window.resume_following();
    f.frame(80);
    CHECK(f.window.visible_lines().first > before.first);
}

TEST_CASE("G-code wheel browsing captures only input over the text window", "[GCodeTextView][GCodeTextWidget]")
{
    TextFrames f;
    const auto before = f.window.visible_lines();
    ImGui::GetIO().MousePos = f.row_point(2);
    ImGui::GetIO().MouseWheel = -1.f;
    f.frame();
    CHECK(ImGui::GetIO().WantCaptureMouse);
    f.frame(80);
    CHECK(f.window.visible_lines().first == before.first + 3);
    const auto scrolled = f.window.visible_lines();
    ImGui::GetIO().MousePos = ImVec2(0, 0);
    ImGui::GetIO().MouseWheel = -1.f;
    f.frame();
    CHECK_FALSE(ImGui::GetIO().WantCaptureMouse);
    CHECK(f.window.visible_lines() == scrolled);
    CHECK_FALSE(f.window.take_requested_line());
}

TEST_CASE("The G-code scrollbar reaches the last mapped source row", "[GCodeTextView][GCodeTextWidget]")
{
    TextFrames f;
    const ImGuiWindow* w = ImGui::FindWindowByName("G-code");
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::GetIO().MousePos = ImVec2(750.f - style.WindowPadding.x - 0.5f * style.ScrollbarSize,
        w->Pos.y + 560.f - style.WindowPadding.y - 4.f);
    ImGui::GetIO().MouseDown[0] = true;
    f.frame();
    ImGui::GetIO().MouseDown[0] = false;
    f.frame();
    CHECK(f.window.visible_lines().second == 100);
    CHECK_FALSE(f.window.take_requested_line());
}

TEST_CASE("Loading another G-code result cancels a row click that began on the old result", "[GCodeTextView][GCodeTextWidget]")
{
    TextFrames f;
    ImGui::GetIO().MousePos = f.row_point(2);
    ImGui::GetIO().MouseDown[0] = true;
    f.frame();
    f.window.reset();
    f.window.load_gcode(f.file.string(), f.line_ends);
    ImGui::GetIO().MouseDown[0] = false;
    f.frame();
    CHECK_FALSE(f.window.take_requested_line());
    CHECK(f.window.selected_line_id() == 0);
}

TEST_CASE("Short mapped G-code files render their first and last rows safely", "[GCodeTextView][GCodeTextWidget]")
{
    TextFrames f(3);
    CHECK(f.window.visible_lines().first == 1);
    CHECK(f.window.visible_lines().second == 3);
}

TEST_CASE("Changing plates resets G-code browsing even when the file mapping is reused", "[GCodeTextView][GCodeTextWidget]")
{
    TextFrames f;
    const auto before = f.window.visible_lines();
    ImGui::GetIO().MousePos = f.row_point(2);
    ImGui::GetIO().MouseDown[0] = true;
    f.frame();
    ImGui::GetIO().MouseDown[0] = false;
    f.frame();
    REQUIRE(f.window.selected_line_id() != 0);
    f.window.reset_view();
    CHECK(f.window.selected_line_id() == 0);
    CHECK_FALSE(f.window.take_requested_line());
    f.frame(80);
    CHECK(f.window.visible_lines().first > before.first);
}
