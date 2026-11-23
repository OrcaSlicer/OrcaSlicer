#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>

#include "LegendAxis.hpp"
#include "LegendModel.hpp"
#include "LegendText.hpp"
#include "CanvasUi/Alert.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

namespace CanvasUi {
class PanelBody;
} // namespace CanvasUi

std::string                           legend_filter_text(const std::string& range, double share, bool layers);
std::optional<CanvasUi::AlertOptions> legend_limit_note(const LegendScale& scale, const LegendUnits& units);
// A time, or a number with decimals.
std::string legend_scale_value(const LegendScale& scale, double value, int decimals, const LegendUnits& units);
// The scale's unit after the value, unless the value is a time, which has its units.
std::string legend_with_unit(const LegendScale& scale, const std::string& value);

// The bar of a scale in the legend's body: the histogram over it, the axis labels, the profile's limits, the current
// move, the brush that picks the values kept in colour, the row of that filter and the note on the limits.
class LegendScaleView
{
public:
    std::function<void(float, float)> hover_range;
    // The values of the current mode to keep in colour, or none for all of them.
    std::function<void(std::optional<std::array<float, 2>>)> set_range;

    void render(CanvasUi::PanelBody& body, const LegendModel& model, const LegendUnits& units, bool histogram);
    // The exact values to keep in colour, after a double click on an edge of the range; drawn outside the body.
    void render_range_popover(const LegendScale& scale);

private:
    struct Sizes;
    class Bar;

    LegendBrush           m_brush;
    // The exact-range popover opens in the next draw, after a double click on an edge.
    bool                  m_range_pending{false};
    CanvasUi::Rect        m_range_anchor;
    std::array<double, 2> m_range_edit{0.0, 0.0};
};

} // namespace GUI
} // namespace Slic3r
