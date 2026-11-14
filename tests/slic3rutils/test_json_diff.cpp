#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

using json = nlohmann::json;

TEST_CASE("An empty delta restores cached device status and preserves subsequent updates", "[JsonDiff][Regression]")
{
    const auto empty_delta = GENERATE(json{}, json::object());
    json full = {{"print", {{"gcode_state", "RUNNING"}, {"mc_percent", 25}, {"mc_remaining_time", 10}}}};
    json_diff decoder;
    REQUIRE(decoder.diff2all_base_reset(full) == 0);
    json restored;
    REQUIRE(decoder.diff2all(empty_delta, restored) == 0);
    CHECK(restored == full);
    REQUIRE(decoder.diff2all({{"print", {{"mc_percent", 26}}}}, restored) == 0);
    CHECK(restored["print"]["mc_percent"] == 26);
    CHECK(restored["print"]["gcode_state"] == "RUNNING");
    CHECK(restored["print"]["mc_remaining_time"] == 10);
}

TEST_CASE("A null field in a device delta still replaces its previous value", "[JsonDiff][Regression]")
{
    json full = {{"print", {{"subtask_name", "Part"}, {"mc_percent", 25}}}};
    json_diff decoder;
    REQUIRE(decoder.diff2all_base_reset(full) == 0);
    json restored;
    REQUIRE(decoder.diff2all({{"print", {{"subtask_name", nullptr}}}}, restored) == 0);
    CHECK(restored["print"]["subtask_name"].is_null());
    CHECK(restored["print"]["mc_percent"] == 25);
}
