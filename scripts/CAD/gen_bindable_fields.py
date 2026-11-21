#!/usr/bin/env python3
"""Regenerate docs/CAD/mcp_bindable_fields.md from a RUNNING app.

The set of fields `set_feature_expr` accepts comes from the app itself (describe_tools, which prints the kernel's own
allow-list, CadDocument::bindable_fields). FEATURE says which feature each field drives; the script FAILS when the app
reports a field this table does not know, or the table names one the app no longer accepts, so the doc cannot drift.

    ORCA_CAD_MCP=/tmp/orca-test.sock python3 scripts/CAD/gen_bindable_fields.py          # rewrite the doc
    ORCA_CAD_MCP=/tmp/orca-test.sock python3 scripts/CAD/gen_bindable_fields.py --check  # fail if the doc is stale
"""
import json, os, re, socket, sys

SOCK = os.environ.get("ORCA_CAD_MCP") or os.environ.get("SNAPORCA_MCP") or "/tmp/orca-cad.sock"   # SNAPORCA_MCP: the old name
DOC = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "CAD", "mcp_bindable_fields.md")

# field -> (feature, what it is, unit)
FEATURE = {
    "distance": ("Extrude", "depth (first side when two-sided)", "mm"),
    "distance2": ("Extrude (end = two-sided)", "second-side depth", "mm"),
    "taper_deg": ("Extrude", "taper (draft) of the side walls", "deg"),
    "height": ("Sketch (rectangle shape)", "height", "mm"),
    "width": ("Sketch (rectangle shape)", "width", "mm"),
    "radius": ("Sketch (circle shape)", "radius", "mm"),
    "revolve_angle": ("Revolve", "sweep angle", "deg"),
    "dressup_size": ("Fillet (radius), Chamfer (distance)", "size of the dress-up", "mm"),
    "draft_angle": ("Draft", "draft angle", "deg"),
    "shell_thickness": ("Shell", "wall thickness", "mm"),
    "thicken_thickness": ("Thicken", "thickness", "mm"),
    "rib_depth": ("Rib", "depth", "mm"),
    "rib_thickness": ("Rib", "thickness", "mm"),
    "cut_offset": ("Cut (split by plane)", "offset of the cutting plane", "mm"),
    "bool_tolerance": ("Boolean", "fuzzy tolerance", "mm"),
    "pattern_count": ("Pattern", "instances along the first direction (rounded)", ""),
    "pattern_spacing": ("Pattern", "step along the first direction", "mm"),
    "pattern_count2": ("Pattern (grid)", "instances along the second direction (rounded, 1..500)", ""),
    "pattern_spacing2": ("Pattern (grid)", "step along the second direction", "mm"),
    "pattern_angle": ("Pattern (circular)", "total angle", "deg"),
    "hole_x": ("Hole", "x in the sketch plane's frame", "mm"),
    "hole_y": ("Hole", "y in the sketch plane's frame", "mm"),
    "hole_diameter": ("Hole", "diameter", "mm"),
    "hole_depth": ("Hole", "depth", "mm"),
    "hole_cbore_diameter": ("Hole (counterbore)", "counterbore diameter", "mm"),
    "hole_cbore_depth": ("Hole (counterbore)", "counterbore depth", "mm"),
    "hole_csink_diameter": ("Hole (countersink)", "countersink diameter", "mm"),
    "hole_csink_angle": ("Hole (countersink)", "countersink angle", "deg"),
    "thread_x": ("Thread", "x in the sketch plane's frame", "mm"),
    "thread_y": ("Thread", "y in the sketch plane's frame", "mm"),
    "thread_diameter": ("Thread", "nominal diameter (stored as the radius)", "mm"),
    "thread_radius": ("Thread", "radius (kept for older recipes)", "mm"),
    "thread_height": ("Thread", "thread length", "mm"),
    "thread_depth": ("Thread", "thread depth", "mm"),
    "thread_pitch": ("Thread", "pitch", "mm"),
    "helix_height": ("Helix", "height", "mm"),
    "helix_pitch": ("Helix", "pitch", "mm"),
    "helix_radius": ("Helix", "radius", "mm"),
    "helix_taper_deg": ("Helix", "taper", "deg"),
    "import_scale_x": ("Sketch (imported outline)", "scale in x", ""),
    "import_scale_y": ("Sketch (imported outline)", "scale in y", ""),
    "mate_angle": ("Mate", "angle", "deg"),
    "mate_offset": ("Mate", "offset", "mm"),
    "plane_angle_tilt": ("Plane", "tilt", "deg"),
    "plane_offset": ("Plane", "offset", "mm"),
    "plane_u_size": ("Plane", "display size u", "mm"),
    "plane_v_size": ("Plane", "display size v", "mm"),
    "xf_angle_deg": ("Transform", "rotation", "deg"),
    "sm_thickness": ("Sheet rule", "sheet thickness", "mm"),
    "sm_radius": ("Sheet rule", "bend radius", "mm"),
    "sm_k": ("Sheet rule", "K-factor", ""),
    "sm_gap": ("Sheet rule", "hem gap", "mm"),
    "sm_angle": ("Edge flange", "bend angle", "deg"),
}


def call(method, **params):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    r = json.loads(buf.decode())
    if "error" in r:
        sys.exit("%s: %s" % (method, r["error"]))
    return r["result"]


def live_fields():
    tools = call("describe_tools")["tools"]
    t = next(t for t in tools if t["name"] == "set_feature_expr")
    m = re.search(r"Fields: (.*?), and dim:<N>", t["summary"])
    if not m:
        sys.exit("describe_tools no longer lists the bindable fields in set_feature_expr")
    return [f.strip() for f in m.group(1).split(",")]


def main():
    live = live_fields()
    missing = sorted(set(live) - set(FEATURE))
    stale = sorted(set(FEATURE) - set(live))
    if missing or stale:
        sys.exit("FEATURE table out of date. App reports but not described: %s. Described but not accepted: %s" % (missing, stale))
    rows = ["| Field | Drives | Feature | Unit |", "|---|---|---|---|"]
    for f in sorted(live, key=lambda f: (FEATURE[f][0], f)):
        feat, what, unit = FEATURE[f]
        rows.append("| `%s` | %s | %s | %s |" % (f, what, feat, unit or "-"))
    rows.append("| `dim:<N>` | value of dimension N of a sketch (`sketch_dimensions` lists the indexes) | Sketch | mm / deg |")
    text = ("# Fields `set_feature_expr` can bind\n\n"
            "Generated by `scripts/CAD/gen_bindable_fields.py` from a running app (the kernel's allow-list, "
            "`CadDocument::bindable_fields`). Do not edit by hand.\n\n"
            "`set_feature_expr {feature, field, expr}` binds one field to an expression over the variables. "
            "A field not in this list is refused with `unknown parameter: <field>` and the document is left unchanged.\n\n"
            + "\n".join(rows) + "\n")
    if "--check" in sys.argv:
        cur = open(DOC).read() if os.path.exists(DOC) else ""
        if cur != text:
            sys.exit("docs/CAD/mcp_bindable_fields.md is stale: run scripts/CAD/gen_bindable_fields.py")
        print("mcp_bindable_fields.md is current")
    else:
        open(DOC, "w").write(text)
        print("wrote", os.path.normpath(DOC), len(live), "fields")


if __name__ == "__main__":
    main()
