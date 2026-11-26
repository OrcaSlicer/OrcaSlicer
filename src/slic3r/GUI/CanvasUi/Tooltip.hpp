#pragma once

#include <string>

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;
class TextMeasure;

// The tooltip of the last item, disabled ones included, once the hover delay has passed.
void item_tooltip(const std::string& text, const std::string& key = {});
// The tooltip of something hovered that is not an ImGui item, such as a zone drawn by hand, under an id of
// its own in the current ImGui id scope.
void tooltip_for(const char* id, const std::string& text, const std::string& key = {});
Size tooltip_size(const Metrics& metrics, const TextMeasure& measure, const std::string& text);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
