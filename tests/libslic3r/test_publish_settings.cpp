#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "catch2/matchers/catch_matchers.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PublishSelection.hpp"
#include "libslic3r/PublishSettings.hpp"

using namespace Slic3r;

namespace {

PublishRowId make_publish_row(std::vector<std::string> keys, PublishSection section, int owner = -1, int column = -1,
                              PublishRowKind kind = PublishRowKind::Key)
{
    PublishRowId row;
    row.keys    = std::move(keys);
    row.owner   = owner;
    row.column  = column;
    row.section = section;
    row.kind    = kind;
    return row;
}

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

// A target preset carrying the per-variant lists one extruder rule reads: one id and one variant
// string per column. Both id options are integer vectors.
DynamicPrintConfig make_extruder_target(const std::string &id_key, const std::string &variant_key,
                                        const std::vector<int> &ids, const std::vector<std::string> &variants)
{
    DynamicPrintConfig target;
    target.set_key_value(id_key, new ConfigOptionInts(ids));
    target.set_key_value(variant_key, new ConfigOptionStrings(variants));
    return target;
}

// Two slots on the same two-variant filament preset: file columns 0 and 1 belong to slot 0, 2 and
// 3 to slot 1, so a slot's local variant index is not its file column.
DynamicPrintConfig make_shared_variant_config()
{
    DynamicPrintConfig config = make_variant_config();
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1, 2, 2 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow",
                                                                                     "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.0, 1.1, 1.2 };
    return config;
}

// One filament preset config: a variant-bearing key carries one element per variant of that preset,
// every other filament key one element.
DynamicPrintConfig make_filament_preset_config(size_t variants)
{
    DynamicPrintConfig config;
    config.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable(std::vector<double>(variants, 0.9)));
    config.set_key_value("fan_min_speed", new ConfigOptionFloats(std::vector<double>(variants, 50.0)));
    config.set_key_value("filament_cost", new ConfigOptionFloats({ 20.0 }));
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

TEST_CASE("A Full copy drops a variant key the file cannot fill", "[PublishSettings]")
{
    // The author's slot has two variants but the file carries only one value for the key, so the
    // column of the second variant cannot be filled from the author. The copy must not keep the
    // receiver's one-element vector beside an author-length variant list: readers size the layout
    // from the list and would read the wrong column. The key is dropped instead.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionStrings>("filament_type", true)->values             = { "PLA" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard" };
    pla.config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot        = 0;
    entry.full        = true;
    entry.full_keys   = { "filament_retraction_length" };
    entry.preset_name = "Author PLA";

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    const Preset *copy = bundle.filaments.find_preset(bundle.filament_presets.front(), false, true);
    REQUIRE(copy != nullptr);
    const auto *variants = copy->config.opt<ConfigOptionStrings>("filament_extruder_variant");
    REQUIRE(variants != nullptr);
    REQUIRE(variants->values == std::vector<std::string>{ "Direct Drive Standard", "Direct Drive High Flow" });
    CHECK_FALSE(copy->config.has("filament_retraction_length"));

    // The invariant: no variant-bearing key present in the copy is shorter than the list.
    for (const std::string &key : filament_options_with_variant)
        if (const ConfigOptionVectorBase *vec = dynamic_cast<const ConfigOptionVectorBase *>(copy->config.option(key)))
            CHECK(vec->size() == variants->size());
}

TEST_CASE("A parentless project-embedded preset keeps its own variants on reload", "[PublishSettings]")
{
    // A Full Publish copy is stored project-embedded with no parent, so the reload path must
    // resolve it from its own config instead of dropping it for a missing inherit.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionStrings>("filament_type", true)->values             = { "PLA" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value           = std::string();
    config.option<ConfigOptionStrings>("filament_settings_id", true)->values       = { "Author PLA" };
    Preset::normalize(config);

    PresetBundle bundle;
    add_inmemory_preset(bundle.filaments, "My PLA");

    Preset embedded(Preset::TYPE_FILAMENT, "Author PLA");
    embedded.config                = config;
    embedded.is_project_embedded   = true;
    embedded.loaded                = true;

    std::vector<Preset *> project_presets{ &embedded };
    PresetsConfigSubstitutions substitutions;

    // The save side must embed the copy at all: a parentless preset used to produce no diff and
    // was therefore never written into the project archive.
    Preset *differed = bundle.filaments.get_preset_differed_for_save(embedded);
    REQUIRE(differed != nullptr);
    CHECK(differed->config.opt<ConfigOptionStrings>("filament_extruder_variant")->values ==
          std::vector<std::string>{ "Direct Drive Standard", "Direct Drive High Flow" });
    delete differed;

    bundle.filaments.load_project_embedded_presets(project_presets, "filament", substitutions,
                                                  ForwardCompatibilitySubstitutionRule::Enable);

    const Preset *loaded = bundle.filaments.find_preset("Author PLA", false, true);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->inherits().empty());
    CHECK(loaded->config.opt<ConfigOptionStrings>("filament_extruder_variant")->values ==
          std::vector<std::string>{ "Direct Drive Standard", "Direct Drive High Flow" });
    check_double_vector(loaded->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values, { 0.9, 1.2 });
    for (const std::string &key : filament_options_with_variant)
        if (const ConfigOptionVectorBase *vec = dynamic_cast<const ConfigOptionVectorBase *>(loaded->config.option(key)))
            CHECK(vec->size() == loaded->config.opt<ConfigOptionStrings>("filament_extruder_variant")->size());
}

