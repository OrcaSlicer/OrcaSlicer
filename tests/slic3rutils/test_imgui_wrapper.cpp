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

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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

namespace {

// ImGui frames whose items ImGuiWrapper records, as it does for the MCP server.
struct RecordedFrames
{
    ImGuiWrapper imgui;
    int          owner = 0;

    RecordedFrames()
    {
        imgui.set_display_size(800.f, 600.f);
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        imgui.set_item_recording(true);
        imgui.set_frame_owner(&owner);
    }

    // One frame drawing `draw` inside a window; end_frame() publishes what it recorded.
    template<class Draw> void frame(Draw draw)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
        ImGui::SetNextWindowSize(ImVec2(400.f, 300.f));
        ImGui::Begin("panel");
        draw();
        ImGui::End();
        imgui.end_frame();
    }

    std::vector<ImGuiWrapper::RecordedItem> items() const { return imgui.recorded_items(&owner); }
    // The items automation could list: those with a label or a name (the window's own title bar and
    // resize grip are items too).
    std::size_t listable() const
    {
        std::size_t n = 0;
        for (const auto& item : items())
            n += item.label.empty() && item.name.empty() ? 0 : 1;
        return n;
    }
    std::vector<ImGuiWrapper::RecordedText> texts() const { return imgui.recorded_texts(&owner); }

    std::vector<ImGuiWrapper::RecordedItem> named(const std::string& name) const
    {
        std::vector<ImGuiWrapper::RecordedItem> out;
        for (const auto& item : items())
            if (item.name == name)
                out.push_back(item);
        return out;
    }

