#!/usr/bin/env python3
"""Keeps the release project in line with the release-pick label and the release branches.

Reads everything first, prints the planned changes as Markdown (also to $GITHUB_STEP_SUMMARY), and
changes the project only with --apply. With RELEASE_STATUS_DISCORD_WEBHOOK set, an applied run also
edits the Discord status post (RELEASE_STATUS_MESSAGE_ID), or creates it when no id is set and
CREATE_DISCORD_POST is "true"; a dry run prints a preview. A Discord failure is a warning, not a failed run. Run by .github/workflows/release_pick.yml.

  RELEASE_PROJECT_TOKEN=... python3 scripts/release_pick_sync.py --owner OrcaSlicer --project 15 \\
      --repo OrcaSlicer/OrcaSlicer --repo-dir . [--apply]

The clone needs origin/main, origin/rc/*, origin/release/* and the release tags.
"""

import argparse
import datetime
import os
import re
import subprocess
import sys
from dataclasses import replace

from release_pick_git import branch_base, branch_picks, patch_id_matches, release_branches, remote_branches
from release_pick_discord import Discord, DiscordError, build_message
from release_pick_github import GitHub, GitHubError
from release_pick_rules import needs_attention, plan_changes, project_after

LABEL = "release-pick"
DISCORD_POST_TITLE = "Release status"


def collect_picks(gh, repo_dir, repo, milestones):
    """(picks, resolved, reverted): {milestone: {pr: branch}}, every pick with the pull request it was
    matched to (None when none was found), and the picks a later revert undid."""
    branches = remote_branches(repo_dir)
    picks, resolved, reverted = {}, [], []
    for milestone in milestones:
        for branch in release_branches(milestone, branches):
            found, undone = branch_picks(repo_dir, branch_base(repo_dir, milestone, branch), branch)
            matches = patch_id_matches(repo_dir, found + undone)

            def number_of(pick):
                if pick.pr is None and pick.sha in matches:
                    prs = gh.prs_for_commit(repo, matches[pick.sha])
                    return prs[0] if prs else None
                return pick.pr

            for pick in found:
                number = number_of(pick)
                resolved.append((pick, number))
                if number is not None:
                    picks.setdefault(milestone, {}).setdefault(number, branch)
            # A reverted pick with no pull request found cancels out on the branch; there is nothing to report.
            reverted += [replace(pick, pr=number) for pick in undone if (number := number_of(pick)) is not None]
    # A pull request picked again after its revert is on the release; the revert needs no attention.
    reverted = [p for p in reverted if p.pr not in picks.get(_release_of(p.branch), {})]
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
        elif change.action == "archive":
            lines.append(f"- #{change.number}: archived ({change.reason})")
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
            elif change.action == "archive":
                gh.archive_item(project.id, change.item_id)
            else:
                item_id = change.item_id or new_ids[change.number]
                gh.set_status(project.id, item_id, project.status_field_id, project.status_options[change.status])
        except (GitHubError, KeyError) as error:
            return applied, f"#{change.number} {change.action}: {error}"
    return len(changes), None


def _release_of(branch):
    match = re.search(r"v(\d+\.\d+\.\d+)$", branch)
    return match.group(1) if match else None


def _discord_payload(project, labelled, changes, reverted, unmatched, milestones, owner, number, repo, now):
    by_number = {i.number: i for i in project.items}
    attention = [(by_number[n].milestone, n, by_number[n].kind, "closed without a merge")
                 for n in needs_attention(project.items)]
    attention += [(_release_of(p.branch), p.pr, "PullRequest", f"reverted on {p.branch} ({_short(p)})")
                  for p in reverted]
    by_release = {}
    for pick in unmatched:
        by_release.setdefault(_release_of(pick.branch), []).append((pick.branch, _short(pick)))
    return build_message(project_after(project.items, labelled, changes), attention, by_release, milestones,
                         f"https://github.com/orgs/{owner}/projects/{number}", repo, now)


def run(gh, repo_dir, owner, number, repo, apply, out, discord=None, post_id=None, allow_create=False, now=None):
    try:
        project = gh.project(owner, number, repo)
        labelled = gh.labelled_items(repo, LABEL)
        milestones = gh.open_milestones(repo)
        picks, resolved, reverted = collect_picks(gh, repo_dir, repo, milestones)
    except (GitHubError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", None) or error
        print(f"Reading failed, nothing changed: {detail}", file=out)
        return 1

    reverted_prs = {(_release_of(p.branch), p.pr) for p in reverted}
    changes = plan_changes(project.items, labelled, picks, milestones, reverted_prs)
    tracked = _tracked_prs(project.items + labelled)
    unmatched = [pick for pick, number in resolved if number is None or number not in tracked]

    lines = _report(changes, needs_attention(project.items), reverted, unmatched)
    now = now or datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")

    def payload():
        return _discord_payload(project, labelled, changes, reverted, unmatched, milestones, owner, number, repo,
                                now)

    if apply:
        applied, error = _apply(gh, project, changes)
        if error:
            lines += ["", f"Failed: {error}"]
        lines += ["", f"Applied {applied} of {len(changes)} changes."]
        code = 1 if error else 0
        if discord and not error:
            # Discord is a view of the project: any failure there is a warning, never a failed sync. Errors
            # from Discord itself never carry the webhook URL.
            try:
                if post_id:
                    discord.edit_post(post_id, payload())
                    lines.append("Updated the Discord status post.")
                elif allow_create:
                    new_id = discord.create_post(DISCORD_POST_TITLE, payload())
                    lines.append(f"Created the Discord status post. Set the repository variable "
                                 f"RELEASE_STATUS_MESSAGE_ID to {new_id} so later runs edit it.")
                else:
                    lines.append("::notice::No RELEASE_STATUS_MESSAGE_ID yet: run the workflow by hand once with "
                                 "\"Create the Discord status post\" ticked to create it.")
            except Exception as discord_error:  # noqa: BLE001
                lines.append(f"::warning::Discord update failed: {discord_error}")
    else:
        lines += ["", "Dry run: no changes applied."]
        code = 0
        if discord:
            try:
                embeds = payload()["embeds"]
                lines += ["", "## Discord post (preview, not sent)"]
                for embed in embeds:
                    lines += ["", f"### {embed['title']}", embed["description"]]
            except Exception as discord_error:  # noqa: BLE001
                lines.append(f"::warning::Discord preview failed: {discord_error}")
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
        # The workflow skips before this until the app is set up; a local run without a token reads nothing.
        print("::notice::RELEASE_PROJECT_TOKEN is not set; skipping the release-pick sync.")
        return 0

    class Tee:
        def __init__(self):
            self.parts = []

        def write(self, text):
            self.parts.append(text)
            sys.stdout.write(text)

    out = Tee()
    webhook = os.environ.get("RELEASE_STATUS_DISCORD_WEBHOOK")
    code = run(GitHub(token, label=LABEL), args.repo_dir, args.owner, args.project, args.repo, args.apply, out,
               discord=Discord(webhook) if webhook else None,
               post_id=os.environ.get("RELEASE_STATUS_MESSAGE_ID") or None,
               # Only a manual run that asks for it creates the post, so no other run can add another.
               allow_create=os.environ.get("CREATE_DISCORD_POST") == "true")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        # Workflow commands only mean something in the log; the summary page shows plain words.
        text = re.sub(r"(?m)^::warning::", "Warning: ", "".join(out.parts))
        text = re.sub(r"(?m)^::notice::", "Note: ", text)
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
