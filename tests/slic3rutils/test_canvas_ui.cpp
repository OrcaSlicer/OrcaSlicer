#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "slic3r/GUI/CanvasUi/CanvasFrame.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Corners.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Icons.hpp"
#include "slic3r/GUI/CanvasUi/ImGuiBridge.hpp"
#include "slic3r/GUI/CanvasUi/Input.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/ScrollArea.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Contrast runs from 1 for equal colors to 21 for black on white", "[CanvasUi][Theme]")
{
    CHECK_THAT(CanvasUi::contrast_ratio(CanvasUi::Color(1, 1, 1), CanvasUi::Color(0, 0, 0)), WithinAbs(21.0, 0.01));
    CHECK_THAT(CanvasUi::contrast_ratio(CanvasUi::Color(.5f, .5f, .5f), CanvasUi::Color(.5f, .5f, .5f)), WithinAbs(1.0, 0.001));
}

namespace {

ImU32 to_u32(const CanvasUi::Color& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }

} // namespace

TEST_CASE("A colour made of bytes gives back the same bytes", "[CanvasUi][Theme]")
{
    for (int b = 0; b < 256; ++b) {
        const uint8_t byte = uint8_t(b);
        CHECK(to_u32(CanvasUi::Color::from_bytes(byte, byte, byte)) == IM_COL32(b, b, b, 255));
        CHECK(to_u32(CanvasUi::rgb((b << 16) | (b << 8) | b, float(b) / 255.f)) == IM_COL32(b, b, b, b));
    }
}

TEST_CASE("Mixing and fading land on whole colour bytes", "[CanvasUi][Theme]")
{
    CHECK(to_u32(CanvasUi::mix(CanvasUi::rgb(0x000000), CanvasUi::rgb(0xffffff), .5f)) == IM_COL32(128, 128, 128, 255));
    const CanvasUi::Color mixed = CanvasUi::mix(CanvasUi::rgb(0x3d3d43), CanvasUi::rgb(0xff7a8c), .3f);
    CHECK(to_u32(mixed) == IM_COL32(119, 79, 89, 255));
    CHECK_THAT(mixed.r, WithinAbs(119.f / 255.f, 1e-6));
    CHECK(to_u32(CanvasUi::faded(CanvasUi::rgb(0x009688, .45f), .5f)) == IM_COL32(0x00, 0x96, 0x88, 58));
    CHECK(to_u32(CanvasUi::faded(CanvasUi::rgb(0x009688, .45f), 1.5f)) == IM_COL32(0x00, 0x96, 0x88, 115));
    CHECK(to_u32(CanvasUi::faded(CanvasUi::rgb(0x009688), 0.f)) == IM_COL32(0x00, 0x96, 0x88, 0));
}

TEST_CASE("Text on a preview panel reads at 4.5:1 or more", "[CanvasUi][Theme]")
{
    const bool          dark  = GENERATE(false, true);
    const CanvasUi::Theme& theme = CanvasUi::theme(dark);

    CHECK(CanvasUi::contrast_ratio(theme.foreground, theme.card) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.muted_foreground, theme.card) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.inactive_foreground, CanvasUi::blend_over(theme.secondary, theme.card)) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.inactive_foreground, theme.tabs_list) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.primary_strong, theme.card) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.foreground, CanvasUi::blend_over(theme.selection, theme.card)) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.tooltip_foreground, theme.tooltip) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(CanvasUi::blend_over(theme.tooltip_muted_foreground, theme.tooltip), theme.tooltip) >= 4.5f);
    const CanvasUi::Color warning_surface = CanvasUi::blend_over(theme.warning_surface, theme.card);
    CHECK(CanvasUi::contrast_ratio(theme.foreground, warning_surface) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.warning, warning_surface) >= 3.f);
}

TEST_CASE("On a hovered surface text reads at 4.5:1 and marks at 3:1", "[CanvasUi][Theme]")
{
    const bool             dark  = GENERATE(false, true);
    const CanvasUi::Theme& theme = CanvasUi::theme(dark);
    const CanvasUi::Color  hover = CanvasUi::blend_over(theme.accent, theme.card);

    CHECK(CanvasUi::contrast_ratio(theme.foreground, hover) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.primary_strong, hover) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.muted_foreground, hover) >= 3.f);
}

