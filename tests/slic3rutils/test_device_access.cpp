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

#include <wx/timer.h>

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

using namespace Slic3r;

namespace {

void configure_printhost(DynamicPrintConfig& config,
                         const std::string& agent_id,
                         const std::string& host,
                         const std::string& port,
                         const std::string& access_code)
{
    config.set("printer_agent", agent_id, true);
    config.set("print_host", host, true);
    config.set("printhost_port", port, true);
    config.set("printhost_apikey", access_code, true);
}

} // namespace

TEST_CASE("Moonraker permits an empty access code", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    machine.set_access_code("", false);

    REQUIRE(machine.has_access_right());
}

TEST_CASE("Other printer agents require an access code", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = GENERATE(BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, "qidi", "snapmaker", "crealityprint", "plugin");

    machine.set_access_code("", false);
    REQUIRE_FALSE(machine.has_access_right());

    machine.set_access_code("88888888", false);
    REQUIRE(machine.has_access_right());
}

TEST_CASE("Matching Moonraker configuration replaces a cached access code", "[DeviceAccess]")
{
    const std::string configured_code = GENERATE(std::string(), std::string("configured"), std::string("88888888"));

    PresetBundle bundle;
    configure_printhost(bundle.printers.get_edited_preset().config,
                        MOONRAKER_PRINTER_AGENT_ID,
                        "127.0.0.1",
                        "7125",
                        configured_code);

    MachineObject machine(nullptr, nullptr, "test", "127.0.0.1:7125", "127.0.0.1:7125");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    machine.set_access_code("stale", false);

    REQUIRE(machine.reconcile_printhost_access_code(bundle, false));
    REQUIRE(machine.get_access_code() == configured_code);
}

TEST_CASE("Non-Moonraker access codes are not replaced from printer configuration", "[DeviceAccess]")
{
    const std::string agent_id = GENERATE(BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, "qidi", "snapmaker", "crealityprint", "plugin");

    PresetBundle bundle;
    configure_printhost(bundle.printers.get_edited_preset().config, agent_id, "127.0.0.1", "7125", "configured");

    MachineObject machine(nullptr, nullptr, "test", "127.0.0.1:7125", "127.0.0.1:7125");
    machine.printer_agent_id = agent_id;
    machine.set_access_code("88888888", false);

    REQUIRE_FALSE(machine.reconcile_printhost_access_code(bundle, false));
    REQUIRE(machine.get_access_code() == "88888888");
}

TEST_CASE("Unmatched Moonraker configuration does not replace a cached access code", "[DeviceAccess]")
{
    PresetBundle bundle;
    configure_printhost(bundle.printers.get_edited_preset().config,
                        MOONRAKER_PRINTER_AGENT_ID,
                        "192.0.2.1",
                        "7125",
                        "configured");

    MachineObject machine(nullptr, nullptr, "test", "127.0.0.1:7125", "127.0.0.1:7125");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    machine.set_access_code("88888888", false);

    REQUIRE_FALSE(machine.reconcile_printhost_access_code(bundle, false));
    REQUIRE(machine.get_access_code() == "88888888");
}

TEST_CASE("Conflicting Moonraker configurations do not replace a cached access code", "[DeviceAccess]")
{
    PresetBundle bundle;
    configure_printhost(bundle.printers.get_edited_preset().config,
                        MOONRAKER_PRINTER_AGENT_ID,
                        "127.0.0.1",
                        "7125",
                        "edited");

    DynamicPrintConfig physical = bundle.physical_printers.default_config();
    configure_printhost(physical, MOONRAKER_PRINTER_AGENT_ID, "127.0.0.1", "7125", "physical");
    bundle.physical_printers.load_printer("", "Moonraker", std::move(physical), false);

    MachineObject machine(nullptr, nullptr, "test", "127.0.0.1:7125", "127.0.0.1:7125");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    machine.set_access_code("88888888", false);

    REQUIRE_FALSE(machine.reconcile_printhost_access_code(bundle, false));
    REQUIRE(machine.get_access_code() == "88888888");
}
