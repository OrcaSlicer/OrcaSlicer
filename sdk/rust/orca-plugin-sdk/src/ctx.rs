//! Ctx — the per-plugin handle through which guest code reaches host
//! services. Stored as a thread-safe singleton populated by
//! `orca_plugin_register` (via `export_plugin!`) and cleared by
//! `orca_plugin_unregister`.

use crate::abi;
use crate::error::Result;

use core::ffi::CStr;
use core::ptr::null_mut;
use core::sync::atomic::{AtomicPtr, Ordering};
use std::ffi::CString;

/// Per-plugin context — wraps the engine's `orca_plugin_host_t`.
/// All host calls go through Ctx so guest code never sees raw FFI.
#[derive(Copy, Clone)]
pub struct Ctx {
    host: *const abi::orca_plugin_host_t,
}

unsafe impl Send for Ctx {}
unsafe impl Sync for Ctx {}

#[repr(i32)]
#[derive(Copy, Clone, Debug)]
pub enum LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

impl Ctx {
    pub fn log(&self, level: LogLevel, msg: &str) {
        let host = unsafe { &*self.host };
        if let Ok(c) = CString::new(msg) {
            (host.log)(level as i32, c.as_ptr());
        }
    }

    pub fn log_info(&self, msg: &str) {
        self.log(LogLevel::Info, msg);
    }
    pub fn log_warn(&self, msg: &str) {
        self.log(LogLevel::Warn, msg);
    }
    pub fn log_error(&self, msg: &str) {
        self.log(LogLevel::Error, msg);
    }

    pub fn placeholder_set_string(&self, name: &str, value: &str) -> Result<()> {
        let host = unsafe { &*self.host };
        let n = CString::new(name).map_err(|_| crate::Error::InvalidArgument)?;
        let v = CString::new(value).map_err(|_| crate::Error::InvalidArgument)?;
        crate::error::check((host.placeholder_set_string)(n.as_ptr(), v.as_ptr()))
    }

    pub fn placeholder_set_int(&self, name: &str, value: i64) -> Result<()> {
        let host = unsafe { &*self.host };
        let n = CString::new(name).map_err(|_| crate::Error::InvalidArgument)?;
        crate::error::check((host.placeholder_set_int)(n.as_ptr(), value))
    }

    pub fn placeholder_set_float(&self, name: &str, value: f64) -> Result<()> {
        let host = unsafe { &*self.host };
        let n = CString::new(name).map_err(|_| crate::Error::InvalidArgument)?;
        crate::error::check((host.placeholder_set_float)(n.as_ptr(), value))
    }

    pub fn load_profile_pack(&self, dir: &str) -> Result<()> {
        let host = unsafe { &*self.host };
        let d = CString::new(dir).map_err(|_| crate::Error::InvalidArgument)?;
        crate::error::check((host.load_profile_pack)(d.as_ptr()))
    }

    /// Raw host pointer — escape hatch for advanced consumers
    /// (the events / presets / device modules use this).
    pub(crate) fn host(&self) -> *const abi::orca_plugin_host_t {
        self.host
    }
}

// --- Singleton storage ------------------------------------------------
//
// One Ctx per process, since one plugin .so is loaded once. Stored as
// AtomicPtr so the export_plugin! macro can populate it from
// orca_plugin_register and the with_host helper can read it from any
// thread without UB.

static HOST: AtomicPtr<abi::orca_plugin_host_t> = AtomicPtr::new(null_mut());

#[doc(hidden)]
pub fn set_host(p: *const abi::orca_plugin_host_t) {
    HOST.store(p as *mut _, Ordering::Release);
}

#[doc(hidden)]
pub fn clear_host() {
    HOST.store(null_mut(), Ordering::Release);
}

/// Run `f` with the current Ctx, if any. Returns ORCA_ERR_INVALID_ARGUMENT
/// when the host has not been bound yet.
pub fn with_host<R>(f: impl FnOnce(&Ctx) -> R) -> R
where
    R: From<crate::Error>,
{
    let p = HOST.load(Ordering::Acquire) as *const abi::orca_plugin_host_t;
    if p.is_null() {
        return crate::Error::InvalidArgument.into();
    }
    let ctx = Ctx { host: p };
    f(&ctx)
}

// Allow the macro and integrations to skip the conversion bound when
// the closure already returns a Result.
impl From<crate::Error> for Result<()> {
    fn from(e: crate::Error) -> Self {
        Err(e)
    }
}

/// Borrow-style variant for callers that just want an Option<&Ctx>.
pub fn current() -> Option<Ctx> {
    let p = HOST.load(Ordering::Acquire) as *const abi::orca_plugin_host_t;
    if p.is_null() {
        None
    } else {
        Some(Ctx { host: p })
    }
}

/// Read a NUL-terminated C string handed back by the host. Frees the
/// buffer via host->string_free; safe to call only on pointers the host
/// allocated.
pub(crate) unsafe fn take_host_string(s: *const core::ffi::c_char) -> Option<String> {
    if s.is_null() {
        return None;
    }
    let owned = unsafe { CStr::from_ptr(s).to_string_lossy().into_owned() };
    if let Some(ctx) = current() {
        let host = unsafe { &*ctx.host() };
        (host.string_free)(s);
    }
    Some(owned)
}
