#!/usr/bin/env python3
"""Plates and objects, as the test_objects entry of tools/mcp/test-entries/objects.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_objects.py [scenario ...]

Three parts run one after another in one app. The plates of the Prepare view: adding, duplicating, naming, locking,
selecting and deleting plates and undoing that; what a lock keeps Arrange and Auto-Orient from doing; a plate's own
settings; the filament slots its filament orders count on; and the plate icons and plate menu. Where objects stand
and how many there are: adding a shape at a size, a place, a plate and a name; moving, turning and scaling an
instance, moving it to another plate, and Auto Drop; moving and scaling a part, the selection and Mirror; the number
of instances and Set as Individual Objects; Drop to Bed, Center, Scale to Fit and Auto-Orient; Arrange and Fill bed;
Clone; and leaving instances out of the print. What objects are made of and how the list orders them: renaming and
deleting, the layer commands, Split and Assemble, the parts and their types, ordering and the print order, Fix Model,
Reload from disk and Replace 3D file, and the keep_painting preference. Last, what the project file keeps of all
three, through one save and reopen.

After every step the test reads the plates back with plate_list and the objects with object_list, and compares them
with the state the entry writes out. It runs on the Generic Klipper Printer 0.4 nozzle, and exits 0 when every check
passes. Scenarios named on the command line (by their method names, such as arranging) run alone; cloning arranges
with the options arranging stores, so it runs after it.
"""

import copy
import math
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, SLOT_COLOURS, Checks, add_slot, apart, corner,  # noqa: E402
                    gap, gcode_prints, inside, objects, plates, rect, run, scene, selection, slots, square, start,
                    menus_left, unsaved, until, wait)

CUBE = {"shape": "cube", "size": [20, 20, 20]}
START = {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}
# How far along x and y an instance a call adds goes from the object's last one: 0.05 of the bed's 250 mm.
STEP = 12.5
FILAMENT_ADD = "orca_command:filament_add:orca"
FILAMENT_DELETE = "orca_command:filament_delete:orca"
# The plate types the Klipper printer offers with support_multi_bed_types on.
PLATE_TYPES = ["Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate", "Textured Cool Plate",
               "Supertack Plate"]
# A plate's settings as plate_list gives them.
PLATE_KEYS = ["curr_bed_type", "print_sequence", "spiral_mode", "first_layer_print_sequence",
              "other_layers_print_sequence", "overrides"]

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(REPO, "tools", "mcp", "tests", "fixtures")
# The system's own form of the path, as the loader names the object after its last separator.
RINGS = os.path.normpath(os.path.join(REPO, "tests", "data", "two_hollow_squares.obj"))
DRC = os.path.join(REPO, "resources", "handy_models", "OrcaCube_v2.drc")
STAR = os.path.join(FIXTURES, "star.svg")
HEIGHTMAP = os.path.join(FIXTURES, "heightmap.png")
RING = [10.67, 10.67, 2.8]
RINGS_SIZE = [68.67, 10.67, 2.8]
# The endings of Fix Model's notices (the object list's fix_through_cgal()).
REPAIR_ENDINGS = ("has been repaired", "have been repaired", "needed no repair", "Failed to repair",
                  "Repairing was canceled")

# A cube of side 1 from the origin: its corners and its twelve triangles, each turned outward; the last is the one
# holed.stl lacks, and the two top ones (4, 5, 6) and (4, 6, 7) are the ones top4.stl splits into four.
CORNERS = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
TRIANGLES = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]

ROW = re.compile(r'^"(.*)" (\d+)( \*)?( L)?$')


def P(*rows):
    """Plates written as the entry writes them: '"Copy" 1 * L' is a plate named Copy with one object, current and
    locked."""
    out = []
    for row in rows:
        m = ROW.match(row)
        assert m, row
        out.append({"name": m.group(1), "current": bool(m.group(3)), "locked": bool(m.group(4)),
                    "objects": int(m.group(2))})
    return out


def S(**own):
    """A plate's settings as the entry writes them: the project's (Cool Plate, by layer, spiral off, both orders
    Auto, no overrides) but for those given."""
    out = {"curr_bed_type": "Cool Plate", "print_sequence": "by layer", "spiral_mode": False,
           "first_layer_print_sequence": None, "other_layers_print_sequence": None, "overrides": []}
    out.update(own)
    return out


def plate_settings(client):
    """Each plate's settings from plate_list, PLATE_KEYS of each, in plate order."""
    return [{k: item.get(k) for k in PLATE_KEYS} for item in client.call("plate_list", limit=500)["items"]]