TEST_CASE("A Full copy drops an unreceived variant key when the receiver lists the variants in another order", "[PublishSettings]")
{
    // The file carries no value for the key, and the receiver's two variants are in the other
    // order. Keeping the receiver's values and relabelling them with the author's names would put
    // the High Flow value under the Standard variant, so the key is dropped and reported.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("filament_diameter", true)->values          = { 1.75 };
    config.option<ConfigOptionStrings>("filament_colour", true)->values           = { "#FF0000" };
    config.option<ConfigOptionInts>("filament_self_index", true)->values          = { 1, 1 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard",
                                                                                     "Direct Drive High Flow" };
    config.option<ConfigOptionStrings>("filament_type", true)->values             = { "PLA" };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset     &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive High Flow",
                                                                                          "Direct Drive Standard" };
    pla.config.option<ConfigOptionFloats>("filament_flow_ratio", true)->values = { 1.05, 0.95 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot        = 0;
    entry.full        = true;
    entry.preset_name = "Author PLA";

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    const Preset *copy = bundle.filaments.find_preset(bundle.filament_presets.front(), false, true);
    REQUIRE(copy != nullptr);
    CHECK(copy->config.opt<ConfigOptionStrings>("filament_extruder_variant")->values ==
          std::vector<std::string>{ "Direct Drive Standard", "Direct Drive High Flow" });
    CHECK_FALSE(copy->config.has("filament_flow_ratio"));
    CHECK_FALSE(pub.skipped_keys.empty());
}

TEST_CASE("Resizing the filament count keeps variant-bearing vectors at their variant length", "[PublishSettings]")
{
    // set_num_filaments resizes the per-slot filament keys to the filament count. Two keys in that
    // list are variant-bearing, so resizing them truncated every variant but the first; readers
    // clamp, so the extra variants silently reused the first value.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard", "Direct Drive High Flow" };
    // filament_retract_length_nc is declared coFloats but its default is nullable, so it is stored
    // as ConfigOptionFloatsNullable.
    config.option<ConfigOptionFloatsNullable>("filament_retract_length_nc", true)->values = { 1.5, 2.5 };
    config.option<ConfigOptionStrings>("volumetric_speed_coefficients", true)->values = { "a", "b" };
    config.option<ConfigOptionStrings>("filament_colour", true)->values = { "#FF0000" };

    config.set_num_filaments(1);

    const auto *retract_nc = config.opt<ConfigOptionFloatsNullable>("filament_retract_length_nc");
    REQUIRE(retract_nc != nullptr);
    CHECK(retract_nc->size() == 2);
    const auto *volumetric = config.opt<ConfigOptionStrings>("volumetric_speed_coefficients");
    REQUIRE(volumetric != nullptr);
    CHECK(volumetric->size() == 2);
    // A per-slot key is still sized to the filament count.
    const auto *colour = config.opt<ConfigOptionStrings>("filament_colour");
    REQUIRE(colour != nullptr);
    CHECK(colour->size() == 1);
}

TEST_CASE("A filament key is variant-bearing exactly when the variant option list names it", "[PublishSettings]")
{
    // The dialog classifies every Filament tab key with this rule, so a wrong answer builds one
    // row per variant for a per-slot key, or the reverse.
    CHECK(publish_is_variant_bearing("filament_retraction_length"));
    CHECK(publish_is_variant_bearing("filament_retraction_length#3"));
    CHECK(publish_is_variant_bearing("filament_flow_ratio"));
    CHECK_FALSE(publish_is_variant_bearing("filament_cost"));
    CHECK_FALSE(publish_is_variant_bearing("filament_colour"));
    CHECK_FALSE(publish_is_variant_bearing("filament_cost#2"));
}

