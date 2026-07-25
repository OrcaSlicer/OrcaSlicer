#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// A 2-extruder printer whose second extruder holds both a Standard and a High Flow nozzle
// (nozzle_volume_type Hybrid), described by extruder_nozzle_stats. The variant lists carry one
// column per (extruder x volume type) as composed from the presets.
DynamicPrintConfig make_hybrid_printer_config()
{
    DynamicPrintConfig config;
    config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1", "Standard#3|High Flow#2"};
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHybrid};
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                 "Direct Drive Standard,Direct Drive High Flow"};
    return config;
}

void add_print_variant_columns(DynamicPrintConfig &config)
{
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1, 1, 2, 2};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                  "Direct Drive Standard", "Direct Drive High Flow"};
    config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {30., 200., 50., 500.};
}

} // namespace

TEST_CASE("apply_override fills nil entries from the 0-based default index", "[Config]")
{
    ConfigOptionFloats machine({10., 20., 30.});
    ConfigOptionFloatsNullable filament;
    filament.values = {ConfigOptionFloatsNullable::nil_value(), 42.};

    SECTION("a nil entry picks the slot addressed by its 0-based index") {
        std::vector<int> slot_index{2, 0};
        ConfigOptionFloats resolved(machine);
        REQUIRE(resolved.apply_override(&filament, slot_index));
        REQUIRE(resolved.values == std::vector<double>({30., 42.}));
    }

    SECTION("an index past the machine slots falls back to the first slot") {
        std::vector<int> slot_index{5, 0};
        ConfigOptionFloats resolved(machine);
        REQUIRE(resolved.apply_override(&filament, slot_index));
        REQUIRE(resolved.values == std::vector<double>({10., 42.}));
    }

    SECTION("a negative index (unresolved slot) falls back to the first slot") {
        std::vector<int> slot_index{-1, 0};
        ConfigOptionFloats resolved(machine);
        REQUIRE(resolved.apply_override(&filament, slot_index));
        REQUIRE(resolved.values == std::vector<double>({10., 42.}));
    }
}

TEST_CASE("get_config_index_base resolves (volume type, extruder type, id) to a slot", "[Config]")
{
    const std::vector<std::string> variant_list = {"Direct Drive Standard", "Direct Drive High Flow",
                                                   "Direct Drive Standard", "Direct Drive High Flow"};
    const std::vector<int> variant_ids = {1, 1, 2, 2};

    SECTION("a matching (variant, id) pair yields its slot") {
        REQUIRE(get_config_index_base(nvtStandard, etDirectDrive, 1, variant_list, variant_ids) == 0);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 1, variant_list, variant_ids) == 1);
        REQUIRE(get_config_index_base(nvtStandard, etDirectDrive, 2, variant_list, variant_ids) == 2);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 2, variant_list, variant_ids) == 3);
    }

    SECTION("no matching column falls back to slot 0") {
        REQUIRE(get_config_index_base(nvtStandard, etDirectDrive, 3, variant_list, variant_ids) == 0);
        REQUIRE(get_config_index_base(nvtStandard, etBowden, 1, variant_list, variant_ids) == 0);
    }

    SECTION("Hybrid is not a preset variant string and falls back to slot 0") {
        REQUIRE(get_config_index_base(nvtHybrid, etDirectDrive, 2, variant_list, variant_ids) == 0);
    }
}

TEST_CASE("get_extruder_nozzle_volume_count reads the per-extruder volume-type layout", "[Config]")
{
    std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;

    SECTION("absent stats fall back to one slot per extruder") {
        DynamicPrintConfig config;
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 2);
        REQUIRE(nozzle_volume_types.size() == 2);
        REQUIRE(nozzle_volume_types[0].empty());
        REQUIRE(nozzle_volume_types[1].empty());
    }

    SECTION("stats sized differently from the extruder count are ignored") {
        DynamicPrintConfig config;
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1"};
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 2);
        REQUIRE(nozzle_volume_types[0].empty());
        REQUIRE(nozzle_volume_types[1].empty());
    }

    SECTION("single volume type per extruder counts one slot each") {
        DynamicPrintConfig config;
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1", "High Flow#1"};
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 2);
        REQUIRE(nozzle_volume_types[0] == std::vector<NozzleVolumeType>{nvtStandard});
        REQUIRE(nozzle_volume_types[1] == std::vector<NozzleVolumeType>{nvtHighFlow});
    }

    SECTION("a mixed-nozzle extruder contributes one slot per volume type, ascending enum order") {
        DynamicPrintConfig config;
        // list High Flow first in the token string: parsing must still order Standard before High Flow
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#3", "High Flow#3|Standard#3"};
        REQUIRE(config.get_extruder_nozzle_volume_count(2, nozzle_volume_types) == 3);
        REQUIRE(nozzle_volume_types[0] == std::vector<NozzleVolumeType>{nvtStandard});
        REQUIRE(nozzle_volume_types[1] == std::vector<NozzleVolumeType>({nvtStandard, nvtHighFlow}));
    }
}

