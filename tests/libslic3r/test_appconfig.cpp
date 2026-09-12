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

TEST_CASE("AppConfig Speed Dial recent count defaults, clamps and parses", "[AppConfig]") {
    AppConfig config;

    SECTION("unset falls back to the default") {
        REQUIRE(config.get_speed_dial_recent_count() == SPEED_DIAL_RECENT_COUNT_DEFAULT);
    }

    SECTION("zero disables recents") {
        config.set(SETTING_SPEED_DIAL_RECENT_COUNT, "0");
        REQUIRE(config.get_speed_dial_recent_count() == 0);
    }

    SECTION("a value in range is returned as-is") {
        config.set(SETTING_SPEED_DIAL_RECENT_COUNT, "7");
        REQUIRE(config.get_speed_dial_recent_count() == 7);
    }

    SECTION("the maximum is kept") {
        config.set(SETTING_SPEED_DIAL_RECENT_COUNT, "10");
        REQUIRE(config.get_speed_dial_recent_count() == SPEED_DIAL_RECENT_COUNT_MAX);
    }

    SECTION("values above the maximum clamp down") {
        config.set(SETTING_SPEED_DIAL_RECENT_COUNT, "42");
        REQUIRE(config.get_speed_dial_recent_count() == SPEED_DIAL_RECENT_COUNT_MAX);
    }

    SECTION("negative values clamp up to 0") {
        config.set(SETTING_SPEED_DIAL_RECENT_COUNT, "-3");
        REQUIRE(config.get_speed_dial_recent_count() == 0);
    }

    SECTION("garbage falls back to the default") {
        config.set(SETTING_SPEED_DIAL_RECENT_COUNT, "abc");
        REQUIRE(config.get_speed_dial_recent_count() == SPEED_DIAL_RECENT_COUNT_DEFAULT);
    }
}
