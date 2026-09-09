from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import team_ci_candidate as ci
import run_ai_offline_tests as offline


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.report = {"repository": "example/repo", "pr_number": 12,
                       "head_sha": "1" * 40, "base_sha": "2" * 40,
                       "candidate_sha": "3" * 40, "event_name": "pull_request",
                       "native": True, "cross_platform": False}
        self.pr = {"state": "open", "head": {"sha": "1" * 40},
                   "base": {"sha": "2" * 40, "ref": ci.BRANCH,
                            "repo": {"full_name": "example/repo"}},
                   "merge_commit_sha": "3" * 40}

    def test_live_head_change_invalidates_evidence(self):
        def get(path):
            return {"commit": {"sha": "2" * 40}} if "/branches/" in path else self.pr
        ci.verify_live(self.report, get)
        for part in ("head", "base"):
            with self.subTest(part=part):
                changed = copy.deepcopy(self.pr)
                changed[part]["sha"] = "4" * 40
                with self.assertRaises(ValueError):
                    ci.verify_live(self.report, lambda path: get(path) if "/branches/" in path else changed)
        with self.assertRaises(ValueError):
            ci.verify_live(self.report, lambda path: {"commit": {"sha": "4" * 40}})

    def test_changed_merge_candidate_cannot_reuse_green_head(self):
        self.pr["merge_commit_sha"] = "4" * 40
        with self.assertRaises(ValueError):
            ci.verify_live(self.report, lambda path: {"commit": {"sha": "2" * 40}} if "/branches/" in path else self.pr)

    def test_failed_skipped_missing_native_jobs_never_pass(self):
        results = {name: {"result": "success"} for name in ("inspect", "windows_build", "windows_tests")}
        ci.require_results(self.report, results)
        for state in ("failure", "cancelled", "skipped", None):
            results["windows_tests"] = {"result": state}
            with self.assertRaises(ValueError):
                ci.require_results(self.report, results)
        self.report["native"] = False
        ci.require_results(self.report, {"inspect": {"result": "success"}})

    def test_scope_matches_native_and_cross_platform_changes(self):
        self.assertEqual({"native": False, "cross_platform": False}, ci.scope(["Docs/notes.md"]))
        self.assertEqual({"native": True, "cross_platform": False}, ci.scope(["src/slic3r/GUI/MainFrame.cpp"]))
        for paths in (["deps/Boost/Boost.cmake"], ["src/slic3r/CMakeLists.txt"], ["build_release_macos.sh"],
                      ["cmake/modules/example.cmake"], [".github/actions/apt-install-deps/action.yml"],
                      [".github/workflows/unit_tests.yml"], ["scripts/run_unit_tests.sh"]):
            self.assertEqual({"native": True, "cross_platform": True}, ci.scope(paths))
        self.assertTrue(ci.scope([], upstream=True)["cross_platform"])

    def test_only_exact_merge_parents_accepted_in_real_git_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ci.git(root, "init", "-q")
            ci.git(root, "config", "user.name", "Candidate Test")
            ci.git(root, "config", "user.email", "candidate@example.invalid")
            ci.git(root, "config", "core.autocrlf", "false")
            (root / "src").mkdir()
            (root / "src" / "模型.cpp").write_text("base\n")
            ci.git(root, "add", ".")
            ci.git(root, "commit", "-qm", "base")
            base = ci.git(root, "rev-parse", "HEAD")
            ci.git(root, "checkout", "-qb", "topic")
            (root / "Docs").mkdir()
            ci.git(root, "mv", "src/模型.cpp", "Docs/model.md")
            ci.git(root, "add", ".")
            ci.git(root, "commit", "-qm", "topic")
            head = ci.git(root, "rev-parse", "HEAD")
            ci.git(root, "checkout", "--detach", base)
            ci.git(root, "merge", "--no-ff", "--no-edit", head)
            event = {"number": 12, "repository": {"full_name": "example/repo"}, "pull_request": {
                "head": {"sha": head, "ref": "codex/model-generation"},
                "base": {"sha": base, "ref": ci.BRANCH, "repo": {"full_name": "example/repo"}}}}
            report = ci.inspect(root, event, "pull_request", "example/repo")
            self.assertEqual(head, report["head_sha"])
            self.assertEqual("pending", report["status"])
            self.assertTrue(report["native"], "A rename out of src also deletes native code")
            ci.git(root, "checkout", "--detach", head)
            with self.assertRaises(ValueError):
                ci.inspect(root, event, "pull_request", "example/repo")

    def test_invalid_repository_and_sha_fail_before_git(self):
        with mock.patch.object(ci, "git") as git:
            with self.assertRaises(ValueError):
                ci.inspect(Path.cwd(), {}, "pull_request", "example/repo;command")
            git.assert_not_called()
        with self.assertRaises(ValueError):
            ci.sha("main")

    def test_offline_guard_allows_fixture_server_and_denies_external_network(self):
        offline.network_guard("socket.connect", (None, ("127.0.0.1", 12345)))
        offline.network_guard("socket.getaddrinfo", ("localhost", 80))
        for event, args in (("socket.connect", (None, ("198.51.100.1", 443))),
                            ("socket.getaddrinfo", ("provider.invalid", 443))):
            with self.assertRaises(RuntimeError):
                offline.network_guard(event, args)


if __name__ == "__main__":
    unittest.main()
