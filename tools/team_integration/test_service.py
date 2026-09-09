"""Offline-only fixtures: socket access is denied for the entire test module."""
import copy
import io
import json
from pathlib import Path
import socket
import tempfile
import time
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

from tools.team_integration.config import validate_config
from tools.team_integration.github import GitHub, GitHubHTTPError, HTTPTransport, number, sha
from tools.team_integration.service import Service, Store, accept_message, dependencies

BASE, HEAD, CANDIDATE, NEW = (letter * 40 for letter in "abcd")


def setUpModule():
    global network_guard
    network_guard = patch.object(socket.socket, "connect", side_effect=AssertionError("network forbidden in offline tests"))
    network_guard.start()


def tearDownModule():
    network_guard.stop()


def configuration(path):
    return {"schema_version": 1, "mode": "phase1", "repository": "team/orca", "target_branch": "codex/team/integration",
            "source_branches": ["codex/team/model-generation", "codex/team/smart-slicing", "codex/team/maintenance"],
            "task_branch_prefixes": ["codex/task/", "codex/upstream-sync-"],
            "users": {"ou_model": "model-owner", "ou_slicing": "slicing-owner", "ou_maintainer": "maintainer"},
            "maintainer_ids": ["ou_maintainer"], "chat_id": "oc_team", "bot_open_id": "ou_bot",
            "required_checks": [{"name": "AI integration checks", "app_id": 100}, {"name": "Team integration candidate", "app_id": 100}],
            "candidate_workflow": ".github/workflows/team-integration-candidate.yml", "candidate_artifact": "team-integration-candidate",
            "required_approvals": 1, "github_token_env": "TEAM_GITHUB_TOKEN", "feishu_app_id_env": "TEAM_FEISHU_APP_ID",
            "feishu_app_secret_env": "TEAM_FEISHU_APP_SECRET", "poll_seconds": 30, "database_path": str(path)}


def pull(n=1):
    return {"number": n, "head": {"sha": HEAD, "ref": "codex/team/model-generation", "repo": {"full_name": "team/orca"}},
            "base": {"sha": BASE, "ref": "codex/team/integration", "repo": {"full_name": "team/orca"}},
            "user": {"login": "model-owner"}, "title": "A small change", "body": "", "draft": False,
            "state": "open", "merged": False, "mergeable": True, "mergeable_state": "clean", "merge_commit_sha": CANDIDATE}


def message(action="提交", n=1, message_id="om_event"):
    return {"chat_id": "oc_team", "actor": "ou_model", "sender_type": "user", "message_type": "text",
            "message_id": message_id, "text": f"@_user_1 {action} PR #{n}",
            "mentions": [{"key": "@_user_1", "open_id": "ou_bot"}]}


class FakeGitHub:
    def __init__(self, config):
        self.config, self.pulls, self.base = config, {1: pull()}, BASE
        self.review_rows = [{"id": 1, "user": {"login": "maintainer"}, "state": "APPROVED", "commit_id": HEAD}]
        self.check_rows = [{"id": i + 1, "name": c["name"], "app": {"id": c["app_id"]}, "head_sha": HEAD,
                            "status": "completed", "conclusion": "success", "check_suite": {"id": 7}}
                           for i, c in enumerate(config["required_checks"])]
        self.evidence = {"check_suite_id": 7, "run_id": 9, "url": "https://github.com/team/orca/actions/runs/9"}
        self.protected = True
        self.base_check_rows = copy.deepcopy(self.check_rows)

    def pull(self, n):
        return copy.deepcopy(self.pulls[n])

    def open_pulls(self):
        return [copy.deepcopy(p) for p in self.pulls.values() if p["state"] == "open"]

    def base_sha(self):
        return self.base

    def reviews(self, n):
        return copy.deepcopy(self.review_rows)

    def checks(self, head):
        if head == self.base:
            return [dict(copy.deepcopy(row), head_sha=self.base) for row in self.base_check_rows]
        return copy.deepcopy(self.check_rows)

    def protection_ready(self):
        return self.protected

    def candidate_evidence(self, n, base, head, candidate):
        return copy.deepcopy(self.evidence) if (base, head, candidate) == (BASE, HEAD, CANDIDATE) else None


