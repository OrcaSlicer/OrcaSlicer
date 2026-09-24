#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <slic3r/Utils/AmsPayload.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include "orca_mqtt_mock_broker.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Slic3r::OrcaPrinterAgent;

namespace {
// Probe exposes the protected internals the tests drive.
struct Probe : OrcaPrinterAgent {
    using OrcaPrinterAgent::OrcaPrinterAgent;
    using OrcaPrinterAgent::deliver_to_sink;
    using OrcaPrinterAgent::parse_lan_endpoint;
    using OrcaPrinterAgent::make_lan_client_id;
    using OrcaPrinterAgent::lan_connection_target;
};
}

TEST_CASE("OrcaPrinterAgent forwards a status payload to on_message_fn", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    std::string got_id, got_payload;
    agent.set_on_message_fn([&](std::string id, std::string p){ got_id = std::move(id); got_payload = std::move(p); });
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status"}})", /*local=*/false);
    CHECK(got_id == "dev-1");
    CHECK(got_payload.find("push_status") != std::string::npos);
}

TEST_CASE("OrcaPrinterAgent stamps the get_capabilities nozzle diameter onto push_status frames", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    std::string last_payload;
    agent.set_on_message_fn([&](std::string, std::string p){ last_payload = std::move(p); });

    // Before any capabilities reply, a push_status frame is forwarded untouched.
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status","mc_percent":10}})", /*local=*/false);
    CHECK(last_payload.find("nozzle_diameter") == std::string::npos);

    // The get_capabilities reply is forwarded verbatim; its topology nozzle diameter
    // is cached for the device.
    agent.deliver_to_sink(
        "dev-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"topology":{"tools":[{"id":"T0","nozzle":{"diameter_mm":0.4}}]},"protocol":{"ams_ops":["change_filament"]}}}})",
        /*local=*/false);
    CHECK(last_payload.find("\"command\":\"get_capabilities\"") != std::string::npos);
    CHECK(last_payload.find("\"print\"") == std::string::npos);

    // Later push_status frames for that device get the cached diameter plus a neutral
    // nozzle_type, so MachineObject::parse_json's legacy nozzle parser can run.
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status","mc_percent":20}})", /*local=*/false);
    CHECK(last_payload.find("\"nozzle_diameter\":0.4") != std::string::npos);
    CHECK(last_payload.find("\"nozzle_type\":\"N/A\"") != std::string::npos);

    // A different device is unaffected.
    agent.deliver_to_sink("dev-2", R"({"print":{"command":"push_status"}})", /*local=*/false);
    CHECK(last_payload.find("nozzle_diameter") == std::string::npos);

    // A frame that already carries real nozzle data is not overridden.
    agent.deliver_to_sink("dev-1", R"({"print":{"command":"push_status","nozzle_diameter":0.6}})", /*local=*/false);
    CHECK(last_payload.find("\"nozzle_diameter\":0.6") != std::string::npos);
    CHECK(last_payload.find("N/A") == std::string::npos);

    // The reply's declared ams_ops register on the device (OPCP §7.8): the one
    // declared op passes the client gate, an undeclared one is rejected.
    bool unsupported = false;
    OrcaPrinterAgent::canonicalize_ams_payload("dev-1", R"({"print":{"command":"ams_change_filament","target":1}})", &unsupported);
    CHECK_FALSE(unsupported);
    OrcaPrinterAgent::canonicalize_ams_payload("dev-1", R"({"print":{"command":"ams_control","param":"pause"}})", &unsupported);
    CHECK(unsupported);
}

TEST_CASE("OrcaPrinterAgent::parse_lan_endpoint", "[OrcaPrinterAgent]") {
    std::string h, p;
    REQUIRE(Probe::parse_lan_endpoint("192.168.1.9", h, p));
    CHECK(h == "192.168.1.9"); CHECK(p == "8280");
    REQUIRE(Probe::parse_lan_endpoint("http://host.local:9000/x", h, p));
    CHECK(h == "host.local"); CHECK(p == "9000");
    CHECK_FALSE(Probe::parse_lan_endpoint("", h, p));
}

