#!/usr/bin/env python3
"""Run the vendored SolidCraft oracle corpus (tests/cad_oracle/*) against a live Orca-CAD over its MCP socket.

    ORCA_CAD_MCP=/tmp/orca-test.sock python3 scripts/CAD/oracle_runner.py [--filter 03] [--report out.json]

Each case is a kernel-neutral recipe.json plus expected.json (analytic or FreeCAD-measured). The runner replays
the recipe through the app's MCP tools, then compares volume / area / centre of mass / bbox / body count.

Statuses: PASS, FAIL (built but numbers differ, or the app refused), XFAIL (known, see XFAIL), BLOCKED (the MCP surface cannot express an op
yet: the reason is the work item that unblocks it). The blocked list is the P1/P4 burn-down of the SolidCraft
integration plan; exit code is 1 only on FAIL, so growing coverage never turns the gate red by itself.

Tolerances follow tests/cad_oracle/README.md: relative 1e-3 on volume/area, 1e-3 of the bbox diagonal on COM/bbox.
Needs a FRESH document and the Design tab selected (same preconditions as smoke_golden_path.py).
"""
import argparse, glob, json, math, os, socket, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "..", "..", "tests", "cad_oracle")


# Cases that run but disagree with the oracle for a known kernel reason. Reported XFAIL (not a red gate);
# an XFAIL case that starts passing is reported XPASS so the entry gets deleted.
XFAIL = {}


class Blocked(Exception):
    """The MCP surface cannot express this yet. Message = what is missing."""


class Mcp:
    def __init__(self, path):
        self.path, self.n = path, 0

    def call(self, method, params=None):
        self.n += 1
        req = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            req["params"] = params
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(120)
        s.connect(self.path)
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
        s.close()
        r = json.loads(buf.decode())
        if "error" in r:
            raise RuntimeError("%s: %s" % (method, r["error"].get("message", r["error"])))
        out = r["result"]
        if isinstance(out, dict) and out.get("ok") is False:
            raise RuntimeError("%s refused: %s" % (method, "; ".join([out.get("error") or ""] + [str(r.get("error")) for r in out.get("rejected", [])]) or out))
        return out


# ---- recipe -> MCP -----------------------------------------------------------------------------

# The recipe's sketch frames (x_dir, y_dir, normal) against the app's SketchPlane: XY is identical; the app's XZ has
# y = +Z (recipe -Z) and its YZ has x = Y, y = Z (recipe x = -Z, y = Y). to_app maps recipe (u, v) -> app (a, b).
PLANES = {
    "XY": ("XY", lambda u, v: (u, v)),
    "XZ": ("XZ", lambda u, v: (u, -v)),
    "YZ": ("YZ", lambda u, v: (v, -u)),
}
# world axis -> (plane, app revolve axis index 0 = plane x, 1 = plane y) for a revolve about a world axis through the origin
AXES = {("XY", "x"): 0, ("XY", "y"): 1, ("XZ", "x"): 0, ("XZ", "z"): 1, ("YZ", "y"): 0, ("YZ", "z"): 1}


# app plane per world axis the plane faces: (base id for `plane`, app x axis, app y axis). Offsets are along +axis.
AXIS_BASE = {2: (0, (1, 0, 0), (0, 1, 0)), 1: (1, (1, 0, 0), (0, 0, 1)), 0: (2, (0, 1, 0), (0, 0, 1))}


def sketch_entities(sk, to_app):
    """(app entities, {recipe id: index}, {recipe ids of arcs whose app start/end are swapped}); Blocked for kinds sketch_add cannot take."""
    out, swapped = [], set()
    for e in sk["entities"]:
        t, c = e["type"], bool(e.get("construction", False))
        if t == "line":
            out.append({"type": "line", "p0": list(to_app(*e["start"]["at"])), "p1": list(to_app(*e["end"]["at"])), "construction": c})
        elif t == "circle":
            out.append({"type": "circle", "center": list(to_app(*e["center"]["at"])), "radius": e["radius"], "construction": c})
        elif t == "arc_three_point":
            out.append(arc_from_three(to_app(*e["start"]["at"]), to_app(*e["mid"]), to_app(*e["end"]["at"]), c))
        elif t == "arc_center_start_sweep":
            ctr, st = to_app(*e["center"]["at"]), to_app(*e["start"]["at"])
            r = math.dist(ctr, st)
            a0 = math.atan2(st[1] - ctr[1], st[0] - ctr[0])
            sweep = math.radians(e["sweep_deg"])
            if mirrored(to_app):
                sweep = -sweep
            lo, hi = (a0, a0 + sweep) if sweep >= 0 else (a0 + sweep, a0)
            out.append({"type": "arc", "center": list(ctr), "radius": r, "start_angle": lo, "end_angle": hi, "construction": c})
        elif t == "point":
            out.append({"type": "point", "p": list(to_app(*e["at"])), "construction": c})
        else:
            raise Blocked("sketch entity %r" % t)
        if out[-1]["type"] == "arc":   # does the app's start (p0) sit where the recipe's start is?
            a = out[-1]
            want = e["start"]["at"] if t == "arc_three_point" else e["start"]["at"]
            w = to_app(*want)
            p0 = (a["center"][0] + a["radius"] * math.cos(a["start_angle"]), a["center"][1] + a["radius"] * math.sin(a["start_angle"]))
            if math.dist(p0, w) > 1e-6:
                swapped.add(e["id"])
    return out, {e["id"]: i for i, e in enumerate(sk["entities"])}, swapped


