#!/usr/bin/env python3
"""What objects are made of and how the list orders them, as the test_object_structure entry of
tools/mcp/test-entries/objects.md designs it.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_object_structure.py

Renaming and deleting objects, and the layer commands acting on the selected object; Split to Objects and to Parts,
and Assemble; adding parts of every type, changing a part's type and renaming a part; ordering a plate's objects and
the print order that sets; Fix Model by index and through the object's menu; Reload from disk and Replace 3D file;
and the keep_painting preference, shown against its control. After every step the test reads the objects back with
object_list and compares their names, and the parts' types where the entry writes them, with the state the entry
writes out. It runs on the harness's default printer, the Bambu Lab X1 Carbon 0.4, and exits 0 when every check
passes.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import Checks, corner, gcode_prints, plates, run, scene, selection, wait  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(REPO, "tools", "mcp", "tests", "fixtures")
# The system's own form of the path, as the loader names the object after its last separator.
RINGS = os.path.normpath(os.path.join(REPO, "tests", "data", "two_hollow_squares.obj"))
DRC = os.path.join(REPO, "resources", "handy_models", "OrcaCube_v2.drc")
STAR = os.path.join(FIXTURES, "star.svg")
HEIGHTMAP = os.path.join(FIXTURES, "heightmap.png")

ROW = re.compile(r'^"(.*)" (\d+)( \*)?$')
CUBE = {"shape": "cube", "size": [20, 20, 20]}
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


def P(*rows):
    """Plates written as the entry writes them: '"" 3 *' is an unnamed current plate with three objects."""
    out = []
    for row in rows:
        m = ROW.match(row)
        assert m, row
        out.append({"name": m.group(1), "current": bool(m.group(3)), "locked": False, "objects": int(m.group(2))})
    return out


def centre(box):
    return [(lo + hi) / 2 for lo, hi in zip(box["min"], box["max"])]


def names(objects):
    """The objects' names in index order, from scene()."""
    return [o["name"] for o in objects]


def types(obj):
    """An object's parts' types in order, from its entry in scene()."""
    return [p["type"] for p in obj["parts"]]


def names_of(reply_items):
    """The `index` and `name` of each object a reply names."""
    return [{"index": o.get("index"), "name": o.get("name")} for o in reply_items or []]


class ObjectStructureTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.W = folder
        for name, triangles in (("cube20.stl", cube_triangles(20)), ("cube30.stl", cube_triangles(30)),
                                ("holed.stl", cube_triangles(20, holed=True)),
                                ("top4.stl", cube_triangles(20, top4=True))):
            write_stl(self.file(name), triangles)

    # ---- Calls and reads ----

    def file(self, name):
        return os.path.join(self.W, name)

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def undo(self):
        self.run("undo", history="project")

    def redo(self):
        self.run("redo", history="project")

    def notices(self):
        return [n.get("text", "") for n in self.c.call("app_state").get("notifications", [])]

    def dialogs(self):
        return [d.get("title") for d in self.c.call("app_state").get("dialogs", [])]

    def gizmo(self):
        return self.c.call("app_state").get("gizmo")

    def plates_are(self, label, *rows):
        self.t.equal(f"{label}: the plates", plates(self.c), P(*rows))

    def objects_are(self, label, *rows, plate_rows=None):
        """The objects' names in index order compared with the rows whole, and the parts' types of every object a row
        writes them for ("Assembly: part, modifier"); then the plates, '"" n *' unless `plate_rows` gives them.
        Returns scene()."""
        objects = scene(self.c)
        self.t.equal(f"{label}: the objects", names(objects), [row.split(": ")[0] for row in rows])
        for i, row in enumerate(rows):
            if ": " in row:
                got = types(objects[i]) if i < len(objects) else None
                self.t.equal(f"{label}: object {i}'s parts", got, row.split(": ", 1)[1].split(", "))
        self.plates_are(label, *(plate_rows or [f'"" {len(rows)} *']))
        return objects

    def start(self, name):
        self.run("project_new", discard_changes=True)
        self.run("view_set", tab="prepare")
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
        objects = self.objects_are(f"{s} 1", "Cube", "Cube", "Cube")
        for obj, x in zip(objects, (40, 100, 160)):
            self.placed(f"{s} 1", obj, x, 60)
        reply = self.run("object_rename", object=1, name="middle")
        self.t.equal(f"{s} 2: the reply", names_of(reply.get("objects")), [{"index": 1, "name": "middle"}])
        self.objects_are(f"{s} 2", "Cube", "middle", "Cube")
        reply = self.run("object_delete", objects=[0])
        self.t.equal(f"{s} 3: the reply's objects", names_of(reply.get("objects")), [{"index": 0, "name": "Cube"}])
        self.t.equal(f"{s} 3: the reply's object_count", reply.get("object_count"), 2)
        objects = self.objects_are(f"{s} 3", "middle", "Cube")
        self.placed(f"{s} 3", objects[0], 100, 60)
        self.placed(f"{s} 3", objects[1], 160, 60)
        self.undo()
        self.objects_are(f"{s} 4: the delete was one undo step", "Cube", "middle", "Cube")
        self.redo()
        self.objects_are(f"{s} 5", "middle", "Cube")
        self.run("object_select", objects=[0])
        reply = self.run("object_delete")
        self.t.equal(f"{s} 6: the reply's objects", names_of(reply.get("objects")), [{"index": 0, "name": "middle"}])
        self.t.equal(f"{s} 6: the reply's object_count", reply.get("object_count"), 1)
        objects = self.objects_are(f"{s} 6", "Cube")
        self.placed(f"{s} 6", objects[0], 160, 60)
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
        objects = self.objects_are(f"{s} 8", "Cube", "layered")
        self.t.equal(f"{s} 8: layered has the range", [{k: r.get(k) for k in ("min_z", "max_z")}
                                                      for r in objects[1].get("layer_config_ranges", [])],
                     [{"min_z": 0, "max_z": 5}])
        self.t.check(f"{s} 8: Cube has no range", "layer_config_ranges" not in objects[0], objects[0])
        reply = self.run("object_set_variable_layer_height", action="adaptive")
        self.t.equal(f"{s} 9: the reply's object", reply.get("object"), 1)
        self.t.equal(f"{s} 9: the reply's objects", names_of(reply.get("objects")),
                     [{"index": 1, "name": "layered"}])
        self.t.check(f"{s} 9: the reply has a layer_height_profile", "layer_height_profile" in reply, reply)
        objects = self.objects_are(f"{s} 9", "Cube", "layered")
        points = (objects[1].get("layer_height_profile") or {}).get("points")
        self.t.check(f"{s} 9: layered has a profile with points", isinstance(points, int) and points > 0,
                     objects[1].get("layer_height_profile"))
        self.t.check(f"{s} 9: Cube has no profile", "layer_height_profile" not in objects[0], objects[0])

    def splitting(self):
        s = "splitting and assembling"
        self.start(s)
        name = "two_hollow_squares.obj"
        reply = self.run("project_import", paths=[RINGS])
        self.t.equal(f"{s} 1: the import's added", reply.get("added"), 1)
        objects = self.objects_are(f"{s} 1", f"{name}: part")
        self.t.near(f"{s} 1: its box", objects[0]["bbox"]["size"], RINGS_SIZE, 0.01)
        reply = self.run("object_split", object=0, into="objects")
        self.t.equal(f"{s} 2: the reply's objects", [o.get("index") for o in reply.get("objects", [])], [0, 1])
        self.t.equal(f"{s} 2: the reply's object_count", reply.get("object_count"), 2)
        objects = self.objects_are(f"{s} 2", f"{name}_1: part", f"{name}_2: part")
        self.rings(f"{s} 2", [o["bbox"] for o in objects])
        self.t.equal(f"{s} 2: the pieces are selected", selection(self.c)["objects"], [0, 1])
        self.t.equal(f"{s} 3: the import's added", self.run("project_import", paths=[RINGS]).get("added"), 1)
        reply = self.run("object_split", object=2, into="parts")
        self.t.equal(f"{s} 3: the reply's parts", [{k: p.get(k) for k in ("object", "part")}
                                                  for p in reply.get("parts", [])],
                     [{"object": 2, "part": 0}, {"object": 2, "part": 1}])
        self.t.equal(f"{s} 3: the reply's object_count", reply.get("object_count"), 3)
        objects = self.objects_are(f"{s} 3", f"{name}_1: part", f"{name}_2: part", f"{name}: part, part")
        self.t.equal(f"{s} 3: the parts' names", [p["name"] for p in objects[2]["parts"]], [f"{name}_1", f"{name}_2"])
        self.rings(f"{s} 3: the parts", [p["bbox"] for p in objects[2]["parts"]])
        after4 = (f"{name}: part, part", "Assembly: part, part")
        reply = self.run("object_assemble", objects=[0, 1])
        self.t.equal(f"{s} 4: the reply's object and name", (reply.get("object"), reply.get("name")), (1, "Assembly"))
        objects = self.objects_are(f"{s} 4", *after4)
        self.t.near(f"{s} 4: the pieces keep their places: Assembly's box", objects[1]["bbox"]["size"], RINGS_SIZE,
                    0.01)
        reply = self.run("part_change_type", parts=[[1, 0]], type="modifier")
        self.t.equal(f"{s} 5: the reply's parts", [{k: p.get(k) for k in ("object", "part", "type")}
                                                  for p in reply.get("parts", [])],
                     [{"object": 1, "part": 1, "type": "modifier"}])
        objects = self.objects_are(f"{s} 5", f"{name}: part, part", "Assembly: part, modifier")
        self.t.near(f"{s} 5: a modifier is left out of Assembly's box", objects[1]["bbox"]["size"], RING, 0.01)
        self.undo()
        objects = self.objects_are(f"{s} 6: the change was one undo step", *after4)
        self.t.near(f"{s} 6: Assembly's box", objects[1]["bbox"]["size"][0], RINGS_SIZE[0], 0.01)
        self.run("object_select", objects=[0])
        reply = self.run("object_split", into="objects")
        self.t.equal(f"{s} 7: the reply's objects", [o.get("index") for o in reply.get("objects", [])], [1, 2])
        self.t.equal(f"{s} 7: the reply's object_count", reply.get("object_count"), 3)
        objects = self.objects_are(f"{s} 7: no target splits the selected object, each piece named after its part",
                                   "Assembly: part, part", f"{name}_1: part", f"{name}_2: part")
        self.rings(f"{s} 7", [o["bbox"] for o in objects[1:]])
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

        self.start(f"{s} 10")
        self.run("object_add_primitive", name="scaled", **CUBE)
        self.run("object_transform", object=0, scale=1.5)
        reply = self.run("part_add", object=0, type="modifier", shape="cube")
        self.t.equal(f"{s} 10: the reply's part", reply.get("part"), 1)
        scaled = self.objects_are(f"{s} 10", "scaled: part, modifier")[0]
        self.t.near(f"{s} 10: adding a shape folds the instance's scale into the parts: the instance's scale",
                    scaled["instances"][0]["scale"], [1, 1, 1], 0.001)
        self.t.near(f"{s} 10: part 0's scale", scaled["parts"][0]["scale"], [1.5, 1.5, 1.5], 0.001)
        self.t.near(f"{s} 10: the object's size", scaled["bbox"]["size"], [30, 30, 30], 0.001)

        self.start(f"{s} 11")
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
            objects = self.objects_are(label, *order, plate_rows=plate_rows)
            for obj in objects:
                if obj["name"] in places:
                    self.placed(label, obj, places[obj["name"]], 60)
            return objects

        def order_is(label, reply, order, descending=None):
            self.t.equal(f"{label}: the reply's order", reply.get("order"),
                         [{"name": n, "object": i, "plate": 0} for i, n in enumerate(order)])
            if descending is None:
                self.t.check(f"{label}: the reply has no descending", "descending" not in reply, reply)
            else:
                self.t.equal(f"{label}: the reply's descending", reply.get("descending"), descending)

        objects = state(f"{s} 1", ["tall", "short", "mid"], one)
        self.t.near(f"{s} 1: the heights", [o["bbox"]["size"][2] for o in objects], [30, 10, 20], 0.001)
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
        self.t.equal(f"{s} 13: the order they have changes nothing: the reply's order", reply.get("order"),
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
            objects = self.objects_are(label, *["holed.stl"] * len(edges))
            self.t.equal(f"{label}: open_edges", [o["mesh_errors"]["open_edges"] for o in objects], list(edges))
            self.placed(label, objects[0], 60, 60)
            if len(objects) > 1:
                self.placed(label, objects[1], 128, 128)
            return objects

        self.run("project_import", paths=[holed])
        self.run("object_transform", object=0, position=self.at(60, 60, 0))
        objects = edges_are(f"{s} 1", 3)
        self.t.equal(f"{s} 1: manifold", objects[0]["mesh_errors"]["manifold"], False)
        reply = self.c.call("object_fix", objects=[0])
        self.t.check(f"{s} 2: the reply has no status: the call waited for the repair", "status" not in reply, reply)
        self.t.equal(f"{s} 2: the reply's objects", [o.get("index") for o in reply.get("objects", [])], [0])
        repairs = [{"object": r.get("object"), "changed": r.get("changed"),
                    "before": (r.get("mesh_errors_before") or {}).get("open_edges"),
                    "after": (r.get("mesh_errors_after") or {}).get("open_edges"),
                    "painting_removed": r.get("painting_removed")} for r in reply.get("repairs", [])]
        self.t.equal(f"{s} 2: the reply's repairs", repairs,
                     [{"object": 0, "changed": True, "before": 3, "after": 0, "painting_removed": False}])
        objects = edges_are(f"{s} 2", 0)
        self.t.equal(f"{s} 2: manifold", objects[0]["mesh_errors"]["manifold"], True)
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
        deadline = time.monotonic() + 30
        while True:
            edges = [o["mesh_errors"]["open_edges"] for o in scene(self.c)]
            notices = self.notices()
            met = edges[1:] == [0] and any("has been repaired" in n for n in notices)
            if met or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        self.t.check(f"{s} 5: within 30 s object 1 is repaired and a notice says it has been repaired", met,
                     (edges, notices))
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

        self.start(f"{s} 7")
        self.run("filament_add")
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
        self.run("preferences_set", values={"keep_painting": False})
        enforcers(f"{s} 6", "top4.stl")
        preference(f"{s} 6: the preference as the test found it", False)

    def keeps(self):
        s = "what the project keeps"
        self.start(s)
        self.run("object_add_primitive", shape="cube", size=[30, 30, 10], name="base", position=self.at(60, 60, 0))
        self.run("part_add", object=0, type="negative", shape="cylinder", size=[6, 3], position=self.at(60, 60, 1.5))
        self.run("part_add", object=0, type="modifier", shape="cube", size=[10, 10, 10], position=self.at(60, 60, 5))
        self.run("part_add", object=0, type="precise_seam", shape="cube", size=[2, 2, 2], position=self.at(75, 60, 5))
        self.run("object_rename", object=0, part=2, name="infill zone")
        self.run("part_change_type", parts=[[0, 3]], type="precise_seam", subtype="left")
        self.run("object_add_primitive", name="other", position=self.at(180, 180, 0), **CUBE)
        self.run("object_order", plate=0, order=[1, 0])

        def kept(label):
            base = self.objects_are(label, "other: part", "base: part, negative, modifier, precise_seam")[1]
            self.t.equal(f"{label}: base's parts' names", [p["name"] for p in base["parts"]],
                         ["base", "Generic-Cylinder", "infill zone", "Generic-Cube"])
            for i, place in enumerate([(60, 60, 5), (60, 60, 1.5), (60, 60, 5), (75, 60, 5)]):
                self.part_placed(label, base, i, *place)
            self.t.equal(f"{label}: the seam's subtype", base["parts"][3].get("subtype"), "left")

        kept(f"{s} 1")
        path = self.file("keeps.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.empty(f"{s} 2: the new project")
        self.run("project_open", path=path)
        wait(self.c, "idle")
        kept(f"{s} 2: the project file keeps them")

    def run_all(self):
        for name, scenario in [("renaming and deleting", self.renaming), ("splitting and assembling", self.splitting),
                               ("parts", self.parts), ("ordering", self.ordering),
                               ("repairing a mesh", self.repairing), ("the files a part comes from", self.files),
                               ("painting kept through a mesh change", self.painting),
                               ("what the project keeps", self.keeps)]:
            self.t.scenario(name, scenario)
        try:  # the preference as the test found it, should the painting scenario have broken off
            self.c.call("preferences_set", values={"keep_painting": False})
            self.run("project_new", discard_changes=True)
        except orca_mcp.McpError as e:
            print(f"cleaning up: {e}", flush=True)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-structure-", ignore_cleanup_errors=True) as folder:
        return ObjectStructureTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
