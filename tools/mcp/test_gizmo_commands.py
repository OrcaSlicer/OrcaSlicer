#!/usr/bin/env python3
"""The commands that open a gizmo on a named target, as the test_gizmo_commands entry of
tools/mcp/test-entries/gizmos.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_gizmo_commands.py [scenario ...]

A call that names only its target opens the gizmo on it, and the same call again leaves it open; a refused opening
leaves the model, the gizmo and the selection as it found them. After every step the test reads the state back through
another tool (object_list, app_state) and compares it with the values the entry writes out. It runs on the harness's
default printer, the Bambu Lab X1 Carbon 0.4, and exits 0 when every check passes. Scenario names given on the command
line run those scenarios alone.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import Checks, run, selection, slots, undo_steps, wait  # noqa: E402


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
        listing = self.c.call("object_list", limit=500)
        objs = sorted([o for p in listing["plates"] for o in p["objects"]] + listing.get("outside", []),
                      key=lambda o: o["index"])
        return [{"name": o["name"], "offset": [round(v, 3) for v in o["instances"][0]["offset"]], "parts": len(o["parts"])}
                for o in objs]

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

    def run_all(self, only=()):
        self.run("action_run", id="orca_command:mode_expert:orca")
        self.t.equal("Expert mode", self.c.call("app_state")["mode"], "expert")
        scenarios = [("opening a gizmo on a target", self.opening), ("openings refused", self.refused_openings)]
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
