#pragma once

#include <optional>
#include <string>

#include "Availability.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct NumberFieldOptions
{
    float        width{0.f};
    Availability availability;
};

// Enter confirms (returns true) a value parse_integer() accepts; Esc restores the value the field had when it
// was focused.
bool number_field(const char* id, long long& value, const NumberFieldOptions& options);

// A whole decimal number, a minus allowed only in front; nothing for anything else or a number out of range.
std::optional<long long> parse_integer(const std::string& text);

// A number with a fractional part, kept as it is typed: returns true when the text changed to one parse_decimal()
// accepts, so a value typed is taken without Enter. Esc restores the value the field had when it was focused.
bool number_field(const char* id, double& value, int decimals, const NumberFieldOptions& options);

// A decimal number with a point or a comma before its fraction, a minus allowed only in front; nothing for anything else.
std::optional<double> parse_decimal(const std::string& text);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
