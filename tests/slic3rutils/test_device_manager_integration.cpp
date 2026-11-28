// McpTools.hpp reaches wx's MSW headers through GLCanvas3D.hpp after OCCT has hidden the USER and GDI
// declarations they need; the Windows headers first keep them, as in test_mcp_tools.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <libslic3r/ProjectTask.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Config.hpp>
#include <slic3r/GUI/DeviceCore/DevConfigUtil.h>
#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/DeviceCore/DevFilaSystem.h>
#include <slic3r/GUI/FilamentMappingUtils.hpp>
#include <slic3r/GUI/Mcp/McpTools.hpp>
#include <slic3r/GUI/Operations/Outcome.hpp>
#include <slic3r/GUI/Operations/PrinterOps.hpp>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/PrintConfig.hpp>
#include "catch2/catch_message.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using Slic3r::GUI::MappingSendError;
using Slic3r::GUI::prepare_filament_mapping_for_send;
using json = nlohmann::json;

namespace {

class StubCloudAgent final : public OrcaCloudServiceAgent
{
public:
    StubCloudAgent() : OrcaCloudServiceAgent("") {}

    int get_user_print_info(unsigned int* http_code, std::string* http_body) override
    {
        if (http_code)
            *http_code = 200;
        if (http_body)
            *http_body = R"({"devices":[]})";
        return 0;
    }

    std::string get_user_name() override { return "integration-test-user"; }
    bool is_user_login() override { return logged_in; }
    bool is_server_connected() override { return server_connected; }

    bool logged_in{false};
    bool server_connected{false};
};

class TestPrinterAgent final : public OrcaPrinterAgent
{
public:
    explicit TestPrinterAgent(std::string id)
        : OrcaPrinterAgent(""), m_info{std::move(id), "Integration Test Agent", "1.0", "test agent"}
    {
    }

    AgentInfo get_agent_info() override { return m_info; }

    // This test double represents a legacy agent, not OrcaPrinterAgent behavior.
    bool supports_command(const std::string&, const std::string&) const override { return true; }

    // It models whichever agent is registered under its id: only the Orca agent
    // serializes per-print filament_mapping.
    bool uses_filament_mapping() const override { return m_info.id == "orca"; }

    void note_mqtt_alive_support(const std::string& dev_id, bool supported) override
    {
        ++mqtt_alive_reports;
        mqtt_alive_dev_id    = dev_id;
        mqtt_alive_supported = supported;
    }

    using OrcaPrinterAgent::deliver_to_sink;

    int send_message(std::string dev_id, std::string json_str, int, int) override
    {
        ++cloud_sends;
        return record_message(std::move(dev_id), std::move(json_str));
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int, int) override
    {
        ++lan_sends;
        return record_message(std::move(dev_id), std::move(json_str));
    }

    int connect_printer(const PrinterConnectionParams& params) override
    {
        connected_ca_file = params.ca_file;
        return 0;
    }

    int set_user_selected_machine(std::string dev_id) override
    {
        ++selections;
        return OrcaPrinterAgent::set_user_selected_machine(std::move(dev_id));
    }

    std::string last_message;
    std::string published_device;
    json published_message;
    std::string connected_ca_file;
    int reply = 0; // what a send returns: 0 when the printer took the message
    int selections{0};
    int lan_sends{0};
    int cloud_sends{0};
    int mqtt_alive_reports{0};
    std::string mqtt_alive_dev_id;
    bool mqtt_alive_supported{false};

private:
    int record_message(std::string dev_id, std::string json_str)
    {
        last_message     = json_str;
        published_device = std::move(dev_id);
        published_message = json::parse(json_str);
        return reply;
    }

    AgentInfo m_info;
};

struct ScopedAppConfig
{
    AppConfig config;
};

struct ScopedPrinterModelRegistry
{
    using PrinterModelIdentity = DevPrinterConfigUtil::PrinterModelIdentity;

    explicit ScopedPrinterModelRegistry(std::vector<PrinterModelIdentity> models)
    {
        DevPrinterConfigUtil::set_installed_printer_models(std::move(models));
    }
    ~ScopedPrinterModelRegistry() { DevPrinterConfigUtil::set_installed_printer_models(std::vector<PrinterModelIdentity>{}); }

    ScopedPrinterModelRegistry(const ScopedPrinterModelRegistry&)            = delete;
    ScopedPrinterModelRegistry& operator=(const ScopedPrinterModelRegistry&) = delete;
};

std::string machine_list_response(const std::string& provider, const std::string& agent_id,
                                  std::uint64_t generation, const std::string& name)
{
    json machine;
    machine["dev_id"]      = "integration-device";
    machine["dev_name"]    = name;
    machine["dev_online"]  = true;
    machine["task_status"] = "idle";

    json response;
    response["provider"]   = provider;
    response["agent_id"]   = agent_id;
    response["generation"] = generation;
    response["devices"]    = json::array({machine});
    return response.dump();
}

} // namespace

TEST_CASE("Network agent stamps user-machine responses with request context", "[DeviceManager][integration]")
{
    auto cloud = std::make_shared<StubCloudAgent>();
    NetworkAgent network(cloud, nullptr);
    network.set_printer_agent(std::make_shared<TestPrinterAgent>("integration-agent-a"));

    const std::uint64_t generation_before = network.get_user_machine_list_generation();
    unsigned int        http_code         = 0;
    std::string         body;
    REQUIRE(network.get_user_print_info(&http_code, &body, ORCA_CLOUD_PROVIDER) == 0);

    const json response = json::parse(body);
    CHECK(http_code == 200);
    CHECK(response["provider"] == ORCA_CLOUD_PROVIDER);
    CHECK(response["agent_id"] == "integration-agent-a");
    CHECK(response["generation"] == generation_before + 1);
}

TEST_CASE("An idle LAN printer is liveness probed once per quiet interval", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "idle-lan", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(21);
    obj.last_printer_message_time = now - std::chrono::seconds(21);
    obj.last_request_push = now - std::chrono::seconds(5); // last start-up retry
    const auto last_update = obj.last_update_time;

    REQUIRE(obj.request_liveness_probe(now));
    CHECK(agent->lan_sends == 1);
    CHECK(agent->cloud_sends == 0);
    CHECK(agent->published_message["pushing"]["command"] == "pushall");
    // Sending is not evidence of life: only the printer's full-status reply may reset this.
    CHECK(obj.last_update_time.time_since_epoch().count() ==
          last_update.time_since_epoch().count());

    // No reply yet: the next tick must not overlap or repeat the probe early.
    CHECK_FALSE(obj.request_liveness_probe(now + std::chrono::seconds(1)));
    CHECK(agent->lan_sends == 1);
    CHECK(obj.last_update_time.time_since_epoch().count() ==
          last_update.time_since_epoch().count());

    // Past the display timeout with no reply: the device still shows disconnected, but an open
    // transport keeps retrying no faster than one probe per quiet interval.
    obj.last_update_time = now - std::chrono::seconds(31);
    obj.last_printer_message_time = now - std::chrono::seconds(31);
    const auto stale = obj.last_update_time;
    CHECK_FALSE(obj.is_connected());
    CHECK(obj.request_liveness_probe(now + std::chrono::seconds(21)));
    CHECK(agent->lan_sends == 2);
    CHECK(obj.last_update_time.time_since_epoch().count() ==
          stale.time_since_epoch().count());
    CHECK_FALSE(obj.is_connected());
}

