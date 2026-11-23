#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Context.hpp"
#include "Corners.hpp"
#include "ImGuiBridge.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

namespace {

// Fills the ring between two outlines of as many points, the inner one inside the outer one, its colour going from one to the other.
void fill_between(ImDrawList& list, const std::vector<Point>& inner, const std::vector<Point>& outer, ImU32 inner_color, ImU32 outer_color)
{
    const int    count = int(inner.size());
    const ImVec2 uv    = ImGui::GetFontTexUvWhitePixel();
    list.PrimReserve(6 * count, 2 * count);
    const auto base = list._VtxCurrentIdx;
    for (int i = 0; i < count; ++i) {
        list.PrimWriteVtx(im(inner[size_t(i)]), uv, inner_color);
        list.PrimWriteVtx(im(outer[size_t(i)]), uv, outer_color);
    }
    for (int i = 0; i < count; ++i) {
        const auto in = ImDrawIdx(base + 2 * i), out = ImDrawIdx(in + 1);
        const auto next_in = ImDrawIdx(base + 2 * ((i + 1) % count)), next_out = ImDrawIdx(next_in + 1);
        list.PrimWriteIdx(in), list.PrimWriteIdx(out), list.PrimWriteIdx(next_out);
        list.PrimWriteIdx(in), list.PrimWriteIdx(next_out), list.PrimWriteIdx(next_in);
    }
}

} // namespace

void draw_window_shadow(float rounding, const Elevation& level)
{
    const Context&            ui    = *Context::current();
    const Metrics&            m     = ui.metrics();
    const ImVec2              min   = ImGui::GetWindowPos();
    const ImVec2              max(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
    const std::vector<Point>  inner = rounded_rect_outline(Rect(min.x, min.y, max.x, max.y), rounding, CORNER_SMOOTHING);
    ImDrawList&               list  = *ImGui::GetWindowDrawList();
    list.PushClipRectFullScreen();
    list.PushTextureID(ImGui::GetIO().Fonts->TexID);
    // Fading as a blur does from half its strength at the edge, so that the shadow does not darken the outline into a second line.
    constexpr int RINGS = 6;
    const auto    color_at = [&](int ring) {
        const float rest  = 1.f - float(ring) / float(RINGS);
        Color       color = ui.theme().shadow;
        color.a *= .5f * level.opacity * rest * rest;
        return u32(color);
    };
    std::vector<Point> from = inner;
    for (int i = 1; i <= RINGS; ++i) {
        const float        grow = m.px(level.spread) * float(i) / float(RINGS);
        const float        drop = .5f * grow;
        std::vector<Point> to   = rounded_rect_outline(Rect(min.x - grow, min.y - grow + drop, max.x + grow, max.y + grow + drop), rounding + grow,
                                                       CORNER_SMOOTHING);
        fill_between(list, from, to, color_at(i - 1), color_at(i));
        from = std::move(to);
    }
    list.PopTextureID();
    list.PopClipRect();
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
