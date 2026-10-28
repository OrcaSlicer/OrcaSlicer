#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

#include "libslic3r/Config.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

TEST_CASE("setting values serialize the way their options read them back", "[Mcp]")
{
    CHECK(to_config_string(json(3)) == "3");
    CHECK(to_config_string(json(0.2)) == "0.2");
    CHECK(to_config_string(json(true)) == "1");
    CHECK(to_config_string(json("15%")) == "15%");
    CHECK(to_config_string(json::array({0.4, 0.6}), "nozzle_diameter") == "0.4,0.6");

    // A list of strings whose entries contain the separators survives the round trip.
    const std::vector<std::string> scripts{"/usr/bin/fix --mode=a,b", "second; with a semicolon"};
    Slic3r::ConfigOptionStrings    parsed;
    parsed.deserialize(to_config_string(json(scripts), "post_process"));
    CHECK(parsed.values == scripts);

    CHECK_THROWS_AS(to_config_string(json::object()), ToolFailure);
}