TEST_CASE("A throttled liveness probe does not consume its probe interval", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "throttled-lan", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(21);
    obj.last_printer_message_time = now - std::chrono::seconds(21);
    obj.last_request_push = now - std::chrono::seconds(1); // inside the 3 s serialization window

    CHECK_FALSE(obj.request_liveness_probe(now));
    CHECK(agent->lan_sends == 0);
    CHECK(obj.last_liveness_probe.time_since_epoch().count() == 0);

    // Once the serialization window has passed, the next tick probes; the failed attempt
    // did not postpone it by the 20 s liveness interval.
    CHECK(obj.request_liveness_probe(now + std::chrono::seconds(3)));
    CHECK(agent->lan_sends == 1);
}

TEST_CASE("An active printer is not liveness probed", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "active-lan", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(5);
    obj.last_printer_message_time = now - std::chrono::seconds(5);

    CHECK_FALSE(obj.request_liveness_probe(now));
    CHECK(agent->lan_sends == 0);
    CHECK(agent->cloud_sends == 0);
}

TEST_CASE("The liveness probe uses the cloud path for a cloud printer", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "idle-cloud", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "cloud";
    obj.m_is_online = true;

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(21);
    obj.last_printer_message_time = now - std::chrono::seconds(21);

    REQUIRE(obj.request_liveness_probe(now));
    CHECK(agent->cloud_sends == 1);
    CHECK(agent->lan_sends == 0);
    CHECK(agent->published_message["pushing"]["command"] == "pushall");
}

TEST_CASE("Liveness probing ignores support_mqtt_alive and cloud sign-in", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "idle-suppressed", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);
    obj.is_support_mqtt_alive = true;

    CHECK_FALSE(network.is_user_login(ORCA_CLOUD_PROVIDER));

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(21);
    obj.last_printer_message_time = now - std::chrono::seconds(21);

    REQUIRE(obj.request_liveness_probe(now));
    CHECK(agent->lan_sends == 1);
}

TEST_CASE("A printer's mqtt alive capability reaches the owning agent", "[DeviceManager][Liveness]")
{
    ScopedAppConfig app_config;
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "capability-device";
    machine.dev_name     = "Capability printer";
    machine.dev_ip       = "192.0.2.43";
    machine.printer_type = "C11";
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "access-code") != nullptr);
    REQUIRE(manager.set_selected_machine(machine.dev_id));

    MachineObject* obj = manager.get_selected_machine();
    REQUIRE(obj != nullptr);
    obj->printer_type.clear(); // keep the status parse free of model-profile lookups
    REQUIRE(agent->mqtt_alive_reports == 0);

    // The print-status path is where version-profile merged data lands.
    obj->parse_json("lan", R"({"print":{"command":"push_status","msg":0,"support_mqtt_alive":true}})");
    REQUIRE(agent->mqtt_alive_reports == 1);
    CHECK(agent->mqtt_alive_dev_id == "capability-device");
    CHECK(agent->mqtt_alive_supported);

    // The get_capabilities reply carries the same flag under its flags object.
    obj->parse_json("lan", R"({"info":{"command":"get_capabilities","capabilities":{"flags":{"support_mqtt_alive":false}}}})");
    REQUIRE(agent->mqtt_alive_reports == 2);
    CHECK(agent->mqtt_alive_dev_id == "capability-device");
    CHECK_FALSE(agent->mqtt_alive_supported);

    // A device owned by another agent is not handed to the active one.
    obj->printer_agent_id = "different-agent";
    obj->parse_json("lan", R"({"print":{"command":"push_status","msg":0,"support_mqtt_alive":true}})");
    CHECK(agent->mqtt_alive_reports == 2);
}

TEST_CASE("An unsupported automatic liveness probe stays quiet", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "unsupported-lan", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);
    agent->reply = ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(21);
    obj.last_printer_message_time = now - std::chrono::seconds(21);

    // No wxApp exists here: a probe that reported unsupported would try to open a dialog.
    CHECK_FALSE(obj.request_liveness_probe(now));
    CHECK(agent->lan_sends == 1);
}

TEST_CASE("The selected LAN printer is probed, and loss or deselection stops it", "[DeviceManager][Liveness]")
{
    ScopedAppConfig app_config;
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "selected-lan-device";
    machine.dev_name     = "Selected LAN printer";
    machine.dev_ip       = "192.0.2.41";
    machine.printer_type = "C11";
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "access-code") != nullptr);
    REQUIRE(manager.set_selected_machine(machine.dev_id));

    MachineObject* obj = manager.get_selected_machine();
    REQUIRE(obj != nullptr);
    obj->set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj->last_update_time = now - std::chrono::seconds(21);
    obj->last_printer_message_time = now - std::chrono::seconds(21);
    REQUIRE(manager.check_selected_machine_liveness(now));
    CHECK(agent->lan_sends == 1);

    // No reply before the display timeout: the selected path keeps the disconnected display
    // but keeps retrying the open transport at the quiet interval.
    obj->last_update_time = now - std::chrono::seconds(31);
    obj->last_printer_message_time = now - std::chrono::seconds(31);
    const auto stale = obj->last_update_time;
    CHECK_FALSE(obj->is_connected());
    CHECK(manager.check_selected_machine_liveness(now + std::chrono::seconds(31)));
    CHECK(agent->lan_sends == 2);
    CHECK(obj->last_update_time.time_since_epoch().count() ==
          stale.time_since_epoch().count());

    // Simulate a lost LAN session, then its SSDP discovery: discovery restores generic
    // online state but must not make the closed session probeable again.
    obj->set_lan_session_open(false);
    obj->set_online_state(false);
    manager.on_machine_alive(R"({
        "dev_name":"Selected LAN printer",
        "dev_id":"selected-lan-device",
        "dev_ip":"192.0.2.41",
        "dev_type":"C11",
        "dev_signal":"strong",
        "connect_type":"lan",
        "bind_state":"free"
    })");
    CHECK(obj->is_online());
    CHECK_FALSE(obj->is_lan_session_open());
    obj->last_update_time = now - std::chrono::seconds(21);
    obj->last_printer_message_time = now - std::chrono::seconds(21);
    obj->last_request_push = {};
    obj->last_liveness_probe = {};
    CHECK_FALSE(manager.check_selected_machine_liveness(now + std::chrono::seconds(1)));
    CHECK(agent->lan_sends == 2);

    // Deselecting the selected printer also stops all probing.
    REQUIRE(manager.set_selected_machine(""));
    CHECK_FALSE(manager.check_selected_machine_liveness(now + std::chrono::seconds(2)));
    CHECK(agent->lan_sends == 2);
}

TEST_CASE("A full-status reply resets the selected printer's liveness timer", "[DeviceManager][Liveness]")
{
    ScopedAppConfig app_config;
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "full-status-device";
    machine.dev_name     = "Full status printer";
    machine.dev_ip       = "192.0.2.42";
    machine.printer_type = "C11";
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "access-code") != nullptr);
    REQUIRE(manager.set_selected_machine(machine.dev_id));

    MachineObject* obj = manager.get_selected_machine();
    REQUIRE(obj != nullptr);
    obj->set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj->last_update_time = now - std::chrono::seconds(21);
    obj->last_printer_message_time = now - std::chrono::seconds(21);
    REQUIRE(manager.check_selected_machine_liveness(now));
    CHECK(agent->lan_sends == 1);

    const auto before_reply = obj->last_update_time;
    const auto before_message = obj->last_printer_message_time;
    obj->printer_type.clear(); // keep the status parse free of model-profile lookups
    obj->parse_json("lan", R"({"print":{"command":"push_status","msg":0}})");
    CHECK(obj->last_update_time.time_since_epoch().count() >
          before_reply.time_since_epoch().count());
    CHECK(obj->last_printer_message_time.time_since_epoch().count() >
          before_message.time_since_epoch().count());
    CHECK(obj->is_connected());
    CHECK_FALSE(manager.check_selected_machine_liveness(now + std::chrono::seconds(5)));
    CHECK(agent->lan_sends == 1);
}

