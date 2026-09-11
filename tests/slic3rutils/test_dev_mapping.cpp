// Match the include environment that libslic3r_gui TUs get from pchheader.hpp: Windows.h with
// WIN32_LEAN_AND_MEAN/NOMINMAX must come first so rpcndr.h's `byte` is processed before <cstddef>
// makes std::byte a competing candidate (otherwise the Windows COM headers pulled in via
// DeviceManager.hpp error with an ambiguous `byte`). wx/timer.h must precede DeviceManager.hpp,
// which includes DeviceErrorDialog.hpp (uses wxTimerEvent) before its own wx/timer.h include.
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
#include "slic3r/GUI/DeviceCore/DevMapping.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("Switch-bound AMS trays map to the left extruder", "[DevMapping]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // aux bit 29 = Filament Track Switch installed (DevFilaSwitch.cpp:69-77)
    obj.GetFilaSwitch()->ParseFilaSwitchInfo(json::parse(R"({"aux":"20000000"})"));
    REQUIRE(obj.GetFilaSwitch()->IsInstalled());

    // info bits: 0-3 type(1=AMS), 8-11 extruder(0xE=switch-bound), 24-27 bind_switch_in(0)
    // tray_exist_bits bit 0 marks AMS 0 / tray 0 present so the mapping result survives the
    // is_exists check in is_valid_mapping_result (DevFilaSystem.cpp:769).
    // tray_info_idx/tray_type are intentionally omitted: the tray parse resolves the display
    // filament type via MachineObject::setting_id_to_type(), which reads the GUI preset bundle
    // (wxGetApp().preset_bundle) — unavailable in this headless unit test. The tray filament
    // type (only needed for the type-match below) is set directly after the parse instead.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ {
                "id": "0",
                "info": "00000E01",
                "tray": [ { "id": "0", "tray_color": "FF0000FF" } ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.at("0")->GetBindedExtruderSet().count(MAIN_EXTRUDER_ID) == 1);
    REQUIRE(ams_list.at("0")->GetBindedExtruderSet().count(DEPUTY_EXTRUDER_ID) == 1);

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    tray->m_fila_type = "PLA";

    FilamentInfo fila;
    fila.id    = 0;
    fila.type  = "PLA";
    fila.color = "FF0000FF";

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);       // MappingOption: LEFT_AMS,RIGHT_AMS,LEFT_EXT,RIGHT_EXT (DevMapping.h:13-19)
    map_opt[MappingOption::USE_LEFT_AMS] = true;

    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);

    // A switch-bound AMS feeds BOTH extruders, so a left-only mapping request
    // must still land the filament on the AMS tray.
    REQUIRE(result.size() == 1);
    CHECK(result[0].tray_id == 0);
    CHECK(result[0].ams_id == "0");
}

TEST_CASE("Without a switch the binding set equals the single bound extruder", "[DevMapping]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    REQUIRE_FALSE(obj.GetFilaSwitch()->IsInstalled());

    // AMS 0: info extruder nibble = MAIN (right). AMS 1: nibble = DEPUTY (left).
    // AMS 2: no "info" key at all (old X1/P1 firmware) -> must default to MAIN.
    // tray_exist_bits: bit ams_id*4+tray_id -> 0x111 marks tray 0 of each AMS.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "111",
            "ams": [
                { "id": "0", "info": "00000001", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] },
                { "id": "1", "info": "00000101", "tray": [ { "id": "0", "tray_color": "00FF00FF" } ] },
                { "id": "2",                     "tray": [ { "id": "0", "tray_color": "0000FFFF" } ] }
            ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    // The invariant that keeps the binding-set mapping filter behavior-preserving for
    // ordinary printers: GetBindedExtruderSet() == { GetExtruderId() }, info key or not.
    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.count("1") == 1);
    REQUIRE(ams_list.count("2") == 1);
    for (const char* id : {"0", "1", "2"}) {
        const auto& ams = ams_list.at(id);
        INFO("ams " << id);
        REQUIRE(ams->GetBindedExtruderSet().size() == 1);
        REQUIRE(ams->GetBindedExtruderSet().count(ams->GetExtruderId()) == 1);
    }
    REQUIRE(ams_list.at("0")->GetExtruderId() == MAIN_EXTRUDER_ID);
    REQUIRE(ams_list.at("1")->GetExtruderId() == DEPUTY_EXTRUDER_ID);
    REQUIRE(ams_list.at("2")->GetExtruderId() == MAIN_EXTRUDER_ID);

    for (const char* id : {"0", "1", "2"}) {
        DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray(id, "0");
        REQUIRE(tray != nullptr);
        tray->m_fila_type = "PLA";
    }

    // A left-only request must exclude the MAIN-bound AMSes: the red filament exactly
    // matches AMS 0's red tray, so landing anywhere but AMS 1 (or unmapped) means the
    // exclusion is broken.
    FilamentInfo fila;
    fila.id    = 0;
    fila.type  = "PLA";
    fila.color = "FF0000FF";

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);
    map_opt[MappingOption::USE_LEFT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);
    REQUIRE(result.size() == 1);
    CHECK(result[0].ams_id != "0");
    CHECK(result[0].ams_id != "2");

    // The mirrored right-only request maps to the exact-match MAIN-bound AMS.
    result.clear();
    map_opt[MappingOption::USE_LEFT_AMS]  = false;
    map_opt[MappingOption::USE_RIGHT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);
    REQUIRE(result.size() == 1);
    CHECK(result[0].ams_id == "0");
}

TEST_CASE("Switch-bound AMS with an invalid track is excluded from mapping", "[DevMapping]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    obj.GetFilaSwitch()->ParseFilaSwitchInfo(json::parse(R"({"aux":"20000000"})"));
    REQUIRE(obj.GetFilaSwitch()->IsInstalled());

    // info bits 24-27 = 0xF: switch-bound (0xE) but the input track is not yet valid -
    // the transient while the device is still homing the switch. The AMS must survive
    // (display keeps working) with an EMPTY binding set that excludes it from mapping.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ { "id": "0", "info": "0F000E01", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.at("0")->GetBindedExtruderSet().empty());
    REQUIRE_FALSE(ams_list.at("0")->GetSwitcherPos().has_value());
    REQUIRE_FALSE(obj.GetFilaSwitch()->IsReady());

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    tray->m_fila_type = "PLA";

    FilamentInfo fila;
    fila.id    = 0;
    fila.type  = "PLA";
    fila.color = "FF0000FF";

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);
    map_opt[MappingOption::USE_LEFT_AMS]  = true;
    map_opt[MappingOption::USE_RIGHT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);
    REQUIRE(result.size() == 1);
    CHECK(result[0].tray_id == -1);

    // Without the switch, the same 0xE AMS is dropped from the list entirely.
    MachineObject obj_no_switch(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    REQUIRE_FALSE(obj_no_switch.GetFilaSwitch()->IsInstalled());
    DevFilaSystemParser::ParseV1_0(print_push, &obj_no_switch, obj_no_switch.GetFilaSystem().get(), false);
    REQUIRE(obj_no_switch.GetFilaSystem()->GetAmsList().count("0") == 0);
}
