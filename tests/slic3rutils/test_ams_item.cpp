// why: match the GUI include order to avoid rpcndr.h byte/std::byte
// ambiguity in the Windows COM headers.
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

#include "slic3r/GUI/Widgets/AMSItem.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Configured empty AMS trays remain distinct from unknown trays", "[AMSItem]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "orca";

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"ams":{
        "ams_exist_bits":"1","tray_exist_bits":"3","ams":[
            {"id":"0","info":"0001","tray":[
                {"id":"0","tag_uid":"0000000000000000","tray_info_idx":"","tray_type":"","tray_color":"00000000"},
                {"id":"1"}
            ]}
        ]
    }}})", false);

    const auto& ams_list = machine.GetFilaSystem()->GetAmsList();
    const auto ams_it = ams_list.find("0");
    REQUIRE(ams_it != ams_list.end());
    auto* ams = ams_it->second;
    REQUIRE(ams != nullptr);
    REQUIRE(ams->GetTray("0") != nullptr);
    CHECK(ams->GetTray("0")->is_exists);
    AMSinfo info;
    REQUIRE(info.parse_ams_info(&machine, ams));
    REQUIRE(info.cans.size() == 2);

    CHECK(info.cans[0].is_empty);
    CHECK(info.cans[0].material_state == AMSCanType::AMS_CAN_TYPE_THIRDBRAND);
    CHECK_FALSE(info.cans[1].is_empty);
    CHECK(info.cans[1].material_state == AMSCanType::AMS_CAN_TYPE_THIRDBRAND);
}

TEST_CASE("Empty external slots remain distinct from unknown slots", "[AMSItem]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "orca";

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[
        {"id":"255","tag_uid":"0000000000000000","tray_info_idx":"","tray_type":"","tray_color":"00000000"},
        {"id":"254"}
    ]}})", false);

    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].is_exists);
    AMSinfo empty_info;
    empty_info.parse_ext_info(&machine, machine.vt_slot[0]);
    CHECK(empty_info.cans[0].is_empty);
    CHECK(empty_info.cans[0].material_state == AMSCanType::AMS_CAN_TYPE_VIRTUAL);

    AMSinfo unknown_info;
    unknown_info.parse_ext_info(&machine, machine.vt_slot[1]);
    CHECK_FALSE(unknown_info.cans[0].is_empty);
    CHECK(unknown_info.cans[0].material_state == AMSCanType::AMS_CAN_TYPE_VIRTUAL);
}
