#!/usr/bin/env python3
"""The SELECT panel on the canvas: Faces / Edges / Vertices decide which kinds a click or hover may take, and "Click cycles candidates"
changes what re-clicking does. Real clicks on the panel's checkboxes; the effect is read from ui.candidates (it reports the filter and
the click mode). Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-select-panel.py [socket]
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


c = call("ui.inspect")["canvas"]
for _ in range(2):
    call("ui.click", at=[c["width"] - 30, 120]); time.sleep(0.3); call("ui.key", key="Escape"); time.sleep(0.3)
call("new_document")
call("extrude", width=40, height=30, distance=20)
call("screenshot", path="/tmp/ui-sel-0.png", view="iso", fit=True)
time.sleep(1.0)
call("ui.move", at=[c["width"] / 2, c["height"] / 2]); time.sleep(0.8)
call("ui.move", at=[c["width"] / 2 + 3, c["height"] / 2]); time.sleep(0.8)
hits = call("ui.inspect").get("select_panel", {}).get("checks", [])
check(len(hits) == 4, "the panel shows four switches (faces, edges, vertices, click cycling): %d" % len(hits))
if len(hits) != 4:
    print("select panel: FAIL"); sys.exit(1)


def filt():
    return call("ui.candidates", at=[c["width"] / 2, c["height"] / 2])


def click_box(i):
    call("ui.click", at=hits[i]); time.sleep(0.8)
    call("ui.move", at=[c["width"] / 2, c["height"] / 2]); time.sleep(0.5)


f = filt()
check(f["filter"] == {"face": True, "edge": True, "vertex": True} and f["cycle"] is False, "all kinds on, cycling off by default: %s cycle=%s" % (f["filter"], f["cycle"]))
click_box(2)
f = filt()
check(f["filter"]["vertex"] is False and f["filter"]["edge"] and f["filter"]["face"], "clicking Vertices turns vertex picking off: %s" % f["filter"])
click_box(2)
check(filt()["filter"]["vertex"] is True, "and on again")
click_box(1)
check(filt()["filter"]["edge"] is False, "Edges off")
click_box(1)
click_box(0)
check(filt()["filter"]["face"] is False, "Faces off")
click_box(0)
click_box(3)
check(filt()["cycle"] is True, "Click cycles candidates turns the cycling mode on")
click_box(3)
check(filt()["cycle"] is False, "and off")
print("select panel: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
