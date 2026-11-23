#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "libvgcode/include/Types.hpp"

namespace Slic3r {
class AppConfig;
namespace GUI {

// The modes the legend offers, in the order of its menu.
enum class LegendMode : unsigned char {
    LineType,
    Filament,
    Speed,
    Acceleration,
    Jerk,
    Flow,
    LineWidth,
    LayerHeight,
    PressureAdvance,
    FanSpeed,
    Temperature,
    LayerTime,
    Count
};

inline constexpr size_t LEGEND_MODE_COUNT = size_t(LegendMode::Count);

enum class LegendModeGroup : unsigned char { Categories, Motion, Extrusion, CoolingAndHeat, Time };

LegendModeGroup legend_mode_group(LegendMode mode);
// Speed and flow come planned or actual, layer time on a linear or a logarithmic scale.
bool legend_mode_has_variants(LegendMode mode);
libvgcode::EViewType legend_view_type(LegendMode mode, int variant);

struct LegendModeChoice
{
    LegendMode mode{LegendMode::LineType};
    int        variant{0};
};

// The mode and variant that show a view type; none for the tool view, which no mode offers.
std::optional<LegendModeChoice> legend_mode_of(libvgcode::EViewType type);

// A saved Summary, which the legend does not offer, opens as Filament.
libvgcode::EViewType legend_opened_view_type(libvgcode::EViewType saved);

// Up to max_count values of the steps 1, 2 or 5 times a power of ten, inside [min, max].
std::vector<double> legend_ticks(double min, double max, size_t max_count);
// The values 1, 2 and 5 times a power of ten inside [min, max], thinned to max_count.
std::vector<double> legend_log_ticks(double min, double max, size_t max_count);
// The decimals the labels of ticks need to tell them apart.
int legend_ticks_decimals(const std::vector<double>& ticks);

inline constexpr size_t LEGEND_HISTOGRAM_BINS      = 40;
inline constexpr size_t LEGEND_HISTOGRAM_FINE_BINS = 400;

// The edges of a histogram's bins along a scale: round values from at or below min to at or above max, at most max_count bins;
// on a logarithmic scale the values 1, 2 and 5, or 1 alone, times a power of ten.
std::vector<double> legend_bin_edges(double min, double max, size_t max_count, bool logarithmic);
// The bin of edges a value falls in, clamped to the first and the last.
size_t legend_bin_of(const std::vector<double>& edges, double value);

// Print time, or layers for a scale of layer times, in each bin between round edges of a scale, with the least and the greatest
// value met in each.
struct LegendBins
{
    std::vector<double> edges;
    std::vector<double> amounts;
    std::vector<float>  lowest;
    std::vector<float>  highest;
};

// Bins of equal width merged into bins 1, 2 or 5 times a power of ten wider, so that their edges stay round: the narrowest that
// leave at most max_filled bins with at least min_share of the total amount.
LegendBins legend_merge_bins(const LegendBins& bins, size_t max_filled, double min_share = 0.0);

// A limit of the profile drawn on a scale: its value and the filaments, from 1, that have it.
struct LegendLimit
{
    float            value{0.f};
    std::vector<int> filaments;
};

// One line per distinct limit among the filaments used, from the lowest; a filament without a limit has none.
std::vector<LegendLimit> legend_limits(const std::vector<float>& per_filament, const std::vector<int>& used);

// A move runs at a limit of the profile when its value reaches it, short of rounding in the G-code.
bool legend_at_limit(double value, double limit);
// A layer was slowed down for cooling when it took less than the limit.
bool legend_under_limit(double value, double limit);

enum class LegendRange : unsigned char { WholePrint, VisibleLayers };
enum class LegendPalette : unsigned char { Classic, Perceptual };

// The legend's settings, kept in the app config under legend_* keys.
struct LegendSettings
{
    // The columns of the line types; at least one.
    bool          time{true};
    bool          share{true};
    bool          weight{true};
    bool          length{false};
    bool          histogram{true};
    bool          hover{true};
    LegendRange   range{LegendRange::WholePrint};
    LegendPalette palette{LegendPalette::Classic};
};

// How many settings differ from their defaults.
size_t legend_changed_settings(const LegendSettings& settings);
// The columns as the app config keeps them, as "time,share,weight".
std::string legend_columns_text(const LegendSettings& settings);
// Sets the columns from that text; a text naming none leaves them as they are.
void legend_read_columns(LegendSettings& settings, const std::string& text);

inline constexpr size_t LEGEND_FEW_VALUES        = 4;

enum class LegendSpread : unsigned char { One, Few, Scale };

LegendSpread legend_spread(size_t distinct_values);

// Translated templates of units, each with its numbers as %1% and %2%.
struct LegendUnits
{
    std::string days_hours{"%1%d%2%h"};
    std::string hours_minutes{"%1%h%2%m"};
    std::string minutes_seconds{"%1%m%2%s"};
    std::string seconds{"%1%s"};
    std::string under_second{"<1s"};
    std::string long_days_hours{"%1%d %2%h"};
    std::string long_hours_minutes{"%1%h %2%m"};
    std::string long_minutes_seconds{"%1%m %2%s"};
    std::string long_seconds{"%1%s"};
    std::string grams{"%1% g"};
    std::string kilograms{"%1% kg"};
    std::string ounces{"%1% oz"};
    std::string meters{"%1% m"};
    std::string millimeters{"%1% mm"};
    std::string inches{"%1% in"};
};

// A time in columns: two units, the larger first, rounded to whole minutes from an hour on.
std::string legend_time(float seconds, const LegendUnits& units);
// The same time in the longer form of the print summary's totals.
std::string legend_long_time(float seconds, const LegendUnits& units);
// A whole count with a no-break space between groups of thousands.
std::string legend_count(unsigned long long count);
// A share of a total in percent without the sign: one decimal, "<0.1" for a share too small to show.
std::string legend_percent(double share);
std::string legend_mass(double grams, bool imperial, const LegendUnits& units);
// A mass without its unit, for a column headed g or oz.
std::string legend_mass_value(double grams, bool imperial);
// A length of filament: metres, or inches.
std::string legend_filament_length(double mm, bool imperial, const LegendUnits& units);
// A length of filament without its unit, for a column headed m or in.
std::string legend_length_value(double mm, bool imperial);
// A distance moved: millimetres under a metre, then metres, or inches.
std::string legend_distance(double mm, bool imperial, const LegendUnits& units);
std::string legend_value(double value, int decimals);
int         legend_value_decimals(LegendMode mode);

inline constexpr int LEGEND_EXTRA_DECIMALS = 2;

std::string        legend_value_key(double value, int decimals);
std::optional<int> legend_category_decimals(const std::vector<float>& values, int decimals, bool times);

// The legend's settings, kept in the app config; a value missing or not understood reads as the default.
LegendSettings legend_load_settings(const AppConfig& config);
void           legend_save_settings(AppConfig& config, const LegendSettings& settings);

} // namespace GUI
} // namespace Slic3r
