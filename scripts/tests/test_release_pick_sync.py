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

    def remove_item(self, project_id, item_id):
        self._mutate("remove_item", project_id, item_id)

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


class RunTest(unittest.TestCase):
    def run_sync(self, gh, apply=False, **git):
        out = io.StringIO()
        patches = git_stubs(**git)
        for p in patches:
            p.start()
        self.addCleanup(lambda: [p.stop() for p in patches])
        code = sync.run(gh, ".", "OrcaSlicer", 15, REPO, apply, out)
        return code, out.getvalue()

    def test_dry_run_prints_plan_and_mutates_nothing(self):
        gh = FakeGitHub([item(1, state="MERGED", status="Ready to pick", item_id="P1"),
                         item(2, labelled=False, status="Proposed", item_id="P2")],
                        [item(1, state="MERGED"), item(3, kind="Issue")])
        code, out = self.run_sync(gh, picks=[Pick("s1", "Fix (#1)", 1, "rc/v2.5.2")])
        self.assertEqual(code, 0)
        self.assertEqual(gh.calls, [])
        self.assertIn("- #1: Ready to pick → Picked (on rc/v2.5.2)", out)
        self.assertIn("- #2: removed (no release-pick label)", out)
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
                        [item(3, kind="Issue")], fail_on="remove_item")
        code, out = self.run_sync(gh, apply=True)
        self.assertEqual(code, 1)
        self.assertEqual([c[0] for c in gh.calls], ["add_item", "remove_item"])
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
