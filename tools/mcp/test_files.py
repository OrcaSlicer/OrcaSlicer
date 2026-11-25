#!/usr/bin/env python3
"""The project's files, as the test_files entry of tools/mcp/test-entries/files.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_files.py

It writes into a folder of its own, kept when a check fails. Exits 0 when every check passes.
"""

import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, answer_all, dialogs, gcode, layer_gcode,  # noqa: E402
                    meshes, normal, plates, presets, project, recent, reimport, run, sliced, sliced_flags, start,
                    undo_steps, unsaved, until, values, wait)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BOX_STL = os.path.join(REPO, "tests", "data", "test_stl", "ASCII", "20mmbox-LF.stl")
STEP = os.path.join(REPO, "tests", "data", "named_single_body.step")
BENCHY = os.path.join(REPO, "resources", "handy_models", "3DBenchy.drc")
HEIGHTMAP = os.path.join(HERE, "tests", "fixtures", "heightmap.png")

# The preferences the checks depend on, each at the baseline's value, a new data folder's: an open that asks nothing
# about the project's settings, the recent projects, the STEP options dialog and no slice by itself.
PREFERENCES = {"project_load_behaviour": "ask_when_relevant", "max_recent_count": 18, "recent_models": False,
               "enable_step_mesh_setting": True, "auto_slice_after_change": False}
# A filament preset file no other test writes, imported as a model.
USER_PLA = "MCP Files PLA"
USER_PLA_FILE = {"name": USER_PLA, "filament_settings_id": [USER_PLA], "inherits": GENERIC_PLA, "from": "User",
                 "filament_cost": ["24"], "version": "2.8.0.16"}
NO_GEOMETRY = "The file does not contain any geometry data."
NEW_PROJECT_QUESTION = "Creating a new project: unsaved changes"
LOAD_PROJECT_QUESTION = "Load project: unsaved changes"
SAVE_QUESTION = "Orca Slicer - Save"
STEP_DIALOG = "STEP file import parameters"
NEW_PROJECT = "new_project"
# The undo steps a change of the current plate logs: Plater::select_plate() takes one, and PartPlateList::select_plate()
# another when the plate changes.
SELECTED = ["select partplate!", "select partplate!"]
ROW = re.compile(r'^"(.*)" (\d+)( \*)?$')

# A cube's corners and its twelve triangles, each turned outward.
CORNERS = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
TRIANGLES = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]


def cube_stl(size):
    """An ASCII STL of a cube of `size` mm."""
    lines = ["solid cube"]
    for triangle in TRIANGLES:
        lines += ["  facet normal 0 0 0", "    outer loop"]
        lines += ["      vertex %g %g %g" % tuple(c * size for c in CORNERS[i]) for i in triangle]
        lines += ["    endloop", "  endfacet"]
    lines.append("endsolid cube")
    return "\n".join(lines) + "\n"


def P(*rows):
    """Plates written as the entry writes them: '"" 1 *' is an unnamed plate with one object, current."""
    out = []
    for row in rows:
        m = ROW.match(row)
        assert m, row
        out.append({"name": m.group(1), "current": bool(m.group(3)), "locked": False, "objects": int(m.group(2))})
    return out


