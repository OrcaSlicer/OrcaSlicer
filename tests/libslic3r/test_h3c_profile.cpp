#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "libslic3r/Config.hpp"
#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("H3C fin extrusion temperatures survive config loading and reach custom G-code", "[Config][H3C]")
{
    PrintConfig config;
    REQUIRE(config.fins_extrude_safe_temp.values == std::vector<int>{0});
    config.set_deserialize_strict("fins_extrude_safe_temp", "57,100");
    PlaceholderParser parser;
    parser.apply_config(config);
    CHECK(parser.process("{fins_extrude_safe_temp[0]}") == "57");
    CHECK(parser.process("{fins_extrude_safe_temp[1]}") == "100");
}
