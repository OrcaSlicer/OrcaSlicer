#!/usr/bin/env python3
"""Feature cards: a selection made BEFORE the card opens is the card's input, and the check mark acts only while the
preview is valid. Drives the real keyboard path (ui.key) against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-card.py [socket]      # default /tmp/orca-cad-mcp.sock
Exit 0 = every assertion held.
"""
import json, socket, sys, time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
SHELL, DRESSUP, DRAFT = 6, 3, 12     # DesignPanel::Tool
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


def card():
    time.sleep(0.8)
    return call("ui.inspect")["card"]


try:
    call("sketch_cancel")
except RuntimeError:
    pass
call("new_document")
call("extrude", width=40, height=40, distance=10)
call("screenshot", path="/tmp/ui-card-0.png", view="iso", fit=True)
time.sleep(0.8)
c = call("ui.inspect")["canvas"]
mid = [c["width"] / 2, c["height"] / 2]
empty = [c["width"] - 30, 120]


def close_card():
    call("ui.key", key="Escape")
    time.sleep(0.4)


# a face picked first becomes the Shell card's open face
call("ui.click", at=mid)
check(len(call("ui.inspect")["solid_selection"]["faces"]) == 1, "a click picked a face")
call("ui.key", key="K", mods=["shift"])
k = card()
check(k["tool"] == SHELL, "Shift+K opens the Shell card: %s" % k)
check(k["confirm"], "with a face preselected the check mark is live: %s" % k["status"])

close_card()
check(card()["tool"] == 0, "Esc closes the card")

# a SIDE face: the middle of the view lands on the top face, which a draft cannot taper
org = call("describe_scene")["modeling_origin"]
side = None
for dx, dy in ((20, 0), (-20, 0), (0, 20), (0, -20)):
    w = call("ui.worldToScreen", point=[org[0] + dx, org[1] + dy, 5])
    cand = call("ui.candidates", at=[w["x"], w["y"]])["candidates"]
    if cand and cand[0]["kind"] == "face" and cand[0]["face"] != 5:
        side = [w["x"], w["y"]]
        break
check(side is not None, "found a visible side face")

# Draft needs a face: with none picked the check mark is greyed and the status says why
call("ui.click", at=empty)
check(call("ui.inspect")["solid_selection"]["faces"] == [], "selection cleared")
call("ui.key", key="D", mods=["shift"])
k = card()
check(k["tool"] == DRAFT, "Shift+D opens the Draft card: %s" % k)
check(not k["confirm"], "no face picked: the check mark is greyed: %s" % k["status"])
call("ui.click", at=side)                      # a pick with the card open feeds it
k = card()
check(k["tool"] == DRAFT and k["confirm"], "a side face picked with the card open makes it valid: %s" % k)
close_card()

# the same face picked BEFORE the card opens
call("ui.click", at=empty)
call("ui.click", at=side)
call("ui.key", key="D", mods=["shift"])
k = card()
check(k["tool"] == DRAFT and k["confirm"], "Draft opened over a preselected side face is valid at once: %s" % k)
close_card()

print("ui card:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