TEST_CASE("What shows the state of a control stands out at 3:1 or more", "[CanvasUi][Theme]")
{
    const bool             dark  = GENERATE(false, true);
    const CanvasUi::Theme& theme = CanvasUi::theme(dark);
    const CanvasUi::Color  track = CanvasUi::blend_over(theme.secondary, theme.card);

    CHECK(CanvasUi::contrast_ratio(theme.input, theme.card) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.input, track) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.toggle_pressed_border, track) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.switch_thumb, theme.input) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.primary, theme.card) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.primary, track) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.card, theme.primary) >= 3.f);
}

namespace {

using Clock = CanvasUi::Context::Clock;

void frame_at(CanvasUi::Context& context, Clock::time_point t) { CanvasUi::CanvasFrame(context).begin(false, 1.f, t, 0, nullptr, {}); }

} // namespace

TEST_CASE("A tooltip is due the tooltip delay after its item is first hovered, and asks for that frame", "[CanvasUi]")
{
    CanvasUi::Context          context;
    const Clock::time_point start = Clock::now();

    frame_at(context, start);
    CHECK_FALSE(context.tooltip_due(1));
    REQUIRE(CanvasUi::CanvasFrame(context).wake_at().has_value());
    CHECK(*CanvasUi::CanvasFrame(context).wake_at() == start + CanvasUi::duration::tooltip);
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip / 2);
    CHECK_FALSE(context.tooltip_due(1));
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip);
    CHECK(context.tooltip_due(1));
    CHECK_FALSE(CanvasUi::CanvasFrame(context).wake_at().has_value());
}

TEST_CASE("Hovering another item or nothing starts the tooltip wait again", "[CanvasUi]")
{
    CanvasUi::Context          context;
    const Clock::time_point start = Clock::now();

    frame_at(context, start);
    context.tooltip_due(1);
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip * 4 / 5);
    CHECK_FALSE(context.tooltip_due(2));
    CanvasUi::CanvasFrame(context).end();
    frame_at(context, start + CanvasUi::duration::tooltip * 6 / 5);
    CHECK_FALSE(context.tooltip_due(2));
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip * 7 / 5);
    CanvasUi::CanvasFrame(context).end();
    frame_at(context, start + CanvasUi::duration::tooltip * 2);
    CHECK_FALSE(context.tooltip_due(2));
}

TEST_CASE("A frame asks for nothing unless a component does, and keeps the earliest wake", "[CanvasUi]")
{
    CanvasUi::Context          context;
    const Clock::time_point start = Clock::now();

    frame_at(context, start);
    CHECK_FALSE(CanvasUi::CanvasFrame(context).frame_requested());
    CHECK_FALSE(CanvasUi::CanvasFrame(context).wake_at().has_value());

    context.request_frame_at(start + std::chrono::milliseconds(300));
    context.request_frame_at(start + std::chrono::milliseconds(100));
    context.request_frame();
    CHECK(CanvasUi::CanvasFrame(context).frame_requested());
    CHECK(*CanvasUi::CanvasFrame(context).wake_at() == start + std::chrono::milliseconds(100));

    frame_at(context, start + std::chrono::milliseconds(50));
    CHECK_FALSE(CanvasUi::CanvasFrame(context).frame_requested());
    CHECK_FALSE(CanvasUi::CanvasFrame(context).wake_at().has_value());
}

TEST_CASE("A context made current inside another gives the outer one back", "[CanvasUi]")
{
    CanvasUi::Context outer;
    CanvasUi::Context inner;
    {
        const CanvasUi::ScopedContext outer_scope(outer);
        CHECK(CanvasUi::Context::current() == &outer);
        {
            const CanvasUi::ScopedContext inner_scope(inner);
            CHECK(CanvasUi::Context::current() == &inner);
        }
        CHECK(CanvasUi::Context::current() == &outer);
    }
}

