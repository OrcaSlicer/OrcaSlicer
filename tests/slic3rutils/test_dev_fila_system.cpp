// See test_dev_mapping.cpp for why the Windows.h / wx/timer.h include order matters here.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("AMS info bits 30-31 select the remain-estimate version", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // info bits: 0-3 type(1=AMS), 8-11 extruder(0=MAIN), 30-31 remain_estimate_version.
    // AMS 0 leaves bits 30-31 unset (Legacy); AMS 1 sets bit 30 (0x40000000, Accurate).
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "0",
            "ams": [
                { "id": "0", "info": "00000001" },
                { "id": "1", "info": "40000001" }
            ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.count("1") == 1);
    CHECK(static_cast<int>(ams_list.at("0")->GetRemainEstimateVersion()) == static_cast<int>(DevAms::RemainEstimateVersion::Legacy));
    CHECK(static_cast<int>(ams_list.at("1")->GetRemainEstimateVersion()) == static_cast<int>(DevAms::RemainEstimateVersion::Accurate));
}

TEST_CASE("Tray remain_g and state populate accurate weight and fetch status", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // state bits[5-7]: Refreshing = 1 -> 1 << 5 = 32.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ {
                "id": "0", "info": "00000001",
                "tray": [ { "id": "0", "tray_color": "FF0000FF", "tray_weight": "1000", "remain": 50, "remain_g": 420, "state": 32 } ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    CHECK(tray->remain_g == 420);
    CHECK(static_cast<int>(tray->remain_fetch_status) == static_cast<int>(DevAmsTray::RemainFetchStatus::Refreshing));

    // remain_g is accurate and takes priority over the coarse weight * remain% estimate.
    auto weight = tray->get_filament_remain_weight();
    REQUIRE(weight.has_value());
    CHECK(weight.value() == 420);
}

TEST_CASE("get_filament_remain_weight falls back to weight times remain percent without remain_g", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // No "remain_g" or "state" keys: firmware that doesn't report them yet.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ {
                "id": "0", "info": "00000001",
                "tray": [ { "id": "0", "tray_color": "FF0000FF", "tray_weight": "1000", "remain": 50 } ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    CHECK(tray->remain_g == -1);
    CHECK(static_cast<int>(tray->remain_fetch_status) == static_cast<int>(DevAmsTray::RemainFetchStatus::Done));

    auto weight = tray->get_filament_remain_weight();
    REQUIRE(weight.has_value());
    CHECK(weight.value() == 500); // 1000g * 50%
}

TEST_CASE("GetTrayIdByAmsSlotId and GetTrayNameByTrayId round-trip the tray index map", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "3",
            "ams": [ {
                "id": "0", "info": "00000001",
                "tray": [
                    { "id": "0", "tray_color": "FF0000FF" },
                    { "id": "1", "tray_color": "00FF00FF" }
                ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    // AMS 0 slot 1 -> linear tray index (0*4+1) = 1 -> name "A2" (A=ams 0, slot 1 -> 2nd digit).
    int tray_id = obj.GetFilaSystem()->GetTrayIdByAmsSlotId(0, 1);
    CHECK(tray_id == 1);
    CHECK(obj.GetFilaSystem()->GetTrayNameByTrayId(tray_id) == "A2");

    CHECK(obj.GetFilaSystem()->GetTrayIdByAmsSlotId(5, 0) == -1);
    CHECK(obj.GetFilaSystem()->GetTrayNameByTrayId(999) == "");
    CHECK(obj.GetFilaSystem()->GetTrayNameByTrayId(VIRTUAL_TRAY_MAIN_ID) == "Ext");
}

TEST_CASE("GetCurrentExtruderIdByAmsId resolves a switch-bound AMS via the active extruder slot", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    obj.GetFilaSwitch()->ParseFilaSwitchInfo(json::parse(R"({"aux":"20000000"})"));
    REQUIRE(obj.GetFilaSwitch()->IsInstalled());

    // AMS 0: switch-bound (extruder nibble 0xE), input track A (bind_switch_in=1) -> bound to both extruders.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ { "id": "0", "info": "01000E01", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);
    REQUIRE(obj.GetFilaSystem()->GetAmsList().at("0")->GetBindedExtruderSet().size() == 2);

    // Not yet resolvable: neither extruder's active slot points at AMS 0 yet.
    CHECK_FALSE(obj.GetFilaSystem()->GetCurrentExtruderIdByAmsId("0").has_value());

    // DEPUTY extruder (id 1)'s current slot (snow) now points at AMS 0: snow = (ams_id<<8)|slot_id = 0.
    // MAIN extruder (id 0) stays pointed at a different AMS (5) so only the DEPUTY match applies.
    json extruder_json = json::parse(R"({
        "state": 2,
        "info": [
            { "id": 0, "filam_bak": [], "info": 0, "temp": 0, "spre": 0, "snow": 1280, "star": 0, "stat": 0, "hnow": 0 },
            { "id": 1, "filam_bak": [], "info": 0, "temp": 0, "spre": 0, "snow": 0,    "star": 0, "stat": 0, "hnow": 0 }
        ]
    })");
    ExtderSystemParser::ParseV2_0(extruder_json, obj.GetExtderSystem());

    auto ext_id = obj.GetFilaSystem()->GetCurrentExtruderIdByAmsId("0");
    REQUIRE(ext_id.has_value());
    CHECK(ext_id.value() == DEPUTY_EXTRUDER_ID);

    CHECK_FALSE(obj.GetFilaSystem()->GetCurrentExtruderIdByAmsId("no-such-ams").has_value());
}
