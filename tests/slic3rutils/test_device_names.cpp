// DeviceManager.hpp needs the same Windows and wx include order as the GUI
// precompiled header; see test_dev_mapping.cpp.
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

using namespace Slic3r;

TEST_CASE("LAN printer aliases survive reported name changes", "[DeviceNames]")
{
    MachineObject printer(nullptr, nullptr, "Discovered Printer", "test-printer", "127.0.0.1");
    printer.dev_connection_type = "lan";
    printer.set_local_name("Workshop Printer");

    for (const std::string reported_name : {"Updated Discovery Name", "Another Discovery Name"}) {
        printer.set_dev_name(reported_name);
        CHECK(printer.get_dev_name() == "Workshop Printer");
        CHECK(printer.get_local_name() == "Workshop Printer");
        CHECK(printer.get_reported_name() == reported_name);
    }
}

TEST_CASE("LAN printers without aliases follow reported names", "[DeviceNames]")
{
    MachineObject printer(nullptr, nullptr, "Discovered Printer", "test-printer", "127.0.0.1");
    printer.dev_connection_type = "lan";
    CHECK(printer.get_local_name().empty());

    printer.set_dev_name("Updated Discovery Name");
    CHECK(printer.get_dev_name() == "Updated Discovery Name");

    printer.set_local_name("Workshop Printer");
    printer.set_local_name("");
    CHECK(printer.get_dev_name() == "Updated Discovery Name");
    CHECK(printer.get_local_name().empty());
}

TEST_CASE("Cloud printer names stay independent of LAN aliases", "[DeviceNames]")
{
    MachineObject printer(nullptr, nullptr, "Cloud Printer", "test-printer", "127.0.0.1");
    printer.dev_connection_type = "cloud";
    printer.set_local_name("Workshop Printer");
    printer.set_dev_name("Renamed Cloud Printer");
    CHECK(printer.get_dev_name() == "Renamed Cloud Printer");
    CHECK(printer.get_reported_name() == "Renamed Cloud Printer");

    printer.dev_connection_type = "lan";
    CHECK(printer.get_dev_name() == "Workshop Printer");

    printer.dev_connection_type = "cloud";
    CHECK(printer.get_dev_name() == "Renamed Cloud Printer");
}
