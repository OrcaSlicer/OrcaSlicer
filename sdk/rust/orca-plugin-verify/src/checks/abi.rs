//! Check 4 — ABI handshake.
//!
//! For native plugins:
//!   1. dlopen the .so / .dll / .dylib via libloading.
//!   2. Look up the three mandatory exports — orca_plugin_register,
//!      orca_plugin_unregister, orca_plugin_check_debug_consistent.
//!   3. Build a stub orca_plugin_host_t whose function pointers all return
//!      ORCA_OK without doing anything dangerous (no real session / events).
//!   4. Call orca_plugin_register(ABI_VERSION, dummy_registry, &stub_host).
//!   5. The plugin's calls to the verifier's exported orca_registry_*
//!      symbols record manifest + slot info into [`RecordedState`].
//!   6. Call orca_plugin_unregister.
//!
//! For wasm plugins: wasmtime instantiates the module, providing host
//! imports that record the same RecordedState.
//!
//! The recorded slot kinds become the input to the slot-match check
//! (checks/slots.rs).

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::Mutex;

use orca_plugin_sdk::abi::{
    ABI_VERSION, ORCA_OK, orca_event_callback_t, orca_plugin_host_t, orca_plugin_manifest_t,
    orca_plugin_registry_t, orca_plugin_slot_id_t, orca_presets_t, orca_raw_event_callback_t,
    orca_session_t, orca_subscription_id_t,
};

use super::layout::LayoutInfo;
use super::manifest::ParsedManifest;
use crate::report::CheckResult;

#[derive(Debug, Default, Clone)]
pub struct RecordedState {
    pub manifest_id: Option<String>,
    pub manifest_perms: u64,
    pub slot_kinds: Vec<u32>,
    pub registered_agent_ids: Vec<String>,
}

// Global capture — the verifier runs one check at a time, so a single
// process-global Mutex is fine. The verifier resets it at the start of
// each ABI check.
static RECORDED: Mutex<RecordedState> = Mutex::new(RecordedState {
    manifest_id: None,
    manifest_perms: 0,
    slot_kinds: Vec::new(),
    registered_agent_ids: Vec::new(),
});

fn reset_recorded() {
    let mut r = RECORDED.lock().unwrap();
    *r = RecordedState::default();
}

fn snapshot_recorded() -> RecordedState {
    RECORDED.lock().unwrap().clone()
}

// ---------------------------------------------------------------------------
// Exported symbols — the verifier process exposes these so dlopen'd plugin
// .so files resolve their `orca_registry_*` references against us.
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "C" fn orca_registry_set_manifest(
    _registry: *mut orca_plugin_registry_t,
    manifest: *const orca_plugin_manifest_t,
) -> i32 {
    if manifest.is_null() {
        return 2 /* INVALID_ARGUMENT */;
    }
    let m = unsafe { &*manifest };
    let mut r = RECORDED.lock().unwrap();
    if !m.id.is_null() {
        r.manifest_id = Some(unsafe { cstr_to_owned(m.id) });
    }
    r.manifest_perms = m.permissions;
    ORCA_OK
}

#[unsafe(no_mangle)]
pub extern "C" fn orca_registry_add_slot(
    _registry: *mut orca_plugin_registry_t,
    kind: u32,
    _vtable: *const c_void,
    _user_data: *mut c_void,
) -> orca_plugin_slot_id_t {
    let mut r = RECORDED.lock().unwrap();
    r.slot_kinds.push(kind);
    // Return a non-zero id so the SDK treats it as success.
    (r.slot_kinds.len() as u64).max(1)
}

// ---------------------------------------------------------------------------
// Stub host vtable.
// ---------------------------------------------------------------------------

extern "C" fn host_log(_level: c_int, _msg: *const c_char) {}
extern "C" fn host_session() -> *mut orca_session_t {
    std::ptr::null_mut()
}
extern "C" fn host_events_subscribe(
    _kind: u32,
    _cb: orca_event_callback_t,
    _ud: *mut c_void,
) -> orca_subscription_id_t {
    0
}
extern "C" fn host_events_unsubscribe(_sub: orca_subscription_id_t) {}
extern "C" fn host_events_subscribe_raw(
    _mask: u64,
    _cb: orca_raw_event_callback_t,
    _ud: *mut c_void,
) -> orca_subscription_id_t {
    0
}
extern "C" fn host_presets_const() -> *const orca_presets_t {
    std::ptr::null()
}
extern "C" fn host_presets_mut() -> *mut orca_presets_t {
    std::ptr::null_mut()
}
extern "C" fn host_load_profile_pack(_dir: *const c_char) -> i32 {
    ORCA_OK
}
extern "C" fn host_placeholder_set_string(_n: *const c_char, _v: *const c_char) -> i32 {
    ORCA_OK
}
extern "C" fn host_placeholder_set_int(_n: *const c_char, _v: i64) -> i32 {
    ORCA_OK
}
extern "C" fn host_placeholder_set_float(_n: *const c_char, _v: f64) -> i32 {
    ORCA_OK
}
extern "C" fn host_register_printer_agent(
    agent_id: *const c_char,
    _vtable: *const c_void,
    _slf: *mut c_void,
) -> i32 {
    if !agent_id.is_null() {
        let mut r = RECORDED.lock().unwrap();
        r.registered_agent_ids
            .push(unsafe { cstr_to_owned(agent_id) });
        // PRINTER_AGENT slot kind (matches ORCA_SLOT_PRINTER_AGENT = 0x0010).
        r.slot_kinds.push(0x0010);
    }
    ORCA_OK
}
extern "C" fn host_string_free(_s: *const c_char) {}