TEST_CASE("OrcaPrinterAgent::make_lan_client_id is stable and prefixed", "[OrcaPrinterAgent]") {
    const auto a = Probe::make_lan_client_id("dev-1");
    const auto b = Probe::make_lan_client_id("dev-1");
    CHECK(a == b);                                   // drawn once per process
    CHECK(a.rfind("orcaslicer-lan-dev-1-", 0) == 0);
}

TEST_CASE("connect_printer wires up a LAN Config", "[OrcaPrinterAgent][.integration]") {
    Probe agent("/tmp");
    Slic3r::PrinterConnectionParams params{
        "dev-1", "10.255.255.1", "", "orcasonar", "code", false, ""
    };
    const int rc = agent.connect_printer(params);
    CHECK(rc == BAMBU_NETWORK_SUCCESS);
    CHECK(agent.lan_connection_target() == "ws://10.255.255.1:8280/mqtt");
    CHECK(agent.get_user_selected_machine().empty());   // LAN path must not touch the cloud selection
    agent.disconnect_printer();
}

// why hidden: connect_printer starts a detached connect worker, like the LAN
// test above. The sync mode follows the printer's own AMS capability, not the
// transport: a connected printer stays `none` until its get_capabilities reply
// declares a material system. The registry is process-wide, so this test uses
// ids no other test feeds capabilities for.
TEST_CASE("filament sync follows the printer's AMS capability", "[OrcaPrinterAgent][.integration]") {
    Probe agent("/tmp");
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::none);

    REQUIRE(agent.connect_printer(Slic3r::PrinterConnectionParams{
        "dev-ams-1", "10.255.255.1", "", "orcasonar", "code", false, ""
    }) == BAMBU_NETWORK_SUCCESS);
    // Connected, but no capability reply yet: still none.
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::none);

    // features.fms=true is the authoritative material-system flag.
    agent.deliver_to_sink("dev-ams-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"features":{"fms":true},"ams_ops":["change_filament"]}}}})",
        /*local=*/true);
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::subscription);

    // An explicit false clears it again.
    agent.deliver_to_sink("dev-ams-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"features":{"fms":false}}}}})",
        /*local=*/true);
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::none);

    // filament_slots alone enables sync: a standalone printer with no material
    // system still has slots (REQ-FMS-001).
    agent.deliver_to_sink("dev-ams-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"features":{"fms":false,"filament_slots":true}}}}})",
        /*local=*/true);
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::subscription);

    // Older payloads without features.fms fall back to a non-empty ams_ops.
    agent.deliver_to_sink("dev-ams-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"ams_ops":["change_filament"]}}}})",
        /*local=*/true);
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::subscription);

    // A reply without a protocol block is not a capabilities answer: it is
    // ignored, so a transient malformed reply cannot gate every write. The
    // ams_ops fallback above still holds.
    agent.deliver_to_sink("dev-ams-1",
        R"({"info":{"command":"get_capabilities","capabilities":{}}})",
        /*local=*/true);
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::subscription);

    // Disconnecting forgets the declaration rather than letting it go stale.
    agent.disconnect_printer();
    CHECK(agent.get_filament_sync_mode() == Slic3r::FilamentSyncMode::none);
}

// OrcaSonar's contract: an absent ams_ops key means "no AMS controls" (app.go
// omits it when no driver declares a write op; Qidi is the shipped example).
// Answering get_capabilities without it must gate every AMS write client-side,
// not fall back to the base default's "no record -> never gate".
TEST_CASE("a capability reply without ams_ops gates AMS writes", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    agent.deliver_to_sink("dev-noops",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"features":{"fms":true}}}}})",
        /*local=*/true);

    bool unsupported = false;
    OrcaPrinterAgent::canonicalize_ams_payload(
        "dev-noops",
        R"({"print":{"command":"ams_change_filament","target":0,"slot_id":0,"ams_id":0}})",
        &unsupported);
    CHECK(unsupported);
}

