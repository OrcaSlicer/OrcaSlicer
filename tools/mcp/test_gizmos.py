#!/usr/bin/env python3
"""The Prepare view's gizmos, driven through their panels and through the commands that open them, and the canvas
input they rely on, as the test_gizmos entry of tools/mcp/test-entries/gizmos.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_gizmos.py [scenario ...]

Each gizmo is driven as a person drives it, through the generic GUI tools: its icon, the fields, combos, buttons
and links of its panel, and clicks and strokes at points of the model. The commands that open a gizmo on a named
target are called too: a call that names only its target opens the gizmo on it and the same call again leaves it
open, a refused call leaves the model, the gizmo and the selection as it found them, and a call with an acting
argument closes the open gizmo and does the job as the gizmo's own button would. After every step the test reads
back what the gizmo or the call leaves on the model (placements, painted facets, texture layers, text and SVG parts,
brim ears, parts made or removed, positions in the assemble view), reads the undo before any redo, and reads what a
person keeps again from a saved project. It also tests the canvas input those gizmos rely on, and auto-answer rules
on the preset prompt. It starts from the baseline on the Generic Klipper Printer, reads back the window size and the
preferences its checks depend on, sets Expert mode, and exits 0 when every check passes. Scenario names given on the
command line run those scenarios alone.
"""

import math
import os
import re
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, SLOT_COLOURS, Checks, add_slot, run, selection,  # noqa: E402
                    slots, start, undo_steps, wait)

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures")
STAR = os.path.join(FIXTURES, "star.svg")
HEIGHTMAP = os.path.join(FIXTURES, "heightmap.png")
STANDARD = KLIPPER_STD
FINE = "0.16mm Optimal @MyKlipper"
FUZZY_WARNING = "Warning: Fuzzy skin is disabled, painted fuzzy skin will not take effect!"
BRIM_WARNING = 'Warning: The brim type is not set to "painted", the brim ears will not take effect!'

# One line of widget_list's tree (format_snapshot in UiRefs.cpp): [ref] kind "label" = "value" (class) name=...
# option=... in "window" [flags] @x,y wxh. Labels and values are not escaped, so the label is matched lazily and
# the rest of the line decides where it ends.
LINE = re.compile(r'^ *\[(?P<ref>[^\]]*)\] (?P<kind>\w+)(?: "(?P<label>.*?)")?(?: = "(?P<value>.*?)")?'
                  r'(?: \((?P<cls>[^()]*)\))?(?: name=(?P<name>.*?))?(?: option=\S+)?(?: in ".*?")?'
                  r'(?P<flags>(?: \[[^\]]*\])*)(?: @-?\d+,-?\d+ \d+x\d+)?$')


class Positive:
    """A count the app works out from a stroke: any number above 0 matches it."""

    def __eq__(self, other):
        return isinstance(other, (int, float)) and not isinstance(other, bool) and other > 0

    def __repr__(self):
        return ">0"


class Anything:
    """A value the scenario does not fix."""

    def __eq__(self, other):
        return True

    def __repr__(self):
        return "?"


POS, ANY = Positive(), Anything()


# The imported height map's name: the stem of the file the import writes into the data folder's texture library,
# heightmap or, when the library already holds heightmap.png, heightmap (n) with n from 2.
HEIGHTMAP_NAME = re.compile(r"heightmap(?: \((?:[2-9]|[1-9]\d+)\))?")


def layers(*rows):
    """Texture layers written as the entry writes them: "0 H >0" is the layer in slot 0, the imported height map, with
    painted facets; "?" is a name the scenario does not fix."""
    out = []
    for row in rows:
        slot, name, painted = row.split()
        out.append({"slot": int(slot), "name": ANY if name == "?" else name,
                    "painted": POS if painted == ">0" else int(painted)})
    return out


def centre(box):
    return [(lo + hi) / 2 for lo, hi in zip(box["min"], box["max"])]


class GizmosTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Calls ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def answered(self, tool, **args):
        """A call whose dialog an auto-answer rule answers, followed to its end. The rule presses its button once
        the dialog's modal loop runs, so the first reply may be blocked_by_dialog, which run() would fail."""
        return self.c.await_job(self.c.call(tool, **args), timeout=120)

    def idle(self, timeout_s=45):
        wait(self.c, "idle", timeout_s)

    def undo(self):
        self.run("undo", history="project")

    def redo(self):
        self.run("redo", history="project")

    def camera(self, direction, fit="fit_objects"):
        self.run("view_set", camera=direction)
        self.run("view_set", camera=fit)

    def cube(self, at, size=20, name=None):
        """A cube, or a box when `size` is [x, y, z], resting on the bed at `at`."""
        args = {"shape": "cube", "size": [size, size, size] if isinstance(size, (int, float)) else size,
                "position": [at[0], at[1], 0]}
        if name:
            args["name"] = name
        return self.run("object_add_primitive", **args)

    def filament_slots(self, count):
        """Deletes filament slots until a delete is refused (one is left), then adds slots up to `count`, slot n
        coloured SLOT_COLOURS[n - 1]."""
        for _ in range(32):
            try:
                self.c.call("filament_delete")
            except orca_mcp.McpError:
                break
        for n in range(2, count + 1):
            add_slot(self.c, SLOT_COLOURS[n - 1])

    def click(self, at, **args):
        """A left click at a point of the model, `at` being mouse_gesture's `at`."""
        return self.c.call("mouse_gesture", at=at, **args)

    def press(self, target):
        """widget_click on an ImGui item: {"imgui": label}, {"imgui_name": name}, or with an index."""
        return self.c.call("widget_click", target=target)

    def set(self, target, value):
        return self.c.call("widget_set_value", target=target, value=value)

    def edit(self, label, target, value):
        """widget_set_value on a field that shows another value once the edit is applied; the edit must go
        through, and the state it sets is read back after it."""
        try:
            self.set(target, value)
        except orca_mcp.McpError as e:
            return self.t.check(f"{label}: the edit goes through", False, e)
        return self.t.check(f"{label}: the edit goes through", True)

    def open(self, label, name, canvas="3d"):
        target = {"gizmo": name, **({"canvas": canvas} if canvas != "3d" else {})}
        reply = self.c.call("widget_click", target=target)
        self.t.equal(f"{label}: {name} opens", reply.get("active"), True)
        self.t.equal(f"{label}: the gizmo open", self.gizmo(), {"name": name, "canvas": canvas})
        return reply

    def close(self, label, name, canvas="3d"):
        target = {"gizmo": name, **({"canvas": canvas} if canvas != "3d" else {})}
        reply = self.c.call("widget_click", target=target)
        self.t.equal(f"{label}: {name} closes", reply.get("active"), False)
        self.t.equal(f"{label}: no gizmo open", self.gizmo(), None)

    def close_by(self, label, button):
        """Closes the open gizmo by its panel's Done, Close or Return button."""
        self.press({"imgui": button} if button != "close" else {"imgui_name": button})
        self.t.equal(f"{label}: no gizmo open after {button}", self.gizmo(), None)

    def save_and_reopen(self, name):
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.run("project_open", path=path)
        self.idle()

    # ---- Reads ----

    def listed(self):
        """object_list's objects, on the plates and outside them, in index order."""
        listing = self.c.call("object_list", limit=500, detail=True)
        return sorted([o for p in listing["plates"] for o in p["objects"]] + listing.get("outside", []),
                      key=lambda o: o["index"])

    def scene(self):
        """Each object as {name, offset, rotation_deg, scale, bbox, brim_ears, overrides, layer_height_profile,
        parts}, the first four of its instance 0, each part as {name, type, triangles, mesh_volume, bbox, painted,
        text, svg}."""
        out = []
        for o in self.listed():
            inst = o["instances"][0]
            out.append({"name": o["name"], "offset": inst["offset"], "rotation_deg": inst["rotation_deg"],
                        "scale": inst["scale"], "bbox": inst["bbox"], "brim_ears": o["brim_ears"],
                        "overrides": o["overrides"], "layer_height_profile": o.get("layer_height_profile"),
                        "parts": [{k: p.get(k) for k in ("name", "type", "triangles", "mesh_volume", "bbox", "painted",
                                                         "text", "svg")} for p in o["parts"]]})
        return out

    def objects(self):
        """Each object in index order as {name, offset, parts}: its instance 0's offset to 0.001 mm, its parts'
        count."""
        return [{"name": o["name"], "offset": [round(v, 3) for v in o["instances"][0]["offset"]], "parts": len(o["parts"])}
                for o in self.listed()]

    def shapes(self):
        """Each object in index order as {name, offset, min, max, size, parts}: its instance 0's offset and bbox, and its
        parts as (type, mesh_volume)."""
        out = []
        for o in self.listed():
            inst = o["instances"][0]
            out.append({"name": o["name"], "offset": inst["offset"], "min": inst["bbox"]["min"], "max": inst["bbox"]["max"],
                        "size": inst["bbox"]["size"], "parts": [(p["type"], p["mesh_volume"]) for p in o["parts"]]})
        return out

    def instances(self, obj=0):
        """Object `obj`'s instances, each as {offset, rotation_deg, min, size}."""
        return [{"offset": i["offset"], "rotation_deg": i["rotation_deg"], "min": i["bbox"]["min"], "size": i["bbox"]["size"]}
                for i in self.listed()[obj]["instances"]]

    def gizmo(self):
        g = self.c.call("app_state").get("gizmo")
        return None if g is None else {"name": g.get("name"), "canvas": g.get("canvas")}

    def nodes(self, **args):
        """widget_list's nodes, each as the dict of LINE's groups."""
        _, texts = self.c.call_with_text("widget_list", **args)
        return [m.groupdict() for m in map(LINE.match, (texts[0] if texts else "").splitlines()) if m]

    def panel(self):
        """The open gizmo's ImGui panel, the largest ImGui window that is not a notification: {"texts": its texts in
        order, "items": {name (else label): the value it shows}}."""
        windows, current = [], None
        for n in self.nodes(target="main", include_menus=False, include_web=False, max_nodes=3000):
            if n["kind"] == "imgui_window":
                current = None if (n["label"] or "").startswith("!!") else {"texts": [], "items": {}}
                if current is not None:
                    windows.append(current)
            elif current is not None and n["kind"] == "text":
                current["texts"].append(n["label"])
            elif current is not None and n["kind"] == "imgui":
                current["items"][n["name"] or n["label"]] = n["value"]
            elif n["kind"] not in ("text", "imgui"):
                current = None
        return max(windows, key=lambda w: len(w["texts"]) + len(w["items"]), default={"texts": [], "items": {}})

    def assembly(self):
        """Each object's position in the assemble view, assemble_offset() of its instance 0."""
        reply = self.run("script_run", code="import orca\nresult = [list(o.instances()[0].assemble_offset()) "
                                             "for o in orca.host.model().objects()]")
        assert reply.get("status") == "finished", reply
        return reply["result"]

    def setting(self, obj, key):
        item = self.c.call("settings_get", target="object", object=obj, keys=[key])["items"][0]
        return {"value": item.get("value"), "effective": item.get("effective"), "source": item.get("source")}

    def selection(self):
        return self.c.call("app_state")["selection"]["objects"]

    def selected(self):
        """checks.selection() without the instances of a part selection, which the entry does not fix."""
        sel = selection(self.c)
        return {"parts": sel["parts"]} if "parts" in sel else {"objects": sel["objects"], "instances": sel["instances"]}

    def rules(self):
        return [{k: r.get(k) for k in ("match", "button", "uses")} for r in self.c.call("app_state")["auto_answer_rules"]]

    def toolbar(self, name):
        for n in self.nodes(target="main", query=name, include_web=False):
            if n["kind"] == "gltoolbar" and n["name"] == name:
                return {"enabled": "[disabled]" not in n["flags"], "pressed": "[checked]" in n["flags"]}
        return None

    def slots(self):
        return slots(self.c)

    def process(self):
        """(the selected process preset, its wall_loops)."""
        name = self.c.call("app_state")["presets"]["process"]
        return name, self.c.call("settings_get", type="process", keys=["wall_loops"])["items"][0]["value"]

    def ear_count(self):
        """Object 0's number of brim ears."""
        return self.listed()[0]["brim_ears"]

    @staticmethod
    def brim_file(path):
        """The saved project's Metadata/brim_ear_points.txt as {object_id: [(x, y, z, r), ...]}; {} when the project
        has no ears and so no such file."""
        with zipfile.ZipFile(path) as z:
            if "Metadata/brim_ear_points.txt" not in z.namelist():
                return {}
            text = z.read("Metadata/brim_ear_points.txt").decode("utf-8")
        out = {}
        for line in text.splitlines():
            if not line.startswith("object_id="):
                continue
            head, values = line.split("|", 1)
            numbers = [float(v) for v in values.split()]
            out[int(head.split("=", 1)[1])] = [tuple(numbers[i:i + 4]) for i in range(0, len(numbers), 4)]
        return out

    def a_parts(self):
        """Object 0's parts as (type, its text's text or its SVG's file name, else None). A text read back from a
        project file has an svg entry too, with no file, so its text comes first."""
        out = []
        for p in self.listed()[0]["parts"]:
            name = (p.get("text") or {}).get("text")
            if name is None and p.get("svg"):
                name = os.path.basename(p["svg"].get("path") or "")
            out.append((p["type"], name))
        return out

    @staticmethod
    def fields(reply, keys):
        """The fields of `reply` a row fixes, a target in it by its index or object, instance, part and name."""
        out = {}
        for key in keys:
            value = reply.get(key)
            if isinstance(value, list):
                value = [{k: v for k, v in item.items() if k not in ("object_id", "part_id", "instance_id")} for item in value]
            out[key] = value
        return out

    # ---- Checks ----

    def placed(self, label, obj, offset=None, rot=None, scale=None, size=None, size_tol=0.001):
        """Object `obj` of scene() (a dict) at the placement a row writes: offset to 0.001 mm, rot to 0.01°, scale
        on all three axes, and size to `size_tol`."""
        name = obj["name"]
        if offset is not None:
            self.t.near(f"{label}: {name}'s offset", obj["offset"], offset, 0.001)
        if rot is not None:
            self.t.near(f"{label}: {name}'s rotation", obj["rotation_deg"], rot, 0.01)
        if scale is not None:
            self.t.near(f"{label}: {name}'s scale", obj["scale"], [scale] * 3, 0.001)
        if size is not None:
            self.t.near(f"{label}: {name}'s size", obj["bbox"]["size"], size, size_tol)

    def one(self, label, offset=None, rot=None, scale=None, size=None, size_tol=0.001):
        """The scene holds one object, placed as the row writes."""
        objs = self.scene()
        if self.t.equal(f"{label}: one object", len(objs), 1):
            self.placed(label, objs[0], offset, rot, scale, size, size_tol)
        return objs

    def start(self, name):
        self.run("project_new", discard_changes=True)
        self.run("view_set", tab="prepare")
        self.idle()
        self.t.equal(f"{name}: the start: no objects", self.scene(), [])
        self.t.equal(f"{name}: the start: no gizmo open", self.gizmo(), None)

    # ---- Scenarios ----

    def prompts(self):
        s = "prompts a rule answers"
        self.start(s)
        self.t.equal(f"{s} 0: the process", self.process(), (STANDARD, "3"))
        self.t.equal(f"{s} 0: no rule", self.rules(), [])
        transfer = {"match": "Transfer or discard", "button": "Transfer"}
        self.run("dialog_auto_answer", rules=[transfer])
        self.run("preset_select", type="process", name=STANDARD)
        self.t.equal(f"{s} 1: the process", self.process(), (STANDARD, "3"))
        self.t.equal(f"{s} 1: the rule has answered nothing", self.rules(), [{**transfer, "uses": 0}])
        self.run("settings_set", type="process", values={"wall_loops": 5})
        self.t.equal(f"{s} 2: the process", self.process(), (STANDARD, "5"))
        reply = self.answered("preset_select", type="process", name=FINE)
        self.t.equal(f"{s} 3: the reply's selected", reply.get("selected"), FINE)
        self.t.equal(f"{s} 3: Transfer moved the edit", self.process(), (FINE, "5"))
        self.t.equal(f"{s} 3: the prompt was asked once", self.rules(), [{**transfer, "uses": 1}])
        discard = {"match": "Transfer or discard", "button": "Discard"}
        self.run("dialog_auto_answer", rules=[discard])
        self.t.equal(f"{s} 4: the new rule has answered nothing", self.rules(), [{**discard, "uses": 0}])
        reply = self.answered("preset_select", type="process", name=STANDARD)
        self.t.equal(f"{s} 4: the reply's selected", reply.get("selected"), STANDARD)
        self.t.equal(f"{s} 4: Discard dropped the edit", self.process(), (STANDARD, "3"))
        self.t.equal(f"{s} 4: the prompt was asked once", self.rules(), [{**discard, "uses": 1}])
        self.t.equal(f"{s} 4: the project is not dirty", self.c.call("app_state")["project"]["dirty"], False)
        self.run("dialog_auto_answer", rules=[])

    def move_rotate_scale(self):
        s = "Move, Rotate and Scale"
        self.start(s)
        self.cube([100, 100], name="A")
        self.camera("iso")
        self.one(f"{s} 0", [100, 100, 10], [0, 0, 0], 1, [20, 20, 20])
        self.t.equal(f"{s} 0: the selection", self.selection(), [0])
        self.open(f"{s} 1", "Move")
        self.one(f"{s} 1", [100, 100, 10], [0, 0, 0], 1, [20, 20, 20])
        reply = self.set({"imgui_name": "position_x"}, 90)
        self.t.equal(f"{s} 2: the reply", reply.get("value"), "90.00")
        self.one(f"{s} 2", [90, 100, 10])
        self.undo()
        self.one(f"{s} 3", [100, 100, 10])
        self.redo()
        self.one(f"{s} 4", [90, 100, 10])
        self.close(f"{s} 5", "Move")
        self.run("object_transform", object=0, rotate=[0, 0, 90], mode="absolute")
        self.open(f"{s} 5", "Move")
        reply = self.set({"imgui_name": "coordinates"}, "Object")
        self.t.equal(f"{s} 5: the reply", reply.get("selected"), "Object")
        self.one(f"{s} 5", [90, 100, 10], [0, 0, 90], None, [20, 20, 20])
        panel = self.panel()
        self.t.check(f"{s} 5: the caption reads Relative", "Relative" in panel["texts"] and "Position" not in
                     panel["texts"], panel["texts"])
        self.t.equal(f"{s} 5: position_x reads", panel["items"].get("position_x"), "0.00")
        # The relative field applies the move and shows 0.00 again.
        self.edit(f"{s} 6", {"imgui_name": "position_x"}, 10)
        self.one(f"{s} 6: the move goes along A's own X axis", [90, 110, 10], [0, 0, 90])
        reply = self.set({"imgui_name": "coordinates"}, 0)
        self.t.equal(f"{s} 7: the reply", reply.get("selected"), "World")
        self.one(f"{s} 7", [90, 110, 10], [0, 0, 90])
        panel = self.panel()
        self.t.check(f"{s} 7: the caption reads Position", "Position" in panel["texts"] and "Relative" not in
                     panel["texts"], panel["texts"])
        self.t.equal(f"{s} 7: position_x reads", panel["items"].get("position_x"), "90.00")
        self.close(f"{s} 8", "Move")
        self.open(f"{s} 8", "Rotate")
        reply = self.set({"imgui_name": "absolute_rotation_z"}, 30)
        self.t.equal(f"{s} 8: the reply", reply.get("value"), "30.00")
        turned = 20 * (math.cos(math.radians(30)) + math.sin(math.radians(30)))
        after8 = ([90, 110, 10], [0, 0, 30], None, [turned, turned, 20], 0.01)
        self.one(f"{s} 8", *after8)
        self.undo()
        self.one(f"{s} 9", None, [0, 0, 90], None, [20, 20, 20])
        self.redo()
        self.one(f"{s} 10", *after8)
        # The panel shows the turn it leaves as [180, 180, 180], which is no turn at all.
        self.edit(f"{s} 11", {"imgui_name": "absolute_rotation_z"}, 0)
        self.one(f"{s} 11",[90, 110, 10], [0, 0, 0], None, [20, 20, 20])
        self.close(f"{s} 12", "Rotate")
        self.open(f"{s} 12", "Scale")
        reply = self.set({"imgui_name": "scale_x"}, 120)
        self.t.equal(f"{s} 12: the reply", reply.get("value"), "120.00")
        objs = self.one(f"{s} 12: uniform scale", [90, 110, 12], None, 1.2, [24, 24, 24])
        if objs:
            self.t.near(f"{s} 12: A rests on the bed", objs[0]["bbox"]["min"][2], 0, 0.001)
        self.undo()
        self.one(f"{s} 13", [90, 110, 10], None, 1, [20, 20, 20])
        self.redo()
        self.one(f"{s} 14", [90, 110, 12], None, 1.2, [24, 24, 24])
        self.set({"imgui_name": "scale_x"}, 100)
        self.one(f"{s} 15", [90, 110, 10], None, 1, [20, 20, 20])
        self.close(f"{s} 16", "Scale")
        self.one(f"{s} 16", [90, 110, 10], None, 1, [20, 20, 20])

    def align(self):
        s = "aligning in the Move panel"
        self.start(s)
        self.cube([100, 100], name="A")
        self.cube([150, 150], 40, name="B")

        def both(label, b_offset):
            objs = self.scene()
            if self.t.equal(f"{label}: two objects", [o["name"] for o in objs], ["A", "B"]):
                self.placed(label, objs[0], [100, 100, 10], size=[20, 20, 20])
                self.placed(label, objs[1], b_offset, size=[40, 40, 40])

        both(f"{s} 0", [150, 150, 20])
        self.run("object_select", all=True)
        self.open(f"{s} 1", "Move")
        self.t.equal(f"{s} 1: the selection", self.selection(), [0, 1])
        both(f"{s} 1", [150, 150, 20])
        self.press({"imgui_name": "Align front"})
        both(f"{s} 2: on the front-most edge", [150, 110, 20])
        self.undo()
        both(f"{s} 3: one undo step", [150, 150, 20])
        self.redo()
        both(f"{s} 4", [150, 110, 20])
        self.close(f"{s} 5", "Move")
        both(f"{s} 5", [150, 110, 20])

    def lay_on_face(self):
        s = "lay on face"
        self.start(s)
        self.cube([100, 100], [20, 20, 40], name="P")
        self.camera("iso", "fit_selection")
        after0 = ([100, 100, 20], [0, 0, 0], None, [20, 20, 40])
        self.one(f"{s} 0", *after0)
        self.open(f"{s} 1", "Flatten")
        self.one(f"{s} 1", *after0)
        self.click({"object": 0, "spot": "front"})
        after2 = ([100, 100, 10], [90, 0, 0], None, [20, 40, 20])
        objs = self.one(f"{s} 2", *after2)
        if objs:
            self.t.near(f"{s} 2: the front face lies on the bed", objs[0]["bbox"]["min"][2], 0, 0.001)
        self.undo()
        self.one(f"{s} 3", *after0)
        self.redo()
        self.one(f"{s} 4", *after2)
        self.close(f"{s} 5", "Flatten")
        self.one(f"{s} 5", *after2)

    def painted(self, label, want):
        """Part 0's painted facets, `want` being None for none."""
        objs = self.scene()
        got = objs[0]["parts"][0]["painted"] if objs else "no object"
        self.t.equal(f"{label}: painted", got, want)

    def texture(self, label, want, depth=None):
        """Part 0's texture layers as {slot, name, painted}, compared with `want`, None for no painting; with
        `depth`, every layer's depth_mm too. A layer is named "H" when it is the imported height map: its name is one
        HEIGHTMAP_NAME matches and its path is that name's file in the data folder's texture library."""
        objs = self.scene()
        painted = objs[0]["parts"][0]["painted"] if objs else "no object"
        library = os.path.join(self.c.call("app_state")["config_folder"], "textures", "displacement")

        def name(layer):
            path = os.path.join(library, layer["name"] + ".png")
            same = os.path.normcase(os.path.abspath(layer.get("path") or "")) == os.path.normcase(path)
            return "H" if HEIGHTMAP_NAME.fullmatch(layer["name"]) and same else layer["name"]

        got = None if painted is None else [{"slot": l["slot"], "name": name(l), "painted": l.get("painted")}
                                            for l in painted.get("texture", [])]
        self.t.equal(f"{label}: the texture layers", got, want)
        if depth is not None and painted:
            self.t.near(f"{label}: depth_mm", [l["depth_mm"] for l in painted.get("texture", [])],
                        [depth] * len(painted.get("texture", [])), 1e-6)
        return painted

    def painting(self):
        s = "painting and texture layers"
        self.start(s)
        self.filament_slots(2)
        self.cube([100, 100])
        self.run("settings_set", target="object", object=0, values={"enable_support": True})
        self.camera("iso")
        objs = self.one(f"{s} 0", [100, 100, 10], size=[20, 20, 20])
        if objs:
            self.t.equal(f"{s} 0: one part of 12 triangles", [p["triangles"] for p in objs[0]["parts"]], [12])
        self.painted(f"{s} 0", None)
        self.t.equal(f"{s} 0: two filament slots", self.slots(), "0,0")
        self.t.equal(f"{s} 0: support on for the object", self.setting(0, "enable_support"),
                     {"value": "1", "effective": "1", "source": "object"})
        self.open(f"{s} 1", "FdmSupports")
        self.press({"imgui_name": "Fill"})
        self.click({"object": 0, "spot": "front"})
        self.painted(f"{s} 1", {"supports": {"enforcers": 2, "blockers": 0}})
        self.undo()
        self.painted(f"{s} 2", None)
        self.redo()
        self.painted(f"{s} 3", {"supports": {"enforcers": 2, "blockers": 0}})
        self.press({"imgui_name": "Circle"})
        self.set({"imgui": "Brush size"}, 3)
        self.c.call("mouse_gesture", action="drag", button="right", at={"object": 0, "spot": "top"},
                    to={"object": 0, "point": [5, 5, 10]})
        supports = {"enforcers": 2, "blockers": POS}
        self.painted(f"{s} 4: the right button paints blockers", {"supports": supports})
        self.close_by(f"{s} 5", "Done")
        self.painted(f"{s} 5", {"supports": supports})
        self.open(f"{s} 6", "Seam")
        self.click({"object": 0, "spot": "front"})
        seam = {"supports": supports, "seam": {"enforcers": POS, "blockers": 0}}
        self.painted(f"{s} 6", seam)
        self.undo()
        self.painted(f"{s} 7", {"supports": supports})
        self.redo()
        self.close_by(f"{s} 8", "Done")
        self.painted(f"{s} 8", seam)
        self.run("settings_set", target="object", object=0, values={"fuzzy_skin": "disabled_fuzzy"})
        self.open(f"{s} 9", "Fuzzy Skin Painting")
        self.painted(f"{s} 9", seam)
        self.t.equal(f"{s} 9: fuzzy skin is off for the object", self.setting(0, "fuzzy_skin"),
                     {"value": "disabled_fuzzy", "effective": "disabled_fuzzy", "source": "object"})
        self.t.check(f"{s} 9: the panel warns", FUZZY_WARNING in self.panel()["texts"], self.panel()["texts"])
        self.press({"imgui_name": "Enable painted fuzzy skin"})
        self.painted(f"{s} 10", seam)
        self.t.equal(f"{s} 10: fuzzy skin is Painted only", self.setting(0, "fuzzy_skin"),
                     {"value": "none", "effective": "none", "source": "object"})
        self.t.check(f"{s} 10: the warning is gone", FUZZY_WARNING not in self.panel()["texts"], self.panel()["texts"])
        self.press({"imgui_name": "Fill"})
        self.click({"object": 0, "spot": "left"})
        fuzzy = {**seam, "fuzzy_skin": 2}
        self.painted(f"{s} 11", fuzzy)
        self.undo()
        self.painted(f"{s} 12", seam)
        self.t.equal(f"{s} 12: the link took no undo step of its own", self.setting(0, "fuzzy_skin")["value"], "none")
        self.redo()
        self.close_by(f"{s} 13", "Done")
        self.painted(f"{s} 13", fuzzy)
        self.open(f"{s} 14", "Color Painting")
        self.press({"imgui_name": "extruder_color_2"})
        self.press({"imgui_name": "Fill"})
        self.click({"object": 0, "spot": "front"})
        color = {**fuzzy, "mmu": {"2": 2}}
        self.painted(f"{s} 14", color)
        self.undo()
        self.painted(f"{s} 15", fuzzy)
        self.redo()
        self.close_by(f"{s} 16", "Done")
        self.painted(f"{s} 16", color)
        # Texture Displacement on the same part, still selected, which has painting and no texture layer.
        self.open(f"{s} 17", "Texture Displacement")
        self.texture(f"{s} 17: one layer in slot 0", layers("0 ? 0"))
        self.run("gizmo_pick_file", action="texture_import", path=HEIGHTMAP)
        self.texture(f"{s} 18", layers("0 H 0"), depth=0.4)
        self.press({"imgui_name": "add_layer"})
        self.texture(f"{s} 19", layers("0 H 0", "1 ? 0"))
        self.c.call("mouse_gesture", action="drag", at={"object": 0, "spot": "top"},
                    waypoints=[{"object": 0, "point": [6, 0, 10]}], to={"object": 0, "point": [6, 6, 10]})
        self.idle()
        self.texture(f"{s} 20: the stroke paints the active layer, slot 1", layers("0 H 0", "1 ? >0"))
        self.press({"imgui_name": "Layer 1"})
        self.texture(f"{s} 21", layers("0 H 0", "1 ? >0"))
        self.t.check(f"{s} 21: the open card shows midlevel", "midlevel" in self.panel()["items"], self.panel()["items"])
        self.c.call("mouse_gesture", action="drag", at={"object": 0, "spot": "front"},
                    waypoints=[{"object": 0, "point": [6, -10, 0]}], to={"object": 0, "point": [6, -10, 6]})
        self.idle()
        self.texture(f"{s} 22: the stroke paints slot 0", layers("0 H >0", "1 ? >0"))
        self.press({"imgui_name": "more"})
        self.texture(f"{s} 23", layers("0 H >0", "1 ? >0"))
        self.t.check(f"{s} 23: the link folded the settings", "midlevel" not in self.panel()["items"],
                     self.panel()["items"])
        self.press({"imgui_name": "Remove", "index": 1})
        self.texture(f"{s} 24: slot 1 is gone", layers("0 H >0"))
        self.undo()
        self.texture(f"{s} 25", layers("0 H >0", "1 ? >0"))
        self.redo()
        self.texture(f"{s} 26", layers("0 H >0"))
        self.close_by(f"{s} 27", "close")
        painted = self.texture(f"{s} 27", layers("0 H >0"))
        self.t.equal(f"{s} 27: the other painting stays", {k: v for k, v in (painted or {}).items() if k != "texture"},
                     color)
        self.save_and_reopen("painting.3mf")
        painted = self.texture(f"{s} 28: the project file keeps the layer", layers("0 H >0"), depth=0.4)
        self.t.equal(f"{s} 28: the project file keeps the painting",
                     {k: v for k, v in (painted or {}).items() if k != "texture"}, color)
        self.t.equal(f"{s} 28: two filament slots", self.slots(), "0,0")
        self.t.equal(f"{s} 28: the project file keeps support on", self.setting(0, "enable_support")["value"], "1")
        self.t.equal(f"{s} 28: the project file keeps Painted only", self.setting(0, "fuzzy_skin")["value"], "none")

    def text_part(self, label, kind, text, style, z):
        """Part 1, the text: its type, text.text, text.style and bbox z range to 0.01 mm."""
        objs = self.scene()
        parts = objs[0]["parts"] if objs else []
        if not self.t.equal(f"{label}: two parts", len(parts), 2):
            return None
        part = parts[1]
        got = (part["type"], (part["text"] or {}).get("text"), (part["text"] or {}).get("style"))
        self.t.equal(f"{label}: the text part", got, (kind, text, style))
        self.t.near(f"{label}: the text's z range", [part["bbox"]["min"][2], part["bbox"]["max"][2]], z, 0.01)
        return part

    def style_combo(self):
        return self.panel()["items"].get("style_selector")

    def text(self):
        s = "text"
        self.start(s)
        self.cube([100, 100])
        self.camera("top")
        objs = self.one(f"{s} 0", [100, 100, 10], size=[20, 20, 20])
        if objs:
            self.t.equal(f"{s} 0: one part", len(objs[0]["parts"]), 1)
        reply = self.open(f"{s} 1", "Text")
        self.t.equal(f"{s} 1: the reply says the part was created", reply.get("created"), True)
        self.idle()
        part = self.text_part(f"{s} 1", "part", "Embossed text", ANY, [19.985, 20.985])
        if part:
            self.t.equal(f"{s} 1: the part's name", part["name"], "Embossed text")
            x, y, _ = centre(part["bbox"])
            self.t.check(f"{s} 1: the text lands on the cube's top", 90 <= x <= 110 and 90 <= y <= 110, (x, y))
        reply = self.set({"imgui_name": "Text"}, "Orca\nMCP")
        self.idle()
        self.t.equal(f"{s} 2: the reply", reply.get("value"), "Orca\nMCP")
        part = self.text_part(f"{s} 2", "part", "Orca\nMCP", ANY, [19.985, 20.985])
        if part:
            self.t.equal(f"{s} 2: the part's name", part["name"], "Orca MCP")
        reply = self.set({"imgui": "Style"}, "ITALIC")
        self.idle()
        self.t.equal(f"{s} 3: the reply", reply.get("selected"), "ITALIC")
        self.text_part(f"{s} 3", "part", "Orca\nMCP", "ITALIC", [19.985, 20.985])
        self.t.equal(f"{s} 3: the style combo", self.style_combo(), "ITALIC")
        self.set({"imgui_name": "Combo_Font"}, "HarmonyOS Sans SC")
        self.idle()
        part = self.text_part(f"{s} 4", "part", "Orca\nMCP", "ITALIC", [19.985, 20.985])
        if part:
            self.t.equal(f"{s} 4: the font", part["text"]["font"], "HarmonyOS Sans SC")
        combo = self.style_combo()
        self.t.check(f"{s} 4: the style combo is marked modified", (combo or "").startswith("ITALIC*"), combo)
        self.press({"imgui_name": "Revert font"})
        self.idle()
        part = self.text_part(f"{s} 5", "part", "Orca\nMCP", "ITALIC", [19.985, 20.985])
        if part:
            self.t.check(f"{s} 5: the font is reverted", part["text"]["font"] != "HarmonyOS Sans SC", part["text"])
        self.t.equal(f"{s} 5: the style combo", self.style_combo(), "ITALIC")
        reply = self.set({"imgui": "Depth"}, 2.5)
        self.idle()
        self.t.equal(f"{s} 6: the reply", reply.get("value"), "2.50 mm")
        self.text_part(f"{s} 6", "part", "Orca\nMCP", "ITALIC", [19.985, 22.485])
        combo = self.style_combo()
        self.t.check(f"{s} 6: the style combo is marked modified", (combo or "").startswith("ITALIC*"), combo)
        self.press({"imgui_name": "Revert Depth"})
        self.idle()
        self.text_part(f"{s} 7", "part", "Orca\nMCP", "ITALIC", [19.985, 20.985])
        panel = self.panel()
        self.t.equal(f"{s} 7: the Depth field", panel["items"].get("Depth"), "1.00 mm")
        self.t.equal(f"{s} 7: the style combo", panel["items"].get("style_selector"), "ITALIC")
        self.press({"imgui": "Cut"})
        self.idle()
        cut = ("negative", "Orca\nMCP", "ITALIC", [19.015, 20.015])
        self.text_part(f"{s} 8: the text cuts into the top", *cut)
        self.undo()
        self.text_part(f"{s} 9: the move inward is an undo step of its own", "negative", "Orca\nMCP", "ITALIC",
                       [19.985, 20.985])
        self.undo()
        self.text_part(f"{s} 10", "part", "Orca\nMCP", "ITALIC", [19.985, 20.985])
        self.redo()
        self.redo()
        self.text_part(f"{s} 11", *cut)
        self.close_by(f"{s} 12", "Done")
        self.text_part(f"{s} 12", *cut)
        self.save_and_reopen("text.3mf")
        self.text_part(f"{s} 13: the project file keeps the text", *cut)

    def svg(self):
        s = "SVG"
        self.start(s)
        self.cube([100, 100])
        self.run("object_select", none=True)
        self.camera("top")
        objs = self.one(f"{s} 0", [100, 100, 10])
        if objs:
            self.t.equal(f"{s} 0: one part", len(objs[0]["parts"]), 1)
        self.t.equal(f"{s} 0: nothing selected", self.selection(), [])
        copy = os.path.join(self.folder, "star-copy.svg")

        def star(label, path):
            objs = self.scene()
            if not self.t.equal(f"{label}: two objects", len(objs), 2):
                return
            self.placed(label, objs[0], [100, 100, 10])
            self.t.equal(f"{label}: the cube keeps one part", len(objs[0]["parts"]), 1)
            parts = objs[1]["parts"]
            if self.t.equal(f"{label}: the star has one part", len(parts), 1):
                self.t.equal(f"{label}: the star's file", os.path.normcase(parts[0]["svg"]["path"]),
                             os.path.normcase(path))
            self.t.near(f"{label}: the star's size", objs[1]["bbox"]["size"], [18.6, 17.6, 10], 0.05)
            self.t.near(f"{label}: the star stands on the bed", objs[1]["bbox"]["min"][2], 0, 0.001)

        self.run("object_add_svg", path=STAR)
        self.idle()
        star(f"{s} 1", STAR)
        self.t.equal(f"{s} 1: the gizmo", self.gizmo(), {"name": "SVG", "canvas": "3d"})
        self.t.check(f"{s} 1: the panel names the file", "star" in self.panel()["texts"], self.panel()["texts"])
        self.run("gizmo_pick_file", action="svg_save_as", path=copy)
        star(f"{s} 2", copy)
        with open(copy, encoding="utf-8") as f:
            self.t.check(f"{s} 2: the file holds the star", "M10 1 L12.6 7.2" in f.read(), copy)
        self.t.check(f"{s} 2: the panel names the copy", "star-copy" in self.panel()["texts"], self.panel()["texts"])
        self.run("gizmo_pick_file", action="svg_change", path=STAR)
        self.idle()
        star(f"{s} 3", STAR)
        texts = self.panel()["texts"]
        self.t.check(f"{s} 3: the panel names the file again", "star" in texts and "star-copy" not in texts, texts)
        self.close(f"{s} 4", "SVG")
        self.run("object_select", objects=[0])
        self.camera("top", "fit_selection")
        self.run("object_add_svg", path=STAR)
        self.idle()

        def on_cube(label, path=STAR):
            objs = self.scene()
            parts = objs[0]["parts"] if objs else []
            if not self.t.equal(f"{label}: the cube has two parts", len(parts), 2):
                return
            part = parts[1]
            self.t.equal(f"{label}: part 1", (part["type"], os.path.normcase((part["svg"] or {}).get("path", ""))),
                         ("part", os.path.normcase(path)))
            self.t.near(f"{label}: part 1's z range", [part["bbox"]["min"][2], part["bbox"]["max"][2]],
                        [19.985, 29.985], 0.01)
            x, y, _ = centre(part["bbox"])
            self.t.check(f"{label}: part 1 stands over the top", 90 <= x <= 110 and 90 <= y <= 110, (x, y))
            return objs

        on_cube(f"{s} 4")
        self.t.equal(f"{s} 4: the gizmo", self.gizmo(), {"name": "SVG", "canvas": "3d"})
        self.close(f"{s} 5", "SVG")
        self.undo()
        objs = self.scene()
        self.t.equal(f"{s} 5: adding the part is one undo step", len(objs[0]["parts"]) if objs else None, 1)
        self.redo()
        on_cube(f"{s} 6")
        self.save_and_reopen("svg.3mf")
        # The project file names the SVG's file without its folder (to_xml in bbs_3mf.cpp).
        objs = on_cube(f"{s} 7: the project file", "star.svg")
        if objs and len(objs) == 2:
            self.t.equal(f"{s} 7: the star keeps its file's name", objs[1]["parts"][0]["svg"]["path"], "star.svg")
            self.t.equal(f"{s} 7: both SVG parts keep their file in the project",
                         [objs[1]["parts"][0]["svg"]["path_in_3mf"], objs[0]["parts"][1]["svg"]["path_in_3mf"]],
                         ["3D/star.svg", "3D/star.svg"])

    def measure_panel(self):
        """(Selection 1, Selection 2, [(label, value) of each result row]) from the Measure panel."""
        texts = self.panel()["texts"]
        sel = [texts[texts.index(k) + 1] if k in texts and texts.index(k) + 1 < len(texts) else None
               for k in ("Selection 1", "Selection 2")]
        rows = []
        if "Measure" in texts:
            rest = texts[texts.index("Measure") + 1:]
            while len(rest) >= 2 and not rest[0].endswith(":"):
                rows.append((rest[0], rest[1]))
                rest = rest[2:]
        return sel[0], sel[1], rows

    def measure(self):
        s = "Measure"
        self.start(s)
        self.cube([100, 100])
        self.camera("iso")
        after0 = ([100, 100, 10], [0, 0, 0], 1, [20, 20, 20])
        self.one(f"{s} 0", *after0)
        self.open(f"{s} 1", "Measure")
        self.t.equal(f"{s} 1: the panel", self.measure_panel(), ("None", "None", []))
        self.click({"object": 0, "spot": "front"})
        self.t.equal(f"{s} 2: the panel", self.measure_panel(), ("Plane", "None", []))
        self.click({"object": 0, "spot": "left"})
        self.t.equal(f"{s} 3: two faces at an angle give an angle only", self.measure_panel(),
                     ("Plane", "Plane", [("Angle", "90.000°")]))
        self.click({"object": 0, "point": [-10, 0, -4]}, modifiers=["shift"])
        sel1, sel2, rows = self.measure_panel()
        self.t.equal(f"{s} 4: the selections", (sel1, sel2), ("Plane", "Point on plane"))
        self.t.equal(f"{s} 4: one Distance row", [label for label, _ in rows], ["Distance"])
        m = re.fullmatch(r"(-?\d+(?:\.\d+)?) mm", rows[0][1]) if rows else None
        self.t.check(f"{s} 4: the distance from the front face", m is not None and abs(float(m.group(1)) - 10) <= 0.25,
                     rows)
        self.close(f"{s} 5", "Measure")
        self.one(f"{s} 5: measuring moved nothing", *after0)

    def simplify(self):
        s = "Simplify"
        self.start(s)
        self.run("object_add_primitive", shape="sphere", size=[20], position=[100, 100, 0], name="S")

        def triangles(label, n, size=False):
            objs = self.scene()
            self.t.equal(f"{label}: S's triangles", [p["triangles"] for p in objs[0]["parts"]] if objs else None, [n])
            if size and objs:
                self.t.near(f"{label}: S's size", objs[0]["bbox"]["size"], [20, 20, 20], 0.01)

        triangles(f"{s} 0", 24648, size=True)
        self.open(f"{s} 1", "Simplify")
        self.set({"imgui_name": "use_count"}, True)
        reply = self.set({"imgui": "Decimate ratio"}, 60)
        self.t.equal(f"{s} 1: the reply", reply.get("value"), "60.00%")
        triangles(f"{s} 1", 24648)
        self.close(f"{s} 2: at once, while it works", "Simplify")
        self.idle()
        triangles(f"{s} 2: closing applies nothing", 24648)
        self.open(f"{s} 3", "Simplify")
        self.set({"imgui_name": "use_count"}, True)
        self.set({"imgui": "Decimate ratio"}, 90)
        self.idle()
        triangles(f"{s} 3", 24648)
        texts = self.panel()["texts"]
        self.t.check(f"{s} 3: the panel shows the count asked for", "2465 triangles" in texts, texts)
        self.press({"imgui": "Apply"})
        self.idle()
        triangles(f"{s} 4", 2464)
        self.t.equal(f"{s} 4: Apply closes the gizmo", self.gizmo(), None)
        self.undo()
        triangles(f"{s} 5", 24648)
        self.redo()
        triangles(f"{s} 6", 2464)

    def mesh_boolean(self):
        s = "Mesh Boolean"
        self.start(s)
        self.cube([100, 100])
        self.run("part_add", object=0, type="part", shape="cube", size=[10, 10, 10], position=[110, 110, 20])
        self.run("object_select", objects=[0])
        self.camera("iso")

        def state(label, parts, lo, hi):
            objs = self.scene()
            if not objs:
                self.t.check(f"{label}: the object", False, "no object")
                return
            got = objs[0]["parts"]
            self.t.equal(f"{label}: the parts' types", [p["type"] for p in got], [t for t, _, _ in parts])
            if len(got) == len(parts):
                for i, (p, (_, volume, name)) in enumerate(zip(got, parts)):
                    self.t.near(f"{label}: part {i}'s mesh volume", p["mesh_volume"], volume, 1 if name else 0.001)
                    if name:
                        self.t.check(f"{label}: part {i}'s name ends with {name!r}", p["name"].endswith(name),
                                     p["name"])
            box = objs[0]["bbox"]
            self.t.near(f"{label}: the object's bbox", box["min"] + box["max"], lo + hi, 0.001)

        after0 = ([("part", 8000, None), ("part", 1000, None)], [90, 90, 0], [115, 115, 25])
        state(f"{s} 0", *after0)
        self.open(f"{s} 1", "MeshBoolean")
        self.press({"imgui": "Union", "index": 0})
        state(f"{s} 1", *after0)
        self.press({"imgui_name": "select_source_volume"})
        self.click({"object": 0, "part": 0, "spot": "front"})
        self.press({"imgui_name": "select_tool_volume"})
        self.click({"object": 0, "part": 1, "spot": "top"})
        self.press({"imgui_name": "btn"})
        self.idle()
        after2 = ([("part", 8875, " - union")], [87.5, 87.5, -2.5], [112.5, 112.5, 22.5])
        state(f"{s} 2: the union lands 2.5 mm off", *after2)
        self.t.equal(f"{s} 2: with one part left the gizmo closes", self.gizmo(), None)
        self.undo()
        state(f"{s} 3", *after0)
        self.redo()
        state(f"{s} 4", *after2)

    def brim_ears(self):
        s = "brim ears"
        self.start(s)
        self.cube([100, 100])
        self.camera("iso")

        def ears(label, n, brim_type):
            objs = self.scene()
            self.t.equal(f"{label}: brim ears", objs[0]["brim_ears"] if objs else None, n)
            self.t.equal(f"{label}: brim type", self.setting(0, "brim_type"), brim_type)

        unset = {"value": None, "effective": "auto_brim", "source": "global"}
        painted = {"value": "painted", "effective": "painted", "source": "object"}
        self.one(f"{s} 0", [100, 100, 10])
        ears(f"{s} 0", 0, unset)
        self.open(f"{s} 1", "Brim Ears")
        ears(f"{s} 1", 0, unset)
        texts = self.panel()["texts"]
        self.t.check(f"{s} 1: the panel warns", BRIM_WARNING in texts, texts)
        self.press({"imgui_name": "Set brim type to painted"})
        ears(f"{s} 2", 0, painted)
        texts = self.panel()["texts"]
        self.t.check(f"{s} 2: the warning is gone", BRIM_WARNING not in texts, texts)
        self.press({"imgui": "Auto-generate"})
        ears(f"{s} 3: one ear at each corner", 4, painted)
        self.undo()
        ears(f"{s} 4: the link took no undo step", 0, painted)
        self.redo()
        ears(f"{s} 5", 4, painted)
        self.click({"object": 0, "point": [0, -10, -9.5]})
        ears(f"{s} 6", 5, painted)
        self.undo()
        ears(f"{s} 7", 4, painted)
        self.redo()
        self.close_by(f"{s} 8", "Done")
        ears(f"{s} 8", 5, painted)

    def assembly_view(self):
        s = "assembly in the assemble view"
        self.start(s)
        self.cube([100, 100], name="A")
        self.cube([160, 100], name="B")
        self.run("object_select", all=True)
        self.run("view_set", canvas="assemble")
        self.run("view_set", camera="iso")

        def where(label, prepare, assemble):
            objs = self.scene()
            if self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["A", "B"]):
                for o, offset in zip(objs, prepare):
                    self.placed(label, o, offset)
            got = self.assembly()
            for name, g, w in zip("AB", got, assemble):
                if len(w) == 1:
                    self.t.near(f"{label}: {name}'s height in the assemble view", g[2], w[0], 0.001)
                else:
                    self.t.near(f"{label}: {name} in the assemble view", g, w, 0.001)

        prepare = ([100, 100, 10], [160, 100, 10])
        where(f"{s} 0", prepare, ([10], [10]))
        self.t.equal(f"{s} 0: the selection", self.selection(), [0, 1])
        for obj, x, y in ((0, 100, 100), (1, 60, 160)):
            self.run("object_select", objects=[obj])
            self.open(f"{s} 1", "Move", "assemble")
            self.set({"imgui_name": "position_x", "canvas": "assemble"}, x)
            self.set({"imgui_name": "position_y", "canvas": "assemble"}, y)
            self.close(f"{s} 1", "Move", "assemble")
        after1 = ([100, 100, 10], [60, 160, 10])
        where(f"{s} 1: a move in the assemble view moves only the assembly", prepare, after1)
        self.run("object_select", all=True)
        self.run("view_set", camera="fit_objects")
        self.open(f"{s} 2", "Assembly", "assemble")
        reply = self.set({"imgui": "Mode"}, "Face and face assembly")
        self.t.equal(f"{s} 2: the reply", reply.get("selected"), "Face and face assembly")
        where(f"{s} 2", prepare, after1)
        self.click({"object": 0, "spot": "left"}, canvas="assemble")
        self.click({"object": 1, "spot": "left"}, canvas="assemble")
        where(f"{s} 3", prepare, after1)
        panel = self.panel()
        texts = panel["texts"]
        faces = [texts[texts.index(k) + 1] if k in texts and texts.index(k) + 1 < len(texts) else None
                 for k in ("Face 1 (Fixed)", "Face 2 (Moving)")]
        self.t.equal(f"{s} 3: both faces picked", faces, ["Plane", "Plane"])
        self.t.equal(f"{s} 3: the faces' distance", panel["items"].get("parallel_distance_z"), "40.00")
        self.set({"imgui_name": "parallel_distance_z"}, 0)
        after4 = ([100, 100, 10], [100, 160, 10])
        where(f"{s} 4: B's left face in A's", prepare, after4)
        self.undo()
        where(f"{s} 5", prepare, after1)
        self.redo()
        where(f"{s} 6", prepare, after4)
        self.close_by(f"{s} 7", "Return")
        where(f"{s} 7", prepare, after4)
        self.t.equal(f"{s} 7: the tab", self.c.call("app_state")["tab"], "prepare")
        reply = self.run("object_select", objects=[0])
        self.t.equal(f"{s} 7: a selection goes to Prepare's 3D view", reply.get("canvas"), "3d")
        self.save_and_reopen("assembly.3mf")
        where(f"{s} 8: the project file keeps the assembly", prepare, after4)

    def cut(self):
        s = "cut with a connector"
        self.start(s)
        self.cube([100, 100], name="block")
        self.cube([150, 100], 10, name="other")
        self.run("object_select", objects=[0])
        self.camera("iso", "fit_selection")

        def before(label):
            objs = self.scene()
            if self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["block", "other"]):
                self.placed(label, objs[0], [100, 100, 10])
                self.t.equal(f"{label}: block's parts", [(p["type"], p["mesh_volume"]) for p in objs[0]["parts"]],
                             [("part", 8000)])
                self.placed(label, objs[1], [150, 100, 5])

        def halves(label):
            objs = self.scene()
            if not self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["other", "block", "block"]):
                return
            self.placed(label, objs[0], [150, 100, 5])
            for o, half, want, height in ((objs[1], "upper", [("part", 4800), ("negative", 1.167)], 12),
                                          (objs[2], "lower", [("part", 3200), ("part", 1.167)], None)):
                got = [(p["type"], p["mesh_volume"]) for p in o["parts"]]
                self.t.equal(f"{label}: the {half} half's parts", [t for t, _ in got], [t for t, _ in want])
                self.t.near(f"{label}: the {half} half's mesh volumes", [v for _, v in got], [v for _, v in want],
                            0.001)
                box = o["bbox"]
                self.t.near(f"{label}: the {half} half stands where block stood",
                            [box["min"][0], box["min"][1], box["max"][0], box["max"][1], box["min"][2]],
                            [90, 90, 110, 110, 0], 0.001)
                if height is not None:
                    self.t.near(f"{label}: the {half} half's height", box["size"][2], height, 0.001)

        before(f"{s} 0")
        self.t.equal(f"{s} 0: the selection", self.selection(), [0])
        self.open(f"{s} 1", "Cut")
        reply = self.set({"imgui_name": "move_Z"}, 8)
        self.t.equal(f"{s} 1: the reply", reply.get("value"), "8.00")
        before(f"{s} 1")
        self.press({"imgui": "Add connectors"})
        reply = self.set({"imgui": "Style"}, "Frustum")
        self.t.equal(f"{s} 2: the style reply", reply.get("selected"), "Frustum")
        reply = self.set({"imgui": "Shape"}, "Square")
        self.t.equal(f"{s} 2: the shape reply", reply.get("selected"), "Square")
        self.click({"object": 0, "point": [100, 100, 8], "frame": "world"}, check="point")
        before(f"{s} 2")
        self.press({"imgui": "Confirm connectors"})
        self.press({"imgui": "Perform cut"})
        self.idle()
        halves(f"{s} 3")
        self.t.equal(f"{s} 3: the cut closes the gizmo", self.gizmo(), None)
        self.undo()
        before(f"{s} 4: the cut is one undo step")
        self.redo()
        halves(f"{s} 5")
        self.save_and_reopen("cut.3mf")
        halves(f"{s} 6: the project file")

    def canvas_input(self):
        s = "canvas input"
        self.start(s)
        self.cube([100, 100])
        self.run("part_add", object=0, type="negative", shape="cube", size=[6, 6, 6], position=[100, 100, 20])
        self.camera("iso")

        def cube(label):
            objs = self.one(label, [100, 100, 10], size=[20, 20, 20])
            if objs:
                parts = objs[0]["parts"]
                self.t.equal(f"{label}: the parts", [p["type"] for p in parts], ["part", "negative"])
                if len(parts) == 2:
                    self.t.near(f"{label}: the negative's z range", [parts[1]["bbox"]["min"][2],
                                                                     parts[1]["bbox"]["max"][2]], [17, 23], 0.001)
            return objs

        def cam():
            return self.c.call("app_state")["camera"]

        cube(f"{s} 0")
        cam0 = cam()
        reply = self.c.call("mouse_gesture", action="down", button="middle", x=300, y=300)
        self.t.equal(f"{s} 1: the press is held", reply.get("held"), True)
        self.t.equal(f"{s} 1: a press alone does not move the camera", cam(), cam0)
        reply = self.c.call("mouse_gesture", action="move", x=310, y=300)
        self.t.equal(f"{s} 2: the reply", (reply.get("held"), reply.get("camera_moved")), (True, True))
        cam2 = cam()
        self.t.check(f"{s} 2: the held button pans", cam2["look_at"] != cam0["look_at"], (cam0, cam2))
        self.t.refused(f"{s} 3: a point off screen is refused",
                       lambda: self.c.call("mouse_gesture", action="move",
                                           at={"object": 0, "point": [0, 0, 5000], "frame": "world"}),
                       words="off screen")
        cube(f"{s} 3")
        self.t.equal(f"{s} 3: the camera did not move", cam(), cam2)
        before = cam()
        reply = self.c.call("mouse_gesture", action="move", x=320, y=300)
        self.t.equal(f"{s} 4: the failed gesture released the press", (reply.get("held"), reply.get("camera_moved")),
                     (None, None))
        self.t.equal(f"{s} 4: the camera did not move", cam(), before)
        before = cam()
        self.c.call("mouse_gesture", action="down", button="middle", x=300, y=300)
        reply = self.c.call("mouse_gesture", action="move", x=330, y=300)
        self.t.equal(f"{s} 5: a new press pans", (reply.get("held"), reply.get("camera_moved")), (True, True))
        after = cam()
        reply = self.c.call("mouse_gesture", action="up", x=330, y=300)
        self.t.equal(f"{s} 5: the up releases it", reply.get("held"), None)
        self.t.check(f"{s} 5: the camera moved", after["look_at"] != before["look_at"], (before, after))

        def adaptive_listed():
            return any(n["kind"] == "imgui" and n["label"] == "Adaptive"
                       for n in self.nodes(target="main", query="Adaptive", include_web=False, include_menus=False))

        self.run("object_select", none=True)
        self.t.equal(f"{s} 6: the toolbar item before", self.toolbar("layersediting"),
                     {"enabled": False, "pressed": False})
        self.t.refused(f"{s} 6: a click on the disabled item is refused",
                       lambda: self.c.call("widget_click", target={"gltoolbar": "layersediting"}),
                       words="is disabled")
        self.t.equal(f"{s} 6: the toolbar item after", self.toolbar("layersediting"),
                     {"enabled": False, "pressed": False})
        self.t.equal(f"{s} 6: no Adaptive button", adaptive_listed(), False)
        objs = self.scene()
        self.t.equal(f"{s} 6: no layer height profile", objs[0]["layer_height_profile"] if objs else "no object", None)
        self.run("object_select", objects=[0])
        reply = self.c.call("widget_click", target={"gltoolbar": "layersediting"})
        self.t.equal(f"{s} 7: the reply", reply.get("pressed"), True)
        self.t.equal(f"{s} 7: the toolbar item", self.toolbar("layersediting"), {"enabled": True, "pressed": True})
        self.t.equal(f"{s} 7: the Adaptive button", adaptive_listed(), True)
        self.press({"imgui": "Adaptive"})
        self.idle()
        objs = self.scene()
        profile = objs[0]["layer_height_profile"] if objs else None
        self.t.check(f"{s} 8: a layer height profile with points", bool(profile), profile)
        reply = self.c.call("widget_click", target={"gltoolbar": "layersediting"})
        self.t.equal(f"{s} 9: the reply", reply.get("pressed"), False)
        self.t.equal(f"{s} 9: the toolbar item", self.toolbar("layersediting"), {"enabled": True, "pressed": False})
        self.t.equal(f"{s} 9: no Adaptive button", adaptive_listed(), False)
        objs = self.scene()
        self.t.equal(f"{s} 9: the profile stays", objs[0]["layer_height_profile"] if objs else None, profile)
        self.camera("top")
        self.run("object_select", none=True)
        self.c.call("mouse_gesture", action="move", at={"object": 0, "part": 1, "spot": "top"})
        self.t.equal(f"{s} 10: a move selects nothing", self.selection(), [])
        self.c.call("mouse_gesture", at={"object": 0, "part": 1, "spot": "top"})
        self.t.equal(f"{s} 11: a click on the negative part selects its object", self.selection(), [0])
        self.camera("bottom")
        self.run("object_select", none=True)
        self.c.call("mouse_gesture", at={"object": 0, "spot": "bottom"})
        self.t.equal(f"{s} 12: from below the bed hides nothing", self.selection(), [0])
        self.run("object_select", none=True)
        before = cam()
        self.t.refused(f"{s} 13: from below the cube hides the negative part's top",
                       lambda: self.c.call("mouse_gesture", action="move", at={"object": 0, "part": 1, "spot": "top"}),
                       words="is in front of the point")
        self.t.equal(f"{s} 13: nothing selected", self.selection(), [])
        objs = cube(f"{s} 13")
        self.t.equal(f"{s} 13: the profile stays", objs[0]["layer_height_profile"] if objs else None, profile)
        self.t.equal(f"{s} 13: the camera did not move", cam(), before)

    # ---- The commands that open a gizmo on a named target ----

    def two_objects(self):
        """A 20 mm cube A at (100, 100), and a 20 mm cube B at (150, 100) with a 10 mm cube part; nothing selected."""
        self.cube([100, 100], name="A")
        self.cube([150, 100], name="B")
        self.run("part_add", object=1, type="part", shape="cube", size=[10, 10, 10], position=[160, 110, 20])
        self.run("object_select", none=True)

    AB = [{"name": "A", "offset": [100, 100, 10], "parts": 1}, {"name": "B", "offset": [150, 100, 10], "parts": 2}]

    def opening(self):
        s = "opening a gizmo on a target"
        self.start(s)
        self.filament_slots(2)
        self.two_objects()
        self.t.equal(f"{s} 0: the objects", self.objects(), self.AB)
        self.t.equal(f"{s} 0: two filament slots", self.slots(), "0,0")
        self.t.equal(f"{s} 0: nothing selected", self.selected(), {"objects": [], "instances": []})
        self.t.equal(f"{s} 0: no gizmo open", self.gizmo(), None)

        def opens(row, tool, args, reply_fields, gizmo, sel):
            reply = self.run(tool, **args)
            self.t.equal(f"{s} {row}: the reply", self.fields(reply, ["gizmo", "active"] + list(reply_fields)),
                         {"gizmo": gizmo, "active": True, **reply_fields})
            self.t.equal(f"{s} {row}: the gizmo open", self.gizmo(), {"name": gizmo, "canvas": "3d"})
            self.t.equal(f"{s} {row}: the selection", self.selected(), sel)
            self.t.equal(f"{s} {row}: the objects", self.objects(), self.AB)
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
        self.t.equal(f"{s} 12: the objects", self.objects(), self.AB)

    def refused_openings(self):
        s = "openings refused"
        self.start(s)
        self.filament_slots(1)
        self.two_objects()
        self.t.equal(f"{s} 0: the objects", self.objects(), self.AB)
        self.t.equal(f"{s} 0: one filament slot", self.slots(), "0")
        self.t.equal(f"{s} 0: nothing selected", self.selected(), {"objects": [], "instances": []})

        def refused(row, before, tool, args, code, before_gizmo=None, steps=()):
            self.t.equal(f"{s} {row}: the selection before", self.selected(), before)
            self.t.equal(f"{s} {row}: the gizmo before", self.gizmo(), before_gizmo)
            undo_steps(self.c)
            self.t.refused(f"{s} {row}: refused", lambda: self.c.call(tool, **args), code=code)
            self.t.equal(f"{s} {row}: the objects", self.objects(), self.AB)
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
        """The 20 mm cube A at (100, 100), as after row 0 of cut from arguments."""
        objs = self.shapes()
        if self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["A"]):
            self.t.near(f"{label}: A's offset", objs[0]["offset"], [100, 100, 10], 0.01)
            self.piece(f"{label}: A", objs[0], [8000], size=[20, 20, 20])

    def level_halves(self, label):
        """A cut level at z 10: two pieces 10 mm high where A stood, upper first."""
        objs = self.shapes()
        if self.t.equal(f"{label}: the objects", [o["name"] for o in objs], ["A", "A"]):
            for o, which in zip(objs, ("upper", "lower")):
                self.piece(f"{label}: the {which} piece", o, [4000], size=[20, 20, 10], xy=[90, 90, 110, 110])

    @staticmethod
    def pieces_reply(reply):
        return [{k: p.get(k) for k in ("index", "name", "open_edges")} for p in reply.get("pieces", [])]

    def cut_from_arguments(self):
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
        self.level_halves(f"{s} 2")
        self.t.equal(f"{s} 2: the open Cut gizmo was closed first", self.gizmo(), None)
        self.t.equal(f"{s} 2: the reply", self.pieces_reply(reply),
                     [{"index": 0, "name": "A", "open_edges": 0}, {"index": 1, "name": "A", "open_edges": 0}])
        # The gizmo's own step as it closes, then the cut's.
        self.t.equal(f"{s} 2: the undo steps", undo_steps(self.c), ["Leaving Cut gizmo", "Cut by Plane"])
        self.undo()
        self.whole(f"{s} 3")
        self.redo()
        self.level_halves(f"{s} 4")
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

        # The refused cuts, on the whole cube the undo above leaves.
        plane = {"point": [100, 100, 10], "normal": [0, 0, 1]}
        rows = [
            (12, {"z": 50}, "plane_misses_object", None),
            (13, {"keep": "upper"}, "needs_plane", None),
            (14, {"z": 10, "plane": plane}, None, "z or plane, not both"),
            (15, {"plane": {"point": [100, 100, 10], "normal": [0, 0, 0]}}, None, "normal is zero"),
            (16, {"z": 10, "keep": "upper", "lower": "flip"}, None, "the lower piece is not kept"),
            (17, {"z": 10, "keep": "upper", "to_parts": True}, None, "to_parts keeps both pieces"),
            (18, {"z": 10, "to_parts": True, "upper": "flip"}, None, "to_parts places no piece"),
        ]
        for row, args, code, words in rows:
            undo_steps(self.c)
            self.t.refused(f"{s} {row}: refused", lambda: self.c.call("object_cut", object=0, **args), code=code,
                           words=words)
            self.whole(f"{s} {row}")
            self.t.equal(f"{s} {row}: no gizmo open", self.gizmo(), None)
            self.t.equal(f"{s} {row}: the undo steps", undo_steps(self.c), [])

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

    def lay_from_direction(self):
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

        # The refused calls, on the turned cube the undo above leaves.
        undo_steps(self.c)
        self.t.refused(f"{s} 8: refused", lambda: self.c.call("object_lay_on_face", object=0, frame="world"),
                       code="needs_normal")
        self.turned(f"{s} 8", [tilted])
        self.t.equal(f"{s} 8: no gizmo open", self.gizmo(), None)
        self.t.equal(f"{s} 8: the undo steps", undo_steps(self.c), [])
        self.run("object_lay_on_face", object=0)
        undo_steps(self.c)
        self.t.refused(f"{s} 9: refused", lambda: self.c.call("object_lay_on_face", object=0, normal=[0, 0, 0]),
                       code="zero_normal")
        self.turned(f"{s} 9", [tilted])
        self.t.equal(f"{s} 9: refused before it closes the open gizmo", self.gizmo(), {"name": "Flatten", "canvas": "3d"})
        self.t.equal(f"{s} 9: the undo steps", undo_steps(self.c), [])
        self.run("object_select", none=True)
        self.t.equal(f"{s} 10: no gizmo open", self.gizmo(), None)

        self.run("object_set_instances", object=0, count=2)
        self.turned(f"{s} 11: the new instance copies i0's turn", [tilted, {"rotation": [30, 0, 0]}])
        self.run("object_lay_on_face", object=0, instance=0, normal=[0, 0, -1])
        self.turned(f"{s} 12: the other instance turns with it", [flat, {"rotation": [0, 0, 0]}])

    def ears_near(self, label, got, want):
        """Ears, each (x, y, z, r), against `want` to 0.001."""
        if self.t.equal(f"{label}: the ears' count", len(got), len(want)):
            for k, (g, w) in enumerate(zip(got, want)):
                self.t.near(f"{label}: ear {k}", list(g), list(w), 0.001)

    @staticmethod
    def reply_ears(reply):
        return [tuple(e["position"]) + (e["radius"],) for e in reply.get("ears", [])]

    def brim_from_positions(self):
        s = "brim ears from positions"
        self.start(s)
        self.cube([100, 100], name="A")
        self.run("object_select", none=True)
        # An ear without a radius takes eight first-layer line widths: 120 % of the 0.4 mm nozzle, 0.48 mm.
        two = [(-10, -10, -10.0001, 3), (10, -10, -10.0001, 3.84)]
        self.t.equal(f"{s} 0: no ears", self.ear_count(), 0)
        self.t.equal(f"{s} 0: the object's brim type", self.setting(0, "brim_type"),
                     {"value": None, "effective": "auto_brim", "source": "global"})
        line = self.c.call("settings_get", type="process", keys=["initial_layer_line_width"])["items"][0]["value"]
        self.t.equal(f"{s} 0: the first layer's line width", line, "120%")

        reply = self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 1: the reply", (reply.get("gizmo"), reply.get("active")), ("Brim Ears", True))
        self.t.equal(f"{s} 1: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.t.equal(f"{s} 1: no ears", self.ear_count(), 0)

        points = [{"position": [90, 90], "radius": 3}, {"position": [110, 90]}]
        undo_steps(self.c)
        reply = self.run("object_set_brim_ears", object=0, points=points)
        self.t.equal(f"{s} 2: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 2: Brim Ears was closed first", self.gizmo(), None)
        self.ears_near(f"{s} 2: the reply", self.reply_ears(reply), two)
        self.t.equal(f"{s} 2: the reply's brim type", (reply.get("brim_type"), reply.get("takes_effect")), ("auto_brim", False))
        # The two steps the Brim Ears gizmo records as it closes, then the call's.
        self.t.equal(f"{s} 2: the undo steps", undo_steps(self.c), ["Brim ears edit", "Leaving Brim Ears", "Set brim ears"])
        self.undo()
        self.t.equal(f"{s} 3: no ears", self.ear_count(), 0)
        self.redo()
        self.t.equal(f"{s} 4: two ears", self.ear_count(), 2)

        self.run("settings_set", target="object", object=0, values={"brim_type": "painted"})
        reply = self.run("object_set_brim_ears", object=0, points=points)
        self.t.equal(f"{s} 5: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 5: the object's brim type", self.setting(0, "brim_type"),
                     {"value": "painted", "effective": "painted", "source": "object"})
        self.ears_near(f"{s} 5: the reply", self.reply_ears(reply), two)
        self.t.equal(f"{s} 5: the reply's brim type", (reply.get("brim_type"), reply.get("takes_effect")), ("painted", True))

        path = os.path.join(self.folder, "ears.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.t.equal(f"{s} 6: two ears", self.ear_count(), 2)
        saved = self.brim_file(path)
        self.t.equal(f"{s} 6: the file's objects", sorted(saved), [1])
        self.ears_near(f"{s} 6: the file", saved.get(1, []), two)
        self.run("project_new", discard_changes=True)
        self.run("project_open", path=path)
        self.idle()
        self.t.equal(f"{s} 7: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 7: the object's brim type", self.setting(0, "brim_type")["value"], "painted")

        reply = self.run("object_set_brim_ears", object=0, clear=True)
        self.t.equal(f"{s} 8: no ears", self.ear_count(), 0)
        self.t.equal(f"{s} 8: the reply", reply.get("ears"), [])

        self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 9: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.run("object_set_brim_ears", object=0, points=[{"position": [100, 90], "radius": 5}])
        self.t.equal(f"{s} 9: one ear", self.ear_count(), 1)
        self.t.equal(f"{s} 9: Brim Ears was closed first", self.gizmo(), None)
        path = os.path.join(self.folder, "ear.3mf")
        self.run("project_save", path=path, overwrite=True)
        saved = self.brim_file(path)
        self.t.equal(f"{s} 9: the file's objects", sorted(saved), [1])
        self.ears_near(f"{s} 9: the call's ear, not what the gizmo held", saved.get(1, []), [(0, -10, -10.0001, 5)])

        self.run("object_set_brim_ears", object=0, points=[{"position": [90, 90], "radius": 3}, {"position": [110, 90], "radius": 3}])
        self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 10: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 10: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        undo_steps(self.c)
        self.t.refused(f"{s} 11: refused",
                       lambda: self.c.call("object_set_brim_ears", object=0, points=[{"position": [100, 90], "radius": 150}]),
                       code="radius_out_of_range")
        self.t.equal(f"{s} 11: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 11: a refused call leaves the gizmo open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.t.equal(f"{s} 11: the undo steps", undo_steps(self.c), [])
        self.t.refused(f"{s} 12: refused",
                       lambda: self.c.call("object_set_brim_ears", object=0, points=[{"position": [100, 90]}], clear=True),
                       words="points or clear, not both")
        self.t.equal(f"{s} 12: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 12: Brim Ears stays open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.run("object_select", none=True)
        self.t.equal(f"{s} 13: two ears", self.ear_count(), 2)
        self.t.equal(f"{s} 13: no gizmo open", self.gizmo(), None)

        self.run("object_set_brim_ears", object=0)
        self.t.equal(f"{s} 14: Brim Ears open", self.gizmo(), {"name": "Brim Ears", "canvas": "3d"})
        self.run("object_set_brim_ears", object=0, clear=True)
        self.t.equal(f"{s} 14: no ears", self.ear_count(), 0)
        self.t.equal(f"{s} 14: Brim Ears was closed first", self.gizmo(), None)
        path = os.path.join(self.folder, "cleared.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.t.equal(f"{s} 14: the file holds no ears for A, the gizmo's two not written back",
                     self.brim_file(path).get(1, []), [])

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

        # type without object adds to the selected object, and two are selected.
        self.run("object_select", objects=[0, 1])
        for row, tool, args in [(8, "object_add_text", {}), (9, "object_add_svg", {"path": star})]:
            undo_steps(self.c)
            self.t.refused(f"{s} {row}: refused", lambda: self.c.call(tool, type="modifier", **args), code="one_object_only")
            self.idle()
            state(f"{s} {row}", after4)
            self.t.equal(f"{s} {row}: no new object", len(self.listed()), 2)
            self.t.equal(f"{s} {row}: the selection", self.selected()["objects"], [0, 1])
            self.t.equal(f"{s} {row}: the undo steps", undo_steps(self.c), [])

        # type alone with A's negative part selected: A is selected whole first, as a right click on its row does.
        self.run("object_select", parts=[[0, 3]])
        self.run("object_add_svg", type="modifier", path=star)
        self.idle()
        state(f"{s} 10", after4 + [("modifier", "star.svg")], "SVG")
        self.t.equal(f"{s} 10: no new object", len(self.listed()), 2)

    def run_all(self, only=()):
        self.t.equal("the start: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        # The baseline's window size and preferences, read back since the checks depend on them: where the panels
        # open, what covers a point of the model and the pixels the canvas input scenario presses at follow the
        # window's size; the fields' texts are written in millimetres; and the canvas input scenario's pans need the
        # default camera style with a middle-button drag that pans.
        self.t.equal("the start: the window's size", self.c.call("app_state")["window"]["size"], [1600, 900])
        prefs = {"use_inches": "0", "camera_navigation_style": "0", "middle_mouse_drag_action": "1"}
        got = self.c.call("preferences_get", keys=list(prefs))["items"]
        self.t.equal("the start: the preferences", {i["key"]: i["value"] for i in got}, prefs)
        # Some panel items show only in Expert mode; the baseline's is Simple.
        self.run("action_run", id="orca_command:mode_expert:orca")
        self.t.equal("Expert mode", self.c.call("app_state")["mode"], "expert")
        scenarios = [("prompts a rule answers", self.prompts), ("Move, Rotate and Scale", self.move_rotate_scale),
                     ("aligning in the Move panel", self.align), ("lay on face", self.lay_on_face),
                     ("painting and texture layers", self.painting), ("text", self.text), ("SVG", self.svg),
                     ("Measure", self.measure), ("Simplify", self.simplify), ("Mesh Boolean", self.mesh_boolean),
                     ("brim ears", self.brim_ears), ("assembly in the assemble view", self.assembly_view),
                     ("cut with a connector", self.cut), ("canvas input", self.canvas_input),
                     ("opening a gizmo on a target", self.opening), ("openings refused", self.refused_openings),
                     ("cut from arguments", self.cut_from_arguments),
                     ("lay on face from a direction", self.lay_from_direction),
                     ("brim ears from positions", self.brim_from_positions),
                     ("text and SVG as a part of a named object", self.text_and_svg)]
        for name, scenario in scenarios:
            if not only or name in only:
                self.t.scenario(name, scenario)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-gizmos-") as folder:
        return GizmosTest(client, folder).run_all(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
