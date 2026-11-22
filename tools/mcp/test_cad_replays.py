#!/usr/bin/env python3
"""The Design tests that only send commands, as the test_cad_replays entry of tools/mcp/test-entries/design.md designs
them, run one after another in one app.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_replays.py [part ...]

With no part named, every part runs, in the order of PARTS:
- the six replays in tests/ (cad_sketch_to_solid, cad_sketch_edit, cad_topology_targets, cad_features, cad_doc,
  cad_canvas), each run with replay_run from a new project, and passing when replay_run says it passed;
- round_trip: a Design session made through tool_call, exported with replay_export and replayed into a new project
  to the same part. The session holds an empty sketch commit, a hole that misses and a fillet the kernel refuses,
  which the export leaves out, a shell that reports a rejected rebuild (ok false), which it keeps, and face and edge
  ids passed with a generation, which it drops;
- slow_build: a thread 250 mm deep, which rebuilds off the UI thread behind the progress dialog for several seconds,
  while app_state and a window screenshot answer, another Design command is refused as busy, and (not on Windows,
  where the dialog is the system's task dialog) widget_list lists the progress dialog and a screenshot captures it;
- examples: every example tool_describe documents for the Design commands tool_list lists, and for undo and redo,
  run as written on a
  fresh design, an argument {"from_call": i, "key": k} taking the value under k in the result of call i of the same
  example. An example fails when a call fails or its result has ok false, a non-empty error, or is the person's form
  (interactive); one with `needs` uses a file from outside the app and is skipped.

Each part starts from the same state, whatever the part before it left: start() puts the app in the baseline on the
Generic Klipper Printer, closing what was left open, an open card, sketch, Text dialog or plane pick in the Design
tab among it, and the Design view's switches are set as a new data folder has them. The replays, the exported
session and the screenshots go to a temporary folder, which the run prints. Exits 1 when a check fails or a part
breaks off.
"""

import json
import math
import os
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from cad_gui import reset_switches  # noqa: E402
from checks import Checks, close_open, start, until  # noqa: E402
from orca_mcp import McpError  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPLAYS = ["cad_sketch_to_solid", "cad_sketch_edit", "cad_topology_targets", "cad_features", "cad_doc", "cad_canvas"]
# The longest replay runs about a minute on a loaded machine; a replay held past this has stopped.
REPLAY_TIMEOUT_S = 600

# The shared commands whose examples are the Design history's.
HISTORY = ["undo", "redo"]

# An M8 thread tapped 250 mm deep into a 30 mm square block: 200 turns, several seconds of OCCT pipe and
# boolean work (about 5.5 s on a desktop PC running nothing else). The probes need the build still running
# once it is seen running.
BLOCK = {"width": 30, "height": 30, "distance": 250}
THREAD = {"plane": "XY", "internal": True, "designation": "M8", "length": 250}


# ---------------------------------------------------------------- between parts

def reset(client):
    """The state every part starts from: start()'s baseline on the Generic Klipper Printer, which closes what the part
    before left open, cancel_tool's leftovers in the Design tab among it, and the Design view's switches as a new
    data folder has them, set on the Design tab before Prepare is shown again."""
    start(client)
    reset_switches(client)
    client.call("view_set", tab="prepare")


# ---------------------------------------------------------------- the replays

def replay_part(name):
    def part(c, client, out):
        reply = client.await_job(client.call("replay_run", path=os.path.join(HERE, "tests", name + ".json"),
                                             out_dir=os.path.join(out, name)), timeout=REPLAY_TIMEOUT_S)
        steps = reply.get("steps", [])
        print(f"{name}: {len(steps)} steps in {sum(s.get('duration_ms', 0) for s in steps) / 1000:.1f} s")
        failed = next((s for s in steps if s.get("index") == reply.get("failed_step")), None)
        detail = failed and {k: failed.get(k) for k in ("index", "tool", "args", "error", "expect_failure", "reason")}
        c.check(f"{name}: the replay passes", reply.get("passed") is True, detail or reply)
    return part


