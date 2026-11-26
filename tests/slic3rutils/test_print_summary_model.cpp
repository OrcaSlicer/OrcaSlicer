#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/LegendModel.hpp"
#include "slic3r/GUI/LegendText.hpp"
#include "slic3r/GUI/PrintSummaryModel.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

const CanvasUi::Color RED(1.f, 0.f, 0.f);
const CanvasUi::Color GREEN(0.f, 1.f, 0.f);
const CanvasUi::Color BLUE(0.f, 0.f, 1.f);
const CanvasUi::Color YELLOW(1.f, 1.f, 0.f);

CanvasUi::Color shown(const std::string& hex)
{
    const std::map<std::string, CanvasUi::Color> colors = {{"#0000FF", BLUE}, {"#FFFF00", YELLOW}};
    const auto                                   it     = colors.find(hex);
    return it == colors.end() ? CanvasUi::Color() : it->second;
}

LegendFilament filament(int number, const std::array<double, LEGEND_PART_COUNT>& mm, const std::array<double, LEGEND_PART_COUNT>& grams)
{
    LegendFilament f;
    f.number = number;
    f.mm     = mm;
    f.grams  = grams;
    return f;
}

FilamentChangeStats flush(int grams, int changes)
{
    FilamentChangeStats stats;
    stats.filament_flush_weight       = grams;
    stats.flush_filament_change_count = changes;
    return stats;
}

} // namespace

TEST_CASE("A colour change goes from the filament's current colour to its new one, which the next change starts from", "[PrintSummaryModel]")
{
    const std::vector<double>            zs    = {0.2, 0.4, 0.6, 0.8};
    const std::vector<float>             times = {10.f, 20.f, 30.f, 40.f};
    const std::vector<CustomGCode::Item> items = {{0.4, CustomGCode::ColorChange, 1, "#0000FF", ""},
                                                  {0.6, CustomGCode::PausePrint, 1, "", ""},
                                                  {0.8, CustomGCode::ColorChange, 1, "#FFFF00", ""}};
    const std::vector<SummaryEvent> events = summary_events(items, zs, times, {RED, GREEN}, shown);
    REQUIRE(events.size() == items.size());

    CHECK(events[0].kind == SummaryEvent::Kind::ColorChange);
    CHECK(events[0].layer == 2);
    CHECK_THAT(events[0].time, WithinAbs(times[0], 1e-6));
    CHECK(events[0].before == RED);
    CHECK(events[0].after == BLUE);

    CHECK(events[1].kind == SummaryEvent::Kind::Pause);
    CHECK(events[1].layer == 3);
    CHECK_THAT(events[1].time, WithinAbs(times[0] + times[1], 1e-6));

    CHECK(events[2].kind == SummaryEvent::Kind::ColorChange);
    CHECK(events[2].layer == 4);
    CHECK_THAT(events[2].time, WithinAbs(times[0] + times[1] + times[2], 1e-6));
    CHECK(events[2].before == BLUE);
    CHECK(events[2].after == YELLOW);
}

TEST_CASE("A colour change of a filament the print does not have keeps no colours", "[PrintSummaryModel]")
{
    const std::vector<CustomGCode::Item> items  = {{0.2, CustomGCode::ColorChange, 3, "#0000FF", ""}};
    const std::vector<SummaryEvent>      events = summary_events(items, {0.2}, {10.f}, {RED, GREEN}, shown);
    REQUIRE(events.size() == 1);
    CHECK(events[0].before == CanvasUi::Color());
    CHECK(events[0].after == CanvasUi::Color());
}

TEST_CASE("A height finds the closest layer within the tolerance, and none outside it", "[PrintSummaryModel]")
{
    const std::vector<double> zs = {0.2, 0.4, 0.6};
    CHECK(find_close_layer_idx(zs, 0.4 + 0.5 * EPSILON, EPSILON) == 1);
    CHECK(find_close_layer_idx(zs, 0.2 - 0.5 * EPSILON, EPSILON) == 0);
    CHECK(find_close_layer_idx(zs, 0.6 + 0.5 * EPSILON, EPSILON) == 2);
    CHECK(find_close_layer_idx(zs, 0.5, EPSILON) == -1);
    CHECK(find_close_layer_idx({}, 0.2, EPSILON) == -1);
}

TEST_CASE("The totals take the plater's statistics when it has them, and the filaments' parts otherwise", "[PrintSummaryModel]")
{
    const std::vector<LegendFilament> filaments = {filament(1, {100.0, 20.0, 30.0, 10.0}, {10.0, 2.0, 3.0, 1.0}),
                                                   filament(2, {50.0, 0.0, 0.0, 0.0}, {5.0, 0.0, 0.0, 0.0})};
    const double                      grams     = filaments[0].total_grams() + filaments[1].total_grams();
    const double                      mm        = filaments[0].total_mm() + filaments[1].total_mm();
    const double                      excluded  = 2.0 + 3.0 + 1.0;
    PrintEstimatedStatistics          estimate;
    estimate.modes[0].time         = 3600.f;
    estimate.modes[0].prepare_time = 60.f;

    PrintStatistics plater;
    plater.total_weight        = 50.0;
    plater.total_used_filament = 1000.0;
    plater.total_cost          = 2.5;
    const SummaryTotals from_plater = summary_totals(filaments, plater, estimate, 0, LegendUnits());
    CHECK_THAT(from_plater.grams, WithinAbs(plater.total_weight, 1e-9));
    CHECK_THAT(from_plater.mm, WithinAbs(plater.total_used_filament, 1e-9));
    CHECK_THAT(from_plater.model_grams, WithinAbs(plater.total_weight - excluded, 1e-9));
    CHECK_THAT(from_plater.cost, WithinAbs(plater.total_cost, 1e-9));
    CHECK_THAT(from_plater.time, WithinAbs(estimate.modes[0].time, 1e-3));
    CHECK_THAT(from_plater.prepare, WithinAbs(estimate.modes[0].prepare_time, 1e-3));

    const SummaryTotals from_filaments = summary_totals(filaments, PrintStatistics(), estimate, 0, LegendUnits());
    CHECK_THAT(from_filaments.grams, WithinAbs(grams, 1e-9));
    CHECK_THAT(from_filaments.mm, WithinAbs(mm, 1e-9));
    CHECK_THAT(from_filaments.model_grams, WithinAbs(grams - excluded, 1e-9));
}

