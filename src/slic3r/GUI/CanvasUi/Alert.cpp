#include "Alert.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "Button.hpp"
#include "ComponentParts.hpp"
#include "Context.hpp"
#include "Font.hpp"
#include "FontMeasure.hpp"
#include "Icons.hpp"
#include "ImGuiBridge.hpp"
#include "Layout.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

namespace {

float text_width(const Metrics& m, const AlertOptions& options)
{
    return options.width - 2.f * m.alert_padding - m.alert_icon - m.alert_icon_gap;
}

float title_height(const Metrics& m, const TextMeasure& title_measure, const AlertOptions& options)
{
    return options.title.empty() ? 0.f : title_measure.height(options.title, text_width(m, options));
}

} // namespace

float alert_height(const Metrics& m, const TextMeasure& measure, const TextMeasure& title_measure, const AlertOptions& options)
{
    const float wrap   = text_width(m, options);
    float       height = 2.f * m.alert_padding + title_height(m, title_measure, options);
    if (!options.text.empty())
        height += (options.title.empty() ? 0.f : m.alert_line_gap) + measure.height(options.text, wrap);
    if (!options.actions.empty())
        height += m.alert_actions_gap + button_height(m, ButtonSize::Sm);
    return std::ceil(height);
}

std::optional<size_t> alert(const char* id, const AlertOptions& options)
{
    const Context& ui      = *Context::current();
    const Metrics& m       = metrics();
    const Theme&   t       = theme();
    const bool     warning = options.variant == AlertVariant::Warning;
    const FontMeasure measure;
    const Font        bold = ui.bold_font();
    const FontMeasure title_measure(bold);
    const float    height  = alert_height(m, measure, title_measure, options);
    const ImVec2   min     = ImGui::GetCursorScreenPos();
    const ImVec2   max(min.x + options.width, min.y + height);
    ImDrawList&    list    = *ImGui::GetWindowDrawList();
    const Color    ink     = warning ? t.warning : t.info;
    fill_rounded(list, min, max, u32(warning ? t.warning_surface : t.accent), m.alert_radius);
    stroke_rounded(list, min, max, u32(faded(ink, .35f)), m.alert_radius, m.outline);

    const float text_left = min.x + m.alert_padding + m.alert_icon + m.alert_icon_gap;
    const float wrap      = text_width(m, options);
    float       y         = min.y + m.alert_padding;
    draw_icon(list, warning ? Icon::TriangleAlert : Icon::Info, ImVec2(min.x + m.alert_padding + .5f * m.alert_icon, y + .5f * measure.line_height()),
              m.alert_icon, u32(ink));
    if (!options.title.empty()) {
        list.AddText(face_of(bold), bold.size(), ImVec2(text_left, y), u32(t.foreground), options.title.c_str(), nullptr, wrap);
        y += title_height(m, title_measure, options) + m.alert_line_gap;
    }
    if (!options.text.empty()) {
        list.AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(text_left, y), u32(t.foreground), options.text.c_str(), nullptr, wrap);
        y += measure.height(options.text, wrap);
    }
    ImGui::Dummy(ImVec2(options.width, height));
    const ImVec2 after = ImGui::GetCursorScreenPos();

    std::optional<size_t> pressed;
    float                 x = text_left;
    ImGui::PushID(id);
    for (size_t i = 0; i < options.actions.size(); ++i) {
        ButtonOptions button_options;
        button_options.size    = ButtonSize::Sm;
        button_options.variant = i == 0 ? ButtonVariant::Secondary : ButtonVariant::Ghost;
        ImGui::SetCursorScreenPos(ImVec2(x, y + m.alert_actions_gap));
        const std::string label = options.actions[i] + "##" + std::to_string(i);
        if (button(label.c_str(), label, button_options))
            pressed = i;
        x += button_width(m, measure, label, ButtonSize::Sm) + m.alert_action_gap;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(after);
    return pressed;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
