//! Check 1 — layout.
//!
//! Asserts manifest.json exists at the plugin root, then locates the binary
//! / wasm / ui entry that the rest of the pipeline needs to inspect. Does
//! NOT parse manifest content — that's `manifest.rs`. Layout is intentionally
//! permissive: we accept BOTH the new (manifest.json + <id>.{so,wasm}) and
//! legacy (plugin.json + <name>.{so,wasm}) staging variants so plugin authors
//! who haven't migrated yet still get useful diagnostics.

use std::path::{Path, PathBuf};

use crate::report::CheckResult;

#[derive(Debug, Clone)]
pub struct LayoutInfo {
    pub manifest_path: PathBuf,
    pub schema_path: Option<PathBuf>,
    pub native_binary: Option<PathBuf>,
    pub wasm_binary: Option<PathBuf>,
}

pub struct Outcome {
    pub result: CheckResult,
    pub info: Option<LayoutInfo>,
}

pub fn check(root: &Path) -> Outcome {
    // 1. manifest.json — required. plugin.json is accepted as a legacy alias
    // (the plugin Makefile stages plugin.json renamed to manifest.json,
    // but author scaffolds at HEAD still write plugin.json).
    let manifest_path = find_one(root, &["manifest.json", "plugin.json"]);
    let Some(manifest_path) = manifest_path else {
        return Outcome {
            result: CheckResult::fail(
                "layout",
                format!("no manifest.json (or plugin.json) at {}", root.display()),
            ),
            info: None,
        };
    };

    let schema_path = find_one(root, &["settings.schema.json"]);

    // Binaries — accept any of the platform shapes. We're not picky about
    // which platform this binary is FOR (cross-platform .orcaplugin zips
    // contain all three); the ABI check picks the one matching the verifier
    // host platform.
    let native_binary = find_native_binary(root);
    let wasm_binary = find_one(root, &["index.wasm"]).or_else(|| find_with_ext(root, "wasm"));

    let info = LayoutInfo {
        manifest_path,
        schema_path,
        native_binary,
        wasm_binary,
    };

    // It's OK to have none of the three (a `kind=data` profile pack is just a
    // manifest + files), so the layout check itself passes here. The ABI
    // check is what enforces a binary exists for native/wasm/hybrid kinds.
    Outcome {
        result: CheckResult::pass("layout"),
        info: Some(info),
    }
}

fn find_one(root: &Path, names: &[&str]) -> Option<PathBuf> {
    for n in names {
        let p = root.join(n);
        if p.exists() {
            return Some(p);
        }
    }
    None
}

fn find_with_ext(root: &Path, ext: &str) -> Option<PathBuf> {
    let dir = std::fs::read_dir(root).ok()?;
    for entry in dir.flatten() {
        let p = entry.path();
        if p.extension().map(|e| e == ext).unwrap_or(false) {
            return Some(p);
        }
    }
    None
}

fn find_native_binary(root: &Path) -> Option<PathBuf> {
    // Linux: .so. macOS: .dylib. Windows: .dll. Take whichever exists; if
    // multiple are present (cross-platform pack), prefer the one matching
    // the verifier's own host so the dlopen check exercises the real
    // platform path.
    #[cfg(target_os = "linux")]
    let prefer = "so";
    #[cfg(target_os = "macos")]
    let prefer = "dylib";
    #[cfg(target_os = "windows")]
    let prefer = "dll";
    #[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
    let prefer = "so";

    if let Some(p) = find_with_ext(root, prefer) {
        return Some(p);
    }
    for ext in &["so", "dylib", "dll"] {
        if let Some(p) = find_with_ext(root, ext) {
            return Some(p);
        }
    }
    None
}
