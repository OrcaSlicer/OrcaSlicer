// Phase 3.1 — exercises PluginManager::reload_plugin via Session::reload_plugin.
// Uses the existing loader_plugin fixture (built by Phase 1.5).

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"

#include <filesystem>

TEST_CASE("Phase 3.1 — reload_plugin unloads and re-loads from the same source dir",
          "[plugin][reload]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    namespace fs = std::filesystem;
    const fs::path plugin_dir = fs::path{TEST_FIXTURES_DIR} / "loader_plugin";
    REQUIRE(fs::is_directory(plugin_dir));

    const auto load_rc = session->load_plugin(plugin_dir);
    REQUIRE(load_rc.ok());

    constexpr const char* kId = "test.fixture.loader_plugin";
    REQUIRE(session->is_plugin_loaded(kId));

    const auto slots_after_load = session->registered_slot_count();
    REQUIRE(slots_after_load >= 1);

    SECTION("reload of an existing plugin succeeds and preserves slots") {
        const auto reload_rc = session->reload_plugin(kId);
        REQUIRE(reload_rc.ok());
        CHECK(session->is_plugin_loaded(kId));
        CHECK(session->registered_slot_count() == slots_after_load);
    }

    SECTION("reload of an unknown plugin returns NotFound") {
        const auto rc = session->reload_plugin("does.not.exist");
        REQUIRE_FALSE(rc.ok());
        CHECK(rc.error().code == orca::ErrorCode::NotFound);
    }
}
