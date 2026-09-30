#include <catch2/catch_all.hpp>

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/DeviceCore/DevFilaSystem.h>
#include <slic3r/GUI/FilamentMappingUtils.hpp>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <cstdint>
#include <string>
#include <utility>

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

    using OrcaPrinterAgent::deliver_to_sink;

    int send_message(std::string, std::string json_str, int, int) override
    {
        last_message = std::move(json_str);
        return send_result;
    }

    int send_message_to_printer(std::string, std::string json_str, int, int) override
    {
        last_message = std::move(json_str);
        return send_result;
    }

    std::string last_message;
    int send_result = 0;

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
    printer_agent->send_result = -1;

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
