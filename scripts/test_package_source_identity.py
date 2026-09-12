"""Local Git fixtures and PowerShell preflights; never build or launch Orca."""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import package_source_identity as source


class SourceIdentityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="orca-source-identity-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.run_git("init", "-q")
        self.run_git("config", "user.name", "Local fixture")
        self.run_git("config", "user.email", "fixture@example.invalid")
        self.run_git("config", "core.autocrlf", "false")
        self.write(".gitignore", ".tmp/\n")
        self.write("source.txt", "original\n")
        self.write("removed.txt", "remove me\n")
        self.run_git("add", ".")
        self.run_git("commit", "-qm", "local test fixture")
        self.head = self.run_git("rev-parse", "HEAD").strip()
        self.manifest_path = self.root / ".tmp" / "manifest.json"

    def run_git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.PIPE).decode()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def entry(self, name, action):
        entry = {"path": name, "action": action}
        if action != "delete":
            data = (self.root / name).read_bytes()
            entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        return entry

    def dirty_manifest(self):
        self.write("source.txt", "modified\n")
        self.write("new.txt", "new\n")
        (self.root / "removed.txt").unlink()
        return {"base_head": self.head, "files": [self.entry("source.txt", "replace"),
                self.entry("new.txt", "add"), self.entry("removed.txt", "delete")]}

    def capture(self, manifest):
        self.write(".tmp/manifest.json", json.dumps(manifest))
        return source.capture(self.root, self.manifest_path)

    def test_clean_checkout_needs_no_manifest_or_remote(self):
        result = source.capture(self.root)
        self.assertTrue(result["source_clean"])
        self.assertEqual(result["source_commit"], self.head)
        self.assertEqual(self.run_git("remote"), "")

    def test_dirty_detached_snapshot_is_stable(self):
        self.run_git("checkout", "--detach", "-q")
        manifest = self.dirty_manifest()
        result = self.capture(manifest)
        self.assertFalse(result["source_clean"])
        self.assertIsNone(result["source_branch"])
        self.assertEqual(result, self.capture(manifest))

    def test_staged_new_modified_and_deleted_files_are_accepted(self):
        manifest = self.dirty_manifest()
        self.run_git("add", ".")
        self.assertEqual(len(self.capture(manifest)["files"]), 3)

    def test_dirty_without_manifest_fails(self):
        self.dirty_manifest()
        with self.assertRaisesRegex(ValueError, "complete handoff snapshot"):
            source.capture(self.root)

    def test_omitted_changes_fail(self):
        manifest = self.dirty_manifest()
        for index in range(3):
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "cover"):
                self.capture(dict(manifest, files=manifest["files"][:index] + manifest["files"][index + 1:]))

    def test_wrong_hash_size_and_base_fail(self):
        manifest = self.dirty_manifest()
        variants = []
        for field, value in (("sha256", "0" * 64), ("sha256", None), ("bytes", 999), ("bytes", True)):
            changed = copy.deepcopy(manifest)
            changed["files"][0][field] = value
            variants.append(changed)
        variants.append(dict(manifest, base_head="0" * 40))
        for changed in variants:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                self.capture(changed)

    def test_deleted_file_reappearing_fails(self):
        manifest = self.dirty_manifest()
        self.write("removed.txt", "reappeared")
        with self.assertRaisesRegex(ValueError, "still exists"):
            self.capture(manifest)

    def test_path_escape_and_duplicate_fail(self):
        manifest = self.dirty_manifest()
        for name in ("../outside", "/absolute", "C:/outside", "a\\b", ".git/config", "a/./b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.capture(dict(manifest, files=[{"path": name, "action": "delete"}]))
        manifest["files"].append(dict(manifest["files"][0], path="SOURCE.txt"))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.capture(manifest)

    def test_only_explicit_untracked_research_docs_can_be_excluded(self):
        manifest = self.dirty_manifest()
        self.write("Docs/research.md", "notes")
        manifest["excluded_untracked_docs"] = ["Docs/research.md"]
        self.capture(manifest)
        self.run_git("add", "Docs/research.md")
        with self.assertRaisesRegex(ValueError, "Only untracked"):
            self.capture(manifest)
        manifest["excluded_untracked_docs"] = ["new.txt"]
        with self.assertRaisesRegex(ValueError, "Only untracked"):
            self.capture(manifest)

    def test_source_or_manifest_drift_invalidates_identity(self):
        manifest = self.dirty_manifest()
        before = self.capture(manifest)["source_identity_sha256"]
        self.write("source.txt", "later bytes")
        with self.assertRaisesRegex(ValueError, "mismatch"):
            source.capture(self.root, self.manifest_path)
        manifest["files"][0] = self.entry("source.txt", "replace")
        self.assertNotEqual(before, self.capture(manifest)["source_identity_sha256"])
        self.run_git("checkout", "--detach", "-q")
        self.assertNotEqual(before, self.capture(manifest)["source_identity_sha256"])

    def test_rename_requires_delete_and_add(self):
        (self.root / "source.txt").rename(self.root / "renamed.txt")
        manifest = {"base_head": self.head, "files": [self.entry("source.txt", "delete"), self.entry("renamed.txt", "add")]}
        self.run_git("add", ".")
        self.assertEqual(len(self.capture(manifest)["files"]), 2)

    def test_malformed_manifest_fails(self):
        for value in ([], {"base_head": self.head, "files": [None]}, {"base_head": self.head, "files": {}}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.capture(value)

    @unittest.skipUnless(os.name == "nt" and (shutil.which("pwsh") or shutil.which("powershell")), "Windows PowerShell preflight")
    def test_both_powershell_preflights_accept_dirty_detached_without_remote(self):
        project = Path(__file__).resolve().parents[1]
        for name in ("scripts/package_source_identity.py", "scripts/package_internal_fast.ps1", "release/build_internal.ps1", ".github/team-collaboration.json"):
            self.write(name, (project / name).read_text(encoding="utf-8-sig"))
        self.run_git("add", ".")
        self.run_git("commit", "-qm", "install preflight fixtures")
        self.head = self.run_git("rev-parse", "HEAD").strip()
        self.run_git("checkout", "--detach", "-q")
        manifest = self.dirty_manifest()
        self.capture(manifest)
        # Intentionally non-executable tool placeholders: ValidateOnly must never invoke them.
        self.write(".tmp/never-execute.exe", "This fixture must never be executed.")
        placeholder = self.root / ".tmp" / "never-execute.exe"
        build = self.root / ".tmp" / "build"
        self.write(".tmp/build/CPackConfig.cmake", "# preflight fixture only")
        self.write(".tmp/build/CMakeCache.txt", "\n".join([
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}", f"Python3_EXECUTABLE:FILEPATH={sys.executable}",
            f"CMAKE_COMMAND:INTERNAL={placeholder}", f"CMAKE_CPACK_COMMAND:INTERNAL={placeholder}",
            "ORCA_AI_WINDOWS_INSTALLER:BOOL=ON", "ORCA_AI_DISTRIBUTION_CHANNEL:STRING=internal",
            "ORCA_AI_PACKAGE_REVISION:STRING=fixture", "ORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH=", ""]))
        self.write(".tmp/build/orca_ai_build_info.json", json.dumps({"schema_version": 1,
            "application_commit": self.head, "package_revision": "fixture", "distribution_channel": "internal",
            "sidecar_protocol_version": 2, "sidecar_version": "orcaslicer-ai-sidecar-v9"}))
        powershell = shutil.which("pwsh") or shutil.which("powershell")
        for script in ("scripts/package_internal_fast.ps1", "release/build_internal.ps1"):
            with self.subTest(script=script):
                output = self.root / ".tmp" / "output"
                args = [powershell, "-NoProfile", "-File", str(self.root / script), "-BuildDir", str(build),
                        "-OutputDir", str(output), "-Revision", "fixture", "-SourceManifest", str(self.manifest_path), "-ValidateOnly"]
                result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=40)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertRegex(result.stdout, "True")
                self.assertFalse(output.exists())
                # Both entry points must reject a stale snapshot before invoking any build command.
                self.write("new.txt", "drift")
                rejected = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=40)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("mismatch", rejected.stderr)
                self.assertFalse(output.exists())
                self.write("new.txt", "new\n")


if __name__ == "__main__":
    unittest.main()