TEST_CASE("A synthetic status reload does not postpone the liveness probe", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "synthetic-reload", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(21);
    obj.last_printer_message_time = now - std::chrono::seconds(21);
    const auto before_display = obj.last_update_time;
    const auto before_message = obj.last_printer_message_time;

    // reload_printer_settings() sends an empty object through parse_json(); it must keep
    // refreshing the display clock without counting as a printer message for liveness.
    obj.parse_json("cloud", "{}");
    CHECK(obj.last_update_time.time_since_epoch().count() >
          before_display.time_since_epoch().count());
    CHECK(obj.last_printer_message_time.time_since_epoch().count() ==
          before_message.time_since_epoch().count());

    REQUIRE(obj.request_liveness_probe(now));
    CHECK(agent->lan_sends == 1);
}

TEST_CASE("A device-view refresh does not reset the liveness probe window", "[DeviceManager][Liveness]")
{
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    MachineObject obj(nullptr, &network, "test", "view-refresh", "127.0.0.1");
    obj.printer_agent_id = "integration-agent";
    obj.dev_connection_type = "lan";
    obj.set_lan_session_open(true);

    const auto now = std::chrono::system_clock::now();
    obj.last_update_time = now - std::chrono::seconds(31);
    obj.last_printer_message_time = now - std::chrono::seconds(31);
    obj.last_liveness_probe = now - std::chrono::seconds(5);
    const auto before_display = obj.last_update_time;
    const auto before_message = obj.last_printer_message_time;
    const auto before_probe = obj.last_liveness_probe;

    obj.reset_update_time();
    CHECK(obj.last_update_time.time_since_epoch().count() >
          before_display.time_since_epoch().count());
    CHECK(obj.last_printer_message_time.time_since_epoch().count() ==
          before_message.time_since_epoch().count());
    CHECK(obj.last_liveness_probe.time_since_epoch().count() ==
          before_probe.time_since_epoch().count());

    obj.reset_liveness_window();
    CHECK(obj.last_printer_message_time.time_since_epoch().count() >
          before_message.time_since_epoch().count());
    CHECK(obj.last_liveness_probe.time_since_epoch().count() == 0);
}

TEST_CASE("A cloud printer is probed only while its server channel is open", "[DeviceManager][Liveness]")
{
    ScopedAppConfig app_config;
    const auto cloud = std::make_shared<StubCloudAgent>();
    cloud->logged_in        = true;
    cloud->server_connected = true;
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(cloud, agent);
    DeviceManager manager(&network, false, &app_config.config);

    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        network.get_user_machine_list_generation(),
                                                        "Cloud printer"));
    REQUIRE(manager.set_selected_machine("integration-device"));

    MachineObject* obj = manager.get_selected_machine();
    REQUIRE(obj != nullptr);

    const auto now = std::chrono::system_clock::now();
    obj->last_update_time = now - std::chrono::seconds(21);
    obj->last_printer_message_time = now - std::chrono::seconds(21);
    REQUIRE(manager.check_selected_machine_liveness(now));
    CHECK(agent->cloud_sends == 1);
    CHECK(agent->lan_sends == 0);

    // The transport closed: no further probe until the server channel comes back.
    cloud->server_connected = false;
    obj->last_update_time = now - std::chrono::seconds(21);
    obj->last_printer_message_time = now - std::chrono::seconds(21);
    obj->last_request_push = {};
    obj->last_liveness_probe = {};
    CHECK_FALSE(manager.check_selected_machine_liveness(now + std::chrono::seconds(1)));
    CHECK(agent->cloud_sends == 1);
}

TEST_CASE("A selected cloud printer is probed without cloud sign-in", "[DeviceManager][Liveness]")
{
    ScopedAppConfig app_config;
    const auto cloud = std::make_shared<StubCloudAgent>();
    cloud->logged_in        = false;
    cloud->server_connected = true;
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(cloud, agent);
    DeviceManager manager(&network, false, &app_config.config);

    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        network.get_user_machine_list_generation(),
                                                        "Cloud printer"));
    const auto machines = manager.get_user_machinelist();
    REQUIRE(machines.count("integration-device") == 1);
    MachineObject* obj = machines.at("integration-device");
    REQUIRE(obj != nullptr);
    // Legacy cloud entries may have no explicit connection type; selection treats non-LAN
    // as cloud, and the liveness fallback must do the same.
    obj->dev_connection_type.clear();

    REQUIRE(manager.set_selected_machine("integration-device"));

    // The normal selection accessor stays sign-in gated; the liveness accessor must not be.
    CHECK(manager.get_selected_machine() == nullptr);

    const auto now = std::chrono::system_clock::now();
    obj->last_update_time = now - std::chrono::seconds(21);
    obj->last_printer_message_time = now - std::chrono::seconds(21);
    REQUIRE(manager.check_selected_machine_liveness(now));
    CHECK(agent->cloud_sends == 1);
    CHECK(agent->lan_sends == 0);
}

TEST_CASE("Liveness uses the normal selected object when LAN and cloud share an id", "[DeviceManager][Liveness]")
{
    ScopedAppConfig app_config;
    const auto cloud = std::make_shared<StubCloudAgent>();
    cloud->logged_in        = false;
    cloud->server_connected = true;
    const auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(cloud, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "integration-device";
    machine.dev_name     = "Local duplicate";
    machine.dev_ip       = "192.0.2.43";
    machine.printer_type = "C11";
    MachineObject* local = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(local != nullptr);

    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        network.get_user_machine_list_generation(),
                                                        "Cloud duplicate"));
    REQUIRE(manager.set_selected_machine(machine.dev_id));

    // Signed out, the normal accessor resolves the local object, not the cloud duplicate.
    REQUIRE(manager.get_selected_machine() == local);
    local->set_lan_session_open(true);
    const auto now = std::chrono::system_clock::now();
    local->last_update_time = now - std::chrono::seconds(21);
    local->last_printer_message_time = now - std::chrono::seconds(21);

    REQUIRE(manager.check_selected_machine_liveness(now));
    CHECK(agent->lan_sends == 1);
    CHECK(agent->cloud_sends == 0);
}

TEST_CASE("Device manager ignores stale cloud machine responses", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("integration-agent"));
    network.set_printer_agent(std::make_shared<TestPrinterAgent>("integration-agent"));
    DeviceManager manager(&network, false, &app_config.config);

    const std::uint64_t current_generation = network.get_user_machine_list_generation();
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        current_generation, "Fresh name"));

    auto machines = manager.get_user_machinelist();
    REQUIRE(machines.size() == 1);
    REQUIRE(machines.at("integration-device") != nullptr);
    CHECK(machines.at("integration-device")->get_dev_name() == "Fresh name");

    manager.parse_user_print_info(machine_list_response(BBL_CLOUD_PROVIDER, "integration-agent",
                                                        current_generation, "Stale provider"));
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "other-agent",
                                                        current_generation, "Stale agent"));
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        current_generation + 1, "Stale generation"));

    machines = manager.get_user_machinelist();
    REQUIRE(machines.size() == 1);
    CHECK(machines.at("integration-device")->get_dev_name() == "Fresh name");
}

