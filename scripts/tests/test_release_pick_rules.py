#!/usr/bin/env python3
"""Tests for scripts/release_pick_rules.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from release_pick_rules import (  # noqa: E402
    Item,
    LinkedPr,
    derived_status,
    needs_attention,
    parse_version,
    plan_changes,
    project_after,
)

OPEN = ["2.5.2", "2.5.3"]


def pr(number, state="OPEN", milestone="2.5.2", milestone_open=True, labelled=True, **kw):
    return Item(number=number, kind="PullRequest", state=state, labelled=labelled, milestone=milestone,
                milestone_open=milestone_open, content_id=f"PR_{number}", **kw)


def issue(number, state="OPEN", milestone="2.5.2", milestone_open=True, labelled=True, **kw):
    return Item(number=number, kind="Issue", state=state, labelled=labelled, milestone=milestone,
                milestone_open=milestone_open, content_id=f"I_{number}", **kw)


def in_project(item, status, archived=False):
    return Item(**{**item.__dict__, "item_id": f"PVTI_{item.number}", "status": status, "archived": archived})


def summary(changes):
    return [(c.action, c.number, c.status) for c in changes]


class ParseVersionTest(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(parse_version("2.5.2"), (2, 5, 2))
        self.assertIsNone(parse_version("2.4"))
        self.assertIsNone(parse_version("1.9.0 beta"))


class PlanChangesTest(unittest.TestCase):
    def test_labelled_open_milestone_item_is_added_as_proposed(self):
        changes = plan_changes([], [issue(1)], {}, OPEN)
        self.assertEqual(summary(changes), [("add", 1, None), ("set_status", 1, "Proposed")])
        self.assertEqual(changes[0].content_id, "I_1")
        self.assertIsNone(changes[1].item_id)

    def test_labelled_item_without_milestone_is_added(self):
        changes = plan_changes([], [issue(2, milestone=None, milestone_open=False)], {}, OPEN)
        self.assertEqual(summary(changes), [("add", 2, None), ("set_status", 2, "Proposed")])

    def test_closed_milestone_item_is_not_added(self):
        old = pr(3, state="MERGED", milestone="2.5.1", milestone_open=False)
        self.assertEqual(plan_changes([], [old], {}, OPEN), [])

    def test_unlabelled_item_is_archived_even_on_closed_milestone(self):
        open_one = in_project(issue(4, labelled=False, milestone=None, milestone_open=False), "Proposed")
        old_one = in_project(pr(5, state="MERGED", labelled=False, milestone="2.5.1", milestone_open=False),
                             "Ready to pick")
        changes = plan_changes([open_one, old_one], [], {}, OPEN)
        self.assertEqual(summary(changes), [("archive", 4, None), ("archive", 5, None)])
        self.assertEqual(changes[0].item_id, "PVTI_4")

    def test_picked_item_without_label_is_kept(self):
        item = in_project(pr(6, state="MERGED", labelled=False), "Picked")
        self.assertEqual(plan_changes([item], [], {}, OPEN), [])

    def test_archived_item_is_left_alone(self):
        unlabelled = in_project(issue(7, labelled=False), "Proposed", archived=True)
        stale = in_project(pr(8, state="MERGED"), "In progress", archived=True)
        self.assertEqual(plan_changes([unlabelled, stale], [stale], {}, OPEN), [])

    def test_status_never_moves_backwards(self):
        picked = in_project(pr(9, state="MERGED"), "Picked")
        ahead = in_project(pr(10), "Ready to pick")
        self.assertEqual(plan_changes([picked, ahead], [picked, ahead], {}, OPEN), [])

    def test_unknown_status_is_left_alone(self):
        item = in_project(pr(14, state="MERGED"), "On hold")
        self.assertEqual(plan_changes([item], [item], {}, OPEN), [])

    def test_unlabelled_item_already_on_release_branch_is_kept_as_picked(self):
        item = in_project(pr(15, state="MERGED", labelled=False), "Ready to pick")
        changes = plan_changes([item], [], {"2.5.2": {15: "rc/v2.5.2"}}, OPEN)
        self.assertEqual(summary(changes), [("set_status", 15, "Picked")])

    def test_reverted_pick_moves_back_from_picked(self):
        item = in_project(pr(16, state="MERGED"), "Picked")
        changes = plan_changes([item], [item], {}, OPEN, {("2.5.2", 16)})
        self.assertEqual(summary(changes), [("set_status", 16, "Ready to pick")])
        self.assertEqual(changes[0].reason, "pick reverted")

    def test_issue_whose_pr_pick_was_reverted_moves_back(self):
        item = in_project(issue(17, state="CLOSED", linked_prs=(LinkedPr(18, "MERGED"),)), "Picked")
        changes = plan_changes([item], [item], {}, OPEN, {("2.5.2", 18)})
        self.assertEqual(summary(changes), [("set_status", 17, "Ready to pick")])

    def test_revert_on_another_release_leaves_picked_alone(self):
        item = in_project(pr(19, state="MERGED", milestone="2.5.1", milestone_open=False), "Picked")
        self.assertEqual(plan_changes([item], [item], {}, OPEN, {("2.5.2", 19)}), [])

    def test_picked_again_after_revert_stays_picked(self):
        item = in_project(pr(21, state="MERGED"), "Picked")
        self.assertEqual(plan_changes([item], [item], {"2.5.2": {21: "rc/v2.5.2"}}, OPEN, {("2.5.2", 21)}), [])

    def test_unlabelled_reverted_pick_is_archived(self):
        item = in_project(pr(22, state="MERGED", labelled=False), "Picked")
        self.assertEqual(summary(plan_changes([item], [], {}, OPEN, {("2.5.2", 22)})), [("archive", 22, None)])

    def test_revert_does_not_touch_an_item_short_of_picked(self):
        item = in_project(pr(23, state="MERGED"), "In progress")
        changes = plan_changes([item], [item], {}, OPEN, {("2.5.2", 23)})
        self.assertEqual([(c.status, c.reason) for c in changes], [("Ready to pick", "#23 merged")])

    def test_status_moves_forward(self):
        item = in_project(pr(11, state="MERGED"), "In progress")
        changes = plan_changes([item], [item], {}, OPEN)
        self.assertEqual(summary(changes), [("set_status", 11, "Ready to pick")])
        self.assertEqual((changes[0].item_id, changes[0].old_status), ("PVTI_11", "In progress"))

    def test_pick_on_other_milestone_branch_is_ignored(self):
        item = in_project(pr(12, state="MERGED", milestone="2.5.3"), "Ready to pick")
        self.assertEqual(plan_changes([item], [item], {"2.5.2": {12: "rc/v2.5.2"}}, OPEN), [])

    def test_non_version_milestone_counts_as_open(self):
        item = issue(13, milestone="1.9.0 beta")
        changes = plan_changes([], [item], {}, ["1.9.0 beta", "2.5.2"])
        self.assertEqual(summary(changes), [("add", 13, None), ("set_status", 13, "Proposed")])

    def test_adds_then_archives_then_statuses(self):
        new = issue(30)
        gone = in_project(issue(20, labelled=False), "Proposed")
        moved = in_project(pr(10, state="MERGED"), "In progress")
        changes = plan_changes([gone, moved], [new, moved], {}, OPEN)
        self.assertEqual([c.action for c in changes], ["add", "archive", "set_status", "set_status"])
        self.assertEqual([c.number for c in changes[2:]], [10, 30])


class ProjectAfterTest(unittest.TestCase):
    def test_changes_are_applied(self):
        kept = in_project(pr(1, state="MERGED"), "In progress")
        gone = in_project(issue(2, labelled=False), "Proposed")
        old = in_project(pr(3, state="MERGED", milestone="2.5.1", milestone_open=False), "Picked", archived=True)
        new = issue(4)
        changes = plan_changes([kept, gone, old], [kept, new], {}, OPEN)
        after = {i.number: i.status for i in project_after([kept, gone, old], [kept, new], changes)}
        self.assertEqual(after, {1: "Ready to pick", 4: "Proposed"})


class DerivedStatusTest(unittest.TestCase):
    def test_open_issue_without_pr_is_proposed(self):
        self.assertEqual(derived_status(issue(1), {}, OPEN)[0], "Proposed")

    def test_open_pr_is_in_progress(self):
        self.assertEqual(derived_status(pr(1), {}, OPEN)[0], "In progress")

    def test_issue_with_open_and_merged_prs_takes_furthest(self):
        item = issue(1, linked_prs=(LinkedPr(2, "OPEN", "2.5.2"), LinkedPr(3, "MERGED", "2.5.2")))
        self.assertEqual(derived_status(item, {}, OPEN)[0], "Ready to pick")

    def test_merged_pr_is_ready_to_pick(self):
        self.assertEqual(derived_status(pr(1, state="MERGED"), {}, OPEN)[0], "Ready to pick")

    def test_merged_pr_on_own_milestone_branch_is_picked(self):
        self.assertEqual(derived_status(pr(1, state="MERGED"), {"2.5.2": {1: "rc/v2.5.2"}}, OPEN),
                         ("Picked", "on rc/v2.5.2"))

    def test_issue_whose_merged_pr_was_picked_is_picked(self):
        item = issue(1, linked_prs=(LinkedPr(2, "MERGED", "2.5.2"),))
        self.assertEqual(derived_status(item, {"2.5.2": {2: "rc/v2.5.2"}}, OPEN)[0], "Picked")

    def test_issue_without_milestone_uses_its_pr_milestone(self):
        item = issue(1, milestone=None, milestone_open=False, linked_prs=(LinkedPr(2, "MERGED", "2.5.3"),))
        self.assertEqual(derived_status(item, {"2.5.2": {2: "rc/v2.5.2"}}, OPEN)[0], "Ready to pick")
        self.assertEqual(derived_status(item, {"2.5.3": {2: "rc/v2.5.3"}}, OPEN)[0], "Picked")

    def test_item_without_any_milestone_uses_any_open_milestone(self):
        item = pr(1, state="MERGED", milestone=None, milestone_open=False)
        self.assertEqual(derived_status(item, {"2.5.3": {1: "rc/v2.5.3"}}, OPEN)[0], "Picked")

    def test_closed_unmerged_pr_needs_attention(self):
        closed = pr(1, state="CLOSED")
        self.assertIsNone(derived_status(closed, {}, OPEN))
        self.assertEqual(needs_attention([in_project(closed, "In progress"), in_project(pr(2), "In progress")]),
                         [1])


if __name__ == "__main__":
    unittest.main()