namespace {

// ImGui frames without OpenGL, the font atlas built on the CPU, with the components drawn in a card at the
// top left corner of an 800 × 600 display.
class Frames
{
public:
    explicit Frames(float scale = 1.f, float bold_size = 0.f)
        : m_scale(scale), m_previous(ImGui::GetCurrentContext()), m_imgui(ImGui::CreateContext())
    {
        ImGui::SetCurrentContext(m_imgui);
        ImGuiIO& io    = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.f, 600.f);
        io.KeyMap[ImGuiKey_Enter]  = 13;
        io.KeyMap[ImGuiKey_Escape] = 27;
        io.DeltaTime   = 1.f / 60.f;
        io.IniFilename = nullptr;
        if (bold_size > 0.f) {
            io.Fonts->AddFontDefault();
            ImFontConfig bold;
            bold.SizePixels = bold_size;
            m_fonts.bold    = io.Fonts->AddFontDefault(&bold);
        }
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Frames()
    {
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
    }
    Frames(const Frames&)            = delete;
    Frames& operator=(const Frames&) = delete;

    template<class Draw> void frame(const ImVec2& mouse, bool down, Draw draw)
    {
        ImGuiIO& io     = ImGui::GetIO();
        io.MousePos     = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        CanvasUi::CanvasFrame(m_ui).begin(false, m_scale, m_now, ImGui::GetFrameCount(), nullptr, m_fonts);
        {
            const CanvasUi::ScopedContext scope(m_ui);
            CanvasUi::CardStyle           style;
            style.padding = 0.f;
            style.shadow  = false;
            const CanvasUi::Card card("##test", CanvasUi::Point(0.f, 0.f), CanvasUi::Size(400.f, 300.f), style);
            draw();
        }
        CanvasUi::CanvasFrame(m_ui).end();
        ImGui::EndFrame();
        m_now += std::chrono::milliseconds(16);
    }

    // Moves to at, presses and lets go; whether draw reported a change or a press in any of those frames.
    template<class Draw> bool click(const ImVec2& at, Draw draw)
    {
        bool reported = false;
        const auto record = [&]() { reported |= draw(); };
        frame(ImVec2(-1.f, -1.f), false, record);
        frame(at, false, record);
        frame(at, true, record);
        frame(at, false, record);
        return reported;
    }

    bool frame_requested() { return CanvasUi::CanvasFrame(m_ui).frame_requested(); }

    template<class Draw> void hover(const ImVec2& at, std::chrono::milliseconds time, Draw draw)
    {
        for (const auto end = m_now + time; m_now <= end;)
            frame(at, false, draw);
    }

private:
    float                             m_scale;
    CanvasUi::Fonts                   m_fonts;
    ImGuiContext*                     m_previous;
    ImGuiContext*                     m_imgui;
    CanvasUi::Context                    m_ui;
    CanvasUi::Context::Clock::time_point m_now{};
};

const ImGuiWindow* active_tooltip()
{
    for (const ImGuiWindow* window : GImGui->Windows)
        if (window->Active && std::strncmp(window->Name, "##Tooltip_", 10) == 0)
            return window;
    return nullptr;
}

bool tooltip_shown()
{
    const ImGuiWindow* tooltip = active_tooltip();
    return tooltip != nullptr && !tooltip->Hidden;
}

} // namespace

TEST_CASE("A scroll area scrolls taller content up and down, never sideways", "[CanvasUi]")
{
    Frames frames;
    float  max_x = -1.f, max_y = -1.f;
    const auto draw = [&]() {
        const CanvasUi::ScrollArea scroll("##scroll", CanvasUi::Size(200.f, 80.f), 300.f);
        ImGui::Dummy(ImVec2(180.f, 20.f));
        max_x = ImGui::GetScrollMaxX();
        max_y = ImGui::GetScrollMaxY();
    };
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK(max_y > 0.f);
    CHECK_THAT(max_x, WithinAbs(0.f, 1e-6));
}

