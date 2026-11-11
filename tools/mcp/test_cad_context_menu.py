#!/usr/bin/env python3
"""Checks that the Design tab's right-click offer adapts to what was clicked and lists exactly the
verbs the offer table says apply.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_context_menu.py [rung ...]

With no rung named, every rung runs. The offer is where a sketch's verbs live, and for the many
that have no key it is the only way to them. Each rung opens a sketch on the XY plane, right-clicks
it with mouse_gesture, which lists the menu the click opens without showing it, and runs an item
by its label path with popup_item. The menu is graded against call_cad_tool list_verbs, the
compiled offer table the menu is built from, read for the selection the right-click left; what a
verb drew is graded on the geometry sketch_describe reads back, or for a text on the feature
describe_feature reads. Every action goes through the MCP UI tools, and call_cad_tool only reads.
Exits 0 when every check holds.

The rungs: kinds right-clicks empty space, a line, a circle, a point and two picked lines, and
checks that the menu's header names each, and that a right-click while a line's first point is
down abandons the point and opens nothing. vocabulary checks for those five selections that the
menu lists exactly the label paths the table gives, greys exactly the rows with nothing to offer,
and that the five offer different verbs. author draws a 120 x 80 rectangle with the corner
rectangle chosen from the menu, and no_shortcut one with the centre rectangle, which has no key,
and checks it is centred on its click. curves draws each keyless creation verb (polyline, oblique
and rounded rectangle, 2- and 3-point circle, centre and tangent arc, arc slot, elliptical arc,
polygons of 3, 4, 5, 8 and 12 sides, inscribed and circumscribed) and grades the construction.
transforms moves a line 25, rotates it 30 degrees, scales it 3 times and arrays it at a pitch of 20
four times and at 360 degrees six times, each through its gizmo's value labels. art types "LT"
into the Text dialog twice, cancelling the first text on the Insert step, which must leave no text,
and keeping the second, and imports a 40 x 20 SVG through its file picker, which a rule answers
with the fixture's path (the import is skipped on Linux, where a GTK picker can't take one).
coverage checks that the verbs the rungs drive from the menu are the sketch verbs the table has no
key for.
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orca_mcp  # noqa: E402
from cad_gui import CANVAS, CadGui, until  # noqa: E402

gui = None  # the CadGui main() starts

# The offer's header for each sketch selection: its first item, greyed, naming what the rows act
# on (DesignPanel::offer_header). Keyed by the selection kinds of the offer table (OfferSel).
HEADER = {"SkNone": "Sketch — nothing selected", "SkLine": "Sketch line", "SkArc": "Sketch curve",
          "SkPoint": "Sketch point", "Sk2Ent": "2 sketch entities"}


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


def spread(vals):
    return max(vals) - min(vals)


def lengths(ents):
    return sorted(round(e["length"], 6) for e in ents if e["type"] == "line")


def loops():
    return gui.sketch()["closed_loops"]


def ents(kind=None):
    return [e for e in gui.sketch()["entities"] if kind is None or e["type"] == kind]


def key(k):
    """A tool key or Esc, pressed over the canvas."""
    gui.keys(k, target=CANVAS)


# ---------------------------------------------------------------- the table, as the menu lays it out

def verb_paths(verbs):
    """The label path at which the offer lists each verb that applies, by verb id. `verbs` is
    list_verbs' sketch vocabulary, read with the selection the right-click left.

    It follows DesignPanel::show_offer_menu. A row with two or more verbs for the selection is a
    submenu named for the row, holding each verb directly or, for a verb of a family (Rectangle:
    corner, centre, oblique, rounded), in a submenu named for the family. A row with exactly one is
    that verb alone at the top level. A row with none is greyed (expected_greyed()). No sketch verb
    is held back by the document or sits in a flat row, so a verb is listed exactly when it
    applies."""
    if not all(v["sketch_mode"] for v in verbs):
        raise ValueError("verb_paths lays out the sketch offer: give it list_verbs' sketch verbs")
    rows = {}
    for v in verbs:
        rows.setdefault(v["row"], []).append(v)
    out = {}
    for row in rows.values():
        live = [v for v in row if v["applies"]]
        if len(live) == 1:
            out[live[0]["id"]] = live[0]["name"]
            continue
        for v in live:
            out[v["id"]] = ">".join([v["row_name"]] + ([v["family"]] if v["family"] else []) + [v["name"]])
    return out


def expected_paths(verbs):
    """The label paths the offer lists for the verbs that apply (verb_paths())."""
    return set(verb_paths(verbs).values())


def expected_greyed(verbs):
    """The label paths the offer greys: each row none of whose verbs applies, by the row's name,
    and a verb that applies but has no GUI route, at its path."""
    paths = verb_paths(verbs)
    rows = {}
    for v in verbs:
        rows.setdefault(v["row"], []).append(v)
    out = {row[0]["row_name"] for row in rows.values() if not any(v["applies"] for v in row)}
    return out | {paths[v["id"]] for v in verbs if v["applies"] and not v["action"]}


def offered(offer, verbs):
    """The ids of the verbs `offer` lists as items that can be run."""
    live = set(offer.items) - set(offer.disabled)
    return {verb for verb, path in verb_paths(verbs).items() if path in live}


def choose(offer, X, Y, verb):
    """Run `verb` from `offer`, the menu a right-click at (X, Y) mm listed: the same right-click
    again, running the item at the verb's label path. A verb the menu does not list as an item that
    can be run raises, so a rung never runs a neighbour in its place."""
    path = verb_paths(gui.verbs()).get(verb)
    if path is None or path not in offer.items or path in offer.disabled:
        raise AssertionError(f"{verb} is not offered here ({offer.header}): path {path}, menu {offer.items}")
    reply = gui.choose(X, Y, path)
    if reply.get("ran") != path:
        raise AssertionError(f"choosing {path} ran {reply.get('ran')}: {reply}")


# ---------------------------------------------------------------- fixtures

def keep_as_drawn():
    """Close the value fields the last gesture left open, keeping the geometry as drawn; returns how
    many Esc presses that took.

    Esc means keep-as-drawn only while a field is open: with none open it drops the armed tool,
    and from Select it leaves the sketch. So Esc is pressed only on a field that is there. The
    fields come one after another (a rectangle queues Width, then Height, the next opened as the
    previous one closes), and one that has not opened yet looks like one that never will, so this
    stops only when no field has shown for a while."""
    for n in range(8):
        if until(gui.value_field, timeout=0.7) is None:
            return n
        key("Escape")
        until(lambda: gui.value_field() is None)
    raise AssertionError("value fields kept opening after 8 Esc presses")


def clear_sketch():
    """Empty the open sketch through the UI, keeping it open on the same plane and view.

    A sketch has no select-all (Ctrl+A selects nothing, nor does a drag in Select), so the ribbon's
    Delete selected cannot take everything in one go. Cancel discards the sketch instead, and a new
    one opens on the XY plane and is calibrated again. The camera has not moved, so plane points
    computed before keep their pixels."""
    keep_as_drawn()
    if gui.sketch()["entities"]:
        gui.cancel()
        gui.enter_sketch()
    left = len(gui.sketch()["entities"])
    if left:
        raise AssertionError(f"the sketch still holds {left} entities")


# Which tool each creation verb arms. A verb run from the wrong item arms a neighbouring tool and
# then draws something plausible with it; checking the armed tool fails that where it happens.
ARMS = {"sk_polyline": "polyline", "sk_rect": "rect_corner", "sk_rect_center": "rect_center",
        "sk_rect_oblique": "rect_oblique", "sk_rect_rounded": "rect_rounded",
        "sk_circle_2pt": "circle_2pt", "sk_circle_3pt": "circle_3pt",
        "sk_arc_tangent": "arc_tangent", "sk_arc_center": "arc_center",
        "sk_slot_arc": "slot_arc", "sk_ellipse_arc": "ellipse_arc",
        "sk_poly_3": "polygon", "sk_poly_4": "polygon", "sk_poly_5": "polygon",
        "sk_poly_8": "polygon", "sk_poly_12": "polygon",
        "sk_move": "move", "sk_rotate": "rotate", "sk_scale": "scale",
        "sk_array": "array", "sk_array_polar": "array_polar"}


def arm(rung, verb, X, Y, check_tool=True):
    """Right-click (X, Y) mm and run `verb` from the offer, with no key pressed; returns the offer.
    For a verb in ARMS, checks the tool it armed."""
    o = gui.offer(X, Y)
    if o.header is None:
        d = gui.sketch()
        raise AssertionError(f"the offer did not open for {verb} (tool {d['tool']}, pending {d['pending']})")
    choose(o, X, Y, verb)
    if check_tool and verb in ARMS:
        got = gui.sketch()["tool"]
        gui.check(f"{rung}: {verb} armed the {ARMS[verb]} tool", got == ARMS[verb], f"tool {got}")
    return o


def mid(e):
    return ((e["p0"][0] + e["p1"][0]) / 2.0, (e["p0"][1] + e["p1"][1]) / 2.0)


def poly_click(pt):
    """One click of a multi-segment tool, then close the value field that click opened. The
    polyline opens a Length field after every segment, and the canvas takes no click while one is
    open."""
    gui.click_mm(*pt)
    keep_as_drawn()


def draw_line_at(x0, y0, x1, y1, length):
    """One line clicked from (x0, y0) to (x1, y1) and committed by typing its length and an angle of
    0; returns the line as sketch_describe reads it. The typed length is a dimension the sketch
    solves by moving the line's ends, so the line is not where the clicks put it, and the clicks
    that pick it are placed on the line read back. The tool stays armed, and the fields are closed
    before the caller clicks again."""
    key("l")
    gui.click_mm(x0, y0)
    gui.click_mm(x1, y1)
    gui.values(int(length), 0)
    keep_as_drawn()
    return ents("line")[-1]


def leave():
    """Leave the sketch, discarding what it holds, and empty the design."""
    gui.cancel()
    gui.new_design()


def safe_box():
    """The calibrated region (x0, x1, y0, y1) and its centre."""
    x0, x1, y0, y1 = gui.safe
    return x0, x1, y0, y1, (x0 + x1) / 2.0, (y0 + y1) / 2.0


# ---------------------------------------------------------------- rungs

def rung_kinds():
    """O1: the offer reads what was right-clicked, one kind of element at a time."""
    gui.enter_sketch("l")
    x0, x1, y0, y1, cx, cy = safe_box()
    W, H = x1 - x0, y1 - y0

    # Empty space first. A right-click on a draw tool does one of two jobs, depending on whether
    # an anchor is down: with none it opens the offer, so the offer is reachable from any armed
    # tool.
    hi = y1 - H * 0.12
    o = gui.offer(cx, hi)
    gui.check("kinds: Line armed, nothing anchored: a right-click opens the offer", o.header is not None,
              f"menu {o}")
    gui.check("kinds: on empty space it offers for nothing selected", o.header == HEADER["SkNone"],
              f"header {o.header!r}")
    names = {v["name"] for v in gui.verbs()}
    leaves = [p.split(">")[-1] for p in o.items if p not in o.disabled]
    gui.check("kinds: the sketch vocabulary is the one offered", leaves and all(n in names for n in leaves),
              f"items not named for a sketch verb: {[n for n in leaves if n not in names]}")

    # With an anchor down, the same gesture abandons the anchor instead and offers nothing.
    key("l")
    gui.click_mm(cx, hi)
    o = gui.offer(cx + W * 0.1, hi)
    gui.check("kinds: with an anchor down, a right-click abandons it and opens no offer",
              o.header is None and not o.items, f"menu {o}")
    o = gui.offer(cx, hi)
    gui.check("kinds: and the offer is back on the next right-click", o.header is not None, f"menu {o}")
    key("Escape")

    # A line.
    ax, ay = x0 + W * 0.15, cy
    bx = x0 + W * 0.55
    first = draw_line_at(ax, ay, bx, ay, 40)
    o = gui.offer(*mid(first))
    gui.check("kinds: on a line it offers for a line", o.header == HEADER["SkLine"], f"header {o.header!r}")

    # A circle: every curve takes the same vocabulary, which the header calls a curve.
    key("c")
    ccx, ccy = x0 + W * 0.30, cy + H * 0.25
    gui.click_mm(ccx, ccy)
    gui.click_mm(ccx + W * 0.10, ccy)
    gui.values(20)
    circ = ents("circle")
    gui.check("kinds: one circle drawn to right-click on", len(circ) == 1, f"{len(circ)} circles")
    if not circ:
        raise AssertionError("no circle to right-click on")
    r = circ[0]["radius"]
    o = gui.offer(circ[0]["center"][0] + r, circ[0]["center"][1])
    gui.check("kinds: on a circle it offers for a curve", o.header == HEADER["SkArc"], f"header {o.header!r}")

    # A point.
    key("p")
    pxx, pyy = x0 + W * 0.80, cy + H * 0.25
    gui.click_mm(pxx, pyy)
    o = gui.offer(pxx, pyy)
    gui.check("kinds: on a point it offers for a point", o.header == HEADER["SkPoint"], f"header {o.header!r}")

    # Two lines picked: the pair vocabulary (angle, equal, parallel) is about pairs of curves. The
    # second line's Length label sits above it, toward the first line, so the two lie a third of
    # the region apart, which keeps that label off the first line.
    y2 = ay - H * 0.32
    second = draw_line_at(ax, y2, bx, y2, 40)
    # One Esc drops the armed tool to Select, so the clicks below pick instead of draw; from a draw
    # tool with no anchor down that is all it does. Each click lands where it takes its line and
    # nothing else: clear of the Length labels, whose reach is in pixels, and of the other shapes.
    key("Escape")
    gui.click_mm(*gui.clear_pick(first))
    gui.click_mm(*gui.clear_pick(second), modifiers=["shift"])
    d = gui.sketch()
    gui.check("kinds: two entities picked", len(d["selection"]) == 2,
              f"selection {d['selection']} (tool {d['tool']}, pending {d['pending']}, editing {d['editing']})")
    o = gui.offer(*mid(first))
    d = gui.sketch()
    gui.check("kinds: with two picked it offers for the pair", o.header == HEADER["Sk2Ent"],
              f"header {o.header!r} (tool {d['tool']}, pending {d['pending']}, editing {d['editing']})")
    leave()


def rung_vocabulary():
    """O2: the menu lists exactly what the offer table says applies, selection by selection.

    Five selections, not one: a menu that is the same whatever was clicked would pass any single
    comparison, and only the five against their own tables tell those apart."""
    gui.enter_sketch("l")
    x0, x1, y0, y1, cx, cy = safe_box()
    W, H = x1 - x0, y1 - y0
    ax, ay = x0 + W * 0.15, cy
    bx = x0 + W * 0.55
    line = draw_line_at(ax, ay, bx, ay, 40)

    key("c")
    ccx, ccy = x0 + W * 0.30, cy + H * 0.25
    gui.click_mm(ccx, ccy)
    gui.click_mm(ccx + W * 0.10, ccy)
    gui.values(20)
    circ = ents("circle")
    if len(circ) != 1:
        raise AssertionError(f"{len(circ)} circles drawn, expected one to right-click on")
    circ = circ[0]

    key("p")
    pxx, pyy = x0 + W * 0.80, cy + H * 0.25
    gui.click_mm(pxx, pyy)

    where = [("SkNone", cx, y1 - H * 0.12),
             ("SkLine", *mid(line)),
             ("SkArc", circ["center"][0] + circ["radius"], circ["center"][1]),
             ("SkPoint", pxx, pyy),
             ("Sk2Ent", None, None)]
    seen = {}
    for name, X, Y in where:
        if name == "Sk2Ent":
            # sk_angdist accepts the pair and nothing else, so it is offered from this selection
            # alone.
            keep_as_drawn()
            key("Escape")
            # Each click lands where it takes its entity and nothing else (clear_pick): clear of
            # the line's Length label and the circle's radius label, and of the circle's radius
            # grip, a click on which starts a handle drag that replaces the selection.
            gui.click_mm(*gui.clear_pick(line))
            gui.click_mm(*gui.clear_pick(circ), modifiers=["shift"])
            picked = len(gui.sketch()["selection"])
            gui.check("vocabulary: two entities picked for the pair vocabulary", picked == 2, f"{picked} picked")
            X, Y = mid(line)
        o = gui.offer(X, Y)
        verbs = gui.verbs()
        want_grey = expected_greyed(verbs)
        want = expected_paths(verbs) | want_grey
        got = set(o.items)
        gui.check(f"vocabulary: {name}: the menu lists exactly the table's paths",
                  o.header == HEADER[name] and got == want and len(got) == len(o.items)
                  and set(o.disabled) == want_grey,
                  f"header {o.header!r}, {len(o.items)} items; menu only {sorted(got - want)}; "
                  f"table only {sorted(want - got)}; greyed {sorted(o.disabled)} vs {sorted(want_grey)}")
        seen[name] = offered(o, verbs)

    # The sets differ: an offer that adapts does not show the same rows everywhere.
    gui.check("vocabulary: every selection offers a different set",
              len(set(map(frozenset, seen.values()))) == len(seen),
              ", ".join(f"{k}={len(v)}" for k, v in seen.items()))
    added = seen["SkLine"] - seen["SkNone"]
    gui.check("vocabulary: a picked line adds verbs an empty pick has not", added,
              f"{len(added)} added: {', '.join(sorted(added)[:8])}")
    leave()


def rung_author():
    """O3: a closed profile drawn with the tool chosen from the offer; no tool key is pressed."""
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    w, h = 120.0, 80.0

    o = gui.offer(cx, cy)
    gui.check("author: an empty sketch offers for nothing selected", o.header == HEADER["SkNone"],
              f"header {o.header!r}")
    choose(o, cx, cy, "sk_rect")
    gui.click_mm(cx - w / 2.0, cy - h / 2.0)
    gui.click_mm(cx + w / 2.0, cy + h / 2.0)
    gui.values(int(w), int(h))
    ls = lengths(ents())
    gui.check("author: the sides are the typed 120 x 80", ls == [80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = loops()
    gui.check("author: one closed loop", len(lp) == 1, f"{len(lp)} closed loop(s)")
    gui.check("author: area 9600", len(lp) == 1 and near(abs(lp[0]["area"]), w * h, 1e-9),
              f"areas {[abs(l['area']) for l in lp]}")
    leave()


def rung_no_shortcut():
    """O4: a tool with no key at all, armed from the offer and graded on what it draws.

    Many sketch verbs have no shortcut, and for those the offer is the only way in. Arming the tool
    is not the check; the exact rectangle it then draws is."""
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()

    o = gui.offer(cx, cy)
    verbs = gui.verbs()
    ids = offered(o, verbs)
    keyless = {v["id"] for v in verbs if v["id"] in ids and not v["key"] and v["action"]}
    gui.check("no_shortcut: at least ten verbs offered on an empty sketch have no shortcut", len(keyless) >= 10,
              f"{len(keyless)} of the {len(ids)} offered")
    gui.check("no_shortcut: the centre rectangle is among them", "sk_rect_center" in keyless, sorted(keyless))

    w, h = 120.0, 80.0
    choose(o, cx, cy, "sk_rect_center")
    gui.click_mm(cx, cy)                      # centre
    gui.click_mm(cx + w / 2.0, cy + h / 2.0)  # corner
    gui.values(int(w), int(h))
    es = ents()
    ls = lengths(es)
    gui.check("no_shortcut: the sides are the typed 120 x 80", ls == [80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = loops()
    gui.check("no_shortcut: one closed loop of area 9600", len(lp) == 1 and near(abs(lp[0]["area"]), w * h, 1e-9),
              f"{len(lp)} closed loop(s), areas {[abs(l['area']) for l in lp]}")
    xs = sorted({round(p, 6) for e in es for p in (e["p0"][0], e["p1"][0])})
    ys = sorted({round(p, 6) for e in es for p in (e["p0"][1], e["p1"][1])})
    # Centred on the click to within the click itself: a click lands on a whole pixel, so the plane
    # point it names is only as exact as a pixel. The exact shape is checked above; this tells a
    # centre rectangle from a corner one, which a pixel or so does at 120 x 80.
    tol = 1.5 * gui.mm_per_px(cx, cy)
    mx, my = (xs[0] + xs[-1]) / 2.0, (ys[0] + ys[-1]) / 2.0
    gui.check(f"no_shortcut: centred on the click within 1.5 px ({tol:.3f} mm)",
              abs(mx - cx) <= tol and abs(my - cy) <= tol,
              f"off by {abs(mx - cx):.4f}, {abs(my - cy):.4f} mm")
    leave()


# ---------------------------------------------------------------- the keyless sketch vocabulary

def rung_curves():
    """O5: every creation verb that has no key, drawn from the offer and graded exactly.

    The checks are properties of the construction, which hold whatever pixel a click landed on:
    a regular polygon's sides are equal, a tangent arc's radius at the shared end is square to
    the line, a circumscribed polygon's circumradius is the inscribed one's over cos(pi/n). Where
    a check compares with a click, it allows the click's own rounding to a pixel. Where a value
    field opens, the typed value is graded exactly too."""
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    W, H = x1 - x0, y1 - y0
    free = (cx, y1 - H * 0.10)  # stays empty, to right-click on
    tol = 1.5 * gui.mm_per_px(cx, cy)  # 1.5 px: a click lands on a whole pixel

    # Polyline: a chain closed by clicking its start again, the shape a profile is.
    clear_sketch()
    arm("curves", "sk_polyline", *free)
    ring = [(cx - W * 0.20, cy - H * 0.15), (cx + W * 0.20, cy - H * 0.15),
            (cx + W * 0.20, cy + H * 0.15), (cx - W * 0.20, cy + H * 0.15)]
    for pt in ring:
        poly_click(pt)
    poly_click(ring[0])
    ls, lp = ents("line"), loops()
    gui.check("curves: sk_polyline closed by clicking its start: 4 lines, 1 closed loop",
              len(ls) == 4 and len(lp) == 1, f"{len(ls)} lines, {len(lp)} closed loop(s)")

    # Oblique rectangle: three clicks, the first edge skew, so that it is not axis-aligned.
    clear_sketch()
    arm("curves", "sk_rect_oblique", *free)
    gui.click_mm(cx - W * 0.20, cy - H * 0.10)
    gui.click_mm(cx + W * 0.15, cy + H * 0.05)
    gui.click_mm(cx - W * 0.10, cy + H * 0.20)
    q = ents("line")
    gui.check("curves: sk_rect_oblique drew 4 lines", len(q) == 4, f"{len(q)} lines")
    if len(q) == 4:
        L = sorted(round(e["length"], 9) for e in q)
        gui.check("curves: the oblique rectangle's opposite sides are equal to 1e-9", L[0] == L[1] and L[2] == L[3],
                  f"sides {L}")
        cosines = []
        for i in range(4):
            for j in range(i + 1, 4):
                u = (q[i]["p1"][0] - q[i]["p0"][0], q[i]["p1"][1] - q[i]["p0"][1])
                v = (q[j]["p1"][0] - q[j]["p0"][0], q[j]["p1"][1] - q[j]["p0"][1])
                cosines.append(abs(u[0] * v[0] + u[1] * v[1]) / (math.hypot(*u) * math.hypot(*v)))
        square = sum(1 for c in cosines if c < 1e-9)
        gui.check("curves: and its four corners are right angles to 1e-9", square == 4,
                  f"{square} perpendicular pairs")
        d0 = (q[0]["p1"][0] - q[0]["p0"][0], q[0]["p1"][1] - q[0]["p0"][1])
        gui.check("curves: and it is oblique", abs(d0[0]) > 1e-6 and abs(d0[1]) > 1e-6,
                  f"first edge at {math.degrees(math.atan2(d0[1], d0[0])):.3f} deg")

    # Rounded rectangle: four lines and four arcs of one radius; the third click sets the radius.
    clear_sketch()
    arm("curves", "sk_rect_rounded", *free)
    gui.click_mm(cx - W * 0.20, cy - H * 0.15)
    gui.click_mm(cx + W * 0.20, cy + H * 0.15)
    gui.click_mm(cx + W * 0.20 - W * 0.04, cy + H * 0.15)
    ls, ar = ents("line"), ents("arc")
    gui.check("curves: sk_rect_rounded drew 4 lines and 4 arcs", len(ls) == 4 and len(ar) == 4,
              f"{len(ls)} lines, {len(ar)} arcs")
    if len(ar) == 4:
        rr = sorted(round(e["radius"], 9) for e in ar)
        gui.check("curves: its four corners share one radius to 1e-9", spread(rr) == 0.0, f"radii {rr}")
        lp = loops()
        gui.check("curves: the rounded rectangle is one closed loop", len(lp) == 1, f"{len(lp)} closed loop(s)")
        if len(lp) == 1:
            # The four straight sides span the whole outer box (the top one runs from x_min + r
            # to x_max - r at y_max), so their extent is the rectangle's, and rounding takes off
            # the four corner squares less their quarter discs: r^2 (4 - pi).
            xs = [p for e in ls for p in (e["p0"][0], e["p1"][0])]
            ys = [p for e in ls for p in (e["p0"][1], e["p1"][1])]
            w, h, r = max(xs) - min(xs), max(ys) - min(ys), rr[0]
            want = w * h - r * r * (4 - math.pi)
            gui.check("curves: its area is W*H - r^2 (4 - pi)", near(abs(lp[0]["area"]), want, 1e-6),
                      f"area {abs(lp[0]['area']):.9f}, expected {want:.9f}")

    # Two-point circle: the two clicks are the ends of a diameter.
    clear_sketch()
    arm("curves", "sk_circle_2pt", *free)
    p_a, p_b = (cx - W * 0.18, cy - H * 0.10), (cx + W * 0.18, cy + H * 0.10)
    gui.click_mm(*p_a)
    gui.click_mm(*p_b)
    c2 = ents("circle")
    gui.check("curves: sk_circle_2pt drew 1 circle", len(c2) == 1, f"{len(c2)} circles")
    if c2:
        A, B = gui.landed(*p_a), gui.landed(*p_b)
        m = ((A[0] + B[0]) / 2.0, (A[1] + B[1]) / 2.0)
        off = math.dist(c2[0]["center"], m)
        gui.check("curves: it is centred on the middle of the two clicks within 1.5 px", off <= tol,
                  f"off by {off:.4f} mm, allowed {tol:.4f}")
        gui.check("curves: its radius is half the clicks' distance within 1.5 px",
                  abs(c2[0]["radius"] - math.dist(A, B) / 2.0) <= tol,
                  f"radius {c2[0]['radius']:.6f}, half the distance {math.dist(A, B) / 2.0:.6f}")
        field = until(gui.value_field)
        gui.check("curves: a radius field opens for it, as for the keyed circle", field is not None,
                  "no value field opened")
        if field is not None:
            gui.value(30)
            got = ents("circle")[0]["radius"]
            gui.check("curves: and it takes a typed radius of 30 exactly", near(got, 30.0, 1e-9), f"radius {got:.9f}")
            # A circle has three degrees of freedom, and its typed radius takes one of them.
            dof = gui.sketch()["dof"]
            gui.check("curves: the sketch reports the DoF of what it holds, 2", dof == 2, f"dof {dof}")

    # Three-point circle: all three clicks lie on it.
    clear_sketch()
    arm("curves", "sk_circle_3pt", *free)
    three = [(cx - W * 0.18, cy), (cx, cy + H * 0.18), (cx + W * 0.16, cy - H * 0.06)]
    for pt in three:
        gui.click_mm(*pt)
    c3 = ents("circle")
    gui.check("curves: sk_circle_3pt drew 1 circle", len(c3) == 1, f"{len(c3)} circles")
    if c3:
        ds = [math.dist(gui.landed(*pt), c3[0]["center"]) for pt in three]
        gui.check("curves: all three clicks lie on it within 1.5 px",
                  spread(ds) <= tol and abs(ds[0] - c3[0]["radius"]) <= tol,
                  f"distances {[round(d, 4) for d in ds]}, radius {c3[0]['radius']:.4f}")

    # Centre-point arc: centre, start, end.
    clear_sketch()
    arm("curves", "sk_arc_center", *free)
    C = (cx, cy)
    gui.click_mm(*C)
    gui.click_mm(cx + W * 0.15, cy)
    gui.click_mm(cx, cy + H * 0.15)
    aa = ents("arc")
    gui.check("curves: sk_arc_center drew 1 arc", len(aa) == 1, f"{len(aa)} arcs")
    if aa:
        off = math.dist(aa[0]["center"], gui.landed(*C))
        gui.check("curves: it is centred on the first click within 1.5 px", off <= tol, f"off by {off:.4f} mm")
        for name, pt in (("start", aa[0]["p0"]), ("end", aa[0]["p1"])):
            gap = abs(math.dist(pt, aa[0]["center"]) - aa[0]["radius"])
            gui.check(f"curves: the centre arc's {name} sits on its radius to 1e-9", gap < 1e-9, f"off by {gap:.3e}")

    # Tangent arc: it leaves a line's end square to the radius there, whatever the click.
    clear_sketch()
    # The line it leaves from, drawn by key. Its typed length moves both its ends, so the arc
    # starts on the end read back, not on the one clicked.
    line = draw_line_at(cx - W * 0.20, cy - H * 0.05, cx + W * 0.05, cy - H * 0.05, 40)
    key("Escape")
    arm("curves", "sk_arc_tangent", *free)
    gui.click_mm(*line["p1"])  # the start snaps onto the line's end
    gui.click_mm(cx + W * 0.12, cy + H * 0.12)
    ta = ents("arc")
    gui.check("curves: sk_arc_tangent drew 1 arc off the line's end", len(ta) == 1, f"{len(ta)} arcs")
    if ta:
        end = min((ta[0]["p0"], ta[0]["p1"]), key=lambda p: math.dist(p, line["p1"]))
        rad = (end[0] - ta[0]["center"][0], end[1] - ta[0]["center"][1])
        d = (line["p1"][0] - line["p0"][0], line["p1"][1] - line["p0"][1])
        cosang = abs(rad[0] * d[0] + rad[1] * d[1]) / (math.hypot(*rad) * math.hypot(*d))
        gui.check("curves: its radius at the shared end is square to the line to 1e-9", cosang < 1e-9,
                  f"cos {cosang:.2e}")

    # Arc slot: two rails about one centre, and two end caps about their own.
    clear_sketch()
    arm("curves", "sk_slot_arc", *free)
    gui.click_mm(cx - W * 0.15, cy)           # start
    gui.click_mm(cx, cy - H * 0.10)           # centre
    gui.click_mm(cx + W * 0.15, cy)           # end direction
    gui.click_mm(cx + W * 0.15, cy + H * 0.04)  # width
    sa = ents("arc")
    gui.check("curves: sk_slot_arc drew at least 2 arcs", len(sa) >= 2, f"{len(sa)} arcs")
    if len(sa) >= 2:
        # Grouped by centre, not by size: the two largest arcs may be a rail and a cap.
        groups = {}
        for e in sa:
            groups.setdefault((round(e["center"][0], 9), round(e["center"][1], 9)), []).append(e["radius"])
        rails = max(groups.values(), key=len)
        gui.check("curves: two of its arcs, the rails, share one centre to 1e-9", len(rails) == 2,
                  f"rail radii {[round(r, 6) for r in sorted(rails)]}, {len(groups) - 1} other centre(s)")

    # Elliptical arc: five clicks, both ends on the ellipse.
    clear_sketch()
    arm("curves", "sk_ellipse_arc", *free)
    for pt in ((cx, cy), (cx + W * 0.18, cy), (cx, cy + H * 0.10), (cx + W * 0.18, cy), (cx, cy + H * 0.10)):
        gui.click_mm(*pt)
    ea = ents("ellipse_arc")
    gui.check("curves: sk_ellipse_arc drew 1 elliptical arc", len(ea) == 1, f"{len(ea)} elliptical arcs")
    if ea and "radius" in ea[0]:
        e = ea[0]
        gui.check("curves: its semi-axes have a > b > 0", e["radius"] > e["rminor"] > 0,
                  f"a {e['radius']:.6f}, b {e['rminor']:.6f}")
        for name, pt in (("start", e["p0"]), ("end", e["p1"])):
            X = (pt[0] - e["center"][0], pt[1] - e["center"][1])
            ph = e["rotation"]
            u = (X[0] * math.cos(ph) + X[1] * math.sin(ph)) / e["radius"]
            v = (-X[0] * math.sin(ph) + X[1] * math.cos(ph)) / e["rminor"]
            gui.check(f"curves: the elliptical arc's {name} satisfies (x/a)^2 + (y/b)^2 = 1 to 1e-9",
                      abs(u * u + v * v - 1.0) < 1e-9, f"(x/a)^2 + (y/b)^2 = {u * u + v * v:.12f}")

    # The fixed-count polygons: regular, all vertices on one circle about the centre click.
    for verb, n in (("sk_poly_3", 3), ("sk_poly_4", 4), ("sk_poly_5", 5), ("sk_poly_8", 8), ("sk_poly_12", 12)):
        clear_sketch()
        arm("curves", verb, *free)
        gui.click_mm(cx, cy)
        gui.click_mm(cx + W * 0.15, cy)
        q = ents("line")
        if len(q) != n:
            gui.check(f"curves: {verb} drew {n} sides", False, f"{len(q)} sides")
            continue
        L = [round(e["length"], 9) for e in q]
        ctr = gui.landed(cx, cy)
        R = [math.dist(e["p0"], ctr) for e in q]
        gui.check(f"curves: {verb} drew {n} sides equal to 1e-9, its vertices on one circle within 1.5 px",
                  spread(L) == 0.0 and spread(R) < tol, f"sides {L}, vertex distances spread {spread(R):.4f} mm")

    # Inscribed and circumscribed, a parameter of the polygon tool chosen from the offer: the same
    # two clicks give circumradii in the ratio 1/cos(pi/5) for a pentagon. Each is measured from
    # the polygon's own centre, the mean of its vertices: the centre click lands within a pixel of
    # the sketch's origin, which the view is centred on, and snaps onto it.
    radii = {}
    for verb, fit in (("sk_poly_inscribed", "inscribed"), ("sk_poly_circumscribed", "circumscribed")):
        clear_sketch()
        arm("curves", verb, *free)
        arm("curves", "sk_poly_5", *free)
        gui.click_mm(cx, cy)
        gui.click_mm(cx + W * 0.15, cy)
        vs = [e["p0"] for e in ents("line")] or [(0.0, 0.0)]  # none drawn: a radius of 0
        centre = (sum(v[0] for v in vs) / len(vs), sum(v[1] for v in vs) / len(vs))
        radii[fit] = math.dist(vs[0], centre)
    want = 1.0 / math.cos(math.pi / 5.0)
    got = radii["circumscribed"] / radii["inscribed"] if radii["inscribed"] else 0.0
    gui.check("curves: circumscribed over inscribed circumradius is 1/cos(pi/5) to 1e-6", near(got, want, 1e-6),
              f"{got:.9f}, expected {want:.9f}")
    leave()


def tf_fixture(cx, cy, W, L=40):
    """One horizontal line of exactly L mm, drawn by key, for a transform to act on; returns it as
    sketch_describe reads it, with Select armed.

    Exactly L because the transforms' gizmo seeds itself from its target's size (the pivot at the
    line's middle, the handle at half its length), so the value labels the rung clicks sit where
    the line's length puts them."""
    line = draw_line_at(cx - W * 0.10, cy, cx + W * 0.10, cy, L)
    key("Escape")
    return line


def tf_label(pivot, handle, at):
    """Where the transform gizmo prints its value, by its own formula (render_tf_gizmo): past the
    handle, away from the pivot, by 1.2 label heights, a label height being 15 GL pixels in
    millimetres at `at`."""
    th = max(gui.budget(15.0) * gui.mm_per_px(*at), 1e-4)
    d = (handle[0] - pivot[0], handle[1] - pivot[1])
    n = math.hypot(*d) or 1.0
    return (handle[0] + d[0] / n * th * 1.2, handle[1] + d[1] / n * th * 1.2)


def tf_count_label(pivot, at):
    """Where an array gizmo prints its count ("x4"), by render_tf_gizmo's formula: 1.5 label
    heights up and right of the pivot, a label height being 15 GL pixels."""
    th = max(gui.budget(15.0) * gui.mm_per_px(*at), 1e-4)
    return (pivot[0] + th * 1.5, pivot[1] + th * 1.5)


def rung_transforms():
    """O6: Move, Rotate, Scale, Array and Polar array, none of which has a key.

    Each is a gizmo: the verb from the offer on the line, a click picking the line, a click on the
    value label and the value typed, whose Return applies it. An array has two values, so a typed
    one leaves it pending: its second label, the count, is typed next, and one more Return with no
    field open applies the array. The checks are what each operation promises: a move shifts every
    point by the typed amount and nothing else, a rotation turns the direction by the typed angle
    and keeps the length, a scale multiplies the length and keeps the direction, and the arrays
    repeat the line at the typed pitch or angle, the typed number of times."""
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    W = x1 - x0
    L = 40.0
    half = L / 2.0
    step = max(half * 1.5, 1.0)  # the gizmo's seeded move and pitch (tf_pick)

    def direction(e):
        return math.degrees(math.atan2(e["p1"][1] - e["p0"][1], e["p1"][0] - e["p0"][0]))

    def pick_target(verb):
        """Draw the fixture line, run `verb` from the offer on it and pick it; returns the line and
        its pivot. The transforms are offered for a selection, so the right-click goes on the
        line, which selects it; choosing the verb sets the mode, and one more click picks the
        line as the gizmo's target."""
        clear_sketch()
        before = tf_fixture(cx, cy, W, L)
        piv = mid(before)
        arm("transforms", verb, *piv)
        gui.click_mm(*piv)
        return before, piv

    def type_count(piv, n, kind, first):
        """Click the count label of the array pending about `piv`, type n into the field it opens
        and apply the array with Return. The label is there only while the array is pending, so a
        field that does not open means typing the first value applied the array already."""
        gui.click_mm(*tf_count_label(piv, (cx, cy)))
        opened = until(gui.value_field, timeout=2.0) is not None
        gui.check(f"transforms: the {kind} waits for its count once its {first} is typed", opened,
                  f"no field opened on the count label, and the sketch holds {len(ents('line'))} lines: "
                  f"the {first}'s Return applied the {kind} with its default count")
        if opened:
            gui.value(n)
            gui.keys("Return")

    # Move: 25 mm along +X, and nothing else changes.
    before, piv = pick_target("sk_move")
    gui.click_mm(*tf_label(piv, (piv[0] + step, piv[1]), (cx, cy)))
    gui.value(25)
    after = ents("line")
    gui.check("transforms: sk_move left 1 line", len(after) == 1, f"{len(after)} lines")
    if len(after) == 1:
        dx = [after[0]["p0"][0] - before["p0"][0], after[0]["p1"][0] - before["p1"][0]]
        dy = [after[0]["p0"][1] - before["p0"][1], after[0]["p1"][1] - before["p1"][1]]
        gui.check("transforms: every point moved by +25 in X and 0 in Y, to 1e-9",
                  all(abs(v - 25.0) < 1e-9 for v in dx) and all(abs(v) < 1e-9 for v in dy), f"dx {dx}, dy {dy}")

    # Rotate: 30 degrees about the line's middle, its length kept.
    before, piv = pick_target("sk_rotate")
    handle = (piv[0] + half * math.cos(math.pi / 4), piv[1] + half * math.sin(math.pi / 4))  # seeded at 45 deg
    gui.click_mm(*tf_label(piv, handle, (cx, cy)))
    gui.value(30)
    after = ents("line")
    gui.check("transforms: sk_rotate left 1 line", len(after) == 1, f"{len(after)} lines")
    if len(after) == 1:
        turned = (direction(after[0]) - direction(before)) % 360.0
        gui.check("transforms: it turned by 30 deg to 1e-9", min(abs(turned - 30.0), abs(turned - 210.0)) < 1e-9,
                  f"turned by {turned:.9f} deg")
        gui.check("transforms: and kept its length to 1e-9", abs(after[0]["length"] - before["length"]) < 1e-9,
                  f"length {before['length']:.9f} -> {after[0]['length']:.9f}")

    # Scale: x3 about the line's middle, its direction kept.
    before, piv = pick_target("sk_scale")
    gui.click_mm(*tf_label(piv, (piv[0] + 2.0 * half, piv[1]), (cx, cy)))  # seeded at x2
    gui.value(3)
    after = ents("line")
    gui.check("transforms: sk_scale left 1 line", len(after) == 1, f"{len(after)} lines")
    if len(after) == 1:
        gui.check("transforms: its length is exactly x3, to 1e-9",
                  abs(after[0]["length"] - 3.0 * before["length"]) < 1e-9,
                  f"length {before['length']:.9f} -> {after[0]['length']:.9f}")
        gui.check("transforms: and it kept its direction to 1e-9", abs(direction(after[0]) - direction(before)) < 1e-9,
                  f"direction {direction(before):.9f} -> {direction(after[0]):.9f} deg")

    # Linear array: 4 lines at a pitch of 20.
    before, piv = pick_target("sk_array")
    # A single line's pitch is seeded square to it, +Y for this one (tf_pick).
    gui.click_mm(*tf_label(piv, (piv[0], piv[1] + step), (cx, cy)))
    gui.value(20)
    type_count(piv, 4, "array", "pitch")
    rows = sorted(ents("line"), key=lambda e: e["p0"][1])
    gui.check("transforms: sk_array made 4 lines, the line and 3 copies", len(rows) == 4, f"{len(rows)} lines")
    if len(rows) == 4:
        pitch = [round(rows[i + 1]["p0"][1] - rows[i]["p0"][1], 9) for i in range(3)]
        gui.check("transforms: at a pitch of exactly 20", pitch == [20.0, 20.0, 20.0], f"pitch {pitch}")
        gui.check("transforms: and every copy has the line's length to 1e-9",
                  spread([round(e["length"], 9) for e in rows]) == 0.0, f"lengths {[e['length'] for e in rows]}")

    # Polar array: 6 lines 60 degrees apart about one centre, from 360 degrees and a count of 6.
    before, piv = pick_target("sk_array_polar")
    gui.click_mm(*tf_label(piv, (piv[0] + half, piv[1]), (cx, cy)))  # seeded at 360 deg
    gui.value(360)
    type_count(piv, 6, "polar array", "angle")
    spokes = ents("line")
    gui.check("transforms: sk_array_polar made 6 lines", len(spokes) == 6, f"{len(spokes)} lines")
    if len(spokes) == 6:
        mids = [mid(e) for e in spokes]
        far = max(math.dist(m, mids[0]) for m in mids)
        gui.check("transforms: all six turn about one centre, to 1e-9", far < 1e-9, f"middles up to {far:.3e} apart")
        # Directions mod 360, not 180: a line has an orientation, and folding them into a half
        # turn would put opposite spokes on each other.
        angs = sorted(direction(e) % 360.0 for e in spokes)
        gaps = [round(angs[(i + 1) % 6] - angs[i], 9) % 360.0 for i in range(6)]
        gui.check("transforms: and they are 60 deg apart all the way round, to 1e-9",
                  all(abs(g - 60.0) < 1e-9 for g in gaps), f"gaps {gaps}")
    leave()


# The SVG the art rung imports: a filled 40 x 20 path, 2:1 whatever unit the importer takes a user
# unit for. Filled, not stroked: a stroked path imports as its stroke's outline, two loops each
# inflated by half the pen, whose proportions are the pen's.
SVG = os.path.join(HERE, "tests", "fixtures", "rect_40x20.svg")


def text_dialog_shown():
    return "Text" in gui.modeless()


def ok_text(s):
    """Type `s` into the open Text dialog and press its OK, which puts the text in the design on
    the Insert step."""
    # Not submitted: Enter in the field accepts the dialog, and the OK is pressed instead.
    gui.fill({"label": "Text", "class": "TextInput", "within": "Text"}, s, submit=False)
    gui.press("OK", within="Text")
    if not until(lambda: not text_dialog_shown()):
        raise AssertionError("the Text dialog is still open after OK")


def feature_names():
    """The design's features by name, from describe_scene, which goes through call_cad_tool and so
    reads the design as loaded from the model's copy of it."""
    return [f["name"] for f in gui.cad("describe_scene")["features"]]


def rung_art():
    """O7: Text and SVG, the two verbs that go through a dialog.

    Text opens the Design tab's own Text dialog, which is modeless: answer_dialog and the
    auto-answer rules never see it, so its field takes set_widget_value and its OK a click by
    label. Choosing it ends the empty sketch, and the text becomes a feature of its own, placed
    with the Insert gizmo. A text OK'd and then cancelled there must leave the design as it was,
    and a text kept with Confirm is graded. SVG opens a file picker, which a rule answers with the
    fixture's path before it shows, and the outline lands in the open sketch. What is graded
    survives a change of font or of importer scale: how many closed loops came back, and the
    exact proportions of a shape whose proportions are known."""
    # Text, cancelled. The Insert step's Cancel undoes the text. Every call_cad_tool loads the
    # design from the model's copy when the document is empty, so a text left in that copy comes
    # back in describe_scene even though the canvas dropped it.
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    free = (cx, y1 - (y1 - y0) * 0.10)
    before = feature_names()
    choose(gui.offer(*free), *free, "sk_text")
    if not until(text_dialog_shown):
        raise AssertionError(f"choosing sk_text opened no Text dialog: windows {gui.modeless()}")
    ok_text("LT")
    added = feature_names()
    gui.check("art: OK in the Text dialog adds the text to the design", len(added) == len(before) + 1,
              f"features {added}, before the text {before}")
    gui.press("Cancel")  # the ribbon's Cancel, on the Insert step
    until(lambda: feature_names() == before, timeout=2.0)
    left = feature_names()
    gui.check("art: the ribbon's Cancel after OK leaves no text, read back through call_cad_tool", left == before,
              f"features {left}, before the text {before}")

    # Text, kept.
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    H = y1 - y0
    free = (cx, y1 - H * 0.10)
    o = gui.offer(*free)
    gui.check("art: sk_text is offered on an empty sketch", "sk_text" in offered(o, gui.verbs()),
              f"header {o.header!r}, menu {o.items}")
    choose(o, *free, "sk_text")
    shown = until(text_dialog_shown)
    gui.check("art: choosing it opens the Text dialog", shown, f"windows {gui.modeless()}")
    if not shown:
        raise AssertionError("no Text dialog to type into")
    ok_text("LT")
    gui.confirm()  # the Insert gizmo's Confirm keeps the text where it was placed
    feats = gui.cad("describe_scene")["features"]
    card = gui.cad("describe_feature", feature=feats[-1]["index"])["params"] if feats else {}
    # The text's outline is the feature's regions, each a closed outer loop with the loops inside
    # it as holes, and region_loops counts each region's. Neither L nor T has a hole, so the two
    # letters are two closed loops in all; a stray hole in either would make three.
    loops_in = card.get("region_loops") or []
    gui.check("art: the two letters came back as 2 closed loops",
              len(feats) == 1 and card.get("text_string") == "LT" and sum(loops_in) == 2,
              f"features {[f['name'] for f in feats]}, text {card.get('text_string')!r}, "
              f"{card.get('regions')} regions of {loops_in} loops")
    areas = card.get("region_areas") or []
    gui.check("art: both letters enclose real area", len(areas) == 2 and all(a > 1.0 for a in areas),
              f"region areas {[round(a, 3) for a in areas]} mm²")
    gui.new_design()

    # SVG.
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    free = (cx, y1 - (y1 - y0) * 0.10)
    o = gui.offer(*free)
    gui.check("art: sk_svg is offered too", "sk_svg" in offered(o, gui.verbs()), f"header {o.header!r}, menu {o.items}")
    if sys.platform.startswith("linux"):
        gui.skip("art: SVG import", "a GTK file picker can't take a path from MCP")
        leave()
        return
    # The picker is matched by its prompt, unanchored: a picker answered unshown has no title.
    gui.client.call("auto_answer_dialogs", rules=[{"match": "Import SVG", "button": "ok", "path": SVG}])
    try:
        choose(o, *free, "sk_svg")
    finally:
        try:
            gui.client.call("auto_answer_dialogs", rules=[])
        except Exception as e:  # reported, so that it does not hide an error from the choice
            gui.check("art: the Import SVG rule is cleared", False, f"{type(e).__name__}: {e}")
    ls, lp = ents("line"), loops()
    gui.check("art: the imported path is 4 lines and 1 closed loop", len(lp) == 1 and len(ls) == 4,
              f"{len(ls)} lines, {len(lp)} closed loop(s)")
    if ls:
        xs = [p for e in ls for p in (e["p0"][0], e["p1"][0])]
        ys = [p for e in ls for p in (e["p0"][1], e["p1"][1])]
        w, h = max(xs) - min(xs), max(ys) - min(ys)
        # 1e-6, not 1e-9: the importer rounds coordinates to six decimals of a millimetre. 40 x 20
        # user units at 25.4/96 mm come back as 10.583333 x 5.291667, one rounded down and one
        # up, a ratio of 1.999999811. What the sketcher draws itself is exact to 1e-9.
        gui.check("art: and its proportions survived the import, 2:1 to 1e-6", abs(w / h - 2.0) < 1e-6,
                  f"{w:.9f} x {h:.9f} = {w / h:.9f}")
    leave()


# Every sketch verb the rungs drive from the offer, by id, kept as data so that coverage can check
# it against the table rather than a comment claiming it.
DRIVEN = {
    "sk_rect_center",                                           # no_shortcut
    "sk_polyline", "sk_rect_oblique", "sk_rect_rounded",        # curves
    "sk_circle_2pt", "sk_circle_3pt", "sk_arc_center", "sk_arc_tangent",
    "sk_slot_arc", "sk_ellipse_arc",
    "sk_poly_3", "sk_poly_4", "sk_poly_5", "sk_poly_8", "sk_poly_12",
    "sk_poly_inscribed", "sk_poly_circumscribed",
    "sk_move", "sk_rotate", "sk_scale", "sk_array", "sk_array_polar",   # transforms
    "sk_text", "sk_svg",                                        # art
}


def rung_coverage():
    """O8: every sketch verb with no key is driven from the menu, checked against the table.

    The set of keyless sketch verbs list_verbs serves, less the set the rungs drive, must be empty,
    so a verb added without a rung fails here."""
    sk = gui.verbs()
    keyless = {v["id"] for v in sk if v["action"] and not v["key"]}
    keyed = {v["id"] for v in sk if v["key"]}
    dead = {v["id"] for v in sk if not v["action"]}
    missing = keyless - DRIVEN
    gui.check(f"coverage: all {len(keyless)} keyless sketch verbs are driven from the menu", not missing,
              f"missing {sorted(missing)}")
    stray = DRIVEN - keyless - keyed
    gui.check("coverage: nothing is driven that is not in the table", not stray, f"not in the table: {sorted(stray)}")
    gui.check("coverage: no sketch verb is without a GUI route", not dead,
              f"{len(sk)} sketch verbs, {len(keyed)} with a shortcut, {len(keyless)} without, "
              f"{len(dead)} with no GUI route: {sorted(dead)}")


RUNGS = {"kinds": rung_kinds, "vocabulary": rung_vocabulary,
         "author": rung_author, "no_shortcut": rung_no_shortcut,
         "curves": rung_curves, "transforms": rung_transforms,
         "art": rung_art, "coverage": rung_coverage}


def main():
    global gui
    gui = CadGui(orca_mcp.session_client()[0])
    gui.start()
    return gui.run(RUNGS, sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
