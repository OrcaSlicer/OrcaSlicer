#!/usr/bin/env python3
"""The generic GUI tools on the app's own widgets, as the test_gui_tools entry of
tools/mcp/test-entries/gui_tools.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_gui_tools.py [--out DIR]

The pictures and project files it saves go to --out, else to a new temporary folder. Exits 0 when every check passes.
"""

import argparse
import json
import os
import re
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, WINDOW_SIZE, Checks, menus_left, plates, refusal,  # noqa: E402
                    run, slot_colours, start, until, wait)

ADD_CUBE = "orca_command:add_primitive_cube:orca"
UNDO = "orca_command:undo:orca"
REDO = "orca_command:redo:orca"
PRESET_BUNDLE = "orca_command:preset_bundle:orca"
SETUP_WIZARD = "orca_command:help_setup_wizard:orca"
# The Orca Filament Library's Bambu PLA Basic, which suits any printer and opens Orca's own filament picker.
PLA = "Bambu PLA Basic @System"
FINE = "0.12mm Fine @MyKlipper"
# The Generic Marlin Printer, whose card the setup wizard's printer page ticks.
MARLIN = "MyMarlin 0.4 nozzle"

# A right click on a point of the cube's top, away from the diagonal its two top triangles share (from the iso
# view the ray to the middle runs along that edge), and one on empty space above the bed. The Add Cube action's cube
# is a tenth of the bed's longer side, 25 mm on the Klipper printer's 250 mm bed.
CUBE_POINT = {"object": 0, "point": [6, -6, 12.5]}
EMPTY = {"x": 1000, "y": 150}

OBJECT = ["Add Part>Cube", "Add Negative Part>Cylinder", "Add Modifier>Cylinder", "Add Support Blocker>Cube",
          "Add Support Enforcer>Cube", "Delete", "Printable", "Auto Drop"]
DEFAULT = ["Add Primitive>Cube", "Add Primitive>Cylinder", "Add Primitive>Sphere", "Add Primitive>Cone",
           "Add Primitive>Disc", "Add Primitive>Torus", "Add Primitive>Text", "Add Primitive>SVG",
           "Add Handy models>Orca Cube", "Add Handy models>OrcaSliced Combo", "Add Handy models>Orca Badge",
           "Add Handy models>Orca Tolerance Test", "Add Handy models>3DBenchy", "Add Handy models>Cali Cat",
           "Add Handy models>Autodesk FDM Test", "Add Handy models>Voron Cube", "Add Handy models>Stanford Bunny",
           "Add Handy models>Orca String Hell", "Add Models", "Show Labels"]
# Merge with names the other slot by its preset's alias, the name before " @".
FILAMENT_MENU = ["Edit", "Merge with>Generic PLA", "Decompose Color", "Delete"]
PLATE_TYPES = ["Smooth Cool Plate", "Engineering Plate", "Smooth High Temp Plate", "Textured PEI Plate",
               "Textured Cool Plate", "Cool Plate (SuperTack)"]
COMPARED = f'"{KLIPPER_STD}" vs "{FINE}"'
LAYER_ROW = "Quality>Layer height>Layer height"
BOTTOM_ROW = "Strength>Top/bottom shells>Bottom shell layers"

ROW = re.compile(r'^(\s*)\[r\d+\] row "(.*)$')
FLAGS = re.compile(r' \[([^\[\]]*)\]$')
PLACE = re.compile(r' @-?\d+,-?\d+ \d+x\d+$')


def O(text, printable=True, plate=0):
    """An object as the entry writes it: 'Cube: Cube part + Generic-Cylinder modifier'."""
    return {"object": text, "plate": plate, "printable": printable}


class GuiToolsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def idle(self):
        wait(self.c, "idle")

    def holds(self, label, fn, what, detail=None):
        """Checks that fn() comes true within checks.until()'s 30 s; a wait that runs out fails the check, with
        detail() when given, and the scenario goes on."""
        try:
            until(fn, what)
        except TimeoutError as e:
            self.t.check(label, False, f"{e}" + (f"; {detail()}" if detail else ""))
            return
        self.t.check(label, True)

    def run_answered(self, tool, **args):
        """Calls `tool` and follows its job to the end through a dialog the start's rules answer, which can hold
        the call for a moment (its reply is then blocked_by_dialog); a dialog no rule answers fails the call
        after 60 s."""
        reply = self.c.await_job(self.c.call(tool, **args), timeout=60)
        self.idle()
        return reply

    def objects(self):
        """Each object as {object: "<name>: <part> <type> + ...", plate, printable}, printable being its first
        instance's, in object_list's order; an object outside the plates has plate None."""
        listing = self.c.call("object_list", limit=500, detail=True)
        placed = [(p["index"], o) for p in listing["plates"] for o in p["objects"]]
        placed += [(None, o) for o in listing.get("outside", [])]
        return [{"object": o["name"] + ": " + " + ".join(f"{part['name']} {part['type']}" for part in o["parts"]),
                 "plate": plate, "printable": o["instances"][0]["printable"]} for plate, o in placed]

    def objects_are(self, label, *want):
        self.t.equal(f"{label}: the objects", self.objects(), list(want))

    def state(self):
        return self.c.call("app_state")

    def selection(self):
        sel = self.state().get("selection") or {}
        return {"objects": sel.get("objects", []), "parts": sel.get("parts", [])}

    def selection_is(self, label, objects=(), parts=()):
        self.t.equal(f"{label}: the selection", self.selection(), {"objects": list(objects), "parts": list(parts)})

    @staticmethod
    def menu(reply):
        popup = (reply or {}).get("popup_menu") or {}
        return {k: popup.get(k, []) for k in ("items", "checked", "disabled")}

    def dialogs(self):
        return [d.get("title") for d in self.state().get("dialogs", [])]

    def dialogs_are(self, label, *titles):
        self.t.equal(f"{label}: the dialogs open", self.dialogs(), list(titles))

    def windows(self):
        return [w.get("title") for w in self.c.call("window_list")["items"]]

    def window_ref(self, title):
        refs = [w["ref"] for w in self.c.call("window_list")["items"] if w.get("title") == title and w.get("ref")]
        assert refs, f"no window titled {title}"
        return refs[0]

    def rows(self, target):
        """The rows widget_list lists under `target` ("main", "dialog", "popup" or a window's title), each as
        {path, value, selected, checked}: its labels from the outermost row, joined by ">"; the cells after its
        label, joined by " | ", or None; and whether it is selected, and checked (None for a row without a
        checkbox)."""
        if target not in ("main", "dialog", "popup"):
            target = self.window_ref(target)
        _, texts = self.c.call_with_text("widget_list", target=target, query="row", max_nodes=2000)
        out, stack = [], []
        for line in "\n".join(texts).splitlines():
            m = ROW.match(line)
            if not m:
                continue
            indent, rest = len(m.group(1)), PLACE.sub("", m.group(2))
            flags = []
            while (f := FLAGS.search(rest)):
                flags = [x.strip() for x in f.group(1).split(",")] + flags
                rest = rest[:f.start()]
            rest = rest[:-1] if rest.endswith('"') else rest
            label, value = rest.rsplit('" = "', 1) if '" = "' in rest else (rest, None)
            while stack and stack[-1][0] >= indent:
                stack.pop()
            stack.append((indent, label))
            out.append({"path": ">".join(s[1] for s in stack), "value": value, "selected": "selected" in flags,
                        "checked": True if "checked" in flags else False if "unchecked" in flags else None})
        return out

    def listing(self, target="dialog", query=None, **args):
        """widget_list's tree as text."""
        if query is not None:
            args["query"] = query
        _, texts = self.c.call_with_text("widget_list", target=target, **args)
        return "\n".join(texts)

    def colours(self):
        return slot_colours(self.c)

    def bed_type(self):
        return self.c.call("project_get_settings", keys=["curr_bed_type"])["items"][0]["value"]

    def filaments(self):
        return self.state()["presets"]["filaments"]

    def project_file(self, path):
        with zipfile.ZipFile(path) as z:
            return json.loads(z.read("Metadata/project_settings.config"))

    def canvas_png(self, name):
        path = os.path.join(self.folder, name + ".png")
        self.c.call("window_screenshot", target="canvas", include_image=False, save_path=path, overwrite=True)
        with open(path, "rb") as f:
            return f.read()

    def process_settings(self, *keys):
        items = self.c.call("settings_get", type="process", keys=list(keys))["items"]
        return {i["key"]: i["value"] for i in items}

    def modified(self, kind):
        return [p["name"] for p in self.c.call("preset_list", type=kind, limit=500)["items"] if p.get("modified")]

    def job_ends(self, label, job, timeout=60):
        """Follows a job a call left waiting on a dialog to its end; it must end without an error."""
        try:
            result = self.c.await_job(job, timeout=timeout)
        except orca_mcp.McpError as e:
            self.t.check(f"{label}: the job ends without an error", False, str(e))
            return None
        self.t.check(f"{label}: the job ends without an error", result.get("status") not in ("running", "blocked_by_dialog"), result)
        return result

    def refused_with(self, label, call, words):
        """Checks the call is refused with text holding `words` and returns the refusal's details."""
        details = {}

        def wrapped():
            try:
                return call()
            except orca_mcp.McpError as e:
                details.update(refusal(e)[1])
                raise

        self.t.refused(label, wrapped, words=words)
        return details

    def start(self, name):
        self.began = time.monotonic()  # the scenario's own menus are those closed after this
        self.run("project_new", discard_changes=True, preset_changes="discard")
        self.run("view_set", tab="prepare")
        self.run("dialog_auto_answer", rules=[{"match": "Uniform scale", "button": "ok"},
                                              {"match": "per-object setting mode", "button": "ok"}])
        self.idle()
        self.dialogs_are(f"{name}: the start")
        self.t.equal(f"{name}: the start: the plates", plates(self.c),
                     [{"name": "", "current": True, "locked": False, "objects": 0}])

    def add_cube(self):
        """The Add Cube action's cube, its size dialog answered by the "Uniform scale" rule."""
        self.run_answered("action_run", id=ADD_CUBE)

    def objects_switch(self):
        """The sidebar's Global/Objects switch on Objects, which shows the object list."""
        self.run("widget_set_value", target={"class": "SwitchButton", "label": "Process"}, value=True)
        self.idle()

    def filament_button(self):
        """Filament 1's "Click to edit preset" button: the first of that name widget_list shows on screen. The
        printer's button of that name is listed first but hidden until the pointer is over its panel."""
        for line in self.listing("main", "Click to edit preset").splitlines():
            m = re.search(r'\[(w\d+)\] button "Click to edit preset".* @-?\d+,-?\d+ (\d+)x(\d+)$', line)
            if m and int(m.group(2)) > 0 and int(m.group(3)) > 0:
                return m.group(1)
        raise AssertionError("no Click to edit preset button is shown")

    # ---- Scenarios ----

    def menus(self):
        s = "menus"
        self.start(s)
        self.add_cube()
        self.run("view_set", camera="iso")
        cube, with_modifier = "Cube: Cube part", "Cube: Cube part + Generic-Cylinder modifier"
        self.objects_are(f"{s}: the start", O(cube))
        state = self.state()
        self.t.equal(f"{s}: the start: one filament slot", len(state["presets"]["filaments"]), 1)
        self.t.equal(f"{s}: the start: labels are off", state["view"]["labels"], False)

        # A menu item's job is followed through a dialog the start's rules answer, as run_answered() does. A call
        # that only lists its menu is followed by the app going idle, so that a check that it changed nothing comes
        # after anything it set off.
        def C(**args):
            reply = self.c.await_job(self.c.call("mouse_gesture", action="click", button="right", at=CUBE_POINT,
                                                 **args), timeout=60)
            self.idle()
            return reply

        def E(**args):
            reply = self.c.await_job(self.c.call("mouse_gesture", action="click", button="right", **EMPTY, **args),
                                     timeout=60)
            self.idle()
            return reply

        m = self.menu(C())
        self.t.check(f"{s} 1: the object menu holds every item it must", set(OBJECT) <= set(m["items"]),
                     sorted(set(OBJECT) - set(m["items"])))
        self.t.check(f"{s} 1: the object menu has no Add Primitive>Cube and no Assemble",
                     not {"Add Primitive>Cube", "Assemble"} & set(m["items"]), m["items"])
        self.t.equal(f"{s} 1: the checked items", m["checked"], ["Printable", "Auto Drop"])
        self.dialogs_are(f"{s} 1: listing a menu runs nothing")
        self.objects_are(f"{s} 1", O(cube))

        reply = C(popup_item="add modifier>cylinder")
        self.t.equal(f"{s} 2: the item run, named as the menu shows it", reply.get("ran"), "Add Modifier>Cylinder")
        self.objects_are(f"{s} 2", O(with_modifier))
        self.run("action_run", id=UNDO)
        self.idle()
        self.objects_are(f"{s} 3: the menu item's command is one undo step", O(cube))
        self.run("action_run", id=REDO)
        self.idle()
        self.objects_are(f"{s} 4", O(with_modifier))
        reply = self.run("widget_click", target={"menu": "Edit>Undo"})
        self.idle()
        self.t.equal(f"{s} 5: the reply's item", reply.get("clicked"), "Edit>Undo")
        self.objects_are(f"{s} 5", O(cube))
        reply = self.run("widget_click", target={"menu": "Edit>Redo"})
        self.idle()
        self.t.equal(f"{s} 6: the reply's item", reply.get("clicked"), "Edit>Redo")
        self.objects_are(f"{s} 6", O(with_modifier))

        reply = C(popup_item="Printable")
        self.t.equal(f"{s} 7: the reply's item", reply.get("ran"), "Printable")
        self.objects_are(f"{s} 7", O(with_modifier, printable=False))
        self.t.equal(f"{s} 8: the listing reads the check item's state", self.menu(C())["checked"], ["Auto Drop"])
        self.objects_are(f"{s} 8", O(with_modifier, printable=False))
        C(popup_item="Printable")
        self.objects_are(f"{s} 9", O(with_modifier))

        details = self.refused_with(f"{s} 10: a submenu is refused", lambda: C(popup_item="Add Modifier"),
                                    '"Add Modifier" is a submenu')
        self.t.check(f"{s} 10: the refusal lists the submenu's items",
                     "Add Modifier>Cylinder" in details.get("items", []), details)
        self.objects_are(f"{s} 10", O(with_modifier))
        self.t.refused(f"{s} 11: an item the menu lacks is refused", lambda: C(popup_item="Add Modifier>Pyramid"),
                       words='The menu has no item "Add Modifier>Pyramid"')
        self.objects_are(f"{s} 11", O(with_modifier))

        self.run("object_select", none=True)
        self.selection_is(f"{s} 12: after the select")
        self.t.equal(f"{s} 12: the empty space's menu", self.menu(E()), {"items": DEFAULT, "checked": [], "disabled": []})
        self.objects_are(f"{s} 12", O(with_modifier))

        labels_off = self.canvas_png("labels_off")
        E(popup_item="Show Labels")
        labels_on = self.canvas_png("labels_on")
        self.t.equal(f"{s} 13: labels are on", self.state()["view"]["labels"], True)
        self.t.check(f"{s} 13: the 3D view draws the cube's label", labels_on != labels_off, "the captures are the same")
        self.objects_are(f"{s} 13", O(with_modifier))
        self.t.equal(f"{s} 14: the checked items", self.menu(E())["checked"], ["Show Labels"])
        self.objects_are(f"{s} 14", O(with_modifier))
        E(popup_item="Show Labels")
        labels_off_again = self.canvas_png("labels_off_again")
        self.t.equal(f"{s} 15: labels are off", self.state()["view"]["labels"], False)
        self.t.check(f"{s} 15: the label is gone again", labels_off_again != labels_on, "the captures are the same")
        self.objects_are(f"{s} 15", O(with_modifier))

        E(popup_item="Add Primitive>Cylinder")
        both = (O(with_modifier), O("Cylinder: Cylinder part"))
        self.objects_are(f"{s} 16", *both)
        self.t.equal(f"{s} 16: app_state counts the objects", self.state()["objects"], 2)

        self.run("widget_click", target={"label": "Add one filament"})
        self.idle()
        self.t.equal(f"{s} 17: two filament slots", len(self.filaments()), 2)
        self.objects_are(f"{s} 17", *both)
        self.run("filament_set_colour", filament_slot=1, colour="#FF0000")
        self.run("filament_set_colour", filament_slot=2, colour="#0000FF")
        self.t.equal(f"{s} 18: the colours", self.colours(), "#FF0000;#0000FF")
        self.objects_are(f"{s} 18", *both)
        button = self.filament_button()
        reply = self.c.call("widget_click", target=button)
        self.idle()
        self.t.equal(f"{s} 19: filament 1's menu", self.menu(reply)["items"], FILAMENT_MENU)
        self.dialogs_are(f"{s} 19")
        self.t.equal(f"{s} 19: the tab", self.state()["tab"], "prepare")
        self.objects_are(f"{s} 19", *both)
        reply = self.c.call("widget_click", target=button, popup_item="Delete")
        self.idle()
        self.t.equal(f"{s} 20: the reply's item", reply.get("ran"), "Delete")
        self.t.equal(f"{s} 20: one filament slot", len(self.filaments()), 1)
        self.t.equal(f"{s} 20: the slot whose button was clicked is the one deleted", self.colours(), "#0000FF")
        self.objects_are(f"{s} 20", *both)

        self.run("object_select", all=True)
        self.selection_is(f"{s} 21: after the select", objects=[0, 1])
        m = self.menu(C())
        self.t.check(f"{s} 21: the multi-selection menu holds Assemble, Align left and Delete",
                     {"Assemble", "Align/Distribute>Align left (-X)", "Delete"} <= set(m["items"]), m["items"])
        self.t.check(f"{s} 21: the multi-selection menu has no Add Part>Cube", "Add Part>Cube" not in m["items"],
                     m["items"])
        self.objects_are(f"{s} 21", *both)
        reply = C(popup_item="Delete")
        self.t.equal(f"{s} 22: the reply's item", reply.get("ran"), "Delete")
        self.objects_are(f"{s} 22: the multi-selection's Delete removes both objects")
        state = self.state()
        self.t.equal(f"{s} 22: app_state counts no objects", state["objects"], 0)
        self.t.equal(f"{s} 22: no menu left unseen", menus_left(state, self.began), [])
        self.t.equal(f"{s} 22: no dialog", [d.get("title") for d in state["dialogs"]], [])

    def object_rows(self):
        s = "object list rows"
        self.start(s)
        self.add_cube()
        self.objects_switch()
        cube = "Cube: Cube part"
        self.objects_are(f"{s}: the start", O(cube))
        paths = [r["path"] for r in self.rows("main")]
        self.t.check(f"{s} 1: the rows hold Plate 1 and Cube under it", {"Plate 1", "Plate 1>Cube"} <= set(paths), paths)

        self.run("widget_click", target={"gltoolbar": "addplate"})
        self.idle()
        self.t.equal(f"{s} 2: two plates", len(plates(self.c)), 2)
        self.t.equal(f"{s} 2: the current plate", self.state()["plates"]["current"], 1)
        for step, row, current in ((3, "Plate 1", 0), (4, "Plate 2", 1), (5, "Plate 1", 0)):
            reply = self.run("widget_click", target={"row": row})
            self.idle()
            if step != 5:
                self.t.equal(f"{s} {step}: the reply's selection", reply.get("selected"), [row])
            self.t.equal(f"{s} {step}: the current plate", self.state()["plates"]["current"], current)

        self.run("object_select", none=True)
        self.selection_is(f"{s} 6: after the select")
        reply = self.run("widget_click", target={"row": "Cube"})
        self.t.equal(f"{s} 6: the reply's selection", reply.get("selected"), ["Plate 1>Cube"])
        self.selection_is(f"{s} 6", objects=[0])

        self.run("view_set", look_at=[0, 0, 0], zoom=1)
        camera = self.state()["camera"]
        self.t.near(f"{s} 7: the camera looks at the origin", camera["look_at"], [0, 0, 0], 1e-6)
        self.t.near(f"{s} 7: the zoom", camera["zoom"], 1, 1e-6)
        reply = self.run("widget_click", target={"row": "Plate 1>Cube"}, double=True)
        self.t.equal(f"{s} 8: the reply's selection", reply.get("selected"), ["Plate 1>Cube"])
        box = self.c.call("object_list", detail=True)["plates"][0]["objects"][0]["bbox"]
        centre = [(a + b) / 2 for a, b in zip(box["min"], box["max"])]
        camera = self.state()["camera"]
        self.t.near(f"{s} 8: a double click frames the object", camera["look_at"], centre, 0.01)
        self.t.check(f"{s} 8: the double click zooms in", camera["zoom"] > 1, camera["zoom"])

        m = self.menu(self.run("widget_click", target={"row": "Cube"}, button="right"))
        self.t.check(f"{s} 9: the row's menu is the object's", set(OBJECT) <= set(m["items"]),
                     sorted(set(OBJECT) - set(m["items"])))
        self.t.equal(f"{s} 9: the checked items", m["checked"], ["Printable", "Auto Drop"])
        reply = self.run_answered("widget_click", target={"row": "Cube"}, button="right",
                                  popup_item="Add Modifier>Cylinder")
        self.t.equal(f"{s} 10: the reply's item", reply.get("ran"), "Add Modifier>Cylinder")
        self.objects_are(f"{s} 10", O("Cube: Cube part + Generic-Cylinder modifier"))

        reply = self.run("widget_click", target={"row": "Cube>Generic-Cylinder"})
        self.t.equal(f"{s} 11: the reply's selection", reply.get("selected"), ["Plate 1>Cube>Generic-Cylinder"])
        self.t.equal(f"{s} 11: the parts selected", self.selection()["parts"], [[0, 1]])
        reply = self.run("widget_click", target={"row": "Cube>Cube"}, modifiers=["ctrl"])
        self.t.equal(f"{s} 12: the reply's selection", reply.get("selected"),
                     ["Plate 1>Cube>Cube", "Plate 1>Cube>Generic-Cylinder"])
        self.selection_is(f"{s} 12: both parts select the whole cube", objects=[0])
        self.run("widget_click", target={"row": "Cube>Generic-Cylinder"})
        self.t.equal(f"{s} 13: the part alone is selected", self.selection()["parts"], [[0, 1]])
        reply = self.run_answered("widget_click", target={"row": "Cube>Generic-Cylinder"}, button="right",
                                  popup_item="Change Type>Negative Part")
        self.t.equal(f"{s} 14: the reply's item", reply.get("ran"), "Change Type>Negative Part")
        negative = "Cube: Cube part + Generic-Cylinder negative"
        self.objects_are(f"{s} 14", O(negative))

        self.run("widget_click", target={"row": "Plate 1>Cube", "column": "Printable"})
        self.idle()
        self.objects_are(f"{s} 15", O(negative, printable=False))
        reply = self.run("plate_slice")
        self.t.equal(f"{s} 16: the slice's outcome", [p.get("outcome") for p in reply.get("plates", [])], ["empty"])
        self.t.equal(f"{s} 16: plate 0 is not sliced", self.c.call("plate_list")["items"][0]["sliced"], False)

        path = os.path.join(self.folder, "rows.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.idle()
        self.objects_are(f"{s} 17: after the new project")
        self.run("project_open", path=path)
        self.idle()
        self.objects_are(f"{s} 17: the project file keeps the part's type and the printable flag",
                         O(negative, printable=False))

        self.run("widget_click", target={"row": "Plate 1>Cube", "column": "Printable"})
        self.idle()
        self.objects_are(f"{s} 18", O(negative))
        reply = self.run("plate_slice")
        self.t.equal(f"{s} 19: the slice's outcome", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        self.t.equal(f"{s} 19: plate 0 is sliced", self.c.call("plate_list")["items"][0]["sliced"], True)
        state = self.state()
        self.t.equal(f"{s} 20: no dialog and no menu left unseen", (state["dialogs"], menus_left(state, self.began)), ([], []))

    def compare_presets(self):
        """Opens Compare presets, sets its right-hand process preset, turns the transfer checkbox off and clicks the
        layer height row. The window shows the checkbox once the presets differ; the app makes the window once and
        hides it on closing, so it keeps the checkbox as the test's last opening left it."""
        self.run("widget_click", target={"label": "Compare presets"})
        self.idle()
        opened = "Compare presets" in self.windows()
        self.run("widget_set_value", target={"class": "PresetComboBox", "within": "Compare presets", "index": 5},
                 value=FINE)
        self.run("widget_set_value", target=TRANSFER, value=False)
        rows = self.rows("Compare presets")
        reply = self.run("widget_click", target={"row": LAYER_ROW, "within": "Compare presets"})
        return opened, rows, reply

    def transfer_enabled(self):
        """Whether Compare presets lists its Transfer button enabled. The dialog sets the button's state as the app
        idles (wxEVT_UPDATE_UI), so a click right after the row toggles can find it as it was before them."""
        buttons = [line for line in self.listing(self.window_ref("Compare presets"), max_nodes=3000).splitlines()
                   if '] button "Transfer" ' in line]
        return bool(buttons) and "[disabled]" not in buttons[0]

    def dialog_rows(self):
        s = "dialog rows"
        self.start(s)
        self.add_cube()
        self.objects_switch()
        self.run_answered("widget_click", target={"row": "Cube"}, button="right", popup_item="Add Modifier>Cylinder")
        with_modifier = O("Cube: Cube part + Generic-Cylinder modifier")
        self.objects_are(f"{s}: the start", with_modifier)
        self.t.equal(f"{s}: the start: the process preset", self.state()["presets"]["process"], KLIPPER_STD)
        self.t.check(f"{s}: the start: Compare presets is not open", "Compare presets" not in self.windows(),
                     self.windows())

        opened, rows, reply = self.compare_presets()
        self.t.check(f"{s} 1: Compare presets opens", opened, self.windows())
        self.dialogs_are(f"{s} 1: Compare presets is modeless")
        by_path = {r["path"]: r for r in rows}
        self.t.check(f"{s} 2: the header row compares the two presets", COMPARED in by_path, list(by_path))
        self.t.equal(f"{s} 2: the layer height row holds both presets' values",
                     by_path.get(f"{COMPARED}>{LAYER_ROW}", {}).get("value"), "0.2 | 0.12")
        self.t.check(f"{s} 3: the reply names the row", LAYER_ROW in str(reply.get("clicked")), reply)
        selected = [r["path"] for r in self.rows("Compare presets") if r["selected"]]
        self.t.equal(f"{s} 3: the row is selected", selected, [f"{COMPARED}>{LAYER_ROW}"])

        self.run("keyboard_press", target={"class": "DiffPresetDialog"}, keys="Escape")
        self.idle()
        self.t.check(f"{s} 4: Escape closes Compare presets", "Compare presets" not in self.windows(), self.windows())
        self.t.equal(f"{s} 4: the process preset", self.state()["presets"]["process"], KLIPPER_STD)
        self.t.equal(f"{s} 4: comparing changed no process preset", self.modified("process"), [])

        self.compare_presets()
        self.run("widget_set_value", target=TRANSFER, value=True)
        options = [r for r in self.rows("Compare presets") if r["value"] is not None]
        self.t.check(f"{s} 5: every option row starts checked", options and all(r["checked"] for r in options),
                     [(r["path"], r["checked"]) for r in options])
        for r in options:
            if r["checked"]:
                row = r["path"].split(">", 1)[1]
                self.run("widget_set_value", target={"row": row, "within": "Compare presets"}, value=False)
        self.run("widget_set_value", target={"row": LAYER_ROW, "within": "Compare presets"}, value=True)
        options = {r["path"].split(">", 1)[1]: r["checked"] for r in self.rows("Compare presets")
                   if r["value"] is not None}
        self.t.equal(f"{s} 5: the layer height row alone is checked", [p for p, c in options.items() if c], [LAYER_ROW])
        self.t.equal(f"{s} 5: the bottom shell layers row is unchecked", options.get(BOTTOM_ROW), False)

        self.holds(f"{s} 6: Transfer lists as enabled", self.transfer_enabled, "Compare presets' Transfer enabled",
                   lambda: self.listing(self.window_ref("Compare presets"), max_nodes=3000))
        self.run("widget_click", target={"label": "Transfer", "within": "Compare presets"})
        self.idle()
        self.t.check(f"{s} 6: Transfer closes Compare presets", "Compare presets" not in self.windows(), self.windows())
        self.t.equal(f"{s} 6: the process preset", self.state()["presets"]["process"], FINE)
        self.t.equal(f"{s} 6: the right-hand preset is modified", self.modified("process"), [FINE])
        self.t.equal(f"{s} 6: only the checked row was transferred",
                     self.process_settings("layer_height", "bottom_shell_layers"),
                     {"layer_height": "0.2", "bottom_shell_layers": "5"})

        self.run("object_select", objects=[0])
        self.selection_is(f"{s} 7: after the select", objects=[0])
        job = self.c.call("widget_click", target={"row": "Plate 1>Cube"}, button="right",
                          popup_item="Edit in Parameter Table")
        self.t.equal(f"{s} 7: the click waits on the dialog it opens", job.get("status"), "blocked_by_dialog")
        self.dialogs_are(f"{s} 7", "Object/Part Settings")
        self.t.equal(f"{s} 8: the table's rows", [r["path"] for r in self.rows("dialog")],
                     ["Cube", "Cube", "Generic-Cylinder"])
        cell = self.c.call("widget_click", target={"row": "Generic-Cylinder", "within": "Object/Part Settings",
                                                   "column": "Wall loops"})
        self.t.equal(f"{s} 9: a cell's click selects its part", self.selection()["parts"], [[0, 1]])
        reply = self.c.call("dialog_answer", dialog={"title": "Object/Part Settings"}, button="close")
        self.t.equal(f"{s} 10: the reply says the dialog closed", reply.get("closed"), True)
        self.job_ends(f"{s} 10: the step 7 click", job)
        self.dialogs_are(f"{s} 10")
        # The table builds the clicked part's settings beside it; when that outlasts the reply's time, as the first
        # build in Expert mode can, the click replies blocked_by_dialog and its result comes once the dialog closes.
        self.t.equal(f"{s} 9: the reply's column", self.c.await_job(cell, timeout=60).get("column"), "Wall loops")

        popup = ["Print", "Export G-code file"]
        self.run("widget_click", target={"class": "SideButton", "index": 3})
        self.t.equal(f"{s} 11: the print button's popup", self.popup(), ("SidePopup", popup))
        reply = self.run("widget_click", target={"class": "SideButton", "label": "Print"})
        self.idle()
        self.t.equal(f"{s} 12: the reply's button", reply.get("clicked"), "Print")
        self.t.equal(f"{s} 12: the popup is gone", self.c.call("widget_list", target="popup").get("nodes"), 0)
        self.t.equal(f"{s} 12: the print button shows the mode picked", self.side(),
                     ["Slice plate", None, "Print", None])
        self.dialogs_are(f"{s} 12")
        self.run("widget_click", target={"class": "SideButton", "index": 3})
        self.t.equal(f"{s} 13: the print button's popup", self.popup(), ("SidePopup", popup))
        self.run("widget_click", target={"class": "SideButton", "label": "Export G-code file"})
        self.idle()
        self.t.equal(f"{s} 13: the popup is gone", self.c.call("widget_list", target="popup").get("nodes"), 0)
        self.t.equal(f"{s} 13: the print button shows the mode picked", self.side(),
                     ["Slice plate", None, "Export G-code file", None])
        self.dialogs_are(f"{s} 13")
        state = self.state()
        self.t.equal(f"{s} 14: no dialog and no menu left unseen", (state["dialogs"], menus_left(state, self.began)), ([], []))

    def side(self):
        """The labels of the main window's side buttons, in the order widget_list lists them; None for a button
        without a label."""
        labels = []
        for line in self.listing("main", "SideButton", include_menus=False).splitlines():
            m = re.search(r'\] button(?: "(.*?)")? \(SideButton\)', line)
            if m:
                labels.append(m.group(1))
        return labels

    def popup(self):
        """The popup widget_list lists, as (its class, the labels of its buttons in order); None when it lists
        none."""
        lines = self.listing("popup").splitlines()
        head = re.search(r'\] popup \((\w+)\)', lines[0]) if lines else None
        if not head:
            return None
        return head.group(1), [m.group(1) for m in (re.search(r'\] button "(.*?)"', l) for l in lines[1:]) if m]

    def plate_type_list(self):
        s = "a drop-down list"
        self.t.equal(f"{s}: the start: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        self.start(s)
        # The sidebar shows the plate type list only for a printer that supports several plate types, and on Global,
        # as start() leaves it, one list. The switch is an unsaved edit of the printer, which the next start()
        # discards.
        self.run("settings_set", type="printer", values={"support_multi_bed_types": "1"})
        # The switch finds the plate type the app stored for the printer, so the start sets the one it needs.
        self.run("project_set_settings", values={"curr_bed_type": "Cool Plate"})
        self.idle()
        self.t.equal(f"{s}: the start: the plate type", self.bed_type(), "Cool Plate")

        self.run("widget_click", target={"class": "ComboBox", "label": "Smooth Cool Plate"})
        self.t.equal(f"{s} 1: the drop-down list's rows", [r["path"] for r in self.rows("popup")], PLATE_TYPES)
        reply = self.run("widget_click", target={"row": "Textured PEI Plate"})
        self.idle()
        self.t.equal(f"{s} 2: the reply's row", reply.get("clicked"), "Textured PEI Plate")
        self.t.equal(f"{s} 2: the project's plate type", self.bed_type(), "Textured PEI Plate")
        self.t.equal(f"{s} 2: plate 0's plate type", self.c.call("plate_list")["items"][0]["curr_bed_type"],
                     "Textured PEI Plate")
        path = os.path.join(self.folder, "plate_type.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.t.equal(f"{s} 3: the project file keeps the pick", self.project_file(path).get("curr_bed_type"),
                     "Textured PEI Plate")
        self.run("widget_set_value", target={"class": "ComboBox", "label": "Textured PEI Plate"},
                 value="Smooth Cool Plate")
        self.idle()
        self.t.equal(f"{s} 4: the project's plate type", self.bed_type(), "Cool Plate")
        state = self.state()
        self.t.equal(f"{s} 5: no dialog and no menu left unseen", (state["dialogs"], menus_left(state, self.began)), ([], []))

    def hex_field(self):
        """The hex digits the colour picker's hex field lists."""
        for line in self.listing("dialog", "Hex color").splitlines():
            m = re.search(r'= "([0-9A-Fa-f]*)"', line)
            if m:
                return m.group(1)
        return None

    def pick_colour(self, label):
        """Steps 11, 12, 15 and 16 of the web dialogs: opens the filament picker of slot 1's colour, then its colour
        picker, sets the hex field to 3366CC and presses OK; returns the filament picker's job."""
        swatch = self.c.call("widget_click", target={"label": "1", "class": "wxBitmapButton"})
        more = self.c.call("widget_click", target={"label": "+ More Colors", "within": "Select Filament"})
        self.t.equal(f"{label}: the picker on top", self.dialogs(), ["Select Filament", "Color Picker"])
        self.t.equal(f"{label}: the hex field's value",
                     self.c.call("widget_set_value", target=HEX, value="3366CC").get("value"), "3366CCFF")
        reply = self.c.call("widget_click", target={"web": "OK", "within": "Color Picker"})
        self.t.equal(f"{label}: the picker closes", reply.get("closed"), True)
        self.job_ends(f"{label}: the More Colors click", more)
        return swatch

    def script(self, code, **args):
        """webview_run_javascript's reply, or {"error": its refusal's text}."""
        try:
            return self.c.call("webview_run_javascript", code=code, **args)
        except orca_mcp.McpError as e:
            return {"error": refusal(e)[0]}

    def web_view_ref(self, target):
        """The ref of the first web view widget_list lists under `target`, or None."""
        for line in self.listing(target, "webview").splitlines():
            m = re.search(r'\[(w\d+)\] webview', line)
            if m:
                return m.group(1)
        return None

    def ref_of(self, needle):
        """The ref of the first element of the topmost dialog's page whose line holds `needle`, or None."""
        for line in self.listing("dialog").splitlines():
            m = re.search(r'\[(d\d+)\]', line) if needle in line else None
            if m:
                return m.group(1)
        return None

    def web_dialogs(self):
        s = "web dialogs"
        self.t.equal(f"{s}: the start: the presets", start(self.c, filaments=[PLA]),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [PLA]})
        self.start(s)
        # The wizard card step 25 ticks starts unticked only while its printer is not installed.
        if MARLIN in self.printers():
            self.run("printer_uninstall", models=[{"vendor": "Custom", "model": "Generic Marlin Printer"}])
        self.t.equal(f"{s}: the start: the filaments", self.filaments(), [PLA])
        self.t.equal(f"{s}: the start: the colour", self.colours(), "#26A69A")
        self.t.equal(f"{s}: the start: no region", self.region(), "")
        self.t.equal(f"{s}: the start: the printer", self.state()["presets"]["printer"], KLIPPER)
        self.t.check(f"{s}: the start: no Generic Marlin Printer", MARLIN not in self.printers(), self.printers())

        # A plugin an earlier test left may add web views of its own, so the reply's URL tells the page the script
        # ran in.
        self.run("view_set", tab="home")
        home = self.web_view_ref("main")
        self.t.check(f"{s} 1: the Home tab shows a web view", home is not None, self.listing("main", "webview"))
        reply = self.script("1 + 1")
        self.t.check(f"{s} 1: without a modal, the Home page takes a script",
                     reply.get("result") == 2 and "web/homepage/index.html" in reply.get("url", ""), reply)

        job = self.c.call("action_run", id=PRESET_BUNDLE)
        self.t.equal(f"{s} 2: the action waits on its dialog", job.get("status"), "blocked_by_dialog")
        self.dialogs_are(f"{s} 2", "Preset Bundle")
        page = self.listing("dialog")
        for name in ("name=#refresh_btn", "name=#close_btn", 'label "Version"', 'label "Update"'):
            self.t.check(f"{s} 2: the page lists {name}", name in page, page)
        self.t.refused(f"{s} 3: a web view behind the modal is refused",
                       lambda: self.c.call("webview_run_javascript", code="1", target=home), words="modal dialog is open")
        self.t.equal(f"{s} 4: without a target, the bundle dialog's page takes a script",
                     self.script("document.title").get("result"), "Preset Bundle")
        self.t.equal(f"{s} 5: and by locator",
                     self.script("!!document.getElementById('close_btn')", target=BUNDLE_VIEW).get("result"), True)
        self.script("SendWXMessage(JSON.stringify({command: 'export_page'}))")
        self.holds(f"{s} 6: the export dialog opens above it", lambda: self.dialogs() == ["Preset Bundle", EXPORT],
                   "the dialogs Preset Bundle and Export Preset Bundle", self.dialogs)
        # Its web view shows about:blank until its first navigation starts.
        until(lambda: self.page_lists("ExportPresetDialog", query="webview"), "the Export dialog's web view loaded")
        self.t.refused(f"{s} 7: the dialog under the topmost is refused",
                       lambda: self.c.call("webview_run_javascript", code="1", target=BUNDLE_VIEW), words="is under")
        url = self.script("1").get("url", "")
        self.t.check(f"{s} 8: without a target, the topmost dialog's page is used", "ExportPresetDialog" in url, url)
        self.c.call("dialog_answer", button="close")
        self.holds(f"{s} 9: the export dialog closes", lambda: self.dialogs() == ["Preset Bundle"],
                   "the dialog Preset Bundle alone", self.dialogs)
        # A page that closes its dialog never answers the click: the reply comes from the close, where a click whose
        # page does not answer is refused after the call's 10 s.
        reply = self.c.call("widget_click", target={"css": "#close_btn", "within": "Preset Bundle"})
        self.t.equal(f"{s} 10: the page's Close closes the dialog", reply.get("closed"), True)
        self.t.check(f"{s} 10: and the reply lists no dialog left under it", "dialogs" not in reply, reply)
        self.job_ends(f"{s} 10: the step 2 action", job)
        self.dialogs_are(f"{s} 10")

        self.run("view_set", tab="prepare")
        swatch = self.c.call("widget_click", target={"label": "1", "class": "wxBitmapButton"})
        self.t.equal(f"{s} 11: the swatch waits on its dialog", swatch.get("status"), "blocked_by_dialog")
        self.dialogs_are(f"{s} 11", "Select Filament")
        self.t.check(f"{s} 11: the dialog lists the slot's colour", "#26A69A" in self.listing("dialog"),
                     self.listing("dialog"))
        more = self.c.call("widget_click", target={"label": "+ More Colors", "within": "Select Filament"})
        self.t.equal(f"{s} 12: More Colors waits on its dialog", more.get("status"), "blocked_by_dialog")
        self.dialogs_are(f"{s} 12: the picker on top", "Select Filament", "Color Picker")
        reply = self.c.call("widget_set_value", target=HEX, value="FF0000")
        self.t.equal(f"{s} 13: the reply's value", reply.get("value"), "FF0000FF")
        self.t.equal(f"{s} 13: the hex field", self.hex_field(), "FF0000FF")
        reply = self.c.call("mouse_gesture", target={"css": "#spectrum", "within": "Color Picker"}, action="drag",
                            x=20, y=20, to=[200, 100], steps=6)
        self.t.equal(f"{s} 14: the reply's gesture", reply.get("done"), "drag")
        picked = self.hex_field()
        self.t.check(f"{s} 14: the drag picked another colour",
                     bool(picked and re.fullmatch(r"[0-9A-Fa-f]{8}", picked) and picked.upper() != "FF0000FF"), picked)
        reply = self.c.call("widget_set_value", target=HEX, value="3366CC")
        self.t.equal(f"{s} 15: the reply's value", reply.get("value"), "3366CCFF")
        self.t.equal(f"{s} 15: the hex field", self.hex_field(), "3366CCFF")
        reply = self.c.call("widget_click", target={"web": "OK", "within": "Color Picker"})
        self.t.equal(f"{s} 16: the picker closes", reply.get("closed"), True)
        self.t.equal(f"{s} 16: the dialog under it", [d.get("title") for d in reply.get("dialogs", [])],
                     ["Select Filament"])
        self.job_ends(f"{s} 16: the step 12 click", more)
        page = self.listing("dialog")
        self.t.check(f"{s} 16: the dialog lists the picked colour and not the old one",
                     "#3366CC" in page and "#26A69A" not in page, page)
        self.c.call("dialog_answer", dialog={"title": "Select Filament"}, button="cancel")
        self.job_ends(f"{s} 17: the step 11 click", swatch)
        self.dialogs_are(f"{s} 17")
        self.t.equal(f"{s} 17: a cancelled pick changes nothing", self.colours(), "#26A69A")

        swatch = self.pick_colour(f"{s} 18")
        self.c.call("dialog_answer", dialog={"title": "Select Filament"}, button="ok")
        self.job_ends(f"{s} 18: the swatch's click", swatch)
        self.dialogs_are(f"{s} 18")
        self.t.equal(f"{s} 18: the colour the picker returned is slot 1's", self.colours(), "#3366CC")
        path = os.path.join(self.folder, "colour.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.t.equal(f"{s} 19: the project file keeps the colour", self.project_file(path).get("filament_colour"),
                     ["#3366CC"])

        job = self.c.call("action_run", id=SETUP_WIZARD)
        self.t.equal(f"{s} 20: the action waits on its dialog", job.get("status"), "blocked_by_dialog")
        self.dialogs_are(f"{s} 20", "Setup Wizard")
        self.holds(f"{s} 20: the page lists Europe", lambda: self.page_lists('"Europe"'), "the wizard lists Europe",
                   lambda: self.listing("dialog"))
        reply = self.c.call("widget_click", target={"web": "Europe", **WIZARD})
        self.t.equal(f"{s} 21: the reply's check", reply.get("checked"), True)
        self.t.check(f"{s} 21: the wizard lists Europe checked", self.web_checked("Europe"),
                     self.listing("dialog", "Europe"))
        next_ref = self.ref_of('button "Next"')
        reply = self.c.call("widget_click", target=next_ref)
        self.t.check(f"{s} 22: Next navigates to the printer page",
                     reply.get("navigated") is True and "/guide/21/" in reply.get("url", ""), reply)
        # The reply comes once the web view's URL changes, which the old document can outlast for a while; the ref
        # is stale once the new one runs.
        until(lambda: "/guide/21/" in str(self.script("location.pathname").get("result")),
              "the wizard's page runs at /guide/21/")
        self.t.refused(f"{s} 23: the ref from the page before is stale",
                       lambda: self.c.call("widget_click", target=next_ref), words="page changed")
        reply = self.script("location.reload(); 1")
        self.t.equal(f"{s} 24: a reload at the same URL replies navigated", reply.get("navigated"), True)
        card = "Nozzle 0.4 Marlin"
        until(lambda: self.page_lists(card), f"the wizard lists {card}")
        reply = self.c.call("widget_click", target={"web": card, "kind": "button", **WIZARD})
        self.t.equal(f"{s} 25: the reply's check", reply.get("checked"), True)
        self.t.check(f"{s} 25: the wizard lists the card checked", self.web_checked(card),
                     self.listing("dialog", card))
        reply = self.c.call("dialog_answer", dialog={"title": "Setup Wizard"}, button="close")
        self.t.equal(f"{s} 26: the reply says the wizard closed", reply.get("closed"), True)
        self.job_ends(f"{s} 26: the step 20 action", job)
        self.dialogs_are(f"{s} 26")
        self.t.equal(f"{s} 26: closing the wizard keeps no region", self.region(), "")
        self.t.equal(f"{s} 26: closing the wizard keeps the printer", self.state()["presets"]["printer"], KLIPPER)
        self.t.check(f"{s} 26: closing the wizard installs no printer", MARLIN not in self.printers(),
                     self.printers())

        # The Klipper printer's sidebar offers Flushing volumes once a second filament is added. Its dialog's web
        # view is the app's plain wxWebView, not a WebViewHostDialog's as the bundle dialogs' are.
        self.run("widget_click", target={"label": "Add one filament"})
        flush = self.c.call("widget_click", target={"label": "Flushing volumes"})
        self.holds(f"{s} 27: the Flushing volumes dialog opens", lambda: self.dialogs() == [FLUSH],
                   "the dialog Flushing volumes", self.dialogs)
        # Its web view, too, shows about:blank until its first navigation starts.
        until(lambda: self.page_lists("WipingDialog.html", query="webview"), "the Flushing volumes web view loaded")
        reply = self.script("typeof document.title")
        self.t.check(f"{s} 28: its page takes a script",
                     reply.get("result") == "string" and "web/flush/WipingDialog.html" in reply.get("url", ""), reply)
        self.c.call("dialog_answer", button="close")
        self.job_ends(f"{s} 29: the step 27 click", flush)
        self.dialogs_are(f"{s} 29")

    def printers(self):
        """The names of the printer presets preset_list offers."""
        return [p["name"] for p in self.c.call("preset_list", type="printer", limit=500)["items"]]

    def page_lists(self, text, query=None):
        """Whether the topmost dialog's listing, with `query` when given, holds `text`."""
        return text in self.listing("dialog", query)

    def web_checked(self, label):
        """Whether the topmost dialog's page lists an element whose label holds `label` as checked."""
        return any(label in line and "[checked]" in line for line in self.listing("dialog", label).splitlines())

    def region(self):
        return self.c.call("preferences_get", keys=["region"])["items"][0]["value"]

    def run_all(self, only=None):
        self.t.equal("the start: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        # The baseline's settings the checks depend on: E's pixel and the canvas captures follow the window's size;
        # the menus scenario turns the labels on and off; and without printer agents, a printer without a print host
        # gives the print button's popup its two items.
        self.t.equal("the start: the window's size", self.state()["window"]["size"], WINDOW_SIZE)
        self.t.equal("the start: labels are off", self.state()["view"]["labels"], False)
        self.t.equal("the start: no printer agents",
                     self.c.call("preferences_get", keys=["use_printer_agents"])["items"][0]["value"], False)
        for name, scenario in [("menus", self.menus), ("object list rows", self.object_rows),
                               ("dialog rows", self.dialog_rows), ("web dialogs", self.web_dialogs),
                               ("a drop-down list", self.plate_type_list)]:
            if not only or name in only:
                self.t.scenario(name, scenario)
        return self.t.exit_code()


HEX = {"web": "Hex color", "within": "Color Picker"}
TRANSFER = {"label": "Transfer values from left to right", "within": "Compare presets"}
WIZARD = {"within": "Setup Wizard"}
BUNDLE_VIEW = {"class": "webview", "within": "Preset Bundle"}
EXPORT = "Export Preset Bundle"
FLUSH = "Flushing volumes for filament change"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="folder for the pictures and project files (default: a new temporary folder)")
    ap.add_argument("--only", action="append", help="run only this scenario (repeatable)")
    args = ap.parse_args()
    folder = args.out or tempfile.mkdtemp(prefix="orca-gui-tools-")
    os.makedirs(folder, exist_ok=True)
    print(f"output folder: {folder}", flush=True)
    client, _ = orca_mcp.session_client()
    return GuiToolsTest(client, folder).run_all(args.only)


if __name__ == "__main__":
    sys.exit(main())
