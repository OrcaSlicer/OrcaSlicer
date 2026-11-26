#pragma once

#include <string>
#include <vector>

#include "Availability.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// A setting across the width: its label on the left and its control on the right, the control going under
// the label when both do not fit.
struct FieldOptions
{
    std::string  label;
    float        width{0.f};
    Availability availability;
};

bool field_toggle_group(const char* id, int& selected, const std::vector<std::string>& choices, const FieldOptions& field);
bool field_switch(const char* id, bool& value, const FieldOptions& field);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
