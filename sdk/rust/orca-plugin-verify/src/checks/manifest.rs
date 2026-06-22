//! Check 2 — manifest.json schema.
//!
//! Validates the manifest against the engine's expected shape:
//!   - `id`        — reverse-DNS-ish, regex `^[a-z0-9][a-z0-9.\-_]*\.[a-z0-9.\-_]+$`
//!   - `version`   — valid semver (parsed with the `semver` crate)
//!   - `kind`      — optional; defaults to "native"; one of {native, wasm,
//!     webview, data, hybrid}
//!   - `permissions[]` — strings, each must be in the known set declared in
//!     orca-plugin-sdk.
//!
//! Fields not validated by this pass (delegated to ABI check):
//!   - whether the declared binary actually exists on disk
//!   - whether the declared permissions are actually used by the plugin

use std::path::Path;

use serde::Deserialize;

use crate::report::CheckResult;

/// All permission strings the engine recognizes. Mirror of the table in
/// `sdk/rust/orca-plugin-macros/src/lib.rs::permissions_to_bits`.
pub const KNOWN_PERMISSIONS: &[&str] = &[
    "network",
    "filesystem_read",
    "filesystem_write",
    "settings_read",
    "settings_write",
    "profiles_install",
    "device_control",
    "slice_intercept",
    "gcode_modify",
    "ui_attach",
    "events_raw",
];

const KNOWN_KINDS: &[&str] = &["native", "wasm", "webview", "data", "hybrid"];

#[derive(Debug, Clone, Deserialize)]
pub struct ParsedManifest {
    pub id: String,
    pub version: String,
    #[serde(default)]
    pub kind: Option<String>,
    #[serde(default)]
    pub permissions: Vec<String>,
    /// Slot kinds the plugin declares it will register. Engine doesn't
    /// require this list at load-time — but the verifier uses it to detect
    /// drift between what the manifest claims and what the binary actually
    /// registers via the C ABI.
    #[serde(default)]
    pub provides: Vec<String>,
}

impl ParsedManifest {
    pub fn kind(&self) -> &str {
        self.kind.as_deref().unwrap_or("native")
    }
}

pub struct Outcome {
    pub result: CheckResult,
    pub parsed: Option<ParsedManifest>,
}

pub fn check(path: &Path) -> Outcome {
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            return Outcome {
                result: CheckResult::fail("manifest", format!("read {}: {e}", path.display())),
                parsed: None,
            };
        }
    };
    let parsed: ParsedManifest = match serde_json::from_slice(&bytes) {
        Ok(p) => p,
        Err(e) => {
            return Outcome {
                result: CheckResult::fail("manifest", format!("invalid JSON: {e}")),
                parsed: None,
            };
        }
    };

    let mut errors = Vec::<String>::new();

    if !is_valid_id(&parsed.id) {
        errors.push(format!(
            "id {:?} must be lowercase, reverse-DNS-shaped (e.g. com.example.foo)",
            parsed.id
        ));
    }

    if let Err(e) = semver::Version::parse(&parsed.version) {
        errors.push(format!(
            "version {:?} is not valid semver: {e}",
            parsed.version
        ));
    }

    let kind = parsed.kind();
    if !KNOWN_KINDS.contains(&kind) {
        errors.push(format!("kind {:?} not in {KNOWN_KINDS:?}", kind));
    }

    for p in &parsed.permissions {
        if !KNOWN_PERMISSIONS.contains(&p.as_str()) {
            errors.push(format!("permission {:?} not in {KNOWN_PERMISSIONS:?}", p));
        }
    }

    if errors.is_empty() {
        Outcome {
            result: CheckResult::pass("manifest"),
            parsed: Some(parsed),
        }
    } else {
        Outcome {
            result: CheckResult::fail("manifest", errors.join("\n")),
            parsed: Some(parsed), // still pass it down — later checks can
                                  // surface independent issues.
        }
    }
}

fn is_valid_id(id: &str) -> bool {
    if id.is_empty() {
        return false;
    }
    if !id.contains('.') {
        return false;
    }
    for (i, c) in id.chars().enumerate() {
        match c {
            'a'..='z' | '0'..='9' => {}
            '.' | '-' | '_' => {
                if i == 0 {
                    return false;
                }
            }
            _ => return false,
        }
    }
    true
}
