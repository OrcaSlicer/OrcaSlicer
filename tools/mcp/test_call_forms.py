#!/usr/bin/env python3
"""The forms a call takes, as the test_call_forms entry of tools/mcp/test-entries/gui_tools.md designs them.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_call_forms.py [--out DIR]

Names and value shapes agents send in place of the schema's, each shown by what the call did; settings_list leaving
meta out of a whole listing; and a call with exactly an action's arguments doing what a person's pick does: a test's
dialog asked or silent, View: Top turning the camera of the view shown, Deselect All closing the open gizmo too, Go to
layer through action_run, and plate_slice {preview: true}. It needs the Design tab (launch with --cad), starts on
the Generic Klipper Printer in the sweep's baseline, and exits 0 when every check passes. The files it exports go to
--out, else to a new temporary folder.
"""

import argparse
import os
import struct
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, plates, run, start, until, wait  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CUBE = os.path.join(REPO, "resources", "handy_models", "OrcaCube_v2.drc")
NAME = "OrcaCube_v2.drc"
PREFERENCES = "orca_command:open_preferences:orca"
# What replay_export lists unfinished while the second client's app_wait runs.
WAITING = [{"tool": "app_wait"}]


def stl(path):
    """A binary STL's triangle count and its size in bytes."""
    with open(path, "rb") as f:
        head = f.read(84)
    return struct.unpack("<I", head[80:84])[0], os.path.getsize(path)


