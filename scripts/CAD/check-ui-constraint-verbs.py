#!/usr/bin/env python3
"""Every sketch relation is a verb (con_<name>): with the selection already picked, running the verb applies it in one step - from the
palette, the ribbon, the context menu or run_verb alike. Here through run_verb on a live sketch; the result is read back from the
sketch. Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-constraint-verbs.py [socket]
"""
import json, math, socket, sys, time

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


call("screenshot", path="/tmp/ui-cv.png", view="iso", fit=True)
time.sleep(1.0)
c = call("ui.inspect")["canvas"]
for _ in range(3):
    call("ui.click", at=[c["width"] - 30, 120]); time.sleep(0.3); call("ui.key", key="Escape"); time.sleep(0.3)
call("new_document")
call("sketch_begin", plane="XY")
call("sketch_add", entities=[{"type": "line", "p0": [0, 0], "p1": [40, 6]}, {"type": "line", "p0": [0, 20], "p1": [30, 31]}], infer=False)


def lines():
    return [e for e in call("sketch_describe")["entities"] if e["type"] == "line"]


def ang(l):
    return math.degrees(math.atan2(l["p1"][1] - l["p0"][1], l["p1"][0] - l["p0"][0]))


def n_constraints():
    return call("sketch_describe")["constraints"]


L = lines()
check(abs(ang(L[0]) - ang(L[1])) > 1, "two lines that are not parallel: %.1f vs %.1f deg" % (ang(L[0]), ang(L[1])))
call("sketch_select", entities=[0, 1])
n0 = n_constraints()
r = call("run_verb", verb="con_parallel")
time.sleep(0.6)
L = lines()
check(abs(ang(L[0]) - ang(L[1])) < 1e-3 and n_constraints() == n0 + 1, "con_parallel made them parallel in one step: %.4f vs %.4f deg, constraints %d -> %d" % (ang(L[0]), ang(L[1]), n0, n_constraints()))

call("sketch_select", entities=[0])
r = call("run_verb", verb="con_horizontal")
time.sleep(0.6)
L = lines()
check(abs(L[0]["p1"][1] - L[0]["p0"][1]) < 1e-6, "con_horizontal made the selected line horizontal: dy = %.2e" % (L[0]["p1"][1] - L[0]["p0"][1]))
check(abs(ang(L[0]) - ang(L[1])) < 1e-3, "and the parallel line followed it")

# a relation the selection cannot take says so and changes nothing
n0 = n_constraints()
call("sketch_select", entities=[0])
r = call("run_verb", verb="con_tangent")
time.sleep(0.6)
check(n_constraints() == n0, "con_tangent on one line changes nothing (constraints %d)" % n_constraints())
call("sketch_cancel")

print("constraint verbs: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
