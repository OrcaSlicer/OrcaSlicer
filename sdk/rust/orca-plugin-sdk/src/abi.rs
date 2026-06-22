//! Raw FFI bindings to `engine/include/orca/plugin_api.h`.
//!
//! Every type and function-pointer signature in this file mirrors a
//! corresponding declaration in the C header at the same ABI version.
//! When bumping `ORCA_PLUGIN_ABI_VERSION` on the C side, update this
//! file in lockstep — the compile-time `ABI_VERSION` constant exists
//! so guests refuse to register against a mismatched engine.

#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};

/// Compile-time-known ABI version; matches `ORCA_PLUGIN_ABI_VERSION`
/// in `engine/include/orca/plugin_api.h`. Bump in lockstep with the
/// header.
pub const ABI_VERSION: u32 = 1;

// ---------- Error codes (orca_error_code_t in c_api.h) -------------
pub const ORCA_OK: i32 = 0;
pub const ORCA_ERR_UNKNOWN: i32 = 1;
pub const ORCA_ERR_INVALID_ARGUMENT: i32 = 2;
pub const ORCA_ERR_NOT_FOUND: i32 = 3;
pub const ORCA_ERR_ALREADY_EXISTS: i32 = 4;
pub const ORCA_ERR_IO: i32 = 5;
pub const ORCA_ERR_PARSE: i32 = 6;
pub const ORCA_ERR_CANCELLED: i32 = 7;
pub const ORCA_ERR_UNSUPPORTED: i32 = 8;
pub const ORCA_ERR_PERMISSION_DENIED: i32 = 9;

// ---------- Slot kinds (orca_slot_kind_t) ---------------------------
pub const ORCA_SLOT_PIPELINE_OBSERVER: u32 = 0x0001;
pub const ORCA_SLOT_PIPELINE_INTERCEPTOR: u32 = 0x0002;
pub const ORCA_SLOT_GCODE_FILTER: u32 = 0x0003;
pub const ORCA_SLOT_PLACEHOLDER_PROVIDER: u32 = 0x0004;
pub const ORCA_SLOT_PRINTER_AGENT: u32 = 0x0010;
pub const ORCA_SLOT_PROFILE_PACK: u32 = 0x0020;
pub const ORCA_SLOT_SETTINGS_PAGE: u32 = 0x0100;
pub const ORCA_SLOT_DEVICE_TAB: u32 = 0x0101;
pub const ORCA_SLOT_MENU_COMMAND: u32 = 0x0102;
pub const ORCA_SLOT_SIDEBAR_PANEL: u32 = 0x0103;

// ---------- Permission bits -----------------------------------------
pub const ORCA_PERM_NETWORK: u64 = 1 << 0;
pub const ORCA_PERM_FILESYSTEM_READ: u64 = 1 << 1;
pub const ORCA_PERM_FILESYSTEM_WRITE: u64 = 1 << 2;
pub const ORCA_PERM_SETTINGS_READ: u64 = 1 << 3;
pub const ORCA_PERM_SETTINGS_WRITE: u64 = 1 << 4;
pub const ORCA_PERM_PROFILES_INSTALL: u64 = 1 << 5;
pub const ORCA_PERM_DEVICE_CONTROL: u64 = 1 << 6;
pub const ORCA_PERM_SLICE_INTERCEPT: u64 = 1 << 7;
pub const ORCA_PERM_GCODE_MODIFY: u64 = 1 << 8;
pub const ORCA_PERM_UI_ATTACH: u64 = 1 << 9;
pub const ORCA_PERM_EVENTS_RAW: u64 = 1 << 10;

// ---------- Pipeline step enum --------------------------------------
pub const ORCA_STEP_BEFORE_SLICE: u32 = 1;
pub const ORCA_STEP_AFTER_PERIMETERS: u32 = 2;
pub const ORCA_STEP_AFTER_INFILL: u32 = 3;
pub const ORCA_STEP_AFTER_IRONING: u32 = 4;
pub const ORCA_STEP_AFTER_SUPPORTS: u32 = 5;
pub const ORCA_STEP_BEFORE_WIPE_TOWER: u32 = 6;
pub const ORCA_STEP_AFTER_SKIRT_BRIM: u32 = 7;
pub const ORCA_STEP_BEFORE_GCODE_EXPORT: u32 = 8;
pub const ORCA_STEP_AFTER_GCODE_EXPORT: u32 = 9;

// ---------- Opaque host objects -------------------------------------
#[repr(C)]
pub struct orca_plugin_registry_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct orca_session_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct orca_presets_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct orca_events_t {
    _private: [u8; 0],
}

pub type orca_subscription_id_t = u64;
pub type orca_plugin_slot_id_t = u64;

// ---------- Manifest ------------------------------------------------
#[repr(C)]
pub struct orca_plugin_manifest_t {
    pub struct_size: u32,
    pub id: *const c_char,
    pub name: *const c_char,
    pub version: *const c_char,
    pub author: *const c_char,
    pub description: *const c_char,
    pub permissions: u64,
}

// ---------- Host vtable (extract — matches plugin_api.h §"host") ----
pub type orca_log_fn_t = extern "C" fn(level: c_int, msg: *const c_char);

pub type orca_event_callback_t = extern "C" fn(kind: u32, payload: *const c_void, ud: *mut c_void);

pub type orca_raw_event_callback_t =
    extern "C" fn(kind: u32, payload: *const c_void, payload_size: usize, ud: *mut c_void);

#[repr(C)]
pub struct orca_plugin_host_t {
    pub struct_size: u32,
    pub log: orca_log_fn_t,

    pub session: extern "C" fn() -> *mut orca_session_t,

    pub events_subscribe: extern "C" fn(
        kind: u32,
        cb: orca_event_callback_t,
        ud: *mut c_void,
    ) -> orca_subscription_id_t,
    pub events_unsubscribe: extern "C" fn(orca_subscription_id_t),

    pub events_subscribe_raw: extern "C" fn(
        kind_mask: u64,
        cb: orca_raw_event_callback_t,
        ud: *mut c_void,
    ) -> orca_subscription_id_t,

    pub presets_const: extern "C" fn() -> *const orca_presets_t,
    pub presets_mut: extern "C" fn() -> *mut orca_presets_t,

    pub load_profile_pack: extern "C" fn(dir: *const c_char) -> i32,

    pub placeholder_set_string: extern "C" fn(name: *const c_char, value: *const c_char) -> i32,
    pub placeholder_set_int: extern "C" fn(name: *const c_char, value: i64) -> i32,
    pub placeholder_set_float: extern "C" fn(name: *const c_char, value: f64) -> i32,

    pub register_printer_agent:
        extern "C" fn(agent_id: *const c_char, vtable: *const c_void, slf: *mut c_void) -> i32,

    pub string_free: extern "C" fn(s: *const c_char),
}

// ---------- Registry-side functions the engine fills in -------------
pub type orca_registry_set_manifest_fn =
    extern "C" fn(*mut orca_plugin_registry_t, *const orca_plugin_manifest_t) -> i32;

pub type orca_registry_add_slot_fn = extern "C" fn(
    *mut orca_plugin_registry_t,
    kind: u32,
    vtable: *const c_void,
    user_data: *mut c_void,
) -> orca_plugin_slot_id_t;
