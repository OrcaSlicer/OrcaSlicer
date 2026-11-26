#pragma once

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

// What the legend shows of a sliced result, in millimetres, grams and seconds; the panel formats it.
// The filaments are shared with the print summary, which shows how each was used.

// A line type, or a move, of the line type mode.
struct LegendRow
{
    // The role or the option it toggles.
    int             key{0};
    bool            move{false};
    CanvasUi::Color color;
    std::string     name;
    float           time{0.f};
    // Of the whole print's time.
    double          share{0.0};
    double          filament_mm{0.0};
    double          grams{0.0};
    // Moves: how far they go, when that is known, and how many there are.
    std::optional<double> distance_mm;
    unsigned long long    count{0};
    bool                  visible{true};
};

enum class LegendPart : unsigned char { Model, Support, Flushed, Tower, Count };

inline constexpr size_t LEGEND_PART_COUNT = size_t(LegendPart::Count);

struct LegendFilament
{
    // From 1, as the plater numbers filaments.
    int             number{1};
    CanvasUi::Color color;
    bool            translucent{false};
    std::string     type;
    std::string     name;
    std::array<double, LEGEND_PART_COUNT> mm{};
    std::array<double, LEGEND_PART_COUNT> grams{};

    double total_mm() const { return std::accumulate(mm.begin(), mm.end(), 0.0); }
    double total_grams() const { return std::accumulate(grams.begin(), grams.end(), 0.0); }
};

// One of a few values a scale mode takes.
struct LegendCategory
{
    float           value{0.f};
    // The values shown as this one at the decimals of the mode.
    float           lowest{0.f};
    float           highest{0.f};
    CanvasUi::Color color;
    // From 1.
    int             first_layer{1};
    int             last_layer{1};
    float           time{0.f};
    double          share{0.0};
};

struct LegendHistogram : LegendBins
{
    bool layers{false};
};

struct LegendScale
{
    std::vector<CanvasUi::Color> palette;
    float                        min{0.f};
    float                        max{0.f};
    bool                         logarithmic{false};
    int                          decimals{0};
    int                          category_decimals{0};
    // Translated, as "mm/s"; empty for a value without one.
    std::string                  unit;
    // The values are times of layers.
    bool                         times{false};
    LegendSpread                 spread{LegendSpread::Scale};
    // The values when there are few, from the smallest.
    std::vector<LegendCategory>  categories;
    LegendHistogram              histogram;
    // The value of the last move shown, while the sliders show less than the whole print.
    std::optional<float>         current;
    // The profile's limits on a scale of flow or of layer times, and how much of the print they hold back.
    std::vector<LegendLimit>     limits;
    std::optional<double>        at_limit_share;
    std::optional<unsigned int>  layers_under_limit;
    // The values the preview keeps in colour; the rest is not drawn.
    std::optional<std::array<float, 2>> filter;
};

double legend_filter_share(const LegendHistogram& histogram, const std::array<float, 2>& filter);

inline bool legend_range_filter_shown(bool collapsed, const LegendScale& scale)
{
    return !collapsed && !scale.palette.empty() && scale.spread == LegendSpread::Scale;
}

struct LegendValues
{
    std::vector<LegendCategory> values;
    LegendHistogram             histogram;
    // Print time at the filament's flow limit, layers shorter than the filament's slow-down time.
    double                      time_at_limit{0.0};
    unsigned int                layers_under_limit{0};
};

class LegendValueScan
{
public:
    LegendValueScan(const LegendScale& scale, bool layers, std::vector<float> limits);

    void         add(float value, double time, int layer, int filament);
    LegendValues values() const;

private:
    struct Value
    {
        float  value{0.f};
        float  lowest{std::numeric_limits<float>::max()};
        float  highest{std::numeric_limits<float>::lowest()};
        double time{0.0};
        int    first{0};
        int    last{0};
    };

    int                          m_decimals{0};
    std::vector<float>           m_limits;
    LegendValues                 m_values;
    std::map<std::string, Value> m_by_key;
};

void legend_set_values(LegendScale& scale, const LegendValues& values);

std::optional<std::array<float, 2>> legend_focus_positions(const std::optional<std::array<float, 2>>& values,
                                                           const std::optional<std::array<float, 2>>& span,
                                                           const std::optional<std::array<float, 2>>& filter,
                                                           const std::array<float, 2>&                 bounds,
                                                           const std::function<float(float)>&          position);

// What the menu shows of each mode.
struct LegendModeSummary
{
    // The values in this result, as "10 – 200 mm/s".
    std::string                  range;
    bool                         constant{false};
};

struct LegendTool
{
    int             number{1};
    CanvasUi::Color color;
    double          mm{0.0};
    double          grams{0.0};
};

struct LegendModel
{
    LegendModeChoice                                      choice;
    // The tool view, which no mode of the menu offers.
    bool                                                  tool_view{false};
    std::array<LegendModeSummary, LEGEND_MODE_COUNT>      modes;
    std::vector<LegendRow>                                rows;
    // Line types and moves hidden, against the moves hidden by default.
    size_t                                                hidden{0};
    bool                                                  travel_shown{false};
    LegendScale                                           scale;
    std::vector<LegendFilament>                           filaments;
    std::vector<LegendTool>                               tools;
    // The filament the preview keeps in colour; the rest is not drawn; 0 for none.
    int                                                   isolated{0};
    bool                                                  imperial{false};
};

std::string legend_statistics_text(const std::vector<LegendRow>& rows, bool imperial, const LegendUnits& units);

} // namespace GUI
} // namespace Slic3r
