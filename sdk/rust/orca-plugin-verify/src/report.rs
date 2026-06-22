//! Verifier report — the human-readable + JSON output channel.
//!
//! Every check produces a single `CheckResult` row; the `Report` holds them
//! all and renders to stderr (or stdout if --json). Exit code is driven by
//! `Report::passed()` — any failed row flips the report to non-passing.

use serde::Serialize;
use std::io::Write;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Status {
    Pass,
    Fail,
    Warn,
    Skip,
}

#[derive(Debug, Clone, Serialize)]
pub struct CheckResult {
    pub name: String,
    pub status: Status,
    pub detail: String,
}

impl CheckResult {
    pub fn pass(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            status: Status::Pass,
            detail: String::new(),
        }
    }
    pub fn fail(name: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            status: Status::Fail,
            detail: detail.into(),
        }
    }
    pub fn warn(name: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            status: Status::Warn,
            detail: detail.into(),
        }
    }
    pub fn skip(name: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            status: Status::Skip,
            detail: detail.into(),
        }
    }
}

#[derive(Debug, Default, Serialize)]
pub struct Report {
    pub checks: Vec<CheckResult>,
}

impl Report {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&mut self, r: CheckResult) {
        self.checks.push(r);
    }

    pub fn passed(&self) -> bool {
        self.checks.iter().all(|c| c.status != Status::Fail)
    }

    pub fn error(name: impl Into<String>, detail: impl Into<String>) -> Self {
        let mut r = Self::new();
        r.push(CheckResult::fail(name, detail));
        r
    }

    pub fn print(&self, json: bool) {
        if json {
            // serde_json::to_writer_pretty never fails on a Vec — but the
            // write to stdout might; in that case there's nothing useful to
            // do, just bail.
            let _ = serde_json::to_writer_pretty(std::io::stdout(), self);
            let _ = writeln!(std::io::stdout());
            return;
        }
        let mut err = std::io::stderr();
        for c in &self.checks {
            let badge = match c.status {
                Status::Pass => "[PASS]",
                Status::Fail => "[FAIL]",
                Status::Warn => "[WARN]",
                Status::Skip => "[SKIP]",
            };
            let _ = writeln!(err, "{badge} {}", c.name);
            if !c.detail.is_empty() {
                for line in c.detail.lines() {
                    let _ = writeln!(err, "         {line}");
                }
            }
        }
        let pass_count = self
            .checks
            .iter()
            .filter(|c| c.status == Status::Pass)
            .count();
        let fail_count = self
            .checks
            .iter()
            .filter(|c| c.status == Status::Fail)
            .count();
        let warn_count = self
            .checks
            .iter()
            .filter(|c| c.status == Status::Warn)
            .count();
        let skip_count = self
            .checks
            .iter()
            .filter(|c| c.status == Status::Skip)
            .count();
        let _ = writeln!(
            err,
            "\nsummary: {pass_count} pass / {fail_count} fail / {warn_count} warn / {skip_count} skip"
        );
    }
}
