#!/usr/bin/env python3
"""The release status post in Discord: one forum post whose starter message is edited in place on every
applied run. It lists, per open release, the count for each status, the items Ready to pick with their
titles, the items In progress and Proposed (with titles while the post has room, numbers only once it
does not), and anything that needs attention. It never mentions anyone."""

import json
import re

from release_pick_github import _urllib_transport
from release_pick_rules import STATUSES, parse_version

# Discord limits: 4096 characters per embed description, 6000 for the whole message, 10 embeds, 256 per
# embed title. Lengths here are code points; the margin below 6000 covers the timestamp and a few
# characters that count double in UTF-16, not a message made mostly of emoji.
_DESCRIPTION_LIMIT = 4096
_MESSAGE_LIMIT = 5800
_MAX_EMBEDS = 10
_NAME_LENGTH = 100
_TITLE_LENGTH = 100
_READY_CAP = 15
_LIST_CAP = 60
_LINKS_PER_LINE = 10  # numbers-only lists are split so trimming can shorten a list, not drop it
_NOTES_CAP = 10
_COLOR = 0x4CB6FF
_USER_AGENT = "DiscordBot (https://github.com/OrcaSlicer/OrcaSlicer, 1)"


class DiscordError(Exception):
    pass


def _escape(text):
    """Text shown as is: no formatting, and no masked links, mentions or channel links of its own."""
    return re.sub(r"([\\*_~`|\[\]()<>])", r"\\\1", text)


def _shorten(text, length):
    return text if len(text) <= length else text[:length - 1] + "…"


def _link(number, kind, repo):
    path = "pull" if kind == "PullRequest" else "issues"
    return f"[#{number}](https://github.com/{repo}/{path}/{number})"


def _capped(lines, cap, project_url):
    if len(lines) <= cap:
        return lines
    return lines[:cap] + [f"…and {len(lines) - cap} more in the [project]({project_url})"]


def _titled(listed, repo):
    return [f"{_link(i.number, i.kind, repo)} {_escape(_shorten(i.title, _TITLE_LENGTH))}" for i in listed]


def _lines(items, notes, unmatched, project_url, repo, compact=()):
    """compact: the statuses among In progress and Proposed listed as numbers only, not one titled line each."""
    by_status = {status: sorted((i for i in items if i.status == status), key=lambda i: i.number)
                 for status in STATUSES}
    tally = [f"**{status}** {len(by_status[status])}" for status in reversed(STATUSES)]
    other = sum(1 for i in items if i.status not in STATUSES)
    if other:
        tally.append(f"**Other** {other}")
    lines = [" · ".join(tally)]

    ready = by_status["Ready to pick"]
    if ready:
        lines += ["", f"**Ready to pick ({len(ready)})**"] + _capped(_titled(ready, repo), _READY_CAP, project_url)
    for status in ("In progress", "Proposed"):
        listed = by_status[status]
        if listed and status not in compact:
            lines += ["", f"**{status} ({len(listed)})**"] + _capped(_titled(listed, repo), _LIST_CAP, project_url)
        elif listed:
            links = [_link(i.number, i.kind, repo) for i in listed[:_LIST_CAP]]
            rows = [" · ".join(links[k:k + _LINKS_PER_LINE]) for k in range(0, len(links), _LINKS_PER_LINE)]
            if len(listed) > _LIST_CAP:
                rows[-1] += f" …and {len(listed) - _LIST_CAP} more in the [project]({project_url})"
            lines += ["", f"**{status} ({len(listed)})**"] + rows
    if notes:
        rows = [f"{_link(n, kind, repo)} {_escape(_shorten(reason, _TITLE_LENGTH))}" for n, kind, reason in notes]
        lines += ["", "**Needs attention**"] + _capped(rows, _NOTES_CAP, project_url)
    for branch in dict.fromkeys(b for b, _ in unmatched):
        rows = [_escape(_shorten(line, _TITLE_LENGTH)) for b, line in unmatched if b == branch]
        lines += ["", f"**Unmatched commits on {_escape(branch)}**"] + _capped(rows, _NOTES_CAP, project_url)
    return lines


def _too_big(texts):
    """texts: [(embed title, description)]."""
    return (sum(len(title) + len(text) for title, text in texts) > _MESSAGE_LIMIT
            or any(len(text) > _DESCRIPTION_LIMIT for _, text in texts))


def _fit(blocks, project_url):
    """Drops whole lines from the longest description until every description and the whole message fit,
    ending each shortened description with a pointer to the project."""
    more = f"…more in the [project]({project_url})"
    cut = [False] * len(blocks)

    def text(index):
        return "\n".join(blocks[index][1] + ([more] if cut[index] else []))

    while _too_big([(title, text(i)) for i, (title, _) in enumerate(blocks)]):
        index = max(range(len(blocks)), key=lambda i: len(text(i)))
        lines = blocks[index][1]
        if len(lines) <= 1:
            break
        lines.pop()
        while len(lines) > 1 and lines[-1] == "":
            lines.pop()
        cut[index] = True
    return [(title, text(i)) for i, (title, _) in enumerate(blocks)]


