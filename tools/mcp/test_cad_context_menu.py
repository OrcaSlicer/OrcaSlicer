#!/usr/bin/env python3
"""Checks that the Design tab's right-click offer adapts to what was clicked and lists exactly the
verbs the offer table says apply, that each verb it alone reaches draws what it should, and that
the value fields a drawing opens take what is typed into them with no click.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_context_menu.py [rung ...]

With no rung named, every rung runs. The offer is where a sketch's verbs live, and for the many
that have no key it is the only way to them. Each rung opens a sketch on the XY plane, right-clicks
it with mouse_gesture, which lists the menu the click opens without showing it, and runs an item
by its label path with popup_item. The menu is graded against the Design table it is built from
(CadGui.verbs(): scripts/CAD/tool_atlas.json, for the selection design_describe says the right-click
left); what a verb drew is graded on the geometry sketch_describe reads back, or for a text on
the feature feature_describe reads. Every action goes through the MCP UI tools, and the Design commands only read.
Exits 0 when every check holds.

The rungs: vocabulary right-clicks empty space, a line, a circle, a point and a line and a circle
picked together, and checks for each that the menu's header names it, that the menu lists exactly
the label paths the table gives and greys exactly the rows with nothing to offer, and that the five
offer different verbs; that the Line and Circle tools, armed with nothing anchored, hand the
right-click on to the offer; and that a right-click while a line's first point is down abandons the
point and opens nothing. author draws a 120 x 80 rectangle with the corner
rectangle chosen from the menu, and no_shortcut one with the centre rectangle, which has no key,
and checks it is centred on its click. curves draws each keyless creation verb (polyline, oblique
and rounded rectangle, 2- and 3-point circle, centre and tangent arc, arc slot, elliptical arc,
polygons of 3, 4, 5, 8 and 12 sides, inscribed and circumscribed) and grades the construction; the
rounded rectangle's width, height and corner radius are typed into its three value fields as they
open, each checked to have opened by itself, taken the keyboard and the digits, differed from the
size drawn and committed on Return, and graded on the geometry it set. label draws a rectangle as
wide as puts its width label over one of its constraint badges, keeps it as drawn, picks it in
Select and clicks the label's centre, which opens the field again, and types 71.
transforms moves a line 25, rotates it 30 degrees, scales it 3 times and arrays it at a pitch of 20
four times and at 360 degrees six times, each through its gizmo's value labels. art types "LT"
into the Text dialog twice, cancelling the first text on the Insert step, which must leave no text,
and keeping the second, and imports a 40 x 20 SVG through its file picker, which a rule answers
with the fixture's path.
coverage checks that the verbs the rungs drive from the menu are the sketch verbs the table has no
key for.
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orca_mcp  # noqa: E402
from cad_gui import BADGE_PICK_PX, BADGE_STEP_PX, CadGui, lengths, mid, near, until  # noqa: E402

gui = None  # the CadGui main() starts

# The offer's header for each sketch selection: its first item, greyed, naming what the items act
# on (DesignPanel::offer_header). Keyed by the selection kinds of the offer table (OfferSel).
HEADER = {"SkNone": "Sketch — nothing selected", "SkLine": "Sketch line", "SkArc": "Sketch curve",
          "SkPoint": "Sketch point", "Sk2Ent": "2 sketch entities"}


def spread(vals):
    return max(vals) - min(vals)


def ents(kind=None):
    return [e for e in gui.sketch()["entities"] if kind is None or e["type"] == kind]


# ---------------------------------------------------------------- the table, as the menu lays it out

def verb_paths(verbs):
    """The label path at which the offer lists each verb that applies, by verb id. `verbs` is
    the Design table's sketch vocabulary (CadGui.verbs()), read with the selection the right-click left.

    It follows DesignPanel::show_offer_menu. A row with two or more verbs for the selection is a
    submenu named for the row, holding each verb directly or, for a verb of a family (Rectangle:
    corner, centre, oblique, rounded), in a submenu named for the family. A row with exactly one is
    that verb alone at the top level. A row with none is greyed (expected_greyed()). No sketch verb
    is held back by the document or sits in a flat row, so a verb is listed exactly when it
    applies."""
    if not all(v["sketch_mode"] for v in verbs):
        raise ValueError("verb_paths lays out the sketch offer: give it the table's sketch verbs")
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

def clear_sketch():
    """Empty the open sketch through the UI, keeping it open on the same plane and view.

    A sketch has no select-all (Ctrl+A selects nothing, nor does a drag in Select), so the ribbon's
    Delete selected cannot take everything in one go. Cancel discards the sketch instead, and a new
    one opens on the XY plane and is calibrated again. The camera has not moved, so plane points
    computed before keep their pixels."""
    gui.keep_as_drawn()
    if gui.sketch()["entities"]:
        gui.cancel()
        gui.enter_sketch()
    left = len(gui.sketch()["entities"])
    if left:
        raise AssertionError(f"the sketch still holds {left} entities")


def inside(p):
    """`p` (mm), checked to lie in the region the calibration probes covered, where a click is known
    to reach the plane."""
    x0, x1, y0, y1 = gui.safe
    if not (x0 <= p[0] <= x1 and y0 <= p[1] <= y1):
        raise RuntimeError(f"{p} mm lies outside the calibrated region {gui.safe}: widen the window")
    return p


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


def poly_click(pt):
    """One click of a multi-segment tool, then close the value field that click opened. The
    polyline opens a Length field after every segment, and the canvas takes no click while one is
    open."""
    gui.click_mm(*pt)
    gui.keep_as_drawn()


def draw_line_at(x0, y0, x1, y1, length):
    """One line clicked from (x0, y0) to (x1, y1) and committed by typing its length and an angle of
    0; returns the line as sketch_describe reads it. The typed length is a dimension the sketch
    solves by moving the line's ends, so the line is not where the clicks put it, and the clicks
    that pick it are placed on the line read back. The tool stays armed, and the fields are closed
    before the caller clicks again."""
    gui.canvas_keys("l")
    gui.click_mm(x0, y0)
    gui.click_mm(x1, y1)
    gui.values(int(length), 0)
    gui.keep_as_drawn()
    return ents("line")[-1]


def safe_box():
    """The calibrated region (x0, x1, y0, y1) and its centre."""
    x0, x1, y0, y1 = gui.safe
    return x0, x1, y0, y1, (x0 + x1) / 2.0, (y0 + y1) / 2.0


# ---------------------------------------------------------------- typing into a value field
# Drawing a shape opens a value field on the canvas, filled with the size as drawn and holding the
# keyboard, so a person types the size they want and presses Return. A field that is shown but does
# not get the keyboard commits its prefill on Return, and only the geometry read back afterwards
# tells it from one that works, so the shape is drawn away from every value typed into it.

def extent(d, axis):
    """How far the shape's lines reach along x (0) or y (1): a rectangle's width or height, the
    straight sides of a rounded one included, since they span its whole box."""
    vs = [p[axis] for e in d["entities"] if e["type"] == "line" for p in (e["p0"], e["p1"])]
    return [max(vs) - min(vs)] if vs else []


def width(d):
    return extent(d, 0)


def height(d):
    return extent(d, 1)


def arc_radii(d):
    return [e["radius"] for e in d["entities"] if e["type"] == "arc"]


def differs(prefill, value):
    try:
        return not near(float(prefill), value)
    except ValueError:
        return prefill != str(value)


def type_into_open_field(rung, value, what, measure, opened_by="by itself"):
    """Type `value` into the value field that opens, with no click into it, press Return, and check
    the field took it; returns whether a field opened.

    No click and no target for the keys: the field opens holding the keyboard, as a person sees it.
    widget_list reads the field as an ImGui input whose value is its text, the prefill until
    something is typed and then the digits that landed. Return commits the text the field holds,
    so a field the digits never reached commits its prefill, which the geometry then shows. `what`
    names the quantity `measure` reads off sketch_describe, the one the field sets."""
    field = until(gui.value_field)
    if not gui.check(f"{rung}: a value field opened {opened_by} to take {value}", field is not None,
                     f"none opened to type {value} into"):
        return False
    title, prefill = field.title, field.prefill
    # Typed before the field takes the keyboard, a frame after it shows, the digits would be lost.
    if not gui.check(f"{rung}: {title} took the keyboard with no click into it, to take {value}",
                     until(gui.typing_field) is not None, "it never showed focused"):
        return False

    def shown():
        f = gui.value_field()
        return None if f is None else f.prefill

    gui.text(str(value))
    until(lambda: shown() == str(value))
    typed = shown()
    gui.check(f"{rung}: {title} took the typed {value}, with no click into it", typed == str(value),
              f"it shows {typed!r}" + (", its prefill: the digits never reached it" if typed == prefill else ""))
    gui.check(f"{rung}: the typed {value} is not {title}'s prefill", differs(prefill, value), f"prefill {prefill!r}")

    # A commit closes the field and opens the next one queued, with its own prefill. A value the
    # field refuses leaves it open on the same text, its title replaced by the reason.
    def moved_on():
        f = gui.value_field()
        return f is None or f.prefill != typed

    gui.keys("Return")
    committed = until(moved_on)
    left = gui.value_field()
    gui.check(f"{rung}: Return committed {title}'s {value}: the field closed or the next one opened", committed,
              f"the field is still open, titled {left.title!r} on {left.prefill!r}" if left else "")

    got = []

    def carries():
        got[:] = measure(gui.sketch())
        return bool(got) and all(near(q, value) for q in got)

    ok = until(carries)
    was_prefill = False
    try:
        was_prefill = bool(got) and all(near(q, float(prefill), 1e-3) for q in got)
    except ValueError:
        pass
    gui.check(f"{rung}: {what} is the typed {value}", ok,
              f"{what} reads {[round(q, 6) for q in got]}" + (", the prefill Return committed" if was_prefill else ""))
    return True


def close_fields():
    """Esc on a value field still open after the ones typed into, which closes it and the fields
    queued after it and keeps the rest of the shape as drawn. The next field opens from work the
    last commit sets off, so whether one is open is read once the app has run it
    (CadGui.opened_field())."""
    if gui.opened_field() is None:
        return
    gui.canvas_keys("Escape")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError("Esc left the value field open")


# ---------------------------------------------------------------- rungs

def rung_vocabulary():
    """O1 and O2: the offer reads what was right-clicked, and the menu lists exactly what the offer
    table says applies, selection by selection.

    Five selections, not one: a menu that is the same whatever was clicked would pass any single
    comparison, and only the five against their own tables tell those apart."""
    gui.enter_sketch("l")
    x0, x1, y0, y1, cx, cy = safe_box()
    W, H = x1 - x0, y1 - y0

    # A right-click on a draw tool does one of two jobs, depending on whether an anchor is down:
    # with none it opens the offer, so the offer is reachable from any armed tool; with one it
    # abandons the anchor instead and offers nothing.
    hi = y1 - H * 0.12
    o = gui.offer(cx, hi)
    gui.check("vocabulary: Line armed, nothing anchored: a right-click on empty space offers for nothing selected",
              o.header == HEADER["SkNone"], f"menu {o}")
    names = {v["name"] for v in gui.verbs()}
    leaves = [p.split(">")[-1] for p in o.items if p not in o.disabled]
    gui.check("vocabulary: the sketch vocabulary is the one offered", leaves and all(n in names for n in leaves),
              f"items not named for a sketch verb: {[n for n in leaves if n not in names]}")
    gui.canvas_keys("l")
    gui.click_mm(cx, hi)
    o = gui.offer(cx + W * 0.1, hi)
    gui.check("vocabulary: with an anchor down, a right-click abandons it and opens no offer",
              o.header is None and not o.items, f"menu {o}")
    o = gui.offer(cx, hi)
    gui.check("vocabulary: and the offer is back on the next right-click", o.header is not None, f"menu {o}")
    gui.canvas_keys("Escape")

    ax, ay = x0 + W * 0.15, cy
    bx = x0 + W * 0.55
    line = draw_line_at(ax, ay, bx, ay, 40)

    gui.canvas_keys("c")
    ccx, ccy = x0 + W * 0.30, cy + H * 0.25
    gui.click_mm(ccx, ccy)
    gui.click_mm(ccx + W * 0.10, ccy)
    gui.values(20)
    circ = ents("circle")
    if len(circ) != 1:
        raise AssertionError(f"{len(circ)} circles drawn, expected one to right-click on")
    circ = circ[0]
    # Each draw tool takes the right-click first and hands it back when nothing is anchored.
    o = gui.offer(circ["center"][0] + circ["radius"], circ["center"][1])
    gui.check("vocabulary: Circle armed, nothing anchored: a right-click on the circle offers for a curve",
              o.header == HEADER["SkArc"], f"header {o.header!r} (tool {gui.sketch()['tool']})")

    gui.canvas_keys("p")
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
            gui.keep_as_drawn()
            gui.canvas_keys("Escape")
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

    # The sets differ: an offer that adapts does not show the same items everywhere.
    gui.check("vocabulary: every selection offers a different set",
              len(set(map(frozenset, seen.values()))) == len(seen),
              ", ".join(f"{k}={len(v)}" for k, v in seen.items()))
    added = seen["SkLine"] - seen["SkNone"]
    gui.check("vocabulary: a picked line adds verbs an empty pick has not", added,
              f"{len(added)} added: {', '.join(sorted(added)[:8])}")
    gui.cancel_and_clear()


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
    lp = gui.closed_loops()
    gui.check("author: one closed loop", len(lp) == 1, f"{len(lp)} closed loop(s)")
    gui.check("author: area 9600", len(lp) == 1 and near(abs(lp[0]["area"]), w * h, 1e-9),
              f"areas {[abs(l['area']) for l in lp]}")
    gui.cancel_and_clear()


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
    lp = gui.closed_loops()
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
    gui.cancel_and_clear()


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
    ls, lp = ents("line"), gui.closed_loops()
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
    # Its three value fields, Width, Height and then the corner Radius, are each typed into as they
    # open. The clicks draw it 90 x 48 with corners of 15, away from every typed value and under
    # half of each typed side, which the Width and Height fields require of the radius.
    clear_sketch()
    arm("curves", "sk_rect_rounded", *free)
    for p in ((-51, 27), (39, -21), (30, -9)):
        gui.click_mm(*inside(p))
    for value, what, measure in ((63, "its width", width), (41, "its height", height),
                                 (7, "every corner's radius", arc_radii)):
        if not type_into_open_field("curves", value, what, measure):
            break
    close_fields()
    ls, ar = ents("line"), ents("arc")
    gui.check("curves: sk_rect_rounded drew 4 lines and 4 arcs", len(ls) == 4 and len(ar) == 4,
              f"{len(ls)} lines, {len(ar)} arcs")
    if len(ar) == 4:
        rr = sorted(round(e["radius"], 9) for e in ar)
        gui.check("curves: its four corners share one radius to 1e-9", spread(rr) == 0.0, f"radii {rr}")
        lp = gui.closed_loops()
        gui.check("curves: the rounded rectangle is one closed loop", len(lp) == 1, f"{len(lp)} closed loop(s)")
        if len(lp) == 1:
            # Rounding takes off the four corner squares less their quarter discs: r^2 (4 - pi).
            want = 63.0 * 41.0 - 7.0 * 7.0 * (4 - math.pi)
            gui.check("curves: its area is the typed 63 x 41 less r^2 (4 - pi) for the typed r 7",
                      near(abs(lp[0]["area"]), want, 1e-6), f"area {abs(lp[0]['area']):.9f}, expected {want:.9f}")

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
    gui.canvas_keys("Escape")
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
    for verb, fit in (("sketch_polygon_inscribed", "inscribed"), ("sketch_polygon_circumscribed", "circumscribed")):
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
    gui.cancel_and_clear()


def tf_fixture(cx, cy, W, L=40):
    """One horizontal line of exactly L mm, drawn by key, for a transform to act on; returns it as
    sketch_describe reads it, with Select armed.

    Exactly L because the transforms' gizmo seeds itself from its target's size (the pivot at the
    line's middle, the handle at half its length), so the value labels the rung clicks sit where
    the line's length puts them."""
    line = draw_line_at(cx - W * 0.10, cy, cx + W * 0.10, cy, L)
    gui.canvas_keys("Escape")
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
        opened = until(gui.value_field) is not None
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
    gui.cancel_and_clear()


# ---------------------------------------------------------------- a label opens the field again

def width_label(edge, th):
    """Where a rectangle's width label sits for its first edge `edge`: off the edge's left side by
    18% of its length (at least 8 mm), then by 0.7 label heights and 1.5 mm, the place
    DesignSketchTool::draw_dim_quote gives a live Length quote on side +1. `th` is the label height
    in millimetres."""
    (ax, ay), (bx, by) = edge["p0"], edge["p1"]
    L = math.hypot(bx - ax, by - ay)
    nx, ny = -(by - ay) / L, (bx - ax) / L
    off = max(L * 0.18, 8.0) + th * 0.7 + 1.5
    return ((ax + bx) / 2.0 + nx * off, (ay + by) / 2.0 + ny * off)


# The height of a value label, DesignSketchTool's 15 px, in the canvas's GL pixels as its badges are.
LABEL_PX = 15.0


def width_on_badge(mm_per_gl_px, k):
    """The width at which a rectangle's width label is centred on its top edge's badge `k`: the label
    sits max(0.18 L, 8) + 1.5 mm and 0.7 label heights off the edge (width_label), and the badge
    BADGE_STEP_PX * (k + 1) over the edge's middle (CadGui.badge), both pixel lengths in GL pixels."""
    return ((BADGE_STEP_PX * (k + 1) - 0.7 * LABEL_PX) * mm_per_gl_px - 1.5) / 0.18


def rung_label():
    """A drawn rectangle's width label, clicked at its centre in Select, opens the field again, and a
    value typed there sets the width. The rectangle is as wide as puts that centre on the top edge's
    third constraint badge, its Horizontal, which the label hides: what a person sees there and aims
    at is the label, so the label takes the click. Taking the label promotes the width to a driving
    dimension, so the click adds exactly one constraint; had the hidden badge taken it, it would
    have selected the Horizontal and opened no field. The rectangle is kept as drawn (Esc on its
    fields), so it is where its clicks put it, and Esc then drops the tool to Select. A shape's
    labels show while it is picked, so a click on its top edge picks it first."""
    gui.enter_sketch("r")
    w = width_on_badge(gui.mm_per_px(0, 30) / gui.gl_per_px, 2)
    gui.click_mm(*inside((-w / 2.0, 30)))
    gui.click_mm(*inside((w / 2.0, -30)))
    if until(gui.value_field) is None:
        raise AssertionError("the rectangle opened no value field")
    gui.canvas_keys("Escape")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError("Esc left the rectangle's value field open")
    gui.canvas_keys("Escape")
    if not until(lambda: gui.sketch()["tool"] == "select"):
        raise AssertionError(f"Esc left the tool {gui.sketch()['tool']} armed")

    # The corner rectangle's first edge runs from its first corner along x, so with the first click
    # at the top left it is the top edge, and its width label sits above it.
    es = gui.sketch()["entities"]
    top = es[0]
    ys = [p[1] for e in es for p in (e["p0"], e["p1"])]
    if top["type"] != "line" or not near(top["p0"][1], max(ys), 1e-9) or not near(top["p1"][1], max(ys), 1e-9):
        raise AssertionError(f"the rectangle's first edge is not its top: {es}")
    gui.click_mm(*inside(mid(top)))
    if not until(lambda: gui.sketch()["selection"]):
        raise AssertionError("the click on the top edge picked nothing")

    # The label's centre, read from the window it is drawn in; the badge under it, from the layout.
    mm_per_gl_px = gui.mm_per_px(*mid(top)) / gui.gl_per_px
    want = gui.px(*width_label(top, LABEL_PX * mm_per_gl_px))
    x, y, rw, rh = min(gui.overlays(), key=lambda r: math.dist((r[0] + r[2] / 2.0, r[1] + r[3] / 2.0), want))
    centre = (x + rw / 2.0, y + rh / 2.0)
    if math.dist(centre, gui.px(*gui.badge(top, 2))) > gui.budget(BADGE_PICK_PX):
        raise AssertionError(f"the width label's centre {centre} is off the top edge's third badge "
                             f"{gui.px(*gui.badge(top, 2))}: the badge or label layout moved, and this rung "
                             f"no longer clicks a label drawn over a badge")
    before = gui.sketch()["constraints"]
    gui.gesture(x=round(centre[0]), y=round(centre[1]))
    after = gui.sketch()
    gui.check("label: the click at the label's centre took the label: one driving dimension added",
              after["constraints"] == before + 1,
              f"{before} constraints before the click, {after['constraints']} after"
              + (", the badge under the label selected" if after["selected_constraint"] is not None else ""))
    if type_into_open_field("label", 71, "the top edge's length", lambda d: [d["entities"][0]["length"]],
                            opened_by="from the click at the centre of the top edge's width label"):
        close_fields()
    gui.cancel()


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


def rung_art():
    """O7: Text and SVG, the two verbs that go through a dialog.

    Text opens the Design tab's own Text dialog, which is modeless: dialog_answer and the
    auto-answer rules never see it, so its field takes widget_set_value and its OK a click by
    label. Choosing it ends the empty sketch, and the text becomes a feature of its own, placed
    with the Insert gizmo. A text OK'd and then cancelled there must leave the design as it was,
    and a text kept with Confirm is graded. SVG opens a file picker, which a rule answers with the
    fixture's path before it shows, and the outline lands in the open sketch. What is graded
    survives a change of font or of importer scale: how many closed loops came back, and the
    exact proportions of a shape whose proportions are known."""
    # Text, cancelled. The Insert step's Cancel undoes the text. Every Design command loads the
    # design from the model's copy when the document is empty, so a text left in that copy comes
    # back in design_describe even though the canvas dropped it.
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    free = (cx, y1 - (y1 - y0) * 0.10)
    before = gui.feature_names()
    choose(gui.offer(*free), *free, "sketch_text")
    if not until(text_dialog_shown):
        raise AssertionError(f"choosing sketch_text opened no Text dialog: windows {gui.modeless()}")
    ok_text("LT")
    added = gui.feature_names()
    gui.check("art: OK in the Text dialog adds the text to the design", len(added) == len(before) + 1,
              f"features {added}, before the text {before}")
    gui.press("Cancel")  # the ribbon's Cancel, on the Insert step
    until(lambda: gui.feature_names() == before)
    left = gui.feature_names()
    gui.check("art: the ribbon's Cancel after OK leaves no text, read back through design_describe", left == before,
              f"features {left}, before the text {before}")

    # Text, kept.
    gui.enter_sketch()
    x0, x1, y0, y1, cx, cy = safe_box()
    H = y1 - y0
    free = (cx, y1 - H * 0.10)
    o = gui.offer(*free)
    gui.check("art: sketch_text is offered on an empty sketch", "sketch_text" in offered(o, gui.verbs()),
              f"header {o.header!r}, menu {o.items}")
    choose(o, *free, "sketch_text")
    shown = until(text_dialog_shown)
    gui.check("art: choosing it opens the Text dialog", shown, f"windows {gui.modeless()}")
    if not shown:
        raise AssertionError("no Text dialog to type into")
    ok_text("LT")
    gui.confirm()  # the Insert gizmo's Confirm keeps the text where it was placed
    feats = gui.cad("design_describe")["features"]
    card = gui.cad("feature_describe", feature=feats[-1]["index"])["params"] if feats else {}
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
    gui.check("art: sketch_insert_svg is offered too", "sketch_insert_svg" in offered(o, gui.verbs()), f"header {o.header!r}, menu {o.items}")
    # The picker is matched by its prompt, unanchored: a picker answered unshown has no title.
    gui.client.call("dialog_auto_answer", rules=[{"match": "Import SVG", "button": "ok", "path": SVG}])
    try:
        choose(o, *free, "sketch_insert_svg")
    finally:
        try:
            gui.client.call("dialog_auto_answer", rules=[])
        except Exception as e:  # reported, so that it does not hide an error from the choice
            gui.check("art: the Import SVG rule is cleared", False, f"{type(e).__name__}: {e}")
    ls, lp = ents("line"), gui.closed_loops()
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
    gui.cancel_and_clear()


# Every sketch verb the rungs drive from the offer, by id, kept as data so that coverage can check
# it against the table rather than a comment claiming it.
DRIVEN = {
    "sk_rect_center",                                           # no_shortcut
    "sk_polyline", "sk_rect_oblique", "sk_rect_rounded",        # curves
    "sk_circle_2pt", "sk_circle_3pt", "sk_arc_center", "sk_arc_tangent",
    "sk_slot_arc", "sk_ellipse_arc",
    "sk_poly_3", "sk_poly_4", "sk_poly_5", "sk_poly_8", "sk_poly_12",
    "sketch_polygon_inscribed", "sketch_polygon_circumscribed",
    "sk_move", "sk_rotate", "sk_scale", "sk_array", "sk_array_polar",   # transforms
    "sketch_text", "sketch_insert_svg",                         # art
}


def rung_coverage():
    """O8: every sketch verb with no key is driven from the menu, checked against the table.

    The set of keyless sketch verbs the Design table has, less the set the rungs drive, must be empty,
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


RUNGS = {"vocabulary": rung_vocabulary,
         "author": rung_author, "no_shortcut": rung_no_shortcut,
         "curves": rung_curves, "label": rung_label, "transforms": rung_transforms,
         "art": rung_art, "coverage": rung_coverage}


def main():
    global gui
    # The baseline every test starts from on a shared app and data folder, then the Design tab's own start.
    gui = CadGui(orca_mcp.session_client()[0])
    gui.start()
    return gui.run(RUNGS, sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
