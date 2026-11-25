#!/usr/bin/env python3
"""Item 1 parity check: the SAME feature-tree operations driven through the wx tree and through the
on-canvas ImGui tree must leave the document in the SAME state.

Runs against a live app on the rig (Xvfb display, ORCA_CAD_MCP socket), e.g.

    DISPLAY=:10 python3 scripts/CAD/check-ui-parity.py --sock /tmp/orca-epic.sock --ui wx
    DISPLAY=:10 python3 scripts/CAD/check-ui-parity.py --sock /tmp/orca-epic.sock --ui imgui

Each run builds the same fresh document (sketch, sketch, extrude, plane), performs the operations in
OPS through the chosen UI with real mouse input (xdotool), and prints one parity row per operation:
the document facts read back over MCP. `--ui both` runs the two in turn and diffs the tables;
exit status 1 when any row differs. Coordinates are for the 1920x1080 rig window.

Real clicks need real durations: ImGui reads input once per frame, and the canvas renders on demand,
so every press is held 0.3 s and every move is followed by a pause (xdotool's instant click is
dropped by ImGui; a human click is not).
"""
import argparse, json, socket, subprocess, sys, time

# --- MCP -------------------------------------------------------------------------------------------
class Mcp:
    def __init__(self, path): self.path = path
    def call(self, method, params=None):
        s = socket.socket(socket.AF_UNIX); s.settimeout(60); s.connect(self.path)
        req = {"jsonrpc": "2.0", "id": 1, "method": method}
        if params is not None: req["params"] = params
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"): buf += s.recv(65536)
        s.close()
        r = json.loads(buf.decode())
        if "error" in r: raise RuntimeError(r["error"])
        return r["result"]

def xdo(*a): subprocess.run(["xdotool", *map(str, a)], check=True)
def click(x, y, button=1, hold=0.3):
    xdo("mousemove", x, y); time.sleep(0.8)
    xdo("mousedown", button); time.sleep(hold); xdo("mouseup", button); time.sleep(1.6)

# --- geometry of the two UIs for the 4-feature scenario (rig window 1920x1080) ----------------------
# wx tree: first feature row y=324, 27 px pitch; action icons at the row's right end.
WX = dict(row=lambda i: 324 + 27 * i, eye=453, delete=473, edit=432)
# ImGui tree (ribbon on, tree docked under it): rows at x=566, y=314 + 20*i; popup items 24 px below the click.
IM = dict(row=lambda i: 314 + 20 * i, x=566, menu_dy=dict(rename=19, edit=39, suppress=59, up=83, down=103, zoom=123, delete=147))   # measured from the popup on the rig

def facts(m):
    s = m.call("describe_scene")
    # types, not names: the "Sketch 2" counter differs between runs and is not what is being compared
    return {"features": [(f["type"], f["enabled"]) for f in s["features"]], "bodies": len(s["bodies"])}

def setup(m):
    m.call("new_document")   # a known starting point
    m.call("sketch_begin", {"plane": "XY"}); m.call("sketch_add", {"rect": [0, 0, 30, 20]}); m.call("sketch_commit")
    m.call("extrude", {"width": 20, "height": 20, "distance": 10, "plane": "XY"})
    m.call("plane", {"base": 0, "offset": 30})

# --- operations: (name, wx driver, imgui driver) ------------------------------------------------------
def wx_menu_free_ops(m, ui):
    """Yield (op name, callable). Row indices are those of the 4-feature scenario: 0 Schizzo 1, 1 Sketch,
    2 Extrude, 3 Plane."""
    if ui == "wx":
        yield "suppress Extrude",   lambda: click(WX["eye"], WX["row"](2))
        yield "unsuppress Extrude", lambda: click(WX["eye"], WX["row"](2))
        yield "delete Plane",       lambda: click(WX["delete"], WX["row"](3))
    else:
        def menu(row, item):
            click(IM["x"], IM["row"](row), button=3)
            click(IM["x"] + 50, IM["row"](row) + IM["menu_dy"][item])
        yield "suppress Extrude",   lambda: menu(2, "suppress")
        yield "unsuppress Extrude", lambda: menu(2, "suppress")
        yield "delete Plane",       lambda: menu(3, "delete")

def run(ui, sock):
    m = Mcp(sock)
    # The ImGui tree's coordinates below assume the ribbon is up (the tree docks under it).
    m.call("ui_option", {"name": "imgui_ribbon", "value": ui == "imgui"})
    m.call("ui_option", {"name": "imgui_tree", "value": ui == "imgui"})
    time.sleep(1.5)
    setup(m)
    # Let the overlay lay itself out before the first click: it renders on demand, so wake it up.
    for _ in range(3): xdo("mousemove", 600, 700); time.sleep(0.5); xdo("mousemove", 640, 720); time.sleep(0.5)
    rows = [("initial", facts(m))]
    for name, fn in wx_menu_free_ops(m, ui):
        fn(); time.sleep(1.0)
        rows.append((name, facts(m)))
    return rows

def table(rows):
    return [f"{n:22s} bodies={f['bodies']} " + " ".join(f"{a}{'' if e else '(off)'}" for a, e in f["features"]) for n, f in rows]

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/tmp/orca-epic.sock")
    ap.add_argument("--ui", choices=["wx", "imgui", "both"], default="both")
    a = ap.parse_args()
    results = {}
    for ui in (["wx", "imgui"] if a.ui == "both" else [a.ui]):
        results[ui] = run(ui, a.sock)
        print(f"== {ui}"); print("\n".join(table(results[ui])))
        if a.ui == "both":   # a clean document for the second pass
            Mcp(a.sock).call("ui_option", {"name": "imgui_tree", "value": False})
            Mcp(a.sock).call("ui_option", {"name": "imgui_ribbon", "value": False})
    if len(results) == 2:
        same = [(x[0], x[1] == y[1]) for x, y in zip(results["wx"], results["imgui"])]
        print("== parity"); [print(f"{n:22s} {'SAME' if ok else 'DIFFERENT'}") for n, ok in same]
        sys.exit(0 if all(ok for _, ok in same) else 1)
