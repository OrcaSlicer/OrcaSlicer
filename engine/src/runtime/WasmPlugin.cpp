// Phase 3.2.4 — WasmPlugin lifecycle implementation.

#include "WasmPlugin.hpp"
#include "WasmHost.hpp"
#include "WasmImports.hpp"

#include "../PluginRegistry.hpp"
#include "orca/plugin_api.h"

#include <wasmtime.h>

#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace orca::wasm {

// Per-registered-observer state. Lives in WasmPlugin's owned vector so
// the void* user_data pointer the registry hands the on_step thunk
// stays valid for the plugin's lifetime.
struct GuestObserverState {
    wasmtime_context_t* ctx        = nullptr;  // borrowed; owned by the store
    wasmtime_func_t     func{};
    std::mutex*         store_mtx  = nullptr;  // borrowed; owned by WasmPlugin
};

// on_step thunk — host -> guest bridge invoked by PluginRegistry dispatch.
// Best-effort: a trap or error in the guest is swallowed (logged, dropped)
// so a buggy plugin observer never aborts the slice.
extern "C" void wasm_observer_on_step(orca_pipeline_step_t step,
                                      std::uint64_t        slice_handle,
                                      void*                user_data) {
    auto* s = static_cast<GuestObserverState*>(user_data);
    if (!s || !s->ctx || !s->store_mtx) return;

    std::lock_guard<std::mutex> guard(*s->store_mtx);

    wasmtime_val_t args[2];
    args[0].kind = WASMTIME_I32;
    args[0].of.i32 = static_cast<std::int32_t>(step);
    args[1].kind = WASMTIME_I64;
    args[1].of.i64 = static_cast<std::int64_t>(slice_handle);

    wasm_trap_t*      trap = nullptr;
    wasmtime_error_t* err  = wasmtime_func_call(
        s->ctx, &s->func, args, 2, nullptr, 0, &trap);
    if (err)  wasmtime_error_delete(err);
    if (trap) wasm_trap_delete(trap);
}


namespace {

constexpr const char* kCheckDebugFn   = "orca_plugin_check_debug_consistent";
constexpr const char* kRegisterFn     = "orca_plugin_register";
constexpr const char* kUnregisterFn   = "orca_plugin_unregister";

// Engine-side "is debug" sentinel passed to the guest's check function.
// Mirrors the int flag the native loader uses (PluginManager.cpp:592).
constexpr std::int32_t kEngineIsDebug =
#if !defined(NDEBUG)
    1;
#else
    0;
#endif

ErrorCode map_orca_rc(std::int32_t rc) {
    switch (rc) {
        case ORCA_OK:                    return ErrorCode::Unknown;  // unreachable
        case ORCA_ERR_INVALID_ARGUMENT:  return ErrorCode::InvalidArgument;
        case ORCA_ERR_NOT_FOUND:         return ErrorCode::NotFound;
        case ORCA_ERR_ALREADY_EXISTS:    return ErrorCode::AlreadyExists;
        case ORCA_ERR_IO:                return ErrorCode::IoError;
        case ORCA_ERR_PARSE:             return ErrorCode::ParseError;
        case ORCA_ERR_CANCELLED:         return ErrorCode::Cancelled;
        case ORCA_ERR_UNSUPPORTED:       return ErrorCode::Unsupported;
        case ORCA_ERR_PERMISSION_DENIED: return ErrorCode::PermissionDenied;
        default:                         return ErrorCode::Unknown;
    }
}

} // namespace

struct WasmPlugin::Impl {
    ImportContext                 ctx;
    std::string                   version;
    std::unique_ptr<WasmInstance> instance;
    PluginRegistry*               registry = nullptr;

    // One mutex shared across all GuestObserverState entries — wasmtime
    // stores aren't thread-safe so observer thunks must serialize.
    std::mutex                                        store_mtx;
    std::vector<std::unique_ptr<GuestObserverState>>  observer_states;

    // Single heap-allocated observer vtable — every observer this plugin
    // registers points at it (the per-observer state is the user_data).
    orca_slot_pipeline_observer_t                     observer_vtable{};
};

