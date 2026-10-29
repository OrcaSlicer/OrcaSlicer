#!/usr/bin/env python3
"""Checks that a Design-tab session exported with export_script replays to the same part.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_replay.py

Records a session of cad_call client calls (script steps are not journaled) on the paths a replay
breaks on: a sketch on a base plane, which passes through modeling_origin as every plane does; an
empty sketch commit, which fails and must keep the sketch open for the next call; a rewritten
method failing as an error (a hole that misses), which export_script must leave out; a legacy
method answering ok false, which stays in and replays the same rejection; and face and edge ids
passed with the generation they were read at, which the journal drops, since the replay reaches
other generation numbers. The thread rebuilds behind the progress dialog. The exported script is
replayed into a new project in the same instance, whose generation count has moved on, and the
body's mass properties and the feature list must match. Exits 0 when all hold.
"""

import json
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def cad(client, method, **params):
    return client.call("cad_call", _timeout=300, method=method, params=params)["result"]


def refused(client, method, **params):
    """The error text of a call that must fail, or None when it succeeded."""
    try:
        cad(client, method, **params)
    except orca_mcp.McpError as e:
        return str(e)
    return None


def record(client):
    """The session to export; returns the reads the replay must match and the refusals seen."""
    client.call("export_script", clear=True)
    client.call("new_project", discard_changes=True)
    cad(client, "new_design", discard_changes=True)
    frame = cad(client, "sketch_begin", plane="XY")["frame"]
    empty_commit = refused(client, "sketch_commit")
    # A 40 x 30 plate with an M5 tap-drill bore (minor diameter 4.134), 12 thick.
    cad(client, "sketch_add", rect=[-20, -15, 20, 15])
    cad(client, "sketch_add", entities=[{"type": "circle", "center": [0, 0], "radius": 2.067}])
    cad(client, "sketch_commit")
    cad(client, "extrude", sketch=0, distance=12)
    missed_hole = refused(client, "hole", x=500, through=True)
    corners = cad(client, "query_topology", body=0, edge_kinds=["line"], direction=[0, 0, 1], convex=True)
    cad(client, "fillet", edge=[e["id"] for e in corners["edges"]], radius=3, body=0,
        generation=corners["generation"])
    legacy = cad(client, "fillet", edge=corners["edges"][0]["id"], radius=50, body=0)
    bores = cad(client, "query_topology", body=0, kinds=["cylindrical"])
    bore = next(f["id"] for f in bores["faces"] if f.get("internal"))
    thread = client.await_job(client.call("cad_call", _timeout=300, method="thread",
                                          params={"body": 0, "face": bore, "generation": bores["generation"]}))
    return {"mass": cad(client, "mass_properties", body=0), "scene": cad(client, "describe_scene"),
            "frame": frame, "empty_commit": empty_commit, "missed_hole": missed_hole, "legacy": legacy,
            "thread": thread.get("result", {})}


def same(a, b, rel=1e-6):
    return math.isclose(a, b, rel_tol=rel, abs_tol=1e-6)


def main():
    client, _ = orca_mcp.session_client()
    live = record(client)
    out_dir = tempfile.mkdtemp(prefix="orca-mcp-cad-replay-")
    path = os.path.join(out_dir, "session.json")
    client.call("export_script", path=path, overwrite=True)
    with open(path) as f:
        text = f.read()
    steps = json.loads("\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//")))["steps"]
    calls = [s["args"] for s in steps if s["tool"] == "cad_call"]
    methods = [c["method"] for c in calls]

    client.call("new_project", discard_changes=True)
    replay = client.await_job(client.call("run_script", path=path, out_dir=out_dir), timeout=900)
    mass = cad(client, "mass_properties", body=0)
    scene = cad(client, "describe_scene")
    kinds = lambda s: [f["type"] for f in s["features"]]  # noqa: E731 (names count on across projects)

    origin = live["scene"]["modeling_origin"]
    checks = [
        ("a sketch on XY and the plate are centred on modeling_origin",
         live["frame"]["origin"] == origin and all(same(a, b) for a, b in zip(live["mass"]["center_of_mass"][:2], origin[:2])),
         (live["frame"]["origin"], live["mass"]["center_of_mass"], origin)),
        ("an empty sketch_commit fails", bool(live["empty_commit"]), live["empty_commit"]),
        ("a hole that misses fails as an error", bool(live["missed_hole"]), live["missed_hole"]),
        ("a legacy fillet the kernel refuses answers ok false", live["legacy"].get("ok") is False, live["legacy"]),
        ("the thread is M5, inferred from the bore", live["thread"].get("designation") == "M5", live["thread"]),
        ("the export leaves out the failed calls", methods.count("sketch_commit") == 1 and "hole" not in methods, methods),
        ("the export keeps the legacy ok-false call", methods.count("fillet") == 2, methods),
        ("the export records no generation", all("generation" not in c.get("params", {}) for c in calls), calls),
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
    ]
    print(f"volume {live['mass']['volume']:.3f} mm3; script and replay in {out_dir}")
    failures = 0
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
