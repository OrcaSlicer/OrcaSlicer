//! Typed config access on top of the host vtable's presets accessors.
//!
//! Phase 5.1 ships the structural surface — manifest, accessors,
//! permission handling. Per-key typed getters (the `orca::keys::*`
//! tags from ConfigKeys.hpp) bind in here as Phase 5.1.7 evolves
//! alongside the catalog regenerator.

use crate::abi;
use crate::ctx::current;
use crate::error::{Error, Result, check};

use core::ffi::CStr;
use std::ffi::CString;

/// Borrow a string preset value by key. Returns Ok(None) when the key
/// is absent on the active config; PermissionDenied when the plugin
/// did not declare ORCA_PERM_SETTINGS_READ.
pub fn get_string(key: &str) -> Result<Option<String>> {
    let ctx = current().ok_or(Error::InvalidArgument)?;
    let host = unsafe { &*ctx.host() };

    unsafe extern "C" {
        fn orca_presets_get_string(
            presets: *const abi::orca_presets_t,
            key: *const core::ffi::c_char,
            out: *mut *const core::ffi::c_char,
        ) -> i32;
    }

    let p = (host.presets_const)();
    if p.is_null() {
        return Err(Error::InvalidArgument);
    }
    let k = CString::new(key).map_err(|_| Error::InvalidArgument)?;
    let mut out: *const core::ffi::c_char = core::ptr::null();
    let rc = unsafe { orca_presets_get_string(p, k.as_ptr(), &mut out) };
    if rc == abi::ORCA_ERR_NOT_FOUND {
        return Ok(None);
    }
    check(rc)?;
    Ok(unsafe { crate::ctx::take_host_string(out) })
}

/// Borrow a float preset value by key. Same semantics as get_string.
pub fn get_float(key: &str) -> Result<Option<f64>> {
    let ctx = current().ok_or(Error::InvalidArgument)?;
    let host = unsafe { &*ctx.host() };

    unsafe extern "C" {
        fn orca_presets_get_float(
            presets: *const abi::orca_presets_t,
            key: *const core::ffi::c_char,
            out: *mut f64,
        ) -> i32;
    }

    let p = (host.presets_const)();
    if p.is_null() {
        return Err(Error::InvalidArgument);
    }
    let k = CString::new(key).map_err(|_| Error::InvalidArgument)?;
    let mut out: f64 = 0.0;
    let rc = unsafe { orca_presets_get_float(p, k.as_ptr(), &mut out) };
    if rc == abi::ORCA_ERR_NOT_FOUND {
        return Ok(None);
    }
    check(rc)?;
    Ok(Some(out))
}

/// Set a string preset value. Requires ORCA_PERM_SETTINGS_WRITE.
pub fn set_string(key: &str, value: &str) -> Result<()> {
    let ctx = current().ok_or(Error::InvalidArgument)?;
    let host = unsafe { &*ctx.host() };

    unsafe extern "C" {
        fn orca_presets_set_string(
            presets: *mut abi::orca_presets_t,
            key: *const core::ffi::c_char,
            value: *const core::ffi::c_char,
        ) -> i32;
    }

    let p = (host.presets_mut)();
    if p.is_null() {
        return Err(Error::PermissionDenied);
    }
    let k = CString::new(key).map_err(|_| Error::InvalidArgument)?;
    let v = CString::new(value).map_err(|_| Error::InvalidArgument)?;
    check(unsafe { orca_presets_set_string(p, k.as_ptr(), v.as_ptr()) })
}

/// Defensive consumer for the host's `string_free` pair — exported
/// so authors can write their own typed accessors without depending
/// on the private take_host_string helper.
///
/// # Safety
///
/// `s` must either be null or a pointer previously returned by one of
/// the host's `presets_get_*` accessors and not yet freed. After this
/// call the pointer must not be dereferenced again. The host's
/// allocator owns `s`; calling `free_host_string` on a pointer from a
/// different allocator is undefined behavior.
pub unsafe fn free_host_string(s: *const core::ffi::c_char) {
    if let Some(ctx) = current() {
        let host = unsafe { &*ctx.host() };
        (host.string_free)(s);
    }
    // Silence unused warning when the input was already null.
    let _ = unsafe { CStr::from_ptr(s) };
}