class FilesTest:
    def __init__(self, client, session, folder):
        self.c = client
        self.session = session
        self.t = Checks()
        self.folder = folder
        self.tag = os.path.basename(folder)
        # wall_loops values neither the process (3) nor an earlier attempt has.
        self.next_walls = 4

    # ---- Calls ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def w(self, *names):
        return os.path.join(self.folder, *names)

    def follow(self, reply, tool):
        """Follows the job of a call whose dialogs the test has answered to its end. A dialog still open raises, as
        run() does, since nothing would answer it."""
        deadline = time.monotonic() + 600
        while isinstance(reply, dict) and "job" in reply and reply.get("status") in ("running", "blocked_by_dialog"):
            shown = dialogs(self.c)
            if shown:
                raise orca_mcp.McpError(f"{tool} is held by the dialog {shown}", reply)
            if time.monotonic() > deadline:
                raise TimeoutError(f"{tool} still runs after 600 s")
            reply = self.c.call("app_wait", **{"for": "job", "job": reply["job"], "timeout_s": 10})
        return reply

    def cube(self, name, size, x, y):
        self.run("object_add_primitive", shape="cube", size=[size] * 3, name=name, position=[x, y, 0])

    def cylinder(self, name):
        self.run("object_add_primitive", shape="cylinder", size=[20, 10], name=name)

    def new_project(self):
        self.run("project_new", discard_changes=True, preset_changes="discard")

    def start(self, name, tab="prepare"):
        self.new_project()
        self.run("view_set", tab=tab)
        wait(self.c, "idle")
        self.project_is(f"{name}: the start", "Untitled", None, "clean")
        self.t.equal(f"{name}: the start: no objects", meshes(self.c), [])
        self.t.equal(f"{name}: the start: the plates", plates(self.c), P('"" 0 *'))
        self.t.equal(f"{name}: the start: no dialog", dialogs(self.c), [])

    # ---- Reads ----

    def project_is(self, label, name, file, state):
        want = {"name": name, "file": normal(self.w(file)) if file else None, "dirty": state == "dirty"}
        self.t.equal(f"{label}: the project", project(self.c), want)

    def names_are(self, label, *names):
        self.t.equal(f"{label}: the objects", [m["name"] for m in meshes(self.c)], list(names))

    def placed(self, label, *places):
        """The objects, in order, each (name, x, y): its name and its offset x and y to 0.001 mm."""
        got = meshes(self.c)
        self.t.equal(f"{label}: the objects", [m["name"] for m in got], [p[0] for p in places])
        for m, (name, x, y) in zip(got, places):
            self.t.near(f"{label}: {name} at ({x}, {y})", m["offset"], [x, y], 0.001)

    def meshes_are(self, label, *want, got=None):
        """The objects, in order, each (name, triangles, mesh_volume, size) of its one part: the name, triangles and
        volume exactly, the size to 0.001 mm."""
        got = meshes(self.c) if got is None else got
        self.t.equal(f"{label}: the meshes", [(m["name"], [(p["triangles"], p["mesh_volume"]) for p in m["parts"]])
                                              for m in got], [(n, [(t, v)]) for n, t, v, _ in want])
        for m, (name, _, _, size) in zip(got, want):
            self.t.near(f"{label}: {name}'s size", m["size"], size, 0.001)

    def parts_are(self, label, *want):
        """Object 0's parts, each (type, triangles, mesh_volume); returns their ids."""
        parts = meshes(self.c)[0]["parts"]
        self.t.equal(f"{label}: object 0's parts", [(p["type"], p["triangles"], p["mesh_volume"]) for p in parts],
                     list(want))
        return [p["id"] for p in parts]

    def values_are(self, label, want, type="process"):
        self.t.equal(f"{label}: the {type} values", values(self.c, list(want), type), want)

    def unsaved_is(self, label, keys, type="process"):
        self.t.equal(f"{label}: unsaved {type} keys", unsaved(self.c, type), keys)

    def tab_is(self, label, tab):
        self.t.equal(f"{label}: the tab", self.c.call("app_state")["tab"], tab)

    def no_dialog(self, label):
        self.t.equal(f"{label}: no dialog", dialogs(self.c), [])

    def titles_are(self, label, titles):
        self.t.equal(f"{label}: the dialogs open", [d["title"] for d in dialogs(self.c)], titles)

    def steps_are(self, label, steps):
        self.t.equal(f"{label}: the undo steps", undo_steps(self.c), steps)

    def recent_has(self, label, file, held=True):
        self.t.equal(f"{label}: recent projects {'hold' if held else 'lack'} {file}",
                     normal(self.w(file)) in recent(self.c), held)

    def no_file(self, label, *names):
        self.t.check(f"{label}: no file {'/'.join(names)}", not os.path.exists(self.w(*names)), self.w(*names))

    def plates_are(self, label, *rows):
        self.t.equal(f"{label}: the plates", plates(self.c), P(*rows))

    def flags_are(self, label, flags):
        self.t.equal(f"{label}: the plates sliced", sliced_flags(self.c), flags)

    def sliced_ok(self, label, reply, layers):
        self.t.equal(f"{label}: the slice", [(p.get("outcome"), p.get("layers")) for p in reply.get("plates", [])],
                     [("ok", layers)])

    def saved(self, label, reply, file):
        """A save's or an export's reply: its path the file, and its bytes the file's size on disk."""
        path = self.w(file)
        self.t.equal(f"{label}: the reply's path", normal(reply.get("path", "")), normal(path))
        self.t.equal(f"{label}: the reply's bytes", reply.get("bytes"), os.path.getsize(path) if os.path.exists(path)
                     else "no file")

    def user_filaments(self):
        return [p.get("name") for p in self.c.call("preset_list", type="filament", source="user", limit=500)["items"]]

    # ---- Scenarios ----

    def starts_from(self):
        s = "the start"
        self.t.equal(f"{s}: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        items = self.c.call("preferences_get", keys=list(PREFERENCES))["items"]
        self.t.equal(f"{s}: the preferences", {i["key"]: i["value"] for i in items}, PREFERENCES)
        # Lossless DRC, the baseline's 0, under which the DRC rows read their meshes back exactly. The Preferences
        # dialog's field takes 8 to 30 only.
        drc = self.c.call("preferences_get", keys=["drc_bits"])["items"][0]["value"]
        self.t.equal(f"{s}: drc_bits is 0, lossless", drc, 0)
        if USER_PLA in self.user_filaments():
            self.run("preset_delete", type="filament", name=USER_PLA)
        self.t.check(f"{s}: no {USER_PLA} preset", USER_PLA not in self.user_filaments(), self.user_filaments())
        shutil.copyfile(BOX_STL, self.w("20mmbox-LF.stl"))
        with open(self.w("mcp_files_pla.json"), "w", encoding="utf-8") as f:
            json.dump(USER_PLA_FILE, f, indent=4)

    def new_project_scenario(self):
        s = "a new project"
        self.start(s, tab="home")
        self.run("view_set", tab="home")
        undo_steps(self.c)
        reply = self.c.call("action_run", id=NEW_PROJECT)
        self.t.check(f"{s} 1: the reply is not held by a dialog", reply.get("status") != "blocked_by_dialog", reply)
        self.t.equal(f"{s} 1: the reply's ran", reply.get("ran"), "orca_command:new_project:orca")
        self.follow(reply, "action_run")
        self.project_is(f"{s} 1", "Untitled", None, "clean")
        self.no_dialog(f"{s} 1")
        self.tab_is(f"{s} 1", "prepare")
        self.steps_are(f"{s} 1", ["Reset Project", "New Project"])

        self.run("view_set", tab="home")
        undo_steps(self.c)
        reply = self.run("project_new", silent=True)
        self.t.equal(f"{s} 2: the reply",
                     {k: reply.get(k) for k in ("objects", "names", "plates", "discarded", "modified")},
                     {"objects": 0, "names": [], "plates": 1, "discarded": None, "modified": None})
        self.project_is(f"{s} 2", "Untitled", None, "clean")
        self.tab_is(f"{s} 2", "home")
        self.steps_are(f"{s} 2", ["Reset Project", "New Project"])

        self.cube("box", 20, 128, 128)
        self.run("view_set", tab="home")
        self.t.refused(f"{s} 3: refused", lambda: self.c.call("project_new", silent=True), code="unsaved_changes")
        self.project_is(f"{s} 3", "Untitled", None, "dirty")
        self.placed(f"{s} 3", ("box", 128, 128))
        self.tab_is(f"{s} 3", "home")

        self.run("view_set", tab="home")
        reply = self.run("project_new", discard_changes=True)
        self.t.equal(f"{s} 4: the reply's objects", reply.get("objects"), 0)
        self.project_is(f"{s} 4", "Untitled", None, "clean")
        self.names_are(f"{s} 4")
        self.tab_is(f"{s} 4", "home")

        self.run("settings_set", values={"top_shell_layers": 7})
        self.project_is(f"{s} 5", "Untitled", None, "dirty")
        self.values_are(f"{s} 5", {"top_shell_layers": "7"})
        self.unsaved_is(f"{s} 5", ["top_shell_layers"])

        self.run("view_set", tab="home")
        reply = self.c.call("action_run", id=NEW_PROJECT)
        self.t.equal(f"{s} 6: the reply", reply.get("status"), "blocked_by_dialog")
        self.titles_are(f"{s} 6: a project with no objects asks nothing about saving it", [NEW_PROJECT_QUESTION])
        self.project_is(f"{s} 6", "Untitled", None, "dirty")
        self.run("dialog_answer", dialog={"title": NEW_PROJECT_QUESTION}, button="Discard")
        self.follow(reply, "action_run")
        self.project_is(f"{s} 7", "Untitled", None, "clean")
        self.values_are(f"{s} 7", {"top_shell_layers": "4"})
        self.unsaved_is(f"{s} 7", [])
        self.no_dialog(f"{s} 7")
        self.tab_is(f"{s} 7", "prepare")

        self.run("settings_set", values={"top_shell_layers": 7})
        self.run("view_set", tab="home")
        reply = self.c.call("action_run", id=NEW_PROJECT)
        self.titles_are(f"{s} 8: the question of 6", [NEW_PROJECT_QUESTION])
        self.run("dialog_answer", dialog={"title": NEW_PROJECT_QUESTION}, button="Transfer")
        self.follow(reply, "action_run")
        self.project_is(f"{s} 8", "Untitled", None, "clean")
        self.values_are(f"{s} 8: the change is carried into the new project", {"top_shell_layers": "7"})
        self.unsaved_is(f"{s} 8", ["top_shell_layers"])
        self.tab_is(f"{s} 8", "prepare")

        self.run("view_set", tab="home")
        reply = self.run("project_new", discard_changes=True, preset_changes="discard")
        self.t.equal(f"{s} 9: the reply's presets", (reply.get("discarded"), reply.get("modified")),
                     ({"process": {"name": KLIPPER_STD, "keys": ["top_shell_layers"]}}, None))
        self.project_is(f"{s} 9", "Untitled", None, "clean")
        self.values_are(f"{s} 9", {"top_shell_layers": "4"})
        self.unsaved_is(f"{s} 9", [])
        self.tab_is(f"{s} 9", "home")

        self.run("settings_set", values={"top_shell_layers": 7})
        self.run("view_set", tab="home")
        self.t.refused(f"{s} 10: refused", lambda: self.c.call("project_new", silent=True), code="unsaved_changes")
        self.project_is(f"{s} 10", "Untitled", None, "dirty")
        self.values_are(f"{s} 10", {"top_shell_layers": "7"})
        self.tab_is(f"{s} 10", "home")

        self.run("view_set", tab="home")
        reply = self.run("project_new", discard_changes=True)
        self.t.equal(f"{s} 11: the reply's presets", (reply.get("modified"), reply.get("discarded")),
                     ({"process": {"name": KLIPPER_STD, "keys": ["top_shell_layers"]}}, None))
        self.project_is(f"{s} 11", "Untitled", None, "clean")
        self.values_are(f"{s} 11: the presets' changes carry over by default", {"top_shell_layers": "7"})
        self.unsaved_is(f"{s} 11", ["top_shell_layers"])

    def layer_gcode_scenario(self):
        s = "the question keeps the layer G-code"
        self.start(s)
        self.cube("kept", 20, 100, 100)
        reply = self.run("project_save", path=self.w("kept.3mf"))
        self.project_is(f"{s} 1", "kept", "kept.3mf", "clean")
        self.saved(f"{s} 1", reply, "kept.3mf")

        self.sliced_ok(f"{s} 2", self.run("plate_slice"), 100)
        self.flags_are(f"{s} 2", [True])
        self.project_is(f"{s} 2", "kept", "kept.3mf", "clean")

        reply = self.run("plate_set_custom_gcode", add=[{"layer": 26, "type": "pause"}])
        self.t.equal(f"{s} 3: the reply's layer G-code",
                     [{"z": round(g.get("z", 0), 4), "layer": g.get("layer"), "type": g.get("type")}
                      for g in reply.get("layer_gcode", [])], [{"z": 5.2, "layer": 26, "type": "pause"}])
        self.project_is(f"{s} 3", "kept", "kept.3mf", "dirty")
        pause = [{"z": 5.2, "type": "pause"}]
        rounded = lambda: [{"z": round(g["z"], 4), "type": g["type"]} for g in layer_gcode(self.c)]
        self.t.equal(f"{s} 3: the layer G-code", rounded(), pause)
        self.flags_are(f"{s} 3: a layer code invalidates the slice", [False])

        self.run("object_transform", object=0, translate=[5, 0, 0])
        self.project_is(f"{s} 4", "kept", "kept.3mf", "dirty")
        self.placed(f"{s} 4", ("kept", 105, 100))

        reply = self.c.call("action_run", id=NEW_PROJECT)
        shown = dialogs(self.c)
        self.t.equal(f"{s} 5: the question", [d["title"] for d in shown], [SAVE_QUESTION])
        self.t.check(f"{s} 5: the question says the project has unsaved changes",
                     shown and "The current project has unsaved changes" in (shown[0]["message"] or ""), shown)
        self.run("dialog_answer", dialog={"title": SAVE_QUESTION}, button="Cancel")
        self.follow(reply, "action_run")
        self.project_is(f"{s} 5", "kept", "kept.3mf", "dirty")
        self.placed(f"{s} 5", ("kept", 105, 100))
        self.t.equal(f"{s} 5: the layer G-code", rounded(), pause)
        self.no_dialog(f"{s} 5")

        reply = self.c.call("action_run", id=NEW_PROJECT)
        self.titles_are(f"{s} 6: the question", [SAVE_QUESTION])
        self.run("dialog_answer", dialog={"title": SAVE_QUESTION}, button="Yes")
        self.follow(reply, "action_run")
        self.project_is(f"{s} 6", "Untitled", None, "clean")
        self.names_are(f"{s} 6")
        self.t.equal(f"{s} 6: the layer G-code", rounded(), [])
        self.plates_are(f"{s} 6", '"" 0 *')
        self.tab_is(f"{s} 6", "prepare")

        self.run("project_open", path=self.w("kept.3mf"))
        self.project_is(f"{s} 7", "kept", "kept.3mf", "clean")
        self.placed(f"{s} 7: the Save wrote the move", ("kept", 105, 100))
        self.t.equal(f"{s} 7: the Save wrote the pause", rounded(), pause)

    def opened(self, label, reply):
        """What every open of the source project leaves."""
        self.t.equal(f"{label}: the reply", {k: reply.get(k) for k in ("opened", "objects", "names", "plates")},
                     {"opened": "project", "objects": 1, "names": ["source"], "plates": 1})
        self.placed(label, ("source", 100, 100))
        self.values_are(label, {"wall_loops": "5", "top_shell_layers": "4"})
        self.unsaved_is(label, ["wall_loops"])
        self.tab_is(label, "prepare")
        self.no_dialog(label)

    def open_case(self):
        self.new_project()
        self.run("view_set", tab="home")

    def opening_scenario(self):
        s = "opening a project"
        self.start(s, tab="home")
        self.cube("source", 20, 100, 100)
        self.run("settings_set", values={"wall_loops": 5})
        self.run("project_save", path=self.w("open_src.3mf"))
        for copy in ("clean.3mf", "dirty.3mf", "presets.3mf"):
            shutil.copyfile(self.w("open_src.3mf"), self.w(copy))

        self.open_case()
        self.project_is(f"{s} 1", "Untitled", None, "clean")
        self.recent_has(f"{s} 1", "open_src.3mf")
        for copy in ("clean.3mf", "dirty.3mf", "presets.3mf"):
            self.recent_has(f"{s} 1", copy, held=False)
        undo_steps(self.c)
        reply = self.run("project_open", path=self.w("clean.3mf"))
        self.project_is(f"{s} 2", "clean", "clean.3mf", "clean")
        self.opened(f"{s} 2", reply)
        self.recent_has(f"{s} 2", "clean.3mf")
        self.steps_are(f"{s} 2", ["Reset Project", "Load Project"])

        self.open_case()
        self.cylinder("cyl")
        self.run("view_set", tab="home")
        self.t.refused(f"{s} 3: refused", lambda: self.c.call("project_open", path=self.w("dirty.3mf")),
                       code="unsaved_changes")
        self.project_is(f"{s} 3", "Untitled", None, "dirty")
        self.names_are(f"{s} 3", "cyl")
        self.no_dialog(f"{s} 3")
        self.tab_is(f"{s} 3", "home")
        self.recent_has(f"{s} 3", "dirty.3mf", held=False)
        reply = self.run("project_open", path=self.w("dirty.3mf"), discard_changes=True)
        self.project_is(f"{s} 4", "dirty", "dirty.3mf", "clean")
        self.opened(f"{s} 4", reply)
        self.recent_has(f"{s} 4", "dirty.3mf")

        self.open_case()
        self.run("settings_set", values={"top_shell_layers": 7})
        self.run("view_set", tab="home")
        self.t.refused(f"{s} 5: refused", lambda: self.c.call("project_open", path=self.w("presets.3mf")),
                       code="unsaved_changes")
        self.project_is(f"{s} 5", "Untitled", None, "dirty")
        self.names_are(f"{s} 5")
        self.values_are(f"{s} 5", {"top_shell_layers": "7"})
        self.no_dialog(f"{s} 5")
        reply = self.c.call("project_open", path=self.w("presets.3mf"), discard_changes=True)
        self.t.equal(f"{s} 6: the reply", reply.get("status"), "blocked_by_dialog")
        self.titles_are(f"{s} 6", [LOAD_PROJECT_QUESTION])
        self.names_are(f"{s} 6")
        self.values_are(f"{s} 6", {"top_shell_layers": "7"})
        self.unsaved_is(f"{s} 6", ["top_shell_layers"])
        self.project_is(f"{s} 6", "Untitled", None, "clean")
        self.run("dialog_answer", dialog={"title": LOAD_PROJECT_QUESTION}, button="Discard")
        reply = self.follow(reply, "project_open")
        self.project_is(f"{s} 7", "presets", "presets.3mf", "clean")
        self.opened(f"{s} 7: the file's top_shell_layers, the unsaved 7 gone", reply)
        self.recent_has(f"{s} 7", "presets.3mf")

    def saving_scenario(self):
        s = "saving a project under a name"
        self.start(s)
        undo_steps(self.c)
        reply = self.run("project_save", path=self.w("empty.3mf"))
        self.project_is(f"{s} 1", "empty", "empty.3mf", "clean")
        self.saved(f"{s} 1", reply, "empty.3mf")
        self.recent_has(f"{s} 1: a project with no objects is saved", "empty.3mf")
        self.steps_are(f"{s} 1", [])

        self.new_project()
        reply = self.c.call("project_open", path=self.w("empty.3mf"))
        self.t.equal(f"{s} 2: the reply", reply.get("status"), "blocked_by_dialog")
        answer_all(self.c, self.t, f"{s} 2", [("Warning", NO_GEOMETRY)])
        reply = self.follow(reply, "project_open")
        self.t.equal(f"{s} 2: the job's reply", {k: reply.get(k) for k in ("opened", "objects", "names", "plates")},
                     {"opened": "project", "objects": 0, "names": [], "plates": 1})
        self.project_is(f"{s} 2", "", None, "clean")
        self.plates_are(f"{s} 2", '"" 0 *')

        self.new_project()
        self.cube("box", 20, 100, 100)
        undo_steps(self.c)
        reply = self.run("project_save", path=self.w("saved.3mf"))
        self.project_is(f"{s} 3", "saved", "saved.3mf", "clean")
        self.saved(f"{s} 3", reply, "saved.3mf")
        self.recent_has(f"{s} 3", "saved.3mf")
        self.steps_are(f"{s} 3", [])

        self.run("object_transform", object=0, translate=[5, 0, 0])
        self.project_is(f"{s} 4", "saved", "saved.3mf", "dirty")
        self.placed(f"{s} 4", ("box", 105, 100))

        refusals = [(5, ["saved.gcode.3mf"], {"code": "sliced_file_name"}),
                    (6, ["missing", "saved.3mf"], {"words": "directory does not exist"}),
                    (7, ["saved.3mf"], {"words": "file exists (pass overwrite: true to replace it)"})]
        for step, names, how in refusals:
            undo_steps(self.c)
            self.t.refused(f"{s} {step}: refused", lambda: self.c.call("project_save", path=self.w(*names)), **how)
            self.project_is(f"{s} {step}", "saved", "saved.3mf", "dirty")
            if step != 7:
                self.no_file(f"{s} {step}", *names[:1])
            self.placed(f"{s} {step}", ("box", 105, 100))
            self.steps_are(f"{s} {step}", [])

        self.run("project_open", path=self.w("saved.3mf"), discard_changes=True)
        self.project_is(f"{s} 8", "saved", "saved.3mf", "clean")
        self.placed(f"{s} 8: the refused saves wrote nothing", ("box", 100, 100))

        self.run("object_transform", object=0, translate=[5, 0, 0])
        undo_steps(self.c)
        reply = self.run("project_save", path=self.w("saved.3mf"), overwrite=True)
        self.project_is(f"{s} 9", "saved", "saved.3mf", "clean")
        self.saved(f"{s} 9", reply, "saved.3mf")
        self.placed(f"{s} 9", ("box", 105, 100))
        self.steps_are(f"{s} 9", [])

        self.new_project()
        self.run("project_open", path=self.w("saved.3mf"))
        self.project_is(f"{s} 10", "saved", "saved.3mf", "clean")
        self.placed(f"{s} 10: the file was replaced", ("box", 105, 100))

    def importing_scenario(self):
        s = "importing models"
        box = ("20mmbox-LF.stl", 12, 8000, [20, 20, 20])
        box_stl = self.w("20mmbox-LF.stl")
        self.start(s)
        undo_steps(self.c)
        reply = self.run("project_import", paths=[box_stl])
        self.meshes_are(f"{s} 1", box)
        self.t.equal(f"{s} 1: the reply",
                     {k: reply.get(k) for k in ("opened", "added", "objects", "names", "plates")},
                     {"opened": "models", "added": 1, "objects": 1, "names": ["20mmbox-LF.stl"], "plates": 1})
        self.project_is(f"{s} 1: an untitled project takes the first file's name", "20mmbox-LF", None, "dirty")
        self.steps_are(f"{s} 1", ["Import Objects: 20mmbox-LF.stl"])
        self.unsaved_is(f"{s} 1", [])
        self.recent_has(f"{s} 1", "20mmbox-LF.stl", held=False)
        self.run("undo", history="project")
        self.names_are(f"{s} 2: the import is one undo step")

        self.new_project()
        self.cylinder("cyl")
        self.run("settings_set", values={"top_shell_layers": 7})
        reply = self.run("project_import", paths=[box_stl])
        self.names_are(f"{s} 3", "cyl", "20mmbox-LF.stl")
        self.meshes_are(f"{s} 3: the box", box, got=meshes(self.c)[1:])
        self.t.equal(f"{s} 3: the reply's added", reply.get("added"), 1)
        self.project_is(f"{s} 3", "20mmbox-LF", None, "dirty")
        self.values_are(f"{s} 3", {"top_shell_layers": "7"})
        self.unsaved_is(f"{s} 3", ["top_shell_layers"])
        self.run("undo", history="project")
        self.names_are(f"{s} 4", "cyl")
        self.values_are(f"{s} 4", {"top_shell_layers": "7"})
        self.unsaved_is(f"{s} 4", ["top_shell_layers"])

        self.new_project()
        self.cube("source", 20, 100, 100)
        self.run("settings_set", values={"wall_loops": 5})
        self.run("project_save", path=self.w("import_src.3mf"))
        self.new_project()
        self.cylinder("cyl")
        self.run("settings_set", values={"top_shell_layers": 7})
        self.names_are(f"{s} 5", "cyl")
        self.values_are(f"{s} 5", {"wall_loops": "3", "top_shell_layers": "7"})
        undo_steps(self.c)
        reply = self.c.call("project_import", paths=[self.w("import_src.3mf")])
        self.t.check(f"{s} 6: the reply is not held by a dialog", reply.get("status") != "blocked_by_dialog", reply)
        reply = self.follow(reply, "project_import")
        self.t.equal(f"{s} 6: the reply's added", reply.get("added"), 1)
        self.names_are(f"{s} 6", "cyl", "source")
        self.meshes_are(f"{s} 6: the source", ("source", 12, 8000, [20, 20, 20]), got=meshes(self.c)[1:])
        self.no_dialog(f"{s} 6")
        self.values_are(f"{s} 6: a project's .3mf adds none of its settings",
                        {"wall_loops": "3", "top_shell_layers": "7"})
        self.unsaved_is(f"{s} 6", ["top_shell_layers"])
        self.unsaved_is(f"{s} 6", [], "filament")
        self.unsaved_is(f"{s} 6", [], "printer")
        self.project_is(f"{s} 6", "import_src", None, "dirty")
        self.steps_are(f"{s} 6", ["Import Objects: import_src.3mf"])
        self.run("undo", history="project")
        self.names_are(f"{s} 7", "cyl")
        self.values_are(f"{s} 7", {"wall_loops": "3", "top_shell_layers": "7"})
        self.unsaved_is(f"{s} 7", ["top_shell_layers"])

        self.new_project()
        self.cube("box", 20, 128, 128)
        undo_steps(self.c)
        self.t.refused(f"{s} 8: refused", lambda: self.c.call("project_import", paths=[self.w("missing.stl")]),
                       words="file not found")
        self.names_are(f"{s} 8", "box")
        self.steps_are(f"{s} 8", [])

        for step, path in ((9, HEIGHTMAP), (10, self.w("mcp_files_pla.json"))):
            undo_steps(self.c)
            reply = self.c.call("project_import", paths=[path])
            self.t.equal(f"{s} {step}: the reply", reply.get("status"), "blocked_by_dialog")
            answer_all(self.c, self.t, f"{s} {step}",
                       [("Warning", NO_GEOMETRY), ("Orca Slicer error", "Unknown file format")])
            self.t.refused(f"{s} {step}: the job ends refused", lambda: self.follow(reply, "project_import"),
                           code="nothing_loaded")
            self.names_are(f"{s} {step}", "box")
            self.steps_are(f"{s} {step}", [f"Import Objects: {os.path.basename(path)}"])
            self.project_is(f"{s} {step}: an import that adds nothing names nothing", "Untitled", None, "dirty")
        self.t.check(f"{s} 10: a preset file loads no preset", USER_PLA not in self.user_filaments(),
                     self.user_filaments())
        self.t.equal(f"{s} 10: the slots' presets", presets(self.c)["filaments"], [GENERIC_PLA])

    def zip_scenario(self):
        s = "ZIP archives"
        T = self.tag
        large = (f"{T}_large.stl", 12, 8000, [20, 20, 20])
        middle = (f"{T}_middle.stl", 12, 3375, [15, 15, 15])
        small = (f"{T}_small.stl", 12, 1000, [10, 10, 10])
        self.start(s)
        with zipfile.ZipFile(self.w("cubes.zip"), "w") as archive:
            for entry, size in ((f"parts/{T}_large.stl", 20), (f"parts/{T}_middle.stl", 15), (f"{T}_small.stl", 10)):
                archive.writestr(entry, cube_stl(size))
            archive.writestr("readme.txt", "not a model\n")
        with open(self.w("not_a.zip"), "w") as f:
            f.write("not an archive\n")

        named = [f"{T}_small.stl", f"parts/{T}_large.stl"]
        undo_steps(self.c)
        reply = self.run("project_import", paths=[self.w("cubes.zip")], entries=named)
        self.meshes_are(f"{s} 1: in the archive's order", large, small)
        self.t.equal(f"{s} 1: the reply's added", reply.get("added"), 2)
        self.t.equal(f"{s} 1: the reply's archives",
                     [{"path": normal(a.get("path", "")), "entries": a.get("entries")}
                      for a in reply.get("archives", [])],
                     [{"path": normal(self.w("cubes.zip")), "entries": named}])
        self.steps_are(f"{s} 1", ["Import Objects: cubes.zip"])
        self.no_dialog(f"{s} 1")
        self.run("undo", history="project")
        self.names_are(f"{s} 2: one undo step")

        self.new_project()
        reply = self.run("project_import", paths=[self.w("cubes.zip")])
        self.meshes_are(f"{s} 3", large, middle, small)
        self.t.equal(f"{s} 3: the reply's added", reply.get("added"), 3)
        self.t.equal(f"{s} 3: every model file, and not the text file",
                     [a.get("entries") for a in reply.get("archives", [])],
                     [[f"parts/{T}_large.stl", f"parts/{T}_middle.stl", f"{T}_small.stl"]])
        undo_steps(self.c)
        self.t.refused(f"{s} 4: refused", lambda: self.c.call("project_import", paths=[self.w("not_a.zip")]),
                       code="unreadable_archive")
        self.meshes_are(f"{s} 4", large, middle, small)
        self.steps_are(f"{s} 4", [])

        self.new_project()
        self.cube("inside_zip", 10, 128, 128)
        self.run("project_export", format="3mf", path=self.w("project.3mf"))
        with zipfile.ZipFile(self.w("project.zip"), "w") as archive:
            archive.write(self.w("project.3mf"), "project.3mf")
        self.names_are(f"{s} 5", "inside_zip")

        self.new_project()
        self.cube("first", 20, 80, 128)
        self.cube("second", 30, 176, 128)
        undo_steps(self.c)
        reply = self.run("project_import", paths=[self.w("project.zip")])
        self.names_are(f"{s} 6", "first", "second", "inside_zip")
        self.meshes_are(f"{s} 6: inside_zip", ("inside_zip", 12, 1000, [10, 10, 10]), got=meshes(self.c)[2:])
        self.t.equal(f"{s} 6: the reply's added", reply.get("added"), 1)
        self.t.equal(f"{s} 6: the reply's entries", [a.get("entries") for a in reply.get("archives", [])],
                     [["project.3mf"]])
        self.no_dialog(f"{s} 6")
        self.project_is(f"{s} 6: an archive names no project", "Untitled", None, "dirty")
        self.steps_are(f"{s} 6", ["Import Objects: project.zip"])

    def step_call(self, label, tool, **args):
        """A call that reads the STEP file: it replies blocked_by_dialog on the STEP options, which are answered OK,
        and its job is followed to its end. Returns the job's reply."""
        reply = self.c.call(tool, **args)
        self.t.equal(f"{label}: the reply", (reply.get("status"), [d.get("title") for d in reply.get("dialogs", [])]),
                     ("blocked_by_dialog", [STEP_DIALOG]))
        self.run("dialog_answer", dialog={"title": STEP_DIALOG}, button="OK")
        reply = self.follow(reply, tool)
        self.no_dialog(label)
        return reply

    def step_scenario(self):
        s = "a STEP file's mesh options"
        tetra = ("part", 4, 166.667)
        self.start(s)
        reply = self.step_call(f"{s} 1", "project_import", paths=[STEP])
        self.t.equal(f"{s} 1: the job's reply's added", reply.get("added"), 1)
        ids1 = self.parts_are(f"{s} 1", tetra)
        self.t.equal(f"{s} 1: one object", len(meshes(self.c)), 1)
        self.project_is(f"{s} 1", "named_single_body", None, "dirty")

        self.step_call(f"{s} 2", "part_add", object=0, type="part", path=STEP)
        ids2 = self.parts_are(f"{s} 2", tetra, tetra)
        self.t.equal(f"{s} 2: part 0 keeps its id", ids2[:1], ids1)

        self.run("part_add", object=0, type="part", shape="cube", size=[5, 5, 5])
        ids3 = self.parts_are(f"{s} 3", tetra, tetra, ("part", 12, 125))
        self.t.equal(f"{s} 3: parts 0 and 1 keep their ids", ids3[:2], ids2)
        self.no_dialog(f"{s} 3")

        reply = self.step_call(f"{s} 4", "part_replace", object=0, part=2, path=STEP)
        ids4 = self.parts_are(f"{s} 4", tetra, tetra, tetra)
        self.t.equal(f"{s} 4: parts 0 and 1 keep their ids", ids4[:2], ids2)
        self.t.equal(f"{s} 4: part 2 is the reply's part_id", ids4[2], (reply.get("parts") or [{}])[0].get("part_id"))
        self.t.check(f"{s} 4: part 2 is not the cube", ids4[2] != ids3[2], (ids4, ids3))
        self.t.equal(f"{s} 4: one object", len(meshes(self.c)), 1)

    def exported(self, label, call):
        """An export of the meshes: no undo step, and the project untitled and dirty."""
        undo_steps(self.c)
        reply = call()
        self.steps_are(label, [])
        self.project_is(label, "Untitled", None, "dirty")
        return reply

    def folder_is(self, label, folder, sizes):
        names = sorted(os.listdir(self.w(folder)))
        self.t.equal(f"{label}: {folder} holds", names, sorted(sizes))
        if sizes and all(isinstance(v, int) for v in sizes.values()):
            self.t.equal(f"{label}: the sizes in {folder}", {n: os.path.getsize(self.w(folder, n)) for n in names},
                         sizes)

    def meshes_scenario(self):
        s = "exporting meshes"
        self.start(s)
        self.cube("small", 20, 100, 100)
        self.cube("big", 30, 160, 100)
        os.makedirs(self.w("each"))
        both =[{"index": 0, "name": "small"}, {"index": 1, "name": "big"}]
        objs = lambda reply: [{"index": o.get("index"), "name": o.get("name")} for o in reply.get("objects", [])]
        files = lambda reply: [normal(f) for f in reply.get("files", [])]

        reply = self.exported(f"{s} 1", lambda: self.run("project_export", format="stl", path=self.w("small.stl"),
                                                          objects=[0]))
        self.t.equal(f"{s} 1: small.stl's size", os.path.getsize(self.w("small.stl")), 684)
        self.t.equal(f"{s} 1: the reply", (objs(reply), reply.get("bytes")), ([{"index": 0, "name": "small"}], 684))

        reply = self.exported(f"{s} 2", lambda: self.run("project_export", format="stl", path=self.w("both.stl")))
        self.t.equal(f"{s} 2: both.stl's size", os.path.getsize(self.w("both.stl")), 1284)
        self.t.equal(f"{s} 2: the reply", (objs(reply), reply.get("bytes")), (both, 1284))

        reply = self.exported(f"{s} 3", lambda: self.run("project_export", format="stl", path=self.w("each"),
                                                          per_object=True))
        self.folder_is(f"{s} 3", "each", {"big.stl": 684, "small.stl": 684})
        self.t.equal(f"{s} 3: the reply", (objs(reply), files(reply), "bytes" in reply),
                     (both, [normal(self.w("each", "small.stl")), normal(self.w("each", "big.stl"))], False))

        reply = self.exported(f"{s} 4", lambda: self.run("project_export", format="stl", path=self.w("each"),
                                                          per_object=True))
        self.folder_is(f"{s} 4", "each", {"big(1).stl": 684, "big.stl": 684, "small(1).stl": 684, "small.stl": 684})
        self.t.equal(f"{s} 4: a name the folder has gets a number", files(reply),
                     [normal(self.w("each", "small(1).stl")), normal(self.w("each", "big(1).stl"))])

        reply = self.exported(f"{s} 5", lambda: self.run("project_export", format="drc", path=self.w("both.drc")))
        self.t.equal(f"{s} 5: the reply", objs(reply), both)
        self.saved(f"{s} 5", reply, "both.drc")

        self.exported(f"{s} 6", lambda: self.t.refused(
            f"{s} 6: refused", lambda: self.c.call("project_export", format="3mf", path=self.w("one.3mf"), objects=[0]),
            code="objects_not_taken"))
        self.no_file(f"{s} 6", "one.3mf")
        self.exported(f"{s} 7", lambda: self.t.refused(
            f"{s} 7: refused",
            lambda: self.c.call("project_export", format="stl", path=self.w("five.stl"), objects=[5]),
            code="no_such_object", words="Object 5 does not exist (the project has objects 0..1)"))
        self.no_file(f"{s} 7", "five.stl")
        self.placed(f"{s} 8: the exports moved nothing", ("small", 100, 100), ("big", 160, 100))

        r = lambda path: reimport(self.c, path)[1]
        self.meshes_are(f"{s}: small.stl read back", ("small.stl", 12, 8000, [20, 20, 20]), got=r(self.w("small.stl")))
        self.meshes_are(f"{s}: both.stl read back, both cubes as placed in one object",
                        ("both.stl", 24, 35000, [85, 30, 30]), got=r(self.w("both.stl")))
        reimport(self.c, self.w("each", "small.stl"))
        self.run("project_import", paths=[self.w("each", "big.stl")])
        self.meshes_are(f"{s}: each read back", ("small.stl", 12, 8000, [20, 20, 20]),
                        ("big.stl", 12, 27000, [30, 30, 30]))
        self.meshes_are(f"{s}: both.drc read back", ("both.drc", 24, 35000, [85, 30, 30]), got=r(self.w("both.drc")))

    def model_3mf_scenario(self):
        s = "exporting the model as a 3MF"
        self.start(s)
        self.cube("box", 20, 100, 100)
        undo_steps(self.c)
        reply = self.run("project_export", format="3mf", path=self.w("model.3mf"))
        self.placed(f"{s} 1", ("box", 100, 100))
        self.saved(f"{s} 1", reply, "model.3mf")
        self.project_is(f"{s} 1: the export does not make the file the project's", "Untitled", None, "dirty")
        self.recent_has(f"{s} 1", "model.3mf", held=False)
        self.steps_are(f"{s} 1", [])
        self.run("undo", history="project")
        self.names_are(f"{s} 2: undo takes back the cube")
        reply, got = reimport(self.c, self.w("model.3mf"))
        self.meshes_are(f"{s} 3", ("box", 12, 8000, [20, 20, 20]), got=got)
        self.t.equal(f"{s} 3: the reply's added", reply.get("added"), 1)
        self.project_is(f"{s} 3", "model", None, "dirty")

    def sliced_scenario(self):
        s = "exporting the sliced file"
        self.start(s)
        plate_1 = "Metadata/plate_1.gcode"
        # A printer outside Bambu Lab is offered Export plate sliced file with "Use 3MF instead of G-code" on; the
        # edit stays unsaved, and the next scenario's new project discards it.
        self.run("settings_set", type="printer", values={"use_3mf": True})
        self.values_are(f"{s}: use_3mf", {"use_3mf": "1"}, "printer")

        def export(step, name, flags, steps=(), **args):
            undo_steps(self.c)
            self.run("project_export", format="sliced_3mf", path=self.w(name), **args)
            self.project_is(f"{s} {step}", "Untitled", None, "dirty")
            self.flags_are(f"{s} {step}", flags)
            self.steps_are(f"{s} {step}", list(steps))
            return sliced(self.w(name))

        self.cube("a", 20, 128, 128)
        wait(self.c, "idle")
        got = export(1, "unsliced.gcode.3mf", [False])
        self.plates_are(f"{s} 1", '"" 1 *')
        self.t.equal(f"{s} 1: a plate not sliced is written without G-code", list(got), [])

        self.sliced_ok(f"{s} 2", self.run("plate_slice"), 100)
        got = export(2, "sliced.gcode.3mf", [True])
        self.plates_are(f"{s} 2", '"" 1 *')
        self.t.equal(f"{s} 2: the G-code entries", list(got), [plate_1])
        self.t.equal(f"{s} 2: the G-code's layers and labels",
                     [(g["layers"], g["labels"]) for g in got.values()], [(100, 1)])

        self.run("plate_add")
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20], name="b", plate=1)
        self.sliced_ok(f"{s} 3", self.run("plate_slice", plates=0), 100)
        self.run("plate_select", plate=0)
        self.plates_are(f"{s} 3", '"" 1 *', '"" 1')
        two = [True, False]
        self.t.equal(f"{s} 4: the G-code entries", list(export(4, "half_current.gcode.3mf", two)), [plate_1])
        self.plates_are(f"{s} 4", '"" 1 *', '"" 1')
        self.t.equal(f"{s} 5: plate 1, not sliced, adds none",
                     list(export(5, "half_all.gcode.3mf", two, plates="all")), [plate_1])
        self.plates_are(f"{s} 5", '"" 1 *', '"" 1')
        self.t.equal(f"{s} 6: the G-code entries",
                     list(export(6, "half_1.gcode.3mf", two, SELECTED, plates=1)), [])
        self.plates_are(f"{s} 6: the plate named is made current", '"" 1', '"" 1 *')
        self.t.equal(f"{s} 7: the G-code entries",
                     list(export(7, "half_0.gcode.3mf", two, SELECTED, plates=0)), [plate_1])
        self.plates_are(f"{s} 7", '"" 1 *', '"" 1')

        undo_steps(self.c)
        self.t.refused(f"{s} 8: refused", lambda: self.c.call("project_export", format="gcode",
                                                              path=self.w("plate_1.gcode"), plates=1),
                       code="plate_not_sliced")
        self.no_file(f"{s} 8", "plate_1.gcode")
        self.plates_are(f"{s} 8: a refused export leaves the current plate", '"" 1 *', '"" 1')
        self.steps_are(f"{s} 8: and the undo history", [])
        self.project_is(f"{s} 8", "Untitled", None, "dirty")
        self.flags_are(f"{s} 8", two)

    def gcode_written_scenario(self):
        s = "the G-code written"
        self.start(s)
        self.cube("box", 20, 128, 128)
        self.run("settings_set", values={"layer_height": 0.25, "initial_layer_print_height": 0.25})
        self.sliced_ok(f"{s} 1", self.run("plate_slice"), 80)
        self.flags_are(f"{s} 1", [True])
        self.names_are(f"{s} 1", "box")

        for step, name in ((2, "box.gcode"), (3, "box_again.gcode")):
            undo_steps(self.c)
            reply = self.run("project_export", format="gcode", path=self.w(name))
            there = os.path.exists(self.w(name))
            self.t.equal(f"{s} {step}: the reply",
                         {"export": reply.get("export"), "path": normal(reply.get("path", ""))},
                         {"export": "ok", "path": normal(self.w(name))})
            self.t.check(f"{s} {step}: the file is there when the reply comes", there)
            facts = gcode(self.w(name)) if there else {"config": {}}
            self.t.equal(f"{s} {step}: the G-code", (facts.get("layers"), facts.get("labels"),
                                                     facts["config"].get("layer_height"),
                                                     facts["config"].get("initial_layer_print_height")),
                         (80, 1, "0.25", "0.25"))
            self.steps_are(f"{s} {step}", [])
            self.project_is(f"{s} {step}", "Untitled", None, "dirty")
            self.names_are(f"{s} {step}", "box")
        self.flags_are(f"{s} 4: an export leaves the slice as it was", [True])
        self.project_is(f"{s} 4", "Untitled", None, "dirty")
        self.run("undo", history="project")
        self.names_are(f"{s} 5: undo takes back the cube")

    def slice_in_thread(self, action):
        """Starts `action` from a second client of the session in a thread; the box gets its result or error."""
        box = {}

        def go():
            try:
                other = orca_mcp.OrcaMcp(self.session["port"], self.session["token"])
                box["result"] = other.await_job(other.call("action_run", id=action))
            except Exception as e:  # reported by the scenario
                box["error"] = e

        thread = threading.Thread(target=go)
        thread.start()
        return thread, box

    def overlapping(self, label, action):
        """Attempts until an export overlaps the second client's slice: returns (walls, refusal, thread, box), or None
        after three attempts in a row that do not overlap or a refusal with other words, failing the check. The app
        decides when the slice starts and ends, so each attempt reads app_state.slicing every 5 ms and exports as soon
        as it reads true; an attempt whose slice ended unseen is a miss."""
        misses = []
        while len(misses) < 3:
            walls = self.next_walls
            self.next_walls += 1
            self.run("settings_set", values={"wall_loops": walls})
            wait(self.c, "idle")
            thread, box = self.slice_in_thread(action)

            def slicing_or_ended():
                if self.c.call("app_state").get("slicing"):
                    return "slicing"
                return None if thread.is_alive() else "ended"

            try:
                seen = until(slicing_or_ended, "the slice shows or the second client's call ends", interval_s=0.005)
            except TimeoutError as e:
                thread.join()
                self.t.check(f"{label}: the slice shows or its call ends", False, f"{walls}: {e}")
                return None
            if seen == "slicing":
                try:
                    reply = self.c.call("project_export", format="gcode", path=self.w(f"during_{walls}.gcode"))
                    misses.append(f"{walls}: the export went through: {reply}")
                except orca_mcp.McpError as e:
                    text = str(e)
                    if "Busy: job" in text:
                        self.t.check(f"{label}: an export during the slice is refused with Busy: job", True)
                        return walls, text, thread, box
                    thread.join()
                    self.t.check(f"{label}: an export during the slice is refused with Busy: job", False, text)
                    return None
            else:
                misses.append(f"{walls}: the slice ended before it was seen")
            thread.join()
        self.t.check(f"{label}: an export during the slice is refused with Busy: job", False, misses)
        return None

    def while_slicing_scenario(self):
        s = "an export while a slice runs"
        self.start(s)
        reply = self.run("project_import", paths=[BENCHY])
        wait(self.c, "idle")
        self.names_are(f"{s} 1", "3DBenchy.drc")
        self.t.equal(f"{s} 1: the reply's added", reply.get("added"), 1)
        self.project_is(f"{s} 1", "3DBenchy", None, "dirty")
        for steps, action, after in (("2-3", "orca_command:slice_and_preview:orca", "after_preview.gcode"),
                                     ("4", "orca_command:slice_all:orca", "after_all.gcode")):
            label = f"{s} {steps}"
            found = self.overlapping(label, action)
            if found is None:
                continue
            walls, _, thread, box = found
            self.no_file(label, f"during_{walls}.gcode")
            self.project_is(label, "3DBenchy", None, "dirty")
            thread.join(600)
            result = box.get("result") or {}
            self.t.check(f"{label}: the second client's call ends without an error", "error" not in box,
                         box.get("error"))
            plates_ = result.get("plates", [])
            self.t.equal(f"{label}: the slice's result", (result.get("status"), [p.get("outcome") for p in plates_]),
                         ("done", ["ok"]))
            layers = plates_[0].get("layers") if plates_ else None
            wait(self.c, "idle")
            undo_steps(self.c)
            reply = self.run("project_export", format="gcode", path=self.w(after))
            self.flags_are(label, [True])
            self.t.equal(f"{label}: the export", reply.get("export"), "ok")
            facts = gcode(self.w(after))
            self.t.equal(f"{label}: the G-code's layers and walls",
                         (facts["layers"], facts["config"].get("wall_loops")), (layers, str(walls)))
            self.tab_is(label, "preview")
            self.project_is(label, "3DBenchy", None, "dirty")
            self.steps_are(label, [])
            self.names_are(label, "3DBenchy.drc")

    def run_all(self):
        for name, scenario in [("the start", self.starts_from),
                               ("a new project", self.new_project_scenario),
                               ("the question keeps the layer G-code", self.layer_gcode_scenario),
                               ("opening a project", self.opening_scenario),
                               ("saving a project under a name", self.saving_scenario),
                               ("importing models", self.importing_scenario),
                               ("ZIP archives", self.zip_scenario),
                               ("a STEP file's mesh options", self.step_scenario),
                               ("exporting meshes", self.meshes_scenario),
                               ("exporting the model as a 3MF", self.model_3mf_scenario),
                               ("exporting the sliced file", self.sliced_scenario),
                               ("the G-code written", self.gcode_written_scenario),
                               ("an export while a slice runs", self.while_slicing_scenario)]:
            self.t.scenario(name, scenario)
        return self.t.exit_code()


def main():
    client, session = orca_mcp.session_client()
    folder = tempfile.mkdtemp(prefix="orca-files-")
    code = FilesTest(client, session, folder).run_all()
    if code == 0:
        shutil.rmtree(folder, ignore_errors=True)
    else:
        print(f"the files written are kept in {folder}")
    return code


if __name__ == "__main__":
    sys.exit(main())
