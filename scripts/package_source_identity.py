"""Read-only source identity for internal packages; never builds or uses remotes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *args])


def git_paths(root: Path, *args: str) -> set[str]:
    return {p.decode("utf-8") for p in git(root, *args).split(b"\0") if p}


def source_path(root: Path, name: str) -> Path:
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError("Snapshot file paths must be repository-relative POSIX paths")
    parts = name.split("/")
    if PurePosixPath(name).is_absolute() or any(p in {"", ".", "..", ".git"} for p in parts):
        raise ValueError(f"Invalid snapshot path: {name}")
    path = root.joinpath(*parts)
    if not path.resolve().is_relative_to(root):
        raise ValueError(f"Snapshot path leaves the source tree: {name}")
    return path


def capture(root: Path, manifest_path: Path | None = None) -> dict:
    root = root.resolve()
    head = git(root, "rev-parse", "HEAD").decode().strip()
    branch = git(root, "branch", "--show-current").decode().strip() or None
    changed = git_paths(root, "diff", "--no-ext-diff", "--no-renames", "--name-only", "-z", "HEAD")
    untracked = git_paths(root, "ls-files", "--others", "--exclude-standard", "-z")
    added = set(untracked)
    base_paths = git_paths(root, "ls-tree", "-r", "--name-only", "-z", "HEAD")
    added |= changed - base_paths
    changed &= base_paths
    files = []
    manifest_sha = None
    if manifest_path is None:
        if changed or added:
            raise ValueError("Uncommitted source requires -SourceManifest with the complete handoff snapshot; no commit or push is required")
    else:
        raw = manifest_path.read_bytes()
        manifest_sha = hashlib.sha256(raw).hexdigest()
        manifest = json.loads(raw.decode("utf-8-sig"))
        if not isinstance(manifest, dict):
            raise ValueError("Snapshot manifest must be an object")
        if manifest.get("base_head") != head:
            raise ValueError("Snapshot base_head does not match this checkout")
        entries = manifest.get("files")
        if not isinstance(entries, list):
            raise ValueError("Snapshot files must be a list")
        seen = set()
        expected_changed, expected_added = set(), set()
        for entry in entries:
            if not isinstance(entry, dict):
                raise ValueError("Snapshot file entries must be objects")
            name, action = entry["path"], entry["action"]
            path = source_path(root, name)
            if name.casefold() in seen:
                raise ValueError(f"Duplicate snapshot path: {name}")
            seen.add(name.casefold())
            if action not in {"add", "replace", "delete"}:
                raise ValueError(f"Unsupported snapshot action: {action}")
            (expected_added if action == "add" else expected_changed).add(name)
            if action == "delete":
                if path.exists():
                    raise ValueError(f"Deleted snapshot path still exists: {name}")
                files.append({"path": name, "action": action})
                continue
            data = path.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            if not isinstance(entry.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) or digest != entry["sha256"] or type(entry.get("bytes")) is not int or len(data) != entry["bytes"]:
                raise ValueError(f"Snapshot bytes or SHA-256 mismatch: {name}")
            files.append({"path": name, "action": action, "bytes": len(data), "sha256": digest})
        # Only explicit untracked research documents may remain outside a snapshot.
        # Temporary artifacts belong in Git-ignored output directories, not source.
        exclusions = manifest.get("excluded_untracked_docs", [])
        if not isinstance(exclusions, list):
            raise ValueError("excluded_untracked_docs must be a list")
        for name in exclusions:
            source_path(root, name)
            if not name.startswith("Docs/") or not name.endswith(".md") or name not in untracked:
                raise ValueError("Only untracked Docs/*.md files may be excluded")
        if changed != expected_changed or added - set(exclusions) != expected_added:
            raise ValueError("Snapshot does not cover the checkout changes: " + json.dumps({
                "tracked_difference": sorted(changed ^ expected_changed),
                "untracked_difference": sorted((added - set(exclusions)) ^ expected_added),
            }, ensure_ascii=False))
    identity = {
        "source_commit": head,
        "source_branch": branch,
        "source_clean": not bool(changed or added),
        "source_manifest_sha256": manifest_sha,
        "tracked_diff_sha256": hashlib.sha256(git(root, "diff", "--no-ext-diff", "--no-renames", "--binary", "HEAD")).hexdigest(),
        "files": sorted(files, key=lambda f: f["path"]),
    }
    identity["source_identity_sha256"] = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return identity


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(capture(args.root, args.manifest), ensure_ascii=True))
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Internal package source check failed: {exc}\n")


if __name__ == "__main__":
    main()
