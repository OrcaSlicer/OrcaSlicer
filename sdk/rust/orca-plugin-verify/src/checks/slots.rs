//! Check — slot-set equality.
//!
//! manifest.provides[] is supposed to be authoritative documentation of what
//! slot kinds the plugin will register. This check asserts the BINARY agrees:
//! every kind in manifest.provides[] must show up in the recorded ABI
//! handshake's slot_kinds list, and vice versa.
//!
//! The mapping from kind-string to numeric ORCA_SLOT_* constant lives here
//! (single source of truth — the SDK abi.rs constants and the engine's
//! plugin_api.h header). If the manifest references a kind we don't know,
//! that's a fail.
//!
//! ## Data-kind exception
//!
//! `kind: data` plugins have NO binary — the loader path is
//! `PresetBundle::load_vendor_configs_from_json`, which reads the staged
//! profile tree directly. There's no `orca_plugin_register` call to record
//! slot registrations, so the equality check is meaningless. For data
//! plugins the manifest is the only source of truth: we accept only the
//! data-loadable slot kinds (currently just `profile_pack`) and reject any
//! kind that would require a runtime binary handshake.

use std::collections::BTreeSet;

use orca_plugin_sdk::abi;

use super::abi::RecordedState;
use super::manifest::ParsedManifest;
use crate::report::CheckResult;

fn kind_for_name(name: &str) -> Option<u32> {
    Some(match name {
        "pipeline_observer" => abi::ORCA_SLOT_PIPELINE_OBSERVER,
        "pipeline_interceptor" => abi::ORCA_SLOT_PIPELINE_INTERCEPTOR,
        "gcode_filter" => abi::ORCA_SLOT_GCODE_FILTER,
        "placeholder_provider" => abi::ORCA_SLOT_PLACEHOLDER_PROVIDER,
        "printer_agent" => abi::ORCA_SLOT_PRINTER_AGENT,
        "profile_pack" => abi::ORCA_SLOT_PROFILE_PACK,
        "settings_page" => abi::ORCA_SLOT_SETTINGS_PAGE,
        "device_tab" => abi::ORCA_SLOT_DEVICE_TAB,
        "menu_command" => abi::ORCA_SLOT_MENU_COMMAND,
        "sidebar_panel" => abi::ORCA_SLOT_SIDEBAR_PANEL,
        _ => return None,
    })
}

/// Slot kinds that can be carried by a `kind: data` plugin — the engine
/// loads them straight from the staged tree, no binary handshake required.
/// Today that's exclusively `profile_pack` (loaded by
/// `PresetBundle::load_vendor_configs_from_json`).
const DATA_LOADABLE_SLOTS: &[&str] = &["profile_pack"];

pub fn check(manifest: &ParsedManifest, recorded: &RecordedState) -> CheckResult {
    if manifest.kind() == "data" {
        return check_data(manifest);
    }

    // If the manifest doesn't list provides[], skip the equality check
    // (still a pass — the manifest is just less documented). The binary
    // can still register slots.
    if manifest.provides.is_empty() && recorded.slot_kinds.is_empty() {
        return CheckResult::pass("slots");
    }

    let mut errors = Vec::<String>::new();

    let declared: BTreeSet<u32> = manifest
        .provides
        .iter()
        .filter_map(|n| match kind_for_name(n) {
            Some(k) => Some(k),
            None => {
                errors.push(format!("manifest provides {n:?} is not a known slot kind"));
                None
            }
        })
        .collect();
    let registered: BTreeSet<u32> = recorded.slot_kinds.iter().copied().collect();

    let missing_in_binary: Vec<u32> = declared.difference(&registered).copied().collect();
    let undeclared_in_binary: Vec<u32> = registered.difference(&declared).copied().collect();

    if !missing_in_binary.is_empty() {
        errors.push(format!(
            "manifest declared {missing_in_binary:?} but binary didn't register them"
        ));
    }
    if !undeclared_in_binary.is_empty() {
        errors.push(format!(
            "binary registered {undeclared_in_binary:?} but manifest doesn't declare them"
        ));
    }

    if errors.is_empty() {
        CheckResult::pass("slots")
    } else {
        CheckResult::fail("slots", errors.join("\n"))
    }
}

