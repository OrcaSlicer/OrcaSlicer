//! Check 6 — permission honesty.
//!
//! Tries to detect plugins that under-declare their permissions. The check
//! looks DIFFERENT per kind:
//!
//!   - native: walk the binary's dynamic-symbol references with `goblin` and
//!     look for well-known "this means you need X permission" markers
//!     (libcurl / openssl → network; libdl → filesystem; etc.). Best-effort —
//!     warns on suspicious hits but never fails the check, because static
//!     symbol presence ≠ runtime use.
//!
//!   - wasm: parse the imports section with `wasmparser`. Wasm guests can
//!     only do what they import, so this is a HARD check. Every imported
//!     orca_* function maps to a permission bit; if the binary imports a
//!     function whose required permission isn't declared, FAIL.
//!
//! Either way, every recorded slot kind also implies a permission:
//! ORCA_SLOT_PRINTER_AGENT → device_control, ORCA_SLOT_SETTINGS_PAGE /
//! SIDEBAR_PANEL / MENU_COMMAND / DEVICE_TAB → ui_attach, etc. The check
//! flags missing slot-implied permissions as a FAIL.

use orca_plugin_sdk::abi;

use super::abi::RecordedState;
use super::layout::LayoutInfo;
use super::manifest::ParsedManifest;
use crate::report::CheckResult;

pub fn check(
    info: &LayoutInfo,
    manifest: &ParsedManifest,
    recorded: &RecordedState,
) -> CheckResult {
    let declared = perms_bits_from_strings(&manifest.permissions);
    let mut required: u64 = 0;
    let mut warnings = Vec::<String>::new();

    // Slot-implied permissions — applies regardless of kind.
    for &k in &recorded.slot_kinds {
        if let Some((bit, name)) = slot_implies(k) {
            if declared & bit == 0 {
                warnings.push(format!(
                    "slot kind 0x{k:04x} requires permission {name:?}, not declared"
                ));
            }
            required |= bit;
        }
    }

    // Wasm hard check.
    if manifest.kind() == "wasm"
        && let Some(wasm) = &info.wasm_binary
        && let Err(e) = check_wasm_imports(wasm, declared, &mut required, &mut warnings)
    {
        return CheckResult::warn("permissions", format!("wasm import scan failed: {e}"));
    }

    // Native soft check.
    if matches!(manifest.kind(), "native" | "hybrid")
        && let Some(native) = &info.native_binary
    {
        scan_native(native, declared, &mut warnings);
    }

    let missing = required & !declared;
    if missing != 0 {
        return CheckResult::fail(
            "permissions",
            format!(
                "missing permissions: 0x{missing:x} ({}); declared: 0x{declared:x}{}",
                bits_to_names(missing),
                if warnings.is_empty() {
                    String::new()
                } else {
                    format!("\n{}", warnings.join("\n"))
                }
            ),
        );
    }

    if warnings.is_empty() {
        CheckResult::pass("permissions")
    } else {
        CheckResult::warn("permissions", warnings.join("\n"))
    }
}

fn slot_implies(kind: u32) -> Option<(u64, &'static str)> {
    Some(match kind {
        abi::ORCA_SLOT_PIPELINE_INTERCEPTOR => (abi::ORCA_PERM_SLICE_INTERCEPT, "slice_intercept"),
        abi::ORCA_SLOT_GCODE_FILTER => (abi::ORCA_PERM_GCODE_MODIFY, "gcode_modify"),
        abi::ORCA_SLOT_PRINTER_AGENT => (abi::ORCA_PERM_DEVICE_CONTROL, "device_control"),
        abi::ORCA_SLOT_PROFILE_PACK => (abi::ORCA_PERM_PROFILES_INSTALL, "profiles_install"),
        abi::ORCA_SLOT_SETTINGS_PAGE
        | abi::ORCA_SLOT_DEVICE_TAB
        | abi::ORCA_SLOT_MENU_COMMAND
        | abi::ORCA_SLOT_SIDEBAR_PANEL => (abi::ORCA_PERM_UI_ATTACH, "ui_attach"),
        _ => return None,
    })
}

fn perms_bits_from_strings(perms: &[String]) -> u64 {
    let mut bits = 0u64;
    for p in perms {
        bits |= match p.as_str() {
            "network" => abi::ORCA_PERM_NETWORK,
            "filesystem_read" => abi::ORCA_PERM_FILESYSTEM_READ,
            "filesystem_write" => abi::ORCA_PERM_FILESYSTEM_WRITE,
            "settings_read" => abi::ORCA_PERM_SETTINGS_READ,
            "settings_write" => abi::ORCA_PERM_SETTINGS_WRITE,
            "profiles_install" => abi::ORCA_PERM_PROFILES_INSTALL,
            "device_control" => abi::ORCA_PERM_DEVICE_CONTROL,
            "slice_intercept" => abi::ORCA_PERM_SLICE_INTERCEPT,
            "gcode_modify" => abi::ORCA_PERM_GCODE_MODIFY,
            "ui_attach" => abi::ORCA_PERM_UI_ATTACH,
            "events_raw" => abi::ORCA_PERM_EVENTS_RAW,
            _ => 0,
        };
    }
    bits
}

