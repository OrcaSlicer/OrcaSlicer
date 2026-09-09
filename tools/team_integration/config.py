"""Configuration is data; identities and permissions never come from chat/PR text."""
import json
import os
from pathlib import Path
import re


def load_config(path, *, require_secrets=False):
    with open(path, encoding="utf-8") as stream:
        config = json.load(stream)
    validate_config(config)
    if require_secrets:
        for name in ("github_token_env", "feishu_app_id_env", "feishu_app_secret_env"):
            if not os.environ.get(config[name], "").strip():
                raise ValueError("missing environment variable: " + config[name])
    return config


def validate_config(c):
    required = {
        "schema_version", "mode", "repository", "target_branch", "source_branches",
        "task_branch_prefixes", "users", "maintainer_ids", "chat_id", "bot_open_id",
        "required_checks", "candidate_workflow", "candidate_artifact", "required_approvals",
        "github_token_env", "feishu_app_id_env", "feishu_app_secret_env", "poll_seconds",
        "database_path",
    }
    if set(c) != required:
        raise ValueError("configuration keys differ: " + ", ".join(sorted(set(c) ^ required)))
    if "REPLACE" in json.dumps(c) or c["schema_version"] != 1 or c["mode"] != "phase1":
        raise ValueError("fill all placeholders; only schema 1 / phase1 is supported")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", c["repository"]):
        raise ValueError("repository must be owner/repo")
    if c["target_branch"] != "codex/team/integration":
        raise ValueError("target_branch must be codex/team/integration")
    if set(c["source_branches"]) != {"codex/team/model-generation", "codex/team/smart-slicing", "codex/team/maintenance"}:
        raise ValueError("configure the three ADR-007 source branches")
    for prefix in c["task_branch_prefixes"]:
        if not re.fullmatch(r"codex/[a-z0-9/-]+[/\-]", prefix) or prefix == "codex/":
            raise ValueError("task branch prefixes must be narrowly scoped under codex/")
    users = c["users"]
    if (not isinstance(users, dict) or len(users) != 3 or
            len({v.lower() for v in users.values()}) != 3 or
            any(not re.fullmatch(r"ou_[A-Za-z0-9_-]+", k) or
                not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}", v) for k, v in users.items())):
        raise ValueError("map three distinct stable Feishu open IDs to distinct GitHub users")
    if not c["maintainer_ids"] or not set(c["maintainer_ids"]) <= users.keys():
        raise ValueError("maintainer_ids must refer to configured users")
    if not re.fullmatch(r"oc_[A-Za-z0-9_-]+", c["chat_id"]) or not re.fullmatch(r"ou_[A-Za-z0-9_-]+", c["bot_open_id"]):
        raise ValueError("invalid chat_id or bot_open_id")
    if not c["required_checks"]:
        raise ValueError("at least one validated build check is required")
    names = set()
    for check in c["required_checks"]:
        if (set(check) != {"name", "app_id"} or not isinstance(check["name"], str) or
                not check["name"].strip() or type(check["app_id"]) is not int or check["app_id"] <= 0 or
                check["name"] in names):
            raise ValueError("required checks need distinct names and trusted positive app_id values")
        names.add(check["name"])
    if not {"AI integration checks", "Team integration candidate"} <= names:
        raise ValueError("both ADR-007 integration and candidate checks are mandatory")
    if not re.fullmatch(r"\.github/workflows/[A-Za-z0-9_-]+\.ya?ml", c["candidate_workflow"]):
        raise ValueError("candidate_workflow must identify a repository workflow")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", c["candidate_artifact"]):
        raise ValueError("invalid candidate_artifact")
    if type(c["required_approvals"]) is not int or not 1 <= c["required_approvals"] <= 2:
        raise ValueError("one or two current non-author team approvals required")
    if type(c["poll_seconds"]) is not int or c["poll_seconds"] < 15:
        raise ValueError("poll_seconds must be at least 15")
    for field in ("github_token_env", "feishu_app_id_env", "feishu_app_secret_env"):
        if not re.fullmatch(r"[A-Z][A-Z0-9_]+", c[field]):
            raise ValueError("secret fields must name environment variables")
    if not Path(c["database_path"]).is_absolute():
        raise ValueError("database_path must be an absolute private path")
