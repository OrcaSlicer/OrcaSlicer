"""Save a new local coloring iteration checkpoint, including uncommitted source.

Uses the existing package source-identity verifier. Does not commit, restore,
build, or contact a remote. Large evidence and runtimes remain separately linked.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

from package_source_identity import capture, git, git_paths, source_path


def checkpoint(root: Path, iteration: str, name: str, summary: str) -> Path:
    root = root.resolve()
    if not re.fullmatch(r"COL-\d{3}", iteration):
        raise ValueError("Iteration must have the form COL-003")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", name):
        raise ValueError("Name must contain lowercase letters, digits or hyphens")
    if not summary.strip():
        raise ValueError("A description of this checkpoint is required")
    stamp = datetime.now().astimezone()
    destination = root / ".tmp/model-coloring-checkpoints" / (
        f"{iteration}-{stamp:%Y%m%d-%H%M%S-%f}-{name}")
    # Do not accidentally make the checkpoint itself part of the source diff.
    ignored = subprocess.run(["git", "-C", str(root), "check-ignore", "-q",
                              str(destination / "manifest.json")], check=False)
    if ignored.returncode != 0:
        raise ValueError("The checkpoint destination must be Git-ignored")
    head = git(root, "rev-parse", "HEAD").decode().strip()
    changed = git_paths(root, "diff", "--no-ext-diff", "--no-renames",
                        "--name-only", "-z", "HEAD")
    untracked = git_paths(root, "ls-files", "--others", "--exclude-standard", "-z")
    base = git_paths(root, "ls-tree", "-r", "--name-only", "-z", "HEAD")
    patch = git(root, "diff", "--no-ext-diff", "--no-renames", "--binary", "HEAD")
    destination.mkdir(parents=True, exist_ok=False)
    entries = []
    # A ZIP preserves repository-relative names without extending the native
    # Windows path of deeply nested vendored headers beyond MAX_PATH.
    with zipfile.ZipFile(destination / "files.zip", "x", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(changed | untracked):
            source = source_path(root, name)
            if not source.exists():
                entries.append({"path": name, "action": "delete"})
                continue
            data = source.read_bytes()
            archive.writestr(name, data)
            entries.append({"path": name, "action": "replace" if name in base else "add",
                            "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    manifest = {"base_head": head, "iteration": iteration, "created_at": stamp.isoformat(),
                "summary": summary.strip(), "files": entries}
    manifest_path = destination / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    (destination / "tracked.patch").write_bytes(patch)
    identity = capture(root, manifest_path)
    if identity["tracked_diff_sha256"] != hashlib.sha256(patch).hexdigest():
        raise ValueError("Source changed while saving the checkpoint; it is not complete")
    with zipfile.ZipFile(destination / "files.zip") as archive:
        for entry in entries:
            if entry["action"] != "delete" and hashlib.sha256(archive.read(entry["path"])).hexdigest() != entry["sha256"]:
                raise ValueError(f"Archived bytes differ: {entry['path']}")
    (destination / "source-identity.json").write_text(
        json.dumps(identity, ensure_ascii=False, indent=2), encoding="utf-8")
    # Written last: absence of this file means capture did not finish.
    (destination / "COMPLETE.json").write_text(json.dumps({
        "source_identity_sha256": identity["source_identity_sha256"],
        "manifest_sha256": identity["source_manifest_sha256"],
        "files_zip_sha256": hashlib.sha256((destination / "files.zip").read_bytes()).hexdigest(),
        "changed_file_count": len(entries),
        "scope": "Base Git commit plus all tracked changes and non-ignored untracked files; excludes ignored evidence and runtime assets",
    }, indent=2), encoding="utf-8")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--iteration", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--summary", required=True)
    args = parser.parse_args()
    try:
        print(checkpoint(args.root, args.iteration, args.name, args.summary))
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Coloring checkpoint was not completed: {exc}\n")


if __name__ == "__main__":
    main()
