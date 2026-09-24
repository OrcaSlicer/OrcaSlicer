// why: match the GUI include order to avoid rpcndr.h byte/std::byte
// ambiguity in the Windows COM headers.
// why: wx/timer.h must precede DeviceManager.hpp because
// DeviceErrorDialog.hpp uses wxTimerEvent.
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

#include <stdexcept>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;

// Bambu virtual trays follow full-snapshot removals; Orca virtual trays are
// retained after first observation until reconnect.

// Contract: an authoritative empty vir_slot ([] = "known, no virtual slots")
// clears the seeded virtual trays. The consumers were guarded so an empty
// vector is safe.
TEST_CASE("An empty vir_slot clears the virtual trays", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";
    REQUIRE(machine.vt_slot.size() == 1);
    REQUIRE(machine.vt_slot[0].id == "255");

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[]}})", false);

    CHECK(machine.vt_slot.empty());
    CHECK_FALSE(machine.ams_support_virtual_tray);
}

// A frame that omits vir_slot must leave the seeded virtual tray untouched.
TEST_CASE("A missing vir_slot keeps the virtual trays", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";
    REQUIRE(machine.vt_slot.size() == 1);

    machine.parse_json("lan", R"({"print":{"command":"push_status"}})", false);

    CHECK(machine.vt_slot.size() == 1);
    CHECK(machine.ams_support_virtual_tray);
}

// Repopulating after a clear must not rely on the constructor's seed, and must
// re-enable virtual-tray support even though the clear turned the flag off.
TEST_CASE("Virtual trays repopulate after an authoritative clear", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[]}})", false);
    REQUIRE(machine.vt_slot.empty());
    REQUIRE_FALSE(machine.ams_support_virtual_tray);

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[{"id":"255"},{"id":"254"}]}})", false);

    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].id == "255");
    CHECK(machine.vt_slot[1].id == "254");
    CHECK(machine.ams_support_virtual_tray);
}

// A deputy with no main is not an index-1 write into an empty vector.
TEST_CASE("An orphan deputy virtual tray is dropped, not indexed", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[]}})", false);
    REQUIRE(machine.vt_slot.empty());

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[{"id":"254"}]}})", false);

    CHECK(machine.vt_slot.empty());
}

// OrcaSonar emits a virtual slot only when the layout has it, so it counts as
// present even with no filament: the user's configuration wins.
TEST_CASE("A configured vir_slot is present even with no material", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "orca";

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[{"id":"255"},{"id":"254"}]}})", false);

    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].is_exists);
    CHECK(machine.vt_slot[1].is_exists);
}

// A full vir_slot snapshot is authoritative: an id it omits is removed, so a
// tool-count shrink does not leave a stale deputy tray.
TEST_CASE("A populated vir_slot prunes virtual trays it omits", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[{"id":"255"},{"id":"254"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[{"id":"255"}]}})", false);

    REQUIRE(machine.vt_slot.size() == 1);
    CHECK(machine.vt_slot[0].id == "255");
}

// A delta frame (msg=1) is not authoritative for removals: an entry it omits
// stays held, and an entry it names is updated in place (spec §7.3).
TEST_CASE("A delta vir_slot does not prune virtual trays it omits", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[{"id":"255"},{"id":"254"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);

    // Delta names only the deputy: the main must survive, the deputy updates.
    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":1,"vir_slot":[{"id":"254","tag_uid":"ABCDEF0123456789"}]}})", false);

    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].id == "255");
    CHECK(machine.vt_slot[1].id == "254");
    CHECK(machine.vt_slot[1].tag_uid == "ABCDEF0123456789");
}

// An empty delta is not authoritative and must preserve the known layout.
TEST_CASE("An empty delta vir_slot keeps the virtual trays", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "bbl";

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[{"id":"255"},{"id":"254"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":1,"vir_slot":[]}})", false);

    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].id == "255");
    CHECK(machine.vt_slot[1].id == "254");
    CHECK(machine.ams_support_virtual_tray);
}

TEST_CASE("Orca retains seen virtual trays across full snapshots", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "orca";

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[{"id":"255"},{"id":"254"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[{"id":"255","tag_uid":"0123456789ABCDEF"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].tag_uid == "0123456789ABCDEF");
    CHECK(machine.vt_slot[1].id == "254");

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].id == "255");
    CHECK(machine.vt_slot[1].id == "254");
    CHECK(machine.ams_support_virtual_tray);

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0}})", false);
    CHECK(machine.vt_slot.size() == 2);
}

TEST_CASE("Orca forgets retained virtual trays at reconnect", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "orca";
    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[{"id":"255"},{"id":"254"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 2);

    machine.reset_orca_virtual_trays_for_reconnect();
    REQUIRE(machine.vt_slot.size() == 1);
    CHECK(machine.vt_slot[0].id == "255");

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[]}})", false);
    CHECK(machine.vt_slot.empty());
    CHECK_FALSE(machine.ams_support_virtual_tray);
}

