#!/usr/bin/env python3
"""Runs the examples of the Design commands against OrcaSlicer, to show they still work as written.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_examples.py [--tool NAME ...]

Lists the Design commands (search_tools {group: "Design"}), with undo and redo, which document the
Design history, reads each one's examples (search_tools {tool}), and runs every example of the named
commands (of all of them without --tool), each on a fresh, empty design: an open sketch is cancelled
and new_design discards the rest. An argument {"from_call": i, "key": k} is the value under k in the
result of call i of the same example. Every call is checked: an example fails when a call fails, or
when its result has ok false, a non-empty error, or is the person's form (interactive) instead of what
the call asked for. An example with `needs` uses a file from outside the app and is skipped. Prints a
line per example and the counts; exits 1 when any example failed.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from orca_mcp import McpError  # noqa: E402

# The shared commands whose examples are the Design history's.
HISTORY = ["undo", "redo"]


class ExampleFailed(Exception):
    pass


def resolve(args, results):
    """`args` with each {"from_call": i, "key": k} replaced by that earlier result's value."""
    out = {}
    for name, value in args.items():
        if isinstance(value, dict) and "from_call" in value:
            i, key = value["from_call"], value.get("key")
            if not isinstance(i, int) or not 0 <= i < len(results):
                raise ExampleFailed(f"argument {name}: from_call {i} is not an earlier call")
            if not isinstance(results[i], dict) or key not in results[i]:
                raise ExampleFailed(f"argument {name}: the result of call {i} has no key {key!r}")
            value = results[i][key]
        out[name] = value
    return out


def fresh_design(client):
    try:
        client.call_command("sketch_cancel")
    except McpError:  # no sketch was open
        pass
    client.call_command("new_design", discard_changes=True)


def failed(result):
    """Why a call's result says it did not do what it was asked, or None."""
    if not isinstance(result, dict):
        return None
    if result.get("interactive"):
        return "it ran the person's form (interactive)"
    if result.get("ok") is False or result.get("error"):
        return f"ok is {result.get('ok')}, error {result.get('error')!r}"
    return None


def run_example(client, example):
    results = []
    for n, call in enumerate(example["calls"]):
        tool = call["tool"]
        args = resolve(call.get("args", {}), results)
        try:
            result = client.call_command(tool, **args).get("result")
        except McpError as e:
            raise ExampleFailed(f"call {n}: {e}") from None
        if why := failed(result):
            raise ExampleFailed(f"call {n}: {tool}: {why}")
        results.append(result)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", action="append", default=[], metavar="NAME", help="run this command's examples (repeatable)")
    args = ap.parse_args()

    client, _ = orca_mcp.session_client()
    names = client.call("search_tools", group="Design")["names"] + HISTORY
    unknown = [t for t in args.tool if t not in names]
    if unknown:
        sys.exit(f"not Design commands: {', '.join(unknown)}")
    wanted = args.tool or names

    total = skipped = failures = 0
    for name in wanted:
        for example in client.call("search_tools", tool=name).get("examples") or []:
            total += 1
            label = f"{name}: {example['title']}"
            if example.get("needs"):
                skipped += 1
                print(f"skip {label}: needs {example['needs']}")
                continue
            try:
                fresh_design(client)
                run_example(client, example)
                print(f"ok   {label}")
            except (ExampleFailed, McpError) as e:
                failures += 1
                print(f"FAIL {label}: {e}")
    print(f"{total} examples, {skipped} skipped (needs), {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
