#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Thread.hpp"
#include "../slic3rutils/plugin_test_utils.hpp"

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

TEST_CASE("Local machine agent replacements survive save and reload", "[AppConfig]")
{
    ScopedDataDir data_dir("local-machine");
    save_main_thread_id();
    AppConfig config;
    BBLocalMachine saved;
    saved.dev_id           = "127.0.0.1:7125";
    saved.dev_ip           = saved.dev_id;
    saved.dev_name         = "Test printer";
    saved.printer_type     = "test-model";
    saved.printer_agent_id = "orca";
    saved.access_code      = "old-key";
    config.update_local_machine(saved);
    config.save();
    REQUIRE_FALSE(config.dirty());

    BBLocalMachine replacement = saved;
    replacement.printer_agent_id = "moonraker";
    replacement.access_code = GENERATE("", "new-key", "88888888");
    config.update_local_machine(replacement);
    REQUIRE(config.dirty());
    REQUIRE(config.get_local_machines().at(saved.dev_id) == replacement);
    config.save();

    AppConfig reloaded;
    REQUIRE(reloaded.load().empty());
    REQUIRE(reloaded.get_local_machines().at(saved.dev_id) == replacement);
    REQUIRE_FALSE(reloaded.dirty());
    reloaded.update_local_machine(replacement);
    REQUIRE_FALSE(reloaded.dirty());
}

TEST_CASE("Clearing local credentials preserves another agent's record", "[AppConfig]")
{
    AppConfig config;
    BBLocalMachine saved;
    saved.dev_id           = "test_dev";
    saved.printer_agent_id = GENERATE("orca", "qidi", "bbl", "");
    saved.access_code      = "saved-key";
    config.update_local_machine(saved);
    config.set_str("access_code", saved.dev_id, "legacy-key");
    config.set_str("user_access_code", saved.dev_id, "legacy-user-key");

    config.clear_local_machine_access_code(saved.dev_id, "moonraker");

    REQUIRE(config.get_local_machines().at(saved.dev_id) == saved);
    REQUIRE(config.get("access_code", saved.dev_id) == "legacy-key");
    REQUIRE(config.get("user_access_code", saved.dev_id) == "legacy-user-key");
}

TEST_CASE("Clearing local credentials changes only the owning agent's key", "[AppConfig]")
{
    ScopedDataDir data_dir("local-machine-clear");
    save_main_thread_id();
    AppConfig config;
    BBLocalMachine saved;
    saved.dev_id           = "test_dev";
    saved.dev_name         = "Test printer";
    saved.printer_agent_id = GENERATE("moonraker", "qidi");
    saved.access_code      = "saved-key";
    config.update_local_machine(saved);
    config.save();
    REQUIRE_FALSE(config.dirty());

    config.clear_local_machine_access_code(saved.dev_id, saved.printer_agent_id);
    REQUIRE(config.dirty());
    config.save();

    AppConfig reloaded;
    REQUIRE(reloaded.load().empty());
    saved.access_code.clear();
    REQUIRE(reloaded.get_local_machines().at(saved.dev_id) == saved);
    REQUIRE_FALSE(reloaded.dirty());
    reloaded.clear_local_machine_access_code(saved.dev_id, saved.printer_agent_id);
    reloaded.clear_local_machine_access_code("unknown_dev", saved.printer_agent_id);
    REQUIRE(reloaded.get_local_machines().size() == 1);
    REQUIRE_FALSE(reloaded.dirty());
}

TEST_CASE("BBL credential clearing removes legacy fallback without changing foreign records", "[AppConfig]")
{
    AppConfig config;
    BBLocalMachine saved;
    saved.dev_id           = "test_dev";
    saved.printer_agent_id = GENERATE("bbl", "", "moonraker");
    saved.access_code      = "saved-key";
    config.update_local_machine(saved);
    config.set_str("access_code", saved.dev_id, "legacy-key");
    config.set_str("user_access_code", saved.dev_id, "legacy-user-key");

    config.clear_local_machine_access_code(saved.dev_id, GENERATE("bbl", ""));

    if (saved.printer_agent_id != "moonraker")
        saved.access_code.clear();
    REQUIRE(config.get_local_machines().at(saved.dev_id) == saved);
    REQUIRE(config.get("access_code", saved.dev_id).empty());
    REQUIRE(config.get("user_access_code", saved.dev_id).empty());
}
