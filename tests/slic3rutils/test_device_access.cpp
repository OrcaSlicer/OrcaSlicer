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
