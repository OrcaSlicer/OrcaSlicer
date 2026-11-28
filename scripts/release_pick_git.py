#!/usr/bin/env python3
"""Finds which pull requests were cherry-picked onto a release's branches (rc/vX.Y.Z and
release/vX.Y.Z), from a clone whose remote is "origin".

A pick is matched by the "(#N)" at the end of its subject, which squash merges add and cherry-picks
keep; for a revert, the one after the quoted subject (the revert pull request's own). A pick without
one can be matched by its patch id against the commits on main.
"""

import re
import subprocess
from dataclasses import dataclass

from release_pick_rules import parse_version

_TRAILING_NUMBER = re.compile(r"\(#(\d+)\)\s*$")
_REVERT = re.compile(r'^Revert "(.*)"')
_REVERTED_SHA = re.compile(r"This reverts commit ([0-9a-f]{7,40})")
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
    # A revert's quoted subject carries the reverted pull request's number, not its own.
    match = _TRAILING_NUMBER.search(_REVERT.sub("", subject))
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
    """(picks, reverted), newest first: the commits on the branch since base, without merges and version
    bumps, and the picks a later revert on the same branch undid. A revert undoes the commit its message
    names ("This reverts commit ..."), or else the latest pick of the pull request in its subject, so a
    pull request picked again after a revert counts as picked. Reverting that revert restores the pick.
    A revert or reapply with its own "(#N)" after the quoted subject is also a pick of that pull request,
    and undoing such a revert by its commit moves that pull request to reverted."""
    log = _git(repo_dir, "log", "--reverse", "--no-merges", "--format=%H%x1f%s%x1f%b%x1e",
               f"{base}..origin/{branch}")
    picks, reverted = [], []
    undid = {}  # revert sha -> the pick it undid
    redid = {}  # sha of a commit reverting such a revert -> the pick it restored
    for record in log.split("\x1e"):
        if not record.strip():
            continue
        sha, subject, body = record.strip("\n").split("\x1f", 2)
        if _VERSION_BUMP.match(subject):
            continue
        target = _REVERTED_SHA.search(body)
        own = pr_number_from_subject(subject)
        # Reverting a revert ('Revert "Revert ..."', or 'Reapply "..."' since git 2.43) restores the pick.
        restored = next((s for s, p in undid.items() if target and s.startswith(target.group(1)) and p in reverted),
                        None)
        if restored:
            pick = undid.pop(restored)
            reverted.remove(pick)
            picks.append(pick)
            redid[sha] = pick
            # A revert pull request undone here is reverted too.
            revert_pick = next((p for p in picks if p.sha == restored), None)
            if revert_pick:
                picks.remove(revert_pick)
                reverted.append(revert_pick)
            if own is None:
                continue
        elif _REVERT.match(subject):
            number = reverted_number(subject)
            undone = next((p for s, p in redid.items() if target and s.startswith(target.group(1)) and p in picks),
                          None)
            undone = undone or next((p for p in reversed(picks) if target and p.sha.startswith(target.group(1))),
                                    None)
            undone = undone or next((p for p in reversed(picks) if number is not None and p.pr == number), None)
            if undone:
                picks.remove(undone)
                reverted.append(undone)
                undid[sha] = undone
            if own is None:
                continue
        picks.append(Pick(sha, subject, own, branch))
    return picks[::-1], reverted[::-1]


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
