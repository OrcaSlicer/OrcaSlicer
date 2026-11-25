#!/usr/bin/env python3
"""Checks what the Design commands' arguments do in the running app, where the panel's own functions answer.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_commands.py [rung ...]

With no rung named, every rung runs. Each one starts from an empty design, calls the Design commands with
their arguments and grades what design_describe, feature_describe, sketch_describe and body_mass_properties
read back. The unit tests drive the commands' checks on a bare document; these read the replies and the
features the Design panel builds. Exits 0 when every check holds, 2 when the app is not in English or has no
Design tab.

The rungs: words calls each command that takes `operation` with Fusion's words, and checks that its reply
and the feature it adds say the operation in the same word, and that the bodies show it was done; it
suppresses a feature, which design_describe and feature_describe say as suppressed, and adds two aligns that
cannot both hold, which design_describe lists in align_conflicts; and it calls the open sketch's commands by
their type, sketch_extend, sketch_pattern, sketch_transform and sketch_dimension, through their silent forms.
shapes_match_tools draws six shapes twice, each in a sketch of its own: once with its drawing tool, its
right-click menu item chosen and its points clicked on the canvas, and once with sketch_draw, from the geometry
the tool's shape reads back with; the 3-point rectangle, the tangent arc, the slot and the square are drawn at a
slant. The two sketches agree in their entities' types and geometry, their constraints' count and kinds (as
sketch_list_constraints reads them), their loops and their degrees of freedom, and again after one undo of each,
which takes a grouped shape off whole and a polyline's last line only.
The sketch_edit rungs re-open the sketch of a block extruded from it. sketch_edit scales it, and sketch_finish
replaces it in one undo step, which the block follows; sketch_edit_cancel draws in it and cancels, with no dialog and
the design as it was; sketch_edit_refused gives an index past the features, the extrude's, a sketch from the
card's width and height, a Project sketch, and any index while a sketch is open. sketch_edit_failed_replace
shrinks the sketch of a block whose top edge is filleted until the fillet cannot rebuild, and sketch_edit_empty
deletes every entity: in both sketch_finish replies ok false, that the sketch was not replaced and why, the design
and its undo depth are as they were, and the next sketch_create opens a new sketch, which its sketch_finish adds
after the others. The failed replace's session, exported with replay_export, keeps that sketch_finish and replays
in a new project to the same features. sketch_edit_failed_leave leaves the same failing edit through sketch_text {}
and through K, which finish the sketch before they open the Text dialog or Constrain mode: each stops with the
status line saying the sketch could not be replaced, nothing open and the design as it was.
project_model projects a block's edges with no sketch open, the plane left out and null, and each call adds a Project
feature on XY; with a sketch open, new or re-opened with sketch_edit, the block's top face goes into it as four
construction lines and adds no feature.
text writes a word with sketch_text: a Text feature in one undo step, with no dialog or placement step left open, that
extrudes into a body; a face that is not installed and text of spaces only are refused, as is a call with a sketch
open, and sketch_edit refuses the Text feature. svg_art places an SVG file's square on the model with
sketch_insert_svg, an art feature of one 100 mm2 region at the position, which sketch_edit refuses, and into an open
sketch as four lines. scale_artwork runs the Feature tree's Scale artwork on the art twice: Confirm keeps one undo
step, Cancel none, as Edit on the row does.
surface_revolve_flip selects a sketch's loop with design_select, opens the Surface Revolve card with
surface_revolve {}, ticks its Flip and confirms: the feature keeps the Flip.
The refs rungs work on a block whose top edge is filleted and which is then shelled, renumbering its edges.
feature_edit_refs re-points the Fillet with feature_edit's refs at an edge read with body_describe {before}: the Fillet
rounds it, in one undo step that undo takes back, and ids read at a generation the edit rebuilt are refused (stale_ids).
text_edit changes a Text feature's words and height with feature_edit: two regions, 20 mm high, one undo step.
edit_repick opens the Fillet's card from its row, picks another top edge in the view and confirms: the Fillet names
that edge by its id in the model before it, and edit_repick_placed does so with the block placed off where the model
builds it, clicking the edge where the view draws it. edit_repick_datum does the same in a Plane's card, picking the
top face of a second block that a later Fillet renumbered: the Plane names it by its id in the finished model. edit_repick_hole
opens the card of a Hole drilled into a block's side, which a later Fillet renumbers, and picks the block's top face:
the Hole goes into that face, its plane read in the model before it. edit_repick_hidden hides a second block and opens
the Fillet's card: a click on the hidden block picks nothing, and after the block is shown and hidden again with the
card open, an edge picked is still named by its id in the model before the Fillet.
design_view sets view_set's Design options on a block: a section view at a height and its upper half, one at the
model's middle when the height is left out, and each of the four switches on and off, as the reply reads them back.
sketch_normal with no sketch open is refused, leaving the view as it was, and in a sketch on XZ looks along the
plane's normal; the Design options given without tab design are refused.
select works on a box extruded from a sketch, a Plane on its top face, a second Extrude and a sketch on show, each
call straight after the one before it. design_select selects the body, then design_rename_selected names it in its row's
editor; a face, an edge and a vertex, at the point body_describe gives it; the shown sketch's loop, which
design_rename_selected renames and design_zoom_to_selection frames; and each feature, which design_rename_selected, feature_delete,
feature_edit and sketch_constrain_selected then act on, as design_zoom_to_selection does not. {} clears it all. Under an open
card or sketch, and for an id the model lacks or a plane no click could pick, it is refused and the selection stays
as it was. With an edge, a face, a body and a vertex selected, the menu items that take them apply; the canvas draws a
face, an edge and a vertex selected, each differently. A plane is selected while the Origin row shows the planes, and
starts the sketch Sketch waits for, on XY as its sketch_normal camera reads it.
select_loops extrudes loop 0 of a sketch of three squares alone: design_select's loop 1 is then the loop
sketch_describe calls 1, which design_zoom_to_selection centres the view on; loop 0 is refused, naming the extrude that took
it, and so is a loop past the sketch's.
confirm_card opens the Extrude card on a committed rectangle's loop and confirms it with design_confirm_tool, whose reply names
the card and the new feature; one body exists. cancel_card cancels the same card with design_cancel_tool, adding nothing, and
cancel_sketch discards a sketch holding a line, with no dialog. text_dialog opens the Text dialog with sketch_text {}:
design_cancel_tool closes it, adding nothing, and design_confirm_tool is refused with the dialog's reason until there is text, then
takes its OK, which opens the text's placement, which a second design_confirm_tool keeps. cancel_plane_pick hides the Origin
row's planes and calls sketch_create {}, which waits for a plane: design_confirm_tool is refused, design_cancel_tool ends the wait,
and no plane can be selected after it. confirm_nothing: both are refused with nothing open. confirm_disabled opens a Fillet card that
has no preview, and design_confirm_tool is refused with the status line. confirm_refused_add presses Commit to Plate, which
confirms an open card first, over a Delete Face card holding no face, whose add refuses, so that it sends nothing and
the Design tab stays in front, and over the Insert placement Scale artwork opens; then it opens the Thicken card on a
face and clicks a committed sketch's loop, which lets go of the face and leaves the Confirm on, and presses the Confirm,
which the add refuses. None takes an undo step, and each card stays open. confirm_failed_add presses Commit to Plate
over confirm_disabled's Fillet card, whose add runs and whose rebuild fails: no feature and no undo or redo step are
added, the card stays open with the reason on the status line and its pick, and nothing is sent.
place moves a box with body_place, which design_describe reads as its placement and Commit to Plate sends, turns it
about its centre, lays a side face on the bed and resets it; with a sketch open it is refused. place_select selects a
vertex and a face of a placed box: the vertex's placed_point is the placement's, and fit_selection looks at the box
where it stands. place_follows deletes the feature that made the first of two placed boxes: the other keeps its
placement as it becomes body 0, a box added afterwards has none, and undo brings the first back where it stood.
replace commits two bodies to a new project and replaces through design_commit_to_plate's replace: a part of a
cut, cut with a connector in the Cut gizmo, is refused with cut_object, named, and adds no undo step, replace_ids
deletes both objects in the commit's one undo step, and after a replacing commit the selection, on the canvas and in
the Objects list, is what a plain commit leaves, on the 3D view and on the assemble view, which the call keeps.
boolean_opens calls solid_boolean {} and solid_boolean {operation: "cut"} with two bodies and nothing selected: each replies that
a tool is open, and the card it opened, confirmed with design_confirm_tool, adds a join and a cut; with target and tool the
cut runs. action_pick opens the Speed Dial in a sketch with a line selected and picks the Polar array action, an item
folded into sketch_pattern, which arms the polar array tool, and the Trim action, sketch_trim's own, which arms the trim
tool. action_pick_replay runs the Polar array action with action_run, exports the session, whose step carries the
item's arguments {"type": "circular"}, and replays it in a new project to the same armed tool.
"""

import json
import math
import os
import re
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks  # noqa: E402
import orca_mcp  # noqa: E402
from cad_gui import CANVAS, CadGui, mid, near, until  # noqa: E402

gui = None  # the CadGui main() starts


def square(half):
    """A closed square of four lines, 2 * half on a side, centred on the plane's origin."""
    corners = [[-half, -half], [half, -half], [half, half], [-half, half]]
    return [{"type": "line", "p0": corners[i], "p1": corners[(i + 1) % 4]} for i in range(4)]


def bodies():
    return len(gui.cad("design_describe")["bodies"])


def volume(body):
    return gui.cad("body_mass_properties", body=body)["volume"]


def operation_of(feature):
    return gui.cad("feature_describe", feature=feature)["params"].get("operation")


def refused(tool, **args):
    """Why the Design command `tool` refuses `args`; empty when it runs."""
    try:
        gui.cad(tool, **args)
    except orca_mcp.McpError as e:
        return str(e)
    return ""


def coordinate_system(body, point):
    """A coordinate system at the world point `point` on body `body`; its feature index."""
    return gui.cad("construct_coordinate_system", type="point_world", body=body, point=point)["coordsys_index"]


# ---------------------------------------------------------------- the words

def rung_words():
    # A 10 x 10 square on the block's bottom face, cut 5 mm up into the 20 x 20 x 10 block.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", entities=square(5))
    sketch = gui.cad("sketch_finish")["sketch_index"]
    r = gui.cad("solid_extrude", sketch=sketch, distance=5, operation="cut", body=0)
    gui.check("words: extrude replies the operation it was given", r.get("operation") == "cut", r)
    gui.check("words: solid_extrude's reply says no boolean", "boolean" not in r, r)
    gui.check("words: the cut takes 10 x 10 x 5 out of the block", near(volume(0), 4000.0 - 500.0, 1e-3),
              f"volume {volume(0)}")
    gui.check("words: the extrude reads back as a cut", operation_of(r["extrude_index"]) == "cut",
              operation_of(r["extrude_index"]))

    # A 15 x 10 x 10 block overlapping the first by 5 x 10 x 10, joined into it.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    gui.cad("solid_extrude", operation="new_body", distance=10, profile=[[5, -5], [20, -5], [20, 5], [5, 5]])
    gui.check("words: two bodies before the boolean", bodies() == 2, bodies())
    r = gui.cad("solid_boolean", operation="join", target=0, tool=1)
    gui.check("words: boolean join leaves one body of two", bodies() == 1, bodies())
    gui.check("words: the joined body holds both blocks", near(volume(0), 4000.0 + 1500.0 - 500.0, 1e-3),
              f"volume {volume(0)}")
    gui.check("words: the boolean reads back as a join", operation_of(r["boolean_index"]) == "join",
              operation_of(r["boolean_index"]))

    # A block against the XZ plane, mirrored across it and joined to its image.
    gui.new_design()
    gui.cad("solid_extrude", distance=5, profile=[[-5, 0], [5, 0], [5, 10], [-5, 10]])
    r = gui.cad("solid_mirror", plane="XZ", operation="join")
    gui.check("words: mirror join leaves one body", bodies() == 1, bodies())
    gui.check("words: the joined mirror is twice the block", near(volume(0), 2 * 500.0, 1e-3), f"volume {volume(0)}")
    gui.check("words: the mirror reads back as a join", operation_of(r["mirror_index"]) == "join",
              operation_of(r["mirror_index"]))

    # A block's extrude suppressed: the design lists it as suppressed, and its body is gone.
    gui.new_design()
    extrude = gui.cad("solid_extrude", distance=10, width=20, height=20)["extrude_index"]
    item = gui.cad("design_describe")["features"][extrude]
    gui.check("words: design_describe says suppressed, not enabled",
              item.get("suppressed") is False and "enabled" not in item, item)
    r = gui.cad("feature_suppress", feature=extrude, suppressed=True)
    gui.check("words: feature_suppress replies suppressed", r.get("suppressed") is True and r.get("changed") is True, r)
    gui.check("words: the suppressed extrude reads back so",
              gui.cad("feature_describe", feature=extrude).get("suppressed") is True,
              gui.cad("feature_describe", feature=extrude))
    gui.check("words: the suppressed extrude's body is gone", bodies() == 0, bodies())

    # Two aligns driving one body, which cannot both hold: the second is the conflict.
    gui.new_design()
    gui.cad("solid_extrude", distance=5, width=10, height=10)
    gui.cad("solid_extrude", operation="new_body", distance=10, profile=[[20, -3], [26, -3], [26, 3], [20, 3]])
    cs_a, cs_b, cs_c = coordinate_system(0, [0, 0, 0]), coordinate_system(1, [0, 0, 0]), coordinate_system(0, [10, 10, 10])
    first = gui.cad("solid_align", type="fastened", cs_a=cs_c, cs_b=cs_b)
    gui.check("words: align replies align_index", "align_index" in first and "mate_index" not in first, first)
    second = gui.cad("solid_align", type="fastened", cs_a=cs_a, cs_b=cs_b)
    design = gui.cad("design_describe")
    conflicts = design.get("align_conflicts", [])
    gui.check("words: design_describe lists the second align as the conflict",
              len(conflicts) == 1 and conflicts[0].get("feature") == second.get("align_index"), design.get("align_conflicts"))
    gui.check("words: design_describe says no mate_conflicts", "mate_conflicts" not in design, list(design))

    # The open sketch's commands: a line extended to the line below it, a circle repeated about the
    # origin, a line turned upright, and a width that sketch_dimension holds and sketch_constrain refuses.
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", entities=[{"type": "line", "p0": [-10, 0], "p1": [10, 0]},
                                     {"type": "line", "p0": [0, 3], "p1": [0, 8]},
                                     {"type": "circle_center", "center": [20, 0], "radius": 2},
                                     {"type": "line", "p0": [30, 0], "p1": [34, 0]}])
    why = refused("sketch_trim", at=[0, 3], extend=True)
    gui.check("words: sketch_trim refuses extend as an argument it does not take", "unknown argument extend" in why, why)
    r = gui.cad("sketch_extend", at=[0, 3])
    gui.check("words: sketch_extend runs the line down to the one below it",
              r.get("entity") == 1 and near(r["geometry"]["p0"][1], 0.0) and "extend" not in r, r)
    r = gui.cad("sketch_pattern", type="circular", entities=[2], count=4, pivot=[0, 0])
    gui.check("words: sketch_pattern circular adds three copies", r.get("type") == "circular" and r.get("added") == 3, r)
    copy = gui.sketch()["entities"][r["first_index"]]
    gui.check("words: the first copy is a quarter turn round the origin",
              near(copy["center"][0], 0.0) and near(copy["center"][1], 20.0), copy)
    r = gui.cad("sketch_transform", type="rotate", entities=[3], angle_deg=90, pivot=[30, 0])
    line = gui.sketch()["entities"][3]
    gui.check("words: sketch_transform rotate turns the line upright",
              r.get("type") == "rotate" and near(line["p1"][0], 30.0) and near(line["p1"][1], 4.0), [r, line])
    width = {"type": "distance", "refs": [{"entity": 0, "role": "p0"}, {"entity": 0, "role": "p1"}], "value": 25}
    why = refused("sketch_constrain", **width)
    gui.check("words: sketch_constrain refuses a distance", "type must be one of" in why, why)
    r = gui.cad("sketch_dimension", **width)
    line = gui.sketch()["entities"][0]
    gui.check("words: sketch_dimension holds the line 25 long",
              r["items"][0].get("type") == "distance" and
              near(math.hypot(line["p1"][0] - line["p0"][0], line["p1"][1] - line["p0"][1]), 25.0), [r["items"], line])
    gui.cad("sketch_cancel")