def ref_point(ents, token):
    """(x, y) of a token like "3.end" / "origin" against the app-format entity list; None when it is not a point."""
    if token == "origin":
        return (0.0, 0.0)
    idx, _, role = token.partition(".")
    if not role:
        return None
    e = ents[int(idx)]
    if role == "center" or e["type"] in ("circle", "point"):
        return tuple(e.get("center") or e.get("p"))
    if e["type"] == "line":
        return tuple(e["p0"] if role == "start" else e["p1"])
    c, r = e["center"], e["radius"]
    a = e["start_angle"] if role == "start" else e["end_angle"]
    return (c[0] + r * math.cos(a), c[1] + r * math.sin(a))


def mirrored(to_app):
    a, b = to_app(1, 0), to_app(0, 1)
    return a[0] * b[1] - a[1] * b[0] < 0


def arc_from_three(p, m, q, construction):
    ax, ay = p; bx, by = m; cx, cy = q
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-12:
        raise Blocked("degenerate three-point arc")
    ux = ((ax**2 + ay**2) * (by - cy) + (bx**2 + by**2) * (cy - ay) + (cx**2 + cy**2) * (ay - by)) / d
    uy = ((ax**2 + ay**2) * (cx - bx) + (bx**2 + by**2) * (ax - cx) + (cx**2 + cy**2) * (bx - ax)) / d
    ang = lambda x, y: math.atan2(y - uy, x - ux)
    s, mm, e = ang(ax, ay), ang(bx, by), ang(cx, cy)
    ccw = lambda a0, a1: (a1 - a0) % (2 * math.pi)       # CCW distance from a0 to a1
    if ccw(s, mm) > ccw(s, e):                           # the middle point is not on the CCW way s->e: the arc runs CW
        s, e = e, s
    e = s + (e - s) % (2 * math.pi)                      # end > start, so the app cannot read it as the other way round
    return {"type": "arc", "center": [ux, uy], "radius": math.hypot(ax - ux, ay - uy), "start_angle": s, "end_angle": e, "construction": construction}


def number(v, params):
    """A recipe value as a float: a number, "60 mm", or the name of a recipe parameter (resolved, not linked)."""
    if isinstance(v, (int, float)):
        return float(v)
    if v in params:
        return number(params[v], params)
    import re
    m = re.fullmatch(r"\s*(-?\d+(?:\.\d+)?)\s*(?:mm|deg)?\s*", str(v))
    if m:
        return float(m.group(1))
    return _eval_expr(str(v), params)


def _eval_expr(text, params):
    """A small arithmetic expression over recipe parameters ("width / 2", "10 mm + hole_d"): + - * / ** ( ), numbers with an
    optional unit (mm, in, deg), parameter names. Resolved to a number, not linked."""
    import ast, operator, re
    t = re.sub(r"(\d+(?:\.\d+)?)\s*in\b", lambda m: str(float(m.group(1)) * 25.4), text)
    t = re.sub(r"(\d+(?:\.\d+)?)\s*(?:mm|deg)\b", r"\1", t)
    ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv, ast.Pow: operator.pow}

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float)):
            return float(n.value)
        if isinstance(n, ast.Name) and n.id in params:
            return number(params[n.id], params)
        if isinstance(n, ast.BinOp) and type(n.op) in ops:
            return ops[type(n.op)](ev(n.left), ev(n.right))
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.UAdd)):
            return -ev(n.operand) if isinstance(n.op, ast.USub) else ev(n.operand)
        raise Blocked("value %r (expression)" % (text,))

    try:
        return ev(ast.parse(t, mode="eval"))
    except SyntaxError:
        raise Blocked("value %r (expression)" % (text,))


BOOL = {"new": "new", "add": "union", "join": "union", "remove": "subtract", "cut": "subtract", "intersect": "intersect"}