WasmPlugin::WasmPlugin(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

WasmPlugin::~WasmPlugin() {
    if (!impl_ || !impl_->instance) return;
    if (impl_->instance->has_export(kUnregisterFn)) {
        auto rc = impl_->instance->call_void(kUnregisterFn);
        if (!rc.ok()) {
            std::fprintf(stderr,
                "[orca][wasm][%s] %s failed: %s\n",
                impl_->ctx.plugin_id.c_str(),
                kUnregisterFn,
                rc.error().message.c_str());
        }
    }
}

Result<std::unique_ptr<WasmPlugin>>
WasmPlugin::load(WasmHost& host, const Manifest& manifest, Session* session,
                 PluginRegistry* registry) {
    using R = Result<std::unique_ptr<WasmPlugin>>;

    if (manifest.id.empty())
        return R{Error{ErrorCode::InvalidArgument,
                       "WasmPlugin::load: empty manifest id"}};
    if (manifest.wasm_path.empty())
        return R{Error{ErrorCode::InvalidArgument,
                       "WasmPlugin::load: empty wasm_path for " + manifest.id}};

    auto impl = std::make_unique<Impl>();
    impl->ctx.plugin_id   = manifest.id;
    impl->ctx.permissions = manifest.permissions;
    impl->ctx.session     = session;
    impl->version         = manifest.version;
    impl->registry        = registry;
    impl->observer_vtable.struct_size = sizeof(orca_slot_pipeline_observer_t);
    impl->observer_vtable.on_step     = &wasm_observer_on_step;

    auto inst_res = host.load_wasm(manifest.wasm_path, impl->ctx);
    if (!inst_res.ok())
        return R{inst_res.error()};
    impl->instance = std::move(inst_res).value();

    // The slot sink must be live before orca_plugin_register runs so the
    // guest can call orca_register_pipeline_observer mid-register. Build
    // the WasmPlugin object now and patch ictx.slot_sink to point at it
    // before the lifecycle exports fire.
    auto plugin = std::unique_ptr<WasmPlugin>(new WasmPlugin(std::move(impl)));
    plugin->impl_->ctx.slot_sink = plugin.get();
    auto& pimpl = *plugin->impl_;

    // 1. Debug/release consistency. The export is optional — guests
    //    targeting a single ABI flavor can skip it.
    if (pimpl.instance->has_export(kCheckDebugFn)) {
        auto rc = pimpl.instance->call_i32_to_i32(kCheckDebugFn, kEngineIsDebug);
        if (!rc.ok()) return R{rc.error()};
        if (rc.value() != 0)
            return R{Error{ErrorCode::Unsupported,
                           manifest.id +
                           ": orca_plugin_check_debug_consistent reported mismatch"}};
    }

    // 2. orca_plugin_register — mandatory.
    if (!pimpl.instance->has_export(kRegisterFn))
        return R{Error{ErrorCode::NotFound,
                       manifest.id + ": missing required export " + kRegisterFn}};

    auto reg = pimpl.instance->call_i32_i64_to_i32(
        kRegisterFn,
        static_cast<std::int32_t>(ORCA_PLUGIN_ABI_VERSION),
        /*registry_handle*/ reinterpret_cast<std::int64_t>(pimpl.registry));
    if (!reg.ok()) return R{reg.error()};
    if (reg.value() != ORCA_OK)
        return R{Error{map_orca_rc(reg.value()),
                       manifest.id + ": " + kRegisterFn + " returned " +
                       std::to_string(reg.value())}};

    return ok(std::move(plugin));
}

std::uint64_t WasmPlugin::register_observer_from_wasm(
    wasmtime_context_t* ctx,
    wasmtime_func_t     guest_func)
{
    if (!impl_ || !impl_->registry || !ctx) return 0;

    auto state = std::make_unique<GuestObserverState>();
    state->ctx       = ctx;
    state->func      = guest_func;
    state->store_mtx = &impl_->store_mtx;

    impl_->registry->set_current_plugin_id(impl_->ctx.plugin_id);
    const auto slot_id = impl_->registry->add_slot(
        ORCA_SLOT_PIPELINE_OBSERVER,
        &impl_->observer_vtable,
        state.get(),
        /*priority=*/0);
    impl_->registry->clear_current_plugin_id();

    if (slot_id == 0) return 0;
    impl_->observer_states.push_back(std::move(state));
    return static_cast<std::uint64_t>(slot_id);
}

const std::string& WasmPlugin::id() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->ctx.plugin_id : empty;
}

const std::string& WasmPlugin::version() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->version : empty;
}

std::uint64_t WasmPlugin::permissions() const noexcept {
    return impl_ ? impl_->ctx.permissions : 0;
}

} // namespace orca::wasm
