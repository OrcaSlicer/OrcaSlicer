//! Integration tests for orca-plugin-verify.
//!
//! These tests author small in-memory fixture plugins (manifest.json plus a
//! stub binary if needed) and run the verifier against them, asserting the
//! correct pass / fail status per check. Doesn't depend on the build-tree
//! staging of other engine fixtures so it can run on its own.

use std::fs;
use std::path::PathBuf;
use std::process::Command;
use tempfile::TempDir;

fn verifier() -> PathBuf {
    let mut p = std::env::current_exe().unwrap();
    // current_exe lives at target/debug/deps/integration-<hash>
    while p.file_name().map(|n| n != "deps").unwrap_or(true) {
        if !p.pop() {
            panic!("could not walk back to target/<profile>/deps");
        }
    }
    p.pop(); // -> target/debug
    p.push("orca-plugin-verify");
    if !p.exists() {
        panic!(
            "verifier binary not at {}; run `cargo build` first",
            p.display()
        );
    }
    p
}

fn run(dir: &std::path::Path, args: &[&str]) -> (String, String, i32) {
    let out = Command::new(verifier())
        .args(args)
        .arg(dir)
        .output()
        .expect("verifier invocation failed");
    (
        String::from_utf8_lossy(&out.stdout).into_owned(),
        String::from_utf8_lossy(&out.stderr).into_owned(),
        out.status.code().unwrap_or(-1),
    )
}

fn write_manifest(dir: &std::path::Path, json: &str) {
    fs::write(dir.join("manifest.json"), json).unwrap();
}

#[test]
fn missing_manifest_fails_layout() {
    let tmp = TempDir::new().unwrap();
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 1, "expected non-zero exit on missing manifest");
    assert!(e.contains("[FAIL] layout"), "layout should fail; got:\n{e}");
}

#[test]
fn bad_id_fails_manifest_check() {
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "BadCapsIdWithoutDot",
        "version":     "0.1.0",
        "permissions": []
    }"#,
    );
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 1);
    assert!(
        e.contains("[FAIL] manifest"),
        "manifest should fail; got:\n{e}"
    );
}

#[test]
fn bad_semver_fails_manifest_check() {
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.foo",
        "version":     "not-a-version",
        "permissions": []
    }"#,
    );
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 1);
    assert!(
        e.contains("[FAIL] manifest") && e.contains("semver"),
        "manifest should flag semver; got:\n{e}"
    );
}

#[test]
fn unknown_permission_fails_manifest_check() {
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.foo",
        "version":     "0.1.0",
        "permissions": ["network", "do_anything_you_want"]
    }"#,
    );
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 1);
    assert!(
        e.contains("[FAIL] manifest") && e.contains("do_anything_you_want"),
        "manifest should flag unknown perm; got:\n{e}"
    );
}

#[test]
fn bad_settings_schema_fails_schema_check() {
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.foo",
        "version":     "0.1.0",
        "kind":        "data",
        "permissions": []
    }"#,
    );
    // A `$ref` that points nowhere is fine — but a malformed `type` rejects.
    // Easier: completely invalid JSON Schema construct.
    fs::write(tmp.path().join("settings.schema.json"), r#"{"type": 42}"#).unwrap();
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 1);
    assert!(e.contains("[FAIL] schema"), "schema should fail; got:\n{e}");
}

#[test]
fn data_only_plugin_passes_without_binary() {
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.profile-pack",
        "version":     "0.1.0",
        "kind":        "data",
        "provides":    ["profile_pack"],
        "permissions": ["profiles_install"]
    }"#,
    );
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 0, "data plugin should pass; stderr:\n{e}");
    // Data plugins skip the slot equality check (no binary). The slots row
    // must render either PASS or SKIP — explicitly NOT FAIL.
    assert!(
        !e.contains("[FAIL] slots"),
        "data plugin must not fail the slots check; got:\n{e}"
    );
}

#[test]
fn data_only_pack_with_profile_pack_passes_slots() {
    // Fixture mirrors `engine/plugins/sample-profiles/manifest.json` —
    // `kind:data, provides:[profile_pack]`. The slot row should be SKIP
    // with a "data plugin: manifest asserts" detail, and the overall
    // verifier should exit 0.
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.sample-profiles",
        "name":        "Sample Profile Pack",
        "version":     "0.1.0",
        "kind":        "data",
        "author":      "Example",
        "description": "Data-only plugin fixture exercising the slots check.",
        "provides":    ["profile_pack"],
        "permissions": ["profiles_install"]
    }"#,
    );
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(rc, 0, "data-only pack should pass; stderr:\n{e}");
    assert!(
        !e.contains("[FAIL] slots"),
        "data-only pack must not fail the slots check; got:\n{e}"
    );
    assert!(
        e.contains("[SKIP] slots") || e.contains("[PASS] slots"),
        "expected slots row to be SKIP or PASS; got:\n{e}"
    );
    assert!(
        e.contains("data plugin") && e.contains("profile_pack"),
        "expected slots detail to mention 'data plugin' + 'profile_pack'; got:\n{e}"
    );
}

#[test]
fn data_plugin_with_native_slot_fails_slots() {
    // A data plugin that claims a binary-only slot (e.g. pipeline_observer)
    // can't possibly register it — there is no binary. The slots check
    // must fail with a clear message.
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.confused",
        "version":     "0.1.0",
        "kind":        "data",
        "provides":    ["pipeline_observer"],
        "permissions": []
    }"#,
    );
    let (_o, e, rc) = run(tmp.path(), &[]);
    assert_eq!(
        rc, 1,
        "binary-only slot on data plugin must fail; stderr:\n{e}"
    );
    assert!(
        e.contains("[FAIL] slots") && e.contains("pipeline_observer"),
        "slots row should fail with the offending kind; got:\n{e}"
    );
}

#[test]
fn json_output_parses_as_json() {
    let tmp = TempDir::new().unwrap();
    write_manifest(
        tmp.path(),
        r#"{
        "id":          "com.example.profile-pack",
        "version":     "0.1.0",
        "kind":        "data",
        "provides":    ["profile_pack"],
        "permissions": []
    }"#,
    );
    let (out, _e, rc) = run(tmp.path(), &["--json"]);
    assert_eq!(rc, 0);
    let v: serde_json::Value = serde_json::from_str(&out)
        .unwrap_or_else(|err| panic!("JSON didn't parse: {err}\nstdout was:\n{out}"));
    let checks = v["checks"].as_array().expect("checks array missing");
    assert!(!checks.is_empty());
}