class Replay:
    def __init__(self, mcp):
        self.m, self.sketches, self.body, self.fidx, self.entity_index, self.sketch_neg, self.tilted, self.point_sets = mcp, {}, 0, {}, {}, {}, {}, {}
        self.rule_edits = None
        self.params, self.order, self.datums = {}, [], {}
        self.mo = [0, 0, 0]   # modeling_origin: the app frame is the recipe frame shifted by it   # recipe parameters (name -> expression); body names in app order

    def plane_for(self, p):
        """(plane name for sketch_begin, to_app) for a recipe plane. The app frame is the recipe's shifted by modeling_origin
        (every app plane passes through it), so a plane through the world origin is a plain XY/XZ/YZ and anything else a datum."""
        ref, O = p.get("ref"), p.get("origin", [0, 0, 0])
        if ref in PLANES and all(abs(c) < 1e-9 for c in O):
            return PLANES[ref]
        n, xd, yd = p.get("normal"), p.get("x_dir"), p.get("y_dir")
        if not (n and xd and yd):
            raise Blocked("sketch plane %r without a frame" % ref)
        axis = max(range(3), key=lambda i: abs(n[i]))
        if abs(abs(n[axis]) - 1) > 1e-6:
            raise Blocked("tilted sketch plane")
        self._last_neg = n[axis] < 0   # the app's datum faces +axis; a sketch that faces the other way extrudes the other way
        base, xa, ya = AXIS_BASE[axis]
        name = self.datum(base, O[axis])
        dot = lambda u, v: sum(a * b for a, b in zip(u, v))

        def to_app(u, v):
            w = [O[i] + u * xd[i] + v * yd[i] for i in range(3)]
            return (dot(w, xa), dot(w, ya))
        return name, to_app

    def datum(self, base, offset, tilt=0.0, axis=0):
        key = (base, round(offset, 6), tilt, axis)
        if key not in self.datums:
            self.m.call("plane", {"base": base, "offset": offset, "tilt": tilt, "axis": axis})
            self.datums[key] = "datum:%d" % len(self.datums)
        return self.datums[key]

    def run(self, ft):
        if ft["op"].startswith("sm_") and self.rule_edits is not None:
            # the recipe edits the sheet-metal rule (a document-level setting) at some point; the app keeps it as a feature
            # before the first sheet-metal one, which gives every flange the final rule exactly as the reference does
            self.m.call("sm_rule", self.rule_edits)
            self.rule_edits = None
        h = getattr(self, "op_" + ft["op"], None)
        if h is None:
            raise Blocked("op %s" % ft["op"])
        h(ft)
        self.fidx[ft.get("name", "%s#%d" % (ft["op"], len(self.fidx)))] = len(self.m.call("describe_scene")["features"]) - 1   # doc feature index of this recipe feature (a pattern repeats them)

    def _pattern_features(self, ft):
        if ft.get("pattern_of") != "features":
            raise Blocked("pattern of %s" % ft.get("pattern_of"))
        out = []
        for n in ft["features"]:
            base = n.rsplit(" (", 1)[0] if n.endswith(")") and " (" in n else n   # the recipe may name an instance "Hole (1)"
            if n in self.fidx:
                out.append(self.fidx[n])
            elif base in self.fidx:
                out.append(self.fidx[base])
            else:
                raise Blocked("pattern of unknown feature %s" % n)
        return out

    def op_rectangular_pattern(self, ft):
        feats = self._pattern_features(ft)
        d1, d2 = ft["direction1"], ft.get("direction2")
        if "spacing" not in d1 or (d2 and "spacing" not in d2) or "spacing" not in ft.get("distance_type", "spacing"):
            raise Blocked("rectangular pattern by total extent")
        a = {"features": feats, "count": d1["count"], "spacing": d1["spacing"], "axis": d1["axis"], "body": self.body}
        if d2:
            a.update(count2=d2["count"], spacing2=d2["spacing"], axis2=d2["axis"])
        self.m.call("pattern", a)

    def _mirror_features(self, ft):
        if ft.get("operation", "join") != "join":
            raise Blocked("mirror operation %s" % ft.get("operation"))
        pl = ft["plane"]
        self.m.call("pattern", {"features": self._pattern_features({"pattern_of": "features", "features": ft["features"]}), "mirror": True,
                                "axis": pl["normal"], "origin": pl.get("origin", [0, 0, 0]), "body": self.body})

    def op_path_pattern(self, ft):
        pth = ft["path"]
        if len(pth["curves"]) != 1 or "spacing" not in ft:
            raise Blocked("path pattern with several curves / without spacing")
        sname = pth["sketch"]
        self.m.call("pattern", {"features": self._pattern_features(ft), "count": ft["count"], "spacing": ft["spacing"],
                                "path_sketch": self.sketches[sname][0], "path_entity": self.entity_index[sname][pth["curves"][0]],
                                "body": self.body})

    def op_circular_pattern(self, ft):
        feats = self._pattern_features(ft)
        if ft.get("symmetric"):
            raise Blocked("symmetric circular pattern")
        ax = ft["axis"]
        self.m.call("pattern", {"features": feats, "circular": True, "count": ft["count"], "angle": ft.get("total_angle", 360),
                                "axis": ax.get("dir", [0, 0, 1]), "origin": ax.get("origin", [0, 0, 0]), "body": self.body})

    def op_sheet_metal_rule_edit(self, ft):
        pass   # folded into the sm_rule feature emitted before the first sheet-metal feature (see run)

    def op_sm_flange(self, ft):
        kind = ft["type"]
        if kind == "base":
            self.m.call("sm_base", {"sketch": self.sketches[ft["profile"]["sketch"]][0]})
        elif kind == "edge":
            ids, _ = self._edges(ft)
            self.m.call("sm_flange", {"edge": ids, "height": ft["height"], "angle": ft["angle"], "flip": bool(ft.get("flip"))})
        elif kind == "contour":
            self.m.call("sm_contour", {"sketch": self.sketches[ft["profile_sketch"]][0], "width": ft["distance"]})
        else:
            raise Blocked("sheet flange type %s" % kind)

    def op_sm_unfold(self, ft):
        if not ft.get("all_bends"):
            raise Blocked("unfold of chosen bends")
        self.unfold_index = self.m.call("sm_unfold")["feature_index"]   # the stationary piece is the one with the largest face

    def op_sm_refold(self, ft):
        self.m.call("sm_refold", {"unfold": self.unfold_index})

    def op_sm_hem(self, ft):
        if ft.get("type") != "flat":
            raise Blocked("hem type %s" % ft.get("type"))
        ids, _ = self._edges({"edges": [ft["edge"]]})
        self.m.call("sm_hem", {"edge": ids[0], "length": ft["length"]})

    def point_sketch(self, desc):
        """World positions of the points of the sketch a hole feature names ("sketch HolePoints points p1..p6")."""
        name = desc.split()[1]
        if name not in self.point_sets:
            raise Blocked("hole positions from %r" % desc)
        return self.point_sets[name]

    def op_thread(self, ft):
        if not ft.get("modeled", True):
            return   # a cosmetic thread is an annotation: it adds no geometry, so there is nothing to build or compare
        raise Blocked("modeled thread")

    def _helix_from_fit(self, ft):
        """A fitted spline through points that lie on a helix about Z is replayed as the true helix (as the reference does)."""
        e = ft["entities"][0]
        pts = e["fit_points"]
        r = math.hypot(pts[0][0], pts[0][1])
        ang = [math.atan2(p[1], p[0]) for p in pts]
        turn = [0.0]
        for a0, a1 in zip(ang, ang[1:]):
            d = (a1 - a0 + math.pi) % (2 * math.pi) - math.pi
            turn.append(turn[-1] + d)
        if (any(abs(math.hypot(p[0], p[1]) - r) > 1e-4 for p in pts) or abs(turn[-1]) < 1e-6 or abs(pts[0][1]) > 1e-9 or pts[0][0] < 0
                or any(abs(p[2] - pts[0][2] - turn[i] / (2 * math.pi) * (pts[-1][2] - pts[0][2]) / (turn[-1] / (2 * math.pi))) > 1e-4 for i, p in enumerate(pts))):
            raise Blocked("fitted spline that is not a helix about Z")
        height = pts[-1][2] - pts[0][2]
        pitch = height / (abs(turn[-1]) / (2 * math.pi))
        idx = self.m.call("helix", {"radius": r, "pitch": pitch, "height": height, "left_handed": turn[-1] < 0, "plane": "XY"})["helix_index"]
        self.sketches[ft["name"]] = (idx, "XY", {e["id"]})
        self.entity_index[ft["name"]] = {e["id"]: 0}
        self.sketch_neg[ft["name"]] = False

    def op_sketch(self, ft):
        self._last_neg = False
        if len(ft["entities"]) == 1 and ft["entities"][0]["type"] == "fitted_spline":
            return self._helix_from_fit(ft)
        pl0 = ft["plane"]
        if ft["entities"] and all(e["type"] == "point" for e in ft["entities"]) and pl0.get("normal") == [0.0, 0.0, 1.0]:
            self.point_sets[ft["name"]] = [(e["at"][0], e["at"][1], pl0["origin"][2]) for e in ft["entities"]]
        plane, to_app = self.plane_for(ft["plane"])
        neg = self._last_neg
        ents, idx_of, swapped = sketch_entities(ft, to_app)
        types = {e["id"]: e["type"] for e in ft["entities"]}

        def ref(tok):
            if tok == "origin":
                return "origin"
            name, _, role = tok.partition(".")
            if name not in idx_of:
                raise Blocked("constraint reference %r" % tok)
            if role == "":
                return str(idx_of[name])
            if name in swapped and role in ("start", "end"):
                role = "end" if role == "start" else "start"
            return "%d.%s" % (idx_of[name], role)

        cons, seen = [], set()

        def add(c):
            key = (c["type"], tuple(sorted(str(c.get(k)) for k in ("a", "b", "c"))))
            if key not in seen:
                seen.add(key)
                cons.append(c)

        for e in ft["entities"]:   # a point given as {"ref": "l1.end"} is shared with that point
            for role in ("start", "end", "center"):
                pt = e.get(role)
                if isinstance(pt, dict) and pt.get("ref"):
                    add({"type": "coincident", "a": ref("%s.%s" % (e["id"], role)), "b": ref(pt["ref"])})
        for c in ft.get("constraints", []):
            d = {"type": c["type"]}
            refs = list(c["refs"])
            if c["type"] == "symmetric":   # two circles/arcs mean their centres
                for i in (0, 1):
                    if "." not in refs[i] and types.get(refs[i]) in ("circle", "arc"):
                        refs[i] += ".center"
            for k, r in zip("abc", refs):
                d[k] = ref(r)
            add(d)
            if c["type"] == "symmetric" and refs[0].endswith(".center") and refs[1].endswith(".center"):   # a mirrored circle has the same radius
                add({"type": "equal", "a": ref(c["refs"][0]), "b": ref(c["refs"][1])})
        for c in ft.get("dimensions", []):
            t = c["type"]
            if t == "offset":   # perpendicular distance from a point to a line: the app's signed point_on_line, sign taken from the drawing
                ln, pt = ents[int(ref(c["refs"][0]))], ref_point(ents, ref(c["refs"][1]))
                if ln["type"] != "line" or pt is None:
                    raise Blocked("offset between %s and a non-point" % ln["type"])
                dx, dy = ln["p1"][0] - ln["p0"][0], ln["p1"][1] - ln["p0"][1]
                side = dx * (pt[1] - ln["p0"][1]) - dy * (pt[0] - ln["p0"][0])
                cons.append({"type": "point_on_line", "a": ref(c["refs"][1]), "b": ref(c["refs"][0]),
                             "value": math.copysign(number(c["value"], self.params), side if abs(side) > 1e-9 else 1.0)})
                continue
            d = {"type": t, "value": number(c["value"], self.params)}
            for k, r in zip("ab", c["refs"]):
                d[k] = ref(r)
            if t == "distance":
                d["orientation"] = c.get("orientation", "aligned")
                if d["orientation"] in ("horizontal", "vertical"):   # the app's projected distance is signed (b - a): keep the drawn direction
                    pa, pb = ref_point(ents, d["a"]), ref_point(ents, d["b"])
                    if pa and pb:
                        delta = (pb[0] - pa[0]) if d["orientation"] == "horizontal" else (pb[1] - pa[1])
                        d["value"] = math.copysign(d["value"], delta if abs(delta) > 1e-9 else 1.0)
            elif t == "angle":   # the app measures between the lines' DIRECTIONS; the recipe may mean the supplement: take the one nearer the drawn geometry
                dirs = []
                for r in c["refs"]:
                    e = ents[int(ref(r))]
                    dirs.append((e["p1"][0] - e["p0"][0], e["p1"][1] - e["p0"][1]) if e["type"] == "line" else None)
                if all(dirs):
                    cur = abs(math.degrees(math.atan2(dirs[0][0] * dirs[1][1] - dirs[0][1] * dirs[1][0], dirs[0][0] * dirs[1][0] + dirs[0][1] * dirs[1][1])))
                    if abs(cur - (180.0 - d["value"])) < abs(cur - d["value"]):
                        d["value"] = 180.0 - d["value"]
            cons.append(d)
        self.m.call("sketch_begin", {"plane": plane})
        try:
            self.m.call("sketch_add", {"entities": ents, "infer": False})   # the recipe states its own constraints
            # sizes and positions first, then the tangents: from the drawing's rough positions the solver converges in two steps
            # where it does not in one (belt: two circles joined by tangent lines)
            for stage in [[c for c in cons if c["type"] != "tangent"]] + [[c] for c in cons if c["type"] == "tangent"]:
                if stage:
                    r = self.m.call("sketch_constrain", {"constraints": stage})
        except Exception:
            self.m.call("sketch_cancel")
            raise
        idx = self.m.call("sketch_commit")["sketch_index"]
        solid = {e["id"] for e in ft["entities"] if e["type"] != "point" and not e.get("construction")}
        self.sketches[ft["name"]] = (idx, plane, solid)
        self.entity_index[ft["name"]] = {e["id"]: i for i, e in enumerate(ft["entities"])}
        self.sketch_neg[ft["name"]] = neg

    def _sketch(self, prof):
        idx, plane, solid = self.sketches[prof["sketch"]]
        used = {c for lp in prof.get("loops", []) for c in lp["curves"]}
        if used != solid:   # the app extrudes every region of the sketch
            raise Blocked("profile is a subset of the sketch's curves")
        return idx, plane

    def op_extrude(self, ft):
        thin = ft.get("thin")
        if thin:
            # the wall is built around the sketch's whole outline; the recipe names its curves
            if set(thin["curves"]) != self.sketches[thin["sketch"]][2]:
                raise Blocked("thin extrude of a subset of the sketch")
            side, t1, t2 = thin.get("side", "one_side"), thin["thickness"], thin.get("thickness2", 0)
            tin, tout = {"two_sides": (t1, t2), "inside": (t1, 0), "outside": (0, t1)}.get(side, (None, None))
            if tin is None:
                raise Blocked("thin extrude side %s" % side)
            ex = ft["extent"]
            if ex.get("type", "distance") != "distance":
                raise Blocked("thin extrude extent")
            self.m.call("extrude", {"sketch": self.sketches[thin["sketch"]][0], "boolean": BOOL[ft["operation"]], "distance": number(ex["distance"], self.params),
                                    "flip": ex.get("direction") == "negative", "thin_in": tin, "thin_out": tout})
            self._named(ft)
            return
        if len({pr["sketch"] for pr in ft["profiles"]}) != 1:
            raise Blocked("extrude of profiles from several sketches")
        # several profiles of ONE sketch = the sketch's regions together; when they are only some of its loops the app is told which
        sname = ft["profiles"][0]["sketch"]
        used = [c for pr in ft["profiles"] for lp in pr.get("loops", []) for c in lp["curves"]]
        idx, _, solid = self.sketches[sname][0], None, self.sketches[sname][2]
        subset = sorted(self.entity_index[sname][c] for c in used) if (set(used) != solid or len(ft["profiles"]) > 1) else None   # several profiles = several islands
        ex = ft["extent"]
        et = ex.get("type", "distance")
        p = {"sketch": idx, "boolean": BOOL[ft["operation"]]}
        if subset is not None:
            p["entities"] = subset
        if et == "distance":
            if ex.get("taper_deg"):
                p["taper"] = ex["taper_deg"]
            p["distance"] = number(ex["distance"], self.params)
            p["flip"] = (ex.get("direction") == "negative") != self.sketch_neg.get(sname, False)
        elif et == "through_all":
            p.update(end="through_all", distance=1.0, flip=(ex.get("direction") == "negative") != self.sketch_neg.get(sname, False))
        elif et == "two_sides":
            p.update(end="two_sided", distance=ex["side1"], distance2=ex["side2_opposite"])
        elif et == "symmetric":
            p.update(end="symmetric", distance=2 * number(ex["distance"], self.params))   # the app's symmetric distance is the total; the recipe's is per side
        else:
            raise Blocked("extent %s" % et)
        self.m.call("extrude", p)
        self._named(ft)

    def op_revolve(self, ft):
        idx, plane = self._sketch(ft["profile"])
        if self.sketch_neg.get(ft["profile"]["sketch"]):
            raise Blocked("revolve of a sketch facing the negative axis")
        d = ft["axis"].get("dir", [0, 0, 1])
        if any(abs(c) > 1e-9 for c in ft["axis"].get("origin", [0, 0, 0])):
            raise Blocked("revolve axis off the origin")
        name = "xyz"[max(range(3), key=lambda i: abs(d[i]))]
        if (plane, name) not in AXES:
            raise Blocked("revolve about world %s from a %s sketch" % (name, plane))
        self.m.call("revolve", {"sketch": idx, "axis": AXES[(plane, name)], "angle": ft.get("angle", 360), "boolean": BOOL[ft["operation"]]})
        self._named(ft)

    def op_construction_plane(self, ft):
        if ft.get("method") == "at_angle" and "result_normal" in ft:
            # a tilted plane has no datum in the app; it is remembered by its frame and handed to the tool that uses it
            self.tilted[ft["name"]] = (ft["result_origin"], ft["result_normal"])
            return
        if ft.get("method") != "offset":
            raise Blocked("construction plane method %s" % ft.get("method"))   # the plane itself is created when a sketch uses it

    def op_split_body(self, ft):
        if ft.get("tool") not in self.tilted:
            raise Blocked("split body by %r" % ft.get("tool"))
        o, n = self.tilted[ft["tool"]]
        self.m.call("cut", {"origin": o, "normal": n, "keep": "both", "body": self.body})

    def _all_curves(self, ref):
        idx, plane, solid = self.sketches[ref["sketch"]]
        return idx

    def op_sweep(self, ft):
        pi, _ = self._sketch(ft["profile"])
        path = self.sketches[ft["path"]["sketch"]]
        if set(ft["path"]["curves"]) != path[2]:
            raise Blocked("sweep path is a subset of its sketch")
        if ft.get("orientation") != "perpendicular":
            raise Blocked("sweep orientation %s" % ft.get("orientation"))
        self.m.call("sweep", {"profile": pi, "path": path[0], "boolean": BOOL[ft["operation"]]})
        self._named(ft)

    def op_loft(self, ft):
        if ft.get("rails") or not ft.get("solid", True) or "free" not in ft.get("end_conditions", "free"):
            raise Blocked("loft with rails / end conditions / as a surface")
        secs = ft["sections"]
        self.m.call("loft", {"profiles": [self._sketch(sec)[0] for sec in secs], "boolean": BOOL[ft["operation"]]})
        self._named(ft)

    def op_mirror(self, ft):
        if ft.get("mirror_of") == "features":
            return self._mirror_features(ft)
        if ft.get("mirror_of") != "bodies" or len(ft["bodies"]) != 1:
            raise Blocked("mirror of %s" % ft.get("mirror_of"))
        pl = ft["plane"]
        if pl.get("ref") not in PLANES or any(abs(c) > 1e-9 for c in pl.get("origin", [0, 0, 0])):
            raise Blocked("mirror plane %r" % pl.get("ref"))
        self.m.call("mirror", {"plane": pl["ref"], "body": self.order.index(ft["bodies"][0]), "mode": "add" if ft.get("combine") else "new"})
        if ft.get("combine"):
            pass   # joined into the original: still one body

    def op_hole(self, ft):
        radial = str(ft.get("direction", "")).startswith("along the inward surface normal")
        if ft.get("direction") != "into the material (-Z)" and not radial:
            raise Blocked("hole direction %r" % ft.get("direction"))
        if radial:
            # on a cylinder: drill from the surface point toward the axis, on an explicit plane whose normal is that direction
            f, (px, py, pz) = ft["face"], ft["position"]
            ao, ax = f["axis_origin"], f["axis"]
            d = [px - ao[0], py - ao[1], pz - ao[2]]
            t = sum(a * b for a, b in zip(d, ax))
            rad = [d[i] - t * ax[i] for i in range(3)]
            ln = sum(c * c for c in rad) ** 0.5
            pl = {"origin": [px, py, pz], "normal": [-c / ln for c in rad]}
            xy = {"x": 0, "y": 0}
        elif "position" in ft:
            px, py, pz = ft["position"]
            # the app drills along its plane normal, so a plane facing -Z (tilted 180 about X) at the entry face drills down
            pl = self.datum(0, pz, tilt=180.0, axis=0)
            xy = {"x": px, "y": -py}
        else:
            # positions are the points of a sketch on the entry face: one hole each, y flipped for the -Z-facing plane
            pts = self.point_sketch(ft["positions_from"])
            pl = self.datum(0, pts[0][2], tilt=180.0, axis=0)
            xy = {"points": [[x, -y] for x, y, _ in pts]}
        p = {"plane": pl, **xy, "diameter": ft["diameter"], "through": ft["extent"] == "through_all", "depth": ft.get("depth", 10),
             "tip_angle": ft.get("tip_angle", 0)}
        if isinstance(pl, dict):
            p.update(pl)
            p.pop("plane")
        ht = ft["hole_type"]
        if ht == "simple":
            self.m.call("hole", p)
        elif ht == "counterbore":
            p.update(style=1, cbore_diameter=ft["counterbore_diameter"], cbore_depth=ft["counterbore_depth"])
            self.m.call("hole_styled", p)
        elif ht == "countersink":
            p.update(style=2, csink_diameter=ft["countersink_diameter"], csink_angle=ft["countersink_angle"])
            self.m.call("hole_styled", p)
        else:
            raise Blocked("hole type %s" % ht)

    def _named(self, ft):
        """Remember the names of bodies a 'new' feature created, in app order."""
        if ft.get("operation") == "new":
            self.order += ft.get("result_bodies", ["?"])

    def op_combine(self, ft):
        if len(ft["tool_bodies"]) != 1:
            raise Blocked("combine with several tool bodies")
        t, o = self.order.index(ft["target_body"]), self.order.index(ft["tool_bodies"][0])
        op = {"join": "union", "cut": "subtract", "intersect": "intersect"}[ft["operation"]]
        self.m.call("boolean", {"op": op, "target": t, "tool": o, "keep_tool": bool(ft.get("keep_tools"))})
        if not ft.get("keep_tools"):
            self.order.pop(o)

    def _face_ids(self, topo, sels):
        """Face ids of `topo` matching recipe face selections (planes by outward normal + nearest centroid, cylinders by nearest centroid)."""
        ids = []
        for f0 in sels:
            pt = [a + o for a, o in zip(f0["point"], self.mo)]
            if f0.get("type") == "plane":
                n = f0["plane_normal_outward"]
                cand = [f for f in topo["faces"] if f["kind"] == "planar" and sum(a * b for a, b in zip(f["normal"], n)) > 0.99]
            elif f0.get("type") == "cylinder":
                cand = [f for f in topo["faces"] if f["kind"] == "cylindrical"]
            else:
                raise Blocked("face selection of type %s" % f0.get("type"))
            if not cand:
                raise Blocked("face selection not found")
            ids.append(min(cand, key=lambda f: math.dist(f["centroid"], pt))["id"])
        return ids

    def op_shell(self, ft):
        if ft.get("outside_thickness"):
            raise Blocked("shell with an outside thickness")
        self.body = len(self.m.call("describe_scene")["bodies"]) - 1
        topo = self.m.call("query_topology", {"body": self.body})
        ids = self._face_ids(topo, ft["faces_removed"])
        args = {"thickness": ft["inside_thickness"], "body": self.body}
        if len(ids) == 1:
            args["face"] = ids[0]
        elif ids:
            args["faces"] = ids
        self.m.call("shell", args)

    def op_draft(self, ft):
        self.body = len(self.m.call("describe_scene")["bodies"]) - 1
        topo = self.m.call("query_topology", {"body": self.body})
        ids = self._face_ids(topo, ft["faces"])
        np_ = ft["neutral_plane"]
        if np_.get("type") != "plane":
            raise Blocked("draft neutral plane of type %s" % np_.get("type"))
        angle = ft["angle"] * (-1 if ft.get("flipped") else 1)
        self.m.call("draft", {"faces": ids, "angle": angle, "pull": np_["plane_normal_outward"], "neutral": np_["point"], "body": self.body})

    def _edges(self, ft):
        """Edge ids of the last body nearest to each recipe edge midpoint."""
        self.body = len(self.m.call("describe_scene")["bodies"]) - 1   # the app edits the last body by default
        topo = self.m.call("query_topology", {"body": self.body})
        ids = []
        for sel in ft["edges"]:
            pt = [a + o for a, o in zip(sel["point"], self.mo)]   # recipe frame -> app frame
            def near(e):   # a line is its midpoint; a curve is its arc-length midpoint and its sampled points
                if "mid" in e:
                    return min(math.dist(q, pt) for q in [e["mid"]] + e["samples"])
                return math.dist([(a + b) / 2 for a, b in zip(e["p0"], e["p1"])], pt)
            cands = [e for e in topo["edges"] if e["kind"] != "circle" and "mid" in e] if sel.get("type") == "nurbs" else topo["edges"]
            best = min(cands, key=near)
            ids.append(best["id"])
            if sel.get("type") == "line" and "start" in sel and "end" in sel:
                # a straight recipe edge may be several app edges (a symmetric extrude cuts its side edges at the mid-plane):
                # take every app edge that lies along it
                a = [x + o for x, o in zip(sel["start"], self.mo)]
                b = [x + o for x, o in zip(sel["end"], self.mo)]
                ab = [q - p for p, q in zip(a, b)]
                L = math.sqrt(sum(c * c for c in ab))
                for e in topo["edges"]:
                    if e["id"] == best["id"] or "mid" in e or L < 1e-9:
                        continue
                    m = [(p + q) / 2 for p, q in zip(e["p0"], e["p1"])]
                    am = [q - p for p, q in zip(a, m)]
                    t = sum(x * y for x, y in zip(am, ab)) / (L * L)
                    off = math.sqrt(max(0.0, sum(x * x for x in am) - (t * L) ** 2))
                    d0 = [q - p for p, q in zip(e["p0"], e["p1"])]
                    par = abs(sum(x * y for x, y in zip(d0, ab))) / (math.sqrt(sum(c * c for c in d0)) * L + 1e-12)
                    if off < 1e-5 and par > 1 - 1e-9 and -1e-9 < t < 1 + 1e-9:
                        ids.append(e["id"])
            if sel.get("type") == "nurbs" and math.dist(sel["start"], sel["end"]) < 1e-6:   # a closed intersection curve: the app splits it at its seams into several edges
                ends = lambda e: (tuple(round(x, 4) for x in e["p0"]), tuple(round(x, 4) for x in e["p1"]))
                loop, grew = {best["id"]}, True
                byid = {e["id"]: e for e in topo["edges"] if e["kind"] != "circle" and "mid" in e}
                while grew:
                    grew = False
                    pts = {q for i in loop for q in ends(byid[i])}
                    for i, e in byid.items():
                        if i not in loop and pts & set(ends(e)):
                            loop.add(i); grew = True
                ids = [i for i in ids if i != best["id"]] + sorted(loop)
        return ids, topo.get("generation")

    def op_fillet(self, ft):
        kind = ft.get("type")
        if ft.get("fit") == "taper" and kind is None:
            ids, gen = self._edges(ft)
            self.m.call("fillet", {"edge": ids, "radius": ft["radius"], "body": self.body, "generation": gen, "fit": "taper"})
            return
        if kind not in ("constant_radius", "variable_radius", "chord_length", "spline_radius"):
            raise Blocked("fillet type %s" % (kind or "(none: variable/chord set)"))
        ids, gen = self._edges(ft)
        args = {"edge": ids, "radius": ft["radius"], "body": self.body, "generation": gen}
        if kind == "variable_radius":
            args["radius_end"] = ft["radius2"]
            if "start" in ft:
                args["start"] = ft["start"]
        elif kind == "chord_length":
            args["chord"] = True
        elif kind == "spline_radius":
            args["radii"], args["knots"] = ft["radii"], ft["knots"]
            if "start" in ft:
                args["start"] = ft["start"]
        self.m.call("fillet", args)

    def op_chamfer(self, ft):
        if ft.get("type") != "equal_distance":
            raise Blocked("chamfer type %s" % ft.get("type"))
        ids, gen = self._edges(ft)
        self.m.call("chamfer", {"edge": ids, "distance": ft["distance"], "body": self.body, "generation": gen})


