#!/usr/bin/env python3
"""Autonomous 2D-sketch loop: drive the Design tab's sketch layer over the MCP socket and
assert the things that decide whether a profile is buildable.

WHY THIS EXISTS. The 2D layer used to be reachable only by clicking, so every question about it
("is this loop closed?", "did the offset survive?", "is the circle a void or a second body?")
cost a GUI session and a human. The socket verbs make each one a call, and this script is the
loop: build a known profile, ask the app what it thinks it has, compare against arithmetic.

RUN IT AGAINST A RUNNING APP:
    SNAPORCA_MCP=/tmp/mcp.sock <binary>          # launch with the socket enabled
    python3 scripts/mcp-sketch-smoke.py [socket] # default /tmp/mcp.sock

Exit 0 = every assertion held. Anything else prints the first mismatch and stops.
"""
import json, math, socket, sys

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mcp.sock"
_n = 0


def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": _n, "method": method,
                           "params": params}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    r = json.loads(buf.decode().strip())
    if "error" in r:
        raise RuntimeError(f"{method}: {r['error']}")
    return r["result"]


def near(a, b, tol=1e-6):
    return abs(a - b) < tol


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}", file=sys.stderr)
        sys.exit(1)
    print(f"  ok  {what}")


def areas(rep):
    return sorted(round(l["area"], 6) for l in rep["closed_loops"])


print("1. a rectangle is one closed loop of exactly its own area")
try:
    call("sketch_cancel")
except Exception:
    pass
call("sketch_begin", plane="XY")
call("sketch_add", rect=[0, 0, 80, 50])
r = call("sketch_describe")
check(r["buildable"], "buildable")
check(areas(r) == [4000.0], f"one loop of 4000 mm^2 (got {areas(r)})")

print("2. a circle inside it is a VOID, not a second profile")
call("sketch_add", type="circle", center=[40, 25], radius=10)
r = call("sketch_describe")
outer = [l for l in r["closed_loops"] if near(l["area"], 4000.0)][0]
check(len(outer["holes"]) == 1, "the rectangle encloses exactly one void")
hole = r["closed_loops"][outer["holes"][0]]
check(near(hole["area"], math.pi * 100), f"the void is pi*r^2 (got {hole['area']})")

print("3. offsetting the outer loop inward keeps it CLOSED and exact")
call("sketch_select", entities=[0, 1, 2, 3])
call("sketch_offset", distance=5)
r = call("sketch_describe")
check(r["open_ends"] == [], "no open ends after the offset")
check(any(near(l["area"], 70 * 40) for l in r["closed_loops"]),
      f"the offset loop is 70x40 (got {areas(r)})")

print("4. a gap is REPORTED with its coordinates, then healed into a constraint")
call("sketch_cancel")
call("sketch_begin", plane="XY")
call("sketch_add", entities=[
    {"type": "line", "p0": [0, 0], "p1": [60, 0]},
    {"type": "line", "p0": [60, 0], "p1": [60, 40]},
    {"type": "line", "p0": [60, 40], "p1": [0, 40]},
    {"type": "line", "p0": [0, 40], "p1": [0.4, 0]},   # 0.4 mm short of closing
])
r = call("sketch_validate", tolerance=1.0)
check(not r["buildable"], "a 0.4 mm gap makes the profile unbuildable")
check(len(r["open_ends"]) == 2, f"both free ends are named (got {r['open_ends']})")
dof_before = r["dof"]
r = call("sketch_heal", tolerance=1.0)
check(r["welded"] == 1, f"one pair welded (got {r['welded']})")
check(r["buildable"] and r["open_ends"] == [], "healed profile is buildable")
check(areas(r) == [2400.0], f"healed loop is 60x40 (got {areas(r)})")
check(r["dof"] < dof_before,
      f"the weld recorded a real constraint: DoF {dof_before} -> {r['dof']}")

print("5. construction geometry is excluded from the profile")
call("sketch_select", entities=[0])
call("sketch_construction")
r = call("sketch_describe")
check(not r["buildable"], "turning one side into a guide opens the profile again")
call("sketch_construction")
r = call("sketch_describe")
check(r["buildable"], "turning it back closes it again")

call("sketch_cancel")
print("\nall sketch assertions held")
