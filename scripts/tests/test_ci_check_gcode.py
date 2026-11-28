#!/usr/bin/env python3
"""Tests for scripts/ci_check_gcode.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest
import urllib.error
import zipfile
from unittest import mock

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import ci_check_gcode as cc  # noqa: E402

REPO = "owner/repo"
LIMIT = 1000


def make_zip(members, compression=zipfile.ZIP_DEFLATED):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression) as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    return buffer.getvalue()


class FakeApi:
    """Answers get() from a dict of path -> body (or a function of the params), get_bytes() from blobs."""

    def __init__(self, gets=None, blobs=None):
        self.gets = gets or {}
        self.blobs = blobs or {}
        self.calls = []

    def get(self, path, **params):
        self.calls.append(("GET", path, params))
        body = self.gets[path]
        return body(params) if callable(body) else body

    def get_bytes(self, path, limit):
        self.calls.append(("BYTES", path, limit))
        return self.blobs[path]


def artifact(artifact_id, size=100, expired=False):
    return {"id": artifact_id, "name": cc.ARTIFACT, "size_in_bytes": size, "expired": expired}


def commit(sha, title):
    return {"sha": sha, "commit": {"message": f"{title}\n\nBody text."}}


def run(run_id, sha, event="push"):
    return {"id": run_id, "head_sha": sha, "event": event}


class ReadArtifactTest(unittest.TestCase):
    def api_with(self, data, size=None):
        art = artifact(7, size=len(data) if size is None else size)
        return FakeApi(blobs={f"repos/{REPO}/actions/artifacts/7/zip": data}), art

    def test_reads_the_named_members_and_leaves_out_missing_ones(self):
        api, art = self.api_with(make_zip({"fingerprints.tsv": "a\tb\t\n", "made_with.txt": "x\n"}))
        got = cc.read_artifact(api, REPO, art, ["fingerprints.tsv", "report.txt"], LIMIT)
        self.assertEqual(got, {"fingerprints.tsv": b"a\tb\t\n"})

    def test_a_member_larger_than_the_limit_is_refused_whatever_the_directory_says(self):
        data = make_zip({"report.txt": "x" * (LIMIT + 1)})
        # The directory's sizes come from the pull request, so they are not trusted.
        forged = data.replace((LIMIT + 1).to_bytes(4, "little"), (10).to_bytes(4, "little"))
        api, art = self.api_with(forged)
        with self.assertRaises(cc.ArtifactError):
            cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)

    def test_a_member_honestly_larger_than_the_limit_is_refused(self):
        api, art = self.api_with(make_zip({"report.txt": "x" * (LIMIT + 1)}))
        with self.assertRaises(cc.ArtifactError):
            cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)

    def test_a_member_is_never_decompressed_past_the_limit(self):
        # A zip bomb must not be inflated whole before its size is checked.
        sizes = []
        real_read = zipfile.ZipExtFile.read

        def read(member, n=-1):
            sizes.append(n)
            return real_read(member, n)

        api, art = self.api_with(make_zip({"report.txt": "x" * (LIMIT * 50)}))
        with mock.patch.object(zipfile.ZipExtFile, "read", read), self.assertRaises(cc.ArtifactError):
            cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)
        self.assertEqual(sizes, [LIMIT + 1])

    def test_a_corrupt_or_encrypted_member_is_an_artifact_error(self):
        data = bytearray(make_zip({"report.txt": "z" * 500}))
        local = data.index(b"PK\x03\x04")
        start = local + 30 + int.from_bytes(data[local + 26:local + 28], "little")
        corrupt = bytearray(data)
        corrupt[start:start + 4] = b"\xff\xff\xff\xff"
        # Bit 0 of the general purpose flags, in both headers, marks a member as encrypted.
        encrypted = bytearray(data)
        central = encrypted.index(b"PK\x01\x02")
        encrypted[local + 6] |= 1
        encrypted[central + 8] |= 1
        for bad in (bytes(corrupt), bytes(encrypted)):
            api, art = self.api_with(bad)
            with self.assertRaises(cc.ArtifactError):
                cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)

    def test_an_artifact_over_the_size_cap_is_refused_before_it_is_downloaded(self):
        api, art = self.api_with(b"", size=cc.ZIP_FACTOR * LIMIT + 1)
        with self.assertRaises(cc.ArtifactError):
            cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)
        self.assertEqual([c for c in api.calls if c[0] == "BYTES"], [])

    def test_the_download_itself_is_capped(self):
        api, art = self.api_with(make_zip({"report.txt": "ok"}))
        cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)
        self.assertEqual(api.calls[-1][2], cc.ZIP_FACTOR * LIMIT)

    def test_a_member_not_asked_for_is_never_opened(self):
        api, art = self.api_with(make_zip({"report.txt": "ok", "other.bin": "y" * (LIMIT * 3)}))
        self.assertEqual(cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT), {"report.txt": b"ok"})

    def test_a_member_compressed_with_bzip2_or_lzma_is_refused(self):
        # read(limit + 1) does not bound how much of such a member is decompressed.
        for compression in (zipfile.ZIP_BZIP2, zipfile.ZIP_LZMA):
            api, art = self.api_with(make_zip({"report.txt": "ok"}, compression))
            with self.assertRaises(cc.ArtifactError):
                cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)

    def test_a_stored_member_is_read(self):
        api, art = self.api_with(make_zip({"report.txt": "ok"}, zipfile.ZIP_STORED))
        self.assertEqual(cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT), {"report.txt": b"ok"})

    def test_a_file_that_is_not_a_zip_is_an_artifact_error(self):
        api, art = self.api_with(b"not a zip")
        with self.assertRaises(cc.ArtifactError):
            cc.read_artifact(api, REPO, art, ["report.txt"], LIMIT)


class FindArtifactTest(unittest.TestCase):
    def test_skips_an_expired_artifact(self):
        api = FakeApi(gets={f"repos/{REPO}/actions/runs/5/artifacts": {
            "artifacts": [artifact(1, expired=True), artifact(2)]}})
        self.assertEqual(cc.find_artifact(api, REPO, 5)["id"], 2)

    def test_none_when_the_run_has_no_artifact(self):
        api = FakeApi(gets={f"repos/{REPO}/actions/runs/5/artifacts": {"artifacts": []}})
        self.assertIsNone(cc.find_artifact(api, REPO, 5))


class FindBaselineTest(unittest.TestCase):
    def api(self, commits, runs, artifacts):
        def runs_of(params):
            # GitHub filters by event; the fake does the same.
            return {"workflow_runs": [r for r in runs if r["event"] == params["event"]]}
        gets = {
            f"repos/{REPO}/commits": commits,
            f"repos/{REPO}/actions/workflows/build_all.yml/runs": runs_of,
        }
        for run_id, arts in artifacts.items():
            gets[f"repos/{REPO}/actions/runs/{run_id}/artifacts"] = {"artifacts": arts}
        return FakeApi(gets=gets)

    COMMITS = [commit("c3" * 20, "Third"), commit("c2" * 20, "Second"), commit("c1" * 20, "First")]

    def test_the_base_commit_has_fingerprints(self):
        api = self.api(self.COMMITS, [run(30, "c3" * 20)], {30: [artifact(300)]})
        baseline = cc.find_baseline(api, REPO, "main", "c3" * 20)
        self.assertEqual((baseline.sha, baseline.run_id, baseline.artifact["id"], baseline.between),
                         ("c3" * 20, 30, 300, []))

    def test_an_older_commit_has_them_and_the_commits_after_it_are_listed(self):
        api = self.api(self.COMMITS, [run(10, "c1" * 20)], {10: [artifact(100)]})
        baseline = cc.find_baseline(api, REPO, "main", "c3" * 20)
        self.assertEqual(baseline.sha, "c1" * 20)
        self.assertEqual(baseline.title, "First")
        self.assertEqual(baseline.between, [("c3c3c3c3c3", "Third"), ("c2c2c2c2c2", "Second")])

    def test_a_pull_request_run_of_a_base_commit_is_not_a_baseline(self):
        api = self.api(self.COMMITS, [run(31, "c3" * 20, event="pull_request"), run(20, "c2" * 20)],
                       {31: [artifact(310)], 20: [artifact(200)]})
        self.assertEqual(cc.find_baseline(api, REPO, "main", "c3" * 20).run_id, 20)

    def test_the_newest_run_of_a_commit_is_used(self):
        api = self.api(self.COMMITS, [run(30, "c3" * 20), run(35, "c3" * 20, event="schedule")],
                       {30: [artifact(300)], 35: [artifact(350)]})
        self.assertEqual(cc.find_baseline(api, REPO, "main", "c3" * 20).run_id, 35)

    def test_a_nightly_run_is_a_baseline(self):
        api = self.api(self.COMMITS, [run(40, "c3" * 20, event="schedule")], {40: [artifact(400)]})
        self.assertEqual(cc.find_baseline(api, REPO, "main", "c3" * 20).run_id, 40)

    def test_a_run_without_a_live_artifact_is_skipped(self):
        api = self.api(self.COMMITS, [run(30, "c3" * 20), run(20, "c2" * 20)],
                       {30: [artifact(300, expired=True)], 20: [artifact(200)]})
        self.assertEqual(cc.find_baseline(api, REPO, "main", "c3" * 20).run_id, 20)

    def test_none_when_no_commit_in_reach_has_fingerprints(self):
        api = self.api(self.COMMITS, [run(30, "c3" * 20)], {30: []})
        self.assertIsNone(cc.find_baseline(api, REPO, "main", "c3" * 20))

    def test_asks_for_the_commits_ending_at_the_base_on_the_named_branch(self):
        api = self.api(self.COMMITS, [], {})
        cc.find_baseline(api, REPO, "release/2.5", "c3" * 20)
        self.assertIn(("GET", f"repos/{REPO}/commits", {"sha": "c3" * 20, "per_page": cc.COMMITS_BACK}), api.calls)
        for event in ("push", "schedule"):
            self.assertIn(("GET", f"repos/{REPO}/actions/workflows/build_all.yml/runs",
                           {"branch": "release/2.5", "event": event, "per_page": 100}), api.calls)


def rows(**slices):
    return {name: (digest, "") for name, digest in slices.items()}


MADE_WITH = "fingerprint script abc\nrunner Linux aarch64\n"


class FenceTest(unittest.TestCase):
    def test_a_run_of_backticks_cannot_close_the_fence(self):
        fenced = cc.fence(b"a ```` b ````` c\n")
        body = fenced.split("\n")[1:-2]
        self.assertTrue(fenced.startswith("````\n") and fenced.endswith("\n````\n"))
        self.assertFalse(any("````" in line for line in body))

    def test_control_characters_are_dropped_and_bad_utf8_replaced(self):
        fenced = cc.fence(b"a\x00b\x1b[31mc\td\xff\x7fe\r\n")
        self.assertIn("ab[31mc\td\ufffde\n", fenced)

    def test_bidi_controls_c1_and_line_separators_are_dropped_but_joiners_kept(self):
        fenced = cc.fence("a\u202eb\u0085c\u2066d\u2028e \U0001f469\u200d\U0001f52c\n".encode())
        self.assertIn("abcde \U0001f469\u200d\U0001f52c\n", fenced)

    def test_invisible_characters_are_dropped_but_joiners_kept(self):
        fenced = cc.fence("a\u200bb\u2060c\ufeffd\u206ae\U000e0041f\u200cg\n".encode())
        self.assertIn("abcdef\u200cg\n", fenced)

    def test_a_long_report_is_cut_on_a_line_boundary_inside_the_fence(self):
        fenced = cc.fence(("x" * 99 + "\n").encode() * 1000)
        self.assertLessEqual(len(fenced.encode()), cc.FENCE_LIMIT + 200)
        self.assertIn("report cut", fenced)
        self.assertTrue(fenced.endswith("\n````\n"))
        lines = fenced.split("\n")[1:-3]
        self.assertTrue(all(line == "x" * 99 for line in lines))

    def test_a_single_line_longer_than_the_cut_is_cut_too(self):
        fenced = cc.fence(b"y" * (cc.FENCE_LIMIT * 2))
        self.assertLessEqual(len(fenced.encode()), cc.FENCE_LIMIT + 200)
        self.assertTrue(fenced.endswith("\n````\n"))

    def test_text_without_a_final_newline_still_closes_the_fence(self):
        self.assertEqual(cc.fence(b"abc"), "````\nabc\n````\n")


class BuildReportTest(unittest.TestCase):
    def baseline(self, between=()):
        return cc.Baseline("b" * 40, "Base title", 1, artifact(1), list(between))

    def test_names_the_baseline_and_says_what_changed(self):
        text = cc.build_report(self.baseline(), rows(A__x="1", A__y="2"), rows(A__x="1", A__y="3"),
                               MADE_WITH, MADE_WITH, "main")
        self.assertIn("bbbbbbbbbb (Base title), this pull request's base commit", text)
        # The baseline moves with every push to the branch, so it comes last, after its own line.
        self.assertLess(text.index("G-code changed"), text.index("\n" + cc.BASELINE_LINE))
        self.assertIn("G-code changed for 1 of 2 compared slices", text)
        self.assertNotIn("Warning", text)

    def test_lists_the_commits_between_an_older_baseline_and_the_base(self):
        text = cc.build_report(self.baseline([("c3c3c3c3c3", "Third")]), rows(A__x="1"), rows(A__x="1"),
                               MADE_WITH, MADE_WITH, "main")
        self.assertIn("c3c3c3c3c3 Third", text)
        self.assertIn("show here too", text)
        self.assertNotIn("base commit,", text)

    def test_warns_when_the_script_or_runner_differs(self):
        text = cc.build_report(self.baseline(), rows(A__x="1"), rows(A__x="1"),
                               MADE_WITH, MADE_WITH.replace("aarch64", "x86_64"), "main")
        self.assertIn("Warning", text)

    def test_warns_when_the_baseline_does_not_say_what_made_it(self):
        text = cc.build_report(self.baseline(), rows(A__x="1"), rows(A__x="1"), None, MADE_WITH, "main")
        self.assertIn("Warning", text)

    def test_says_so_when_no_slice_could_be_compared(self):
        text = cc.build_report(self.baseline(), rows(A__x="1"), rows(B__y="1"), MADE_WITH, MADE_WITH, "main")
        self.assertNotIn("the same for all 0", text)
        self.assertIn("No slice could be compared", text)
        self.assertIn("Only in base (1)", text)

    def test_without_a_baseline_says_nothing_was_compared(self):
        text = cc.build_report(None, None, rows(A__x="1"), None, MADE_WITH, "release/2.5")
        self.assertIn("No build of release/2.5", text)
        self.assertIn("nothing was compared", text)


HEAD_REPO = "someone/repo"
SHA = "d" * 40
BOT = {"login": "github-actions[bot]"}


def pull(number=5, labels=(cc.LABEL,), base="main", head_repo=HEAD_REPO, sha=SHA):
    return {"number": number, "labels": [{"name": name} for name in labels], "base": {"ref": base},
            "head": {"sha": sha, "ref": "fix", "repo": {"full_name": head_repo}}}


def pr_run(run_id, status="completed", conclusion="success", head_repo=HEAD_REPO, branch="fix"):
    return {"id": run_id, "status": status, "conclusion": conclusion, "head_branch": branch,
            "head_repository": {"full_name": head_repo}}


class CommentFake(FakeApi):
    """A repository with pull requests, Build all runs, artifacts and the pull request's comments."""

    def __init__(self, pulls, runs=(), artifacts=None, comments=()):
        super().__init__()
        self.comments = [dict(c) for c in comments]
        self.sent = []
        self.gets[f"repos/{REPO}/pulls"] = list(pulls)
        self.gets[f"repos/{REPO}/actions/workflows/build_all.yml/runs"] = {"workflow_runs": list(runs)}
        for run_id, (members, size) in (artifacts or {}).items():
            self.gets[f"repos/{REPO}/actions/runs/{run_id}/artifacts"] = {"artifacts": [artifact(run_id * 10, size)]}
            self.blobs[f"repos/{REPO}/actions/artifacts/{run_id * 10}/zip"] = make_zip(members)
        for run in runs:
            self.gets.setdefault(f"repos/{REPO}/actions/runs/{run['id']}/artifacts", {"artifacts": []})
        self.gets[f"repos/{REPO}/issues/5/comments"] = lambda params: (
            self.comments if params.get("page", 1) == 1 else [])

    def send(self, method, path, body=None):
        self.sent.append((method, path, body))


