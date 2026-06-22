//! Check pipeline.
//!
//! Runs each check in order, threading the parsed manifest + recorded
//! registry state down so later checks don't have to re-parse / reload.

mod abi;
mod layout;
mod manifest;
mod permissions;
mod schema;
mod slots;
mod smoke;

use std::path::Path;

use crate::report::{CheckResult, Report};

pub fn run(root: &Path, smoke: bool) -> Report {
    let mut report = Report::new();

    // 1. Layout — fail-fast: nothing else makes sense if the tree is wrong.
    let layout_outcome = layout::check(root);
    report.push(layout_outcome.result.clone());
    let Some(layout_info) = layout_outcome.info else {
        return report;
    };

    // 2. Manifest schema.
    let manifest_outcome = manifest::check(&layout_info.manifest_path);
    report.push(manifest_outcome.result.clone());
    let Some(parsed) = manifest_outcome.parsed else {
        return report;
    };

    // 3. settings.schema.json validity, if present.
    if let Some(schema_path) = &layout_info.schema_path {
        report.push(schema::check(schema_path));
    } else {
        report.push(CheckResult::skip(
            "schema",
            "no settings.schema.json — skipped",
        ));
    }

    // 4. ABI handshake — load the binary (native or wasm) and record the
    // slot kinds it registers.
    let abi_outcome = abi::check(&layout_info, &parsed);
    report.push(abi_outcome.result.clone());

    if let Some(recorded) = &abi_outcome.recorded {
        // 5. Slot-set equality.
        report.push(slots::check(&parsed, recorded));

        // 6. Permission honesty.
        report.push(permissions::check(&layout_info, &parsed, recorded));

        // 7. Smoke fire (opt-in).
        if smoke {
            report.push(smoke::check(&layout_info, recorded));
        }
    }

    report
}