// A malformed get_capabilities reply must be ignored, not read as "no
// capabilities": doing so would set ops_known with an empty set and gate every
// AMS write until a good reply arrives.
TEST_CASE("a malformed capability reply does not gate AMS writes", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    agent.deliver_to_sink("dev-malformed",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"ams_ops":["change_filament"]}}}})",
        /*local=*/true);
    CHECK(Slic3r::ams_op_supported("dev-malformed", "change_filament"));

    agent.deliver_to_sink("dev-malformed",
        R"({"info":{"command":"get_capabilities","capabilities":{}}})",
        /*local=*/true);
    // The malformed reply changed nothing, rather than clearing the op set.
    CHECK(Slic3r::ams_op_supported("dev-malformed", "change_filament"));
}

TEST_CASE("post-connect sequence is subscribe then 4 requests in order", "[OrcaPrinterAgent]") {
    struct SeqProbe : OrcaPrinterAgent {
        using OrcaPrinterAgent::OrcaPrinterAgent;
        std::vector<std::string> calls;
        void emit_connect_sequence(const std::string& dev_id,
            std::function<void(const std::string&)> /*sub*/,
            std::function<void(const std::string&)> /*req*/) override {
            OrcaPrinterAgent::emit_connect_sequence(dev_id,
                [&](const std::string& id){ calls.push_back("sub:" + id); },
                [&](const std::string& body){ calls.push_back(body); });
        }
    } probe("/tmp");
    probe.run_connect_sequence_for_test("dev-1");
    REQUIRE(probe.calls.size() == 5);
    CHECK(probe.calls[0] == "sub:dev-1");
    CHECK(probe.calls[1].find("\"pushing\"") != std::string::npos);
    CHECK(probe.calls[1].find("\"start\"")   != std::string::npos);
    CHECK(probe.calls[2].find("pushall")     != std::string::npos);
    CHECK(probe.calls[3].find("get_version") != std::string::npos);
    CHECK(probe.calls[4].find("get_capabilities") != std::string::npos);
    for (auto& c : probe.calls)
        if (auto pos = c.find("sequence_id"); pos != std::string::npos)
            CHECK(c.substr(pos).find("\"2") != std::string::npos);
}

