#!/usr/bin/env python3
"""The commands that open a gizmo on a named target, as the test_gizmo_commands entry of
tools/mcp/test-entries/gizmos.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_gizmo_commands.py [scenario ...]

A call that names only its target opens the gizmo on it, and the same call again leaves it open; a refused opening
leaves the model, the gizmo and the selection as it found them. A call with an acting argument (object_cut's plane)
closes the open gizmo and does the job as the gizmo's own button would, one undo step. After every step the test reads
the state back through another tool (object_list, app_state, the undo log, the saved project) and compares it with the
values the entry writes out. It runs on the harness's default printer, the Bambu Lab X1 Carbon 0.4, and exits 0 when
every check passes. Scenario names given on the command line run those scenarios alone.
"""

import os
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import Checks, run, selection, slots, undo_steps, wait  # noqa: E402

STAR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures", "star.svg")


class GizmoCommandsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Calls ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def idle(self, timeout_s=45):
        wait(self.c, "idle", timeout_s)

    def cube(self, at, size=20, name=None):
        args = {"shape": "cube", "size": [size, size, size], "position": [at[0], at[1], 0]}
        if name:
            args["name"] = name
        return self.run("object_add_primitive", **args)

    def filament_slots(self, count):
        """Deletes filament slots until a delete is refused (one is left), then adds `count` - 1."""
        for _ in range(32):
            try:
                self.c.call("filament_delete")
            except orca_mcp.McpError:
                break
        for _ in range(count - 1):
            self.run("filament_add")

    # ---- Reads ----

    def scene(self):
        """Each object of object_list in index order as {name, offset, parts}: its instance 0's offset, its parts'
        count."""
        return [{"name": o["name"], "offset": [round(v, 3) for v in o["instances"][0]["offset"]], "parts": len(o["parts"])}
                for o in self.listed()]

    def listed(self):
        """object_list's objects, on the plates and outside them, in index order."""
        listing = self.c.call("object_list", limit=500)
        return sorted([o for p in listing["plates"] for o in p["objects"]] + listing.get("outside", []),
                      key=lambda o: o["index"])

    def shapes(self):
        """Each object in index order as {name, offset, min, max, size, parts}: its instance 0's offset and bbox, and its
        parts as (type, mesh_volume)."""
        out = []
        for o in self.listed():
            inst = o["instances"][0]
            out.append({"name": o["name"], "offset": inst["offset"], "min": inst["bbox"]["min"], "max": inst["bbox"]["max"],
                        "size": inst["bbox"]["size"], "parts": [(p["type"], p["mesh_volume"]) for p in o["parts"]]})
        return out

    def gizmo(self):
        g = self.c.call("app_state").get("gizmo")
        return None if g is None else {"name": g.get("name"), "canvas": g.get("canvas")}

    def selected(self):
        """checks.selection() without the instances of a part selection, which the entry does not fix."""
        sel = selection(self.c)
        return {"parts": sel["parts"]} if "parts" in sel else {"objects": sel["objects"], "instances": sel["instances"]}

    @staticmethod
    def fields(reply, keys):
        """The fields of `reply` a row fixes, a target in it by its index or object, instance, part and name."""
        out = {}
        for key in keys:
            value = reply.get(key)
            if isinstance(value, list):
                value = [{k: v for k, v in item.items() if k not in ("object_id", "part_id")} for item in value]
            out[key] = value
        return out

    def start(self, name):
        self.run("project_new", discard_changes=True)
        self.run("view_set", tab="prepare")
        self.idle()
        self.t.equal(f"{name}: the start: no objects", self.scene(), [])
        self.t.equal(f"{name}: the start: no gizmo open", self.gizmo(), None)

    def two_objects(self, label):
        """A 20 mm cube A at (100, 100), and a 20 mm cube B at (150, 100) with a 10 mm cube part; nothing selected."""
        self.cube([100, 100], name="A")
        self.cube([150, 100], name="B")
        self.run("part_add", object=1, type="part", shape="cube", size=[10, 10, 10], position=[160, 110, 20])
        self.run("object_select", none=True)

    AB = [{"name": "A", "offset": [100, 100, 10], "parts": 1}, {"name": "B", "offset": [150, 100, 10], "parts": 2}]

    # ---- Scenarios ----

    def opening(self):
        s = "opening a gizmo on a target"
        self.start(s)
        self.filament_slots(2)
        self.two_objects(s)
        self.t.equal(f"{s} 0: the objects", self.scene(), self.AB)
        self.t.equal(f"{s} 0: two filament slots", slots(self.c), "0,0")
        self.t.equal(f"{s} 0: nothing selected", self.selected(), {"objects": [], "instances": []})
        self.t.equal(f"{s} 0: no gizmo open", self.gizmo(), None)

        def opens(row, tool, args, reply_fields, gizmo, sel):
            reply = self.run(tool, **args)
            self.t.equal(f"{s} {row}: the reply", self.fields(reply, ["gizmo", "active"] + list(reply_fields)),
                         {"gizmo": gizmo, "active": True, **reply_fields})
            self.t.equal(f"{s} {row}: the gizmo open", self.gizmo(), {"name": gizmo, "canvas": "3d"})
            self.t.equal(f"{s} {row}: the selection", self.selected(), sel)
            self.t.equal(f"{s} {row}: the objects", self.scene(), self.AB)
            return reply

        a0 = {"instances": [{"object": 0, "instance": 0, "name": "A"}]}
        first = opens(1, "object_paint", {"object": 0, "type": "supports"}, a0, "FdmSupports",
                      {"objects": [0], "instances": [[0, 0]]})
        again = self.run("object_paint", object=0, type="supports")
        self.t.equal(f"{s} 2: the same reply", again, first)
        self.t.equal(f"{s} 2: FdmSupports stays open", self.gizmo(), {"name": "FdmSupports", "canvas": "3d"})
        opens(3, "object_paint", {"object": 0, "type": "seam"}, a0, "Seam", {"objects": [0], "instances": [[0, 0]]})
        opens(4, "object_paint", {"object": 0, "type": "color"}, a0, "Color Painting", {"objects": [0], "instances": [[0, 0]]})
        opens(5, "object_paint", {"object": 0, "type": "fuzzy_skin"}, a0, "Fuzzy Skin Painting",
              {"objects": [0], "instances": [[0, 0]]})
        self.run("object_select", none=True)
        self.t.equal(f"{s} 6: no gizmo open", self.gizmo(), None)
        self.t.equal(f"{s} 6: nothing selected", self.selected(), {"objects": [], "instances": []})
        opens(7, "part_boolean", {"object": 1}, {"instances": [{"object": 1, "instance": 0, "name": "B"}]}, "MeshBoolean",
              {"objects": [1], "instances": [[1, 0]]})
        reply = self.run("object_simplify", object=1, part=1)
        self.t.equal(f"{s} 8: the reply", (reply.get("gizmo"), reply.get("active"),
                                           [(p["object"], p["part"]) for p in reply.get("parts", [])]),
                     ("Simplify", True, [(1, 1)]))
        self.t.equal(f"{s} 8: the gizmo open", self.gizmo(), {"name": "Simplify", "canvas": "3d"})
        self.t.equal(f"{s} 8: the selection", self.selected(), {"parts": [[1, 1]]})
        reply = self.run("object_simplify", object=0)
        self.t.equal(f"{s} 9: the reply: A's only part", (reply.get("gizmo"), reply.get("active"),
                                                          [(p["object"], p["part"]) for p in reply.get("parts", [])]),
                     ("Simplify", True, [(0, 0)]))
        self.t.equal(f"{s} 9: the gizmo open", self.gizmo(), {"name": "Simplify", "canvas": "3d"})
        # An object of one part shows no part row in the object list, which selects it whole.
        self.t.equal(f"{s} 9: the selection", self.selected(), {"objects": [0], "instances": [[0, 0]]})
        opens(10, "object_measure", {"objects": [0, 1]},
              {"objects": [{"index": 0, "name": "A"}, {"index": 1, "name": "B"}]}, "Measure",
              {"objects": [0, 1], "instances": [[0, 0], [1, 0]]})
        self.run("object_select", none=True)
        self.run("object_select", parts=[[1, 1]])
        reply = self.run("object_measure")
        self.t.equal(f"{s} 11: the reply: the selected part", (reply.get("gizmo"), reply.get("active"),
                                                               [(p["object"], p["part"]) for p in reply.get("parts", [])]),
                     ("Measure", True, [(1, 1)]))
        self.t.equal(f"{s} 11: the gizmo open", self.gizmo(), {"name": "Measure", "canvas": "3d"})
        self.t.equal(f"{s} 11: the selection", self.selected(), {"parts": [[1, 1]]})
        self.run("object_select", none=True)
        self.t.equal(f"{s} 12: no gizmo open", self.gizmo(), None)
        self.t.equal(f"{s} 12: nothing selected", self.selected(), {"objects": [], "instances": []})
        self.t.equal(f"{s} 12: the objects", self.scene(), self.AB)

    def refused_openings(self):
        s = "openings refused"
        self.start(s)
        self.filament_slots(1)
        self.two_objects(s)
        self.t.equal(f"{s} 0: the objects", self.scene(), self.AB)
        self.t.equal(f"{s} 0: one filament slot", slots(self.c), "0")
        self.t.equal(f"{s} 0: nothing selected", self.selected(), {"objects": [], "instances": []})

        def refused(row, before, tool, args, code, before_gizmo=None, steps=()):
            self.t.equal(f"{s} {row}: the selection before", self.selected(), before)
            self.t.equal(f"{s} {row}: the gizmo before", self.gizmo(), before_gizmo)
            undo_steps(self.c)
            self.t.refused(f"{s} {row}: refused", lambda: self.c.call(tool, **args), code=code)
            self.t.equal(f"{s} {row}: the objects", self.scene(), self.AB)
            self.t.equal(f"{s} {row}: no gizmo open", self.gizmo(), None)
            self.t.equal(f"{s} {row}: the undo steps", undo_steps(self.c), list(steps))
            self.t.equal(f"{s} {row}: the selection is put back", self.selected(), before)

        self.run("object_select", objects=[1])
        refused(1, {"objects": [1], "instances": [[1, 0]]}, "part_boolean", {"object": 0}, "needs_two_parts")
        self.run("object_select", none=True)
        refused(2, {"objects": [], "instances": []}, "object_paint", {"object": 0, "type": "color"}, "needs_two_filaments")
        self.run("object_select", parts=[[1, 1]])
        refused(3, {"parts": [[1, 1]]}, "object_simplify", {"object": 1}, "needs_one_part")
        self.run("object_select", none=True)
        refused(4, {"objects": [], "instances": []}, "object_measure", {}, "needs_selection")
        self.run("object_paint", object=1, type="supports")
        refused(5, {"objects": [1], "instances": [[1, 0]]}, "part_boolean", {"object": 0}, "needs_two_parts",
                before_gizmo={"name": "FdmSupports", "canvas": "3d"}, steps=["Leaving Paint-on supports"])

    # ---- Cut ----

    def undo(self):
        self.run("undo", history="project")

    def redo(self):
        self.run("redo", history="project")

    def save_and_reopen(self, name):
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.run("project_open", path=path)
        self.idle()

    def piece(self, label, o, volumes, size=None, height=None, xy=None, x=None):
        """Checks one object of shapes(): its parts' types and mesh volumes (to 1 mm³), that it rests on the bed, and
        when given its bbox size, its height, its x and y range [x0, y0, x1, y1], or its x range [x0, x1] (to 0.01)."""
        self.t.equal(f"{label}: the parts' types", [t for t, _ in o["parts"]], ["part"] * len(volumes))
        self.t.near(f"{label}: the parts' mesh volumes", [v for _, v in o["parts"]], list(volumes), 1)
        self.t.near(f"{label}: rests on the bed", o["min"][2], 0, 0.01)
        if size is not None:
            self.t.near(f"{label}: the size", o["size"], size, 0.01)
        if height is not None:
            self.t.near(f"{label}: the height", o["size"][2], height, 0.01)
        if xy is not None:
            self.t.near(f"{label}: the x and y range", [o["min"][0], o["min"][1], o["max"][0], o["max"][1]], xy, 0.01)
        if x is not None:
            self.t.near(f"{label}: the x range", [o["min"][0], o["max"][0]], x, 0.01)

    def whole(self, label):
        """The 20 mm cube A at (100, 100), as after the cut scenarios' row 0."""
        objs = self.shapes()
        if self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["A"]):
            self.t.near(f"{label}: A's offset", objs[0]["offset"], [100, 100, 10], 0.01)
            self.piece(f"{label}: A", objs[0], [8000], size=[20, 20, 20])

    def halves(self, label):
        """A cut level at z 10: two pieces 10 mm high where A stood, upper first."""
        objs = self.shapes()
        if self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["A", "A"]):
            for o, which in zip(objs, ("upper", "lower")):
                self.piece(f"{label}: the {which} piece", o, [4000], size=[20, 20, 10], xy=[90, 90, 110, 110])

    @staticmethod
    def pieces_reply(reply):
        return [{k: p.get(k) for k in ("index", "name", "open_edges")} for p in reply.get("pieces", [])]

    def cut(self):
        s = "cut from arguments"
        self.start(s)
        self.cube([100, 100], name="A")
        self.run("object_select", none=True)
        self.whole(f"{s} 0")

        undo_steps(self.c)
        reply = self.run("object_cut", object=0)
        self.t.equal(f"{s} 1: the reply", (reply.get("gizmo"), reply.get("active")), ("Cut", True))
        self.t.equal(f"{s} 1: the Cut gizmo open", self.gizmo(), {"name": "Cut", "canvas": "3d"})
        # The Cut gizmo records a step of its own as it opens, as for a click on its icon.
        self.t.equal(f"{s} 1: the undo steps", undo_steps(self.c), ["Entering Cut gizmo"])
        self.whole(f"{s} 1")

        reply = self.run("object_cut", object=0, z=10)
        self.halves(f"{s} 2")
        self.t.equal(f"{s} 2: the open Cut gizmo was closed first", self.gizmo(), None)
        self.t.equal(f"{s} 2: the reply", self.pieces_reply(reply),
                     [{"index": 0, "name": "A", "open_edges": 0}, {"index": 1, "name": "A", "open_edges": 0}])
        # The gizmo's own step as it closes, then the cut's.
        self.t.equal(f"{s} 2: the undo steps", undo_steps(self.c), ["Leaving Cut gizmo", "Cut by Plane"])
        self.undo()
        self.whole(f"{s} 3")
        self.redo()
        self.halves(f"{s} 4")
        self.undo()
        self.whole(f"{s} 5")

        reply = self.run("object_cut", object=0, plane={"point": [100, 100, 10], "normal": [1, 0, 1]})
        objs = self.shapes()
        if self.t.equal(f"{s} 6: the objects", [o["name"] for o in objs], ["A", "A"]):
            # The plane x + z = 110 halves the cube into two prisms: the upper one lies on its cut face, its far edge
            # 10√2 mm above it; the lower one stays as cut.
            self.piece(f"{s} 6: the upper piece", objs[0], [4000], height=14.14)
            self.piece(f"{s} 6: the lower piece", objs[1], [4000], size=[20, 20, 20], x=[90, 110])
        self.t.equal(f"{s} 6: the reply", [(p["index"], p["open_edges"]) for p in self.pieces_reply(reply)], [(0, 0), (1, 0)])
        self.undo()
        self.whole(f"{s} 7")

        reply = self.run("object_cut", object=0, z=10, keep="upper")
        objs = self.shapes()
        if self.t.equal(f"{s} 8: the objects", [o["name"] for o in objs], ["A"]):
            self.piece(f"{s} 8: the upper piece", objs[0], [4000], size=[20, 20, 10])
        self.t.equal(f"{s} 8: the reply", self.pieces_reply(reply), [{"index": 0, "name": "A", "open_edges": 0}])
        self.undo()
        self.whole(f"{s} 9")

        reply = self.run("object_cut", object=0, z=10, to_parts=True)
        objs = self.shapes()
        if self.t.equal(f"{s} 10: the objects", [o["name"] for o in objs], ["A"]):
            self.piece(f"{s} 10: A", objs[0], [4000, 4000], size=[20, 20, 20], xy=[90, 90, 110, 110])
        self.t.equal(f"{s} 10: the reply's object", {k: reply.get("object", {}).get(k) for k in ("index", "name")},
                     {"index": 0, "name": "A"})
        self.t.equal(f"{s} 10: the reply's parts", len(reply.get("parts", [])), 2)
        self.undo()
        self.whole(f"{s} 11")

        self.run("object_cut", object=0, z=10)
        self.halves(f"{s} 12")
        self.save_and_reopen("cut.3mf")
        self.halves(f"{s} 13: saved and reopened")

    def refused_cuts(self):
        s = "cuts refused"
        self.start(s)
        self.cube([100, 100], name="A")
        self.run("object_select", none=True)
        self.whole(f"{s} 0")
        plane = {"point": [100, 100, 10], "normal": [0, 0, 1]}
        rows = [
            (1, {"z": 50}, "plane_misses_object", None),
            (2, {"keep": "upper"}, "needs_plane", None),
            (3, {"z": 10, "plane": plane}, None, "z or plane, not both"),
            (4, {"plane": {"point": [100, 100, 10], "normal": [0, 0, 0]}}, None, "normal is zero"),
            (5, {"z": 10, "keep": "upper", "lower": "flip"}, None, "the lower piece is not kept"),
            (6, {"z": 10, "keep": "upper", "to_parts": True}, None, "to_parts keeps both pieces"),
            (7, {"z": 10, "to_parts": True, "upper": "flip"}, None, "to_parts places no piece"),
        ]
        for row, args, code, words in rows:
            undo_steps(self.c)
            self.t.refused(f"{s} {row}: refused", lambda: self.c.call("object_cut", object=0, **args), code=code,
                           words=words)
            self.whole(f"{s} {row}")
            self.t.equal(f"{s} {row}: no gizmo open", self.gizmo(), None)
            self.t.equal(f"{s} {row}: the undo steps", undo_steps(self.c), [])

    # ---- Lay on face, brim ears, text and SVG ----

    def instances(self, obj=0):
        """Object `obj`'s instances from object_list, each as {offset, rotation_deg, min, size}."""
        o = self.listed()[obj]
        return [{"offset": i["offset"], "rotation_deg": i["rotation_deg"], "min": i["bbox"]["min"], "size": i["bbox"]["size"]}
                for i in o["instances"]]

    def turned(self, label, want):
        """Checks object 0's instances against `want`, a list of {offset?, rotation, size?}: offsets and sizes to 0.01 mm,
        angles to 0.01 degrees, and every instance resting on the bed."""
        got = self.instances()
        if not self.t.equal(f"{label}: the instances", len(got), len(want)):
            return
        for k, (g, w) in enumerate(zip(got, want)):
            self.t.near(f"{label}: i{k}'s rotation", g["rotation_deg"], w["rotation"], 0.01)
            self.t.near(f"{label}: i{k} rests on the bed", g["min"][2], 0, 0.01)
            if "offset" in w:
                self.t.near(f"{label}: i{k}'s offset", g["offset"], w["offset"], 0.01)
            if "size" in w:
                self.t.near(f"{label}: i{k}'s size", g["size"], w["size"], 0.01)

    def lay_on_face(self):
        s = "lay on face from a direction"
        self.start(s)
        self.cube([100, 100], name="A")
        self.run("object_transform", object=0, rotate=[30, 0, 0], mode="absolute")
        self.run("object_select", none=True)
        tilted = {"offset": [100, 100, 13.66], "rotation": [30, 0, 0], "size": [20, 27.32, 27.32]}
        flat = {"offset": [100, 100, 10], "rotation": [0, 0, 0], "size": [20, 20, 20]}
        self.turned(f"{s} 0", [tilted])

        reply = self.run("object_lay_on_face", object=0)
        self.t.equal(f"{s} 1: the reply", (reply.get("gizmo"), reply.get("active")), ("Flatten", True))
        self.t.equal(f"{s} 1: Lay on Face open", self.gizmo(), {"name": "Flatten", "canvas": "3d"})
        self.turned(f"{s} 1", [tilted])

        undo_steps(self.c)
        reply = self.run("object_lay_on_face", object=0, normal=[0, 0, -1])
        self.turned(f"{s} 2", [flat])
        self.t.equal(f"{s} 2: Lay on Face was closed first", self.gizmo(), None)
        self.t.equal(f"{s} 2: the reply", (reply.get("object"), reply.get("instance")), (0, 0))
        self.t.near(f"{s} 2: the reply's rotation", reply.get("rotation"), [0, 0, 0], 0.01)
        self.t.equal(f"{s} 2: the undo steps", undo_steps(self.c), ["Gizmo-Place on Face"])
        self.undo()
        self.turned(f"{s} 3", [tilted])
        self.redo()
        self.turned(f"{s} 4", [flat])
        self.undo()
        self.turned(f"{s} 5", [tilted])

        self.run("object_lay_on_face", object=0, normal=[0, 0.5, -0.866], frame="world")
        self.turned(f"{s} 6: the world direction of A's -Z face gives the same turn", [flat])
        self.undo()
        self.turned(f"{s} 7", [tilted])

        self.run("object_set_instances", object=0, count=2)
        self.turned(f"{s} 8: the new instance copies i0's turn", [tilted, {"rotation": [30, 0, 0]}])
        self.run("object_lay_on_face", object=0, instance=0, normal=[0, 0, -1])
        self.turned(f"{s} 9: the other instance turns with it", [flat, {"rotation": [0, 0, 0]}])
        self.save_and_reopen("lay.3mf")
        self.turned(f"{s} 10: saved and reopened", [flat, {"rotation": [0, 0, 0]}])

    def brim_file(self, path):
        """The saved project's Metadata/brim_ear_points.txt as {object_id: [(x, y, z, r), ...]}."""
        with zipfile.ZipFile(path) as z:
            text = z.read("Metadata/brim_ear_points.txt").decode("utf-8")
        out = {}
        for line in text.splitlines():
            if not line.startswith("object_id="):
                continue
            head, values = line.split("|", 1)
            numbers = [float(v) for v in values.split()]
            out[int(head.split("=", 1)[1])] = [tuple(numbers[i:i + 4]) for i in range(0, len(numbers), 4)]
        return out

    def ears_near(self, label, got, want):
        """Ears, each (x, y, z, r), against `want` to 0.001."""
        if self.t.equal(f"{label}: the ears' count", len(got), len(want)):
            for k, (g, w) in enumerate(zip(got, want)):
                self.t.near(f"{label}: ear {k}", list(g), list(w), 0.001)

    @staticmethod
    def reply_ears(reply):
        return [tuple(e["position"]) + (e["radius"],) for e in reply.get("ears", [])]

    def brim_ears(self):
        return self.listed()[0]["brim_ears"]

    def setting(self, obj, key):
        item = self.c.call("settings_get", target="object", object=obj, keys=[key])["items"][0]
        return {"value": item.get("value"), "effective": item.get("effective")}

    def ears(self):
        s = "brim ears from positions"
        self.start(s)
        self.cube([100, 100], name="A")
        self.run("object_select", none=True)
        two = [(-10, -10, -10.0001, 3), (10, -10, -10.0001, 4)]
        self.t.equal(f"{s} 0: no ears", self.brim_ears(), 0)
        self.t.equal(f"{s} 0: the object's brim type", self.setting(0, "brim_type"), {"value": None, "effective": "auto_brim"})
        line = self.c.call("settings_get", type="process", keys=["initial_layer_line_width"])["items"][0]["value"]
        self.t.equal(f"{s} 0: the first layer's line width", line, "0.5")

        reply = self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 1: the reply", (reply.get("gizmo"), reply.get("active")), ("Brim Ears", True))
        self.t.equal(f"{s} 1: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.t.equal(f"{s} 1: no ears", self.brim_ears(), 0)

        points = [{"position": [90, 90], "radius": 3}, {"position": [110, 90]}]
        undo_steps(self.c)
        reply = self.run("object_set_brim_ears", object=0, points=points)
        self.t.equal(f"{s} 2: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 2: Brim Ears was closed first", self.gizmo(), None)
        self.ears_near(f"{s} 2: the reply", self.reply_ears(reply), two)
        self.t.equal(f"{s} 2: the reply's brim type", (reply.get("brim_type"), reply.get("takes_effect")), ("auto_brim", False))
        # The two steps the Brim Ears gizmo records as it closes, then the call's.
        self.t.equal(f"{s} 2: the undo steps", undo_steps(self.c), ["Brim ears edit", "Leaving Brim Ears", "Set brim ears"])
        self.undo()
        self.t.equal(f"{s} 3: no ears", self.brim_ears(), 0)
        self.redo()
        self.t.equal(f"{s} 4: two ears", self.brim_ears(), 2)

        self.run("settings_set", target="object", object=0, values={"brim_type": "painted"})
        reply = self.run("object_set_brim_ears", object=0, points=points)
        self.t.equal(f"{s} 5: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 5: the object's brim type", self.setting(0, "brim_type"), {"value": "painted", "effective": "painted"})
        self.ears_near(f"{s} 5: the reply", self.reply_ears(reply), two)
        self.t.equal(f"{s} 5: the reply's brim type", (reply.get("brim_type"), reply.get("takes_effect")), ("painted", True))

        path = os.path.join(self.folder, "ears.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.t.equal(f"{s} 6: two ears", self.brim_ears(), 2)
        saved = self.brim_file(path)
        self.t.equal(f"{s} 6: the file's objects", sorted(saved), [1])
        self.ears_near(f"{s} 6: the file", saved.get(1, []), two)
        self.run("project_new", discard_changes=True)
        self.run("project_open", path=path)
        self.idle()
        self.t.equal(f"{s} 7: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 7: the object's brim type", self.setting(0, "brim_type")["value"], "painted")

        reply = self.run("object_set_brim_ears", object=0, clear=True)
        self.t.equal(f"{s} 8: no ears", self.brim_ears(), 0)
        self.t.equal(f"{s} 8: the reply", reply.get("ears"), [])

        self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 9: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.run("object_set_brim_ears", object=0, points=[{"position": [100, 90], "radius": 5}])
        self.t.equal(f"{s} 9: one ear", self.brim_ears(), 1)
        self.t.equal(f"{s} 9: Brim Ears was closed first", self.gizmo(), None)
        path = os.path.join(self.folder, "ear.3mf")
        self.run("project_save", path=path, overwrite=True)
        saved = self.brim_file(path)
        self.t.equal(f"{s} 9: the file's objects", sorted(saved), [1])
        self.ears_near(f"{s} 9: the call's ear, not what the gizmo held", saved.get(1, []), [(0, -10, -10.0001, 5)])

        self.run("object_set_brim_ears", object=0, points=[{"position": [90, 90], "radius": 3}, {"position": [110, 90], "radius": 3}])
        self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 10: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 10: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        undo_steps(self.c)
        self.t.refused(f"{s} 11: refused",
                       lambda: self.c.call("object_set_brim_ears", object=0, points=[{"position": [100, 90], "radius": 150}]),
                       code="radius_out_of_range")
        self.t.equal(f"{s} 11: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 11: a refused call leaves the gizmo open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.t.equal(f"{s} 11: the undo steps", undo_steps(self.c), [])
        self.t.refused(f"{s} 12: refused",
                       lambda: self.c.call("object_set_brim_ears", object=0, points=[{"position": [100, 90]}], clear=True),
                       words="points or clear, not both")
        self.t.equal(f"{s} 12: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 12: Brim Ears stays open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.run("object_select", none=True)
        self.t.equal(f"{s} 13: two ears", self.brim_ears(), 2)
        self.t.equal(f"{s} 13: no gizmo open", self.gizmo(), None)

    def a_parts(self):
        """A's parts as (type, its text's text or its SVG's file name, else None). A text read back from a project file
        has an svg entry too, with no file, so its text comes first."""
        out = []
        for p in self.listed()[0]["parts"]:
            name = (p.get("text") or {}).get("text")
            if name is None and p.get("svg"):
                name = os.path.basename(p["svg"].get("path") or "")
            out.append((p["type"], name))
        return out

    def text_and_svg(self):
        s = "text and SVG as a part of a named object"
        self.start(s)
        self.cube([100, 100], name="A")
        self.cube([150, 100], name="B")
        self.run("object_select", objects=[1])
        star = os.path.join(self.folder, "star.svg")
        shutil.copyfile(STAR, star)

        def state(label, parts, gizmo=None):
            self.t.equal(f"{label}: A's parts", self.a_parts(), parts)
            self.t.equal(f"{label}: B has one part", len(self.listed()[1]["parts"]), 1)
            if gizmo is not None:
                self.t.equal(f"{label}: the gizmo open", self.gizmo(), {"name": gizmo, "canvas": "3d"})

        state(f"{s} 0", [("part", None)])
        self.t.equal(f"{s} 0: B selected", self.selected(), {"objects": [1], "instances": [[1, 0]]})
        after1 = [("part", None), ("part", "Embossed text")]
        after2 = after1 + [("negative", "Embossed text")]
        # An object keeps its parts before its negative parts and its modifiers (ModelObject::sort_volumes).
        after3 = after1 + [("part", "star.svg"), ("negative", "Embossed text")]
        after4 = after3 + [("modifier", "star.svg")]

        self.run("object_add_text", object=0)
        self.idle()
        state(f"{s} 1", after1, "Text")
        self.run("object_select", none=True)
        self.run("object_add_text", object=0, type="negative")
        self.idle()
        state(f"{s} 2", after2, "Text")
        self.run("object_select", objects=[1])
        self.run("object_add_svg", object=0, path=star)
        self.idle()
        state(f"{s} 3", after3, "SVG")
        self.run("object_select", none=True)
        self.run("object_add_svg", object=0, type="modifier", path=star)
        self.idle()
        state(f"{s} 4", after4, "SVG")
        self.run("object_select", none=True)
        self.undo()
        state(f"{s} 5: adding the part is one undo step", after3)
        self.redo()
        state(f"{s} 6", after4)

        self.run("dialog_auto_answer", rules=[{"match": ".", "button": "ok", "path": star}])
        self.run("object_add_svg", object=0, type="modifier")
        self.idle()
        rules = self.c.call("app_state")["auto_answer_rules"]
        self.t.equal(f"{s} 7: the file picker opened", [r.get("uses") for r in rules], [1])
        state(f"{s} 7: nothing was added", after4)
        self.run("dialog_auto_answer", rules=[])
        self.save_and_reopen("parts.3mf")
        state(f"{s} 8: saved and reopened", after4)

    def run_all(self, only=()):
        self.run("action_run", id="orca_command:mode_expert:orca")
        self.t.equal("Expert mode", self.c.call("app_state")["mode"], "expert")
        scenarios = [("opening a gizmo on a target", self.opening), ("openings refused", self.refused_openings),
                     ("cut from arguments", self.cut), ("cuts refused", self.refused_cuts),
                     ("lay on face from a direction", self.lay_on_face), ("brim ears from positions", self.ears),
                     ("text and SVG as a part of a named object", self.text_and_svg)]
        for name, scenario in scenarios:
            if not only or name in only:
                self.t.scenario(name, scenario)
        self.run("project_new", discard_changes=True)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-gizmo-commands-") as folder:
        return GizmoCommandsTest(client, folder).run_all(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
