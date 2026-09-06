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

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

using namespace Slic3r;

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
    machine.printer_agent_id = GENERATE(
        BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, "qidi", "snapmaker", "crealityprint", "plugin", "unknown");

    machine.set_access_code("", false);
    REQUIRE_FALSE(machine.has_access_right());

    machine.set_access_code("88888888", false);
    REQUIRE(machine.has_access_right());
}

TEST_CASE("Saved local credentials cannot cross printer agents", "[DeviceAccess]")
{
    AppConfig config;

    BBLocalMachine saved;
    saved.dev_id           = "test_dev";
    saved.printer_agent_id = "qidi";
    saved.access_code      = "configured-key";
    config.update_local_machine(saved);

    BBLocalMachine moonraker = saved;
    moonraker.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    moonraker.access_code      = "";
    config.update_local_machine(moonraker);
    REQUIRE(config.get_local_machines().at(saved.dev_id) == saved);

    saved.printer_agent_id = "";
    AppConfig legacy_config;
    legacy_config.update_local_machine(saved);
    legacy_config.update_local_machine(moonraker);
    REQUIRE(legacy_config.get_local_machines().at(saved.dev_id) == saved);

    BBLocalMachine bbl = saved;
    bbl.printer_agent_id = BBL_PRINTER_AGENT_ID;
    legacy_config.update_local_machine(bbl);
    REQUIRE(legacy_config.get_local_machines().at(saved.dev_id) == bbl);

    for (const char* section : {"access_code", "user_access_code"}) {
        AppConfig flat_legacy_config;
        saved.access_code = "";
        flat_legacy_config.update_local_machine(saved);
        flat_legacy_config.set(section, saved.dev_id, "flat-legacy-key");

        flat_legacy_config.update_local_machine(moonraker);
        REQUIRE(flat_legacy_config.get_local_machines().at(saved.dev_id) == saved);

        flat_legacy_config.update_local_machine(bbl);
        REQUIRE(flat_legacy_config.get_local_machines().at(saved.dev_id) == bbl);
    }
}
