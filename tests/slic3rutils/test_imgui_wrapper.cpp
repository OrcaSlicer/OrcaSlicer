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

    void draw_slider()
    {
        ImGui::Begin("window");
        ImGui::SliderFloat("##slider", &value, 0.f, 1.f);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        slider_center    = ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2);
        ImGui::End();
    }

    // Presses the left button at pos, or releases it, and draws the slider in two frames.
    void mouse(const ImVec2& pos, bool down)
    {
        ImGui::GetIO().MousePos     = pos;
        ImGui::GetIO().MouseDown[0] = down;
        frame([this] { draw_slider(); });
        frame([this] { draw_slider(); });
    }

    char   text[16] = "";
    float  value    = 0.f;
    ImVec2 slider_center;
};

void draw_modal()
{
    if (ImGui::BeginPopupModal("modal"))
        ImGui::EndPopup();
}

void draw_dropdown(bool open)
{
    ImGui::Begin("window");
    if (open)
        ImGui::OpenPopup("dropdown");
    if (ImGui::BeginPopup("dropdown")) {
        ImGui::Text("item");
        ImGui::EndPopup();
    }
    ImGui::End();
}

wxKeyEvent key_event(wxEventType type, int key_code, int modifiers = wxMOD_NONE)
{
    wxKeyEvent evt(type);
    evt.m_keyCode = key_code;
    evt.SetControlDown(modifiers & wxMOD_CONTROL);
    evt.SetShiftDown(modifiers & wxMOD_SHIFT);
    evt.SetAltDown(modifiers & wxMOD_ALT);
    return evt;
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

TEST_CASE("a mouse event replaces a modifier whose key-up went to another window", "[ImGuiWrapper][Regression]")
{
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f);
    ImGui::GetIO().KeyCtrl = true;

    wxMouseEvent move(wxEVT_MOTION);
    imgui.update_mouse_data(move);
    CHECK_FALSE(ImGui::GetIO().KeyCtrl);

    move.SetControlDown(true);
    imgui.update_mouse_data(move);
    CHECK(ImGui::GetIO().KeyCtrl);
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

TEST_CASE("ImGui owns the whole keyboard under a modal or in an active InputText", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    CHECK_FALSE(f.imgui.owns_keyboard());

    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK(f.imgui.owns_keyboard());

    f.frame([] {});
    CHECK_FALSE(f.imgui.owns_keyboard());
    f.frame([] {
        ImGui::OpenPopup("modal");
        draw_modal();
    });
    CHECK(f.imgui.owns_keyboard());
}

TEST_CASE("an open dropdown owns the whole keyboard until it is closed", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    f.frame([] { draw_dropdown(true); });
    CHECK(f.imgui.popup_open());
    CHECK(f.imgui.owns_keyboard());

    f.imgui.close_top_popup();
    f.frame([] { draw_dropdown(false); });
    CHECK_FALSE(f.imgui.popup_open());
    CHECK_FALSE(f.imgui.owns_keyboard());
}

TEST_CASE("closing the top popup leaves a modal open", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    f.frame([] {
        ImGui::OpenPopup("modal");
        draw_modal();
    });
    f.imgui.close_top_popup();
    f.frame([] { draw_modal(); });
    CHECK(f.imgui.modal_popup_open());
}

TEST_CASE("a held slider owns the keyboard but not the keys Global shortcuts use", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    f.mouse(ImVec2(0, 0), false);
    CHECK_FALSE(f.imgui.item_held());
    f.mouse(f.slider_center, true);
    CHECK(f.imgui.item_held());
    CHECK(f.imgui.owns_keyboard());
    CHECK_FALSE(f.imgui.owns_key(KeyChord{ 'M' }));
    CHECK_FALSE(f.imgui.owns_key(KeyChord{ WXK_LEFT }));

    f.mouse(f.slider_center, false);
    CHECK_FALSE(f.imgui.owns_keyboard());
}

TEST_CASE("a slider typed into once is not a text field when dragged", "[ImGuiWrapper][Regression]")
{
    ImGuiFrames f;
    f.mouse(ImVec2(0, 0), false);
    ImGui::GetIO().KeyCtrl = true; // Ctrl+click types a value
    f.mouse(f.slider_center, true);
    ImGui::GetIO().KeyCtrl = false;
    f.mouse(f.slider_center, false);
    REQUIRE(f.imgui.text_input_active());

    f.mouse(ImVec2(0, 0), true); // a click elsewhere closes the field
    f.mouse(ImVec2(0, 0), false);
    f.mouse(f.slider_center, true);
    REQUIRE(ImGui::GetInputTextState(ImGui::GetActiveID()) != nullptr);
    CHECK_FALSE(f.imgui.text_input_active());
    CHECK_FALSE(f.imgui.owns_key(KeyChord{ 'M' }));
}

TEST_CASE("a key's char event gets its key-down's answer", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, 'A')));
    f.frame([] {}); // the field closes before the char arrives
    REQUIRE_FALSE(f.imgui.text_input_active());
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_CHAR, 'a')));

    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, 'B')));
    f.frame([&] { f.draw_input(true); }); // the field opens before the char arrives
    f.frame([&] { f.draw_input(false); });
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_CHAR, 'b')));
    // A char with no key-down before it is decided on its own.
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_CHAR, 'c')));
}

TEST_CASE("a held key's repeats and release get its press's answer", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, WXK_RIGHT)));
    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, WXK_RIGHT)));
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_UP, WXK_RIGHT)));

    CHECK(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, WXK_RIGHT)));
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_KEY_UP, WXK_RIGHT)));
    // The release also clears the press's answer for a later char with no key-down.
    f.frame([] {});
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_CHAR, 'a')));
}

TEST_CASE("a key pressed again after the canvas lost the focus is decided afresh", "[ImGuiWrapper]")
{
    ImGuiFrames f;
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, 'M')));
    f.imgui.forget_key_claims(); // the key-up went to the window that took the focus

    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, 'M')));
}

TEST_CASE("an active ImGui field claims every key, whether it edits text or not", "[ImGuiWrapper]")
{
    const int key = GENERATE(int('A'), int('D'), int(WXK_SHIFT));
    ImGuiFrames f;
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, key, wxMOD_CONTROL)));
    CHECK_FALSE(f.imgui.claim_key_event(key_event(wxEVT_KEY_UP, key, wxMOD_CONTROL)));

    f.frame([&] { f.draw_input(true); });
    f.frame([&] { f.draw_input(false); });
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_KEY_DOWN, key, wxMOD_CONTROL)));
    CHECK(f.imgui.claim_key_event(key_event(wxEVT_KEY_UP, key, wxMOD_CONTROL)));
}