TEST_CASE("update_values_to_printer_extruders expands one slot per (extruder x volume type)", "[Config]")
{
    SECTION("Hybrid extruder yields three slots, extruder-ascending then volume-ascending") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        add_print_variant_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 3);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 50., 500.}));
        REQUIRE(config.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>({1, 2, 2}));
        REQUIRE(config.option<ConfigOptionStrings>("print_extruder_variant")->values ==
                std::vector<std::string>({"Direct Drive Standard", "Direct Drive Standard", "Direct Drive High Flow"}));
    }

    SECTION("stride-2 options keep (normal, silent) pairs together per slot") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        config.option<ConfigOptionInts>("printer_extruder_id", true)->values = {1, 1, 2, 2};
        config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                        "Direct Drive Standard", "Direct Drive High Flow"};
        config.option<ConfigOptionFloats>("machine_max_speed_x", true)->values = {100., 50., 110., 55., 120., 60., 130., 65.};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);

        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("machine_max_speed_x")->values ==
                std::vector<double>({100., 50., 120., 60., 130., 65.}));
    }

    SECTION("single-slot expansion on a Hybrid extruder resolves via the filament volume type") {
        DynamicPrintConfig printer_config = make_hybrid_printer_config();

        DynamicPrintConfig filament_config;
        filament_config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};
        filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20.};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = printer_config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        SECTION("default filament volume type selects the Standard column") {
            std::vector<int> variant_index = filament_config.update_values_to_printer_extruders(printer_config, extruder_count, count,
                nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, 2);
            REQUIRE(variant_index == std::vector<int>({0}));
            REQUIRE(filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12.}));
        }

        SECTION("a High Flow filament volume type selects the High Flow column") {
            std::vector<int> variant_index = filament_config.update_values_to_printer_extruders(printer_config, extruder_count, count,
                nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, 2, nvtHighFlow);
            REQUIRE(variant_index == std::vector<int>({1}));
            REQUIRE(filament_config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({20.}));
        }
    }

    SECTION("an extruder without per-type stats does not overrun the slot table when another is Hybrid") {
        DynamicPrintConfig config;
        // e0 carries no per-type stats (empty entry), so the summed volume-type count (2) does
        // not exceed the extruder count even though the Hybrid e1 emits one slot per volume type.
        config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"", "Standard#3|High Flow#3"};
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHybrid};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        add_print_variant_columns(config);

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 2);
        REQUIRE(nozzle_volume_types[0].empty());

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        // e0 resolves by its configured type; the Hybrid e1 emits one slot per stats volume type
        REQUIRE(variant_index == std::vector<int>({0, 2, 3}));
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 50., 500.}));
    }

    SECTION("without Hybrid or extra slots the expansion matches the per-extruder resolution") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        add_print_variant_columns(config);

        // compute what the per-extruder loop resolves directly, before the arrays are rewritten
        std::vector<int> expected_index;
        for (int e_index = 0; e_index < 2; e_index++)
            expected_index.push_back(config.get_index_for_extruder(e_index + 1, "print_extruder_id", etDirectDrive,
                e_index == 0 ? nvtStandard : nvtHighFlow, "print_extruder_variant"));
        REQUIRE(expected_index == std::vector<int>({0, 3}));

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 2);

        std::vector<int> variant_index = config.update_values_to_printer_extruders(config, extruder_count, count, nozzle_volume_types,
            print_options_with_variant, "print_extruder_id", "print_extruder_variant");

        REQUIRE(variant_index == expected_index);
        REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>({30., 500.}));
    }
}

TEST_CASE("update_values_to_printer_extruders_for_multiple_filaments resolves per-filament slots", "[Config]")
{
    auto make_filament_arrays = [](DynamicPrintConfig &config) {
        config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1, 2, 2};
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow",
                                                                                         "Direct Drive Standard", "Direct Drive High Flow"};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20., 13., 21.};
    };

    std::set<std::string> filament_keys = filament_options_with_variant;
    filament_keys.insert("filament_self_index");

    SECTION("filament_volume_map picks the concrete volume type on a Hybrid extruder") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {2, 2};
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtStandard, nvtHighFlow};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 21.}));
        REQUIRE(config.option<ConfigOptionStrings>("filament_extruder_variant")->values ==
                std::vector<std::string>({"Direct Drive Standard", "Direct Drive High Flow"}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }

    SECTION("a volume map not sized to the filament count is ignored") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {2, 2};
        // the registered default is a single-element vector; it must not override slot resolution
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtStandard};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        // Hybrid resolves as Standard when no usable per-filament map exists
        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 13.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }

    SECTION("a single-filament explicit assignment on a Hybrid extruder is honored") {
        DynamicPrintConfig config = make_hybrid_printer_config();
        config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1};
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12., 20.};
        config.option<ConfigOptionInts>("filament_map", true)->values = {2};
        // sized to the (single) filament count: the producers guarantee sizing, so a
        // single-filament map is as trustworthy as any other and the explicit High Flow
        // request must win over the Hybrid->Standard fallback
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtHighFlow};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({20.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1}));
    }

    SECTION("without Hybrid or extra slots the volume map is not consulted") {
        DynamicPrintConfig config;
        config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHighFlow};
        config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow"};
        make_filament_arrays(config);
        config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
        // sized to the filament count, but inert because no extruder exposes multiple volume types
        config.option<ConfigOptionInts>("filament_volume_map", true)->values = {nvtHighFlow, nvtStandard};

        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        int extruder_count = 2;
        int count = config.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        REQUIRE(count == 2);

        config.update_values_to_printer_extruders_for_multiple_filaments(config, extruder_count, count, filament_keys,
            "filament_self_index", "filament_extruder_variant");

        // filament 1 keeps its extruder's Standard column, filament 2 its extruder's High Flow column
        REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>({12., 21.}));
        REQUIRE(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>({1, 2}));
    }
}
