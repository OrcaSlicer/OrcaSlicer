#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

TEST_CASE("AppConfig network version helpers", "[AppConfig]") {
    AppConfig config;

    SECTION("skipped versions starts empty") {
        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.empty());
    }

    SECTION("add and check skipped version") {
        config.add_skipped_network_version("02.01.01.52");
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE_FALSE(config.is_network_version_skipped("02.03.00.62"));
    }

    SECTION("multiple skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.00.02.50");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 2);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE(config.is_network_version_skipped("02.00.02.50"));
    }

    SECTION("clear skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.clear_skipped_network_versions();
        REQUIRE_FALSE(config.is_network_version_skipped("02.01.01.52"));
    }

    SECTION("duplicate add is idempotent") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.01.01.52");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 1);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
    }
}

// The GUI-layer FilamentInventoryStore (src/slic3r/GUI/FilamentInventoryStore.hpp) is a thin
// wrapper over get/set_printer_setting(printer, "loaded_filaments", ...); this exercises the
// AppConfig side of that contract directly, since AppConfig has no GUI dependency and is
// constructible in this suite.
TEST_CASE("Per-printer loaded_filaments setting round-trips through AppConfig", "[AppConfig]") {
    AppConfig config;

    SECTION("absent for a printer that was never set") {
        REQUIRE_FALSE(config.has_printer_setting("Test Printer", "loaded_filaments"));
        REQUIRE(config.get_printer_setting("Test Printer", "loaded_filaments").empty());
    }

    SECTION("set/get round-trip") {
        config.set_printer_setting("Test Printer", "loaded_filaments", "#FF0000;PLA|#00FF00;PETG");
        REQUIRE(config.has_printer_setting("Test Printer", "loaded_filaments"));
        REQUIRE(config.get_printer_setting("Test Printer", "loaded_filaments") == "#FF0000;PLA|#00FF00;PETG");
    }

    SECTION("distinct printers keep independent values") {
        config.set_printer_setting("Printer A", "loaded_filaments", "value-a");
        config.set_printer_setting("Printer B", "loaded_filaments", "value-b");
        REQUIRE(config.get_printer_setting("Printer A", "loaded_filaments") == "value-a");
        REQUIRE(config.get_printer_setting("Printer B", "loaded_filaments") == "value-b");
    }

    SECTION("clearing the printer's settings drops the key") {
        config.set_printer_setting("Test Printer", "loaded_filaments", "value");
        config.clear_printer_settings("Test Printer");
        REQUIRE_FALSE(config.has_printer_setting("Test Printer", "loaded_filaments"));
    }
}
