#pragma once

#include <cmath>

#include "Corners.hpp"
#include "Tokens.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Sizes of the toolkit in screen pixels: the logical sizes of the design rules times the scale of the
// interface. Heights of rows and controls and the vertical gaps between them are whole pixels: ImGui floors
// the cursor after each item, so a fractional height would leave the drawn stack shorter than the measured
// one, and stacked rows keep their lines sharp.
struct Metrics
{
    explicit Metrics(float scale = 1.f) : scale(scale) {}

    float scale;

    // The steps are added before scaling, so that a height made of a line and its padding rounds once.
    template<class... Steps> float px(Steps... steps) const { return (steps.logical() + ...) * scale; }
    template<class... Steps> float whole_px(Steps... steps) const { return std::round((steps.logical() + ...) * scale); }

    float card_radius{px(Radius::lg)};
    float card_padding{px(space<12>())};
    float card_gap{px(space<8>())};
    float scrollbar{px(space<6>())};
    // The shadow that settings opened over content cast down on it, and how much narrower it is.
    float shadow_below_height{px(space<8>())};
    // Outlines and dividers stay one physical pixel whatever the scale.
    float outline{1.f};

    float control_padding{px(space<8>())};
    // Between components stacked in a card.
    float item_gap{whole_px(space<4>())};
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
