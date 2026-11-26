#pragma once

#include <vector>

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

enum class Corners : unsigned char {
    None        = 0,
    TopLeft     = 1,
    TopRight    = 2,
    BottomRight = 4,
    BottomLeft  = 8,
    Top         = TopLeft | TopRight,
    Bottom      = BottomRight | BottomLeft,
    All         = Top | Bottom,
};

constexpr Corners operator|(Corners a, Corners b) { return Corners((unsigned char) a | (unsigned char) b); }
constexpr bool    has(Corners set, Corners corner) { return ((unsigned char) set & (unsigned char) corner) != 0; }

inline constexpr int CORNER_POINTS = 16;

// The outline of a rectangle with rounded corners, clockwise on the screen from the left end of the top left corner.
// Smoothing, from 0 to 1, stretches each corner along its sides into a curve of continuous curvature;
// 0 is a circular arc. A corner shrinks to fit a short side, losing its smoothing first. Every outline has
// 4 * CORNER_POINTS points, a square corner as many as a round one, so that two outlines join point by point.
std::vector<Point> rounded_rect_outline(const Rect& rect, float radius, float smoothing, Corners corners = Corners::All);

constexpr float nested_radius(float outer, float inset) { return outer > inset ? outer - inset : 0.f; }

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
