"""Offline tests: synthetic repositories only; no remote fetch, SDK or provider access."""

import contextlib
import copy
import io
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import bootstrap


EXAMPLE = Path(__file__).with_name("team-collaboration.example.json")


def complete_config():
    config = bootstrap.read_json(EXAMPLE)
    config["repository"] = "example-org/fixture-project"
    config["developers"] = {"model_generation": "fixture-model", "smart_slicing": "fixture-slicing",
                            "maintenance": "fixture-maintenance"}
    return config


class ConfigTests(unittest.TestCase):
    def test_unconfigured_example_is_valid(self):
        bootstrap.validate_config(bootstrap.read_json(EXAMPLE))

    def test_generated_codeowners_satisfy_actual_integration_verifier(self):
        repository = Path(__file__).resolve().parents[2]
        lock = bootstrap.read_json(repository / "docs/architecture/ai-integration-lock.json")
        verifier_path = repository / "scripts/verify_ai_integration.py"
        spec = importlib.util.spec_from_file_location("team_bootstrap_integration_verifier", verifier_path)
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        generated = bootstrap.codeowners(repository, complete_config(), lock)
        for filename in ("ModelColorCleanup.hpp", "ModelObjText.hpp"):
            self.assertIn(f"/src/slic3r/GUI/AI/Model/{filename} @fixture-model @fixture-maintenance", generated)
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            (fixture / ".github").mkdir()
            owners_path = fixture / ".github/CODEOWNERS"
            owners_path.write_text(generated, encoding="utf-8")

            def ownership_errors():
                return [error for error in verifier.validate_source_constants(lock, fixture)
                        if ".github/CODEOWNERS" in error["message"]]

            self.assertEqual(ownership_errors(), [])
            # Prove the real contract catches the previous generator output.
            without_wildcard = "\n".join(line for line in generated.splitlines()
                                         if not line.startswith("/src/slic3r/GUI/AI/Model/ModelFinishing.* "))
            owners_path.write_text(without_wildcard, encoding="utf-8")
            self.assertTrue(any("model finishing CODEOWNER" in error["message"] for error in ownership_errors()))

    def test_config_rejects_permissions_and_identity_injection(self):
        mutations = [
            ("repository", "owner/../../repo"), ("repository", "owner/repo\nmalicious"),
            ("repository", "https://github.com/owner/repo"), ("repository", "owner/repo.git"),
            ("integration_branch", "main"), ("required_checks", []),
            ("required_checks", ["AI integration checks"]),
            ("automation", {"mode": "notify_and_preview", "auto_merge": True}),
            ("automation", {"mode": "notify_and_preview", "auto_merge": 0}),
            ("bootstrap", {"baseline_sha": "HEAD"}),
            ("bootstrap", {"baseline_sha": "a" * 40 + "\n"}),
            ("feishu", {"chat_id": "oc_chat", "users": {"ou_person": "stranger"}}),
            ("feishu", {"chat_id": "oc_chat", "users": {"email@example.com": "fixture-model"}}),
        ]
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                config = complete_config()
                config[key] = value
                with self.assertRaises(bootstrap.PreparationError):
                    bootstrap.validate_config(config)
        for login in ("@person", "person\n* @attacker", "--force", "person/other", "a" * 40):
            with self.subTest(login=login):
                config = complete_config()
                config["developers"]["maintenance"] = login
                with self.assertRaises(bootstrap.PreparationError):
                    bootstrap.validate_config(config)

    def test_distinct_people_and_no_secret_fields(self):
        config = complete_config()
        config["developers"]["maintenance"] = "FIXTURE-MODEL"
        with self.assertRaises(bootstrap.PreparationError):
            bootstrap.validate_config(config)
        config = complete_config()
        config["feishu"]["app_secret"] = "do-not-store-secrets-here"
        with self.assertRaises(bootstrap.PreparationError):
            bootstrap.validate_config(config)

    def test_remote_url_parser(self):
        for url in ("https://github.com/Example-Org/Fixture-Project.git",
                    "git@github.com:example-org/fixture-project.git",
                    "ssh://git@github.com/example-org/fixture-project.git"):
            self.assertEqual(bootstrap.remote_repository(url), "example-org/fixture-project")
        for url in ("https://secret@github.com/o/r.git", "https://github.com:secret/o/r.git",
                    "https://github.com.evil.test/o/r", "https://github.com/o/r?token=secret"):
            self.assertIsNone(bootstrap.remote_repository(url))


class RepositoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        self.env = patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.run_git("init", "-b", "fixture-start")
        self.run_git("config", "user.name", "Offline Fixture")
        self.run_git("config", "user.email", "fixture@example.invalid")
        self.run_git("config", "commit.gpgsign", "false")
        self.run_git("config", "core.autocrlf", "false")
        (self.root / "fixture.txt").write_text("baseline\n", encoding="utf-8")
        self.run_git("add", "fixture.txt")
        self.run_git("commit", "-m", "Offline fixture baseline")
        self.sha = self.run_git("rev-parse", "HEAD")
        self.config = complete_config()
        self.config["bootstrap"]["baseline_sha"] = self.sha
        self.lock = {"boundaries": {
            "model_generation_owned_paths": ["src/model"],
            "smart_slicing_owned_paths": ["src/slicing"],
            "shared_runtime_owned_paths": ["src/runtime.cpp"],
            "integration_owned_paths": [".github", "src/Plater.cpp"],
        }}

    def run_git(self, *args):
        result = subprocess.run(["git", "-C", str(self.root), *args], text=True, encoding="utf-8",
                                capture_output=True, check=True)
        return result.stdout.strip()

    def refs(self):
        return self.run_git("for-each-ref", "--format=%(refname) %(objectname)", "refs/heads")

    def test_check_is_read_only_and_reports_missing_inputs(self):
        before = self.refs()
        (self.root / "untracked.txt").write_text("unfinished", encoding="utf-8")
        result = bootstrap.inspect(self.root, bootstrap.read_json(EXAMPLE))
        self.assertEqual(before, self.refs())
        self.assertFalse(result["working_tree_clean"])
        self.assertIn("repository", result["missing_configuration"])
        self.assertIn("developers.smart_slicing", result["missing_configuration"])
        self.assertEqual(result["origin"], "missing")
        self.assertFalse(result["remote_protection_verified"])

    def test_origin_is_local_config_only_and_redacted(self):
        self.run_git("remote", "add", "origin", "https://secret@github.com/example-org/fixture-project.git")
        result = bootstrap.inspect(self.root, self.config)
        self.assertEqual(result["origin"], "mismatch")
        self.assertNotIn("secret", json.dumps(result))
        self.run_git("remote", "set-url", "origin", "git@github.com:example-org/fixture-project.git")
        self.assertEqual(bootstrap.inspect(self.root, self.config)["origin"], "matches_repository")

    def test_prepare_is_atomic_idempotent_and_does_not_checkout(self):
        result = bootstrap.prepare_branches(self.root, self.config, self.sha)
        self.assertEqual(set(result["created"]), set(bootstrap.BRANCHES.values()))
        self.assertEqual(self.run_git("branch", "--show-current"), "fixture-start")
        for branch in bootstrap.BRANCHES.values():
            self.assertEqual(self.run_git("rev-parse", branch), self.sha)
        self.assertEqual(bootstrap.prepare_branches(self.root, self.config, self.sha)["created"], [])
        self.assertEqual(self.run_git("remote"), "")

    def test_preexisting_current_role_branch_is_allowed(self):
        self.run_git("branch", "-m", "codex/team/maintenance")
        result = bootstrap.prepare_branches(self.root, self.config, self.sha)
        self.assertEqual(len(result["created"]), 3)
        self.assertEqual(self.run_git("branch", "--show-current"), "codex/team/maintenance")

    def test_dirty_and_untracked_work_blocks_branch_creation(self):
        for filename in ("fixture.txt", "untracked.txt"):
            with self.subTest(filename=filename):
                target = self.root / filename
                before = self.refs()
                target.write_text("unfinished\n", encoding="utf-8")
                with self.assertRaises(bootstrap.PreparationError):
                    bootstrap.prepare_branches(self.root, self.config, self.sha)
                self.assertEqual(before, self.refs())
                if filename == "fixture.txt":
                    target.write_text("baseline\n", encoding="utf-8")
                else:
                    target.unlink()

    def test_branch_collision_blocks_all_creation(self):
        self.run_git("branch", "codex/team/model-generation", self.sha)
        (self.root / "fixture.txt").write_text("next\n", encoding="utf-8")
        self.run_git("commit", "-am", "Offline next commit")
        current = self.run_git("rev-parse", "HEAD")
        self.config["bootstrap"]["baseline_sha"] = current
        before = self.refs()
        with self.assertRaises(bootstrap.PreparationError):
            bootstrap.prepare_branches(self.root, self.config, current)
        self.assertEqual(before, self.refs())

    def test_baseline_must_be_explicit_current_commit(self):
        for baseline in ("HEAD", "a" * 40, self.sha[:7], self.sha + "\ncreate refs/heads/evil HEAD"):
            with self.subTest(baseline=baseline), self.assertRaises(bootstrap.PreparationError):
                bootstrap.prepare_branches(self.root, self.config, baseline)
        config = copy.deepcopy(self.config)
        config["bootstrap"]["baseline_sha"] = "b" * 40
        with self.assertRaises(bootstrap.PreparationError):
            bootstrap.prepare_branches(self.root, config, self.sha)

    def test_plan_generates_real_owners_and_protection_without_git_changes(self):
        output = Path(self.temp.name) / "plan"
        before = self.refs()
        result = bootstrap.create_plan(self.root, self.config, self.lock, output)
        self.assertEqual(before, self.refs())
        self.assertFalse(result["applied"])
        protection = bootstrap.read_json(output / "integration-branch-protection.json")
        reviews = protection["required_pull_request_reviews"]
        self.assertTrue(protection["required_status_checks"]["strict"])
        self.assertNotIn("contexts", protection["required_status_checks"])
        self.assertEqual([check["context"] for check in protection["required_status_checks"]["checks"]], bootstrap.CHECKS)
        self.assertTrue(reviews["dismiss_stale_reviews"])
        self.assertTrue(reviews["require_code_owner_reviews"])
        self.assertTrue(reviews["require_last_push_approval"])
        self.assertEqual(reviews["required_approving_review_count"], 1)
        self.assertTrue(protection["enforce_admins"])
        self.assertFalse(protection["allow_force_pushes"])
        self.assertFalse(protection["allow_deletions"])
        self.assertFalse(protection["required_linear_history"])
        owners = (output / "CODEOWNERS").read_text(encoding="utf-8")
        self.assertIn("/src/slic3r/GUI/AI/Model/ModelFinishing.cpp @fixture-model @fixture-maintenance", owners)
        self.assertIn("/src/runtime.cpp @fixture-model @fixture-slicing @fixture-maintenance", owners)
        deployment = bootstrap.read_json(output / "deployment-plan.json")
        self.assertIn("codex%2Fteam%2Fintegration", deployment["integration_protection_request"]["api_path"])
        self.assertEqual(len(deployment["developer_protection_requests"]), 3)
        with self.assertRaises(bootstrap.PreparationError):
            bootstrap.create_plan(self.root, self.config, self.lock, output)

    def test_plan_refuses_unknown_owners_and_unsafe_lock_paths(self):
        output = Path(self.temp.name) / "plan"
        config = copy.deepcopy(self.config)
        config["developers"]["smart_slicing"] = None
        with self.assertRaises(bootstrap.PreparationError):
            bootstrap.create_plan(self.root, config, self.lock, output)
        self.assertFalse(output.exists())
        for path in ("../outside", "/root", "src/../../outside", "* @attacker", "src/a\n*", "src\\a"):
            with self.subTest(path=path):
                lock = copy.deepcopy(self.lock)
                lock["boundaries"]["integration_owned_paths"] = [path]
                with self.assertRaises(bootstrap.PreparationError):
                    bootstrap.create_plan(self.root, self.config, lock, output)
                self.assertFalse(output.exists())

    def test_cli_check_uses_explicit_configuration(self):
        config_path = Path(self.temp.name) / "config.json"
        config_path.write_text(json.dumps(self.config), encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = bootstrap.main(["--repo", str(self.root), "--config", str(config_path), "check"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["head_sha"], self.sha)


if __name__ == "__main__":
    unittest.main()
