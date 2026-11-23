#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendScaleView.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/CanvasUi/Alert.hpp"
#include "libvgcode/include/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;
using libvgcode::EViewType;

namespace {

bool is_round(double value)
{
    if (value == 0.0)
        return true;
    const double power   = std::pow(10.0, std::floor(std::log10(std::abs(value))));
    const double leading = std::abs(value) / power;
    for (const double step : {1.0, 2.0, 5.0})
        if (std::abs(std::fmod(leading, step)) < 1e-9 || std::abs(std::fmod(leading, step) - step) < 1e-9)
            return true;
    return false;
}

// Every tick a whole number of one step that is itself 1, 2 or 5 times a power of ten.
void check_round_steps(const std::vector<double> &ticks)
{
    REQUIRE(ticks.size() >= 2);
    const double step = ticks[1] - ticks[0];
    CHECK(is_round(step));
    for (const double tick : ticks)
        CHECK_THAT(tick / step - std::round(tick / step), WithinAbs(0.0, 1e-6));
}

const std::string NBSP = "\xC2\xA0";

} // namespace

TEST_CASE("Every view type a menu offered maps to one mode and back", "[LegendText]")
{
    std::set<EViewType> offered;
    for (size_t i = 0; i < LEGEND_MODE_COUNT; ++i) {
        const LegendMode mode = LegendMode(i);
        for (int variant = 0; variant < (legend_mode_has_variants(mode) ? 2 : 1); ++variant) {
            const EViewType type = legend_view_type(mode, variant);
            CHECK(offered.insert(type).second);
            const std::optional<LegendModeChoice> back = legend_mode_of(type);
            REQUIRE(back.has_value());
            CHECK(back->mode == mode);
            CHECK(back->variant == variant);
        }
    }
    CHECK(offered.count(EViewType::Summary) == 0);
    CHECK(offered.count(EViewType::Tool) == 0);
}

TEST_CASE("The planned and actual speeds are one mode", "[LegendText]")
{
    CHECK(legend_view_type(LegendMode::Speed, 0) == EViewType::Speed);
    CHECK(legend_view_type(LegendMode::Speed, 1) == EViewType::ActualSpeed);
    CHECK(legend_view_type(LegendMode::Flow, 1) == EViewType::ActualVolumetricFlowRate);
    CHECK(legend_view_type(LegendMode::LayerTime, 1) == EViewType::LayerTimeLogarithmic);
    CHECK_FALSE(legend_mode_has_variants(LegendMode::Jerk));
}

TEST_CASE("A saved summary opens as the filament view, other views as they are", "[LegendText]")
{
    CHECK(legend_opened_view_type(EViewType::Summary) == EViewType::ColorPrint);
    const EViewType other = GENERATE(EViewType::FeatureType, EViewType::ColorPrint, EViewType::ActualSpeed, EViewType::PressureAdvance);
    CHECK(legend_opened_view_type(other) == other);
}

TEST_CASE("The legend's settings read back as saved, and a value not understood reads as the default", "[LegendText]")
{
    LegendSettings changed;
    changed.time      = false;
    changed.weight    = false;
    changed.histogram = false;
    changed.hover     = false;
    changed.range     = LegendRange::VisibleLayers;
    changed.palette   = LegendPalette::Perceptual;
    Slic3r::AppConfig config;
    legend_save_settings(config, changed);
    const LegendSettings read = legend_load_settings(config);
    CHECK_FALSE(read.time);
    CHECK(read.share);
    CHECK_FALSE(read.weight);
    CHECK_FALSE(read.histogram);
    CHECK_FALSE(read.hover);
    CHECK(read.range == LegendRange::VisibleLayers);
    CHECK(read.palette == LegendPalette::Perceptual);

    Slic3r::AppConfig odd;
    odd.set("legend_histogram", "no");
    odd.set("legend_hover", "off");
    odd.set("legend_range", "some");
    odd.set("legend_palette", "warm");
    const LegendSettings fallen_back = legend_load_settings(odd);
    CHECK(fallen_back.histogram);
    CHECK(fallen_back.hover);
    CHECK(fallen_back.range == LegendRange::WholePrint);
    CHECK(fallen_back.palette == LegendPalette::Classic);
}

