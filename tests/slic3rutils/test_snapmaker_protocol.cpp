#include <catch2/catch_test_macros.hpp>

#include <string>

#include "slic3r/Utils/DeviceJson.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

// Orca: CARD_UID is an array of tag bytes for a tagged spool and the number 0 otherwise.

TEST_CASE("Reads a CARD_UID reported as an array of tag bytes", "[SnapmakerProtocol]")
{
    const auto slot = nlohmann::json::parse(R"({
        "VERSION": 1, "VENDOR": "ELEGOO", "MANUFACTURER": "ELEGOO", "MAIN_TYPE": "PLA",
        "SUB_TYPE": "Translucent", "BED_TEMP": 60, "FIRST_LAYER_TEMP": 210,
        "OTHER_LAYER_TEMP": 210, "HOTEND_MAX_TEMP": 220, "HOTEND_MIN_TEMP": 210,
        "ARGB_COLOR": 4293519847, "DIAMETER": 175, "OFFICIAL": true,
        "CARD_UID": [4, 214, 239, 50, 197, 42, 129], "SPOOL_ID": "132"})");

    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(slot) == "0004D6EF32C52A81");
    CHECK(Slic3r::json_number_or(slot, "BED_TEMP", 0) == 60);
    CHECK(Slic3r::json_number_or(slot, "FIRST_LAYER_TEMP", 0) == 210);
    CHECK(Slic3r::json_string_or(slot, "VENDOR", "NONE") == "ELEGOO");
}

TEST_CASE("A lane with no tag reports no tag id", "[SnapmakerProtocol]")
{
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"VENDOR":"NONE","CARD_UID":0})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":[0,0,0]})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"VENDOR":"NONE"})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":"04D6EF"})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":[4,"x"]})")).empty());
}

TEST_CASE("A numeric CARD_UID still renders as a 16-digit tag id", "[SnapmakerProtocol]")
{
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":81985529216486895})")) ==
          "0123456789ABCDEF");
}

TEST_CASE("A field of the wrong type falls back instead of throwing", "[SnapmakerProtocol]")
{
    const auto slot = nlohmann::json::parse(R"({"BED_TEMP":"hot","NOZZLE_TEMP":"220","FIRST_LAYER_TEMP":null,"VENDOR":7})");
    CHECK(Slic3r::json_number_or(slot, "BED_TEMP", 0) == 0);
    // A whole number written as a string is still a number.
    CHECK(Slic3r::json_number_or(slot, "NOZZLE_TEMP", 0) == 220);
    CHECK(Slic3r::json_number_or(slot, "FIRST_LAYER_TEMP", 7) == 7);
    CHECK(Slic3r::json_number_or(slot, "MISSING", 42) == 42);
    CHECK(Slic3r::json_string_or(slot, "VENDOR", "NONE") == "NONE");
}
