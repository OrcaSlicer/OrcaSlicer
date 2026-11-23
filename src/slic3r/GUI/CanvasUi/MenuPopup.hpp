#pragma once

#include <optional>
#include <vector>

#include "Menu.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

void open_popup(const char* id);
bool popup_open(const char* id);
// Draws the open menu popup id from its items, beside its anchor and inside the screen, at least min_width
// wide. Returns the id of the item picked in this frame.
std::optional<int> show_menu_popup(const char* id, const Positioner& anchor, const std::vector<MenuItem>& items, float min_width = 0.f);

// The menu popup id, its items built by items() only while it is open.
template<class Items> std::optional<int> menu_popup(const char* id, const Positioner& anchor, Items&& items, float min_width = 0.f)
{
    if (!popup_open(id))
        return std::nullopt;
    return show_menu_popup(id, anchor, items(), min_width);
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
