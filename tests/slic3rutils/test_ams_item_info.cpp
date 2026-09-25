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
#include "slic3r/GUI/Widgets/AMSItem.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("AMSinfo::parse_ams_info carries the accurate remaining weight and fetch status to the tray widget", "[AMSItem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // tag_uid must be non-zero for IsBBL_Filament(); state bit 5 (value 32) = Refreshing.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ {
                "id": "0", "info": "00000001",
                "tray": [ { "id": "0", "tag_uid": "1234ABCD", "tray_color": "FF0000FF",
                             "tray_weight": "1000", "remain": 80, "remain_g": 750, "state": 32 } ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    // Set the type directly (see test_dev_mapping.cpp): setting_id_to_type() needs a live preset bundle.
    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    tray->m_fila_type = "PLA";

    DevAms* ams = obj.GetFilaSystem()->GetAmsById("0");
    REQUIRE(ams != nullptr);

    AMSinfo info;
    REQUIRE(info.parse_ams_info(&obj, ams, /*remain_flag=*/true, /*humidity_flag=*/false));
    REQUIRE(info.cans.size() == 1);

    const Caninfo& can = info.cans[0];
    CHECK(can.material_remain == 80);
    REQUIRE(can.material_remain_weight_g.has_value());
    CHECK(can.material_remain_weight_g.value() == 750);
    CHECK(static_cast<int>(can.remain_fetch_status) == static_cast<int>(DevAmsTray::RemainFetchStatus::Refreshing));
}

TEST_CASE("AMSinfo::parse_ams_info leaves the remaining weight unset when remain detection is off", "[AMSItem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ {
                "id": "0", "info": "00000001",
                "tray": [ { "id": "0", "tag_uid": "1234ABCD", "tray_color": "FF0000FF",
                             "tray_weight": "1000", "remain": 80, "remain_g": 750 } ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    tray->m_fila_type = "PLA";

    DevAms* ams = obj.GetFilaSystem()->GetAmsById("0");
    REQUIRE(ams != nullptr);

    AMSinfo info;
    // remain_flag = false: same as remain detection disabled in AMS settings.
    REQUIRE(info.parse_ams_info(&obj, ams, /*remain_flag=*/false, /*humidity_flag=*/false));
    REQUIRE(info.cans.size() == 1);

    const Caninfo& can = info.cans[0];
    CHECK(can.material_remain == 100); // falls back to the "full" default, matching the existing behavior
    CHECK_FALSE(can.material_remain_weight_g.has_value());
}