# ---------------------------------------------------------------- the shapes

# The tool a drawing item arms, as sketch_describe names it, so that an item chosen one off is caught where it is. The
# polyline, oblique rectangle, tangent arc and square items are left out: test_cad_context_menu's arm() checks what
# each of them arms, chosen from the same offer on empty space.
ARMS = {"sk_rect": "rect_corner", "sk_slot": "slot"}


def menu_path(verb):
    """The label path of the drawing item `verb` in the offer a right-click on empty space in a sketch opens: its
    row, its family when it has one, and its name, as DesignPanel::show_offer_menu lays out a row of many verbs."""
    v = next(v for v in gui.verbs() if v["id"] == verb)
    return ">".join([v["row_name"]] + ([v["family"]] if v["family"] else []) + [v["name"]])


def same_sketch_value(a, b, key=""):
    """Two sketch_describe values equal: numbers to within 1e-6, an angle modulo a whole turn."""
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        d = a - b
        if key in ("start_angle", "end_angle", "rotation"):
            d = math.remainder(d, 2 * math.pi)
        return abs(d) <= 1e-6
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same_sketch_value(a[k], b[k], k) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same_sketch_value(x, y, key) for x, y in zip(a, b))
    return a == b


def drawn():
    """The open sketch as sketch_describe reads it, with the kinds of its constraints as sketch_list_constraints
    reads them, sorted, under constraint_types."""
    out = gui.sketch()
    out["constraint_types"] = sorted(c["type"] for c in gui.cad("sketch_list_constraints")["items"])
    return out


def disagreement(by_tool, by_command):
    """What two drawn() readings disagree in, of the entities' types and geometry, the constraint count and kinds, the
    loops and dof; empty when they agree."""
    out = [k for k in ("entities", "closed_loops") if not same_sketch_value(by_tool[k], by_command[k])]
    return out + [f"{k} {by_tool[k]} != {by_command[k]}" for k in ("constraints", "constraint_types", "dof")
                  if by_tool[k] != by_command[k]]


def shapes(box):
    """The shapes shapes_match_tools draws: for each, its menu item, what both sketches get first by sketch_draw, the tool's
    clicks (mm), how many entities it adds and how many an undo leaves of them, and the sketch_draw arguments that
    draw it again, from what the tool drew (the entities after the first ones). Positions are shares of the
    calibrated region `box` (x0, x1, y0, y1), so that every click lands in it whatever the window, and none falls
    near the sketch's origin at its centre, which a click would snap onto.

    The tool infers constraints within 3 degrees and sketch_draw only what holds exactly, so the polyline, whose
    lines are no shape of their own, has its sides off the axes and off right angles to each other, where both see
    the same. The 3-point rectangle, the tangent arc, the slot and the square are drawn at a slant, where sketch_draw
    gives them the right angles, the tangencies and the equal ends their tools infer."""
    x0, x1, y0, y1 = box
    cx, cy, s = (x0 + x1) / 2, (y0 + y1) / 2, min(x1 - x0, y1 - y0)

    def at(fx, fy):
        return (cx + fx * s, cy + fy * s)

    def heading(p, deg, length):
        return (p[0] + length * s * math.cos(math.radians(deg)), p[1] + length * s * math.sin(math.radians(deg)))

    def lines(ents):
        return [e for e in ents if e["type"] == "line"]

    def arcs(ents):
        return [e for e in ents if e["type"] == "arc"]

    def width_of(ls):
        # The far side's signed distance to the left of p0 to p1, the tool's own reading of its third click.
        (ax, ay), (bx, by), (fx, fy) = ls[0]["p0"], ls[0]["p1"], ls[1]["p1"]
        n = math.hypot(bx - ax, by - ay)
        return (-(by - ay) * (fx - bx) + (bx - ax) * (fy - by)) / n

    corner_a = at(-0.25, -0.25)
    corner_b = heading(corner_a, 25, 0.4)
    tri_a = at(-0.25, -0.25)
    tri_b = heading(tri_a, 20, 0.4)
    tri_c = heading(tri_b, 100, 0.35)
    leave = (at(-0.3, -0.25), heading(at(-0.3, -0.25), 30, 0.25))
    slot_c0 = at(-0.25, -0.15)
    slot_c1 = heading(slot_c0, 32, 0.35)
    square = at(0.1, -0.05)

    def square_args(ents):
        # The Square tool's centre and first vertex, which sketch_draw's radius and angle give again.
        ls = lines(ents)
        c = [sum(l["p0"][k] for l in ls) / 4 for k in (0, 1)]
        v = ls[0]["p0"]
        return {"center": c, "sides": 4, "radius": math.hypot(v[0] - c[0], v[1] - c[1]),
                "angle": math.atan2(v[1] - c[1], v[0] - c[0])}
    return [
        {"type": "rectangle_2_point", "verb": "sk_rect", "setup": [], "clicks": [at(-0.3, -0.2), at(0.2, 0.15)],
         "adds": 4, "left": 0, "args": lambda e: {"p0": lines(e)[0]["p0"], "p1": lines(e)[1]["p1"]}},
        {"type": "polyline", "verb": "sk_polyline", "setup": [], "clicks": [tri_a, tri_b, tri_c, tri_a], "each_click": True,
         "adds": 3, "left": 2, "args": lambda e: {"points": [l["p0"] for l in lines(e)] + [lines(e)[0]["p0"]]}},
        {"type": "rectangle_3_point", "verb": "sk_rect_oblique", "setup": [],
         "clicks": [corner_a, corner_b, heading(corner_b, 115, 0.3)], "adds": 4, "left": 0,
         "args": lambda e: {"p0": lines(e)[0]["p0"], "p1": lines(e)[0]["p1"], "width": width_of(lines(e))}},
        {"type": "arc_tangent", "verb": "sk_arc_tangent",
         "setup": [{"type": "line", "p0": list(leave[0]), "p1": list(leave[1])}],
         "clicks": [leave[1], at(0.15, 0.15)], "adds": 1, "left": 0, "args": lambda e: {"end": e[0]["p1"]}},
        {"type": "slot", "verb": "sk_slot", "setup": [], "clicks": [slot_c0, slot_c1, heading(slot_c1, 122, 0.06)],
         "adds": 4, "left": 0,
         "args": lambda e: {"c0": arcs(e)[1]["center"], "c1": arcs(e)[0]["center"], "width": 2 * arcs(e)[0]["radius"]}},
        {"type": "polygon", "verb": "sk_poly_4", "setup": [], "clicks": [square, heading(square, 23, 0.18)],
         "adds": 4, "left": 0, "args": square_args},
    ]


# ---------------------------------------------------------------- a committed sketch edited

def rectangle_block():
    """A 20 x 10 rectangle on XY from the plane's origin, finished as feature 0 and extruded 5 mm whole as feature 1,
    so that the block follows the sketch: 1000 mm3."""
    gui.new_design()
    return block_on_rectangle()


def block_on_rectangle():
    """rectangle_block() in the design as it stands."""
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", type="rectangle_2_point", p0=[0, 0], p1=[20, 10])
    sketch = gui.cad("sketch_finish")["sketch_index"]
    gui.cad("solid_extrude", sketch=sketch, distance=5)
    return sketch


def state():
    """What an edit that is not kept must leave as it was: the features by type and name, the undo depth and the
    block's volume."""
    design = gui.cad("design_describe")
    return {"features": [(f["type"], f["name"]) for f in design["features"]], "undo_depth": design["undo_depth"],
            "volume": round(volume(0), 6)}


def opens_a_new_sketch(rung):
    """sketch_create opens a sketch of its own, which sketch_finish adds after the others rather than putting in place
    of the sketch an edit ended on."""
    features = len(gui.cad("design_describe")["features"])
    why = refused("sketch_create", plane="XY")
    if not gui.check(f"{rung}: sketch_create then opens a sketch", not why, why):
        return
    gui.cad("sketch_draw", type="circle_center", center=[40, 0], radius=2)
    r = gui.cad("sketch_finish")
    gui.check(f"{rung}: the next sketch is added, not put in place of another",
              r.get("sketch_index") == features and "replaced" not in r, r)


def rung_sketch_edit():
    sketch = rectangle_block()
    before = gui.cad("design_describe")
    gui.check("sketch_edit: the block is 1000 mm3", near(volume(0), 1000.0, 1e-6), volume(0))
    r = gui.cad("sketch_edit", sketch=sketch)
    gui.check("sketch_edit: the reply names the sketch and its four entities",
              r.get("sketch") == sketch and r.get("entities") == 4 and r.get("plane", {}).get("base") == "XY", r)
    gui.check("sketch_edit: sketch_describe reads the sketch's four entities", len(gui.sketch()["entities"]) == 4,
              gui.sketch()["entities"])
    gui.cad("sketch_transform", type="scale", scale=2, pivot=[0, 0], entities=[0, 1, 2, 3])
    r = gui.cad("sketch_finish")
    gui.check("sketch_edit: sketch_finish replaces the sketch it re-opened",
              r.get("replaced") is True and r.get("sketch_index") == sketch, r)
    after = gui.cad("design_describe")
    gui.check("sketch_edit: no feature is added", len(after["features"]) == len(before["features"]),
              [f["name"] for f in after["features"]])
    gui.check("sketch_edit: the block follows the sketch, twice as wide and long", near(volume(0), 4000.0, 1e-6),
              volume(0))
    gui.check("sketch_edit: the replace is one undo step", after["undo_depth"] == before["undo_depth"] + 1,
              f"{before['undo_depth']} -> {after['undo_depth']}")
    gui.client.call_command("undo", history="design")
    gui.check("sketch_edit: one undo brings the block back", near(volume(0), 1000.0, 1e-6), volume(0))


def rung_sketch_edit_cancel():
    sketch = rectangle_block()
    before = state()
    gui.cad("sketch_edit", sketch=sketch)
    gui.cad("sketch_draw", type="circle_center", center=[10, 5], radius=2)
    gui.cad("sketch_cancel")
    dialogs = gui.client.call("app_state").get("dialogs", [])
    gui.check("sketch_edit_cancel: sketch_cancel asks nothing", not dialogs, dialogs)
    gui.check("sketch_edit_cancel: no sketch is left open", gui._sketch_or_none() is None, gui._sketch_or_none())
    gui.check("sketch_edit_cancel: the design is as it was", state() == before, f"{before} -> {state()}")
    kept = gui.cad("sketch_describe", sketch=sketch)["entities"]
    gui.check("sketch_edit_cancel: the sketch keeps its four entities", len(kept) == 4, kept)
    opens_a_new_sketch("sketch_edit_cancel")


def rung_sketch_edit_refused():
    sketch = rectangle_block()
    why = refused("sketch_edit", sketch=7)
    gui.check("sketch_edit_refused: an index past the features", "out of range" in why, why)
    why = refused("sketch_edit", sketch=sketch + 1)
    gui.check("sketch_edit_refused: the extrude is not a sketch", "is not a sketch" in why, why)
    # A block's sketch from the card's width and height has no entities; a projection follows its body.
    shape = gui.cad("solid_extrude", operation="new_body", distance=5, width=4, height=4)["sketch_index"]
    why = refused("sketch_edit", sketch=shape)
    gui.check("sketch_edit_refused: a sketch not drawn from entities", "not drawn from entities" in why, why)
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    projected = gui.cad("sketch_project", source_body=0, face=top["faces"][0]["id"], plane="XY",
                        generation=top["generation"])
    why = refused("sketch_edit", sketch=projected["project_index"])
    gui.check("sketch_edit_refused: a Project sketch", "Project" in why, why)
    gui.cad("sketch_create", plane="XY")
    why = refused("sketch_edit", sketch=sketch)
    gui.check("sketch_edit_refused: any index while a sketch is open", "a sketch is already open" in why, why)
    gui.cad("sketch_cancel")