# ---------------------------------------------------------------- round_trip

def cad(client, tool, **args):
    return client.call("tool_call", _timeout=300, tool=tool, args=args)


def refused(client, tool, **args):
    """The error text of a call that must fail, or None when it succeeded."""
    try:
        cad(client, tool, **args)
    except McpError as e:
        return str(e)
    return None


def record(client):
    """The session to export, made through tool_call (replay steps are not journaled); returns the reads the replay
    must match and the refusals seen. The thread rebuilds behind the progress dialog."""
    client.call("replay_export", clear=True)
    client.call("project_new", discard_changes=True)
    cad(client, "new_design", discard_changes=True)
    frame = cad(client, "sketch_create", plane="XY")["frame"]
    empty_commit = refused(client, "sketch_finish")
    # A 40 x 30 plate with an M5 tap-drill bore (minor diameter 4.134), 12 thick.
    cad(client, "sketch_draw", type="rectangle_2_point", p0=[-20, -15], p1=[20, 15])
    cad(client, "sketch_draw", entities=[{"type": "circle_center", "center": [0, 0], "radius": 2.067}])
    cad(client, "sketch_finish")
    cad(client, "extrude", sketch=0, distance=12)
    missed_hole = refused(client, "hole", x=500, through=True)
    corners = cad(client, "body_describe", body=0, edge_kinds=["line"], direction=[0, 0, 1], convex=True)
    cad(client, "fillet", edges=[e["id"] for e in corners["edges"]], radius=3, body=0,
        generation=corners["generation"])
    refused_fillet = refused(client, "fillet", edges=corners["edges"][0]["id"], radius=50, body=0)
    # No wall 20 thick fits in the 12 mm plate, so the kernel makes no shell.
    rejected_shell = cad(client, "shell", thickness=20, body=0)
    bores = cad(client, "body_describe", body=0, kinds=["cylindrical"])
    bore = next(f["id"] for f in bores["faces"] if f.get("internal"))
    thread = client.await_job(client.call("tool_call", _timeout=300, tool="thread",
                                          args={"body": 0, "face": bore, "generation": bores["generation"]}))
    return {"mass": cad(client, "body_mass_properties", body=0), "scene": cad(client, "describe_design"),
            "frame": frame, "empty_commit": empty_commit, "missed_hole": missed_hole, "refused_fillet": refused_fillet,
            "rejected_shell": rejected_shell, "thread": thread}


def same(a, b, rel=1e-6):
    return math.isclose(a, b, rel_tol=rel, abs_tol=1e-6)


