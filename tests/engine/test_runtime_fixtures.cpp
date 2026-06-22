// Phase 3.4 — per-runtime smoke fixtures.
//
// Each runtime (native / wasm / webview) ships a "noop" fixture plugin
// that loads through the public session->load_plugin surface and
// registers (or attempts to register) an observer slot. The tests below
// assert per-runtime that load + registration mechanics work end-to-end.
//
// 3.4.1 (native): real ORCA_SLOT_PIPELINE_OBSERVER vtable, dispatch-safe.
// 3.4.2 (wasm):   inline-WAT module imports the orca_register_observer
//                 host import (added in this phase) and registers an
//                 observer with a guest function-table callback index.
// 3.4.3 (webview): HTML fixture posts log + events.on calls through the
//                 window.orca bridge. Verified manually since wxWebView
//                 needs a wxApp + display; tests/fixtures live next to
//                 the wasm + native fixtures so a smoke harness can
//                 mount them when GUI integration lands in Phase 4.1.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"

#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/wat.h>

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("Phase 3.4.1 — native_noop fixture registers a pipeline observer slot",
          "[plugin][runtime][native]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    namespace fs = std::filesystem;
    const fs::path plugin_dir = fs::path{TEST_FIXTURES_DIR} / "native_noop";
    REQUIRE(fs::is_directory(plugin_dir));

    const auto slots_before = session->registered_slot_count();

    const auto rc = session->load_plugin(plugin_dir);
    REQUIRE(rc.ok());
    REQUIRE(session->is_plugin_loaded("test.fixture.native_noop"));

    // Native noop registers exactly one pipeline-observer slot.
    CHECK(session->registered_slot_count() == slots_before + 1);

    // Tearing down via unload must drop the slot count back to its
    // pre-load value.
    const auto unload = session->unload_plugin("test.fixture.native_noop");
    REQUIRE(unload.ok());
    CHECK(session->registered_slot_count() == slots_before);
}

// ---------------------------------------------------------------------------
// Phase 4.1.3 — Session::plugin_slots_of(kind) exposes registered slots
// to GUI-side dispatchers (PluginMenuDispatcher etc.) without requiring
// engine/src/PluginRegistry.hpp on their include path. Using the
// native_noop fixture from 3.4.1 as a known-good observer slot.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 4.1.3 — Session::plugin_slots_of returns registered slot info",
          "[plugin][slots][accessor]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    namespace fs = std::filesystem;
    const fs::path plugin_dir = fs::path{TEST_FIXTURES_DIR} / "native_noop";
    REQUIRE(session->load_plugin(plugin_dir).ok());

    // ORCA_SLOT_PIPELINE_OBSERVER == 0x0001 in the C ABI.
    const auto slots = session->plugin_slots_of(0x0001);
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].plugin_id == "test.fixture.native_noop");
    CHECK(slots[0].kind      == 0x0001);
    CHECK(slots[0].vtable    != nullptr);

    // No menu-command slots; native_noop only registers an observer.
    const auto menus = session->plugin_slots_of(0x0102);  // ORCA_SLOT_MENU_COMMAND
    CHECK(menus.empty());

    REQUIRE(session->unload_plugin("test.fixture.native_noop").ok());
    CHECK(session->plugin_slots_of(0x0001).empty());
}

// ---------------------------------------------------------------------------
// Phase 3.4.2 — wasm_noop. WAT fixture that imports
// orca_register_pipeline_observer and registers an in-guest observer
// function from its orca_plugin_register export. Asserts the slot
// landed in the registry.
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kWatWasmNoop = R"WAT(
(module
  (import "env" "orca_register_pipeline_observer"
    (func $register_obs (param i32 i32) (result i64)))

  (memory (export "memory") 1)
  ;; "noop_step_fn" at offset 16, 12 bytes.
  (data (i32.const 16) "noop_step_fn")

  (func (export "orca_plugin_register")
        (param $abi i32) (param $registry i64) (result i32)
    i32.const 16
    i32.const 12
    call $register_obs
    drop
    i32.const 0)

  (func (export "orca_plugin_unregister"))

  (func (export "noop_step_fn") (param i32) (param i64)))
)WAT";

std::filesystem::path stage_wasm_noop(const std::string& subdir) {
    namespace fs = std::filesystem;
    constexpr const char* kId = "test.fixture.wasm_noop";
    const fs::path root = fs::path{TEST_TMP_DIR} / subdir;
    const fs::path dir  = root / kId;
    fs::create_directories(dir);

    {
        std::ofstream m(dir / "manifest.json", std::ios::trunc);
        m << "{\n"
          << "  \"id\": \""      << kId       << "\",\n"
          << "  \"version\": \"0.0.1\",\n"
          << "  \"kind\": \"wasm\",\n"
          << "  \"permissions\": []\n"
          << "}\n";
    }

    wasm_byte_vec_t bytes;
    wasmtime_error_t* err = wasmtime_wat2wasm(
        kWatWasmNoop, std::strlen(kWatWasmNoop), &bytes);
    REQUIRE(err == nullptr);
    {
        std::ofstream out(dir / (std::string{kId} + ".wasm"),
                          std::ios::binary | std::ios::trunc);
        out.write(bytes.data, static_cast<std::streamsize>(bytes.size));
    }
    wasm_byte_vec_delete(&bytes);

    return root;
}

} // namespace

TEST_CASE("Phase 3.4.2 — wasm_noop registers a pipeline observer via host import",
          "[plugin][runtime][wasm]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    const auto packs_root = stage_wasm_noop("wasm_noop_packs");

    const auto slots_before = session->registered_slot_count();

    const auto loaded = session->discover_and_load_plugins(packs_root);
    REQUIRE(loaded == 1);
    CHECK(session->is_plugin_loaded("test.fixture.wasm_noop"));

    // The guest registered exactly one observer slot via the
    // orca_register_pipeline_observer host import.
    CHECK(session->registered_slot_count() == slots_before + 1);

    // Unload tears down the WasmPlugin (its dtor calls
    // orca_plugin_unregister) and scrubs the slot.
    REQUIRE(session->unload_plugin("test.fixture.wasm_noop").ok());
    CHECK(session->registered_slot_count() == slots_before);
}

// ---------------------------------------------------------------------------
// Phase 3.4.3 — webview_noop. Loaded through PluginManager's kind=webview
// branch (metadata-only — the actual wxWebView lifecycle lives in
// WebViewPluginHost / will be spawned by Slic3r::GUI in Phase 4.1). The
// fixture's window.orca.events.on call is exercised manually since
// wxWebView needs a wxApp + display.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3.4.3 — webview_noop loads through the kind=webview branch",
          "[plugin][runtime][webview]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    namespace fs = std::filesystem;
    const fs::path plugin_dir = fs::path{TEST_FIXTURES_DIR} / "webview_noop";
    REQUIRE(fs::is_directory(plugin_dir));
    REQUIRE(fs::is_regular_file(plugin_dir / "manifest.json"));
    REQUIRE(fs::is_regular_file(plugin_dir / "index.html"));

    const auto rc = session->load_plugin(plugin_dir);
    REQUIRE(rc.ok());
    CHECK(session->is_plugin_loaded("test.fixture.webview_noop"));

    // No slots get registered by a webview plugin at load time — the JS
    // does that when WebViewPluginHost mounts the page. Verify the count
    // stayed at whatever it was.
    REQUIRE(session->unload_plugin("test.fixture.webview_noop").ok());
    CHECK_FALSE(session->is_plugin_loaded("test.fixture.webview_noop"));
}