def round_top_edge():
    """One top edge along x of rectangle_block()'s block rounded by 4 mm, as feature 2: an edge a block a tenth the
    size cannot take."""
    edges = gui.cad("body_describe", body=0, edge_kinds=["line"], direction=[1, 0, 0])
    top = max(edges["edges"], key=lambda e: e["p0"][2])
    gui.cad("solid_fillet", edges=top["id"], radius=4, body=0, generation=edges["generation"])


def outcome(tool, **args):
    """The Design command's reply, or {"raised": its error} when the call fails, so that what comes after it is
    still checked."""
    try:
        return gui.cad(tool, **args)
    except orca_mcp.McpError as e:
        return {"raised": str(e)}


def not_replaced(rung, reply, sketch, why):
    """sketch_finish's reply for a re-opened sketch it did not put back: ok and replaced false, the sketch's index,
    and an error that says it was not replaced and why (`why` in it, or any reason when `why` is empty)."""
    error = reply.get("error", "")
    reason = error.split("the sketch was not replaced: ", 1)[1] if error.startswith("the sketch was not replaced: ") else ""
    gui.check(f"{rung}: sketch_finish replies that the sketch was not replaced, and why",
              reply.get("ok") is False and reply.get("replaced") is False and reply.get("sketch_index") == sketch
              and reason.strip() != "" and why in reason, reply)


def steps_of(path):
    """The steps of the replay replay_export wrote at `path`, its // comment lines left out."""
    with open(path) as f:
        text = f.read()
    return json.loads("\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//")))["steps"]


def replay_in_new_project(path, out_dir):
    """replay_run's reply for the session replay_export wrote at `path`, replayed into a new project on the Design tab,
    where the live session ran: a new project shows Prepare, which refuses a Design tab action as wrong_mode."""
    gui.client.call("project_new", discard_changes=True)
    gui.show_design()
    return gui.client.await_job(gui.client.call("replay_run", path=path, out_dir=out_dir), timeout=900)


def rung_sketch_edit_failed_replace():
    # The session the replay must reproduce starts on the empty design.
    gui.new_design()
    gui.client.call("replay_export", clear=True)
    sketch = block_on_rectangle()
    round_top_edge()
    before = state()
    gui.cad("sketch_edit", sketch=sketch)
    gui.cad("sketch_transform", type="scale", scale=0.1, pivot=[0, 0], entities=[0, 1, 2, 3])
    not_replaced("sketch_edit_failed_replace", outcome("sketch_finish"), sketch, "")
    gui.check("sketch_edit_failed_replace: no sketch is left open", gui._sketch_or_none() is None, gui._sketch_or_none())
    gui.check("sketch_edit_failed_replace: the design is as it was, with no undo step",
              state() == before, f"{before} -> {state()}")
    opens_a_new_sketch("sketch_edit_failed_replace")

    # The replay keeps the sketch_finish that ended the edit, so that it ends there too, and the sketch after it is
    # a new one: the exported session replays in a new project to the same features.
    live = [f["type"] for f in gui.cad("design_describe")["features"]]
    out_dir = tempfile.mkdtemp(prefix="orca-mcp-sketch-edit-")
    path = os.path.join(out_dir, "session.json")
    gui.client.call("replay_export", path=path, overwrite=True)
    tools = [s["tool"] for s in steps_of(path)]
    gui.check("sketch_edit_failed_replace: the export keeps the sketch_finish that did not replace",
              tools.count("sketch_edit") == 1 and tools.count("sketch_finish") == 3, tools)
    replay = replay_in_new_project(path, out_dir)
    gui.check("sketch_edit_failed_replace: the exported session replays", replay.get("passed") is True,
              [s for s in replay.get("steps", []) if not s.get("ok")][:1])
    replayed = [f["type"] for f in gui.cad("design_describe")["features"]]
    gui.check("sketch_edit_failed_replace: the replay ends with the live run's features", replayed == live,
              (replayed, live))
    gui.show_design()


def failing_edit():
    """rectangle_block() with its top edge rounded, its sketch re-opened with sketch_edit and shrunk to a tenth, which
    the fillet cannot rebuild on: what state() read before the edit, and the sketch's index."""
    sketch = rectangle_block()
    round_top_edge()
    before = state()
    gui.cad("sketch_edit", sketch=sketch)
    gui.cad("sketch_transform", type="scale", scale=0.1, pivot=[0, 0], entities=[0, 1, 2, 3])
    return before, sketch


def rung_sketch_edit_failed_leave():
    # sketch_text {}, the Text item, and K, Constrain, finish the open sketch before they open the Text dialog or
    # Constrain mode on it. A re-opened sketch whose replace fails stops each there: nothing opens on the sketch as it
    # was, and the status line says why.
    for route, leave in (("sketch_text {}", lambda: gui.cad("sketch_text")),
                         ("K", lambda: gui.keys("k", target=CANVAS))):
        rung = f"sketch_edit_failed_leave, {route}"
        before, sketch = failing_edit()
        leave()
        gui.check(f"{rung}: the status line says the sketch could not be replaced",
                  status_says("The sketch could not be replaced"), [w.label for w in gui.widgets() if w.kind == "text"][:8])
        gui.check(f"{rung}: no Text dialog opens", not gui.modeless(), gui.modeless())
        gui.check(f"{rung}: neither Constrain mode nor a sketch is open", gui._sketch_or_none() is None, gui._sketch_or_none())
        gui.check(f"{rung}: no card is open", not card_open(), card_open())
        gui.check(f"{rung}: the design is as it was", state() == before, f"{before} -> {state()}")
        box = [p for e in gui.cad("sketch_describe", sketch=sketch)["entities"] for p in (e["p0"], e["p1"])]
        size = [max(p[k] for p in box) - min(p[k] for p in box) for k in (0, 1)]
        gui.check(f"{rung}: the sketch keeps its 20 x 10 rectangle", near(size[0], 20) and near(size[1], 10), box)
        gui._cancel_modeless()   # a Text dialog the route opened, so that the next starts from none


def rung_sketch_edit_empty():
    sketch = rectangle_block()
    before = state()
    gui.cad("sketch_edit", sketch=sketch)
    gui.cad("sketch_delete", entities=[0, 1, 2, 3])
    not_replaced("sketch_edit_empty", outcome("sketch_finish"), sketch, "empty")
    kept = gui.cad("sketch_describe", sketch=sketch)["entities"]
    gui.check("sketch_edit_empty: the sketch keeps its four entities", len(kept) == 4, kept)
    gui.check("sketch_edit_empty: the design is as it was", state() == before, f"{before} -> {state()}")
    opens_a_new_sketch("sketch_edit_empty")


# ---------------------------------------------------------------- projection

def is_xy(frame, origin):
    """Whether the reply's `frame` is the XY plane through the design's modeling origin."""
    want = {"normal": [0, 0, 1], "x_axis": [1, 0, 0], "origin": origin}
    return all(len(frame.get(k, [])) == 3 and all(near(a, b) for a, b in zip(frame[k], v)) for k, v in want.items())


def rung_project_model():
    # A 20 x 20 x 10 block, projected with no sketch open.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    origin = gui.cad("design_describe")["modeling_origin"]
    for args in ({"source_body": 0}, {"source_body": 0, "plane": None}):
        r = outcome("sketch_project", **args)
        features = gui.cad("design_describe")["features"]
        index = r.get("project_index")
        gui.check(f"project_model: sketch_project {args} adds a Project feature",
                  "ok" not in r and index == len(features) - 1 and features[index]["type"] == "Project", [r, features])
        gui.check(f"project_model: sketch_project {args} replies scope feature", r.get("scope") == "feature", r)
        gui.check(f"project_model: sketch_project {args} projects onto XY", is_xy(r.get("frame", {}), origin), r)

    # With a sketch open, the top face's edges go into it as construction lines, once, and no feature is added.
    count = len(gui.cad("design_describe")["features"])

    def top_face():
        top = gui.cad("body_describe", body=0, at_extreme="+z")
        return {"source_body": 0, "face": top["faces"][0]["id"], "generation": top["generation"]}

    gui.cad("sketch_create", plane="XY")
    r = outcome("sketch_project", **top_face())
    ents = gui.sketch()["entities"]
    gui.check("project_model: with a sketch open, the top face goes into it as four construction lines",
              r.get("scope") == "sketch" and r.get("added") == 4 and len(ents) == 4
              and all(e["type"] == "line" and e["construction"] for e in ents), [r, ents])
    gui.cad("sketch_draw", type="circle_center", center=[0, 0], radius=3)
    sketch = gui.cad("sketch_finish")["sketch_index"]
    gui.check("project_model: the sketch is the one feature added", sketch == count, sketch)

    # A committed sketch takes them through sketch_edit. Its finish rebuilt the model, so the face's id is read again.
    gui.cad("sketch_edit", sketch=sketch)
    r = outcome("sketch_project", **top_face())
    gui.check("project_model: a sketch re-opened with sketch_edit takes them too", r.get("scope") == "sketch"
              and r.get("first_index") == 5 and r.get("added") == 4, r)
    gui.cad("sketch_finish")
    kept = gui.cad("sketch_describe", sketch=sketch)["entities"]
    gui.check("project_model: the edited sketch keeps them, construction beside its circle",
              len(kept) == 9 and sum(1 for e in kept if e["construction"]) == 8, kept)
    gui.check("project_model: no feature is added by either",
              len(gui.cad("design_describe")["features"]) == count + 1, gui.cad("design_describe")["features"])


# ---------------------------------------------------------------- text and art

# A drawing 40 mm square with one filled 10 x 10 mm square off its centre.
SQUARE_SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="40mm" viewBox="0 0 40 40">'
              '<rect x="20" y="5" width="10" height="10" fill="black"/></svg>')


def square_svg():
    """SQUARE_SVG written to a temporary folder; its path."""
    path = os.path.join(tempfile.mkdtemp(prefix="orca-mcp-svg-"), "square.svg")
    with open(path, "w") as f:
        f.write(SQUARE_SVG)
    return path


def undo_depth():
    return gui.cad("design_describe")["undo_depth"]


def features():
    return len(gui.cad("design_describe")["features"])


def nothing_open(rung):
    """No dialog is open, modal or modeless, and no sketch session: no placement step was left for a person."""
    dialogs = gui.client.call("app_state").get("dialogs", [])
    gui.check(f"{rung}: no dialog is open", not dialogs and not gui.modeless(), [dialogs, gui.modeless()])
    gui.check(f"{rung}: no sketch session is open", gui._sketch_or_none() is None, gui._sketch_or_none())


def rung_text():
    gui.new_design()
    before = undo_depth()
    r = outcome("sketch_text", text="Orca", height=12)
    index = r.get("text_index")
    if not gui.check("text: sketch_text replies text_index", isinstance(index, int), r):
        return
    params = gui.cad("feature_describe", feature=index)["params"]
    gui.check("text: the feature reads the words, the height and four regions",
              params.get("text_string") == "Orca" and near(params.get("text_height", 0), 12) and params.get("regions") == 4,
              params)
    gui.check("text: one undo step", undo_depth() == before + 1, f"{before} -> {undo_depth()}")
    nothing_open("text")
    r = outcome("solid_extrude", sketch=index, distance=2)
    gui.check("text: the text extrudes into a body, nothing open refusing it", "ok" not in r and bodies() > 0, r)

    # Refused, changing nothing: a face that is not installed, and text of spaces only. No feature, no undo step and no
    # error in design_describe.
    count, depth = features(), undo_depth()
    for what, args, reason in (("a font that does not exist", {"text": "Orca", "font": "No Such Face 3.14"}, "not installed"),
                               ("text of spaces only", {"text": "   "}, "no outline")):
        why = refused("sketch_text", **args)
        gui.check(f"text: {what} is refused", reason in why, why)
        gui.check(f"text: {what} adds no feature", features() == count, features())
        gui.check(f"text: {what} takes no undo step", undo_depth() == depth, f"{depth} -> {undo_depth()}")
        error = gui.cad("design_describe").get("error", "")
        gui.check(f"text: {what} leaves design_describe's error empty", error == "", error)

    # Not inside an open sketch, and its feature is no sketch to edit.
    gui.cad("sketch_create", plane="XY")
    why = refused("sketch_text", text="Orca", height=12)
    gui.check("text: with a sketch open it is refused as busy", "busy" in why, why)
    gui.cad("sketch_cancel")
    why = refused("sketch_edit", sketch=index)
    gui.check("text: sketch_edit of the Text feature is refused as text", "is text" in why, why)


def art_at(position):
    """sketch_insert_svg of SQUARE_SVG on the model, its centre at `position`: the art's index, or None."""
    r = outcome("sketch_insert_svg", path=square_svg(), position=position)
    gui.check("sketch_insert_svg on the model replies art_index", isinstance(r.get("art_index"), int), r)
    return r.get("art_index")


def rung_svg_art():
    gui.new_design()
    before = undo_depth()
    index = art_at([5, 5])
    if index is None:
        return
    params = gui.cad("feature_describe", feature=index)["params"]
    areas, offset = params.get("region_areas", []), params.get("import_offset", [])
    gui.check("svg_art: the art's one region is the 10 x 10 square",
              params.get("regions") == 1 and len(areas) == 1 and near(areas[0], 100.0, 1e-3), params)
    gui.check("svg_art: the art's import_offset is the position",
              len(offset) == 2 and near(offset[0], 5.0) and near(offset[1], 5.0), params)
    gui.check("svg_art: one undo step", undo_depth() == before + 1, f"{before} -> {undo_depth()}")
    nothing_open("svg_art")
    why = refused("sketch_edit", sketch=index)
    gui.check("svg_art: sketch_edit of it is refused as imported art", "imported art" in why, why)

    # In an open sketch the square's sides go into it as lines, twice the size.
    gui.cad("sketch_create", plane="XY")
    r = outcome("sketch_insert_svg", path=square_svg(), scale=2)
    ents = gui.sketch()["entities"]
    gui.check("svg_art: with a sketch open, the square goes into it as four lines 20 long",
              r.get("scope") == "sketch" and r.get("added") == 4 and len(ents) == 4
              and all(e["type"] == "line" and not e["construction"] and near(e["length"], 20.0, 1e-4) for e in ents),
              [r, ents])
    gui.cad("sketch_cancel")


