#!/usr/bin/env python3
"""Decides how the release project should change: which issues and pull requests belong in it and which
status each one has. Pure functions only; release_pick_sync.py reads the data and applies the result.

- The release-pick label is the only way in. A labelled item is added when its milestone is open or it
  has none; an unlabelled item is archived unless it is on its release branch, or Picked and its pick
  was not reverted.
- Items in a status column other than the four below are left alone.
- Archived project items are never touched, even when labelled again; a maintainer unarchives them.
- Statuses only move forward: Proposed, In progress, Ready to pick, Picked. The one exception is a
  Picked item whose pick was reverted on its release branch, which goes back to the status it now has.
- An item is Picked when its merged pull request is on a release branch of its own milestone.
"""

import re
from dataclasses import dataclass, replace

STATUSES = ("Proposed", "In progress", "Ready to pick", "Picked")

# milestone -> {pull request number -> release branch it was picked onto}
Picks = dict


@dataclass(frozen=True)
class LinkedPr:
    number: int
    state: str  # OPEN, CLOSED or MERGED
    milestone: str | None = None


@dataclass(frozen=True)
class Item:
    number: int
    kind: str  # Issue or PullRequest
    state: str  # OPEN, CLOSED or MERGED
    labelled: bool
    milestone: str | None
    milestone_open: bool
    content_id: str
    linked_prs: tuple = ()
    item_id: str | None = None  # set for items already in the project
    status: str | None = None
    archived: bool = False
    title: str = ""


@dataclass(frozen=True)
class Change:
    action: str  # add, archive or set_status
    number: int
    content_id: str
    item_id: str | None
    old_status: str | None
    status: str | None
    reason: str


def parse_version(title):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", title or "")
    return tuple(int(part) for part in match.groups()) if match else None


def _rank(status):
    return STATUSES.index(status) if status in STATUSES else -1


def _release_milestones(milestones, open_milestones):
    """The first of milestones that is a version, or every open milestone when there is none."""
    own = next((m for m in milestones if parse_version(m)), None)
    return [own] if own else [m for m in open_milestones if parse_version(m)]


def _picked_branch(pr_number, milestones, picks, open_milestones):
    """The branch the pull request was picked onto, on a release of _release_milestones."""
    for milestone in _release_milestones(milestones, open_milestones):
        branch = picks.get(milestone, {}).get(pr_number)
        if branch:
            return branch
    return None


def derived_status(item, picks, open_milestones):
    """(status, reason) from the item's GitHub state, or None for an item closed without a merge."""
    if item.kind == "PullRequest":
        prs = [LinkedPr(item.number, item.state, item.milestone)]
    else:
        prs = list(item.linked_prs)
    best = None
    for linked in prs:
        if linked.state == "OPEN":
            found = ("In progress", f"#{linked.number} is open")
        elif linked.state == "MERGED":
            branch = _picked_branch(linked.number, [item.milestone, linked.milestone], picks, open_milestones)
            found = ("Picked", f"on {branch}") if branch else ("Ready to pick", f"#{linked.number} merged")
        else:
            continue
        if best is None or _rank(found[0]) > _rank(best[0]):
            best = found
    if best:
        return best
    if item.state == "OPEN":
        return ("Proposed", "no pull request yet")
    return None


def _wanted(item):
    return item.labelled and (item.milestone_open or item.milestone is None)


def _reverted(item, reverted, open_milestones):
    prs = [LinkedPr(item.number, item.state, item.milestone)] if item.kind == "PullRequest" else item.linked_prs
    return any((milestone, linked.number) in reverted
               for linked in prs for milestone in _release_milestones([item.milestone, linked.milestone],
                                                                       open_milestones))


def plan_changes(project, labelled, picks, open_milestones, reverted=frozenset()):
    """reverted: (milestone, pull request number) of each pick a revert undid on that release's branch."""
    in_project = {(i.kind, i.number) for i in project}
    adds, archives, statuses = [], [], []

    for item in labelled:
        if (item.kind, item.number) in in_project or not _wanted(item):
            continue
        adds.append(Change("add", item.number, item.content_id, None, None, None, "labelled release-pick"))
        derived = derived_status(item, picks, open_milestones)
        if derived:
            statuses.append(Change("set_status", item.number, item.content_id, None, None, *derived))

    for item in project:
        # A status outside the four is a column a maintainer added by hand; leave those items alone.
        if item.archived or (item.status is not None and item.status not in STATUSES):
            continue
        derived = derived_status(item, picks, open_milestones)
        picked_now = bool(derived) and derived[0] == "Picked"
        pick_reverted = (item.status == "Picked" and bool(derived) and not picked_now
                  and _reverted(item, reverted, open_milestones))
        if not item.labelled and (item.status != "Picked" or pick_reverted) and not picked_now:
            archives.append(Change("archive", item.number, item.content_id, item.item_id, item.status, None,
                                   "no release-pick label"))
            continue
        if pick_reverted:
            statuses.append(Change("set_status", item.number, item.content_id, item.item_id, item.status,
                                   derived[0], "pick reverted"))
        elif derived and _rank(derived[0]) > _rank(item.status):
            statuses.append(Change("set_status", item.number, item.content_id, item.item_id, item.status,
                                   *derived))

    def by_number(change):
        return change.number

    return sorted(adds, key=by_number) + sorted(archives, key=by_number) + sorted(statuses, key=by_number)


def needs_attention(project):
    """Numbers of project items closed without a merge."""
    return sorted(i.number for i in project
                  if not i.archived and i.state == "CLOSED" and derived_status(i, {}, []) is None)


def project_after(project, labelled, changes):
    """The non-archived project items once changes are applied, with their new statuses."""
    archived = {c.number for c in changes if c.action == "archive"}
    added = {c.number for c in changes if c.action == "add"}
    status = {c.number: c.status for c in changes if c.action == "set_status"}
    items = [i for i in project if not i.archived and i.number not in archived]
    items += [i for i in labelled if i.number in added]
    return [replace(i, status=status.get(i.number, i.status)) for i in items]
