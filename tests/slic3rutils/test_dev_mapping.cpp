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

TEST_CASE("A toolchanger's tools all resolve in filament mapping", "[DevMapping]")
{
    // Regression guard for two independent defects that both hid tools behind a "tool 0 works
    // by coincidence" facade for Moonraker toolchangers (Snapmaker U1 and similar):
    //
    // 1) DevMapping.cpp's tray-index arms (_parse_tray_info and the tray_index computation in
    //    ams_filament_mapping) originally didn't match DevAms::TOOLCHANGER, so every unit fell
    //    through to the AMS-group formula/assert(0) and only unit 0 (index 0) ended up
    //    distinguishable; tools 1-3 collided or were dropped and stayed unmapped (tray_id == -1).
    //    Fixed by routing TOOLCHANGER through the same "ams_id + slot_id" arm as N3S.
    //
    // 2) is_valid_mapping_result (called internally by ams_filament_mapping with
    //    check_empty_slot=true) rejects a result whose tray no longer reports is_exists. For any
    //    unit type >= 4 that isn't AMS_LITE_MIXED, DevFilaSystem.cpp computed is_exists via a
    //    "some_bit + (ams_id - 128)" formula that assumes real N3S hardware's ams_id-128 wire
    //    scheme. MoonrakerPrinterAgent::build_ams_payload instead publishes 0-based
    //    ams_exist_bits/tray_exist_bits (`1 << ams_id`), so that shift went out of range and
    //    is_exists always came back false, invalidating the first tray is_valid_mapping_result
    //    examined (mutating just that entry to tray_id == -1 before returning early). Fixed by
    //    giving TOOLCHANGER its own arm that reads bit ams_id directly, leaving the N3S
    //    128-offset arithmetic untouched.
    //
    // Both defects independently reduce the same observable symptom: with only one of the two
    // fixes in place, either tools 1-3 stay at tray_id -1 (defect 1) or tool 0 does (defect 2).
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // info bits 0-3 = 6 (DevAms::TOOLCHANGER), matching the "%04X" of TOOLCHANGER that
    // MoonrakerPrinterAgent::build_ams_payload writes into each unit's "info" field.
    // tray_exist_bits bit n marks unit n / slot 0, mirroring build_ams_payload's
    // `tray_exist_bits |= (1 << slot_index)` where slot_index == ams_id for these units.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "F",
            "ams": [
                { "id": "0", "info": "0006", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] },
                { "id": "1", "info": "0006", "tray": [ { "id": "0", "tray_color": "00FF00FF" } ] },
                { "id": "2", "info": "0006", "tray": [ { "id": "0", "tray_color": "0000FFFF" } ] },
                { "id": "3", "info": "0006", "tray": [ { "id": "0", "tray_color": "FFFF00FF" } ] }
            ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    std::vector<std::string> colors = {"FF0000FF", "00FF00FF", "0000FFFF", "FFFF00FF"};
    std::vector<FilamentInfo> filaments;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(ams_list.count(std::to_string(i)) == 1);
        // TOOLCHANGER units carry no extruder nibble, so DevFilaSystemParser defaults them to
        // MAIN_EXTRUDER_ID; select them via the right-AMS mapping option below.
        REQUIRE(ams_list.at(std::to_string(i))->GetExtruderId() == MAIN_EXTRUDER_ID);

        // tray_info_idx/tray_type are omitted from the payload above (as in the other tests in
        // this file) because resolving them needs the GUI preset bundle, unavailable headless.
        // Set the display filament type directly, as the other tests here do.
        DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray(std::to_string(i), "0");
        REQUIRE(tray != nullptr);
        tray->m_fila_type = "PLA";

        FilamentInfo fila;
        fila.id    = i;
        fila.type  = "PLA";
        fila.color = colors[i];
        filaments.push_back(fila);
    }

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);
    map_opt[MappingOption::USE_RIGHT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, filaments, result, map_opt, {}, false);

    // The defining property both fixes restore: every tool resolves, not just tool 0, and the
    // result is accepted as valid (exercises the is_exists check both fixes touch).
    REQUIRE(result.size() == 4);
    for (int i = 0; i < 4; ++i) {
        INFO("filament " << i);
        REQUIRE(result[i].tray_id >= 0);
        CHECK(result[i].tray_id == i);
    }
    CHECK(DevMappingUtil::is_valid_mapping_result(&obj, result, true));
}

