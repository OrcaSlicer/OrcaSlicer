// Phase 3.2.3 — WasmImports: host capability surface exposed to wasm guests.
//
// Each `orca_*` host import maps to a member of orca_plugin_host_t in
// plugin_api.h. Imports that need a permission grant return a non-zero
// orca_error_code_t when the guest's permission bitset lacks the required
// bit, mirroring how the native vtable refuses the call without trapping
// the plugin.
//
// String marshalling convention: every (ptr, len) i32 pair is interpreted
// as `len` UTF-8 bytes starting at guest linear memory offset `ptr`. There
// are no NUL-terminated strings in the guest ABI — len is authoritative.
//
// Function-callback imports (events_subscribe + register_printer_agent)
// require per-plugin storage of guest function indices and live with
// WasmPlugin in Phase 3.2.4. They are not part of this file's surface
// because their state-management concern is plugin lifecycle, not the
// imports infrastructure itself.

#pragma once

#include "orca/Result.hpp"

#include <wasmtime/func.h>     // wasmtime_func_t (POD by-value)
#include <wasmtime/store.h>    // wasmtime_context_t (opaque)

#include <cstdint>
#include <string>

struct wasmtime_linker;
typedef struct wasmtime_linker wasmtime_linker_t;
struct wasm_engine_t;

namespace orca {
class Session;
} // namespace orca

namespace orca::wasm {

/// Interface used by slot-registration host imports to register slots
/// back through the engine's PluginRegistry. WasmPlugin implements this;
/// WasmHost wires it into ImportContext after instantiation so the
/// orca_plugin_register call from the guest can hit it.
struct PluginSlotSink {
    virtual ~PluginSlotSink() = default;

    /// The guest just called `orca_register_pipeline_observer("name")`.
    /// `guest_func` is the exported function the guest wants invoked when
    /// the observer fires; the implementation must keep it alive (or look
    /// it back up via the store + instance) for as long as the slot lives.
    /// Returns the slot id on success, 0 on failure.
    virtual std::uint64_t register_observer_from_wasm(
        wasmtime_context_t* ctx,
        wasmtime_func_t     guest_func) = 0;
};

/// Per-instance host context. Stored on the wasmtime_store_t via
/// wasmtime_store_new(engine, data, finalizer) so every import callback
/// reaches it through wasmtime_context_get_data.
struct ImportContext {
    /// ORCA_PERM_* bitset granted at install / load time.
    std::uint64_t permissions = 0;

    /// Plugin id from manifest.json — used as the log prefix and the
    /// owner attribute when imports register slots.
    std::string plugin_id;

    /// Engine session this guest is bound to. May be null in unit
    /// tests; capability-gated imports that need it return
    /// ORCA_ERR_INVALID_ARGUMENT in that case.
    Session* session = nullptr;

    /// Slot-registration imports route through this sink. Set by
    /// WasmPlugin AFTER WasmHost::load_wasm returns; null while the
    /// instance is still being assembled. Imports that need it must
    /// no-op (return 0 / error) when sink is null.
    PluginSlotSink* slot_sink = nullptr;
};

/// Define every orca_* import on `linker` against `engine`. After this
/// call any wasm module that imports "orca_log", "orca_check_permission",
/// "orca_placeholder_set_string", etc. from module name "env" will be
/// satisfiable by wasmtime_linker_instantiate.
///
/// The linker itself is created by the caller and shared across all
/// modules that need the same import surface.
Result<void> install_imports(wasmtime_linker_t* linker,
                             wasm_engine_t* engine);

} // namespace orca::wasm