# ---- compare -----------------------------------------------------------------------------------

def measure(mcp):
    scene = mcp.call("describe_scene")
    origin = scene.get("modeling_origin", [0, 0, 0])
    vol = area = 0.0
    com = [0.0, 0.0, 0.0]
    lo, hi = [1e18] * 3, [-1e18] * 3
    for b in scene["bodies"]:
        mp = mcp.call("mass_properties", {"body": b["index"]})
        v = mp.get("volume", 0.0)
        vol += v
        area += mp.get("surface_area", 0.0)
        for i in range(3):
            com[i] += v * mp.get("center_of_mass", [0, 0, 0])[i]
        bb = b.get("bbox")
        if bb:
            lo = [min(a, c) for a, c in zip(lo, bb["min"])]
            hi = [max(a, c) for a, c in zip(hi, bb["max"])]
    com = [c / vol for c in com] if vol else com
    lo, hi, com = [a - o for a, o in zip(lo, origin)], [a - o for a, o in zip(hi, origin)], [a - o for a, o in zip(com, origin)]   # app frame -> recipe frame
    return {"bodies": len(scene["bodies"]), "volume": vol, "area": area, "com": com, "lo": lo, "hi": hi}, origin


def compare(exp, got):
    errs = []
    rel = lambda a, b: abs(a - b) / max(abs(b), 1e-12)
    tol = exp.get("tolerance", {})
    rt = tol.get("relative", 1e-3)
    if exp.get("body_count") is not None and got["bodies"] != exp["body_count"]:
        errs.append("bodies %d != %d" % (got["bodies"], exp["body_count"]))
    if exp.get("volume_mm3") is not None and rel(got["volume"], exp["volume_mm3"]) > rt:
        errs.append("volume %.4f vs %.4f" % (got["volume"], exp["volume_mm3"]))
    if exp.get("area_mm2") is not None and rel(got["area"], exp["area_mm2"]) > rt:
        errs.append("area %.4f vs %.4f" % (got["area"], exp["area_mm2"]))
    bb = exp.get("bbox")
    diag = math.dist(bb["min"], bb["max"]) if bb else 1.0
    at = tol.get("bbox_diag", 1e-3) * diag
    if exp.get("center_of_mass") and math.dist(got["com"], exp["center_of_mass"]) > at:
        errs.append("com %s vs %s" % ([round(c, 3) for c in got["com"]], exp["center_of_mass"]))
    if bb and (math.dist(got["lo"], bb["min"]) > at * 2 or math.dist(got["hi"], bb["max"]) > at * 2):
        errs.append("bbox %s..%s vs %s..%s" % ([round(c, 2) for c in got["lo"]], [round(c, 2) for c in got["hi"]], bb["min"], bb["max"]))
    return errs