class FakeNotifier:
    def __init__(self):
        self.calls = []
        self.fail = False

    def upsert(self, card_id, dedup_id, card):
        self.calls.append((card_id, dedup_id, card))
        if self.fail:
            raise RuntimeError("offline notification failure")
        return card_id or "om_card"


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "state.sqlite3"
        self.config = configuration(self.path)
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.github, self.notifier = FakeGitHub(self.config), FakeNotifier()
        self.service = Service(self.config, self.store, self.github, self.notifier)

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def state(self, n=1):
        return self.store.job(n)["state"]

    def test_config_fails_closed(self):
        validate_config(self.config)
        for field, value in [("mode", "auto-merge"), ("required_checks", []), ("required_approvals", 0),
                             ("target_branch", "main"), ("users", {}), ("database_path", "relative.db")]:
            config = copy.deepcopy(self.config)
            config[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_config(config)

    def test_complete_ci_and_review_reports_manual_ready_never_merges(self):
        self.service.tick()
        self.assertEqual("ready", self.state())
        self.assertFalse(self.github.pulls[1]["merged"])
        self.assertIn("人工", self.store.job(1)["detail"])
        self.assertEqual(1, len(self.notifier.calls))

    def test_failed_build_leaves_other_pr_free(self):
        self.github.check_rows[0]["conclusion"] = "failure"
        self.github.pulls[2] = pull(2)
        self.service.tick()
        self.assertEqual("failed", self.state(1))
        self.assertEqual("failed", self.state(2))

    def test_conflicting_pr_does_not_block_independent_pr(self):
        self.github.pulls[1]["mergeable"] = False
        self.github.pulls[2] = pull(2)
        self.service.tick()
        self.assertEqual("conflict", self.state(1))
        self.assertEqual("ready", self.state(2))

    def test_two_prs_and_post_merge_conflict(self):
        self.github.pulls[2] = pull(2)
        self.service.tick()
        self.assertEqual(["ready", "ready"], [self.state(1), self.state(2)])
        self.github.pulls[1].update(state="closed", merged=True)
        self.github.base = NEW
        self.github.pulls[2]["mergeable"] = False
        self.service.tick()
        self.assertEqual("merged", self.state(1))
        self.assertEqual("conflict", self.state(2))

    def test_push_invalidates_old_checks_and_approval(self):
        self.service.tick()
        self.github.pulls[1]["head"]["sha"] = NEW
        self.service.tick()
        self.assertEqual("pending", self.state())
        self.assertEqual(NEW, self.store.job(1)["head"])

    def test_base_change_invalidates_old_candidate(self):
        self.service.tick()
        self.github.base = NEW
        self.service.tick()
        self.assertEqual("pending", self.state())

    def test_race_author_push_during_final_recheck(self):
        before = pull()
        self.github.pulls[1]["head"]["sha"] = NEW
        state, detail = self.service.evaluate(before, BASE)
        self.assertEqual("pending", state)
        self.assertIn("旧结果失效", detail)

    def test_race_base_change_during_final_recheck(self):
        self.github.base = NEW
        self.assertEqual("pending", self.service.evaluate(pull(), BASE)[0])

    def test_missing_ci_and_protection_never_ready(self):
        self.github.evidence = None
        self.service.tick()
        self.assertEqual("pending", self.state())
        self.github.evidence = {"check_suite_id": 7, "url": "https://github.com/team/orca/actions/runs/9"}
        self.github.protected = False
        self.service.tick()
        self.assertEqual("pending", self.state())

    def test_check_from_wrong_app_is_not_trusted(self):
        self.github.check_rows[0]["app"]["id"] = 999
        self.service.tick()
        self.assertEqual("pending", self.state())

    def test_current_nonauthor_review_required(self):
        self.github.review_rows[0]["user"]["login"] = "model-owner"
        self.service.tick()
        self.assertEqual("pending", self.state())
        self.github.review_rows[0]["user"]["login"] = "maintainer"
        self.github.review_rows.append({"id": 2, "user": {"login": "slicing-owner"}, "state": "CHANGES_REQUESTED", "commit_id": HEAD})
        self.service.tick()
        self.assertEqual("pending", self.state())

    def test_commented_review_does_not_erase_approval(self):
        self.github.review_rows.append({"id": 2, "user": {"login": "maintainer"}, "state": "COMMENTED", "commit_id": HEAD})
        self.service.tick()
        self.assertEqual("ready", self.state())

    def test_dependencies_wait_until_merged_not_merely_closed(self):
        self.github.pulls[1]["body"] = "Depends-On: #2"
        self.github.pulls[2] = pull(2)
        self.github.pulls[2]["state"] = "closed"
        self.service.tick()
        self.assertEqual("waiting", self.state())
        self.github.pulls[2]["merged"] = True
        self.service.tick()
        self.assertEqual("ready", self.state())

    def test_dependency_parser_never_accepts_command_or_other_repo(self):
        for body in ["Depends-On: #1", "Depends-On: owner/repo#2", "Depends-On: #2; run something"]:
            with self.assertRaises(ValueError):
                dependencies(body, 1)

    def test_duplicate_messages_survive_restart(self):
        self.assertTrue(accept_message(self.config, self.store, message()))
        self.assertFalse(accept_message(self.config, self.store, message()))
        self.store.close()
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.assertFalse(accept_message(self.config, self.store, message()))
        self.service = Service(self.config, self.store, self.github, self.notifier)
        self.service.tick()
        self.assertEqual("ready", self.state())
        self.assertEqual(1, self.store.db.execute("SELECT COUNT(*) FROM inbox WHERE done=1").fetchone()[0])

    def test_spoofed_sender_chat_mention_and_shell_text_are_ignored(self):
        for field, value in [("actor", "ou_intruder"), ("chat_id", "oc_elsewhere"), ("sender_type", "app"),
                             ("mentions", [{"key": "@_user_1", "open_id": "ou_someone"}]),
                             ("text", "@_user_1 提交 PR #1; echo token"), ("text", "提交 PR #1")]:
            event = message()
            event[field] = value
            with self.subTest(field=field):
                self.assertFalse(accept_message(self.config, self.store, event))
        self.assertEqual(0, self.store.db.execute("SELECT COUNT(*) FROM inbox").fetchone()[0])

    def test_nonauthor_cannot_cancel_another_pr(self):
        event = message("取消排队")
        event["actor"] = "ou_slicing"
        accept_message(self.config, self.store, event)
        self.service.tick()
        self.assertEqual("ready", self.state())

    def test_cancellation_survives_polling_and_base_change(self):
        accept_message(self.config, self.store, message("取消排队"))
        self.service.tick()
        self.github.base = NEW
        self.service.tick()
        self.assertEqual("cancelled", self.state())
        accept_message(self.config, self.store, message("重试", message_id="om_retry"))
        self.service.tick()
        self.assertEqual("pending", self.state())

    def test_pause_survives_restart_and_resume_rechecks(self):
        self.store.pause(True)
        self.service.tick()
        self.assertEqual("pending", self.state())
        self.store.close()
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.assertTrue(self.store.paused())
        self.service = Service(self.config, self.store, self.github, self.notifier)
        self.store.pause(False)
        self.service.tick()
        self.assertEqual("ready", self.state())

    def test_notification_failure_never_requeues_and_recovers_same_card(self):
        self.notifier.fail = True
        self.service.tick()
        self.assertEqual("ready", self.state())
        self.assertEqual(1, self.store.job(1)["failures"])
        self.notifier.fail = False
        self.store.db.execute("UPDATE jobs SET next_send=0")
        self.store.db.commit()
        self.service.tick()
        self.assertEqual(self.notifier.calls[0][1], self.notifier.calls[1][1])
        self.github.pulls[1]["head"]["sha"] = NEW
        self.service.tick()
        self.assertEqual("om_card", self.notifier.calls[-1][0])
        self.assertEqual(1, len(self.store.jobs()))

    def test_ambiguous_send_outside_dedup_window_does_not_create_duplicate(self):
        self.notifier.fail = True
        self.service.tick()
        self.store.db.execute("UPDATE jobs SET create_started=?,next_send=0", (time.time() - 3601,))
        self.store.db.commit()
        self.notifier.fail = False
        self.service.tick()
        self.assertEqual(1, len(self.notifier.calls))

    def test_restart_updates_existing_card(self):
        self.service.tick()
        self.store.close()
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.service = Service(self.config, self.store, self.github, self.notifier)
        self.github.pulls[1]["head"]["sha"] = NEW
        self.service.tick()
        self.assertEqual("om_card", self.notifier.calls[-1][0])

    def test_repository_identity_cannot_reuse_state(self):
        with self.assertRaises(ValueError):
            Store(self.path, "somewhere/else", self.config["chat_id"], self.config["bot_open_id"])

    def test_chat_and_bot_identity_changes_are_rejected_after_restart(self):
        self.service.tick()
        self.store.close()
        for chat, bot in [("oc_elsewhere", "ou_bot"), ("oc_team", "ou_other_bot")]:
            with self.subTest(chat=chat, bot=bot), self.assertRaises(ValueError):
                Store(self.path, self.config["repository"], chat, bot)
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.assertEqual("om_card", self.store.job(1)["card_id"])

    def test_populated_legacy_database_requires_explicit_identity_migration(self):
        self.service.tick()
        self.store.db.execute("DELETE FROM settings WHERE key IN ('chat_id','bot_open_id')")
        self.store.db.commit()
        with self.assertRaisesRegex(ValueError, "explicit offline migration"):
            Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.assertEqual(0, self.store.db.execute("SELECT COUNT(*) FROM settings WHERE key='chat_id'").fetchone()[0])

    def test_empty_legacy_database_can_bind_identity_without_reusing_cards(self):
        self.store.db.execute("DELETE FROM settings WHERE key IN ('chat_id','bot_open_id')")
        self.store.db.commit()
        self.store.close()
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.assertEqual("oc_team", self.store.db.execute("SELECT value FROM settings WHERE key='chat_id'").fetchone()[0])

    def test_github_outage_revokes_cached_ready_state(self):
        self.service.tick()
        with patch.object(self.github, "base_sha", side_effect=RuntimeError("offline outage")):
            with self.assertRaises(RuntimeError):
                self.service.tick()
        self.assertEqual("pending", self.state())

    def test_integration_failure_pauses_persistently_and_resume_repauses(self):
        self.service.tick()
        self.github.base_check_rows[0]["conclusion"] = "failure"
        self.service.tick()
        self.assertTrue(self.store.paused())
        self.assertEqual("pending", self.state())
        self.assertIn("revert PR", self.store.pause_reason())
        self.store.close()
        self.store = Store(self.path, self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.assertTrue(self.store.paused())
        self.service = Service(self.config, self.store, self.github, self.notifier)
        self.store.pause(False)
        self.service.tick()
        self.assertTrue(self.store.paused())

    def test_missing_or_running_integration_checks_never_admit_pr(self):
        self.github.base_check_rows = []
        self.service.tick()
        self.assertEqual("pending", self.state())
        self.assertFalse(self.store.paused())
        self.github.base_check_rows = copy.deepcopy(self.github.check_rows)
        self.github.base_check_rows[0]["status"] = "in_progress"
        self.service.tick()
        self.assertEqual("pending", self.state())
        self.assertFalse(self.store.paused())

    def test_old_base_failure_does_not_override_current_base_success(self):
        rows = copy.deepcopy(self.github.check_rows)
        rows = [dict(row, head_sha=BASE) for row in rows]
        old = dict(rows[0], head_sha=NEW, id=999, conclusion="failure")
        with patch.object(self.github, "checks", return_value=rows + [old]):
            self.assertEqual("ready", self.service.baseline(BASE)[0])

    def test_post_merge_failure_updates_existing_card_and_blocks_next_pr(self):
        self.github.pulls[2] = pull(2)
        self.service.tick()
        self.github.pulls[1].update(state="closed", merged=True)
        self.github.base = NEW
        self.github.base_check_rows[0]["conclusion"] = "failure"
        self.service.tick()
        self.assertEqual("merged", self.state(1))
        self.assertEqual("pending", self.state(2))
        self.assertTrue(self.store.paused())
        self.assertIn("revert PR", self.store.job(1)["detail"])
        self.assertEqual("om_card", self.notifier.calls[-1][0])

    def test_nonexistent_pr_command_does_not_block_later_commands(self):
        accept_message(self.config, self.store, message(n=99))
        accept_message(self.config, self.store, message("取消排队", message_id="om_valid"))
        original = self.github.pull

        def get_pull(n):
            if n == 99:
                raise GitHubHTTPError(404)
            return original(n)

        with patch.object(self.github, "pull", side_effect=get_pull):
            self.service.tick()
        self.assertEqual("cancelled", self.state())

    def test_teammate_can_read_status_but_not_mutate(self):
        event = message("查状态")
        event["actor"] = "ou_slicing"
        self.assertTrue(accept_message(self.config, self.store, event))
        self.service.tick()
        self.assertEqual("ready", self.state())

    def test_fork_and_wrong_base_never_queue(self):
        self.github.pulls[1]["head"]["repo"]["full_name"] = "attacker/orca"
        self.service.tick()
        self.assertIsNone(self.store.job(1))


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.config = configuration(Path(tempfile.gettempdir()) / "never-created.sqlite3")
        self.evidence = {"schema_version": 1, "repository": "team/orca", "pr_number": 1, "head_sha": HEAD,
                         "base_sha": BASE, "candidate_sha": CANDIDATE, "status": "success", "run_id": 9999999999, "run_attempt": 2}
        self.run = {"id": 9999999999, "run_attempt": 2, "path": self.config["candidate_workflow"], "head_sha": HEAD,
                    "event": "pull_request", "status": "completed", "conclusion": "success", "check_suite_id": 7}
        self.parents = [BASE, HEAD]
        self.archive_name = "candidate.json"
        self.requests = []
        self.github = GitHub(self.config, self)

    def get(self, path, *, archive=False):
        self.requests.append((path, archive))
        if "/git/commits/" in path:
            return {"sha": CANDIDATE, "parents": [{"sha": parent} for parent in self.parents]}
        if "/actions/workflows/" in path:
            return {"workflow_runs": [self.run]}
        if "/actions/runs/" in path:
            return {"artifacts": [{"id": 88, "name": "team-integration-candidate", "expired": False}]}
        if archive:
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, "w") as zipped:
                zipped.writestr(self.archive_name, json.dumps(self.evidence))
            return buffer.getvalue()
        raise AssertionError("unexpected offline route: " + path)

    def test_artifact_matches_repository_workflow_run_attempt_and_both_heads(self):
        result = self.github.candidate_evidence(1, BASE, HEAD, CANDIDATE)
        self.assertEqual(7, result["check_suite_id"])
        self.assertTrue(all(path.startswith("/repos/team/orca/") for path, _ in self.requests))

    def test_stale_attempt_or_baseline_is_rejected(self):
        for field, value in [("run_attempt", 1), ("base_sha", NEW), ("repository", "attacker/orca"), ("pr_number", 2)]:
            original = self.evidence[field]
            self.evidence[field] = value
            with self.subTest(field=field):
                self.assertIsNone(self.github.candidate_evidence(1, BASE, HEAD, CANDIDATE))
            self.evidence[field] = original

    def test_workflow_or_event_mismatch_is_rejected(self):
        for field, value in [("path", ".github/workflows/forged.yml"), ("event", "push"), ("conclusion", "failure")]:
            original = self.run[field]
            self.run[field] = value
            with self.subTest(field=field):
                self.assertIsNone(self.github.candidate_evidence(1, BASE, HEAD, CANDIDATE))
            self.run[field] = original

    def test_wrong_merge_parents_never_use_ci(self):
        self.parents = [NEW, HEAD]
        self.assertIsNone(self.github.candidate_evidence(1, BASE, HEAD, CANDIDATE))
        self.assertEqual(1, len(self.requests))

    def test_archive_traversal_not_extracted(self):
        self.archive_name = "../candidate.json"
        with self.assertRaises(ValueError):
            self.github.candidate_evidence(1, BASE, HEAD, CANDIDATE)

    def test_identifiers_cannot_supply_paths_or_shell(self):
        for bad in ["abc", "a" * 39, "a" * 40 + ";", "$(token)"]:
            with self.assertRaises(ValueError):
                sha(bad)
        for bad in [True, "1", -1, 0, "1; command"]:
            with self.assertRaises(ValueError):
                number(bad)

    def test_http_adapter_only_sends_get_to_github(self):
        transport = HTTPTransport("offline-test-token")
        response = io.BytesIO(b'{"ok":true}')
        with patch.object(transport.opener, "open", return_value=response) as request:
            self.assertTrue(transport.get("/repos/team/orca/pulls/1")["ok"])
        sent = request.call_args.args[0]
        self.assertEqual("GET", sent.method)
        self.assertEqual("https://api.github.com/repos/team/orca/pulls/1", sent.full_url)

    def test_artifact_redirect_drops_authentication(self):
        transport = HTTPTransport("offline-test-token")
        redirect = urllib.error.HTTPError("https://api.github.com/", 302, "Found",
                                          {"Location": "https://artifacts.blob.core.windows.net/object?sig=offline"}, None)
        with patch.object(transport.opener, "open", side_effect=[redirect, io.BytesIO(b"zip-data")]) as request:
            self.assertEqual(b"zip-data", transport.get("/repos/team/orca/actions/artifacts/1/zip", archive=True))
        signed_request = request.call_args_list[1].args[0]
        self.assertFalse(signed_request.has_header("Authorization"))

    def test_untrusted_artifact_redirect_is_rejected(self):
        transport = HTTPTransport("offline-test-token")
        redirect = urllib.error.HTTPError("https://api.github.com/", 302, "Found", {"Location": "https://attacker.test/"}, None)
        with patch.object(transport.opener, "open", side_effect=redirect) as request:
            with self.assertRaises(RuntimeError):
                transport.get("/repos/team/orca/actions/artifacts/1/zip", archive=True)
        self.assertEqual(1, request.call_count)

    def protection(self):
        return {"enforce_admins": {"enabled": True}, "allow_force_pushes": {"enabled": False},
                "allow_deletions": {"enabled": False}, "required_conversation_resolution": {"enabled": True},
                "required_pull_request_reviews": {"dismiss_stale_reviews": True, "require_code_owner_reviews": True,
                    "required_approving_review_count": 1, "require_last_push_approval": True,
                    "bypass_pull_request_allowances": {"users": [], "teams": [], "apps": []}},
                "required_status_checks": {"strict": True, "checks": [
                    {"context": c["name"], "app_id": c["app_id"]} for c in self.config["required_checks"]]}}

    def test_complete_protection_policy_is_required(self):
        protection = self.protection()
        with patch.object(self.github, "get", return_value=protection):
            self.assertTrue(self.github.protection_ready())
        for key in ("enforce_admins", "allow_force_pushes", "allow_deletions", "required_conversation_resolution"):
            changed = copy.deepcopy(protection)
            changed[key]["enabled"] = not changed[key]["enabled"]
            with self.subTest(key=key), patch.object(self.github, "get", return_value=changed):
                self.assertFalse(self.github.protection_ready())
            del changed[key]
            with self.subTest(missing=key), patch.object(self.github, "get", return_value=changed):
                self.assertFalse(self.github.protection_ready())
        protection["required_pull_request_reviews"]["require_last_push_approval"] = False
        with patch.object(self.github, "get", return_value=protection):
            self.assertFalse(self.github.protection_ready())

    def test_review_bypass_allowances_never_admit(self):
        for actor_type in ("users", "teams", "apps"):
            protection = self.protection()
            protection["required_pull_request_reviews"]["bypass_pull_request_allowances"][actor_type] = [{"id": 100}]
            with self.subTest(actor_type=actor_type), patch.object(self.github, "get", return_value=protection):
                self.assertFalse(self.github.protection_ready())


if __name__ == "__main__":
    unittest.main()
