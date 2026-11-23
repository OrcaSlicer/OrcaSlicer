#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;
class TextMeasure;

enum class AlertVariant : unsigned char { Info, Warning };

struct AlertOptions
{
    AlertVariant             variant{AlertVariant::Warning};
    std::string              title;
    std::string              text;
    // Small buttons under the text, the first one the main action.
    std::vector<std::string> actions;
    float                    width{0.f};
};

float alert_height(const Metrics& metrics, const TextMeasure& measure, const TextMeasure& title_measure, const AlertOptions& options);
// A callout at the cursor with its actions. Returns the index of the action pressed.
std::optional<size_t> alert(const char* id, const AlertOptions& options);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
