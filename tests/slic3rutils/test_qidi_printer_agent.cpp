#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include <slic3r/Utils/QidiPrinterAgent.hpp>

#include <string>

using namespace Slic3r;

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
