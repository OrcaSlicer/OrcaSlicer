#!/usr/bin/env python3
"""Undo is one step per committed command, and nothing else is a step: a failed command leaves the document and the undo depth
untouched, selection / camera / the rollback bar / opening and cancelling a card are not steps, and undo + redo round-trip.
Reads `undo_depth` from inspect_design. Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-undo.py [socket]
"""
import json, socket, sys, time

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


def depth():
    return call("inspect_design")["undo_depth"]


def nfeat():
    return len(call("describe_scene")["features"])


def show():
    call("screenshot", path="/tmp/ui-undo.png", view="iso", fit=True)
    time.sleep(1.0)


def clear_canvas():
    c = call("ui.inspect")["canvas"]
    for _ in range(3):
        call("ui.click", at=[c["width"] - 30, 120])
        time.sleep(0.3)
        call("ui.key", key="Escape")
        time.sleep(0.4)


show()
clear_canvas()
call("new_document")


def step(name, fn, want, features=None):
    d0, f0 = depth(), nfeat()
    try:
        fn()
    except RuntimeError:
        pass
    d1, f1 = depth(), nfeat()
    check(d1 - d0 == want, "%s: %+d undo step(s) (wanted %+d)" % (name, d1 - d0, want))
    if features is not None:
        check(f1 - f0 == features, "%s: %+d feature(s) (wanted %+d)" % (name, f1 - f0, features))


def sketch_and_commit():
    call("sketch_begin", plane="XY")
    call("sketch_add", rect=[-20, -15, 20, 15])
    call("sketch_commit")


step("sketch (begin, add, commit)", sketch_and_commit, 1, features=1)
step("extrude", lambda: call("extrude", sketch=0, distance=10), 1, features=1)
step("a failing fillet changes nothing", lambda: call("fillet", edge=[0], radius=500), 0, features=0)
step("a failing extrude changes nothing", lambda: call("extrude", sketch=99, distance=10), 0, features=0)
step("set_variable", lambda: call("set_variable", name="w", expr="12"), 1)
step("a bad expression changes nothing", lambda: call("set_variable", name="bad", expr="1 +* 2"), 0)
step("rollback bar", lambda: call("rollback", index=1), 0)
step("rollback restored", lambda: call("rollback", index=-1), 0)
step("selection clicks", lambda: (call("ui.click", at=[call("ui.inspect")["canvas"]["width"] / 2, call("ui.inspect")["canvas"]["height"] / 2]), time.sleep(0.4)), 0)
clear_canvas()
step("opening a card and cancelling it", lambda: (call("ui.card", open="shell", set={"shell_thickness": 2}), clear_canvas()), 0)
step("a card confirmed", lambda: call("ui.card", open="extrude", set={"distance": 5}, confirm=True), 1, features=1)

# undo and redo walk back and forth over exactly those steps
n_f = nfeat()
d = depth()
call("undo")
check(depth() == d - 1 and nfeat() == n_f - 1, "undo removes the last committed command (depth %d -> %d, features %d -> %d)" % (d, depth(), n_f, nfeat()))
call("redo")
check(depth() == d and nfeat() == n_f, "redo puts it back")
r = call("undo")
call("undo")
call("redo")
call("redo")
check(depth() == d and nfeat() == n_f, "two undos and two redos return to the same state")

# a batch is one step per command it ran (it is a script, not a transaction on the stack)
d0 = depth()
call("batch", commands=[{"method": "extrude", "params": {"width": 10, "height": 10, "distance": 3}},
                        {"method": "extrude", "params": {"width": 8, "height": 8, "distance": 3, "boolean": "union"}}])
print("info batch of 2 extrudes (4 features) = %+d undo step(s)" % (depth() - d0))
d1 = depth()
r = call("batch", commands=[{"method": "extrude", "params": {"width": 10, "height": 10, "distance": 3}},
                            {"method": "fillet", "params": {"edge": [0], "radius": 500}}], rollback=True)
check(depth() == d1 and not r.get("ok"), "a batch that fails and rolls back leaves no undo steps behind (%+d)" % (depth() - d1))

print("undo: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
