#include <catch2/catch_all.hpp>

#include "slic3r/Utils/Moonraker.hpp"

using Slic3r::MoonrakerStorage::is_gcode_destination_root;

// Orca: pins Moonraker::get_storage's root-filtering rule -- see is_gcode_destination_root's doc
// for the footgun this guards (PrintHostSendDialog defaulting to storage_names.front(), which
// used to resolve to "config" on a real Snapmaker U1 because the old writable-only filter let
// every writable root through in server-reported order).

TEST_CASE("The canonical gcodes root is a print-job destination", "[MoonrakerStorage]")
{
    CHECK(is_gcode_destination_root("gcodes"));
}

TEST_CASE("A case-insensitive gcode-named variant is a print-job destination", "[MoonrakerStorage]")
{
    CHECK(is_gcode_destination_root("GCodes"));
    CHECK(is_gcode_destination_root("usb_gcodes"));
}

TEST_CASE("Non-print roots are rejected", "[MoonrakerStorage]")
{
    CHECK_FALSE(is_gcode_destination_root("config"));
    CHECK_FALSE(is_gcode_destination_root("logs"));
    CHECK_FALSE(is_gcode_destination_root("timelapse"));
    CHECK_FALSE(is_gcode_destination_root("camera"));
    CHECK_FALSE(is_gcode_destination_root(""));
}
