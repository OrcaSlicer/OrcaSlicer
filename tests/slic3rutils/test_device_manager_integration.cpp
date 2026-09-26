#include <catch2/catch_all.hpp>

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <libslic3r/AppConfig.hpp>
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <cstdint>
#include <string>
#include <utility>

using namespace Slic3r;
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
};

class TestPrinterAgent final : public OrcaPrinterAgent
{
public:
    explicit TestPrinterAgent(std::string id)
        : OrcaPrinterAgent(""), m_info{std::move(id), "Integration Test Agent", "1.0", "test agent"}
    {
    }

    AgentInfo get_agent_info() override { return m_info; }

private:
    AgentInfo m_info;
};

struct ScopedAppConfig
{
    AppConfig config;
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

TEST_CASE("Orca capability reply populates connector-scope features and commands", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("orca"));
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine orca_machine;
    orca_machine.dev_id       = "orca-device";
    orca_machine.dev_name     = "Orca device";
    orca_machine.dev_ip       = "192.0.2.21";
    orca_machine.printer_type = "C11";
    MachineObject* orca_obj = manager.insert_local_device(orca_machine, "lan", "free", "", "access-code");
    REQUIRE(orca_obj != nullptr);
    orca_obj->printer_agent_id = "orca";

    orca_obj->parse_new_info2(json::parse(R"({
        "command": "get_capabilities",
        "supported_features": {"fms": true, "filament_slots": true, "filament_mapping": true},
        "supported_commands": ["print.push_status", "print.ams_get_rfid"],
        "capabilities": {
            "flags": {},
            "protocol": {
                "features": {"filament_mapping": true},
                "supported_commands": ["print.ams_change_filament"]
            }
        }
    })"));
    CHECK(orca_obj->is_support_fms);
    CHECK(orca_obj->is_support_filament_slots);
    CHECK(orca_obj->is_support_filament_mapping);
    CHECK(orca_obj->supported_commands.count("print.push_status") == 1);
    CHECK(orca_obj->supported_commands.count("print.ams_get_rfid") == 1);
    CHECK(orca_obj->supported_commands.count("print.ams_change_filament") == 1);

    // Absent or false reads as unsupported, and a later reply without commands clears the set.
    orca_obj->parse_new_info2(json::parse(R"({
        "command": "get_capabilities",
        "supported_features": {"fms": false, "filament_slots": false, "filament_mapping": false},
        "capabilities": {"flags": {}}
    })"));
    CHECK_FALSE(orca_obj->is_support_fms);
    CHECK_FALSE(orca_obj->is_support_filament_slots);
    CHECK_FALSE(orca_obj->is_support_filament_mapping);
    CHECK(orca_obj->supported_commands.empty());

    // A reply without capabilities.flags must still parse the Orca features and
    // commands (fail-closed: don't retain stale "supported" values).
    orca_obj->parse_new_info2(json::parse(R"({
        "command": "get_capabilities",
        "supported_features": {"filament_mapping": true},
        "capabilities": {
            "protocol": {"supported_commands": ["print.ams_get_rfid"]}
        }
    })"));
    CHECK(orca_obj->is_support_filament_mapping);
    CHECK_FALSE(orca_obj->is_support_filament_slots);
    CHECK(orca_obj->supported_commands.count("print.ams_get_rfid") == 1);

    // A non-Orca agent id leaves the connector-scope fields untouched.
    BBLocalMachine bbl_machine;
    bbl_machine.dev_id       = "bbl-device";
    bbl_machine.dev_name     = "Bambu device";
    bbl_machine.dev_ip       = "192.0.2.22";
    bbl_machine.printer_type = "C11";
    MachineObject* bbl_obj = manager.insert_local_device(bbl_machine, "lan", "free", "", "access-code");
    REQUIRE(bbl_obj != nullptr);
    bbl_obj->printer_agent_id = "bbl";

    bbl_obj->parse_new_info2(json::parse(R"({
        "command": "get_capabilities",
        "supported_features": {"fms": true, "filament_slots": true, "filament_mapping": true},
        "supported_commands": ["print.push_status"],
        "capabilities": {
            "flags": {},
            "protocol": {
                "features": {"fms": true, "filament_slots": true, "filament_mapping": true},
                "supported_commands": ["print.ams_get_rfid"]
            }
        }
    })"));
    CHECK_FALSE(bbl_obj->is_support_fms);
    CHECK_FALSE(bbl_obj->is_support_filament_slots);
    CHECK_FALSE(bbl_obj->is_support_filament_mapping);
    CHECK(bbl_obj->supported_commands.empty());
}

TEST_CASE("Orca per-command AMS gate requires fms and the advertised command", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("orca"));
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "orca-gate";
    machine.dev_name     = "Orca gate";
    machine.dev_ip       = "192.0.2.30";
    machine.printer_type = "C11";
    MachineObject* obj = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(obj != nullptr);
    obj->printer_agent_id = "orca";

    // fms off: no macro-backed AMS command is allowed even if listed.
    obj->is_support_fms = false;
    obj->supported_commands.insert("print.ams_control");
    CHECK_FALSE(obj->orca_ams_command_supported("print.ams_control"));

    // fms on: only the commands actually advertised are allowed.
    obj->is_support_fms = true;
    CHECK(obj->orca_ams_command_supported("print.ams_control"));
    CHECK_FALSE(obj->orca_ams_command_supported("print.ams_get_rfid"));

    // Bambu keeps the legacy permissive path.
    obj->printer_agent_id = "bbl";
    CHECK(obj->orca_ams_command_supported("print.anything"));
}