def row_menu(name, item):
    """The item `item` of the Feature tree row `name`'s right-click menu."""
    gui.client.call("widget_click", target={"row": name}, button="right", popup_item=item)


def rung_scale_artwork():
    # The placement Scale artwork opens is behind the Insert card's Confirm and Cancel, as Edit's is. Enter
    # confirms what is open and Esc cancels it, as the card's buttons do.
    gui.new_design()
    index = art_at([0, 0])
    if index is None:
        return
    name = gui.cad("design_describe")["features"][index]["name"]
    before = undo_depth()
    row_menu(name, "Scale artwork")
    gui.keys("Return", target=CANVAS)
    gui.check("scale_artwork: Scale artwork, then Confirm, keeps one undo step", undo_depth() == before + 1,
              f"{before} -> {undo_depth()}")
    row_menu(name, "Scale artwork")
    gui.keys("Escape", target=CANVAS)
    gui.check("scale_artwork: Scale artwork again, then Cancel, keeps none", undo_depth() == before + 1,
              f"{before} -> {undo_depth()}")
    nothing_open("scale_artwork")
    # Edit on the same row does as Scale artwork does.
    row_menu(name, "Edit")
    gui.keys("Return", target=CANVAS)
    gui.check("scale_artwork: Edit on the row, then Confirm, keeps one undo step too", undo_depth() == before + 2,
              f"{before} -> {undo_depth()}")
    nothing_open("scale_artwork")


# ---------------------------------------------------------------- the model's gaps

def rung_surface_revolve_flip():
    # The Surface Revolve card keeps its Flip when it adds the feature, as its preview and an edit keep it.
    gui.new_design()
    gui.cad("sketch_create", plane="XZ")
    gui.cad("sketch_draw", type="rectangle_2_point", p0=[10, 0], p1=[14, 20])
    sketch = gui.cad("sketch_finish")["sketch_index"]
    # Its menu item takes a picked loop, and the card revolves the sketch the loop is of.
    gui.cad("design_select", sketch=sketch, loop=0)
    opened = gui.cad("surface_revolve")
    shown = until(lambda: any(w.kind == "toggle" and w.label == "Flip direction" for w in gui.widgets()))
    if not gui.check("surface_revolve_flip: surface_revolve {} opens the Surface Revolve card, with its Flip",
                     opened.get("interactive") is True and shown, [opened, shown]):
        return
    ticked = gui.press("Flip direction")
    gui.check("surface_revolve_flip: Flip is ticked", ticked.get("checked") is True, ticked)
    gui.press("Confirm")
    settled()
    added = [f for f in gui.cad("design_describe")["features"] if f["type"] == "SurfaceRevolve"]
    if not gui.check("surface_revolve_flip: Confirm adds a Surface Revolve", len(added) == 1, added):
        return
    params = gui.cad("feature_describe", feature=added[0]["index"])["params"]
    gui.check("surface_revolve_flip: the feature keeps Flip", params.get("flip") is True, params)


def view(**args):
    """view_set's reply to `args` given with tab design."""
    return gui.client.call("view_set", tab="design", **args)


def view_refused(**args):
    """Why view_set refuses `args`; empty when it runs."""
    try:
        gui.client.call("view_set", **args)
    except orca_mcp.McpError as e:
        return str(e)
    return ""


def rung_design_view():
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    box = gui.cad("design_describe")["bodies"][0]["bbox"]
    middle = (box["min"][2] + box["max"][2]) / 2
    section = view(section={"on": True, "z": 2, "keep": "upper"}).get("design", {}).get("section")
    gui.check("design_view: the reply has the section view asked for", section == {"on": True, "z": 2, "keep": "upper"},
              section)
    view(section={"on": False})
    section = view(section={"on": True}).get("design", {}).get("section") or {}
    gui.check("design_view: with z left out the section cuts at the model's middle, keeping the lower half",
              section.get("on") is True and near(section.get("z") or 0, middle, 1e-6) and section.get("keep") == "lower",
              [section, middle])
    section = view(section={"on": False}).get("design", {}).get("section")
    gui.check("design_view: off, the section view has no cut to report", section == {"on": False, "z": None, "keep": None},
              section)

    # Each switch set true then false reads back as set, and is then put back as it was.
    start = view().get("design", {})
    for switch in ("origin_planes", "world_axes", "origin", "bed"):
        for on in (True, False):
            shown = view(**{switch: on}).get("design", {}).get(switch)
            gui.check(f"design_view: {switch} set {on} reads back {on}", shown is on, shown)
        if switch in start:
            view(**{switch: start[switch]})

    # With no sketch open there is no sketch to look at, and the refused call leaves the view as it was.
    why = view_refused(tab="design", camera="sketch_normal", section={"on": True})
    gui.check("design_view: sketch_normal with no sketch open is refused", "No sketch is open in the Design tab" in why,
              why)
    section = view().get("design", {}).get("section") or {}
    gui.check("design_view: the refused call leaves the section view off", section.get("on") is False, section)
    # So is a camera the Design view does not take, before the section view changes.
    why = view_refused(tab="design", camera="fit_bed", section={"on": True})
    section = view().get("design", {}).get("section") or {}
    gui.check("design_view: fit_bed is refused, leaving the section view off",
              "The Design tab has no bed to fit the view to." in why and section.get("on") is False, [why, section])

    frame = gui.cad("sketch_create", plane="XZ")["frame"]
    state = view(camera="sketch_normal").get("camera_state", {})
    seen_from = [p - t for p, t in zip(state.get("position", []), state.get("look_at", []))]
    length = math.sqrt(sum(a * a for a in seen_from))
    along = sum(a * n for a, n in zip(seen_from, frame["normal"])) / length if length else 0
    gui.check("design_view: in a sketch on XZ, sketch_normal looks along the plane's normal", along >= 0.999,
              [state, frame["normal"]])
    # The Design tab's options given without tab design are refused, whatever tab is asked for.
    for args in ({"tab": "prepare", "origin": True}, {"tab": "prepare", "camera": "sketch_normal"},
                 {"camera": "sketch_normal"}):
        why = view_refused(**args)
        gui.check(f"design_view: {args} is refused as the Design tab's", "are the Design tab's: give them with tab design"
                  in why, why)
    gui.cad("sketch_cancel")


def select(**args):
    return gui.cad("design_select", **args)


def selected():
    """design_describe's selection id, picked_plane and feature_row."""
    d = gui.cad("design_describe")
    return d["selection"]["id"], d.get("picked_plane"), d.get("feature_row")


def rename_selected(old, name):
    """Call design_rename_selected {}, then type `name` into the row editor it opens on the row named `old`, as a person does,
    and commit it with Enter. The editor opens holding the row's name."""
    reply = gui.cad("design_rename_selected")
    found = until(lambda: next((w for w in gui.widgets() if w.kind == "text" and w.rect and w.value == old), None))
    if found is None:
        raise AssertionError(f"design_rename_selected opened no row editor on {old}: {reply}")
    gui.fill({"ref": found.ref}, name)
    until(lambda: name in gui.feature_names() or any(b["name"] == name for b in gui.cad("design_describe")["bodies"]))


