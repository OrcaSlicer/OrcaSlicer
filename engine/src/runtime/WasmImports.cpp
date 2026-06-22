// Phase 3.2.3 — WasmImports implementation.
//
// Every import lives in the "env" module per the WASI / cdylib convention.
// Each callback grabs the wasmtime_context_t from the caller, casts the
// store data to ImportContext*, validates the permission bit (if any),
// reads any (ptr, len) string args from guest linear memory, then dispatches
// to engine services.
//
// Permission failures DO NOT trap the guest. They return non-zero
// orca_error_code_t values so the guest can recover cleanly — same
// contract as the native host vtable.

#include "WasmImports.hpp"

#include "orca/Session.hpp"
#include "orca/Presets.hpp"
#include "orca/c_api.h"
#include "orca/plugin_api.h"

#include "orca/PlaceholderProvider.hpp"
#include "libslic3r/PlaceholderParser.hpp"

#include <wasm.h>
#include <wasmtime.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace orca::wasm {

namespace {

// ---------------------------------------------------------------------------
// Memory marshalling — read a length-prefixed byte run out of guest
// linear memory. Returns empty string on any bounds failure; the caller
// is expected to treat that as an invalid-argument case rather than trap.
// ---------------------------------------------------------------------------
std::string read_guest_string(wasmtime_caller_t* caller,
                              std::int32_t guest_ptr,
                              std::int32_t guest_len) {
    if (guest_len <= 0) return {};

    wasmtime_context_t* ctx = wasmtime_caller_context(caller);

    wasmtime_extern_t mem_ext;
    if (!wasmtime_caller_export_get(caller, "memory", sizeof("memory") - 1, &mem_ext))
        return {};
    if (mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
        wasmtime_extern_delete(&mem_ext);
        return {};
    }

    std::uint8_t* base = wasmtime_memory_data(ctx, &mem_ext.of.memory);
    std::size_t   size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);

    const auto end = static_cast<std::size_t>(guest_ptr) +
                     static_cast<std::size_t>(guest_len);
    if (!base || end > size)
        return {};

    return std::string(reinterpret_cast<const char*>(base + guest_ptr),
                       static_cast<std::size_t>(guest_len));
}

ImportContext* import_ctx_from(wasmtime_caller_t* caller) {
    wasmtime_context_t* ctx = wasmtime_caller_context(caller);
    return static_cast<ImportContext*>(wasmtime_context_get_data(ctx));
}

// ---------------------------------------------------------------------------
// Import body helpers — each `host_*` function below has the wasmtime
// callback signature and is registered via define_func() at the bottom.
// ---------------------------------------------------------------------------

/// orca_log(level: i32, ptr: i32, len: i32) -> ()
wasm_trap_t* host_log(void* /*env*/, wasmtime_caller_t* caller,
                      const wasmtime_val_t* args, std::size_t nargs,
                      wasmtime_val_t* /*results*/, std::size_t /*nresults*/) {
    if (nargs < 3) return nullptr;
    const auto level = args[0].of.i32;
    const auto ptr   = args[1].of.i32;
    const auto len   = args[2].of.i32;

    auto* ictx = import_ctx_from(caller);
    const auto msg = read_guest_string(caller, ptr, len);

    std::fprintf(stderr, "[orca][wasm][%s][%d] %s\n",
                 ictx ? ictx->plugin_id.c_str() : "?",
                 level,
                 msg.c_str());
    return nullptr;
}

/// orca_abort(ptr: i32, len: i32) -> () (traps the guest)
wasm_trap_t* host_abort(void* /*env*/, wasmtime_caller_t* caller,
                        const wasmtime_val_t* args, std::size_t nargs,
                        wasmtime_val_t* /*results*/, std::size_t /*nresults*/) {
    std::string msg = "wasm plugin aborted";
    if (nargs >= 2) {
        const auto ptr = args[0].of.i32;
        const auto len = args[1].of.i32;
        auto user = read_guest_string(caller, ptr, len);
        if (!user.empty()) msg = std::move(user);
    }
    return wasmtime_trap_new(msg.data(), msg.size());
}

/// orca_check_permission(perm_bit: i64) -> i32  (1 = granted, 0 = denied)
wasm_trap_t* host_check_permission(void* /*env*/, wasmtime_caller_t* caller,
                                   const wasmtime_val_t* args, std::size_t nargs,
                                   wasmtime_val_t* results, std::size_t nresults) {
    std::uint64_t perm = 0;
    if (nargs >= 1) perm = static_cast<std::uint64_t>(args[0].of.i64);

    auto* ictx = import_ctx_from(caller);
    const std::int32_t granted = (ictx && (ictx->permissions & perm) == perm) ? 1 : 0;

    if (nresults >= 1) {
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = granted;
    }
    return nullptr;
}

