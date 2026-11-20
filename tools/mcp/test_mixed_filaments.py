#!/usr/bin/env python3
"""The mixed filaments of the filament list, as the test_mixed_filaments entry of
tools/mcp/test-entries/filaments_settings.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_mixed_filaments.py

Reading the mixed filaments with filament_list; adding one with filament_add_mixed and changing one with
filament_edit_mixed, each from its arguments and through its dialog, where the gradient is set; a mixed slot having no
preset or colour of its own; and what deleting or merging a filament that a mix mixes does to the mix: broken, as the
sidebar leaves it, and repaired by an edit. After every step the test reads the slots and the mixes back with
filament_list and compares them, whole, with the state the entry writes out. Every scenario runs on the Generic
Klipper Printer 0.4 from a start of its own (checks.start()) with three physical slots, and the test puts slot 1's
colour back at its end. It exits 0 when every check passes.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, filament_rows, run, slots, start, wait  # noqa: E402

START = {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}
RED, BLUE = "#FF0000", "#0000FF"
HEX = re.compile(r"^#[0-9A-F]{6}$")
EDIT, ADD = "Edit Mixed Filament", "Add Mixed Filament"


class MixedFilamentsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder
        self.slot1_colour = None

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def rows(self):
        return filament_rows(self.c)

    def row(self, slot):
        return self.rows()[slot - 1]

    def mixes(self):
        """The mixed rows as the entry writes them: `4 = 2,3 @ 50,50, bottom 3 top 2, per part`, joined by `·`."""
        out = []
        for row in self.rows():
            if not row["mixed"]:
                continue
            text = f"{row['filament_slot']} = {','.join(map(str, row['components']))} @ {','.join(map(str, row['ratios']))}"
            gradient = row.get("gradient")
            if gradient:
                text += f", bottom {gradient['bottom']} top {gradient['top']}"
                if gradient.get("per_part"):
                    text += ", per part"
                if gradient.get("curve") != "linear":
                    text += f", curve {gradient.get('curve')}"
            if row.get("broken"):
                text += ", broken"
            out.append(text)
        return " · ".join(out)

    def state_is(self, label, want_slots, want_mixes):
        self.t.equal(f"{label}: the slots", slots(self.c), want_slots)
        self.t.equal(f"{label}: the mixes", self.mixes(), want_mixes)

    def dialogs(self):
        return [{"title": d.get("title"), "buttons": [b.get("label") for b in d.get("buttons", [])],
                 "message": d.get("message", "")} for d in self.c.call("app_state").get("dialogs", [])]

    def on(self, name):
        """The filament of the object named `name`, from object_list."""
        listing = self.c.call("object_list", limit=500)
        found = [o for p in listing["plates"] for o in p["objects"] if o["name"] == name]
        found += [o for o in listing.get("outside", []) if o["name"] == name]
        return found[0]["filament"] if len(found) == 1 else found

    def held(self, label, reply, title):
        """A call the dialog titled `title` holds: the reply waits on it, and it is the one dialog open, with OK and
        Cancel."""
        self.t.equal(f"{label}: the call waits on a dialog", reply.get("status"), "blocked_by_dialog")
        shown = self.dialogs()
        self.t.equal(f"{label}: the dialog", [d["title"] for d in shown], [title])
        self.t.check(f"{label}: with OK and Cancel", bool(shown) and {"OK", "Cancel"} <= set(shown[0]["buttons"]), shown)
        return shown

    def finish(self, label, reply):
        """The job of a call a dialog held, once the dialog is answered, and no dialog left open."""
        result = self.c.await_job(reply, timeout=60)
        self.t.equal(f"{label}: no dialog is left open", self.dialogs(), [])
        return result

    def set_in_dialog(self, title, **values):
        for name, value in values.items():
            self.c.call("widget_set_value", target={"name": name, "within": title}, value=value)

    def click(self, title, label):
        self.c.call("widget_click", target={"label": label, "within": title})

    def cube(self, name, x):
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20], name=name, position=[x, 100, 0])

    def save_delete_open(self, label, name, slot, want_slots):
        """Adds a cube, saves the project to `name`, deletes mixed filament `slot`, and opens the file again. The cube is
        there so that the open shows no warning, which the app gives for a project file that holds no object."""
        self.cube("kept", 100)
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        self.run("filament_delete", filament_slot=slot)
        self.t.equal(f"{label}: after the delete", slots(self.c), want_slots)
        self.run("project_open", path=path, discard_changes=True)
        wait(self.c, "idle")
        self.t.equal(f"{label}: the cube is back", self.on("kept"), 1)

    def begin(self, label, colours=False):
        """The scenarios' start: three physical slots of Generic PLA, slots 2 and 3 red and blue with `colours`."""
        self.t.equal(f"{label}: the start's presets", start(self.c), START)
        if self.slot1_colour is None:
            self.slot1_colour = self.row(1)["colour"]
        for _ in range(2):
            self.run("filament_add")
        self.t.equal(f"{label}: three physical slots", slots(self.c), "0,0,0")
        self.t.equal(f"{label}: of Generic PLA", [(r.get("preset"), r.get("type")) for r in self.rows()],
                     [(GENERIC_PLA, "PLA")] * 3)
        if colours:
            self.run("filament_set_colour", filament_slot=2, colour=RED)
            self.run("filament_set_colour", filament_slot=3, colour=BLUE)
            self.t.equal(f"{label}: slots 2 and 3 red and blue", [r["colour"] for r in self.rows()[1:]], [RED, BLUE])

    @staticmethod
    def red_blue(colour):
        return int(colour[1:3], 16), int(colour[5:7], 16)

    # ---- Scenarios ----

    def adding(self):
        s = "adding and reading"
        self.begin(s)
        reply = self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.t.equal(f"{s} 1: the reply", reply, {"added": True, "filament_slot": 4})
        self.state_is(f"{s} 1", "0,0,0,1", "4 = 2,3 @ 50,50")
        row = self.row(4)
        self.t.check(f"{s} 1: the mix has a colour and no preset or type of its own",
                     bool(HEX.match(row.get("colour", ""))) and "preset" not in row and "type" not in row, row)
        self.t.equal(f"{s} 1: the physical slots keep their preset", [r.get("preset") for r in self.rows()[:3]],
                     [GENERIC_PLA] * 3)
        self.t.equal(f"{s} 1: the matrix is 4 x 4", len(self.c.call("filament_list")["flush_volumes_matrix"]), 16)

        reply = self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.t.equal(f"{s} 2: a mix the project has is not added again", reply, {"added": False, "filament_slot": 4})
        self.state_is(f"{s} 2", "0,0,0,1", "4 = 2,3 @ 50,50")

        reply = self.run("filament_add_mixed", components=[1, 2, 3], ratios=[20, 30, 50])
        self.t.equal(f"{s} 3: the reply", reply, {"added": True, "filament_slot": 5})
        after3 = ("0,0,0,1,1", "4 = 2,3 @ 50,50 · 5 = 1,2,3 @ 20,30,50")
        self.state_is(f"{s} 3", *after3)

        self.t.refused(f"{s} 4: components without ratios are refused",
                       lambda: self.c.call("filament_add_mixed", components=[2, 3]), code="components_and_ratios")
        self.state_is(f"{s} 4", *after3)

        reply = self.run("filament_add")
        self.t.equal(f"{s} 5: a physical slot goes before the mixes", reply.get("filament_slot"), 4)
        after5 = ("0,0,0,0,1,1", "5 = 2,3 @ 50,50 · 6 = 1,2,3 @ 20,30,50")
        self.state_is(f"{s} 5", *after5)

        self.save_delete_open(f"{s} 6", "mixes.3mf", 6, "0,0,0,0,1")
        self.state_is(f"{s} 6: the project file keeps its mixes", *after5)

    def no_preset(self):
        s = "a mixed slot has no preset or colour of its own"
        self.begin(s)
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        unchanged = ("0,0,0,1", "4 = 2,3 @ 50,50")
        self.state_is(f"{s}: the start", *unchanged)
        code = "mixed_filament_slot"

        self.t.refused(f"{s} 1: settings_get", lambda: self.c.call(
            "settings_get", type="filament", filament_slot=4, keys=["filament_cost"]), code=code)
        self.state_is(f"{s} 1", *unchanged)
        self.t.refused(f"{s} 2: settings_set", lambda: self.c.call(
            "settings_set", type="filament", filament_slot=4, values={"filament_cost": 99}), code=code)
        saved = self.c.call("settings_get", type="filament", filament_slot=2, compare=":saved")
        self.t.equal(f"{s} 2: the preset slots 1 to 3 hold has no unsaved change", saved.get("items"), [])
        self.state_is(f"{s} 2", *unchanged)
        self.t.refused(f"{s} 3: settings_list", lambda: self.c.call(
            "settings_list", type="filament", filament_slot=4), code=code)
        self.state_is(f"{s} 3", *unchanged)
        self.t.refused(f"{s} 4: preset_save", lambda: self.c.call(
            "preset_save", type="filament", filament_slot=4, name="MCP Mixed Slot"), code=code)
        users = [p.get("name") for p in self.c.call("preset_list", type="filament", source="user", limit=500)["items"]]
        self.t.check(f"{s} 4: no preset was saved", "MCP Mixed Slot" not in users, users)
        self.state_is(f"{s} 4", *unchanged)

        items = self.c.call("preset_list", type="filament", query="Generic PLA", limit=500)["items"]
        used = [i.get("filament_slots") for i in items if i.get("name") == GENERIC_PLA]
        self.t.equal(f"{s} 5: the mixed slot does not use its first component's preset", used, [[1, 2, 3]])
        self.state_is(f"{s} 5", *unchanged)

        self.t.refused(f"{s} 6: another colour", lambda: self.c.call(
            "filament_set_colour", filament_slot=4, colour="#00FF00"), words="Slot 4 is a mixed filament")
        own = self.row(4)["colour"]
        self.t.check(f"{s} 6: the mix's colour is not set", own != "#00FF00", own)
        self.state_is(f"{s} 6", *unchanged)
        reply = self.run("filament_set_colour", filament_slot=4, colour=own.lower())
        self.t.equal(f"{s} 7: its own colour, in any case, changes nothing", reply.get("changed"), False)
        self.state_is(f"{s} 7", *unchanged)

        self.t.refused(f"{s} 8: a mixed filament's flushing volume", lambda: self.c.call(
            "filament_set_flushing", flush_volumes_matrix={"index": 3, "value": 50}),
            words="belongs to a mixed filament, which is never flushed")
        entry = self.c.call("filament_list")["flush_volumes_matrix"][3]
        self.t.check(f"{s} 8: entry 3 is not set", entry != 50, entry)
        self.state_is(f"{s} 8", *unchanged)

    def changing(self):
        s = "changing a mix"
        self.begin(s, colours=True)
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.cube("on_mix", 100)
        self.run("settings_set", target="object", object=0, values={"extruder": 4})
        self.state_is(f"{s}: the start", "0,0,0,1", "4 = 2,3 @ 50,50")
        self.t.equal(f"{s}: the start: the cube on the mix", self.on("on_mix"), 4)

        def edit(**args):
            return self.run("filament_edit_mixed", filament_slot=4, **args)

        reply = edit(components=[2, 3], ratios=[70, 30])
        self.t.equal(f"{s} 1: the reply", reply, {"filament_slot": 4, "changed": True})
        self.state_is(f"{s} 1", "0,0,0,1", "4 = 2,3 @ 70,30")
        red, blue = self.red_blue(self.row(4)["colour"])
        self.t.check(f"{s} 1: the blend has more red than blue", red > blue, self.row(4)["colour"])
        self.t.equal(f"{s} 1: the cube stays on the mix", self.on("on_mix"), 4)

        reply = edit(components=[2, 3], ratios=[70, 30])
        self.t.equal(f"{s} 2: the same mix changes nothing", reply.get("changed"), False)
        self.state_is(f"{s} 2", "0,0,0,1", "4 = 2,3 @ 70,30")

        edit(components=[2, 3], ratios=[30, 70])
        self.state_is(f"{s} 3", "0,0,0,1", "4 = 2,3 @ 30,70")
        red, blue = self.red_blue(self.row(4)["colour"])
        self.t.check(f"{s} 3: the blend has more blue than red", blue > red, self.row(4)["colour"])
        self.t.equal(f"{s} 3: the cube stays on the mix", self.on("on_mix"), 4)

        edit(components=[1, 2, 3], ratios=[20, 30, 50])
        after4 = ("0,0,0,1", "4 = 1,2,3 @ 20,30,50")
        self.state_is(f"{s} 4: the slot keeps its number", *after4)
        self.t.equal(f"{s} 4: the cube stays on the mix", self.on("on_mix"), 4)

        for step, args, code in [
                (5, {"filament_slot": 2, "components": [2, 3], "ratios": [50, 50]}, "not_mixed_filament"),
                (6, {"filament_slot": 9, "components": [1, 2, 3], "ratios": [20, 30, 50]}, "no_such_slot"),
                (7, {"filament_slot": 4, "components": [2, 3]}, "components_and_ratios"),
                (8, {"filament_slot": 4, "components": [2, 2], "ratios": [50, 50]}, "duplicate_component")]:
            self.t.refused(f"{s} {step}: refused", lambda: self.c.call("filament_edit_mixed", **args), code=code)
            self.state_is(f"{s} {step}", *after4)
            self.t.equal(f"{s} {step}: the cube stays on the mix", self.on("on_mix"), 4)

    def gradient(self):
        s = "a gradient, through the dialogs"
        self.begin(s)
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.state_is(f"{s}: the start", "0,0,0,1", "4 = 2,3 @ 50,50")

        reply = self.c.call("filament_edit_mixed", filament_slot=4)
        self.held(f"{s} 1", reply, EDIT)
        self.state_is(f"{s} 1", "0,0,0,1", "4 = 2,3 @ 50,50")

        self.set_in_dialog(EDIT, gradient=True, gradient_direction=1, per_part_gradient=True)
        self.click(EDIT, "OK")
        self.t.equal(f"{s} 2: the job's reply", self.finish(f"{s} 2", reply), {"filament_slot": 4, "changed": True})
        self.state_is(f"{s} 2", "0,0,0,1", "4 = 2,3 @ 50,50, bottom 3 top 2, per part")

        self.run("filament_edit_mixed", filament_slot=4, components=[3, 2], ratios=[50, 50])
        self.state_is(f"{s} 3: a gradient stays for two", "0,0,0,1", "4 = 3,2 @ 50,50, bottom 2 top 3, per part")

        self.run("filament_edit_mixed", filament_slot=4, components=[1, 2, 3], ratios=[20, 30, 50])
        after4 = ("0,0,0,1", "4 = 1,2,3 @ 20,30,50")
        self.state_is(f"{s} 4: a gradient goes for three", *after4)

        reply = self.c.call("filament_edit_mixed", filament_slot=4)
        self.held(f"{s} 5", reply, EDIT)
        self.click(EDIT, "Cancel")
        self.t.equal(f"{s} 5: the job's reply", self.finish(f"{s} 5", reply), {"filament_slot": 4, "changed": False})
        self.state_is(f"{s} 5", *after4)

        reply = self.c.call("filament_add_mixed")
        self.held(f"{s} 6", reply, ADD)
        self.state_is(f"{s} 6", *after4)
        self.set_in_dialog(ADD, gradient=True)
        self.click(ADD, "OK")
        self.t.equal(f"{s} 7: the job's reply", self.finish(f"{s} 7", reply), {"added": True, "filament_slot": 5})
        after7 = ("0,0,0,1,1", "4 = 1,2,3 @ 20,30,50 · 5 = 1,2 @ 50,50, bottom 1 top 2")
        self.state_is(f"{s} 7", *after7)

        reply = self.c.call("filament_add_mixed")
        self.held(f"{s} 8", reply, ADD)
        self.click(ADD, "Cancel")
        self.t.equal(f"{s} 8: the job's reply", self.finish(f"{s} 8", reply), {"added": False})
        self.state_is(f"{s} 8", *after7)

        self.save_delete_open(f"{s} 9", "gradient.3mf", 5, "0,0,0,1")
        self.state_is(f"{s} 9: the project file keeps the gradient", *after7)

    def deleting(self):
        s = "deleting or merging a filament that a mix mixes"
        self.begin(s)
        self.run("filament_add_mixed", components=[1, 2], ratios=[50, 50])
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.cube("on_one", 70)
        self.cube("on_mix", 130)
        self.run("settings_set", target="object", object=1, values={"extruder": 4})
        start_state = ("0,0,0,1,1", "4 = 1,2 @ 50,50 · 5 = 2,3 @ 50,50")
        self.state_is(f"{s}: the start", *start_state)
        self.t.equal(f"{s}: the start: the cubes' filaments", (self.on("on_one"), self.on("on_mix")), (1, 4))

        reply = self.c.call("filament_delete", filament_slot=1, merge_into=4)
        shown = self.held(f"{s} 1", reply, "Warning")
        self.t.check(f"{s} 1: the question says the mix uses the filament",
                     bool(shown) and "uses this physical filament" in shown[0]["message"], shown)
        self.c.call("dialog_answer", button="Cancel")
        self.t.refused(f"{s} 1: the job", lambda: self.c.await_job(reply, timeout=60), code="merge_cancelled")
        self.t.equal(f"{s} 1: no dialog is left open", self.dialogs(), [])
        self.state_is(f"{s} 1", *start_state)
        self.t.equal(f"{s} 1: the cube stays on filament 1", self.on("on_one"), 1)

        self.t.refused(f"{s} 2: the merge answered no ahead", lambda: self.c.call(
            "filament_delete", filament_slot=1, merge_into=4, break_mix=False), code="mixed_uses_slot")
        self.t.equal(f"{s} 2: no dialog opened", self.dialogs(), [])
        self.state_is(f"{s} 2", *start_state)

        reply = self.run("filament_delete", filament_slot=1, merge_into=4, break_mix=True)
        self.t.equal(f"{s} 3: the reply", {k: reply.get(k) for k in ("filament_slot", "merged_into", "broken_mixed")},
                     {"filament_slot": 1, "merged_into": 3, "broken_mixed": [3]})
        self.t.equal(f"{s} 3: no dialog opened", self.dialogs(), [])
        after3 = ("0,0,1,1", "3 = 0,1 @ 50,50, broken · 4 = 1,2 @ 50,50")
        self.state_is(f"{s} 3", *after3)
        self.t.equal(f"{s} 3: the cubes are on the mix", (self.on("on_one"), self.on("on_mix")), (3, 3))

        reply = self.c.call("plate_slice")
        self.t.equal(f"{s} 4: the slice of an object on a broken mix ends in an error",
                     [(p.get("index"), p.get("outcome"), p.get("error")) for p in reply.get("plates", [])],
                     [(0, "error", "A mixed filament has a deleted or mismatched component")])
        self.state_is(f"{s} 4", *after3)

        reply = self.run("filament_edit_mixed", filament_slot=3, components=[1, 2], ratios=[50, 50])
        self.t.equal(f"{s} 5: an edit repairs it", reply.get("changed"), True)
        self.state_is(f"{s} 5", "0,0,1,1", "3 = 1,2 @ 50,50 · 4 = 1,2 @ 50,50")

        reply = self.run("filament_delete", filament_slot=2)
        self.t.equal(f"{s} 6: the reply names the broken mixes", reply.get("broken_mixed"), [2, 3])
        self.t.equal(f"{s} 6: no dialog opened", self.dialogs(), [])
        after6 = ("0,1,1", "2 = 1,0 @ 50,50, broken · 3 = 1,0 @ 50,50, broken")
        self.state_is(f"{s} 6", *after6)

        self.t.refused(f"{s} 7: the last physical slot", lambda: self.c.call("filament_delete", filament_slot=1),
                       code="last_filament_slot")
        self.state_is(f"{s} 7", *after6)

        self.t.refused(f"{s} 8: a mix beside one physical slot", lambda: self.c.call("filament_delete", filament_slot=2),
                       code="mixed_list_hidden")
        self.state_is(f"{s} 8", *after6)

    def run_all(self):
        for name, scenario in [("adding and reading", self.adding), ("no preset of its own", self.no_preset),
                               ("changing a mix", self.changing), ("a gradient", self.gradient),
                               ("deleting or merging", self.deleting)]:
            self.t.scenario(name, scenario)
        if self.slot1_colour is not None:
            self.run("filament_set_colour", filament_slot=1, colour=self.slot1_colour)
            self.t.equal("slot 1's colour is put back", self.row(1)["colour"], self.slot1_colour)
        self.run("project_new", discard_changes=True, preset_changes="discard")
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-mixed-") as folder:
        return MixedFilamentsTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