def canvas_rows(out_dir, name):
    """The upper three quarters of a canvas screenshot's rows as its PNG stores them, apart from the status line and
    the readout the canvas draws along its bottom: equal for two screenshots exactly when their pixels there are."""
    path = os.path.join(out_dir, name + ".png")
    gui.client.call("window_screenshot", target="canvas", include_image=False, save_path=path, overwrite=True)
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, width, height, channels = 8, b"", 0, 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour not in (2, 6):
                raise AssertionError(f"{path} is not an 8-bit RGB or RGBA PNG")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    stride = width * channels + 1   # a filter byte, then the row
    return zlib.decompress(idat)[:stride * (height * 3 // 4)]


def select_model():
    """A 40 x 40 x 10 box extruded from a committed square on XY, which the box takes, so the view does not show it; a
    Plane on its top face; a second Extrude, a block beside it; and a circle sketched on XY that nothing extrudes, which
    the view shows. Returns their feature indices."""
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", type="rectangle_2_point", p0=[-20, -20], p1=[20, 20])
    sketch = gui.cad("sketch_finish")["sketch_index"]
    box = gui.cad("solid_extrude", sketch=sketch, distance=10)["extrude_index"]
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    plane = gui.cad("construct_plane", type="coincident", face_a={"body": 0, "face": top["faces"][0]["id"]},
                    generation=top["generation"])["plane_index"]
    second = gui.cad("solid_extrude", distance=5, operation="new_body",
                     profile=[[30, -5], [40, -5], [40, 5], [30, 5]])["extrude_index"]
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", type="circle_center", center=[-40, 0], radius=5)
    shown = gui.cad("sketch_finish")["sketch_index"]
    return {"sketch": sketch, "box": box, "plane": plane, "second": second, "shown": shown}


def rung_select():
    f = select_model()
    out_dir = tempfile.mkdtemp(prefix="orca-mcp-select-")
    gen = generation()
    top = gui.cad("body_describe", body=0, at_extreme="+z")["faces"][0]["id"]
    along_x = gui.cad("body_describe", body=0, edge_kinds=["line"], direction=[1, 0, 0])["edges"]
    edge = max(along_x, key=lambda e: e["p0"][1] + e["p0"][2])["id"]   # the top edge at +y
    vertex = max(gui.cad("body_describe", body=0, vertices=True)["vertices"], key=lambda v: sum(v["p"]))   # a top corner

    # The body, as a click on its row of the Bodies list selects it: Rename edits that row.
    r = select(body=0)
    gui.check("select: {body: 0} reads back body_solid", r["selection"]["id"] == "body_solid" and selected()[0] == "body_solid",
              [r, selected()])
    rename_selected(gui.cad("design_describe")["bodies"][0]["name"], "Lid")
    body = gui.cad("design_describe")["bodies"][0]
    gui.check("select: design_rename_selected then names the body in its row's editor", body["name"] == "Lid" and body["user_name"],
              body)

    # A face, edges and a vertex, as a click picks them, each straight after the one before.
    r = select(body=0, face=top, generation=gen)
    gui.check("select: {body, face} reads back face_planar", r["selection"]["id"] == "face_planar", r)
    r = select(body=0, edges=[edge], generation=gen)
    gui.check("select: {body, edges} reads back edge_str", r["selection"]["id"] == "edge_str", r)
    r = select(body=0, vertex=vertex["id"], generation=gen)
    point, placed = r.get("point") or [], r.get("placed_point") or []
    gui.check("select: {body, vertex} reads back vertex, at the point body_describe gives it, where the view draws it",
              r["selection"]["id"] == "vertex" and len(point) == 3 and all(near(a, b) for a, b in zip(point, vertex["p"]))
              and len(placed) == 3 and all(near(a, b) for a, b in zip(placed, point)), [r, vertex])

    # The shown sketch's loop, straight after the vertex: its Sketch's row is the one Rename edits.
    r = select(sketch=f["shown"], loop=0)
    gui.check("select: {sketch, loop} reads back sk_loop", r["selection"]["id"] == "sk_loop", r)
    rename_selected(gui.feature_names()[f["shown"]], "Ring")
    gui.check("select: design_rename_selected then renames the Sketch", gui.feature_names()[f["shown"]] == "Ring",
              gui.feature_names())
    r = gui.cad("design_zoom_to_selection")
    gui.check("select: design_zoom_to_selection applies to the loop", r.get("applies") is True, r)

    # Features of the Feature list, straight after the loop, which the four menu items that act on its row act on.
    r = select(feature=f["box"])
    gui.check("select: {feature} reads back the feature's row, and nothing picked in the view",
              r["feature"] == f["box"] and r["selection"]["id"] == "none" and selected() == ("none", None, f["box"]), r)
    rename_selected(gui.feature_names()[f["box"]], "Block")
    gui.check("select: design_rename_selected then renames that feature", gui.feature_names()[f["box"]] == "Block",
              gui.feature_names())
    names = gui.feature_names()
    select(feature=f["second"])
    gui.cad("feature_delete")
    settled()
    gui.check("select: feature_delete then deletes the second Extrude", names[f["second"]] not in gui.feature_names(),
              gui.feature_names())
    gui.client.call_command("undo", history="design")
    settled()
    gui.check("select: one undo brings it back", gui.feature_names() == names, gui.feature_names())
    select(feature=f["plane"])
    rename_selected(gui.feature_names()[f["plane"]], "Datum")
    gui.check("select: design_rename_selected then renames the Plane", gui.feature_names()[f["plane"]] == "Datum",
              gui.feature_names())
    before = [(x["type"], x["name"]) for x in gui.cad("design_describe")["features"]]
    r = gui.cad("feature_edit")
    gui.check("select: feature_edit {} then opens the Plane's card", "a feature card is open" in (r.get("busy") or ""), r)
    gui.cancel()
    select(feature=f["shown"])
    r = gui.cad("sketch_constrain_selected")
    gui.check("select: sketch_constrain_selected then enters Constrain mode on the Sketch",
              "constraining a sketch" in (r.get("busy") or ""), r)
    gui.cancel()
    after = [(x["type"], x["name"]) for x in gui.cad("design_describe")["features"]]
    gui.check("select: the card and Constrain mode, cancelled, leave the features as they were", after == before,
              [before, after])
    select(feature=f["box"])
    why = refused("design_zoom_to_selection")
    gui.check("select: design_zoom_to_selection is refused for a feature, as with nothing selected",
              "does not apply to what is selected" in why and '"selection":"none"' in why.replace(" ", ""), why)

    r = select()
    gui.check("select: {} clears every selection", r["selection"]["id"] == "none" and r["plane"] is None
              and r["feature"] is None and selected() == ("none", None, None), [r, selected()])

    # What is open: a card, then a sketch, refuses it, and the selection stays.
    gen = generation()
    select(body=0)
    r = gui.cad("solid_shell")
    gui.check("select: solid_shell {} opens the Shell card", "a feature card is open" in (r.get("busy") or ""), r)
    held = gui.cad("design_describe")["selection"]
    for args in ({"body": 0, "face": top, "generation": gen}, {}):
        why = refused("design_select", **args)
        gui.check(f"select: {args} is refused as busy with the card", "design_busy" in why and "a feature card is open" in why,
                  why)
    gui.check("select: the card's selection stays", gui.cad("design_describe")["selection"] == held,
              [held, gui.cad("design_describe")["selection"]])
    gui.cancel()
    gui.cad("sketch_create", plane="XY")
    for args in ({"body": 0}, {}):
        why = refused("design_select", **args)
        gui.check(f"select: {args} is refused as busy with the sketch", "design_busy" in why and "a sketch is open" in why, why)
    gui.cad("sketch_cancel")

    # A refused call changes nothing.
    start = view().get("design", {})
    view(origin=False)
    gen = generation()
    select(body=0, face=top, generation=gen)
    for what, args in (("a face the body lacks", {"body": 0, "face": 999}), ("a body with a sketch", {"body": 0, "sketch": 0}),
                       ("a plane while no click could pick one", {"plane": "XY"})):
        why = refused("design_select", **args)
        gui.check(f"select: {what} is refused", why != "", why)
        gui.check(f"select: {what}, refused, leaves the face selected", selected()[0] == "face_planar", selected())
    why = refused("design_select", body=0, face=top, generation=gen + 1)
    gui.check("select: a stale generation is refused", "stale_ids" in why, why)

    # The menu items that take what is selected apply to it.
    select(body=0, edges=[edge], generation=gen)
    r = gui.cad("solid_fillet")
    gui.check("select: with an edge selected solid_fillet {} applies and opens its card",
              r.get("applies") is True and "a feature card is open" in (r.get("busy") or ""), r)
    gui.cancel()
    select(body=0, face=top, generation=gen)
    r = gui.cad("solid_delete_face")
    gui.check("select: with a face selected solid_delete_face {} applies", r.get("applies") is True, r)
    gui.cancel()
    for what, args in (("a body", {"body": 0}), ("a vertex", {"body": 0, "vertex": vertex["id"], "generation": gen})):
        select(**args)
        r = gui.cad("design_zoom_to_selection")
        gui.check(f"select: with {what} selected design_zoom_to_selection applies", r.get("applies") is True, r)

    # Each is drawn selected, and differently.
    gui.show_design()
    shots = {}
    for name, args in (("face", {"body": 0, "face": top}), ("edge", {"body": 0, "edges": [edge]}),
                       ("vertex", {"body": 0, "vertex": vertex["id"]}), ("nothing", {})):
        select(**args)
        shots[name] = canvas_rows(out_dir, name)
    for name in ("face", "edge", "vertex"):
        gui.check(f"select: the canvas draws the {name} selected", shots[name] != shots["nothing"], name)
    gui.check("select: a face, an edge and a vertex are each drawn apart",
              len({shots["face"], shots["edge"], shots["vertex"]}) == 3, "two of the three screenshots are equal")

    # Planes: refused while no click could pick one, the sketch Sketch waits for, and else the plane picked.
    view(origin=False)
    why = refused("design_select", plane="XY")
    gui.check("select: a plane is refused while the planes are hidden and nothing waits for one", why != "", why)
    gui.cad("sketch_create")
    select(plane="XY")
    # The sketch's plane, read back as sketch_normal looks at it: along +Z, at the XY plane's origin.
    state = view(camera="sketch_normal").get("camera_state", {})
    origin = gui.cad("design_describe")["modeling_origin"]
    seen_from = [p - t for p, t in zip(state.get("position", []), state.get("look_at", []))]
    length = math.sqrt(sum(a * a for a in seen_from))
    gui.check("select: {plane: XY} opens the sketch Sketch waits for, on XY",
              length > 0 and seen_from[2] / length >= 0.999
              and all(near(a, b, 1e-3) for a, b in zip(state.get("look_at", []), origin)), [state, origin])
    gui.cad("sketch_cancel")
    view(origin=True)
    select(body=0)
    r = select(plane="XZ")
    gui.check("select: with the Origin row's planes up, {plane} picks it",
              r["plane"] == "XZ" and r["selection"]["id"] == "none" and selected()[1] == "XZ", [r, selected()])
    select()
    view(origin=bool(start.get("origin")))


def rung_select_loops():
    # Three squares in one sketch, its loop 0 extruded alone: the loops left keep the numbers sketch_describe and
    # extrude give them.
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    for x in (-30, 0, 30):
        gui.cad("sketch_draw", type="rectangle_2_point", p0=[x - 5, -5], p1=[x + 5, 5])
    sketch = gui.cad("sketch_finish")["sketch_index"]
    described = gui.cad("sketch_describe", sketch=sketch)
    ents, loops = described["entities"], described["closed_loops"]
    origin = gui.cad("design_describe")["modeling_origin"]

    def centre(loop):
        points = [ents[i][k] for i in loop["entities"] for k in ("p0", "p1")]
        return [origin[0] + sum(p[0] for p in points) / len(points), origin[1] + sum(p[1] for p in points) / len(points)]

    extruded = gui.cad("solid_extrude", sketch=sketch, loop=0, distance=5)["extrude_index"]
    name = gui.cad("design_describe")["features"][extruded]["name"]
    r = select(sketch=sketch, loop=1)
    gui.check("select_loops: {sketch, loop: 1} reads back sk_loop", r["selection"]["id"] == "sk_loop", r)
    # design_zoom_to_selection centres the view on what is selected: the loop sketch_describe calls 1.
    gui.cad("design_zoom_to_selection")
    look = gui.client.call("app_state").get("camera", {}).get("look_at", [])
    want = centre(loops[1])
    gui.check("select_loops: it selects the loop sketch_describe calls 1",
              len(look) == 3 and near(look[0], want[0], 1e-3) and near(look[1], want[1], 1e-3), [look, want])
    why = refused("design_select", sketch=sketch, loop=0)
    gui.check("select_loops: the extruded loop 0 is refused, naming the extrude that took it", name in why, why)
    why = refused("design_select", sketch=sketch, loop=len(loops))
    gui.check("select_loops: a loop past the sketch's is refused", f"has {len(loops)} closed loops" in why, why)


def generation():
    return gui.cad("design_describe")["generation"]


def fillet_shell_block():
    """A 40 x 20 x 10 block, its top edge along y at +x rounded 2 mm (the Fillet), then shelled 1 mm open at the
    bottom, which renumbers its faces and edges after the Fillet; returns the Fillet's index and its edge's id."""
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=40, height=20)
    along_y = gui.cad("body_describe", body=0, edge_kinds=["line"], direction=[0, 1, 0])
    edge = max(along_y["edges"], key=lambda e: e["p0"][0] + e["p0"][2])
    fillet = gui.cad("solid_fillet", edges=edge["id"], radius=2, body=0, generation=along_y["generation"])["fillet_index"]
    bottom = gui.cad("body_describe", body=0, at_extreme="-z")
    gui.cad("solid_shell", thickness=1, face=bottom["faces"][0]["id"], body=0, generation=bottom["generation"])
    return fillet, edge["id"]


def top_edge_before(fillet):
    """The block's top edge along x at +y, as body_describe {before} reads it in the model before the Fillet."""
    along_x = gui.cad("body_describe", body=0, before=fillet, edge_kinds=["line"], direction=[1, 0, 0])
    return along_x, max(along_x["edges"], key=lambda e: e["p0"][1] + e["p0"][2])


def on_top_edge(edge):
    """Where a click picks the edge top_edge_before() gives: its middle, 1 mm in on the top face (toward -y). Seen
    from above, the edge is the block's outline with no face past it, so a click on the line itself, rounded to a
    whole pixel, can land just outside the block and pick nothing; 1 mm in is well within the edge's pick
    tolerance."""
    m = mid(edge)
    return [m[0], m[1] - 1.0, m[2]]


def rung_feature_edit_refs():
    fillet, rounded = fillet_shell_block()
    along_x, edge = top_edge_before(fillet)
    gui.check("feature_edit_refs: body_describe {before} reads the box the Fillet was made on",
              along_x.get("before") == fillet and along_x.get("edge_count") == 12, along_x)
    gui.check("feature_edit_refs: its generation is the design's", along_x.get("generation") == generation(),
              [along_x.get("generation"), generation()])
    depth = undo_depth()
    r = outcome("feature_edit", feature=fillet, refs={"edges": [edge["id"]]}, generation=along_x["generation"])
    edges = gui.cad("feature_describe", feature=fillet)["refs"]["edges"]
    gui.check("feature_edit_refs: the Fillet rounds the edge named by its id before it",
              "ok" not in r and edges == [edge["id"]], [r, edges])
    gui.check("feature_edit_refs: one undo step", undo_depth() == depth + 1, f"{depth} -> {undo_depth()}")
    why = refused("feature_edit", feature=fillet, refs={"edges": [rounded]}, generation=along_x["generation"])
    gui.check("feature_edit_refs: ids read before the edit rebuilt the model are refused as stale", "stale_ids" in why, why)
    gui.client.call_command("undo", history="design")
    edges = gui.cad("feature_describe", feature=fillet)["refs"]["edges"]
    gui.check("feature_edit_refs: one undo puts the Fillet back on its edge", edges == [rounded], edges)


def rung_text_edit():
    gui.new_design()
    index = outcome("sketch_text", text="Orca", height=12).get("text_index")
    if not gui.check("text_edit: sketch_text adds a Text feature", isinstance(index, int), index):
        return
    depth = undo_depth()
    r = outcome("feature_edit", feature=index, fields={"text_string": "AB", "text_height": 20})
    params = gui.cad("feature_describe", feature=index)["params"]
    gui.check("text_edit: the text reads AB, 20 mm high, in two regions",
              "ok" not in r and params.get("text_string") == "AB" and near(params.get("text_height", 0), 20)
              and params.get("regions") == 2, [r, params])
    gui.check("text_edit: one undo step", undo_depth() == depth + 1, f"{depth} -> {undo_depth()}")


def calibrate_on_face(body, face, gen):
    """Fit the click map on the plane of a flat face of the model, through a sketch opened on it and dropped, in the
    view reset_view() sets; returns the plane's frame, to give a world point in its coordinates (on_frame())."""
    gui.show_design()
    frame = gui.cad("sketch_create", plane={"body": body, "face": face}, generation=gen)["frame"]
    gui.calibrate(aimed=True)
    gui.cancel()
    return frame


def on_frame(frame, p):
    d = [a - b for a, b in zip(p, frame["origin"])]
    return (sum(a * b for a, b in zip(d, frame["x_axis"])), sum(a * b for a, b in zip(d, frame["y_axis"])))


def settled():
    """Wait out the rebuild a card's Confirm runs behind its progress dialog, which refuses every Design command."""
    def answers():
        try:
            gui.cad("design_describe")
            return True
        except orca_mcp.McpError:
            return False
    if not until(answers, timeout=60.0):
        raise AssertionError("the Design tab is still rebuilding after a minute")


def edit_card_of(feature):
    """Open the card that edits `feature` from its Feature tree row, and keep the click map on the canvas, which
    leaving the sketch resized."""
    row_menu(gui.cad("design_describe")["features"][feature]["name"], "Edit")
    gui._follow_canvas()


def rung_edit_repick():
    # In a card that edits a feature, an edge picked again is read in the model the feature sees.
    fillet, _ = fillet_shell_block()
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    frame = calibrate_on_face(0, top["faces"][0]["id"], top["generation"])
    _, edge = top_edge_before(fillet)
    edit_card_of(fillet)
    gui.click_mm(*on_frame(frame, on_top_edge(edge)))
    gui.press("Confirm")
    settled()
    edges = gui.cad("feature_describe", feature=fillet)["refs"]["edges"]
    gui.check("edit_repick: the edge picked in the Fillet's card is its id in body_describe {before}",
              edges == [edge["id"]], f"edges {edges}, the box's top edge along x is {edge['id']}")


def rung_edit_repick_placed():
    # The same block placed 30 mm along y with body_place, in the plane of its top face, which leaves it clear of where
    # the model builds it: the edge clicked where the view draws it, in the Fillet's card, is the edge the Fillet names.
    fillet, _ = fillet_shell_block()
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    frame = calibrate_on_face(0, top["faces"][0]["id"], top["generation"])
    _, edge = top_edge_before(fillet)
    move = [0, 30, 0]
    gui.cad("body_place", body=0, move=move)
    edit_card_of(fillet)
    gui.click_mm(*on_frame(frame, [a + b for a, b in zip(on_top_edge(edge), move)]))
    gui.press("Confirm")
    settled()
    edges = gui.cad("feature_describe", feature=fillet)["refs"]["edges"]
    gui.check("edit_repick_placed: the edge picked where the placed block is drawn is its id in body_describe {before}",
              edges == [edge["id"]], f"edges {edges}, the box's top edge along x is {edge['id']}")


def rung_edit_repick_datum():
    # A Plane on the block's top face, then a Fillet of a second block, which renumbers that block's faces: a face
    # picked again in the Plane's card is the finished model's, where the engine reads it. The second block is wide
    # enough that a click in the middle of its top face is far from its edges, which a click near takes first.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=40, height=20)
    gui.cad("solid_extrude", distance=10, operation="new_body", profile=[[40, -15], [70, -15], [70, 15], [40, 15]])
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    plane = gui.cad("construct_plane", type="coincident", face_a={"body": 0, "face": top["faces"][0]["id"]},
                    generation=top["generation"])["plane_index"]
    upright = gui.cad("body_describe", body=1, edge_kinds=["line"], direction=[0, 0, 1])
    gui.cad("solid_fillet", edges=upright["edges"][0]["id"], radius=1, body=1, generation=upright["generation"])
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    frame = calibrate_on_face(0, top["faces"][0]["id"], top["generation"])
    other = gui.cad("body_describe", body=1, at_extreme="+z")["faces"][0]
    edit_card_of(plane)
    gui.press("Pick Face A")
    gui.click_mm(*on_frame(frame, other["centroid"]))
    gui.press("Confirm")
    settled()
    refs = gui.cad("feature_describe", feature=plane)["refs"]
    gui.check("edit_repick_datum: the face picked in the Plane's card is its id in the finished model",
              refs.get("plane_face_body") == 1 and refs.get("plane_face") == other["id"], [refs, other["id"]])


def rung_edit_repick_hole():
    # A Hole drilled into the block's +x side, then a Fillet of an upright edge, which renumbers the block's faces after
    # the Hole: the top face picked again in the Hole's card gives the hole its plane and bounds from the model before
    # the Hole, where the face's id is. The block is long enough that the middle of its top face is clear of the
    # hole's handles and their labels, which a click near takes first.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=120, height=60)
    side = gui.cad("body_describe", body=0, at_extreme="+x")
    hole = gui.cad("solid_hole", diameter=4, depth=3, x=5, plane={"body": 0, "face": side["faces"][0]["id"]},
                   generation=side["generation"])["hole_index"]
    upright = gui.cad("body_describe", body=0, edge_kinds=["line"], direction=[0, 0, 1])
    gui.cad("solid_fillet", edges=upright["edges"][0]["id"], radius=1, body=0, generation=upright["generation"])
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    frame = calibrate_on_face(0, top["faces"][0]["id"], top["generation"])
    before = gui.cad("body_describe", body=0, before=hole, at_extreme="+z")["faces"][0]
    edit_card_of(hole)
    gui.click_mm(*on_frame(frame, before["centroid"]))
    gui.keep_as_drawn()   # Confirm with a value field open takes the value, not the card
    gui.press("Confirm")
    settled()
    f = gui.cad("feature_describe", feature=hole)
    plane, params = f["plane"], f["params"]
    gui.check("edit_repick_hole: the Hole goes into the top face picked in its card, read in the model before it",
              all(near(a, b, 1e-6) for a, b in zip(plane["normal"], [0, 0, -1]))
              and all(near(a, b, 1e-6) for a, b in zip(plane["origin"], before["centroid"]))
              and f["refs"].get("target_body") == 0,
              [plane, f["refs"], before])
    gui.check("edit_repick_hole: at the face's centre", near(params.get("hole_x", 1), 0) and near(params.get("hole_y", 1), 0),
              params)


def selection():
    return gui.cad("design_describe")["selection"]["id"]


def rung_edit_repick_hidden():
    # Two blocks, the first one's top edge along y at +x filleted and the block then shelled, which renumbers its edges
    # after the Fillet. The second block, hidden, is not picked in the Fillet's card, and showing and hiding it with the
    # card open keeps the card's picks in the model before the Fillet. A click that picks nothing drops the card's pick,
    # so an edge of the first block is picked before the click on the hidden one, which then leaves nothing selected.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=40, height=20)
    gui.cad("solid_extrude", distance=10, operation="new_body", profile=[[40, -15], [70, -15], [70, 15], [40, 15]])
    along_y = gui.cad("body_describe", body=0, edge_kinds=["line"], direction=[0, 1, 0])
    edge = max(along_y["edges"], key=lambda e: e["p0"][0] + e["p0"][2])
    fillet = gui.cad("solid_fillet", edges=edge["id"], radius=2, body=0, generation=along_y["generation"])["fillet_index"]
    bottom = gui.cad("body_describe", body=0, at_extreme="-z")
    gui.cad("solid_shell", thickness=1, face=bottom["faces"][0]["id"], body=0, generation=bottom["generation"])
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    frame = calibrate_on_face(0, top["faces"][0]["id"], top["generation"])
    other = gui.cad("body_describe", body=1, at_extreme="+z")["faces"][0]
    _, edge = top_edge_before(fillet)
    # Named apart from the feature that made it, whose row has an eye of its own.
    gui.cad("body_set", body=1, name="Block B", visible=False)
    edit_card_of(fillet)
    gui.click_mm(*on_frame(frame, on_top_edge(edge)))
    gui.click_mm(*on_frame(frame, other["centroid"]))
    picked = selection()
    gui.check("edit_repick_hidden: a click on the hidden block picks nothing in the Fillet's card", picked == "none", picked)
    # The Bodies row's eye, as a person shows and hides a body with the card open.
    gui.client.call("widget_click", target={"row": "Body 2 — Block B", "column": "Show"})
    gui.client.call("widget_click", target={"row": "Body 2 — Block B", "column": "Hide"})
    gui._follow_canvas()
    gui.click_mm(*on_frame(frame, on_top_edge(edge)))
    gui.click_mm(*on_frame(frame, other["centroid"]))
    picked = selection()
    gui.check("edit_repick_hidden: nor once it was shown and hidden again", picked == "none", picked)
    gui.click_mm(*on_frame(frame, on_top_edge(edge)))
    gui.press("Confirm")
    settled()
    edges = gui.cad("feature_describe", feature=fillet)["refs"]["edges"]
    gui.check("edit_repick_hidden: an edge picked then is its id in body_describe {before}",
              edges == [edge["id"]], f"edges {edges}, the box's top edge along x is {edge['id']}")


# ---------------------------------------------------------------- what is open: design_confirm_tool and design_cancel_tool

def card_open():
    """Whether a feature card is open, as a command that changes the document is refused under one."""
    return "a feature card is open" in refused("design_select")


def status_says(words):
    """Whether the Design tab's status line says `words`."""
    return any(w.kind == "text" and words in w.label for w in gui.widgets())


def extrude_card(rung):
    """A 20 x 10 rectangle committed on XY, its loop selected and the Extrude card opened on it with solid_extrude {};
    returns the feature count with the card open."""
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", type="rectangle_2_point", p0=[0, 0], p1=[20, 10])
    sketch = gui.cad("sketch_finish")["sketch_index"]
    select(sketch=sketch, loop=0)
    r = gui.cad("solid_extrude")
    gui.check(f"{rung}: solid_extrude {{}} opens the Extrude card", "a feature card is open" in (r.get("busy") or ""), r)
    return features()


def rung_confirm_card():
    count = extrude_card("confirm_card")
    r = gui.cad("design_confirm_tool")
    gui.check("confirm_card: design_confirm_tool confirms the card and gives the new feature's index",
              r.get("was") == "card" and "ok" not in r and r.get("feature") == count and r.get("open") is None, r)
    added = gui.cad("design_describe")["features"][count:]
    gui.check("confirm_card: the feature is the Extrude", [f["type"] for f in added] == ["Extrude"], added)
    gui.check("confirm_card: one body exists", bodies() == 1, bodies())


def rung_cancel_card():
    count = extrude_card("cancel_card")
    r = gui.cad("design_cancel_tool")
    gui.check("cancel_card: design_cancel_tool cancels the card", r.get("was") == "card", r)
    gui.check("cancel_card: no feature was added", features() == count and bodies() == 0, [features(), bodies()])
    why = refused("design_cancel_tool")
    gui.check("cancel_card: nothing is open after it", "nothing is open in the Design tab" in why, why)


def rung_cancel_sketch():
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", entities=[{"type": "line", "p0": [0, 0], "p1": [10, 0]}])
    r = gui.cad("design_cancel_tool")
    gui.check("cancel_sketch: design_cancel_tool discards the sketch", r.get("was") == "sketch", r)
    nothing_open("cancel_sketch")
    gui.check("cancel_sketch: no feature was added", features() == 0, features())


def rung_cancel_plane_pick():
    gui.new_design()
    start = view().get("design", {})
    view(origin=False)
    r = gui.cad("sketch_create")
    gui.check("cancel_plane_pick: sketch_create {} with nothing selected waits for a plane", r.get("interactive") is True, r)
    why = refused("design_confirm_tool")
    gui.check("cancel_plane_pick: design_confirm_tool is refused while a plane is picked",
              "a plane is being picked: select one with design_select, or cancel" in why, why)
    r = gui.cad("design_cancel_tool")
    gui.check("cancel_plane_pick: design_cancel_tool ends the plane pick", r.get("was") == "plane_pick", r)
    why = refused("design_select", plane="XY")
    gui.check("cancel_plane_pick: a plane is then refused, as no click can pick one", "no click can pick a plane" in why, why)
    nothing_open("cancel_plane_pick")
    view(origin=bool(start.get("origin")))


def text_dialog(rung):
    """The Text dialog, opened by sketch_text {} in a sketch on XY that holds nothing; False when it did not open."""
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_text")
    return gui.check(f"{rung}: sketch_text {{}} opens the Text dialog", "Text" in gui.modeless(), gui.modeless())


def rung_text_dialog():
    # The Text dialog is open: design_cancel_tool closes it as its Cancel does, and design_confirm_tool takes it as its OK does,
    # which is off until there is text to insert.
    if not text_dialog("text_dialog"):
        return
    r = gui.cad("design_cancel_tool")
    gui.check("text_dialog: design_cancel_tool cancels the Text dialog", r.get("was") == "text" and "Text" not in gui.modeless(),
              [r, gui.modeless()])
    gui.check("text_dialog: it adds nothing", features() == 0, features())
    if not text_dialog("text_dialog"):
        return
    why = refused("design_confirm_tool")
    gui.check("text_dialog: design_confirm_tool is refused while the dialog's OK is off, saying why",
              "Type the text to see its size" in why and "Text" in gui.modeless(), [why, gui.modeless()])
    gui.fill({"label": "Text", "class": "TextInput", "within": "Text"}, "Orca", submit=False)
    r = gui.cad("design_confirm_tool")
    gui.check("text_dialog: design_confirm_tool takes the Text dialog's OK, which opens the text's placement",
              r.get("was") == "text" and "ok" not in r and r.get("feature") == 0 and r.get("open") == "insert"
              and "Text" not in gui.modeless(), r)
    r = gui.cad("design_confirm_tool")
    gui.check("text_dialog: design_confirm_tool then keeps the text where it is placed", r.get("was") == "insert" and "ok" not in r, r)
    added = [f["name"] for f in gui.cad("design_describe")["features"]]
    gui.check("text_dialog: the design holds the text", len(added) == 1 and added[0].startswith("Text"), added)
    nothing_open("text_dialog")


def rung_confirm_nothing():
    gui.new_design()
    for tool in ("design_confirm_tool", "design_cancel_tool"):
        why = refused(tool)
        gui.check(f"confirm_nothing: {tool} is refused with nothing open", "nothing is open in the Design tab" in why, why)


def top_edge_along_x(body):
    """The top edge along x at +y of box `body`, and the generation its id was read at."""
    along_x = gui.cad("body_describe", body=body, edge_kinds=["line"], direction=[1, 0, 0])
    return max(along_x["edges"], key=lambda e: e["p0"][1] + e["p0"][2])["id"], along_x["generation"]


def rung_confirm_disabled():
    # A 2 mm fillet, the card's default, of the top edge of a plate 0.5 mm thick has no preview, so Confirm is off.
    gui.new_design()
    gui.cad("solid_extrude", distance=0.5, width=20, height=20)
    edge, gen = top_edge_along_x(0)
    select(body=0, edges=[edge], generation=gen)
    r = gui.cad("solid_fillet")
    if not gui.check("confirm_disabled: solid_fillet {} opens the Fillet card", "a feature card is open" in (r.get("busy") or ""), r):
        return
    depth = undo_depth()
    why = refused("design_confirm_tool")
    gui.check("confirm_disabled: design_confirm_tool is refused with the status line", "Invalid" in why, why)
    gui.check("confirm_disabled: the card stays open, and nothing was added",
              card_open() and features() == 2 and undo_depth() == depth, [card_open(), features(), undo_depth()])
    gui.cancel()


COMMIT = "Commit to Plate — send the solid to Prepare"   # the ribbon's Commit to Plate button, in assembly mode


def plate_objects():
    """How many objects the Prepare plate holds, on its plates and off them."""
    listed = gui.client.call("object_list")
    return sum(len(p["objects"]) for p in listed.get("plates", [])) + len(listed.get("outside", []))


def commit_stops(rung):
    """Press Commit to Plate over a card whose Confirm does not take, and check that it stops there: nothing sent to
    the plate, the Design tab still in front."""
    sent, tab = plate_objects(), gui.client.call("app_state").get("tab")
    gui.press(COMMIT)
    gui.check(f"{rung}: Commit to Plate sends nothing to the plate", plate_objects() == sent, [sent, plate_objects()])
    now = gui.client.call("app_state").get("tab")
    gui.check(f"{rung}: the Design tab stays in front", now == tab, [tab, now])


def rung_confirm_refused_add():
    # An add the card refuses takes no undo step and leaves the card open, and Commit to Plate over it stops there. The
    # Delete Face card opens holding no face until one is added, so its add refuses.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    select(body=0, face=top["faces"][0]["id"], generation=top["generation"])
    r = gui.cad("solid_delete_face")
    if not gui.check("confirm_refused_add: solid_delete_face {} opens the Delete Face card", "a feature card is open" in (r.get("busy") or ""), r):
        return
    count, depth = features(), undo_depth()
    commit_stops("confirm_refused_add")
    gui.check("confirm_refused_add: Commit to Plate over the refused add takes no undo step and adds nothing",
              undo_depth() == depth and features() == count, [depth, undo_depth(), count, features()])
    kept = card_open()
    gui.check("confirm_refused_add: the Delete Face card stays open", kept, kept)
    gui.show_design()
    if kept:
        gui.cancel()

    # The Insert placement Scale artwork opens is confirmed by its own Confirm, not by a card's: Commit to Plate takes
    # no undo step over it, and it stays open.
    gui.new_design()
    art = art_at([0, 0])
    if art is None:
        return
    row_menu(gui.cad("design_describe")["features"][art]["name"], "Scale artwork")
    depth = undo_depth()
    gui.press(COMMIT)
    gui.check("confirm_refused_add: Commit to Plate over the Insert placement takes no undo step", undo_depth() == depth,
              [depth, undo_depth()])
    gui.check("confirm_refused_add: the Insert placement stays open", card_open(), card_open())
    gui.show_design()
    gui.keys("Escape", target=CANVAS)
    nothing_open("confirm_refused_add")

    # The Thicken card opened on a face, whose preview passed, keeps its Confirm on when a click on a committed
    # sketch's loop lets go of the face: its add refuses, with no face to thicken. The sketch is a square on the plane
    # of the box's top face, beside the box, where a click picks it and nothing else.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    gui.cad("sketch_create", plane={"body": 0, "face": top["faces"][0]["id"]}, generation=top["generation"])
    gui.cad("sketch_draw", type="rectangle_2_point", p0=[25, -5], p1=[35, 5])
    gui.cad("sketch_finish")
    top = gui.cad("body_describe", body=0, at_extreme="+z")
    calibrate_on_face(0, top["faces"][0]["id"], top["generation"])
    select(body=0, face=top["faces"][0]["id"], generation=top["generation"])
    r = gui.cad("solid_thicken")
    if not gui.check("confirm_refused_add: solid_thicken {} opens the Thicken card on the face", "a feature card is open" in (r.get("busy") or ""), r):
        return
    gui._follow_canvas()
    gui.click_mm(30, 0)
    gui.check("confirm_refused_add: a click on the sketch's loop lets go of the face", selection() == "sk_loop", selection())
    count, depth = features(), undo_depth()
    gui.confirm()
    gui.check("confirm_refused_add: the card's Confirm, refused by its add, takes no undo step and adds nothing",
              undo_depth() == depth and features() == count, [depth, undo_depth(), count, features()])
    kept = card_open()
    gui.check("confirm_refused_add: the Thicken card stays open, saying why",
              kept and status_says("Pick a solid face to thicken first"), kept)
    if kept:
        gui.cancel()


def redo_depth():
    return gui.cad("design_describe")["redo_depth"]


def rung_confirm_failed_add():
    # The Fillet card of confirm_disabled, a 2 mm fillet of the top edge of a plate 0.5 mm thick: Commit to Plate
    # confirms it, its add runs and its rebuild fails. The fillet is taken back with no undo step, the card stays open
    # with the reason, and Commit to Plate stops there.
    gui.new_design()
    gui.cad("solid_extrude", distance=0.5, width=20, height=20)
    edge, gen = top_edge_along_x(0)
    select(body=0, edges=[edge], generation=gen)
    r = gui.cad("solid_fillet")
    if not gui.check("confirm_failed_add: solid_fillet {} opens the Fillet card", "a feature card is open" in (r.get("busy") or ""), r):
        return
    count, depth, redo = features(), undo_depth(), redo_depth()
    commit_stops("confirm_failed_add")
    gui.check("confirm_failed_add: a Confirm whose rebuild fails keeps no feature and no undo step",
              features() == count and undo_depth() == depth and redo_depth() == redo,
              [count, features(), depth, undo_depth(), redo, redo_depth()])
    gui.check("confirm_failed_add: the card stays open, saying why", card_open() and status_says("could not be rebuilt"),
              [card_open(), [w.label for w in gui.widgets() if w.kind == "text"][:8]])
    gui.check("confirm_failed_add: the card keeps its pick, the edge", selection() == "edge_str", selection())
    gui.check("confirm_failed_add: the design rebuilds as it was", "error" not in gui.cad("design_describe") and bodies() == 1,
              gui.cad("design_describe").get("error"))
    gui.show_design()
    if card_open():
        gui.cancel()


# ---------------------------------------------------------------- where a body stands: body_place

def apply(placement, p):
    """The world point `placement` (design_describe's, three rows of four numbers) puts the model's point `p` at."""
    return [sum(row[k] * p[k] for k in range(3)) + row[3] for row in placement]


def translation(body):
    """Body `body`'s placement's translation, or None where the model places it."""
    placement = gui.cad("design_describe")["bodies"][body].get("placement")
    return [row[3] for row in placement] if placement else None


def placed_box(body):
    """Body `body`'s bounding box where it stands: its model bbox's corners through its placement, as (min, max)."""
    b = gui.cad("design_describe")["bodies"][body]
    lo, hi = b["bbox"]["min"], b["bbox"]["max"]
    corners = [[(lo, hi)[(i >> k) & 1][k] for k in range(3)] for i in range(8)]
    if b.get("placement"):
        corners = [apply(b["placement"], c) for c in corners]
    return [min(c[k] for c in corners) for k in range(3)], [max(c[k] for c in corners) for k in range(3)]


def plate_offset(replace):
    """Commit the design to the plate as an assembly, replacing object `replace` (None: none), and return how far along
    x Design Body 1's part sits from Design Body 2's, and the object's index. The plate places an object where it has
    room, so a part's place is read against another part of the same object."""
    r = gui.cad("design_commit_to_plate", mode="assembly", replace=[] if replace is None else [replace])
    index = r["objects"][0]["index"]
    listed = gui.client.call("object_list", detail=True)
    objects = [o for p in listed.get("plates", []) for o in p["objects"]] + listed.get("outside", [])
    parts = {p["name"]: p for o in objects if o["index"] == index for p in o["parts"]}
    return parts["Design Body 1"]["bbox"]["min"][0] - parts["Design Body 2"]["bbox"]["min"][0], index


def rung_place():
    # A 30 x 20 x 10 box, body 0, and a 10 mm block beside it, body 1, which the plate measures the box against.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=30, height=20)
    gui.cad("solid_extrude", distance=10, operation="new_body", profile=[[60, -5], [70, -5], [70, 5], [60, 5]])
    design = gui.cad("design_describe")
    gui.check("place: design_describe reads the Commit button's mode", design.get("commit_mode") in ("assembly", "bodies"),
              design.get("commit_mode"))
    box = design["bodies"][0]
    gui.check("place: a body where the model has it has no placement", "placement" in box and box["placement"] is None
              and box["moved"] is False, box)
    unmoved, committed = plate_offset(None)

    r = gui.cad("body_place", body=0, move=[10, 0, 0])
    t = translation(0)
    gui.check("place: move [10, 0, 0] gives a placement translated by (10, 0, 0)",
              "ok" not in r and r.get("moved") is True and r.get("placement") is not None and t is not None
              and all(near(a, b) for a, b in zip(t, [10, 0, 0])), [r, t])
    gui.check("place: design_describe reads it moved", gui.cad("design_describe")["bodies"][0]["moved"] is True,
              gui.cad("design_describe")["bodies"][0])
    moved, _ = plate_offset(committed)
    gui.check("place: Commit to Plate sends the box 10 further along x", near(moved - unmoved, 10, 1e-3), [unmoved, moved])
    gui.show_design()

    lo, hi = placed_box(0)
    centre = [(a + b) / 2 for a, b in zip(lo, hi)]
    gui.cad("body_place", body=0, rotate={"axis": "z", "angle": 90})
    lo, hi = placed_box(0)
    turned = [(a + b) / 2 for a, b in zip(lo, hi)]
    gui.check("place: rotate 90 degrees about z turns it about its centre: 20 along x, 30 along y, the centre kept",
              near(hi[0] - lo[0], 20) and near(hi[1] - lo[1], 30) and all(near(a, b) for a, b in zip(turned, centre)),
              [lo, hi, centre])

    side = gui.cad("body_describe", body=0, at_extreme="-y")
    gui.cad("body_place", body=0, face=side["faces"][0]["id"], generation=side["generation"])
    lo, hi = placed_box(0)
    gui.check("place: face lays the side face whose normal is -y on the bed: lowest z 0, 20 high",
              near(lo[2], 0, 1e-4) and near(hi[2] - lo[2], 20, 1e-4), [lo, hi])

    r = gui.cad("body_place", body=0, reset=True)
    box = gui.cad("design_describe")["bodies"][0]
    gui.check("place: reset puts it back where the model has it", r.get("placement") is None and r.get("moved") is False
              and box["placement"] is None and box["moved"] is False, [r, box])

    gui.cad("sketch_create", plane="XY")
    why = refused("body_place", body=0, move=[1, 0, 0])
    gui.check("place: with a sketch open it is refused as busy", "design_busy" in why and "a sketch is open" in why, why)
    gui.cad("sketch_cancel")


def rung_place_select():
    # The box moved 50 mm along x and turned a quarter turn about z, clear of where the model has it.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=30, height=20)
    model_lo, model_hi = placed_box(0)
    gui.cad("body_place", body=0, move=[50, 0, 0], rotate={"axis": "z", "angle": 90})
    placement = gui.cad("design_describe")["bodies"][0]["placement"]
    listed = gui.cad("body_describe", body=0, vertices=True)
    vertex = max(listed["vertices"], key=lambda v: sum(v["p"]))
    r = select(body=0, vertex=vertex["id"], generation=listed["generation"])
    point, placed = r.get("point") or [], r.get("placed_point") or []
    gui.check("place_select: a vertex's point is body_describe's and its placed_point the placement's",
              len(point) == 3 and all(near(a, b) for a, b in zip(point, vertex["p"])) and len(placed) == 3
              and all(near(a, b) for a, b in zip(placed, apply(placement, point))), [r, vertex, placement])

    top = gui.cad("body_describe", body=0, at_extreme="+z")
    select(body=0, face=top["faces"][0]["id"], generation=top["generation"])
    lo, hi = placed_box(0)
    gui.check("place_select: the placed body is clear of where the model has it",
              hi[0] < model_lo[0] or lo[0] > model_hi[0], [lo, hi, model_lo, model_hi])
    look = view(camera="fit_selection").get("camera_state", {}).get("look_at", [])
    gui.check("place_select: fit_selection centres inside the placed body",
              len(look) == 3 and all(lo[k] - 1e-3 <= look[k] <= hi[k] + 1e-3 for k in range(3)), [look, lo, hi])
    select()


def rung_place_follows():
    # Two boxes as two bodies, each placed. Deleting the feature that made body 0 renumbers the other box as
    # body 0, and its placement goes with it; a box added afterwards stands where the model has it.
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=20, height=20)
    gui.cad("solid_extrude", distance=10, operation="new_body", profile=[[40, -10], [60, -10], [60, 10], [40, 10]])
    gui.cad("body_place", body=0, move=[0, 30, 0])
    gui.cad("body_place", body=1, move=[50, 0, 0])
    maker = gui.cad("design_describe")["bodies"][0]["source_feature"]
    gui.cad("feature_delete", feature=maker)
    count, moved = bodies(), translation(0)
    gui.check("place_follows: the box left has the placement it had as body 1",
              count == 1 and moved is not None and all(near(a, b) for a, b in zip(moved, [50, 0, 0])), [count, moved])
    gui.cad("solid_extrude", distance=10, operation="new_body", profile=[[-60, -10], [-40, -10], [-40, 10], [-60, 10]])
    gui.check("place_follows: a box added afterwards has none", bodies() == 2 and translation(1) is None
              and translation(0) is not None, [bodies(), translation(0), translation(1)])
    # Undo takes the new box away, and then the delete: the first box comes back where it stood.
    gui.client.call_command("undo", history="design")
    gui.client.call_command("undo", history="design")
    back = [translation(b) for b in range(bodies())]
    gui.check("place_follows: an undo of the delete brings the first box back where it stood",
              len(back) == 2 and None not in back and all(near(a, b) for a, b in zip(back[0], [0, 30, 0]))
              and all(near(a, b) for a, b in zip(back[1], [50, 0, 0])), back)

