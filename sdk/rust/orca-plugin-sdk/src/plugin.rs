//! Plugin trait + lifecycle helpers.
//!
//! A typical guest implements this trait + calls the [`export_plugin!`]
//! macro to generate the three mandatory C exports. The proc-macro
//! crate in Phase 5.2 builds on top of this; until that lands the
//! declarative macro here covers the same surface.

use crate::abi;
use crate::ctx::Ctx;
use crate::error::Result;

/// What a plugin self-describes as. Constants — no per-instance state.
pub trait Plugin: Sized + 'static {
    /// Reverse-DNS identifier matching the directory name under
    /// `<data_dir>/plugins/<id>/`.
    const ID: &'static str;
    /// Semver string, e.g. "0.1.0".
    const VERSION: &'static str;

    /// Human-readable name shown in the Plugin Manager dialog.
    const NAME: &'static str = Self::ID;
    const AUTHOR: &'static str = "";
    const DESCRIPTION: &'static str = "";

    /// ORCA_PERM_* bits the plugin declares it needs. Display-only for
    /// native plugins; ENFORCED for WASM plugins by the import gates.
    const PERMISSIONS: u64 = 0;

    /// Called once on load, after the engine validates the ABI handshake.
    /// `ctx` lets the plugin reach host services (logging, events,
    /// presets, slot registration).
    fn register(ctx: &Ctx) -> Result<()>;

    /// Called once just before the engine drops the plugin. Default
    /// implementation does nothing; override to release per-instance
    /// state, unsubscribe events, etc.
    fn unregister() {}
}

/// Build a struct_size-correct manifest from a Plugin implementation
/// and pass it to `orca_registry_set_manifest`. Plugin authors usually
/// reach this through the export_plugin! macro instead of calling it
/// directly.
///
/// # Safety
///
/// `registry` must be a valid, non-null pointer to an
/// `orca_plugin_registry_t` owned by the engine and still live for the
/// duration of this call. In practice this is the registry pointer the
/// engine hands to `orca_plugin_register`; callers must not forge or
/// outlive that pointer.
pub unsafe fn write_manifest<P: Plugin>(registry: *mut abi::orca_plugin_registry_t) -> Result<()> {
    use crate::error::Error;
    use core::mem::size_of;
    use std::ffi::CString;

    // CStrings live until end of scope, which is after the registry
    // call returns — pointers stay valid throughout.
    let id = CString::new(P::ID).map_err(|_| Error::InvalidArgument)?;
    let name = CString::new(P::NAME).map_err(|_| Error::InvalidArgument)?;
    let ver = CString::new(P::VERSION).map_err(|_| Error::InvalidArgument)?;
    let auth = CString::new(P::AUTHOR).map_err(|_| Error::InvalidArgument)?;
    let desc = CString::new(P::DESCRIPTION).map_err(|_| Error::InvalidArgument)?;

    let manifest = abi::orca_plugin_manifest_t {
        struct_size: size_of::<abi::orca_plugin_manifest_t>() as u32,
        id: id.as_ptr(),
        name: name.as_ptr(),
        version: ver.as_ptr(),
        author: auth.as_ptr(),
        description: desc.as_ptr(),
        permissions: P::PERMISSIONS,
    };

    // The C API lives in the engine; the symbol resolves at link time
    // for native cdylib plugins via the dynamic-symbol table the
    // engine process exports.
    unsafe extern "C" {
        fn orca_registry_set_manifest(
            r: *mut abi::orca_plugin_registry_t,
            m: *const abi::orca_plugin_manifest_t,
        ) -> i32;
    }

    crate::error::check(unsafe { orca_registry_set_manifest(registry, &manifest) })
}

/// Boilerplate emitter for the three mandatory plugin exports. The
/// proc-macro crate (Phase 5.2) wraps this so authors write
/// `#[orca::plugin] struct MyPlugin;` instead.
///
/// ```ignore
/// struct MyPlugin;
/// impl orca_plugin_sdk::Plugin for MyPlugin {
///     const ID: &'static str = "com.example.demo";
///     const VERSION: &'static str = "0.1.0";
///     fn register(ctx: &orca_plugin_sdk::Ctx) -> orca_plugin_sdk::Result<()> {
///         ctx.log_info("hello");
///         Ok(())
///     }
/// }
/// orca_plugin_sdk::export_plugin!(MyPlugin);
/// ```
#[macro_export]
macro_rules! export_plugin {
    ($plugin:ty) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn orca_plugin_check_debug_consistent(_is_debug: i32) -> i32 {
            // Rust cdylib plugins use a single ABI regardless of the
            // engine's debug/release status — Cargo profiles don't
            // change the wire format.
            0
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn orca_plugin_register(
            abi_version: u32,
            registry: *mut $crate::abi::orca_plugin_registry_t,
            host: *const $crate::abi::orca_plugin_host_t,
        ) -> i32 {
            if abi_version != $crate::abi::ABI_VERSION {
                return $crate::abi::ORCA_ERR_UNSUPPORTED;
            }
            // Stash the host pointer for the Ctx singleton.
            // SAFETY: host is engine-owned, valid until unregister.
            $crate::ctx::set_host(host);

            // SAFETY: `registry` is the engine-owned pointer the host
            // just handed us in `orca_plugin_register`; it stays valid
            // for the duration of this call.
            if let Err(e) = unsafe { $crate::plugin::write_manifest::<$plugin>(registry) } {
                return e.as_code();
            }
            $crate::ctx::with_host(|ctx| <$plugin as $crate::Plugin>::register(ctx))
                .map(|_| $crate::abi::ORCA_OK)
                .unwrap_or_else(|e| e.as_code())
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn orca_plugin_unregister() {
            <$plugin as $crate::Plugin>::unregister();
            $crate::ctx::clear_host();
        }
    };
}
