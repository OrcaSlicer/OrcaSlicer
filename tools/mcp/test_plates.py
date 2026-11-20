#!/usr/bin/env python3
"""The plates of the Prepare view, as the test_plates entry of tools/mcp/test-entries/plates.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_plates.py

Adding, duplicating, naming, locking, selecting and deleting plates and undoing that; what a lock keeps Arrange
and Auto-Orient from doing; a plate's own settings; the filament slots its filament orders count on; and the plate
icons and plate menu. After every step the test reads the plates back with plate_list and compares them, whole,
with the state the entry writes out. It runs on the Generic Klipper Printer 0.4 nozzle, whose
support_multi_bed_types the settings scenario turns on so that a plate can take a plate type of its own, and exits
0 when every check passes.
"""

import math
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, objects, plates, run, slots, start, wait  # noqa: E402

ROW = re.compile(r'^"(.*)" (\d+)( \*)?( L)?$')
CUBE = {"shape": "cube", "size": [20, 20, 20]}
FILAMENT_ADD = "orca_command:filament_add:orca"
FILAMENT_DELETE = "orca_command:filament_delete:orca"
START = {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}
# The plate types the Klipper printer offers with support_multi_bed_types on.
PLATE_TYPES = ["Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate", "Textured Cool Plate",
               "Supertack Plate"]


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


