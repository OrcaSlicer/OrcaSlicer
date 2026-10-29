#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "catch2/matchers/catch_matchers.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PublishSettings.hpp"

using namespace Slic3r;

namespace {

void check_double_vector(const std::vector<double> &actual, std::initializer_list<double> expected)
{
    REQUIRE(actual.size() == expected.size());
    size_t index = 0;
    for (double value : expected)
        REQUIRE_THAT(actual[index++], Catch::Matchers::WithinAbs(value, 1e-6));
}

Preset &add_inmemory_preset(PresetCollection &coll, const std::string &name)
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = std::string();
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

// One slot with two variants (Standard, High Flow) and one slot with one variant, as
// full_config(false) lays them out: variant-bearing keys carry one element per (slot, variant)
// concatenated in slot order, every other filament key carries one element per slot.
DynamicPrintConfig make_variant_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000", "#00FF00" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1, 2 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow",
                                                                                     "Direct Drive Standard" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.0, 1.2 };
    config.option<ConfigOptionFloats>("filament_cost", true)->values                      = { 20.0, 30.0 };
    config.option<ConfigOptionInts>("filament_map", true)->values                         = { 1, 1 };
    config.option<ConfigOptionInts>("filament_volume_map", true)->values                  = { int(nvtStandard), int(nvtStandard) };
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values           = { int(nvtStandard) };
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values                = { int(etDirectDrive) };
    return config;
}

} // namespace

TEST_CASE("Slot columns follow filament_self_index for uneven variant counts", "[PublishSettings]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_self_index", new ConfigOptionInts({ 1, 1, 2, 3, 3, 3 }));
    config.set_key_value("filament_extruder_variant",
                         new ConfigOptionStrings({ "A", "B", "C", "D", "E", "F" }));

    CHECK(publish_filament_columns(config, 0) == std::vector<size_t>{ 0, 1 });
    CHECK(publish_filament_columns(config, 1) == std::vector<size_t>{ 2 });
    CHECK(publish_filament_columns(config, 2) == std::vector<size_t>{ 3, 4, 5 });
    CHECK(publish_filament_columns(config, 3).empty());

    for (size_t slot = 0; slot < 3; ++slot) {
        const std::vector<size_t> columns = publish_filament_columns(config, slot);
        for (size_t local = 0; local < columns.size(); ++local)
            CHECK(publish_filament_local_to_file(config, slot, local) == std::optional<size_t>(columns[local]));
    }
    CHECK_FALSE(publish_filament_local_to_file(config, 1, 1).has_value());
}

TEST_CASE("Variant matching is exact and never falls back to another variant", "[PublishSettings]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_self_index", new ConfigOptionInts({ 1, 1 }));
    config.set_key_value("filament_extruder_variant",
                         new ConfigOptionStrings({ "Direct Drive Standard", "Direct Drive High Flow" }));

    CHECK(publish_match_variant(config, std::nullopt, "Direct Drive High Flow") == std::optional<size_t>(1));
    CHECK(publish_match_variant(config, size_t(0), "Direct Drive High Flow") == std::optional<size_t>(1));
    CHECK(publish_match_variant(config, std::nullopt, "Direct Drive Extra High Flow") == std::nullopt);
    CHECK(publish_match_variant(config, size_t(0), "Bowden Standard") == std::nullopt);
}

TEST_CASE("Publish key parsing rejects a missing or malformed suffix", "[PublishSettings]")
{
    CHECK(publish_key_index("retraction_length#2") == 2);
    CHECK(publish_key_index("retraction_length#0") == 0);
    CHECK(publish_key_index("retraction_length") == -1);
    CHECK(publish_key_index("retraction_length#") == -1);
    CHECK(publish_key_index("retraction_length#abc") == -1);
    CHECK(publish_key_index("retraction_length#2x") == -1);
    CHECK(publish_base_key("retraction_length#2") == "retraction_length");
}

TEST_CASE("Resolved column reports a missing variant as inexact and returns the first element", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    SECTION("the printer's variant is defined by the preset") {
        const PublishResolvedColumn resolved = publish_resolved_column(config, 0);
        CHECK(resolved.exact);
        CHECK(resolved.file_column == std::optional<size_t>(0));
        CHECK(resolved.required_variant == "Direct Drive Standard");
        CHECK(resolved.used_variant == "Direct Drive Standard");
    }

    SECTION("the printer needs a variant the preset does not define") {
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtHighFlow) };
        // Slot 0 defines Standard and High Flow, so it resolves exactly.
        CHECK(publish_resolved_column(config, 0).exact);
        CHECK(publish_resolved_column(config, 0).file_column == std::optional<size_t>(1));

        // Slot 1 only defines Standard.
        const PublishResolvedColumn resolved = publish_resolved_column(config, 1);
        CHECK_FALSE(resolved.exact);
        CHECK(resolved.file_column == std::optional<size_t>(2));
        CHECK(resolved.required_variant == "Direct Drive High Flow");
        CHECK(resolved.used_variant == "Direct Drive Standard");
    }

    SECTION("a hybrid extruder resolves through the slot's own volume type") {
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtHybrid) };
        config.option<ConfigOptionInts>("filament_volume_map", true)->values        = { int(nvtStandard), int(nvtStandard) };
        const PublishResolvedColumn resolved = publish_resolved_column(config, 0);
        CHECK(resolved.exact);
        CHECK(resolved.file_column == std::optional<size_t>(0));

        config.option<ConfigOptionInts>("filament_volume_map", true)->values = { int(nvtHighFlow), int(nvtStandard) };
        const PublishResolvedColumn high_flow = publish_resolved_column(config, 0);
        CHECK(high_flow.exact);
        CHECK(high_flow.file_column == std::optional<size_t>(1));
    }
}

