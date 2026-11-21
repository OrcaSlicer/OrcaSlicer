#!/usr/bin/env python3
"""While a line is drawn the HUD names what the cursor is locked onto: [//] parallel, [T] perpendicular (as the perpendicular sign),
[End] / [Mid] / [On] / [Centre] / [Origin] for a snap target; nothing when the cursor is free. Real mouse moves in a live sketch.
Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-guide-tags.py [socket]
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


try:
    call("sketch_cancel")
except RuntimeError:
    pass
call("new_document")
call("extrude", width=40, height=40, distance=10)
call("screenshot", path="/tmp/ui-tags-0.png", view="iso", fit=True)
time.sleep(0.5)
org = call("describe_scene")["modeling_origin"]
top = call("ui.worldToScreen", point=[org[0], org[1], 10.0])
call("ui.click", at=[top["x"], top["y"]])
call("run_verb", verb="sketch")
time.sleep(0.5)
c = call("ui.inspect")["canvas"]
cx, cy = c["width"] / 2, c["height"] / 2


def hud(at, mods=None):
    kw = {"mods": mods} if mods else {}
    call("ui.move", at=at, **kw)
    time.sleep(0.5)
    call("ui.move", at=[at[0] + 1, at[1]], **kw)   # a second move: the frame that draws the HUD has seen the pointer
    time.sleep(0.6)
    return call("ui.inspect").get("hud", "")


# a reference line, drawn at about 20 degrees; its start is where the snap targets are
call("ui.key", key="L")
a0, a1 = [cx - 320, cy + 200], [cx - 120, cy + 120]   # left of the centre: the tool offer that opens on entry sits right of it
# Shift bypasses the grid: the ends stay where they were clicked, whatever the zoom of this canvas puts the grid dots at
call("ui.move", at=a0, mods=["shift"]); call("ui.click", at=a0, mods=["shift"]); time.sleep(0.5)
call("ui.move", at=a1, mods=["shift"]); call("ui.click", at=a1, mods=["shift"]); time.sleep(0.5)
call("ui.key", key="Escape")
call("ui.key", key="L")

# the free cursor in the second line's first point: no tag
call("ui.move", at=[cx + 150, cy + 250])
call("ui.click", at=[cx + 150, cy + 250]); time.sleep(0.5)
h = hud([cx + 330, cy + 190])
print("info free cursor HUD: %r" % h)

# 3 degrees off parallel to the reference line: the parallel tag
h = hud([cx + 350, cy + 250 - int(0.365 * 200) - 8])
check("//" in h, "near parallel to the reference line the HUD says [//]: %r" % h)

# Ctrl switches inference off: no tag
h = hud([cx + 350, cy + 250 - int(0.365 * 200) - 8], mods=["ctrl"])
check("//" not in h, "with Ctrl held there is no guide tag: %r" % h)
call("ui.key", key="Escape")
call("ui.key", key="Escape")

# the first point of a new line near the end of the reference line: End
call("ui.key", key="L")
h = hud([a1[0] + 3, a1[1] + 2])
check("[End]" in h or "[Fine]" in h, "over the end of the reference line the HUD says End (or its Italian, \"Fine\"): %r" % h)
h = hud([(a0[0] + a1[0]) // 2 + 2, (a0[1] + a1[1]) // 2 + 1])
check("[Mid]" in h or "[Metà]" in h or "[Punto medio]" in h, "over its middle the HUD says Mid: %r" % h)
call("ui.key", key="Escape")
call("ui.key", key="Escape")
call("sketch_cancel")
print("guide tags: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
