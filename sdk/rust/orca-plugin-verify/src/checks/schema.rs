//! Check 5 — `settings.schema.json` validity.
//!
//! If the plugin ships a settings schema (Phase 4.2.2 declarative settings
//! UI consumes it), assert it parses as JSON Schema. The jsonschema crate's
//! compile step is the validity gate — we don't care which draft is in use
//! as long as one of the supported ones validates.

use std::path::Path;

use crate::report::CheckResult;

pub fn check(path: &Path) -> CheckResult {
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => return CheckResult::fail("schema", format!("read {}: {e}", path.display())),
    };
    let value: serde_json::Value = match serde_json::from_slice(&bytes) {
        Ok(v) => v,
        Err(e) => {
            return CheckResult::fail("schema", format!("settings.schema.json invalid JSON: {e}"));
        }
    };

    match jsonschema::draft202012::new(&value) {
        Ok(_) => CheckResult::pass("schema"),
        Err(e) => CheckResult::fail(
            "schema",
            format!("settings.schema.json failed JSON Schema compile: {e}"),
        ),
    }
}