// Hidden: spawns the connect worker and attempts a real (failing) connect.
TEST_CASE("selecting a cloud printer configures the fleet socket", "[OrcaPrinterAgent][.integration]") {
    auto cloud = std::make_shared<Slic3r::OrcaCloudServiceAgent>("/tmp");
    cloud->set_api_base_url("api.example.com");
    OrcaPrinterAgent agent("/tmp");
    agent.set_cloud_agent(cloud);

    agent.set_user_selected_machine("printer-uuid-1");
    // The configure runs on the connect worker; poll rather than racing it.
    std::string url;
    for (int i = 0; i < 300; ++i) {
        url = cloud->selected_printer_mqtt_url();
        if (!url.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(url == "wss://api.example.com/api/v1/printers/mqtt");

    agent.set_user_selected_machine("");     // selection changes do not tear down the fleet socket
    CHECK(cloud->selected_printer_mqtt_url() == "wss://api.example.com/api/v1/printers/mqtt");
}

TEST_CASE("a stale-generation inbound message is dropped", "[OrcaPrinterAgent]") {
    struct GenProbe : OrcaPrinterAgent {
        using OrcaPrinterAgent::OrcaPrinterAgent;
        using OrcaPrinterAgent::make_lan_message_handler;   // expose for the test
    };
    GenProbe agent("/tmp");
    int hits = 0;
    agent.set_on_message_fn([&](std::string, std::string){ ++hits; });
    auto handler_gen1 = agent.make_lan_message_handler(/*generation=*/1);
    // m_lan_generation starts at 0; two bumps -> 2, so the epoch-1 handler is stale.
    agent.bump_lan_generation_for_test();
    agent.bump_lan_generation_for_test();
    handler_gen1("dev-1", "{}");                            // late callback from gen 1
    CHECK(hits == 0);
}

TEST_CASE("connect_server does not start an MQTT socket", "[OrcaCloud]") {
    auto cloud = std::make_shared<Slic3r::OrcaCloudServiceAgent>("/tmp");
    cloud->set_api_base_url("127.0.0.1:1");     // no session -> connect_server short-circuits before any probe
    cloud->connect_server();
    REQUIRE(cloud->get_mqtt_connection() != nullptr);       // created in the ctor
    CHECK_FALSE(cloud->get_mqtt_connection()->is_running()); // never started
    CHECK(cloud->selected_printer_mqtt_url().empty());
}

TEST_CASE("send_message* reject when there is no connection", "[OrcaPrinterAgent]") {
    OrcaPrinterAgent agent("/tmp");   // no cloud agent, no LAN connection
    CHECK(agent.send_message("d", "{}", 0, 0)            == BAMBU_NETWORK_ERR_INVALID_HANDLE);
    CHECK(agent.send_message_to_printer("d", "{}", 0, 0) == BAMBU_NETWORK_ERR_INVALID_HANDLE);
    CHECK(agent.send_message("", "{}", 0, 0)             == BAMBU_NETWORK_ERR_INVALID_HANDLE);   // empty dev_id
}

TEST_CASE("send_message_to_printer publishes on the LAN connection", "[OrcaPrinterAgent][.integration]") {
    orca_mqtt_test::MockBroker broker;
    OrcaPrinterAgent agent("/tmp");
    const auto ep = broker.host_port();
    agent.connect_printer(Slic3r::PrinterConnectionParams{"dev-1", ep.first + ":" + ep.second, "", "orcasonar", "code", false, ""});

    for (int i = 0; i < 150 && broker.connect_count() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(broker.connect_count() >= 1);

    CHECK(agent.send_message_to_printer("dev-1", R"({"print":{"command":"pause","sequence_id":"20007"}})", 0, 0)
          == BAMBU_NETWORK_SUCCESS);

    // on_connected also publishes 4 requests; poll until "pause" specifically shows up.
    bool saw_pause = false;
    for (int i = 0; i < 150 && !saw_pause; ++i) {
        for (const auto& r : broker.received_requests())
            if (r.find("pause") != std::string::npos) { saw_pause = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(saw_pause);
    agent.disconnect_printer();
}

TEST_CASE("destroying an agent mid-connect does not hang or crash", "[OrcaPrinterAgent]") {
    for (int i = 0; i < 20; ++i) {
        auto agent = std::make_unique<OrcaPrinterAgent>("/tmp");
        agent->connect_printer(Slic3r::PrinterConnectionParams{"dev-1", "127.0.0.1:1", "", "orcasonar", "code", false, ""});  // nothing listening: instant ECONNREFUSED
        agent.reset();   // ~OrcaPrinterAgent must stop the conn, join the thread, and not hang/crash
    }
    SUCCEED();
}

// OPCP spec §7.8: Bambu wire conventions must be decoded at the agent funnel,
// never smuggled through as numeric lanes (an external-spool select once read
// as slot_id=0 and physically loaded gate 0).
TEST_CASE("OrcaPrinterAgent rewrites Bambu ams_* payloads onto the canonical OrcaSonar bodies", "[OrcaPrinterAgent]") {
    auto canon = [](const std::string& dev, const std::string& in, bool* unsupported = nullptr) {
        bool local = false;
        return OrcaPrinterAgent::canonicalize_ams_payload(dev, in, unsupported ? unsupported : &local);
    };

    auto out = nlohmann::json::parse(canon("dev-c1",
        R"({"print":{"command":"ams_change_filament","sequence_id":"1","target":5,"slot_id":1,"ams_id":1,"curr_temp":210,"tar_temp":220}})"));
    CHECK(out["print"]["selector"] == "lane");
    // Coordinates are the resolver's own form; target (a BBL tray id) is dropped.
    CHECK(out["print"]["ams_id"] == 1);
    CHECK(out["print"]["slot_id"] == 1);
    CHECK(!out["print"].contains("lane"));
    CHECK(!out["print"].contains("target"));
    CHECK(out["print"]["tar_temp"] == 220);

    // External-spool selection ("254" arrives hacked to 255 with slot_id=0):
    // must become the selector op, never a lane.
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_change_filament","ams_id":255,"target":255,"slot_id":0}})"));
    CHECK(out["print"]["selector"] == "external");
    CHECK(!out["print"].contains("lane"));
    CHECK(!out["print"].contains("ams_id"));

    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_change_filament","ams_id":0,"target":255,"slot_id":255}})"));
    CHECK(out["print"]["selector"] == "unload");

    // A coordinate-less body must not fabricate a flat lane from a BBL tray id;
    // the server validates the address and answers -19.
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_change_filament","target":6}})"));
    CHECK(!out["print"].contains("lane"));
    CHECK(!out["print"].contains("target"));
    CHECK(out["print"]["selector"] == "lane");

    // Box coordinates are forwarded unchanged: a fabricated flat lane would be
    // the BBL tray id, which is not the layout lane for wide/sparse boxes.
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_filament_setting","ams_id":1,"slot_id":2,"tray_id":2,"tray_type":"PLA"}})"));
    CHECK(out["print"]["ams_id"] == 1);
    CHECK(out["print"]["slot_id"] == 2);
    CHECK(!out["print"].contains("lane"));
    CHECK(!out["print"].contains("tray_id"));
    CHECK(out["print"]["tray_type"] == "PLA");

    // Wide-box dual form: both addressings resolve to one slot server-side.
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_filament_setting","ams_id":1,"slot_id":5,"tray_type":"PLA"}})"));
    CHECK(out["print"]["ams_id"] == 1);
    CHECK(out["print"]["slot_id"] == 5);
    CHECK(!out["print"].contains("lane"));
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_filament_setting","ams_id":2,"slot_id":1,"tray_type":"PLA"}})"));
    CHECK(out["print"]["ams_id"] == 2);
    CHECK(out["print"]["slot_id"] == 1);

    // A lane-only body must pass through untouched, never become lane = -5.
    const std::string lane_only = R"({"print":{"command":"ams_filament_setting","lane":5,"tray_type":"PLA"}})";
    CHECK(canon("dev-c1", lane_only) == lane_only);

    // External/direct spool (Bambu 254/255): forwarded as the canonical
    // ams_id address, never a fabricated lane (REQ-FMS-001).
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_filament_setting","ams_id":255,"slot_id":0,"tray_id":0,"tray_type":"PLA"}})"));
    CHECK(out["print"]["ams_id"] == 255);
    CHECK(out["print"]["slot_id"] == 0);
    CHECK(!out["print"].contains("lane"));
    CHECK(!out["print"].contains("tray_id"));

    // Legacy RFID call shape (ams_id+slot_id, no tray_id) flattens to tray_id.
    out = nlohmann::json::parse(canon("dev-c1", R"({"print":{"command":"ams_get_rfid","ams_id":1,"slot_id":2}})"));
    CHECK(out["print"]["tray_id"] == 6);
    CHECK(!out["print"].contains("ams_id"));

    const std::string canonical = R"({"print":{"command":"ams_change_filament","selector":"lane","lane":3}})";
    CHECK(canon("dev-c1", canonical) == canonical);
    CHECK(canon("dev-c1", R"({"print":{"command":"pause"}})") == R"({"print":{"command":"pause"}})");
    CHECK(canon("dev-c1", "not json at all") == "not json at all");

    // Declared ams_ops gate at the client: only change_filament is supported.
    Slic3r::register_ams_ops("dev-c2", {"change_filament"});
    bool unsupported = false;
    canon("dev-c2", R"({"print":{"command":"ams_change_filament","target":255,"slot_id":255}})", &unsupported);
    CHECK(unsupported);
    unsupported = false;
    canon("dev-c2", R"({"print":{"command":"ams_control","param":"pause"}})", &unsupported);
    CHECK(unsupported);
    unsupported = false;
    canon("dev-c2", R"({"print":{"command":"ams_change_filament","target":1}})", &unsupported);
    CHECK(!unsupported);
    // A selector-carrying canonical body is gated on its selector's op too.
    unsupported = false;
    canon("dev-c2", R"({"print":{"command":"ams_change_filament","selector":"lane","lane":1}})", &unsupported);
    CHECK(!unsupported);
    unsupported = false;
    canon("dev-c2", R"({"print":{"command":"ams_change_filament","selector":"external"}})", &unsupported);
    CHECK(unsupported);

    // A lane can resolve to an external slot server-side (§7.8), so a device
    // that declares only external still admits a canonical lane write.
    Slic3r::register_ams_ops("dev-c4", {"external"});
    unsupported = false;
    canon("dev-c4", R"({"print":{"command":"ams_change_filament","selector":"lane","lane":1}})", &unsupported);
    CHECK(!unsupported);
    unsupported = false;
    canon("dev-c4", R"({"print":{"command":"ams_change_filament","selector":"external"}})", &unsupported);
    CHECK(!unsupported);
    // Other selectors still gate on their own token.
    unsupported = false;
    canon("dev-c4", R"({"print":{"command":"ams_change_filament","selector":"unload"}})", &unsupported);
    CHECK(unsupported);

    // A device with no capabilities record is never gated (server backstops).
    unsupported = false;
    canon("dev-c3", R"({"print":{"command":"ams_user_setting","ams_id":0}})", &unsupported);
    CHECK(!unsupported);
}

