#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "LegendModel.hpp"
#include "LegendText.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
struct PrintEstimatedStatistics;
struct PrintStatistics;
struct StatisticsByExtruderCount;
namespace CustomGCode {
struct Item;
}

namespace GUI {

// What the print summary shows of a sliced result, in millimetres, grams and seconds; the panel formats it.

// The filaments a nozzle of a two-nozzle printer prints.
struct SummaryNozzle
{
    std::string      title;
    std::vector<int> numbers;
    double           grams{0.0};
};

struct SummaryGroupedFilament
{
    int             number{1};
    CanvasUi::Color color;
    std::string     type;
};

// How the filaments are spread over the two nozzles, and what grouping them otherwise would save.
struct SummaryGrouping
{
    std::vector<SummaryGroupedFilament> left;
    std::vector<SummaryGroupedFilament> right;
    bool                               optimal{true};
    // How this grouping compares with the optimal one, or with one nozzle; empty when it does not differ.
    std::string                        comparison;
};

struct SummaryEvent
{
    enum class Kind : unsigned char { Pause, ColorChange, ToolChange, Template, Custom, Unknown };

    Kind            kind{Kind::Unknown};
    // From 1.
    int             layer{1};
    // From the start of the print to the layer.
    float           time{0.f};
    int             extruder{1};
    CanvasUi::Color before;
    CanvasUi::Color after;
};

struct SummaryProfiles
{
    std::string                              printer;
    std::string                              print;
    std::vector<std::pair<int, std::string>> filaments;
};

struct SummaryTotals
{
    float  time{0.f};
    float  prepare{0.f};
    double mm{0.0};
    double grams{0.0};
    double model_grams{0.0};
    double cost{0.0};
    // The normal and the stealth mode take different times.
    bool   modes_differ{false};
    bool   stealth{false};
};

struct PrintSummaryModel
{
    SummaryTotals                  totals;
    std::vector<LegendFilament>    filaments;
    unsigned int                   filament_changes{0};
    unsigned int                   tool_changes{0};
    std::vector<SummaryNozzle>     nozzles;
    std::optional<SummaryGrouping> grouping;
    std::vector<SummaryEvent>      events;
    std::optional<SummaryProfiles> profiles;
    // The filament the preview keeps in colour; the rest is not drawn; 0 for none.
    int                            isolated{0};
    bool                           imperial{false};
};

// The length and the mass of filament the volumes of its parts take, at its diameter and density.
std::pair<std::array<double, LEGEND_PART_COUNT>, std::array<double, LEGEND_PART_COUNT>> filament_parts(const std::array<double, LEGEND_PART_COUNT>& volumes,
                                                                                                       float diameter, float density);
void add_filament_volumes(std::array<double, LEGEND_PART_COUNT>& volumes, const PrintEstimatedStatistics& statistics, size_t filament);

SummaryTotals summary_totals(const std::vector<LegendFilament>& filaments, const PrintStatistics& plater, const PrintEstimatedStatistics& estimate,
                             size_t time_mode, const LegendUnits& units);

// Find an index of a value in a sorted vector, which is in <z-eps, z+eps>.
// Returns -1 if there is no such member.
int find_close_layer_idx(const std::vector<double>& zs, double z, double eps);

std::vector<SummaryEvent> summary_events(const std::vector<CustomGCode::Item>& items, const std::vector<double>& layer_zs,
                                         const std::vector<float>& layer_times, std::vector<CanvasUi::Color> colors,
                                         const std::function<CanvasUi::Color(const std::string&)>& shown_color);

// Rounds a difference of filament for the grouping block: past 1000 only its first three digits are told.
int grouping_number(float num);

void summary_compare_grouping(SummaryGrouping& grouping, const StatisticsByExtruderCount& stats);

} // namespace GUI
} // namespace Slic3r
