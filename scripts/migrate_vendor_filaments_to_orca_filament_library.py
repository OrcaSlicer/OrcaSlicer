#!/usr/bin/env python3
"""
Migrate legacy vendor filament bases to OrcaFilamentLibrary inheritance.

Problem (issue #12162):
Many vendor profile bundles ship their own copies of base filament presets
(fdm_filament_common + fdm_filament_*). Those presets don't inherit from the
global OrcaFilamentLibrary, so downstream filament presets anchored to those
bases are not globally available across printers.

Strategy:
- Remove vendor bundle "filament_list" entries that point to the legacy base
  files (filament/fdm_filament_common.json and filament/fdm_filament_*.json).
- Delete the corresponding legacy base files under resources/profiles/<Vendor>/filament/.

At runtime, preset inheritance resolution already falls back to
OrcaFilamentLibrary when a referenced base preset isn't present in the vendor.
So vendor-specific filament presets that inherit from fdm_filament_pla, etc.
will now resolve to OrcaFilamentLibrary's base presets.

This script is intended to be run from the repo root.
It is idempotent.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
PROFILES_DIR = REPO_ROOT / "resources" / "profiles"


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _dump_json(path: Path, data: Any) -> None:
    # Keep key order stable (Python preserves insertion order) and use 4-space
    # indent to match existing profile bundle formatting.
    path.write_text(json.dumps(data, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")


def _is_vendor_bundle(path: Path) -> bool:
    if path.suffix.lower() != ".json":
        return False
    # Skip the global library bundle; it is the target of the migration.
    if path.name == "OrcaFilamentLibrary.json":
        return False
    # Skip any file that is clearly not a top-level bundle descriptor.
    # Bundle descriptors have a top-level "name" and at least one of the lists.
    try:
        data = _load_json(path)
    except Exception:
        return False
    return isinstance(data, dict) and isinstance(data.get("name"), str) and any(
        k in data for k in ("machine_model_list", "process_list", "filament_list", "printer_list")
    )


def _should_drop_filament_entry(entry: dict[str, Any]) -> bool:
    sub_path = str(entry.get("sub_path", ""))
    if sub_path == "filament/fdm_filament_common.json":
        return True
    # Drop all legacy base material presets in vendor bundles.
    return sub_path.startswith("filament/fdm_filament_") and sub_path.endswith(".json")


def migrate() -> tuple[int, int]:
    updated_bundles = 0
    removed_files = 0

    for bundle_path in sorted(PROFILES_DIR.glob("*.json")):
        if not _is_vendor_bundle(bundle_path):
            continue

        data = _load_json(bundle_path)
        filament_list = data.get("filament_list")
        if not isinstance(filament_list, list) or not filament_list:
            continue

        new_filament_list = []
        dropped = False
        for entry in filament_list:
            if isinstance(entry, dict) and _should_drop_filament_entry(entry):
                dropped = True
                continue
            new_filament_list.append(entry)

        if dropped:
            data["filament_list"] = new_filament_list
            _dump_json(bundle_path, data)
            updated_bundles += 1

        vendor = data.get("name")
        if not isinstance(vendor, str) or not vendor:
            continue

        vendor_filament_dir = PROFILES_DIR / vendor / "filament"
        if not vendor_filament_dir.is_dir():
            continue

        # Remove legacy base files. If they're already gone, keep going.
        for p in vendor_filament_dir.glob("fdm_filament_*.json"):
            # Matches fdm_filament_common.json and fdm_filament_<material>.json
            try:
                p.unlink()
                removed_files += 1
            except FileNotFoundError:
                pass

    return updated_bundles, removed_files


def main() -> None:
    if PROFILES_DIR.name != "profiles" or not PROFILES_DIR.is_dir():
        raise SystemExit(f"profiles dir not found: {PROFILES_DIR}")

    bundles, files = migrate()
    print(f"Updated bundles: {bundles}")
    print(f"Removed legacy base filament files: {files}")


if __name__ == "__main__":
    # Ensure we run with repo root as CWD for consistent relative paths when invoked.
    os.chdir(REPO_ROOT)
    main()