TEST_CASE("A painter paints in the colour given, and its scopes give the clip and the fringe back", "[CanvasUi]")
{
    Frames frames;
    frames.frame(ImVec2(-1.f, -1.f), false, []() {
        const CanvasUi::Painter paint  = CanvasUi::Painter::background();
        ImDrawList&          list   = *ImGui::GetBackgroundDrawList();
        const CanvasUi::Color   accent = CanvasUi::Context::current()->theme().primary;
        paint.fill_rect(CanvasUi::Rect(10.f, 10.f, 20.f, 20.f), accent);
        CHECK(list.VtxBuffer.back().col == ImGui::GetColorU32(ImVec4(accent.r, accent.g, accent.b, accent.a)));

        const int clips = list._ClipRectStack.Size;
        {
            const CanvasUi::Painter::Clip outer(paint, CanvasUi::Rect(0.f, 0.f, 100.f, 100.f));
            const CanvasUi::Painter::Clip inner(paint, CanvasUi::Rect(50.f, 50.f, 200.f, 200.f));
            const ImVec4               clip = list._ClipRectStack.back();
            CHECK_THAT(clip.x, WithinAbs(50., 1e-6));
            CHECK_THAT(clip.y, WithinAbs(50., 1e-6));
            CHECK_THAT(clip.z, WithinAbs(100., 1e-6));
            CHECK_THAT(clip.w, WithinAbs(100., 1e-6));
        }
        CHECK(list._ClipRectStack.Size == clips);

        REQUIRE((list.Flags & ImDrawListFlags_AntiAliasedFill) != 0);
        {
            const CanvasUi::Painter::SharpFills sharp(paint);
            CHECK((list.Flags & ImDrawListFlags_AntiAliasedFill) == 0);
        }
        CHECK((list.Flags & ImDrawListFlags_AntiAliasedFill) != 0);

        const float fringe = list._FringeScale;
        paint.fill_convex({CanvasUi::Point(0.f, 0.f), CanvasUi::Point(10.f, 0.f), CanvasUi::Point(10.f, 10.f)}, accent, 5.f);
        CHECK_THAT(list._FringeScale, WithinAbs(fringe, 1e-6));
    });
}

TEST_CASE("The context gives the faces the canvas loaded, and the current font for a missing one", "[CanvasUi]")
{
    Frames frames;
    frames.frame(ImVec2(-1.f, -1.f), false, []() {
        CanvasUi::Context        context;
        ImFont* const         face = ImGui::GetIO().Fonts->Fonts[0];
        CanvasUi::Fonts          fonts;
        fonts.mono[size_t(CanvasUi::MonoStyle::Bold) * CanvasUi::MONO_FONT_SIZES.size() + size_t(CanvasUi::MonoSize::Code)] = face;
        CanvasUi::CanvasFrame(context).begin(false, 1.f, Clock::now(), 0, nullptr, fonts);
        CHECK(CanvasUi::face_of(context.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Code)) == face);
        CHECK(context.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Code).size() == face->FontSize * face->Scale);
        CHECK(CanvasUi::face_of(context.mono_font(CanvasUi::MonoStyle::Italic, CanvasUi::MonoSize::Caption)) == ImGui::GetFont());
        CHECK(CanvasUi::face_of(context.bold_font()) == ImGui::GetFont());
    });
}

TEST_CASE("A font knows the characters it lacks", "[CanvasUi]")
{
    Frames frames;
    frames.frame(ImVec2(-1.f, -1.f), false, []() {
        const CanvasUi::Font font = CanvasUi::Context::current()->regular_font();
        CHECK(font.has_glyphs("G1 X120.815"));
        CHECK_FALSE(font.has_glyphs("G1 \xD0\x96"));
        CHECK_THAT(font.width("G1"), WithinAbs(ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.f, "G1").x, 1e-6));
    });
}