// filament_setting is advertised by filament_slots alone (OPCP §7.8), so a
// standalone printer with no ams_ops can still write slots, while the material
// writes stay gated.
TEST_CASE("a filament_slots reply admits ams_filament_setting without ams_ops", "[OrcaPrinterAgent]") {
    Probe agent("/tmp");
    agent.deliver_to_sink("dev-slots",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"features":{"fms":false,"filament_slots":true}}}}})",
        /*local=*/true);

    bool unsupported = false;
    OrcaPrinterAgent::canonicalize_ams_payload(
        "dev-slots",
        R"({"print":{"command":"ams_filament_setting","ams_id":255,"slot_id":0,"tray_id":0,"tray_type":"PLA"}})",
        &unsupported);
    CHECK_FALSE(unsupported);

    unsupported = false;
    OrcaPrinterAgent::canonicalize_ams_payload(
        "dev-slots",
        R"({"print":{"command":"ams_change_filament","target":1,"slot_id":1,"ams_id":0}})",
        &unsupported);
    CHECK(unsupported);
}

// A box wider than 4 slots is shown as several 4-tray units, so the BBL tray id
// a panel reports must split into that unit's own (ams_id, slot_id). A flat lane
// would be the tray id, which is not the layout lane for a wide box
// (REQ-STS-008 §7.8).
TEST_CASE("an AMS tray selection sends the tray's ams_id and slot_id", "[OrcaPrinterAgent]") {
    // Unit 2 tray 1 is BBL tray 9: a 6-slot box's slot 5 addressed as (2, 1).
    auto body = nlohmann::json::parse(OrcaPrinterAgent::build_ams_change_filament_body(9, 123));
    CHECK(body["print"]["selector"] == "lane");
    CHECK(body["print"]["ams_id"] == 2);
    CHECK(body["print"]["slot_id"] == 1);
    CHECK_FALSE(body["print"].contains("lane"));

    // Unit 0 tray 3 is tray 3.
    body = nlohmann::json::parse(OrcaPrinterAgent::build_ams_change_filament_body(3, 124));
    CHECK(body["print"]["ams_id"] == 0);
    CHECK(body["print"]["slot_id"] == 3);
}

