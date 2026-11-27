#include <catch2/catch_all.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include "libslic3r/Config.hpp"
#include <vector>
#include <cstddef>
#include <string>
#include <utility>

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

TEST_CASE("deep_diff reports a serialized list as one option that transfers and reverts whole", "[PresetDiff][Config]")
{
    std::vector<std::string> serialized_keys;
    for (const auto &[key, def] : print_config_def.options)
        if (def.gui_flags == "serialized")
            serialized_keys.push_back(key);
    REQUIRE(std::find(serialized_keys.begin(), serialized_keys.end(), "post_process") != serialized_keys.end());

    using Values = std::vector<std::string>;
    const auto [saved_values, edited_values] = GENERATE(std::make_pair(Values{}, Values{"a"}),
                                                        std::make_pair(Values{"b", "c"}, Values{"a"}),
                                                        std::make_pair(Values{"a", "b"}, Values{"a"}));
    for (const std::string &key : serialized_keys) {
        INFO(key);
        Preset reference(Preset::TYPE_PRINT, "ref");
        reference.config.set_key_value(key, new ConfigOptionStrings(saved_values));
        Preset edited = reference;
        edited.config.option<ConfigOptionStrings>(key)->values = edited_values;

        const std::vector<std::string> expected{key};
        const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
        CHECK(diff == expected);
        if (diff != expected)
            continue;

        DynamicPrintConfig transferred = reference.config;
        transferred.apply_only(edited.config, diff);
        CHECK(transferred.option<ConfigOptionStrings>(key)->values == edited_values);

        DynamicPrintConfig reverted = edited.config;
        reverted.apply_only(reference.config, diff);
        CHECK(reverted.option<ConfigOptionStrings>(key)->values == saved_values);
    }
}

TEST_CASE("deep_diff reports a serialized list that only one preset has as one option", "[PresetDiff][Config]")
{
    Preset reference(Preset::TYPE_PRINT, "ref");
    Preset edited(Preset::TYPE_PRINT, "edited");
    edited.config.set_key_value("post_process", new ConfigOptionStrings({"a", "b"}));

    const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
    CHECK(diff == std::vector<std::string>{"post_process"});
}
