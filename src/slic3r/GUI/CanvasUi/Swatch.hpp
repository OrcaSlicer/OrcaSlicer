#pragma once

#include <string>

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

class Painter;

// A sample of the colour a line type, a filament or a value is drawn in.
struct Swatch
{
    Color       color;
    // Only its outline, for something hidden.
    bool        hollow{false};
    // Written on it, as a filament's number.
    std::string label;
    // Over a checkerboard, as a see-through filament.
    bool        translucent{false};
};

// On background: a colour too close to it gets a ring, and a hollow one too close turns grey.
void draw_swatch(const Painter& paint, const Rect& rect, const Swatch& swatch, const Color& background);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