TEST_CASE("A dirty variant index maps to the file column of every slot whose preset is the edited one",
          "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();
    // Slot 0 and slot 1 share the edited preset with two variants each; slot 2 uses another preset.
    config.option<ConfigOptionInts>("filament_self_index", true)->values = { 1, 1, 2, 2, 3 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {
        "Direct Drive Standard", "Direct Drive High Flow",
        "Direct Drive Standard", "Direct Drive High Flow",
        "Direct Drive Standard"
    };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.0, 1.1, 1.2, 1.3 };

    const std::vector<std::string> slot_presets{ "My PLA", "My PLA", "Other PLA" };
    const std::vector<bool>        slot_is_mixed{ false, false, false };

    // Local index 1 of the edited preset is High Flow: column 1 on slot 0, column 3 on slot 1.
    CHECK(publish_dirty_material_keys(config, slot_presets, slot_is_mixed, "My PLA",
                                      { "filament_retraction_length#1" }) ==
          std::vector<std::string>{ "filament_retraction_length#1", "filament_retraction_length#3" });
}

TEST_CASE("A dirty variant index skips mixed slots", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();
    config.option<ConfigOptionInts>("filament_self_index", true)->values = { 1, 1, 2, 2, 3 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {
        "Direct Drive Standard", "Direct Drive High Flow",
        "Direct Drive Standard", "Direct Drive High Flow",
        "Direct Drive Standard"
    };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.0, 1.1, 1.2, 1.3 };

    // A mixed slot's preset name is a copy of its component's, so it carries the edited name too.
    const std::vector<std::string> slot_presets{ "My PLA", "My PLA", "My PLA" };
    const std::vector<bool>        slot_is_mixed{ false, false, true };

    CHECK(publish_dirty_material_keys(config, slot_presets, slot_is_mixed, "My PLA",
                                      { "filament_retraction_length#0" }) ==
          std::vector<std::string>{ "filament_retraction_length#0", "filament_retraction_length#2" });
}

TEST_CASE("A dirty per-slot key maps to every slot of the edited preset", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    const std::vector<std::string> slot_presets{ "My PLA", "My PLA" };
    const std::vector<bool>        slot_is_mixed{ false, false };

    // A per-slot key has one value in the preset, so the dirty key carries "#0" and the mapped
    // publish key is the plain base key, once for both slots.
    CHECK(publish_dirty_material_keys(config, slot_presets, slot_is_mixed, "My PLA",
                                      { "filament_cost#0", "filament_diameter#0" }) ==
          std::vector<std::string>{ "filament_cost", "filament_diameter" });
}

TEST_CASE("A dirty variant index outside a slot's variants contributes nothing", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    // Slot 0 defines two variants (columns 0 and 1); local index 2 names a variant it does not have.
    const std::vector<std::string> slot_presets{ "My PLA", "Other PLA" };
    const std::vector<bool>        slot_is_mixed{ false, false };

    CHECK(publish_dirty_material_keys(config, slot_presets, slot_is_mixed, "My PLA",
                                      { "filament_retraction_length#2" }).empty());
}

TEST_CASE("A dirty key of another preset contributes nothing", "[PublishSettings]")
{
    DynamicPrintConfig config = make_variant_config();

    const std::vector<std::string> slot_presets{ "Other PLA", "Other PLA" };
    const std::vector<bool>        slot_is_mixed{ false, false };

    CHECK(publish_dirty_material_keys(config, slot_presets, slot_is_mixed, "My PLA",
                                      { "filament_retraction_length#0", "filament_cost#0" }).empty());
}

TEST_CASE("Checked state round-trips through published keys and material entries", "[PublishSettings]")
{
    const PublishRowId printer_row = make_publish_row({ "retraction_length#1" }, PublishSection::Printer, -1, 1);
    const PublishRowId slot_row    = make_publish_row({ "filament_cost" }, PublishSection::Material, 0, -1);
    const PublishRowId multi_row =
        make_publish_row({ "overhang_speed_0#2", "overhang_speed_1#2" }, PublishSection::Material, 0, 2);
    const std::vector<PublishRowId> rows{ printer_row, slot_row, multi_row };

    PublishSelection selection;
    selection.set_owner_info(0, PublishOwnerInfo());
    selection.set_owner_enabled(0, true);
    selection.set_checked(printer_row, true);
    selection.set_checked(slot_row, true);
    selection.set_checked(multi_row, true);

    // The rows carry publish-ready keys, and the selection emits them unchanged.
    CHECK(selection.published_keys(rows) == std::vector<std::string>{ "retraction_length#1" });

    // Both material rows land in one entry per owner, in row order and key order.
    const std::vector<PublishedMaterialEntry> entries = selection.material_entries(rows);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].slot == 0);
    CHECK(entries[0].keys ==
          std::vector<std::string>{ "filament_cost", "overhang_speed_0#2", "overhang_speed_1#2" });
}

