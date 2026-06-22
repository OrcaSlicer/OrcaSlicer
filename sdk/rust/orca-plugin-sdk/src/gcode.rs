//! GcodePostprocessor — typed wrapper for ORCA_SLOT_GCODE_FILTER.
//!
//! Plugins implementing this trait receive every chunk of generated
//! G-code right after `Print::export_gcode` and BEFORE any
//! `gcode_post_process_script`. The output is what the next stage of
//! the pipeline sees, so a noop implementation just returns its input
//! unchanged.
//!
//! Permission: ORCA_PERM_GCODE_MODIFY.

use crate::abi;
use crate::ctx::current;
use crate::error::{Error, Result};

use core::ffi::c_void;
use std::ffi::CString;

pub trait GcodePostprocessor: Send + Sync + 'static {
    /// Called once per generated G-code file. `path` is the temporary
    /// file the engine just wrote; the implementation may read it,
    /// rewrite it in place, or replace it entirely. Returning Err
    /// aborts the export.
    fn filter(&self, path: &std::path::Path) -> Result<()>;
}

/// Register a GcodePostprocessor with the engine's slot registry.
/// The processor lives for the rest of the plugin's lifetime; there
/// is no unregister hook because slot lifetime mirrors plugin lifetime
/// (PluginRegistry::remove_by_plugin scrubs everything on unload).
///
/// # Safety
///
/// `registry` must be a valid, non-null pointer to an
/// `orca_plugin_registry_t` owned by the engine and still live for the
/// duration of this call. In practice this is the registry pointer the
/// engine hands to `orca_plugin_register`; callers must not forge or
/// outlive that pointer.
pub unsafe fn register<P: GcodePostprocessor>(
    registry: *mut abi::orca_plugin_registry_t,
    processor: P,
) -> Result<u64> {
    unsafe extern "C" {
        fn orca_registry_add_slot(
            r: *mut abi::orca_plugin_registry_t,
            kind: u32,
            vtable: *const c_void,
            user_data: *mut c_void,
        ) -> u64;
    }

    let _ = current().ok_or(Error::InvalidArgument)?;

    // Build a static-lifetime vtable struct + a heap-leaked Box for the
    // processor instance. The Box leaks intentionally because the
    // engine never returns user_data to us (matches the symmetric
    // pattern used by events::subscribe).
    let processor: Box<dyn GcodePostprocessor> = Box::new(processor);
    let processor_ptr = Box::into_raw(Box::new(processor)) as *mut c_void;

    #[repr(C)]
    struct GcodeFilterVtable {
        struct_size: u32,
        filter: extern "C" fn(path: *const core::ffi::c_char, handle: u64, ud: *mut c_void) -> i32,
    }

    extern "C" fn thunk(path: *const core::ffi::c_char, _handle: u64, ud: *mut c_void) -> i32 {
        if path.is_null() || ud.is_null() {
            return abi::ORCA_ERR_INVALID_ARGUMENT;
        }
        let proc_ref = unsafe { &*(ud as *const Box<dyn GcodePostprocessor>) };
        let path_str = match unsafe { core::ffi::CStr::from_ptr(path).to_str() } {
            Ok(s) => s,
            Err(_) => return abi::ORCA_ERR_INVALID_ARGUMENT,
        };
        match proc_ref.filter(std::path::Path::new(path_str)) {
            Ok(()) => abi::ORCA_OK,
            Err(e) => e.as_code(),
        }
    }

    // Leak the vtable too so its lifetime spans the plugin's life.
    let vtable: &'static GcodeFilterVtable = Box::leak(Box::new(GcodeFilterVtable {
        struct_size: core::mem::size_of::<GcodeFilterVtable>() as u32,
        filter: thunk,
    }));

    let slot_id = unsafe {
        orca_registry_add_slot(
            registry,
            abi::ORCA_SLOT_GCODE_FILTER,
            vtable as *const _ as *const c_void,
            processor_ptr,
        )
    };
    if slot_id == 0 {
        Err(Error::Unknown)
    } else {
        Ok(slot_id)
    }
}

/// Convenience: turn a path into a CString, useful for postprocessors
/// that shell out and need the path in C-string form.
pub fn path_to_cstring(p: &std::path::Path) -> Option<CString> {
    let s = p.to_str()?;
    CString::new(s).ok()
}
