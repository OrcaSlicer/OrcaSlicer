// Phase 3.2.2 + 3.2.3 — WasmHost + WasmImports smoke tests.
//
// (a) WasmHost.load_wasm accepts a minimal 8-byte empty module (magic +
//     version), rejects missing files with NotFound, and rejects garbage
//     bytes with ParseError.
// (b) Imports installed by Phase 3.2.3 are callable from a guest module:
//     a WAT fixture imports orca_check_permission and asserts the host
//     reports the granted bit correctly. This round-trips
//     wasmtime_linker_instantiate + a host callback + the ImportContext
//     permission gate end-to-end.

#include <catch2/catch_all.hpp>

#include "runtime/WasmHost.hpp"
#include "runtime/WasmImports.hpp"
#include "orca/plugin_api.h"

#include <wasmtime.h>
#include <wasmtime/wat.h>
#include <wasm.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path write_empty_wasm(const std::string& name) {
    namespace fs = std::filesystem;
    const fs::path out = fs::path{TEST_TMP_DIR} / name;
    fs::create_directories(out.parent_path());

    // 8-byte minimal wasm: magic + version 1.
    constexpr std::array<std::uint8_t, 8> kEmptyWasm{
        0x00, 0x61, 0x73, 0x6d,   // \0asm
        0x01, 0x00, 0x00, 0x00,   // version 1
    };
    std::ofstream out_s(out, std::ios::binary | std::ios::trunc);
    out_s.write(reinterpret_cast<const char*>(kEmptyWasm.data()),
                static_cast<std::streamsize>(kEmptyWasm.size()));
    return out;
}

} // namespace

TEST_CASE("Phase 3.2.2 — WasmHost loads a minimal empty wasm module",
          "[wasm][host]") {
    orca::wasm::WasmHost host;
    orca::wasm::ImportContext ictx{};

    SECTION("load_wasm succeeds on a valid empty module") {
        const auto path = write_empty_wasm("empty_module.wasm");

        auto res = host.load_wasm(path, ictx);
        REQUIRE(res.ok());

        const auto& instance = res.value();
        REQUIRE(instance != nullptr);
        CHECK(instance->module_size() == 8);
    }

    SECTION("load_wasm returns NotFound for a missing file") {
        namespace fs = std::filesystem;
        const fs::path missing = fs::path{TEST_TMP_DIR} / "does_not_exist.wasm";
        std::error_code ec;
        fs::remove(missing, ec);

        auto res = host.load_wasm(missing, ictx);
        REQUIRE_FALSE(res.ok());
        CHECK(res.error().code == orca::ErrorCode::NotFound);
    }

    SECTION("load_wasm returns ParseError for garbage bytes") {
        namespace fs = std::filesystem;
        const fs::path junk = fs::path{TEST_TMP_DIR} / "garbage.wasm";
        std::ofstream out_s(junk, std::ios::binary | std::ios::trunc);
        const char garbage[] = "this is not a wasm module";
        out_s.write(garbage, sizeof(garbage) - 1);
        out_s.close();

        auto res = host.load_wasm(junk, ictx);
        REQUIRE_FALSE(res.ok());
        CHECK(res.error().code == orca::ErrorCode::ParseError);
    }
}

// ---------------------------------------------------------------------------
// Phase 3.2.3 — host imports round-trip through wasmtime_linker_instantiate.
// A WAT fixture imports orca_check_permission, exports a `check` function
// that calls it with ORCA_PERM_NETWORK, and returns the granted bit. We
// invoke `check` via wasmtime_func_call and assert it sees the same value
// the ImportContext was constructed with.
// ---------------------------------------------------------------------------
namespace {

std::filesystem::path write_check_permission_wasm(const std::string& name) {
    namespace fs = std::filesystem;
    const fs::path out = fs::path{TEST_TMP_DIR} / name;

    // WAT source: import orca_check_permission(i64)->i32 from "env", expose
    // exported "check" that forwards a constant perm bit through it.
    const std::string wat =
        "(module\n"
        "  (import \"env\" \"orca_check_permission\""
        "    (func $check_perm (param i64) (result i32)))\n"
        "  (func (export \"check\") (param $perm i64) (result i32)\n"
        "    local.get $perm\n"
        "    call $check_perm)\n"
        "  (memory (export \"memory\") 1))\n";

    wasm_byte_vec_t bytes;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &bytes);
    REQUIRE(err == nullptr);

    std::ofstream out_s(out, std::ios::binary | std::ios::trunc);
    out_s.write(bytes.data, static_cast<std::streamsize>(bytes.size));
    out_s.close();
    wasm_byte_vec_delete(&bytes);
    return out;
}

} // namespace

TEST_CASE("Phase 3.2.3 — WasmImports orca_check_permission round-trips",
          "[wasm][imports]") {
    orca::wasm::WasmHost host;
    orca::wasm::ImportContext ictx{};
    ictx.plugin_id   = "test.fixture.wasm_check_perm";
    ictx.permissions = ORCA_PERM_NETWORK; // grants only NETWORK

    const auto path = write_check_permission_wasm("check_perm.wasm");

    auto res = host.load_wasm(path, ictx);
    REQUIRE(res.ok());
    auto& instance = res.value();
    REQUIRE(instance != nullptr);

    SECTION("granted bit reports as 1") {
        const auto rc = instance->call_i64_to_i32(
            "check", static_cast<std::int64_t>(ORCA_PERM_NETWORK));
        REQUIRE(rc.ok());
        CHECK(rc.value() == 1);
    }

    SECTION("ungranted bit reports as 0") {
        const auto rc = instance->call_i64_to_i32(
            "check", static_cast<std::int64_t>(ORCA_PERM_FILESYSTEM_WRITE));
        REQUIRE(rc.ok());
        CHECK(rc.value() == 0);
    }
}