TEST_CASE("The selection model publishes row keys verbatim", "[PublishSettings]")
{
    // A Filament Cooling row mixes a variant-bearing key with a per-slot key, so the row's column
    // must not be appended to the per-slot key.
    const PublishRowId cooling_row =
        make_publish_row({ "fan_min_speed#3", "fan_cooling_layer_time" }, PublishSection::Material, 0, 3);
    const PublishRowId printer_row = make_publish_row({ "nozzle_temperature#3" }, PublishSection::Printer, -1, 3);
    const std::vector<PublishRowId> rows{ cooling_row, printer_row };

    PublishSelection selection;
    selection.set_owner_info(0, PublishOwnerInfo());
    selection.set_checked(cooling_row, true);
    selection.set_checked(printer_row, true);

    const std::vector<PublishedMaterialEntry> entries = selection.material_entries(rows);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].keys == std::vector<std::string>{ "fan_min_speed#3", "fan_cooling_layer_time" });

    CHECK(selection.published_keys(rows) == std::vector<std::string>{ "nozzle_temperature#3" });
}

TEST_CASE("A disabled row is never exported, even when checked", "[PublishSettings]")
{
    const PublishRowId printer_row  = make_publish_row({ "retraction_length#1" }, PublishSection::Printer, -1, 1);
    const PublishRowId material_row = make_publish_row({ "filament_cost" }, PublishSection::Material, 0, -1);
    const std::vector<PublishRowId> rows{ printer_row, material_row };

    PublishSelection selection;
    selection.set_owner_info(0, PublishOwnerInfo());
    selection.set_owner_enabled(0, true);
    selection.set_checked(printer_row, true);
    selection.set_enabled(printer_row, false);
    selection.set_checked(material_row, true);
    selection.set_enabled(material_row, false);

    CHECK(selection.published_keys(rows).empty());
    CHECK(selection.material_entries(rows).empty());
}

TEST_CASE("Full publish exports every variant of the owner", "[PublishSettings]")
{
    const PublishRowId key_row = make_publish_row({ "filament_cost" }, PublishSection::Material, 0, -1);
    const std::vector<PublishRowId> rows{ key_row };

    PublishOwnerInfo info;
    info.filament_type = "PLA High Speed";
    info.full_keys     = { "filament_flow_ratio", "filament_max_volumetric_speed" };

    PublishSelection selection;
    selection.set_owner_info(0, info);
    selection.set_owner_enabled(0, true);
    selection.set_full_publish(0, true);
    selection.set_checked(key_row, true);

    const std::vector<PublishedMaterialEntry> entries = selection.material_entries(rows);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].full);
    CHECK(entries[0].full_keys == info.full_keys);
    CHECK(entries[0].publish_type);
    CHECK(entries[0].publish_type_value == "PLA");
    // Full Publish ignores the owner's per-key rows.
    CHECK(entries[0].keys.empty());
}

TEST_CASE("Mixed dependency issues name the component that is not published", "[PublishSettings]")
{
    DynamicPrintConfig project;
    // Slot 0 is mixed from slot 2 (1-based), so the only component owner to check is slot 1.
    project.set_key_value("filament_mixed_components", new ConfigOptionStrings({ "2", "" }));

    PublishSelection selection;
    PublishOwnerInfo mixed;
    mixed.mixed = true;
    selection.set_owner_info(0, mixed);
    PublishOwnerInfo component;
    component.filament_type = "PLA";
    selection.set_owner_info(1, component);

    // The component's "Type" requirement row, unchecked.
    const std::vector<PublishRowId> rows{
        make_publish_row({ "filament_type" }, PublishSection::Material, 1, -1, PublishRowKind::Type)
    };

    SECTION("a disabled component is reported") {
        selection.set_owner_enabled(1, false);
        const std::vector<MixedDependencyIssue> issues = unpublished_mixed_components(selection, rows, project);
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].mixed_slot == 0);
        CHECK(issues[0].component_slot == 1);
        CHECK(issues[0].reason == MixedDependencyIssue::Reason::Disabled);
    }

    SECTION("an enabled fully published component clears the issue") {
        selection.set_owner_enabled(1, true);
        selection.set_full_publish(1, true);
        CHECK(unpublished_mixed_components(selection, rows, project).empty());
    }

    SECTION("an enabled component with neither type nor full publish is reported") {
        selection.set_owner_enabled(1, true);
        const std::vector<MixedDependencyIssue> issues = unpublished_mixed_components(selection, rows, project);
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].mixed_slot == 0);
        CHECK(issues[0].component_slot == 1);
        CHECK(issues[0].reason == MixedDependencyIssue::Reason::MaterialNotPublished);
    }
}

