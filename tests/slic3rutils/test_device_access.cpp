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

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

using namespace Slic3r;

TEST_CASE("Moonraker permits an empty access code", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    machine.set_access_code("", false);

    REQUIRE(machine.has_access_right());
}

TEST_CASE("Moonraker logout revokes access until explicitly rebound", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    const std::string code = GENERATE("", "configured-key");
    machine.set_access_code(code, false);
    REQUIRE(machine.has_access_right());

    machine.revoke_access();
    REQUIRE(machine.get_access_code().empty());
    REQUIRE_FALSE(machine.has_access_right());

    machine.set_access_code(code, false);
    REQUIRE(machine.has_access_right());
    REQUIRE(machine.get_access_code() == code);
}

TEST_CASE("Revoked access prevents calls to the printer agent", "[DeviceAccess]")
{
    class RecordingAgent : public MoonrakerPrinterAgent
    {
    public:
        RecordingAgent() : MoonrakerPrinterAgent("") {}
        int connections = 0;
        int connect_printer(const PrinterConnectionParams&) override
        {
            ++connections;
            return 0;
        }
        int disconnect_printer() override { return 0; }
    };

    auto agent = std::make_shared<RecordingAgent>();
    NetworkAgent network(nullptr, agent);
    MachineObject machine(nullptr, &network, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = GENERATE(BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, MOONRAKER_PRINTER_AGENT_ID);
    machine.set_access_code("configured-key", false);
    REQUIRE(machine.connect() == 0);
    REQUIRE(agent->connections == 1);

    machine.revoke_access();
    REQUIRE(machine.connect() == -1);
    REQUIRE(agent->connections == 1);

    machine.set_access_code("configured-key", false);
    REQUIRE(machine.connect() == 0);
    REQUIRE(agent->connections == 2);
}

TEST_CASE("Other printer agents require an access code", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = GENERATE(
        BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, "qidi", "snapmaker", "crealityprint", "plugin", "unknown");

    machine.set_access_code("", false);
    REQUIRE_FALSE(machine.has_access_right());

    machine.set_access_code("88888888", false);
    REQUIRE(machine.has_access_right());
}

TEST_CASE("Rebinding Moonraker with an empty key saves the machine", "[DeviceAccess]")
{
    AppConfig config;
    DeviceManager manager(nullptr, false, &config);
    MachineObject machine(&manager, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id    = MOONRAKER_PRINTER_AGENT_ID;
    machine.dev_connection_type = "lan";
    machine.revoke_access();

    machine.set_access_code("");
    const auto& machines = config.get_local_machines();
    REQUIRE(machines.count("test_dev") == 1);
    REQUIRE(machines.at("test_dev").printer_agent_id == MOONRAKER_PRINTER_AGENT_ID);
    REQUIRE(machines.at("test_dev").access_code.empty());
}

TEST_CASE("Moonraker discovery leaves another agent's saved machine alone", "[DeviceAccess]")
{
    AppConfig config;
    NetworkAgent network(nullptr, std::make_shared<MoonrakerPrinterAgent>(""));
    DeviceManager manager(&network, false, &config);

    const std::string owner = GENERATE(as<std::string>{}, ORCA_PRINTER_AGENT_ID, MOONRAKER_PRINTER_AGENT_ID);
    BBLocalMachine saved;
    saved.dev_id           = "192.0.2.10:7125";
    saved.dev_ip           = saved.dev_id;
    saved.printer_agent_id = owner;
    config.update_local_machine(saved);

    manager.on_machine_alive(R"({"dev_name":"Klipper","dev_id":"192.0.2.10:7125","dev_ip":"192.0.2.10:7125",
        "dev_type":"","dev_signal":"0","connect_type":"lan","bind_state":"free"})");
    REQUIRE((manager.get_local_machine(saved.dev_id) != nullptr) == (owner == MOONRAKER_PRINTER_AGENT_ID));
}
