#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/QidiPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;

TEST_CASE("A filament mapped to a Box slot saves that slot in its tool variable", "[QidiPrinterAgent]")
{
    // Filament 2 is mapped to slot 5; filaments 0 and 1 have no slot.
    // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name needs its own quotes.
    CHECK(QidiPrinterAgent::build_tool_slot_script("[-1, -1, 5]") == "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot5'\"");
}

TEST_CASE("Each mapped filament gets its own tool variable line", "[QidiPrinterAgent]")
{
    CHECK(QidiPrinterAgent::build_tool_slot_script("[3, -1, 0]") ==
          "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot3'\"\n"
          "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot0'\"");
}

TEST_CASE("A mapping without a usable Box slot saves nothing", "[QidiPrinterAgent]")
{
    const std::string mapping = GENERATE(as<std::string>{},
                                         "",           // empty mapping
                                         "[]",         // no filaments
                                         "[-1, -1]",   // no filament has a slot
                                         "[16]",       // past the firmware's slot15
                                         "[\"slot1\"]", // not a slot index
                                         "{\"0\": 1}", // not an array
                                         "[1,");       // not valid JSON
    CAPTURE(mapping);
    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping).empty());
}

TEST_CASE("Filaments past the firmware's last tool variable are skipped", "[QidiPrinterAgent]")
{
    // The firmware has value_t0 to value_t15: filament 15 maps to slot 1, filament 16 to slot 4.
    std::string mapping = "[";
    for (int filament = 0; filament < 15; ++filament)
        mapping += "-1, ";
    mapping += "1, 4]";

    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping) == "SAVE_VARIABLE VARIABLE=value_t15 VALUE=\"'slot1'\"");
}

TEST_CASE("The print start of a 3MF names the plate to print", "[QidiPrinterAgent]")
{
    PrintParams params;
    params.plate_index = 2;

    CHECK(QidiPrinterAgent::print_start_request(params, "Cube.gcode.3mf") ==
          nlohmann::json{{"filename", "Cube.gcode.3mf"}, {"plateindex", "2"}});
    CHECK(QidiPrinterAgent::print_start_request(params, "Cube.gcode") == nlohmann::json{{"filename", "Cube.gcode"}});
}

TEST_CASE("Qidi slot response rejects null variables without throwing", "[QidiPrinterAgent]")
{
    const std::string response = R"({
        "result": {
            "status": {
                "save_variables": {
                    "variables": null
                }
            }
        }
    })";
    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = true;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response(response, status, variables, error));
    CHECK_FALSE(parsed);
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("variables"));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("object"));
}

TEST_CASE("Qidi slot response rejects missing and non-object fields without throwing", "[QidiPrinterAgent]")
{
    std::string response;

    SECTION("missing result")
    {
        response = R"({})";
    }

    SECTION("non-object result")
    {
        response = R"({"result":null})";
    }

    SECTION("missing status")
    {
        response = R"({"result":{}})";
    }

    SECTION("non-object status")
    {
        response = R"({"result":{"status":null}})";
    }

    SECTION("missing save_variables")
    {
        response = R"({"result":{"status":{}}})";
    }

    SECTION("non-object save_variables")
    {
        response = R"({"result":{"status":{"save_variables":null}}})";
    }

    SECTION("missing variables")
    {
        response = R"({"result":{"status":{"save_variables":{}}}})";
    }

    SECTION("scalar")
    {
        response = R"({"result":{"status":{"save_variables":{"variables":42}}}})";
    }

    SECTION("array")
    {
        response = R"({"result":{"status":{"save_variables":{"variables":[]}}}})";
    }

    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = true;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response(response, status, variables, error));
    CHECK_FALSE(parsed);
}

TEST_CASE("Qidi slot response exposes valid status and variables", "[QidiPrinterAgent]")
{
    const std::string response = R"({
        "result": {
            "status": {
                "save_variables": {
                    "variables": {
                        "box_count": 2,
                        "color_slot0": 3
                    }
                },
                "box_stepper slot0": {
                    "runout_button": 0
                }
            }
        }
    })";
    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = false;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response(response, status, variables, error));
    REQUIRE(parsed);
    CHECK(status.is_object());
    CHECK(variables.is_object());
    CHECK(variables.at("box_count") == 2);
    CHECK(status.contains("box_stepper slot0"));
}

TEST_CASE("Qidi slot response rejects invalid JSON", "[QidiPrinterAgent]")
{
    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = true;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response("{not json", status, variables, error));
    CHECK_FALSE(parsed);
    CHECK(error == "Invalid JSON response");
}
