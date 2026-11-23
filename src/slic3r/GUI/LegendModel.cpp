#include "LegendModel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "LegendText.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

LegendValueScan::LegendValueScan(const LegendScale& scale, bool layers, std::vector<float> limits)
    : m_decimals(scale.decimals), m_limits(std::move(limits))
{
    LegendHistogram& histogram = m_values.histogram;
    histogram.edges  = legend_bin_edges(scale.min, scale.max, scale.logarithmic ? LEGEND_HISTOGRAM_BINS : LEGEND_HISTOGRAM_FINE_BINS, scale.logarithmic);
    histogram.amounts.assign(histogram.edges.empty() ? 0 : histogram.edges.size() - 1, 0.0);
    histogram.lowest.assign(histogram.amounts.size(), std::numeric_limits<float>::max());
    histogram.highest.assign(histogram.amounts.size(), std::numeric_limits<float>::lowest());
    histogram.layers = layers;
}

void LegendValueScan::add(float value, double time, int layer, int filament)
{
    LegendHistogram& histogram = m_values.histogram;
    if (filament >= 0 && size_t(filament) < m_limits.size()) {
        if (histogram.layers) {
            if (legend_under_limit(value, m_limits[size_t(filament)]))
                ++m_values.layers_under_limit;
        } else if (legend_at_limit(value, m_limits[size_t(filament)]))
            m_values.time_at_limit += time;
    }
    if (!histogram.amounts.empty()) {
        const size_t bin = legend_bin_of(histogram.edges, value);
        histogram.amounts[bin] += histogram.layers ? 1.0 : time;
        histogram.lowest[bin]  = std::min(histogram.lowest[bin], value);
        histogram.highest[bin] = std::max(histogram.highest[bin], value);
    }
    const std::string key = legend_value_key(value, m_decimals);
    if (m_by_key.size() > LEGEND_FEW_VALUES && m_by_key.find(key) == m_by_key.end())
        return;
    Value& v = m_by_key[key];
    if (v.time == 0.0 && v.first == 0) {
        v.value = value;
        v.first = layer;
    }
    v.lowest  = std::min(v.lowest, value);
    v.highest = std::max(v.highest, value);
    v.time += time;
    v.first = std::min(v.first, layer);
    v.last  = std::max(v.last, layer);
}

LegendValues LegendValueScan::values() const
{
    LegendValues values = m_values;
    for (const auto& [key, v] : m_by_key)
        values.values.push_back({v.value, v.lowest, v.highest, CanvasUi::Color(), v.first, v.last, float(v.time), 0.0});
    std::sort(values.values.begin(), values.values.end(), [](const LegendCategory& a, const LegendCategory& b) { return a.value < b.value; });
    return values;
}

void legend_set_values(LegendScale& scale, const LegendValues& values)
{
    scale.histogram = values.histogram;
    if (!scale.limits.empty()) {
        if (scale.histogram.layers)
            scale.layers_under_limit = values.layers_under_limit;
        else {
            const double total = std::accumulate(scale.histogram.amounts.begin(), scale.histogram.amounts.end(), 0.0);
            scale.at_limit_share = total > 0.0 ? values.time_at_limit / total : 0.0;
        }
    }
    scale.spread = legend_spread(values.values.size());
    if (scale.spread == LegendSpread::Few) {
        std::vector<float> category_values;
        for (const LegendCategory& c : values.values)
            category_values.push_back(c.value);
        if (const std::optional<int> decimals = legend_category_decimals(category_values, scale.decimals, scale.times))
            scale.category_decimals = *decimals;
        else
            scale.spread = LegendSpread::Scale;
    }
    if (scale.spread == LegendSpread::Scale)
        return;
    double total = 0.0;
    for (const LegendCategory& c : values.values)
        total += c.time;
    for (LegendCategory c : values.values) {
        c.share = total > 0.0 ? c.time / total : 0.0;
        scale.categories.push_back(c);
    }
}

double legend_filter_share(const LegendHistogram& histogram, const std::array<float, 2>& filter)
{
    double in = 0.0, total = 0.0;
    for (size_t i = 0; i < histogram.amounts.size(); ++i) {
        total += histogram.amounts[i];
        const double lo = histogram.edges[i], hi = histogram.edges[i + 1];
        const double overlap = std::max(0.0, std::min(hi, double(filter[1])) - std::max(lo, double(filter[0])));
        in += hi > lo ? histogram.amounts[i] * overlap / (hi - lo) : 0.0;
    }
    return total > 0.0 ? in / total : 0.0;
}

std::optional<std::array<float, 2>> legend_focus_positions(const std::optional<std::array<float, 2>>& values,
                                                           const std::optional<std::array<float, 2>>& span,
                                                           const std::optional<std::array<float, 2>>& filter,
                                                           const std::array<float, 2>&                 bounds,
                                                           const std::function<float(float)>&          position)
{
    // A few values each colour their paths, so a narrow interval around them picks out their paths alone.
    if (values)
        return std::array<float, 2>{std::max(0.f, position((*values)[0]) - 1e-3f), std::min(1.f, position((*values)[1]) + 1e-3f)};
    if (span)
        return std::array<float, 2>{position((*span)[0]), position((*span)[1])};
    if (filter) {
        const float lo = std::max((*filter)[0], bounds[0]);
        const float hi = std::min((*filter)[1], bounds[1]);
        return lo <= hi ? std::array<float, 2>{position(lo), position(hi)} : std::array<float, 2>{1.f, 0.f};
    }
    return std::nullopt;
}

std::string legend_statistics_text(const std::vector<LegendRow>& rows, bool imperial, const LegendUnits& units)
{
    std::string text;
    for (const LegendRow& row : rows) {
        text += row.name + "\t" + (row.time > 0.f ? legend_time(row.time, units) : std::string()) + "\t" + legend_percent(row.share) + "\t";
        text += row.move ? std::to_string(row.count) : legend_mass(row.grams, imperial, units);
        text += "\n";
    }
    return text;
}

} // namespace GUI
} // namespace Slic3r
