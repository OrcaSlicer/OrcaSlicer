#!/usr/bin/env python3
"""The sheet-metal card: base flange, edge flange (end edge picked with a real canvas click), hem, unfold and refold,
each built THROUGH THE CARD (ui.card: open, set, confirm) and compared with the analytic volume of the same part.
Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-sheet.py [socket]      # default /tmp/orca-cad-mcp.sock
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


def card(open_=None, **kw):
    r = call("ui.card", **({"open": open_} if open_ else {}), set=kw.pop("set", {}), confirm=kw.pop("confirm", True))
    time.sleep(0.5)
    return r


def show():
    call("screenshot", path="/tmp/ui-sheet.png", view="iso", fit=True)
    time.sleep(1.0)


def click_edge(topo_edge):
    show()
    p = [(a + b) / 2 for a, b in zip(topo_edge["p0"], topo_edge["p1"])]
    w = call("ui.worldToScreen", point=p)
    call("ui.click", at=[w["x"], w["y"]])
    time.sleep(0.6)


def edge_at(body, mid, tol=1e-3):
    for e in call("query_topology", body=body)["edges"]:
        if "mid" in e:
            continue
        m = [(a + b) / 2 for a, b in zip(e["p0"], e["p1"])]
        if math.dist(m, mid) < tol and e["kind"] == "line":
            return e
    return None


clear_canvas()
call("new_document")
mo = call("describe_scene").get("modeling_origin", [0, 0, 0])

# base flange: 100 x 60 plate, the rule thickness 2.5
call("sketch_begin", plane="XY")
call("sketch_add", entities=[{"type": "line", "p0": [-50, -30], "p1": [50, -30]}, {"type": "line", "p0": [50, -30], "p1": [50, 30]},
                             {"type": "line", "p0": [50, 30], "p1": [-50, 30]}, {"type": "line", "p0": [-50, 30], "p1": [-50, -30]}], infer=False)
call("sketch_commit")
r = card("sheet:1")
check(r["ok"], "base flange card confirmed")
check(near(vol(), 100 * 60 * 2.5, 1e-3), "base flange volume %.1f = 15000" % vol())

# the rule through the card: thickness 3 rebuilds the plate (the rule goes in front of the base: card appended at the end,
# so edit through MCP-free way is not needed here; a second document checks it below)

# edge flange: click the y-min top edge, 20 high, 90 deg
top_front = edge_at(0, [mo[0], mo[1] - 30, 2.5])
check(top_front is not None, "the plate's front top edge is in the topology")
click_edge(top_front)
r = card("sheet:2", set={"sheet_height": 20.0, "sheet_angle": 90.0})
check(r["ok"], "edge flange card confirmed")
section = 55 * 2.5 + math.pi / 4 * (25 - 6.25) + 15 * 2.5
check(near(vol(), 100 * section, 1e-3), "edge flange volume %.1f = %.1f" % (vol(), 100 * section))
n_features = len(call("describe_scene")["features"])

# hem on the flange top edge
topo = call("query_topology", body=0)
hem_edge = None
for e in topo["edges"]:
    if "mid" in e or e["kind"] != "line":
        continue
    if abs(e["p0"][2] - 20) < 1e-3 and abs(e["p1"][2] - 20) < 1e-3 and abs(e["p0"][1] - (mo[1] - 30)) < 1e-3:
        hem_edge = e
        break
check(hem_edge is not None, "the flange's top outer edge is in the topology")
v_before = vol()
click_edge(hem_edge)
r = card("sheet:4", set={"sheet_length": 8.0})
check(r["ok"], "hem card confirmed")
check(vol() > v_before + 100, "the hem added material (%.1f -> %.1f)" % (v_before, vol()))
call("undo")

# unfold, then refold through the cards
r = card("sheet:5")
check(r["ok"], "unfold card confirmed")
flat = call("query_topology", body=0)
zs = [v for e in flat["edges"] for v in (e["p0"][2], e["p1"][2])]
check(max(zs) - min(zs) < 2.6, "the unfolded sheet is flat (z extent %.2f)" % (max(zs) - min(zs)))
r = card("sheet:6")
check(r["ok"], "refold card confirmed")
check(near(vol(), 100 * section, 1e-3), "refolded volume %.1f = %.1f" % (vol(), 100 * section))

# the rule card sets the thickness for a sheet built after it
call("new_document")
call("sketch_begin", plane="XY")
call("sketch_add", entities=[{"type": "line", "p0": [-50, -30], "p1": [50, -30]}, {"type": "line", "p0": [50, -30], "p1": [50, 30]},
                             {"type": "line", "p0": [50, 30], "p1": [-50, 30]}, {"type": "line", "p0": [-50, 30], "p1": [-50, -30]}], infer=False)
call("sketch_commit")
r = card("sheet:0", set={"sheet_thickness": 1.0})
check(r["ok"], "rule card confirmed")
r = card("sheet:1")
check(near(vol(), 100 * 60 * 1.0, 1e-3), "base flange after a 1 mm rule: %.1f = 6000" % vol())

print("sheet card: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
