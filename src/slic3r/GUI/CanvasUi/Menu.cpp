#include "Menu.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "Measure.hpp"
#include "Metrics.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

MenuItem MenuItem::item(int id, std::string label, std::string key)
{
    MenuItem i;
    i.id    = id;
    i.label = std::move(label);
    i.key   = std::move(key);
    return i;
}

MenuItem MenuItem::checkbox_item(int id, std::string label, bool checked, Indicator indicator)
{
    MenuItem i  = item(id, std::move(label));
    i.indicator = indicator;
    i.checked   = checked;
    i.closes    = false;
    return i;
}

MenuItem MenuItem::radio_item(int id, std::string label, bool checked)
{
    MenuItem i  = item(id, std::move(label));
    i.indicator = Indicator::Radio;
    i.checked   = checked;
    return i;
}

MenuItem MenuItem::group_label(std::string label)
{
    MenuItem i;
    i.kind  = Kind::GroupLabel;
    i.label = std::move(label);
    return i;
}

MenuItem MenuItem::separator()
{
    MenuItem i;
    i.kind = Kind::Separator;
    return i;
}

float menu_text_left(const Metrics& metrics, const std::vector<MenuItem>& items)
{
    const bool marks = std::any_of(items.begin(), items.end(),
                                   [](const MenuItem& item) { return item.kind == MenuItem::Kind::Item && item.indicator != Indicator::None; });
    return marks ? metrics.menu_text_left : metrics.menu_text_inset;
}

Size menu_size(const Metrics& metrics, const TextMeasure& measure, const std::vector<MenuItem>& items, float min_width)
{
    const float text_left  = menu_text_left(metrics, items);
    float       width      = 0.f;
    float       height     = 0.f;
    for (const MenuItem& item : items) {
        switch (item.kind) {
        case MenuItem::Kind::Item: {
            float row = text_left + measure.width(item.label) + metrics.menu_text_inset;
            for (const std::string* right : {&item.key, &item.value})
                if (!right->empty())
                    row += metrics.menu_key_gap + measure.width(*right);
            width = std::max(width, row);
            height += metrics.menu_row;
            break;
        }
        case MenuItem::Kind::GroupLabel:
            width = std::max(width, measure.width(item.label) + 2.f * metrics.menu_label_inset);
            height += metrics.menu_group_label;
            break;
        case MenuItem::Kind::Separator: height += metrics.menu_separator; break;
        }
    }
    return {std::ceil(std::max(min_width, width + 2.f * metrics.menu_padding_x)), std::ceil(height + 2.f * metrics.menu_padding_y)};
}

Positioner Positioner::point(float x, float y) { return {{x, x, true}, {y, y, true}}; }

Positioner Positioner::beside(const Rect& anchor, float gap)
{
    return {{anchor.right + gap, anchor.left - gap, true}, {anchor.top, anchor.bottom, false}};
}

Positioner Positioner::below(const Rect& anchor, float gap)
{
    return {{anchor.left, anchor.right, true}, {anchor.bottom + gap, anchor.top - gap, true}};
}

float position_popup_axis(const PositionerAxis& axis, float size, float screen_min, float screen_max)
{
    const bool fits_forward  = axis.forward_from + size <= screen_max;
    const bool fits_backward = axis.backward_from - size >= screen_min;
    const bool forward       = axis.forward ? fits_forward || !fits_backward : !fits_backward && fits_forward;
    const float start        = forward ? axis.forward_from : axis.backward_from - size;
    return std::clamp(start, screen_min, std::max(screen_min, screen_max - size));
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