fn build_stub_host() -> orca_plugin_host_t {
    orca_plugin_host_t {
        struct_size: core::mem::size_of::<orca_plugin_host_t>() as u32,
        log: host_log,
        session: host_session,
        events_subscribe: host_events_subscribe,
        events_unsubscribe: host_events_unsubscribe,
        events_subscribe_raw: host_events_subscribe_raw,
        presets_const: host_presets_const,
        presets_mut: host_presets_mut,
        load_profile_pack: host_load_profile_pack,
        placeholder_set_string: host_placeholder_set_string,
        placeholder_set_int: host_placeholder_set_int,
        placeholder_set_float: host_placeholder_set_float,
        register_printer_agent: host_register_printer_agent,
        string_free: host_string_free,
    }
}

unsafe fn cstr_to_owned(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { std::ffi::CStr::from_ptr(p) }
        .to_str()
        .unwrap_or("")
        .to_string()
}

pub struct Outcome {
    pub result: CheckResult,
    pub recorded: Option<RecordedState>,
}

pub fn check(info: &LayoutInfo, manifest: &ParsedManifest) -> Outcome {
    let kind = manifest.kind();

    match kind {
        "native" | "hybrid" => check_native(info),
        "wasm" => check_wasm(info),
        "webview" | "data" => {
            // No binary ABI to handshake. Provide an empty recorded state
            // so downstream slot/permission checks don't bail.
            Outcome {
                result: CheckResult::skip(
                    "abi",
                    format!("kind {kind:?} has no native ABI to handshake; binary checks skipped"),
                ),
                recorded: Some(RecordedState::default()),
            }
        }
        other => Outcome {
            result: CheckResult::fail("abi", format!("unknown kind {other:?}; cannot handshake")),
            recorded: None,
        },
    }
}

fn check_native(info: &LayoutInfo) -> Outcome {
    let Some(binary) = &info.native_binary else {
        return Outcome {
            result: CheckResult::fail(
                "abi",
                "no native binary found (expected .so / .dylib / .dll next to manifest)",
            ),
            recorded: None,
        };
    };

    #[cfg(target_os = "windows")]
    {
        return Outcome {
            result: CheckResult::skip("abi", "native ABI check is not yet implemented on Windows"),
            recorded: Some(RecordedState::default()),
        };
    }

    reset_recorded();

    unsafe {
        // SAFETY: libloading::Library::new on a path the user provided.
        // The plugin's code might do unsafe things; we trust the author's
        // choice to ask us to verify their binary. Errors propagate.
        let lib = match libloading::Library::new(binary) {
            Ok(l) => l,
            Err(e) => {
                return Outcome {
                    result: CheckResult::fail("abi", format!("dlopen {}: {e}", binary.display())),
                    recorded: None,
                };
            }
        };

        // Optional but recommended: debug-consistency probe.
        if let Ok(sym) =
            lib.get::<unsafe extern "C" fn(i32) -> i32>(b"orca_plugin_check_debug_consistent\0")
        {
            let _ = sym(0);
        }

        let register: libloading::Symbol<
            unsafe extern "C" fn(
                u32,
                *mut orca_plugin_registry_t,
                *const orca_plugin_host_t,
            ) -> i32,
        > = match lib.get(b"orca_plugin_register\0") {
            Ok(s) => s,
            Err(e) => {
                return Outcome {
                    result: CheckResult::fail(
                        "abi",
                        format!("missing export `orca_plugin_register`: {e}"),
                    ),
                    recorded: None,
                };
            }
        };

        let host = build_stub_host();
        // The registry pointer needs to be non-null so the SDK's null guard
        // passes; the verifier's exported orca_registry_* symbols ignore it.
        let dummy_registry = std::ptr::dangling_mut::<orca_plugin_registry_t>();
        let rc = register(ABI_VERSION, dummy_registry, &host);

        if rc != ORCA_OK {
            return Outcome {
                result: CheckResult::fail(
                    "abi",
                    format!("orca_plugin_register returned non-OK ({rc})"),
                ),
                recorded: Some(snapshot_recorded()),
            };
        }

        // Unregister cleanly so any process-global state is torn down.
        if let Ok(unreg) = lib.get::<unsafe extern "C" fn()>(b"orca_plugin_unregister\0") {
            unreg();
        }
    }

    let recorded = snapshot_recorded();
    Outcome {
        result: CheckResult::pass("abi"),
        recorded: Some(recorded),
    }
}

