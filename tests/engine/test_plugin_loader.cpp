// Catch2 smoke test for the Phase 1.5.1 plugin loader.
//
// Drives the Session plugin-host surface end-to-end against a fixture plugin
// built as a SHARED library by tests/engine/fixtures/loader_plugin/. The
// orchestrator's CMake wiring passes TEST_FIXTURES_DIR pointing at the build
// directory that holds the fixture binary + its manifest; when that define is
// missing the loader cases are skipped (the negative-path cases still run).

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Presets.hpp"
#include "orca/Project.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/Events.hpp"
#include "orca/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

#ifdef TEST_FIXTURES_DIR
constexpr const char* kFixturesDir = TEST_FIXTURES_DIR;
#else
constexpr const char* kFixturesDir = "";
#endif

namespace fs = std::filesystem;

TEST_CASE("Loader fixture plugin loads + registers + unloads", "[engine][plugins][loader]") {
    if (std::string(kFixturesDir).empty()) {
        SKIP("TEST_FIXTURES_DIR not defined at compile time");
    }

    fs::path fixture_dir = fs::path(kFixturesDir) / "loader_plugin";
    if (!fs::exists(fixture_dir)) {
        SKIP("Fixture plugin directory missing: " + fixture_dir.string());
    }

    auto s = orca::Session::create();
    REQUIRE(s != nullptr);

    REQUIRE(s->loaded_plugin_ids().empty());
    REQUIRE(s->registered_slot_count() == 0);

    SECTION("load via load_plugin(dir)") {
        auto r = s->load_plugin(fixture_dir);
        if (!r.ok()) {
            FAIL("load_plugin failed: " + r.error().message);
        }

        auto ids = s->loaded_plugin_ids();
        REQUIRE(ids.size() == 1);
        REQUIRE(ids[0] == "test.fixture.loader_plugin");

        // The fixture registers one ORCA_SLOT_PIPELINE_OBSERVER.
        REQUIRE(s->registered_slot_count() >= 1);
        REQUIRE(s->is_plugin_loaded("test.fixture.loader_plugin"));

        auto unr = s->unload_plugin("test.fixture.loader_plugin");
        REQUIRE(unr.ok());
        REQUIRE_FALSE(s->is_plugin_loaded("test.fixture.loader_plugin"));
        REQUIRE(s->registered_slot_count() == 0);
    }

    SECTION("load via discover_and_load on parent dir") {
        // discover_and_load_plugins walks <plugins_dir>/<id>/manifest.json
        // entries. Point it at the parent so the loader_plugin/ subdir is
        // discovered as the canonical layout.
        fs::path parent = fixture_dir.parent_path();
        auto count = s->discover_and_load_plugins(parent);
        REQUIRE(count >= 1);
        REQUIRE(s->is_plugin_loaded("test.fixture.loader_plugin"));

        s->unload_all_plugins();
        REQUIRE(s->loaded_plugin_ids().empty());
    }
}

TEST_CASE("load_plugin on a non-existent directory returns an error", "[engine][plugins][loader]") {
    auto s = orca::Session::create();
    REQUIRE(s);

    auto r = s->load_plugin(fs::path("/definitely/does/not/exist/orca-fake-plugin"));
    REQUIRE_FALSE(r.ok());
    // Don't pin on the exact error code — any non-OK is fine.
}

TEST_CASE("unload_plugin on an unloaded id returns NotFound", "[engine][plugins][loader]") {
    auto s = orca::Session::create();
    REQUIRE(s);

    auto r = s->unload_plugin("never.was.loaded");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == orca::ErrorCode::NotFound);
}
