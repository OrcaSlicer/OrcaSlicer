#!/usr/bin/env python3
"""Where objects stand and how many there are, as the test_objects entry of tools/mcp/test-entries/objects.md designs
them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_objects.py [scenario ...]

Adding a shape at a size, a place, a plate and a name; moving, turning and scaling an instance, moving it to another
plate, and Auto Drop; moving and scaling a part, the selection and Mirror; the number of instances and Set as
Individual Objects; Drop to Bed, Center, Scale to Fit and Auto-Orient; Arrange and Fill bed; Clone; leaving instances
out of the print; and what the project file keeps. After every step the test reads the plates with plate_list and every
object and instance with object_list, and compares them with the state the entry writes out: the objects' names,
plates and instance counts, and each instance's place, turn, scale, mirror, printable flag, height above the bed and
Auto Drop. It runs on the harness's default printer, the Bambu Lab X1 Carbon 0.4, and exits 0 when every check passes.
Scenarios named on the command line (by their method names, such as arranging) run alone.
"""

import copy
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (Checks, apart, corner, gap, gcode_prints, inside, plates, rect, run, scene, selection,  # noqa: E402
                    square, wait)
from test_plates import P, turn  # noqa: E402

CUBE = {"shape": "cube", "size": [20, 20, 20]}


def close(got, want, tolerance):
    return len(got) == len(want) and all(abs(g - w) <= tolerance for g, w in zip(got, want))


def row_of(obj):
    """An object as the entry's tables write it: its name, @ and the plate its instances stand on, and ×n for n
    instances."""
    on = sorted({inst["plate"] for inst in obj["instances"]})
    n = len(obj["instances"])
    return f"{obj['name']}@{','.join(map(str, on))}" + (f"×{n}" if n > 1 else "")


def I(plate, at=None, **values):
    """What one instance must hold: its plate and place +(x, y) from that plate's corner (None: the app places it),
    and by default no turn, no scale, no mirror, printable and resting on the bed. A value of None is not compared."""
    out = {"plate": plate, "at": at, "rot": [0, 0, 0], "scale": [1, 1, 1], "mirrored": False, "printable": True,
           "z": 0}
    out.update(values)
    return out


def named(reply, key="objects"):
    """The (index, name) pairs a reply's list of objects gives."""
    return [(o.get("index"), o.get("name")) for o in reply.get(key, [])]


class ObjectsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder
        self.want = []

    # ---- Calls ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def undo(self):
        self.run("undo", history="project")

    def redo_to(self, state):
        """Redoes the last step undone; the objects are then expected as `state`."""
        self.run("redo", history="project")
        self.want = copy.deepcopy(state)

    def refused(self, label, tool, code=None, words=None, **args):
        return self.t.refused(label, lambda: self.c.call(tool, **args), code=code, words=words)

    def corner(self, plate, dx, dy, z=0):
        cx, cy = corner(self.c, plate)
        return [cx + dx, cy + dy, z]

    # ---- What the test expects ----

    def add(self, name, *instances, auto_drop=True):
        self.want.append({"name": name, "auto_drop": auto_drop, "inst": list(instances)})

    def inst(self, obj, i=0):
        return self.want[obj]["inst"][i]

    def by_name(self, name):
        found = [w for w in self.want if w["name"] == name]
        assert len(found) == 1, f"{len(found)} objects named {name}"
        return found[0]

    def saved(self):
        return copy.deepcopy(self.want)

    # ---- Reading the state back ----

    def start(self, name):
        self.run("project_new", discard_changes=True)
        self.run("view_set", tab="prepare")
        wait(self.c, "idle")
        self.want = []
        self.empty(f"{name}: the start")

    def empty(self, label):
        """What project_new leaves: one empty plate, current, and no objects."""
        self.t.equal(f"{label}: the plates", plates(self.c), P('"" 0 *'))
        self.t.equal(f"{label}: no objects", scene(self.c), [])

    def expect(self, label, plates_row, objects_row, any_order=False):
        """Compares plates() with plates_row, the objects' names, plates and instance counts with objects_row, and
        every instance with what self.want holds for it; gives scene() for the step's own checks."""
        self.t.equal(f"{label}: the plates", plates(self.c), P(*plates_row.split(" · ")))
        sc = scene(self.c)
        got, want = [row_of(o) for o in sc], objects_row.split(" · ") if objects_row else []
        if any_order:
            got, want = sorted(got), sorted(want)
        self.t.equal(f"{label}: the objects", got, want)
        self.placements(label, sc, any_order)
        return sc

    def placements(self, label, sc, by_name):
        corners = [item["origin"] for item in self.c.call("plate_list", limit=500)["items"]]
        problems = []
        if len(sc) != len(self.want):
            problems.append(f"{len(sc)} objects, the test expects {len(self.want)}")
        pairs = [(o, self.by_name(o["name"])) for o in sc] if by_name else zip(sc, self.want)
        for obj, want in pairs:
            who = f"object {obj['index']} ({obj['name']})"
            if obj["auto_drop"] != want["auto_drop"]:
                problems.append(f"{who}: auto_drop {obj['auto_drop']}")
            if len(obj["instances"]) != len(want["inst"]):
                problems.append(f"{who}: {len(obj['instances'])} instances, the test expects {len(want['inst'])}")
                continue
            for inst, w in zip(obj["instances"], want["inst"]):
                where = f"{who} instance {inst['index']}"
                if w["at"] is not None and inst["plate"] >= 0:
                    cx, cy = corners[inst["plate"]]
                    at = [inst["offset"][0] - cx, inst["offset"][1] - cy]
                    if not close(at, w["at"], 0.001):
                        problems.append(f"{where}: at +({at[0]:.3f}, {at[1]:.3f}), expected +{tuple(w['at'])}")
                if w["rot"] is not None and not close(turn(inst["rotation_deg"]), turn(w["rot"]), 1e-6):
                    problems.append(f"{where}: rotation_deg {inst['rotation_deg']}, expected {w['rot']}")
                if w["scale"] is not None and not close(inst["scale"], w["scale"], 0.001):
                    problems.append(f"{where}: scale {inst['scale']}, expected {w['scale']}")
                for key in ("mirrored", "printable"):
                    if inst[key] != w[key]:
                        problems.append(f"{where}: {key} {inst[key]}")
                if w["z"] is not None and abs(inst["bbox"]["min"][2] - w["z"]) > 0.001:
                    problems.append(f"{where}: bbox.min z {inst['bbox']['min'][2]}, expected {w['z']}")
        self.t.check(f"{label}: every instance as expected", not problems, "; ".join(problems))

    def find(self, sc, name):
        found = [o for o in sc if o["name"] == name]
        assert len(found) == 1, f"{len(found)} objects named {name}"
        return found[0]

    def squares(self, sc, plate):
        return [square(i) for o in sc for i in o["instances"] if i["plate"] == plate]

    def spread_on(self, label, plate, squares):
        """The squares lie inside the plate's rectangle, each apart from every other."""
        r = rect(self.c, plate)
        self.t.check(f"{label}: every square inside plate {plate}", all(inside(s, r) for s in squares), (squares, r))
        crossing = [(a, b) for i, a in enumerate(squares) for b in squares[i + 1:] if not apart(a, b)]
        self.t.check(f"{label}: the squares apart", not crossing, crossing)

    def size(self, label, inst, want, tolerance=0.01):
        self.t.near(f"{label}: bbox.size", inst["bbox"]["size"], want, tolerance)

    # ---- Scenarios ----

    def adding(self):
        s = "adding a shape"
        self.start(s)
        self.run("plate_add")
        self.run("plate_add")
        self.expect(f"{s} 1", '"" 0 · "" 0 · "" 0 *', "")
        reply = self.run("object_add_primitive", plate=1, name="first", **CUBE)
        self.t.equal(f"{s} 2: the reply's object, plate and objects", (reply.get("object"), reply.get("plate"),
                     named(reply)), (0, 1, [(0, "first")]))
        self.add("first", I(1))
        sc = self.expect(f"{s} 2", '"" 0 · "" 1 * · "" 0', "first@1")
        self.spread_on(f"{s} 2: first", 1, self.squares(sc, 1))
        self.size(f"{s} 2: first's", sc[0]["instances"][0], [20, 20, 20])
        self.t.equal(f"{s} 2: selection() objects", selection(self.c)["objects"], [0])
        after2 = self.saved()
        reply = self.run("object_add_primitive", name="second", position=self.corner(1, 112.8, 110), **CUBE)
        self.t.equal(f"{s} 3: the reply's object and plate", (reply.get("object"), reply.get("plate")), (1, 1))
        self.t.near(f"{s} 3: the reply's offset", reply.get("offset"), self.corner(1, 112.8, 110, 10), 0.001)
        self.add("second", I(1, (112.8, 110)))
        after3 = self.saved()
        self.expect(f"{s} 3", '"" 0 · "" 2 * · "" 0', "first@1 · second@1")
        self.t.equal(f"{s} 3: a size adds the shape whatever is selected, and selects it",
                     selection(self.c)["objects"], [1])
        self.undo()
        self.want = after2
        self.expect(f"{s} 4: adding, naming and placing second were one undo step", '"" 0 · "" 1 * · "" 0', "first@1")
        self.redo_to(after3)
        self.expect(f"{s} 5", '"" 0 · "" 2 * · "" 0', "first@1 · second@1")
        self.run("object_select", none=True)
        self.run("object_add_primitive", shape="cube", size=[40, 30, 10])
        self.add("Cube", I(1))
        sc = self.expect(f"{s} 6: named after its shape", '"" 0 · "" 3 * · "" 0', "first@1 · second@1 · Cube@1")
        self.size(f"{s} 6: Cube's", sc[2]["instances"][0], [40, 30, 10])
        self.spread_on(f"{s} 6: Cube", 1, [square(sc[2]["instances"][0])])
        self.refused(f"{s} 7: a cylinder's size of three is refused", "object_add_primitive",
                     words="A cylinder's size is [Diameter, Height]", shape="cylinder", size=[1, 2, 3])
        self.expect(f"{s} 7", '"" 0 · "" 3 * · "" 0', "first@1 · second@1 · Cube@1")

    def moving(self):
        s = "moving an instance"
        self.start(s)
        self.run("object_add_primitive", name="block", position=self.corner(0, 100, 100), **CUBE)
        self.run("object_select", none=True)
        self.run("object_add_primitive", name="other", position=self.corner(0, 200, 200), **CUBE)
        self.add("block", I(0, (100, 100)))
        self.add("other", I(0, (200, 200)))
        rows = '"" 2 *', "block@0 · other@0"
        self.expect(f"{s} 1", *rows)
        block = self.inst(0)
        steps = [
            (2, {"scale": 1.5}, {"scale": [1.5] * 3}, [30, 30, 30], "a scale that grows it drops it back"),
            (3, {"size": [45, None, None]}, {"scale": [2.25] * 3}, [45, 45, 45], "one size keeps the proportions"),
            (4, {"size": [30, 20, 10]}, {"scale": [1.5, 1, 0.5]}, [30, 20, 10], ""),
            (5, {"rotate": [0, 0, 30], "mode": "absolute"}, {"rot": [0, 0, 30]}, [35.98, 32.32, 10], ""),
            (6, {"rotate": [0, 0, 15]}, {"rot": [0, 0, 45]}, [35.36, 35.36, 10], "a call is relative by default"),
            (7, {"translate": [10, -20, 0]}, {"at": (110, 80)}, None, ""),
            (8, {"position": self.corner(0, 50, 60)}, {"at": (50, 60)}, None, "position is where it goes"),
        ]
        for step, args, change, box, why in steps:
            self.run("object_transform", object=0, **args)
            block.update(change)
            sc = self.expect(f"{s} {step}" + (f": {why}" if why else ""), *rows)
            if box:
                self.size(f"{s} {step}: block's", sc[0]["instances"][0], box)
        self.run("plate_add")
        self.expect(f"{s} 9", '"" 2 · "" 0 *', rows[1])
        reply = self.run("object_transform", object=0, plate=1)
        self.t.equal(f"{s} 10: the reply's plate", reply.get("plate"), 1)
        block["plate"] = 1
        self.expect(f"{s} 10", '"" 1 · "" 1 *', "block@1 · other@0")
        listing = self.c.call("object_list", plate=1)["plates"]
        self.t.equal(f"{s} 10: object_list of plate 1 lists block alone, with its instance 0",
                     [(p["index"], [(o["name"], [i["index"] for i in o["instances"]]) for o in p["objects"]])
                      for p in listing], [(1, [("block", [0])])])
        self.run("object_transform", object=0, plate=0)
        block["plate"] = 0
        rows = '"" 2 · "" 0 *', "block@0 · other@0"
        self.expect(f"{s} 11: moving an instance leaves the current plate", *rows)
        self.run("object_transform", object=0, translate=[0, 0, 15])
        self.expect(f"{s} 12: with Auto Drop on, a lifted object drops", *rows)
        reply = self.run("object_set_auto_drop", objects=[0], auto_drop=False)
        self.t.equal(f"{s} 13: the reply", (named(reply), reply.get("auto_drop")), ([(0, "block")], False))
        self.want[0]["auto_drop"] = False
        after13 = self.saved()
        self.expect(f"{s} 13", *rows)
        self.run("object_transform", object=1, translate=[0, 0, 15])
        self.expect(f"{s} 14: other's Auto Drop is on", *rows)
        self.run("object_transform", object=0, translate=[0, 0, 15])
        block["z"] = 15
        sc = self.expect(f"{s} 15: with Auto Drop off it stays where it is moved", *rows)
        self.t.near(f"{s} 15: block's bbox.max z", sc[0]["instances"][0]["bbox"]["max"][2], 25, 0.001)
        self.undo()
        self.want = after13
        self.expect(f"{s} 16: the move was one undo step", *rows)

    def parts_are(self, label, sc, obj, wants):
        """Each part of object `obj` as `wants` gives it: name, type, place +(x, y, z) from its instance's plate's
        corner, scale, mirror and, where not None, turn."""
        cx, cy = corner(self.c, sc[obj]["instances"][0]["plate"])
        problems = []
        parts = sc[obj]["parts"]
        if len(parts) != len(wants):
            problems.append(f"{len(parts)} parts, the test expects {len(wants)}")
        for part, w in zip(parts, wants):
            where = f"part {part['index']}"
            at = [part["position"][0] - cx, part["position"][1] - cy, part["position"][2]]
            if (part["name"], part["type"]) != (w["name"], w["type"]):
                problems.append(f"{where}: {part['name']!r} of type {part['type']}")
            if not close(at, w["at"], 0.001):
                problems.append(f"{where}: at +({at[0]:.3f}, {at[1]:.3f}, {at[2]:.3f}), expected +{tuple(w['at'])}")
            if not close(part["scale"], w["scale"], 0.001):
                problems.append(f"{where}: scale {part['scale']}, expected {w['scale']}")
            if w["rot"] is not None and not close(turn(part["rotation_deg"]), turn(w["rot"]), 1e-6):
                problems.append(f"{where}: rotation_deg {part['rotation_deg']}, expected {w['rot']}")
            if part["mirrored"] != w["mirrored"]:
                problems.append(f"{where}: mirrored {part['mirrored']}")
        self.t.check(f"{label}: the parts of {sc[obj]['name']}", not problems, "; ".join(problems))

    def moving_a_part(self):
        s = "moving a part"
        self.start(s)
        self.run("object_add_primitive", shape="cube", size=[30, 30, 10], name="base",
                 position=self.corner(0, 100, 100))
        self.run("object_select", none=True)
        self.run("object_add_primitive", name="single", position=self.corner(0, 200, 200), **CUBE)
        self.add("base", I(0, (100, 100)))
        self.add("single", I(0, (200, 200)))
        rows = '"" 2 *', "base@0 · single@0"
        def part(at, **values):
            """Part 1 of base, the cube part_add adds, at +(x, y, z): unturned, unscaled and not mirrored by default."""
            return {"name": "Generic-Cube", "type": "part", "at": at, "scale": [1, 1, 1], "rot": [0, 0, 0],
                    "mirrored": False, **values}

        part0 = {"name": "base", "type": "part", "at": (100, 100, 5), "scale": [1, 1, 1], "rot": [0, 0, 0],
                 "mirrored": False}

        def step(label, part1, base0=part0):
            sc = self.expect(label, *rows)
            self.parts_are(label, sc, 0, [base0, part1])
            return sc

        reply = self.run("part_add", object=0, type="part", shape="cube", size=[10, 10, 10],
                         position=self.corner(0, 120, 100, 15))
        self.t.equal(f"{s} 1: the reply's part and part_count", (reply.get("part"), reply.get("part_count")), (1, 2))
        sc = step(f"{s} 1", part((120, 100, 15)))
        p1 = sc[0]["parts"][1]
        self.t.equal(f"{s} 1: part 1's filament and triangles", (p1.get("filament"), p1["triangles"]), (1, 12))
        self.t.near(f"{s} 1: part 1's mesh_volume and scaled_volume", [p1["mesh_volume"], p1["scaled_volume"]],
                    [1000, 1000], 0.001)
        self.t.near(f"{s} 1: base's bbox.size", sc[0]["bbox"]["size"], [40, 30, 20], 0.001)
        self.run("object_transform", object=0, size=[60, None, None])
        self.inst(0)["scale"] = [1.5] * 3
        sc = step(f"{s} 2: the size scales both parts about the instance's place", part((130, 100, 22.5)),
                  base0={**part0, "at": (100, 100, 7.5)})
        self.t.near(f"{s} 2: base's bbox.size", sc[0]["bbox"]["size"], [60, 45, 30], 0.001)
        self.undo()
        self.inst(0)["scale"] = [1, 1, 1]
        step(f"{s} 3", part((120, 100, 15)))
        reply = self.run("object_transform", object=0, part=1, translate=self.corner(0, 110, 100, 15), mode="absolute")
        self.t.near(f"{s} 4: the reply's part_info.position", reply.get("part_info", {}).get("position"),
                    self.corner(0, 110, 100, 15), 0.001)
        step(f"{s} 4", part((110, 100, 15)))
        self.run("object_transform", object=0, part=1, translate=[5, 0, 0])
        step(f"{s} 5", part((115, 100, 15)))
        self.undo()
        step(f"{s} 6: each part move is one undo step", part((110, 100, 15)))
        reply = self.run("object_transform", object=0, part=1, scale=2, mode="absolute")
        self.t.near(f"{s} 7: the reply's size", reply.get("size"), [20, 20, 20], 0.001)
        sc = step(f"{s} 7", part((110, 100, 15), scale=[2, 2, 2]))
        self.t.near(f"{s} 7: part 1's scaled_volume", sc[0]["parts"][1]["scaled_volume"], 8000, 0.001)
        reply = self.run("object_transform", object=0, part=1, size=[None, None, 3])
        self.t.near(f"{s} 8: the reply's size: one size keeps the proportions", reply.get("size"), [3, 3, 3], 0.001)
        sc = step(f"{s} 8", part((110, 100, 15), scale=[0.3] * 3))
        self.t.near(f"{s} 8: part 1's mesh_volume and scaled_volume",
                    [sc[0]["parts"][1]["mesh_volume"], sc[0]["parts"][1]["scaled_volume"]], [1000, 27], 0.001)
        self.run("object_transform", object=0, part=1, rotate=[0, 0, 45])
        turned = part((110, 100, 15), scale=[0.3] * 3, rot=[0, 0, 45])
        step(f"{s} 9", turned)
        refusals = [
            (10, {"object": 0, "part": 1, "rotate": [0, 0, 0], "mode": "absolute"},
             "rotate a part relative to its current rotation"),
            (11, {"object": 0, "part": 1, "plate": 1}, "plate moves an instance"),
            (12, {"object": 1, "part": 0, "translate": [1, 0, 0]}, "object 1 has one part"),
        ]
        for n, args, words in refusals:
            self.refused(f"{s} {n}: refused", "object_transform", words=words, **args)
            step(f"{s} {n}", turned)
        reply = self.run("object_select", parts=[[0, 1]])
        self.t.equal(f"{s} 13: the reply", reply, {"canvas": "3d", "empty": False, "objects": [0],
                                                   "instances": [[0, 0]], "parts": [[0, 1]]})
        self.t.equal(f"{s} 13: selection()", selection(self.c), {"objects": [0], "instances": [[0, 0]],
                                                                  "parts": [[0, 1]]})
        step(f"{s} 13", turned)
        reply = self.run("object_select", objects=[0])
        self.t.equal(f"{s} 14: the reply lists no parts for a whole instance",
                     {k: reply.get(k) for k in ("objects", "instances", "parts")},
                     {"objects": [0], "instances": [[0, 0]], "parts": None})
        self.t.equal(f"{s} 14: selection()", selection(self.c), {"objects": [0], "instances": [[0, 0]]})
        step(f"{s} 14", turned)
        reply = self.run("object_mirror", parts=[[0, 1]], axis="x")
        self.t.equal(f"{s} 15: the reply names part 1 of base",
                     [(p.get("object"), p.get("part")) for p in reply.get("parts", [])], [(0, 1)])
        mirrored = part((110, 100, 15), scale=[0.3] * 3, rot=None, mirrored=True)
        step(f"{s} 15: a part mirrors in its place", mirrored)
        self.run("object_mirror", objects=[0], axis="x")
        self.inst(0)["mirrored"] = True
        step(f"{s} 16: an instance mirrors about the centre of the selection's box",
             {**mirrored, "at": (90, 100, 15)})
        self.run("object_mirror", objects=[0], axis="y")
        self.inst(0).update(mirrored=False, rot=[0, 0, 180])
        step(f"{s} 17: a mirror along X and one along Y make a half turn", {**mirrored, "at": (90, 100, 15)})
        self.run("object_select", none=True)
        self.refused(f"{s} 18: a mirror of nothing selected is refused", "object_mirror", code="nothing_selected",
                     axis="x")
        step(f"{s} 18", {**mirrored, "at": (90, 100, 15)})

    def instances(self):
        s = "the number of instances"
        self.start(s)
        self.run("object_add_primitive", name="a", position=self.corner(0, 60, 60), **CUBE)
        self.run("object_select", none=True)
        self.run("object_add_primitive", name="b", position=self.corner(0, 180, 60), **CUBE)
        self.run("part_add", object=1, type="modifier", shape="cube", size=[5, 5, 5])

        def a(n):
            """a's n instances: each one a call adds goes 12.8 mm further along x and y than the one before."""
            return [I(0, (60 + 12.8 * k, 60 + 12.8 * k)) for k in range(n)]

        self.add("a", *a(1))
        self.add("b", I(0, (180, 60)))
        after1 = self.saved()
        self.expect(f"{s} 1", '"" 2 *', "a@0 · b@0")
        self.run("object_select", none=True)
        self.refused(f"{s} 2: no target and nothing selected is refused", "object_set_instances",
                     code="nothing_selected", count=3)
        self.expect(f"{s} 2", '"" 2 *', "a@0 · b@0")
        reply = self.run("object_set_instances", object=0, count=3)
        self.t.equal(f"{s} 3: the reply", (reply.get("object"), reply.get("instances"), named(reply)),
                     (0, 3, [(0, "a")]))
        self.want[0]["inst"] = a(3)
        self.expect(f"{s} 3", '"" 2 *', "a@0×3 · b@0")
        self.t.equal(f"{s} 3: selection() objects", selection(self.c)["objects"], [0])
        self.undo()
        self.want = copy.deepcopy(after1)
        self.expect(f"{s} 4: the count was one undo step", '"" 2 *', "a@0 · b@0")
        self.run("object_set_instances", object=0, count=" 2 ")
        self.want[0]["inst"] = a(2)
        self.expect(f"{s} 5: a count written as text is read as the number", '"" 2 *', "a@0×2 · b@0")
        self.run("object_set_instances", object=0, delta=1)
        self.want[0]["inst"] = a(3)
        self.expect(f"{s} 6", '"" 2 *', "a@0×3 · b@0")
        self.run("object_set_instances", object=0, delta=-1)
        self.want[0]["inst"] = a(2)
        self.expect(f"{s} 7: the last instance goes", '"" 2 *', "a@0×2 · b@0")
        self.run("object_set_instances", object=0, count=1)
        self.want[0]["inst"] = a(1)
        self.expect(f"{s} 8", '"" 2 *', "a@0 · b@0")
        for count, code, words in [(0, None, "count must be >= 1"), (-2, None, "count must be >= 1"),
                                   (1001, "bad_instance_count", None), (2.5, None, "count must be of type integer"),
                                   ("three", None, "count must be of type integer")]:
            self.refused(f"{s} 9: a count of {count!r} is refused", "object_set_instances", code=code, words=words,
                         object=0, count=count)
            self.expect(f"{s} 9: after a count of {count!r}", '"" 2 *', "a@0 · b@0")
        self.run("object_select", objects=[0, 1])
        self.refused(f"{s} 10: two objects selected are refused", "object_set_instances", code="one_object_only",
                     count=2)
        self.expect(f"{s} 10", '"" 2 *', "a@0 · b@0")
        self.run("object_set_instances", object=0, count=2)
        self.want[0]["inst"] = a(2)
        reply = self.run("object_select", instances=[[0, 1]])
        self.t.equal(f"{s} 11: the reply", (reply.get("objects"), reply.get("instances")), ([0], [[0, 1]]))
        self.t.equal(f"{s} 11: selection()", selection(self.c), {"objects": [0], "instances": [[0, 1]]})
        self.expect(f"{s} 11", '"" 2 *', "a@0×2 · b@0")
        reply = self.run("object_select", objects=[0])
        self.t.equal(f"{s} 12: the selection names the instances it is in", reply.get("instances"),
                     [[0, 0], [0, 1]])
        self.expect(f"{s} 12", '"" 2 *', "a@0×2 · b@0")
        self.run("object_select", instances=[[0, 1]])
        self.run("object_set_instances", count=4)
        self.want[0]["inst"] = a(4)
        self.expect(f"{s} 13: the selected instance stands for its object", '"" 2 *', "a@0×4 · b@0")
        self.run("object_select", parts=[[1, 1]])
        self.run("object_set_instances", count=2)
        self.want[1]["inst"] = [I(0, (180, 60)), I(0, (192.8, 72.8))]
        self.expect(f"{s} 14: the selected part stands for its object", '"" 2 *', "a@0×4 · b@0×2")
        self.run("object_set_printable", instances=[[1, 0]], printable=False)
        self.inst(1, 0)["printable"] = False
        self.refused(f"{s} 15: more instances of an object with one not printable are refused",
                     "object_set_instances", code="unprintable_instance", object=1, count=3)
        self.expect(f"{s} 15", '"" 2 *', "a@0×4 · b@0×2")
        self.run("object_set_instances", object=1, count=1)
        self.want[1]["inst"] = [I(0, (180, 60), printable=False)]
        self.expect(f"{s} 16: fewer instances need no printable rule", '"" 2 *', "a@0×4 · b@0")
        self.run("object_set_printable", objects=[1], printable=True)
        self.inst(1)["printable"] = True
        self.run("object_select", objects=[1])
        reply = self.c.call("action_run", id="obj_set_instances")
        self.t.equal(f"{s} 17: the action waits on its dialog", reply.get("status"), "blocked_by_dialog")
        self.t.equal(f"{s} 17: the dialog open", [d.get("title") for d in self.c.call("app_state")["dialogs"]],
                     ["Copies of the selected object"])
        self.run("dialog_answer", button="cancel")
        done = wait(self.c, "job", job=reply.get("job"))
        self.t.equal(f"{s} 17: the job's result after Cancel", (done.get("object"), done.get("instances")), (1, 1))
        self.expect(f"{s} 17", '"" 2 *', "a@0×4 · b@0")
        self.run("object_set_instances", object=1, count=2)
        self.want[1]["inst"] = [I(0, (180, 60)), I(0, (192.8, 72.8))]
        after_count = self.saved()
        reply = self.run("object_separate_instances", object=1)
        self.t.equal(f"{s} 18: the reply's objects", named(reply), [(1, "b"), (2, "b")])
        self.want[1]["inst"] = [I(0, (180, 60))]
        self.add("b", I(0, (192.8, 72.8)))
        sc = self.expect(f"{s} 18", '"" 3 *', "a@0×4 · b@0 · b@0")
        self.t.equal(f"{s} 18: each instance's object keeps the parts", [[p["type"] for p in o["parts"]] for o in sc],
                     [["part"], ["part", "modifier"], ["part", "modifier"]])
        self.undo()
        self.want = after_count
        self.expect(f"{s} 19: Set as Individual Objects was one undo step", '"" 2 *', "a@0×4 · b@0×2")
        reply = self.run("object_separate_instances", instances=[[0, 3]])
        self.t.equal(f"{s} 20: the reply's objects", named(reply), [(2, "a")])
        self.want[0]["inst"] = a(3)
        self.add("a", I(0, (98.4, 98.4)))
        self.expect(f"{s} 20: where the instance stood", '"" 3 *', "a@0×3 · b@0×2 · a@0")
        self.run("object_select", objects=[0])
        reply = self.run("object_separate_instances")
        self.t.equal(f"{s} 21: the reply's objects", named(reply), [(0, "a"), (3, "a"), (4, "a")])
        self.want[0]["inst"] = a(1)
        self.add("a", I(0, (85.6, 85.6)))
        self.add("a", I(0, (72.8, 72.8)))
        self.expect(f"{s} 21: each instance but the first becomes an object, the last instance first", '"" 5 *',
                    "a@0 · b@0×2 · a@0 · a@0 · a@0")
        self.start(f"{s} 22")
        self.run("view_set", camera="top")
        self.run("object_add_primitive", name="cut", **CUBE)
        self.run("object_select", objects=[0])
        self.c.call("widget_click", target={"gizmo": "Cut"})
        self.c.call("widget_set_value", target={"imgui_name": "move_Z"}, value=10)
        self.c.call("widget_click", target={"imgui": "Add connectors"})
        self.c.call("mouse_gesture", at={"object": 0, "spot": "top"}, check="point")
        self.c.call("widget_click", target={"imgui": "Confirm connectors"})
        self.c.call("widget_click", target={"imgui": "Perform cut"})
        wait(self.c, "idle")
        self.refused(f"{s} 22: instances of a cut object are refused", "object_set_instances", code="cut_object",
                     object=0, count=2)
        self.add("cut", I(0))
        self.add("cut", I(0))
        self.expect(f"{s} 22: the cut's halves keep one instance each", '"" 2 *', "cut@0 · cut@0")

    def placing(self):
        s = "placing objects"
        self.start(s)
        self.run("plate_add")
        self.run("plate_add")
        self.run("object_add_primitive", plate=1, name="first", position=self.corner(1, 40, 60), **CUBE)
        self.run("object_add_primitive", plate=1, name="second", position=self.corner(1, 100, 60), **CUBE)
        self.run("part_add", object=1, type="part", shape="cube", size=[5, 5, 5],
                 position=self.corner(1, 115, 60, 2.5))
        self.run("object_add_primitive", plate=2, name="third", position=self.corner(2, 60, 60), **CUBE)
        self.add("first", I(1, (40, 60)))
        self.add("second", I(1, (100, 60)))
        self.add("third", I(2, (60, 60)))
        rows = '"" 0 · "" 2 · "" 1 *', "first@1 · second@1 · third@2"

        def second_part(label, sc, at):
            part = sc[1]["parts"][1]
            cx, cy = corner(self.c, 1)
            self.t.near(f"{label}: second's part at +{at}", [part["position"][0] - cx, part["position"][1] - cy,
                                                              part["position"][2]], list(at), 0.001)

        sc = self.expect(f"{s} 1", *rows)
        second_part(f"{s} 1", sc, (115, 60, 2.5))
        reply = self.run("object_mirror", objects=[1], axis="x")
        self.t.equal(f"{s} 2: the reply names second", named(reply), [(1, "second")])
        self.inst(1).update(mirrored=True, at=(107.5, 60))
        sc = self.expect(f"{s} 2: second mirrors about the centre of its box", *rows)
        second_part(f"{s} 2", sc, (92.5, 60, 2.5))
        self.run("object_transform", object=1, translate=[0, 0, 15], drop_to_bed=False)
        self.inst(1)["z"] = 15
        self.expect(f"{s} 3: an object of two parts stays above the bed", *rows)
        self.run("object_transform", object=0, translate=[0, 0, 15], drop_to_bed=False)
        self.expect(f"{s} 4: an object of one part left above the bed drops anyway", *rows)
        reply = self.run("object_drop", objects=[1])
        self.t.equal(f"{s} 5: the reply", (reply.get("moved"), named(reply)), (True, [(1, "second")]))
        self.inst(1)["z"] = 0
        self.expect(f"{s} 5", *rows)
        reply = self.run("object_drop", objects=[1])
        self.t.equal(f"{s} 6: a drop of what rests on the bed is not refused", reply.get("moved"), False)
        self.expect(f"{s} 6", *rows)
        self.run("object_transform", object=1, translate=[0, 0, 15], drop_to_bed=False)
        self.run("object_select", objects=[1])
        reply = self.run("object_drop")
        self.t.equal(f"{s} 7: the reply's moved", reply.get("moved"), True)
        self.expect(f"{s} 7: no target drops the selection", *rows)
        for step in (8, 9):
            reply = self.run("object_center", objects=[0, 2])
            self.t.equal(f"{s} {step}: the reply's plates", reply.get("plates"),
                         [{"plate": 1, "objects": [0]}, {"plate": 2, "objects": [2]}])
            self.inst(0)["at"] = self.inst(2)["at"] = (128, 128)
            self.expect(f"{s} {step}: each goes to the centre of its own plate", *rows)
        self.run("object_add_primitive", plate=1, name="fourth", position=self.corner(1, 128, 60), **CUBE)
        self.add("fourth", I(1, (128, 60)))
        self.expect(f"{s} 10", '"" 0 · "" 3 * · "" 1', "first@1 · second@1 · third@2 · fourth@1")
        reply = self.run("object_center", objects=[0, 3], plate=0)
        self.t.equal(f"{s} 11: the reply's plates", reply.get("plates"), [{"plate": 0, "objects": [0, 3]}])
        self.inst(0).update(plate=0, at=(128, 162))
        self.inst(3).update(plate=0, at=(128, 94))
        self.expect(f"{s} 11: the two move as one group", '"" 2 · "" 1 * · "" 1',
                    "first@0 · second@1 · third@2 · fourth@0")
        self.run("object_select", objects=[2])
        reply = self.run("object_center")
        self.t.equal(f"{s} 12: the reply's plates", reply.get("plates"), [{"plate": 1, "objects": [2]}])
        self.inst(2).update(plate=1, at=(128, 128))
        rows = '"" 2 · "" 2 * · "" 0', "first@0 · second@1 · third@1 · fourth@0"
        self.expect(f"{s} 12: the selection goes to the centre of the current plate", *rows)
        self.run("object_transform", object=2, rotate=[45, 0, 0], mode="absolute")
        self.inst(2)["rot"] = [45, 0, 0]
        sc = self.expect(f"{s} 13", *rows)
        self.t.near(f"{s} 13: third's bbox.size z", sc[2]["instances"][0]["bbox"]["size"][2], 28.28, 0.01)
        reply = self.run("object_auto_orient", objects=[1, 2])
        self.t.equal(f"{s} 14: the reply's objects", named(reply), [(1, "second"), (2, "third")])
        self.t.equal(f"{s} 14: the reply's rotations", [(r.get("object"), r.get("instance"))
                                                        for r in reply.get("rotations", [])], [(1, 0), (2, 0)])
        self.inst(1)["rot"] = self.inst(2)["rot"] = None
        sc = self.expect(f"{s} 14: naming objects turns no other", *rows)
        third = sc[2]["instances"][0]
        self.t.near(f"{s} 14: third lies on a face, rotation_deg x", third["rotation_deg"][0], 90, 0.1)
        self.t.near(f"{s} 14: third lies on a face, bbox.size z", third["bbox"]["size"][2], 20, 0.01)
        self.t.equal(f"{s} 14: selection() objects", selection(self.c)["objects"], [1, 2])
        self.run("object_select", none=True)
        self.run("object_transform", object=0, rotate=[0, 45, 0], mode="absolute")
        reply = self.run("object_auto_orient")
        rotations = {(r.get("object"), r.get("instance")): r.get("rotation_deg") for r in reply.get("rotations", [])}
        self.t.equal(f"{s} 15: with nothing selected every object is oriented", sorted(rotations),
                     [(0, 0), (1, 0), (2, 0), (3, 0)])
        for w in self.want:
            w["inst"][0]["rot"] = None
        sc = self.expect(f"{s} 15", *rows)
        self.t.check(f"{s} 15: each rotation the reply gives is the instance's",
                     all(close(turn(rotations[(o["index"], 0)]), turn(o["instances"][0]["rotation_deg"]), 1e-6)
                         for o in sc if (o["index"], 0) in rotations), rotations)
        self.t.near(f"{s} 15: first lies on a face, bbox.size z", sc[0]["instances"][0]["bbox"]["size"][2], 20, 0.01)
        s_fit = 11.888
        for step, name, index, plates_row in [(16, "fit", 4, '"" 2 · "" 3 * · "" 0'),
                                              (17, "fit2", 5, '"" 2 · "" 4 * · "" 0')]:
            at = (60, 200) if name == "fit" else (200, 60)
            self.run("object_add_primitive", plate=1, name=name, position=self.corner(1, *at), **CUBE)
            if name == "fit":
                reply = self.run("object_scale_to_fit", objects=[index])
            else:
                self.run("object_select", objects=[index])
                reply = self.run("object_scale_to_fit")
            scales = reply.get("scales", [])
            self.t.equal(f"{s} {step}: the reply's scales name {name}",
                         [(x.get("object"), x.get("instance")) for x in scales], [(index, 0)])
            self.t.near(f"{s} {step}: the reply's scale", scales[0].get("scale") if scales else None, [s_fit] * 3,
                        0.001)
            self.add(name, I(1, None, scale=[s_fit] * 3))
            rows = plates_row, rows[1] + f" · {name}@1"
            sc = self.expect(f"{s} {step}", *rows)
            fit = sc[index]["instances"][0]
            cx, cy = corner(self.c, 1)
            self.t.near(f"{s} {step}: {name} fills the widest free part of the bed, at",
                        [fit["offset"][0] - cx, fit["offset"][1] - cy], [137, 128], 0.01)
            self.size(f"{s} {step}: {name}'s", fit, [237.76] * 3, 0.02)

    def arranging(self):
        s = "arranging and filling the bed"
        self.start(s)
        for step, args in [(1, {}), (2, {"plates": 0})]:
            self.refused(f"{s} {step}: an arrange with no objects is refused", "object_arrange",
                         code="no_objects_to_arrange", **args)
            self.expect(f"{s} {step}", '"" 0 *', "")
        for name in ("A", "B"):
            self.run("object_add_primitive", name=name, position=self.corner(0, 128, 128), **CUBE)
            self.run("object_select", none=True)
        self.add("A", I(0, (128, 128)))
        self.add("B", I(0, (128, 128)))
        after3 = self.saved()
        rows = '"" 2 *', "A@0 · B@0"
        self.expect(f"{s} 3: one on the other", *rows, any_order=True)

        def two(label, at_least=None, under=None):
            sc = self.expect(label, *rows, any_order=True)
            a, b = (square(self.find(sc, n)["instances"][0]) for n in ("A", "B"))
            self.spread_on(label, 0, [a, b])
            if at_least is not None:
                self.t.check(f"{label}: the gap between A and B at least {at_least}", gap(a, b) >= at_least, gap(a, b))
            if under is not None:
                self.t.check(f"{label}: the gap between A and B under {under}", gap(a, b) < under, gap(a, b))
            return sc

        self.run("view_set", tab="preview")
        reply = self.run("object_arrange")
        self.t.equal(f"{s} 4: the reply's off_plate and plates", (reply.get("off_plate"), reply.get("plates")),
                     ([], 1))
        self.t.equal(f"{s} 4: an arrange shows the Prepare view", self.c.call("app_state").get("tab"), "prepare")
        for w in self.want:
            w["inst"][0]["at"] = None
        two(f"{s} 4")
        self.undo()
        self.want = copy.deepcopy(after3)
        self.expect(f"{s} 5: the arrange was one undo step", *rows, any_order=True)
        for w in self.want:
            w["inst"][0]["at"] = None
        reply = self.run("object_arrange", plates=0, spacing=30, rotate=False, align_y=False)
        settings = reply.get("settings", {})
        self.t.equal(f"{s} 6: the reply's settings and off_plate", (settings.get("spacing"), settings.get("rotate"),
                     settings.get("align_y"), reply.get("off_plate")), (30, False, False, []))
        sc = two(f"{s} 6", at_least=29.9)
        for name in ("A", "B"):
            self.run("object_transform", object=self.find(sc, name)["index"], position=self.corner(0, 128, 128))
        reply = self.run("object_arrange")
        self.t.equal(f"{s} 7: the arrange options panel kept the spacing", reply.get("settings", {}).get("spacing"), 30)
        two(f"{s} 7", at_least=29.9)
        reply = self.run("object_arrange", plates=0, spacing=10)
        self.t.equal(f"{s} 8: the reply's spacing", reply.get("settings", {}).get("spacing"), 10)
        sc = two(f"{s} 8: the spacing decides how far apart they go", at_least=9.9, under=29.9)
        self.run("plate_add")
        self.refused(f"{s} 9: an arrange of an empty plate is refused", "object_arrange",
                     words="Plate 1 has no objects to arrange", plates=1)
        self.expect(f"{s} 9", '"" 2 · "" 0 *', rows[1], any_order=True)
        self.run("object_transform", object=self.find(sc, "A")["index"], position=self.corner(0, 60, 60))
        self.run("object_transform", object=self.find(sc, "B")["index"], position=self.corner(0, 180, 180))
        self.run("object_add_primitive", name="C", plate=1, position=self.corner(1, 128, 128), **CUBE)
        self.by_name("A")["inst"] = [I(0, (60, 60))]
        self.by_name("B")["inst"] = [I(0, (180, 180))]
        self.add("C", I(1, (128, 128)))
        rows = '"" 2 · "" 1 *', "A@0 · B@0 · C@1"
        sc = self.expect(f"{s} 10", *rows, any_order=True)
        self.refused(f"{s} 11: a plate the project lacks is refused", "object_arrange",
                     words="Plate 7 does not exist (the project has plates 0..1)", plates=7)
        self.expect(f"{s} 11", *rows, any_order=True)
        self.refused(f"{s} 12: fill_object with plates is refused", "object_arrange",
                     words="fill_object fills the object's own plate", fill_object=0, plates=0)
        self.expect(f"{s} 12", *rows, any_order=True)
        self.run("object_set_instances", object=self.find(sc, "C")["index"], count=3)
        reply = self.run("object_arrange", plates=1, spacing=10, rotate=False)
        settings = reply.get("settings", {})
        self.t.equal(f"{s} 13: the reply's settings", (settings.get("spacing"), settings.get("rotate")), (10, False))
        self.by_name("C")["inst"] = [I(1) for _ in range(3)]
        after13 = self.saved()
        rows = '"" 2 · "" 1 *', "A@0 · B@0 · C@1×3"
        sc = self.expect(f"{s} 13: arranging one plate leaves the other plates' objects", *rows, any_order=True)
        self.spread_on(f"{s} 13: C", 1, self.squares(sc, 1))
        self.run("object_arrange", plates=0)
        self.by_name("A")["inst"] = [I(0)]
        self.by_name("B")["inst"] = [I(0)]
        rows = '"" 2 * · "" 1', rows[1]
        sc = self.expect(f"{s} 14: the plate arranged is made current", *rows, any_order=True)
        self.spread_on(f"{s} 14: A and B", 0, self.squares(sc, 0))
        self.t.check(f"{s} 14: C's three inside plate 1",
                     all(inside(q, rect(self.c, 1)) for q in self.squares(sc, 1)), self.squares(sc, 1))
        self.run("object_transform", object=self.find(sc, "A")["index"], position=self.corner(0, 60, 60))
        self.run("object_transform", object=self.find(sc, "B")["index"], position=self.corner(0, 180, 180))
        a = self.find(sc, "A")["index"]
        reply = self.run("object_arrange", fill_object=a)
        sc = scene(self.c)
        n = len(self.find(sc, "A")["instances"])
        self.t.check(f"{s} 15: A has at least 2 instances", n >= 2, n)
        self.t.equal(f"{s} 15: the reply", {k: reply.get(k) for k in ("object", "filled_with", "added", "instances",
                                                                         "plate", "off_plate")},
                     {"object": a, "filled_with": "instances", "added": n - 1, "instances": n, "plate": 0,
                      "off_plate": []})
        self.by_name("A")["inst"] = [I(0) for _ in range(n)]
        self.by_name("B")["inst"] = [I(0, (180, 180))]
        sc = self.expect(f"{s} 15: Fill bed adds to its object alone, and moves no other", rows[0],
                         f"A@0×{n} · B@0 · C@1×3", any_order=True)
        self.spread_on(f"{s} 15: plate 0", 0, self.squares(sc, 0))
        self.undo()
        self.want = after13
        sc = self.expect(f"{s} 16: Fill bed was one undo step", rows[0], "A@0 · B@0 · C@1×3", any_order=True)
        c = self.find(sc, "C")["index"]
        reply = self.run("object_arrange", fill_object=c)
        sc = scene(self.c)
        m = len(self.find(sc, "C")["instances"])
        self.t.check(f"{s} 17: C has at least 4 instances", m >= 4, m)
        self.t.equal(f"{s} 17: the reply", {k: reply.get(k) for k in ("plate", "added", "instances")},
                     {"plate": 1, "added": m - 3, "instances": m})
        self.by_name("C")["inst"] = [I(1) for _ in range(m)]
        sc = self.expect(f"{s} 17: an object on another plate fills its own plate, made current", '"" 2 · "" 1 *',
                         f"A@0 · B@0 · C@1×{m}", any_order=True)
        self.spread_on(f"{s} 17: C", 1, self.squares(sc, 1))

    def cloning(self):
        s = "cloning"
        self.start(s)
        self.run("object_select", none=True)
        self.run("object_add_primitive", name="cube", position=self.corner(0, 60, 60), **CUBE)
        self.add("cube", I(0, (60, 60)))
        after0 = self.saved()
        reply = self.run("object_clone", objects=[0], arrange=False)
        copied = reply.get("copied", {})
        self.t.equal(f"{s} 1: the reply", (named(reply), sorted(copied), named(copied), reply.get("object_count"),
                                           reply.get("arranged")),
                     ([(1, "cube")], ["objects"], [(0, "cube")], 2, False))
        self.add("cube", I(0))
        sc = self.expect(f"{s} 1: without an arrange the source stays", '"" 2 *', "cube@0 · cube@0")
        self.spread_on(f"{s} 1", 0, self.squares(sc, 0))
        self.t.equal(f"{s} 1: selection() objects", selection(self.c)["objects"], [1])
        self.undo()
        self.want = copy.deepcopy(after0)
        self.expect(f"{s} 2", '"" 1 *', "cube@0")
        reply = self.run("object_clone", objects=[0], count=3, arrange=True)
        self.t.equal(f"{s} 3: the reply", (named(reply), reply.get("object_count"), reply.get("arranged")),
                     ([(1, "cube"), (2, "cube"), (3, "cube")], 4, True))
        self.want = [{"name": "cube", "auto_drop": True, "inst": [I(0)]} for _ in range(4)]
        sc = self.expect(f"{s} 3", '"" 4 *', "cube@0 · cube@0 · cube@0 · cube@0")
        self.spread_on(f"{s} 3", 0, self.squares(sc, 0))
        self.undo()
        self.inst(0)["at"] = (60, 60)
        self.expect(f"{s} 4: the arrangement is an undo step of its own", '"" 4 *',
                    "cube@0 · cube@0 · cube@0 · cube@0")
        self.undo()
        self.want = copy.deepcopy(after0)
        self.expect(f"{s} 5: the copies were one undo step", '"" 1 *', "cube@0")
        self.run("view_set", camera="iso")
        self.run("view_set", camera="fit_objects")
        self.run("object_select", objects=[0])
        wait(self.c, "idle")
        reply = self.c.call("mouse_gesture", at={"object": 0, "spot": "top"}, button="right", popup_item="Clone")
        self.t.equal(f"{s} 6: the gesture ran", reply.get("ran"), "Clone")
        wait(self.c, "dialog")
        self.t.equal(f"{s} 6: the dialog that opens", [d.get("title") for d in self.c.call("app_state")["dialogs"]],
                     ["Clone"])
        # The dialog stays open, its fields disabled, while it clones and arranges, and the answer's job follows that.
        self.c.await_job(self.c.call("dialog_answer", dialog={"title": "Clone"}, button="OK"))
        wait(self.c, "idle")
        self.t.equal(f"{s} 6: no dialog open afterwards", self.c.call("app_state").get("dialogs"), [])
        self.want = [{"name": "cube", "auto_drop": True, "inst": [I(0)]} for _ in range(2)]
        sc = self.expect(f"{s} 6", '"" 2 *', "cube@0 · cube@0")
        self.spread_on(f"{s} 6", 0, self.squares(sc, 0))
        self.run("object_set_instances", object=0, count=2)
        self.run("object_select", instances=[[0, 1]])
        reply = self.run("object_clone", arrange=False)
        copied = reply.get("copied", {})
        self.t.equal(f"{s} 7: the reply", ([(i.get("object"), i.get("instance")) for i in copied.get("instances", [])],
                                           "objects" in copied, named(reply), reply.get("object_count")),
                     ([(0, 1)], False, [(2, "cube")], 3))
        self.want[0]["inst"].append(I(0))
        self.add("cube", I(0))
        self.expect(f"{s} 7: a selected instance is copied as a new object of one instance", '"" 3 *',
                    "cube@0×2 · cube@0 · cube@0")

    def printable(self):
        s = "leaving instances out of the print"
        self.start(s)
        self.run("object_select", none=True)
        self.run("object_add_primitive", shape="cube", size=[10, 10, 10], name="a", position=self.corner(0, 60, 60))
        self.run("object_set_instances", object=0, count=3)
        self.run("object_select", none=True)
        self.run("object_add_primitive", shape="cube", size=[15, 15, 15], name="b", position=self.corner(0, 180, 60))
        self.add("a", I(0, (60, 60)), I(0, (72.8, 72.8)), I(0, (85.6, 85.6)))
        self.add("b", I(0, (180, 60)))
        after1 = self.saved()
        rows = '"" 2 *', "a@0×3 · b@0"

        def sliced(label, step, a_copies):
            reply = self.run("plate_slice", plates=0)
            self.t.equal(f"{label}: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
            path = os.path.join(self.folder, f"printable-{step}.gcode")
            self.run("project_export", format="gcode", path=path, overwrite=True)
            self.t.equal(f"{label}: the G-code prints a in {a_copies} copies and b in 1", sorted(gcode_prints(path)),
                         [("a", a_copies), ("b", 1)])

        self.expect(f"{s} 1", *rows)
        sliced(f"{s} 1", 1, 3)
        reply = self.run("object_set_printable", instances=[[0, 1]], printable=False)
        self.t.equal(f"{s} 2: the reply", ([(i.get("object"), i.get("instance"), i.get("printable"))
                                            for i in reply.get("instances", [])], "objects" in reply),
                     ([(0, 1, False)], False))
        self.inst(0, 1)["printable"] = False
        self.expect(f"{s} 2", *rows)
        sliced(f"{s} 3: an instance that is not printable is left out of the G-code", 3, 2)
        self.expect(f"{s} 3", *rows)
        self.undo()
        self.want = after1
        self.expect(f"{s} 4: Printable was one undo step", *rows)
        reply = self.run("object_set_printable", objects=[0], printable=False)
        self.t.equal(f"{s} 5: the reply", ([(i.get("object"), i.get("instance"), i.get("printable"))
                                            for i in reply.get("instances", [])], named(reply)),
                     ([(0, 0, False), (0, 1, False), (0, 2, False)], [(0, "a")]))
        for inst in self.want[0]["inst"]:
            inst["printable"] = False
        self.expect(f"{s} 5: an object sets every instance of its own and no other's", *rows)

    def kept(self):
        s = "what the project keeps"
        self.start(s)
        self.run("plate_add")
        self.run("object_add_primitive", plate=0, name="kept", position=self.corner(0, 60, 60), **CUBE)
        self.run("object_set_instances", object=0, count=2)
        self.run("object_transform", object=0, instance=0, rotate=[0, 0, 30], mode="absolute")
        self.run("object_transform", object=0, instance=1, scale=1.5)
        self.run("object_select", instances=[[0, 1]])
        self.run("object_mirror", axis="x")
        self.run("object_set_printable", instances=[[0, 1]], printable=False)
        self.add("kept", I(0, (60, 60), rot=None, mirrored=True),
                 I(0, (72.8, 72.8), scale=[1.5] * 3, mirrored=True, printable=False))

        def turned(label, sc):
            self.size(f"{label}: kept's instance 0, turned 30 degrees,", sc[0]["instances"][0],
                      [20 * (math.cos(math.radians(30)) + math.sin(math.radians(30)))] * 2 + [20])

        turned(f"{s} 1", self.expect(f"{s} 1: Mirror of the selected instance mirrors the object's other one",
                                     '"" 1 * · "" 0', "kept@0×2"))
        self.run("object_add_primitive", plate=1, name="lifted", position=self.corner(1, 60, 60), **CUBE)
        self.run("object_set_auto_drop", objects=[1], auto_drop=False)
        self.run("object_transform", object=1, translate=[0, 0, 15])
        self.add("lifted", I(1, (60, 60), z=15), auto_drop=False)
        rows = "kept@0×2 · lifted@1"
        turned(f"{s} 2", self.expect(f"{s} 2", '"" 1 · "" 1 *', rows))
        path = os.path.join(self.folder, "kept.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.empty(f"{s} 3: after project_new")
        self.run("project_open", path=path)
        wait(self.c, "idle")
        turned(f"{s} 3", self.expect(f"{s} 3: the project file keeps them; opening it makes plate 0 current",
                                     '"" 1 * · "" 1', rows))

    def run_all(self, only=()):
        scenarios = [("adding a shape", self.adding), ("moving an instance", self.moving),
                     ("moving a part", self.moving_a_part), ("the number of instances", self.instances),
                     ("placing objects", self.placing), ("arranging and filling the bed", self.arranging),
                     ("cloning", self.cloning), ("leaving instances out of the print", self.printable),
                     ("what the project keeps", self.kept)]
        for name, scenario in scenarios:
            if not only or scenario.__name__ in only:
                self.t.scenario(name, scenario)
        self.run("project_new", discard_changes=True)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-objects-") as folder:
        return ObjectsTest(client, folder).run_all(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
