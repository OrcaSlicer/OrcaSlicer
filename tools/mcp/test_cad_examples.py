#!/usr/bin/env python3
"""Runs the examples of the CAD tool table against OrcaSlicer, to show they still work as written.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_examples.py [--tool NAME ...]

Lists the CAD tools, describes them all, and runs every example of the named tools (of all of them
without --tool), each on a fresh, empty design: an open sketch is cancelled and new_design discards
the rest. An argument {"from_call": i, "key": k} is the value under k in the reply of call i of the
same example. A call to a tool whose failure is "reply" fails the example when its reply has ok
false or an error. An example with `needs` uses a file from outside the app and is skipped. Prints a
line per example and the counts; exits 1 when any example failed.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from orca_mcp import McpError  # noqa: E402


class ExampleFailed(Exception):
    pass


def resolve(args, replies):
    """`args` with each {"from_call": i, "key": k} replaced by that earlier reply's value."""
    out = {}
    for name, value in args.items():
        if isinstance(value, dict) and "from_call" in value:
            i, key = value["from_call"], value.get("key")
            if not isinstance(i, int) or not 0 <= i < len(replies):
                raise ExampleFailed(f"argument {name}: from_call {i} is not an earlier call")
            if not isinstance(replies[i], dict) or key not in replies[i]:
                raise ExampleFailed(f"argument {name}: the reply of call {i} has no key {key!r}")
            value = replies[i][key]
        out[name] = value
    return out


def fresh_design(client):
    try:
        client.call_cad_tool("sketch_cancel")
    except McpError:  # no sketch was open
        pass
    client.call_cad_tool("new_design", discard_changes=True)


def run_example(client, example, failure):
    replies = []
    for n, call in enumerate(example["calls"]):
        tool = call["tool"]
        args = resolve(call.get("args", {}), replies)
        try:
            reply = client.call_cad_tool(tool, **args)
        except McpError as e:
            raise ExampleFailed(f"call {n}: {e}") from None
        if failure.get(tool) == "reply" and isinstance(reply, dict) and (reply.get("ok") is False or reply.get("error")):
            raise ExampleFailed(f"call {n}: {tool}: ok is {reply.get('ok')}, error {reply.get('error')!r}")
        replies.append(reply)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", action="append", default=[], metavar="NAME", help="run this tool's examples (repeatable)")
    args = ap.parse_args()

    client, _ = orca_mcp.session_client()
    names = [t["name"] for t in client.call("list_cad_tools")["tools"]]
    unknown = [t for t in args.tool if t not in names]
    if unknown:
        sys.exit(f"not CAD tools: {', '.join(unknown)}")
    rows = client.call("describe_cad_tool", tool=names)["tools"]
    failure = {row["name"]: row.get("failure") for row in rows}
    wanted = set(args.tool) or set(names)

    total = skipped = failed = 0
    for row in rows:
        if row["name"] not in wanted:
            continue
        for example in row["examples"]:
            total += 1
            label = f"{row['name']}: {example['title']}"
            if example.get("needs"):
                skipped += 1
                print(f"skip {label}: needs {example['needs']}")
                continue
            try:
                fresh_design(client)
                run_example(client, example, failure)
                print(f"ok   {label}")
            except (ExampleFailed, McpError) as e:
                failed += 1
                print(f"FAIL {label}: {e}")
    print(f"{total} examples, {skipped} skipped (needs), {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