TEST_CASE("Device manager filters and rehomes devices by printer-agent ownership", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto agent_a = std::make_shared<TestPrinterAgent>("integration-agent-a");
    auto agent_b = std::make_shared<TestPrinterAgent>("integration-agent-b");
    NetworkAgent network(nullptr, agent_a);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "integration-lan-device";
    machine.dev_name     = "Integration LAN device";
    machine.dev_ip       = "192.0.2.10";
    machine.printer_type = "C11";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    CHECK(object->printer_agent_id == "integration-agent-a");
    CHECK(manager.get_my_machine_list("integration-agent-a").count(machine.dev_id) == 1);
    CHECK(manager.get_my_machine_list("integration-agent-b").empty());

    network.set_printer_agent(agent_b);
    CHECK(manager.get_my_machine_list("integration-agent-b").empty());

    manager.on_machine_alive(R"({
        "dev_name":"Rediscovered device",
        "dev_id":"integration-lan-device",
        "dev_ip":"192.0.2.10",
        "dev_type":"C11",
        "dev_signal":"strong",
        "connect_type":"lan",
        "bind_state":"free"
    })");

    CHECK(object->printer_agent_id == "integration-agent-b");
    CHECK(manager.get_my_machine_list("integration-agent-a").empty());
    CHECK(manager.get_my_machine_list("integration-agent-b").count(machine.dev_id) == 1);
}

TEST_CASE("A saved printer model is unverified until the printer advertises it", "[DeviceManager][integration]")
{
    const ScopedPrinterModelRegistry registry({{"C11", {"C11", "Bambu Lab P1P"}}});
    ScopedAppConfig app_config;
    const std::string agent_id = "integration-agent";
    BBLocalMachine saved;
    saved.dev_id           = "saved-model-device";
    saved.dev_name         = "Saved model printer";
    saved.dev_ip           = "192.0.2.30";
    saved.printer_type     = "C11";
    saved.printer_agent_id = agent_id;
    saved.access_code      = "access-code";
    app_config.config.update_local_machine(saved);

    auto agent = std::make_shared<TestPrinterAgent>(agent_id);
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    MachineObject* object = manager.get_local_machine(saved.dev_id);
    REQUIRE(object != nullptr);
    CHECK(object->printer_type == "C11");
    CHECK_FALSE(object->has_advertised_printer_model());

    // An empty dev_type makes no model claim and must not confirm the saved identity.
    manager.on_machine_alive(R"({
        "dev_name":"Saved model printer",
        "dev_id":"saved-model-device",
        "dev_ip":"192.0.2.30",
        "dev_type":"",
        "dev_signal":"strong",
        "connect_type":"lan",
        "bind_state":"free"
    })");
    CHECK_FALSE(object->has_advertised_printer_model());

    // Any agent can advertise the model; no agent-specific list governs identity.
    manager.on_machine_alive(R"({
        "dev_name":"Saved model printer",
        "dev_id":"saved-model-device",
        "dev_ip":"192.0.2.30",
        "dev_type":"C11",
        "dev_signal":"strong",
        "connect_type":"lan",
        "bind_state":"free"
    })");
    CHECK(object->printer_type == "C11");
    CHECK(object->has_advertised_printer_model());
}

TEST_CASE("Only a model reported by the printer is marked advertised on insert", "[DeviceManager][integration]")
{
    const ScopedPrinterModelRegistry registry({{"C11", {"C11", "Bambu Lab P1P"}}});
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine preset_model;
    preset_model.dev_id       = "preset-model-device";
    preset_model.dev_name     = "Preset model printer";
    preset_model.dev_ip       = "192.0.2.31";
    preset_model.printer_type = "C11";
    MachineObject* preset_object = manager.insert_local_device(preset_model, "lan", "free", "", "access-code");
    REQUIRE(preset_object != nullptr);
    CHECK(preset_object->printer_type == "C11");
    CHECK_FALSE(preset_object->has_advertised_printer_model());
    // An unverified preset-derived model must not block compatibility checks.
    CHECK(DevPrinterConfigUtil::is_printer_model_compatible("C11", *preset_object));

    BBLocalMachine advertised_model;
    advertised_model.dev_id       = "advertised-model-device";
    advertised_model.dev_name     = "Advertised model printer";
    advertised_model.dev_ip       = "192.0.2.32";
    advertised_model.printer_type = "C11";
    MachineObject* advertised_object = manager.insert_local_device(advertised_model, "lan", "free", "", "access-code", true);
    REQUIRE(advertised_object != nullptr);
    CHECK(advertised_object->printer_type == "C11");
    CHECK(advertised_object->has_advertised_printer_model());
}

TEST_CASE("An unrecognized advertised model stays unknown and does not block", "[DeviceManager][integration]")
{
    const ScopedPrinterModelRegistry registry({{"C11", {"C11", "Bambu Lab P1P"}}});
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine unknown_model;
    unknown_model.dev_id       = "unknown-model-device";
    unknown_model.dev_name     = "Unknown model printer";
    unknown_model.dev_ip       = "192.0.2.33";
    unknown_model.printer_type = "NewModel";
    MachineObject* unknown_object = manager.insert_local_device(unknown_model, "lan", "free", "", "access-code", true);
    REQUIRE(unknown_object != nullptr);
    CHECK(unknown_object->printer_type.empty());
    CHECK_FALSE(unknown_object->has_advertised_printer_model());
    CHECK(DevPrinterConfigUtil::is_optional_printer_model_id("NewModel"));
    CHECK(DevPrinterConfigUtil::is_printer_model_compatible("C11", *unknown_object));

    // A later positive report of a different unrecognized model also clears the old identity.
    BBLocalMachine known_model;
    known_model.dev_id       = "known-model-device";
    known_model.dev_name     = "Known model printer";
    known_model.dev_ip       = "192.0.2.34";
    known_model.printer_type = "C11";
    MachineObject* known_object = manager.insert_local_device(known_model, "lan", "free", "", "access-code", true);
    REQUIRE(known_object != nullptr);
    REQUIRE(known_object->has_advertised_printer_model());

    manager.on_machine_alive(R"({
        "dev_name":"Known model printer",
        "dev_id":"known-model-device",
        "dev_ip":"192.0.2.34",
        "dev_type":"NewModel",
        "dev_signal":"strong",
        "connect_type":"lan",
        "bind_state":"free"
    })");
    CHECK(known_object->printer_type.empty());
    CHECK_FALSE(known_object->has_advertised_printer_model());
}

TEST_CASE("An installed vendor model name resolves to its canonical id", "[DeviceManager][integration]")
{
    const ScopedPrinterModelRegistry registry({{"Qidi-Q2", {"Qidi-Q2", "Qidi Q2"}}});
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    CHECK(DevPrinterConfigUtil::resolve_printer_model_id("Qidi-Q2") == "Qidi-Q2");
    CHECK(DevPrinterConfigUtil::resolve_printer_model_id("Qidi Q2") == "Qidi-Q2");

    BBLocalMachine machine;
    machine.dev_id       = "qidi-model-device";
    machine.dev_name     = "Qidi printer";
    machine.dev_ip       = "192.0.2.35";
    machine.printer_type = "Qidi Q2";
    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code", true);
    REQUIRE(object != nullptr);
    CHECK(object->printer_type == "Qidi-Q2");
    CHECK(object->has_advertised_printer_model());
    CHECK(DevPrinterConfigUtil::is_printer_model_compatible("Qidi-Q2", *object));
    // An alias/name on the source side is normalized before equality.
    CHECK(DevPrinterConfigUtil::is_printer_model_compatible("Qidi Q2", *object));
}

