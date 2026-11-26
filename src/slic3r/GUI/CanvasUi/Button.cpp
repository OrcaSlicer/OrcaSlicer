#include "Button.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <imgui/imgui.h>

#include "ComponentParts.hpp"
#include "Context.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"
#include "Icons.hpp"
#include "Tooltip.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

namespace {

bool icon_button(const char* id, const ButtonOptions& options)
{
    const Metrics& m    = metrics();
    const Theme&   t    = theme();
    const float    side = options.width > 0.f ? options.width : m.icon_button;
    const Hit      h    = hit(id, ImVec2(side, side), options.availability);
    ImDrawList&    list = *ImGui::GetWindowDrawList();

    if (options.pressed) {
        fill_rounded(list, h.min, h.max, u32(t.selection), m.icon_button_radius);
        stroke_rounded(list, h.min, h.max, u32(t.primary), m.icon_button_radius, m.outline);
    }
    else if (h.hovered)
        fill_rounded(list, h.min, h.max, u32(t.accent), m.icon_button_radius);
    const Color ink = !options.availability.enabled ? t.disabled_foreground : options.pressed ? t.primary_strong : t.foreground;
    if (options.icon)
        draw_icon(list, *options.icon, ImVec2(0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y)), side * (m.button_icon / m.icon_button), u32(ink));
    if (options.has_popup) {
        const float  inset  = m.menu_indicator_inset;
        const ImVec2 corner(h.max.x - inset, h.max.y - inset);
        list.AddTriangleFilled(ImVec2(corner.x - m.menu_indicator, corner.y), corner, ImVec2(corner.x, corner.y - m.menu_indicator),
                               u32(options.pressed ? t.primary_strong : t.muted_foreground));
    }
    if (h.hovered && !(options.has_popup && options.pressed) && !options.tooltip.empty())
        item_tooltip(options.tooltip, options.key);
    reason_tooltip(options.availability);
    return h.clicked;
}

} // namespace

bool button(const char* id, const std::string& label, const ButtonOptions& options)
{
    if (options.size == ButtonSize::Icon)
        return icon_button(id, options);
    const Metrics&         m            = metrics();
    const Theme&           t            = theme();
    const bool             is_small     = options.size == ButtonSize::Sm;
    const Availability&    availability = options.availability;
    const FontMeasure      measure;
    const std::string_view text         = visible_label(label);
    const float            text_w       = measure.width(text);
    const float            icon_w       = options.icon ? (is_small ? m.small_button_icon : m.button_icon) : 0.f;
    const float            radius  = is_small ? m.small_button_radius : m.control_radius;
    const ImVec2           size(options.width > 0.f ? options.width : button_width(m, measure, label, options.size, options.icon.has_value()),
                                button_height(m, options.size));
    const Hit   h    = hit(id, size, availability);
    ImDrawList& list = *ImGui::GetWindowDrawList();

    Color fill(0.f, 0.f, 0.f, 0.f);
    Color ink = t.foreground;
    if (options.variant == ButtonVariant::Default) {
        fill = h.hovered ? blend_over(t.accent, t.primary_strong) : t.primary_strong;
        ink  = t.primary_foreground;
    } else if (options.variant == ButtonVariant::Secondary) {
        fill = t.secondary;
        if (h.hovered)
            fill = t.secondary_hover;
    } else {
        ink = t.primary_strong;
        // On the hover surface the accent text loses contrast, so it turns to the foreground.
        if (h.hovered) {
            fill = t.accent;
            ink  = t.foreground;
        }
    }
    if (!availability.enabled) {
        fill = options.variant == ButtonVariant::Ghost ? Color(0.f, 0.f, 0.f, 0.f) : t.secondary;
        ink  = t.disabled_foreground;
    }
    fill_rounded(list, h.min, h.max, u32(fill), radius);
    if (options.variant == ButtonVariant::Secondary)
        stroke_rounded(list, h.min, h.max, u32(t.input), radius, m.outline);
    const float content = (icon_w > 0.f ? icon_w + m.button_icon_gap : 0.f) + text_w;
    const float left    = 0.5f * (h.min.x + h.max.x - content);
    const float middle  = 0.5f * (h.min.y + h.max.y);
    if (options.icon)
        draw_icon(list, *options.icon, ImVec2(left + .5f * icon_w, middle), icon_w, u32(ink));
    const float text_x = left + content - text_w;
    list.AddText(ImVec2(text_x, middle - .5f * measure.line_height()), u32(ink), text.data(), text.data() + text.size());
    if (availability.enabled && !options.tooltip.empty())
        item_tooltip(options.tooltip, options.key);
    else
        reason_tooltip(availability);
    return h.clicked;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