/// Slot check for `kind: data` plugins. There is no binary, so the
/// manifest's `provides[]` is the only source of truth. We require it to be
/// non-empty and contain only data-loadable slot kinds.
fn check_data(manifest: &ParsedManifest) -> CheckResult {
    if manifest.provides.is_empty() {
        return CheckResult::fail(
            "slots",
            "data plugin must declare at least one slot it provides \
             (expected e.g. [\"profile_pack\"])",
        );
    }

    let mut unknown: Vec<&str> = Vec::new();
    let mut binary: Vec<&str> = Vec::new();
    let mut accepted: Vec<&str> = Vec::new();

    for name in &manifest.provides {
        let n = name.as_str();
        if DATA_LOADABLE_SLOTS.contains(&n) {
            accepted.push(n);
        } else if kind_for_name(n).is_some() {
            binary.push(n);
        } else {
            unknown.push(n);
        }
    }

    let mut errors = Vec::<String>::new();
    if !unknown.is_empty() {
        errors.push(format!(
            "manifest provides {unknown:?} which are not known slot kinds"
        ));
    }
    if !binary.is_empty() {
        errors.push(format!(
            "data plugins cannot register binary-only slot kinds {binary:?}; \
             data plugins have no binary to handshake. \
             Allowed for kind:data: {DATA_LOADABLE_SLOTS:?}"
        ));
    }

    if errors.is_empty() {
        CheckResult::skip(
            "slots",
            format!("data plugin: manifest asserts {accepted:?}"),
        )
    } else {
        CheckResult::fail("slots", errors.join("\n"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest(kind: &str, provides: &[&str]) -> ParsedManifest {
        ParsedManifest {
            id: "com.example.foo".into(),
            version: "0.1.0".into(),
            kind: Some(kind.into()),
            permissions: Vec::new(),
            provides: provides.iter().map(|s| (*s).to_string()).collect(),
        }
    }

    fn recorded(slot_kinds: &[u32]) -> RecordedState {
        RecordedState {
            manifest_id: None,
            manifest_perms: 0,
            slot_kinds: slot_kinds.to_vec(),
            registered_agent_ids: Vec::new(),
        }
    }

    use crate::report::Status;

    // -------- data-kind plugins --------

    #[test]
    fn data_kind_with_profile_pack_passes() {
        let m = manifest("data", &["profile_pack"]);
        let r = recorded(&[]);
        let res = check(&m, &r);
        // Data plugins skip the equality check (no binary). They produce a
        // Skip with a "manifest asserts ..." detail — explicitly NOT a Fail.
        assert_eq!(
            res.status,
            Status::Skip,
            "data + profile_pack must not fail; got {res:?}"
        );
        assert!(
            res.detail.contains("data plugin"),
            "detail should mention data plugin; got {:?}",
            res.detail
        );
        assert!(
            res.detail.contains("profile_pack"),
            "detail should mention asserted kinds; got {:?}",
            res.detail
        );
    }

    #[test]
    fn data_kind_empty_provides_fails() {
        let m = manifest("data", &[]);
        let r = recorded(&[]);
        let res = check(&m, &r);
        assert_eq!(res.status, Status::Fail);
        assert!(
            res.detail.contains("at least one slot"),
            "detail should explain the empty-provides failure; got {:?}",
            res.detail
        );
    }

    #[test]
    fn data_kind_with_native_slot_fails() {
        let m = manifest("data", &["pipeline_observer"]);
        let r = recorded(&[]);
        let res = check(&m, &r);
        assert_eq!(res.status, Status::Fail);
        assert!(
            res.detail.contains("pipeline_observer"),
            "detail should name the rejected slot; got {:?}",
            res.detail
        );
        assert!(
            res.detail.contains("data plugins cannot register"),
            "detail should explain why; got {:?}",
            res.detail
        );
    }

    #[test]
    fn data_kind_with_unknown_slot_fails() {
        let m = manifest("data", &["bogus_kind"]);
        let r = recorded(&[]);
        let res = check(&m, &r);
        assert_eq!(res.status, Status::Fail);
        assert!(res.detail.contains("bogus_kind"));
    }

    // -------- native / wasm / webview / hybrid (regression) --------

    #[test]
    fn native_kind_match_passes() {
        let m = manifest("native", &["pipeline_observer"]);
        let r = recorded(&[abi::ORCA_SLOT_PIPELINE_OBSERVER]);
        assert_eq!(check(&m, &r).status, Status::Pass);
    }

    #[test]
    fn native_kind_empty_both_passes() {
        let m = manifest("native", &[]);
        let r = recorded(&[]);
        assert_eq!(check(&m, &r).status, Status::Pass);
    }

    #[test]
    fn native_kind_manifest_undeclared_registration_fails() {
        let m = manifest("native", &[]);
        let r = recorded(&[abi::ORCA_SLOT_PRINTER_AGENT]);
        let res = check(&m, &r);
        assert_eq!(res.status, Status::Fail);
        assert!(
            res.detail.contains("binary registered"),
            "detail should call out the binary registration; got {:?}",
            res.detail
        );
    }

    #[test]
    fn native_kind_manifest_declared_but_binary_silent_fails() {
        let m = manifest("native", &["printer_agent"]);
        let r = recorded(&[]);
        let res = check(&m, &r);
        assert_eq!(res.status, Status::Fail);
        assert!(
            res.detail.contains("manifest declared"),
            "detail should call out the missing registration; got {:?}",
            res.detail
        );
    }

    #[test]
    fn native_kind_unknown_slot_name_in_manifest_fails() {
        let m = manifest("native", &["totally_made_up"]);
        let r = recorded(&[]);
        let res = check(&m, &r);
        assert_eq!(res.status, Status::Fail);
        assert!(res.detail.contains("totally_made_up"));
    }

    #[test]
    fn wasm_kind_match_passes() {
        let m = manifest("wasm", &["pipeline_observer"]);
        let r = recorded(&[abi::ORCA_SLOT_PIPELINE_OBSERVER]);
        assert_eq!(check(&m, &r).status, Status::Pass);
    }

    #[test]
    fn hybrid_kind_match_passes() {
        let m = manifest("hybrid", &["printer_agent"]);
        let r = recorded(&[abi::ORCA_SLOT_PRINTER_AGENT]);
        assert_eq!(check(&m, &r).status, Status::Pass);
    }

    #[test]
    fn webview_kind_empty_both_passes() {
        // Webview-only plugins are similar to data in that their abi-check
        // skips, but the equality fall-through still hits the empty-empty
        // pass branch.
        let m = manifest("webview", &[]);
        let r = recorded(&[]);
        assert_eq!(check(&m, &r).status, Status::Pass);
    }
}