TEST_CASE("The time modes differ only when their estimates read differently", "[PrintSummaryModel]")
{
    PrintEstimatedStatistics estimate;
    estimate.modes[0].time = 3600.f;
    estimate.modes[1].time = 3600.f;
    CHECK_FALSE(summary_totals({}, PrintStatistics(), estimate, 0, LegendUnits()).modes_differ);
    estimate.modes[1].time = 4200.f;
    CHECK(summary_totals({}, PrintStatistics(), estimate, 0, LegendUnits()).modes_differ);
}

TEST_CASE("A filament's parts are its volumes as a length at its diameter and a mass at its density", "[PrintSummaryModel]")
{
    const std::array<double, LEGEND_PART_COUNT> volumes  = {1000.0, 200.0, 300.0, 100.0};
    const float                                 diameter = 1.75f, density = 1.24f;
    const auto [mm, grams]                               = filament_parts(volumes, diameter, density);
    const double area                                    = PI * std::pow(0.5 * double(diameter), 2);
    for (size_t part = 0; part < LEGEND_PART_COUNT; ++part) {
        CHECK_THAT(mm[part] * area, WithinAbs(volumes[part], 1e-6));
        CHECK_THAT(grams[part], WithinAbs(volumes[part] * double(density) / 1000.0, 1e-9));
    }
    CHECK_THAT(filament_parts(volumes, 0.f, density).first[size_t(LegendPart::Model)], WithinAbs(0.0, 1e-12));
}

TEST_CASE("A filament's volumes add up over the plates by part, and a filament a plate lacks adds nothing", "[PrintSummaryModel]")
{
    PrintEstimatedStatistics plate;
    plate.model_volumes_per_extruder      = {{0, 100.0}, {1, 40.0}};
    plate.support_volumes_per_extruder    = {{0, 20.0}};
    plate.flush_per_filament              = {{0, 5.0}};
    plate.wipe_tower_volumes_per_extruder = {{1, 7.0}};

    std::array<double, LEGEND_PART_COUNT> first{};
    add_filament_volumes(first, plate, 0);
    add_filament_volumes(first, plate, 0);
    CHECK_THAT(first[size_t(LegendPart::Model)], WithinAbs(2 * 100.0, 1e-9));
    CHECK_THAT(first[size_t(LegendPart::Support)], WithinAbs(2 * 20.0, 1e-9));
    CHECK_THAT(first[size_t(LegendPart::Flushed)], WithinAbs(2 * 5.0, 1e-9));
    CHECK_THAT(first[size_t(LegendPart::Tower)], WithinAbs(0.0, 1e-12));

    std::array<double, LEGEND_PART_COUNT> missing{};
    add_filament_volumes(missing, plate, 2);
    for (const double volume : missing)
        CHECK_THAT(volume, WithinAbs(0.0, 1e-12));
}

TEST_CASE("A grouping worse than the optimal one is compared with it, else with one nozzle when it saves", "[PrintSummaryModel]")
{
    StatisticsByExtruderCount stats;
    stats.stats_by_single_extruder     = flush(50, 8);
    stats.stats_by_multi_extruder_curr = flush(30, 5);
    stats.stats_by_multi_extruder_best = flush(20, 3);
    SummaryGrouping worse;
    summary_compare_grouping(worse, stats);
    CHECK_FALSE(worse.optimal);
    CHECK(worse.comparison == "Increase " + std::to_string(30 - 20) + "g filament and " + std::to_string(5 - 3) +
                                  " changes compared to optimal grouping.");

    stats.stats_by_multi_extruder_best = stats.stats_by_multi_extruder_curr;
    SummaryGrouping optimal;
    summary_compare_grouping(optimal, stats);
    CHECK(optimal.optimal);
    CHECK(optimal.comparison == "Save " + std::to_string(50 - 30) + "g filament and " + std::to_string(8 - 5) +
                                    " changes compared to a printer with one nozzle.");

    stats.stats_by_single_extruder = stats.stats_by_multi_extruder_curr;
    SummaryGrouping same;
    summary_compare_grouping(same, stats);
    CHECK(same.optimal);
    CHECK(same.comparison.empty());
}

TEST_CASE("A difference of filament past 1000 grams tells its first three digits", "[PrintSummaryModel]")
{
    CHECK(grouping_number(999.f) == 999);
    CHECK(grouping_number(1234.f) == 123);
}