TEST_CASE("The tool view belongs to no mode", "[LegendText]") { CHECK_FALSE(legend_mode_of(EViewType::Tool).has_value()); }

TEST_CASE("Ticks of a scale are round numbers inside its range", "[LegendText]")
{
    const std::vector<double> ticks = legend_ticks(10.0, 200.0, 6);
    REQUIRE(ticks.size() >= 3);
    CHECK(ticks.size() <= 6);
    check_round_steps(ticks);
    for (const double tick : ticks) {
        CHECK(tick >= 10.0);
        CHECK(tick <= 200.0);
    }
    CHECK_THAT(ticks.back(), WithinAbs(200.0, 1e-9));
    CHECK(legend_ticks_decimals(ticks) == 0);
}

TEST_CASE("Ticks of a narrow scale keep the decimals that tell them apart", "[LegendText]")
{
    const std::vector<double> ticks = legend_ticks(0.012, 0.047, 5);
    CHECK(ticks.size() <= 5);
    check_round_steps(ticks);
    CHECK(legend_ticks_decimals(ticks) == 2);
}

TEST_CASE("A scale of one value has no ticks", "[LegendText]") { CHECK(legend_ticks(9.0, 9.0, 6).empty()); }

TEST_CASE("Logarithmic ticks are 1, 2 and 5 times powers of ten", "[LegendText]")
{
    const std::vector<double> ticks = legend_log_ticks(3.0, 400.0, 8);
    REQUIRE_FALSE(ticks.empty());
    CHECK(ticks.size() <= 8);
    for (const double tick : ticks) {
        CHECK(is_round(tick));
        CHECK(tick >= 3.0);
        CHECK(tick <= 400.0);
    }
    const std::vector<double> few = legend_log_ticks(1.0, 1e6, 4);
    CHECK(few.size() <= 4);
}

TEST_CASE("One value, up to four, then a scale", "[LegendText]")
{
    CHECK(legend_spread(1) == LegendSpread::One);
    CHECK(legend_spread(2) == LegendSpread::Few);
    CHECK(legend_spread(4) == LegendSpread::Few);
    CHECK(legend_spread(5) == LegendSpread::Scale);
}

TEST_CASE("Times come in the translated units", "[LegendText]")
{
    const LegendUnits english;
    CHECK(legend_time(25 * 60 + 56, english) == "25m56s");
    CHECK(legend_time(3600 + 20 * 60 + 40, english) == "1h21m");
    CHECK(legend_time(2 * 86400 + 3 * 3600, english) == "2d3h");
    CHECK(legend_time(42, english) == "42s");
    CHECK(legend_time(0.4f, english) == "<1s");
    CHECK(legend_long_time(3600 + 27 * 60, english) == "1h 27m");

    LegendUnits russian;
    russian.hours_minutes   = "%1%ч%2%м";
    russian.minutes_seconds = "%1%м%2%с";
    CHECK(legend_time(25 * 60 + 56, russian) == "25м56с");
    CHECK(legend_time(4 * 3600 + 12 * 60, russian) == "4ч12м");
}

TEST_CASE("Counts are whole with a no-break space between thousands", "[LegendText]")
{
    CHECK(legend_count(7) == "7");
    CHECK(legend_count(17012) == "17" + NBSP + "012");
    CHECK(legend_count(1234567) == "1" + NBSP + "234" + NBSP + "567");
}

TEST_CASE("Shares have one decimal and say when they are too small", "[LegendText]")
{
    CHECK(legend_percent(0.0) == "0");
    CHECK(legend_percent(0.0001) == "<0.1");
    CHECK(legend_percent(0.094) == "9.4");
}

TEST_CASE("Masses and lengths follow the unit system once", "[LegendText]")
{
    const LegendUnits units;
    CHECK(legend_mass(9.28, false, units) == "9.28 g");
    CHECK(legend_mass(1532.0, false, units) == "1.53 kg");
    CHECK(legend_mass(28.349523125, true, units) == "1.00 oz");
    CHECK(legend_filament_length(3110.0, false, units) == "3.11 m");
    CHECK(legend_filament_length(254.0, true, units) == "10.00 in");
    CHECK(legend_length_value(3110.0, false) == "3.11");
    CHECK(legend_length_value(254.0, true) == "10.00");
    CHECK(legend_distance(512.0, false, units) == "512 mm");
    CHECK(legend_distance(12340.0, false, units) == "12.34 m");
}

