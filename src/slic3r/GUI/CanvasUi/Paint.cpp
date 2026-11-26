#include "Paint.hpp"

#include <functional>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <string_view>
#include <vector>

#include <imgui/imgui.h>

#include "Context.hpp"
#include "Font.hpp"
#include "Icons.hpp"
#include "ImGuiBridge.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Corners.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

static std::vector<ImVec2> im(const std::vector<Point>& points)
{
    std::vector<ImVec2> out;
    out.reserve(points.size());
    for (const Point& p : points)
        out.push_back(CanvasUi::im(p));
    return out;
}

Painter Painter::background() { return Painter(*ImGui::GetBackgroundDrawList()); }

Painter Painter::current() { return Painter(*ImGui::GetWindowDrawList()); }

namespace {

void path_outline(ImDrawList& list, const Rect& rect, float radius, Corners corners)
{
    const std::vector<Point> outline = rounded_rect_outline(rect, radius, CORNER_SMOOTHING, corners);
    for (const Point& p : outline)
        if (list._Path.Size == 0 || std::abs(list._Path.back().x - p.x) > 1e-3f || std::abs(list._Path.back().y - p.y) > 1e-3f)
            list.PathLineTo(im(p));
}

} // namespace

void fill_rounded(ImDrawList& list, const ImVec2& min, const ImVec2& max, ImU32 color, float radius, Corners corners)
{
    if (radius <= 0.f || corners == Corners::None) {
        list.AddRectFilled(min, max, color);
        return;
    }
    path_outline(list, Rect(min.x, min.y, max.x, max.y), radius, corners);
    list.PathFillConvex(color);
}

void stroke_rounded(ImDrawList& list, const ImVec2& min, const ImVec2& max, ImU32 color, float radius, float width, Corners corners)
{
    if (radius <= 0.f || corners == Corners::None) {
        list.AddRect(min, max, color, 0.f, ImDrawFlags_None, width);
        return;
    }
    // Half a pixel in, as ImGui strokes a rectangle, so that a one pixel line covers whole pixels.
    path_outline(list, Rect(min.x + .5f, min.y + .5f, max.x - .5f, max.y - .5f), radius, corners);
    list.PathStroke(color, ImDrawFlags_Closed, width);
}

static void with_window_rect(const std::function<void(ImDrawList&, const ImVec2&, const ImVec2&)>& draw)
{
    ImDrawList&  list = *ImGui::GetWindowDrawList();
    const ImVec2 min  = ImGui::GetWindowPos();
    list.PushClipRectFullScreen();
    draw(list, min, ImVec2(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight()));
    list.PopClipRect();
}

void fill_window(ImU32 color, float radius)
{
    with_window_rect([&](ImDrawList& list, const ImVec2& min, const ImVec2& max) { fill_rounded(list, min, max, color, radius); });
}

void stroke_window(ImU32 color, float radius, float width)
{
    with_window_rect([&](ImDrawList& list, const ImVec2& min, const ImVec2& max) { stroke_rounded(list, min, max, color, radius, width); });
}

void Painter::fill_rect(const Rect& rect, const Color& color, float radius, Corners corners) const
{
    fill_rounded(*m_list, CanvasUi::im(rect.min()), CanvasUi::im(rect.max()), u32(color), radius, corners);
}

void Painter::stroke_rect(const Rect& rect, const Color& color, float width, float radius, Corners corners) const
{
    stroke_rounded(*m_list, CanvasUi::im(rect.min()), CanvasUi::im(rect.max()), u32(color), radius, width, corners);
}

void Painter::fill_gradient(const Rect& rect, const Color& from, const Color& to, GradientAxis axis) const
{
    const ImU32 a = u32(from), b = u32(to);
    if (axis == GradientAxis::Horizontal)
        m_list->AddRectFilledMultiColor(CanvasUi::im(rect.min()), CanvasUi::im(rect.max()), a, b, b, a);
    else
        m_list->AddRectFilledMultiColor(CanvasUi::im(rect.min()), CanvasUi::im(rect.max()), a, a, b, b);
}

void Painter::fill_gradient(const Rect& rect, const std::vector<Color>& stops, GradientAxis axis) const
{
    if (stops.size() < 2) {
        if (!stops.empty())
            fill_rect(rect, stops.front());
        return;
    }
    const bool  horizontal = axis == GradientAxis::Horizontal;
    const float from       = horizontal ? rect.left : rect.top;
    const float length     = horizontal ? rect.width() : rect.height();
    const float step       = length / float(stops.size() - 1);
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const float a = from + float(i) * step;
        const float b = i + 2 == stops.size() ? from + length : from + float(i + 1) * step;
        fill_gradient(horizontal ? Rect(a, rect.top, b, rect.bottom) : Rect(rect.left, a, rect.right, b), stops[i], stops[i + 1], axis);
    }
}