def turn(degrees):
    """The rotation matrix, flattened, of rotation_deg [x, y, z] as the app composes it (z, then y, then x), so that
    two sets of angles that name one turn compare equal: [180, 90, 180] is [0, 90, 0]."""
    a, b, c = (math.radians(d) for d in degrees)
    rx = [[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]]
    ry = [[math.cos(b), 0, math.sin(b)], [0, 1, 0], [-math.sin(b), 0, math.cos(b)]]
    rz = [[math.cos(c), -math.sin(c), 0], [math.sin(c), math.cos(c), 0], [0, 0, 1]]
    mul = lambda p, q: [[sum(p[i][k] * q[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return [v for row in mul(rz, mul(ry, rx)) for v in row]


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


def cube_triangles(size, holed=False, top4=False):
    """The triangles of a cube of `size` mm from the origin: without the last one when `holed`, with its top as four
    triangles about the top's centre when `top4`."""
    points = [tuple(c * size for c in corner_) for corner_ in CORNERS]
    triangles = [tuple(points[i] for i in t) for t in TRIANGLES]
    if holed:
        triangles = triangles[:-1]
    if top4:
        centre = (size / 2, size / 2, size)
        triangles = [t for i, t in enumerate(triangles) if i not in (2, 3)]
        triangles += [(points[a], points[b], centre) for a, b in ((4, 5), (5, 6), (6, 7), (7, 4))]
    return triangles


def write_stl(path, triangles):
    """An ASCII STL of the triangles."""
    lines = ["solid cube"]
    for triangle in triangles:
        lines += ["  facet normal 0 0 0", "    outer loop"]
        lines += ["      vertex %g %g %g" % vertex for vertex in triangle]
        lines += ["    endloop", "  endfacet"]
    lines.append("endsolid cube")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def centre(box):
    return [(lo + hi) / 2 for lo, hi in zip(box["min"], box["max"])]


def names(objects_):
    """The objects' names in index order, from scene()."""
    return [o["name"] for o in objects_]


def types(obj):
    """An object's parts' types in order, from its entry in scene()."""
    return [p["type"] for p in obj["parts"]]


def names_of(reply_items):
    """The `index` and `name` of each object a reply names."""
    return [{"index": o.get("index"), "name": o.get("name")} for o in reply_items or []]


class Part:
    """What the three parts share: the client, the checks they print, the test's folder W and the calls they make."""

    def __init__(self, client, checks, folder):
        self.c = client
        self.t = checks
        self.W = folder

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def undo(self):
        self.run("undo", history="project")

    def redo(self):
        self.run("redo", history="project")

    def plates_are(self, label, *rows):
        self.t.equal(f"{label}: the plates", plates(self.c), P(*rows))


class Plates(Part):
    """The plates: after every step plates() is compared whole with the row."""

    # ---- Calls and reads ----

    def objects_are(self, label, on, outside=()):
        self.t.equal(f"{label}: the objects", objects(self.c), (sorted(on), sorted(outside)))

    def start(self, name):
        self.t.equal(f"{name}: the start: the presets", start(self.c), START)
        self.began = time.monotonic()  # the scenario's own menus are those closed after this
        wait(self.c, "idle")
        self.plates_are(f"{name}: the start", '"" 0 *')

    def listing(self):
        """Each object of object_list with the plate it is listed on, as (plate, object)."""
        listing = self.c.call("object_list", limit=500, detail=True)
        return [(p["index"], o) for p in listing["plates"] for o in p["objects"]]

    def find(self, name, plate):
        found = [o for p, o in self.listing() if p == plate and o["name"] == name]
        assert len(found) == 1, f"{len(found)} objects named {name} on plate {plate}"
        return found[0]

    def rule_uses(self, match):
        for rule in self.c.call("app_state").get("auto_answer_rules", []):
            if rule.get("match") == match:
                return rule.get("uses", 0)
        return None

    # ---- Checks on objects ----

    def at(self, label, obj, corner_, dx, dy):
        """The object's instance 0 at the plate's corner + (dx, dy), resting on the bed."""
        inst = obj["instances"][0]
        self.t.near(f"{label}: {obj['name']} at its plate's corner + ({dx}, {dy})", inst["offset"][:2],
                    [corner_[0] + dx, corner_[1] + dy], 0.001)
        self.t.near(f"{label}: {obj['name']} rests on the bed", inst["bbox"]["min"][2], 0, 0.001)

    def squares_apart_on(self, label, plate, names_):
        """The objects named lie inside the plate's rectangle and their squares do not overlap."""
        r = rect(self.c, plate)
        squares = []
        for name in names_:
            sq = square(self.find(name, plate)["instances"][0])
            squares.append(sq)
            self.t.check(f"{label}: {name} lies inside plate {plate}", inside(sq, r), (sq, r))
        self.t.check(f"{label}: {' and '.join(names_)} do not overlap", apart(*squares), squares)

    # ---- Scenarios ----

    def life(self):
        s = "a plate's life"
        self.start(s)
        self.t.refused(f"{s} 1: plate_delete of the only plate is refused", lambda: self.c.call("plate_delete"),
                       code="last_plate")
        self.plates_are(f"{s} 1", '"" 0 *')
        self.run("plate_add")
        self.plates_are(f"{s} 2", '"" 0', '"" 0 *')
        self.run("plate_add")
        self.plates_are(f"{s} 3", '"" 0', '"" 0', '"" 0 *')
        self.run("plate_rename", plate=1, name="Left")
        self.plates_are(f"{s} 4", '"" 0', '"Left" 0 *', '"" 0')
        self.run("plate_lock", plate=2, locked=True)
        after5 = ('"" 0', '"Left" 0 *', '"" 0 L')
        self.plates_are(f"{s} 5", *after5)
        self.run("plate_delete", plate=0)
        after6 = ('"Left" 0 *', '"" 0 L')
        self.plates_are(f"{s} 6", *after6)
        self.undo()
        self.plates_are(f"{s} 7", *after5)
        self.redo()
        self.plates_are(f"{s} 8", *after6)

    def by_index(self):
        s = "by index"
        self.start(s)
        self.run("object_add_primitive", name="first", **CUBE)
        self.plates_are(f"{s} 1", '"" 1 *')
        self.objects_are(f"{s} 1", [(0, "first")])
        self.run("plate_add")
        self.plates_are(f"{s} 2", '"" 1', '"" 0 *')
        self.run("object_add_primitive", name="second", plate=1, **CUBE)
        self.plates_are(f"{s} 3", '"" 1', '"" 1 *')
        self.objects_are(f"{s} 3", [(0, "first"), (1, "second")])
        self.run("plate_select", plate=0)
        self.plates_are(f"{s} 4", '"" 1 *', '"" 1')
        self.run("plate_duplicate", plate=1)
        self.plates_are(f"{s} 5", '"" 1 *', '"" 1', '"" 1')
        self.objects_are(f"{s} 5", [(0, "first"), (1, "second"), (2, "second")])
        self.run("plate_duplicate")
        self.plates_are(f"{s} 6", '"" 1 *', '"" 1', '"" 1', '"" 1')
        self.objects_are(f"{s} 6", [(0, "first"), (1, "second"), (2, "second"), (3, "first")])
        self.run("plate_rename", plate=2, name="Copy")
        self.plates_are(f"{s} 7", '"" 1', '"" 1', '"Copy" 1 *', '"" 1')
        self.run("plate_select", plate=1)
        self.run("plate_rename", name="Second")
        after8 = ('"" 1', '"Second" 1 *', '"Copy" 1', '"" 1')
        self.plates_are(f"{s} 8", *after8)
        self.run("plate_rename", name="Second")
        self.plates_are(f"{s} 9", *after8)
        self.undo()
        self.plates_are(f"{s} 10: the unchanged name took no undo step", '"" 1', '"" 1 *', '"Copy" 1', '"" 1')
        self.redo()
        self.plates_are(f"{s} 11", *after8)
        reply = self.run("plate_lock", plate=3, locked=True)
        self.t.equal(f"{s} 12: the reply says the lock changed", reply.get("changed"), True)
        self.plates_are(f"{s} 12", '"" 1', '"Second" 1 *', '"Copy" 1', '"" 1 L')
        self.run("plate_lock", plate=3)
        self.plates_are(f"{s} 13: without locked the lock toggles", *after8)
        self.run("plate_lock")
        after14 = ('"" 1', '"Second" 1 * L', '"Copy" 1', '"" 1')
        self.plates_are(f"{s} 14: the current plate", *after14)
        reply = self.run("plate_lock", locked=True)
        self.t.equal(f"{s} 15: the reply says the lock did not change", reply.get("changed"), False)
        self.plates_are(f"{s} 15", *after14)
        self.undo()
        self.plates_are(f"{s} 16: the unchanged lock took no undo step", *after8)
        self.redo()
        self.run("plate_lock", locked=False)
        self.plates_are(f"{s} 17", *after8)
        self.run("plate_select", plate=2)
        after18 = ('"" 1', '"Second" 1', '"Copy" 1 *', '"" 1')
        self.plates_are(f"{s} 18", *after18)
        self.run("plate_select")
        self.plates_are(f"{s} 19: without a plate the current plate stays", *after18)
        self.run("plate_delete", plate=1)
        after20 = ('"" 1', '"Copy" 1 *', '"" 1')
        self.plates_are(f"{s} 20", *after20)
        self.objects_are(f"{s} 20", [(0, "first"), (1, "second"), (2, "first")], ["second"])
        self.run("plate_delete")
        self.plates_are(f"{s} 21: deleting the current plate makes the plate before it current", '"" 1 *', '"" 1')
        self.objects_are(f"{s} 21", [(0, "first"), (1, "first")], ["second", "second"])
        self.undo()
        self.plates_are(f"{s} 22", *after20)
        self.objects_are(f"{s} 22", [(0, "first"), (1, "second"), (2, "first")], ["second"])

    def duplicating(self):
        s = "duplicating a plate"
        self.start(s)
        c0 = corner(self.c, 0)
        self.run("object_add_primitive", name="cube", position=[c0[0] + 40, c0[1] + 60, 0], **CUBE)
        self.run("object_transform", object=0, rotate=[0, 0, 30], mode="absolute")
        self.plates_are(f"{s} 1", '"" 1 *')
        cube = self.find("cube", 0)
        self.at(f"{s} 1", cube, c0, 40, 60)
        self.t.near(f"{s} 1: the cube is turned 30 degrees", cube["instances"][0]["rotation_deg"], [0, 0, 30], 0.001)
        self.t.near(f"{s} 1: the turned cube's size", cube["instances"][0]["bbox"]["size"],
                    [20 * (math.cos(math.radians(30)) + math.sin(math.radians(30)))] * 2 + [20], 0.01)
        self.t.equal(f"{s} 1: the cube's triangles", cube["parts"][0]["triangles"], 12)
        self.t.near(f"{s} 1: the cube's mesh volume", cube["parts"][0]["mesh_volume"], 8000, 0.001)
        self.run("object_add_primitive", shape="cylinder", size=[20, 30], name="cylinder",
                 position=[c0[0] + 120, c0[1] + 80, 0])
        self.plates_are(f"{s} 2", '"" 2 *')
        cylinder = self.find("cylinder", 0)
        self.at(f"{s} 2", cylinder, c0, 120, 80)
        self.t.near(f"{s} 2: the cylinder's size", cylinder["instances"][0]["bbox"]["size"], [20, 20, 30], 0.1)
        self.t.near(f"{s} 2: the cylinder's mesh volume", cylinder["parts"][0]["mesh_volume"],
                    math.pi * 10 ** 2 * 30, math.pi * 10 ** 2 * 30 * 0.01)
        places = {"cube": (40, 60), "cylinder": (120, 80)}

        def sources(label):
            for name, (dx, dy) in places.items():
                self.at(label, self.find(name, 0), c0, dx, dy)

        reply = self.run("plate_duplicate", plate=0)
        self.t.equal(f"{s} 3: the reply's new plate", reply.get("new_plate"), 1)
        self.plates_are(f"{s} 3", '"" 2 *', '"" 2')
        c1 = corner(self.c, 1)
        for name, (dx, dy) in places.items():
            source, copy_ = self.find(name, 0), self.find(name, 1)
            self.at(f"{s} 3: the copy", copy_, c1, dx, dy)
            si, ci = source["instances"][0], copy_["instances"][0]
            self.t.near(f"{s} 3: the {name}'s copy is turned as it", ci["rotation_deg"], si["rotation_deg"], 0.001)
            self.t.near(f"{s} 3: the {name}'s copy is scaled as it", ci["scale"], si["scale"], 0.001)
            self.t.near(f"{s} 3: the {name}'s copy has its size", ci["bbox"]["size"], si["bbox"]["size"], 0.001)
            self.t.equal(f"{s} 3: the {name}'s copy has its mesh",
                         [(p["triangles"], p["mesh_volume"]) for p in copy_["parts"]],
                         [(p["triangles"], p["mesh_volume"]) for p in source["parts"]])
        sources(f"{s} 3: the source")
        self.objects_are(f"{s} 3", [(0, "cube"), (0, "cylinder"), (1, "cube"), (1, "cylinder")])
        copy_ = self.find("cube", 1)
        self.run("object_transform", object=copy_["index"], translate=[10, 0, 0])
        self.t.near(f"{s} 4: the cube's copy moved 10 mm along x", self.find("cube", 1)["instances"][0]["offset"][:2],
                    [c1[0] + 50, c1[1] + 60], 0.001)
        self.plates_are(f"{s} 4", '"" 2 *', '"" 2')
        sources(f"{s} 4: the source, which a copy's move leaves")
        self.undo()
        self.undo()
        self.plates_are(f"{s} 5: the duplicate is one undo step", '"" 2 *')
        self.objects_are(f"{s} 5", [(0, "cube"), (0, "cylinder")])
        sources(f"{s} 5")

    def lock(self):
        s = "what a lock does"
        self.start(s)
        side = {"shape": "cube", "size": [20, 20, 10]}
        c0 = corner(self.c, 0)
        self.run("object_add_primitive", name="A", position=[c0[0] + 30, c0[1] + 30, 0], **side)
        self.run("object_transform", object=0, rotate=[0, 90, 0], mode="absolute")
        self.run("plate_add")
        c1 = corner(self.c, 1)
        self.run("object_add_primitive", name="B", plate=1, position=[c1[0] + 30, c1[1] + 30, 0], **side)
        self.run("object_transform", object=1, rotate=[0, 90, 0], mode="absolute")
        self.run("plate_lock", plate=0, locked=True)
        self.plates_are(f"{s} 1", '"" 1 L', '"" 1 *')

        def a_stands(label, dx=30):
            a = self.find("A", 0)["instances"][0]
            self.t.near(f"{label}: A stands where it was put", a["offset"][:2], [c0[0] + dx, c0[1] + 30], 0.001)
            self.t.near(f"{label}: A is turned on its side", turn(a["rotation_deg"]), turn([0, 90, 0]), 1e-6)
            self.t.near(f"{label}: A stands 20 mm tall", a["bbox"]["size"], [10, 20, 20], 0.01)

        a_stands(f"{s} 1")
        reply = self.run("object_arrange")
        self.t.check(f"{s} 2: the arrange skipped the locked plate 0",
                     {"plate": 0, "reason": "locked"} in reply.get("skipped_plates", []), reply)
        self.plates_are(f"{s} 2", '"" 1 L', '"" 1 *')
        a_stands(f"{s} 2")
        self.t.equal(f"{s} 2: B stays on plate 1", [p for p, o in self.listing() if o["name"] == "B"], [1])
        self.t.refused(f"{s} 3: arranging the locked plate is refused",
                       lambda: self.c.call("object_arrange", plates=0),
                       words='Plate 0 is locked; unlock it first (plate_lock {"plate": 0, "locked": false})')
        self.plates_are(f"{s} 3", '"" 1 L', '"" 1 *')
        a_stands(f"{s} 3")
        self.t.refused(f"{s} 4: Auto-Orient of the locked plate is refused",
                       lambda: self.c.call("object_auto_orient", plates=[0]), code="plate_locked")
        self.plates_are(f"{s} 4", '"" 1 L', '"" 1 *')
        a_stands(f"{s} 4")
        reply = self.run("object_auto_orient", objects=[0, 1])
        self.t.equal(f"{s} 5: the reply names B alone", [o.get("index") for o in reply.get("objects", [])], [1])
        self.t.near(f"{s} 5: B lies flat", self.find("B", 1)["instances"][0]["bbox"]["size"][2], 10, 0.01)
        self.plates_are(f"{s} 5", '"" 1 L', '"" 1 *')
        a_stands(f"{s} 5")
        self.run("object_transform", object=0, translate=[5, 0, 0])
        self.plates_are(f"{s} 6", '"" 1 L', '"" 1 *')
        a_stands(f"{s} 6: a lock does not stop a move by hand", dx=35)
        self.run("plate_lock", plate=0, locked=False)
        self.run("object_auto_orient", objects=[0])
        self.plates_are(f"{s} 7", '"" 1', '"" 1 *')
        self.t.near(f"{s} 7: A lies flat once its plate is unlocked", self.find("A", 0)["instances"][0]["bbox"]["size"][2],
                    10, 0.01)
        self.run("object_arrange")
        wait(self.c, "idle")
        self.plates_are(f"{s} 8: plate 1, empty now, is kept", '"" 2', '"" 0 *')
        self.squares_apart_on(f"{s} 8", 0, ["A", "B"])

    def own_settings(self):
        s = "a plate's own settings"
        self.start(s)
        # A plate takes a plate type of its own only on a printer that offers several; an unsaved printer edit, which
        # the next start() discards.
        self.run("settings_set", type="printer", values={"support_multi_bed_types": "1"})
        add_slot(self.c, SLOT_COLOURS[1])
        self.t.equal(f"{s}: two filament slots", slots(self.c), "0,0")
        self.run("project_set_settings", values={"curr_bed_type": "Cool Plate"})
        self.run("object_add_primitive", name="first", **CUBE)
        self.run("plate_add")
        self.run("object_add_primitive", name="second", plate=1, **CUBE)
        self.plates_are(f"{s}: the start", '"" 1', '"" 1 *')

        def settings_are(label, p0, p1):
            self.t.equal(f"{label}: the plates' settings", plate_settings(self.c), [p0, p1])

        settings_are(f"{s}: the start", S(), S())
        after1 = S(curr_bed_type="Textured PEI Plate", print_sequence="by object",
                   overrides=["curr_bed_type", "print_sequence"])
        self.run("plate_set_settings", plate=1, values={"curr_bed_type": "Textured PEI Plate", "print_sequence": "by object"})
        settings_are(f"{s} 1", S(), after1)
        got = {i["key"]: i for i in self.c.call("plate_get_settings", plate=1,
                                                keys=["curr_bed_type", "print_sequence", "spiral_mode"])["items"]}
        self.t.equal(f"{s} 1: plate_get_settings gives the plate type", got["curr_bed_type"]["value"],
                     "Textured PEI Plate")
        self.t.equal(f"{s} 1: plate_get_settings gives the print sequence", got["print_sequence"]["value"], "by object")
        self.t.equal(f"{s} 1: the plate types offered", got["curr_bed_type"]["meta"].get("offered"), PLATE_TYPES)
        self.t.equal(f"{s} 1: spiral mode is the project's", (got["spiral_mode"]["value"],
                                                               got["spiral_mode"]["effective"]), (None, False))
        self.undo()
        settings_are(f"{s} 2", S(), S())
        self.redo()
        settings_are(f"{s} 3", S(), after1)
        reply = self.run("plate_set_settings", plate=1, values={"print_sequence": None})
        self.t.equal(f"{s} 4: the reply's effective print sequence", reply.get("effective", {}).get("print_sequence"),
                     "by layer")
        after4 = S(curr_bed_type="Textured PEI Plate", overrides=["curr_bed_type"])
        settings_are(f"{s} 4", S(), after4)
        refusals = [
            (5, {"plate": 1, "values": {"curr_bed_type": "Glass Plate", "print_sequence": "by object"}},
             'does not offer curr_bed_type "Glass Plate"'),
            (6, {"plate": 0, "values": {"first_layer_print_sequence": [1, 1]}},
             "A filament order lists every filament slot 1..2 once"),
            (7, {"plate": 0, "values": {"other_layers_print_sequence": [{"from": 2, "order": [1, 2]},
                                                                         {"from": 5, "to": 9, "order": [2, 1]}]}},
             "Layer ranges overlap"),
            (8, {"plate": 0, "values": {"skirt_start_angle": 90}}, '"skirt_start_angle" is a process setting'),
        ]
        for step, args, words in refusals:
            self.t.refused(f"{s} {step}: refused", lambda: self.c.call("plate_set_settings", **args), words=words)
            settings_are(f"{s} {step}", S(), after4)
        self.t.refused(f"{s} 9: a lock of a plate the project lacks is refused",
                       lambda: self.c.call("plate_lock", plate=2, locked=True), code="no_such_plate")
        settings_are(f"{s} 9", S(), after4)
        self.plates_are(f"{s} 9", '"" 1', '"" 1 *')
        self.run("plate_set_settings", plate=0, values={
            "first_layer_print_sequence": [2, 1],
            "other_layers_print_sequence": [{"from": 10, "order": [1, 2]}, {"from": 2, "to": 9, "order": [2, 1]}]})
        settings_are(f"{s} 10: the ranges come back sorted", S(
            first_layer_print_sequence=[2, 1],
            other_layers_print_sequence=[{"from": 2, "to": 9, "order": [2, 1]}, {"from": 10, "order": [1, 2]}],
            overrides=["first_layer_print_sequence", "other_layers_print_sequence"]), after4)
        self.plates_are(f"{s} 10: the plate set is made current", '"" 1 *', '"" 1')
        self.run("plate_set_settings", values={"first_layer_print_sequence": None, "other_layers_print_sequence": None})
        settings_are(f"{s} 11: with no plate it sets the current plate", S(), after4)
        spiral = "Spiral mode only works"
        self.run("dialog_auto_answer", rules=[{"match": spiral, "button": "yes"}])
        self.t.equal(f"{s} 12: the spiral rule has answered nothing yet", self.rule_uses(spiral), 0)
        self.run("plate_set_settings", plate=0, values={"spiral_mode": True})
        settings_are(f"{s} 12", S(spiral_mode=True, overrides=["spiral_mode"]), after4)
        self.t.equal(f"{s} 12: the spiral mode question was asked once", self.rule_uses(spiral), 1)
        self.run("dialog_auto_answer", rules=[])
        self.run("plate_set_settings", plate=0, values={"spiral_mode": None})
        settings_are(f"{s} 13", S(), after4)
        reply = self.run("plate_slice", plates=1)
        self.t.equal(f"{s} 14: the slice of plate 1", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        items = self.c.call("plate_list", limit=500)["items"]
        self.t.equal(f"{s} 14: plate 1 is sliced and plate 0 is not", [i["sliced"] for i in items], [False, True])
        self.t.equal(f"{s} 14: plate 1 has no layer G-code", items[1]["layer_gcode"], [])
        settings_are(f"{s} 14", S(), after4)

    def filament_slots(self):
        s = "filament slots"
        self.start(s)
        self.t.refused(f"{s} 1: deleting the only slot is refused", lambda: self.c.call("filament_delete"),
                       code="last_filament_slot")
        self.t.equal(f"{s} 1", slots(self.c), "0")
        steps = [(2, FILAMENT_ADD, "Added filament slot 2.", "0,0"), (3, FILAMENT_ADD, "Added filament slot 3.", "0,0,0")]
        for step, action, message, want in steps:
            reply = self.run("action_run", id=action)
            self.t.equal(f"{s} {step}: the message", reply.get("message"), message)
            self.t.equal(f"{s} {step}", slots(self.c), want)
        self.t.refused(f"{s} 4: deleting slot 4 of 3 is refused",
                       lambda: self.c.call("filament_delete", filament_slot=4), code="no_such_slot")
        self.t.equal(f"{s} 4", slots(self.c), "0,0,0")
        reply = self.run("filament_delete", filament_slot=2)
        self.t.equal(f"{s} 5: the message", reply.get("message"), "Deleted filament slot 2.")
        self.t.equal(f"{s} 5", slots(self.c), "0,0")
        job = self.c.call("widget_click", target={"label": "Add Mixed Filament"})
        wait(self.c, "dialog")
        dialogs = self.c.call("app_state").get("dialogs", [])
        self.t.equal(f"{s} 6: the dialog that opens", [d.get("title") for d in dialogs], ["Add Mixed Filament"])
        self.run("dialog_answer", button="OK")
        self.c.await_job(job)
        wait(self.c, "idle")
        self.t.equal(f"{s} 6", slots(self.c), "0,0,1")
        self.t.refused(f"{s} 7: a filament order is refused while the project has mixed filaments",
                       lambda: self.c.call("plate_set_settings", plate=0,
                                           values={"first_layer_print_sequence": [2, 1]}),
                       words="cannot be set while the project has mixed filaments")
        self.t.equal(f"{s} 7: plate 0's order stays Auto",
                     self.c.call("plate_list")["items"][0]["first_layer_print_sequence"], None)
        self.t.equal(f"{s} 7", slots(self.c), "0,0,1")
        reply = self.run("action_run", id=FILAMENT_ADD)
        self.t.equal(f"{s} 8: the message", reply.get("message"), "Added filament slot 3.")
        self.t.equal(f"{s} 8: a new slot goes ahead of the mixed one", slots(self.c), "0,0,0,1")
        reply = self.run("action_run", id=FILAMENT_DELETE)
        self.t.equal(f"{s} 9: the message", reply.get("message"), "Deleted filament slot 3.")
        self.t.equal(f"{s} 9", slots(self.c), "0,0,1")
        self.run("widget_click", target={"label": "Remove last mixed filament"})
        wait(self.c, "idle")
        self.t.equal(f"{s} 10", slots(self.c), "0,0")

    def icons(self):
        s = "plate icons"
        self.start(s)
        _, texts = self.c.call_with_text("widget_list", target="main", query="plate_icon")
        listing = "\n".join(texts)
        shown = set()
        for line in listing.splitlines():
            if "plate_icon" in line and re.search(r"\bplate 0\b", line):
                m = re.search(r"plate_icon[^\"]*\"([^\"]*)\"", line)
                if m:
                    shown.add("plate" if "name=plate" in line else m.group(1))
        self.t.equal(f"{s} 1: the icons plate 0 shows", shown,
                     {"plate", "delete", "orient", "arrange", "lock", "settings", "name", "move_to_front"})
        if self.t.failed and self.t.failed[-1].startswith(f"{s} 1"):
            print(listing)
        self.plates_are(f"{s} 1", '"" 0 *')
        icon = lambda name, plate, **args: self.c.call("widget_click", target={"plate_icon": name, "plate": plate},
                                                       **args)
        icon("lock", 0)
        self.plates_are(f"{s} 2", '"" 0 * L')
        icon("lock", 0)
        self.plates_are(f"{s} 3", '"" 0 *')
        reply = icon("settings", 0)
        titles = [d.get("title") for d in reply.get("dialogs", [])]
        self.t.equal(f"{s} 4: the dialog that opens", titles, ["Plate Settings"])
        self.run("dialog_answer", dialog={"title": "Plate Settings"}, button="cancel")
        wait(self.c, "idle")
        self.plates_are(f"{s} 4", '"" 0 *')
        self.t.equal(f"{s} 4: plate 0 keeps the project's settings",
                     self.c.call("plate_list")["items"][0]["overrides"], [])
        self.run("widget_click", target={"gltoolbar": "addplate"})
        wait(self.c, "idle")
        self.plates_are(f"{s} 5", '"" 0', '"" 0 *')
        x0, y0, x1, y1 = rect(self.c, 1)
        middle = [(x0 + x1) / 2, (y0 + y1) / 2, 0]
        for name in ("A", "B"):
            self.run("object_add_primitive", name=name, plate=1, position=middle, **CUBE)
        self.plates_are(f"{s} 6", '"" 0', '"" 2 *')
        for name in ("A", "B"):
            self.t.near(f"{s} 6: {name} at plate 1's middle", self.find(name, 1)["instances"][0]["offset"][:2],
                        middle[:2], 0.001)
        reply = icon("arrange", 1)
        self.t.equal(f"{s} 7: the reply follows the arrange", (reply.get("started"), reply.get("done")), (True, True))
        wait(self.c, "idle")
        self.plates_are(f"{s} 7", '"" 0', '"" 2 *')
        self.squares_apart_on(f"{s} 7", 1, ["A", "B"])
        reply = icon("plate", 1, button="right")
        items = reply.get("popup_menu", {}).get("items", [])
        for item in ("Edit Plate Name", "Lock", "Delete Plate"):
            self.t.check(f"{s} 8: the plate menu lists {item}", item in items, items)
        self.plates_are(f"{s} 8", '"" 0', '"" 2 *')
        reply = icon("plate", 1, button="right", popup_item="Lock")
        self.t.equal(f"{s} 9: the reply's item", reply.get("ran"), "Lock")
        self.plates_are(f"{s} 9", '"" 0', '"" 2 * L')
        icon("lock", 1)
        self.plates_are(f"{s} 10", '"" 0', '"" 2 *')
        icon("move_to_front", 1)
        wait(self.c, "idle")
        self.plates_are(f"{s} 11", '"" 2 *', '"" 0')
        self.t.equal(f"{s} 11: both cubes on plate 0", sorted(o["name"] for p, o in self.listing() if p == 0), ["A", "B"])
        state = self.c.call("app_state")
        self.t.equal(f"{s} 12: no dialog and no menu left open",
                     (state.get("dialogs"), menus_left(state, self.began)), ([], []))

    def scenarios(self):
        return [("a plate's life", self.life), ("by index", self.by_index), ("duplicating a plate", self.duplicating),
                ("what a lock does", self.lock), ("a plate's own settings", self.own_settings),
                ("filament slots", self.filament_slots), ("plate icons", self.icons)]


class Places(Part):
    """Where objects stand and how many there are: after every step the plates, and every object's name, plate and
    instance count, and every instance against self.want."""

    def __init__(self, client, checks, folder):
        super().__init__(client, checks, folder)
        self.want = []

    # ---- Calls ----

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
        self.t.equal(f"{name}: the start: the presets", start(self.c), START)
        wait(self.c, "idle")
        self.want = []
        self.empty(f"{name}: the start")

    def renew(self, name):
        """A project_new within a scenario, and the check of the empty project it leaves."""
        self.run("project_new", discard_changes=True)
        wait(self.c, "idle")
        self.want = []
        self.empty(f"{name}: the start")

    def empty(self, label):
        """What project_new leaves: one empty plate, current, and no objects."""
        self.plates_are(label, '"" 0 *')
        self.t.equal(f"{label}: no objects", scene(self.c), [])

    def expect(self, label, plates_row, objects_row, any_order=False):
        """Compares plates() with plates_row, the objects' names, plates and instance counts with objects_row, and
        every instance with what self.want holds for it; gives scene() for the step's own checks."""
        self.plates_are(label, *plates_row.split(" · "))
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
        listing = self.c.call("object_list", plate=1, detail=True)["plates"]
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
            """a's n instances: each one a call adds goes STEP further along x and y than the one before."""
            return [I(0, (60 + STEP * k, 60 + STEP * k)) for k in range(n)]

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
        self.want[1]["inst"] = [I(0, (180, 60)), I(0, (180 + STEP, 60 + STEP))]
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
        self.want[1]["inst"] = [I(0, (180, 60)), I(0, (180 + STEP, 60 + STEP))]
        after_count = self.saved()
        reply = self.run("object_separate_instances", object=1)
        self.t.equal(f"{s} 18: the reply's objects", named(reply), [(1, "b"), (2, "b")])
        self.want[1]["inst"] = [I(0, (180, 60))]
        self.add("b", I(0, (180 + STEP, 60 + STEP)))
        sc = self.expect(f"{s} 18", '"" 3 *', "a@0×4 · b@0 · b@0")
        self.t.equal(f"{s} 18: each instance's object keeps the parts", [[p["type"] for p in o["parts"]] for o in sc],
                     [["part"], ["part", "modifier"], ["part", "modifier"]])
        self.undo()
        self.want = after_count
        self.expect(f"{s} 19: Set as Individual Objects was one undo step", '"" 2 *', "a@0×4 · b@0×2")
        reply = self.run("object_separate_instances", instances=[[0, 3]])
        self.t.equal(f"{s} 20: the reply's objects", named(reply), [(2, "a")])
        self.want[0]["inst"] = a(3)
        self.add("a", I(0, (60 + 3 * STEP, 60 + 3 * STEP)))
        self.expect(f"{s} 20: where the instance stood", '"" 3 *', "a@0×3 · b@0×2 · a@0")
        self.run("object_select", objects=[0])
        reply = self.run("object_separate_instances")
        self.t.equal(f"{s} 21: the reply's objects", named(reply), [(0, "a"), (3, "a"), (4, "a")])
        self.want[0]["inst"] = a(1)
        self.add("a", I(0, (60 + 2 * STEP, 60 + 2 * STEP)))
        self.add("a", I(0, (60 + STEP, 60 + STEP)))
        self.expect(f"{s} 21: each instance but the first becomes an object, the last instance first", '"" 5 *',
                    "a@0 · b@0×2 · a@0 · a@0 · a@0")
        self.renew(f"{s} 22")
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
            self.inst(0)["at"] = self.inst(2)["at"] = (125, 125)
            self.expect(f"{s} {step}: each goes to the centre of its own plate", *rows)
        self.run("object_add_primitive", plate=1, name="fourth", position=self.corner(1, 125, 60), **CUBE)
        self.add("fourth", I(1, (125, 60)))
        self.expect(f"{s} 10", '"" 0 · "" 3 * · "" 1', "first@1 · second@1 · third@2 · fourth@1")
        reply = self.run("object_center", objects=[0, 3], plate=0)
        self.t.equal(f"{s} 11: the reply's plates", reply.get("plates"), [{"plate": 0, "objects": [0, 3]}])
        self.inst(0).update(plate=0, at=(125, 157.5))
        self.inst(3).update(plate=0, at=(125, 92.5))
        self.expect(f"{s} 11: the two move as one group", '"" 2 · "" 1 * · "" 1',
                    "first@0 · second@1 · third@2 · fourth@0")
        self.run("object_select", objects=[2])
        reply = self.run("object_center")
        self.t.equal(f"{s} 12: the reply's plates", reply.get("plates"), [{"plate": 1, "objects": [2]}])
        self.inst(2).update(plate=1, at=(125, 125))
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
        # Four faces of the tilted cube tie as the best to lie on, and which one the app takes is not fixed, so the
        # cube's height says it lies on a face and its turn is not checked.
        third = sc[2]["instances"][0]
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
        # The bed has no excluded area, so the one free volume is the whole bed, 250 mm each way: the box, grown by
        # 0.01 mm on every side, scales by 250 / 20.02, and its centre goes to the bed's.
        s_fit = 12.488
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
            self.t.near(f"{s} {step}: {name} fills the bed, its centre on the bed's, at",
                        [fit["offset"][0] - cx, fit["offset"][1] - cy], [125, 125], 0.01)
            self.size(f"{s} {step}: {name}'s", fit, [249.75] * 3, 0.02)

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
        reply = self.run("object_arrange", spacing=0, rotate=False)
        settings = reply.get("settings", {})
        self.t.equal(f"{s} 4: the reply's settings, off_plate and plates", (settings.get("spacing"),
                     settings.get("rotate"), reply.get("off_plate"), reply.get("plates")), (0, False, [], 1))
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
        self.add("a", I(0, (60, 60)), I(0, (60 + STEP, 60 + STEP)), I(0, (60 + 2 * STEP, 60 + 2 * STEP)))
        self.add("b", I(0, (180, 60)))
        after1 = self.saved()
        rows = '"" 2 *', "a@0×3 · b@0"
        self.expect(f"{s} 1", *rows)
        reply = self.run("object_set_printable", instances=[[0, 1]], printable=False)
        self.t.equal(f"{s} 2: the reply", ([(i.get("object"), i.get("instance"), i.get("printable"))
                                            for i in reply.get("instances", [])], "objects" in reply),
                     ([(0, 1, False)], False))
        self.inst(0, 1)["printable"] = False
        self.expect(f"{s} 2", *rows)
        reply = self.run("plate_slice", plates=0)
        self.t.equal(f"{s} 3: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        path = os.path.join(self.W, "printable.gcode")
        self.run("project_export", format="gcode", path=path, overwrite=True)
        self.t.equal(f"{s} 3: an instance that is not printable is left out of the G-code: a in 2 copies and b in 1",
                     sorted(gcode_prints(path)), [("a", 2), ("b", 1)])
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
                 I(0, (60 + STEP, 60 + STEP), scale=[1.5] * 3, mirrored=True, printable=False))

        def turned(label, sc):
            self.size(f"{label}: kept's instance 0, turned 30 degrees,", sc[0]["instances"][0],
                      [20 * (math.cos(math.radians(30)) + math.sin(math.radians(30)))] * 2 + [20])

        turned(f"{s} 1", self.expect(f"{s} 1: Mirror of the selected instance mirrors the object's other one",
                                     '"" 1 * · "" 0', "kept@0×2"))
        self.run("object_add_primitive", plate=1, name="lifted", position=self.corner(1, 60, 60), **CUBE)
        self.run("object_set_auto_drop", objects=[1], auto_drop=False)
        self.run("object_transform", object=1, translate=[0, 0, 15])
        self.add("lifted", I(1, (60, 60), z=15), auto_drop=False)
        turned(f"{s} 2", self.expect(f"{s} 2", '"" 1 · "" 1 *', "kept@0×2 · lifted@1"))

        self.run("object_add_primitive", plate=0, shape="cube", size=[30, 30, 10], name="base",
                 position=self.corner(0, 180, 60))
        self.run("part_add", object=2, type="negative", shape="cylinder", size=[6, 3],
                 position=self.corner(0, 180, 60, 1.5))
        self.run("part_add", object=2, type="modifier", shape="cube", size=[10, 10, 10],
                 position=self.corner(0, 180, 60, 5))
        self.run("part_add", object=2, type="precise_seam", shape="cube", size=[2, 2, 2],
                 position=self.corner(0, 195, 60, 5))
        self.run("object_rename", object=2, part=2, name="infill zone")
        self.run("part_change_type", parts=[[2, 3]], type="precise_seam", subtype="left")
        self.run("object_add_primitive", plate=0, name="other", position=self.corner(0, 180, 180), **CUBE)
        self.run("object_order", plate=0, order=[0, 3, 2])
        self.add("other", I(0, (180, 180)))
        self.add("base", I(0, (180, 60)))
        rows = "kept@0×2 · lifted@1 · other@0 · base@0"

        def part(name, type_, at):
            return {"name": name, "type": type_, "at": at, "scale": [1, 1, 1], "rot": [0, 0, 0], "mirrored": False}

        def state(label, plates_row):
            sc = self.expect(label, plates_row, rows)
            turned(label, sc)
            self.parts_are(label, sc, 2, [part("other", "part", (180, 180, 10))])
            self.parts_are(label, sc, 3, [part("base", "part", (180, 60, 5)),
                                          part("Generic-Cylinder", "negative", (180, 60, 1.5)),
                                          part("infill zone", "modifier", (180, 60, 5)),
                                          part("Generic-Cube", "precise_seam", (195, 60, 5))])
            self.t.equal(f"{label}: the seam's subtype", sc[3]["parts"][3].get("subtype"), "left")

        state(f"{s} 3", '"" 3 * · "" 1')
        # A plate takes a plate type of its own only on a printer that offers several; an unsaved printer edit,
        # which the project file keeps.
        self.run("settings_set", type="printer", values={"support_multi_bed_types": "1"})
        self.run("project_set_settings", values={"curr_bed_type": "Cool Plate"})
        self.run("plate_rename", plate=0, name="Left")
        self.run("plate_set_settings", plate=1, values={"curr_bed_type": "Textured PEI Plate"})
        self.run("plate_lock", plate=1, locked=True)
        own = [S(), S(curr_bed_type="Textured PEI Plate", overrides=["curr_bed_type"])]
        state(f"{s} 4", '"Left" 3 · "" 1 * L')
        self.t.equal(f"{s} 4: the plates' settings", plate_settings(self.c), own)

        path = os.path.join(self.W, "keeps.3mf")
        self.run("project_save", path=path, overwrite=True)
        # The new project drops the printer's edit, so that the open asks nothing about it.
        self.run("project_new", discard_changes=True, preset_changes="discard")
        self.empty(f"{s} 5: after project_new")
        self.t.equal(f"{s} 5: after project_new the printer has no unsaved edit", unsaved(self.c, "printer"), [])
        self.run("project_open", path=path)
        wait(self.c, "idle")
        state(f"{s} 5: the project file keeps them; opening it makes plate 0 current", '"Left" 3 * · "" 1 L')
        self.t.equal(f"{s} 5: the project file keeps a plate's own settings", plate_settings(self.c), own)
        self.t.equal(f"{s} 5: the project brings back the printer's support_multi_bed_types",
                     unsaved(self.c, "printer"), ["support_multi_bed_types"])

    def scenarios(self):
        return [("adding a shape", self.adding), ("moving an instance", self.moving),
                ("moving a part", self.moving_a_part), ("the number of instances", self.instances),
                ("placing objects", self.placing), ("arranging and filling the bed", self.arranging),
                ("cloning", self.cloning), ("leaving instances out of the print", self.printable)]


class Structure(Part):
    """What objects are made of and how the list orders them: after every step the objects' names in index order,
    and the parts' types where a row writes them."""

    def __init__(self, client, checks, folder):
        super().__init__(client, checks, folder)
        for name, triangles in (("cube20.stl", cube_triangles(20)), ("cube30.stl", cube_triangles(30)),
                                ("holed.stl", cube_triangles(20, holed=True)),
                                ("top4.stl", cube_triangles(20, top4=True))):
            write_stl(self.file(name), triangles)

    # ---- Calls and reads ----

    def file(self, name):
        return os.path.join(self.W, name)

    def notices(self):
        return [n.get("text", "") for n in self.c.call("app_state").get("notifications", [])]

    def dialogs(self):
        return [d.get("title") for d in self.c.call("app_state").get("dialogs", [])]

    def gizmo(self):
        return self.c.call("app_state").get("gizmo")

    def objects_are(self, label, *rows, plate_rows=None):
        """The objects' names in index order compared with the rows whole, and the parts' types of every object a row
        writes them for ("Assembly: part, modifier"); then the plates, '"" n *' unless `plate_rows` gives them.
        Returns scene()."""
        objects_ = scene(self.c)
        self.t.equal(f"{label}: the objects", names(objects_), [row.split(": ")[0] for row in rows])
        for i, row in enumerate(rows):
            if ": " in row:
                got = types(objects_[i]) if i < len(objects_) else None
                self.t.equal(f"{label}: object {i}'s parts", got, row.split(": ", 1)[1].split(", "))
        self.plates_are(label, *(plate_rows or [f'"" {len(rows)} *']))
        return objects_

    def start(self, name):
        self.t.equal(f"{name}: the start: the presets", start(self.c), START)
        wait(self.c, "idle")
        self.empty(f"{name}: the start")
        self.c0 = corner(self.c, 0)

    def renew(self, name):
        """A project_new within a scenario, and the check of the empty project it leaves."""
        self.run("project_new", discard_changes=True)
        wait(self.c, "idle")
        self.empty(f"{name}: the start")
        self.c0 = corner(self.c, 0)

    def empty(self, label):
        self.plates_are(label, '"" 0 *')
        self.t.equal(f"{label}: no objects", scene(self.c), [])

    def at(self, x, y, z=None):
        """A place given from plate 0's corner, in the app's coordinates."""
        return [self.c0[0] + x, self.c0[1] + y] + ([] if z is None else [z])

    def placed(self, label, obj, x, y):
        """The object's instance 0 at +(x, y) from plate 0's corner."""
        self.t.near(f"{label}: {obj['name']} at +({x}, {y})", obj["instances"][0]["offset"][:2], self.at(x, y), 0.001)

    def part_placed(self, label, obj, part, x, y, z, tolerance=0.001):
        self.t.near(f"{label}: part {part} of {obj['name']} at +({x}, {y}, {z})", obj["parts"][part]["position"],
                    self.at(x, y, z), tolerance)

    def rings(self, label, boxes):
        """Two boxes, each a ring's, standing side by side: their centres 58 mm apart along x and level along y."""
        for i, box in enumerate(boxes):
            self.t.near(f"{label}: ring {i + 1}'s box", box["size"], RING, 0.01)
        a, b = sorted((centre(box) for box in boxes), key=lambda c: c[0])
        self.t.near(f"{label}: the rings side by side", [b[0] - a[0], b[1] - a[1]], [58, 0], 0.01)

    # ---- Scenarios ----

    def renaming(self):
        s = "renaming and deleting"
        self.start(s)
        for x in (40, 100, 160):
            self.run("object_add_primitive", position=self.at(x, 60, 0), **CUBE)
            self.run("object_select", none=True)
        objects_ = self.objects_are(f"{s} 1", "Cube", "Cube", "Cube")
        for obj, x in zip(objects_, (40, 100, 160)):
            self.placed(f"{s} 1", obj, x, 60)
        reply = self.run("object_rename", object=1, name="middle")
        self.t.equal(f"{s} 2: the reply", names_of(reply.get("objects")), [{"index": 1, "name": "middle"}])
        self.objects_are(f"{s} 2", "Cube", "middle", "Cube")
        reply = self.run("object_delete", objects=[0])
        self.t.equal(f"{s} 3: the reply's objects", names_of(reply.get("objects")), [{"index": 0, "name": "Cube"}])
        self.t.equal(f"{s} 3: the reply's object_count", reply.get("object_count"), 2)
        objects_ = self.objects_are(f"{s} 3", "middle", "Cube")
        self.placed(f"{s} 3", objects_[0], 100, 60)
        self.placed(f"{s} 3", objects_[1], 160, 60)
        self.undo()
        self.objects_are(f"{s} 4: the delete was one undo step", "Cube", "middle", "Cube")
        self.redo()
        self.objects_are(f"{s} 5", "middle", "Cube")
        self.run("object_select", objects=[0])
        reply = self.run("object_delete")
        self.t.equal(f"{s} 6: the reply's objects", names_of(reply.get("objects")), [{"index": 0, "name": "middle"}])
        self.t.equal(f"{s} 6: the reply's object_count", reply.get("object_count"), 1)
        objects_ = self.objects_are(f"{s} 6", "Cube")
        self.placed(f"{s} 6", objects_[0], 160, 60)
        self.t.refused(f"{s} 7: a name with a slash is refused", lambda: self.c.call("object_rename", object=0,
                                                                                       name="a/b"), code="invalid_name")
        self.t.refused(f"{s} 7: an empty name is refused", lambda: self.c.call("object_rename", object=0, name=""),
                       code="empty_name")
        self.objects_are(f"{s} 7", "Cube")
        self.run("object_add_primitive", name="layered", position=self.at(60, 160, 0), **CUBE)
        self.run("object_select", objects=[1])
        reply = self.run("object_set_height_ranges", ranges=[{"min_z": 0, "max_z": 5}])
        self.t.equal(f"{s} 8: the reply's object", reply.get("object"), 1)
        self.t.equal(f"{s} 8: the reply's objects", names_of(reply.get("objects")), [{"index": 1, "name": "layered"}])
        self.t.equal(f"{s} 8: the reply's range", [{k: r.get(k) for k in ("min_z", "max_z")}
                                                  for r in reply.get("layer_config_ranges", [])],
                     [{"min_z": 0, "max_z": 5}])
        objects_ = self.objects_are(f"{s} 8", "Cube", "layered")
        self.t.equal(f"{s} 8: layered has the range", [{k: r.get(k) for k in ("min_z", "max_z")}
                                                      for r in objects_[1].get("layer_config_ranges", [])],
                     [{"min_z": 0, "max_z": 5}])
        self.t.check(f"{s} 8: Cube has no range", "layer_config_ranges" not in objects_[0], objects_[0])
        reply = self.run("object_set_variable_layer_height", action="adaptive")
        self.t.equal(f"{s} 9: the reply's object", reply.get("object"), 1)
        self.t.equal(f"{s} 9: the reply's objects", names_of(reply.get("objects")),
                     [{"index": 1, "name": "layered"}])
        self.t.check(f"{s} 9: the reply has a layer_height_profile", "layer_height_profile" in reply, reply)
        objects_ = self.objects_are(f"{s} 9", "Cube", "layered")
        points = (objects_[1].get("layer_height_profile") or {}).get("points")
        self.t.check(f"{s} 9: layered has a profile with points", isinstance(points, int) and points > 0,
                     objects_[1].get("layer_height_profile"))
        self.t.check(f"{s} 9: Cube has no profile", "layer_height_profile" not in objects_[0], objects_[0])

    def naming_by_id(self):
        s = "naming by id"
        self.start(s)

        def three_cubes():
            for name, x in (("a", 40), ("b", 100), ("c", 160)):
                self.run("object_add_primitive", name=name, position=self.at(x, 60, 0), **CUBE)
                self.run("object_select", none=True)

        def short_rows(name):
            """The rows object_list gives the named object without detail, one per plate it has instances on."""
            listing = self.c.call("object_list", limit=500)
            groups = [p["objects"] for p in listing["plates"]] + [listing.get("outside", [])]
            return [row for rows in groups for row in rows if row["name"] == name]

        def ids_beside(label, obj):
            """The short rows of `obj` give each instance's id at the place its index has in `instances`."""
            by_index = {inst["index"]: inst["id"] for inst in obj["instances"]}
            for row in short_rows(obj["name"]):
                self.t.equal(f"{label}: {obj['name']}'s instance_ids beside instances {row.get('instances')}",
                             row.get("instance_ids"), [by_index.get(i) for i in row.get("instances", [])])

        def instance_offsets(obj):
            return [inst["offset"][:2] for inst in obj["instances"]]

        def picked(items, keys):
            return [{k: item.get(k) for k in keys} for item in items or []]

        three_cubes()
        objects_ = self.objects_are(f"{s} 1", "a", "b", "c")
        A, B, C = (o["id"] for o in objects_)
        self.t.check(f"{s} 1: the ids differ", len({A, B, C}) == 3, (A, B, C))
        self.t.check(f"{s} 1: no id is an index the project has", min(A, B, C) > 2, (A, B, C))
        c_instance = objects_[2]["instances"][0]["id"]
        for obj in objects_:
            rows = short_rows(obj["name"])
            self.t.equal(f"{s} 1: {obj['name']}'s short rows", [(r.get("instances"), r.get("instance_ids")) for r in rows],
                         [([0], [obj["instances"][0]["id"]])])
        self.run("replay_export", clear=True)

        reply = self.run("object_rename", object_id=B, name="bee")
        self.t.equal(f"{s} 2: the reply's objects", picked(reply.get("objects"), ("index", "object_id", "name")),
                     [{"index": 1, "object_id": B, "name": "bee"}])
        self.objects_are(f"{s} 2", "a", "bee", "c")

        self.run("object_delete", object_ids=[A])
        objects_ = self.objects_are(f"{s} 3", "bee", "c")
        self.placed(f"{s} 3", objects_[0], 100, 60)
        self.placed(f"{s} 3", objects_[1], 160, 60)
        self.t.equal(f"{s} 3: the ids stay with their objects", [o["id"] for o in objects_], [B, C])

        self.run("object_transform", object_id=C, translate=[0, 20, 0])
        objects_ = self.objects_are(f"{s} 4", "bee", "c")
        self.placed(f"{s} 4", objects_[0], 100, 60)
        self.placed(f"{s} 4", objects_[1], 160, 80)

        def places_as_after_4(label):
            objects_ = self.objects_are(label, "bee", "c")
            self.placed(label, objects_[0], 100, 60)
            self.placed(label, objects_[1], 160, 80)

        self.t.refused(f"{s} 5: an index and an id of different objects are refused",
                       lambda: self.c.call("object_transform", object=1, object_id=B, translate=[0, 20, 0]),
                       words='The object at this index is now "c"')
        places_as_after_4(f"{s} 5")
        self.t.refused(f"{s} 6: an id no object has is refused",
                       lambda: self.c.call("object_transform", object_id=A, translate=[0, 20, 0]),
                       code="invalid_argument", words=f"No object has id {A}")
        places_as_after_4(f"{s} 6")
        self.t.refused(f"{s} 7: an id given as an index is named for what it is",
                       lambda: self.c.call("object_transform", object=C, translate=[0, 20, 0]),
                       code="invalid_argument", words=f"{C} is the id of object 1")
        places_as_after_4(f"{s} 7")

        self.run("settings_set", target="object", object_id=C, values={"wall_loops": 5})
        objects_ = self.objects_are(f"{s} 8", "bee", "c")
        self.t.check(f"{s} 8: c overrides wall_loops", "wall_loops" in objects_[1].get("overrides", []), objects_[1])
        self.t.check(f"{s} 8: bee does not", "wall_loops" not in objects_[0].get("overrides", []), objects_[0])

        self.run("object_set_instances", object_id=C, count=3)
        objects_ = self.objects_are(f"{s} 9", "bee", "c")
        c = objects_[1]
        self.t.equal(f"{s} 9: c's instances", len(c["instances"]), 3)
        I0, I1, I2 = (inst["id"] for inst in c["instances"])
        self.t.equal(f"{s} 9: instance 0 keeps its id", I0, c_instance)
        self.t.check(f"{s} 9: the instances' ids differ", len({I0, I1, I2}) == 3, (I0, I1, I2))
        ids_beside(f"{s} 9", c)
        before = instance_offsets(c)

        self.run("object_transform", instance_id=I2, translate=[0, 20, 0])
        objects_ = self.objects_are(f"{s} 10", "bee", "c")
        moved = instance_offsets(objects_[1])
        self.t.near(f"{s} 10: instance 2 moved 20 mm along y", moved[2], [before[2][0], before[2][1] + 20], 0.001)
        self.t.near(f"{s} 10: instance 0 stays", moved[0], before[0], 0.001)
        self.t.near(f"{s} 10: instance 1 stays", moved[1], before[1], 0.001)

        reply = self.run("object_set_printable", instance_ids=[I1], printable=False)
        self.t.equal(f"{s} 11: the reply's instances", picked(reply.get("instances"), ("object", "instance", "instance_id")),
                     [{"object": 1, "instance": 1, "instance_id": I1}])
        objects_ = self.objects_are(f"{s} 11", "bee", "c")
        self.t.equal(f"{s} 11: c's printable", [inst["printable"] for inst in objects_[1]["instances"]],
                     [True, False, True])

        self.run("object_delete", instance_ids=[I0])
        objects_ = self.objects_are(f"{s} 12", "bee", "c")
        self.t.equal(f"{s} 12: c's instances keep their ids", [inst["id"] for inst in objects_[1]["instances"]], [I1, I2])
        self.t.equal(f"{s} 12: c's printable", [inst["printable"] for inst in objects_[1]["instances"]], [False, True])

        reply = self.run("object_set_printable", instance_ids=[I1], printable=True)
        self.t.equal(f"{s} 13: the reply's instances", picked(reply.get("instances"), ("object", "instance", "instance_id")),
                     [{"object": 1, "instance": 0, "instance_id": I1}])
        objects_ = self.objects_are(f"{s} 13", "bee", "c")
        self.t.equal(f"{s} 13: c's printable", [inst["printable"] for inst in objects_[1]["instances"]], [True, True])
        after_13 = instance_offsets(objects_[1])

        self.run("part_add", object_id=B, type="modifier", shape="cube")
        objects_ = self.objects_are(f"{s} 14", "bee: part, modifier", "c")
        P = objects_[0]["parts"][1]["id"]

        reply = self.run("part_change_type", part_ids=[P], type="negative")
        self.t.equal(f"{s} 15: the reply's parts", picked(reply.get("parts"), ("object", "part", "part_id", "type")),
                     [{"object": 0, "part": 1, "part_id": P, "type": "negative"}])
        self.objects_are(f"{s} 15", "bee: part, negative", "c")

        self.run("object_rename", part_id=P, name="hole")
        objects_ = self.objects_are(f"{s} 16", "bee: part, negative", "c")
        self.t.equal(f"{s} 16: bee's part 1 named hole", objects_[0]["parts"][1]["name"], "hole")

        reply = self.run("object_order", order_ids=[C, B])
        self.t.equal(f"{s} 17: the reply's order", picked(reply.get("order"), ("name", "object_id")),
                     [{"name": "c", "object_id": C}, {"name": "bee", "object_id": B}])
        self.objects_are(f"{s} 17", "c", "bee")

        self.t.refused(f"{s} 18: Fill bed of an id no object has is refused",
                       lambda: self.c.call("object_arrange", fill_object_id=A),
                       code="invalid_argument", words=f"No object has id {A}")
        objects_ = self.objects_are(f"{s} 18", "c", "bee")
        self.t.near(f"{s} 18: c's instances where they were", [v for xy in instance_offsets(objects_[0]) for v in xy],
                    [v for xy in after_13 for v in xy], 0.001)
        self.placed(f"{s} 18", objects_[1], 100, 60)

        replay = self.run("replay_export").get("replay", {})
        targets = ("object", "objects", "instance", "instances", "part", "parts", "order")
        steps = [step for step in replay.get("steps", []) if step.get("tool") != "app_wait"]
        self.t.equal(f"{s} 19: the replay's steps and their targets",
                     [(step.get("tool"), {k: v for k, v in step.get("args", {}).items() if k in targets}) for step in steps],
                     [("object_rename", {"object": 1}), ("object_delete", {"objects": [0]}),
                      ("object_transform", {"object": 1}), ("settings_set", {"object": 1}),
                      ("object_set_instances", {"object": 1}), ("object_transform", {"object": 1, "instance": 2}),
                      ("object_set_printable", {"instances": [[1, 1]]}), ("object_delete", {"instances": [[1, 0]]}),
                      ("object_set_printable", {"instances": [[1, 0]]}), ("part_add", {"object": 0}),
                      ("part_change_type", {"parts": [[0, 1]]}), ("object_rename", {"object": 0, "part": 1}),
                      ("object_order", {"order": [1, 0]})])
        with_ids = [(step.get("tool"), k) for step in steps for k in step.get("args", {}) if k.endswith(("_id", "_ids"))]
        self.t.equal(f"{s} 19: no step names an id", with_ids, [])

        self.renew(s)
        three_cubes()
        fresh = [o["id"] for o in self.objects_are(f"{s} 20: the cubes again", "a", "b", "c")]
        self.t.check(f"{s} 20: the new cubes' ids are none of A, B and C", not {A, B, C} & set(fresh), fresh)
        self.run("replay_run", replay=replay)
        objects_ = self.objects_are(f"{s} 20", "c", "bee: part, negative")
        self.t.check(f"{s} 20: c overrides wall_loops", "wall_loops" in objects_[0].get("overrides", []), objects_[0])
        self.t.equal(f"{s} 20: c's printable", [inst["printable"] for inst in objects_[0]["instances"]], [True, True])
        self.t.equal(f"{s} 20: bee's part 1 named hole", objects_[1]["parts"][1]["name"], "hole")

    def splitting(self):
        s = "splitting and assembling"
        self.start(s)
        name = "two_hollow_squares.obj"
        reply = self.run("project_import", paths=[RINGS])
        self.t.equal(f"{s} 1: the import's added", reply.get("added"), 1)
        objects_ = self.objects_are(f"{s} 1", f"{name}: part")
        self.t.near(f"{s} 1: its box", objects_[0]["bbox"]["size"], RINGS_SIZE, 0.01)
        reply = self.run("object_split", object=0, into="objects")
        self.t.equal(f"{s} 2: the reply's objects", [o.get("index") for o in reply.get("objects", [])], [0, 1])
        self.t.equal(f"{s} 2: the reply's object_count", reply.get("object_count"), 2)
        objects_ = self.objects_are(f"{s} 2", f"{name}_1: part", f"{name}_2: part")
        self.rings(f"{s} 2", [o["bbox"] for o in objects_])
        self.t.equal(f"{s} 2: the pieces are selected", selection(self.c)["objects"], [0, 1])
        self.t.equal(f"{s} 3: the import's added", self.run("project_import", paths=[RINGS]).get("added"), 1)
        reply = self.run("object_split", object=2, into="parts")
        self.t.equal(f"{s} 3: the reply's parts", [{k: p.get(k) for k in ("object", "part")}
                                                  for p in reply.get("parts", [])],
                     [{"object": 2, "part": 0}, {"object": 2, "part": 1}])
        self.t.equal(f"{s} 3: the reply's object_count", reply.get("object_count"), 3)
        objects_ = self.objects_are(f"{s} 3", f"{name}_1: part", f"{name}_2: part", f"{name}: part, part")
        self.t.equal(f"{s} 3: the parts' names", [p["name"] for p in objects_[2]["parts"]], [f"{name}_1", f"{name}_2"])
        self.rings(f"{s} 3: the parts", [p["bbox"] for p in objects_[2]["parts"]])
        after4 = (f"{name}: part, part", "Assembly: part, part")
        reply = self.run("object_assemble", objects=[0, 1])
        self.t.equal(f"{s} 4: the reply's object and name", (reply.get("object"), reply.get("name")), (1, "Assembly"))
        objects_ = self.objects_are(f"{s} 4", *after4)
        self.t.near(f"{s} 4: the pieces keep their places: Assembly's box", objects_[1]["bbox"]["size"], RINGS_SIZE,
                    0.01)
        reply = self.run("part_change_type", parts=[[1, 0]], type="modifier")
        self.t.equal(f"{s} 5: the reply's parts", [{k: p.get(k) for k in ("object", "part", "type")}
                                                  for p in reply.get("parts", [])],
                     [{"object": 1, "part": 1, "type": "modifier"}])
        objects_ = self.objects_are(f"{s} 5", f"{name}: part, part", "Assembly: part, modifier")
        self.t.near(f"{s} 5: a modifier is left out of Assembly's box", objects_[1]["bbox"]["size"], RING, 0.01)
        self.undo()
        objects_ = self.objects_are(f"{s} 6: the change was one undo step", *after4)
        self.t.near(f"{s} 6: Assembly's box", objects_[1]["bbox"]["size"][0], RINGS_SIZE[0], 0.01)
        self.run("object_select", objects=[0])
        reply = self.run("object_split", into="objects")
        self.t.equal(f"{s} 7: the reply's objects", [o.get("index") for o in reply.get("objects", [])], [1, 2])
        self.t.equal(f"{s} 7: the reply's object_count", reply.get("object_count"), 3)
        objects_ = self.objects_are(f"{s} 7: no target splits the selected object, each piece named after its part",
                                    "Assembly: part, part", f"{name}_1: part", f"{name}_2: part")
        self.rings(f"{s} 7", [o["bbox"] for o in objects_[1:]])
        self.run("object_select", objects=[1, 2])
        reply = self.run("object_assemble")
        self.t.equal(f"{s} 8: the reply's object and name", (reply.get("object"), reply.get("name")), (1, "Assembly"))
        self.objects_are(f"{s} 8: no target assembles the selected objects", "Assembly: part, part",
                         "Assembly: part, part")

    def parts(self):
        s = "parts"
        self.start(s)
        self.run("object_add_primitive", shape="cube", size=[40, 40, 10], name="block", position=self.at(100, 100, 0))
        reply = self.run("part_add", object=0, type="negative", shape="cylinder", size=[6.2, 3.2],
                         position=self.at(100, 100, 1.6))
        self.t.equal(f"{s} 1: the reply's part and part_count", (reply.get("part"), reply.get("part_count")), (1, 2))
        block = self.objects_are(f"{s} 1", "block: part, negative")[0]
        negative = block["parts"][1]
        self.part_placed(f"{s} 1", block, 1, 100, 100, 1.6)
        self.t.near(f"{s} 1: the negative part's bottom", negative["bbox"]["min"][2], 0, 0.01)
        self.t.near(f"{s} 1: the negative part's height", negative["bbox"]["size"][2], 3.2, 0.01)
        self.t.check(f"{s} 1: the negative part's width is 6.1 to 6.21", 6.1 <= negative["bbox"]["size"][0] <= 6.21,
                     negative["bbox"])
        self.t.near(f"{s} 1: a negative part is left out of block's box", block["bbox"]["size"][2], 10, 0.001)
        self.undo()
        self.objects_are(f"{s} 2: adding a part is one undo step", "block: part")
        self.redo()
        self.objects_are(f"{s} 3", "block: part, negative")
        reply = self.run("part_add", object=0, type="modifier", path=DRC)
        self.t.equal(f"{s} 4: the reply's part and part_count", (reply.get("part"), reply.get("part_count")), (2, 3))
        block = self.objects_are(f"{s} 4", "block: part, negative, modifier")[0]
        self.t.equal(f"{s} 4: the modifier's name", block["parts"][2]["name"], "OrcaCube_v2.drc")
        reply = self.run("part_add", object=0, type="part", shape="cube", size=[10, 10, 20],
                         position=self.at(125, 100, 10))
        self.t.equal(f"{s} 5: parts are listed by type: the reply's part", reply.get("part"), 1)
        block = self.objects_are(f"{s} 5", "block: part, part, negative, modifier")[0]
        cube = block["parts"][1]
        self.t.equal(f"{s} 5: the new part's name and filament", (cube["name"], cube.get("filament")),
                     ("Generic-Cube", 1))
        self.t.near(f"{s} 5: the new part's height", cube["bbox"]["size"][2], 20, 0.01)
        self.t.near(f"{s} 5: block's height", block["bbox"]["size"][2], 20, 0.001)
        self.run("part_add", object=0, type="support_blocker", shape="sphere", position=self.at(110, 110, 10))
        block = self.objects_are(f"{s} 6", "block: part, part, negative, modifier, support_blocker")[0]
        self.part_placed(f"{s} 6: the blocker", block, 4, 110, 110, 10)
        after7 = "block: part, part, negative, modifier, support_blocker, precise_seam"
        self.run("part_add", object=0, type="precise_seam", shape="cube", size=[2, 2, 2])
        block = self.objects_are(f"{s} 7", after7)[0]
        self.t.equal(f"{s} 7: the seam's subtype", block["parts"][5].get("subtype"), "center")
        refusals = [("a shape and a path", {"type": "modifier", "shape": "cube", "path": DRC}, "Give shape or path"),
                    ("a cylinder of three sizes", {"type": "modifier", "shape": "cylinder", "size": [1, 2, 3]},
                     "A cylinder's size is [Diameter, Height]"),
                    ("an SVG file", {"type": "part", "path": STAR}, "SVG parts come from object_add_svg"),
                    ("an image", {"type": "modifier", "path": HEIGHTMAP}, "The file could not be loaded")]
        for case, args, words in refusals:
            self.t.refused(f"{s} 8: {case} is refused", lambda: self.c.call("part_add", object=0, **args), words=words)
            self.objects_are(f"{s} 8: after {case}", after7)
        self.run("object_set_instances", object=0, count=2)
        self.t.refused(f"{s} 9: a part on an object of two instances is refused",
                       lambda: self.c.call("part_add", object=0, type="modifier", shape="cube"),
                       words="object 0 has 2 instances")
        self.objects_are(f"{s} 9", after7)

        self.renew(f"{s} 10")
        self.run("object_add_primitive", name="scaled", **CUBE)
        self.run("object_transform", object=0, scale=1.5)
        reply = self.run("part_add", object=0, type="modifier", shape="cube")
        self.t.equal(f"{s} 10: the reply's part", reply.get("part"), 1)
        scaled = self.objects_are(f"{s} 10", "scaled: part, modifier")[0]
        self.t.near(f"{s} 10: adding a shape folds the instance's scale into the parts: the instance's scale",
                    scaled["instances"][0]["scale"], [1, 1, 1], 0.001)
        self.t.near(f"{s} 10: part 0's scale", scaled["parts"][0]["scale"], [1.5, 1.5, 1.5], 0.001)
        self.t.near(f"{s} 10: the object's size", scaled["bbox"]["size"], [30, 30, 30], 0.001)

        self.renew(f"{s} 11")
        self.run("object_add_primitive", shape="cube", size=[30, 30, 10], name="plain")
        self.run("part_add", object=0, type="modifier", shape="cube", size=[10, 10, 10])
        plain = self.objects_are(f"{s} 11", "plain: part, modifier")[0]
        self.t.equal(f"{s} 11: the parts' names", [p["name"] for p in plain["parts"]], ["plain", "Generic-Cube"])
        self.run("object_select", parts=[[0, 1]])
        reply = self.run("part_change_type", type="negative")
        self.t.equal(f"{s} 12: no target changes the selected part: the reply's parts",
                     [{k: p.get(k) for k in ("object", "part", "type")} for p in reply.get("parts", [])],
                     [{"object": 0, "part": 1, "type": "negative"}])
        self.objects_are(f"{s} 12", "plain: part, negative")
        self.run("part_change_type", parts=[[0, 1]], type="support_blocker")
        self.objects_are(f"{s} 13", "plain: part, support_blocker")
        self.undo()
        self.objects_are(f"{s} 14: each change is one undo step", "plain: part, negative")
        self.run("part_change_type", parts=[[0, 1]], type="precise_seam")
        plain = self.objects_are(f"{s} 15", "plain: part, precise_seam")[0]
        self.t.equal(f"{s} 15: the seam's subtype", plain["parts"][1].get("subtype"), "center")
        self.run("part_change_type", parts=[[0, 1]], type="part")
        self.objects_are(f"{s} 16: a seam made a part again is listed after the first part", "plain: part, part")
        self.run("part_change_type", parts=[[0, 1]], type="modifier")
        self.objects_are(f"{s} 16", "plain: part, modifier")
        self.t.refused(f"{s} 17: the last solid part's type is refused",
                       lambda: self.c.call("part_change_type", parts=[[0, 0]], type="modifier"),
                       code="last_solid_part")
        self.objects_are(f"{s} 17", "plain: part, modifier")
        self.run("object_select", objects=[0])
        self.t.refused(f"{s} 18: an object's row is not a part",
                       lambda: self.c.call("part_change_type", type="part"), code="wrong_selection")
        self.objects_are(f"{s} 18", "plain: part, modifier")
        self.run("object_select", objects=[0])
        self.run("object_add_svg", path=STAR)
        wait(self.c, "idle")
        self.run("object_select", none=True)
        plain = self.objects_are(f"{s} 19", "plain: part, part, modifier")[0]
        svg_path = (plain["parts"][1].get("svg") or {}).get("path", "")
        self.t.check(f"{s} 19: part 1's svg.path ends in star.svg", svg_path.endswith("star.svg"), svg_path)
        self.t.equal(f"{s} 19: no gizmo open afterwards", self.gizmo(), None)
        self.t.refused(f"{s} 20: an SVG part cannot be a support enforcer",
                       lambda: self.c.call("part_change_type", parts=[[0, 1]], type="support_enforcer"),
                       code="text_part")
        self.objects_are(f"{s} 20", "plain: part, part, modifier")
        self.run("object_select", objects=[0])
        self.run("object_rename", name="base")
        self.run("object_select", parts=[[0, 2]])
        self.run("object_rename", name="infill zone")
        base = self.objects_are(f"{s} 21: no target renames the selected object, then the selected part",
                                "base: part, part, modifier")[0]
        self.t.equal(f"{s} 21: part 0 keeps its name, part 2 is renamed",
                     (base["parts"][0]["name"], base["parts"][2]["name"]), ("plain", "infill zone"))
        self.t.refused(f"{s} 22: a name with a slash is refused", lambda: self.c.call("object_rename", name="a/b"),
                       code="invalid_name")
        base = self.objects_are(f"{s} 22", "base: part, part, modifier")[0]
        self.t.equal(f"{s} 22: part 2's name", base["parts"][2]["name"], "infill zone")

    def ordering(self):
        s = "ordering"
        self.start(s)
        places = {"tall": 30, "short": 128, "mid": 226}
        for name, height in (("tall", 30), ("short", 10), ("mid", 20)):
            self.run("object_add_primitive", shape="cube", size=[20, 20, height], name=name,
                     position=self.at(places[name], 60, 0))
            self.run("object_select", none=True)
        one = ['"" 3 *']
        two = ['"" 3', '"" 0 *']
        first = ['"" 3 *', '"" 0']

        def state(label, order, plate_rows):
            objects_ = self.objects_are(label, *order, plate_rows=plate_rows)
            for obj in objects_:
                if obj["name"] in places:
                    self.placed(label, obj, places[obj["name"]], 60)
            return objects_

        def entries(reply):
            """The reply's order with the fields this scenario fixes: each entry's name, index and plate."""
            return [{k: e.get(k) for k in ("name", "object", "plate")} for e in reply.get("order") or []]

        def order_is(label, reply, order, descending=None):
            self.t.equal(f"{label}: the reply's order", entries(reply),
                         [{"name": n, "object": i, "plate": 0} for i, n in enumerate(order)])
            if descending is None:
                self.t.check(f"{label}: the reply has no descending", "descending" not in reply, reply)
            else:
                self.t.equal(f"{label}: the reply's descending", reply.get("descending"), descending)

        objects_ = state(f"{s} 1", ["tall", "short", "mid"], one)
        self.t.near(f"{s} 1: the heights", [o["bbox"]["size"][2] for o in objects_], [30, 10, 20], 0.001)
        ascending = ["short", "mid", "tall"]
        reply = self.run("object_order", by="height", plate=0)
        order_is(f"{s} 2", reply, ascending, False)
        state(f"{s} 2", ascending, one)
        reply = self.run("object_order", by="height", plate=0)
        order_is(f"{s} 3: a call that gives plate sorts ascending", reply, ascending, False)
        state(f"{s} 3", ascending, one)
        reply = self.run("object_order", by="height", descending=True)
        order_is(f"{s} 4", reply, ["tall", "mid", "short"], True)
        state(f"{s} 4", ["tall", "mid", "short"], one)
        reply = self.run("object_order", by="height")
        order_is(f"{s} 5: from a descending order it sorts ascending", reply, ascending, False)
        state(f"{s} 5", ascending, one)
        reply = self.run("object_order", by="height")
        order_is(f"{s} 6: an order they already have is reversed", reply, ["tall", "mid", "short"], True)
        state(f"{s} 6", ["tall", "mid", "short"], one)
        self.run("plate_add")
        self.run("plate_select", plate=1)
        self.t.refused(f"{s} 7: the current plate is empty", lambda: self.c.call("object_order", by="size"),
                       code="no_objects_to_order")
        state(f"{s} 7", ["tall", "mid", "short"], two)
        self.t.refused(f"{s} 8: all must be a boolean", lambda: self.c.call("object_order", by="size", all="every"),
                       words="all must be of type boolean")
        state(f"{s} 8", ["tall", "mid", "short"], two)
        reply = self.run("object_order", by="size", all=True)
        order_is(f"{s} 9: every plate's objects, by volume", reply, ascending, False)
        state(f"{s} 9", ascending, two)
        reply = self.run("object_order", by="name", all=True)
        order_is(f"{s} 10: by name", reply, ["mid", "short", "tall"], False)
        state(f"{s} 10", ["mid", "short", "tall"], two)
        self.run("plate_set_settings", plate=0, values={"print_sequence": "by object"})
        state(f"{s} 11: plate_set_settings makes the plate it sets current", ["mid", "short", "tall"], first)
        self.t.equal(f"{s} 11: plate 0's print_sequence",
                     self.c.call("plate_list", limit=500)["items"][0]["print_sequence"], "by object")
        given = ["tall", "mid", "short"]
        reply = self.run("object_order", plate=0, order=[2, 0, 1])
        order_is(f"{s} 12", reply, given)
        state(f"{s} 12", given, first)
        reply = self.run("object_order", plate=0, order=[0, 1, 2])
        self.t.equal(f"{s} 13: the order they have changes nothing: the reply's order", entries(reply),
                     [{"name": n, "object": i, "plate": 0} for i, n in enumerate(given)])
        state(f"{s} 13", given, first)
        self.undo()
        state(f"{s} 14: step 12 was one undo step and step 13 took none", ["mid", "short", "tall"], first)
        self.redo()
        state(f"{s} 15", given, first)
        reply = self.run("plate_slice", plates=0)
        self.t.equal(f"{s} 16: the slice's outcome", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        path = self.file("order.gcode")
        self.run("project_export", format="gcode", plates=0, path=path)
        self.t.equal(f"{s} 16: the list's order is the print order", [name for name, _ in gcode_prints(path)], given)
        state(f"{s} 16", given, first)

    def repairing(self):
        s = "repairing a mesh"
        self.start(s)
        holed = self.file("holed.stl")

        def edges_are(label, *edges):
            objects_ = self.objects_are(label, *["holed.stl"] * len(edges))
            self.t.equal(f"{label}: open_edges", [o["mesh_errors"]["open_edges"] for o in objects_], list(edges))
            self.placed(label, objects_[0], 60, 60)
            if len(objects_) > 1:
                self.placed(label, objects_[1], 128, 128)
            return objects_

        self.run("project_import", paths=[holed])
        self.run("object_transform", object=0, position=self.at(60, 60, 0))
        objects_ = edges_are(f"{s} 1", 3)
        self.t.equal(f"{s} 1: manifold", objects_[0]["mesh_errors"]["manifold"], False)
        reply = self.c.call("object_fix", objects=[0])
        self.t.check(f"{s} 2: the reply has no status: the call waited for the repair", "status" not in reply, reply)
        self.t.equal(f"{s} 2: the reply's objects", [o.get("index") for o in reply.get("objects", [])], [0])
        repairs = [{"object": r.get("object"), "changed": r.get("changed"),
                    "before": (r.get("mesh_errors_before") or {}).get("open_edges"),
                    "after": (r.get("mesh_errors_after") or {}).get("open_edges"),
                    "painting_removed": r.get("painting_removed")} for r in reply.get("repairs", [])]
        self.t.equal(f"{s} 2: the reply's repairs", repairs,
                     [{"object": 0, "changed": True, "before": 3, "after": 0, "painting_removed": False}])
        objects_ = edges_are(f"{s} 2", 0)
        self.t.equal(f"{s} 2: manifold", objects_[0]["mesh_errors"]["manifold"], True)
        # Read once the app is idle, so that a dialog or notice the repair posted after its reply would be seen.
        wait(self.c, "idle")
        self.t.equal(f"{s} 2: no dialog", self.dialogs(), [])
        notices = self.notices()
        self.t.equal(f"{s} 2: no notice of Fix Model",
                     [n for n in notices if any(ending in n for ending in REPAIR_ENDINGS)], [])
        self.undo()
        edges_are(f"{s} 3: the repair was one undo step", 3)
        self.redo()
        edges_are(f"{s} 4", 0)
        self.run("project_import", paths=[holed])
        self.run("object_transform", object=1, position=self.at(128, 128, 0))
        self.run("view_set", camera="iso")
        self.run("view_set", camera="fit_objects")
        self.run("object_select", objects=[1])
        # The 3D view draws the moved object before the click, so that it finds the object under the pointer.
        wait(self.c, "idle")
        reply = self.c.call("mouse_gesture", action="click", button="right", at={"object": 1, "spot": "top"},
                            popup_item="Modify>Fix Model")
        self.t.equal(f"{s} 5: the gesture's reply", reply.get("ran"), "Modify>Fix Model")
        # The menu item's command runs after the gesture's reply, and its notice comes once it has repaired.
        seen = {}

        def repaired():
            seen["open_edges"] = [o["mesh_errors"]["open_edges"] for o in scene(self.c)]
            seen["notices"] = self.notices()
            return seen["open_edges"][1:] == [0] and any("has been repaired" in n for n in seen["notices"])

        try:
            met = until(repaired, "object 1 repaired and a notice saying so")
        except TimeoutError:
            met = False
        self.t.check(f"{s} 5: within 30 s object 1 is repaired and a notice says it has been repaired", met, seen)
        edges_are(f"{s} 5", 0, 0)
        self.undo()
        edges_are(f"{s} 6: the menu item's repair was one undo step", 0, 3)

    def files(self):
        s = "the files a part comes from"
        self.start(s)
        reload = self.file("reload.stl")
        shutil.copyfile(self.file("cube20.stl"), reload)
        self.run("project_import", paths=[reload])
        self.run("object_transform", object=0, position=self.at(100, 100, 0))

        def volume_is(label, volume):
            obj = self.objects_are(label, "reload.stl: part")[0]
            self.t.near(f"{label}: part 0's mesh_volume", obj["parts"][0]["mesh_volume"], volume, 0.001)
            self.placed(label, obj, 100, 100)
            return obj

        volume_is(f"{s} 1", 8000)
        shutil.copyfile(self.file("cube30.stl"), reload)
        reply = self.run("object_reload_from_disk", objects=[0])
        reloaded = reply.get("reloaded", [])
        self.t.equal(f"{s} 2: the reply's reloaded", [{k: r.get(k) for k in ("object", "part")} for r in reloaded],
                     [{"object": 0, "part": 0}])
        self.t.check(f"{s} 2: the reloaded path ends in reload.stl",
                     all(r.get("path", "").endswith("reload.stl") for r in reloaded), reloaded)
        self.t.equal(f"{s} 2: the reply's missing and unmatched", (reply.get("missing"), reply.get("unmatched")),
                     ([], []))
        self.t.check(f"{s} 2: the reply has no unreadable", "unreadable" not in reply, reply)
        obj = volume_is(f"{s} 2", 27000)
        self.t.near(f"{s} 2: the object rests on the bed", obj["instances"][0]["bbox"]["min"][2], 0, 0.001)
        self.undo()
        volume_is(f"{s} 3: the reload was one undo step", 8000)
        self.redo()
        volume_is(f"{s} 4", 27000)
        os.remove(reload)
        reply = self.run("object_reload_from_disk", objects=[0])
        missing = reply.get("missing", [])
        self.t.equal(f"{s} 5: the reply's missing", [{k: r.get(k) for k in ("object", "part")} for r in missing],
                     [{"object": 0, "part": 0}])
        self.t.check(f"{s} 5: the missing path ends in reload.stl",
                     all(r.get("path", "").endswith("reload.stl") for r in missing), missing)
        self.t.equal(f"{s} 5: the reply's reloaded", reply.get("reloaded"), [])
        self.t.equal(f"{s} 5: no file dialog asks for it", self.dialogs(), [])
        volume_is(f"{s} 5", 27000)
        self.undo()
        volume_is(f"{s} 6: the call that reloaded nothing took no undo step", 8000)

        self.renew(f"{s} 7")
        add_slot(self.c, SLOT_COLOURS[1])
        self.run("project_import", paths=[self.file("cube20.stl")])
        self.run("object_transform", object=0, position=self.at(100, 100, 0))
        self.run("part_add", object=0, type="modifier", shape="cylinder", size=[10, 10], position=self.at(100, 100, 10))
        self.run("settings_set", type="process", target="part", object=0, part=0, values={"extruder": 2})

        def parts_are(label):
            obj = self.objects_are(label, "cube20.stl: part, modifier")[0]
            self.placed(label, obj, 100, 100)
            return obj

        def part0_is(label, obj, triangles):
            part = obj["parts"][0]
            self.t.equal(f"{label}: part 0's filament and triangles", (part.get("filament"), part["triangles"]),
                         (2, triangles))
            self.t.near(f"{label}: part 0's mesh_volume", part["mesh_volume"], 8000, 0.001)
            self.part_placed(label, obj, 0, 100, 100, 10)

        def part1_is_cylinder(label, obj):
            part = obj["parts"][1]
            self.t.check(f"{label}: part 1 has no filament", "filament" not in part, part)
            self.t.check(f"{label}: part 1's mesh_volume under 785.4", part["mesh_volume"] < 785.4,
                         part["mesh_volume"])
            self.part_placed(label, obj, 1, 100, 100, 10)

        obj = parts_are(f"{s} 7")
        part0_is(f"{s} 7", obj, 12)
        part1_is_cylinder(f"{s} 7", obj)
        old_id = obj["parts"][0]["id"]
        reply = self.run("part_replace", object=0, part=0, path=self.file("top4.stl"))
        obj = parts_are(f"{s} 8")
        replaced = reply.get("parts", [])
        self.t.equal(f"{s} 8: the reply's parts", [{k: p.get(k) for k in ("object", "part", "name")} for p in replaced],
                     [{"object": 0, "part": 0, "name": "top4.stl"}])
        new_id = replaced[0].get("part_id") if replaced else None
        self.t.check(f"{s} 8: the reply's part_id is new and the one object_list gives",
                     new_id != old_id and new_id == obj["parts"][0]["id"], (old_id, new_id, obj["parts"][0]["id"]))
        part0_is(f"{s} 8: the part keeps its type, filament and place", obj, 14)
        part1_is_cylinder(f"{s} 8", obj)
        self.run("part_replace", object=0, part=1, path=self.file("top4.stl"))
        obj = parts_are(f"{s} 9")
        part = obj["parts"][1]
        self.t.check(f"{s} 9: part 1 has no filament", "filament" not in part, part)
        self.t.equal(f"{s} 9: part 1's triangles", part["triangles"], 14)
        self.t.near(f"{s} 9: part 1's mesh_volume", part["mesh_volume"], 8000, 0.001)
        self.part_placed(f"{s} 9: the mesh keeps its place in its file", obj, 1, 110, 110, 15, 0.05)
        self.undo()
        part1_is_cylinder(f"{s} 10", parts_are(f"{s} 10"))
        self.undo()
        obj = parts_are(f"{s} 11: each replace was one undo step")
        self.t.equal(f"{s} 11: part 0's triangles", obj["parts"][0]["triangles"], 12)

    def painting(self):
        s = "painting kept through a mesh change"
        self.start(s)
        painted = self.file("painted.stl")
        shutil.copyfile(self.file("cube20.stl"), painted)
        self.run("project_import", paths=[painted])
        self.run("view_set", camera="front")
        self.run("view_set", camera="fit_objects")
        self.run("object_select", objects=[0])
        wait(self.c, "idle")
        reply = self.c.call("widget_click", target={"gizmo": "FdmSupports"})
        self.t.equal(f"{s} 1: the Paint-on supports gizmo opens", reply.get("active"), True)
        self.c.call("widget_click", target={"imgui_name": "Fill"})
        self.c.call("mouse_gesture", at={"object": 0, "spot": "front"})
        self.c.call("widget_click", target={"imgui": "Done"})
        wait(self.c, "idle")

        def enforcers(label, name, more=True):
            part = self.objects_are(label, f"{name}: part")[0]["parts"][0]
            count = ((part.get("painted") or {}).get("supports") or {}).get("enforcers")
            if more:
                self.t.check(f"{label}: part 0 has painted enforcers", isinstance(count, int) and count > 0,
                             part.get("painted"))
            else:
                self.t.check(f"{label}: part 0 has no painting", "painted" not in part, part.get("painted"))
            return part

        def preference(label, value):
            items = self.c.call("preferences_get", keys=["keep_painting"])["items"]
            self.t.equal(f"{label}: keep_painting", [i.get("value") for i in items], [value])

        enforcers(f"{s} 1", "painted.stl")
        self.t.equal(f"{s} 1: no gizmo open afterwards", self.gizmo(), None)
        self.run("preferences_set", values={"keep_painting": False})
        reply = self.run("object_reload_from_disk", objects=[0])
        self.t.equal(f"{s} 2: the reply's reloaded", [{k: r.get(k) for k in ("object", "part")}
                                                     for r in reply.get("reloaded", [])], [{"object": 0, "part": 0}])
        enforcers(f"{s} 2: with the preference off a reload drops the painting", "painted.stl", more=False)
        preference(f"{s} 2", False)
        self.undo()
        enforcers(f"{s} 3", "painted.stl")
        self.run("preferences_set", values={"keep_painting": True})
        self.run("object_reload_from_disk", objects=[0])
        enforcers(f"{s} 4: with it on the same call keeps the painting", "painted.stl")
        preference(f"{s} 4", True)
        self.run("part_replace", object=0, part=0, path=self.file("top4.stl"))
        part = enforcers(f"{s} 5: the painting is carried onto a mesh of other triangles", "top4.stl")
        self.t.equal(f"{s} 5: part 0's triangles", part["triangles"], 14)

    def scenarios(self):
        return [("renaming and deleting", self.renaming), ("naming by id", self.naming_by_id),
                ("splitting and assembling", self.splitting),
                ("parts", self.parts), ("ordering", self.ordering), ("repairing a mesh", self.repairing),
                ("the files a part comes from", self.files),
                ("painting kept through a mesh change", self.painting)]


def run_all(client, folder, only=()):
    checks = Checks()
    places = Places(client, checks, folder)
    parts = [Plates(client, checks, folder), places, Structure(client, checks, folder)]
    scenarios = [item for part in parts for item in part.scenarios()]
    scenarios.append(("what the project keeps", places.kept))
    for name, scenario in scenarios:
        if not only or scenario.__name__ in only:
            checks.scenario(name, scenario)
    return checks.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-objects-", ignore_cleanup_errors=True) as folder:
        return run_all(client, folder, sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
