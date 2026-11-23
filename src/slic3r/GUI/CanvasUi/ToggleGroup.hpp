#pragma once

#include <string>
#include <vector>

#include "Availability.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct ToggleGroupOptions
{
    // Zero fits the choices side by side.
    float        width{0.f};
    Availability availability;
};

// The choices side by side, or one under another when they do not fit the width.
bool toggle_group(const char* id, int& selected, const std::vector<std::string>& choices, const ToggleGroupOptions& options = {});

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
