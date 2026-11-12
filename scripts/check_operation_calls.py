#!/usr/bin/env python3
"""Checks that the callers of the operations layer make no owner call an operation stands for.

The Speed Dial's command runners, the MCP tools and the plugin API's bindings change the app through
src/slic3r/GUI/Operations (docs/HLSD/mcp-server.md, "Operations"). This script bans, in their source
files, the owner methods those operations call, by method name. A ban is on a name, so it also meets
calls that belong to another function: a banned call that stays is listed in
scripts/operation_calls_baseline.txt, by file and exact source line, under the reason it stays.

The check fails on a banned call that is not listed, and on a listed line that is no longer in the
source, so the list shrinks, or is edited in plain sight.

    python3 scripts/check_operation_calls.py           # check
    python3 scripts/check_operation_calls.py --print   # print every banned call found, as baseline lines

It does not catch a rule written again through calls that are not banned, such as a check made of
getters; the rule in the design doc and review cover that.
"""

import argparse
import collections
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = "scripts/operation_calls_baseline.txt"

# The callers: the command runners, the tools (the CAD tools are one entry point of their own behind
# call_cad_tool) and the plugin API.
CALLERS = ["src/slic3r/GUI/NativeCommands.cpp", "src/slic3r/GUI/Mcp/McpTools*.cpp", "src/slic3r/plugin/host/*.cpp"]
NOT_CALLERS = {"src/slic3r/GUI/Mcp/McpToolsCad.cpp"}

# Owner methods an operation calls, by the operation that stands for them.
BANNED = {
    "start_gcode_export": ["export_gcode_to"],
    "import_models": ["load_files"],
    "open_project": ["load_project"],
    "new_project": ["new_project"],
    "save_project_as": ["save_project"],
    "export_sliced_3mf": ["export_gcode_3mf"],
    "export_model_3mf": ["export_core_3mf"],
    "export_stl": ["export_stl"],
    "select_plate": ["select_plate"],
    "set_plate_locked": ["lock_plate", "set_plate_locked"],
    "start_arrange": ["set_prepare_state", "arrange"],
    "start_slice": ["request_slice"],
    "show_layer_range": ["SetHigherValue", "SetLowerValue", "show_layer_range"],
    "select_preset": ["select_preset"],
    "assign_filament_preset": ["set_filament_preset", "select_filament_preset"],
    "import_presets": ["import_configs"],
    "set_instance_count": ["set_number_of_copies"],
    "send_gcode_to_host": ["send_gcode_legacy"],
    "the undo snapshot of an operation": ["take_snapshot"],
}

# A call of a method: obj.name( or obj->name( . An operation is a free function (Ops::name), so the
# operations that share a name with an owner method are not met.
CALL = re.compile(r"(?:\.|->)\s*(" + "|".join(sorted({m for names in BANNED.values() for m in names})) + r")\s*\(")


def caller_files():
    files = set()
    for pattern in CALLERS:
        for path in glob.glob(os.path.join(REPO, pattern)):
            rel = os.path.relpath(path, REPO).replace(os.sep, "/")
            if rel not in NOT_CALLERS:
                files.add(rel)
    return sorted(files)


def banned_calls():
    """{file: [stripped source line, ...]} for every line that makes a banned call, in file order."""
    found = collections.OrderedDict()
    for rel in caller_files():
        with open(os.path.join(REPO, rel), encoding="utf-8") as f:
            for line in f:
                text = line.strip()
                if text.startswith("//"):
                    continue
                if CALL.search(text.split("//")[0]):
                    found.setdefault(rel, []).append(text)
    return found


def read_baseline():
    """{file: Counter of listed lines}. A [file] line starts a file's section, # starts a reason."""
    listed = collections.OrderedDict()
    current = None
    with open(os.path.join(REPO, BASELINE), encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            text = line.strip()
            if not text or text.startswith("#"):
                continue
            if text.startswith("[") and text.endswith("]") and "(" not in text:
                current = listed.setdefault(text[1:-1], collections.Counter())
            elif current is None:
                sys.exit(f"{BASELINE}:{number}: a source line before any [file] section")
            else:
                current[text] += 1
    return listed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--print", action="store_true", help="print every banned call found, as baseline lines")
    args = ap.parse_args()

    found = banned_calls()
    if args.print:
        for rel, lines in found.items():
            print(f"[{rel}]")
            for text in lines:
                print(text)
            print()
        return 0

    listed = read_baseline()
    problems = []
    for rel, lines in found.items():
        left = collections.Counter(lines)
        left.subtract(listed.get(rel, collections.Counter()))
        for text, count in left.items():
            if count > 0:
                problems.append(f"{rel}: a banned owner call that is not in {BASELINE}:\n    {text}")
    for rel, counts in listed.items():
        left = collections.Counter(counts)
        left.subtract(collections.Counter(found.get(rel, [])))
        for text, count in left.items():
            if count > 0:
                problems.append(f"{BASELINE}: [{rel}] lists a line the source no longer has:\n    {text}")
    if problems:
        print("\n".join(problems))
        print("\nA command runner, a tool or a plugin API binding changes the app through an operation of "
              "src/slic3r/GUI/Operations.\nCall the operation, or, where the call belongs to another function or to "
              f"a path that asks a person, list the line in {BASELINE} under its reason.")
        return 1
    calls = sum(len(lines) for lines in found.values())
    print(f"{calls} banned owner calls in {len(caller_files())} caller files, all listed in {BASELINE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
