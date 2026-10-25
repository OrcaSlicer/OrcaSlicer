#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <libslic3r/AppConfig.hpp>
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

    int send_message(std::string dev_id, std::string json_str, int, int) override
    {
        published_device = std::move(dev_id);
        published_message = json::parse(json_str);
        return 0;
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        return send_message(std::move(dev_id), std::move(json_str), qos, flag);
    }

    std::string published_device;
    json published_message;

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
    const std::vector<std::string> colors{"FF000080", "0000FF40", "00FF00FF"};

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
