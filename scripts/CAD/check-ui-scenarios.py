#!/usr/bin/env python3
"""GUI scenarios as data: tests/cad_scenarios/*.json, each a list of steps driven through the same MCP / ui.* surface a person's input
reaches the app by (cards, real canvas clicks, keys), with expectations on the resulting document. Adding a scenario is adding a file.

    python3 scripts/CAD/check-ui-scenarios.py [socket] [scenario-dir]

Steps (one key each):
  {"mcp": "extrude", "params": {...}, "refused": true}   an MCP tool call ("refused": it must fail and change nothing)
  {"card": {"open": "dressup", "set": {...}, "confirm": true}}   the wx card driver (ui.card)
  {"click_edge": {"axis": "x", "z": "max", "y": "min"}}  a real click on the body edge parallel to that axis at that extreme
  {"click": [x, y]} / {"key": "Escape", "mods": []}      raw canvas input
  {"expect": {"volume": 1234.5, "rel": 1e-3, "features": 3, "bodies": 1, "undo_delta": 1}}   undo_delta: steps since the last "mark"
  {"mark": true}                                          remember the undo depth
"""
import glob, json, os, socket, sys, time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
HERE = os.path.dirname(os.path.abspath(__file__))
DIR = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.dirname(HERE)), "tests", "cad_scenarios")
if not os.path.isdir(DIR):
    DIR = "/tmp/orc/tests/cad_scenarios"
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


def show():
    call("screenshot", path="/tmp/ui-scn.png", view="iso", fit=True)
    time.sleep(1.0)


def clear_canvas():
    c = call("ui.inspect")["canvas"]
    for _ in range(3):
        call("ui.click", at=[c["width"] - 30, 120]); time.sleep(0.3)
        call("ui.key", key="Escape"); time.sleep(0.4)


def pick_edge(sel):
    topo = call("query_topology", body=sel.get("body", 0))
    axis = {"x": 0, "y": 1, "z": 2}[sel["axis"]]
    cands = []
    for e in topo["edges"]:
        if "mid" in e or e["kind"] != "line":
            continue
        d = [abs(a - b) for a, b in zip(e["p0"], e["p1"])]
        if all(d[i] < 1e-6 for i in range(3) if i != axis) and d[axis] > 1e-6:
            cands.append(e)
    for k in ("x", "y", "z"):
        if k in sel and cands:
            i = {"x": 0, "y": 1, "z": 2}[k]
            ext = (max if sel[k] == "max" else min)(((e["p0"][i] + e["p1"][i]) / 2) for e in cands)
            cands = [e for e in cands if abs((e["p0"][i] + e["p1"][i]) / 2 - ext) < 1e-6]
    if not cands:
        raise RuntimeError("no edge matches %s" % sel)
    e = cands[0]
    p = [(a + b) / 2 for a, b in zip(e["p0"], e["p1"])]
    w = call("ui.worldToScreen", point=p)
    show()
    w = call("ui.worldToScreen", point=p)
    call("ui.click", at=[w["x"], w["y"]])
    time.sleep(0.7)


fails = []
state = {"mark": 0}


def run(path):
    sc = json.load(open(path))
    name = sc.get("name", os.path.basename(path))
    clear_canvas()
    call("new_document")
    state["mark"] = call("inspect_design")["undo_depth"]
    for i, step in enumerate(sc["steps"]):
        where = "%s step %d (%s)" % (name, i + 1, next(iter(step)))
        try:
            if "mcp" in step:
                if step.get("refused"):   # the call must be refused, and change nothing
                    try:
                        call(step["mcp"], **step.get("params", {}))
                    except RuntimeError:
                        continue
                    raise RuntimeError("expected %s to be refused" % step["mcp"])
                call(step["mcp"], **step.get("params", {}))
            elif "card" in step:
                c = step["card"]
                show()
                r = call("ui.card", **({"open": c["open"]} if "open" in c else {}), set=c.get("set", {}), confirm=c.get("confirm", True))
                if c.get("confirm", True) and not r["ok"]:
                    raise RuntimeError("the card did not confirm: %s" % r["card"])
                time.sleep(0.5)
            elif "click_edge" in step:
                pick_edge(step["click_edge"])
            elif "click" in step:
                call("ui.click", at=step["click"]); time.sleep(0.5)
            elif "key" in step:
                call("ui.key", key=step["key"], mods=step.get("mods", [])); time.sleep(0.4)
            elif "mark" in step:
                state["mark"] = call("inspect_design")["undo_depth"]
            elif "expect" in step:
                ex = step["expect"]
                d = call("inspect_design")
                if "volume" in ex:
                    v = sum(b.get("volume", 0) for b in d["bodies"])
                    rel = ex.get("rel", 1e-3)
                    assert abs(v - ex["volume"]) <= rel * abs(ex["volume"]), "volume %.3f, wanted %.3f" % (v, ex["volume"])
                if "features" in ex:
                    assert len(d["features"]) == ex["features"], "%d features, wanted %d" % (len(d["features"]), ex["features"])
                if "bodies" in ex:
                    assert len(d["bodies"]) == ex["bodies"], "%d bodies, wanted %d" % (len(d["bodies"]), ex["bodies"])
                if "undo_delta" in ex:
                    assert d["undo_depth"] - state["mark"] == ex["undo_delta"], "%+d undo steps, wanted %+d" % (d["undo_depth"] - state["mark"], ex["undo_delta"])
                if "error" in ex:
                    assert (d["error"] == "") == (not ex["error"]), "error state: %r" % d["error"]
            else:
                raise RuntimeError("unknown step %s" % list(step))
        except Exception as e:  # noqa: BLE001
            print("FAIL %s: %s" % (where, e))
            fails.append(where)
            return
    print("ok   %s" % name)


files = sorted(glob.glob(os.path.join(DIR, "*.json")))
if not files:
    print("no scenarios in %s" % DIR)
    sys.exit(2)
for f in files:
    run(f)
print("scenarios: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