TEST_CASE("A mixed component that is itself a mixed slot is not reported", "[PublishSettings]")
{
    DynamicPrintConfig project;
    // Slot 0 is mixed from slot 2 (1-based), which is itself a mixed slot.
    project.set_key_value("filament_mixed_components", new ConfigOptionStrings({ "2", "" }));

    PublishSelection selection;
    PublishOwnerInfo mixed;
    mixed.mixed = true;
    selection.set_owner_info(0, mixed);
    selection.set_owner_info(1, mixed);

    // A mixed slot has no "Type" requirement row of its own.
    const std::vector<PublishRowId> rows;

    CHECK(unpublished_mixed_components(selection, rows, project).empty());
}

TEST_CASE("A checked Type requirement row clears the mixed dependency", "[PublishSettings]")
{
    DynamicPrintConfig project;
    project.set_key_value("filament_mixed_components", new ConfigOptionStrings({ "2", "" }));

    PublishSelection selection;
    PublishOwnerInfo mixed;
    mixed.mixed = true;
    selection.set_owner_info(0, mixed);
    PublishOwnerInfo component;
    component.filament_type = "PLA";
    selection.set_owner_info(1, component);
    selection.set_owner_enabled(1, true);

    const PublishRowId type_row =
        make_publish_row({ "filament_type" }, PublishSection::Material, 1, -1, PublishRowKind::Type);
    const std::vector<PublishRowId> rows{ type_row };
    selection.set_checked(type_row, true);
    selection.set_enabled(type_row, true);

    CHECK(unpublished_mixed_components(selection, rows, project).empty());
}

TEST_CASE("A component the dialog does not show is not reported", "[PublishSettings]")
{
    DynamicPrintConfig project;
    // Slot 0 is mixed from slot 2 (1-based); no owner is registered for slot 1, so the dialog
    // has no row for it and cannot have left it unpublished.
    project.set_key_value("filament_mixed_components", new ConfigOptionStrings({ "2" }));

    PublishSelection selection;
    PublishOwnerInfo mixed;
    mixed.mixed = true;
    selection.set_owner_info(0, mixed);

    const std::vector<PublishRowId> rows;

    CHECK(unpublished_mixed_components(selection, rows, project).empty());
}

TEST_CASE("The printer rule uses the project's nozzle type", "[PublishSettings]")
{
    // One extruder running a Standard nozzle; the preset lists Standard then High Flow.
    const DynamicPrintConfig target = make_extruder_target("printer_extruder_id", "printer_extruder_variant", { 1, 1 },
                                                          { "Direct Drive Standard", "Direct Drive High Flow" });
    DynamicPrintConfig printer;
    printer.set_key_value("extruder_type", new ConfigOptionEnumsGeneric({ int(etDirectDrive), int(etDirectDrive) }));
    DynamicPrintConfig project;
    project.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric({ int(nvtStandard) }));

    const std::vector<PublishExtruderEntry> entries =
        publish_extruder_rule(target, printer, project, 0, false, "printer_extruder_id", "printer_extruder_variant");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].nozzle_type == int(nvtStandard));
    CHECK(entries[0].column == 0);

    // The second extruder has no project nozzle type, so it is out of bounds rather than column 0.
    CHECK(publish_extruder_rule(target, printer, project, 1, false, "printer_extruder_id", "printer_extruder_variant").empty());
}

TEST_CASE("The printer rule treats a hybrid nozzle as standard", "[PublishSettings]")
{
    const DynamicPrintConfig target = make_extruder_target("printer_extruder_id", "printer_extruder_variant", { 1, 1 },
                                                          { "Direct Drive Standard", "Direct Drive High Flow" });
    DynamicPrintConfig printer;
    printer.set_key_value("extruder_type", new ConfigOptionEnumsGeneric({ int(etDirectDrive) }));
    DynamicPrintConfig project;
    project.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric({ int(nvtHybrid) }));

    const std::vector<PublishExtruderEntry> entries =
        publish_extruder_rule(target, printer, project, 0, false, "printer_extruder_id", "printer_extruder_variant");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].nozzle_type == int(nvtStandard));
    CHECK(entries[0].column == 0);
}

TEST_CASE("The process rule gives two entries for a hybrid extruder", "[PublishSettings]")
{
    const DynamicPrintConfig target = make_extruder_target("print_extruder_id", "print_extruder_variant", { 1, 1 },
                                                          { "Direct Drive Standard", "Direct Drive High Flow" });
    DynamicPrintConfig printer;
    printer.set_key_value("extruder_type", new ConfigOptionEnumsGeneric({ int(etDirectDrive) }));
    DynamicPrintConfig project;
    project.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric({ int(nvtHybrid) }));

    const std::vector<PublishExtruderEntry> entries =
        publish_extruder_rule(target, printer, project, 0, true, "print_extruder_id", "print_extruder_variant");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].nozzle_type == int(nvtStandard));
    CHECK(entries[0].column == 0);
    CHECK(entries[1].nozzle_type == int(nvtHighFlow));
    CHECK(entries[1].column == 1);
}