TEST_CASE("A variant-bearing material key keeps only that slot's own column", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    std::vector<PublishedMaterialEntry> entries(1);
    entries[0].slot = 1; // the one-variant slot after the two-variant slot
    entries[0].keys = { "filament_retraction_length", "filament_cost" };

    std::vector<std::string> notices;
    publish_normalize_material_entries(entries, config, &notices);
    CHECK(notices.empty());
    REQUIRE(entries[0].keys.size() == 2);
    // The variant-bearing key now names its file column; the per-slot key stays plain.
    CHECK(entries[0].keys[0] == "filament_retraction_length#2");
    CHECK(entries[0].keys[1] == "filament_cost");

    const DynamicPrintConfig filtered = filter_published_config(config, {}, entries);

    // The slot's own column is kept; the other slots' columns are masked to the option default.
    const auto *retraction = filtered.opt<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(retraction != nullptr);
    REQUIRE(retraction->size() == 3);
    REQUIRE_THAT(retraction->get_at(2), Catch::Matchers::WithinAbs(1.2, 1e-6));
    const auto *retraction_default =
        dynamic_cast<const ConfigOptionFloatsNullable *>(print_config_def.get("filament_retraction_length")->default_value.get());
    REQUIRE(retraction_default != nullptr);
    REQUIRE_THAT(retraction->get_at(0), Catch::Matchers::WithinAbs(retraction_default->get_at(0), 1e-6));
    REQUIRE_THAT(retraction->get_at(1), Catch::Matchers::WithinAbs(retraction_default->get_at(0), 1e-6));

    // The per-slot key keeps the slot's value and masks the other slot.
    const auto *cost = filtered.opt<ConfigOptionFloats>("filament_cost");
    REQUIRE(cost != nullptr);
    REQUIRE(cost->size() == 2);
    REQUIRE_THAT(cost->get_at(1), Catch::Matchers::WithinAbs(30.0, 1e-6));
    const auto *cost_default = dynamic_cast<const ConfigOptionFloats *>(print_config_def.get("filament_cost")->default_value.get());
    REQUIRE(cost_default != nullptr);
    REQUIRE_THAT(cost->get_at(0), Catch::Matchers::WithinAbs(cost_default->get_at(0), 1e-6));
}

TEST_CASE("A variant-bearing material key whose column is out of range is dropped", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    std::vector<PublishedMaterialEntry> entries(1);
    entries[0].slot = 0;
    entries[0].keys = { "filament_retraction_length#4" };

    const DynamicPrintConfig filtered = filter_published_config(config, {}, entries);
    CHECK(filtered.option("filament_retraction_length") == nullptr);
}

TEST_CASE("A supplied material column belonging to another slot is dropped on export", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    std::vector<PublishedMaterialEntry> entries(1);
    entries[0].slot = 0;
    entries[0].keys = { "filament_retraction_length#2" }; // column 2 belongs to slot 1

    const DynamicPrintConfig filtered = filter_published_config(config, {}, entries);
    CHECK(filtered.option("filament_retraction_length") == nullptr);
}

TEST_CASE("A variant vector shorter than the file layout is dropped instead of shifting columns", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();
    // Slot 0 has two columns, slot 1 one, but the flattened vector carries only two values: every
    // column past index 1 belongs to another slot's value, so no column can be trusted.
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.0 };

    std::vector<PublishedMaterialEntry> entries(1);
    entries[0].slot = 0;
    entries[0].keys = { "filament_retraction_length#1" };

    const DynamicPrintConfig filtered = filter_published_config(config, {}, entries);
    CHECK(filtered.option("filament_retraction_length") == nullptr);
}

TEST_CASE("A variant vector longer than the file layout is dropped instead of shifting columns", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();
    // One value too many: every column after the extra value belongs to another slot.
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.0, 1.2, 1.3 };

    std::vector<PublishedMaterialEntry> entries(1);
    entries[0].slot = 0;
    entries[0].keys = { "filament_retraction_length#0" };

    const DynamicPrintConfig filtered = filter_published_config(config, {}, entries);
    CHECK(filtered.option("filament_retraction_length") == nullptr);
}

