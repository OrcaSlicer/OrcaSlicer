#include <catch2/catch_all.hpp>

// PluginRegistry lives in engine/src/ (engine-internal). The engine_tests
// CMakeLists adds ${CMAKE_SOURCE_DIR}/engine/src to the include path so we
// can drive it directly for unit tests.
#include "PluginRegistry.hpp"

#include "orca/plugin_api.h"
#include "orca/Result.hpp"

#include <string>
#include <vector>

namespace {

// Fake vtables — just non-null pointers; the registry stores them opaquely.
int g_vt_a = 1;
int g_vt_b = 2;
int g_vt_c = 3;

} // namespace

TEST_CASE("PluginRegistry rejects add_slot without a current plugin id", "[engine][plugins][registry]") {
    orca::PluginRegistry reg;
    REQUIRE(reg.slot_count() == 0);

    // No set_current_plugin_id -> add_slot returns 0 (failure).
    auto id = reg.add_slot(ORCA_SLOT_PIPELINE_OBSERVER, &g_vt_a, nullptr);
    REQUIRE(id == 0);
    REQUIRE(reg.slot_count() == 0);
}

TEST_CASE("PluginRegistry add / remove slot lifecycle", "[engine][plugins][registry]") {
    orca::PluginRegistry reg;

    reg.set_current_plugin_id("test.plugin.alpha");

    auto id1 = reg.add_slot(ORCA_SLOT_PIPELINE_OBSERVER, &g_vt_a, nullptr, /*priority=*/10);
    auto id2 = reg.add_slot(ORCA_SLOT_PIPELINE_OBSERVER, &g_vt_b, nullptr, /*priority=*/5);
    auto id3 = reg.add_slot(ORCA_SLOT_GCODE_FILTER,      &g_vt_c, nullptr);

    REQUIRE(id1 != 0);
    REQUIRE(id2 != 0);
    REQUIRE(id3 != 0);
    REQUIRE(id1 != id2);
    REQUIRE(id1 != id3);
    REQUIRE(id2 != id3);
    REQUIRE(reg.slot_count() == 3);

    SECTION("snapshot orders by priority ascending then slot_id") {
        auto snap = reg.snapshot(ORCA_SLOT_PIPELINE_OBSERVER);
        REQUIRE(snap.size() == 2);
        // id2 has priority 5 (< id1's 10) so it comes first.
        REQUIRE(snap[0].vtable == &g_vt_b);
        REQUIRE(snap[1].vtable == &g_vt_a);
    }

    SECTION("snapshot returns empty for an unused kind") {
        auto snap = reg.snapshot(ORCA_SLOT_PRINTER_AGENT);
        REQUIRE(snap.empty());
    }

    SECTION("remove_slot drops the entry") {
        auto r = reg.remove_slot(id1);
        REQUIRE(r.ok());
        REQUIRE(reg.slot_count() == 2);

        auto snap = reg.snapshot(ORCA_SLOT_PIPELINE_OBSERVER);
        REQUIRE(snap.size() == 1);
        REQUIRE(snap[0].slot_id == id2);
    }

    SECTION("remove_slot on a stale id returns NotFound") {
        auto r = reg.remove_slot(99999);
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error().code == orca::ErrorCode::NotFound);
    }

    SECTION("remove_by_plugin drops every slot for that id") {
        reg.set_current_plugin_id("test.plugin.beta");
        auto id4 = reg.add_slot(ORCA_SLOT_PIPELINE_OBSERVER, &g_vt_c, nullptr);
        REQUIRE(id4 != 0);
        REQUIRE(reg.slot_count() == 4);

        reg.remove_by_plugin("test.plugin.alpha");
        REQUIRE(reg.slot_count() == 1);

        // The single survivor belongs to beta.
        auto snap = reg.snapshot(ORCA_SLOT_PIPELINE_OBSERVER);
        REQUIRE(snap.size() == 1);
        REQUIRE(snap[0].plugin_id == "test.plugin.beta");
    }
}

TEST_CASE("PluginRegistry manifest storage", "[engine][plugins][registry]") {
    orca::PluginRegistry reg;
    reg.set_current_plugin_id("test.plugin.manifest");

    orca::StoredManifest m;
    m.id          = "test.plugin.manifest";
    m.name        = "Manifest Test";
    m.version     = "1.0.0";
    m.author      = "Tests";
    m.description = "self-check";
    m.permissions = ORCA_PERM_NETWORK | ORCA_PERM_FILESYSTEM_READ;

    auto r = reg.set_manifest(m);
    REQUIRE(r.ok());

    auto list = reg.manifests();
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].id == "test.plugin.manifest");
    REQUIRE(list[0].permissions == (ORCA_PERM_NETWORK | ORCA_PERM_FILESYSTEM_READ));
}

TEST_CASE("PluginRegistry rejects mismatched manifest id", "[engine][plugins][registry]") {
    orca::PluginRegistry reg;
    reg.set_current_plugin_id("test.plugin.x");

    orca::StoredManifest m;
    m.id = "test.plugin.y";
    auto r = reg.set_manifest(m);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == orca::ErrorCode::InvalidArgument);
}
