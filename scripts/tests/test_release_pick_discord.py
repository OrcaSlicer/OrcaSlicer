#!/usr/bin/env python3
"""Tests for scripts/release_pick_discord.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts -v
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from release_pick_discord import Discord, DiscordError, build_message  # noqa: E402
from release_pick_rules import Item  # noqa: E402

REPO = "o/r"
PROJECT = "https://github.com/orgs/o/projects/1"
NOW = "2026-11-18T17:30:00+00:00"


def item(number, status, kind="PullRequest", milestone="2.5.2", title=None, state="OPEN"):
    return Item(number=number, kind=kind, state=state, labelled=True, milestone=milestone, milestone_open=True,
                content_id=f"C_{number}", item_id=f"P_{number}", status=status, title=title or f"Title {number}")


def build(items, attention=(), unmatched=None):
    return build_message(items, list(attention), unmatched or {}, PROJECT, REPO, NOW)


class BuildMessageTest(unittest.TestCase):
    def test_one_embed_per_release_with_counts(self):
        message = build([item(1, "Picked"), item(2, "Ready to pick"), item(3, "In progress", kind="Issue"),
                         item(4, "Proposed", milestone="2.5.3")])
        self.assertEqual([e["title"] for e in message["embeds"]], ["2.5.2 · 3 items", "2.5.3 · 1 item"])
        self.assertEqual(message["embeds"][0]["url"], PROJECT)
        self.assertIn("**Picked** 1 · **Ready to pick** 1 · **In progress** 1 · **Proposed** 0",
                      message["embeds"][0]["description"])
        self.assertEqual(message["embeds"][0]["timestamp"], NOW)

    def test_ready_to_pick_shows_titles_and_others_numbers_only(self):
        text = build([item(1, "Picked"), item(2, "Ready to pick", title="Fix the *thing*"),
                      item(3, "In progress", kind="Issue")])["embeds"][0]["description"]
        self.assertIn("[#2](https://github.com/o/r/pull/2) Fix the \\*thing\\*", text)
        self.assertIn("[#3](https://github.com/o/r/issues/3)", text)
        self.assertNotIn("Title 3", text)
        self.assertNotIn("#1]", text)

    def test_never_pings(self):
        self.assertEqual(build([item(1, "Proposed")])["allowed_mentions"], {"parse": []})

    def test_items_without_milestone_come_last(self):
        message = build([item(1, "Proposed", milestone=None), item(2, "Proposed")])
        self.assertEqual([e["title"] for e in message["embeds"]], ["2.5.2 · 1 item", "No milestone · 1 item"])

    def test_attention_and_unmatched_go_to_their_release(self):
        message = build([item(1, "In progress", state="CLOSED"), item(2, "Proposed", milestone="2.5.3")],
                        attention=[(1, "closed without a merge")],
                        unmatched={"2.5.2": [("rc/v2.5.2", "abc123456 Translations refresh")]})
        first, second = (e["description"] for e in message["embeds"])
        self.assertIn("**Needs attention**\n[#1](https://github.com/o/r/pull/1) closed without a merge", first)
        self.assertIn("**Unmatched commits on rc/v2.5.2**\nabc123456 Translations refresh", first)
        self.assertNotIn("Needs attention", second)

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
        self.assertLessEqual(sum(len(e["title"]) + len(e["description"]) for e in message["embeds"]), 6000)
        self.assertIn("and ", description)
        self.assertIn(f"[project]({PROJECT})", description)


class FakeTransport:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def __call__(self, method, url, body, headers):
        self.requests.append((method, url, json.loads(body) if body else None, headers))
        return self.responses.pop(0)


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

    def test_error_hides_the_webhook_token(self):
        with self.assertRaises(DiscordError) as raised:
            Discord(HOOK, FakeTransport([(404, {"message": "Unknown Message"})])).edit_post("999", {})
        self.assertNotIn("secret-token", str(raised.exception))
        self.assertIn("404", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