TEST_CASE("The extruder rule omits a column the preset does not define", "[PublishSettings]")
{
    // The project asks for a High Flow nozzle and the preset only defines Standard.
    const DynamicPrintConfig target = make_extruder_target("printer_extruder_id", "printer_extruder_variant", { 1 },
                                                          { "Direct Drive Standard" });
    DynamicPrintConfig printer;
    printer.set_key_value("extruder_type", new ConfigOptionEnumsGeneric({ int(etDirectDrive) }));
    DynamicPrintConfig project;
    project.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric({ int(nvtHighFlow) }));

    CHECK(publish_extruder_rule(target, printer, project, 0, false, "printer_extruder_id", "printer_extruder_variant").empty());
}

TEST_CASE("A loaded bundle's variant keys keep their per-column values in the uncompacted full config",
          "[PublishSettings]")
{
    // The dialog reads "#N" values from the real tabs, so every variant column a tab keys must
    // survive full_config(false) with the preset's own value at that index.
    PresetBundle bundle;
    add_inmemory_preset(bundle.filaments, "My PLA");
    bundle.filament_presets = { "My PLA" };

    Preset &printer = add_inmemory_preset(bundle.printers, "Variant Printer");
    printer.config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = { 0.4, 0.4 };
    printer.config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = { int(etDirectDrive),
                                                                                       int(etDirectDrive) };
    printer.config.option<ConfigOptionInts>("printer_extruder_id", true)->values = { 1, 1 };
    printer.config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {
        "Direct Drive Standard", "Direct Drive High Flow"
    };
    printer.config.option<ConfigOptionFloats>("retraction_length", true)->values = { 0.8, 0.9 };
    printer.config.option<ConfigOptionFloats>("z_hop", true)->values = { 0.2, 0.4 };
    printer.config.option<ConfigOptionBools>("wipe", true)->values = { true, false };
    bundle.printers.select_preset_by_name("Variant Printer", true);

    Preset &process = add_inmemory_preset(bundle.prints, "Variant Process");
    process.config.option<ConfigOptionInts>("print_extruder_id", true)->values = { 1, 1 };
    process.config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {
        "Direct Drive Standard", "Direct Drive High Flow"
    };
    process.config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 200.0, 150.0 };
    process.config.option<ConfigOptionFloats>("initial_layer_speed", true)->values = { 45.0, 30.0 };
    process.config.option<ConfigOptionBools>("enable_overhang_speed", true)->values = { true, false };
    bundle.prints.select_preset_by_name("Variant Process", true);

    const DynamicPrintConfig &printer_config = bundle.printers.get_edited_preset().config;
    const DynamicPrintConfig &print_config   = bundle.prints.get_edited_preset().config;
    const DynamicPrintConfig  uncompacted    = bundle.full_config(false);

    // Per-element serialized values, the text a "#N" key resolves to.
    auto columns = [](const DynamicPrintConfig &config, const std::string &key) {
        const auto *values = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        return values == nullptr ? std::vector<std::string>{} : values->vserialize();
    };

    // Both presets must actually be the selected ones, with the two-column layout set above.
    REQUIRE(columns(printer_config, "retraction_length").size() == 2);
    REQUIRE(columns(print_config, "outer_wall_speed").size() == 2);

    std::set<std::string> keys = print_options_with_variant;
    keys.insert(printer_options_with_variant_1.begin(), printer_options_with_variant_1.end());

    // The Printer section reads every allowlisted key as "key#column", so the allowlist has to be
    // inside the per-variant set; a key outside it would index a shared array by a column.
    for (const std::string &key : publishable_printer_keys())
        CHECK(printer_options_with_variant_1.count(key) == 1);

    for (const std::string &key : keys) {
        const bool printer_key = printer_options_with_variant_1.count(key) != 0;
        const std::vector<std::string> expected = columns(printer_key ? printer_config : print_config, key);
        if (expected.empty())
            continue;

        const std::vector<std::string> actual = columns(uncompacted, key);
        INFO("key " << key);
        REQUIRE(actual.size() == expected.size());
        for (size_t n = 0; n < expected.size(); ++n)
            CHECK(actual[n] == expected[n]);
    }

    // Every column the extruder rule resolves for a printer row must index the uncompacted
    // vector the dialog reads, for every allowlisted key (widths are per key).
    const std::vector<PublishExtruderEntry> printer_rule =
        publish_extruder_rule(printer_config, printer_config, bundle.project_config, 0, false,
                              "printer_extruder_id", "printer_extruder_variant");
    REQUIRE(printer_rule.size() == 1);
    for (const std::string &key : publishable_printer_keys()) {
        const ConfigOption *option = uncompacted.option(key);
        REQUIRE(option != nullptr);
        const auto *values = dynamic_cast<const ConfigOptionVectorBase *>(option);
        if (values == nullptr)
            continue; // a scalar option is addressed whole
        for (const PublishExtruderEntry &entry : printer_rule) {
            INFO("key " << key);
            CHECK(entry.column < values->size());
        }
    }

    // full_config(false) applies the project config after the presets, so a project override
    // reaches the dialog's snapshot while the preset keeps its own value.
    bundle.project_config.set_key_value("retraction_length", new ConfigOptionFloats({ 1.1, 1.2 }));
    const DynamicPrintConfig overridden = bundle.full_config(false);
    const auto *overridden_length = dynamic_cast<const ConfigOptionFloats *>(overridden.option("retraction_length"));
    const auto *preset_length     = dynamic_cast<const ConfigOptionFloats *>(printer_config.option("retraction_length"));
    REQUIRE(overridden_length != nullptr);
    REQUIRE(preset_length != nullptr);
    check_double_vector(overridden_length->values, { 1.1, 1.2 });
    check_double_vector(preset_length->values, { 0.8, 0.9 });
}