    std::optional<ImGuiWrapper::RecordedItem> labelled(const std::string& label) const
    {
        for (const auto& item : items())
            if (item.label == label)
                return item;
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("a control drawn without an ImGui item is recorded under its name in each ID scope", "[ImGuiWrapper]")
{
    RecordedFrames f;
    f.frame([&] {
        for (int row = 0; row < 2; ++row) {
            ImGui::PushID(row);
            ImGui::Dummy(ImVec2(20.f, 20.f));
            f.imgui.record_named_item("Remove");
            ImGui::PopID();
        }
    });
    const auto removes = f.named("Remove");
    REQUIRE(removes.size() == 2);
    CHECK(removes[0].id != removes[1].id);
    CHECK(removes[0].y < removes[1].y);
    CHECK(removes[0].kind == "button");
}

TEST_CASE("naming a real ImGui item names its own record", "[ImGuiWrapper]")
{
    RecordedFrames f;
    f.frame([&] {
        ImGui::Button("##reset");
        f.imgui.record_named_item("Reset");
    });
    const auto resets = f.named("Reset");
    REQUIRE(resets.size() == 1);
    CHECK(resets[0].label == "##reset");
    CHECK(f.listable() == 1);
}

TEST_CASE("a named toggle reports whether it is on", "[ImGuiWrapper]")
{
    const bool on = GENERATE(false, true);
    RecordedFrames f;
    f.frame([&] {
        ImGui::Button("##brush");
        f.imgui.record_named_item("Brush", "toggle", on ? 1 : 0);
    });
    const auto brush = f.named("Brush");
    REQUIRE(brush.size() == 1);
    CHECK((brush[0].flags & ImGuiItemStatusFlags_Checkable) != 0);
    CHECK(((brush[0].flags & ImGuiItemStatusFlags_Checked) != 0) == on);
}

TEST_CASE("naming records nothing while recording is off", "[ImGuiWrapper]")
{
    RecordedFrames f;
    f.imgui.set_item_recording(false);
    f.frame([&] {
        ImGui::Dummy(ImVec2(20.f, 20.f));
        f.imgui.record_named_item("Circle");
    });
    f.imgui.set_item_recording(true);
    CHECK(f.listable() == 0);
}

TEST_CASE("fields report their kind, shown value and whether they take typing", "[ImGuiWrapper]")
{
    RecordedFrames f;
    float slider = 2.5f, locked = 1.f, drag = 3.f;
    char  single[16] = "abc", multi[32] = "one\ntwo";
    f.frame([&] {
        ImGui::SliderFloat("##slider", &slider, 0.f, 10.f, "%.2f");
        ImGui::SliderFloat("##locked", &locked, 0.f, 10.f, "%.1f", ImGuiSliderFlags_NoInput);
        ImGui::DragFloat("##drag", &drag, 1.f, 0.f, 10.f, "%.3f");
        ImGui::InputText("##single", single, sizeof(single));
        ImGui::InputTextMultiline("##multi", multi, sizeof(multi));
        if (ImGui::BeginCombo("##combo", "Local"))
            ImGui::EndCombo();
    });
    const auto check = [&](const char* label, const char* kind, const char* value, bool inputable) {
        const auto item = f.labelled(label);
        REQUIRE(item.has_value());
        CHECK(item->kind == kind);
        CHECK(item->value == value);
        CHECK(((item->flags & ImGuiItemStatusFlags_Inputable) != 0) == inputable);
    };
    check("##slider", "slider", "2.50", true);
    check("##locked", "slider", "1.0", false);
    check("##drag", "drag", "3.000", true);
    check("##single", "input", "abc", true);
    check("##multi", "multiline", "one\ntwo", true);
    check("##combo", "combo", "Local", false);
}

TEST_CASE("text is recorded apart from the items", "[ImGuiWrapper]")
{
    RecordedFrames f;
    f.frame([&] {
        ImGui::Text("Brush size");
        ImGui::SameLine();
        ImGui::Button("Apply");
    });
    const auto texts = f.texts();
    REQUIRE(texts.size() == 1);
    CHECK(texts[0].text == "Brush size");
    CHECK(texts[0].window == "panel");
    CHECK(f.listable() == 1);
    CHECK(f.labelled("Apply").has_value());
}

TEST_CASE("an item outside its window's visible part keeps its label and says it is clipped", "[ImGuiWrapper]")
{
    RecordedFrames f;
    f.frame([&] {
        ImGui::BeginChild("##list", ImVec2(200.f, 60.f));
        ImGui::Button("Top");
        ImGui::Dummy(ImVec2(10.f, 400.f));
        ImGui::Button("Bottom");
        ImGui::EndChild();
    });
    const auto top    = f.labelled("Top");
    const auto bottom = f.labelled("Bottom");
    REQUIRE(top.has_value());
    CHECK_FALSE(top->clipped);
    REQUIRE(bottom.has_value());
    CHECK(bottom->clipped);
}

TEST_CASE("same-named controls drawn in one ID scope get records of their own", "[ImGuiWrapper]")
{
    RecordedFrames f;
    const auto     draw = [&] {
        for (int row = 0; row < 2; ++row) {
            ImGui::Dummy(ImVec2(20.f, 20.f));
            f.imgui.record_named_item("Revert");
        }
    };
    f.frame(draw);
    const auto first = f.named("Revert");
    REQUIRE(first.size() == 2);
    CHECK(first[0].id != first[1].id);
    // The same ids frame after frame, so a ref to the second row keeps finding it.
    f.frame(draw);
    const auto again = f.named("Revert");
    REQUIRE(again.size() == 2);
    CHECK(again[1].id == first[1].id);
}

TEST_CASE("a combo with an empty label and its unlabelled options are named on their own records", "[ImGuiWrapper]")
{
    RecordedFrames f;
    // The pattern of GizmoObjectManipulation::render_combo(): "##" as the label, each option an
    // empty Selectable followed by its text. The first frame opens the popup, the second draws it.
    const auto draw = [&](bool open) {
        const ImGuiID combo = ImGui::GetCurrentWindow()->GetIDNoKeepAlive("##");
        if (open)
            ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, combo));
        if (ImGui::BeginCombo("##", "World")) {
            for (int i = 0; i < 2; ++i) {
                ImGui::PushID(i);
                ImGui::Selectable("", i == 0);
                f.imgui.record_named_item(i == 0 ? "World" : "Object", "option");
                ImGui::SameLine();
                ImGui::Text("%s", i == 0 ? "World" : "Object");
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        f.imgui.record_item_name(combo, "coordinates", "combo");
    };
    f.frame([&] { draw(true); });
    f.frame([&] { draw(false); });
    const auto combo = f.named("coordinates");
    REQUIRE(combo.size() == 1);
    CHECK(combo[0].kind == "combo");
    CHECK(combo[0].value == "World"); // the value stays on the named record
    const auto object = f.named("Object");
    REQUIRE(object.size() == 1);
    CHECK(object[0].kind == "option");
    CHECK(object[0].label.empty()); // the Selectable's own record, so a click on it picks it
}
