#pragma once

#include <string>
#include <vector>

#include "Availability.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;
class TextMeasure;

enum class Indicator : unsigned char { None, Check, Radio, Visibility };

struct MenuItem
{
    enum class Kind : unsigned char { Item, GroupLabel, Separator };

    Kind        kind{Kind::Item};
    // What menu() returns when the item is picked.
    int         id{-1};
    std::string label;
    std::string key;
    Indicator   indicator{Indicator::None};
    bool        checked{false};
    Availability availability;
    bool        closes{true};
    // On the right, where a key goes: what the item holds, as a range of values.
    std::string value;
    // The current choice of a select, tinted.
    bool        selected{false};
    // Still offered, but with little to show.
    bool        muted{false};

    static MenuItem item(int id, std::string label, std::string key = {});
    // Keeps the menu open, as a setting may be flipped several times in a row.
    static MenuItem checkbox_item(int id, std::string label, bool checked, Indicator indicator = Indicator::Check);
    static MenuItem radio_item(int id, std::string label, bool checked);
    static MenuItem group_label(std::string label);
    static MenuItem separator();
};

// Where the text of a menu's items starts in their row: after the column of marks only when an item has a mark.
float menu_text_left(const Metrics& metrics, const std::vector<MenuItem>& items);

// The outer size of a menu: as wide as its widest row or section header, so that a translation widens the
// menu instead of running out of it, and at least min_width.
Size menu_size(const Metrics& metrics, const TextMeasure& measure, const std::vector<MenuItem>& items, float min_width = 0.f);

// Where a menu opens along one axis: forward from forward_from, or backward from backward_from.
struct PositionerAxis
{
    float forward_from;
    float backward_from;
    bool  forward{true};
};

struct Positioner
{
    PositionerAxis x;
    PositionerAxis y;

    // Right of and below the point, as a context menu opens.
    static Positioner point(float x, float y);
    // Right of the rectangle, gap away, bottoms aligned: the menu grows up.
    static Positioner beside(const Rect& anchor, float gap);
    // Under the rectangle, gap away, left edges aligned; over it when it does not fit below.
    static Positioner below(const Rect& anchor, float gap);
};

// The start of a menu of the given size on the axis: in its preferred direction, in the other one when only
// that fits between the screen edges, then pushed inside them.
float position_popup_axis(const PositionerAxis& axis, float size, float screen_min, float screen_max);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