def prepare_objects():
    """[(index, id, name)] of every Prepare object, in index order, as object_list reads them."""
    listed = gui.client.call("object_list")
    objects = [o for p in listed.get("plates", []) for o in p["objects"]] + listed.get("outside", [])
    return sorted({(o["index"], o["id"], o["name"]) for o in objects})


def selected_objects():
    """The Prepare objects app_state reads as selected, on the canvas shown: the assemble view's while it is shown."""
    return gui.client.call("app_state").get("selection", {}).get("objects")


def listed_selection():
    """The Objects list's selected rows, as widget_list notes them ("object 1", "plate 0"), read without changing the
    view. The list shows only with the sidebar's Process switch on Objects, which the sweep's cleanup turns back."""
    gui.client.call("widget_set_value", target={"class": "SwitchButton", "label": "Process"}, value=True)
    quiet = {"include_web": False, "include_imgui": False, "include_menus": False}
    _, texts = gui.client.call_with_text("widget_list", query="ObjectList", **quiet)
    found = re.search(r"\[(w\d+)\] list \(ObjectList\)", "\n".join(texts))
    if found is None:
        return None
    _, texts = gui.client.call_with_text("widget_list", target=found.group(1), max_nodes=500, **quiet)
    return re.findall(r"\[([^\[\]]*), selected\]", "\n".join(texts))


