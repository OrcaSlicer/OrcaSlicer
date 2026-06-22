//! Check 7 — smoke fire (opt-in, --smoke).
//!
//! After the ABI check has confirmed the plugin loads, registers, and
//! unregisters cleanly, the --smoke pass dlopen's again and reloads the
//! plugin to assert the load → unload → load cycle is also clean. Plugins
//! that leak per-load state usually crash on the second load (TLS still
//! pointing at freed memory, static init guard tripping, etc.).
//!
//! For wasm we re-instantiate twice against the same Module — exercises
//! per-Instance state without touching disk again.
//!
//! Slot-specific synthetic payload dispatch isn't done here because the
//! slot vtables are opaque pointers; without the engine's full host
//! infrastructure we can't safely invoke them. The "real" smoke test is
//! the marketplace publish flow loading the plugin into a sandboxed engine
//! and walking a canary slice — that lives at the Phase 7.2 boundary and
//! reuses this binary's --smoke pass as a prerequisite.

use super::abi::RecordedState;
use super::layout::LayoutInfo;
use crate::report::CheckResult;

pub fn check(info: &LayoutInfo, _recorded: &RecordedState) -> CheckResult {
    if let Some(native) = &info.native_binary {
        return reload_native(native);
    }
    if let Some(wasm) = &info.wasm_binary {
        return reload_wasm(wasm);
    }
    CheckResult::skip("smoke", "no binary to smoke-test")
}

fn reload_native(path: &std::path::Path) -> CheckResult {
    #[cfg(target_os = "windows")]
    {
        return CheckResult::skip("smoke", "native smoke is not implemented on Windows yet");
    }

    for pass in 0..2 {
        unsafe {
            match libloading::Library::new(path) {
                Ok(_lib) => {
                    // Library drop runs dlclose; that's the load/unload
                    // cycle this pass is exercising. Calling register/
                    // unregister inside is duplicative with the ABI check.
                }
                Err(e) => {
                    return CheckResult::fail(
                        "smoke",
                        format!("native reload pass {pass} failed: {e}"),
                    );
                }
            }
        }
    }
    CheckResult::pass("smoke")
}

fn reload_wasm(path: &std::path::Path) -> CheckResult {
    use wasmtime::*;
    let engine = Engine::default();
    let module = match Module::from_file(&engine, path) {
        Ok(m) => m,
        Err(e) => return CheckResult::fail("smoke", format!("wasm Module::from_file failed: {e}")),
    };
    for pass in 0..2 {
        let mut store: Store<()> = Store::new(&engine, ());
        let mut linker: Linker<()> = Linker::new(&engine);
        if let Err(e) = super::abi::stub_wasm_imports(&mut linker) {
            return CheckResult::fail(
                "smoke",
                format!("wasm stub setup failed on pass {pass}: {e}"),
            );
        }
        if let Err(e) = linker.instantiate(&mut store, &module) {
            return CheckResult::fail(
                "smoke",
                format!("wasm reinstantiate pass {pass} failed: {e}"),
            );
        }
    }
    CheckResult::pass("smoke")
}
