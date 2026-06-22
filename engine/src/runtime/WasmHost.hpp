// Phase 3.2.2 — WasmHost: wraps a single shared wasmtime_engine_t and exposes
// a load_wasm(path) -> WasmInstance entry point so callers (PluginManager,
// the wasm fixture tests, etc.) don't depend on wasmtime.h directly.
//
// Design note: this header deliberately uses opaque void* state so consumers
// only need <orca/Result.hpp> + standard library. The wasmtime C API headers
// only get pulled into WasmHost.cpp / WasmPlugin.cpp where the runtime work
// actually happens.
#pragma once

#include "orca/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace orca::wasm {

struct ImportContext;

/// RAII wrapper for one instantiated wasm module. Owns the wasmtime
/// store + module + instance triple. Pimpl so this header doesn't drag
/// wasmtime.h into every consumer.
class WasmInstance {
public:
    struct Impl;
    explicit WasmInstance(std::unique_ptr<Impl> impl) noexcept;
    ~WasmInstance();

    WasmInstance(const WasmInstance&)            = delete;
    WasmInstance& operator=(const WasmInstance&) = delete;
    WasmInstance(WasmInstance&&)                 = delete;
    WasmInstance& operator=(WasmInstance&&)      = delete;

    /// Compiled wasm bytecode size in bytes (informational/telemetry).
    std::size_t module_size() const noexcept;

    /// Invoke an exported function with one i64 argument that returns one
    /// i32. Returns ErrorCode::NotFound if the export does not exist or
    /// has the wrong signature, ParseError on a runtime trap, and Unknown
    /// on any other wasmtime error.
    Result<std::int32_t> call_i64_to_i32(const std::string& fn_name,
                                         std::int64_t arg);

    /// Invoke an exported function with one i32 argument returning i32.
    /// Used by WasmPlugin for orca_plugin_check_debug_consistent.
    Result<std::int32_t> call_i32_to_i32(const std::string& fn_name,
                                         std::int32_t arg);

    /// Invoke (i32, i64) -> i32. Used by WasmPlugin for
    /// orca_plugin_register(abi_version, registry_handle).
    Result<std::int32_t> call_i32_i64_to_i32(const std::string& fn_name,
                                             std::int32_t arg0,
                                             std::int64_t arg1);

    /// Invoke a parameterless void-returning export. Used by WasmPlugin
    /// for orca_plugin_unregister.
    Result<void> call_void(const std::string& fn_name);

    /// True if `fn_name` is exported as a function. Lets WasmPlugin probe
    /// optional exports (check_debug_consistent, unregister) without
    /// triggering a "wasm export not found" Result error.
    bool has_export(const std::string& fn_name) const;

private:
    std::unique_ptr<Impl> impl_;
};

/// One per process. Owns the wasmtime_engine_t — expensive to construct,
/// cheap to share. Stores are created per loaded plugin so guest memory
/// stays isolated, but they all hand back to the same engine.
class WasmHost {
public:
    WasmHost();
    ~WasmHost();

    WasmHost(const WasmHost&)            = delete;
    WasmHost& operator=(const WasmHost&) = delete;

    /// Read `path`, compile it as a wasm module, install the orca_* host
    /// imports against `ctx` (see WasmImports.hpp), and instantiate. The
    /// ImportContext must outlive the returned WasmInstance — it is
    /// referenced by every import callback for permission decisions and
    /// engine-services access. Pass an empty / default-constructed
    /// ImportContext for smoke loads that don't call host imports.
    Result<std::unique_ptr<WasmInstance>>
    load_wasm(const std::filesystem::path& path, ImportContext& ctx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca::wasm
