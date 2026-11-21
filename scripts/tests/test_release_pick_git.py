#!/usr/bin/env python3
"""Tests for scripts/release_pick_git.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts -v
"""

import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from release_pick_git import (  # noqa: E402
    branch_base,
    branch_picks,
    patch_id_matches,
    pr_number_from_subject,
    release_branches,
    remote_branches,
    reverted_number,
)


class SubjectTest(unittest.TestCase):
    def test_trailing_number_is_read(self):
        self.assertEqual(pr_number_from_subject("Fix Cut gizmo right clicks (#17200)"), 17200)

    def test_revert_subject_gives_its_own_number(self):
        self.assertEqual(pr_number_from_subject('Revert "Fix X (#17200)" (#17300)'), 17300)
        self.assertIsNone(pr_number_from_subject('Revert "Fix X (#17200)"'))
        self.assertEqual(pr_number_from_subject('Revert "Revert "Fix X (#17200)" (#17300)" (#17400)'), 17400)
        self.assertEqual(reverted_number('Revert "Fix X (#17200)" (#17300)'), 17200)
        self.assertEqual(reverted_number('Revert "Revert "Fix X (#17200)" (#17300)"'), 17300)
        self.assertEqual(reverted_number('Revert "Fix X (#17200)"'), 17200)
        self.assertIsNone(reverted_number("Fix X (#17200)"))

    def test_only_trailing_number_counts(self):
        self.assertIsNone(pr_number_from_subject("Adapt #17336 for release/v2.5"))
        self.assertEqual(pr_number_from_subject("Follow-up to (#1) cleanup (#2)"), 2)


def git(cwd, *args):
    return subprocess.run(["git", "-C", cwd, *args], check=True, text=True, capture_output=True).stdout.strip()


