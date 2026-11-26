#pragma once

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// What the pointer does over an area the panel draws itself, counted only while the area is what the pointer is seen
// over: not under a menu, popover or other window, and not past the clip of a scroll area. Held lasts from the press
// to the release wherever the pointer goes.
struct ZoneState
{
    bool hovered{false};
    bool pressed{false};
    bool held{false};
    bool released{false};
    bool double_clicked{false};
};

ZoneState zone(const char* id, const Rect& rect);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
