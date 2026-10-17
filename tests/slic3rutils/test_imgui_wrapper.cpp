// How ImGuiWrapper turns wx mouse events into ImGui input, and when it reports that ImGui owns a key.
// ImGuiWrapper lives in libslic3r_gui; this is the suite that links it. Same Windows include prologue
// as test_texture_color_palette.cpp (wx pulls in <windows.h>; keep WIN32_LEAN_AND_MEAN / NOMINMAX
// ahead of it).
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
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <wx/event.h>

#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/KeyChord.hpp"

using Catch::Matchers::WithinAbs;
using Slic3r::GUI::ImGuiWrapper;
using Slic3r::GUI::KeyChord;

namespace {

// ImGui's wheel after one wheel event, as wx on macOS builds it: a delta of 10 and the rotation in whole
// points of trackpad travel.
float wheel_after(int rotation, wxMouseWheelAxis axis)
{
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f); // update_mouse_data() ignores input until the display has a size

    wxMouseEvent evt(wxEVT_MOUSEWHEEL);
    evt.m_wheelRotation = rotation;
    evt.m_wheelDelta    = 10;
    evt.m_wheelAxis     = axis;
    imgui.update_mouse_data(evt);
    return ImGui::GetIO().MouseWheel;
}

// Runs ImGui frames without OpenGL by building the font atlas on the CPU.
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

    // SetKeyboardFocusHere() activates the field in the next frame that draws it.
    void draw_input(bool focus)
    {
        ImGui::Begin("window");
        if (focus)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##text", text, sizeof(text));
        ImGui::End();
    }

    char text[16] = "";
};

void draw_modal()
{
    if (ImGui::BeginPopupModal("modal"))
        ImGui::EndPopup();
}

} // namespace

TEST_CASE("a vertical wheel event steps ImGui's wheel in its own direction", "[ImGuiWrapper]")
{
    const int rotation = GENERATE(1, 7, 120, -1, -7, -120);
    CHECK_THAT(wheel_after(rotation, wxMOUSE_WHEEL_VERTICAL), WithinAbs(rotation > 0 ? 1.0 : -1.0, 0.0));
}

// Both kinds arrive from a macOS trackpad while scrolling; see ImGuiWrapper::update_mouse_data().
TEST_CASE("a wheel event with no rotation or on the horizontal axis does not step ImGui's wheel", "[ImGuiWrapper][Regression]")
{
    const auto [rotation, axis] = GENERATE(table<int, wxMouseWheelAxis>({
        { 0, wxMOUSE_WHEEL_VERTICAL },
        { 5, wxMOUSE_WHEEL_HORIZONTAL },
        { -5, wxMOUSE_WHEEL_HORIZONTAL },
    }));
    CHECK_THAT(wheel_after(rotation, axis), WithinAbs(0.0, 0.0));
}

TEST_CASE("an InputText counts as active only while it holds the keyboard", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    f.frame([&] { f.draw_input(false); });
    CHECK_FALSE(f.imgui.text_input_active());

    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK(f.imgui.text_input_active());

    f.frame([] {});
    f.frame([] {});
    CHECK_FALSE(f.imgui.text_input_active());
}

TEST_CASE("a modal counts as open from its first frame until a frame no longer draws it", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    CHECK_FALSE(f.imgui.modal_popup_open());

    f.frame([] {
        ImGui::OpenPopup("modal");
        draw_modal();
    });
    CHECK(f.imgui.modal_popup_open());

    // A frame opened by input, before anything is drawn in it.
    ImGui::NewFrame();
    CHECK(f.imgui.modal_popup_open());
    draw_modal();
    ImGui::EndFrame();
    CHECK(f.imgui.modal_popup_open());

    // A frame that no longer draws it, as when its canvas is hidden.
    f.frame([] {});
    REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
    CHECK_FALSE(f.imgui.modal_popup_open());
}

TEST_CASE("ImGui owns every key under a modal and text-editing keys in an active InputText", "[ImGuiWrapper]")
{
    ImGuiFrames    f;
    const KeyChord letter{ 'A' };
    const KeyChord save{ 'S', wxMOD_CONTROL };
    CHECK_FALSE(f.imgui.owns_key(letter));
    CHECK_FALSE(f.imgui.owns_key(save));

    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK(f.imgui.owns_key(letter));
    CHECK_FALSE(f.imgui.owns_key(save));

    f.frame([] {
        ImGui::OpenPopup("modal");
        draw_modal();
    });
    CHECK(f.imgui.owns_key(save));
}
