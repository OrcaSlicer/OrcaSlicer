#!/usr/bin/env python3
"""The on-canvas feature tree selects several rows (Ctrl toggles, Shift extends from the primary row) and its row commands act on all
of them: Copy / Paste make copies at the bar, Suppress and Delete take the whole selection. Real clicks (ui.click with modifiers); the
row commands go through ui.tree (the menu items are popups). Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-tree-multi.py [socket]
"""
import json, socket, sys, time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
_n = 0


def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(60)
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


def feats():
    return [(f["name"], f["enabled"]) for f in call("describe_scene")["features"]]


def tree(n):
    last = None
    for _ in range(20):
        t = call("ui.inspect").get("tree")
        if t and len(t["rows_y"]) == n and t == last:
            return t
        last = t
        time.sleep(0.5)
    return {"rows_y": [], "x": 0, "selected": []}


def click_row(i, mods=None, n=None):
    t = tree(n if n is not None else len(feats()))
    kw = {"mods": mods} if mods else {}
    call("ui.click", at=[t["x"] + 40, t["rows_y"][i]], **kw)
    time.sleep(0.8)
    return call("ui.inspect")["tree"]["selected"]


call("ui.option", name="imgui_tree", value=True) if False else call("ui_option", name="imgui_tree", value=True)
call("screenshot", path="/tmp/ui-multi.png", view="iso", fit=True)
time.sleep(1.0)
call("new_document")
for w in (20, 16, 12):
    call("extrude", width=w, height=w, distance=5, boolean="union")
n = len(feats())
check(n == 6, "six features (three sketch + extrude pairs): %d" % n)

sel = click_row(1)
check(sel == [1], "a plain click selects one row: %s" % sel)
sel = click_row(3, mods=["ctrl"])
check(sorted(sel) == [1, 3], "Ctrl+click adds a row: %s" % sel)
sel = click_row(1, mods=["ctrl"])
check(sorted(sel) == [3], "Ctrl+click on a selected row takes it out: %s" % sel)
sel = click_row(1)
sel = click_row(4, mods=["shift"])
check(sorted(sel) == [1, 2, 3, 4], "Shift+click extends from the primary row: %s" % sel)

# copy two rows (a sketch and its extrude) and paste: they land at the end, the extrude follows its own copy of the sketch
click_row(4)
sel = click_row(5, mods=["ctrl"])
call("ui.tree", event="copy", row=5)
call("ui.tree", event="paste", row=5)
time.sleep(0.8)
f = feats()
check(len(f) == n + 2, "paste added two features: %d -> %d" % (n, len(f)))
check(f[-1][0].endswith(" copy") and f[-2][0].endswith(" copy"), "the copies are named: %s" % [x[0] for x in f[-2:]])
scene = call("describe_scene")
check(len(scene["bodies"]) >= 1 and not call("inspect_design")["error"], "the document rebuilds with the copies")

# suppress two rows together
n = len(feats())
click_row(n - 2)
click_row(n - 1, mods=["ctrl"])
call("ui.tree", event="toggle", row=n - 1)
time.sleep(0.8)
f = feats()
check(not f[-1][1] and not f[-2][1], "Suppress takes the whole selection: %s" % [x[1] for x in f[-2:]])

# delete the pasted copies together
click_row(n - 2)
click_row(n - 1, mods=["ctrl"])
call("ui.tree", event="delete", row=n - 1)
time.sleep(1.0)
check(len(feats()) == n - 2, "Delete takes the whole selection: %d features left" % len(feats()))

print("tree multi-select: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