def part_round_trip(c, client, out):
    live = record(client)
    path = os.path.join(out, "round_trip", "session.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    client.call("replay_export", path=path, overwrite=True)
    with open(path) as f:
        text = f.read()
    steps = json.loads("\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//")))["steps"]
    # A Design command's call is journaled as its own; the session starts with project_new.
    calls = [s for s in steps if s["tool"] != "project_new"]
    tools = [s["tool"] for s in calls]

    client.call("project_new", discard_changes=True)
    replay = client.await_job(client.call("replay_run", path=path, out_dir=os.path.dirname(path)), timeout=900)
    mass = cad(client, "body_mass_properties", body=0)
    scene = cad(client, "describe_design")
    kinds = lambda s: [f["type"] for f in s["features"]]  # noqa: E731 (names count on across projects)

    origin = live["scene"]["modeling_origin"]
    print(f"round_trip: volume {live['mass']['volume']:.3f} mm3")
    for name, ok, detail in [
        ("a sketch on XY and the plate are centred on modeling_origin",
         live["frame"]["origin"] == origin and all(same(a, b) for a, b in zip(live["mass"]["center_of_mass"][:2], origin[:2])),
         (live["frame"]["origin"], live["mass"]["center_of_mass"], origin)),
        ("an empty sketch_finish fails", bool(live["empty_commit"]), live["empty_commit"]),
        ("a hole that misses fails as an error", bool(live["missed_hole"]), live["missed_hole"]),
        ("a fillet the kernel refuses fails as an error with the kernel's reason",
         bool(live["refused_fillet"]) and "Invalid arguments" not in live["refused_fillet"], live["refused_fillet"]),
        ("a shell thicker than the plate reports the rejected rebuild in its reply (ok false)",
         live["rejected_shell"].get("ok") is False, live["rejected_shell"]),
        ("the thread is M5, inferred from the bore", live["thread"].get("designation") == "M5", live["thread"]),
        ("the export leaves out the failed calls", tools.count("sketch_finish") == 1 and "hole" not in tools, tools),
        ("the export leaves out the refused fillet and keeps the one that ran", tools.count("fillet") == 1, tools),
        ("the export keeps the shell that reported a rejected rebuild in its reply", tools.count("shell") == 1, tools),
        ("the export records no generation", all("generation" not in s.get("args", {}) for s in calls), calls),
        ("the exported session replays", replay.get("passed") is True,
         [s for s in replay.get("steps", []) if not s.get("ok")][:1]),
        ("the same features", kinds(scene) == kinds(live["scene"]), (kinds(scene), kinds(live["scene"]))),
        ("the same bodies and undo depth",
         len(scene["bodies"]) == len(live["scene"]["bodies"]) and scene["undo_depth"] == live["scene"]["undo_depth"],
         (len(scene["bodies"]), scene["undo_depth"], live["scene"]["undo_depth"])),
        ("the same volume and surface area",
         same(mass["volume"], live["mass"]["volume"]) and same(mass["surface_area"], live["mass"]["surface_area"]),
         (mass["volume"], live["mass"]["volume"])),
        ("the same centre of mass",
         all(same(a, b) for a, b in zip(mass["center_of_mass"], live["mass"]["center_of_mass"])),
         (mass["center_of_mass"], live["mass"]["center_of_mass"])),
    ]:
        c.check(f"round_trip: {name}", ok, detail)


# ---------------------------------------------------------------- slow_build

def progress_dialog_ref(client):
    """The ref of the rebuild's progress dialog (titled "Design") in widget_list, or None."""
    _, texts = client.call_with_text("widget_list")
    for line in "\n".join(texts).splitlines():
        m = re.match(r'\[(w\d+)\] (?:window|dialog) "Design" \((\w+)\)', line)
        if m and "Progress" in m.group(2):
            return m.group(1)
    return None


def refusal(client):
    """The error another Design command, describe_design, is refused with, or None when it answers."""
    try:
        client.call("tool_call", tool="describe_design")
        return None
    except McpError as e:
        return str(e)


