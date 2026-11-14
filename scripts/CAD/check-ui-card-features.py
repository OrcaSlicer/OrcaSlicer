#!/usr/bin/env python3
"""The wx feature cards drive the same kernel features the MCP tools do: fillet size laws, thin extrude, several open faces
on Shell and Draft, feature patterns and mirror. Each case builds the part through the CARD (ui.card: open, set, confirm,
with the face/edge picked by a real canvas click) and compares the result with the analytic value or with the same part
built through MCP. Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-card-features.py [socket]      # default /tmp/orca-cad-mcp.sock
Exit 0 = every assertion held.
"""
import json, math, socket, sys, time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
_n = 0


def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(90)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": _n, "method": method, "params": params}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    r = json.loads(buf.decode())
    if "error" in r:
        raise RuntimeError("%s: %s" % (method, r["error"]))
    return r["result"]


fails = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        fails.append(msg)


def vol():
    return call("mass_properties", body=0)["volume"]


def near(a, b, rel=2e-3):
    return abs(a - b) <= rel * abs(b)


def clear_canvas():
    c = call("ui.inspect")["canvas"]
    for _ in range(3):
        call("ui.click", at=[c["width"] - 30, 120])
        time.sleep(0.3)
        call("ui.key", key="Escape")
        time.sleep(0.4)
    return c


def fresh():
    clear_canvas()
    call("new_document")


def show():
    call("screenshot", path="/tmp/ui-cardf.png", view="iso", fit=True)
    time.sleep(1.0)


def click_edge(topo_edge):
    p = [(a + b) / 2 for a, b in zip(topo_edge["p0"], topo_edge["p1"])]
    w = call("ui.worldToScreen", point=p)
    call("ui.click", at=[w["x"], w["y"]])
    time.sleep(0.6)


def top_front_edge(body=0):
    topo = call("query_topology", body=body)
    cands = [e for e in topo["edges"] if abs(e["p0"][1] - e["p1"][1]) < 1e-6 and abs(e["p0"][0] - e["p1"][0]) > 30
             and min(e["p0"][2], e["p1"][2]) > 19.9]
    return min(cands, key=lambda e: e["p0"][1])


# ---- fillet size laws ---------------------------------------------------------------------------------------------
fresh()
call("extrude", width=40, height=30, distance=20)
show()
click_edge(top_front_edge())
check(len(call("ui.inspect")["solid_selection"]["edges"]) == 1, "the top front edge is picked")
r = call("ui.card", open="dressup", set={"law": 1, "size": 2, "size2": 5}, confirm=True)
check(r["ok"], "the variable-radius fillet card confirms: %s" % r["card"])
want = 40 * 30 * 20 - (1 - math.pi / 4) * 40 * (2 * 2 + 2 * 5 + 5 * 5) / 3
check(near(vol(), want), "variable fillet 2 -> 5 removes the analytic volume: %.1f vs %.1f" % (vol(), want))

fresh()
call("extrude", width=40, height=30, distance=20)
show()
click_edge(top_front_edge())
r = call("ui.card", open="dressup", set={"law": 2, "size": 4}, confirm=True)
r2 = 2 * math.sqrt(2)
want = 40 * 30 * 20 - (1 - math.pi / 4) * r2 * r2 * 40
check(r["ok"] and near(vol(), want), "chord-length fillet 4 = radius 2*sqrt(2): %.1f vs %.1f" % (vol(), want))

# ---- thin extrude -------------------------------------------------------------------------------------------------
fresh()
call("sketch_begin", plane="XY")
call("sketch_add", rect=[0, 0, 40, 40])
call("sketch_commit")
r = call("ui.card", open="extrude", set={"distance": 10, "thin_in": 3, "thin_out": 1}, confirm=True)
check(r["ok"] and near(vol(), (42 * 42 - 34 * 34) * 10.0), "thin extrude 3 in / 1 out is a wall around the outline: %.1f" % vol())

# ---- shell / draft with several faces -----------------------------------------------------------------------------
def box_two_faces():
    call("extrude", width=50, height=40, distance=30)
    show()
    c = call("ui.inspect")["canvas"]
    mid = [c["width"] / 2, c["height"] / 2]
    org = call("describe_scene")["modeling_origin"]
    side = None
    for dx, dy in ((25, 0), (-25, 0), (0, 20), (0, -20)):
        w = call("ui.worldToScreen", point=[org[0] + dx, org[1] + dy, 15])
        cand = call("ui.candidates", at=[w["x"], w["y"]])["candidates"]
        if cand and cand[0]["kind"] == "face" and cand[0]["face"] != 5:
            side = [w["x"], w["y"]]
            break
    return mid, side


fresh()
mid, side = box_two_faces()
check(side is not None, "a visible side face to open")
cand = call("ui.candidates", at=side)["candidates"][0]
nrm = [f for f in call("query_topology", body=0)["faces"] if f["id"] == cand["face"]][0]["normal"]
call("ui.click", at=mid)                                    # the top face
time.sleep(0.5)
call("ui.card", open="shell", set={"shell_thickness": 3})
call("ui.card", set={"add_shell_face": 1})                   # the top face goes on the list
call("ui.click", at=side)                                    # the side face is the live pick
time.sleep(0.5)
r = call("ui.card", confirm=True)
along_x = abs(nrm[0]) > 0.5
cavity = (47 * 34 * 27) if along_x else (44 * 37 * 27)
check(r["ok"] and near(vol(), 50 * 40 * 30 - cavity), "shell with the top and a side face open leaves the analytic wall volume: %.1f vs %.1f" % (vol(), 50 * 40 * 30 - cavity))

fresh()
mid, side = box_two_faces()
call("ui.click", at=side)
time.sleep(0.5)
call("ui.card", open="draft", set={"draft_angle": 5})
r = call("ui.card", confirm=True)
check(r["ok"], "the draft card confirms over a picked side face: %s" % r["card"])

# ---- feature pattern and mirror from the card ---------------------------------------------------------------------
fresh()
call("extrude", width=60, height=40, distance=5)
call("sketch_begin", plane="XY")
call("sketch_add", type="circle", center=[-20, 0], radius=2)
si = call("sketch_commit")["sketch_index"]
call("extrude", sketch=si, distance=20, boolean="subtract", end="through_all")
n_feat = len(call("describe_scene")["features"])
hole_feature = n_feat - 1
v0 = vol()
r = call("ui.card", open="pattern", set={"pattern_what": 1, "pattern_type": 0, "pattern_axis": 0, "count": 3, "spacing": 15}, check=[hole_feature], confirm=True)
one_hole = math.pi * 4 * 5
check(r["ok"] and near(vol(), v0 - 2 * one_hole), "a 3-hole row from the Pattern card: %.1f vs %.1f" % (vol(), v0 - 2 * one_hole))

fresh()
call("extrude", width=60, height=40, distance=5)
call("sketch_begin", plane="XY")
call("sketch_add", type="circle", center=[-20, 0], radius=2)
si = call("sketch_commit")["sketch_index"]
call("extrude", sketch=si, distance=20, boolean="subtract", end="through_all")
hole_feature = len(call("describe_scene")["features"]) - 1
v0 = vol()
r = call("ui.card", open="pattern", set={"pattern_what": 1, "pattern_type": 2, "pattern_axis": 0}, check=[hole_feature], confirm=True)
check(r["ok"] and near(vol(), v0 - one_hole), "mirroring the hole across the YZ plane from the Pattern card: %.1f vs %.1f" % (vol(), v0 - one_hole))

print("ui card features:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
