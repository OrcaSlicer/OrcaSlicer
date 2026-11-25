"""Posts a notice on inactive PRs and adds the labels stale.yml needs to close them.

stale.yml sets the limits and day counts in its environment. It needs the workflow's token, because the
stale bot counts a comment from a person as activity.
"""

import datetime
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse

REPO = os.environ["GITHUB_REPOSITORY"]
LIMIT = max(0, int(os.environ["NOTICE_LIMIT"]))
RENOTICE_DAYS = int(os.environ["RENOTICE_DAYS"])
CLOSE_DAYS = int(os.environ["PR_CLOSE_DAYS"])
DRY_RUN = os.environ.get("DRY_RUN", "false") == "true"
# Dry runs post nothing, so they have no deadline
DEADLINE = None if DRY_RUN else time.monotonic() + 60 * int(os.environ["NOTICE_MINUTES"])
WAIT_LABEL = "wait for response"
STALE_LABEL = "stale"
TEAM = ("MEMBER", "OWNER", "COLLABORATOR")
MARKER = "<!-- inactive-pr-notice -->"
GH_TRIES = 3
GH_TIMEOUT = 120
# gh errors worth another try; others, such as a 404, fail at once
TRANSIENT = re.compile(r"HTTP (5\d\d|429)|rate limit|timed? ?out|connection|EOF|Something went wrong", re.IGNORECASE)

# Days without activity before each notice, and its text
NOTICES = {
    "waiting": (int(os.environ["PR_STALE_DAYS"]), """We asked for changes or more information on this PR, and nothing has happened on it for at least {days} days. It will be closed in {close} days.

To keep it open, leave a comment. If you push new commits, please also leave a comment so we know it's ready for review again. If you no longer have time for it, that's fine. You can close it yourself."""),
    "conflicts": (int(os.environ["CONFLICT_DAYS"]), """This PR has conflicts with the main branch, and nothing has happened on it for at least {days} days. It will be closed in {close} days.

If you still want to work on it, please fix the conflicts and leave a comment. If you no longer have time for it, that's fine. You can close it yourself."""),
    "unreviewed-conflicts": (int(os.environ["CONFLICT_DAYS"]), """We're sorry that nobody on the team has reviewed this PR yet. It has conflicts with the main branch, and nothing has happened on it for at least {days} days. It will be closed in {close} days.

If you still want a review, please fix the conflicts and leave a comment. If you no longer have time for it, that's fine. You can close it yourself."""),
    "unreviewed": (int(os.environ["UNREVIEWED_DAYS"]), """We're sorry that nobody on the team has reviewed this PR yet. Nothing has happened on it for at least {days} days. It will be closed in {close} days.

If you still want a review, leave a comment. If you no longer have time for it, that's fine. You can close it yourself."""),
    "draft": (int(os.environ["DRAFT_DAYS"]), """Nothing has happened on this draft PR for at least {days} days. It will be closed in {close} days.

If you're still working on it, leave a comment to keep it open. If it is closed, you can open a new PR later and link this one."""),
}

QUERY = """query($owner: String!, $name: String!, $cursor: String) {
  repository(owner: $owner, name: $name) {
    pullRequests(states: OPEN, first: 40, after: $cursor) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number isDraft mergeable baseRefName authorAssociation
        author { __typename login }
        labels(first: 100) { nodes { name } }
        assignees { totalCount }
        milestone { title }
        commits(last: 1) { nodes { commit { committedDate } } }
        comments(last: 30) { totalCount nodes { author { __typename login } authorAssociation createdAt body } }
        reviews(last: 20) { totalCount nodes { author { __typename login } authorAssociation submittedAt } }
        labelEvents: timelineItems(last: 100, itemTypes: [LABELED_EVENT]) {
          nodes { ... on LabeledEvent { createdAt label { name } actor { login } } }
        }
      }
    }
  }
}"""

# One page of a PR's comments or reviews
HISTORY = """query($owner: String!, $name: String!, $number: Int!, $cursor: String) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      %s(first: 100, after: $cursor) { pageInfo { hasNextPage endCursor } nodes { author { __typename login } authorAssociation createdAt body } }
    }
  }
}"""


def gh(*args, tries=GH_TRIES, missing_ok=False):
    target = next((a for a in args if a.startswith("repos/") or a == "graphql"), args[0])
    for attempt in range(1, tries + 1):
        try:
            result = subprocess.run(["gh", *args], capture_output=True, encoding="utf-8", timeout=GH_TIMEOUT)
        except subprocess.TimeoutExpired:
            result = subprocess.CompletedProcess(args, 1, "", f"timed out after {GH_TIMEOUT} s")
        if result.returncode == 0 or (missing_ok and "HTTP 404" in result.stderr):
            return result.stdout
        print(f"gh api {target} failed (try {attempt} of {tries}): {result.stderr.strip()}", file=sys.stderr)
        if attempt == tries or not TRANSIENT.search(result.stderr):
            break
        time.sleep(10 * attempt)
    raise RuntimeError(f"gh api {target} failed")


