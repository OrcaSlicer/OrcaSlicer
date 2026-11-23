#include "LegendAxis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "LegendText.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr double TRACE_SHARE  = .005;
constexpr double MINOR_SHARE  = .02;
constexpr float  TRACE_WEIGHT = .15f;
constexpr float  MINOR_WEIGHT = .25f;

} // namespace

LegendAxis::LegendAxis(float min, float max, bool logarithmic, const LegendBins& bins, float left, float right, float widest)
    : m_min(min), m_max(max), m_logarithmic(logarithmic), m_bins(bins)
{
    const double total = std::accumulate(bins.amounts.begin(), bins.amounts.end(), 0.0);
    for (size_t i = 0; i < bins.amounts.size() && total > 0.0; ++i) {
        const double lo = std::max<double>(bins.edges[i], min);
        const double hi = std::min<double>(bins.edges[i + 1], max);
        if (!(hi > lo))
            continue;
        const bool trace = bins.amounts[i] < TRACE_SHARE * total;
        if (trace && !m_pieces.empty() && m_pieces.back().empty)
            m_pieces.back().hi = hi;
        else
            m_pieces.push_back({lo, hi, 0.f, 0.f, trace, trace ? TRACE_WEIGHT : bins.amounts[i] < MINOR_SHARE * total ? MINOR_WEIGHT : 1.f});
    }
    if (m_pieces.empty()) {
        m_pieces.push_back({min, max, left, right});
        return;
    }
    float full = 0.f, sliver = 0.f;
    for (const LegendAxisPiece& p : m_pieces)
        (p.empty ? sliver : full) += p.weight;
    float unit       = (right - left) / (full + sliver);
    float empty_unit = unit;
    if (sliver > 0.f && unit > widest) {
        unit       = widest;
        empty_unit = (right - left - unit * full) / sliver;
    }
    float x = left;
    for (LegendAxisPiece& p : m_pieces) {
        p.x0 = x;
        x += (p.empty ? empty_unit : unit) * p.weight;
        p.x1 = x;
    }
    m_pieces.back().x1 = right;
}

float LegendAxis::t_of(double value) const
{
    double t = 0.0;
    if (m_logarithmic && m_min > 0.f && value > 0.0)
        t = (std::log(value) - std::log(m_min)) / (std::log(m_max) - std::log(m_min));
    else if (m_max > m_min)
        t = (value - m_min) / (m_max - m_min);
    return std::clamp(float(t), 0.f, 1.f);
}

double LegendAxis::value_at_t(float t) const
{
    if (m_logarithmic && m_min > 0.f)
        return std::exp(std::log(double(m_min)) + double(t) * (std::log(double(m_max)) - std::log(double(m_min))));
    return double(m_min) + double(t) * double(m_max - m_min);
}

float LegendAxis::x_on(double value) const
{
    for (const LegendAxisPiece& p : m_pieces)
        if (value <= p.hi || &p == &m_pieces.back()) {
            const float t0 = t_of(p.lo), t1 = t_of(p.hi);
            const float f  = t1 > t0 ? (t_of(std::max(value, p.lo)) - t0) / (t1 - t0) : 0.f;
            return p.x0 + std::clamp(f, 0.f, 1.f) * (p.x1 - p.x0);
        }
    return m_pieces.front().x0;
}

double LegendAxis::value_on(float x) const
{
    for (const LegendAxisPiece& p : m_pieces)
        if (x <= p.x1 || &p == &m_pieces.back()) {
            const float f  = p.x1 > p.x0 ? std::clamp((x - p.x0) / (p.x1 - p.x0), 0.f, 1.f) : 0.f;
            const float t0 = t_of(p.lo), t1 = t_of(p.hi);
            return value_at_t(t0 + f * (t1 - t0));
        }
    return m_min;
}

std::optional<size_t> LegendAxis::bin_at(float x) const
{
    for (size_t i = 0; i < m_bins.amounts.size(); ++i) {
        if (m_bins.amounts[i] <= 0.0)
            continue;
        const float x0 = x_on(std::max<double>(m_bins.edges[i], m_min));
        const float x1 = x_on(std::min<double>(m_bins.edges[i + 1], m_max));
        if (x >= x0 && x < x1)
            return i;
    }
    return std::nullopt;
}

std::array<float, 2> LegendAxis::bin_range(size_t bin) const
{
    return {float(std::max<double>(m_bins.edges[bin], m_min)), float(std::min<double>(m_bins.edges[bin + 1], m_max))};
}

