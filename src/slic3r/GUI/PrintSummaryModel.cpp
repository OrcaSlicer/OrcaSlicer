#include "PrintSummaryModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

std::pair<std::array<double, LEGEND_PART_COUNT>, std::array<double, LEGEND_PART_COUNT>> filament_parts(const std::array<double, LEGEND_PART_COUNT>& volumes,
                                                                                                       float diameter, float density)
{
    const double area = PI * std::pow(0.5 * double(diameter), 2);
    std::pair<std::array<double, LEGEND_PART_COUNT>, std::array<double, LEGEND_PART_COUNT>> parts{};
    for (size_t part = 0; part < LEGEND_PART_COUNT; ++part) {
        parts.first[part]  = area > 0.0 ? volumes[part] / area : 0.0;
        parts.second[part] = volumes[part] * double(density) * 0.001;
    }
    return parts;
}

void add_filament_volumes(std::array<double, LEGEND_PART_COUNT>& volumes, const PrintEstimatedStatistics& statistics, size_t filament)
{
    const auto volume = [filament](const std::map<size_t, double>& from) {
        const auto it = from.find(filament);
        return it == from.end() ? 0.0 : it->second;
    };
    volumes[size_t(LegendPart::Model)] += volume(statistics.model_volumes_per_extruder);
    volumes[size_t(LegendPart::Support)] += volume(statistics.support_volumes_per_extruder);
    volumes[size_t(LegendPart::Flushed)] += volume(statistics.flush_per_filament);
    volumes[size_t(LegendPart::Tower)] += volume(statistics.wipe_tower_volumes_per_extruder);
}

SummaryTotals summary_totals(const std::vector<LegendFilament>& filaments, const PrintStatistics& plater, const PrintEstimatedStatistics& estimate,
                             size_t time_mode, const LegendUnits& units)
{
    SummaryTotals totals;
    totals.time    = float(estimate.modes[time_mode].time);
    totals.prepare = estimate.modes[time_mode].prepare_time;
    double excluded = 0.0, grams = 0.0, mm = 0.0;
    for (const LegendFilament& filament : filaments) {
        excluded += filament.grams[size_t(LegendPart::Support)] + filament.grams[size_t(LegendPart::Flushed)] + filament.grams[size_t(LegendPart::Tower)];
        grams += filament.total_grams();
        mm += filament.total_mm();
    }
    // A G-code file opened on its own leaves the plater's statistics empty; its filaments still tell the totals.
    const bool from_plater = plater.total_weight > 0.0;
    totals.mm              = from_plater ? plater.total_used_filament : mm;
    totals.grams           = from_plater ? plater.total_weight : grams;
    totals.model_grams     = totals.grams - excluded;
    totals.cost            = plater.total_cost;
    std::vector<std::string> times;
    for (const PrintEstimatedStatistics::Mode& m : estimate.modes)
        if (m.time > 0.0) {
            const std::string text = legend_long_time(float(m.time), units);
            if (std::find(times.begin(), times.end(), text) == times.end())
                times.push_back(text);
        }
    totals.modes_differ = times.size() > 1;
    return totals;
}

int find_close_layer_idx(const std::vector<double>& zs, double z, double eps)
{
    if (zs.empty())
        return -1;
    auto it_h = std::lower_bound(zs.begin(), zs.end(), z);
    if (it_h == zs.end()) {
        auto it_l = it_h;
        --it_l;
        if (z - *it_l < eps)
            return int(zs.size() - 1);
    } else if (it_h == zs.begin()) {
        if (*it_h - z < eps)
            return 0;
    } else {
        auto it_l = it_h;
        --it_l;
        double dist_l = z - *it_l;
        double dist_h = *it_h - z;
        if (std::min(dist_l, dist_h) < eps)
            return (dist_l < dist_h) ? int(it_l - zs.begin()) : int(it_h - zs.begin());
    }
    return -1;
}