def assemble_view_shown():
    """Whether Prepare shows its assemble view, whose Return button's window widget_list lists only then."""
    return gui.client.call("widget_list", query="Assembly Return", include_web=False, include_menus=False)["nodes"] > 0


def refusal(call):
    """(code, text) of the refusal `call()` gets; (None, None) when it runs."""
    try:
        call()
    except orca_mcp.McpError as e:
        text, details = checks.refusal(e)
        return details.get("code"), text
    return None, None


def rung_replace():
    # design_commit_to_plate's replace deletes as Prepare's Delete does: a part of a cut is refused before anything
    # changes, naming it, a replacing commit is one Prepare undo step, and it leaves the selection a plain commit
    # leaves, on the canvas and in the Objects list, in the Prepare view it was called in. The new project shows
    # Prepare, and New Design is on the Design tab's ribbon.
    gui.client.call("project_new", discard_changes=True)
    gui.show_design()
    gui.new_design()
    gui.cad("solid_extrude", distance=10, width=30, height=20)
    gui.cad("solid_extrude", distance=10, operation="new_body", profile=[[60, -5], [70, -5], [70, 5], [60, 5]])
    names = ["Design Body 1", "Design Body 2"]

    r = gui.cad("design_commit_to_plate", mode="bodies")
    gui.check("replace: a plain bodies commit sends Design Body 1 at 0 and Design Body 2 at 1, and replaces nothing",
              [(o["index"], o["name"]) for o in r["objects"]] == [(0, names[0]), (1, names[1])] and r["replaced"] == [],
              r)
    old = prepare_objects()
    gui.check("replace: object_list reads those two", [(i, n) for i, _, n in old] == [(0, names[0]), (1, names[1])],
              old)
    gui.check("replace: a plain bodies commit leaves the last body's object selected, on the canvas and in the list",
              selected_objects() == [1] and listed_selection() == ["object 1"], [selected_objects(), listed_selection()])

    # A cut keeps its cut information only with a connector, as the Cut gizmo makes one: the pieces of a plain plane cut
    # are no parts of a cut.
    gui.client.call("view_set", tab="prepare", camera="top")
    gui.client.call("object_select", objects=[0])
    gui.client.call("widget_click", target={"gizmo": "Cut"})
    gui.client.call("widget_set_value", target={"imgui_name": "move_Z"}, value=5)
    gui.client.call("widget_click", target={"imgui": "Add connectors"})
    gui.client.call("mouse_gesture", at={"object": 0, "spot": "top"}, check="point")
    gui.client.call("widget_click", target={"imgui": "Confirm connectors"})
    gui.client.call("widget_click", target={"imgui": "Perform cut"})
    checks.wait(gui.client, "idle")
    cut = prepare_objects()
    gui.check("replace: the cut leaves Design Body 2 at 0 and two pieces", len(cut) == 3 and cut[0][2] == names[1], cut)
    code, text = refusal(lambda: gui.cad("design_commit_to_plate", mode="bodies", replace=[0, 1]))
    gui.check("replace: Design Body 2 and a part of a cut are refused with cut_object, which names the part",
              code == "cut_object" and f"Object 1 ({cut[1][2]})" in (text or "") and "Object 0" not in (text or ""),
              [code, text])
    gui.check("replace: the refused call leaves the objects as they were", prepare_objects() == cut, prepare_objects())
    gui.client.call_command("undo", history="project")
    back = prepare_objects()
    gui.check("replace: the next undo takes back the cut, so the refused call added no undo step", back == old,
              [back, old])

    old_ids = [i for _, i, _ in old]
    r = gui.cad("design_commit_to_plate", mode="bodies", replace_ids=old_ids)
    made = sorted((o["index"], o["object_id"], o["name"]) for o in r["objects"])
    gui.check("replace: replace_ids replaces both, at 0 and 1",
              [(o["index"], o["name"]) for o in r["replaced"]] == [(0, names[0]), (1, names[1])], r["replaced"])
    gui.check("replace: the new objects are at 0 and 1", [(i, n) for i, _, n in made] == [(0, names[0]), (1, names[1])],
              made)
    now = prepare_objects()
    gui.check("replace: object_list reads exactly the new two, none of them an old one",
              now == made and not set(old_ids) & {i for _, i, _ in now}, [now, made, old_ids])
    gui.check("replace: a replacing bodies commit leaves the last body's object selected, on the canvas and in the "
              "list, as a plain one does", selected_objects() == [1] and listed_selection() == ["object 1"],
              [selected_objects(), listed_selection()])
    gui.client.call_command("undo", history="project")
    back = prepare_objects()
    gui.check("replace: one undo takes back the commit and the deletion", back == old, [back, old])

    r = gui.cad("design_commit_to_plate", mode="assembly", replace=[0])
    gui.check("replace: an assembly commit replacing one object sends Design Assembly, 2 parts, at 1",
              [(o["index"], o["name"], o["parts"]) for o in r["objects"]] == [(1, "Design Assembly", 2)]
              and r["replaced"] == [{"index": 0, "name": names[0]}], r)
    now = prepare_objects()
    gui.check("replace: object_list reads Design Body 2 at 0 and Design Assembly at 1",
              [(i, n) for i, _, n in now] == [(0, names[1]), (1, "Design Assembly")], now)
    gui.check("replace: the new object is selected, on the canvas and in the list, after a one-object delete",
              selected_objects() == [1] and listed_selection() == ["object 1"],
              [selected_objects(), listed_selection()])
    gui.client.call_command("undo", history="project")

    # On the assemble view, called there: a commit replacing one object, then a plain one (the assemble view refuses
    # an undo, so the plain one comes second). The view's scene is not reloaded with new objects, so a plain commit
    # selects none of them there and leaves the plate's row.
    gui.client.call("view_set", canvas="assemble")
    r = gui.cad("design_commit_to_plate", mode="assembly", replace=[0])
    gui.check("replace: on the assemble view, an assembly commit replacing one object sends Design Assembly at 1",
              [(o["index"], o["name"]) for o in r["objects"]] == [(1, "Design Assembly")], r)
    replaced = [assemble_view_shown(), selected_objects(), listed_selection()]
    gui.check("replace: the replacing commit leaves the assemble view shown", replaced[0], replaced)
    r = gui.cad("design_commit_to_plate", mode="assembly")
    plain = [assemble_view_shown(), selected_objects(), listed_selection()]
    gui.check("replace: a plain commit there leaves it shown too, with nothing selected and the plate's row",
              plain == [True, [], ["plate 0"]], plain)
    gui.check("replace: the replacing commit leaves the selection the plain one leaves", replaced == plain,
              [replaced, plain])
    gui.client.call("view_set", canvas="3d")
    gui.show_design()