TEST_CASE("A material variant row resolves to its slot's preset and local variant index", "[PublishSettings]")
{
    const DynamicPrintConfig uncompacted = make_shared_variant_config();
    const DynamicPrintConfig preset      = make_filament_preset_config(2);

    // Slot 0 owns file columns 0 and 1, which are also its local indices.
    const PublishWriteLocation slot0_high = publish_write_location(
        make_publish_row({ "filament_retraction_length#1" }, PublishSection::Material, 0, 1), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(slot0_high.target == PublishWriteTarget::FilamentPreset);
    CHECK(slot0_high.owner == 0);
    CHECK(slot0_high.option_index == 1);

    // Slot 1 shares the preset but owns file columns 2 and 3: its rows address its own variants,
    // never the file column.
    const PublishWriteLocation slot1_low = publish_write_location(
        make_publish_row({ "filament_retraction_length#2" }, PublishSection::Material, 1, 2), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(slot1_low.target == PublishWriteTarget::FilamentPreset);
    CHECK(slot1_low.owner == 1);
    CHECK(slot1_low.option_index == 0);

    const PublishWriteLocation slot1_high = publish_write_location(
        make_publish_row({ "filament_retraction_length#3" }, PublishSection::Material, 1, 3), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(slot1_high.target == PublishWriteTarget::FilamentPreset);
    CHECK(slot1_high.owner == 1);
    CHECK(slot1_high.option_index == 1);

    // A column no slot defines is no address, not a clamped one.
    const PublishWriteLocation beyond = publish_write_location(
        make_publish_row({ "filament_retraction_length#4" }, PublishSection::Material, 1, 4), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(beyond.target == PublishWriteTarget::None);
}

TEST_CASE("A slot-level material row resolves to the first option index", "[PublishSettings]")
{
    const DynamicPrintConfig uncompacted = make_shared_variant_config();
    const DynamicPrintConfig preset      = make_filament_preset_config(2);

    const PublishWriteLocation location = publish_write_location(
        make_publish_row({ "filament_cost" }, PublishSection::Material, 1, -1), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(location.target == PublishWriteTarget::FilamentPreset);
    CHECK(location.owner == 1);
    CHECK(location.option_index == 0);

    // A key the target preset does not carry has no address.
    const PublishWriteLocation missing = publish_write_location(
        make_publish_row({ "filament_flow_ratio" }, PublishSection::Material, 1, -1), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(missing.target == PublishWriteTarget::None);
}

TEST_CASE("A merged material row resolves from its variant key", "[PublishSettings]")
{
    // A Filament row mixes a variant-bearing key with a per-slot key; the variant key carries the
    // column, so neither the plain key nor the file column decides the index.
    const DynamicPrintConfig uncompacted = make_shared_variant_config();
    const DynamicPrintConfig preset      = make_filament_preset_config(2);

    const PublishWriteLocation location = publish_write_location(
        make_publish_row({ "fan_min_speed#3", "filament_cost" }, PublishSection::Material, 1, 3), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(location.target == PublishWriteTarget::FilamentPreset);
    CHECK(location.owner == 1);
    CHECK(location.option_index == 1);
}

TEST_CASE("A printer row resolves to the edited printer preset and its column", "[PublishSettings]")
{
    DynamicPrintConfig uncompacted = make_variant_config();
    uncompacted.option<ConfigOptionFloats>("retraction_length", true)->values = { 0.8, 0.9 };
    const DynamicPrintConfig preset; // printer rows address the printer preset, not the owner's

    const PublishWriteLocation location = publish_write_location(
        make_publish_row({ "retraction_length#1" }, PublishSection::Printer, -1, 1), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(location.target == PublishWriteTarget::PrinterPreset);
    CHECK(location.owner == -1);
    CHECK(location.option_index == 1);

    // A column the config does not hold is no address, not a clamped one.
    const PublishWriteLocation beyond = publish_write_location(
        make_publish_row({ "retraction_length#2" }, PublishSection::Printer, -1, 2), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(beyond.target == PublishWriteTarget::None);
}

TEST_CASE("A process row resolves to the edited print preset and its column", "[PublishSettings]")
{
    DynamicPrintConfig uncompacted = make_variant_config();
    uncompacted.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 200.0, 150.0 };
    const DynamicPrintConfig preset; // process rows address the print preset

    const PublishWriteLocation location = publish_write_location(
        make_publish_row({ "outer_wall_speed#1" }, PublishSection::Process, -1, 1), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(location.target == PublishWriteTarget::ProcessPreset);
    CHECK(location.owner == -1);
    CHECK(location.option_index == 1);

    // A value with no per-column dimension is addressed whole.
    const PublishWriteLocation scalar = publish_write_location(
        make_publish_row({ "layer_height" }, PublishSection::Process, -1, -1), uncompacted, preset,
        /*full_owner=*/false);
    CHECK(scalar.target == PublishWriteTarget::ProcessPreset);
    CHECK(scalar.option_index == 0);
}

TEST_CASE("A Full Publish row has no write target", "[PublishSettings]")
{
    const DynamicPrintConfig uncompacted = make_shared_variant_config();
    const DynamicPrintConfig preset      = make_filament_preset_config(2);

    // The owner's whole preset publishes; its per-key rows are not written one by one.
    const PublishWriteLocation location = publish_write_location(
        make_publish_row({ "filament_retraction_length#3" }, PublishSection::Material, 1, 3), uncompacted, preset,
        /*full_owner=*/true);
    CHECK(location.target == PublishWriteTarget::None);
    CHECK(location.owner == -1);
}

TEST_CASE("A requirement row has no write target", "[PublishSettings]")
{
    const DynamicPrintConfig uncompacted = make_shared_variant_config();
    const DynamicPrintConfig preset      = make_filament_preset_config(2);

    // Color and Type rows carry no config key of their own.
    CHECK(publish_write_location(make_publish_row({}, PublishSection::Material, 0, -1, PublishRowKind::Type),
                                 uncompacted, preset, /*full_owner=*/false)
              .target == PublishWriteTarget::None);
    CHECK(publish_write_location(make_publish_row({}, PublishSection::Material, 0, -1, PublishRowKind::Color),
                                 uncompacted, preset, /*full_owner=*/false)
              .target == PublishWriteTarget::None);
}

TEST_CASE("A row whose column falls back to the first variant has no write target", "[PublishSettings]")
{
    DynamicPrintConfig uncompacted = make_variant_config();
    uncompacted.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtHighFlow) };

    // Slot 1 defines Standard only, so the file carries its first column as a fallback.
    const PublishResolvedColumn resolved = publish_resolved_column(uncompacted, 1);
    REQUIRE_FALSE(resolved.exact);
    REQUIRE(resolved.file_column == std::optional<size_t>(2));

    const DynamicPrintConfig slot1_preset = make_filament_preset_config(1);
    const PublishWriteLocation fallback   = publish_write_location(
        make_publish_row({ "filament_retraction_length#2" }, PublishSection::Material, 1, 2), uncompacted, slot1_preset,
        /*full_owner=*/false);
    CHECK(fallback.target == PublishWriteTarget::None);

    // Slot 0 defines the variant the printer needs, so its own row stays writable.
    const DynamicPrintConfig slot0_preset = make_filament_preset_config(2);
    const PublishWriteLocation exact      = publish_write_location(
        make_publish_row({ "filament_retraction_length#1" }, PublishSection::Material, 0, 1), uncompacted, slot0_preset,
        /*full_owner=*/false);
    CHECK(exact.target == PublishWriteTarget::FilamentPreset);
    CHECK(exact.owner == 0);
    CHECK(exact.option_index == 1);
}

TEST_CASE("A mixed definition key resolves to the project config", "[PublishSettings]")
{
    const DynamicPrintConfig uncompacted = make_shared_variant_config();
    const DynamicPrintConfig preset; // mixed keys are project arrays, not preset keys

    const PublishWriteLocation location = publish_write_location(
        make_publish_row({ "filament_mixed_components", "filament_mixed_gradient" }, PublishSection::Material, 0, -1),
        uncompacted, preset, /*full_owner=*/false);
    CHECK(location.target == PublishWriteTarget::Project);
    CHECK(location.owner == -1);
}
