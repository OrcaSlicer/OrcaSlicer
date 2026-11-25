#!/usr/bin/env python3
"""The four MCP behaviours an agent hit and the app now guarantees (run against a RUNNING app):

  1. sketch_begin on a face reports that face's frame ("face:<id>", origin, normal), not "XY".
  2. A rejected constraint says why in one shape: kind + the conflicting constraints named like the request.
  3. The second direction of a grid pattern (count2 / spacing2) is bindable: set_feature_expr moves the copies.
  4. A call that takes a face/edge id without `generation` is refused once this client's last read is out of date;
     a client that never read the topology is not blocked.

    ORCA_CAD_MCP=/tmp/orca-test.sock python3 scripts/CAD/check-mcp-api-gaps.py
"""
import json, os, socket, sys

SOCK = sys.argv[1] if len(sys.argv) > 1 else (os.environ.get("ORCA_CAD_MCP") or os.environ.get("SNAPORCA_MCP") or "/tmp/orca-cad.sock")
fails = []


class Client:
    """One connection = one client: the server remembers what THIS connection last read."""
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(90); self.s.connect(SOCK); self.n = 0

    def raw(self, method, **params):
        self.n += 1
        self.s.sendall((json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            d = self.s.recv(65536)
            if not d:
                break
            buf += d
        return json.loads(buf.decode())

    def call(self, method, **params):
        r = self.raw(method, **params)
        if "error" in r:
            raise RuntimeError("%s: %s" % (method, r["error"]))
        return r["result"]


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + ((" - " + str(detail)) if detail and not ok else ""))
    if not ok:
        fails.append(name)


def box(c):
    c.call("new_document"); c.call("sketch_begin", plane="XY"); c.call("sketch_add", rect=[-20, -15, 20, 15]); c.call("sketch_commit")
    c.call("extrude", sketch=0, distance=10)


# 1 ---------------------------------------------------------------------------------------------------------------
c = Client(); box(c)
top = next(f for f in c.call("query_topology", body=0)["faces"] if abs(f["normal"][2] - 1) < 1e-6)
r = c.call("sketch_begin", face=top["id"], body=0)
check("sketch_begin face: plane is face:<id>", r.get("plane") == "face:%d" % top["id"], r)
check("sketch_begin face: frame is reported", r.get("normal") == [0.0, 0.0, 1.0] and abs(r["origin"][2] - 10) < 1e-6, r)
c.call("sketch_cancel")
r = c.call("sketch_begin", plane="XZ")
check("sketch_begin plane: still reports the plane asked for", r.get("plane") == "XZ" and abs(r["normal"][1]) == 1, r)
c.call("sketch_cancel")

# 2 ---------------------------------------------------------------------------------------------------------------
c.call("new_document"); c.call("sketch_begin", plane="XY"); c.call("sketch_add", rect=[-20, -15, 20, 15])
r = c.call("sketch_constrain", atomic=False, constraints=[{"type": "symmetric_y", "a": "0.start", "b": "0.end"}])
rej = r["rejected"][0] if r.get("rejected") else {}
check("rejected constraint: kind and conflicts, in the request's vocabulary",
      rej.get("kind") == "redundant_or_inconsistent" and rej.get("conflicts")
      and any(x["of"] == "request" and x["constraint"].startswith("symmetric_y a=0.start b=0.end") for x in rej["conflicts"])
      and any(x["of"] == "existing" and x["constraint"].split()[0] in ("horizontal", "horizontal_points") for x in rej["conflicts"]), rej)
r = c.call("sketch_constrain", constraints=[{"type": "nonsense"}])
check("malformed constraint: same shape", r["rejected"][0].get("kind") == "malformed" and r["rejected"][0].get("conflicts") == [], r)
c.call("sketch_cancel")

# 3 ---------------------------------------------------------------------------------------------------------------
box(c)
# a boss on the box (a union that changes the existing body), repeated over a 2 x 2 grid
c.call("sketch_begin", plane="XY"); c.call("sketch_add", type="circle", center=[-10, 0], radius=3); c.call("sketch_commit")
c.call("extrude", sketch=2, distance=15, boolean="union")
p = c.call("pattern", features=[3], axis=[1, 0, 0], count=2, spacing=20, axis2=[0, 1, 0], count2=2, spacing2=10)
check("grid pattern builds", p.get("ok") is True, p)
pi = p["pattern_index"]
vol = lambda: sum(b["volume"] for b in c.call("inspect_design")["bodies"])
v0 = vol()
r = c.call("set_feature_expr", feature=pi, field="pattern_count2", expr="3")
check("pattern_count2 binds", r.get("ok") is True, r)
v1 = vol()
check("pattern_count2 changes the copies", v1 > v0 + 1.0, (v0, v1))
r = c.call("set_feature_expr", feature=pi, field="pattern_spacing2", expr="45")
check("pattern_spacing2 binds", r.get("ok") is True, r)
check("the field list reports both", {"pattern_count2", "pattern_spacing2"} <= set(
    next(t for t in c.call("describe_tools")["tools"] if t["name"] == "set_feature_expr")["summary"].replace(",", " ").split()))

# 4 ---------------------------------------------------------------------------------------------------------------
reader = Client(); box(reader)
topo = reader.call("query_topology", body=0)
edge = topo["edges"][0]["id"]
reader.call("set_feature_expr", feature=1, field="distance", expr="12")   # the model moved on: the ids just read are old
r = reader.raw("fillet", edge=edge, radius=1)
check("stale id without `generation` is refused after this client's read", r.get("error", {}).get("code") == -32010, r)
topo = reader.call("query_topology", body=0)
r = reader.raw("fillet", edge=topo["edges"][0]["id"], radius=1)
check("a fresh read makes the same call valid", "result" in r and r["result"].get("ok") is True, r)
fresh = Client()   # never read the topology: nothing to compare against, nothing is blocked
r = fresh.raw("fillet", edge=0, radius=0.5)
check("a client that never read is not blocked", r.get("error", {}).get("code") != -32010, r)

print("api gaps: " + ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