def report_artifact(report="G-code changed for 1 of 2 compared slices\n", number="5\n"):
    return ({"report.txt": report, "pr_number.txt": number}, 500)


def comment(comment_id, body):
    return {"id": comment_id, "user": BOT, "body": body}


class CommentTest(unittest.TestCase):
    def run_comment(self, api, head_repo=HEAD_REPO):
        with contextlib.redirect_stdout(io.StringIO()):
            cc.comment(api, REPO, head_repo, "fix")
        return api.sent

    def posted(self, sent):
        self.assertEqual(len(sent), 1)
        method, path, body = sent[0]
        self.assertEqual((method, path), ("POST", f"repos/{REPO}/issues/5/comments"))
        return body["body"]

    def test_no_open_pull_request_from_the_branch_does_nothing(self):
        self.assertEqual(self.run_comment(CommentFake([])), [])

    def test_a_pull_request_from_another_repository_with_the_same_branch_name_is_ignored(self):
        api = CommentFake([pull(head_repo="other/repo")], [pr_run(1)], {1: report_artifact()})
        self.assertEqual(self.run_comment(api), [])

    def test_a_pull_request_without_the_label_gets_no_comment(self):
        api = CommentFake([pull(labels=("bug-fix",))], [pr_run(1)], {1: report_artifact()})
        self.assertEqual(self.run_comment(api), [])

    def test_a_pull_request_into_another_branch_is_told_why_there_is_no_report(self):
        api = CommentFake([pull(base="feature/x")], [pr_run(1)], {1: report_artifact()})
        body = self.posted(self.run_comment(api))
        self.assertIn("no `Build all` run", body)
        self.assertNotIn("G-code changed", body)

    def test_a_commit_without_a_build_is_told_why(self):
        body = self.posted(self.run_comment(CommentFake([pull()])))
        self.assertIn("no `Build all` run", body)
        self.assertTrue(body.startswith(cc.MARKER))

    def test_a_build_still_running_waits_for_its_own_completion(self):
        api = CommentFake([pull()], [pr_run(2, status="in_progress"), pr_run(1)], {1: report_artifact()})
        self.assertEqual(self.run_comment(api), [])

    def test_a_build_waiting_for_approval_is_not_finished(self):
        api = CommentFake([pull()], [pr_run(2, conclusion="action_required")], {})
        self.assertEqual(self.run_comment(api), [])

    def test_a_build_without_the_artifact_is_told_why(self):
        body = self.posted(self.run_comment(CommentFake([pull()], [pr_run(1)], {})))
        self.assertIn("made no comparison", body)

    def test_a_run_from_another_repository_at_the_same_commit_is_not_used(self):
        api = CommentFake([pull()], [pr_run(2, head_repo="other/repo")], {2: report_artifact("forged\n")})
        body = self.posted(self.run_comment(api))
        self.assertNotIn("forged", body)
        self.assertIn("no `Build all` run", body)

    def test_a_run_from_another_branch_of_the_same_repository_is_not_used(self):
        api = CommentFake([pull()], [pr_run(2, branch="other")], {2: report_artifact("forged\n")})
        self.assertNotIn("forged", self.posted(self.run_comment(api)))

    def test_a_broken_artifact_falls_through_to_the_note(self):
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact()})
        api.blobs[f"repos/{REPO}/actions/artifacts/10/zip"] = b"not a zip"
        self.assertIn("made no comparison", self.posted(self.run_comment(api)))

    def test_an_artifact_naming_another_pull_request_is_not_used(self):
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact(number="6\n")})
        self.assertIn("made no comparison", self.posted(self.run_comment(api)))

    def test_the_report_is_posted_in_a_fence_for_the_current_head(self):
        body = self.posted(self.run_comment(CommentFake([pull()], [pr_run(1)], {1: report_artifact()})))
        self.assertIn(cc.HEADING, body)
        self.assertIn("own build", body)
        self.assertIn("````\nG-code changed for 1 of 2 compared slices\n````", body)
        self.assertTrue(body.rstrip().endswith(f"<sub>For commit {SHA}.</sub>"))

    def test_each_pull_request_from_the_branch_is_handled(self):
        api = CommentFake([pull(base="main"), pull(base="release/2.5")], [pr_run(1)], {1: report_artifact()})
        self.assertEqual(len(self.run_comment(api)), 2)

    def body_for(self, report, sha=SHA):
        api = CommentFake([pull(sha=sha)], [pr_run(1)], {1: report_artifact(report)})
        return self.posted(self.run_comment(api))

    def test_the_same_comment_is_kept(self):
        old = self.body_for("same\n")
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("same\n")}, [comment(9, old)])
        self.assertEqual(self.run_comment(api), [])

    def test_a_new_commit_or_baseline_with_the_same_result_edits_in_place(self):
        old = self.body_for("same\nBaseline: compared with aaaa\n", sha="e" * 40)
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("same\nBaseline: compared with bbbb\n")},
                          [comment(9, old)])
        sent = self.run_comment(api)
        self.assertEqual([(m, p) for m, p, _ in sent], [("PATCH", f"repos/{REPO}/issues/comments/9")])

    def test_a_changed_result_replaces_the_comment(self):
        old = self.body_for("before\n")
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("after\n")},
                          [comment(9, old), {"id": 8, "user": {"login": "someone"}, "body": cc.MARKER}])
        sent = self.run_comment(api)
        # Posted before the old one goes, so a failed post leaves the old comment.
        self.assertEqual([(m, p) for m, p, _ in sent],
                         [("POST", f"repos/{REPO}/issues/5/comments"), ("DELETE", f"repos/{REPO}/issues/comments/9")])

    def test_other_bot_comments_and_comments_without_a_user_are_left_alone(self):
        old = self.body_for("before\n")
        others = [comment(7, "@someone the build for `build-all` starts now."), {"id": 6, "user": None, "body": "x"}]
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("after\n")}, others + [comment(9, old)])
        deleted = [p for m, p, _ in self.run_comment(api) if m == "DELETE"]
        self.assertEqual(deleted, [f"repos/{REPO}/issues/comments/9"])

    def test_an_identical_comment_with_other_line_ends_is_kept(self):
        old = self.body_for("same\n").replace("\n", "\r\n").rstrip()
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("same\n")}, [comment(9, old)])
        self.assertEqual(self.run_comment(api), [])

    def test_a_crlf_comment_with_the_same_result_is_edited_in_place(self):
        old = self.body_for("same\nBaseline: compared with aaaa\n", sha="e" * 40).replace("\n", "\r\n")
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("same\nBaseline: compared with bbbb\n")},
                          [comment(9, old)])
        self.assertEqual([m for m, _, _ in self.run_comment(api)], ["PATCH"])

    def test_a_comment_left_by_a_failed_delete_is_cleaned_up_without_posting_again(self):
        current = self.body_for("same\n")
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("same\n")},
                          [comment(8, self.body_for("older\n")), comment(9, current)])
        self.assertEqual([(m, p) for m, p, _ in self.run_comment(api)], [("DELETE", f"repos/{REPO}/issues/comments/8")])

    def failing_download(self, error):
        api = CommentFake([pull()], [pr_run(2), pr_run(1)], {2: report_artifact("new\n"), 1: report_artifact("old\n")})
        real = api.get_bytes

        def get_bytes(path, limit):
            if path.endswith("/20/zip"):
                raise error
            return real(path, limit)

        api.get_bytes = get_bytes
        return api

    def test_an_artifact_that_is_gone_falls_through_to_an_older_run(self):
        for code in (404, 410):
            with self.subTest(code=code):
                api = self.failing_download(urllib.error.HTTPError("x", code, "Gone", {}, None))
                self.assertIn("````\nold\n````", self.posted(self.run_comment(api)))

    def test_a_download_that_fails_for_another_reason_posts_nothing(self):
        # A passing outage must not replace the comment with a note that the build made no comparison.
        for error in (urllib.error.HTTPError("x", 503, "Unavailable", {}, None), urllib.error.URLError("dns"),
                      TimeoutError(), ConnectionResetError()):
            with self.subTest(error=type(error).__name__):
                api = self.failing_download(error)
                with self.assertRaises(type(error)):
                    self.run_comment(api)
                self.assertEqual(api.sent, [])

    def test_the_comment_is_found_past_the_first_page(self):
        old = self.body_for("before\n")
        api = CommentFake([pull()], [pr_run(1)], {1: report_artifact("after\n")})
        pages = {1: [comment(100 + i, "other") for i in range(100)], 2: [comment(9, old)]}
        api.gets[f"repos/{REPO}/issues/5/comments"] = lambda params: pages.get(params["page"], [])
        deleted = [p for m, p, _ in self.run_comment(api) if m == "DELETE"]
        self.assertEqual(deleted, [f"repos/{REPO}/issues/comments/9"])


