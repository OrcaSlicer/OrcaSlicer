#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("convert_to_nvt_type maps extruder variant strings to nozzle volume types", "[Config]")
{
    SECTION("Direct Drive variants") {
        REQUIRE(convert_to_nvt_type("Direct Drive Standard") == nvtStandard);
        REQUIRE(convert_to_nvt_type("Direct Drive High Flow") == nvtHighFlow);
        REQUIRE(convert_to_nvt_type("Direct Drive TPU High Flow") == nvtTPUHighFlow);
        REQUIRE(convert_to_nvt_type("Direct Drive E3D High Flow") == nvtE3DHighFlow);
        REQUIRE(convert_to_nvt_type("Direct Drive Extra High Flow") == nvtExtraHighFlow);
    }

    SECTION("Bowden variants") {
        REQUIRE(convert_to_nvt_type("Bowden Standard") == nvtStandard);
        REQUIRE(convert_to_nvt_type("Bowden High Flow") == nvtHighFlow);
        REQUIRE(convert_to_nvt_type("Bowden E3D High Flow") == nvtE3DHighFlow);
        REQUIRE(convert_to_nvt_type("Bowden Extra High Flow") == nvtExtraHighFlow);
    }

    SECTION("Unparsable strings fall back to hybrid") {
        REQUIRE(convert_to_nvt_type("Unknown Extruder") == nvtHybrid);
        REQUIRE(convert_to_nvt_type("") == nvtHybrid);
        REQUIRE(convert_to_nvt_type("High Flow") == nvtHybrid);
        REQUIRE(convert_to_nvt_type("Direct Drive") == nvtHybrid);
    }

    SECTION("Whitespace around the volume-type remainder is trimmed") {
        REQUIRE(convert_to_nvt_type("Direct Drive  High Flow ") == nvtHighFlow);
        REQUIRE(convert_to_nvt_type(" Bowden Standard") == nvtStandard);
    }
}

TEST_CASE("E3D High Flow is nozzle volume type 5, after the reserved 4", "[Config]")
{
    REQUIRE(int(nvtE3DHighFlow) == 5);
    REQUIRE(get_valid_nozzle_volume_type() == std::set<NozzleVolumeType>{nvtStandard, nvtHighFlow, nvtTPUHighFlow, nvtE3DHighFlow, nvtExtraHighFlow});
    REQUIRE(get_nozzle_volume_type_string(nvtE3DHighFlow) == "E3D High Flow");
    REQUIRE(get_extruder_variant_string(etDirectDrive, nvtE3DHighFlow) == "Direct Drive E3D High Flow");
    REQUIRE(get_extruder_variant_string(etBowden, nvtE3DHighFlow) == "Bowden E3D High Flow");

    SECTION("nozzle_volume_type round-trips it by name") {
        DynamicPrintConfig config;
        config.set_deserialize_strict("nozzle_volume_type", "Standard,E3D High Flow");
        REQUIRE(config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values == std::vector<int>{nvtStandard, nvtE3DHighFlow});
        REQUIRE(config.opt_serialize("nozzle_volume_type") == "Standard,E3D High Flow");
    }

    SECTION("its variant is found by value, not mistaken for High Flow") {
        const std::vector<std::string> variant_list = {"Direct Drive Standard", "Direct Drive High Flow", "Direct Drive E3D High Flow"};
        const std::vector<int>         variant_ids  = {1, 1, 1};
        REQUIRE(get_config_index_base(nvtE3DHighFlow, etDirectDrive, 1, variant_list, variant_ids) == 2);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 1, variant_list, variant_ids) == 1);
    }
}

TEST_CASE("Extra High Flow is nozzle volume type 6, after E3D High Flow", "[Config]")
{
    REQUIRE(int(nvtExtraHighFlow) == 6);
    REQUIRE(get_nozzle_volume_type_string(nvtExtraHighFlow) == "Extra High Flow");
    REQUIRE(get_extruder_variant_string(etDirectDrive, nvtExtraHighFlow) == "Direct Drive Extra High Flow");
    REQUIRE(get_extruder_variant_string(etBowden, nvtExtraHighFlow) == "Bowden Extra High Flow");

    SECTION("nozzle_volume_type round-trips it by name") {
        DynamicPrintConfig config;
        config.set_deserialize_strict("nozzle_volume_type", "Standard,Extra High Flow");
        REQUIRE(config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values == std::vector<int>{nvtStandard, nvtExtraHighFlow});
        REQUIRE(config.opt_serialize("nozzle_volume_type") == "Standard,Extra High Flow");
    }

    SECTION("its variant is found by value, not mistaken for High Flow") {
        const std::vector<std::string> variant_list = {"Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Extra High Flow"};
        const std::vector<int>         variant_ids  = {1, 1, 1};
        REQUIRE(get_config_index_base(nvtExtraHighFlow, etDirectDrive, 1, variant_list, variant_ids) == 2);
        REQUIRE(get_config_index_base(nvtHighFlow, etDirectDrive, 1, variant_list, variant_ids) == 1);
    }
}

TEST_CASE("get_extruder_supported_nozzle_volume_types reads the extruder's variant list", "[Config]")
{
    DynamicPrintConfig config;
    // H2C: the left extruder offers E3D High Flow, the right (nozzle rack) extruder does not.
    config.set_deserialize_strict("extruder_type", "Direct Drive,Direct Drive");
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {
        "Direct Drive Standard,Direct Drive High Flow,Direct Drive E3D High Flow",
        "Direct Drive Standard,Direct Drive High Flow"};

    REQUIRE(get_extruder_supported_nozzle_volume_types(config, 0) == std::set<NozzleVolumeType>{nvtStandard, nvtHighFlow, nvtE3DHighFlow});
    REQUIRE(get_extruder_supported_nozzle_volume_types(config, 1) == std::set<NozzleVolumeType>{nvtStandard, nvtHighFlow});

    SECTION("an E3D-only list does not report plain High Flow") {
        config.option<ConfigOptionStrings>("extruder_variant_list")->values[1] = "Direct Drive Standard,Direct Drive E3D High Flow";
        REQUIRE(get_extruder_supported_nozzle_volume_types(config, 1) == std::set<NozzleVolumeType>{nvtStandard, nvtE3DHighFlow});
    }

    SECTION("an Extra High Flow list does not report plain High Flow") {
        config.option<ConfigOptionStrings>("extruder_variant_list")->values[1] = "Direct Drive Standard,Direct Drive Extra High Flow";
        REQUIRE(get_extruder_supported_nozzle_volume_types(config, 1) == std::set<NozzleVolumeType>{nvtStandard, nvtExtraHighFlow});
    }

    SECTION("an extruder past the profile's lists gives the empty, unknown set") {
        REQUIRE(get_extruder_supported_nozzle_volume_types(config, 2).empty());
        REQUIRE(get_extruder_supported_nozzle_volume_types(config, -1).empty());
    }
}
