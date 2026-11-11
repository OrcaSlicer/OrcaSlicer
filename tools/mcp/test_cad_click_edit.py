#!/usr/bin/env python3
"""Checks that a value field the Design tab opens takes what is typed into it, with no click into it.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_click_edit.py [rung ...]

With no rung named, every rung runs. Drawing a shape opens a value field on the canvas, filled with
the size as drawn and holding the keyboard, so a person types the size they want and presses
Return. Each rung draws with clicks on the canvas, types each value as soon as its field shows, with
no click into the field and no target for the keys, and presses Return. For every field it checks
that the field opened by itself, that the typed digits landed in it, that the typed value is not the
one the field opened with, that Return committed it (the field closed or the next one opened), and
that the geometry sketch_describe reads back has the typed value. A field that is shown but does not
get the keyboard commits its prefill on Return, and only those last checks tell it from one that
works. Every action goes through the MCP UI tools, and call_cad_tool only reads. Exits 0 when every
check holds.

The rungs: line, rect, circle, slot, polygon, ellipse and arc draw with their tool's key and type
into the first field or two (line 61; rect 62 and 43; circle 64; slot 66; polygon 67; ellipse 68;
arc 69). rounded_rect arms the rounded rectangle from the right-click offer, its only route, and
types its width, height and corner radius (63, 41, 7). label draws a rectangle as wide as puts its
width label over one of its constraint badges, keeps it as drawn, picks it in Select and clicks the
label's centre, which opens the field again, and types 71.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from cad_gui import BADGE_PICK_PX, BADGE_STEP_PX, CANVAS, CadGui, until  # noqa: E402

gui = None  # the CadGui main() starts


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


def key(k):
    """A tool key or Esc, pressed over the canvas."""
    gui.keys(k, target=CANVAS)


def inside(p):
    """`p` (mm), checked to lie in the region the calibration probes covered, where a click is known
    to reach the plane."""
    x0, x1, y0, y1 = gui.safe
    if not (x0 <= p[0] <= x1 and y0 <= p[1] <= y1):
        raise RuntimeError(f"{p} mm lies outside the calibrated region {gui.safe}: widen the window")
    return p


# ---------------------------------------------------------------- what a typed value sets
# Each reads, from sketch_describe, the quantities of the drawn shape that one field sets; the
# field took its value when every one of them is that value.

def lines(d):
    return [e for e in d["entities"] if e["type"] == "line"]


def line_lengths(d):
    return [e["length"] for e in lines(d)]


def extent(d, axis):
    """How far the shape's lines reach along x (0) or y (1): a rectangle's width or height, the
    straight sides of a rounded one included, since they span its whole box."""
    vs = [p[axis] for e in lines(d) for p in (e["p0"], e["p1"])]
    return [max(vs) - min(vs)] if vs else []


def width(d):
    return extent(d, 0)


def height(d):
    return extent(d, 1)


def radii(kind):
    """The radius of every `kind` entity; an ellipse's is its semi-major axis."""
    return lambda d: [e["radius"] for e in d["entities"] if e["type"] == kind]


# ---------------------------------------------------------------- the field

def differs(prefill, value):
    try:
        return not near(float(prefill), value)
    except ValueError:
        return prefill != str(value)