def paged(query, path, *variables):
    """All nodes of the connection at path, where path starts below the query's data."""
    owner, name = REPO.split("/")
    cursor, nodes = None, []
    while True:
        args = ["api", "graphql", "-f", "query=" + query, "-f", "owner=" + owner, "-f", "name=" + name, *variables]
        if cursor:
            args += ["-f", "cursor=" + cursor]
        page = json.loads(gh(*args))["data"]
        for key in path:
            page = page[key]
        nodes += page["nodes"]
        if not page["pageInfo"]["hasNextPage"]:
            return nodes
        cursor = page["pageInfo"]["endCursor"]


def days_since(stamp, now):
    return (now - parse_time(stamp)).days


def is_person(actor):
    return bool(actor) and actor["__typename"] == "User"


def parse_time(stamp):
    return datetime.datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def full_comments(pr):
    """The PR's comments, reading the whole history when the fetched window doesn't hold them all."""
    nodes = pr["comments"]["nodes"]
    if pr["comments"]["totalCount"] <= len(nodes):
        return nodes
    return paged(HISTORY % "comments", ("repository", "pullRequest", "comments"), "-F", f"number={pr['number']}")


def answered_notice(pr):
    """Whether a person replied to this PR's notice while the wait label that the notice added is still on it."""
    labels = {label["name"] for label in pr["labels"]["nodes"]}
    added = [e for e in pr["labelEvents"]["nodes"] if e and e["label"]["name"] == WAIT_LABEL]
    if WAIT_LABEL not in labels or STALE_LABEL in labels or not added or not added[-1]["actor"]:
        return False

    def notices(nodes):
        return [c for c in nodes if c["author"] and not is_person(c["author"]) and c["body"].startswith(MARKER)]

    comments = pr["comments"]["nodes"]
    # A notice can be older than the fetched window
    if not notices(comments):
        comments = full_comments(pr)
    if not notices(comments):
        return False
    notice, added = notices(comments)[-1], added[-1]
    # The notice adds the label seconds before it posts, so a later add came from someone else
    gap = (parse_time(notice["createdAt"]) - parse_time(added["createdAt"])).total_seconds()
    if added["actor"]["login"] != notice["author"]["login"] or not 0 <= gap <= 300:
        return False
    replies = [c["createdAt"] for c in comments if is_person(c["author"])]
    replies += [r["submittedAt"] for r in pr["reviews"]["nodes"] if is_person(r["author"]) and r["submittedAt"]]
    replies += [c["commit"]["committedDate"] for c in pr["commits"]["nodes"]]
    return max(replies) > notice["createdAt"]


def team_replied(pr):
    author = pr["author"]["login"]

    def replied(nodes):
        return any(is_person(x["author"]) and x["author"]["login"] != author and x["authorAssociation"] in TEAM
                   for x in nodes)

    for key in ("comments", "reviews"):
        fetched = pr[key]
        nodes = fetched["nodes"]
        # An older reply can be outside the fetched window
        if not replied(nodes) and fetched["totalCount"] > len(nodes):
            nodes = paged(HISTORY % key, ("repository", "pullRequest", key), "-F", f"number={pr['number']}")
        if replied(nodes):
            return True
    return False


def pick_notice(pr, now):
    """The case and idle days of the notice this PR should get now, or None."""
    labels = {label["name"] for label in pr["labels"]["nodes"]}
    if (STALE_LABEL in labels or not is_person(pr["author"]) or pr["baseRefName"] != "main"
            or pr["assignees"]["totalCount"] or pr["milestone"]):
        return None
    comments, reviews = pr["comments"]["nodes"], pr["reviews"]["nodes"]

    def noticed(nodes):
        return any(c["body"].startswith(MARKER) and days_since(c["createdAt"], now) < RENOTICE_DAYS for c in nodes)

    if noticed(comments):
        return None

    activity = [c["createdAt"] for c in comments if is_person(c["author"])]
    activity += [r["submittedAt"] for r in reviews if is_person(r["author"]) and r["submittedAt"]]
    # A rebase or amend sets a new commit date
    activity += [c["commit"]["committedDate"] for c in pr["commits"]["nodes"]]
    idle = days_since(max(activity), now)

    if WAIT_LABEL in labels:
        case = "waiting" if not pr["isDraft"] else None
    elif pr["authorAssociation"] in TEAM:
        case = None
    elif pr["isDraft"]:
        case = "draft"
    # GitHub computes this on request, so the next run sees the real value
    elif pr["mergeable"] == "UNKNOWN":
        case = None
    elif pr["mergeable"] == "CONFLICTING":
        case = "conflicts"
    else:
        case = "unreviewed"
    if not case or idle < NOTICES[case][0]:
        return None
    # Checked last because they can read a PR's whole history
    if noticed(full_comments(pr)):
        return None
    if case in ("conflicts", "unreviewed"):
        replied = team_replied(pr)
        if case == "unreviewed" and replied:
            return None
        if case == "conflicts" and not replied:
            case = "unreviewed-conflicts"
    return case, idle