def turn(degrees):
    """The rotation matrix, flattened, of rotation_deg [x, y, z] as the app composes it (z, then y, then x), so that
    two sets of angles that name one turn compare equal: [180, 90, 180] is [0, 90, 0]."""
    a, b, c = (math.radians(d) for d in degrees)
    rx = [[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]]
    ry = [[math.cos(b), 0, math.sin(b)], [0, 1, 0], [-math.sin(b), 0, math.cos(b)]]
    rz = [[math.cos(c), -math.sin(c), 0], [math.sin(c), math.cos(c), 0], [0, 0, 1]]
    mul = lambda p, q: [[sum(p[i][k] * q[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return [v for row in mul(rz, mul(ry, rx)) for v in row]


class PlatesTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def undo(self):
        self.run("undo", history="project")

    def redo(self):
        self.run("redo", history="project")

    def plates_are(self, label, *rows):
        self.t.equal(f"{label}: the plates", plates(self.c), P(*rows))

    def objects_are(self, label, on, outside=()):
        self.t.equal(f"{label}: the objects", objects(self.c), (sorted(on), sorted(outside)))

    def start(self, name):
        self.t.equal(f"{name}: the start: the presets", start(self.c), START)
        self.run("view_set", tab="prepare")
        wait(self.c, "idle")
        self.plates_are(f"{name}: the start", '"" 0 *')

    def listing(self):
        """Each object of object_list with the plate it is listed on, as (plate, object)."""
        listing = self.c.call("object_list", limit=500)
        return [(p["index"], o) for p in listing["plates"] for o in p["objects"]]

    def find(self, name, plate):
        found = [o for p, o in self.listing() if p == plate and o["name"] == name]
        assert len(found) == 1, f"{len(found)} objects named {name} on plate {plate}"
        return found[0]

    def corner(self, plate):
        return self.c.call("plate_list", limit=500)["items"][plate]["origin"]

    def rectangle(self, plate):
        item = self.c.call("plate_list", limit=500)["items"][plate]
        (x, y), (w, d) = item["origin"], item["size"]
        return x, y, x + w, y + d

    def save_and_reopen(self, name, preset_changes="keep"):
        """Saves the project to a new file, starts a new one, keeping or discarding the presets' unsaved changes as
        `preset_changes` says, and opens the file. The project holds an object, as a project file that holds none
        opens after the app's warning that it holds no geometry."""
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True, preset_changes=preset_changes)
        self.run("project_open", path=path)
        wait(self.c, "idle")

    def slots(self):
        return slots(self.c)

    def rule_uses(self, match):
        for rule in self.c.call("app_state").get("auto_answer_rules", []):
            if rule.get("match") == match:
                return rule.get("uses", 0)
        return None

    # ---- Checks on objects ----

    def at(self, label, obj, corner, dx, dy):
        """The object's instance 0 at the plate's corner + (dx, dy), resting on the bed."""
        inst = obj["instances"][0]
        self.t.near(f"{label}: {obj['name']} at its plate's corner + ({dx}, {dy})", inst["offset"][:2],
                    [corner[0] + dx, corner[1] + dy], 0.001)
        self.t.near(f"{label}: {obj['name']} rests on the bed", inst["bbox"]["min"][2], 0, 0.001)

    def squares_apart_on(self, label, plate, names):
        """The objects named lie inside the plate's rectangle and their squares do not overlap."""
        x0, y0, x1, y1 = self.rectangle(plate)
        boxes = []
        for name in names:
            box = self.find(name, plate)["instances"][0]["bbox"]
            boxes.append(box)
            inside = box["min"][0] >= x0 and box["min"][1] >= y0 and box["max"][0] <= x1 and box["max"][1] <= y1
            self.t.check(f"{label}: {name} lies inside plate {plate}", inside, (box, (x0, y0, x1, y1)))
        a, b = boxes
        apart = (a["max"][0] <= b["min"][0] or b["max"][0] <= a["min"][0] or a["max"][1] <= b["min"][1] or
                 b["max"][1] <= a["min"][1])
        self.t.check(f"{label}: {' and '.join(names)} do not overlap", apart, (a, b))

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
        self.run("object_add_primitive", name="kept", plate=0, **CUBE)
        after9 = ('"Left" 1 *', '"" 0 L')
        self.plates_are(f"{s} 9: the cube on plate 0", *after9)
        self.save_and_reopen("life.3mf")
        self.plates_are(f"{s} 9: the project file keeps the names and locks", *after9)
        self.objects_are(f"{s} 9: and the cube on plate 0", [(0, "kept")])

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
        c0 = self.corner(0)
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
        c1 = self.corner(1)
        for name, (dx, dy) in places.items():
            source, copy = self.find(name, 0), self.find(name, 1)
            self.at(f"{s} 3: the copy", copy, c1, dx, dy)
            si, ci = source["instances"][0], copy["instances"][0]
            self.t.near(f"{s} 3: the {name}'s copy is turned as it", ci["rotation_deg"], si["rotation_deg"], 0.001)
            self.t.near(f"{s} 3: the {name}'s copy is scaled as it", ci["scale"], si["scale"], 0.001)
            self.t.near(f"{s} 3: the {name}'s copy has its size", ci["bbox"]["size"], si["bbox"]["size"], 0.001)
            self.t.equal(f"{s} 3: the {name}'s copy has its mesh",
                         [(p["triangles"], p["mesh_volume"]) for p in copy["parts"]],
                         [(p["triangles"], p["mesh_volume"]) for p in source["parts"]])
        sources(f"{s} 3: the source")
        self.objects_are(f"{s} 3", [(0, "cube"), (0, "cylinder"), (1, "cube"), (1, "cylinder")])
        copy = self.find("cube", 1)
        self.run("object_transform", object=copy["index"], translate=[10, 0, 0])
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
        c0 = self.corner(0)
        self.run("object_add_primitive", name="A", position=[c0[0] + 30, c0[1] + 30, 0], **side)
        self.run("object_transform", object=0, rotate=[0, 90, 0], mode="absolute")
        self.run("plate_add")
        c1 = self.corner(1)
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
        self.run("action_run", id=FILAMENT_ADD)
        self.t.equal(f"{s}: two filament slots", self.slots(), "0,0")
        self.run("project_set_settings", values={"curr_bed_type": "Cool Plate"})
        self.run("object_add_primitive", name="first", **CUBE)
        self.run("plate_add")
        self.run("object_add_primitive", name="second", plate=1, **CUBE)
        self.plates_are(f"{s}: the start", '"" 1', '"" 1 *')
        keys = ["curr_bed_type", "print_sequence", "spiral_mode", "first_layer_print_sequence",
                "other_layers_print_sequence", "overrides"]

        def S(**own):
            out = {"curr_bed_type": "Cool Plate", "print_sequence": "by layer", "spiral_mode": False,
                   "first_layer_print_sequence": None, "other_layers_print_sequence": None, "overrides": []}
            out.update(own)
            return out

        def settings_are(label, p0, p1):
            got = [{k: item.get(k) for k in keys} for item in self.c.call("plate_list", limit=500)["items"]]
            self.t.equal(f"{label}: the plates' settings", got, [p0, p1])

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
        # The new project drops the printer's edit, so that the open asks nothing about it; the project brings it back.
        self.save_and_reopen("settings.3mf", preset_changes="discard")
        settings_are(f"{s} 15: the project file keeps a plate's own settings", S(), after4)
        edited = [i["key"] for i in self.c.call("settings_get", type="printer", compare=":saved", limit=50)["items"]]
        self.t.equal(f"{s} 15: the project brings back the printer's support_multi_bed_types", edited,
                     ["support_multi_bed_types"])

    def filament_slots(self):
        s = "filament slots"
        self.start(s)
        self.t.refused(f"{s} 1: deleting the only slot is refused", lambda: self.c.call("filament_delete"),
                       code="last_filament_slot")
        self.t.equal(f"{s} 1", self.slots(), "0")
        steps = [(2, FILAMENT_ADD, "Added filament slot 2.", "0,0"), (3, FILAMENT_ADD, "Added filament slot 3.", "0,0,0")]
        for step, action, message, slots in steps:
            reply = self.run("action_run", id=action)
            self.t.equal(f"{s} {step}: the message", reply.get("message"), message)
            self.t.equal(f"{s} {step}", self.slots(), slots)
        self.t.refused(f"{s} 4: deleting slot 4 of 3 is refused",
                       lambda: self.c.call("filament_delete", filament_slot=4), code="no_such_slot")
        self.t.equal(f"{s} 4", self.slots(), "0,0,0")
        reply = self.run("filament_delete", filament_slot=2)
        self.t.equal(f"{s} 5: the message", reply.get("message"), "Deleted filament slot 2.")
        self.t.equal(f"{s} 5", self.slots(), "0,0")

        def add_mixed(step):
            job = self.c.call("widget_click", target={"label": "Add Mixed Filament"})
            wait(self.c, "dialog")
            dialogs = self.c.call("app_state").get("dialogs", [])
            self.t.equal(f"{s} {step}: the dialog that opens", [d.get("title") for d in dialogs], ["Add Mixed Filament"])
            self.run("dialog_answer", button="OK")
            self.c.await_job(job)
            wait(self.c, "idle")
            self.t.equal(f"{s} {step}", self.slots(), "0,0,1")

        add_mixed(6)
        reply = self.run("filament_delete", filament_slot=3)
        self.t.equal(f"{s} 7: the message", reply.get("message"), "Deleted filament slot 3.")
        self.t.equal(f"{s} 7: a mixed slot is deleted", self.slots(), "0,0")
        add_mixed(8)
        self.t.refused(f"{s} 9: a filament order is refused while the project has mixed filaments",
                       lambda: self.c.call("plate_set_settings", plate=0,
                                           values={"first_layer_print_sequence": [2, 1]}),
                       words="cannot be set while the project has mixed filaments")
        self.t.equal(f"{s} 9: plate 0's order stays Auto",
                     self.c.call("plate_list")["items"][0]["first_layer_print_sequence"], None)
        self.t.equal(f"{s} 9", self.slots(), "0,0,1")
        reply = self.run("action_run", id=FILAMENT_ADD)
        self.t.equal(f"{s} 10: the message", reply.get("message"), "Added filament slot 3.")
        self.t.equal(f"{s} 10: a new slot goes ahead of the mixed one", self.slots(), "0,0,0,1")
        reply = self.run("action_run", id=FILAMENT_DELETE)
        self.t.equal(f"{s} 11: the message", reply.get("message"), "Deleted filament slot 3.")
        self.t.equal(f"{s} 11", self.slots(), "0,0,1")
        self.run("widget_click", target={"label": "Remove last mixed filament"})
        wait(self.c, "idle")
        self.t.equal(f"{s} 12", self.slots(), "0,0")

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
        x0, y0, x1, y1 = self.rectangle(1)
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
                     (state.get("dialogs"), state.get("closed_menus")), ([], []))

    def run_all(self):
        for name, scenario in [("a plate's life", self.life), ("by index", self.by_index),
                               ("duplicating a plate", self.duplicating), ("what a lock does", self.lock),
                               ("a plate's own settings", self.own_settings), ("filament slots", self.filament_slots),
                               ("plate icons", self.icons)]:
            self.t.scenario(name, scenario)
        self.c.call("dialog_auto_answer", rules=[])
        self.run("project_new", discard_changes=True)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-plates-") as folder:
        return PlatesTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
