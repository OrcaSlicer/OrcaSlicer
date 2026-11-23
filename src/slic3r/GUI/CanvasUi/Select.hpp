#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Availability.hpp"
#include "Menu.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct SelectOptions
{
    float        width{0.f};
    // The menu, at least as wide as the field.
    float        menu_width{0.f};
    Availability availability;
    std::string  tooltip;
};

// A field with the current choice and a chevron: a click opens the items under it.
// Returns the id of the item picked in this frame.
std::optional<int> select(const char* id, const std::string& value, const std::vector<MenuItem>& items, const SelectOptions& options);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
