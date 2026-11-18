#!/usr/bin/env python3
"""The release status post in Discord: one forum post whose starter message is edited in place on every
applied run. It lists, per release, the count for each status, the items Ready to pick with their titles,
the numbers In progress and Proposed, and anything that needs attention. It never mentions anyone."""

import json
import re

from release_pick_github import _urllib_transport
from release_pick_rules import STATUSES, parse_version

# Discord limits: 4096 characters per embed description, 6000 for the whole message, 10 embeds.
_DESCRIPTION_LIMIT = 4096
_MESSAGE_LIMIT = 5900
_MAX_EMBEDS = 10
_TITLE_LENGTH = 100
_COLOR = 0x4CB6FF
_USER_AGENT = "DiscordBot (https://github.com/OrcaSlicer/OrcaSlicer, 1)"


class DiscordError(Exception):
    pass


def _escape(text):
    return re.sub(r"([\\*_~`|])", r"\\\1", text)


def _link(item, repo):
    path = "pull" if item.kind == "PullRequest" else "issues"
    return f"[#{item.number}](https://github.com/{repo}/{path}/{item.number})"


def _more(count, project_url):
    return f"…and {count} more in the [project]({project_url})"


def _release_order(milestone):
    if milestone is None:
        return (2, (), "")
    version = parse_version(milestone)
    return (0, version, "") if version else (1, (), milestone)


def _description(items, attention, unmatched, project_url, repo, ready_cap, list_cap):
    by_status = {status: sorted((i for i in items if i.status == status), key=lambda i: i.number)
                 for status in STATUSES}
    lines = [" · ".join(f"**{status}** {len(by_status[status])}" for status in reversed(STATUSES))]

    ready = by_status["Ready to pick"]
    if ready:
        lines += ["", f"**Ready to pick ({len(ready)})**"]
        for item in ready[:ready_cap]:
            title = item.title if len(item.title) <= _TITLE_LENGTH else item.title[:_TITLE_LENGTH - 1] + "…"
            lines.append(f"{_link(item, repo)} {_escape(title)}")
        if len(ready) > ready_cap:
            lines.append(_more(len(ready) - ready_cap, project_url))

    for status in ("In progress", "Proposed"):
        listed = by_status[status]
        if listed:
            text = " · ".join(_link(i, repo) for i in listed[:list_cap])
            if len(listed) > list_cap:
                text += " " + _more(len(listed) - list_cap, project_url)
            lines += ["", f"**{status} ({len(listed)})**", text]

    if attention:
        lines += ["", "**Needs attention**"] + [f"{_link(item, repo)} {_escape(reason)}" for item, reason in attention]
    for branch in dict.fromkeys(b for b, _ in unmatched):
        commits = [_escape(line) for b, line in unmatched if b == branch]
        shown = commits[:list_cap]
        if len(commits) > len(shown):
            shown.append(f"…and {len(commits) - len(shown)} more")
        lines += ["", f"**Unmatched commits on {branch}**"] + shown
    return "\n".join(lines)


def build_message(items, attention, unmatched, project_url, repo, now):
    """The webhook payload. items: the project after this run's changes (statuses set); attention:
    [(number, reason)]; unmatched: {milestone: [(branch, "<sha> <subject>")]}; now: ISO 8601."""
    releases = {}
    for item in items:
        releases.setdefault(item.milestone, []).append(item)
    order = sorted(releases, key=_release_order)[:_MAX_EMBEDS]
    by_number = {item.number: item for item in items}

    def embeds(ready_cap, list_cap):
        result = []
        for milestone in order:
            members = releases[milestone]
            notes = [(by_number[n], reason) for n, reason in attention
                     if n in by_number and by_number[n].milestone == milestone]
            name = milestone or "No milestone"
            count = f"{len(members)} item" + ("" if len(members) == 1 else "s")
            result.append({
                "title": f"{name} · {count}",
                "url": project_url,
                "color": _COLOR,
                "description": _description(members, notes, unmatched.get(milestone, []), project_url, repo,
                                            ready_cap, list_cap),
                "timestamp": now,
            })
        if not result:
            result.append({"title": "Release status", "url": project_url, "color": _COLOR, "timestamp": now,
                           "description": "No release-pick items in open milestones."})
        return result

    ready_cap, list_cap = 15, 60
    while True:
        result = embeds(ready_cap, list_cap)
        too_long = any(len(e["description"]) > _DESCRIPTION_LIMIT for e in result) or \
            sum(len(e["title"]) + len(e["description"]) for e in result) > _MESSAGE_LIMIT
        if not too_long or (ready_cap, list_cap) == (1, 1):
            break
        ready_cap, list_cap = max(1, ready_cap // 2), max(1, list_cap // 2)
    for embed in result:
        embed["description"] = embed["description"][:_DESCRIPTION_LIMIT]
    return {"embeds": result, "allowed_mentions": {"parse": []}}


class Discord:
    """A webhook on a forum channel. The post's id is both its thread id and its starter message id."""

    def __init__(self, webhook_url, transport=None):
        self._url = webhook_url.rstrip("/")
        self._transport = transport or _urllib_transport

    def _send(self, method, url, payload):
        headers = {"Content-Type": "application/json", "User-Agent": _USER_AGENT}
        status, data = self._transport(method, url, json.dumps(payload).encode(), headers)
        if status not in (200, 204):
            # The webhook URL holds its token, so it never goes into an error.
            raise DiscordError(f"Discord {method} failed: HTTP {status}: {data}")
        return data

    def create_post(self, thread_name, payload):
        data = self._send("POST", f"{self._url}?wait=true", {**payload, "thread_name": thread_name})
        return str(data["id"])

    def edit_post(self, post_id, payload):
        self._send("PATCH", f"{self._url}/messages/{post_id}?thread_id={post_id}", payload)
