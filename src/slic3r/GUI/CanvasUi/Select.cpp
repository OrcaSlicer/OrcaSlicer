#include "Select.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "ComponentParts.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Icons.hpp"
#include "Measure.hpp"
#include "Menu.hpp"
#include "MenuPopup.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Tooltip.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

std::optional<int> select(const char* id, const std::string& value, const std::vector<MenuItem>& items, const SelectOptions& options)
{
    const Metrics& m     = metrics();
    const Theme&   t     = theme();
    const float    width = options.width > 0.f ? options.width : m.px(space<160>());
    const Hit      h     = hit(id, ImVec2(width, m.control_height), options.availability);
    const std::string menu_id = std::string(id) + "/menu";
    const bool        open    = popup_open(menu_id.c_str());
    ImDrawList&    list  = *ImGui::GetWindowDrawList();

    const Color fill = !options.availability.enabled ? t.secondary : h.hovered && !open ? t.secondary_hover : t.secondary;
    fill_rounded(list, h.min, h.max, u32(fill), m.control_radius);
    if (open)
        stroke_rounded(list, h.min, h.max, u32(t.primary), m.control_radius, m.focus_ring);
    else
        stroke_rounded(list, h.min, h.max, u32(t.input), m.control_radius, m.outline);

    const Color       ink     = options.availability.enabled ? t.foreground : t.disabled_foreground;
    const float       chevron = m.menu_icon;
    const float       right   = h.max.x - m.control_padding - chevron;
    const FontMeasure measure;
    const std::string shown   = ellipsize(measure, value, right - m.inline_gap - (h.min.x + m.control_padding));
    const float       middle  = .5f * (h.min.y + h.max.y);
    list.AddText(ImVec2(h.min.x + m.control_padding, middle - .5f * measure.line_height()), u32(ink), shown.c_str());
    draw_icon(list, Icon::ChevronDown, ImVec2(right + .5f * chevron, middle), chevron, u32(options.availability.enabled ? t.muted_foreground : ink));

    if (h.clicked && !open)
        open_popup(menu_id.c_str());
    if (h.hovered && !open && !options.tooltip.empty())
        item_tooltip(options.tooltip);
    reason_tooltip(options.availability);
    const Rect anchor(h.min.x, h.min.y, h.max.x, h.max.y);
    return menu_popup(menu_id.c_str(), Positioner::below(anchor, m.popup_side_offset), [&items]() -> const std::vector<MenuItem>& { return items; },
                      std::max(width, options.menu_width));
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