class CallFormsTest:
    def __init__(self, client, session, folder):
        self.c = client
        self.session = session
        self.t = Checks()
        self.folder = folder

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def state(self):
        return self.c.call("app_state")

    def objects(self):
        """Each object as {name, plate, offset, bbox}, offset and bbox being its first instance's."""
        listing = self.c.call("object_list", limit=500, detail=True)
        return [{"name": o["name"], "plate": p["index"], "offset": o["instances"][0]["offset"],
                 "bbox": o["instances"][0]["bbox"]} for p in listing["plates"] for o in p["objects"]]

    def names(self):
        return [o["name"] for o in self.objects()]

    def selection(self):
        sel = self.state().get("selection") or {}
        return {"objects": sel.get("objects", []), "parts": sel.get("parts", [])}

    def selected(self, label, *objects):
        self.t.equal(f"{label}: the selection", self.selection(), {"objects": list(objects), "parts": []})

    def camera(self):
        cam = self.state()["camera"]
        return {k: cam.get(k) for k in ("elevation", "azimuth", "look_at", "zoom")}

    def legend(self):
        return self.c.call("plate_slice_stats", include=["view"])["view"]["legend"]

    def modified(self):
        out = []
        for kind in ("printer", "process", "filament"):
            out += [p["name"] for p in self.c.call("preset_list", type=kind, limit=500)["items"] if p.get("modified")]
        return out

    def dialogs(self):
        return [d.get("title") for d in self.state().get("dialogs", [])]

    def job_ends(self, label, job):
        try:
            self.c.await_job(job, timeout=60)
            self.t.check(f"{label}: the job ends without an error", True)
        except orca_mcp.McpError as e:
            self.t.check(f"{label}: the job ends without an error", False, str(e))

    def start(self, name):
        self.run("project_new", discard_changes=True, preset_changes="discard")
        self.run("view_set", tab="prepare")
        self.run("dialog_auto_answer", rules=[{"match": "restore", "button": "no"}])
        wait(self.c, "idle")
        self.t.equal(f"{name}: the start: the objects", self.objects(), [])
        self.t.equal(f"{name}: the start: the dialogs open", self.dialogs(), [])
        self.t.equal(f"{name}: the start: the plates", plates(self.c),
                     [{"name": "", "current": True, "locked": False, "objects": 0}])

    # ---- Scenarios ----

    def agent_names(self):
        s = "names and values agents send"
        self.start(s)
        reply = self.run("project_import", paths=CUBE)
        self.t.equal(f"{s} 1: a lone path: the reply's added", reply.get("added"), 1)
        self.t.equal(f"{s} 1: the names", self.names(), [NAME])
        self.run("project_import", paths=[CUBE])
        self.t.equal(f"{s} 2: the names", self.names(), [NAME, NAME])
        self.run("object_select", objects=[0])
        self.selected(f"{s} 3", 0)
        self.run("object_select", object="1")
        self.selected(f"{s} 4: object and a string index", 1)
        reply = self.run("object_rename", name="renamed")
        self.t.equal(f"{s} 5: the reply", reply.get("ran"), "object_rename")
        self.t.equal(f"{s} 5: the selected object was renamed", self.names(), [NAME, "renamed"])

        reply = self.run("action_run", action="view_top")
        self.t.equal(f"{s} 6: action and the short name", reply.get("ran"), "orca_command:view_top:orca")
        self.t.near(f"{s} 6: the camera looks down", self.camera()["elevation"], 90, 0.1)
        reply = self.run("view_set", canvas="front")
        self.t.equal(f"{s} 7: canvas for camera", reply.get("camera"), "front")
        cam = self.camera()
        self.t.near(f"{s} 7: the camera's elevation and azimuth", [cam["elevation"], cam["azimuth"]], [0, -90], 0.1)

        for i in (0, 1):
            self.run("object_transform", object=i, position=[100, 110, 0])
        for o in self.objects():
            self.t.near(f"{s} 8: position: {o['name']} at x 100, y 110", o["offset"][:2], [100, 110], 0.01)
        self.run("object_select", objects=[0])
        self.selected(f"{s} 9: after the select", 0)
        self.run("keyboard_press", key="Escape", target={"name": "OrcaSceneCanvas"})
        self.selected(f"{s} 9: key for keys: Escape reached the 3D view")

        job = self.c.call("action_run", id=PREFERENCES)
        self.t.equal(f"{s} 10: the action waits on its dialog", job.get("status"), "blocked_by_dialog")
        self.t.equal(f"{s} 10: the dialogs open", self.dialogs(), ["Preferences"])
        self.wait_without_condition(s)

        self.run("object_select", objects=[0])
        self.selected(f"{s} 12: after the select", 0)
        reply = self.run("widget_click", target={"gizmo": "Support Painting"})
        self.t.equal(f"{s} 12: a gizmo by its toolbar title",
                     (reply.get("gizmo"), reply.get("active")), ("FdmSupports", True))
        self.t.equal(f"{s} 12: the gizmo open", (self.state().get("gizmo") or {}).get("name"), "FdmSupports")
        reply = self.run("widget_click", target={"gizmo": "support painting"})
        self.t.equal(f"{s} 13: the title in another case closes it", reply.get("active"), False)
        self.t.equal(f"{s} 13: no gizmo open", self.state().get("gizmo"), None)

        self.run("object_arrange", plate="0")
        wait(self.c, "idle")
        self.apart_on_plate_0(f"{s} 14: plate as a string")
        reply = self.run("plate_slice", plate="0")
        self.t.equal(f"{s} 15: plate as a string: the slice",
                     [(p.get("index"), p.get("outcome")) for p in reply.get("plates", [])], [(0, "ok")])
        self.t.equal(f"{s} 15: plate 0 is sliced", self.c.call("plate_list")["items"][0]["sliced"], True)

        reply = self.run("view_set", tab="preview",
                         legend={"sparse_infill": False, "Travel": False, "Prime tower": False, "Pauses": False})
        self.t.equal(f"{s} 16: legend rows in another case or with _", reply.get("legend"),
                     {"Sparse infill": False, "travel": False})
        legend = self.legend()
        self.t.equal(f"{s} 16: the rows hidden", (legend["features"].get("Sparse infill"), legend["options"].get("travel")),
                     (False, False))
        self.run("view_set", legend={"Sparse infill": True, "travel": True})
        legend = self.legend()
        self.t.equal(f"{s} 17: the rows shown", (legend["features"].get("Sparse infill"), legend["options"].get("travel")),
                     (True, True))

        self.run("view_set", tab="prepare")
        self.t.equal(f"{s} 18: the tab", self.state()["tab"], "prepare")
        gcode = os.path.join(self.folder, "forms.gcode")
        reply = self.run("project_export", path=gcode, overwrite=True)
        self.t.equal(f"{s} 18: the export", reply.get("export"), "ok")
        with open(gcode, encoding="utf-8", errors="replace") as f:
            first = f.readline().rstrip("\r\n")
        self.t.equal(f"{s} 18: the format came from the path: G-code", first, "; HEADER_BLOCK_START")
        path = os.path.join(self.folder, "forms.stl")
        reply = self.run("project_export", path=path, overwrite=True)
        self.t.equal(f"{s} 19: the objects written", [o.get("index") for o in reply.get("objects", [])], [0, 1])
        self.t.equal(f"{s} 19: the reply's bytes are the file's", reply.get("bytes"), os.path.getsize(path))
        triangles = sum(part["triangles"] for p in self.c.call("object_list", detail=True)["plates"] for o in p["objects"]
                        for part in o["parts"])
        count, size = stl(path)
        self.t.equal(f"{s} 19: the STL holds the objects' triangles", count, triangles)
        self.t.equal(f"{s} 19: the STL's size", size, 84 + 50 * count)

        items = self.c.call("settings_list", type="process", limit=5)["items"]
        self.t.equal(f"{s} 20: five settings", len(items), 5)
        self.t.check(f"{s} 20: each with a value and no meta", all("value" in i and "meta" not in i for i in items), items)
        items = self.c.call("settings_get", type="process", keys=["wall_loops"])["items"]
        self.t.equal(f"{s} 21: settings_get gives the meta", items[0].get("meta", {}).get("type"), "int")

    def wait_without_condition(self, s):
        """Step 11: a second client's app_wait {} while Preferences is open, seen unfinished in replay_export."""
        other = orca_mcp.OrcaMcp(self.session["port"], self.session["token"])
        done = {}

        def wait_idle():
            try:
                started = time.monotonic()
                done["reply"] = other.call("app_wait", _timeout=120)
                done["seconds"] = time.monotonic() - started
            except Exception as e:  # reported by the checks below
                done["error"] = e

        thread = threading.Thread(target=wait_idle, daemon=True)
        thread.start()
        running = lambda: self.c.call("replay_export").get("left_out_running")
        # A wait never seen fails its check, and the scenario goes on to answer Preferences.
        try:
            seen = until(lambda: running() == WAITING and WAITING, "replay_export lists the second client's app_wait")
        except TimeoutError:
            seen = running()
        self.t.equal(f"{s} 11: replay_export lists the wait unfinished", seen, WAITING)
        # Preferences stays open a second, the window the wait must last through.
        time.sleep(1)
        self.t.equal(f"{s} 11: a second later the wait is still unfinished", running(), WAITING)
        self.t.equal(f"{s} 11: Preferences is still open", self.dialogs(), ["Preferences"])
        self.c.call("dialog_answer", dialog={"title": "Preferences"}, button="close")
        thread.join(60)
        reply = done.get("reply") or {}
        self.t.check(f"{s} 11: the wait ends without an error", "error" not in done and not thread.is_alive(), done)
        self.t.equal(f"{s} 11: the wait is satisfied", reply.get("satisfied"), True)
        self.t.check(f"{s} 11: the wait lasted at least a second", reply.get("waited_ms", 0) >= 1000, reply)
        self.t.check(f"{s} 11: replay_export lists nothing unfinished", "left_out_running" not in self.c.call("replay_export"),
                     self.c.call("replay_export").get("left_out_running"))
        self.t.equal(f"{s} 11: no dialog", self.dialogs(), [])

    def apart_on_plate_0(self, label):
        objects = self.objects()
        self.t.equal(f"{label}: both objects on plate 0", [o["plate"] for o in objects], [0, 0])
        item = self.c.call("plate_list")["items"][0]
        (x, y), (w, d) = item["origin"], item["size"]
        for o in objects:
            box = o["bbox"]
            inside = box["min"][0] >= x and box["min"][1] >= y and box["max"][0] <= x + w and box["max"][1] <= y + d
            self.t.check(f"{label}: {o['name']} lies inside plate 0", inside, (box, (x, y, w, d)))
        a, b = (o["bbox"] for o in objects)
        apart = (a["max"][0] <= b["min"][0] or b["max"][0] <= a["min"][0] or a["max"][1] <= b["min"][1] or
                 b["max"][1] <= a["min"][1])
        self.t.check(f"{label}: the objects' footprints are apart", apart, (a, b))

    def test_dialog(self):
        s = "a test's dialog, asked or silent"
        self.start(s)
        project = self.state()["project"]
        self.t.equal(f"{s}: the start: the project's name", project.get("name"), "Untitled")
        self.t.equal(f"{s}: the start: no preset modified", self.modified(), [])
        job = self.c.call("calibration_generate_test", test="temperature")
        self.t.equal(f"{s} 1: the test asks in its dialog", job.get("status"), "blocked_by_dialog")
        self.t.equal(f"{s} 1: the dialogs open", self.dialogs(), ["Temperature calibration"])
        self.c.call("dialog_answer", dialog={"title": "Temperature calibration"}, button="cancel")
        self.job_ends(f"{s} 2: the step 1 call", job)
        wait(self.c, "idle")
        self.t.equal(f"{s} 2: the dialogs open", self.dialogs(), [])
        self.t.equal(f"{s} 2: Cancel adds no object", self.objects(), [])
        project = self.state()["project"]
        self.t.equal(f"{s} 2: the project is as it was", (project.get("name"), project.get("dirty")), ("Untitled", False))
        self.t.equal(f"{s} 2: no preset modified", self.modified(), [])
        reply = self.run("calibration_generate_test", test="temperature", silent=True, discard_changes=True,
                         preset_changes="discard")
        wait(self.c, "idle")
        self.t.equal(f"{s} 3: the reply", (reply.get("test"), reply.get("names")), ("temperature", ["temperature_tower.drc"]))
        self.t.equal(f"{s} 3: the names", self.names(), ["temperature_tower.drc"])
        self.t.equal(f"{s} 3: the project's name", self.state()["project"].get("name"), "Nozzle temperature test")
        self.t.equal(f"{s} 3: silent asks nothing", self.dialogs(), [])

    def shown_view(self):
        s = "the shown view"
        self.start(s)
        self.run("project_import", paths=[CUBE])
        self.run("object_transform", object=0, size=[20, 20, 20])
        objects = self.objects()
        self.t.equal(f"{s}: the start: one object on plate 0", [o["plate"] for o in objects], [0])
        self.t.near(f"{s}: the start: 20 mm", objects[0]["bbox"]["size"] if objects else None, [20, 20, 20], 0.01)

        self.run("view_set", tab="prepare", camera="front")
        self.t.equal(f"{s} 1: the tab", self.state()["tab"], "prepare")
        self.t.near(f"{s} 1: the Prepare view's elevation", self.camera()["elevation"], 0, 0.1)
        reply = self.run("view_set", tab="design", camera="front")
        self.t.near(f"{s} 2: the reply's elevation", (reply.get("camera_state") or {}).get("elevation"), 0, 0.1)
        self.t.equal(f"{s} 2: the tab", self.state()["tab"], "design")
        self.t.near(f"{s} 2: the Design tab's elevation", self.camera()["elevation"], 0, 0.1)
        reply = self.run("action_run", id="view_top")
        self.t.equal(f"{s} 3: the action run", reply.get("ran"), "orca_command:view_top:orca")
        self.t.near(f"{s} 3: the reply's elevation", (reply.get("camera_state") or {}).get("elevation"), 90, 0.1)
        self.t.equal(f"{s} 3: the tab", self.state()["tab"], "design")
        self.t.near(f"{s} 3: the Design tab's elevation", self.camera()["elevation"], 90, 0.1)
        self.run("view_set", tab="prepare")
        self.t.equal(f"{s} 4: the tab", self.state()["tab"], "prepare")
        self.t.near(f"{s} 4: View: Top left the Prepare view's camera", self.camera()["elevation"], 0, 0.1)

        self.run("object_select", objects=[0])
        self.run("widget_click", target={"gizmo": "Support Painting"})
        self.selected(f"{s} 5", 0)
        self.t.equal(f"{s} 5: the gizmo open", (self.state().get("gizmo") or {}).get("name"), "FdmSupports")
        reply = self.run("action_run", id="deselect_all")
        self.t.equal(f"{s} 6: the reply", (reply.get("ran"), reply.get("empty"), reply.get("canvas")),
                     ("orca_command:deselect_all:orca", True, "3d"))
        self.selected(f"{s} 6")
        self.t.equal(f"{s} 6: Deselect All closes the open gizmo too", self.state().get("gizmo"), None)

        reply = self.run("plate_slice", preview=True)
        self.t.equal(f"{s} 7: the slice", (reply.get("status"), [p.get("outcome") for p in reply.get("plates", [])]),
                     ("done", ["ok"]))
        self.t.equal(f"{s} 7: the tab", self.state()["tab"], "preview")
        self.t.equal(f"{s} 7: plate 0 is sliced", self.c.call("plate_list")["items"][0]["sliced"], True)
        self.run("action_run", id="go_to_layer", args={"percent": 50})
        view = self.c.call("plate_slice_stats", include=["view"])["view"]
        self.t.equal(f"{s} 8: the layers", view.get("layer_count"), 100)
        self.t.equal(f"{s} 8: the range shown", view.get("layers"), [1, 51])

    def run_all(self):
        self.t.equal("the start: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        for name, scenario in [("names and values agents send", self.agent_names),
                               ("a test's dialog, asked or silent", self.test_dialog),
                               ("the shown view", self.shown_view)]:
            self.t.scenario(name, scenario)
        return self.t.exit_code()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="folder for the exported files (default: a new temporary folder)")
    args = ap.parse_args()
    folder = args.out or tempfile.mkdtemp(prefix="orca-call-forms-")
    os.makedirs(folder, exist_ok=True)
    print(f"output folder: {folder}", flush=True)
    client, session = orca_mcp.session_client()
    return CallFormsTest(client, session, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