class GetBytesTest(unittest.TestCase):
    """Api.get_bytes with urllib faked: the storage host must never see the token."""

    def fake(self, location="https://storage.example/blob", body=b"zipdata"):
        seen = []

        class Opener:
            def open(self, request, timeout):
                if isinstance(request, str):
                    seen.append(("storage", request))
                    if body is None:
                        raise urllib.error.HTTPError(request, 302, "Found", {"Location": "http://x/"}, None)
                    return io.BytesIO(body)
                seen.append(("api", request.get_header("Authorization")))
                raise urllib.error.HTTPError(request.full_url, 302, "Found", {"Location": location}, None)

        self.build_opener = mock.patch.object(cc.urllib.request, "build_opener", return_value=Opener())
        return seen, self.build_opener, \
            mock.patch.object(cc.urllib.request, "urlopen", side_effect=AssertionError("not via urlopen"))

    def test_the_redirect_is_followed_without_the_token(self):
        seen, opener, urlopen = self.fake()
        with opener, urlopen:
            self.assertEqual(cc.Api("secret").get_bytes("repos/x/zip", 100), b"zipdata")
        self.assertEqual(seen, [("api", "Bearer secret"), ("storage", "https://storage.example/blob")])

    def test_a_download_larger_than_the_limit_is_refused(self):
        _, opener, urlopen = self.fake(body=b"x" * 101)
        with opener, urlopen, self.assertRaises(cc.ArtifactError):
            cc.Api("secret").get_bytes("repos/x/zip", 100)

    def test_storage_redirecting_again_is_not_followed(self):
        seen, opener, urlopen = self.fake(body=None)
        with opener as build_opener, urlopen, self.assertRaises(urllib.error.HTTPError):
            cc.Api("secret").get_bytes("repos/x/zip", 100)
        # Both hops go through an opener that turns a redirect into an error.
        build_opener.assert_called_once_with(cc._NoRedirect)
        self.assertEqual(len([s for s in seen if s[0] == "storage"]), 1)

    def test_a_redirect_that_is_not_https_is_refused(self):
        seen, opener, urlopen = self.fake(location="http://storage.example/blob")
        with opener, urlopen, self.assertRaises(cc.ArtifactError):
            cc.Api("secret").get_bytes("repos/x/zip", 100)
        self.assertEqual([s for s in seen if s[0] == "storage"], [])


