#!/usr/bin/env python3
"""Run clang-tidy on the C++ files changed since a base revision.

Check findings are reported only on added or modified lines, so existing code
is not held to checks it predates. A changed source file that does not compile
(for example one that only built because the precompiled header supplied an
include) fails wherever the error is. Headers are compiled on their own, and
fail only on errors in their changed lines. Deleting an #include also fails
on every use, changed or not, that now lacks the header it provided.

The checks come from .clang-tidy at the repository root. The compile database
must come from a configure with SLIC3R_PCH=OFF, or the precompiled header hides
missing includes.

    python3 scripts/clang_tidy_diff.py -p build-tidy --base origin/main
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import PurePosixPath

SOURCE_EXTENSIONS = {".cpp", ".cc", ".cxx"}
HEADER_EXTENSIONS = {".hpp", ".h", ".hxx"}
CHECKED_DIRS = ("src/", "tests/")
# Vendored code inside the checked directories.
EXCLUDED_DIRS = ("src/glad/", "tests/catch2/")

# Per file. A deleted include can leave hundreds of follow-on errors.
MAX_REPORTED = 30

HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
DIAGNOSTIC_RE = re.compile(r"^(.+?):(\d+):(\d+): (error|warning): (.*)$")
FIX_MESSAGE_RE = re.compile(r"^\s+Message:\s+(['\"])(.*)\1$")
FIX_INCLUDE_RE = re.compile(r"^\s+ReplacementText:\s+['\"]#include ([^\\'\"]*)")
REMOVED_INCLUDE_RE = re.compile(r'^-\s*#\s*include\s*[<"]([^>"]+)[>"]')


@dataclass
class FileChange:
    lines: list = field(default_factory=list)            # [first, last] ranges of added lines
    removed_includes: set = field(default_factory=set)   # header names whose #include was deleted


@dataclass
class Diagnostic:
    file: str
    line: int
    col: int
    level: str
    message: str
    include: str = ""   # header named by the fix, when the fix adds an #include

    @property
    def is_compile_error(self):
        return self.message.endswith("[clang-diagnostic-error]")

    def __str__(self):
        return self.message + (f" (add #include {self.include})" if self.include else "")


def parse_diff(diff):
    """Map each file in a `git diff -U0` to the lines it adds and the includes it deletes."""
    changes = {}
    change = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            target = line[4:]
            change = changes.setdefault(target[2:], FileChange()) if target.startswith("b/") else None
            continue
        if change is None:
            continue
        match = HUNK_RE.match(line)
        if match:
            first = int(match.group(1))
            count = int(match.group(2) or 1)
            if count > 0:
                change.lines.append([first, first + count - 1])
            continue
        match = REMOVED_INCLUDE_RE.match(line)
        if match:
            change.removed_includes.add(match.group(1))
    return changes


def is_checked(path):
    if not path.startswith(CHECKED_DIRS) or path.startswith(EXCLUDED_DIRS):
        return False
    return PurePosixPath(path).suffix in SOURCE_EXTENSIONS | HEADER_EXTENSIONS


def changed_files(merge_base):
    # Against the working tree, so a local run covers uncommitted edits too.
    diff = subprocess.run(["git", "diff", "-U0", "--no-color", "--no-ext-diff", "--diff-filter=AMR", merge_base],
                          check=True, capture_output=True, text=True).stdout
    return {path: change for path, change in parse_diff(diff).items() if is_checked(path)}


def compiled_sources(build_dir):
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        return {os.path.realpath(os.path.join(entry["directory"], entry["file"])) for entry in json.load(f)}


def run_clang_tidy(clang_tidy, build_dir, path, lines, extra_args):
    """Run clang-tidy on one file, limited to `lines` unless it is None."""
    with tempfile.TemporaryDirectory() as tmp:
        fixes = os.path.join(tmp, "fixes.yaml")
        # The compile database comes from the system clang, which may know warning
        # flags a newer clang-tidy has dropped.
        cmd = [clang_tidy, "-p", build_dir, "--quiet", "--export-fixes=" + fixes,
               "--extra-arg=-Wno-unknown-warning-option", "--extra-arg=-ferror-limit=0", *extra_args, path]
        if lines is not None:
            cmd.insert(1, "--line-filter=" + json.dumps([{"name": path, "lines": lines}]))
        result = subprocess.run(cmd, capture_output=True, text=True)
        suggestions = parse_suggested_includes(fixes)
    output = result.stdout + result.stderr
    return result.returncode, output, parse_diagnostics(output, suggestions)


def parse_suggested_includes(fixes_path):
    """Map each message in an --export-fixes file to the header its fix includes."""
    if not os.path.exists(fixes_path):
        return {}
    suggestions = {}
    message = None
    with open(fixes_path, encoding="utf-8") as f:
        for line in f:
            match = FIX_MESSAGE_RE.match(line)
            if match:
                message = match.group(2).replace("''", "'")
                continue
            match = FIX_INCLUDE_RE.match(line)
            if match and message:
                suggestions[message] = match.group(1)
                message = None
    return suggestions


def parse_diagnostics(output, suggestions):
    diagnostics = []
    for line in output.splitlines():
        match = DIAGNOSTIC_RE.match(line)
        if match:
            file, row, col, level, message = match.groups()
            include = suggestions.get(re.sub(r" \[[^]]*\]$", "", message), "")
            diagnostics.append(Diagnostic(file, int(row), int(col), level, message, include))
    return diagnostics


def in_ranges(line, ranges):
    return any(first <= line <= last for first, last in ranges)


def names_removed_include(include, removed):
    """Whether `include` (<a/b.hpp>) is one of the deleted includes, however it was spelled."""
    name = include.strip('<>"')
    return bool(name) and any(name == r or name.endswith("/" + r) or r.endswith("/" + name) for r in removed)


def own_errors(diagnostics, path):
    real = os.path.realpath(path)
    return [d for d in diagnostics if d.is_compile_error and os.path.realpath(d.file) == real]


def error_sites(errors, text):
    """Where each error points, as (source line, column): stable across edits to other lines,
    unlike line numbers, and across a removed include, unlike clang's wording."""
    lines = text.splitlines()
    return Counter((lines[d.line - 1].strip() if d.line <= len(lines) else "", d.col) for d in errors)


