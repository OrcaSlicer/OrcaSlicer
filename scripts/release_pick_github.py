#!/usr/bin/env python3
"""The GitHub API calls release_pick_sync.py needs: reading the release project, the labelled issues
and pull requests, open milestones and a commit's pull requests, and changing project items."""

import json
import urllib.error
import urllib.request
from dataclasses import dataclass

from release_pick_rules import Item, LinkedPr, parse_version

API = "https://api.github.com"

_ISSUE_FIELDS = """
fragment IssueFields on Issue {
  id number title state repository { nameWithOwner } labels(first: 50) { nodes { name } } milestone { title state }
  closedByPullRequestsReferences(first: 10) {
    nodes { number state merged repository { nameWithOwner } milestone { title } }
  }
}"""

_PR_FIELDS = """
fragment PrFields on PullRequest {
  id number title state repository { nameWithOwner } labels(first: 50) { nodes { name } } milestone { title state }
}"""

_PROJECT_QUERY = _ISSUE_FIELDS + _PR_FIELDS + """
query($owner: String!, $number: Int!, $after: String) {
  organization(login: $owner) { projectV2(number: $number) {
    id
    field(name: "Status") { ... on ProjectV2SingleSelectField { id options { id name } } }
    items(first: 100, after: $after, archivedStates: [ARCHIVED, NOT_ARCHIVED]) {
      pageInfo { hasNextPage endCursor }
      nodes {
        id isArchived
        fieldValueByName(name: "Status") { ... on ProjectV2ItemFieldSingleSelectValue { name } }
        content { __typename ... on Issue { ...IssueFields } ... on PullRequest { ...PrFields } }
      }
    }
  } }
}"""

_LABELLED_QUERY = """
query($owner: String!, $name: String!, $labels: [String!], $after: String) {
  repository(owner: $owner, name: $name) {
    %s(labels: $labels, first: 100, after: $after) {
      pageInfo { hasNextPage endCursor }
      nodes { __typename ...%s }
    }
  }
}"""

_MILESTONES_QUERY = """
query($owner: String!, $name: String!, $after: String) {
  repository(owner: $owner, name: $name) {
    milestones(first: 100, after: $after, states: OPEN) { pageInfo { hasNextPage endCursor } nodes { title } }
  }
}"""

_ADD = """mutation($project: ID!, $content: ID!) {
  addProjectV2ItemById(input: {projectId: $project, contentId: $content}) { item { id } }
}"""

_ARCHIVE = """mutation($project: ID!, $item: ID!) {
  archiveProjectV2Item(input: {projectId: $project, itemId: $item}) { item { id } }
}"""

_SET_STATUS = """mutation($project: ID!, $item: ID!, $field: ID!, $option: String!) {
  updateProjectV2ItemFieldValue(input: {projectId: $project, itemId: $item, fieldId: $field,
                                        value: {singleSelectOptionId: $option}}) { projectV2Item { id } }
}"""


class GitHubError(Exception):
    pass


@dataclass
class Project:
    id: str
    status_field_id: str
    status_options: dict
    items: list


def _urllib_transport(method, url, body, headers):
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.status, json.loads(response.read() or b"null")
    except urllib.error.HTTPError as error:
        return error.code, {"message": error.read().decode(errors="replace")}


def _linked_prs(content, repo):
    linked = []
    for pr in (content.get("closedByPullRequestsReferences") or {}).get("nodes") or []:
        if (pr.get("repository") or {}).get("nameWithOwner") != repo:
            continue
        state = "MERGED" if pr.get("merged") else pr["state"]
        linked.append(LinkedPr(pr["number"], state, (pr.get("milestone") or {}).get("title")))
    return tuple(linked)


