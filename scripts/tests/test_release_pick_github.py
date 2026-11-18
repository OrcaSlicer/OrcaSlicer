#!/usr/bin/env python3
"""Tests for scripts/release_pick_github.py (stdlib unittest, no external deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts -v
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from release_pick_github import GitHub, GitHubError  # noqa: E402
from release_pick_rules import LinkedPr  # noqa: E402

REPO = "OrcaSlicer/OrcaSlicer"


class FakeTransport:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def __call__(self, method, url, body, headers):
        self.requests.append((method, url, json.loads(body) if body else None, headers))
        return self.responses.pop(0)


def issue(number, repo=REPO, labels=("release-pick",), milestone=("2.5.2", "OPEN"), linked=()):
    return {"__typename": "Issue", "id": f"I_{number}", "number": number, "state": "OPEN",
            "repository": {"nameWithOwner": repo}, "labels": {"nodes": [{"name": n} for n in labels]},
            "milestone": {"title": milestone[0], "state": milestone[1]} if milestone else None,
            "closedByPullRequestsReferences": {"nodes": list(linked)}}


def pull(number, state="OPEN", repo=REPO, labels=("release-pick",), milestone=("2.5.2", "OPEN")):
    return {"__typename": "PullRequest", "id": f"PR_{number}", "number": number, "state": state, "title": f"Pull {number}",
            "repository": {"nameWithOwner": repo}, "labels": {"nodes": [{"name": n} for n in labels]},
            "milestone": {"title": milestone[0], "state": milestone[1]} if milestone else None}


def project_page(nodes, next_cursor=None):
    return (200, {"data": {"organization": {"projectV2": {
        "id": "PVT_1",
        "field": {"id": "FIELD_1", "options": [{"id": "o1", "name": "Proposed"}, {"id": "o4", "name": "Picked"}]},
        "items": {"pageInfo": {"hasNextPage": next_cursor is not None, "endCursor": next_cursor},
                  "nodes": nodes}}}}})


def project_node(item_id, content, status="Proposed", archived=False):
    return {"id": item_id, "isArchived": archived, "content": content,
            "fieldValueByName": {"name": status} if status else None}


def connection_page(kind, nodes, next_cursor=None):
    return (200, {"data": {"repository": {kind: {
        "pageInfo": {"hasNextPage": next_cursor is not None, "endCursor": next_cursor}, "nodes": nodes}}}})


class ReadTest(unittest.TestCase):
    def test_project_items_follow_pagination(self):
        transport = FakeTransport([
            project_page([project_node("A", issue(1))], "c1"),
            project_page([project_node("B", pull(2))]),
        ])
        project = GitHub("t", transport).project("OrcaSlicer", 15, REPO)
        self.assertEqual([i.number for i in project.items], [1, 2])
        self.assertIsNone(transport.requests[0][2]["variables"]["after"])
        self.assertEqual(transport.requests[1][2]["variables"]["after"], "c1")
        self.assertEqual((project.id, project.status_field_id), ("PVT_1", "FIELD_1"))

    def test_status_options_are_mapped_by_name(self):
        project = GitHub("t", FakeTransport([project_page([])])).project("OrcaSlicer", 15, REPO)
        self.assertEqual(project.status_options, {"Proposed": "o1", "Picked": "o4"})

    def test_item_fields_are_read(self):
        node = project_node("A", pull(2, state="MERGED", labels=("bug-fix",), milestone=("2.5.1", "CLOSED")),
                            status="Picked", archived=True)
        item = GitHub("t", FakeTransport([project_page([node])])).project("OrcaSlicer", 15, REPO).items[0]
        self.assertEqual((item.kind, item.state, item.labelled, item.milestone, item.milestone_open),
                         ("PullRequest", "MERGED", False, "2.5.1", False))
        self.assertEqual((item.item_id, item.status, item.archived, item.content_id), ("A", "Picked", True, "PR_2"))
        self.assertEqual(item.title, "Pull 2")

    def test_drafts_and_other_repos_are_skipped(self):
        nodes = [project_node("A", {"__typename": "DraftIssue"}), project_node("B", issue(3, repo="x/y")),
                 project_node("C", None)]
        self.assertEqual(GitHub("t", FakeTransport([project_page(nodes)])).project("OrcaSlicer", 15, REPO).items, [])

    def test_linked_prs_from_other_repos_are_dropped(self):
        linked = [{"number": 5, "state": "MERGED", "merged": True, "repository": {"nameWithOwner": REPO},
                   "milestone": {"title": "2.5.2"}},
                  {"number": 6, "state": "OPEN", "merged": False, "repository": {"nameWithOwner": "x/y"},
                   "milestone": None}]
        node = project_node("A", issue(1, linked=linked))
        item = GitHub("t", FakeTransport([project_page([node])])).project("OrcaSlicer", 15, REPO).items[0]
        self.assertEqual(item.linked_prs, (LinkedPr(5, "MERGED", "2.5.2"),))

    def test_labelled_items_follow_pagination(self):
        transport = FakeTransport([
            connection_page("issues", [issue(1)], "c1"),
            connection_page("issues", [issue(2, milestone=None)]),
            connection_page("pullRequests", [pull(3)]),
        ])
        items = GitHub("t", transport).labelled_items(REPO, "release-pick")
        self.assertEqual([i.number for i in items], [1, 2, 3])
        self.assertEqual(transport.requests[1][2]["variables"]["after"], "c1")
        self.assertEqual(transport.requests[0][2]["variables"]["labels"], ["release-pick"])
        self.assertTrue(all(i.item_id is None for i in items))

    def test_open_milestones_skip_non_versions_and_follow_pagination(self):
        transport = FakeTransport([
            connection_page("milestones", [{"title": "2.6.0"}, {"title": "1.9.0 beta"}], "c1"),
            connection_page("milestones", [{"title": "2.5.2"}]),
        ])
        self.assertEqual(GitHub("t", transport).open_milestones(REPO), ["2.6.0", "2.5.2"])
        self.assertEqual(transport.requests[1][2]["variables"]["after"], "c1")
        self.assertIn("after: $after", transport.requests[0][2]["query"])

    def test_prs_for_commit_keeps_merged_only(self):
        transport = FakeTransport([(200, [{"number": 7, "merged_at": "2026-11-01T00:00:00Z"},
                                          {"number": 8, "merged_at": None}])])
        self.assertEqual(GitHub("t", transport).prs_for_commit(REPO, "abc"), [7])
        method, url, _, headers = transport.requests[0]
        self.assertEqual((method, url), ("GET", "https://api.github.com/repos/OrcaSlicer/OrcaSlicer/commits/abc/pulls"))
        self.assertEqual(headers["Authorization"], "Bearer t")


class WriteTest(unittest.TestCase):
    def test_mutation_payloads(self):
        transport = FakeTransport([
            (200, {"data": {"addProjectV2ItemById": {"item": {"id": "NEW"}}}}),
            (200, {"data": {"archiveProjectV2Item": {"item": {"id": "OLD"}}}}),
            (200, {"data": {"updateProjectV2ItemFieldValue": {"projectV2Item": {"id": "NEW"}}}}),
        ])
        gh = GitHub("t", transport)
        self.assertEqual(gh.add_item("PVT_1", "I_1"), "NEW")
        gh.archive_item("PVT_1", "OLD")
        gh.set_status("PVT_1", "NEW", "FIELD_1", "o4")
        variables = [r[2]["variables"] for r in transport.requests]
        self.assertEqual(variables[0], {"project": "PVT_1", "content": "I_1"})
        self.assertEqual(variables[1], {"project": "PVT_1", "item": "OLD"})
        self.assertEqual(variables[2], {"project": "PVT_1", "item": "NEW", "field": "FIELD_1", "option": "o4"})
        self.assertIn("addProjectV2ItemById", transport.requests[0][2]["query"])
        self.assertIn("archiveProjectV2Item", transport.requests[1][2]["query"])
        self.assertIn("singleSelectOptionId", transport.requests[2][2]["query"])


class ErrorsTest(unittest.TestCase):
    def test_graphql_error_raises(self):
        with self.assertRaises(GitHubError):
            GitHub("t", FakeTransport([(200, {"errors": [{"message": "x"}]})])).graphql("query { x }", {})

    def test_http_error_raises(self):
        with self.assertRaises(GitHubError):
            GitHub("t", FakeTransport([(502, {})])).graphql("query { x }", {})


if __name__ == "__main__":
    unittest.main()
