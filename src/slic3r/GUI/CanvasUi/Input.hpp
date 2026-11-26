#pragma once

#include <string>

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Pointer
{
    Point pos;
    Point delta;
    // The left button went down in this frame, and is down.
    bool   pressed{false};
    bool   held{false};
    // The right button went down in this frame.
    bool   context_pressed{false};
    // Over a panel or any other overlay, not over the bare scene.
    bool   over_panel{false};
    bool   alt{false};
    bool   shift{false};
    bool   double_clicked{false};
    bool   released{false};
};

Pointer pointer();
// Keeps the mouse with the panels in this frame, so that the scene under them does not act on it.
void capture_mouse();
bool escape_pressed();
// Whether a field or another item of any panel holds the keyboard, in this frame or the last, so that a key
// it took is not taken again.
bool input_active();
void copy_to_clipboard(const std::string& text);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
