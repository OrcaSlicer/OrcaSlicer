#!/usr/bin/env python3
"""Finds which pull requests were cherry-picked onto a release's branches (rc/vX.Y.Z and
release/vX.Y.Z), from a clone whose remote is "origin".

A pick is matched by the "(#N)" at the end of its subject, which squash merges add and cherry-picks
keep. A pick without one can be matched by its patch id against the commits on main.
"""

import re
import subprocess
from dataclasses import dataclass

from release_pick_rules import parse_version

_TRAILING_NUMBER = re.compile(r"\(#(\d+)\)\s*$")
_REVERT = re.compile(r'^Revert "(.*)"')
_VERSION_BUMP = re.compile(r"^set the version to ", re.IGNORECASE)


@dataclass(frozen=True)
class Pick:
    sha: str
    subject: str
    pr: int | None
    branch: str


def _git(repo_dir, *args, stdin=None):
    return subprocess.run(["git", "-C", repo_dir, *args], check=True, text=True, capture_output=True,
                          input=stdin).stdout


def pr_number_from_subject(subject):
    if _REVERT.match(subject):
        return None
    match = _TRAILING_NUMBER.search(subject)
    return int(match.group(1)) if match else None


def reverted_number(subject):
    match = _REVERT.match(subject)
    return pr_number_from_subject(match.group(1)) if match else None


def remote_branches(repo_dir):
    refs = _git(repo_dir, "for-each-ref", "--format=%(refname:strip=3)", "refs/remotes/origin").split()
    return {ref for ref in refs if ref != "HEAD"}


def release_branches(milestone, branches):
    return [b for b in (f"rc/v{milestone}", f"release/v{milestone}") if b in branches]


def branch_base(repo_dir, milestone, branch):
    """The previous stable release tag when it is in the branch's history; otherwise the point where
    the branch left main (a release cut from main does not contain the previous release's tag)."""
    target = parse_version(milestone)
    tags = []
    for tag in _git(repo_dir, "tag", "--list", "v*").split():
        version = parse_version(tag[1:])
        if version and version < target:
            tags.append((version, tag))
    if tags:
        previous = max(tags)[1]
        is_ancestor = subprocess.run(["git", "-C", repo_dir, "merge-base", "--is-ancestor", previous,
                                      f"origin/{branch}"], capture_output=True).returncode == 0
        if is_ancestor:
            return previous
    return _git(repo_dir, "merge-base", "origin/main", f"origin/{branch}").strip()


def branch_picks(repo_dir, base, branch):
    """(picks, reverted): the commits on the branch since base, without merges and version bumps, and
    the picks whose pull request a later revert on the same branch undid."""
    log = _git(repo_dir, "log", "--no-merges", "--format=%H%x1f%s", f"{base}..origin/{branch}")
    commits = [line.split("\x1f", 1) for line in log.splitlines() if line]
    undone = {n for _, subject in commits if (n := reverted_number(subject)) is not None}
    picks, reverted = [], []
    for sha, subject in commits:
        if _REVERT.match(subject) or _VERSION_BUMP.match(subject):
            continue
        pick = Pick(sha, subject, pr_number_from_subject(subject), branch)
        (reverted if pick.pr in undone else picks).append(pick)
    return picks, reverted


def _patch_id(repo_dir, sha):
    diff = _git(repo_dir, "show", "--format=", sha)
    out = _git(repo_dir, "patch-id", "--stable", stdin=diff).split()
    return out[0] if out else None


def patch_id_matches(repo_dir, picks, limit=50):
    """{pick sha: main sha} for picks without a pull request number whose change equals a commit on
    main touching the same files."""
    matches = {}
    for pick in picks:
        if pick.pr is not None:
            continue
        files = _git(repo_dir, "diff-tree", "--no-commit-id", "--name-only", "-r", pick.sha).split("\n")
        files = [f for f in files if f]
        if not files:
            continue
        fork = _git(repo_dir, "merge-base", "origin/main", pick.sha).strip()
        candidates = _git(repo_dir, "log", "--no-merges", "--format=%H", "-n", str(limit),
                          f"{fork}..origin/main", "--", *files).split()
        wanted = _patch_id(repo_dir, pick.sha)
        for candidate in candidates:
            if wanted and _patch_id(repo_dir, candidate) == wanted:
                matches[pick.sha] = candidate
                break
    return matches