TEST_CASE("A value of a scale takes its unit unless it is a time", "[LegendText]")
{
    LegendScale scale;
    scale.unit = "mm/s";
    CHECK(legend_with_unit(scale, legend_scale_value(scale, 12.345, 1, LegendUnits())) == "12.3 mm/s");
    scale.unit.clear();
    CHECK(legend_with_unit(scale, legend_scale_value(scale, 12.345, 1, LegendUnits())) == "12.3");
    scale.unit  = "s";
    scale.times = true;
    CHECK(legend_with_unit(scale, legend_scale_value(scale, 75.0, 1, LegendUnits())) == legend_time(75.f, LegendUnits()));
}

TEST_CASE("Values keep the decimals of their mode", "[LegendText]")
{
    CHECK(legend_value(9.0, legend_value_decimals(LegendMode::Jerk)) == "9.0");
    CHECK(legend_value(0.02, legend_value_decimals(LegendMode::PressureAdvance)) == "0.020");
    CHECK(legend_value(220.0, legend_value_decimals(LegendMode::Temperature)) == "220");
}

TEST_CASE("Histogram bins cover the scale on round edges", "[LegendText]")
{
    const std::vector<double> edges = legend_bin_edges(7.0, 214.0, LEGEND_HISTOGRAM_BINS, false);
    REQUIRE(edges.size() >= 2);
    CHECK(edges.size() - 1 <= LEGEND_HISTOGRAM_BINS);
    CHECK(edges.front() <= 7.0);
    CHECK(edges.back() >= 214.0);
    const double step = edges[1] - edges[0];
    CHECK_THAT(step, WithinAbs(10.0, 1e-9));
    CHECK_THAT(edges.front(), WithinAbs(0.0, 1e-9));
    for (size_t i = 1; i < edges.size(); ++i)
        CHECK_THAT(edges[i] - edges[i - 1], WithinAbs(step, 1e-9));
}

TEST_CASE("Histogram bins of a small range keep its decimals", "[LegendText]")
{
    const std::vector<double> edges = legend_bin_edges(0.08, 0.2, LEGEND_HISTOGRAM_BINS, false);
    REQUIRE(edges.size() >= 2);
    CHECK(edges.size() - 1 <= LEGEND_HISTOGRAM_BINS);
    CHECK(edges.front() <= 0.08 + 1e-12);
    CHECK(edges.back() >= 0.2 - 1e-12);
}

