#pragma once

#include <optional>
#include <string>

#include "Availability.hpp"
#include "Icons.hpp"
#include "Measure.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Every component takes its id, its value when it has one, and its options, and returns whether the user
// changed or pressed it in this frame. Measure.hpp gives the sizes of all of them.

enum class ButtonVariant : unsigned char { Default, Secondary, Ghost };

struct ButtonOptions
{
    ButtonVariant variant{ButtonVariant::Secondary};
    ButtonSize    size{ButtonSize::Default};
    // A labelled button's width, zero to fit the label; an icon button's side, zero for the default.
    float         width{0.f};
    Availability  availability;
    // Left of the label in the label's colour, or alone on an icon button.
    std::optional<Icon> icon;
    // Pressed in, as a toolbar button whose popup is open.
    bool          pressed{false};
    // A small triangle in the corner: the button opens a popup.
    bool          has_popup{false};
    std::string   tooltip;
    std::string   key;
};

bool button(const char* id, const std::string& label, const ButtonOptions& options = {});

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
