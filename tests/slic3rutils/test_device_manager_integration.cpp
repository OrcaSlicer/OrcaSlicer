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
#include <slic3r/GUI/DeviceCore/DevChamber.h>
#include <slic3r/GUI/DeviceCore/DevCtrl.h>
#include <slic3r/GUI/DeviceCore/DevDefs.h>
#include <slic3r/GUI/DeviceCore/DevLamp.h>
#include <slic3r/GUI/DeviceCore/DevPrintOptions.h>
#include <slic3r/GUI/DeviceCore/DevUpgrade.h>
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
#include <functional>
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

    int send_message(std::string dev_id, std::string json_str, int qos, int) override
    {
        sent.emplace_back(qos, json_str);
        last_message = json_str;
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

    std::string last_message;
    std::vector<std::pair<int, std::string>> sent; // (qos, message) of every send, in order
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

namespace {

// Every message `printer` handed its agent, one per line as "qos <message>", with each sequence id checked
// to be a decimal string and then masked so the text compares across runs. Re-serialising keeps the bytes:
// nlohmann::json writes keys sorted and numbers in shortest form.
std::string published_command(const ConnectedPrinter& p)
{
    std::string text;
    for (const auto& [qos, sent] : p.agent->sent) {
        json message = json::parse(sent);
        for (auto& [ns, body] : message.items())
            if (body.is_object() && body.contains("sequence_id")) {
                const json&       id     = body["sequence_id"];
                const std::string digits = id.is_string() ? id.get<std::string>() : std::string();
                CHECK(id.is_string());
                CHECK_FALSE(digits.empty());
                CHECK(digits.find_first_not_of("0123456789") == std::string::npos);
                body["sequence_id"] = "#";
            }
        text += (text.empty() ? "" : "\n") + std::to_string(qos) + " " + message.dump();
    }
    return text;
}

struct PublishedCommand
{
    const char*                         name;
    std::function<int(MachineObject&)> send;
    const char*                         expected;
};

} // namespace

// Pins the exact JSON each device-layer command sends, so moving the builders cannot change a byte.
TEST_CASE("Each device-layer command publishes its established JSON", "[DeviceManager][Commands]")
{
    using LIGHT = DevLamp::LIGHT_EFFECT;
    const std::vector<PublishedCommand> commands = {
        {"get_version", [](MachineObject& m) { return m.command_get_version(false); }, R"(1 {"info":{"command":"get_version","sequence_id":"#"}})"},
        {"get_access_code", [](MachineObject& m) { return m.command_get_access_code(); }, R"(0 {"system":{"command":"get_access_code","sequence_id":"#"}})"},
        {"pushall", [](MachineObject& m) { return m.command_request_push_all(true); }, R"(0 {"pushing":{"command":"pushall","push_target":1,"sequence_id":"#","version":1}})"},
        {"pushing start", [](MachineObject& m) { return m.command_pushing("start"); }, R"(0 {"pushing":{"command":"start","sequence_id":"#"}})"},
        {"clean_print_error", [](MachineObject& m) { return m.command_clean_print_error("subtask-7", 50348044); }, R"(0 {"print":{"command":"clean_print_error","print_error":50348044,"sequence_id":"#","subtask_id":"subtask-7"}})"},
        {"uiop", [](MachineObject& m) { return m.command_clean_print_error_uiop(50348044); }, R"(0 {"system":{"action":"close","command":"uiop","err":"0300400C","name":"print_error","sequence_id":"#","source":1,"type":"dialog"}})"},
        {"upgrade_confirm", [](MachineObject& m) { return m.command_upgrade_confirm(); }, R"(0 {"upgrade":{"command":"upgrade_confirm","sequence_id":"#","src_id":1}})"},
        {"consistency_confirm", [](MachineObject& m) { return m.command_consistency_upgrade_confirm(); }, R"(0 {"upgrade":{"command":"consistency_confirm","sequence_id":"#","src_id":1}})"},
        {"upgrade module", [](MachineObject& m) { return m.command_upgrade_module("https://example.com/f.bin", "ams", "01.02.03"); }, R"(0 {"upgrade":{"command":"start","module":"ams","sequence_id":"#","src_id":1,"url":"https://example.com/f.bin","version":"01.02.03"}})"},
        {"skip_objects", [](MachineObject& m) { return m.command_task_partskip({3, 9}); }, R"(1 {"print":{"command":"skip_objects","obj_list":[3,9],"sequence_id":"#"}})"},
        {"stop", [](MachineObject& m) { return m.command_task_abort(); }, R"(1 {"print":{"command":"stop","param":"","sequence_id":"#"}})"},
        {"stop job", [](MachineObject& m) { return m.command_task_cancel("job-42"); }, R"(1 {"print":{"command":"stop","job_id":"job-42","param":"","sequence_id":"#"}})"},
        {"pause", [](MachineObject& m) { return m.command_task_pause(); }, R"(1 {"print":{"command":"pause","param":"","sequence_id":"#"}})"},
        {"resume", [](MachineObject& m) { return m.command_task_resume(); }, R"(1 {"print":{"command":"resume","param":"","sequence_id":"#"}})"},
        {"idle_ignore", [](MachineObject& m) { return m.command_hms_idle_ignore("0300-1A00-0001", 2); }, R"(1 {"print":{"command":"idle_ignore","err":"0300-1A00-0001","sequence_id":"#","type":2}})"},
        {"hms resume", [](MachineObject& m) { return m.command_hms_resume("0300-1A00-0001", "job-42"); }, R"(1 {"print":{"command":"resume","err":"0300-1A00-0001","job_id":"job-42","param":"reserve","sequence_id":"#"}})"},
        {"hms ignore", [](MachineObject& m) { return m.command_hms_ignore("0300-1A00-0001", "job-42"); }, R"(1 {"print":{"command":"ignore","err":"0300-1A00-0001","job_id":"job-42","param":"reserve","sequence_id":"#"}})"},
        {"hms stop", [](MachineObject& m) { return m.command_hms_stop("0300-1A00-0001", "job-42"); }, R"(1 {"print":{"command":"stop","err":"0300-1A00-0001","job_id":"job-42","param":"reserve","sequence_id":"#"}})"},
        {"buzzer_ctrl", [](MachineObject& m) { return m.command_stop_buzzer(); }, R"(1 {"print":{"command":"buzzer_ctrl","mode":0,"sequence_id":"#"}})"},
        {"set_nozzle_temp", [](MachineObject& m) { return m.command_set_nozzle_new(1, 215); }, R"(1 {"print":{"command":"set_nozzle_temp","extruder_index":1,"sequence_id":"#","target_temp":215}})"},
        {"refresh_nozzle", [](MachineObject& m) { return m.command_refresh_nozzle(); }, R"(1 {"print":{"command":"refresh_nozzle","sequence_id":"#"}})"},
        {"set_ctt", [](MachineObject& m) { return m.command_set_chamber(45); }, R"(1 {"print":{"command":"set_ctt","ctt_val":45,"sequence_id":"#"}})"},
        {"ams_change_filament load", [](MachineObject& m) { return m.command_ams_change_filament(true, "1", "2", 210, 230); }, R"(0 {"print":{"ams_id":1,"command":"ams_change_filament","curr_temp":210,"sequence_id":"#","slot_id":2,"tar_temp":230,"target":6}})"},
        {"ams_change_filament unload", [](MachineObject& m) { return m.command_ams_change_filament(false, "0", "0", 220, 220, 1); }, R"(0 {"print":{"ams_id":0,"command":"ams_change_filament","curr_temp":220,"extruder_id":1,"sequence_id":"#","slot_id":255,"tar_temp":220,"target":255}})"},
        {"ams_user_setting", [](MachineObject& m) { return m.command_ams_user_settings(true, false, true); }, R"(0 {"print":{"ams_id":-1,"calibrate_remain_flag":true,"command":"ams_user_setting","sequence_id":"#","startup_read_option":true,"tray_read_option":false}})"},
        {"ams_filament_setting", [](MachineObject& m) {
             return m.command_ams_filament_settings(0, 3, "GFA00", "GFSA00", "FF0000FF", "PLA", 190, 230, {"FF0000FF", "00FF00FF"}, 1);
         }, R"(0 {"print":{"ams_id":0,"cols":["FF0000FF","00FF00FF"],"command":"ams_filament_setting","ctype":1,"nozzle_temp_max":230,"nozzle_temp_min":190,"sequence_id":"#","setting_id":"GFSA00","slot_id":3,"tray_color":"FF0000FF","tray_id":3,"tray_info_idx":"GFA00","tray_type":"PLA"}})"},
        {"ams_control", [](MachineObject& m) { return m.command_ams_control("pause"); }, R"(0 {"print":{"command":"ams_control","param":"pause","sequence_id":"#"}})"},
        {"auto_stop_ams_dry", [](MachineObject& m) { return m.command_ams_drying_stop(); }, R"(0 {"print":{"command":"auto_stop_ams_dry","sequence_id":"#"}})"},
        {"extrusion_cali", [](MachineObject& m) { return m.command_start_extrusion_cali(2, 220, 60, 21.5f, "GFSA00"); }, R"(0 {"print":{"bed_temp":60,"command":"extrusion_cali","max_volumetric_speed":21.5,"nozzle_temp":220,"sequence_id":"#","tray_id":2}})"},
        {"extrusion_cali_set", [](MachineObject& m) { return m.command_extrusion_cali_set(2, "GFSA00", "PLA", 0.02f, 1.4f, 60, 220, 21.5f); }, R"(0 {"print":{"bed_temp":60,"command":"extrusion_cali_set","k_value":0.019999999552965164,"max_volumetric_speed":21.5,"n_coef":1.399999976158142,"nozzle_temp":220,"sequence_id":"#","tray_id":2}})"},
        {"print_speed", [](MachineObject& m) { return m.command_set_printing_speed(SPEED_LEVEL_SILENCE); }, R"(0 {"print":{"command":"print_speed","param":"1","sequence_id":"#"}})"},
        {"print_option auto_recovery", [](MachineObject& m) { return m.command_set_printing_option(true); }, R"(0 {"print":{"auto_recovery":true,"command":"print_option","option":1,"sequence_id":"#"}})"},
        {"print_option nozzle_blob_detect", [](MachineObject& m) { return m.command_nozzle_blob_detect(true); }, R"(0 {"print":{"command":"print_option","nozzle_blob_detect":true,"sequence_id":"#"}})"},
        {"print_option sound_enable", [](MachineObject& m) { return m.command_set_prompt_sound(false); }, R"(0 {"print":{"command":"print_option","sequence_id":"#","sound_enable":false}})"},
        {"print_option filament_tangle_detect", [](MachineObject& m) { return m.command_set_filament_tangle_detect(true); }, R"(0 {"print":{"command":"print_option","filament_tangle_detect":true,"sequence_id":"#"}})"},
        {"print_option auto_switch_filament", [](MachineObject& m) { return m.command_ams_switch_filament(true); }, R"(0 {"print":{"auto_switch_filament":true,"command":"print_option","sequence_id":"#"}})"},
        {"print_option air_print_detect", [](MachineObject& m) { return m.command_ams_air_print_detect(false); }, R"(0 {"print":{"air_print_detect":false,"command":"print_option","sequence_id":"#"}})"},
        {"set_extrusion_length", [](MachineObject& m) { return m.command_extruder_control(0, 10.7); }, R"(0 {"print":{"command":"set_extrusion_length","extruder_index":0,"length":10,"sequence_id":"#"}})"},
        {"get_pa_calibration_result", [](MachineObject& m) { return m.command_get_pa_calibration_result(0.4f); }, R"(0 {"print":{"command":"extrusion_cali_get_result","nozzle_diameter":"0.4","sequence_id":"#"}})"},
        {"flowrate_get_result", [](MachineObject& m) { return m.command_get_flow_ratio_calibration_result(0.4f); }, R"(0 {"print":{"command":"flowrate_get_result","nozzle_diameter":"0.4","sequence_id":"#"}})"},
        {"ipcam_record_set", [](MachineObject& m) { return m.command_ipcam_record(true); }, R"(0 {"camera":{"command":"ipcam_record_set","control":"enable","sequence_id":"#"}})"},
        {"ipcam_timelapse", [](MachineObject& m) { return m.command_ipcam_timelapse(false); }, R"(0 {"camera":{"command":"ipcam_timelapse","control":"disable","sequence_id":"#"}})"},
        {"ipcam_resolution_set", [](MachineObject& m) { return m.command_ipcam_resolution_set("1080p"); }, R"(0 {"camera":{"command":"ipcam_resolution_set","resolution":"1080p","sequence_id":"#"}})"},
        {"ipcam_get_media_info", [](MachineObject& m) { return m.command_ipcam_check_timelapse_storage("emmc", 120); }, R"(0 {"camera":{"command":"ipcam_get_media_info","sequence_id":"#","storage":"emmc","sub_command":"is_timelapse_storage_enough","total_layer":120}})"},
        {"ipcam_delete_oldest_timelapse", [](MachineObject& m) { return m.command_ipcam_delete_oldest_timelapse("sdcard", 120); }, R"(0 {"camera":{"command":"ipcam_delete_oldest_timelapse","sequence_id":"#","storage":"sdcard","total_layer":120}})"},
        {"xcam_control_set", [](MachineObject& m) { return m.command_xcam_control("first_layer_inspector", true, "medium"); }, R"(0 {"xcam":{"command":"xcam_control_set","control":true,"enable":true,"halt_print_sensitivity":"medium","module_name":"first_layer_inspector","print_halt":true,"sequence_id":"#"}})"},
        {"xcam spaghetti", [](MachineObject& m) { return m.command_xcam_control_spaghetti_detection(true, "never_halt"); }, R"(0 {"xcam":{"command":"xcam_control_set","control":true,"enable":true,"halt_print_sensitivity":"never_halt","module_name":"spaghetti_detector","print_halt":true,"sequence_id":"#"}})"},
        {"close_air_filt", [](MachineObject& m) { return m.command_purification_disable(); }, R"(1 {"print":{"command":"close_air_filt","sequence_id":"#"}})"},
        {"set_door_stat", [](MachineObject& m) { m.command_set_door_open_check(MachineObject::DOOR_OPEN_CHECK_ENABLE_WARNING); return 0; }, R"(0 {"system":{"command":"set_door_stat","config":1,"sequence_id":"#"}})"},
        {"print_cache_set", [](MachineObject& m) { m.command_set_save_remote_print_file_to_storage(!m.get_save_remote_print_file_to_storage()); return 0; }, R"(0 {"system":{"command":"print_cache_set","config":true,"sequence_id":"#"}})"},
        {"ledctrl", [](MachineObject& m) { m.GetLamp()->CtrlSetChamberLight(LIGHT::LIGHT_EFFECT_ON); return 0; }, R"(0 {"system":{"command":"ledctrl","interval_time":1000,"led_mode":"on","led_node":"chamber_light","led_off_time":500,"led_on_time":500,"loop_times":1,"sequence_id":"#"}})" "\n" R"(0 {"system":{"command":"ledctrl","interval_time":1000,"led_mode":"on","led_node":"chamber_light2","led_off_time":500,"led_on_time":500,"loop_times":1,"sequence_id":"#"}})"},
        {"select_extruder", [](MachineObject& m) { return m.GetCtrl()->command_select_extruder(1); }, R"(1 {"print":{"command":"select_extruder","extruder_index":1,"sequence_id":"#"}})"},
        {"chamber set_ctt", [](MachineObject& m) { return m.GetChamber()->CtrlSetChamberTemp(40); }, R"(1 {"print":{"command":"set_ctt","ctt_val":40,"sequence_id":"#"}})"},
        {"upgrade start", [](MachineObject& m) { return m.GetUpgrade().lock()->CtrlUpgradeModule("https://example.com/f.bin", "ota", "01.09.00"); }, R"(0 {"upgrade":{"command":"start","module":"ota","sequence_id":"#","src_id":1,"url":"https://example.com/f.bin","version":"01.09.00"}})"},
        {"ams_reset", [](MachineObject& m) { return m.GetFilaSystem()->CtrlAmsReset(); }, R"(0 {"print":{"command":"ams_reset","sequence_id":"#"}})"},
        {"ams_filament_drying start", [](MachineObject& m) { return m.GetFilaSystem()->CtrlAmsStartDryingHour(1, "PETG", 65, 8, true, 40, false); }, R"(0 {"print":{"ams_id":1,"close_power_conflict":false,"command":"ams_filament_drying","cooling_temp":40,"duration":8,"filament":"PETG","humidity":0,"mode":1,"rotate_tray":true,"sequence_id":"#","temp":65}})"},
        {"ams_filament_drying stop", [](MachineObject& m) { return m.GetFilaSystem()->CtrlAmsStopDrying(1); }, R"(0 {"print":{"ams_id":1,"close_power_conflict":false,"command":"ams_filament_drying","cooling_temp":0,"duration":0,"filament":"","humidity":0,"mode":0,"rotate_tray":false,"sequence_id":"#","temp":0}})"},
        {"heating protect", [](MachineObject& m) { return m.GetPrintOptions()->command_set_against_continued_heating_mode(true); }, R"(0 {"print":{"command":"set_against_continued_heating_mode","enable":true,"sequence_id":"#"}})"},
        {"options print_option", [](MachineObject& m) { return m.GetPrintOptions()->command_set_printing_option(true, &m); }, R"(0 {"print":{"auto_recovery":true,"command":"print_option","option":1,"sequence_id":"#"}})"},
        {"options air_purification", [](MachineObject& m) { return m.GetPrintOptions()->command_xcam_control_purify_air_at_print_end(2); }, R"(0 {"print":{"air_purification":2,"command":"print_option","sequence_id":"#"}})"},
        {"options ipcam_cap_pic_set", [](MachineObject& m) { return m.GetPrintOptions()->command_snapshot_control(1); }, R"(0 {"camera":{"command":"ipcam_cap_pic_set","control":"enable","sequence_id":"#"}})"},
        {"options nozzle_blob_detect_v2", [](MachineObject& m) { return m.GetPrintOptions()->command_smart_nozzle_blob_detect_mode(2); }, R"(0 {"print":{"command":"print_option","nozzle_blob_detect_v2":2,"sequence_id":"#"}})"},
        {"options xcam", [](MachineObject& m) { return m.GetPrintOptions()->command_xcam_control_fod_check(true); }, R"(0 {"xcam":{"command":"xcam_control_set","control":true,"enable":true,"module_name":"fod_check","print_halt":true,"sequence_id":"#"}})"},
    };

    for (const PublishedCommand& command : commands) {
        DYNAMIC_SECTION(command.name)
        {
            ConnectedPrinter p("IDLE");
            REQUIRE(command.send(p.printer) == 0);
            REQUIRE_FALSE(p.agent->last_message.empty());
            CHECK(published_command(p) == command.expected);
        }
    }
}