TEST_CASE("An installed upgrade kit changes the shown model but not the advertised identity", "[DeviceManager][integration]")
{
    const ScopedPrinterModelRegistry registry({{"C11", {"C11", "Bambu Lab P1P"}},
                                               {"C12", {"C12", "Bambu Lab P1P + Upgrade Kit"}}});
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine p1p;
    p1p.dev_id       = "upgrade-kit-device";
    p1p.dev_name     = "P1P with upgrade kit";
    p1p.dev_ip       = "192.0.2.36";
    p1p.printer_type = "C11";
    MachineObject* object = manager.insert_local_device(p1p, "lan", "free", "", "access-code", true);
    REQUIRE(object != nullptr);
    object->is_support_upgrade_kit = true;
    object->installed_upgrade_kit  = true;

    // The printer still advertises C11; only the effective model used for preset comparison is C12.
    CHECK(object->printer_type == "C11");
    CHECK(object->has_advertised_printer_model());
    CHECK(object->get_show_printer_type() == "C12");
}

TEST_CASE("Model recognition follows the live registry and keeps Bambu legacy aliases", "[DeviceManager][integration]")
{
    DevPrinterConfigUtil::set_installed_printer_models(std::vector<DevPrinterConfigUtil::PrinterModelIdentity>{});
    CHECK(DevPrinterConfigUtil::resolve_printer_model_id("C11").empty());
    CHECK(DevPrinterConfigUtil::resolve_printer_model_id("3DPrinter-X1") == "BL-P002");
    CHECK(DevPrinterConfigUtil::resolve_printer_model_id("3DPrinter-X1-Carbon") == "BL-P001");

    {
        // A duplicated profile model entry with the same id is profile data, not an identity
        // conflict; the id still resolves.
        const ScopedPrinterModelRegistry registry({{"C11", {"C11", "Bambu Lab P1P"}},
                                                   {"C11", {"C11", "Bambu Lab P1P"}}});
        CHECK(DevPrinterConfigUtil::resolve_printer_model_id("C11") == "C11");
        CHECK(DevPrinterConfigUtil::resolve_printer_model_id("Bambu Lab P1P") == "C11");
    }
    CHECK(DevPrinterConfigUtil::resolve_printer_model_id("C11").empty());
}

TEST_CASE("Network agent rejects capability queries for a different device owner", "[DeviceManager][integration]")
{
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("bbl"));

    CHECK(network.owns_agent("bbl"));
    CHECK_FALSE(network.owns_agent("orca"));
    CHECK(network.supports_command("bbl", "device", "print.ams_control"));
    CHECK_FALSE(network.supports_command("orca", "device", "print.ams_control"));
    CHECK_FALSE(network.supports_feature("orca", "device", "filament_mapping"));

    // The mapping dialect is queried the same way: a legacy owner never inherits
    // Orca's, and a non-owner is never queried at all.
    CHECK_FALSE(network.uses_filament_mapping("bbl"));
    CHECK_FALSE(network.uses_filament_mapping("orca"));

    NetworkAgent orca_network(nullptr, std::make_shared<TestPrinterAgent>("orca"));
    CHECK(orca_network.uses_filament_mapping("orca"));
    CHECK_FALSE(orca_network.uses_filament_mapping("bbl"));
}

// Send-time mapping policy belongs to the printer agent's dialect, not to an
// agent id: the Orca dialect gets the no-AMS normalization plus both refusals.
TEST_CASE("Per-print mapping send policy follows the owning agent's dialect", "[DeviceManager][integration]")
{
    auto orca_agent = std::make_shared<TestPrinterAgent>("orca");
    NetworkAgent network(nullptr, orca_agent);
    MachineObject obj(nullptr, &network, "test", "orca-mapping-policy", "127.0.0.1");
    obj.printer_agent_id = "orca";

    // No AMS: the auto-selected external spool is dropped before the capability
    // gate, so it cannot refuse a print nobody mapped.
    std::string external_only = R"([{"ams_id":255,"slot_id":0}])";
    CHECK(prepare_filament_mapping_for_send(&obj, external_only, {}) == MappingSendError::none);
    CHECK(external_only.empty());

    // An AMS makes that a real target, but nothing advertised the capability yet.
    obj.GetFilaSystem()->GetAmsList()["0"] = new DevAms("0", 0, DevAms::AMS);
    std::string mapped = R"([{"ams_id":0,"slot_id":0}])";
    CHECK(prepare_filament_mapping_for_send(&obj, mapped, {}) == MappingSendError::unsupported);
    CHECK(mapped == R"([{"ams_id":0,"slot_id":0}])"); // the refusal leaves it intact

    // Once the connector advertises the capability the mapping passes the gate.
    orca_agent->deliver_to_sink(obj.get_dev_id(),
                        R"({"info":{"command":"get_capabilities","supported_features":{"filament_mapping":true}}})",
                        false);
    CHECK(obj.printer_supports_feature("filament_mapping"));
    CHECK(prepare_filament_mapping_for_send(&obj, mapped, {}) == MappingSendError::none);

    // An unrecorded owner still sends through the active agent, so it takes
    // that agent's dialect too rather than falling back to the legacy payload.
    MachineObject unowned(nullptr, &network, "test", "orca-unowned", "127.0.0.1");
    CHECK(unowned.printer_agent_id.empty());
    CHECK(unowned.printer_uses_filament_mapping());
    std::string unowned_external = R"([{"ams_id":255,"slot_id":0}])";
    CHECK(prepare_filament_mapping_for_send(&unowned, unowned_external, {}) == MappingSendError::none);
    CHECK(unowned_external.empty()); // the Orca normalization ran

    // A partially mapped print is refused even while the capability holds.
    FilamentInfo mapped_entry;
    mapped_entry.ams_id = "0";
    mapped_entry.slot_id = "0";
    FilamentInfo unmapped_entry;
    CHECK(prepare_filament_mapping_for_send(&obj, mapped, {mapped_entry, unmapped_entry}) ==
          MappingSendError::incomplete);
}

// The legacy payload is the thing being protected: no normalization, no refusal.
TEST_CASE("A legacy printer agent keeps its mapping payload untouched", "[DeviceManager][integration]")
{
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("bbl"));
    MachineObject obj(nullptr, &network, "test", "bbl-mapping-policy", "127.0.0.1");
    obj.printer_agent_id = "bbl";

    FilamentInfo mapped_entry;
    mapped_entry.ams_id = "0";
    mapped_entry.slot_id = "0";
    FilamentInfo unmapped_entry;

    std::string external_only = R"([{"ams_id":255,"slot_id":0}])";
    CHECK(prepare_filament_mapping_for_send(&obj, external_only, {mapped_entry, unmapped_entry}) ==
          MappingSendError::none);
    CHECK(external_only == R"([{"ams_id":255,"slot_id":0}])");
}