def find_notices(number, since):
    """The ids of the notices posted on this PR since the given time."""
    return gh("api", f"repos/{REPO}/issues/{number}/comments?since={since}", "--jq",
              f'.[] | select(.user.type == "Bot" and (.body | startswith("{MARKER}"))) | .id').split()


def post_comment(number, body, since):
    """The id of the new comment, posted once even when GitHub fails after creating it."""
    for attempt in range(1, GH_TRIES + 1):
        try:
            return gh("api", f"repos/{REPO}/issues/{number}/comments", "-f", f"body={body}", "--jq", ".id", tries=1).strip()
        except RuntimeError:
            # Posting again is only safe once GitHub confirms the last try created nothing
            if posted := find_notices(number, since):
                return posted[0]
            if attempt < GH_TRIES:
                time.sleep(10 * attempt)
    raise RuntimeError(f"posting the notice on #{number} failed")


def post_notice(number, case, labels):
    # The stale bot counts any change after the stale label as activity, so the stale label goes on last
    added_wait = WAIT_LABEL not in labels
    # A minute early, so a GitHub clock behind the runner's still finds a notice this call posted
    since = (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(minutes=1)).strftime("%Y-%m-%dT%H:%M:%SZ")
    # Adding a label the PR already has changes nothing, and the fetched labels can be minutes old
    gh("api", f"repos/{REPO}/issues/{number}/labels", "-f", f"labels[]={WAIT_LABEL}", "--silent")
    comment = None
    try:
        days, text = NOTICES[case]
        comment = post_comment(number, MARKER + "\n" + text.format(days=days, close=CLOSE_DAYS), since)
        gh("api", f"repos/{REPO}/issues/{number}/labels", "-f", f"labels[]={STALE_LABEL}", "--silent")
    except RuntimeError:
        # Without the stale label nothing closes the PR, so the notice is undone and the next run tries again
        try:
            comments = set(find_notices(number, since))
        except RuntimeError:
            comments = set()
        comments |= {comment} if comment else set()
        undo = [f"repos/{REPO}/issues/comments/{c}" for c in comments]
        if added_wait:
            undo.append(wait_label_path(number))
        # Each step is tried even when one before it fails; gh logs the failure
        for path in undo:
            try:
                gh("api", "-X", "DELETE", path, "--silent", missing_ok=True)
            except RuntimeError:
                pass
        raise


def wait_label_path(number):
    return f"repos/{REPO}/issues/{number}/labels/{urllib.parse.quote(WAIT_LABEL)}"


def main():
    now = datetime.datetime.now(datetime.timezone.utc)
    prs = paged(QUERY, ("repository", "pullRequests"))
    failed = []

    for pr in prs:
        labels = {label["name"] for label in pr["labels"]["nodes"]}
        # A draft is only on the stale track while a notice is open, so the stale step never starts its own clock on one
        draft = pr["isDraft"] and WAIT_LABEL in labels and STALE_LABEL not in labels
        try:
            if not (draft or answered_notice(pr)):
                continue
            print(f"#{pr['number']} {'draft' if draft else 'answered'}: removing {WAIT_LABEL}")
            if not DRY_RUN:
                # Another bot can remove the label between the fetch and this call
                gh("api", "-X", "DELETE", wait_label_path(pr["number"]), "--silent", missing_ok=True)
        except RuntimeError:
            failed.append(pr["number"])

    todo = []
    for pr in prs:
        try:
            if notice := pick_notice(pr, now):
                todo.append((notice, pr))
        except RuntimeError:
            failed.append(pr["number"])
    # Longest idle first
    todo.sort(key=lambda item: item[0][1], reverse=True)
    posted = 0
    for position, ((case, idle), pr) in enumerate(todo):
        if position == LIMIT:
            if not DRY_RUN:
                break
            print(f"-- over the limit of {LIMIT}, not posted tonight --")
        # A notice cut off by the step timeout would stay up with nothing to close the PR
        if DEADLINE and time.monotonic() > DEADLINE:
            print(f"Out of time after {posted} notices; the rest wait for the next run")
            break
        print(f"#{pr['number']} {case}: idle {idle} days")
        if DRY_RUN:
            if position < LIMIT:
                posted += 1
            continue
        try:
            post_notice(pr["number"], case, {label["name"] for label in pr["labels"]["nodes"]})
            posted += 1
        except RuntimeError:
            failed.append(pr["number"])
        # Stays under GitHub's limit on comments per minute
        time.sleep(2)
    failed = sorted(set(failed))
    print(f"{'would-post' if DRY_RUN else 'posted'}={posted} eligible={len(todo)} limit={LIMIT} failed={len(failed)} dry-run={str(DRY_RUN).lower()}")
    if failed:
        sys.exit("Failed on " + " ".join(f"#{n}" for n in failed))


if __name__ == "__main__":
    main()
