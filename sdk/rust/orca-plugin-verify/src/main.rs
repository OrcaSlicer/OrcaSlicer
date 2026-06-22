//! `orca-plugin-verify` — author-side validation of a built `.orcaplugin`.
//!
//! Runs a pipeline of checks against either a packaged `.orcaplugin` zip or
//! an already-unpacked plugin directory. Used by plugin authors locally and
//! by the Phase 7.2 marketplace publish flow as a gate before accepting a
//! submission.

use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;

use orca_plugin_verify::{checks, report, unpack};

#[derive(Parser, Debug)]
#[command(
    name = "orca-plugin-verify",
    version,
    about = "Validate an Orca plugin against the engine ABI before shipping."
)]
struct Args {
    /// Path to a `.orcaplugin` zip OR an unpacked plugin directory.
    path: PathBuf,

    /// Smoke-fire each declared slot with a synthetic payload after the
    /// other checks pass. Catches plugins that register slots but panic on
    /// first invocation.
    #[arg(long)]
    smoke: bool,

    /// Emit machine-readable JSON instead of the colored human report. Used
    /// by the marketplace CI pipeline.
    #[arg(long)]
    json: bool,
}

fn main() -> ExitCode {
    let args = Args::parse();

    let unpacked = match unpack::resolve(&args.path) {
        Ok(u) => u,
        Err(e) => {
            let r = report::Report::error("unpack", format!("could not resolve plugin path: {e}"));
            r.print(args.json);
            return ExitCode::from(1);
        }
    };

    let report = checks::run(&unpacked.root, args.smoke);
    report.print(args.json);

    if report.passed() {
        ExitCode::SUCCESS
    } else {
        ExitCode::from(1)
    }
}
