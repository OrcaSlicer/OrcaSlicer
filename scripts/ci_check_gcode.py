#!/usr/bin/env python3
"""Compares a pull request's sliced G-code with its base branch's, for the check-gcode label.

The Slice check job of build_all.yml fingerprints the G-code of every shipped printer preset
(gcode_fingerprint.py) and uploads the gcode-fingerprints artifact. On a pull request,
`report` then finds the newest build of the base branch with fingerprints, at or before the
pull request's base commit, and writes report.txt into the same artifact. Pushes and the
nightly build are the baselines; they upload only fingerprints.tsv and made_with.txt.

Environment: GH_TOKEN, and optionally GITHUB_API_URL.
"""

import argparse
import hashlib
import io
import json
import os
import platform
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gcode_fingerprint  # noqa: E402

# Must match the artifact name and the workflow file in build_all.yml.
ARTIFACT = "gcode-fingerprints"
WORKFLOW = "build_all.yml"
# How far back from the base commit a baseline is looked for.
COMMITS_BACK = 30
# A deflated artifact is refused when it is larger than this many times the member limit.
ZIP_FACTOR = 4
# fingerprints.tsv is about 60 KB for 1,336 slices.
FINGERPRINTS_LIMIT = 4 * 1024 * 1024
SMALL_LIMIT = 4096
# How many slice names are listed under each vendor.
NAMES_PER_VENDOR = 20
# The report's baseline paragraph starts with this. It changes with every push to the base branch,
# so the commenter edits a comment in place when only that part changed.
BASELINE_LINE = "Baseline: "
# A comment holds at most 65,536 characters; the report inside it is cut well before.
FENCE_LIMIT = 50000
FENCE = "````"
# Only these are bounded by a capped read; bzip2 and LZMA decompress a whole block at once.
READABLE_COMPRESSION = (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)


class ArtifactError(Exception):
    """An artifact that cannot be read. The message never holds the artifact's own text."""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Api:
    def __init__(self, token, url="https://api.github.com"):
        self.token = token
        self.url = url.rstrip("/")

    def _request(self, method, path, params=None, body=None):
        query = f"?{urllib.parse.urlencode(params)}" if params else ""
        data = json.dumps(body).encode() if body is not None else None
        return urllib.request.Request(f"{self.url}/{path}{query}", data=data, method=method, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": "2022-11-28",
        })

    def get(self, path, **params):
        with urllib.request.urlopen(self._request("GET", path, params), timeout=30) as response:
            return json.load(response)

    def get_bytes(self, path, limit):
        """The body of a download, read up to `limit` bytes. The redirect to storage is followed
        without the token, which the storage host must not see."""
        opener = urllib.request.build_opener(_NoRedirect)
        try:
            response = opener.open(self._request("GET", path), timeout=60)
        except urllib.error.HTTPError as error:
            if error.code not in (301, 302, 303, 307, 308):
                raise
            location = error.headers["Location"]
            error.close()
            response = urllib.request.urlopen(location, timeout=60)
        with response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ArtifactError("the artifact is larger than its limit")
        return data


@dataclass
class Baseline:
    sha: str
    title: str
    run_id: int
    artifact: dict
    between: list = field(default_factory=list)  # (short sha, title), the base first


def short(sha):
    return sha[:10]


def title_of(commit):
    message = commit["commit"]["message"]
    return message.splitlines()[0] if message else ""


def find_artifact(api, repo, run_id):
    """The run's unexpired gcode-fingerprints artifact, or None."""
    body = api.get(f"repos/{repo}/actions/runs/{run_id}/artifacts", name=ARTIFACT)
    for art in body["artifacts"]:
        if art["name"] == ARTIFACT and not art["expired"]:
            return art
    return None


def read_artifact(api, repo, art, names, limit):
    """{name: bytes} for each of `names` the artifact holds. The zip is read in memory, and its
    directory is the pull request's data, so each member is read with a cap of its own."""
    if art["size_in_bytes"] > ZIP_FACTOR * limit:
        raise ArtifactError("the artifact is larger than its limit")
    data = api.get_bytes(f"repos/{repo}/actions/artifacts/{art['id']}/zip", limit=ZIP_FACTOR * limit)
    found = {}
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            present = {info.filename: info for info in zf.infolist()}
            for name in names:
                if name not in present:
                    continue
                if present[name].compress_type not in READABLE_COMPRESSION:
                    raise ArtifactError("an artifact file uses a compression that is not read")
                with zf.open(present[name]) as member:
                    content = member.read(limit + 1)
                if len(content) > limit:
                    raise ArtifactError("an artifact file is larger than its limit")
                found[name] = content
    except (zipfile.BadZipFile, zipfile.LargeZipFile, NotImplementedError, EOFError, OSError) as error:
        raise ArtifactError("the artifact is not a readable zip") from error
    return found


