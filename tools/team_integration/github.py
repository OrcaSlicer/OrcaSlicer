"""GitHub read-only adapter. No subprocess, checkout, dispatch, or merge API."""
import io
import json
import re
import urllib.error
import urllib.parse
import urllib.request
import zipfile


def sha(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("GitHub returned an invalid full SHA")
    return value


def number(value):
    if type(value) is not int or not 1 <= value <= 9223372036854775807:
        raise ValueError("invalid PR number")
    return value


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GitHubHTTPError(RuntimeError):
    def __init__(self, status):
        self.status = status
        super().__init__("GitHub HTTP " + str(status))


class HTTPTransport:
    def __init__(self, token):
        self.token = token
        self.opener = urllib.request.build_opener(NoRedirect)

    def get(self, path, *, archive=False):
        if not path.startswith("/repos/") or ".." in path or "\\" in path:
            raise ValueError("invalid GitHub API path")
        request = urllib.request.Request("https://api.github.com" + path, headers={
            "Authorization": "Bearer " + self.token,
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "orca-team-integration-phase1",
        }, method="GET")
        try:
            response = self.opener.open(request, timeout=30)
        except urllib.error.HTTPError as exc:
            if archive and exc.code == 302:
                url = exc.headers.get("Location", "")
                parsed = urllib.parse.urlsplit(url)
                hostname = parsed.hostname or ""
                if (parsed.scheme != "https" or parsed.username or parsed.password or parsed.port not in (None, 443) or
                        not (hostname.endswith(".blob.core.windows.net") or hostname.endswith(".githubusercontent.com"))):
                    raise RuntimeError("unexpected GitHub artifact host") from None
                # Never forward the repository token to signed object-storage URLs.
                response = self.opener.open(urllib.request.Request(url, method="GET"), timeout=30)
            else:
                raise GitHubHTTPError(exc.code) from None
        with response:
            payload = response.read(2_000_001)
        if len(payload) > 2_000_000:
            raise ValueError("GitHub response exceeds size limit")
        return payload if archive else json.loads(payload)


class GitHub:
    def __init__(self, config, transport):
        self.config = config
        self.transport = transport
        self.root = "/repos/" + config["repository"]

    def get(self, suffix, **kwargs):
        return self.transport.get(self.root + suffix, **kwargs)

    def pages(self, suffix, field=None):
        values = []
        for page in range(1, 101):
            result = self.get(suffix + ("&" if "?" in suffix else "?") + f"per_page=100&page={page}")
            rows = result[field] if field else result
            if not isinstance(rows, list):
                raise ValueError("unexpected GitHub list response")
            values.extend(rows)
            if len(rows) < 100:
                return values
        raise ValueError("GitHub pagination limit reached; manual inspection required")

    def pull(self, pr):
        result = self.get(f"/pulls/{number(pr)}")
        if result.get("number") != pr:
            raise ValueError("GitHub PR number mismatch")
        sha(result["head"]["sha"])
        sha(result["base"]["sha"])
        return result

    def open_pulls(self):
        return self.pages("/pulls?state=open&base=" + urllib.parse.quote(self.config["target_branch"], safe=""))

    def base_sha(self):
        ref = urllib.parse.quote(self.config["target_branch"], safe="")
        return sha(self.get("/git/ref/heads/" + ref)["object"]["sha"])

    def reviews(self, pr):
        return self.pages(f"/pulls/{number(pr)}/reviews")

    def checks(self, head):
        return self.pages(f"/commits/{sha(head)}/check-runs?filter=latest", "check_runs")

    def protection_ready(self):
        ref = urllib.parse.quote(self.config["target_branch"], safe="")
        protection = self.get("/branches/" + ref + "/protection")
        reviews = protection.get("required_pull_request_reviews") or {}
        status = protection.get("required_status_checks") or {}
        checks = status.get("checks") or []
        bypass = reviews.get("bypass_pull_request_allowances", {})
        return (protection.get("enforce_admins", {}).get("enabled") is True and
                protection.get("allow_force_pushes", {}).get("enabled") is False and
                protection.get("allow_deletions", {}).get("enabled") is False and
                protection.get("required_conversation_resolution", {}).get("enabled") is True and
                reviews.get("dismiss_stale_reviews") is True and reviews.get("require_code_owner_reviews") is True and
                reviews.get("require_last_push_approval") is True and
                isinstance(bypass, dict) and not any(bypass.values()) and
                reviews.get("required_approving_review_count", 0) >= self.config["required_approvals"] and
                status.get("strict") is True and
                all(any(check.get("context") == required["name"] and check.get("app_id") == required["app_id"]
                        for check in checks) for required in self.config["required_checks"]))

    def candidate_evidence(self, pr, base, head, candidate):
        commit = self.get("/git/commits/" + sha(candidate))
        if sha(commit["sha"]) != candidate or [sha(p["sha"]) for p in commit["parents"]] != [base, head]:
            return None
        workflow = urllib.parse.quote(self.config["candidate_workflow"].split("/")[-1], safe="")
        runs = self.pages(f"/actions/workflows/{workflow}/runs?event=pull_request&head_sha={sha(head)}", "workflow_runs")
        runs = [r for r in runs if r.get("path", "").split("@")[0] == self.config["candidate_workflow"] and
                r.get("head_sha") == head and r.get("event") == "pull_request"]
        # A newer failed/in-progress rerun invalidates an older successful run.
        if not runs:
            return None
        run = max(runs, key=lambda r: (r["id"], r.get("run_attempt", 1)))
        if run.get("status") != "completed" or run.get("conclusion") != "success":
            return None
        artifacts = self.pages(f"/actions/runs/{number(run['id'])}/artifacts", "artifacts")
        artifacts = [a for a in artifacts if a.get("name") == self.config["candidate_artifact"] and not a.get("expired")]
        if len(artifacts) != 1:
            return None
        archive = self.get(f"/actions/artifacts/{number(artifacts[0]['id'])}/zip", archive=True)
        with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
            entries = zipped.infolist()
            if len(entries) != 1 or entries[0].filename != "candidate.json" or entries[0].file_size > 65536:
                raise ValueError("invalid candidate artifact")
            evidence = json.loads(zipped.read(entries[0]))
        expected = {"schema_version": 1, "repository": self.config["repository"], "pr_number": pr,
                    "head_sha": head, "base_sha": base, "candidate_sha": candidate, "status": "success",
                    "run_id": run["id"], "run_attempt": run.get("run_attempt", 1)}
        if any(evidence.get(k) != v for k, v in expected.items()):
            return None
        return {"check_suite_id": run["check_suite_id"], "run_id": run["id"],
                "url": f"https://github.com/{self.config['repository']}/actions/runs/{run['id']}"}
