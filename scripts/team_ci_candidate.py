"""Record and validate the exact GitHub merge candidate; never merge or push."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.parse
import urllib.request

BRANCH = "codex/team/integration"


def sha(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("Expected a complete 40-character Git SHA")
    return value


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True, encoding="utf-8").stdout.strip()


def scope(paths: list[str], *, initial: bool = False, upstream: bool = False) -> dict:
    cross = initial or upstream or any(
        p.startswith(("deps/", "deps_src/", "cmake/", ".github/actions/", ".github/workflows/"))
        or p.endswith("CMakeLists.txt") or p == "version.inc"
        or p.startswith(("build_linux", "build_release_macos", "scripts/build_", "scripts/run_unit_tests")) for p in paths)
    native = cross or any(p.startswith(("src/", "tests/", "resources/", "localization/", "tools/ai/"))
                          or p.startswith("build_") for p in paths)
    return {"native": native, "cross_platform": cross}


def inspect(root: Path, event: dict, event_name: str, repository: str) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid repository")
    if event.get("repository", {}).get("full_name") != repository:
        raise ValueError("Event repository mismatch")
    candidate = sha(git(root, "rev-parse", "HEAD"))
    number = None
    initial = upstream = False
    if event_name == "pull_request":
        pr = event["pull_request"]
        if pr["base"]["ref"] != BRANCH or pr["base"]["repo"]["full_name"] != repository:
            raise ValueError("PR must target the configured integration repository and branch")
        head, base = sha(pr["head"]["sha"]), sha(pr["base"]["sha"])
        number = event["number"]
        if type(number) is not int or number < 1:
            raise ValueError("Invalid PR number")
        parents = git(root, "rev-list", "--parents", "-n", "1", candidate).split()[1:]
        if parents != [base, head]:
            raise ValueError("Checkout is not the merge of the event's exact base and PR HEAD; refresh the PR")
        upstream = pr["head"]["ref"].startswith("codex/upstream-sync-")
    elif event_name == "merge_group":
        group = event["merge_group"]
        if group["base_ref"] != f"refs/heads/{BRANCH}":
            raise ValueError("Merge group targets another branch")
        head, base = sha(group["head_sha"]), sha(group["base_sha"])
        if candidate != head:
            raise ValueError("Checkout does not match merge group HEAD")
        git(root, "merge-base", "--is-ancestor", base, candidate)
        # A merge group may contain an upstream/CI change: cover all platforms.
        upstream = True
    elif event_name == "push":
        if event["ref"] != f"refs/heads/{BRANCH}" or sha(event["after"]) != candidate:
            raise ValueError("Push does not match integration checkout")
        head, base = candidate, sha(event["before"])
        initial = base == "0" * 40
        if initial:
            base = head
    else:
        raise ValueError("Only pull_request, merge_group and integration push are supported")
    names = git(root, "ls-files", "-z") if initial else git(root, "diff", "--no-renames", "--name-only", "-z", base, candidate)
    paths = [name for name in names.split("\0") if name]
    return {"schema_version": 1, "repository": repository, "pr_number": number,
            "event_name": event_name, "head_sha": head, "base_sha": base,
            "candidate_sha": candidate, "status": "pending",
            **scope(paths, initial=initial, upstream=upstream)}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise ValueError("GitHub API redirects are not accepted")


def api_get(path: str, token: str) -> dict:
    request = urllib.request.Request("https://api.github.com/" + path, headers={
        "Authorization": "Bearer " + token, "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28"})
    with urllib.request.build_opener(NoRedirect).open(request, timeout=30) as response:
        return json.load(response)


def verify_live(report: dict, get) -> None:
    repo = report["repository"]
    branch = get(f"repos/{repo}/branches/{urllib.parse.quote(BRANCH, safe='')}")
    expected = report["candidate_sha"] if report["event_name"] == "push" else report["base_sha"]
    if branch["commit"]["sha"] != expected:
        raise ValueError("Integration HEAD changed during checks; refresh candidate and rerun")
    if report["event_name"] == "pull_request":
        pr = get(f"repos/{repo}/pulls/{report['pr_number']}")
        if (pr["state"] != "open" or pr["head"]["sha"] != report["head_sha"]
                or pr["base"]["sha"] != report["base_sha"]
                or pr["base"]["ref"] != BRANCH
                or pr["base"]["repo"]["full_name"] != repo
                or pr.get("merge_commit_sha") != report["candidate_sha"]):
            raise ValueError("PR/base/merge candidate changed during checks; refresh and rerun")


def require_results(report: dict, results: dict) -> None:
    required = {"inspect"}
    if report["native"]:
        required.update(("windows_build", "windows_tests"))
    if report["cross_platform"]:
        required.update(("linux_build", "linux_tests", "macos_build", "macos_tests"))
    for name in required:
        if results.get(name, {}).get("result") != "success":
            raise ValueError(f"Required candidate job did not succeed: {name}")


def inherit_verified_base(report: dict, checks: list[dict]) -> None:
    """Only skip native builds when the unchanged base has successful evidence.

    In particular, a documentation push must not turn an unbuilt or failed
    bootstrap baseline green by cancelling its first full build.
    """
    matches = [check for check in checks if check.get("name") == "Team integration candidate"
               and check.get("head_sha") == report["base_sha"]
               and check.get("app", {}).get("id") == 15368]
    latest = max(matches, key=lambda check: check["id"]) if matches else {}
    verified = latest.get("status") == "completed" and latest.get("conclusion") == "success"
    report["base_candidate_verified"] = verified
    if not verified:
        report.update(native=True, cross_platform=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("inspect", "complete"))
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    if args.command == "inspect":
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        report = inspect(args.repo_root, event, os.environ["GITHUB_EVENT_NAME"], os.environ["GITHUB_REPOSITORY"])
        response = api_get(f"repos/{report['repository']}/commits/{report['base_sha']}/check-runs?filter=latest&per_page=100",
                           os.environ["GITHUB_TOKEN"])
        # Missing, failed, cancelled, truncated or not-yet-complete evidence
        # widens verification instead of allowing a path-only success.
        inherit_verified_base(report, response["check_runs"] if response.get("total_count", 0) <= 100 else [])
        report.update(run_id=int(os.environ["GITHUB_RUN_ID"]), run_attempt=int(os.environ["GITHUB_RUN_ATTEMPT"]))
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                for name in ("native", "cross_platform"):
                    output.write(f"{name}={str(report[name]).lower()}\n")
    else:
        report = json.loads(args.report.read_text(encoding="utf-8"))
        require_results(report, json.loads(os.environ["CANDIDATE_JOB_RESULTS"]))
        if (report["schema_version"] != 1 or report["status"] != "pending"
                or report["repository"] != os.environ["GITHUB_REPOSITORY"]
                or report["event_name"] != os.environ["GITHUB_EVENT_NAME"]
                or report["run_id"] != int(os.environ["GITHUB_RUN_ID"])
                or report["run_attempt"] != int(os.environ["GITHUB_RUN_ATTEMPT"])
                or report["candidate_sha"] != sha(git(args.repo_root, "rev-parse", "HEAD"))):
            raise ValueError("Candidate report does not belong to this run/checkout")
        verify_live(report, lambda path: api_get(path, os.environ["GITHUB_TOKEN"]))
        report["status"] = "success"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
