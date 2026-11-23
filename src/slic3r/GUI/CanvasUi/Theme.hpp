#pragma once

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Theme
{
    bool  dark{false};
    Color card;
    Color card_border;
    Color foreground;
    Color muted_foreground;
    // An option or a tab not selected: readable on the track of a toggle group and on the tabs list.
    Color inactive_foreground;
    Color disabled_foreground;
    Color border;
    Color secondary;
    Color secondary_hover;
    Color input;
    // Hovered and highlighted surfaces.
    Color accent;
    Color primary;
    // Text and icons in the primary colour.
    Color primary_strong;
    // Text and icons on a primary_strong surface.
    Color primary_foreground;
    // A selected item or row: a tint of the primary colour.
    Color selection;
    Color tooltip;
    Color tooltip_foreground;
    Color tooltip_muted_foreground;
    // The pressed item of a toggle group, raised over its track.
    Color toggle_pressed;
    Color toggle_pressed_border;
    Color toggle_pressed_shadow;
    Color switch_thumb;
    Color switch_thumb_shadow;
    Color scrollbar_thumb;
    Color scrollbar_thumb_hover;
    Color scrollbar_thumb_active;
    Color tabs_list;
    // Fainter than accent in the dark theme, so a hovered tab does not outshine the selected one.
    Color tab_hover;
    // The darkest edge of the shadow that settings opened over content cast on it.
    Color shadow_below;
    // The bar at the left of the current line of a list.
    Color current_line;
    // A position indicator that is not dragged, unlike a scrollbar.
    Color position_indicator;
    Color position_indicator_thumb;
    // Around a mark whose own colour contrasts too little with the card.
    Color contrast_outline;
    // The darkest layer of a card's or a popup's shadow.
    Color shadow;
    // Text and icons of a warning, and the tint of its surface.
    Color warning;
    Color warning_surface;
    Color info;
};

const Theme& theme(bool dark);

// Secondary text, which takes the foreground on the tint of a hovered or selected item, where muted falls under 4.5:1.
inline const Color& muted_text(const Theme& theme, bool on_tint) { return on_tint ? theme.foreground : theme.muted_foreground; }

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