def rung_shapes_match_tools():
    gui.new_design()
    gui.enter_sketch()
    x0, x1, y0, y1 = gui.safe
    free = ((x0 + x1) / 2, y1 - (y1 - y0) * 0.10)   # stays empty, to right-click on
    gui.cancel()
    for shape in shapes(gui.safe):
        name, setup = shape["type"], shape["setup"]

        # With the tool: its item chosen from the offer on empty space, its points clicked.
        gui.enter_sketch()
        if setup:
            gui.cad("sketch_draw", entities=setup)
        path = menu_path(shape["verb"])
        reply = gui.choose(*free, path)
        if reply.get("ran") != path:
            raise AssertionError(f"choosing {path} ran {reply.get('ran')}: {reply}")
        if shape["verb"] in ARMS:
            armed = gui.sketch()["tool"]
            gui.check(f"shapes_match_tools: {path} arms the {ARMS[shape['verb']]} tool", armed == ARMS[shape["verb"]],
                      armed)
        for p in shape["clicks"]:
            gui.click_mm(*p)
            if shape.get("each_click"):
                gui.keep_as_drawn()
        gui.keep_as_drawn()
        by_tool = drawn()
        added = by_tool["entities"][len(setup):]
        gui.check(f"shapes_match_tools: the {name} tool drew {shape['adds']} entities", len(added) == shape["adds"],
                  [e["type"] for e in added])
        gui.keys("Ctrl+Z", target=CANVAS)
        by_tool_undone = drawn()
        left = len(by_tool_undone["entities"]) - len(setup)
        gui.check(f"shapes_match_tools: one undo leaves {shape['left']} of the {name}'s entities", left == shape["left"], left)
        gui.cancel()

        # With sketch_draw, from what the tool drew.
        args = shape["args"](added)
        gui.enter_sketch()
        if setup:
            gui.cad("sketch_draw", entities=setup)
        gui.cad("sketch_draw", type=name, **args)
        by_command = drawn()
        gui.keys("Ctrl+Z", target=CANVAS)
        by_command_undone = drawn()
        gui.cancel()

        off = disagreement(by_tool, by_command)
        gui.check(f"shapes_match_tools: {name} by sketch_draw {args} is the tool's", not off,
                  f"{off}: tool {by_tool}, sketch_draw {by_command}")
        off = disagreement(by_tool_undone, by_command_undone)
        gui.check(f"shapes_match_tools: {name} and the tool's still agree after one undo", not off,
                  f"{off}: tool {by_tool_undone}, sketch_draw {by_command_undone}")


def two_bodies():
    """A 20 x 20 x 10 mm block and a 6 x 6 x 20 mm post through it, as two bodies, with nothing selected."""
    gui.new_design()
    gui.cad("solid_extrude", width=20, height=20, distance=10)
    gui.cad("solid_extrude", width=6, height=6, distance=20, operation="new_body")
    gui.cad("design_select")


def boolean_card(rung, args, operation):
    """Calls boolean with `args`, which open the Boolean card, and confirms the card: its Boolean has `operation`."""
    r = gui.cad("solid_boolean", **args)
    gui.check(f"{rung}: solid_boolean {args} with nothing selected replies that a tool is open",
              r.get("interactive") is True and r.get("tool") == "solid_boolean", r)
    gui.check(f"{rung}: the Boolean card is open", card_open() and "a feature card is open" in (r.get("busy") or ""), r)
    done = gui.cad("design_confirm_tool")
    gui.check(f"{rung}: the card's Confirm adds the Boolean", done.get("was") == "card" and "ok" not in done, done)
    gui.check(f"{rung}: the card's Boolean is a {operation}", operation_of(done.get("feature")) == operation,
              operation_of(done.get("feature")))
    gui.check(f"{rung}: one body is left", bodies() == 1, bodies())


def rung_boolean_opens():
    # The Boolean items take two bodies, which the tab never reports as what is selected, so a call opens the card as
    # the toolbar button does, whatever is selected.
    two_bodies()
    boolean_card("boolean_opens", {}, "join")
    two_bodies()
    boolean_card("boolean_opens", {"operation": "cut"}, "cut")
    two_bodies()
    r = gui.cad("solid_boolean", operation="cut", target=0, tool=1)
    gui.check("boolean_opens: solid_boolean {operation: cut, target, tool} runs", "ok" not in r and "interactive" not in r, r)
    gui.check("boolean_opens: it cut the post from the block", near(volume(0), 20 * 20 * 10 - 6 * 6 * 10, 1e-3), volume(0))


def line_selected():
    """An open sketch on XY holding one line, selected."""
    gui.new_design()
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", type="line", p0=[0, 0], p1=[10, 0])
    gui.cad("sketch_select", entities=[0])


def rung_action_pick():
    # A pick from the Speed Dial runs the action picked: a folded item's action with the item's arguments, a merged
    # item's as its command's own action.
    line_selected()
    checks.dial_pick(gui.client, "Polar array", canvas=CANVAS)
    gui.check("action_pick: the Polar array action arms the polar array tool", gui.sketch()["tool"] == "array_polar",
              gui.sketch()["tool"])
    gui.keys("Escape", target=CANVAS)
    checks.dial_pick(gui.client, "Trim", canvas=CANVAS)
    gui.check("action_pick: the Trim action arms the trim tool", gui.sketch()["tool"] == "trim", gui.sketch()["tool"])
    gui.cad("sketch_cancel")


def rung_action_pick_replay():
    # action_run of a folded item's action is journaled with the item's arguments, so the exported session replays the
    # pick.
    gui.new_design()
    gui.client.call("replay_export", clear=True)
    gui.cad("sketch_create", plane="XY")
    gui.cad("sketch_draw", type="line", p0=[0, 0], p1=[10, 0])
    gui.cad("sketch_select", entities=[0])
    r = gui.client.call("action_run", id="sk_array_polar")
    gui.check("action_pick_replay: action_run of the Polar array action opens its tool",
              r.get("interactive") is True, r)
    gui.check("action_pick_replay: it arms the polar array tool", gui.sketch()["tool"] == "array_polar", gui.sketch()["tool"])
    out_dir = tempfile.mkdtemp(prefix="orca-mcp-action-pick-replay-")
    path = os.path.join(out_dir, "session.json")
    gui.client.call("replay_export", path=path, overwrite=True)
    picks = [s for s in steps_of(path) if s["tool"] == "action_run"]
    gui.check("action_pick_replay: the export keeps the pick with the item's arguments",
              len(picks) == 1 and picks[0]["args"].get("id") == "orca_command:sk_array_polar:orca"
              and picks[0]["args"].get("args") == {"type": "circular"}, picks)
    gui.cad("sketch_cancel")
    replay = replay_in_new_project(path, out_dir)
    gui.check("action_pick_replay: the exported session replays", replay.get("passed") is True,
              [s for s in replay.get("steps", []) if not s.get("ok")][:1])
    gui.check("action_pick_replay: the replayed pick arms the polar array tool", gui.sketch()["tool"] == "array_polar",
              gui.sketch()["tool"])
    gui.cad("sketch_cancel")
    gui.show_design()


RUNGS = {"words": rung_words, "shapes_match_tools": rung_shapes_match_tools, "sketch_edit": rung_sketch_edit,
         "sketch_edit_cancel": rung_sketch_edit_cancel, "sketch_edit_refused": rung_sketch_edit_refused,
         "sketch_edit_failed_replace": rung_sketch_edit_failed_replace,
         "sketch_edit_failed_leave": rung_sketch_edit_failed_leave, "sketch_edit_empty": rung_sketch_edit_empty,
         "project_model": rung_project_model, "text": rung_text, "svg_art": rung_svg_art,
         "scale_artwork": rung_scale_artwork, "surface_revolve_flip": rung_surface_revolve_flip,
         "feature_edit_refs": rung_feature_edit_refs, "text_edit": rung_text_edit, "edit_repick": rung_edit_repick,
         "edit_repick_placed": rung_edit_repick_placed,
         "edit_repick_datum": rung_edit_repick_datum, "edit_repick_hole": rung_edit_repick_hole,
         "edit_repick_hidden": rung_edit_repick_hidden, "design_view": rung_design_view, "select": rung_select,
         "select_loops": rung_select_loops, "confirm_card": rung_confirm_card, "cancel_card": rung_cancel_card,
         "cancel_sketch": rung_cancel_sketch, "cancel_plane_pick": rung_cancel_plane_pick,
         "text_dialog": rung_text_dialog, "confirm_nothing": rung_confirm_nothing, "confirm_disabled": rung_confirm_disabled,
         "confirm_refused_add": rung_confirm_refused_add, "confirm_failed_add": rung_confirm_failed_add,
         "place": rung_place, "place_select": rung_place_select,
         "place_follows": rung_place_follows, "replace": rung_replace, "boolean_opens": rung_boolean_opens, "action_pick": rung_action_pick,
         "action_pick_replay": rung_action_pick_replay}


def main():
    global gui
    # The baseline every test starts from on a shared app and data folder, then the Design tab's own start.
    gui = CadGui(orca_mcp.session_client()[0])
    gui.start()
    return gui.run(RUNGS, sys.argv[1:] or list(RUNGS))


if __name__ == "__main__":
    sys.exit(main())
