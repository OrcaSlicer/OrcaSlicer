#!/usr/bin/env python3
"""Keeps the release project in line with the release-pick label and the release branches.

Reads everything first, prints the planned changes as Markdown (also to $GITHUB_STEP_SUMMARY), and
changes the project only with --apply. Run by .github/workflows/release_pick.yml.

  RELEASE_PROJECT_TOKEN=... python3 scripts/release_pick_sync.py --owner OrcaSlicer --project 15 \\
      --repo OrcaSlicer/OrcaSlicer --repo-dir . [--apply]

The clone needs origin/main, origin/rc/*, origin/release/* and the release tags.
"""

import argparse
import os
import subprocess
import sys

from release_pick_git import branch_base, branch_picks, patch_id_matches, release_branches, remote_branches
from release_pick_github import GitHub, GitHubError
from release_pick_rules import needs_attention, plan_changes

LABEL = "release-pick"


def collect_picks(gh, repo_dir, repo, milestones):
    """(picks, resolved, reverted): {milestone: {pr: branch}}, every pick with the pull request it was
    matched to (None when none was found), and the picks a later revert undid."""
    branches = remote_branches(repo_dir)
    picks, resolved, reverted = {}, [], []
    for milestone in milestones:
        for branch in release_branches(milestone, branches):
            found, undone = branch_picks(repo_dir, branch_base(repo_dir, milestone, branch), branch)
            reverted.extend(undone)
            matches = patch_id_matches(repo_dir, found)
            for pick in found:
                number = pick.pr
                if number is None and pick.sha in matches:
                    prs = gh.prs_for_commit(repo, matches[pick.sha])
                    number = prs[0] if prs else None
                resolved.append((pick, number))
                if number is not None:
                    picks.setdefault(milestone, {}).setdefault(number, branch)
    return picks, resolved, reverted


def _tracked_prs(items):
    """Pull request numbers the project follows, directly or through an issue they close."""
    numbers = set()
    for item in items:
        if item.kind == "PullRequest":
            numbers.add(item.number)
        numbers.update(pr.number for pr in item.linked_prs)
    return numbers


def _short(pick):
    return f"{pick.sha[:9]} {pick.subject}"


def _report(changes, attention, reverted, unmatched):
    lines = ["## Planned changes"]
    new_status = {c.number: c.status for c in changes if c.action == "set_status" and c.item_id is None}
    for change in changes:
        if change.action == "add":
            lines.append(f"- #{change.number}: added as {new_status.get(change.number, 'no status')}")
        elif change.action == "remove":
            lines.append(f"- #{change.number}: removed ({change.reason})")
        elif change.item_id is not None:
            lines.append(f"- #{change.number}: {change.old_status or 'no status'} → {change.status} ({change.reason})")
    if not changes:
        lines.append("none")

    lines += ["", "## Needs attention"]
    lines += [f"- #{n}: closed without a merge" for n in attention]
    lines += [f"- #{p.pr}: reverted on {p.branch} ({_short(p)})" for p in reverted]
    if not attention and not reverted:
        lines.append("none")

    if unmatched:
        for branch in dict.fromkeys(p.branch for p in unmatched):
            lines += ["", f"## Unmatched commits on {branch}"]
            lines += [f"- {_short(p)}" for p in unmatched if p.branch == branch]
    else:
        lines += ["", "## Unmatched commits", "none"]
    return lines


def _apply(gh, project, changes):
    """Applies changes in order and stops at the first failure. Returns (applied count, error)."""
    new_ids = {}
    for applied, change in enumerate(changes):
        try:
            if change.action == "add":
                new_ids[change.number] = gh.add_item(project.id, change.content_id)
            elif change.action == "remove":
                gh.remove_item(project.id, change.item_id)
            else:
                item_id = change.item_id or new_ids[change.number]
                gh.set_status(project.id, item_id, project.status_field_id, project.status_options[change.status])
        except (GitHubError, KeyError) as error:
            return applied, f"#{change.number} {change.action}: {error}"
    return len(changes), None


def run(gh, repo_dir, owner, number, repo, apply, out):
    try:
        project = gh.project(owner, number, repo)
        labelled = gh.labelled_items(repo, LABEL)
        milestones = gh.open_milestones(repo)
        picks, resolved, reverted = collect_picks(gh, repo_dir, repo, milestones)
    except (GitHubError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", None) or error
        print(f"Reading failed, nothing changed: {detail}", file=out)
        return 1

    changes = plan_changes(project.items, labelled, picks, milestones)
    tracked = _tracked_prs(project.items + labelled)
    unmatched = [pick for pick, number in resolved if number is None or number not in tracked]

    lines = _report(changes, needs_attention(project.items), reverted, unmatched)
    if apply:
        applied, error = _apply(gh, project, changes)
        if error:
            lines += ["", f"Failed: {error}"]
        lines += ["", f"Applied {applied} of {len(changes)} changes."]
        code = 1 if error else 0
    else:
        lines += ["", "Dry run: no changes applied."]
        code = 0
    print("\n".join(lines), file=out)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--project", type=int, required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--repo-dir", default=".")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    token = os.environ.get("RELEASE_PROJECT_TOKEN")
    if not token:
        # Expected until the secret is added, and on runs that get no secrets: skip instead of failing.
        print("::notice::RELEASE_PROJECT_TOKEN is not set; skipping the release-pick sync.")
        return 0

    class Tee:
        def __init__(self):
            self.parts = []

        def write(self, text):
            self.parts.append(text)
            sys.stdout.write(text)

    out = Tee()
    code = run(GitHub(token, label=LABEL), args.repo_dir, args.owner, args.project, args.repo, args.apply, out)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("".join(out.parts))
    return code


if __name__ == "__main__":
    sys.exit(main())
