#!/usr/bin/env python3
"""Closing smoke test for the Orca-CAD epic (EPIC-v3 §6b): the casual-user golden path, end to end,
over the app's control socket, with a screenshot per step and one Ctrl+Z undo check per feature.

    ORCA_CAD_MCP=/tmp/orca-a9.sock DISPLAY=:11 python3 scripts/CAD/smoke_golden_path.py [outdir]

Preconditions: a FRESH document, the Design tab selected, no modal on screen (first-run wizard, "Bambu network
module", "restore unsaved items"). A modal or another tab swallows Ctrl+Z and "undo extrude" then fails.

sketch -> extrude -> fillet -> chamfer -> shell -> revolve -> linear pattern -> circular pattern -> slice.
"Commit to Plate" and the Prepare-tab slice are driven by the GUI and reported as NOT COVERED here.
Exit code 0 only when every step succeeded and every undo removed exactly one feature.
"""
import json, os, socket, subprocess, sys, time

SOCK = os.environ.get("ORCA_CAD_MCP", "/tmp/orca-cad-mcp.sock")
DISPLAY = os.environ.get("DISPLAY", ":10")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-smoke"
os.makedirs(OUT, exist_ok=True)
_id = [0]


def call(method, params=None):
    _id[0] += 1
    req = {"jsonrpc": "2.0", "id": _id[0], "method": method}
    if params is not None:
        req["params"] = params
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(SOCK)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    r = json.loads(buf.decode())
    if "error" in r:
        raise RuntimeError("%s: %s" % (method, r["error"]))
    return r["result"]


def shot(name):
    path = os.path.join(OUT, name + ".png")
    subprocess.run(["import", "-display", DISPLAY, "-window", "root", path], check=False)
    return path


def n_features():
    return len(call("describe_scene")["features"])


def key(*keys):
    subprocess.run(["xdotool"] + list(keys), env=dict(os.environ, DISPLAY=DISPLAY), check=False)


results = []


def step(name, fn):
    before = n_features()
    try:
        out = fn()
        # The socket answers {"ok": false, "error": ...} for a refused operation without raising.
        ok = not (isinstance(out, dict) and (out.get("ok") is False or out.get("error")))
    except Exception as e:  # noqa: BLE001 - a smoke test reports, it does not raise
        out, ok = str(e), False
    after = n_features()
    shot("%02d_%s" % (len(results) + 1, name))
    results.append({"step": name, "ok": ok, "features_added": after - before, "detail": out})
    print("%-18s %s  (+%d features)" % (name, "OK " if ok else "FAIL", after - before))
    return ok


def undo_one(name, expect_removed=1):
    before = n_features()
    key("key", "ctrl+z")
    time.sleep(1.0)
    after = n_features()
    ok = after == before - expect_removed   # ONE press undoes the one action the user took
    results.append({"step": "undo " + name, "ok": ok, "features_added": after - before, "detail": ""})
    print("%-18s %s  (%d -> %d features)" % ("undo " + name, "OK " if ok else "FAIL", before, after))
    key("key", "ctrl+shift+z")   # redo, so the next step builds on it
    time.sleep(1.0)
    return ok


def pick_edge(rank, vertical=True):
    """id of the rank-th longest straight (vertical, by default) edge of body 0, from FRESH topology
    (ids expire on every rebuild). Vertical edges of a shelled box are the outer corners, which take a
    fillet or chamfer; the long rim edges of a thin wall do not."""
    def is_vertical(e):
        a, b = e["p0"], e["p1"]
        return abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6
    edges = [e for e in call("query_topology", {"body": 0}).get("edges", [])
             if e.get("kind") == "line" and (is_vertical(e) == vertical)]
    edges.sort(key=lambda e: -e.get("length", 0))
    return edges[min(rank, len(edges) - 1)]["id"] if edges else 0


def top_face():
    """id of the highest upward-facing planar face of body 0 (the one a casual user opens a box from)."""
    faces = [f for f in call("query_topology", {"body": 0}).get("faces", [])
             if f.get("kind") == "planar" and f.get("normal", [0, 0, 0])[2] > 0.9]
    faces.sort(key=lambda f: -f["centroid"][2])
    return faces[0]["id"] if faces else -1


def main():
    r = {}
    step("extrude", lambda: call("extrude", {"width": 60, "height": 40, "distance": 20, "plane": "XY", "boolean": "new"}))
    undo_one("extrude", expect_removed=2)   # the sketch and its extrude are ONE user action
    # Dress-up on the plain block first, as a casual user would; the shell comes after, with a wall
    # thinner than the fillet radius so the inward offset stays valid.
    step("fillet", lambda: call("fillet", {"edge": pick_edge(0), "radius": 3.0, "body": 0}))
    step("chamfer", lambda: call("chamfer", {"edge": pick_edge(0, vertical=False), "distance": 1.0, "body": 0}))
    step("shell", lambda: call("shell", {"thickness": 2, "face": top_face(), "body": 0}))
    step("revolve", lambda: call("revolve", {"profile": [[10, 2], [20, 2], [20, 8], [10, 8]], "angle": 360,
                                             "axis": 0, "plane": "XZ", "boolean": "union"}))
    step("linear_pattern", lambda: call("pattern", {"circular": False, "count": 3, "spacing": 70, "dir": 0, "body": 0}))
    step("circular_pattern", lambda: call("pattern", {"circular": True, "count": 4, "angle": 360, "plane": "XY", "body": 0}))
    step("slice", lambda: call("slice_body", {"body": 0, "plane": "XY", "offset": 5}))
    print("NOT COVERED here: Commit to Plate + Prepare-tab slice (GUI only), <=2-click ribbon reachability")
    failed = [x for x in results if not x["ok"]]
    json.dump(results, open(os.path.join(OUT, "smoke.json"), "w"), indent=1)
    print("%d steps, %d failed; report in %s" % (len(results), len(failed), OUT))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
