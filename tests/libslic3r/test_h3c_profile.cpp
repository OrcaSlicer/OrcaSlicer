#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "libslic3r/Config.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("H3C fin extrusion temperatures survive config loading and reach custom G-code", "[Config][H3C]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.option<ConfigOptionInts>("fins_extrude_safe_temp")->values == std::vector<int>{0});
    config.set_deserialize_strict("fins_extrude_safe_temp", "57,100");
    PlaceholderParser parser;
    parser.apply_config(config);
    CHECK(parser.process("{fins_extrude_safe_temp[0]}") == "57");
    CHECK(parser.process("{fins_extrude_safe_temp[1]}") == "100");
}

TEST_CASE("TPU285 retains its dedicated nozzle identity in presets and rack statistics", "[Config][H3C]")
{
    CHECK(int(nvtTPU285) == 4);
    CHECK(convert_to_nvt_type("Direct Drive TPU285") == nvtTPU285);
    CHECK(get_extruder_variant_string(etDirectDrive, nvtTPU285) == "Direct Drive TPU285");
    DynamicPrintConfig config;
    config.set_deserialize_strict("nozzle_volume_type", "TPU285");
    CHECK(config.opt_serialize("nozzle_volume_type") == "TPU285");
    const auto stats = get_extruder_nozzle_stats({"Standard#7|High Flow#1|TPU285#1"});
    REQUIRE(stats.size() == 1);
    CHECK(stats.front().at(nvtTPU285) == 1);
    CHECK(save_extruder_nozzle_stats_to_string(stats).front() == "Standard#7|High Flow#1|TPU285#1");
}