TEST_CASE("Both AMS unit shapes MoonrakerPrinterAgent::build_ams_payload emits parse into the right unit/tray layout", "[DevMapping]")
{
    // Regression guard for the vendor-caller mismatch after MoonrakerPrinterAgent::build_ams_payload
    // grew a per-vendor unit shape: SnapmakerPrinterAgent (a physical toolchanger) must use one
    // 1-slot TOOLCHANGER unit per tool, while CrealityPrintAgent/QidiPrinterAgent (4-slot MMU boxes)
    // must keep chunking into AMS_LITE units of up to 4 slots. This pins the parser's view of both
    // payload shapes so a future caller regression (e.g. a vendor reverting to the wrong shape, or
    // a hardcoded unit count) shows up as a wrong unit/tray count here.
    SECTION("Toolchanger shape: 4 lanes become 4 one-slot units")
    {
        // Mirrors what SnapmakerPrinterAgent::fetch_filament_info now requests via
        // build_ams_payload(slot_count, slot_count - 1, trays, AmsUnitShape::Toolchanger) for a
        // 4-tool U1: one unit per tool, each with a single tray at id "0" (info 0006 ==
        // DevAms::TOOLCHANGER). Before the fix, Snapmaker's caller passed a hardcoded ams_count of
        // 1 into the (by-then-toolchanger-shaped) builder, which emits exactly `ams_count` units,
        // so this payload would have collapsed to ONE unit / ONE tray instead of four -- this
        // SECTION fails with `ams_list.size() == 1` if that regresses.
        json print_push = json::parse(R"({
            "ams": {
                "tray_exist_bits": "F",
                "ams": [
                    { "id": "0", "info": "0006", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] },
                    { "id": "1", "info": "0006", "tray": [ { "id": "0", "tray_color": "00FF00FF" } ] },
                    { "id": "2", "info": "0006", "tray": [ { "id": "0", "tray_color": "0000FFFF" } ] },
                    { "id": "3", "info": "0006", "tray": [ { "id": "0", "tray_color": "FFFF00FF" } ] }
                ]
            }
        })");

        MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
        DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

        const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
        REQUIRE(ams_list.size() == 4);
        for (int i = 0; i < 4; ++i) {
            INFO("unit " << i);
            REQUIRE(ams_list.count(std::to_string(i)) == 1);
            CHECK(ams_list.at(std::to_string(i))->GetTrays().size() == 1);
        }

        // Global tray index == unit index for TOOLCHANGER (DevFilaSystem.cpp GetTrayIndexMap).
        auto tray_index_map = obj.GetFilaSystem()->GetTrayIndexMap();
        for (int i = 0; i < 4; ++i) {
            INFO("tray index " << i);
            REQUIRE(tray_index_map.count(i) == 1);
            CHECK(tray_index_map.at(i).first == i);   // ams_id
            CHECK(tray_index_map.at(i).second == 0);  // slot_id
        }
    }

    SECTION("Box4 shape: 4 slots stay chunked into a single AMS_LITE unit")
    {
        // Mirrors what CrealityPrintAgent/QidiPrinterAgent now request via
        // build_ams_payload(box_count, ..., trays, AmsUnitShape::Box4): one unit per up-to-4
        // slots (info 0002 == DevAms::AMS_LITE), byte-identical to the payload shape
        // build_ams_payload produced before it grew a per-tool TOOLCHANGER arm.
        json print_push = json::parse(R"({
            "ams": {
                "tray_exist_bits": "F",
                "ams": [
                    { "id": "0", "info": "0002", "tray": [
                        { "id": "0", "tray_color": "FF0000FF" },
                        { "id": "1", "tray_color": "00FF00FF" },
                        { "id": "2", "tray_color": "0000FFFF" },
                        { "id": "3", "tray_color": "FFFF00FF" }
                    ] }
                ]
            }
        })");

        MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
        DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

        const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
        REQUIRE(ams_list.size() == 1);
        REQUIRE(ams_list.count("0") == 1);
        CHECK(ams_list.at("0")->GetTrays().size() == 4);

        // Global tray index == ams_id*4 + slot_id for AMS_LITE (DevFilaSystem.cpp GetTrayIndexMap).
        auto tray_index_map = obj.GetFilaSystem()->GetTrayIndexMap();
        for (int i = 0; i < 4; ++i) {
            INFO("tray index " << i);
            REQUIRE(tray_index_map.count(i) == 1);
            CHECK(tray_index_map.at(i).first == 0);   // ams_id
            CHECK(tray_index_map.at(i).second == i);  // slot_id
        }
    }
}
