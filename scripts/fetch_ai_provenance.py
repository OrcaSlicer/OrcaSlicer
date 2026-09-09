#!/usr/bin/env python3
"""Restore only pinned historical Git objects for CI; never follow feature branches.

Local use: python scripts/fetch_ai_provenance.py --check-only
CI use: python scripts/fetch_ai_provenance.py
The normal command may fetch only when GITHUB_ACTIONS is exactly 'true'.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from verify_ai_integration import validate_document


def pinned_commits(document: dict) -> list[str]:
    errors = validate_document(document)
    if errors:
        raise ValueError(f"Integration lock has {len(errors)} schema/contract errors; run verify_ai_integration.py --json")
    values = [document["upstream"]["sha"]]
    values.extend(source["sha"] for source in document["feature_sources"].values())
    values.extend(receipt["integration_commit"] for receipt in document["integration_receipts"].values())
    if any(not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{40}", value) is None for value in values):
        raise ValueError("Every provenance commit must be a complete lowercase 40-character Git SHA")
    return list(dict.fromkeys(values))


def run_git(repo_root: Path, arguments: list[str]) -> subprocess.CompletedProcess:
    environment = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_NO_LAZY_FETCH="1")
    return subprocess.run(["git", "--no-optional-locks", "-C", str(repo_root), *arguments],
                          capture_output=True, timeout=60, env=environment)


def has_commit(repo_root: Path, commit: str) -> bool:
    return run_git(repo_root, ["cat-file", "-e", f"{commit}^{{commit}}"]).returncode == 0


def restore_provenance(repo_root: Path, document: dict, *, check_only: bool = False) -> dict:
    commits = pinned_commits(document)
    missing = [commit for commit in commits if not has_commit(repo_root, commit)]
    fetched = []
    if missing and not check_only:
        if os.environ.get("GITHUB_ACTIONS") != "true":
            raise ValueError("Fetching fixed provenance objects requires GITHUB_ACTIONS=true; use --check-only locally")
        remaining = []
        for commit in missing:
            try:
                result = run_git(repo_root, [
                    "fetch", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules",
                    "--refmap=", "--no-auto-maintenance", "origin", commit,
                ])
                present = has_commit(repo_root, commit)
            except subprocess.TimeoutExpired:
                remaining.append(commit)
                continue
            if result.returncode != 0 or not present:
                remaining.append(commit)
            else:
                fetched.append(commit)
        missing = remaining
    return {"schema": "orcaslicer.ai-provenance-objects/v1", "check_only": check_only,
            "status": "missing_fixed_objects" if missing else "complete",
            "commits": commits, "fetched": fetched, "missing": missing,
            "development_refs_changed": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--lock", type=Path, help="Defaults to REPO/docs/architecture/ai-integration-lock.json")
    parser.add_argument("--check-only", action="store_true", help="Inspect local object availability without fetching")
    args = parser.parse_args(argv)
    try:
        lock_path = args.lock or args.repo_root / "docs/architecture/ai-integration-lock.json"
        document = json.loads(lock_path.read_text(encoding="utf-8-sig"))
        result = restore_provenance(args.repo_root, document, check_only=args.check_only)
        print(json.dumps(result, indent=2))
        return 1 if result["missing"] else 0
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        # Never print captured Git output: credentials may be embedded in origin URLs.
        message = "Git availability check timed out" if isinstance(exc, subprocess.TimeoutExpired) else str(exc)
        print(json.dumps({"status": "error", "error": message}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