TEST_CASE("Orca stores a deputy virtual tray at its stable index", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    machine.printer_agent_id = "orca";

    machine.parse_json("lan", R"({"print":{"command":"push_status","msg":0,"vir_slot":[{"id":"254"}]}})", false);

    REQUIRE(machine.vt_slot.size() == 2);
    CHECK(machine.vt_slot[0].id == "255");
    CHECK_FALSE(machine.vt_slot[0].is_exists);
    CHECK(machine.vt_slot[1].id == "254");
    CHECK(machine.vt_slot[1].is_exists);
}

TEST_CASE("Capability flags parse without a DeviceManager", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.parse_json("lan",
        R"({"info":{"command":"get_capabilities","capabilities":{"flags":{"support_tunnel_mqtt":true}}}})", false);

    CHECK(machine.is_support_tunnel_mqtt);
}

// A filament setting landing on the virtual tray also marks it present.
TEST_CASE("A virtual tray setting marks the tray present", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[{"id":"255"}]}})", false);
    REQUIRE(machine.vt_slot.size() == 1);
    machine.vt_slot[0].is_exists = false;

    machine.parse_json("lan", R"({"print":{"command":"ams_filament_setting","ams_id":255,"tray_id":255,"tray_color":"FF0000FF","tray_type":"PLA","tray_info_idx":"GFA00","nozzle_temp_min":190,"nozzle_temp_max":230}})", false);

    CHECK(machine.vt_slot[0].is_exists);
}

// acks that target the virtual tray must survive an emptied vt_slot.
TEST_CASE("Virtual tray acks are safe with no virtual tray", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.parse_json("lan", R"({"print":{"command":"push_status","vir_slot":[]}})", false);
    REQUIRE(machine.vt_slot.empty());

    machine.parse_json("lan", R"({"print":{"command":"ams_filament_setting","ams_id":255,"tray_id":255}})", false);
    machine.parse_json("lan", R"({"print":{"command":"extrusion_cali_set","tray_id":255,"k_value":0.02}})", false);

    CHECK(machine.vt_slot.empty());
}

// Only Bambu's own agent treats a non-zero tag_uid as an RFID lock; agent-managed
// printers send it as metadata and must keep their trays editable.
TEST_CASE("Only the BBL agent is RFID-locking", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.printer_agent_id = "bbl";
    CHECK(machine.is_bbl_agent());
    CHECK_FALSE(machine.is_orca_agent());

    machine.printer_agent_id = "orca";
    CHECK_FALSE(machine.is_bbl_agent());
    CHECK(machine.is_orca_agent());

    machine.printer_agent_id = "";
    CHECK(machine.is_bbl_agent());
}

// An OPCP error ack carries result "fail" and a reason; a success ack (or one
// without result) must not be mistaken for a failure.
TEST_CASE("AMS filament setting acks expose OPCP failures", "[DeviceManager]")
{
    std::string reason;

    const json failure = json::parse(R"({"result":"fail","errno":-19,"reason":"unknown slot"})");
    CHECK(MachineObject::ams_filament_ack_failed(failure, reason));
    CHECK(reason == "unknown slot");

    reason = "stale";
    const json success = json::parse(R"({"result":"success","errno":0})");
    CHECK_FALSE(MachineObject::ams_filament_ack_failed(success, reason));
    CHECK(reason.empty());

    const json plain = json::parse(R"({"ams_id":1,"tray_id":2})");
    CHECK_FALSE(MachineObject::ams_filament_ack_failed(plain, reason));
}

// An ack that targets a tray but omits tray_id must not abort the frame: it
// falls back to slot 0 instead of an unguarded get on a missing key.
TEST_CASE("An AMS filament ack without a tray_id targets slot 0", "[DeviceManager]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    const json ams = json::parse(R"({"ams":{"tray_exist_bits":"1","ams":[
        { "id": "0", "info": "00000001", "tray": [ { "id": "0" } ] } ]}})");
    DevFilaSystemParser::ParseV1_0(ams, &machine, machine.GetFilaSystem().get(), false);
    REQUIRE(machine.GetFilaSystem()->GetAmsTray("0", "0") != nullptr);

    CHECK_NOTHROW(machine.parse_json("lan", R"({"print":{"command":"ams_filament_setting","ams_id":0,"tray_color":"FF0000FF","tray_type":"PLA","tray_info_idx":"GFA00","nozzle_temp_min":190,"nozzle_temp_max":230}})", false));

    const DevAmsTray* tray = machine.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    CHECK(tray->color == "FF0000FF");
}