TEST_CASE("The pointer reads the mouse of the frame, and a panel's place is where its next component starts", "[CanvasUi]")
{
    Frames             frames;
    CanvasUi::Pointer     seen;
    ImVec2             cursor;
    const auto draw = [&]() {
        seen = CanvasUi::pointer();
        CanvasUi::place(CanvasUi::Point(30.f, 40.f));
        cursor = ImGui::GetCursorScreenPos();
    };
    frames.frame(ImVec2(30.f, 40.f), false, draw);
    frames.frame(ImVec2(30.f, 40.f), true, draw);
    CHECK_THAT(seen.pos.x, WithinAbs(30., 1e-6));
    CHECK_THAT(seen.pos.y, WithinAbs(40., 1e-6));
    CHECK(seen.pressed);
    CHECK(seen.held);
    CHECK_FALSE(seen.context_pressed);
    CHECK(seen.over_panel);
    CHECK_THAT(cursor.x, WithinAbs(30., 1e-6));
    CHECK_THAT(cursor.y, WithinAbs(40., 1e-6));
    frames.frame(ImVec2(35.f, 40.f), true, draw);
    CHECK_FALSE(seen.pressed);
    CHECK(seen.held);
    CHECK_THAT(seen.delta.x, WithinAbs(5., 1e-6));
    frames.frame(ImVec2(600.f, 500.f), false, draw);
    frames.frame(ImVec2(600.f, 500.f), false, draw);
    CHECK_FALSE(seen.over_panel);
}

TEST_CASE("Esc is pressed in the frame its key goes down", "[CanvasUi]")
{
    Frames frames;
    bool   pressed = false;
    ImGui::GetIO().KeysDown[27] = true;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() { pressed = CanvasUi::escape_pressed(); });
    CHECK(pressed);
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() { pressed = CanvasUi::escape_pressed(); });
    CHECK_FALSE(pressed);
    ImGui::GetIO().KeysDown[27] = false;
}

TEST_CASE("A card paints into itself even from inside a scroll area of it", "[CanvasUi]")
{
    Frames frames;
    int    card_before = 0, card_after = 0;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        const CanvasUi::Card card("##outer", CanvasUi::Point(420.f, 10.f), CanvasUi::Size(200.f, 200.f));
        ImDrawList&       list = *ImGui::GetWindowDrawList();
        {
            const CanvasUi::ScrollArea scroll("##scroll", CanvasUi::Size(180.f, 100.f), 300.f);
            card_before = list.VtxBuffer.Size;
            card.painter().fill_rect(CanvasUi::Rect(430.f, 20.f, 440.f, 30.f), CanvasUi::Color(1.f, 0.f, 0.f));
            card_after = list.VtxBuffer.Size;
        }
    });
    CHECK(card_after > card_before);
}

TEST_CASE("A row's background is the card's own shape cut to the row, so rows near a corner meet without notches", "[CanvasUi]")
{
    Frames frames;
    std::vector<std::pair<float, float>> rows;
    std::vector<float>                   shape_heights;
    std::vector<ImVec4>                  clips;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        CanvasUi::CardStyle style;
        style.padding = 0.f;
        style.shadow  = false;
        const CanvasUi::Card card("##rows", CanvasUi::Point(420.f, 10.f), CanvasUi::Size(200.f, 200.f), style);
        ImDrawList&          list = *ImGui::GetWindowDrawList();
        for (const float top : {100.f, 196.f, 205.f}) {
            const int vertices = list.VtxBuffer.Size;
            card.row_background(top, top + 9.f, CanvasUi::Color(1.f, 0.f, 0.f));
            float low = FLT_MAX, high = -FLT_MAX;
            for (int i = vertices; i < list.VtxBuffer.Size; ++i) {
                low  = std::min(low, list.VtxBuffer[i].pos.y);
                high = std::max(high, list.VtxBuffer[i].pos.y);
            }
            shape_heights.push_back(high - low);
            rows.emplace_back(top, std::min(top + 9.f, 209.f));
        }
        for (const ImDrawCmd& cmd : list.CmdBuffer)
            if (cmd.ElemCount > 0)
                clips.push_back(cmd.ClipRect);
    });
    REQUIRE(shape_heights.size() == 3);
    for (const float height : shape_heights)
        CHECK(height > 150.f);
    for (const auto& [top, bottom] : rows)
        CHECK(std::any_of(clips.begin(), clips.end(), [top = top, bottom = bottom](const ImVec4& clip) {
            return std::abs(clip.y - top) < 1e-3f && std::abs(clip.w - bottom) < 1e-3f;
        }));
}

