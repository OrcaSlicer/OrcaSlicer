#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"

#include <array>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

LegendScale linear_scale(float min, float max, int decimals = 0)
{
    LegendScale scale;
    scale.min      = min;
    scale.max      = max;
    scale.decimals = decimals;
    return scale;
}

struct Sample
{
    float  value;
    double time;
    int    layer;
    int    filament;
};

LegendValues scan(const LegendScale& scale, bool layers, const std::vector<float>& limits, const std::vector<Sample>& samples)
{
    LegendValueScan scan(scale, layers, limits);
    for (const Sample& s : samples)
        scan.add(s.value, s.time, s.layer, s.filament);
    return scan.values();
}

double sum(const std::vector<double>& amounts) { return std::accumulate(amounts.begin(), amounts.end(), 0.0); }

} // namespace

TEST_CASE("A histogram of values holds their print time, a histogram of layer times holds one per layer", "[LegendModel]")
{
    const LegendScale         scale   = linear_scale(0.f, 100.f);
    const std::vector<Sample> samples = {{10.f, 2.0, 1, 0}, {10.f, 3.0, 2, 0}, {90.f, 5.0, 3, 0}};

    const LegendValues by_time = scan(scale, false, {}, samples);
    REQUIRE_FALSE(by_time.histogram.amounts.empty());
    CHECK_FALSE(by_time.histogram.layers);
    CHECK_THAT(sum(by_time.histogram.amounts), WithinAbs(2.0 + 3.0 + 5.0, 1e-9));
    CHECK_THAT(by_time.histogram.amounts[legend_bin_of(by_time.histogram.edges, 10.0)], WithinAbs(2.0 + 3.0, 1e-9));

    const LegendValues by_layers = scan(scale, true, {}, samples);
    CHECK(by_layers.histogram.layers);
    CHECK_THAT(sum(by_layers.histogram.amounts), WithinAbs(double(samples.size()), 1e-9));
    CHECK_THAT(by_layers.histogram.amounts[legend_bin_of(by_layers.histogram.edges, 10.0)], WithinAbs(2.0, 1e-9));
}

TEST_CASE("A value that appears once over the print is one category with all of the time", "[LegendModel]")
{
    LegendScale scale = linear_scale(20.f, 20.f);
    legend_set_values(scale, scan(scale, false, {}, {{20.f, 4.0, 2, 0}, {20.f, 6.0, 5, 0}}));
    CHECK(scale.spread == LegendSpread::One);
    REQUIRE(scale.categories.size() == 1);
    CHECK(scale.categories[0].first_layer == 2);
    CHECK(scale.categories[0].last_layer == 5);
    CHECK_THAT(scale.categories[0].time, WithinAbs(4.0 + 6.0, 1e-6));
    CHECK_THAT(scale.categories[0].share, WithinAbs(1.0, 1e-9));
}

TEST_CASE("A few values are categories from the smallest, each with its share of the time and its layers", "[LegendModel]")
{
    LegendScale               scale   = linear_scale(20.f, 60.f);
    const std::vector<Sample> samples = {{60.f, 1.0, 4, 0}, {20.f, 2.0, 3, 0}, {40.f, 3.0, 1, 0}, {20.f, 4.0, 7, 0}};
    legend_set_values(scale, scan(scale, false, {}, samples));
    CHECK(scale.spread == LegendSpread::Few);
    REQUIRE(scale.categories.size() == 3);
    CHECK_THAT(scale.categories[0].value, WithinAbs(20.0, 1e-6));
    CHECK_THAT(scale.categories[1].value, WithinAbs(40.0, 1e-6));
    CHECK_THAT(scale.categories[2].value, WithinAbs(60.0, 1e-6));
    CHECK(scale.categories[0].first_layer == 3);
    CHECK(scale.categories[0].last_layer == 7);
    const double total = 1.0 + 2.0 + 3.0 + 4.0;
    CHECK_THAT(scale.categories[0].share, WithinAbs((2.0 + 4.0) / total, 1e-9));
    CHECK_THAT(scale.categories[1].share, WithinAbs(3.0 / total, 1e-9));
    CHECK_THAT(scale.categories[2].share, WithinAbs(1.0 / total, 1e-9));
}