TEST_CASE("A slot with no exact variant match publishes the value it prints with", "[PublishSettings]")
{
    DynamicPrintConfig config   = make_variant_config();
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Bowden Standard", "Bowden Standard",
                                                                                     "Bowden Standard" };
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtStandard) };

    std::vector<PublishedMaterialEntry> entries(1);
    entries[0].slot = 0;
    entries[0].keys = { "filament_retraction_length" };

    std::vector<std::string> notices;
    publish_normalize_material_entries(entries, config, &notices);
    REQUIRE(entries[0].keys.size() == 1);
    // The variant the printer needs is missing: publish the first column, and say so.
    CHECK(entries[0].keys[0] == "filament_retraction_length#0");
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("Direct Drive Standard") != std::string::npos);
    CHECK(notices[0].find("Bowden Standard") != std::string::npos);

    const DynamicPrintConfig filtered = filter_published_config(config, {}, entries);
    const auto *retraction = filtered.opt<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(retraction != nullptr);
    REQUIRE_THAT(retraction->get_at(0), Catch::Matchers::WithinAbs(0.9, 1e-6));
}

TEST_CASE("Published material keys name their file column and import onto that variant", "[PublishSettings]")
{
    // The author's file: one slot with two variants. Column 1 is the High Flow column.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionStrings>("filament_type", true)->values             = { "PLA" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                          "Direct Drive High Flow" };
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5, 0.6 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot = 0;
    entry.keys = { "filament_retraction_length#1" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // Variant 1 (High Flow) takes the published value; variant 0 is untouched.
    check_double_vector(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values,
                        { 0.5, 1.2 });
    CHECK(pub.skipped_keys.empty());
}

TEST_CASE("A published variant the receiver lacks is reported and not written", "[PublishSettings]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    // The receiver defines only the Standard variant.
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard" };
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot = 0;
    entry.keys = { "filament_retraction_length#1" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    check_double_vector(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values,
                        { 0.5 });
    REQUIRE(pub.skipped_keys.size() == 1);
    CHECK(pub.skipped_keys.front().find("Direct Drive High Flow") != std::string::npos);
}

TEST_CASE("A published column belonging to another slot is skipped and reported", "[PublishSettings]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000", "#00FF00" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 2 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive Standard" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard" };
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA", "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot = 0; // names slot 0, but column 1 belongs to slot 1
    entry.keys = { "filament_retraction_length#1" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    check_double_vector(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values,
                        { 0.5 });
    REQUIRE(pub.skipped_keys.size() == 1);
    CHECK(pub.skipped_keys.front().find("does not belong") != std::string::npos);
}

TEST_CASE("A published file without file columns imports positionally", "[PublishSettings]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000", "#00FF00" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 2 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive Standard" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard" };
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA", "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot = 1;
    entry.keys = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    check_double_vector(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values,
                        { 1.2 });
    CHECK(pub.skipped_keys.empty());
}

TEST_CASE("Full Publish copies every variant of the author's slot", "[PublishSettings]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionStrings>("filament_type", true)->values             = { "PLA" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard" };
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot       = 0;
    entry.full       = true;
    entry.full_keys  = { "filament_retraction_length" };
    entry.preset_name = "Author PLA";

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The slot is re-pointed to a standalone copy carrying the author's two variants.
    REQUIRE(bundle.filament_presets.size() == 1);
    const Preset *copy = bundle.filaments.find_preset(bundle.filament_presets.front(), false, true);
    REQUIRE(copy != nullptr);
    CHECK(copy->config.opt<ConfigOptionStrings>("filament_extruder_variant")->values ==
          std::vector<std::string>{ "Direct Drive Standard", "Direct Drive High Flow" });
    check_double_vector(copy->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values, { 0.9, 1.2 });
}

TEST_CASE("A per-slot filament key after a multi-variant slot keeps its own slot's value", "[PublishSettings]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000", "#00FF00" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1, 2 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow",
                                                                                     "Direct Drive Standard" };
    config.option<ConfigOptionFloats>("filament_cost", true)->values              = { 20.0, 30.0 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionFloats>("filament_cost", true)->values = { 1.0 };
    bundle.filament_presets = { "My PLA", "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot = 1;
    entry.keys = { "filament_cost" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    check_double_vector(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloats>("filament_cost")->values, { 30.0 });
    CHECK(pub.skipped_keys.empty());
}

TEST_CASE("Full Publish leaves the receiver's value when the file carries a variant key short of the layout", "[PublishSettings]")
{
    // A file that never laid a variant-bearing key out per variant carries one value for the whole
    // slot list, so it publishes nothing for a slot past the first column: the copy keeps the
    // receiver's value and the key is not reported as skipped.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000", "#00FF00" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 2 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive Standard" };
    // The nullable filament overrides are added to the config def at run time, so they are not part
    // of the static full_print_config() key set: create the key before reading it back.
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
    Preset::normalize(config);
    REQUIRE(config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->size() == 1);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA", "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot        = 1; // column 1, past the file's single value
    entry.full        = true;
    entry.full_keys   = { "filament_retraction_length" };
    entry.preset_name = "Author PLA";

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    CHECK(pub.skipped_keys.empty());
    const Preset *copy = bundle.filaments.find_preset(bundle.filament_presets[1], false, true);
    REQUIRE(copy != nullptr);
    check_double_vector(copy->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values, { 0.5 });
}