namespace {

// Clockwise on the screen and convex, once repeated points of square corners are dropped.
bool convex_clockwise(const std::vector<CanvasUi::Point>& outline)
{
    std::vector<CanvasUi::Point> points;
    for (const CanvasUi::Point& p : outline)
        if (points.empty() || std::hypot(p.x - points.back().x, p.y - points.back().y) > 1e-4f)
            points.push_back(p);
    for (size_t i = 0; i < points.size(); ++i) {
        const CanvasUi::Point& a = points[i];
        const CanvasUi::Point& b = points[(i + 1) % points.size()];
        const CanvasUi::Point& c = points[(i + 2) % points.size()];
        if ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) < -1e-3f)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("Two presses make a double click by the frame's clock, not ImGui's", "[CanvasUi]")
{
    Frames                    frames;
    std::vector<bool>         double_clicks;
    const auto                draw = [&]() {
        const CanvasUi::Pointer p = CanvasUi::pointer();
        if (p.pressed)
            double_clicks.push_back(p.double_clicked);
    };
    const ImVec2 at(100.f, 50.f);
    frames.hover(at, std::chrono::milliseconds(400), draw);
    frames.frame(at, true, draw);
    frames.frame(at, false, draw);
    frames.frame(ImVec2(102.f, 51.f), true, draw);
    frames.frame(at, false, draw);
    frames.hover(at, std::chrono::milliseconds(400), draw);
    frames.frame(at, true, draw);
    frames.frame(at, false, draw);
    frames.frame(ImVec2(160.f, 50.f), true, draw);
    frames.frame(at, false, draw);
    CHECK(double_clicks == std::vector<bool>{false, true, false, false});
}

TEST_CASE("A rounded outline is convex, inside its rectangle and as long whatever its corners", "[CanvasUi]")
{
    const float smoothing = GENERATE(0.f, .6f, 1.f);
    const CanvasUi::Rect rect = GENERATE(CanvasUi::Rect(0.f, 0.f, 200.f, 100.f), CanvasUi::Rect(0.f, 0.f, 40.f, 20.f));
    for (const CanvasUi::Corners corners : {CanvasUi::Corners::All, CanvasUi::Corners::Top, CanvasUi::Corners::None}) {
        const std::vector<CanvasUi::Point> outline = CanvasUi::rounded_rect_outline(rect, 12.f, smoothing, corners);
        CHECK(outline.size() == 4 * size_t(CanvasUi::CORNER_POINTS));
        CHECK(convex_clockwise(outline));
        for (const CanvasUi::Point& p : outline) {
            CHECK(p.x >= rect.left - 1e-3f);
            CHECK(p.x <= rect.right + 1e-3f);
            CHECK(p.y >= rect.top - 1e-3f);
            CHECK(p.y <= rect.bottom + 1e-3f);
        }
    }
}

TEST_CASE("Without smoothing a corner is a circular arc, and a short side takes the smoothing off before the radius", "[CanvasUi]")
{
    const CanvasUi::Rect               rect(0.f, 0.f, 200.f, 100.f);
    const std::vector<CanvasUi::Point> arc = CanvasUi::rounded_rect_outline(rect, 12.f, 0.f);
    for (int i = CanvasUi::CORNER_POINTS; i < 2 * CanvasUi::CORNER_POINTS; ++i)
        CHECK_THAT(std::hypot(arc[size_t(i)].x - 188.f, arc[size_t(i)].y - 12.f), WithinAbs(12., 1e-2));

    const std::vector<CanvasUi::Point> smooth = CanvasUi::rounded_rect_outline(rect, 12.f, 1.f);
    CHECK_THAT(smooth[size_t(CanvasUi::CORNER_POINTS)].x, WithinAbs(176., 1e-3));

    const std::vector<CanvasUi::Point> pill = CanvasUi::rounded_rect_outline(CanvasUi::Rect(0.f, 0.f, 40.f, 20.f), 12.f, 1.f);
    CHECK_THAT(pill[size_t(CanvasUi::CORNER_POINTS)].x, WithinAbs(30., 1e-3));
    CHECK_THAT(pill[size_t(2 * CanvasUi::CORNER_POINTS - 1)].y, WithinAbs(10., 1e-3));
}
