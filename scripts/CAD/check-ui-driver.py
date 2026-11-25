#!/usr/bin/env python3
"""The ui.* synthetic-input driver reaches the live widgets: a click on a part's face selects it, Escape clears it,
focus and ImGui state are observable. Run against a RUNNING app (ORCA_CAD_MCP=<sock>) with the Design tab visible.

    python3 scripts/CAD/check-ui-driver.py [socket]      # default /tmp/orca-cad-mcp.sock
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


call("new_document")
call("extrude", width=40, height=40, distance=20)
call("screenshot", path="/tmp/ui-driver-0.png", view="iso", fit=True)   # brings the Design tab forward and frames the part
time.sleep(0.5)
st = call("ui.inspect")
check(st["canvas"]["shown"] and st["canvas"]["width"] > 100, "the design canvas is visible (%s)" % st["canvas"])

org = call("describe_scene")["modeling_origin"]
top = call("ui.worldToScreen", point=[org[0], org[1], 20.0])
check(top["in_front"] and 0 < top["x"] < st["canvas"]["width"] and 0 < top["y"] < st["canvas"]["height"], "the top face centre projects inside the canvas (%.0f, %.0f)" % (top["x"], top["y"]))

before = call("ui.inspect")["selection_kind"]
call("ui.click", at=[top["x"], top["y"]])
after = call("ui.inspect")
check(after["selection_kind"] != before, "a click on the face changes the selection kind (%s -> %s)" % (before, after["selection_kind"]))
check(after["focus"]["is_canvas"] or after["imgui"] is not None, "focus is observable after the click: %s" % after["focus"])
call("screenshot", path="/tmp/ui-driver-1.png")

call("ui.key", key="Escape")
cleared = call("ui.inspect")["selection_kind"]
check(cleared == before, "Escape clears the selection (%s)" % cleared)

call("ui.drag", **{"from": [top["x"], top["y"]], "to": [top["x"] + 40, top["y"] + 20], "steps": 6, "button": 2})
call("ui.text", text="x")
check("result" in {"result": call("ui.inspect")}, "the app survives drag and text")
print("ui driver:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