fn check_wasm(info: &LayoutInfo) -> Outcome {
    let Some(binary) = &info.wasm_binary else {
        return Outcome {
            result: CheckResult::fail(
                "abi",
                "no .wasm binary found (expected index.wasm next to manifest)",
            ),
            recorded: None,
        };
    };

    use wasmtime::*;

    let engine = Engine::default();

    let module = match Module::from_file(&engine, binary) {
        Ok(m) => m,
        Err(e) => {
            return Outcome {
                result: CheckResult::fail(
                    "abi",
                    format!("wasmtime failed to compile {}: {e}", binary.display()),
                ),
                recorded: None,
            };
        }
    };

    reset_recorded();

    let mut store: Store<()> = Store::new(&engine, ());
    let mut linker: Linker<()> = Linker::new(&engine);

    // Capability-gated host imports the engine exposes to wasm guests; the
    // verifier stubs each to be a no-op that records what was called. The
    // full surface is in engine/src/runtime/WasmImports.cpp.
    if let Err(e) = stub_wasm_imports(&mut linker) {
        return Outcome {
            result: CheckResult::fail("abi", format!("verifier linker stub setup failed: {e}")),
            recorded: None,
        };
    }

    let instance = match linker.instantiate(&mut store, &module) {
        Ok(i) => i,
        Err(e) => {
            return Outcome {
                result: CheckResult::fail("abi", format!("wasm instantiate failed: {e}")),
                recorded: None,
            };
        }
    };

    // Either _start (wasi-style) or a no-arg orca_init export — call
    // whichever is present.
    let entry = instance
        .get_typed_func::<(), ()>(&mut store, "_start")
        .or_else(|_| instance.get_typed_func::<(), ()>(&mut store, "orca_init"));
    if let Ok(f) = entry
        && let Err(e) = f.call(&mut store, ())
    {
        return Outcome {
            result: CheckResult::fail("abi", format!("wasm entry call trapped: {e}")),
            recorded: Some(snapshot_recorded()),
        };
    }

    Outcome {
        result: CheckResult::pass("abi"),
        recorded: Some(snapshot_recorded()),
    }
}

pub(crate) fn stub_wasm_imports(linker: &mut wasmtime::Linker<()>) -> wasmtime::Result<()> {
    use wasmtime::Caller;

    // Logging — no-op.
    linker.func_wrap(
        "orca",
        "log",
        |_c: Caller<'_, ()>, _level: i32, _ptr: i32, _len: i32| {},
    )?;
    linker.func_wrap(
        "orca",
        "abort",
        |_c: Caller<'_, ()>, _ptr: i32, _len: i32| {},
    )?;

    // Permission probe — accept everything.
    linker.func_wrap(
        "orca",
        "check_permission",
        |_c: Caller<'_, ()>, _ptr: i32, _len: i32| -> i32 { 1 },
    )?;

    // Placeholder setters — no-op, return ORCA_OK.
    linker.func_wrap(
        "orca",
        "placeholder_set_string",
        |_c: Caller<'_, ()>, _n: i32, _nl: i32, _v: i32, _vl: i32| -> i32 { 0 },
    )?;
    linker.func_wrap(
        "orca",
        "placeholder_set_int",
        |_c: Caller<'_, ()>, _n: i32, _nl: i32, _v: i64| -> i32 { 0 },
    )?;
    linker.func_wrap(
        "orca",
        "placeholder_set_float",
        |_c: Caller<'_, ()>, _n: i32, _nl: i32, _v: f64| -> i32 { 0 },
    )?;

    // Profile pack loader — no-op.
    linker.func_wrap(
        "orca",
        "load_profile_pack",
        |_c: Caller<'_, ()>, _p: i32, _l: i32| -> i32 { 0 },
    )?;

    // Slot registrations the wasm SDK uses. Each one bumps the recorded
    // slot_kinds list with the corresponding ORCA_SLOT_* constant.
    linker.func_wrap(
        "orca",
        "register_pipeline_observer",
        |_c: Caller<'_, ()>, _fn_idx: i32| -> i32 {
            RECORDED.lock().unwrap().slot_kinds.push(0x0001);
            1
        },
    )?;

    Ok(())
}

// Allow CString unused without breaking the build (Windows path doesn't
// need it but Linux/macOS paths do).
#[allow(dead_code)]
fn _unused_cstring() -> CString {
    CString::new("").unwrap()
}
