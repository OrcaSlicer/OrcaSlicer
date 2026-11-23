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

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/Mcp/McpTools.hpp>
#include <slic3r/GUI/Operations/Outcome.hpp>
#include <slic3r/GUI/Operations/PrinterOps.hpp>
#include <libslic3r/AppConfig.hpp>
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <cstdint>
#include <optional>
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
        return reply;
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        return send_message(std::move(dev_id), std::move(json_str), qos, flag);
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

    std::string published_device;
    json published_message;
    std::string connected_ca_file;
    int reply = 0; // what a send returns: 0 when the printer took the message
    int selections{0};

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
