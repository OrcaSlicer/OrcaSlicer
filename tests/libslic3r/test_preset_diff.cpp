#include <catch2/catch_all.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>

using namespace Slic3r;

// Regression test for the python-plugin branch's intentional divergence from
// upstream in add_correct_opts_to_diff() (src/libslic3r/Preset.cpp): a vector
// option entry whose index is beyond the reference vector's length is reported
// dirty even when it duplicates an existing value. On main these duplicates
// were NOT flagged. See the comment on add_correct_opts_to_diff() in src/libslic3r/Preset.cpp.
TEST_CASE("deep_diff flags new vector entries that duplicate values[0]", "[PresetDiff][Config]")
{
    // reference: single-extruder vector (one entry)
    Preset reference(Preset::TYPE_PRINTER, "ref");
    reference.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});

    // edited: a second extruder entry was added whose value duplicates the first
    Preset edited(Preset::TYPE_PRINTER, "edited");
    edited.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.4});

    // deep_compare = true routes through deep_diff() -> add_correct_opts_to_diff()
    std::vector<std::string> diff =
        PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);

    // The new index #1 is reported dirty even though 0.4 == values[0] (0.4).
    REQUIRE(std::find(diff.begin(), diff.end(), "nozzle_diameter#1") != diff.end());

    // Sanity: the unchanged existing index #0 is NOT reported, so the rule is
    // specific to new indices rather than flagging the whole vector.
    REQUIRE(std::find(diff.begin(), diff.end(), "nozzle_diameter#0") == diff.end());
}

TEST_CASE("deep_diff identifies the changed extruder printable area", "[PresetDiff][Config]")
{
    const Vec2ds left_area{
        Vec2d(0., 0.), Vec2d(100., 0.), Vec2d(100., 100.), Vec2d(0., 100.)};
    const Vec2ds right_area{
        Vec2d(100., 0.), Vec2d(200., 0.), Vec2d(200., 100.), Vec2d(100., 100.)};
    Vec2ds edited_right_area = right_area;
    edited_right_area[2] = Vec2d(210., 100.);

    Preset reference(Preset::TYPE_PRINTER, "ref");
    reference.config.set_key_value(
        "extruder_printable_area", new ConfigOptionPointsGroups{left_area, right_area});

    Preset edited(Preset::TYPE_PRINTER, "edited");
    edited.config.set_key_value(
        "extruder_printable_area", new ConfigOptionPointsGroups{left_area, edited_right_area});

    const std::vector<std::string> diff =
        PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);

    REQUIRE(std::find(diff.begin(), diff.end(), "extruder_printable_area#0") == diff.end());
    REQUIRE(std::find(diff.begin(), diff.end(), "extruder_printable_area#1") != diff.end());
}

TEST_CASE("deep_diff preserves printable area count changes during transfer", "[PresetDiff][Config]")
{
    const size_t edited_count = GENERATE(size_t(0), size_t(1), size_t(3));
    const Vec2ds area{Vec2d(0., 0.), Vec2d(100., 0.), Vec2d(100., 100.), Vec2d(0., 100.)};

    Preset reference(Preset::TYPE_PRINTER, "ref");
    reference.config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups{area, area});

    Preset edited = reference;
    edited.config.set_key_value(
        "extruder_printable_area", new ConfigOptionPointsGroups(std::vector<Vec2ds>(edited_count, area)));

    const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
    REQUIRE(diff == std::vector<std::string>{"extruder_printable_area"});

    DynamicPrintConfig transferred = reference.config;
    transferred.apply_only(edited.config, diff);
    REQUIRE(*transferred.option("extruder_printable_area") == *edited.config.option("extruder_printable_area"));
}

TEST_CASE("deep_diff distinguishes absolute and percentage speeds for each variant", "[PresetDiff][Config]")
{
    const size_t changed_index = GENERATE(size_t(0), size_t(1));
    Preset reference(Preset::TYPE_PRINT, "ref");
    reference.config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercents{{50., false}, {50., false}});

    Preset edited = reference;
    edited.config.option<ConfigOptionFloatsOrPercents>("small_perimeter_speed")->values[changed_index].percent = true;

    const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
    REQUIRE(diff == std::vector<std::string>{"small_perimeter_speed#" + std::to_string(changed_index)});

    DynamicPrintConfig transferred = reference.config;
    transferred.apply_only(edited.config, diff);
    REQUIRE(*transferred.option("small_perimeter_speed") == *edited.config.option("small_perimeter_speed"));
}