std::vector<SummaryEvent> summary_events(const std::vector<CustomGCode::Item>& items, const std::vector<double>& layer_zs,
                                         const std::vector<float>& layer_times, std::vector<CanvasUi::Color> colors,
                                         const std::function<CanvasUi::Color(const std::string&)>& shown_color)
{
    std::vector<SummaryEvent> events;
    for (const CustomGCode::Item& item : items) {
        SummaryEvent event;
        switch (item.type) {
        case CustomGCode::PausePrint: event.kind = SummaryEvent::Kind::Pause; break;
        case CustomGCode::ColorChange: event.kind = SummaryEvent::Kind::ColorChange; break;
        case CustomGCode::ToolChange: event.kind = SummaryEvent::Kind::ToolChange; break;
        case CustomGCode::Template: event.kind = SummaryEvent::Kind::Template; break;
        case CustomGCode::Custom: event.kind = SummaryEvent::Kind::Custom; break;
        default: event.kind = SummaryEvent::Kind::Unknown; break;
        }
        const int layer = find_close_layer_idx(layer_zs, item.print_z, EPSILON);
        event.layer     = layer + 1;
        for (int i = 0; i < layer && size_t(i) < layer_times.size(); ++i)
            event.time += layer_times[i];
        event.extruder = item.extruder;
        if (event.kind == SummaryEvent::Kind::ColorChange && item.extruder >= 1 && size_t(item.extruder) <= colors.size()) {
            event.before                      = colors[size_t(item.extruder) - 1];
            event.after                       = shown_color(item.color);
            colors[size_t(item.extruder) - 1] = event.after;
        }
        events.push_back(event);
    }
    return events;
}

int grouping_number(float num)
{
    if (num > 1000) {
        const std::string number_str = std::to_string(num);
        return std::stoi(number_str.substr(0, 3));
    }
    return static_cast<int>(num);
}

void summary_compare_grouping(SummaryGrouping& grouping, const StatisticsByExtruderCount& stats)
{
    const float delta_weight_to_single_ext = stats.stats_by_single_extruder.filament_flush_weight - stats.stats_by_multi_extruder_curr.filament_flush_weight;
    const float delta_weight_to_best = stats.stats_by_multi_extruder_curr.filament_flush_weight - stats.stats_by_multi_extruder_best.filament_flush_weight;
    // The displayed "hand changes" delta uses the per-nozzle flush_filament_change_count.
    const int delta_change_to_single_ext = stats.stats_by_single_extruder.flush_filament_change_count - stats.stats_by_multi_extruder_curr.flush_filament_change_count;
    const int delta_change_to_best = stats.stats_by_multi_extruder_curr.flush_filament_change_count - stats.stats_by_multi_extruder_best.flush_filament_change_count;
    const bool any_less_to_single_ext = delta_weight_to_single_ext > EPSILON || delta_change_to_single_ext > 0;
    const bool any_more_to_best       = delta_weight_to_best > EPSILON || delta_change_to_best > 0;
    wxString tip;
    if (any_more_to_best) {
        grouping.optimal = false;
        if (delta_weight_to_best >= 0 && delta_change_to_best >= 0)
            tip = format_wxstr(_L_PLURAL("Increase %1%g filament and %2% change compared to optimal grouping.",
                                         "Increase %1%g filament and %2% changes compared to optimal grouping.", delta_change_to_best),
                               grouping_number(delta_weight_to_best), delta_change_to_best);
        else if (delta_weight_to_best >= 0 && delta_change_to_best < 0)
            tip = format_wxstr(_L_PLURAL("Increase %1%g filament and save %2% change compared to optimal grouping.",
                                         "Increase %1%g filament and save %2% changes compared to optimal grouping.", std::abs(delta_change_to_best)),
                               grouping_number(delta_weight_to_best), std::abs(delta_change_to_best));
        else if (delta_weight_to_best < 0 && delta_change_to_best >= 0)
            tip = format_wxstr(_L_PLURAL("Save %1%g filament and increase %2% change compared to optimal grouping.",
                                         "Save %1%g filament and increase %2% changes compared to optimal grouping.", delta_change_to_best),
                               grouping_number(std::abs(delta_weight_to_best)), delta_change_to_best);
    } else if (any_less_to_single_ext) {
        if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext >= 0)
            tip = format_wxstr(_L_PLURAL("Save %1%g filament and %2% change compared to a printer with one nozzle.",
                                         "Save %1%g filament and %2% changes compared to a printer with one nozzle.", delta_change_to_single_ext),
                               grouping_number(delta_weight_to_single_ext), delta_change_to_single_ext);
        else if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext < 0)
            tip = format_wxstr(_L_PLURAL("Save %1%g filament and increase %2% change compared to a printer with one nozzle.",
                                         "Save %1%g filament and increase %2% changes compared to a printer with one nozzle.", std::abs(delta_change_to_single_ext)),
                               grouping_number(delta_weight_to_single_ext), std::abs(delta_change_to_single_ext));
        else if (delta_weight_to_single_ext < 0 && delta_change_to_single_ext >= 0)
            tip = format_wxstr(_L_PLURAL("Increase %1%g filament and save %2% change compared to a printer with one nozzle.",
                                         "Increase %1%g filament and save %2% changes compared to a printer with one nozzle.", delta_change_to_single_ext),
                               grouping_number(std::abs(delta_weight_to_single_ext)), delta_change_to_single_ext);
    }
    grouping.comparison = into_u8(tip);
}

} // namespace GUI
} // namespace Slic3r