class CommentCommandTest(unittest.TestCase):
    def test_an_error_prints_its_type_and_status_only_and_exits_zero(self):
        error = urllib.error.HTTPError("https://x", 502, "line from the artifact", {}, None)
        stdout = io.StringIO()
        with mock.patch.object(cc, "comment", side_effect=error), contextlib.redirect_stdout(stdout):
            code = cc.main(["comment", "--repo", REPO, "--head-repo", HEAD_REPO, "--head-branch", "fix"])
        self.assertEqual(code, 0)
        self.assertNotIn("line from the artifact", stdout.getvalue())
        self.assertIn("502", stdout.getvalue())


class ReportCommandTest(unittest.TestCase):
    def run_report(self, folder, error=None, text="report\n"):
        out, summary = os.path.join(folder, "report.txt"), os.path.join(folder, "summary.md")
        stdout = io.StringIO()
        side = {"side_effect": error} if error else {"return_value": text}
        with mock.patch.object(cc, "report_main", **side), contextlib.redirect_stdout(stdout):
            code = cc.main(["report", "--repo", REPO, "--branch", "main", "--base-sha", "b" * 40,
                            "--head-dir", folder, "--out", out, "--summary", summary])
        with open(out, encoding="utf-8") as f, open(summary, encoding="utf-8") as g:
            return code, stdout.getvalue(), f.read(), g.read()

    def test_an_error_writes_its_type_only_and_exits_zero(self):
        with tempfile.TemporaryDirectory() as folder:
            code, stdout, report, summary = self.run_report(folder, ValueError("line from the artifact"))
        self.assertEqual(code, 0)
        for text in (stdout, report, summary):
            self.assertNotIn("line from the artifact", text)
        self.assertIn("could not be made (ValueError)", report)

    def test_any_error_still_writes_a_report(self):
        with tempfile.TemporaryDirectory() as folder:
            code, _, report, _ = self.run_report(folder, RuntimeError("x"))
        self.assertEqual(code, 0)
        self.assertIn("could not be made (RuntimeError)", report)

    def test_the_summary_holds_the_report_inside_a_fence(self):
        with tempfile.TemporaryDirectory() as folder:
            _, _, report, summary = self.run_report(folder, text="G-code is the same\n")
        self.assertEqual(report, "G-code is the same\n")
        self.assertIn("````\nG-code is the same\n````\n", summary)

    def test_made_with_names_the_script_and_the_runner(self):
        with tempfile.TemporaryDirectory() as folder:
            out = os.path.join(folder, "made_with.txt")
            self.assertEqual(cc.main(["made-with", "-o", out]), 0)
            with open(out, encoding="utf-8") as f:
                lines = f.read().splitlines()
        self.assertRegex(lines[0], r"^fingerprint script [0-9a-f]{64}$")
        self.assertTrue(lines[1].startswith("runner "))


if __name__ == "__main__":
    unittest.main()