// A filament frame can arrive after the get_capabilities reply was missed (the
// topology resolved late, or Klipper restarted). While the device is still
// unconfirmed, one filament frame must re-ask, throttled, and stop once a reply
// arrives so the session can leave FilamentSyncMode::none.
TEST_CASE("a filament frame re-requests capabilities while the topology is unconfirmed", "[OrcaPrinterAgent]") {
    struct RefreshProbe : OrcaPrinterAgent {
        using OrcaPrinterAgent::OrcaPrinterAgent;
        using OrcaPrinterAgent::deliver_to_sink;
        std::vector<std::string> refreshes;
        void request_filament_capabilities(const std::string& dev_id, bool /*local*/) override {
            refreshes.push_back(dev_id);
        }
    } agent("/tmp");
    agent.set_on_message_fn([](std::string, std::string) {});

    const std::string frame = R"({"print":{"command":"push_status","vir_slot":[{"id":"255"}]}})";
    agent.deliver_to_sink("dev-refresh-1", frame, /*local=*/true);
    REQUIRE(agent.refreshes.size() == 1);

    // Throttled: a second frame immediately after does not ask again.
    agent.deliver_to_sink("dev-refresh-1", frame, /*local=*/true);
    CHECK(agent.refreshes.size() == 1);

    // A capability reply confirms the topology: no further re-request.
    agent.deliver_to_sink("dev-refresh-1",
        R"({"info":{"command":"get_capabilities","capabilities":{"protocol":{"features":{"filament_slots":true}}}}})",
        /*local=*/true);
    agent.deliver_to_sink("dev-refresh-1", frame, /*local=*/true);
    CHECK(agent.refreshes.size() == 1);

    // A status without filament state never triggers a request.
    agent.deliver_to_sink("dev-refresh-2", R"({"print":{"command":"push_status","mc_percent":10}})", /*local=*/true);
    CHECK(agent.refreshes.size() == 1);
}