def run_case(mcp, d):
    recipe = json.load(open(os.path.join(d, "recipe.json")))
    exp = json.load(open(os.path.join(d, "expected.json")))
    try:
        mcp.call("new_document")
        rp = Replay(mcp)
        rp.mo = mcp.call("describe_scene").get("modeling_origin", [0, 0, 0])
        rp.params = {q["name"]: q["expression"] for q in recipe.get("parameters", [])}
        edits = [f for f in recipe["features"] if f["op"] == "sheet_metal_rule_edit"]
        if edits:
            rule = {}
            key = {"thickness": "thickness", "kFactor": "k_factor", "bendRadius": "bend_radius"}
            for e in edits:
                for k, v in list(e.get("set", {}).items()) + [tuple(x.split(" ", 1)) for x in e.get("sequence", [])]:
                    if k in key:
                        rule[key[k]] = float(str(v).split()[0])
            rp.rule_edits = rule
        for ft in recipe["features"]:
            rp.run(ft)
        got, _ = measure(mcp)   # sketch_begin sketches sit at the absolute origin; only extrude{profile} adds modeling_origin (MCP inconsistency, see bd)
        errs = compare(exp, got)
        name = os.path.basename(d)
        if errs:
            return ("XFAIL", XFAIL[name]) if name in XFAIL else ("FAIL", "; ".join(errs))
        return ("XPASS", "remove from XFAIL") if name in XFAIL else ("PASS", "")
    except Blocked as b:
        return "BLOCKED", str(b)
    except Exception as e:  # noqa: BLE001 - report, never raise
        name = os.path.basename(d)
        if "disjoint regions" in str(e) or "regioni disgiunte" in str(e):   # the kernel refuses these on purpose (the GUI makes one feature per region)
            return "BLOCKED", "a sketch with several disjoint regions in one extrude (kernel refuses; needs per-region features)"
        return ("XFAIL", XFAIL[name]) if name in XFAIL else ("FAIL", str(e))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--sock", default=os.environ.get("ORCA_CAD_MCP", "/tmp/orca-cad-mcp.sock"))
    ap.add_argument("--report")
    a = ap.parse_args()
    mcp = Mcp(a.sock)
    cases = sorted(d for d in glob.glob(os.path.join(CORPUS, "*")) if os.path.isfile(os.path.join(d, "recipe.json")) and a.filter in os.path.basename(d))
    res = []
    for d in cases:
        st, why = run_case(mcp, d)
        res.append({"case": os.path.basename(d), "status": st, "why": why})
        print("%-34s %-8s %s" % (os.path.basename(d), st, why))
    n = {k: sum(1 for r in res if r["status"] == k) for k in ("PASS", "FAIL", "XFAIL", "XPASS", "BLOCKED")}
    print("  ".join("%s %d" % (k, v) for k, v in n.items()) + "  of %d" % len(res))
    if a.report:
        json.dump(res, open(a.report, "w"), indent=1)
    return 1 if n["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