class ScratchRepoTest(unittest.TestCase):
    """origin: main with tag v1.0.0, then main moves on; rc/v1.0.1 cherry-picks from main after v1.0.0;
    release/v1.1.0 branches from main after v1.0.0."""

    def commit(self, path, text, subject):
        with open(os.path.join(self.work, path), "w") as f:
            f.write(text)
        git(self.work, "add", path)
        git(self.work, "commit", "-qm", subject)
        return git(self.work, "rev-parse", "HEAD")

    def push_rc(self):
        git(self.work, "push", "-q", "origin", "rc/v1.0.1")
        git(self.work, "fetch", "-q", "origin")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        origin = os.path.join(self.tmp.name, "origin.git")
        self.work = os.path.join(self.tmp.name, "work")
        git(self.tmp.name, "init", "-q", "--bare", origin)
        git(self.tmp.name, "init", "-q", "-b", "main", self.work)
        for key, value in (("user.name", "T"), ("user.email", "t@example.invalid"), ("commit.gpgsign", "false")):
            git(self.work, "config", key, value)
        git(self.work, "remote", "add", "origin", origin)

        self.commit("a.txt", "a\n", "Base")
        git(self.work, "tag", "v1.0.0")
        git(self.work, "tag", "v1.0.0-rc")
        self.fix_a = self.commit("a.txt", "a\nfix\n", "Fix A (#5)")
        self.fix_b = self.commit("b.txt", "b\n", "Fix B (#6)")
        git(self.work, "branch", "release/v1.1.0")
        self.commit("c.txt", "c\n", "Fix C (#7)")

        git(self.work, "checkout", "-q", "-b", "rc/v1.0.1", "v1.0.0")
        git(self.work, "cherry-pick", "-x", self.fix_a)
        git(self.work, "commit", "-q", "--amend", "-m", "Fix A for the release")
        git(self.work, "cherry-pick", self.fix_b)
        self.commit("version.txt", "1.0.1\n", "Set the version to 1.0.1-rc")
        git(self.work, "tag", "v1.0.1")
        git(self.work, "checkout", "-q", "-b", "side")
        self.commit("side.txt", "s\n", "Side work")
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        git(self.work, "merge", "-q", "--no-ff", "-m", "Merge something", "side")
        git(self.work, "checkout", "-q", "main")
        git(self.work, "push", "-q", "origin", "main", "rc/v1.0.1", "release/v1.1.0", "--tags")
        git(self.work, "fetch", "-q", "origin")

    def test_remote_and_release_branches(self):
        self.assertEqual(remote_branches(self.work), {"main", "rc/v1.0.1", "release/v1.1.0"})
        self.assertEqual(branch_base(self.work, "1.0.1", "rc/v1.0.1"), "v1.0.0")
        self.assertEqual(release_branches("1.0.1", remote_branches(self.work)), ["rc/v1.0.1"])
        self.assertEqual(release_branches("1.0.1", {"rc/v1.0.1", "release/v1.0.1", "release/v1.0.0"}),
                         ["rc/v1.0.1", "release/v1.0.1"])

    def test_base_is_previous_stable_tag(self):
        self.assertEqual(branch_base(self.work, "1.0.1", "rc/v1.0.1"), "v1.0.0")

    def test_base_falls_back_to_merge_base(self):
        # v1.0.1 sits on the rc branch, not in release/v1.1.0's history
        self.assertEqual(branch_base(self.work, "1.1.0", "release/v1.1.0"),
                         git(self.work, "rev-parse", "origin/release/v1.1.0"))

    def test_branch_picks_skip_merges_and_version_bumps(self):
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        subjects = [p.subject for p in picks]
        self.assertNotIn("Set the version to 1.0.1-rc", subjects)
        self.assertNotIn("Merge something", subjects)
        self.assertEqual({(p.subject, p.pr) for p in picks if p.subject.startswith("Fix A")},
                         {("Fix A for the release", None)})
        self.assertEqual(reverted, [])
        self.assertTrue(all(p.branch == "rc/v1.0.1" for p in picks))
        newest_first = git(self.work, "log", "--no-merges", "--format=%H", "v1.0.0..origin/rc/v1.0.1").split()
        bump = git(self.work, "log", "--format=%H", "--grep=^Set the version", "-n1", "origin/rc/v1.0.1")
        self.assertEqual([p.sha for p in picks], [s for s in newest_first if s != bump])

    def test_reverted_pick_is_not_picked(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-edit", fix_b_pick)
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertNotIn(6, [p.pr for p in picks])
        self.assertEqual([p.pr for p in reverted], [6])

    def test_reverted_reworded_pick_is_matched_by_commit(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        reworded = git(self.work, "log", "--format=%H", "--grep=^Fix A for the release", "-n1")
        git(self.work, "revert", "--no-edit", reworded)
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertNotIn(reworded, [p.sha for p in picks])
        self.assertEqual([p.sha for p in reverted], [reworded])

    def test_pick_again_after_revert_counts_as_picked(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-edit", fix_b_pick)
        git(self.work, "cherry-pick", self.fix_b)
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.sha for p in reverted], [fix_b_pick])
        self.assertIn(6, [p.pr for p in picks])

    def test_revert_without_commit_line_is_matched_by_number(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-commit", fix_b_pick)
        git(self.work, "commit", "-qm", 'Revert "Fix B (#6)"')
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.sha for p in reverted], [fix_b_pick])
        self.assertNotIn(6, [p.pr for p in picks])

    def test_revert_pull_request_is_a_pick(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-commit", fix_b_pick)
        git(self.work, "commit", "-qm", 'Revert "Fix B (#6)" (#8)')
        # A revert pull request from main whose reverted change was never picked here
        self.commit("c.txt", "", 'Revert "Fix C (#7)" (#9)')
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.sha for p in reverted], [fix_b_pick])
        self.assertEqual([p.pr for p in picks][:2], [9, 8])
        self.assertNotIn(6, [p.pr for p in picks])

    def test_reverting_a_revert_pull_request_reverts_it(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-commit", fix_b_pick)
        git(self.work, "commit", "-qm", 'Revert "Fix B (#6)" (#8)', "-m", f"This reverts commit {fix_b_pick}.")
        revert = git(self.work, "rev-parse", "HEAD")
        git(self.work, "revert", "--no-commit", revert)
        git(self.work, "commit", "-qm", 'Reapply "Fix B (#6)" (#9)', "-m", f"This reverts commit {revert}.")
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.pr for p in reverted], [8])
        self.assertEqual([p.pr for p in picks][:2], [9, 6])

    def test_reverting_the_revert_restores_the_pick(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-edit", fix_b_pick)
        git(self.work, "revert", "--no-edit", "HEAD")
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual(reverted, [])
        self.assertIn(fix_b_pick, [p.sha for p in picks])

    def test_reverting_the_reapply_reverts_the_pick_again(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-edit", fix_b_pick)
        git(self.work, "revert", "--no-edit", "HEAD")
        git(self.work, "revert", "--no-edit", "HEAD")
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.sha for p in reverted], [fix_b_pick])
        self.assertNotIn(fix_b_pick, [p.sha for p in picks])

    def test_revert_of_a_reapply_already_undone_by_number_is_ignored(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        fix_b_pick = git(self.work, "log", "--format=%H", "--grep=^Fix B", "-n1")
        git(self.work, "revert", "--no-edit", fix_b_pick)
        git(self.work, "revert", "--no-edit", "HEAD")
        reapply = git(self.work, "rev-parse", "HEAD")
        git(self.work, "revert", "--no-commit", reapply)
        git(self.work, "commit", "-qm", 'Revert "Fix B (#6)"')
        # The change is already gone, so the revert of the reapply is empty; only its message matters here.
        git(self.work, "commit", "-q", "--allow-empty", "-m", 'Revert "Reapply "Fix B (#6)""',
            "-m", f"This reverts commit {reapply}.")
        self.push_rc()
        picks, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.sha for p in reverted], [fix_b_pick])
        self.assertNotIn(6, [p.pr for p in picks])

    def test_reverted_picks_are_newest_first(self):
        git(self.work, "checkout", "-q", "rc/v1.0.1")
        picks, _ = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        fix_b_pick = next(p.sha for p in picks if p.pr == 6)
        reworded = next(p.sha for p in picks if p.subject == "Fix A for the release")
        git(self.work, "revert", "--no-edit", reworded)
        git(self.work, "revert", "--no-edit", fix_b_pick)
        self.push_rc()
        _, reverted = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        self.assertEqual([p.sha for p in reverted], [fix_b_pick, reworded])

    def test_patch_id_matches_reworded_cherry_pick(self):
        picks, _ = branch_picks(self.work, "v1.0.0", "rc/v1.0.1")
        reworded = next(p for p in picks if p.subject == "Fix A for the release")
        side = next(p for p in picks if p.subject == "Side work")
        self.assertEqual(patch_id_matches(self.work, [reworded, side]), {reworded.sha: self.fix_a})


if __name__ == "__main__":
    unittest.main()
