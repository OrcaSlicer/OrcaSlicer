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
        fenced = cc.fence(b"a\x00b\x1b[31mc\td\xff\n")
        self.assertIn("ab[31mc\td\ufffd", fenced)

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