TEST_CASE("AMS metadata retains setting_id for legacy printer agents", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto printer_agent = std::make_shared<TestPrinterAgent>("bbl");
    NetworkAgent network(nullptr, printer_agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "bbl-setting-id";
    machine.dev_name     = "Bambu setting id";
    machine.dev_ip       = "192.0.2.32";
    machine.printer_type = "C11";
    MachineObject* obj = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(obj != nullptr);

    REQUIRE(obj->command_ams_filament_settings(0, 1, "GFL99", "preset-setting", "00FF00FF", "PLA", 190, 220) == 0);
    const json payload = json::parse(printer_agent->last_message);
    CHECK(payload["print"]["setting_id"] == "preset-setting");
}

TEST_CASE("AMS user settings do not update local state when publishing fails", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto printer_agent = std::make_shared<TestPrinterAgent>("bbl");
    NetworkAgent network(nullptr, printer_agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "bbl-ams-setting-failure";
    machine.dev_name     = "Bambu AMS settings";
    machine.dev_ip       = "192.0.2.33";
    machine.printer_type = "C11";
    MachineObject* obj = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(obj != nullptr);

    auto& settings = obj->GetFilaSystem()->GetAmsSystemSetting();
    settings.SetDetectOnInsertEnabled(false);
    settings.SetDetectOnPowerupEnabled(false);
    settings.SetDetectRemainEnabled(false);
    printer_agent->reply = -1;

    CHECK(obj->command_ams_user_settings(true, true, true) != 0);
    CHECK(settings.IsDetectOnInsertEnabled() == false);
    CHECK(settings.IsDetectOnPowerupEnabled() == false);
    CHECK(settings.IsDetectRemainEnabled() == false);
}

// The AMS dialogs resolve their filament list from the connected device's model. OrcaSonar's
// model id is optional (the agent falls back to "orcasonar"), so the resolver must stand in
// with the selected printer profile instead of yielding no model at all.
TEST_CASE("Filament printer model resolution falls back to the selected profile", "[DeviceManager][integration]")
{
    PresetBundle bundle;

    VendorProfile qidi("Qidi");
    qidi.name = "Qidi";
    VendorProfile::PrinterModel model;
    model.model_id = "Qidi-Q1Pro";
    model.name     = "Qidi Q1 Pro";
    qidi.models.push_back(model);
    bundle.vendors.emplace(qidi.id, qidi);

    // A vendor model id the device reported resolves through the vendor catalog.
    CHECK(resolve_filament_printer_model("Qidi-Q1Pro", &bundle) == "Qidi Q1 Pro");

    // The OrcaSonar fallback id has no vendor model; the selected profile stands in.
    CHECK(resolve_filament_printer_model("orcasonar", &bundle).empty());
    bundle.printers.get_selected_preset().config.set_key_value("printer_model", new ConfigOptionString("Generic Klipper Printer"));
    CHECK(resolve_filament_printer_model("orcasonar", &bundle) == "Generic Klipper Printer");
    CHECK(resolve_filament_printer_model("", &bundle) == "Generic Klipper Printer");

    CHECK(resolve_filament_printer_model("orcasonar", nullptr).empty());
}

// The per-tray K/N records are Bambu firmware's flow-dynamics calibration. Agents with no
// printer-side records must not offer the AMS K/N controls (they would show a synthesized
// default and then refuse to confirm it).
TEST_CASE("Flow-dynamics K/N is offered for Bambu agents only", "[DeviceManager][integration]")
{
    MachineObject bbl(nullptr, nullptr, "test", "bbl-device", "127.0.0.1");
    bbl.printer_agent_id = "bbl";
    CHECK(bbl.supports_extrusion_cali());

    MachineObject orca(nullptr, nullptr, "test", "orca-device", "127.0.0.1");
    orca.printer_agent_id = "orca";
    CHECK_FALSE(orca.supports_extrusion_cali());

    MachineObject moonraker(nullptr, nullptr, "test", "moonraker-device", "127.0.0.1");
    moonraker.printer_agent_id = "moonraker";
    CHECK_FALSE(moonraker.supports_extrusion_cali());

    // No agent id predates the agent split and keeps the Bambu path.
    MachineObject legacy(nullptr, nullptr, "test", "legacy-device", "127.0.0.1");
    CHECK(legacy.supports_extrusion_cali());
}

TEST_CASE("A non-Bambu device without a model is not given Bambu's C11 model", "[DeviceManager][integration]")
{
    const std::string agent_id = GENERATE(std::string(MOONRAKER_PRINTER_AGENT_ID),
                                          std::string(QIDI_PRINTER_AGENT_ID),
                                          std::string(SNAPMAKER_PRINTER_AGENT_ID),
                                          std::string(CREALITYPRINT_PRINTER_AGENT_ID),
                                          std::string("integration-agent"));
    CAPTURE(agent_id);

    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>(agent_id);
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id   = "moonraker-device";
    machine.dev_name = "Moonraker printer";
    machine.dev_ip   = "192.0.2.20";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    CHECK(object->printer_agent_id == agent_id);
    CHECK(object->printer_type.empty());
}

TEST_CASE("A Bambu device without a model still gets the C11 fallback", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>(BBL_PRINTER_AGENT_ID);
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id   = "bambu-device";
    machine.dev_name = "Bambu printer";
    machine.dev_ip   = "192.0.2.21";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    CHECK(object->printer_type == "C11");
}

TEST_CASE("A known model survives a non-Bambu rebind that does not name one", "[DeviceManager][integration]")
{
    const std::string agent_id = GENERATE(std::string(MOONRAKER_PRINTER_AGENT_ID),
                                          std::string(QIDI_PRINTER_AGENT_ID),
                                          std::string(SNAPMAKER_PRINTER_AGENT_ID),
                                          std::string(CREALITYPRINT_PRINTER_AGENT_ID),
                                          std::string("integration-agent"));
    CAPTURE(agent_id);

    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>(agent_id);
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "known-model-device";
    machine.dev_name     = "Known model printer";
    machine.dev_ip       = "192.0.2.22";
    machine.printer_type = "test_model";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    REQUIRE(object->printer_type == "test_model");

    machine.printer_type.clear();
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "access-code") == object);
    CHECK(object->printer_type == "test_model");
}

TEST_CASE("A Bambu rebind without a model keeps the C11 fallback", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>(BBL_PRINTER_AGENT_ID);
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "known-model-device";
    machine.dev_name     = "Known model printer";
    machine.dev_ip       = "192.0.2.22";
    machine.printer_type = "BL-P001";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    REQUIRE(object->printer_type == "BL-P001");

    machine.printer_type.clear();
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "access-code") == object);
    CHECK(object->printer_type == "C11");
}

TEST_CASE("Device manager remembers the last selected machine per printer agent", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto agent_a = std::make_shared<TestPrinterAgent>("integration-agent-a");
    auto agent_b = std::make_shared<TestPrinterAgent>("integration-agent-b");
    NetworkAgent network(nullptr, agent_a);
    DeviceManager manager(&network, false, &app_config.config);

    manager.record_user_last_machine("machine-of-a");
    network.set_printer_agent(agent_b);
    manager.record_user_last_machine("machine-of-b");

    network.set_printer_agent(agent_a);
    CHECK(manager.get_user_last_machine() == "machine-of-a");
    network.set_printer_agent(agent_b);
    CHECK(manager.get_user_last_machine() == "machine-of-b");

    network.set_printer_agent(nullptr);
    manager.record_user_last_machine("machine-without-agent");
    CHECK(app_config.config.get_section("last_selected_machine").size() == 2);

    // A forget leaves no entry behind, an empty one included.
    network.set_printer_agent(agent_b);
    manager.record_user_last_machine("");
    CHECK(app_config.config.get_section("last_selected_machine").count("integration-agent-b") == 0);
    app_config.config.set("last_selected_machine", "integration-agent-b", "");
    manager.record_user_last_machine("");
    CHECK(app_config.config.get_section("last_selected_machine").count("integration-agent-b") == 0);
    network.set_printer_agent(agent_a);
    CHECK(manager.get_user_last_machine() == "machine-of-a");
}

