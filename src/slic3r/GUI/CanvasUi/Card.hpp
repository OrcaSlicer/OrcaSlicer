#pragma once

#include "Paint.hpp"
#include "Types.hpp"

struct ImDrawList;

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct CardStyle
{
    // Where pos sits on the card: (0, 0) its top left, (1, 1) its bottom right.
    Point pivot;
    // A negative padding takes the toolkit's card padding.
    float  padding{-1.f};
    bool   shadow{true};
};

// A panel over the scene: an opaque card with a 1 px outline, a soft shadow and rounded corners, open
// from construction to destruction. A zero width or height fits the card to its content on that axis.
// The card opens its own ImGui id scope, stacks its components the item gap apart, and does not scroll with
// the wheel; a ScrollArea inside it does.
class Card
{
public:
    Card(const char* id, const Point& pos, const Size& size, const CardStyle& style = {});
    ~Card();
    Card(const Card&)            = delete;
    Card& operator=(const Card&) = delete;

    Painter painter() const;
    Rect    rect() const { return m_rect; }
    // Under the mouse at the start of the frame, and not a scroll area in it.
    bool    hovered() const;

    // The background of a row across the card, kept inside the outline and rounded where the row
    // reaches the rounded top or bottom of the card.
    void row_background(float top, float bottom, const Color& color) const;

private:
    Rect        m_rect;
    float       m_radius{0.f};
    ImDrawList* m_list{nullptr};
    bool        m_hovered{false};
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
