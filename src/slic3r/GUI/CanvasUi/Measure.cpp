#include "Measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Metrics.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

float MonospaceMeasure::width(std::string_view text) const
{
    const auto characters = std::count_if(text.begin(), text.end(), [](char c) { return !is_utf8_continuation(c); });
    return float(characters) * m_char_width;
}

float MonospaceMeasure::height(std::string_view text, float wrap_width) const
{
    int   lines = 1;
    float line  = 0.f;
    for (size_t start = 0; start < text.size();) {
        const size_t space = std::min(text.find(' ', start), text.size());
        const float  word  = width(text.substr(start, space - start));
        if (line > 0.f && line + width(" ") + word > wrap_width) {
            ++lines;
            line = word;
        } else
            line += (line > 0.f ? width(" ") : 0.f) + word;
        start = space + 1;
    }
    return float(lines) * m_line_height;
}

std::string ellipsize(const TextMeasure& measure, std::string_view text, float width)
{
    if (measure.width(text) <= width)
        return std::string(text);
    static constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";
    const float room = width - measure.width(ELLIPSIS);
    size_t      end  = text.size();
    while (end > 0) {
        do
            --end;
        while (end > 0 && is_utf8_continuation(text[end]));
        if (measure.width(text.substr(0, end)) <= room)
            break;
    }
    return std::string(text.substr(0, end)) + std::string(ELLIPSIS);
}

std::string_view visible_label(std::string_view label) { return label.substr(0, label.find("##")); }

float button_width(const Metrics& metrics, const TextMeasure& measure, std::string_view label, ButtonSize size, bool with_icon)
{
    if (size == ButtonSize::Icon)
        return metrics.icon_button;
    const bool  is_small = size == ButtonSize::Sm;
    const float icon  = with_icon ? (is_small ? metrics.small_button_icon : metrics.button_icon) + metrics.button_icon_gap : 0.f;
    return measure.width(visible_label(label)) + icon + 2.f * (is_small ? metrics.small_button_padding : metrics.button_padding);
}

float button_height(const Metrics& metrics, ButtonSize size)
{
    return size == ButtonSize::Sm ? metrics.small_button : size == ButtonSize::Icon ? metrics.icon_button : metrics.control_height;
}

float toggle_width(const Metrics& metrics, const TextMeasure& choices, std::string_view option)
{
    return choices.width(visible_label(option)) + 2.f * metrics.toggle_padding;
}

float toggle_group_width(const Metrics& metrics, const TextMeasure& choices, const std::vector<std::string>& options)
{
    float width = float(options.size() + 1) * metrics.toggle_gap;
    for (const std::string& option : options)
        width += toggle_width(metrics, choices, option);
    return width;
}

float toggle_group_height(const Metrics& metrics, const TextMeasure& choices, const std::vector<std::string>& options, float width)
{
    if (toggle_group_width(metrics, choices, options) <= width)
        return metrics.control_height;
    return float(options.size()) * (metrics.control_height - metrics.toggle_gap) + metrics.toggle_gap;
}

float tab_width(const Metrics& metrics, const TextMeasure& choices, std::string_view label)
{
    return choices.width(visible_label(label)) + 2.f * metrics.tab_padding;
}

float tabs_list_width(const Metrics& metrics, const TextMeasure& choices, const std::vector<std::string>& labels)
{
    float width = float(std::max<size_t>(labels.size(), 1) - 1) * metrics.tab_gap;
    for (const std::string& label : labels)
        width += tab_width(metrics, choices, label);
    return width;
}

float position_field_width(const TextMeasure& measure, uint64_t current, uint64_t total, float padding)
{
    return measure.width(std::to_string(current)) + measure.width(" / " + std::to_string(total)) + 2.f * padding;
}

bool field_fits_inline(const Metrics& metrics, const TextMeasure& measure, std::string_view label, float control_width, float width)
{
    return measure.width(visible_label(label)) + metrics.field_gap + control_width <= width;
}

float field_toggle_group_height(const Metrics& metrics, const TextMeasure& measure, const TextMeasure& choices, std::string_view label,
                            const std::vector<std::string>& options, float width)
{
    if (field_fits_inline(metrics, measure, label, toggle_group_width(metrics, choices, options), width))
        return metrics.field_height;
    return measure.height(visible_label(label), width) + metrics.field_label_gap + toggle_group_height(metrics, choices, options, width) +
           metrics.field_stack_bottom;
}

float switch_label_width(const Metrics& metrics, float width) { return width - metrics.switch_width - metrics.field_gap; }

float field_switch_height(const Metrics& metrics, const TextMeasure& measure, std::string_view label, float width)
{
    return std::max(metrics.field_height, measure.height(visible_label(label), switch_label_width(metrics, width)) + 2.f * metrics.switch_text_padding);
}

float field_switch_width(const Metrics& metrics, const TextMeasure& measure, std::string_view label)
{
    return std::ceil(measure.width(visible_label(label))) + metrics.outline + metrics.field_gap + metrics.switch_width;
}

float row_width(float gap, const std::vector<RowSlot>& slots)
{
    float width = float(std::max<size_t>(slots.size(), 1) - 1) * gap;
    for (const RowSlot& slot : slots)
        width += slot.grow ? 0.f : slot.width;
    return width;
}

std::vector<RowSpan> row_layout(float left, float right, float gap, const std::vector<RowSlot>& slots)
{
    const float fixed      = row_width(gap, slots);
    float       shrinkable = 0.f;
    int         grow       = 0;
    for (const RowSlot& slot : slots) {
        shrinkable += slot.shrink && !slot.grow ? slot.width : 0.f;
        grow += slot.grow ? 1 : 0;
    }
    const float overflow = std::max(0.f, fixed - (right - left));
    const float keep     = shrinkable > 0.f ? std::max(0.f, 1.f - overflow / shrinkable) : 1.f;
    const float share    = grow > 0 ? std::max(0.f, right - left - fixed) / float(grow) : 0.f;
    std::vector<RowSpan> spans;
    spans.reserve(slots.size());
    for (const RowSlot& slot : slots) {
        spans.push_back({left, slot.grow ? share : slot.shrink ? slot.width * keep : slot.width});
        left = spans.back().right() + gap;
    }
    return spans;
}

float stack_height(const Metrics& metrics, const std::vector<float>& heights)
{
    float height = 0.f;
    for (const float h : heights)
        height += h;
    return height + float(std::max<size_t>(heights.size(), 1) - 1) * metrics.item_gap;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
