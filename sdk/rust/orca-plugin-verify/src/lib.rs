//! Library entry point for `orca-plugin-verify`.
//!
//! The crate ships both a `main.rs` binary (the `orca-plugin-verify` CLI)
//! and this library so sibling crates — chiefly `orca-plugin-publish` —
//! can call the verifier in-process without shelling out. The library
//! exposes only the pipeline used by the marketplace publish gate:
//! [`verify_path`], which mirrors what `main.rs` does sans process exit.

pub mod checks;
pub mod report;
pub mod unpack;

use std::path::Path;

pub use report::Report;

/// Verify a `.orcaplugin` zip or an unpacked plugin directory.
///
/// `smoke` mirrors the `--smoke` CLI flag: when `true`, each declared slot
/// is fired with a synthetic payload after the structural checks pass.
///
/// Returns a [`Report`] describing every check outcome. Callers decide
/// what to do with a failed report — the publish CLI hard-gates on
/// `report.passed()`.
pub fn verify_path(path: &Path, smoke: bool) -> Report {
    match unpack::resolve(path) {
        Ok(u) => checks::run(&u.root, smoke),
        Err(e) => Report::error("unpack", format!("could not resolve plugin path: {e}")),
    }
}
