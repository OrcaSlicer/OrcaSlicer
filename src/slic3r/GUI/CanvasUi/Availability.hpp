#pragma once

#include <string>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Whether a component can be used, and why not when it cannot: a disabled component ignores the mouse and
// shows its reason, when there is one, in a tooltip.
struct Availability
{
    bool        enabled{true};
    std::string reason;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
