#!/usr/bin/env python3
"""Apply registered host-build fixes, never inference or model patches."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

LLVM_OVERLAY_ORIGINAL = "285fbd72d4743ecefa4694ea0d21927461a6cd8c6ae951193f67c4064e74b284"
LLVM_CONFIGURE_ORIGINAL = "eaf0f4175c69f180e5df3038688472532b6485f9c292ca1909e96db8339d2421"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-base", required=True, type=Path)
    parser.add_argument("--receipts", required=True, type=Path)
    args = parser.parse_args()
    relative = "utils/bazel/overlay_directories.py"
    target = args.output_base / "external/llvm-raw" / relative
    if not target.is_file():
        print("LLVM dependency has not been fetched; no host patch applied.")
        return
    current = target.read_bytes()
    backup = args.receipts / "llvm-overlay-pristine.py"
    if hashlib.sha256(current).hexdigest() == LLVM_OVERLAY_ORIGINAL:
        original = current
    elif backup.is_file() and hashlib.sha256(backup.read_bytes()).hexdigest() == LLVM_OVERLAY_ORIGINAL:
        original = backup.read_bytes()
    else:
        raise SystemExit("Unregistered LLVM overlay source; refusing to overwrite it.")
    before = original.decode("utf-8")
    after = before.replace("import os\n", "import os\nimport shutil\n", 1).replace(
        "    os.symlink(os.path.abspath(from_path), os.path.abspath(to_path))",
        "    # Orca Windows source builds do not require symlink privileges.\n"
        "    if os.name == 'nt':\n"
        "        if os.path.isdir(from_path):\n"
        "            shutil.copytree(os.path.abspath(from_path), os.path.abspath(to_path))\n"
        "        else:\n"
        "            shutil.copy2(os.path.abspath(from_path), os.path.abspath(to_path))\n"
        "    else:\n"
        "        os.symlink(os.path.abspath(from_path), os.path.abspath(to_path))", 1)
    patched = after.encode("utf-8")
    if current not in (original, patched):
        raise SystemExit("LLVM overlay contains changes beyond the registered copy patch.")
    args.receipts.mkdir(parents=True, exist_ok=True)
    if not backup.is_file(): backup.write_bytes(original)
    if current != patched: target.write_bytes(patched)
    patch = "".join(difflib.unified_diff(before.splitlines(keepends=True), after.splitlines(keepends=True),
                                     fromfile="a/"+relative, tofile="b/"+relative))
    configure = target.with_name("configure.bzl")
    configure_current = configure.read_bytes()
    configure_backup = args.receipts / "llvm-configure-pristine.bzl"
    if hashlib.sha256(configure_current).hexdigest() == LLVM_CONFIGURE_ORIGINAL:
        configure_before = configure_current
    elif configure_backup.is_file() and hashlib.sha256(configure_backup.read_bytes()).hexdigest() == LLVM_CONFIGURE_ORIGINAL:
        configure_before = configure_backup.read_bytes()
    else: raise SystemExit("Unregistered LLVM configure source; refusing to overwrite it.")
    configure_after = configure_before.replace(b"repository_ctx.execute(cmd, timeout = 20)",
        b'repository_ctx.execute(cmd, timeout = 600 if repository_ctx.os.name.lower().startswith("windows") else 20)', 1)
    if configure_current not in (configure_before, configure_after):
        raise SystemExit("LLVM configure contains unregistered changes.")
    if not configure_backup.is_file(): configure_backup.write_bytes(configure_before)
    if configure_current != configure_after: configure.write_bytes(configure_after)
    patch += "".join(difflib.unified_diff(configure_before.decode().splitlines(keepends=True),
        configure_after.decode().splitlines(keepends=True), fromfile="a/utils/bazel/configure.bzl", tofile="b/utils/bazel/configure.bzl"))
    (args.receipts / "llvm-windows-overlay-copy.patch").write_text(patch, encoding="utf-8", newline="\n")
    result = {"schema": "orca.semantic-build-compatibility/v1", "dependency": "LLVM 7a33569510535f0b917a2e50f644bf57490aee24",
              "file": str(target), "baseline_sha256": LLVM_OVERLAY_ORIGINAL,
              "patched_sha256": hashlib.sha256(patched).hexdigest(),
              "reason": "Copy identical source contents for the Windows build overlay without requiring symlink privileges.",
              "runtime_algorithm_changes": False}
    result["configure"] = {"file": str(configure), "baseline_sha256": LLVM_CONFIGURE_ORIGINAL,
                           "patched_sha256": hashlib.sha256(configure_after).hexdigest(),
                           "reason": "Windows content copies need more than upstream's 20 second symlink timeout; maximum 600 seconds."}
    (args.receipts / "llvm-overlay-patch.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print("Verified registered LLVM Windows build-overlay copy patch.")


if __name__ == "__main__": main()
