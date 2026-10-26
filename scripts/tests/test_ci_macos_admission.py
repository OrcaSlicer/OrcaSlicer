#!/usr/bin/env python3
"""Tests for scripts/ci_macos_admission.py (stdlib unittest, no external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import datetime
import os
import sys
import unittest
import urllib.error
from unittest import mock

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import ci_macos_admission as admission  # noqa: E402

UTC = datetime.timezone.utc
NOON = datetime.datetime(2026, 10, 10, 12, 0, tzinfo=UTC)


def job(name, status="completed", conclusion="success", labels=("macos-15",)):
    return {"name": name, "status": status, "conclusion": conclusion, "labels": list(labels)}


def gate(arch, status="completed", conclusion="success", priority=False):
    suffix = " (priority)" if priority else ""
    return job(f"build_macos_arch ({arch}) / Wait for a macOS runner{suffix}", status, conclusion,
               labels=("ubuntu-24.04",))


def build(arch, status="in_progress"):
    return job(f"build_macos_arch ({arch}) / Build Deps / Build OrcaSlicer / Build OrcaSlicer",
               status, None if status != "completed" else "success")


CHECK_CACHE = job("build_macos_arch (arm64) / Check Cache", labels=("ubuntu-24.04",))
UNIVERSAL_DONE = job("Build macOS Universal / Build OrcaSlicer")
TESTS_DONE = job("macOS arm64 / Unit Tests")
PUSH = {"id": 1, "event": "push", "head_branch": "main", "status": "in_progress"}
PR = {"id": 2, "event": "pull_request", "head_branch": "topic", "status": "in_progress"}


class MacosDoneTest(unittest.TestCase):
    def test_not_done_before_the_final_jobs_are_listed(self):
        self.assertFalse(admission.macos_done([CHECK_CACHE, build("arm64", "completed")]))

    def test_not_done_while_a_final_job_runs(self):
        running = job("macOS arm64 / Unit Tests", "in_progress", None)
        self.assertFalse(admission.macos_done([UNIVERSAL_DONE, running]))

    def test_done_when_the_final_jobs_finished(self):
        self.assertTrue(admission.macos_done([build("arm64", "completed"), UNIVERSAL_DONE, TESTS_DONE]))

    def test_done_when_the_final_jobs_were_skipped(self):
        # A skipped caller job is listed under its own name, without labels.
        skipped = [job("Build macOS Universal", conclusion="skipped", labels=()),
                   job("macOS arm64", conclusion="skipped", labels=())]
        self.assertTrue(admission.macos_done(skipped))


class RunDemandTest(unittest.TestCase):
    def test_push_reserves_before_its_macos_jobs_are_listed(self):
        self.assertEqual(admission.run_demand(PUSH, []), admission.RESERVE)
        self.assertEqual(admission.run_demand(PUSH, [CHECK_CACHE]), admission.RESERVE)

    def test_push_keeps_its_reserve_between_stages(self):
        jobs = [build("arm64", "completed"), build("x86_64", "completed")]
        self.assertEqual(admission.run_demand(PUSH, jobs), admission.RESERVE)

    def test_push_releases_when_macos_is_done(self):
        jobs = [build("arm64", "completed"), UNIVERSAL_DONE, TESTS_DONE]
        self.assertEqual(admission.run_demand(PUSH, jobs), 0)

    def test_queued_jobs_count_as_busy(self):
        jobs = [build("arm64", "queued")]
        self.assertEqual(admission.run_demand(PR, jobs), 1)

    def test_pull_request_holds_nothing_before_it_is_let_in(self):
        self.assertEqual(admission.run_demand(PR, [CHECK_CACHE, gate("arm64", "in_progress", None)]), 0)

    def test_one_arch_let_in_holds_a_runner_until_it_is_built(self):
        # Including before its first macOS job is listed, and between its stages.
        self.assertEqual(admission.run_demand(PR, [gate("arm64")]), 1)
        self.assertEqual(admission.run_demand(PR, [gate("arm64"), build("arm64")]), 1)

    def test_one_arch_let_in_holds_nothing_once_it_is_built(self):
        # Its other arch may be in the line behind a pull request waiting for this
        # runner, so holding it would deadlock.
        jobs = [gate("arm64"), build("arm64", "completed"), gate("x86_64", "pending", None)]
        self.assertEqual(admission.run_demand(PR, jobs), 0)

    def test_one_arch_let_in_holds_nothing_once_it_failed(self):
        failed = job("build_macos_arch (arm64) / Build Deps / Build Deps", conclusion="failure")
        self.assertEqual(admission.run_demand(PR, [gate("arm64"), failed]), 0)

    def test_both_arches_let_in_hold_only_what_still_builds(self):
        # The universal build and tests are not held for in advance.
        jobs = [gate("arm64"), gate("x86_64"), build("arm64", "completed"), build("x86_64")]
        self.assertEqual(admission.run_demand(PR, jobs), 1)

    def test_universal_build_and_tests_count_once_queued(self):
        built = [gate("arm64"), gate("x86_64"), build("arm64", "completed"), build("x86_64", "completed")]
        self.assertEqual(admission.run_demand(PR, built), 0)
        queued = [job("Build macOS Universal / Build OrcaSlicer", "queued", None),
                  job("macOS arm64 / Unit Tests", "in_progress", None)]
        self.assertEqual(admission.run_demand(PR, built + queued), 2)

    def test_pull_request_releases_when_macos_is_done(self):
        jobs = [gate("arm64"), gate("x86_64"), UNIVERSAL_DONE, TESTS_DONE]
        self.assertEqual(admission.run_demand(PR, jobs), 0)

    def test_pull_request_without_the_line_holds_what_it_runs(self):
        # A run started from a workflow without the line.
        self.assertEqual(admission.run_demand(PR, [build("arm64"), build("x86_64", "queued")]), 2)

    def test_finished_and_non_macos_jobs_are_not_counted(self):
        jobs = [build("arm64", "completed"), CHECK_CACHE,
                job("build_linux (ubuntu-24.04) / Check Cache", "in_progress", None, ("ubuntu-24.04",))]
        self.assertEqual(admission.run_demand(PR, jobs), 0)


class PriorityTest(unittest.TestCase):
    WAITING = [gate("arm64", "in_progress", None, priority=True), gate("x86_64", "pending", None, priority=True)]

    def test_normal_line_yields_to_a_waiting_priority_run(self):
        self.assertEqual(admission.run_demand(PR, self.WAITING, yield_to_priority=True), admission.RESERVE)

    def test_priority_line_does_not_count_priority_runs_behind_it(self):
        self.assertEqual(admission.run_demand(PR, self.WAITING), 0)

    def test_a_waiting_normal_run_is_not_yielded_to(self):
        waiting = [gate("arm64", "in_progress", None), gate("x86_64", "pending", None)]
        self.assertEqual(admission.run_demand(PR, waiting, yield_to_priority=True), 0)

    def test_a_priority_run_half_let_in_still_counts_its_waiting_arch(self):
        jobs = [gate("arm64", priority=True), gate("x86_64", "pending", None, priority=True)]
        self.assertEqual(admission.run_demand(PR, jobs), 1)
        self.assertEqual(admission.run_demand(PR, jobs, yield_to_priority=True), 2)

    def test_skipped_priority_jobs_of_other_platforms_are_not_waiting(self):
        jobs = [job("build_linux (ubuntu-24.04) / Wait for a macOS runner (priority)",
                    conclusion="skipped", labels=("ubuntu-24.04",))]
        self.assertEqual(admission.run_demand(PR, jobs, yield_to_priority=True), 0)

    def test_measure_from_each_line(self):
        api = FakeApi({("runs", "pending"): [dict(PR, status="pending")], jobs_path(2): self.WAITING})
        total, lines = admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertEqual(total, admission.RESERVE)
        self.assertIn("priority, waiting", lines[0])
        total, _ = admission.measure(api, "o/r", "build_all.yml", 99, NOON, priority=True)
        self.assertEqual(total, 0)

    def test_own_waiting_run_is_not_counted(self):
        api = FakeApi({("runs", "pending"): [dict(PR, status="pending")], jobs_path(2): self.WAITING})
        total, _ = admission.measure(api, "o/r", "build_all.yml", 2, NOON)
        self.assertEqual(total, 0)


class FakeApi:
    """Answers GET requests from a dict of path -> list of pages (or one body)."""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def get(self, path, **params):
        self.calls.append((path, params))
        if path.endswith("/runs") and ("status" in params or "created" in params):
            runs = self.responses.get(("runs", params.get("status", "recent")), [])
            start = (params["page"] - 1) * params["per_page"]
            return {"total_count": len(runs), "workflow_runs": runs[start:start + params["per_page"]]}
        if path.endswith("/runs"):
            return {"workflow_runs": self.responses.get(("runs", params.get("event")), [])[:1]}
        if path.endswith("/jobs"):
            return {"jobs": self.responses.get(path, [])}
        # A run read by its id, by default a pull request with no jobs listed.
        return self.responses.get(path, dict(PR, id=int(path.rsplit("/", 1)[-1])))


def jobs_path(run_id):
    return f"repos/o/r/actions/runs/{run_id}/jobs"


class MeasureTest(unittest.TestCase):
    def test_sums_in_progress_and_queued_runs(self):
        queued_pr = dict(PR, id=3, status="queued")
        api = FakeApi({
            ("runs", "in_progress"): [PUSH, PR],
            ("runs", "queued"): [queued_pr],
            jobs_path(1): [CHECK_CACHE],
            jobs_path(2): [gate("arm64"), build("arm64")],
            jobs_path(3): [gate("arm64"), gate("x86_64"), build("arm64", "queued")],
        })
        total, lines = admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertEqual(total, admission.RESERVE + 1 + admission.RESERVE)
        self.assertEqual(len(lines), 3)

    def test_counts_a_pending_run_that_holds_runners(self):
        # One of its jobs waits in a concurrency group while its macOS builds run.
        pending_pr = dict(PR, status="pending")
        api = FakeApi({("runs", "pending"): [pending_pr], jobs_path(2): [gate("arm64"), build("arm64")]})
        total, _ = admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertEqual(total, 1)

    def test_a_push_waiting_behind_another_holds_nothing(self):
        api = FakeApi({("runs", "pending"): [dict(PUSH, status="pending")], jobs_path(1): []})
        total, _ = admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertEqual(total, 0)

    def test_reads_every_page_of_runs(self):
        runs = [dict(PR, id=i) for i in range(150)]
        responses = {("runs", "in_progress"): runs}
        responses.update({jobs_path(i): [gate("arm64")] for i in range(150)})
        total, _ = admission.measure(FakeApi(responses), "o/r", "build_all.yml", 999, NOON)
        self.assertEqual(total, 150)

    def test_reserves_for_the_nightly_until_its_run_appears(self):
        due = datetime.datetime(2026, 10, 10, 2, 10, tzinfo=UTC)
        yesterday = {"created_at": "2026-10-09T02:20:00Z"}
        total, lines = admission.measure(
            FakeApi({("runs", "schedule"): [yesterday]}), "OrcaSlicer/OrcaSlicer", "build_all.yml", 99, due)
        self.assertEqual(total, admission.RESERVE)
        self.assertIn("nightly", lines[0])

    def test_no_nightly_reserve_once_its_run_exists(self):
        due = datetime.datetime(2026, 10, 10, 2, 30, tzinfo=UTC)
        today = {"created_at": "2026-10-10T02:21:00Z"}
        total, _ = admission.measure(
            FakeApi({("runs", "schedule"): [today]}), "OrcaSlicer/OrcaSlicer", "build_all.yml", 99, due)
        self.assertEqual(total, 0)

    def test_no_nightly_reserve_in_a_fork(self):
        due = datetime.datetime(2026, 10, 10, 2, 10, tzinfo=UTC)
        total, _ = admission.measure(FakeApi({}), "fork/OrcaSlicer", "build_all.yml", 99, due)
        self.assertEqual(total, 0)

    def test_counts_a_run_in_both_lists_once(self):
        api = FakeApi({
            ("runs", "in_progress"): [PUSH],
            ("runs", "queued"): [dict(PUSH, status="queued")],
            jobs_path(1): [],
        })
        total, _ = admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertEqual(total, admission.RESERVE)

    def test_counts_its_own_run(self):
        jobs = [gate("arm64"), build("arm64"), gate("x86_64", "in_progress", None)]
        api = FakeApi({("runs", "in_progress"): [PR], jobs_path(2): jobs})
        self.assertEqual(admission.measure(api, "o/r", "build_all.yml", 2, NOON)[0], 1)

    def test_a_run_whose_other_arch_waits_behind_this_one_does_not_block_it(self):
        # The line is A-arm64, B-arm64, A-x86_64. A's arm64 is built and its
        # x86_64 is pending behind B, so A must not hold a runner B waits for.
        other = dict(PR, id=1, status="pending")
        api = FakeApi({
            ("runs", "pending"): [other, dict(PR, status="pending")],
            jobs_path(1): [gate("arm64"), build("arm64", "completed"), gate("x86_64", "pending", None)],
            jobs_path(2): [gate("arm64", "in_progress", None)],
        })
        total, _ = admission.measure(api, "o/r", "build_all.yml", 2, NOON)
        self.assertEqual(total, 0)

    def test_counts_its_own_run_when_no_status_list_has_it(self):
        # The run moves from pending to in_progress as its last gate leaves the
        # line, and is in neither list while that gate checks.
        jobs = [gate("x86_64"), build("x86_64", "queued"), gate("arm64", "in_progress", None)]
        api = FakeApi({jobs_path(2): jobs})
        self.assertEqual(admission.measure(api, "o/r", "build_all.yml", 2, NOON)[0], 1)

    def test_counts_a_recent_run_that_no_status_list_has(self):
        api = FakeApi({("runs", "recent"): [PR], jobs_path(2): [gate("arm64"), build("arm64")]})
        self.assertEqual(admission.measure(api, "o/r", "build_all.yml", 99, NOON)[0], 1)

    def test_lists_recent_runs_from_a_day_before(self):
        api = FakeApi({})
        admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertIn(">=2026-10-09T12:00:00Z", [params.get("created") for _, params in api.calls])

    def test_finished_recent_runs_are_not_read(self):
        done = dict(PUSH, status="completed")
        api = FakeApi({("runs", "recent"): [done]})
        total, _ = admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertEqual(total, 0)
        self.assertNotIn(jobs_path(1), [path for path, _ in api.calls])

    def test_no_nightly_lookup_outside_its_window(self):
        api = FakeApi({})
        admission.measure(api, "o/r", "build_all.yml", 99, NOON)
        self.assertFalse(any("event" in params for _, params in api.calls))


class ApiTest(unittest.TestCase):
    class Response:
        def __init__(self, headers):
            self.headers = headers

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def read(self):
            return b"{}"

    def test_reports_calls_and_the_rate_limit_left(self):
        api = admission.Api("token")
        responses = [self.Response({}),
                     self.Response({"X-RateLimit-Remaining": "812", "X-RateLimit-Limit": "1000"})]
        with mock.patch.object(admission.urllib.request, "urlopen", side_effect=responses):
            api.get("first")
            calls_before = api.calls
            api.get("second")
        self.assertEqual(api.usage(0), "  API calls this check: 2, rate limit left: 812 of 1000")
        self.assertEqual(api.usage(calls_before), "  API calls this check: 1, rate limit left: 812 of 1000")

    def test_reads_the_rate_limit_from_a_failed_request(self):
        api = admission.Api("token")
        headers = {"X-RateLimit-Remaining": "0", "X-RateLimit-Limit": "1000"}
        error = urllib.error.HTTPError("url", 403, "Forbidden", headers, None)
        with mock.patch.object(admission.urllib.request, "urlopen", side_effect=error), \
             self.assertRaises(urllib.error.HTTPError):
            api.get("runs")
        self.assertEqual(api.usage(0), "  API calls this check: 1, rate limit left: 0 of 1000")


class WaitTest(unittest.TestCase):
    def run_wait(self, results, limit=5, wait_minutes=60, poll_seconds=120):
        results = iter(results)
        now = [0.0]

        def measure_now():
            result = next(results)
            if isinstance(result, Exception):
                raise result
            return result, []

        def sleep(seconds):
            now[0] += seconds

        reason = admission.wait(measure_now, limit, wait_minutes, poll_seconds,
                                clock=lambda: now[0], sleep=sleep, log=lambda _: None)
        return reason, now[0]

    def test_lets_in_when_a_runner_fits(self):
        self.assertEqual(self.run_wait([4]), ("a runner is free", 0))

    def test_waits_while_full(self):
        self.assertEqual(self.run_wait([5, 6, 4]), ("a runner is free", 240))


    def test_lets_in_after_repeated_api_errors(self):
        error = urllib.error.URLError("rate limited")
        reason, _ = self.run_wait([error, error, error])
        self.assertEqual(reason, "the queue could not be read")

    def test_a_good_read_resets_the_error_count(self):
        error = urllib.error.URLError("rate limited")
        reason, _ = self.run_wait([error, error, 5, error, error, 3])
        self.assertEqual(reason, "a runner is free")

    def test_lets_in_at_the_time_limit(self):
        reason, elapsed = self.run_wait([5] * 100, wait_minutes=10, poll_seconds=120)
        self.assertEqual(reason, "it waited 10 minutes")
        self.assertLess(elapsed, 600)


if __name__ == "__main__":
    unittest.main()
