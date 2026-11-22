#!/usr/bin/env python3
"""Checks that a Design-tab call whose job fails after its reply is left out of replay_export.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_failed_job.py

A coil swept along a helix makes every rebuild of the design slow, so a hole that misses it, which
the kernel refuses only after the rebuild and takes back with another, replies with a running job
past the reply budget and then fails. The budget is 45 s, or the cap ORCA_MCP_REPLY_BUDGET_MS sets
when the app and this script have it (the sweep sets 4 s), and the coil is sized for it. An export
while it runs leaves it out and names it in left_out_running; once it has failed, a rename made after it is in the export, the hole is not, and
the replay runs into a new project to the same features and coil. The session's first coil, of 12
turns, times a build, and the coil's turns are sized from it, taking a build's time to grow with the
cube of the turns, so the failing call outlives the budget on a faster or slower machine; the session
is made again only when the sized coil has more turns. A busy machine bends that curve, so when the
hole still fails inside the budget, or its job has ended by the time of the export, the session is
made again with a bigger coil. Takes four to eight minutes with the full budget, under a
minute with a 4 s one. Exits 0 when every check passes.
"""

import json
import math
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks  # noqa: E402
import orca_mcp  # noqa: E402

# The coil's build time to aim for. The missed hole takes about as long as the build, so this is past the
# reply budget by a third of it and by at least 4 s: the hole is still running at the export after its reply.
REBUILD_S = max(orca_mcp.reply_budget_s() * 4 / 3, orca_mcp.reply_budget_s() + 4)


def cad(client, tool, **args):
    """A Design command's reply, or the envelope of the job it runs as."""
    return client.call("tool_call", _timeout=600, tool=tool, args=args)


def coil(client, turns):
    """A new design holding one coil: a 1.2 mm wire swept along a helix of radius 10 and pitch 3. Each call's job is
    followed to its end, since under the sweep's 4 s budget one may reply running on a loaded machine."""
    client.await_job(cad(client, "new_design", discard_changes=True))
    client.await_job(cad(client, "helix", radius=10, pitch=3, height=3 * turns))
    client.await_job(cad(client, "sketch_create", plane="XZ"))
    client.await_job(cad(client, "sketch_draw", type="circle_center", center=[10, 0], radius=1.2))
    client.await_job(cad(client, "sketch_finish"))
    start = time.time()
    client.await_job(cad(client, "sweep", sketch=1, path=0))
    return time.time() - start


def main():
    client, _ = orca_mcp.session_client()
    out_dir = tempfile.mkdtemp(prefix="orca-mcp-cad-failed-job-")
    path = os.path.join(out_dir, "session.json")
    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))

    # ---- The session to export ----
    # Its first coil, of 12 turns, times the build; the session is made again only when the coil it
    # sizes has more turns. The hole must reply running and still run at the export after its reply.
    # One that fails inside the budget, or whose job has ended by the export, is made again with a
    # bigger coil.
    checks.start(client)
    # An app's first Design command builds the Design tab, which can outlast a 4 s budget. Built before the session
    # is recorded, it adds no job to the export, which a replay into the built tab would wait for in vain.
    client.await_job(cad(client, "new_design", discard_changes=True))
    client.call("replay_export", clear=True)
    client.call("project_new", discard_changes=True)
    built = 12
    calibration = coil(client, built)
    turns = max(built, min(40, round(built * (REBUILD_S / max(calibration, 0.5)) ** (1 / 3))))
    print(f"{built} turns built in {calibration:.1f} s; the test coil has {turns}", flush=True)
    during = {}
    running = False
    while True:
        if turns != built:
            client.call("replay_export", clear=True)
            client.call("project_new", discard_changes=True)
            coil(client, turns)
            built = turns
        start = time.time()
        try:
            started = cad(client, "hole", x=500, through=True)
        except orca_mcp.McpError as e:
            if turns >= 40 or "removed no material" not in str(e):
                raise
            print(f"the hole failed inside the budget, in {time.time() - start:.1f} s; trying {min(40, turns + 4)} turns",
                  flush=True)
            turns = min(40, turns + 4)
            continue
        if started.get("status") != "running":
            break
        during = client.call("replay_export")
        # Asked of the job itself, which needs no UI thread (the rebuild may hold it); a failed job's
        # reply is its error. The export leaves this app_wait out with the hole.
        try:
            running = client.call("app_wait", **{"for": "job", "job": started["job"], "timeout_s": 0}).get("status") == "running"
        except orca_mcp.McpError:
            running = False
        if during.get("left_out_running") or running or turns >= 40:
            break
        print(f"the hole's job had ended by the export, {time.time() - start:.1f} s after it began; trying {min(40, turns + 4)} turns",
              flush=True)
        try:
            client.await_job(started)
        except orca_mcp.McpError:
            pass
        turns = min(40, turns + 4)
    check("the missed hole replies with its running job", started.get("status") == "running",
          f"{started.get('status')} after {time.time() - start:.1f} s; the coil rebuilds too fast")
    check("an export while it runs leaves it out and names it",
          during.get("left_out_running") == [{"tool": "hole", "job": started.get("job")}],
          during.get("left_out_running") if running else
          f"{during.get('left_out_running')}; at {turns} turns the hole still ends before the export, so this machine is too fast or "
          "too loaded for the test")
    failure = None
    try:
        client.await_job(started)
    except orca_mcp.McpError as e:
        failure = str(e)
    check("the hole then fails as an error", failure and "removed no material" in failure,
          f"{failure} after {time.time() - start:.1f} s")
    cad(client, "feature_rename", feature=2, name="Coil")
    live_scene = cad(client, "describe_design")
    live_mass = cad(client, "body_mass_properties", body=0)

    after = client.call("replay_export", path=path, overwrite=True)
    # A Design command's call is journaled as its own.
    tools = [s["tool"] for s in after["replay"]["steps"]]
    check("the export leaves the failed hole out", "hole" not in tools, tools)
    check("and keeps the rename made after it", "feature_rename" in tools, tools)
    check("and leaves nothing out", "left_out_running" not in after, after.get("left_out_running"))

    # ---- The replay ----
    # A replayed build can run slower than the live one on a loaded machine, past the two minutes a replay
    # waits on a step's job by default, so the export is run with ten for each step.
    with open(path) as f:
        exported = json.load(f)
    for step in exported["steps"]:
        step["timeout_s"] = 600
    run_path = os.path.join(out_dir, "run.json")
    with open(run_path, "w") as f:
        json.dump(exported, f, indent=1)
    client.call("project_new", discard_changes=True)
    replay = client.await_job(client.call("replay_run", path=run_path, out_dir=out_dir), timeout=1800)
    check("the exported session replays", replay.get("passed") is True,
          json.dumps([s for s in replay.get("steps", []) if not s.get("ok")][:1]))
    scene = cad(client, "describe_design")
    mass = cad(client, "body_mass_properties", body=0)
    # Feature names count on across projects; the renamed one keeps its name.
    kinds = lambda s: [f["type"] for f in s["features"]]  # noqa: E731
    check("the same features, the coil renamed", kinds(scene) == kinds(live_scene) and scene["features"][2]["name"] == "Coil",
          (scene["features"], live_scene["features"]))
    check("the same coil", math.isclose(mass["volume"], live_mass["volume"], rel_tol=1e-6), (mass["volume"], live_mass["volume"]))

    print(f"replay and its run in {out_dir}")
    failed = [c for c in results if not c[1]]
    for name, ok, detail in results:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
