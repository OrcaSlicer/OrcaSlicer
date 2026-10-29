#!/usr/bin/env python3
"""The Ctrl+K command palette, and the focus invariant behind it: typing into an ImGui field never also fires a
shortcut. Drives the real event path (ui.key / ui.text / ui.click) against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-palette.py [socket]      # default /tmp/orca-cad-mcp.sock
Exit 0 = every assertion held.
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


def pal():
    time.sleep(0.4)   # a frame or two: ImGui state lands after the next paint
    return call("ui.inspect")["palette"]


try:
    call("sketch_cancel")
except RuntimeError:
    pass
call("new_document")
call("extrude", width=40, height=40, distance=10)
call("screenshot", path="/tmp/ui-pal-0.png", view="iso", fit=True)   # brings the Design tab forward
time.sleep(0.8)
c = call("ui.inspect")["canvas"]
call("ui.click", at=[c["width"] / 2, c["height"] - 40])   # empty canvas: focus on the GL canvas, nothing picked
time.sleep(0.3)
check(not pal()["open"], "the palette starts closed")

call("ui.key", key="K", mods=["ctrl"])
check(pal()["open"], "Ctrl+K opens it")
check(len(pal()["shown"]) > 0, "with an empty query it lists commands: %s" % pal()["shown"][:4])

# typing goes to the box and ONLY to the box
before = call("describe_scene")
call("ui.text", text="xaf")                          # x, a, f are all model-mode shortcuts
st = pal()
check(st["query"] == "xaf", "the typed text reached the box: %r" % st["query"])
check(call("describe_scene") == before, "typing did not fire a shortcut (document unchanged)")
call("ui.key", key="Escape")
check(not pal()["open"], "Esc closes it")

call("ui.key", key="K", mods=["ctrl"])
pal()
# the label is translated (the rig runs Italian): type the first letters of what the box itself shows for Extrude
call("ui.text", text="e")
lab = pal()["labels"].get("extrude")
call("ui.key", key="Backspace")
check(lab is not None, "Extrude is listed as %r" % lab)
call("ui.text", text=(lab or "extr")[:3])
st = pal()
check(len(st["shown"]) > 0 and st["shown"][0] in ("extrude", "surf_extrude"), "'extr' ranks Extrude first: %s" % st["shown"])
check(st["shown"].index("extrude") < st["shown"].index("surf_extrude") if "surf_extrude" in st["shown"] and "extrude" in st["shown"] else True,
      "prefix beats word-start")
call("ui.key", key="Down")
check(pal()["cursor"] == min(1, len(pal()["shown"]) - 1), "Down moves the highlight")
call("ui.key", key="Up")
call("ui.key", key="Up")
check(pal()["cursor"] == 0, "Up clamps at the top")
call("ui.key", key="Escape")

# the focus invariant: after the palette closes, shortcuts work again and the next open still takes text
call("ui.key", key="K", mods=["ctrl"])
pal()
call("ui.text", text="hol")
check(pal()["query"] == "hol", "the box takes text again after being closed and reopened")
call("ui.key", key="Escape")
check(not call("ui.inspect")["imgui"]["want_text_input"], "closing it releases the keyboard to the shortcuts")

# Enter runs the highlighted command and closes the box; the run is recorded as a recent
call("ui.key", key="K", mods=["ctrl"])
pal()
call("ui.text", text="s")
lab = pal()["labels"].get("sketch")
call("ui.key", key="Backspace")
call("ui.text", text=(lab or "sket")[:3])
check(pal()["shown"][:1] == ["sketch"], "'%s' puts Sketch first: %s" % ((lab or "sket")[:3], pal()["shown"][:3]))
call("ui.key", key="Enter")
check(not pal()["open"], "Enter runs the highlighted command and closes the box")
call("ui.key", key="Escape")      # leave whatever the command opened
call("ui.key", key="Escape")
call("ui.key", key="K", mods=["ctrl"])
check(pal()["shown"][:1] == ["sketch"], "and Sketch is now the first recent: %s" % pal()["shown"][:3])
call("ui.key", key="Escape")

print("ui palette:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
