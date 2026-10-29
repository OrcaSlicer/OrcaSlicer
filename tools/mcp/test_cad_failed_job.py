#!/usr/bin/env python3
"""Checks that a Design-tab call whose job fails after its reply is left out of export_script.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_failed_job.py

A coil swept along a helix makes every rebuild of the design slow, so a hole that misses it, which
the kernel refuses only after the rebuild and takes back with another, replies with a running job
past the 45 s budget and then fails. An export while it runs leaves it out and names it in
left_out_running; once it has failed, a rename made after it is in the export, the hole is not, and
the script replays into a new project to the same features and coil. The coil's turns are sized
from a short calibration build, taking a build's time to grow with the cube of the turns, so the
failing call outlives the budget on a faster or slower machine; a busy machine bends that curve, so
when the hole still fails inside the budget the session is made again with a bigger coil. Takes four
to five minutes. Exits 0 when every check passes.
"""

import json
import math
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

# The coil's build time to aim for. The missed hole takes about as long as the build, so this is well
# past the reply budget.
REBUILD_S = 60.0


def cad(client, method, **params):
    return client.call("cad_call", _timeout=600, method=method, params=params)


def coil(client, turns):
    """A new design holding one coil: a 1.2 mm wire swept along a helix of radius 10 and pitch 3."""
    cad(client, "new_design", discard_changes=True)
    cad(client, "helix", radius=10, pitch=3, height=3 * turns)
    cad(client, "sketch_begin", plane="XZ")
    cad(client, "sketch_add", type="circle", center=[10, 0], radius=1.2)
    cad(client, "sketch_commit")
    start = time.time()
    client.await_job(cad(client, "sweep", sketch=1, path=0))
    return time.time() - start


def main():
    client, _ = orca_mcp.session_client()
    out_dir = tempfile.mkdtemp(prefix="orca-mcp-cad-failed-job-")
    path = os.path.join(out_dir, "session.json")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    client.call("new_project", discard_changes=True)
    calibration = coil(client, 12)
    turns = max(12, min(40, round(12 * (REBUILD_S / max(calibration, 0.5)) ** (1 / 3))))
    print(f"12 turns built in {calibration:.1f} s; the test coil has {turns}", flush=True)

    # ---- The session to export ----
    while True:
        client.call("export_script", clear=True)
        client.call("new_project", discard_changes=True)
        coil(client, turns)
        start = time.time()
        try:
            started = cad(client, "hole", x=500, through=True)
            break
        except orca_mcp.McpError as e:
            if turns >= 40 or "removed no material" not in str(e):
                raise
            print(f"the hole failed inside the budget, in {time.time() - start:.1f} s; trying {min(40, turns + 4)} turns",
                  flush=True)
            turns = min(40, turns + 4)
    check("the missed hole replies with its running job", started.get("status") == "running",
          f"{started.get('status')} after {time.time() - start:.1f} s; the coil rebuilds too fast")
    during = client.call("export_script")
    check("an export while it runs leaves it out and names it",
          during.get("left_out_running") == [{"tool": "cad_call", "job": started.get("job")}], during.get("left_out_running"))
    failure = None
    try:
        client.await_job(started)
    except orca_mcp.McpError as e:
        failure = str(e)
    check("the hole then fails as an error", failure and "removed no material" in failure,
          f"{failure} after {time.time() - start:.1f} s")
    cad(client, "feature_rename", feature=2, name="Coil")
    live_scene = cad(client, "describe_scene")["result"]
    live_mass = cad(client, "mass_properties", body=0)["result"]

    after = client.call("export_script", path=path, overwrite=True)
    calls = [s["args"] for s in after["script"]["steps"] if s["tool"] == "cad_call"]
    methods = [c["method"] for c in calls]
    check("the export leaves the failed hole out", "hole" not in methods, methods)
    check("and keeps the rename made after it", "feature_rename" in methods, methods)
    check("and leaves nothing out", "left_out_running" not in after, after.get("left_out_running"))

    # ---- The replay ----
    client.call("new_project", discard_changes=True)
    replay = client.await_job(client.call("run_script", path=path, out_dir=out_dir), timeout=900)
    check("the exported session replays", replay.get("passed") is True,
          json.dumps([s for s in replay.get("steps", []) if not s.get("ok")][:1]))
    scene = cad(client, "describe_scene")["result"]
    mass = cad(client, "mass_properties", body=0)["result"]
    # Feature names count on across projects; the renamed one keeps its name.
    kinds = lambda s: [f["type"] for f in s["features"]]  # noqa: E731
    check("the same features, the coil renamed", kinds(scene) == kinds(live_scene) and scene["features"][2]["name"] == "Coil",
          (scene["features"], live_scene["features"]))
    check("the same coil", math.isclose(mass["volume"], live_mass["volume"], rel_tol=1e-6), (mass["volume"], live_mass["volume"]))

    print(f"script and replay in {out_dir}")
    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
