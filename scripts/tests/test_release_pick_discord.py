#!/usr/bin/env python3
"""Tests for scripts/release_pick_discord.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts -v
"""

import json
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from release_pick_discord import Discord, DiscordError, build_message  # noqa: E402
from release_pick_rules import Item  # noqa: E402

REPO = "o/r"
PROJECT = "https://github.com/orgs/o/projects/1"
NOW = "2026-11-18T17:30:00+00:00"
OPEN = ["2.5.2", "2.5.3"]


def item(number, status, kind="PullRequest", milestone="2.5.2", title=None, state="OPEN"):
    return Item(number=number, kind=kind, state=state, labelled=True, milestone=milestone, milestone_open=True,
                content_id=f"C_{number}", item_id=f"P_{number}", status=status, title=title or f"Title {number}")


def build(items, attention=(), unmatched=None, milestones=OPEN):
    return build_message(items, list(attention), unmatched or {}, list(milestones), PROJECT, REPO, NOW)


def total(message):
    return sum(len(e["title"]) + len(e["description"]) for e in message["embeds"])


class BuildMessageTest(unittest.TestCase):
    def test_one_embed_per_release_with_counts(self):
        message = build([item(1, "Picked"), item(2, "Ready to pick"), item(3, "In progress", kind="Issue"),
                         item(4, "Proposed", milestone="2.5.3")])
        self.assertEqual([e["title"] for e in message["embeds"]], ["2.5.2 · 3 items", "2.5.3 · 1 item"])
        self.assertEqual(message["embeds"][0]["url"], PROJECT)
        self.assertIn("**Picked** 1 · **Ready to pick** 1 · **In progress** 1 · **Proposed** 0",
                      message["embeds"][0]["description"])
        self.assertEqual(message["embeds"][0]["timestamp"], NOW)

    def test_titles_are_shown_when_they_fit_and_picked_items_only_counted(self):
        text = build([item(1, "Picked"), item(2, "Ready to pick", title="Fix the *thing*"),
                      item(3, "In progress", kind="Issue"), item(4, "Proposed")])["embeds"][0]["description"]
        self.assertIn("[#2](https://github.com/o/r/pull/2) Fix the \\*thing\\*", text)
        self.assertIn("**In progress (1)**\n[#3](https://github.com/o/r/issues/3) Title 3", text)
        self.assertIn("**Proposed (1)**\n[#4](https://github.com/o/r/pull/4) Title 4", text)
        self.assertNotIn("#1]", text)

    def test_largest_section_drops_its_titles_first(self):
        items = [item(n, "In progress", title="In progress title " * 5) for n in range(1000, 1030)]
        items += [item(n, "Proposed", title="Proposed title") for n in range(2000, 2010)]
        text = build(items)["embeds"][0]["description"]
        self.assertNotIn("In progress title", text)
        self.assertIn("[#1000](https://github.com/o/r/pull/1000) · [#1001]", text)
        self.assertIn("[#2000](https://github.com/o/r/pull/2000) Proposed title", text)
        self.assertNotIn("…more in the", text)

    def test_release_that_fits_keeps_its_titles_when_another_is_too_long(self):
        items = [item(n, "In progress", title="Long title " * 9) for n in range(1000, 1040)]
        items.append(item(5, "Proposed", milestone="2.5.3", title="Small release"))
        first, second = (e["description"] for e in build(items)["embeds"])
        self.assertNotIn("Long title", first)
        self.assertIn("[#5](https://github.com/o/r/pull/5) Small release", second)

    def test_other_release_gives_up_titles_before_notes_are_cut(self):
        items = [item(n, "Ready to pick", title="R" * 100) for n in range(17000, 17015)]
        items += [item(n, "Proposed", milestone="2.5.3", title="P" * 75) for n in range(18000, 18018)]
        attention = [("2.5.2", n, "PullRequest", "closed without a merge " * 5) for n in range(19000, 19010)]
        message = build_message(items, attention, {}, OPEN, PROJECT, "OrcaSlicer/OrcaSlicer", NOW)
        first, second = (e["description"] for e in message["embeds"])
        self.assertNotIn("PPP", second)
        self.assertGreater(len(first), 3900)  # cut only to its own limit, not to make room for titles
        self.assertLessEqual(total(message), 5800)

    def test_release_over_its_own_limit_leaves_other_titles_alone(self):
        items = [item(n, "Ready to pick", title="R" * 100) for n in range(17000, 17015)]
        items += [item(n, "Proposed", milestone="2.5.3", title="Small release") for n in range(18000, 18003)]
        attention = [("2.5.2", n, "PullRequest", "closed without a merge " * 5) for n in range(19000, 19010)]
        message = build_message(items, attention, {}, OPEN, PROJECT, "OrcaSlicer/OrcaSlicer", NOW)
        self.assertIn("[#18000](https://github.com/OrcaSlicer/OrcaSlicer/pull/18000) Small release",
                      message["embeds"][1]["description"])

    def test_long_numbers_only_list_is_shortened_not_dropped(self):
        items = [item(n, "Ready to pick", title="R" * 60) for n in range(16000, 16015)]
        items += [item(n, "Proposed", kind="Issue", title="Long title " * 9) for n in range(17000, 17100)]
        attention = [("2.5.2", n, "PullRequest", "closed without a merge") for n in range(19000, 19003)]
        message = build_message(items, attention, {}, ["2.5.2"], PROJECT, "OrcaSlicer/OrcaSlicer", NOW)
        text = message["embeds"][0]["description"]
        self.assertIn("[#17000](https://github.com/OrcaSlicer/OrcaSlicer/issues/17000) · [#17001]", text)
        self.assertGreaterEqual(text.count("/issues/170"), 20)
        self.assertNotIn("Long title", text)
        self.assertGreater(len(text), 3500)
        self.assertLessEqual(len(text), 4096)

    def test_all_sections_drop_titles_before_lines_are_cut(self):
        items = [item(n, "In progress", title="Long title " * 9) for n in range(1000, 1060)]
        items += [item(n, "Proposed", title="Long title " * 9) for n in range(2000, 2060)]
        message = build(items)
        text = message["embeds"][0]["description"]
        self.assertNotIn("Long title", text)
        self.assertLessEqual(len(text), 4096)
        self.assertLessEqual(total(message), 5800)

    def test_title_cannot_carry_its_own_link(self):
        text = build([item(2, "Ready to pick", title="[free](https://evil.example) <#123>")])["embeds"][0]["description"]
        self.assertIn("\\[free\\]\\(https://evil.example\\) \\<#123\\>", text)

    def test_long_milestone_name_is_bounded(self):
        milestone = "1.0." + "9" * 300
        message = build([item(1, "Proposed", milestone=milestone)], milestones=[milestone])
        self.assertLessEqual(len(message["embeds"][0]["title"]), 256)

    def test_items_without_a_status_are_counted(self):
        text = build([item(1, "Proposed"), item(2, None), item(3, "On hold")])["embeds"][0]["description"]
        self.assertIn("**Other** 2", text)

    def test_never_pings(self):
        self.assertEqual(build([item(1, "Proposed")])["allowed_mentions"], {"parse": []})

    def test_items_without_milestone_come_last(self):
        message = build([item(1, "Proposed", milestone=None), item(2, "Proposed")])
        self.assertEqual([e["title"] for e in message["embeds"]], ["2.5.2 · 1 item", "No milestone · 1 item"])

    def test_closed_milestones_are_left_out(self):
        message = build([item(1, "Picked", milestone="2.5.1"), item(2, "Proposed")])
        self.assertEqual([e["title"] for e in message["embeds"]], ["2.5.2 · 1 item"])

    def test_newest_releases_are_kept_when_there_are_too_many(self):
        milestones = [f"2.6.{n}" for n in range(12)]
        message = build([item(n, "Proposed", milestone=m) for n, m in enumerate(milestones, 1)],
                        milestones=milestones)
        titles = [e["title"] for e in message["embeds"]]
        self.assertEqual(len(titles), 10)
        self.assertEqual((titles[0], titles[-1]), ("2.6.2 · 1 item", "2.6.11 · 1 item"))

    def test_attention_and_unmatched_go_to_their_release(self):
        message = build([item(1, "In progress", state="CLOSED"), item(2, "Proposed", milestone="2.5.3")],
                        attention=[("2.5.2", 1, "PullRequest", "closed without a merge")],
                        unmatched={"2.5.2": [("rc/v2.5.2", "abc123456 Translations refresh")]})
        first, second = (e["description"] for e in message["embeds"])
        self.assertIn("**Needs attention**\n[#1](https://github.com/o/r/pull/1) closed without a merge", first)
        self.assertIn("**Unmatched commits on rc/v2.5.2**\nabc123456 Translations refresh", first)
        self.assertNotIn("Needs attention", second)

    def test_notes_for_a_release_without_items_still_show(self):
        message = build([item(2, "Proposed")], attention=[("2.5.3", 9, "PullRequest", "reverted on rc/v2.5.3")])
        self.assertEqual([e["title"] for e in message["embeds"]], ["2.5.2 · 1 item", "2.5.3 · 0 items"])
        self.assertIn("[#9](https://github.com/o/r/pull/9) reverted on rc/v2.5.3", message["embeds"][1]["description"])

    def test_empty_project_still_says_so(self):
        message = build([])
        self.assertEqual(len(message["embeds"]), 1)
        self.assertIn("No release-pick items", message["embeds"][0]["description"])

    def test_large_release_stays_under_discord_limits(self):
        items = [item(n, "Ready to pick", title="A long title " * 20) for n in range(1000, 1200)]
        items += [item(n, "In progress") for n in range(2000, 2400)]
        message = build(items)
        description = message["embeds"][0]["description"]
        self.assertLessEqual(len(description), 4096)
        self.assertLessEqual(total(message), 6000)
        self.assertIn(f"[project]({PROJECT})", description)

    def test_many_releases_with_notes_stay_under_the_total_limit(self):
        milestones = [f"3.0.{n}" for n in range(10)]
        items, attention, unmatched = [], [], {}
        for m_index, milestone in enumerate(milestones):
            for k in range(6):
                number = m_index * 100 + k
                items.append(item(number, ("Ready to pick", "In progress", "Proposed")[k % 3], milestone=milestone,
                                  title="**bold** _x_ " * 12, state="CLOSED"))
                attention.append((milestone, number, "PullRequest", "closed without a merge " * 3))
            unmatched[milestone] = [(f"rc/v{milestone}", "abcdef123 " + "subject " * 30)] * 20
        message = build(items, attention, unmatched, milestones)
        self.assertLessEqual(total(message), 5800)  # margin for clients counting UTF-16 units
        for embed in message["embeds"]:
            self.assertLessEqual(len(embed["description"]), 4096)
            for line in embed["description"].split("\n"):
                whole = len(re.findall(r"\[#\d+\]\(https://[^)\s]+\)", line))
                self.assertEqual(line.count("[#"), whole, line)  # no link cut in half


