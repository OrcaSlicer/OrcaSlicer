#pragma once

#include <cstdint>
#include <optional>

#include "Font.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Whether the position field is being edited, and the number typed, kept by the panel between frames.
struct PositionField
{
    bool      editing{false};
    bool      focus{false};
    long long value{0};
    // The frame it was last drawn in; a field left out of a frame, as by a hidden panel, stops editing.
    int       drawn_frame{-1};
};

struct PositionFieldOptions
{
    Font  regular;
    Font  bold;
    Size  size;
    float padding{0.f};
};

// "current / total" in a long list, right-aligned, the current position bold at the regular face's size; a
// click turns it into a number field to jump with. Returns the number Enter confirms; Esc or a click
// elsewhere leaves the field.
std::optional<long long> position_field(const char* id, PositionField& field, uint64_t current, uint64_t total,
                                        const PositionFieldOptions& options);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
