#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "nlohmann/json.hpp"
#include <string>

#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

using namespace Slic3r;

TEST_CASE("A matching lane forwards its exact string filament id", "[SnapmakerPrinterAgent]")
{
    const std::string expected_id = GENERATE(as<std::string>{}, "Pa123 AbC", "123", "  Pa123  ");
    const nlohmann::json lanes = {{"record", {{"lane", "1"}, {"filament_id", expected_id}}}};
    std::string filament_id;
    std::string vendor = "Generic";

    SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor);

    CHECK(filament_id == expected_id);
    CHECK(vendor == "Generic");
}

TEST_CASE("A lane is matched by its lane value instead of its entry key", "[SnapmakerPrinterAgent]")
{
    const nlohmann::json lanes = {
        {"0", {{"lane", "1"}, {"filament_id", "slot-one"}, {"vendor_name", "Vendor one"}}},
        {"1", {{"lane", "0"}, {"filament_id", "slot-zero"}, {"vendor_name", "Vendor zero"}}}
    };
    const int slot = GENERATE(0, 1);
    std::string filament_id;
    std::string vendor;

    SnapmakerPrinterAgent::apply_lane_data(lanes, slot, filament_id, vendor);

    CHECK(filament_id == (slot == 0 ? "slot-zero" : "slot-one"));
    CHECK(vendor == (slot == 0 ? "Vendor zero" : "Vendor one"));
}

TEST_CASE("Metadata for another slot leaves the filament id and vendor untouched", "[SnapmakerPrinterAgent]")
{
    const nlohmann::json lanes = {{"record", {{"lane", "1"}, {"filament_id", "other-id"}, {"vendor_name", "Other vendor"}}}};
    const int slot = GENERATE(0, 2, 3);
    std::string filament_id = "original-id";
    std::string vendor = "Generic";

    SnapmakerPrinterAgent::apply_lane_data(lanes, slot, filament_id, vendor);

    CHECK(filament_id == "original-id");
    CHECK(vendor == "Generic");
}

TEST_CASE("An empty or nonstring filament id leaves the slot available for fallback", "[SnapmakerPrinterAgent]")
{
    const nlohmann::json id = GENERATE(nlohmann::json(""), nlohmann::json(123), nlohmann::json(1.5),
                                     nlohmann::json(true), nlohmann::json(nullptr),
                                     nlohmann::json::array(), nlohmann::json::object());
    CAPTURE(id);
    const nlohmann::json lanes = {{"record", {{"lane", "1"}, {"filament_id", id}, {"vendor_name", "Polymaker"}}}};
    std::string filament_id;
    std::string vendor = "Generic";

    SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor);

    CHECK(filament_id.empty());
    CHECK(vendor == "Polymaker");
}

TEST_CASE("Malformed lane values do not select a slot", "[SnapmakerPrinterAgent]")
{
    const nlohmann::json lane = GENERATE(nlohmann::json("1x"), nlohmann::json(""), nlohmann::json(1),
                                       nlohmann::json(" 1"), nlohmann::json("1 "), nlohmann::json("+1"),
                                       nlohmann::json("999999999999999999999"), nlohmann::json(nullptr));
    CAPTURE(lane);
    const nlohmann::json lanes = {{"record", {{"lane", lane}, {"filament_id", "wrong-id"}, {"vendor_name", "Wrong vendor"}}}};
    std::string filament_id = "original-id";
    std::string vendor = "Generic";

    SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor);

    CHECK(filament_id == "original-id");
    CHECK(vendor == "Generic");
}

TEST_CASE("Lane vendor fills an empty or generic printer vendor regardless of case and surrounding space", "[SnapmakerPrinterAgent]")
{
    std::string vendor = GENERATE(as<std::string>{}, "", " \t\r\n", "Generic", "NONE", " gEnErIc \t", "\n none \r");
    CAPTURE(vendor);
    const nlohmann::json lanes = {{"record", {{"lane", "1"}, {"vendor_name", " Polymaker "}}}};
    std::string filament_id;

    SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor);

    CHECK(vendor == " Polymaker ");
    CHECK(filament_id.empty());
}

TEST_CASE("A specific printer vendor is preserved even when the lane forwards an id", "[SnapmakerPrinterAgent]")
{
    const std::string original_vendor = GENERATE(as<std::string>{}, "Snapmaker", "  Polymaker  ", "Generic brand");
    std::string vendor = original_vendor;
    std::string filament_id;
    const nlohmann::json lanes = {{"record", {{"lane", "1"}, {"filament_id", "lane-id"}, {"vendor_name", "Other vendor"}}}};

    SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor);

    CHECK(vendor == original_vendor);
    CHECK(filament_id == "lane-id");
}

TEST_CASE("An empty or nonstring lane vendor leaves the printer vendor untouched", "[SnapmakerPrinterAgent]")
{
    const nlohmann::json lane_vendor = GENERATE(nlohmann::json(""), nlohmann::json(123), nlohmann::json(false),
                                              nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json::object());
    CAPTURE(lane_vendor);
    const nlohmann::json lanes = {{"record", {{"lane", "1"}, {"vendor_name", lane_vendor}}}};
    std::string filament_id;
    std::string vendor = "Generic";

    SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor);

    CHECK(vendor == "Generic");
    CHECK(filament_id.empty());
}

TEST_CASE("An empty namespace or nonobject lane leaves the slot untouched", "[SnapmakerPrinterAgent]")
{
    const nlohmann::json lanes = GENERATE(nlohmann::json::object(), nlohmann::json(nullptr),
                                        nlohmann::json{{"record", nullptr}}, nlohmann::json{{"record", 1}},
                                        nlohmann::json{{"record", "lane"}}, nlohmann::json{{"record", nlohmann::json::array()}},
                                        nlohmann::json{{"record", nlohmann::json::object()}});
    CAPTURE(lanes);
    std::string filament_id = "original-id";
    std::string vendor = "Generic";

    REQUIRE_NOTHROW(SnapmakerPrinterAgent::apply_lane_data(lanes, 1, filament_id, vendor));

    CHECK(filament_id == "original-id");
    CHECK(vendor == "Generic");
}