TEST_CASE("Logarithmic histogram bins step by 1, 2 and 5 times a power of ten", "[LegendText]")
{
    const std::vector<double> edges = legend_bin_edges(3.0, 700.0, LEGEND_HISTOGRAM_BINS, true);
    const std::vector<double> expected{2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
    REQUIRE(edges.size() == expected.size());
    for (size_t i = 0; i < edges.size(); ++i)
        CHECK_THAT(edges[i], WithinAbs(expected[i], 1e-9));
}

TEST_CASE("Logarithmic histogram bins thin to powers of ten when too many", "[LegendText]")
{
    const std::vector<double> edges = legend_bin_edges(1.0, 1e6, 8, true);
    const std::vector<double> expected{1.0, 10.0, 100.0, 1000.0, 1e4, 1e5, 1e6};
    REQUIRE(edges.size() == expected.size());
    for (size_t i = 0; i < edges.size(); ++i)
        CHECK_THAT(edges[i], WithinAbs(expected[i], 1e-6));
}

TEST_CASE("A value falls in its bin, the ends clamped", "[LegendText]")
{
    const std::vector<double> edges{0.0, 10.0, 20.0, 30.0};
    CHECK(legend_bin_of(edges, -5.0) == 0);
    CHECK(legend_bin_of(edges, 0.0) == 0);
    CHECK(legend_bin_of(edges, 9.99) == 0);
    CHECK(legend_bin_of(edges, 10.0) == 1);
    CHECK(legend_bin_of(edges, 30.0) == 2);
    CHECK(legend_bin_of(edges, 99.0) == 2);
}

TEST_CASE("A move at the limit reaches it short of rounding, a layer under the limit is shorter", "[LegendText]")
{
    CHECK(legend_at_limit(12.0, 12.0));
    CHECK(legend_at_limit(11.8, 12.0));
    CHECK_FALSE(legend_at_limit(11.0, 12.0));
    CHECK_FALSE(legend_at_limit(5.0, 0.0));
    CHECK(legend_under_limit(7.9, 8.0));
    CHECK_FALSE(legend_under_limit(8.0, 8.0));
    CHECK_FALSE(legend_under_limit(1.0, 0.0));
}

TEST_CASE("Fine bins merge into the narrowest round bins that fit", "[LegendText]")
{
    LegendBins fine;
    for (int i = 0; i <= 10; ++i)
        fine.edges.push_back(double(i));
    fine.amounts = {1.0, 2.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 0.0, 0.0};
    fine.lowest  = {0.5f, 1.2f, 0.f, 0.f, 0.f, 5.5f, 0.f, 0.f, 0.f, 0.f};
    fine.highest = {0.9f, 1.8f, 0.f, 0.f, 0.f, 5.9f, 0.f, 0.f, 0.f, 0.f};

    const LegendBins same = legend_merge_bins(fine, 3);
    CHECK(same.amounts == fine.amounts);

    const LegendBins merged = legend_merge_bins(fine, 2);
    REQUIRE(merged.edges.size() == 6);
    CHECK_THAT(merged.edges[1] - merged.edges[0], WithinAbs(2.0, 1e-9));
    CHECK_THAT(merged.amounts[0], WithinAbs(3.0, 1e-9));
    CHECK_THAT(merged.amounts[2], WithinAbs(4.0, 1e-9));
    CHECK_THAT(merged.lowest[0], WithinAbs(0.5, 1e-6));
    CHECK_THAT(merged.highest[0], WithinAbs(1.8, 1e-6));
    CHECK_THAT(merged.lowest[2], WithinAbs(5.5, 1e-6));
}

TEST_CASE("Merged bins keep edges on round values of a range that does not start at zero", "[LegendText]")
{
    LegendBins fine;
    for (int i = 0; i <= 30; ++i)
        fine.edges.push_back(7.0 + double(i));
    fine.amounts.assign(30, 1.0);
    const LegendBins merged = legend_merge_bins(fine, 4);
    REQUIRE(merged.edges.size() >= 2);
    const double step = merged.edges[1] - merged.edges[0];
    CHECK_THAT(std::fmod(merged.edges.front(), step), WithinAbs(0.0, 1e-9));
    CHECK(merged.edges.front() <= 7.0);
    CHECK(merged.edges.back() >= 37.0);
    double total = 0.0;
    for (const double a : merged.amounts)
        total += a;
    CHECK_THAT(total, WithinAbs(30.0, 1e-9));
    CHECK(std::count_if(merged.amounts.begin(), merged.amounts.end(), [](double a) { return a > 0.0; }) <= 4);
}

TEST_CASE("A trace of time in many bins does not coarsen the merged bins", "[LegendText]")
{
    LegendBins fine;
    for (int i = 0; i <= 100; ++i)
        fine.edges.push_back(double(i));
    fine.amounts.assign(100, 0.0);
    for (int i = 0; i < 10; ++i)
        fine.amounts[size_t(i)] = 100.0;
    for (int i = 20; i < 100; i += 2)
        fine.amounts[size_t(i)] = .01;
    const LegendBins merged = legend_merge_bins(fine, 10, .005);
    CHECK_THAT(merged.edges[1] - merged.edges[0], WithinAbs(1.0, 1e-9));
}

TEST_CASE("Filaments with the same limit share one line, the others get their own", "[LegendText]")
{
    const std::vector<LegendLimit> limits = legend_limits({12.f, 21.f, 12.f, 0.f}, {1, 2, 3, 4});
    REQUIRE(limits.size() == 2);
    CHECK_THAT(limits[0].value, WithinAbs(12., 1e-6));
    CHECK(limits[0].filaments == std::vector<int>{1, 3});
    CHECK_THAT(limits[1].value, WithinAbs(21., 1e-6));
    CHECK(limits[1].filaments == std::vector<int>{2});
    CHECK(legend_limits({12.f, 21.f}, {2}).size() == 1);
    CHECK(legend_limits({12.f}, {3}).empty());
}

TEST_CASE("The legend's columns round-trip through the app config and count as one setting", "[LegendText]")
{
    LegendSettings settings;
    CHECK(legend_columns_text(settings) == "time,share,weight");
    CHECK(legend_changed_settings(settings) == 0);
    legend_read_columns(settings, "time,length");
    CHECK(settings.time);
    CHECK_FALSE(settings.share);
    CHECK_FALSE(settings.weight);
    CHECK(settings.length);
    CHECK(legend_columns_text(settings) == "time,length");
    settings.palette = LegendPalette::Perceptual;
    CHECK(legend_changed_settings(settings) == 2);
    legend_read_columns(settings, "nothing");
    CHECK(legend_columns_text(settings) == "time,length");
}

TEST_CASE("Values a label would merge but the preview colours apart are categories with labels that tell them apart", "[LegendText]")
{
    const float full = 100.f, below = 254.f / 255.f * 100.f;
    CHECK(legend_value(full, 0) == legend_value(below, 0));
    CHECK(legend_value_key(full, 0) != legend_value_key(below, 0));
    const std::optional<int> decimals = legend_category_decimals({below, full}, 0, false);
    REQUIRE(decimals.has_value());
    CHECK(*decimals == 1);
    CHECK(legend_value(below, *decimals) != legend_value(full, *decimals));
}

TEST_CASE("Values that differ past the decimals the preview can tell apart are one category", "[LegendText]")
{
    CHECK(legend_value_key(0.2f, 2) == legend_value_key(0.20000002f, 2));
    CHECK(legend_value_key(49.601f, 0) == legend_value_key(49.604f, 0));
}

TEST_CASE("Categories take no more decimals than they need, and none of them tell apart makes a scale", "[LegendText]")
{
    CHECK(legend_category_decimals({220.f, 230.f}, 0, false) == std::optional<int>(0));
    CHECK(legend_category_decimals({49.6f, 50.4f}, 0, false) == std::optional<int>(1));
    CHECK(legend_category_decimals({12.f, 15.f}, 1, true) == std::optional<int>(1));
    CHECK_FALSE(legend_category_decimals({12.2f, 12.4f}, 1, true).has_value());
}

TEST_CASE("The range filter lives only while the legend shows it on an open scale", "[LegendText]")
{
    LegendScale scale;
    scale.palette = {CanvasUi::Color(0.f, 0.f, 0.f), CanvasUi::Color(1.f, 1.f, 1.f)};
    scale.spread  = LegendSpread::Scale;
    CHECK(legend_range_filter_shown(false, scale));
    CHECK_FALSE(legend_range_filter_shown(true, scale));
    scale.spread = LegendSpread::Few;
    CHECK_FALSE(legend_range_filter_shown(false, scale));
    scale.spread = LegendSpread::One;
    CHECK_FALSE(legend_range_filter_shown(false, scale));
    scale.spread = LegendSpread::Scale;
    scale.palette.clear();
    CHECK_FALSE(legend_range_filter_shown(false, scale));
}

TEST_CASE("The filter row of a histogram of layers gives the share of the layers", "[LegendText]")
{
    const std::string layers = legend_filter_text("10s–20s", .1, true);
    const std::string time   = legend_filter_text("120–130 mm/s", .2, false);
    CHECK(layers.find("10.0%") != std::string::npos);
    CHECK(layers.find("of the layers") != std::string::npos);
    CHECK(layers.find("of the time") == std::string::npos);
    CHECK(time.find("of the time") != std::string::npos);
}

TEST_CASE("The note under a scale of flow warns only when some of the print runs at the limit", "[LegendText]")
{
    LegendScale scale;
    scale.limits = {{12.f, {1}}};
    scale.at_limit_share = 0.0;
    const std::optional<CanvasUi::AlertOptions> none = legend_limit_note(scale, LegendUnits());
    REQUIRE(none.has_value());
    CHECK(none->variant == CanvasUi::AlertVariant::Info);
    CHECK(none->text.find("capped") == std::string::npos);
    scale.at_limit_share = .23;
    const std::optional<CanvasUi::AlertOptions> some = legend_limit_note(scale, LegendUnits());
    REQUIRE(some.has_value());
    CHECK(some->variant == CanvasUi::AlertVariant::Warning);
    CHECK(some->text.find("23") != std::string::npos);
}