TEST_CASE("More values than a few make a scale without categories", "[LegendModel]")
{
    LegendScale         scale = linear_scale(0.f, 100.f);
    std::vector<Sample> samples;
    for (size_t i = 0; i < LEGEND_FEW_VALUES + 2; ++i)
        samples.push_back({float(10 * i), 1.0, int(i) + 1, 0});
    legend_set_values(scale, scan(scale, false, {}, samples));
    CHECK(scale.spread == LegendSpread::Scale);
    CHECK(scale.categories.empty());
}

TEST_CASE("The share at the flow limit is the time of the moves at their filament's own limit", "[LegendModel]")
{
    const std::vector<float> limits = {10.f, 20.f};
    LegendScale              scale  = linear_scale(0.f, 20.f, 1);
    scale.limits                    = {{10.f, {1}}, {20.f, {2}}};
    const std::vector<Sample> samples = {{10.f, 2.0, 1, 0}, {10.f, 3.0, 1, 1}, {20.f, 5.0, 2, 1}, {5.f, 10.0, 2, 0}, {20.f, 7.0, 3, 2}};
    legend_set_values(scale, scan(scale, false, limits, samples));
    REQUIRE(scale.at_limit_share.has_value());
    CHECK_THAT(*scale.at_limit_share, WithinAbs((2.0 + 5.0) / (2.0 + 3.0 + 5.0 + 10.0 + 7.0), 1e-9));
    CHECK_FALSE(scale.layers_under_limit.has_value());
}

TEST_CASE("The share at the flow limit is zero when no move reaches it, and absent without limits", "[LegendModel]")
{
    const std::vector<Sample> samples = {{5.f, 2.0, 1, 0}, {6.f, 3.0, 2, 0}};
    LegendScale               limited = linear_scale(0.f, 20.f, 1);
    limited.limits                    = {{10.f, {1}}};
    legend_set_values(limited, scan(limited, false, {10.f}, samples));
    REQUIRE(limited.at_limit_share.has_value());
    CHECK_THAT(*limited.at_limit_share, WithinAbs(0.0, 1e-12));

    LegendScale unlimited = linear_scale(0.f, 20.f, 1);
    legend_set_values(unlimited, scan(unlimited, false, {}, samples));
    CHECK_FALSE(unlimited.at_limit_share.has_value());
}

TEST_CASE("Layers under the slow-down time count only the layers whose filament is known", "[LegendModel]")
{
    LegendScale scale = linear_scale(0.f, 30.f);
    scale.times       = true;
    scale.limits      = {{15.f, {1}}};
    legend_set_values(scale, scan(scale, true, {15.f}, {{10.f, 10.0, 1, 0}, {20.f, 20.0, 2, 0}, {12.f, 12.0, 3, -1}}));
    REQUIRE(scale.layers_under_limit.has_value());
    CHECK(*scale.layers_under_limit == 1);
    CHECK_FALSE(scale.at_limit_share.has_value());
}

TEST_CASE("The focus of a few values takes a narrow interval around their positions, kept on the scale", "[LegendModel]")
{
    const std::array<float, 2> bounds   = {0.f, 100.f};
    const auto                 position = [](float value) { return value / 100.f; };
    const auto                 narrow   = legend_focus_positions(std::array<float, 2>{20.f, 40.f}, std::nullopt, std::nullopt, bounds, position);
    REQUIRE(narrow.has_value());
    CHECK((*narrow)[0] < position(20.f));
    CHECK((*narrow)[1] > position(40.f));
    CHECK_THAT((*narrow)[0], WithinAbs(position(20.f), 1e-2));
    CHECK_THAT((*narrow)[1], WithinAbs(position(40.f), 1e-2));

    const auto ends = legend_focus_positions(std::array<float, 2>{0.f, 100.f}, std::nullopt, std::array<float, 2>{50.f, 60.f}, bounds, position);
    REQUIRE(ends.has_value());
    CHECK_THAT((*ends)[0], WithinAbs(0.0, 1e-9));
    CHECK_THAT((*ends)[1], WithinAbs(1.0, 1e-9));
}

