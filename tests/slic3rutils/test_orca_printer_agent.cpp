#include <catch2/catch_test_macros.hpp>
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
        R"({"info":{"command":"get_capabilities","capabilities":{"topology":{"tools":[{"id":"T0","nozzle":{"diameter_mm":0.4}}]}}}})",
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
    const int rc = agent.connect_printer("dev-1", "10.255.255.1", "orcasonar", "code", false);
    CHECK(rc == BAMBU_NETWORK_SUCCESS);
    CHECK(agent.lan_connection_target() == "ws://10.255.255.1:8280/mqtt");
    CHECK(agent.get_user_selected_machine().empty());   // LAN path must not touch the cloud selection
    agent.disconnect_printer();
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
    agent.connect_printer("dev-1", ep.first + ":" + ep.second, "orcasonar", "code", false);

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
        agent->connect_printer("dev-1", "127.0.0.1:1", "orcasonar", "code", false);  // nothing listening: instant ECONNREFUSED
        agent.reset();   // ~OrcaPrinterAgent must stop the conn, join the thread, and not hang/crash
    }
    SUCCEED();
}