class FakeTransport:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def __call__(self, method, url, body, headers):
        self.requests.append((method, url, json.loads(body) if body else None, headers))
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response


HOOK = "https://discord.com/api/webhooks/123/secret-token"


class DiscordTest(unittest.TestCase):
    def test_create_post_starts_a_forum_thread(self):
        transport = FakeTransport([(200, {"id": "999", "channel_id": "999"})])
        self.assertEqual(Discord(HOOK, transport).create_post("Release status", {"embeds": []}), "999")
        method, url, body, headers = transport.requests[0]
        self.assertEqual((method, url), ("POST", HOOK + "?wait=true"))
        self.assertEqual(body["thread_name"], "Release status")
        self.assertIn("DiscordBot", headers["User-Agent"])

    def test_edit_post_patches_the_starter_message_in_its_thread(self):
        transport = FakeTransport([(200, {"id": "999"})])
        Discord(HOOK, transport).edit_post("999", {"embeds": []})
        method, url, body, _ = transport.requests[0]
        self.assertEqual((method, url), ("PATCH", HOOK + "/messages/999?thread_id=999"))
        self.assertNotIn("thread_name", body)

    def test_webhook_with_trailing_whitespace_is_trimmed(self):
        transport = FakeTransport([(200, {"id": "999"})])
        Discord(HOOK + "\n", transport).edit_post("999", {})
        self.assertEqual(transport.requests[0][1], HOOK + "/messages/999?thread_id=999")

    def test_error_hides_the_webhook_token(self):
        with self.assertRaises(DiscordError) as raised:
            Discord(HOOK, FakeTransport([(404, {"message": "Unknown Message"})])).edit_post("999", {})
        self.assertNotIn("secret-token", str(raised.exception))
        self.assertIn("404", str(raised.exception))

    def test_network_error_becomes_discord_error_without_the_token(self):
        transport = FakeTransport([ValueError("URL can't contain control characters: /123/secret-token")])
        with self.assertRaises(DiscordError) as raised:
            Discord(HOOK, transport).edit_post("999", {})
        self.assertNotIn("secret-token", str(raised.exception))

    def test_reply_without_an_id_becomes_discord_error(self):
        with self.assertRaises(DiscordError):
            Discord(HOOK, FakeTransport([(200, {})])).create_post("Release status", {})


if __name__ == "__main__":
    unittest.main()