/// orca_placeholder_set_string(name_ptr,name_len,value_ptr,value_len) -> i32 (orca_error_code_t)
wasm_trap_t* host_placeholder_set_string(void* /*env*/, wasmtime_caller_t* caller,
                                         const wasmtime_val_t* args, std::size_t nargs,
                                         wasmtime_val_t* results, std::size_t nresults) {
    std::int32_t rc = ORCA_ERR_INVALID_ARGUMENT;
    if (nargs >= 4) {
        const auto name  = read_guest_string(caller, args[0].of.i32, args[1].of.i32);
        const auto value = read_guest_string(caller, args[2].of.i32, args[3].of.i32);
        auto* parser = ::orca::placeholder_tls::current();
        if (parser && !name.empty()) {
            parser->set(name, value);
            rc = ORCA_OK;
        } else if (!parser) {
            rc = ORCA_ERR_INVALID_ARGUMENT;
        }
    }
    if (nresults >= 1) {
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = rc;
    }
    return nullptr;
}

/// orca_placeholder_set_int(name_ptr,name_len, value: i64) -> i32
wasm_trap_t* host_placeholder_set_int(void* /*env*/, wasmtime_caller_t* caller,
                                      const wasmtime_val_t* args, std::size_t nargs,
                                      wasmtime_val_t* results, std::size_t nresults) {
    std::int32_t rc = ORCA_ERR_INVALID_ARGUMENT;
    if (nargs >= 3) {
        const auto name = read_guest_string(caller, args[0].of.i32, args[1].of.i32);
        auto* parser = ::orca::placeholder_tls::current();
        if (parser && !name.empty()) {
            parser->set(name, static_cast<int>(args[2].of.i64));
            rc = ORCA_OK;
        }
    }
    if (nresults >= 1) {
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = rc;
    }
    return nullptr;
}

/// orca_placeholder_set_float(name_ptr,name_len, value: f64) -> i32
wasm_trap_t* host_placeholder_set_float(void* /*env*/, wasmtime_caller_t* caller,
                                        const wasmtime_val_t* args, std::size_t nargs,
                                        wasmtime_val_t* results, std::size_t nresults) {
    std::int32_t rc = ORCA_ERR_INVALID_ARGUMENT;
    if (nargs >= 3) {
        const auto name = read_guest_string(caller, args[0].of.i32, args[1].of.i32);
        auto* parser = ::orca::placeholder_tls::current();
        if (parser && !name.empty()) {
            parser->set(name, args[2].of.f64);
            rc = ORCA_OK;
        }
    }
    if (nresults >= 1) {
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = rc;
    }
    return nullptr;
}

/// orca_register_pipeline_observer(name_ptr, name_len) -> i64 (slot_id, 0 on fail)
/// Looks up the named export on the calling instance and registers it as
/// the on_step target for an ORCA_SLOT_PIPELINE_OBSERVER. Routes through
/// ImportContext::slot_sink which the WasmPlugin populated before
/// invoking orca_plugin_register on the guest.
wasm_trap_t* host_register_pipeline_observer(void* /*env*/, wasmtime_caller_t* caller,
                                             const wasmtime_val_t* args, std::size_t nargs,
                                             wasmtime_val_t* results, std::size_t nresults) {
    std::int64_t slot_id = 0;
    if (nargs >= 2) {
        const auto name = read_guest_string(caller, args[0].of.i32, args[1].of.i32);
        auto* ictx = import_ctx_from(caller);
        if (ictx && ictx->slot_sink && !name.empty()) {
            wasmtime_extern_t ext;
            if (wasmtime_caller_export_get(caller, name.data(), name.size(), &ext)) {
                if (ext.kind == WASMTIME_EXTERN_FUNC) {
                    slot_id = static_cast<std::int64_t>(
                        ictx->slot_sink->register_observer_from_wasm(
                            wasmtime_caller_context(caller),
                            ext.of.func));
                }
                wasmtime_extern_delete(&ext);
            }
        }
    }
    if (nresults >= 1) {
        results[0].kind = WASMTIME_I64;
        results[0].of.i64 = slot_id;
    }
    return nullptr;
}