void Painter::line(const Point& a, const Point& b, const Color& color, float width) const
{
    m_list->AddLine(CanvasUi::im(a), CanvasUi::im(b), u32(color), width);
}

void Painter::divider(float left, float right, float bottom) const
{
    const Context& ui = *Context::current();
    const float    y  = bottom - ui.metrics().outline;
    line(Point(left, y), Point(right, y), ui.theme().border, ui.metrics().outline);
}

void Painter::polyline(const std::vector<Point>& points, const Color& color, float width, bool closed) const
{
    const std::vector<ImVec2> path = im(points);
    m_list->AddPolyline(path.data(), int(path.size()), u32(color), closed ? ImDrawFlags_Closed : ImDrawFlags_None, width);
}

void Painter::fill_convex(const std::vector<Point>& points, const Color& color, float softness) const
{
    const std::vector<ImVec2> path   = im(points);
    const float               fringe = m_list->_FringeScale;
    if (softness > 0.f)
        m_list->_FringeScale = softness;
    m_list->AddConvexPolyFilled(path.data(), int(path.size()), u32(color));
    m_list->_FringeScale = fringe;
}

void Painter::fill_circle(const Point& center, float radius, const Color& color) const
{
    m_list->AddCircleFilled(CanvasUi::im(center), radius, u32(color));
}

void Painter::stroke_circle(const Point& center, float radius, const Color& color, float width) const
{
    m_list->AddCircle(CanvasUi::im(center), radius, u32(color), 0, width);
}

void Painter::text(const Font& font, const Point& pos, const Color& color, std::string_view text) const
{
    m_list->AddText(face_of(font), font.size(), CanvasUi::im(pos), u32(color), text.data(), text.data() + text.size());
}

void Painter::text(const Font& font, const Rect& band, const Color& color, std::string_view text) const
{
    this->text(font, Point(band.left, band.top + 0.5f * (band.height() - font.size())), color, text);
}

void Painter::text_wrapped(const Font& font, const Point& pos, float width, const Color& color, std::string_view text) const
{
    m_list->AddText(face_of(font), font.size(), CanvasUi::im(pos), u32(color), text.data(), text.data() + text.size(), width);
}

void Painter::text_in_plane(const Font& font, std::string_view text, const Color& color, const Point& center, const Point& right,
                            const Point& down) const
{
    const ImVec2 extent = face_of(font)->CalcTextSizeA(font.size(), FLT_MAX, 0.f, text.data(), text.data() + text.size());
    const int    start  = m_list->VtxBuffer.Size;
    m_list->PushClipRectFullScreen();
    m_list->AddText(face_of(font), font.size(), ImVec2(center.x - .5f * extent.x, center.y - .5f * extent.y), u32(color), text.data(),
                    text.data() + text.size());
    m_list->PopClipRect();
    for (int i = start; i < m_list->VtxBuffer.Size; ++i) {
        ImDrawVert& v = m_list->VtxBuffer[i];
        const float u = v.pos.x - center.x;
        const float w = v.pos.y - center.y;
        v.pos         = ImVec2(center.x + right.x * u + down.x * w, center.y + right.y * u + down.y * w);
    }
}

void Painter::icon(Icon icon, const Point& center, float size, const Color& color) const
{
    draw_icon(*m_list, icon, CanvasUi::im(center), size, u32(color));
}

void Painter::shadow_below(float left, float right, float y) const
{
    const Context& ui    = *Context::current();
    const Metrics& m     = ui.metrics();
    Color          clear = ui.theme().shadow_below;
    clear.a              = 0.f;
    fill_gradient(Rect(left, y, right, y + m.shadow_below_height), ui.theme().shadow_below, clear,
                  GradientAxis::Vertical);
}

Painter::Clip::Clip(const Painter& painter, const Rect& rect) : m_list(*painter.m_list)
{
    m_list.PushClipRect(CanvasUi::im(rect.min()), CanvasUi::im(rect.max()), true);
}

Painter::Clip::~Clip() { m_list.PopClipRect(); }

Painter::SharpFills::SharpFills(const Painter& painter) : m_list(*painter.m_list), m_flags(m_list.Flags)
{
    m_list.Flags &= ~ImDrawListFlags_AntiAliasedFill;
}

Painter::SharpFills::~SharpFills() { m_list.Flags = m_flags; }

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