def errors_alone_at(revision, clang_tidy, build_dir, path):
    """The error sites a header had when compiled on its own at `revision`."""
    shown = subprocess.run(["git", "show", f"{revision}:{path}"], capture_output=True, text=True)
    if shown.returncode != 0:
        return Counter()
    # Beside the original, so its quoted includes resolve the same way.
    p = PurePosixPath(path)
    copy = str(p.with_name(f".{p.stem}.clang-tidy-base{p.suffix}"))
    try:
        with open(copy, "w", encoding="utf-8") as f:
            f.write(shown.stdout)
        _, _, diagnostics = run_clang_tidy(clang_tidy, build_dir, copy, None, [])
    finally:
        os.remove(copy)
    return error_sites(own_errors(diagnostics, copy), shown.stdout)


def check_file(clang_tidy, build_dir, merge_base, path, change, extra_args):
    """Run clang-tidy on one file and return (failed, output, failing diagnostics)."""
    # A deleted include can orphan uses on unchanged lines, so such a file is
    # checked whole and the findings narrowed here. --fix keeps the line filter
    # so it never rewrites unrelated code.
    whole = bool(change.removed_includes) and not extra_args
    returncode, output, diagnostics = run_clang_tidy(clang_tidy, build_dir, path,
                                                     None if whole else change.lines, extra_args)
    real = os.path.realpath(path)

    def introduced(d):
        return os.path.realpath(d.file) == real and (
            in_ranges(d.line, change.lines) or names_removed_include(d.include, change.removed_includes))

    if PurePosixPath(path).suffix in HEADER_EXTENSIONS:
        # Many existing headers only compile after what their includers include
        # first, so a header is held to errors it introduces: on its changed
        # lines, or anywhere a deleted include leaves it with new errors.
        failing = [d for d in diagnostics if introduced(d)]
        errors = own_errors(diagnostics, path)
        if errors and change.removed_includes:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            before = errors_alone_at(merge_base, clang_tidy, build_dir, path)
            failing += [d for d in errors if d not in failing
                        and (error_sites([d], text) - before)]
        return bool(failing), output, failing
    if whole:
        failing = [d for d in diagnostics if d.is_compile_error or introduced(d)]
        return bool(failing), output, failing
    return returncode != 0, output, diagnostics


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-p", "--build-dir", required=True, help="directory holding compile_commands.json")
    parser.add_argument("--base", default="origin/main", help="revision to diff against (default: origin/main)")
    parser.add_argument("--clang-tidy", default="clang-tidy", help="clang-tidy executable")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    parser.add_argument("extra_args", nargs="*", help="passed to clang-tidy after --, e.g. -- --fix")
    args = parser.parse_args()

    merge_base = subprocess.run(["git", "merge-base", args.base, "HEAD"], check=True,
                                capture_output=True, text=True).stdout.strip()
    files = changed_files(merge_base)
    sources = compiled_sources(args.build_dir)
    todo = []
    for path, change in sorted(files.items()):
        is_source = PurePosixPath(path).suffix in SOURCE_EXTENSIONS
        if is_source and os.path.realpath(path) not in sources:
            print(f"Skipping {path}: not compiled in this configuration")
        elif change.lines or change.removed_includes:
            todo.append((path, change))
    if not todo:
        print("No changed C++ lines to check.")
        return 0

    print(f"Checking {len(todo)} file(s) with {args.clang_tidy}")
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    root = os.getcwd() + os.sep
    failed = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = {path: pool.submit(check_file, args.clang_tidy, args.build_dir, merge_base, path, change, args.extra_args)
                for path, change in todo}
        for path, job in jobs.items():
            file_failed, output, diagnostics = job.result()
            if not file_failed:
                continue
            failed.append(path)
            print(f"\n==== {path}")
            if not diagnostics:
                print(output, end="")
            for d in diagnostics[:MAX_REPORTED]:
                file = d.file.removeprefix(root)
                print(f"{file}:{d.line}:{d.col}: {d.level}: {d}")
                if annotate:
                    print(f"::{d.level} file={file},line={d.line},col={d.col}::{d}")
            if len(diagnostics) > MAX_REPORTED:
                print(f"... and {len(diagnostics) - MAX_REPORTED} more")

    if failed:
        print(f"\nclang-tidy failed on {len(failed)} file(s). Add the includes it names, or apply its "
              "suggestions locally with scripts/run_clang_tidy.sh --fix (scripts\\run_clang_tidy.ps1 -Fix on Windows).")
        return 1
    print("clang-tidy passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