/// orca_load_profile_pack(dir_ptr, dir_len) -> i32 (orca_error_code_t)
/// Permission-gated on ORCA_PERM_PROFILES_INSTALL.
wasm_trap_t* host_load_profile_pack(void* /*env*/, wasmtime_caller_t* caller,
                                    const wasmtime_val_t* args, std::size_t nargs,
                                    wasmtime_val_t* results, std::size_t nresults) {
    std::int32_t rc = ORCA_ERR_INVALID_ARGUMENT;
    auto* ictx = import_ctx_from(caller);
    if (!ictx || !(ictx->permissions & ORCA_PERM_PROFILES_INSTALL)) {
        rc = ORCA_ERR_PERMISSION_DENIED;
    } else if (nargs >= 2) {
        const auto dir = read_guest_string(caller, args[0].of.i32, args[1].of.i32);
        if (!dir.empty() && ictx->session) {
            auto load_res = ictx->session->presets().load_vendor_configs_from_json(
                dir, ::orca::SubstitutionRule::Disable);
            rc = load_res.ok() ? ORCA_OK : ORCA_ERR_IO;
        }
    }
    if (nresults >= 1) {
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = rc;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Linker setup
// ---------------------------------------------------------------------------

struct FuncTypeOwner {
    wasm_functype_t* ty = nullptr;
    ~FuncTypeOwner() { if (ty) wasm_functype_delete(ty); }
};

FuncTypeOwner make_functype(std::initializer_list<wasm_valkind_t> params,
                            std::initializer_list<wasm_valkind_t> results) {
    wasm_valtype_vec_t pv, rv;
    wasm_valtype_vec_new_uninitialized(&pv, params.size());
    wasm_valtype_vec_new_uninitialized(&rv, results.size());
    std::size_t i = 0;
    for (auto k : params)  pv.data[i++] = wasm_valtype_new(k);
    i = 0;
    for (auto k : results) rv.data[i++] = wasm_valtype_new(k);
    return FuncTypeOwner{wasm_functype_new(&pv, &rv)};
}

bool define_func(wasmtime_linker_t* linker,
                 const char* name,
                 const FuncTypeOwner& ty,
                 wasmtime_func_callback_t cb) {
    constexpr const char* kModule = "env";
    return wasmtime_linker_define_func(
               linker,
               kModule, std::strlen(kModule),
               name, std::strlen(name),
               ty.ty,
               cb,
               /*data*/      nullptr,
               /*finalizer*/ nullptr) == nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------
Result<void> install_imports(wasmtime_linker_t* linker,
                             wasm_engine_t* /*engine*/) {
    using R = Result<void>;
    if (!linker)
        return R{Error{ErrorCode::InvalidArgument,
                       "install_imports: linker is null"}};

    const auto t_log               = make_functype({WASM_I32, WASM_I32, WASM_I32}, {});
    const auto t_abort             = make_functype({WASM_I32, WASM_I32},           {});
    const auto t_check_permission  = make_functype({WASM_I64},                     {WASM_I32});
    const auto t_ph_set_string     = make_functype({WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I32});
    const auto t_ph_set_int        = make_functype({WASM_I32, WASM_I32, WASM_I64}, {WASM_I32});
    const auto t_ph_set_float      = make_functype({WASM_I32, WASM_I32, WASM_F64}, {WASM_I32});
    const auto t_load_profile_pack = make_functype({WASM_I32, WASM_I32},           {WASM_I32});
    const auto t_register_observer = make_functype({WASM_I32, WASM_I32},           {WASM_I64});

    if (!define_func(linker, "orca_log",                          t_log,               host_log)             ||
        !define_func(linker, "orca_abort",                        t_abort,             host_abort)           ||
        !define_func(linker, "orca_check_permission",             t_check_permission,  host_check_permission)||
        !define_func(linker, "orca_placeholder_set_string",       t_ph_set_string,     host_placeholder_set_string) ||
        !define_func(linker, "orca_placeholder_set_int",          t_ph_set_int,        host_placeholder_set_int)    ||
        !define_func(linker, "orca_placeholder_set_float",        t_ph_set_float,      host_placeholder_set_float)  ||
        !define_func(linker, "orca_load_profile_pack",            t_load_profile_pack, host_load_profile_pack)      ||
        !define_func(linker, "orca_register_pipeline_observer",   t_register_observer, host_register_pipeline_observer))
        return R{Error{ErrorCode::Unknown,
                       "wasmtime_linker_define_func failed for one of the orca_* imports"}};

    return ok();
}

} // namespace orca::wasm