def part_slow_build(c, client, out):
    """The thread, sweep and loft commands rebuild as their cards do, off the UI thread behind the progress dialog
    (DesignPanel::recompute_guarded); the probes run while the thread builds."""
    shots = os.path.join(out, "slow_build")
    os.makedirs(shots, exist_ok=True)
    client.call("project_new", discard_changes=True)
    client.call("tool_call", tool="new_design", args={"discard_changes": True})
    client.call("tool_call", tool="extrude", args=BLOCK)

    box = {}

    def build():
        try:
            box["result"] = client.call("tool_call", _timeout=600, tool="thread", args=THREAD)
        except McpError as e:
            box["error"] = str(e)

    worker = threading.Thread(target=build)
    started = time.time()
    worker.start()

    # The probes run once the build is seen running: once widget_list lists its progress dialog, or on Windows, where
    # the dialog is the system's task dialog on a thread of its own and no wx window widget_list lists, once another
    # Design command is refused as busy. A build that ends first is seen as "ended", and the probes do not run.
    native_dialog = sys.platform == "win32"

    def running():
        if not worker.is_alive():
            return "ended"
        if native_dialog:
            return "busy" if "Busy" in (refusal(client) or "") else None
        return progress_dialog_ref(client)

    seen = until(running, "the thread's rebuild running", timeout_s=300)
    state_ms, window_shot, dialog_shot, busy = None, None, None, None
    if seen != "ended":
        ref = None if native_dialog else seen
        t0 = time.time()
        client.call("app_state")
        state_ms = (time.time() - t0) * 1000
        window_shot = client.call("window_screenshot", target="main", include_image=False,
                                  save_path=os.path.join(shots, "window.png"))
        if ref is not None:
            dialog_shot = client.call("window_screenshot", target=ref, include_image=False,
                                      save_path=os.path.join(shots, "progress.png"))
        # Asked after the probes, so that its refusal says the build still ran while they were answered.
        busy = refusal(client)
    worker.join(600)
    seconds = time.time() - started
    result = client.await_job(box.get("result")) if "result" in box else None
    scene = client.call("tool_call", tool="describe_design")
    threads = [f for f in scene["features"] if f["type"] == "Thread"]

    print(f"slow_build: the build took {seconds:.1f} s")
    results = [
        ("the thread call succeeds", result is not None and "thread_index" in result and "ok" not in result, box),
        ("app_state answers during the build", state_ms is not None, state_ms),
        ("a window screenshot succeeds during the build", bool(window_shot and window_shot.get("width")), window_shot),
        ("another Design command is refused during the build", bool(busy) and "Busy" in busy, busy),
        ("one thread feature and one body", len(threads) == 1 and len(scene["bodies"]) == 1, scene["features"]),
    ]
    if not native_dialog:
        results[1:1] = [
            ("the progress dialog is listed while it builds", dialog_shot is not None, "no window \"Design\" in widget_list"),
            ("a screenshot of the progress dialog captures it",
             bool(dialog_shot and dialog_shot.get("width")) and dialog_shot.get("width") < (window_shot or {}).get("width", 0),
             dialog_shot),
        ]
    for name, ok, detail in results:
        c.check(f"slow_build: {name}", ok, detail)


# ---------------------------------------------------------------- examples

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
            result = client.call_command(tool, **args)
        except McpError as e:
            raise ExampleFailed(f"call {n}: {e}") from None
        if why := failed(result):
            raise ExampleFailed(f"call {n}: {tool}: {why}")
        results.append(result)


def part_examples(c, client, out):
    listed = [item["name"] for item in client.call("tool_list", group="Design")["items"]]
    c.check("examples: tool_list lists the 88 Design commands", len(listed) == 88, len(listed))
    names = listed + HISTORY
    total = skipped = 0
    ran = set()
    for name in names:
        for example in client.call("tool_describe", tool=name).get("examples") or []:
            total += 1
            label = f"examples: {name}: {example['title']}"
            if example.get("needs"):
                skipped += 1
                print(f"skip {label}: needs {example['needs']}")
                continue
            try:
                # A fresh, empty design, whatever the example before left open: start()'s closing of what is open,
                # cancel_tool's leftovers among it.
                close_open(client)
                client.call_command("new_design", discard_changes=True)
                run_example(client, example)
                c.check(label, True)
                ran.add(name)
            except (ExampleFailed, McpError) as e:
                c.check(label, False, e)
    c.check("examples: at least one example of extrude ran", "extrude" in ran, sorted(ran))
    print(f"examples: {total} examples, {skipped} skipped (needs)")


PARTS = {**{name: replay_part(name) for name in REPLAYS},
         "round_trip": part_round_trip, "slow_build": part_slow_build, "examples": part_examples}


def main():
    names = sys.argv[1:] or list(PARTS)
    unknown = [n for n in names if n not in PARTS]
    if unknown:
        sys.exit(f"unknown parts {' '.join(unknown)}: have {' '.join(PARTS)}")
    client, _ = orca_mcp.session_client()
    # A replay's results are compared with the live run's to 1e-6, closer than a rounded reply keeps.
    client.exact_numbers = True
    out = tempfile.mkdtemp(prefix="orca-mcp-cad-replays-")
    print(f"the replays' output and the screenshots are in {out}")
    c = Checks()
    for name in names:
        c.scenario(name, lambda part=PARTS[name]: (reset(client), part(c, client, out)))
    return c.exit_code()


if __name__ == "__main__":
    sys.exit(main())