def find_baseline(api, repo, branch, base_sha):
    """The newest commit at or before base_sha whose branch build saved fingerprints, or None."""
    commits = api.get(f"repos/{repo}/commits", sha=base_sha, per_page=COMMITS_BACK)
    # Asked for by event: pull request runs from forks' own branches of the same name would
    # otherwise fill the window.
    runs_of = {}
    for event in ("push", "schedule"):
        runs = api.get(f"repos/{repo}/actions/workflows/{WORKFLOW}/runs", branch=branch, event=event, per_page=100)
        for run in runs["workflow_runs"]:
            runs_of.setdefault(run["head_sha"], []).append(run)
    for sha_runs in runs_of.values():
        sha_runs.sort(key=lambda r: r["id"], reverse=True)
    for index, commit in enumerate(commits):
        for run in runs_of.get(commit["sha"], []):
            art = find_artifact(api, repo, run["id"])
            if art:
                between = [(short(c["sha"]), title_of(c)) for c in commits[:index]]
                return Baseline(commit["sha"], title_of(commit), run["id"], art, between)
    return None


def made_with(script_path):
    with open(script_path, "rb") as f:
        script = hashlib.sha256(f.read()).hexdigest()
    return f"fingerprint script {script}\nrunner {platform.system()} {platform.machine()}\n"


def build_report(baseline, base_rows, head_rows, base_made_with, head_made_with, branch):
    if baseline is None:
        return (f"No build of {branch} among the last {COMMITS_BACK} commits up to this pull request's "
                "base has saved fingerprints, so nothing was compared.\n")
    lines = []
    if not base_made_with or not head_made_with:
        lines.append("Warning: one side does not say which fingerprint script and runner made it, "
                     "so slices may differ for that reason alone.")
    elif base_made_with != head_made_with:
        lines.append("Warning: the two sides were made by a different fingerprint script or runner, "
                     "so slices may differ for that reason alone.")
    report = gcode_fingerprint.compare(base_rows, head_rows)
    text = gcode_fingerprint.format_report(report, NAMES_PER_VENDOR)
    if report.same_count + len(report.changed) == 0:
        text = "No slice could be compared: the two sides have none in common.\n" + text.split("\n", 1)[1]
    lines.append(text)
    if baseline.between:
        lines.append(f"{BASELINE_LINE}compared with the build of {short(baseline.sha)} ({baseline.title}). "
                     "These later commits up to the base have no saved fingerprints, so changes "
                     "they made to the G-code show here too:")
        lines += [f"  {sha} {title}" for sha, title in baseline.between]
    else:
        lines.append(f"{BASELINE_LINE}compared with the build of {short(baseline.sha)} ({baseline.title}), "
                     "this pull request's base commit.")
    return "\n".join(lines) + "\n"


def fence(data):
    """Text from an artifact as a Markdown code block nothing in it can close or turn into markup."""
    text = data.decode("utf-8", "replace")
    text = "".join(c for c in text if c in "\n\t" or (c >= " " and c != "\x7f"))
    while FENCE in text:
        text = text.replace(FENCE, "```")
    if len(text.encode()) > FENCE_LIMIT:
        cut = text.encode()[:FENCE_LIMIT].decode("utf-8", "ignore")
        if "\n" in cut:
            cut = cut[:cut.rindex("\n") + 1]
        text = cut + ("" if cut.endswith("\n") else "\n") + "... (report cut)\n"
    if not text.endswith("\n"):
        text += "\n"
    return f"{FENCE}\n{text}{FENCE}\n"


def _rows_from_bytes(data):
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "fingerprints.tsv")
        with open(path, "wb") as f:
            f.write(data)
        return gcode_fingerprint.read_fingerprints(path)


def _read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError:
        return None


def report_main(args, api):
    head_rows = gcode_fingerprint.read_fingerprints(os.path.join(args.head_dir, "fingerprints.tsv"))
    head_made_with = _read_text(os.path.join(args.head_dir, "made_with.txt"))
    baseline = find_baseline(api, args.repo, args.branch, args.base_sha)
    base_rows = base_made_with = None
    if baseline:
        files = read_artifact(api, args.repo, baseline.artifact, ["fingerprints.tsv", "made_with.txt"],
                              FINGERPRINTS_LIMIT)
        if "fingerprints.tsv" not in files:
            raise ArtifactError("the baseline has no fingerprints.tsv")
        base_rows = _rows_from_bytes(files["fingerprints.tsv"])
        base_made_with = files.get("made_with.txt", b"").decode("utf-8", "replace") or None
    return build_report(baseline, base_rows, head_rows, base_made_with, head_made_with, args.branch)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    made = commands.add_parser("made-with", help="print what made a fingerprint file")
    made.add_argument("-o", "--out", required=True)
    report = commands.add_parser("report", help="compare this build's fingerprints with the base branch's")
    report.add_argument("--repo", required=True)
    report.add_argument("--branch", required=True)
    report.add_argument("--base-sha", required=True)
    report.add_argument("--head-dir", required=True)
    report.add_argument("--out", required=True)
    report.add_argument("--summary", help="a job summary file to append the report to")
    args = parser.parse_args(argv)

    if args.command == "made-with":
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gcode_fingerprint.py")
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(made_with(script))
        return 0

    api = Api(os.environ.get("GH_TOKEN", ""), os.environ.get("GITHUB_API_URL", "https://api.github.com"))
    try:
        text = report_main(args, api)
    except (ArtifactError, urllib.error.URLError, OSError, ValueError, KeyError, TypeError) as error:
        # The type only: the message of an error from the baseline's files can hold their text.
        print(f"The comparison could not be made ({type(error).__name__}).")
        text = f"The comparison could not be made ({type(error).__name__}).\n"
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8", newline="\n") as f:
            f.write("## Sliced G-code comparison\n\n" + fence(text.encode()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