std::array<float, 2> LegendAxis::brushed(float a, float b) const
{
    std::array<float, 2> range{float(value_on(std::min(a, b))), float(value_on(std::max(a, b)))};
    for (size_t i = 0; i < m_bins.amounts.size(); ++i) {
        const std::array<float, 2> edges = bin_range(i);
        if (range[0] >= edges[0] && range[0] < edges[1])
            range[0] = edges[0];
        if (range[1] > edges[0] && range[1] <= edges[1])
            range[1] = edges[1];
    }
    return {std::max(range[0], m_min), std::min(range[1], m_max)};
}

bool legend_range_whole(const std::array<float, 2>& range, float min, float max) { return range[0] <= min && range[1] >= max; }

LegendBrushStep legend_brush_step(LegendBrush& b, const LegendBrushInput& input, const LegendAxis& axis)
{
    const std::optional<std::array<float, 2>>& filter = input.filter;
    const std::array<float, 2>                 scale  = axis.scale_range();
    LegendBrushStep                            step;
    const auto set_filter = [&step, &scale](const std::array<float, 2>& range) {
        step.range_set = true;
        step.range     = legend_range_whole(range, scale[0], scale[1]) ? std::nullopt : std::optional<std::array<float, 2>>(range);
    };
    const auto click_bar = [&](float x) {
        const std::optional<size_t> picked = axis.bin_at(x);
        if (!picked)
            return;
        std::array<float, 2> range = axis.bin_range(*picked);
        if (input.shift && filter)
            range = {std::min(range[0], (*filter)[0]), std::max(range[1], (*filter)[1])};
        if (filter && !input.shift && *filter == range) {
            step.range_set = true;
            step.range     = std::nullopt;
        } else
            set_filter(range);
    };

    int edge = -1;
    if (filter) {
        const std::array<float, 2> edges_x{axis.x_on((*filter)[0]), axis.x_on((*filter)[1])};
        for (int i = 0; i < 2; ++i)
            if (input.hovered && std::abs(input.x - edges_x[size_t(i)]) <= input.edge_grab &&
                (edge < 0 || std::abs(input.x - edges_x[size_t(i)]) < std::abs(input.x - edges_x[size_t(edge)])))
                edge = i;
    }
    step.hovered_edge = edge;
    if (input.pressed && input.double_clicked && (edge >= 0 || b.range_before_edge_click) && (filter || b.range_before_edge_click)) {
        const std::array<float, 2> range = b.range_before_edge_click.value_or(*filter);
        if (b.range_before_edge_click)
            set_filter(range);
        b            = LegendBrush();
        step.popover = range;
    } else if (input.pressed) {
        b       = LegendBrush();
        b.phase = LegendBrush::Phase::Pressed;
        b.from  = input.x;
        b.edge  = edge;
    } else if (input.pressed_anywhere)
        b.range_before_edge_click.reset();

    if (b.phase == LegendBrush::Phase::Pressed) {
        // A press by an edge that moves drags the edge; one that does not is a click on the bar under it.
        if (input.held && std::abs(input.x - b.from) > input.drag_threshold)
            b.phase = b.edge >= 0 ? LegendBrush::Phase::EdgeDrag : LegendBrush::Phase::Brushing;
        else if (!input.held) {
            b.phase                   = LegendBrush::Phase::Idle;
            b.range_before_edge_click = b.edge >= 0 ? filter : std::nullopt;
            click_bar(b.from);
        }
    }
    if (b.phase == LegendBrush::Phase::Brushing) {
        if (input.held)
            set_filter(axis.brushed(b.from, std::clamp(input.x, input.left, input.right)));
        else
            b.phase = LegendBrush::Phase::Idle;
    } else if (b.phase == LegendBrush::Phase::EdgeDrag) {
        if (input.held) {
            std::array<float, 2> range = filter.value_or(scale);
            range[size_t(b.edge)]      = std::clamp(float(axis.value_on(input.x)), scale[0], scale[1]);
            if (range[0] > range[1]) {
                std::swap(range[0], range[1]);
                b.edge = 1 - b.edge;
            }
            set_filter(range);
        } else
            b.phase = LegendBrush::Phase::Idle;
    } else if (b.phase == LegendBrush::Phase::Idle && input.hovered)
        if (const std::optional<size_t> bin = axis.bin_at(input.x))
            step.hover = axis.bin_range(*bin);

    step.dragged_edge = b.phase == LegendBrush::Phase::EdgeDrag ? b.edge : -1;
    return step;
}

} // namespace GUI
} // namespace Slic3r