TEST_CASE("Device manager reads the pre-per-agent last machine for Bambu printers until it records one", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    app_config.config.set("user_last_selected_machine", "legacy-machine");
    auto other = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, other);
    DeviceManager manager(&network, false, &app_config.config);

    CHECK(manager.get_user_last_machine() != "legacy-machine");
    network.set_printer_agent(std::make_shared<TestPrinterAgent>(BBL_PRINTER_AGENT_ID));
    CHECK(manager.get_user_last_machine() == "legacy-machine");

    // A forget is a record too: the legacy machine must not come back after it.
    manager.record_user_last_machine("");
    CHECK_FALSE(app_config.config.has("user_last_selected_machine"));
    CHECK(manager.get_user_last_machine() != "legacy-machine");
}

TEST_CASE("Device manager keeps a pending last-machine restore until a cloud list is accepted", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("integration-agent"));
    network.set_printer_agent(std::make_shared<TestPrinterAgent>("integration-agent"));
    DeviceManager manager(&network, false, &app_config.config);
    // Remembered but in no list yet, as a cloud printer is until the account's list arrives.
    manager.record_user_last_machine("remembered-cloud-machine");

    manager.restore_last_machine();
    REQUIRE(manager.has_pending_last_machine_restore());

    const std::uint64_t generation = network.get_user_machine_list_generation();
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent", generation + 1, "Superseded"));
    CHECK(manager.has_pending_last_machine_restore());
    // Responses that leave the list as it is, with no device list or one that can't be read, don't settle it either.
    manager.parse_user_print_info("{}");
    manager.parse_user_print_info(R"({"devices":null})");
    manager.parse_user_print_info(R"({"devices":"not a list"})");
    CHECK(manager.has_pending_last_machine_restore());

    // Parsed directly, as the Device tab's printer list and the print dialogs do.
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent", generation, "Accepted"));
    CHECK(manager.get_user_machinelist().count("integration-device") == 1);
    CHECK_FALSE(manager.has_pending_last_machine_restore());
}

TEST_CASE("Logging out cancels a pending last-machine restore", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("integration-agent"));
    network.set_printer_agent(std::make_shared<TestPrinterAgent>("integration-agent"));
    DeviceManager manager(&network, false, &app_config.config);
    manager.record_user_last_machine("remembered-cloud-machine");

    manager.restore_last_machine();
    REQUIRE(manager.has_pending_last_machine_restore());
    manager.clean_user_info(true);
    CHECK_FALSE(manager.has_pending_last_machine_restore());
}

TEST_CASE("A cloud server connection selects a cloud printer again but leaves a LAN printer alone", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    network.set_printer_agent(agent);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "integration-lan-device";
    machine.dev_name     = "Integration LAN device";
    machine.dev_ip       = "192.0.2.10";
    machine.printer_type = "C11";
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "access-code") != nullptr);
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        network.get_user_machine_list_generation(), "Cloud printer"));
    REQUIRE(manager.get_user_machinelist().count("integration-device") == 1);

    // The agent names the LAN printer once it is connected, as the Bambu network plugin does.
    agent->set_user_selected_machine(machine.dev_id);
    agent->selections = 0;
    manager.reselect_cloud_machine();
    CHECK(agent->selections == 0);

    agent->set_user_selected_machine("integration-device");
    agent->selections = 0;
    manager.reselect_cloud_machine();
    CHECK(agent->selections == 1);
    CHECK(agent->get_user_selected_machine() == "integration-device");
}

TEST_CASE("A CA file stays with the agent that saved it when a device is rebound", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    auto agent_a = std::make_shared<TestPrinterAgent>("integration-agent-a");
    auto agent_b = std::make_shared<TestPrinterAgent>("integration-agent-b");
    NetworkAgent network(nullptr, agent_a);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "integration-lan-device";
    machine.dev_name     = "Integration LAN device";
    machine.dev_ip       = "192.0.2.10";
    machine.printer_type = "C11";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    object->set_ca_file("agent-a-ca.crt");
    CHECK(app_config.config.get_local_machines().at(machine.dev_id).ca_file == "agent-a-ca.crt");

    // Rebinding under the same agent keeps the CA file.
    manager.insert_local_device(machine, "lan", "free", "", "access-code");
    CHECK(object->get_ca_file() == "agent-a-ca.crt");

    network.set_printer_agent(agent_b);
    manager.insert_local_device(machine, "lan", "free", "", "access-code");
    CHECK(object->printer_agent_id == "integration-agent-b");
    CHECK(object->get_ca_file().empty());
    CHECK(app_config.config.get_local_machines().at(machine.dev_id).ca_file.empty());
}

TEST_CASE("A device saved without an agent id connects with the CA file of the agent that connects it", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    const std::string agent_id = GENERATE(std::string(BBL_PRINTER_AGENT_ID), std::string("integration-agent"));
    auto agent = std::make_shared<TestPrinterAgent>(agent_id);
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.set_access_code("access-code", false);
    object.set_ca_file("device-ca.crt", false);
    REQUIRE(object.printer_agent_id.empty());

    REQUIRE(object.connect() == 0);
    // Other agents take the CA from the printer preset, which this test has none of.
    CHECK(agent->connected_ca_file == (agent_id == BBL_PRINTER_AGENT_ID ? "device-ca.crt" : ""));
}

TEST_CASE("AMS filament settings keep legacy single-color commands compatible", "[DeviceManager][AMSMaterialsSetting][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.dev_connection_type = GENERATE("lan", "cloud");

    REQUIRE(object.command_ams_filament_settings(1, 2, "filament-id", "setting-id", "FF000080", "PLA", 190, 220) == 0);

    const json& payload = agent->published_message.at("print");
    CHECK(agent->published_device == object.get_dev_id());
    CHECK(payload.at("command") == "ams_filament_setting");
    CHECK(payload.at("ams_id") == 1);
    CHECK(payload.at("slot_id") == 2);
    CHECK(payload.at("tray_color") == "FF000080");
    CHECK_FALSE(payload.contains("cols"));
    CHECK_FALSE(payload.contains("ctype"));
}

TEST_CASE("AMS filament settings preserve ordered gradient and multicolor RGBA components", "[DeviceManager][AMSMaterialsSetting][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.dev_connection_type = GENERATE("lan", "cloud");
    const int color_type = GENERATE(0, 1);
    const std::vector<std::string> colors{"FF000000", "0000FF40", "00FF00FF", "FF000000", "12345601", "FEDCBAFE", "ABCDEF80"};

    REQUIRE(object.command_ams_filament_settings(1, 2, "filament-id", "setting-id", colors.front(), "PLA", 190, 220,
                                                 colors, color_type) == 0);

    const json& payload = agent->published_message.at("print");
    CHECK(payload.at("tray_color") == colors.front());
    CHECK(payload.at("cols").get<std::vector<std::string>>() == colors);
    CHECK(payload.at("ctype") == color_type);
}

