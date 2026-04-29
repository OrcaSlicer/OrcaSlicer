#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("get_extruder_nozzle_stats parses Standard#0 as zero count", "[Config][H2C]") {
    auto parsed = get_extruder_nozzle_stats({"Standard#0", "Standard#0"});
    REQUIRE(parsed.size() == 2);
    REQUIRE(parsed[0].size() == 1);
    REQUIRE(parsed[0].at(nvtStandard) == 0);
    REQUIRE(parsed[1].at(nvtStandard) == 0);
}

TEST_CASE("get_extruder_nozzle_stats handles empty string as no entries", "[Config][H2C]") {
    auto parsed = get_extruder_nozzle_stats({"", ""});
    REQUIRE(parsed.size() == 2);
    REQUIRE(parsed[0].empty());
    REQUIRE(parsed[1].empty());
}

TEST_CASE("save_extruder_nozzle_stats_to_string drops zero-count entries", "[Config][H2C]") {
    std::vector<std::map<NozzleVolumeType, int>> stats = {
        { {nvtStandard, 0} },
        { {nvtStandard, 0} },
    };
    auto out = save_extruder_nozzle_stats_to_string(stats);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].empty());
    REQUIRE(out[1].empty());
}

TEST_CASE("save_extruder_nozzle_stats_to_string keeps positive counts", "[Config][H2C]") {
    std::vector<std::map<NozzleVolumeType, int>> stats = {
        { {nvtStandard, 1} },
        { {nvtStandard, 4} },
    };
    auto out = save_extruder_nozzle_stats_to_string(stats);
    REQUIRE(out[0] == "Standard#1");
    REQUIRE(out[1] == "Standard#4");
}

TEST_CASE("save then parse preserves positive counts and drops zeros", "[Config][H2C]") {
    std::vector<std::map<NozzleVolumeType, int>> stats = {
        { {nvtStandard, 1}, {nvtHighFlow, 0} },
        { {nvtStandard, 4} },
    };
    auto serialised = save_extruder_nozzle_stats_to_string(stats);
    auto round_tripped = get_extruder_nozzle_stats(serialised);
    REQUIRE(round_tripped.size() == 2);
    REQUIRE(round_tripped[0].size() == 1);
    REQUIRE(round_tripped[0].at(nvtStandard) == 1);
    REQUIRE(round_tripped[0].count(nvtHighFlow) == 0);
    REQUIRE(round_tripped[1].at(nvtStandard) == 4);
}
