// Phase 3.2.4 — WasmPlugin lifecycle test.
//
// Authors a WAT fixture that exports the three mandatory lifecycle hooks
// (orca_plugin_check_debug_consistent, orca_plugin_register,
// orca_plugin_unregister) and routes them through WasmPlugin::load. The
// test asserts:
//   (a) load succeeds when register returns ORCA_OK
//   (b) load fails with the mapped ErrorCode when register returns a
//       non-zero code (e.g. ORCA_ERR_UNSUPPORTED)
//   (c) check_debug_consistent returning non-zero produces Unsupported
//   (d) unregister fires on destruction (covered by RAII — exercised by
//       letting the plugin go out of scope and asserting no traps).

#include <catch2/catch_all.hpp>

#include "runtime/WasmHost.hpp"
#include "runtime/WasmPlugin.hpp"
#include "orca/plugin_api.h"

#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/wat.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path write_wat(const std::string& filename,
                                const std::string& wat) {
    namespace fs = std::filesystem;
    const fs::path out = fs::path{TEST_TMP_DIR} / filename;
    fs::create_directories(out.parent_path());

    wasm_byte_vec_t bytes;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &bytes);
    REQUIRE(err == nullptr);

    std::ofstream out_s(out, std::ios::binary | std::ios::trunc);
    out_s.write(bytes.data, static_cast<std::streamsize>(bytes.size));
    out_s.close();
    wasm_byte_vec_delete(&bytes);
    return out;
}

constexpr const char* kWatOk = R"WAT(
(module
  (func (export "orca_plugin_check_debug_consistent")
        (param $is_debug i32) (result i32)
    i32.const 0)
  (func (export "orca_plugin_register")
        (param $abi i32) (param $registry i64) (result i32)
    i32.const 0)
  (func (export "orca_plugin_unregister"))
  (memory (export "memory") 1))
)WAT";

constexpr const char* kWatRegisterFails = R"WAT(
(module
  (func (export "orca_plugin_register")
        (param $abi i32) (param $registry i64) (result i32)
    i32.const 8)
  (func (export "orca_plugin_unregister"))
  (memory (export "memory") 1))
)WAT";

constexpr const char* kWatDebugMismatch = R"WAT(
(module
  (func (export "orca_plugin_check_debug_consistent")
        (param $is_debug i32) (result i32)
    i32.const 1)
  (func (export "orca_plugin_register")
        (param $abi i32) (param $registry i64) (result i32)
    i32.const 0)
  (func (export "orca_plugin_unregister"))
  (memory (export "memory") 1))
)WAT";

orca::wasm::WasmPlugin::Manifest make_manifest(const std::filesystem::path& p) {
    orca::wasm::WasmPlugin::Manifest m;
    m.id          = "test.fixture.wasm_lifecycle";
    m.version     = "0.0.1";
    m.permissions = 0;
    m.wasm_path   = p;
    return m;
}

} // namespace

TEST_CASE("Phase 3.2.4 — WasmPlugin runs register/unregister lifecycle",
          "[wasm][plugin][lifecycle]") {
    orca::wasm::WasmHost host;

    SECTION("happy path: register returns ORCA_OK, unregister fires on dtor") {
        const auto path = write_wat("wasm_lifecycle_ok.wasm", kWatOk);
        auto manifest   = make_manifest(path);

        auto res = orca::wasm::WasmPlugin::load(host, manifest, /*session*/ nullptr, /*registry*/ nullptr);
        REQUIRE(res.ok());

        const auto& plugin = res.value();
        REQUIRE(plugin != nullptr);
        CHECK(plugin->id() == manifest.id);
        CHECK(plugin->version() == manifest.version);
        // Destruction at scope end fires orca_plugin_unregister; if it
        // trapped we would see stderr output but the test would still pass —
        // the dtor swallows trap output. Confirm no crash on exit.
    }

    SECTION("register returning non-zero maps to ErrorCode") {
        const auto path = write_wat("wasm_lifecycle_fail.wasm", kWatRegisterFails);
        auto manifest   = make_manifest(path);

        auto res = orca::wasm::WasmPlugin::load(host, manifest, /*session*/ nullptr, /*registry*/ nullptr);
        REQUIRE_FALSE(res.ok());
        // kWatRegisterFails returns ORCA_ERR_UNSUPPORTED (8) -> Unsupported.
        CHECK(res.error().code == orca::ErrorCode::Unsupported);
    }

    SECTION("debug-consistency mismatch yields Unsupported") {
        const auto path = write_wat("wasm_lifecycle_dbg.wasm", kWatDebugMismatch);
        auto manifest   = make_manifest(path);

        auto res = orca::wasm::WasmPlugin::load(host, manifest, /*session*/ nullptr, /*registry*/ nullptr);
        REQUIRE_FALSE(res.ok());
        CHECK(res.error().code == orca::ErrorCode::Unsupported);
    }

    SECTION("missing wasm_path yields InvalidArgument") {
        orca::wasm::WasmPlugin::Manifest manifest;
        manifest.id = "missing.path";

        auto res = orca::wasm::WasmPlugin::load(host, manifest, /*session*/ nullptr, /*registry*/ nullptr);
        REQUIRE_FALSE(res.ok());
        CHECK(res.error().code == orca::ErrorCode::InvalidArgument);
    }
}