def _item_from_content(content, repo, label, project_node=None):
    """An Item for an Issue or PullRequest of repo, or None for anything else."""
    if not content or content.get("__typename") not in ("Issue", "PullRequest"):
        return None
    if (content.get("repository") or {}).get("nameWithOwner") != repo:
        return None
    milestone = content.get("milestone")
    status = None
    if project_node:
        status = (project_node.get("fieldValueByName") or {}).get("name")
    return Item(
        number=content["number"],
        kind=content["__typename"],
        state=content["state"],
        labelled=label in [n["name"] for n in (content.get("labels") or {}).get("nodes") or []],
        milestone=milestone["title"] if milestone else None,
        milestone_open=bool(milestone and milestone.get("state") == "OPEN"),
        content_id=content["id"],
        linked_prs=_linked_prs(content, repo) if content["__typename"] == "Issue" else (),
        item_id=project_node["id"] if project_node else None,
        status=status,
        archived=bool(project_node and project_node.get("isArchived")),
        title=content.get("title") or "",
    )


class GitHub:
    def __init__(self, token, transport=None, label="release-pick"):
        self._token = token
        self._transport = transport or _urllib_transport
        self._label = label

    def _request(self, method, url, payload=None):
        headers = {"Authorization": f"Bearer {self._token}", "Accept": "application/vnd.github+json",
                   "User-Agent": "release-pick-sync"}
        body = None
        if payload is not None:
            body = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
        status, data = self._transport(method, url, body, headers)
        if status != 200:
            raise GitHubError(f"{method} {url}: HTTP {status}: {data}")
        return data

    def graphql(self, query, variables):
        data = self._request("POST", f"{API}/graphql", {"query": query, "variables": variables})
        if data.get("errors"):
            raise GitHubError(f"GraphQL: {data['errors']}")
        return data["data"]

    def _pages(self, query, variables, path):
        after = None
        while True:
            node = self.graphql(query, {**variables, "after": after})
            for key in path:
                node = node[key]
            yield node
            connection = node if "pageInfo" in node else node["items"]
            if not connection["pageInfo"]["hasNextPage"]:
                return
            after = connection["pageInfo"]["endCursor"]

    def project(self, owner, number, repo):
        project = None
        for page in self._pages(_PROJECT_QUERY, {"owner": owner, "number": number},
                                ("organization", "projectV2")):
            if project is None:
                field = page.get("field") or {}
                if not field.get("id"):
                    raise GitHubError("The project has no Status single-select field")
                project = Project(page["id"], field["id"], {o["name"]: o["id"] for o in field["options"]}, [])
            for node in page["items"]["nodes"]:
                item = _item_from_content(node.get("content"), repo, self._label, node)
                if item:
                    project.items.append(item)
        return project

    def labelled_items(self, repo, label):
        owner, name = repo.split("/")
        items = []
        for connection, fragment, fields in (("issues", "IssueFields", _ISSUE_FIELDS),
                                             ("pullRequests", "PrFields", _PR_FIELDS)):
            query = fields + _LABELLED_QUERY % (connection, fragment)
            for page in self._pages(query, {"owner": owner, "name": name, "labels": [label]},
                                    ("repository", connection)):
                items.extend(i for i in (_item_from_content(n, repo, self._label) for n in page["nodes"]) if i)
        return items

    def open_milestones(self, repo):
        owner, name = repo.split("/")
        pages = self._pages(_MILESTONES_QUERY, {"owner": owner, "name": name}, ("repository", "milestones"))
        return [n["title"] for page in pages for n in page["nodes"] if parse_version(n["title"])]

    def prs_for_commit(self, repo, sha):
        pulls = self._request("GET", f"{API}/repos/{repo}/commits/{sha}/pulls")
        return [p["number"] for p in pulls if p.get("merged_at")]

    def add_item(self, project_id, content_id):
        data = self.graphql(_ADD, {"project": project_id, "content": content_id})
        return data["addProjectV2ItemById"]["item"]["id"]

    def archive_item(self, project_id, item_id):
        self.graphql(_ARCHIVE, {"project": project_id, "item": item_id})

    def set_status(self, project_id, item_id, field_id, option_id):
        self.graphql(_SET_STATUS, {"project": project_id, "item": item_id, "field": field_id, "option": option_id})
