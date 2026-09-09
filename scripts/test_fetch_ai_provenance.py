"""Network-free tests: every Git process is mocked and its argv is checked."""

import copy
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import fetch_ai_provenance as provenance


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[1]
        self.document = json.loads((self.root / "docs/architecture/ai-integration-lock.json").read_text(encoding="utf-8-sig"))
        self.commits = provenance.pinned_commits(self.document)
        self.present = set()
        self.fetch_fails = False
        self.fetch_without_object = False
        self.calls = []
        self.process = patch.object(provenance.subprocess, "run", side_effect=self.fake_git)
        self.process.start()
        self.addCleanup(self.process.stop)
        self.environment = patch.dict(os.environ, {"GITHUB_ACTIONS": "false"})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def fake_git(self, argv, **kwargs):
        self.calls.append(argv)
        self.assertEqual(argv[:4], ["git", "--no-optional-locks", "-C", str(self.root)])
        self.assertNotIn("shell", kwargs)
        self.assertEqual(kwargs["env"]["GIT_NO_LAZY_FETCH"], "1")
        self.assertEqual(kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")
        self.assertEqual(kwargs["timeout"], 60)
        command = argv[4:]
        if command[:2] == ["cat-file", "-e"]:
            self.assertEqual(len(command), 3)
            commit = command[2].removesuffix("^{commit}")
            self.assertIn(commit, self.commits)
            return subprocess.CompletedProcess(argv, int(commit not in self.present), b"", b"")
        self.assertEqual(command[:-1], ["fetch", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules",
                                       "--refmap=", "--no-auto-maintenance", "origin"])
        self.assertIn(command[-1], self.commits)
        self.assertEqual(os.environ["GITHUB_ACTIONS"], "true")
        if not self.fetch_fails and not self.fetch_without_object:
            self.present.add(command[-1])
        return subprocess.CompletedProcess(argv, int(self.fetch_fails), b"", b"credential-bearing error must stay hidden")

    def test_check_only_never_fetches_even_in_actions(self):
        os.environ["GITHUB_ACTIONS"] = "true"
        result = provenance.restore_provenance(self.root, self.document, check_only=True)
        self.assertEqual(result["missing"], self.commits)
        self.assertEqual(result["status"], "missing_fixed_objects")
        self.assertTrue(all(call[4] == "cat-file" for call in self.calls))

    def test_missing_objects_cannot_trigger_local_fetch(self):
        for value in ("false", "TRUE", "1", ""):
            with self.subTest(value=value):
                os.environ["GITHUB_ACTIONS"] = value
                with self.assertRaisesRegex(ValueError, "GITHUB_ACTIONS=true"):
                    provenance.restore_provenance(self.root, self.document)
        self.assertTrue(all(call[4] == "cat-file" for call in self.calls))

    def test_existing_objects_are_skipped_without_ci_environment(self):
        self.present.update(self.commits)
        result = provenance.restore_provenance(self.root, self.document)
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["fetched"], [])
        self.assertEqual(len(self.calls), len(self.commits))

    def test_ci_fetches_only_missing_fixed_objects_and_rechecks_each(self):
        os.environ["GITHUB_ACTIONS"] = "true"
        self.present.add(self.commits[0])
        result = provenance.restore_provenance(self.root, self.document)
        self.assertEqual(result["fetched"], self.commits[1:])
        self.assertEqual(result["missing"], [])
        self.assertFalse(result["development_refs_changed"])
        for index, call in enumerate(self.calls):
            if call[4] == "fetch":
                self.assertEqual(self.calls[index + 1][4:], ["cat-file", "-e", call[-1] + "^{commit}"])

    def test_fetch_failure_remains_a_missing_fixed_object(self):
        os.environ["GITHUB_ACTIONS"] = "true"
        self.fetch_fails = True
        result = provenance.restore_provenance(self.root, self.document)
        self.assertEqual(result["missing"], self.commits)
        self.assertEqual(result["fetched"], [])
        self.assertNotIn("credential", json.dumps(result))

    def test_successful_fetch_requires_actual_commit_presence(self):
        os.environ["GITHUB_ACTIONS"] = "true"
        self.fetch_without_object = True
        result = provenance.restore_provenance(self.root, self.document)
        self.assertEqual(result["missing"], self.commits)

    def test_invalid_schema_or_sha_is_rejected_before_any_git_process(self):
        for value in ("HEAD", "a" * 40 + ":refs/heads/main", "-f", "a" * 40 + "\n", "f" * 40):
            with self.subTest(value=value):
                document = copy.deepcopy(self.document)
                document["feature_sources"]["smart_slicing"]["sha"] = value
                with self.assertRaises(ValueError):
                    provenance.restore_provenance(self.root, document, check_only=True)
        document = copy.deepcopy(self.document)
        document["schema"] = "unknown"
        with self.assertRaises(ValueError):
            provenance.restore_provenance(self.root, document, check_only=True)
        self.assertEqual(self.calls, [])

    def test_deduplication_and_sha_validation_after_schema_validator(self):
        document = copy.deepcopy(self.document)
        document["integration_receipts"]["model_generation"]["integration_commit"] = self.commits[0]
        # Exercise extraction independently of today's exact identity pins.
        with patch.object(provenance, "validate_document", return_value=[]):
            commits = provenance.pinned_commits(document)
            self.assertEqual(commits.count(self.commits[0]), 1)
            document["upstream"]["sha"] = "HEAD"
            with self.assertRaises(ValueError):
                provenance.pinned_commits(document)

    def test_cli_missing_objects_fail_closed(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exit_code = provenance.main(["--repo-root", str(self.root), "--check-only"])
        self.assertEqual(exit_code, 1)
        self.assertEqual(json.loads(output.getvalue())["status"], "missing_fixed_objects")
        self.present.update(self.commits)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(provenance.main(["--repo-root", str(self.root), "--check-only"]), 0)

    def test_cli_fetch_error_is_nonzero_and_does_not_echo_git_output(self):
        os.environ["GITHUB_ACTIONS"] = "true"
        self.fetch_fails = True
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(provenance.main(["--repo-root", str(self.root)]), 1)
        self.assertNotIn("credential", output.getvalue())


if __name__ == "__main__":
    unittest.main()