TEST_CASE("The focus of a span is the positions of its ends", "[LegendModel]")
{
    const auto position = [](float value) { return value / 100.f; };
    const auto span     = legend_focus_positions(std::nullopt, std::array<float, 2>{20.f, 60.f}, std::nullopt, {0.f, 100.f}, position);
    REQUIRE(span.has_value());
    CHECK_THAT((*span)[0], WithinAbs(position(20.f), 1e-6));
    CHECK_THAT((*span)[1], WithinAbs(position(60.f), 1e-6));
}

TEST_CASE("The focus of a range filter is clipped to the scale, and keeps nothing when it misses the scale", "[LegendModel]")
{
    const std::array<float, 2> bounds   = {0.f, 100.f};
    const auto                 position = [](float value) { return value / 100.f; };
    const auto                 filter   = [&](float lo, float hi) {
        return legend_focus_positions(std::nullopt, std::nullopt, std::array<float, 2>{lo, hi}, bounds, position);
    };

    const auto inside = filter(20.f, 60.f);
    REQUIRE(inside.has_value());
    CHECK_THAT((*inside)[0], WithinAbs(position(20.f), 1e-6));
    CHECK_THAT((*inside)[1], WithinAbs(position(60.f), 1e-6));

    const auto partly = filter(-10.f, 50.f);
    REQUIRE(partly.has_value());
    CHECK_THAT((*partly)[0], WithinAbs(position(bounds[0]), 1e-6));
    CHECK_THAT((*partly)[1], WithinAbs(position(50.f), 1e-6));

    const auto outside = filter(150.f, 200.f);
    REQUIRE(outside.has_value());
    CHECK((*outside)[0] > (*outside)[1]);
}

TEST_CASE("Without a hovered value, span or filter the focus takes no interval", "[LegendModel]")
{
    CHECK_FALSE(legend_focus_positions(std::nullopt, std::nullopt, std::nullopt, {0.f, 100.f}, [](float value) { return value; }).has_value());
}

namespace {

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts(1);
    for (const char c : text) {
        if (c == separator)
            parts.emplace_back();
        else
            parts.back() += c;
    }
    return parts;
}

} // namespace

TEST_CASE("The share of a range filter counts each bin it covers in proportion, of the time or of the layers", "[LegendModel]")
{
    const bool      layers = GENERATE(false, true);
    LegendHistogram histogram;
    histogram.edges   = {0.0, 10.0, 20.0, 30.0};
    histogram.amounts = layers ? std::vector<double>{4.0, 2.0, 2.0} : std::vector<double>{30.0, 10.0, 60.0};
    histogram.layers  = layers;
    const double total = layers ? 8.0 : 100.0;
    const double half_of_first_and_second = layers ? 2.0 + 2.0 : 15.0 + 10.0;

    CHECK_THAT(legend_filter_share(histogram, {5.f, 20.f}), WithinAbs(half_of_first_and_second / total, 1e-9));
    CHECK_THAT(legend_filter_share(histogram, {0.f, 30.f}), WithinAbs(1.0, 1e-9));
    CHECK_THAT(legend_filter_share(histogram, {40.f, 50.f}), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Copied statistics are a line per row of its name, time, share and mass, or count for a move, apart by tabs", "[LegendModel]")
{
    const bool imperial = GENERATE(false, true);
    LegendRow  wall;
    wall.name  = "Outer wall";
    wall.time  = 90.f;
    wall.share = .25;
    wall.grams = 12.5;
    LegendRow travel;
    travel.name  = "Travel";
    travel.move  = true;
    travel.share = 0.0;
    travel.count = 1234;
    const LegendUnits units;

    const std::string text = legend_statistics_text({wall, travel}, imperial, units);
    REQUIRE_FALSE(text.empty());
    CHECK(text.back() == '\n');
    const std::vector<std::string> lines = split(text.substr(0, text.size() - 1), '\n');
    REQUIRE(lines.size() == 2);
    CHECK(split(lines[0], '\t') == std::vector<std::string>{"Outer wall", legend_time(90.f, units), legend_percent(.25), legend_mass(12.5, imperial, units)});
    CHECK(split(lines[1], '\t') == std::vector<std::string>{"Travel", "", legend_percent(0.0), "1234"});
}
