#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "LegendText.hpp"

namespace Slic3r {
namespace GUI {

// A stretch of the legend's axis. Bins with print time take the room, a run of empty bins or of bins with a trace of it
// shrinks to a sliver, so that a print at a few values does not leave most of the histogram blank; within a piece the
// axis is the scale's own.
struct LegendAxisPiece
{
    double lo{0.0};
    double hi{0.0};
    float  x0{0.f};
    float  x1{0.f};
    bool   empty{false};
    float  weight{1.f};
};

// The legend's scale laid along x from left to right over the bins of its histogram, and the brush's picks on it.
class LegendAxis
{
public:
    // A full bin is at most widest wide.
    LegendAxis(float min, float max, bool logarithmic, const LegendBins& bins, float left, float right, float widest);

    const std::vector<LegendAxisPiece>& pieces() const { return m_pieces; }
    std::array<float, 2>                scale_range() const { return {m_min, m_max}; }

    // Where the value lies along the scale, from 0 to 1, and back.
    float  t_of(double value) const;
    double value_at_t(float t) const;
    float  x_on(double value) const;
    double value_on(float x) const;

    // The bin at x, if it has print time, and its values within the scale.
    std::optional<size_t> bin_at(float x) const;
    std::array<float, 2>  bin_range(size_t bin) const;
    // The values between two places on the axis, out to the edges of the bins they fall in.
    std::array<float, 2>  brushed(float a, float b) const;

private:
    float                        m_min;
    float                        m_max;
    bool                         m_logarithmic;
    const LegendBins&            m_bins;
    std::vector<LegendAxisPiece> m_pieces;
};

// A range over the whole scale filters nothing.
bool legend_range_whole(const std::array<float, 2>& range, float min, float max);

// A gesture over the legend's histogram, kept between frames while the button is down: a press not yet a click or a drag,
// a brush across bars, or an edge of the range dragged.
struct LegendBrush
{
    enum class Phase { Idle, Pressed, Brushing, EdgeDrag };
    Phase phase{Phase::Idle};
    float from{0.f};
    // The edge of the range under the press, or the one dragged.
    int   edge{-1};
    // The range before a click on an edge, given back when that click turns out to start a double click.
    std::optional<std::array<float, 2>> range_before_edge_click;
};

struct LegendBrushInput
{
    std::optional<std::array<float, 2>> filter;
    float                               x{0.f};
    bool                                hovered{false};
    bool                                pressed{false};
    bool                                held{false};
    bool                                double_clicked{false};
    bool                                pressed_anywhere{false};
    bool                                shift{false};
    float                               left{0.f};
    float                               right{0.f};
    float                               edge_grab{0.f};
    float                               drag_threshold{0.f};
};

struct LegendBrushStep
{
    int                                 hovered_edge{-1};
    int                                 dragged_edge{-1};
    bool                                range_set{false};
    std::optional<std::array<float, 2>> range;
    std::optional<std::array<float, 2>> hover;
    std::optional<std::array<float, 2>> popover;
};

// A brush over the histogram picks the values to keep in colour: hovering a bar shows its values in the preview, a click
// keeps them, Shift+click adds them, a drag keeps the values it spans, and the edges of the range then drag on their own.
LegendBrushStep legend_brush_step(LegendBrush& brush, const LegendBrushInput& input, const LegendAxis& axis);

} // namespace GUI
} // namespace Slic3r
