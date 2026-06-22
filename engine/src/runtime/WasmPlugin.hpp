// Phase 3.2.4 — WasmPlugin: lifecycle adapter that makes a wasm instance
// behave like a native plugin from PluginManager's perspective.
//
// A wasm plugin must export the same three lifecycle hooks the native ABI
// requires (engine/include/orca/plugin_api.h, §"Mandatory plugin exports"):
//   - orca_plugin_check_debug_consistent(engine_is_debug: i32) -> i32
//   - orca_plugin_register(abi_version: i32, registry_handle: i64) -> i32
//   - orca_plugin_unregister() -> ()
//
// The registry handle is opaque to the guest — it's an uintptr_t-sized
// host handle that the guest passes back to slot-registration imports
// when they exist. For now the host implants the PluginRegistry pointer
// (or any other token) and the guest just round-trips it.
#pragma once

#include "orca/Result.hpp"
#include "WasmImports.hpp"   // PluginSlotSink + ImportContext

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace orca {
class Session;
class PluginRegistry;
} // namespace orca

namespace orca::wasm {

class WasmHost;
class WasmInstance;

class WasmPlugin : public PluginSlotSink {
public:
    /// Manifest fields WasmPlugin needs to bring a guest up. Mirrors the
    /// shape PluginManager has already parsed from manifest.json.
    struct Manifest {
        std::string           id;
        std::string           version;
        std::uint64_t         permissions = 0;
        std::filesystem::path wasm_path;
    };

    /// Compile, instantiate, and run the lifecycle exports against `host`.
    /// On any non-OK orca_plugin_register return code the instance is
    /// dropped and the original code is mapped into the returned Result.
    /// `registry` (when non-null) is what slot-registration imports call
    /// into to add per-plugin slots; pass null for tests that don't use
    /// slot registration.
    static Result<std::unique_ptr<WasmPlugin>>
    load(WasmHost& host, const Manifest& manifest, Session* session,
         PluginRegistry* registry);

    ~WasmPlugin() override;

    // PluginSlotSink — called from orca_register_pipeline_observer.
    std::uint64_t register_observer_from_wasm(
        wasmtime_context_t* ctx,
        wasmtime_func_t     guest_func) override;

    WasmPlugin(const WasmPlugin&)            = delete;
    WasmPlugin& operator=(const WasmPlugin&) = delete;

    const std::string& id() const noexcept;
    const std::string& version() const noexcept;
    std::uint64_t      permissions() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit WasmPlugin(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace orca::wasm
