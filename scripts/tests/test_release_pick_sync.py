#!/usr/bin/env python3
"""Tests for scripts/release_pick_sync.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts -v
"""

import io
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import release_pick_sync as sync  # noqa: E402
from release_pick_git import Pick  # noqa: E402
from release_pick_github import GitHubError, Project  # noqa: E402
from release_pick_rules import Item, LinkedPr  # noqa: E402

REPO = "OrcaSlicer/OrcaSlicer"
OPTIONS = {"Proposed": "o1", "In progress": "o2", "Ready to pick": "o3", "Picked": "o4"}


def item(number, kind="PullRequest", state="OPEN", labelled=True, milestone="2.5.2", status=None, item_id=None,
         linked=()):
    return Item(number=number, kind=kind, state=state, labelled=labelled, milestone=milestone,
                milestone_open=True, content_id=f"C_{number}", linked_prs=linked, item_id=item_id, status=status)


class FakeGitHub:
    def __init__(self, project_items, labelled, commit_prs=None, fail_on=None):
        self.project_items = project_items
        self.labelled = labelled
        self.commit_prs = commit_prs or {}
        self.fail_on = fail_on
        self.calls = []

    def project(self, owner, number, repo):
        return Project("PVT", "FIELD", OPTIONS, self.project_items)

    def labelled_items(self, repo, label):
        if self.fail_on == "labelled_items":
            raise GitHubError("boom")
        return self.labelled

    def open_milestones(self, repo):
        return ["2.5.2"]

    def prs_for_commit(self, repo, sha):
        return self.commit_prs.get(sha, [])

    def _mutate(self, name, *args):
        self.calls.append((name, *args))
        if self.fail_on == name and sum(c[0] == name for c in self.calls) == 1:
            raise GitHubError("mutation failed")

    def add_item(self, project_id, content_id):
        self._mutate("add_item", project_id, content_id)
        return f"NEW_{content_id}"

    def archive_item(self, project_id, item_id):
        self._mutate("archive_item", project_id, item_id)

    def set_status(self, project_id, item_id, field_id, option_id):
        self._mutate("set_status", project_id, item_id, field_id, option_id)


def git_stubs(picks=(), reverted=(), matches=None):
    """Patches for the git side: one branch rc/v2.5.2 with the given picks."""
    return [
        mock.patch.object(sync, "remote_branches", return_value={"main", "rc/v2.5.2"}),
        mock.patch.object(sync, "branch_base", return_value="v2.5.1"),
        mock.patch.object(sync, "branch_picks", return_value=(list(picks), list(reverted))),
        mock.patch.object(sync, "patch_id_matches", return_value=matches or {}),
    ]


class MainTest(unittest.TestCase):
    def test_missing_token_skips_with_notice(self):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(sys, "stdout", out), \
                mock.patch.object(sys, "argv", ["sync", "--owner", "o", "--project", "1", "--repo", "o/r"]):
            self.assertEqual(sync.main(), 0)
        self.assertIn("::notice::RELEASE_PROJECT_TOKEN is not set", out.getvalue())

    def test_webhook_and_post_id_come_from_the_environment(self):
        env = {"RELEASE_PROJECT_TOKEN": "t", "RELEASE_STATUS_DISCORD_WEBHOOK": "https://discord.invalid/h",
               "RELEASE_STATUS_MESSAGE_ID": "999"}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(sync, "run", return_value=0) as run, \
                mock.patch.object(sys, "argv", ["sync", "--owner", "o", "--project", "1", "--repo", "o/r"]):
            self.assertEqual(sync.main(), 0)
        kwargs = run.call_args.kwargs
        self.assertIsInstance(kwargs["discord"], sync.Discord)
        self.assertEqual(kwargs["post_id"], "999")
        self.assertFalse(kwargs["allow_create"])  # a local run creates nothing unless asked

    def test_only_a_run_that_asks_may_create_the_post(self):
        # inputs.create_discord_post is empty on scheduled and event runs
        for value, allowed in (("", False), ("false", False), ("true", True)):
            env = {"RELEASE_PROJECT_TOKEN": "t", "RELEASE_STATUS_DISCORD_WEBHOOK": "https://discord.invalid/h",
                   "CREATE_DISCORD_POST": value}
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(sync, "run", return_value=0) as run, \
                    mock.patch.object(sys, "argv", ["sync", "--owner", "o", "--project", "1", "--repo", "o/r"]):
                sync.main()
            self.assertEqual(run.call_args.kwargs["allow_create"], allowed, value)

    def test_summary_drops_workflow_command_prefixes(self):
        import tempfile
        # With delete=True, Windows will not let main() open the file a second time.
        with tempfile.NamedTemporaryFile("r", suffix=".md", delete=False) as summary:
            self.addCleanup(os.remove, summary.name)
            def fake_run(*args, **kwargs):
                print("::warning::Discord update failed: HTTP 500", file=args[6])
                return 0
            env = {"RELEASE_PROJECT_TOKEN": "t", "GITHUB_STEP_SUMMARY": summary.name}
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(sync, "run", fake_run), \
                    mock.patch.object(sys, "stdout", io.StringIO()) as stdout, \
                    mock.patch.object(sys, "argv", ["sync", "--owner", "o", "--project", "1", "--repo", "o/r"]):
                sync.main()
            self.assertIn("::warning::Discord update failed", stdout.getvalue())
            self.assertEqual(summary.read().strip(), "Warning: Discord update failed: HTTP 500")

    def test_no_webhook_means_no_discord(self):
        with mock.patch.dict(os.environ, {"RELEASE_PROJECT_TOKEN": "t"}, clear=True), \
                mock.patch.object(sync, "run", return_value=0) as run, \
                mock.patch.object(sys, "argv", ["sync", "--owner", "o", "--project", "1", "--repo", "o/r"]):
            sync.main()
        self.assertIsNone(run.call_args.kwargs["discord"])