fn bits_to_names(bits: u64) -> String {
    let mut out = Vec::<&str>::new();
    if bits & abi::ORCA_PERM_NETWORK != 0 {
        out.push("network");
    }
    if bits & abi::ORCA_PERM_FILESYSTEM_READ != 0 {
        out.push("filesystem_read");
    }
    if bits & abi::ORCA_PERM_FILESYSTEM_WRITE != 0 {
        out.push("filesystem_write");
    }
    if bits & abi::ORCA_PERM_SETTINGS_READ != 0 {
        out.push("settings_read");
    }
    if bits & abi::ORCA_PERM_SETTINGS_WRITE != 0 {
        out.push("settings_write");
    }
    if bits & abi::ORCA_PERM_PROFILES_INSTALL != 0 {
        out.push("profiles_install");
    }
    if bits & abi::ORCA_PERM_DEVICE_CONTROL != 0 {
        out.push("device_control");
    }
    if bits & abi::ORCA_PERM_SLICE_INTERCEPT != 0 {
        out.push("slice_intercept");
    }
    if bits & abi::ORCA_PERM_GCODE_MODIFY != 0 {
        out.push("gcode_modify");
    }
    if bits & abi::ORCA_PERM_UI_ATTACH != 0 {
        out.push("ui_attach");
    }
    if bits & abi::ORCA_PERM_EVENTS_RAW != 0 {
        out.push("events_raw");
    }
    out.join(",")
}

fn check_wasm_imports(
    path: &std::path::Path,
    declared: u64,
    required: &mut u64,
    warnings: &mut Vec<String>,
) -> std::io::Result<()> {
    let bytes = std::fs::read(path)?;
    let parser = wasmparser::Parser::new(0);
    for payload in parser.parse_all(&bytes) {
        let payload =
            payload.map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
        if let wasmparser::Payload::ImportSection(reader) = payload {
            // wasmparser 0.251 wraps each section entry in an `Imports`
            // enum that supports the compact-imports proposal. Flatten
            // back to individual `Import` records via `into_imports`.
            for import in reader.into_imports() {
                let import =
                    import.map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
                if import.module != "orca" {
                    continue;
                }
                if let Some((bit, perm_name)) = wasm_import_to_perm(import.name) {
                    *required |= bit;
                    if declared & bit == 0 {
                        warnings.push(format!(
                            "wasm import orca.{} requires permission {perm_name:?}",
                            import.name
                        ));
                    }
                }
            }
        }
    }
    Ok(())
}

fn wasm_import_to_perm(name: &str) -> Option<(u64, &'static str)> {
    Some(match name {
        // Network access via host HTTP imports (engine will gate at runtime).
        "http_get" | "http_post" | "http_send" => (abi::ORCA_PERM_NETWORK, "network"),
        // Filesystem.
        "fs_read" | "fs_open_read" => (abi::ORCA_PERM_FILESYSTEM_READ, "filesystem_read"),
        "fs_write" | "fs_open_write" => (abi::ORCA_PERM_FILESYSTEM_WRITE, "filesystem_write"),
        // Profile pack installation.
        "load_profile_pack" => (abi::ORCA_PERM_PROFILES_INSTALL, "profiles_install"),
        // Raw event subscription.
        "events_subscribe_raw" => (abi::ORCA_PERM_EVENTS_RAW, "events_raw"),
        _ => return None,
    })
}

fn scan_native(path: &std::path::Path, _declared: u64, warnings: &mut Vec<String>) {
    let Ok(bytes) = std::fs::read(path) else {
        return;
    };
    let Ok(obj) = goblin::Object::parse(&bytes) else {
        return;
    };

    let mut dyn_libs = Vec::<String>::new();
    match obj {
        goblin::Object::Elf(e) => {
            for lib in &e.libraries {
                dyn_libs.push((*lib).to_string());
            }
        }
        goblin::Object::Mach(goblin::mach::Mach::Binary(m)) => {
            for lib in m.libs.iter() {
                dyn_libs.push((*lib).to_string());
            }
        }
        goblin::Object::PE(pe) => {
            for imp in &pe.imports {
                dyn_libs.push(imp.dll.to_string());
            }
        }
        _ => {}
    }

    for lib in &dyn_libs {
        let l = lib.to_lowercase();
        if l.contains("curl") || l.contains("ssl") || l.contains("crypto") || l.contains("nghttp") {
            warnings.push(format!(
                "native binary links {lib:?} which usually implies network access; \
                 declare 'network' permission if you do HTTP/HTTPS"
            ));
        }
    }
}
