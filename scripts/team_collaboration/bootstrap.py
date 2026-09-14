#!/usr/bin/env python3
"""Offline ADR-007 repository preparation. Never pushes or changes GitHub settings."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote, urlsplit


SCHEMA = "orcaslicer.team-collaboration/v1"
ROLES = ("model_generation", "smart_slicing", "maintenance")
BRANCHES = {
    "model_generation": "codex/team/model-generation",
    "smart_slicing": "codex/team/smart-slicing",
    "maintenance": "codex/team/maintenance",
    "integration": "codex/team/integration",
}
CHECKS = ["AI integration checks", "Team integration candidate"]
LOGIN = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")


class PreparationError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PreparationError(message)


def object_keys(value, keys, field):
    require(isinstance(value, dict) and set(value) == set(keys),
            f"{field} must contain exactly: {', '.join(keys)}")


def valid_login(value):
    return isinstance(value, str) and LOGIN.fullmatch(value) is not None and "--" not in value


def valid_sha(value):
    return isinstance(value, str) and SHA.fullmatch(value) is not None


def validate_config(config):
    object_keys(config, ("schema", "repository", "integration_branch", "developers",
                        "branches", "required_checks", "automation", "bootstrap", "feishu"), "config")
    require(config["schema"] == SCHEMA, "Unsupported configuration schema")
    repository = config["repository"]
    if repository is not None:
        require(isinstance(repository, str), "repository must be null or owner/repository")
        parts = repository.split("/")
        require(len(parts) == 2 and valid_login(parts[0]) and
                re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", parts[1]) is not None and
                parts[1] not in (".", "..") and not parts[1].endswith(".git"),
                "repository must be a GitHub owner/repository name, without URL or .git suffix")
    object_keys(config["developers"], ROLES, "developers")
    handles = [handle for handle in config["developers"].values() if handle is not None]
    require(all(valid_login(handle) for handle in handles), "developers must use GitHub logins without @")
    require(len({handle.lower() for handle in handles}) == len(handles), "The three developer accounts must be distinct")
    require(config["branches"] == BRANCHES, "branches must match the four ADR-007 long-lived branches")
    require(config["integration_branch"] == BRANCHES["integration"], "integration_branch must be codex/team/integration")
    require(config["required_checks"] == CHECKS, "required_checks must match the two always-triggered team CI job names")
    require(config["automation"] == {"mode": "notify_and_preview", "auto_merge": False}
            and config["automation"]["auto_merge"] is False,
            "Phase one requires notify_and_preview and auto_merge=false")
    object_keys(config["bootstrap"], ("baseline_sha",), "bootstrap")
    baseline = config["bootstrap"]["baseline_sha"]
    require(baseline is None or valid_sha(baseline), "baseline_sha must be null or a full lowercase 40-character Git SHA")
    object_keys(config["feishu"], ("chat_id", "users"), "feishu")
    chat = config["feishu"]["chat_id"]
    require(chat is None or isinstance(chat, str) and re.fullmatch(r"oc_[A-Za-z0-9_-]{1,128}", chat),
            "feishu.chat_id must be null or a stable oc_ chat ID")
    users = config["feishu"]["users"]
    require(isinstance(users, dict), "feishu.users must map stable open IDs to configured GitHub logins")
    known_handles = {handle.lower() for handle in handles}
    for user_id, handle in users.items():
        require(isinstance(user_id, str) and re.fullmatch(r"ou_[A-Za-z0-9_-]{1,128}", user_id),
                "feishu.users keys must be stable ou_ open IDs")
        require(valid_login(handle) and handle.lower() in known_handles,
                "feishu.users values must be configured developer GitHub logins")
    return config


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def git(repo, *args, input_text=None, allow_failure=False):
    # Binary stdin keeps Git's update-ref protocol LF-delimited on Windows as well.
    result = subprocess.run(["git", "--no-optional-locks", "-C", str(repo), *args],
                            input=input_text.encode("utf-8") if input_text is not None else None,
                            capture_output=True)
    result.stdout = result.stdout.decode("utf-8", errors="replace")
    result.stderr = result.stderr.decode("utf-8", errors="replace")
    if result.returncode and not allow_failure:
        # Do not echo command output: remote URLs can contain credentials.
        raise PreparationError(f"Git operation failed: {args[0]}")
    return result


def repository_root(repo):
    requested = Path(repo).resolve()
    top = Path(git(requested, "rev-parse", "--show-toplevel").stdout.strip()).resolve()
    require(requested == top, "--repo must identify the repository root")
    return top


def remote_repository(url):
    """Parse only common credential-free GitHub origin forms; never echo URLs."""
    url = url.strip()
    if url.startswith("git@github.com:"):
        path = url[len("git@github.com:"):]
    else:
        try:
            parsed = urlsplit(url)
            port = parsed.port
        except ValueError:
            return None
        if parsed.hostname != "github.com" or parsed.query or parsed.fragment or parsed.password:
            return None
        if parsed.scheme == "https" and parsed.username is None and port is None:
            path = parsed.path.lstrip("/")
        elif parsed.scheme == "ssh" and parsed.username == "git" and port in (None, 22):
            path = parsed.path.lstrip("/")
        else:
            return None
    return path.removesuffix(".git").lower()


def inspect(repo, config):
    repo = repository_root(repo)
    head_result = git(repo, "rev-parse", "--verify", "HEAD", allow_failure=True)
    head = head_result.stdout.strip() if head_result.returncode == 0 else None
    dirty = bool(git(repo, "status", "--porcelain=v1", "--untracked-files=all").stdout)
    baseline = config["bootstrap"]["baseline_sha"]
    raw_refs = git(repo, "for-each-ref", "--format=%(refname) %(objectname)", "refs/heads/").stdout
    refs = dict(line.split(" ", 1) for line in raw_refs.splitlines())
    origin = git(repo, "remote", "get-url", "origin", allow_failure=True)
    origin_state = "missing"
    if origin.returncode == 0:
        origin_state = "repository_not_configured" if config["repository"] is None else (
            "matches_repository" if remote_repository(origin.stdout) == config["repository"].lower() else "mismatch")
    missing = []
    if config["repository"] is None:
        missing.append("repository")
    missing.extend(f"developers.{role}" for role in ROLES if config["developers"][role] is None)
    if baseline is None:
        missing.append("bootstrap.baseline_sha")
    branches = {}
    for role, branch in BRANCHES.items():
        sha = refs.get(f"refs/heads/{branch}")
        state = "missing" if sha is None else "baseline_not_configured" if baseline is None else (
            "at_baseline" if sha == baseline else "different_commit")
        branches[role] = {"name": branch, "sha": sha, "state": state}
    blockers = list(missing)
    if origin_state != "matches_repository":
        blockers.append(f"origin:{origin_state}")
    if dirty:
        blockers.append("working_tree_has_tracked_or_untracked_changes")
    if baseline is not None and head != baseline:
        blockers.append("HEAD_differs_from_baseline")
    blockers.extend(f"branch:{value['name']}:{value['state']}" for value in branches.values()
                    if value["state"] != "at_baseline")
    return {"schema": "orcaslicer.team-bootstrap-readiness/v1", "mode": "offline",
            "remote_protection_verified": False, "head_sha": head, "working_tree_clean": not dirty,
            "baseline_sha": baseline, "origin": origin_state, "missing_configuration": missing,
            "branches": branches, "blockers": blockers, "local_preparation_complete": not blockers}


def ownership_path(value):
    require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.\-/]+", value) is not None,
            "Ownership paths must be literal repository-relative paths")
    require(not value.startswith("/") and not value.endswith("/") and
            all(part not in ("", ".", "..") for part in value.split("/")), "Unsafe ownership path")
    return value


def codeowners(repo, config, lock):
    developers = config["developers"]
    require(all(developers[role] for role in ROLES), "Real accounts for all three developers are required to generate CODEOWNERS")
    maintenance = developers["maintenance"]
    all_owners = " ".join("@" + developers[role] for role in ROLES)
    boundaries = lock.get("boundaries", {})
    groups = [
        ("model_generation_owned_paths", f"@{developers['model_generation']} @{maintenance}"),
        ("smart_slicing_owned_paths", f"@{developers['smart_slicing']} @{maintenance}"),
        ("shared_runtime_owned_paths", all_owners),
        ("integration_owned_paths", all_owners),
    ]
    lines = ["# Generated by scripts/team_collaboration/bootstrap.py from configured real accounts.",
             "# GitHub requires ONE matching owner approval; it does not require every listed owner.",
             "# Shared changes require a non-author review arranged by the integration maintainer.",
             "* " + all_owners, ""]
    for key, owners in groups:
        paths = boundaries.get(key)
        require(isinstance(paths, list) and paths, f"Integration lock is missing {key}")
        paths = [ownership_path(path) for path in paths]
        if key == "model_generation_owned_paths":
            # This fixed wildcard is part of the verifier's existing CODEOWNERS
            # contract; lock-supplied paths remain strictly literal above.
            paths += ["src/slic3r/GUI/AI/Model/ModelFinishing.*",
                      "src/slic3r/GUI/AI/Model/ModelFinishing.cpp",
                      "src/slic3r/GUI/AI/Model/ModelFinishing.hpp",
                      "src/slic3r/GUI/AI/Model/ModelColorCleanup.hpp",
                      "src/slic3r/GUI/AI/Model/ModelObjText.hpp"]
        if key == "integration_owned_paths":
            paths += ["scripts/team_collaboration", "tools/team_integration",
                      "scripts/team_ci_candidate.py", "scripts/test_team_ci_candidate.py", "deps"]
        lines.append("# " + key)
        for path in sorted(set(paths)):
            suffix = "/" if (Path(repo) / path).is_dir() else ""
            lines.append(f"/{path}{suffix} {owners}")
        lines.append("")
    return "\n".join(lines)


def branch_protection(config):
    return {
        "required_status_checks": {"strict": True,
                                   "checks": [{"context": check} for check in config["required_checks"]]},
        "enforce_admins": True,
        "required_pull_request_reviews": {
            "dismiss_stale_reviews": True,
            "require_code_owner_reviews": True,
            "required_approving_review_count": 1,
            "require_last_push_approval": True,
        },
        "restrictions": None,
        "required_linear_history": False,
        "allow_force_pushes": False,
        "allow_deletions": False,
        "required_conversation_resolution": True,
    }


def create_plan(repo, config, lock, output_dir):
    require(config["repository"] is not None, "Set the real repository before generating a deployment plan")
    require(all(config["developers"].values()), "Set all three real developer accounts before generating a deployment plan")
    readiness = inspect(repo, config)
    owners = codeowners(repo, config, lock)
    endpoint = f"repos/{config['repository']}/branches/{quote(config['integration_branch'], safe='')}/protection"
    deployment = {
        "schema": "orcaslicer.team-bootstrap-deployment/v1", "applied": False,
        "repository": config["repository"], "baseline_sha": config["bootstrap"]["baseline_sha"],
        "origin_url": f"https://github.com/{config['repository']}.git",
        "long_lived_branches": config["branches"], "codeowners_destination": ".github/CODEOWNERS",
        "integration_protection_request": {"method": "PUT", "api_path": endpoint,
                                           "body_file": "integration-branch-protection.json"},
        "developer_branch_policy": {"allow_force_pushes": False, "allow_deletions": False},
        "developer_protection_requests": [
            {"method": "PUT", "api_path": f"repos/{config['repository']}/branches/{quote(BRANCHES[role], safe='')}/protection",
             "body_file": "developer-branch-protection.json"} for role in ROLES
        ],
        "repository_merge_settings": {"allow_merge_commit": True, "delete_branch_on_merge": False},
        "automation": config["automation"],
        "manual_setup": [
            "Confirm the team repository, three accounts, collaborator write access and administrator.",
            "Check the repository plan supports protected branches; private repositories need a suitable paid plan.",
            "Inspect and save the existing remote protection/settings before applying this replacement payload.",
            "Install the generated CODEOWNERS and team workflows on the eventual PR base branch.",
            "Review outstanding local work and upstream synchronization; verify and commit one accepted baseline.",
            "Run prepare-branches with its full SHA from a clean working tree; do not rewrite existing branch history.",
            "Configure the team origin and push the four baseline branches after reviewing repository visibility.",
            "Enable merge commits and preserve long-lived branches after PR merges.",
            "Require the named Actions checks after observing them on a harmless PR; pin their GitHub App if needed.",
            "Apply integration protection without bypass actors; prohibit force-push and deletion on all four branches.",
            "Verify live protection with the GitHub API and a harmless PR; this offline plan does not verify remote settings.",
            "Configure the isolated Windows runner and Feishu app, stable chat/user mapping and private environment secrets.",
            "Complete the ADR acceptance scenarios manually in notify_and_preview mode before proposing automatic merge.",
        ],
        "references": [
            "https://docs.github.com/en/rest/branches/branch-protection#update-branch-protection",
            "https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/about-code-owners",
        ],
    }
    developer_protection = dict(branch_protection(config), required_status_checks=None,
                                required_pull_request_reviews=None, required_conversation_resolution=False)
    payloads = {"readiness.json": readiness, "integration-branch-protection.json": branch_protection(config),
                "developer-branch-protection.json": developer_protection, "deployment-plan.json": deployment}
    output = Path(output_dir).resolve()
    require(not any(part.lower() == ".git" for part in output.parts), "Do not place generated artifacts inside .git")
    names = [*payloads, "CODEOWNERS"]
    require(not output.exists() or output.is_dir(), "--output-dir must be a directory")
    require(not any((output / name).exists() or (output / name).is_symlink() for name in names),
            "Output files already exist; choose a fresh output directory")
    output.mkdir(parents=True, exist_ok=True)
    for name, payload in payloads.items():
        (output / name).write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (output / "CODEOWNERS").write_text(owners, encoding="utf-8")
    return {"mode": "offline", "applied": False, "output_dir": str(output), "files": names,
            "blockers": readiness["blockers"]}


def prepare_branches(repo, config, baseline):
    require(valid_sha(baseline), "--baseline must be a full lowercase 40-character Git commit SHA")
    configured = config["bootstrap"]["baseline_sha"]
    require(configured in (None, baseline), "Explicit baseline differs from bootstrap.baseline_sha")
    readiness = inspect(repo, config)
    require(readiness["working_tree_clean"], "Save all tracked and untracked work before creating baseline branches")
    require(readiness["head_sha"] == baseline, "HEAD must equal the explicitly accepted baseline SHA")
    resolved = git(repo, "rev-parse", "--verify", f"{baseline}^{{commit}}").stdout.strip()
    require(resolved == baseline, "--baseline must identify a commit directly")
    for info in readiness["branches"].values():
        require(info["sha"] in (None, baseline), f"Existing branch {info['name']} differs from baseline; no branches changed")
    missing = [info["name"] for info in readiness["branches"].values() if info["sha"] is None]
    if missing:
        # Git's ref transaction creates every missing branch atomically, without checkout or force.
        commands = ["start"]
        commands += [f"create refs/heads/{branch} {baseline}" for branch in missing]
        verified_refs = {f"refs/heads/{info['name']}" for info in readiness["branches"].values() if info["sha"] is not None}
        current_ref = git(repo, "symbolic-ref", "--quiet", "HEAD", allow_failure=True).stdout.strip() or "HEAD"
        verified_refs.add(current_ref)
        commands += [f"verify {ref} {baseline}" for ref in sorted(verified_refs)]
        commands += ["prepare", "commit", ""]
        git(repo, "update-ref", "--stdin", input_text="\n".join(commands))
    return {"mode": "local_only", "baseline_sha": baseline, "created": missing,
            "checkout_changed": False, "remote_changed": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--config", help="Defaults to REPO/.github/team-collaboration.json")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("check", help="Read local configuration, origin and branch readiness; no network or writes")
    plan = subparsers.add_parser("plan", help="Generate offline deployment artifacts from real configured accounts")
    plan.add_argument("--output-dir", required=True)
    branches = subparsers.add_parser("prepare-branches", help="Create only missing local branches at an accepted clean HEAD")
    branches.add_argument("--baseline", required=True)
    args = parser.parse_args(argv)
    try:
        repo = repository_root(args.repo)
        config = validate_config(read_json(args.config or repo / ".github/team-collaboration.json"))
        if args.command == "check":
            result = inspect(repo, config)
        elif args.command == "plan":
            result = create_plan(repo, config, read_json(repo / "docs/architecture/ai-integration-lock.json"), args.output_dir)
        else:
            result = prepare_branches(repo, config, args.baseline)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except (PreparationError, OSError, ValueError) as exc:
        print(json.dumps({"error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