TEST_CASE("AMS filament settings default a supplied single-color list to type two", "[DeviceManager][AMSMaterialsSetting][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.dev_connection_type = GENERATE("lan", "cloud");
    const std::vector<std::string> colors{"FF000080"};

    REQUIRE(object.command_ams_filament_settings(1, 2, "filament-id", "setting-id", colors.front(), "PLA", 190, 220,
                                                 colors) == 0);

    const json& payload = agent->published_message.at("print");
    CHECK(payload.at("cols").get<std::vector<std::string>>() == colors);
    CHECK(payload.at("ctype") == 2);
}

namespace {

// A printer in `state`, connected through the printer agent double, whose serial number (dev_id) is SERIAL-0123.
struct ConnectedPrinter
{
    ScopedAppConfig                   app_config;
    std::shared_ptr<TestPrinterAgent> agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent                      network{nullptr, agent};
    DeviceManager                     manager{&network, false, &app_config.config};
    MachineObject                     printer{&manager, &network, "Test printer", "SERIAL-0123", "192.0.2.10"};

    explicit ConnectedPrinter(const std::string& state)
    {
        printer.print_status        = state;
        printer.dev_connection_type = "lan";
    }
};

// The code of the refusal of `command` to `printer`, or "" for none.
std::string refusal_code(MachineObject* printer, GUI::Ops::JobCommand command)
{
    const std::optional<GUI::Ops::Refusal> refusal = GUI::Ops::job_command_refusal(printer, command);
    return refusal ? refusal->code : "";
}

} // namespace

TEST_CASE("A job command is refused unless the printer's state allows it", "[DeviceManager][Printer]")
{
    using GUI::Ops::JobCommand;
    CHECK(refusal_code(nullptr, JobCommand::Pause) == "no_printer_connected");
    CHECK(refusal_code(nullptr, JobCommand::Resume) == "no_printer_connected");
    CHECK(refusal_code(nullptr, JobCommand::Stop) == "no_printer_connected");

    // Pause, Resume and Stop, as the Device tab enables its buttons: can_pause(), can_resume(), can_abort().
    const auto [state, pause, resume, stop] = GENERATE(table<std::string, std::string, std::string, std::string>({
        {"RUNNING", "", "printer_state", ""},
        {"PAUSE", "printer_state", "", ""},
        {"IDLE", "printer_state", "printer_state", "printer_state"},
    }));
    INFO(state);
    ConnectedPrinter p(state);
    CHECK(refusal_code(&p.printer, JobCommand::Pause) == pause);
    CHECK(refusal_code(&p.printer, JobCommand::Resume) == resume);
    CHECK(refusal_code(&p.printer, JobCommand::Stop) == stop);
    // A refusal by the state names it.
    if (const std::optional<GUI::Ops::Refusal> refusal = GUI::Ops::job_command_refusal(&p.printer, JobCommand::Resume))
        CHECK(refusal->args == std::vector<std::string>{state});
}

TEST_CASE("Resume is refused where the job resumes only at the printer, and nothing is published", "[DeviceManager][Printer]")
{
    ConnectedPrinter p("PAUSE");
    // A processing task such as laser, which check_resume_condition() tells the person to resume at the printer.
    p.printer.jobState_ = 2;
    REQUIRE(p.printer.resume_only_at_printer());
    const GUI::Ops::Outcome<GUI::Ops::JobCommandSent> sent = GUI::Ops::send_job_command(&p.printer, GUI::Ops::JobCommand::Resume);
    REQUIRE_FALSE(sent);
    CHECK(std::string(sent.refusal().code) == "resume_at_printer");
    CHECK(p.agent->published_message.is_null());
}

TEST_CASE("Pause, resume and stop publish the printer's command", "[DeviceManager][Printer]")
{
    using GUI::Ops::JobCommand;
    const auto [state, command, published] = GENERATE(table<std::string, JobCommand, std::string>({
        {"RUNNING", JobCommand::Pause, "pause"},
        {"PAUSE", JobCommand::Resume, "resume"},
        {"PAUSE", JobCommand::Stop, "stop"},
    }));
    INFO(published);
    ConnectedPrinter p(state);
    const GUI::Ops::Outcome<GUI::Ops::JobCommandSent> sent = GUI::Ops::send_job_command(&p.printer, command);
    REQUIRE(sent);
    CHECK(sent.value().printer == "Test printer");
    CHECK(sent.value().state == state);
    CHECK(p.agent->published_device == "SERIAL-0123");
    CHECK(p.agent->published_message.at("print").at("command") == published);
}

TEST_CASE("A job command the printer did not take is not reported sent", "[DeviceManager][Printer]")
{
    ConnectedPrinter p("RUNNING");
    p.agent->reply = -1;
    const GUI::Ops::Outcome<GUI::Ops::JobCommandSent> sent = GUI::Ops::send_job_command(&p.printer, GUI::Ops::JobCommand::Pause);
    REQUIRE_FALSE(sent);
    CHECK(std::string(sent.refusal().code) == "not_sent");
    CHECK(sent.refusal().args == std::vector<std::string>{"-1"});
}

TEST_CASE("A command the printer's agent cannot send is named unsupported", "[DeviceManager][Printer]")
{
    CHECK(std::string(GUI::Ops::job_command_failure(ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED).code) == "command_not_supported");
    CHECK(std::string(GUI::Ops::job_command_failure(ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE).code) == "command_not_supported");
    CHECK(std::string(GUI::Ops::job_command_failure(-1).code) == "not_sent");
}

TEST_CASE("The printer's status is read from the machine, publishes nothing and leaves out its serial number", "[DeviceManager][Printer]")
{
    CHECK(GUI::Mcp::printer_status_reply(GUI::Ops::printer_status(nullptr)) == json{{"connected", false}});

    ConnectedPrinter p("RUNNING");
    p.printer.printer_type     = "test-model";
    p.printer.mc_print_stage   = 2;
    p.printer.mc_print_percent = 42;
    p.printer.mc_left_time     = 600;
    p.printer.curr_layer       = 3;
    p.printer.total_layers     = 100;
    p.printer.subtask_name     = "cube";
    p.printer.print_error      = 0;
    const json reply = GUI::Mcp::printer_status_reply(GUI::Ops::printer_status(&p.printer));
    INFO(reply.dump());
    CHECK(reply.at("connected") == true);
    CHECK(reply.at("name") == "Test printer");
    CHECK(reply.at("printer_type") == "test-model");
    // No preset bundle and no printer file names the model, so it is unknown.
    CHECK(reply.at("model").is_null());
    CHECK(reply.at("state") == "RUNNING");
    CHECK(reply.at("stage") == 2);
    CHECK(reply.at("percent") == 42);
    CHECK(reply.at("remaining_seconds") == 600);
    CHECK(reply.at("layer") == 3);
    CHECK(reply.at("total_layers") == 100);
    CHECK(reply.at("file") == "cube");
    CHECK(reply.at("error_code") == 0);
    CHECK(reply.at("temperatures").at("nozzles").is_array());
    CHECK(reply.at("temperatures").contains("bed"));
    CHECK(reply.at("temperatures").contains("chamber"));
    CHECK(reply.dump().find("SERIAL-0123") == std::string::npos);
    CHECK(p.agent->published_message.is_null());

    // A model the tab names without a bundle.
    p.printer.printer_type = "orcasonar";
    CHECK(GUI::Ops::printer_model_name(p.printer, nullptr) == "OrcaSonar Printer");
}