def build_message(items, attention, unmatched, open_milestones, project_url, repo, now):
    """The webhook payload.

    items: the project after this run's changes; attention: [(milestone, number, kind, reason)];
    unmatched: {milestone: [(branch, "<sha> <subject>")]}; open_milestones: the open release titles;
    now: ISO 8601. Releases on closed milestones are left out; with more than ten, the newest are kept.
    """
    shown = set(open_milestones)
    releases, notes = {}, {}
    for item in items:
        if item.milestone is None or item.milestone in shown:
            releases.setdefault(item.milestone, []).append(item)
    for milestone, number, kind, reason in attention:
        if milestone is None or milestone in shown:
            notes.setdefault(milestone, []).append((number, kind, reason))
    keys = set(releases) | set(notes) | {m for m in unmatched if m in shown}
    versions = sorted((m for m in keys if m is not None), key=parse_version)
    slots = _MAX_EMBEDS - (1 if None in keys else 0)
    order = versions[-slots:] + ([None] if None in keys else [])

    compact = {milestone: set() for milestone in order}

    def render():
        blocks = []
        for milestone in order:
            members = releases.get(milestone, [])
            name = _shorten(milestone, _NAME_LENGTH) if milestone else "No milestone"
            count = f"{len(members)} item" + ("" if len(members) == 1 else "s")
            blocks.append((f"{name} · {count}", _lines(members, notes.get(milestone, []), unmatched.get(milestone, []),
                                                      project_url, repo, compact[milestone])))
        return blocks

    def titled_sections(milestones):
        """(size of the titled lines shown, position, milestone, status) for each section still titled."""
        found = []
        for index, milestone in enumerate(milestones):
            for status in ("In progress", "Proposed"):
                listed = sorted((i for i in releases.get(milestone, []) if i.status == status), key=lambda i: i.number)
                if listed and status not in compact[milestone]:
                    found.append((len("\n".join(_titled(listed[:_LIST_CAP], repo))), index, milestone, status))
        return found

    # Titles while they fit: the largest titled section goes to numbers only until the message fits. A
    # description over its own limit gives up its own titles; other releases give up theirs only while
    # the whole message is too long even once each description is cut to its limit.
    blocks = render()
    while _too_big([(title, "\n".join(lines)) for title, lines in blocks]):
        texts = [(title, "\n".join(lines)) for title, lines in blocks]
        over = [m for m, (_, text) in zip(order, texts) if len(text) > _DESCRIPTION_LIMIT]
        sections = titled_sections(over)
        if not sections and sum(len(t) + min(len(x), _DESCRIPTION_LIMIT) for t, x in texts) > _MESSAGE_LIMIT:
            sections = titled_sections(order)
        if not sections:
            break
        _, _, milestone, status = max(sections)
        compact[milestone].add(status)
        blocks = render()
    if not blocks:
        blocks.append(("Release status", ["No release-pick items in open milestones."]))

    embeds = [{"title": title, "url": project_url, "color": _COLOR, "description": description, "timestamp": now}
              for title, description in _fit(blocks, project_url)]
    return {"embeds": embeds, "allowed_mentions": {"parse": []}}


class Discord:
    """A webhook on a forum channel. The post's id is both its thread id and its starter message id."""

    def __init__(self, webhook_url, transport=None):
        self._url = webhook_url.strip().rstrip("/")
        self._transport = transport or _urllib_transport

    def _send(self, method, url, payload):
        # The webhook URL holds its token, so neither it nor an exception text that may quote it goes into
        # an error.
        headers = {"Content-Type": "application/json", "User-Agent": _USER_AGENT}
        try:
            status, data = self._transport(method, url, json.dumps(payload).encode(), headers)
        except Exception as error:  # noqa: BLE001 - any transport failure, reported without its text
            raise DiscordError(f"Discord {method} failed: {type(error).__name__}") from None
        if status not in (200, 204):
            raise DiscordError(f"Discord {method} failed: HTTP {status}: {data}")
        return data

    def create_post(self, thread_name, payload):
        data = self._send("POST", f"{self._url}?wait=true", {**payload, "thread_name": thread_name})
        try:
            return str(data["id"])
        except (KeyError, TypeError):
            raise DiscordError("Discord POST failed: the reply had no message id") from None

    def edit_post(self, post_id, payload):
        self._send("PATCH", f"{self._url}/messages/{post_id}?thread_id={post_id}", payload)