def type_into_open_field(rung, value, what, measure, opened_by="by itself"):
    """Type `value` into the value field that opens, with no click into it, press Return, and check
    the field took it; returns whether a field opened.

    No click and no target for the keys: the field opens holding the keyboard, as a person sees it.
    list_widgets reads the field as an ImGui input whose value is its text, the prefill until
    something is typed and then the digits that landed. Return commits the text the field holds,
    so a field the digits never reached commits its prefill, which the geometry then shows. `what`
    names the quantity `measure` reads off sketch_describe, the one the field sets."""
    field = until(gui.value_field)
    if not gui.check(f"{rung}: a value field opened {opened_by}", field is not None,
                     f"none opened to type {value} into"):
        return False
    title, prefill = field.title, field.prefill

    def shown():
        f = gui.value_field()
        return None if f is None else f.prefill

    gui.text(str(value))
    until(lambda: shown() == str(value), timeout=2.0)
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
    gui.check(f"{rung}: Return committed {title}: the field closed or the next one opened", committed,
              f"the field is still open, titled {left.title!r} on {left.prefill!r}" if left else "")

    got = []

    def carries():
        got[:] = measure(gui.sketch())
        return bool(got) and all(near(q, value) for q in got)

    ok = until(carries, timeout=1.0)
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
    queued after it and keeps the rest of the shape as drawn. The next field opens a moment after
    the last commit, so this waits a while for one before deciding none is open."""
    if until(gui.value_field, timeout=1.0) is None:
        return
    key("Escape")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError("Esc left the value field open")


# ---------------------------------------------------------------- one tool each

# The clicks (mm) that draw each shape, and for each field the drawing opens in turn, the value
# typed into it, what of the shape that value sets, and how to read that off sketch_describe. Each
# shape is drawn well away from the values typed into it, so a committed prefill cannot pass for a
# typed value. The polygon's field is its side; the slot's first is the distance between its
# centres.
TOOLS = {
    "line": ("l", [(-51, 27), (48, 27)], [(61, "its length", line_lengths)]),
    "rect": ("r", [(-51, 27), (48, -30)], [(62, "its width", width), (43, "its height", height)]),
    "circle": ("c", [(-6, 3), (39, 3)], [(64, "its radius", radii("circle"))]),
    "slot": ("s", [(-51, 15), (30, 15), (30, -3)], [(66, "the length of both flanks", line_lengths)]),
    "polygon": ("g", [(-6, 3), (36, 3)], [(67, "every side", line_lengths)]),
    "ellipse": ("e", [(-6, 3), (51, 3), (-6, -27)], [(68, "its semi-major axis", radii("ellipse"))]),
    "arc": ("a", [(-48, -9), (42, -9), (-3, 21)], [(69, "its radius", radii("arc"))]),
}


def draw_and_type(rung, clicks, fields):
    for p in clicks:
        gui.click_mm(*inside(p))
    for value, what, measure in fields:
        if not type_into_open_field(rung, value, what, measure):
            return
    close_fields()
    gui.cancel()


def tool_rung(rung):
    tool_key, clicks, fields = TOOLS[rung]

    def run():
        gui.enter_sketch(tool_key)
        draw_and_type(rung, clicks, fields)

    return run


# ---------------------------------------------------------------- from the offer

def offer_path(offer, verb):
    """The label path at which `offer` lists `verb` as an item that can be run, from list_verbs: the
    verb's row, then its family, then its name, or its name alone for a row that offers only it."""
    v = next((v for v in gui.verbs() if v["id"] == verb), None)
    if v is None:
        raise AssertionError(f"list_verbs has no {verb}")
    full = ">".join([v["row_name"]] + ([v["family"]] if v["family"] else []) + [v["name"]])
    for path in (full, v["name"]):
        if path in offer.items and path not in offer.disabled:
            return path
    raise AssertionError(f"{verb} is not offered here ({offer.header}): menu {offer.items}")


def rung_rounded_rect():
    """The rounded rectangle has no key, so it is armed from the right-click offer, and its three
    fields come in turn: width, height, then the corner radius the third click set. The radius drawn
    (about 15) is under half of each typed side, which the field requires of a width or height."""
    gui.enter_sketch()
    x0, x1, y0, y1 = gui.safe
    empty = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
    path = offer_path(gui.offer(*empty), "sk_rect_rounded")
    reply = gui.choose(*empty, path)
    if reply.get("ran") != path:
        raise AssertionError(f"choosing {path} ran {reply.get('ran')}: {reply}")
    tool = gui.sketch()["tool"]
    if not gui.check("rounded_rect: the offer armed the rounded rectangle tool", tool == "rect_rounded",
                     f"tool {tool}"):
        return
    draw_and_type("rounded_rect", [(-51, 27), (39, -21), (30, -9)],
                  [(63, "its width", width), (41, "its height", height), (7, "every corner's radius", radii("arc"))])


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
    width = width_on_badge(gui.mm_per_px(0, 30) / gui.gl_per_px, 2)
    gui.click_mm(*inside((-width / 2.0, 30)))
    gui.click_mm(*inside((width / 2.0, -30)))
    if until(gui.value_field) is None:
        raise AssertionError("the rectangle opened no value field")
    key("Escape")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError("Esc left the rectangle's value field open")
    key("Escape")
    if not until(lambda: gui.sketch()["tool"] == "select"):
        raise AssertionError(f"Esc left the tool {gui.sketch()['tool']} armed")

    # The corner rectangle's first edge runs from its first corner along x, so with the first click
    # at the top left it is the top edge, and its width label sits above it.
    ents = gui.sketch()["entities"]
    top = ents[0]
    ys = [p[1] for e in ents for p in (e["p0"], e["p1"])]
    if top["type"] != "line" or not near(top["p0"][1], max(ys), 1e-9) or not near(top["p1"][1], max(ys), 1e-9):
        raise AssertionError(f"the rectangle's first edge is not its top: {ents}")
    mid = ((top["p0"][0] + top["p1"][0]) / 2.0, (top["p0"][1] + top["p1"][1]) / 2.0)
    gui.click_mm(*inside(mid))
    if not until(lambda: gui.sketch()["selection"], timeout=2.0):
        raise AssertionError("the click on the top edge picked nothing")

    # The label's centre, read from the window it is drawn in; the badge under it, from the layout.
    mm_per_gl_px = gui.mm_per_px(*mid) / gui.gl_per_px
    want = gui.px(*width_label(top, LABEL_PX * mm_per_gl_px))
    x, y, w, h = min(gui.overlays(), key=lambda r: math.dist((r[0] + r[2] / 2.0, r[1] + r[3] / 2.0), want))
    centre = (x + w / 2.0, y + h / 2.0)
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


RUNGS = {**{name: tool_rung(name) for name in TOOLS}, "rounded_rect": rung_rounded_rect, "label": rung_label}


def main():
    global gui
    gui = CadGui(orca_mcp.session_client()[0])
    gui.start()
    return gui.run(RUNGS, sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