// Deselecting a cloud device forgets its declaration, so a later session starts
// from "no reply yet" instead of a stale one. With no record an undeclared op is
// admitted again, which is how a freshly selected device starts.
TEST_CASE("deselecting a cloud device forgets its capabilities", "[OrcaPrinterAgent]") {
    OrcaPrinterAgent agent("/tmp");
    Slic3r::register_ams_ops("dev-cloud-clear", {"change_filament"});
    Slic3r::register_filament_slots("dev-cloud-clear", true);
    CHECK_FALSE(Slic3r::ams_op_supported("dev-cloud-clear", "external"));

    agent.set_user_selected_machine("dev-cloud-clear");
    agent.set_user_selected_machine("");

    CHECK(Slic3r::ams_op_supported("dev-cloud-clear", "external"));
    CHECK_FALSE(Slic3r::has_filament_slots("dev-cloud-clear"));
}

// A LAN feed for the same device id must keep its declaration when the cloud
// selection is cleared: clearing it would strand an independently live session.
// why hidden: spawns the LAN connect worker, like the other connect tests.
TEST_CASE("deselecting a cloud device keeps a live LAN declaration", "[OrcaPrinterAgent][.integration]") {
    OrcaPrinterAgent agent("/tmp");
    Slic3r::register_ams_ops("dev-lan-keep", {"change_filament"});
    Slic3r::register_filament_slots("dev-lan-keep", true);

    REQUIRE(agent.connect_printer(Slic3r::PrinterConnectionParams{
        "dev-lan-keep", "10.255.255.1", "", "orcasonar", "code", false, ""
    }) == BAMBU_NETWORK_SUCCESS);
    agent.set_user_selected_machine("dev-lan-keep");
    agent.set_user_selected_machine("");

    CHECK_FALSE(Slic3r::ams_op_supported("dev-lan-keep", "external"));
    CHECK(Slic3r::has_filament_slots("dev-lan-keep"));

    agent.disconnect_printer();
}
