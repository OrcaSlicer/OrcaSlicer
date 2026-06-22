// Phase 3.2.6 — PluginManager dispatches kind=="wasm" manifests to WasmPlugin.
//
// Builds a wasm fixture at runtime: writes a manifest.json with
// "kind": "wasm" and a sibling .wasm file (generated via
// wasmtime_wat2wasm) into a temp dir under TEST_TMP_DIR. Then drives the
// public engine surface — session->load_plugin(dir) — and asserts the
// wasm plugin was discovered, instantiated, and is enumerable via
// loaded_plugin_ids.
//
// Unload + drop must fire orca_plugin_unregister cleanly: the test ends
// by explicitly calling session->unload_plugin and asserting the count
// drops to zero, exercising the wasm_plugin destruction path through
// PluginManager::unload_plugin.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"

#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/wat.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr const char* kWatLifecycle = R"WAT(
(module
  (func (export "orca_plugin_register")
        (param $abi i32) (param $registry i64) (result i32)
    i32.const 0)
  (func (export "orca_plugin_unregister"))
  (memory (export "memory") 1))
)WAT";

std::filesystem::path stage_wasm_plugin(const std::string& subdir,
                                        const std::string& id,
                                        const std::string& version,
                                        const std::string& wat) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path{TEST_TMP_DIR} / subdir;
    const fs::path dir  = root / id;
    fs::create_directories(dir);

    // manifest.json with kind=wasm.
    {
        std::ofstream m(dir / "manifest.json", std::ios::trunc);
        m << "{\n"
          << "  \"id\": \""      << id      << "\",\n"
          << "  \"version\": \"" << version << "\",\n"
          << "  \"kind\": \"wasm\",\n"
          << "  \"permissions\": []\n"
          << "}\n";
    }

    // <id>.wasm assembled from WAT inline.
    wasm_byte_vec_t bytes;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &bytes);
    REQUIRE(err == nullptr);
    {
        std::ofstream out(dir / (id + ".wasm"),
                          std::ios::binary | std::ios::trunc);
        out.write(bytes.data, static_cast<std::streamsize>(bytes.size));
    }
    wasm_byte_vec_delete(&bytes);

    return root;
}

} // namespace

TEST_CASE("Phase 3.2.6 — PluginManager routes kind=\"wasm\" to WasmPlugin",
          "[plugin][wasm][dispatch]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    constexpr const char* kId      = "test.fixture.wasm_dispatch";
    constexpr const char* kVersion = "0.0.1";

    const auto packs_root = stage_wasm_plugin(
        "wasm_dispatch_packs", kId, kVersion, kWatLifecycle);

    SECTION("discover_and_load loads the wasm plugin and unload tears it down") {
        const auto before = session->loaded_plugin_ids().size();

        const auto loaded = session->discover_and_load_plugins(packs_root);
        CHECK(loaded == 1);

        CHECK(session->is_plugin_loaded(kId));
        CHECK(session->loaded_plugin_ids().size() == before + 1);

        // Unload — WasmPlugin's destructor fires orca_plugin_unregister and
        // PluginManager scrubs the registry record.
        const auto unload = session->unload_plugin(kId);
        REQUIRE(unload.ok());
        CHECK_FALSE(session->is_plugin_loaded(kId));
        CHECK(session->loaded_plugin_ids().size() == before);
    }

    SECTION("manifest missing the .wasm entry returns NotFound") {
        namespace fs = std::filesystem;
        const fs::path dir = packs_root / kId;
        fs::remove(dir / (std::string{kId} + ".wasm"));

        auto rc = session->load_plugin(dir);
        REQUIRE_FALSE(rc.ok());
        CHECK(rc.error().code == orca::ErrorCode::NotFound);
    }
}