class FakeDiscord:
    def __init__(self, fail=False):
        self.calls = []
        self.fail = fail

    def create_post(self, thread_name, payload):
        self.calls.append(("create", thread_name, payload))
        return "777"

    def edit_post(self, post_id, payload):
        self.calls.append(("edit", post_id, payload))
        if self.fail:
            raise sync.DiscordError("Discord PATCH failed: HTTP 404")


class RunTest(unittest.TestCase):
    def run_sync(self, gh, apply=False, discord=None, post_id=None, allow_create=True, **git):
        out = io.StringIO()
        patches = git_stubs(**git)
        for p in patches:
            p.start()
        self.addCleanup(lambda: [p.stop() for p in patches])
        code = sync.run(gh, ".", "OrcaSlicer", 15, REPO, apply, out, discord=discord, post_id=post_id,
                        allow_create=allow_create, now="2026-11-18T17:30:00+00:00")
        return code, out.getvalue()

    def test_apply_edits_the_discord_post_with_the_new_state(self):
        discord = FakeDiscord()
        gh = FakeGitHub([item(1, state="MERGED", status="In progress", item_id="P1")], [item(1, state="MERGED")])
        code, out = self.run_sync(gh, apply=True, discord=discord, post_id="999")
        self.assertEqual(code, 0)
        (action, post_id, payload), = discord.calls
        self.assertEqual((action, post_id), ("edit", "999"))
        self.assertIn("**Ready to pick** 1", payload["embeds"][0]["description"])
        self.assertEqual(payload["embeds"][0]["url"], "https://github.com/orgs/OrcaSlicer/projects/15")

    def test_apply_without_post_id_creates_the_post(self):
        discord = FakeDiscord()
        code, out = self.run_sync(FakeGitHub([], []), apply=True, discord=discord)
        self.assertEqual(discord.calls[0][:2], ("create", "Release status"))
        self.assertIn("RELEASE_STATUS_MESSAGE_ID to 777", out)

    def test_dry_run_previews_the_post_without_sending(self):
        discord = FakeDiscord()
        gh = FakeGitHub([item(1, status="Proposed", item_id="P1")], [item(1)])
        code, out = self.run_sync(gh, discord=discord, post_id="999")
        self.assertEqual(discord.calls, [])
        self.assertIn("## Discord post (preview, not sent)", out)
        self.assertIn("2.5.2 · 1 item", out)

    def test_scheduled_run_without_post_id_does_not_create_one(self):
        discord = FakeDiscord()
        code, out = self.run_sync(FakeGitHub([], []), apply=True, discord=discord, allow_create=False)
        self.assertEqual((code, discord.calls), (0, []))
        self.assertIn("run the workflow by hand once", out)

    def test_a_crash_building_the_post_never_blocks_the_sync(self):
        gh = FakeGitHub([item(2, labelled=False, status="Proposed", item_id="P2")], [])
        with mock.patch.object(sync, "build_message", side_effect=RuntimeError("boom")):
            code, out = self.run_sync(gh, apply=True, discord=FakeDiscord(), post_id="999")
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in gh.calls], ["archive_item"])
        self.assertIn("::warning::Discord update failed", out)

    def test_a_crash_building_the_preview_is_a_warning(self):
        with mock.patch.object(sync, "build_message", side_effect=RuntimeError("boom")):
            code, out = self.run_sync(FakeGitHub([], []), discord=FakeDiscord(), post_id="999")
        self.assertEqual(code, 0)
        self.assertIn("::warning::Discord preview failed: boom", out)

    def test_discord_failure_is_a_warning(self):
        code, out = self.run_sync(FakeGitHub([], []), apply=True, discord=FakeDiscord(fail=True), post_id="999")
        self.assertEqual(code, 0)
        self.assertIn("::warning::Discord update failed", out)

    def test_no_discord_update_after_a_failed_mutation(self):
        discord = FakeDiscord()
        gh = FakeGitHub([item(2, labelled=False, status="Proposed", item_id="P2")], [], fail_on="archive_item")
        code, out = self.run_sync(gh, apply=True, discord=discord, post_id="999")
        self.assertEqual((code, discord.calls), (1, []))

    def test_reverted_pick_and_unmatched_commit_reach_the_post(self):
        discord = FakeDiscord()
        gh = FakeGitHub([item(1, state="MERGED", status="Picked", item_id="P1")], [item(1, state="MERGED")])
        self.run_sync(gh, apply=True, discord=discord, post_id="999",
                      picks=[Pick("aaaaaaaaaaaa", "Translations refresh", None, "rc/v2.5.2")],
                      reverted=[Pick("cccccccccccc", "Fix one (#1)", 1, "rc/v2.5.2")])
        text = discord.calls[0][2]["embeds"][0]["description"]
        self.assertIn("reverted on rc/v2.5.2", text)
        self.assertIn("**Unmatched commits on rc/v2.5.2**", text)

    def test_dry_run_prints_plan_and_mutates_nothing(self):
        gh = FakeGitHub([item(1, state="MERGED", status="Ready to pick", item_id="P1"),
                         item(2, labelled=False, status="Proposed", item_id="P2")],
                        [item(1, state="MERGED"), item(3, kind="Issue")])
        code, out = self.run_sync(gh, picks=[Pick("s1", "Fix (#1)", 1, "rc/v2.5.2")])
        self.assertEqual(code, 0)
        self.assertEqual(gh.calls, [])
        self.assertIn("- #1: Ready to pick → Picked (on rc/v2.5.2)", out)
        self.assertIn("- #2: archived (no release-pick label)", out)
        self.assertIn("- #3: added as Proposed", out)
        self.assertTrue(out.rstrip().endswith("Dry run: no changes applied."))

    def test_apply_adds_then_sets_status_on_new_item_id(self):
        gh = FakeGitHub([], [item(3, kind="Issue")])
        code, out = self.run_sync(gh, apply=True)
        self.assertEqual(code, 0)
        self.assertEqual(gh.calls, [("add_item", "PVT", "C_3"), ("set_status", "PVT", "NEW_C_3", "FIELD", "o1")])
        self.assertTrue(out.rstrip().endswith("Applied 2 of 2 changes."))

    def test_no_mutation_when_read_fails(self):
        gh = FakeGitHub([item(2, labelled=False, status="Proposed", item_id="P2")], [], fail_on="labelled_items")
        code, out = self.run_sync(gh, apply=True)
        self.assertEqual(code, 1)
        self.assertEqual(gh.calls, [])
        self.assertIn("boom", out)

    def test_stops_at_first_failed_mutation(self):
        gh = FakeGitHub([item(2, labelled=False, status="Proposed", item_id="P2"),
                         item(4, labelled=False, status="Proposed", item_id="P4")],
                        [item(3, kind="Issue")], fail_on="archive_item")
        code, out = self.run_sync(gh, apply=True)
        self.assertEqual(code, 1)
        self.assertEqual([c[0] for c in gh.calls], ["add_item", "archive_item"])
        self.assertIn("Applied 1 of 4 changes.", out)

    def test_unmatched_and_reverted_picks_are_reported(self):
        gh = FakeGitHub([item(1, state="MERGED", status="Picked", item_id="P1")], [item(1, state="MERGED")])
        code, out = self.run_sync(gh, picks=[Pick("aaaaaaaaaaaa", "Translations refresh", None, "rc/v2.5.2"),
                                             Pick("bbbbbbbbbbbb", "Fix other (#99)", 99, "rc/v2.5.2")],
                                  reverted=[Pick("cccccccccccc", "Fix one (#1)", 1, "rc/v2.5.2")])
        self.assertIn("## Unmatched commits on rc/v2.5.2", out)
        self.assertIn("- aaaaaaaaa Translations refresh", out)
        self.assertIn("- bbbbbbbbb Fix other (#99)", out)
        self.assertIn("- #1: reverted on rc/v2.5.2 (ccccccccc Fix one (#1))", out)
        self.assertIn("- #1: Picked → Ready to pick (pick reverted)", out)

    def test_reverted_reworded_pick_resolves_pr_through_commit_api(self):
        gh = FakeGitHub([item(9, state="MERGED", status="Picked", item_id="P9")], [item(9, state="MERGED")],
                        commit_prs={"mainsha": [9]})
        code, out = self.run_sync(gh, reverted=[Pick("ffffffffffff", "Fix reworded", None, "rc/v2.5.2")],
                                  matches={"ffffffffffff": "mainsha"})
        self.assertIn("- #9: reverted on rc/v2.5.2 (fffffffff Fix reworded)", out)
        self.assertIn("- #9: Picked → Ready to pick (pick reverted)", out)
        self.assertIn("ffffffffffff", [p.sha for p in sync.patch_id_matches.call_args.args[1]])

    def test_unresolved_reverted_pick_is_dropped(self):
        gh = FakeGitHub([item(9, state="MERGED", status="Picked", item_id="P9")], [item(9, state="MERGED")])
        code, out = self.run_sync(gh, reverted=[Pick("ffffffffffff", "Fix reworded", None, "rc/v2.5.2")])
        self.assertNotIn("reverted on", out)
        self.assertIn("## Planned changes\nnone", out)

    def test_pr_picked_again_after_revert_needs_no_attention(self):
        gh = FakeGitHub([item(1, state="MERGED", status="Picked", item_id="P1")], [item(1, state="MERGED")])
        code, out = self.run_sync(gh, picks=[Pick("dddddddddddd", "Fix one (#1)", 1, "rc/v2.5.2")],
                                  reverted=[Pick("cccccccccccc", "Fix one (#1)", 1, "rc/v2.5.2")])
        self.assertNotIn("reverted on", out)
        self.assertIn("## Planned changes\nnone", out)

    def test_pick_matched_through_linked_issue_is_not_unmatched(self):
        issue = item(5, kind="Issue", state="CLOSED", status="Ready to pick", item_id="P5",
                     linked=(LinkedPr(6, "MERGED", "2.5.2"),))
        gh = FakeGitHub([issue], [issue])
        code, out = self.run_sync(gh, picks=[Pick("dddddddddddd", "Fix (#6)", 6, "rc/v2.5.2")])
        self.assertIn("- #5: Ready to pick → Picked (on rc/v2.5.2)", out)
        self.assertIn("## Unmatched commits\nnone", out)

    def test_patch_id_match_resolves_pr_through_commit_api(self):
        gh = FakeGitHub([item(7, state="MERGED", status="Ready to pick", item_id="P7")], [item(7, state="MERGED")],
                        commit_prs={"mainsha": [7]})
        code, out = self.run_sync(gh, picks=[Pick("eeeeeeeeeeee", "Fix reworded", None, "rc/v2.5.2")],
                                  matches={"eeeeeeeeeeee": "mainsha"})
        self.assertIn("- #7: Ready to pick → Picked (on rc/v2.5.2)", out)

    def test_closed_without_merge_needs_attention(self):
        gh = FakeGitHub([item(8, state="CLOSED", status="In progress", item_id="P8")], [item(8, state="CLOSED")])
        code, out = self.run_sync(gh)
        self.assertIn("- #8: closed without a merge", out)


if __name__ == "__main__":
    unittest.main()
