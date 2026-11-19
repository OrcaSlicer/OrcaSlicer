#!/usr/bin/env python3
"""Runs every Speed Dial action of tests/data/speed_dial_actions.json through action_run and checks that it does its
row's job: the change its variant alone makes, the window it opens, the file it writes, or the refusal of an action this
harness cannot give a state to (a printer, an AMS, an account).

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_every_action.py [--out DIR]

ROWS holds one row per action key; a key of the actions file with no row, or a row of a key the file lacks, fails the
test. Before each action its state is set up afresh by calls (STATES), so that no action sees what another left: New
Project and Delete All among them. A row's dialog is answered as the row says: a button of one of the app's own
dialogs, a text typed into it first, or a rule for a native file picker registered beforehand, with a file in the
output folder.

For every action that ran, the reply's ran is the action's id, and the step replay_export writes for the call is an
action_run of that id with the arguments its command journals: for Go to tab and Go to layer the value under the
action's input token, and none for every other action, whose command journals nothing of its own (a step with the id
alone replays the same, since the action supplies its fixed arguments). The calibration actions are among those: they
open their test's dialog, and calibration_generate_test journals its arguments only when it runs the test itself. The
action names the command of tests/data/speed_dial_action_calls.json in action_search. An action expected to be refused
has neither: action_run adds ran only when the command ran, and a failed call is not exported.

Three actions open a web page in the system's browser and one opens the configuration folder in the file manager; the
test runs them once and leaves what they opened. Send G-code, Synchronize Filament List from AMS and Sync Presets are
expected to be refused: nothing is signed in, and on the seeded printer, which prints through the Bambu network, Send
G-code would open the dialog that sends to the printer's storage, which messages a printer, so its row makes the printer
a print host with no address first, and discards that change after. Import, Import
Configs and SVG open a file picker that dialog_auto_answer cannot give a file (a multi-select picker, and one whose
caller reads GetPaths()): the rule cancels it, and the row checks that the picker was answered and nothing loaded. The
dialogs whose choices are a person's (Export Preset Bundle, the Setup Wizard, the Network Test) are closed. Every file the
test writes is in its output folder (a new temporary folder unless --out names one) or in the app's configuration
folder; Start Recording's folder, where Take Screenshot saves too, is set to the output folder through the Start
Recording dialog before the actions run. Exits 0 when every check passes.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import checks  # noqa: E402
import orca_mcp  # noqa: E402

DATA = os.path.join(HERE, "..", "..", "tests", "data")
FIXTURES = os.path.join(HERE, "tests", "fixtures")
PLUGINS = os.path.join(HERE, "tests", "plugins")
PREFIX, SUFFIX = "orca_command:", ":orca"
BED_CENTRE = 128.0  # the seeded Bambu Lab X1 Carbon's 256 mm bed
CANVAS = {"name": "OrcaSceneCanvas"}
IMPORTED_PRESET = "MCP Import PLA"  # the filament fixtures/mcp_import_pla.json holds
LOCAL_PLUGIN, SIDE_LOADED = "MCP Tools Fixture", "MCP Web Fixture"  # the fixture plugins' names, from their manifests


def action_id(key):
    return PREFIX + key + SUFFIX


class Run:
    """The client, the output folder and what one action's checks read."""

    def __init__(self, client, out, datadir):
        self.client, self.out, self.datadir = client, out, datadir
        self.reply = None    # the action's final reply
        self.before = None   # what the row's before() read after the state was set up
        self.windows = []    # the titles window_list gave before the action ran
        self.seen = []       # the titles of the dialogs the row's answers found open

    def call(self, tool, /, **args):
        return self.client.call(tool, **args)

    def wait(self, reply):
        return self.client.await_job(reply)

    def objects(self):
        """Every object, in object_list's order of plates, each with the plate it stands on (None off every plate)."""
        listing = self.call("object_list")
        out = [dict(o, plate=p["index"]) for p in listing.get("plates", []) for o in p.get("objects", [])]
        return out + [dict(o, plate=None) for o in listing.get("outside", []) if isinstance(o, dict)]

    def names(self):
        return [o["name"] for o in self.objects()]

    def plates(self):
        return self.call("plate_list").get("items", [])

    def state(self):
        return self.call("app_state")

    def slots(self):
        return len(self.state().get("presets", {}).get("filaments", []))

    def file(self, name):
        path = os.path.join(self.out, name)
        return path if os.path.isfile(path) and os.path.getsize(path) > 0 else None

    def path(self, name):
        return os.path.join(self.out, name)

    def preference(self, key):
        items = self.call("preferences_get", keys=[key]).get("items", [])
        return items[0].get("value") if items else None

    def plugin_names(self):
        return [r.get("name") for r in self.call("plugin_list").get("plugins", [])]

    def canvas_height(self):
        """The height of the 3D view as widget_list gives it."""
        _, texts = self.client.call_with_text("widget_list", include_menus=False, include_imgui=False, include_web=False)
        for line in "\n".join(texts).splitlines():
            if 'canvas "OrcaSceneCanvas"' in line:
                return int(line.rsplit("x", 1)[1])
        return None

    def imgui_windows(self):
        _, texts = self.client.call_with_text("widget_list", include_menus=False, include_web=False)
        return [line.split('"')[1] for line in "\n".join(texts).splitlines() if "imgui_window" in line and '"' in line]

    def new_windows(self):
        """The windows the action opened: those its answers found open, and those still open that were not before."""
        shown = [w["title"] for w in self.call("window_list").get("items", []) if w["title"] not in self.windows]
        return self.seen + [t for t in shown if t not in self.seen]


# ---- The files the actions read ----

def box(x0, size):
    """The twelve facets of an axis-aligned cube of `size` mm whose corner is at (x0, 0, 0), as ASCII STL text."""
    v = [(x0 + dx * size, dy * size, dz * size) for dx in (0, 1) for dy in (0, 1) for dz in (0, 1)]
    faces = [(0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6), (0, 1, 5), (0, 5, 4), (2, 6, 7), (2, 7, 3), (0, 4, 6), (0, 6, 2),
             (1, 3, 7), (1, 7, 5)]
    return "".join("facet normal 0 0 0\nouter loop\n" + "".join("vertex %g %g %g\n" % v[i] for i in f) + "endloop\nendfacet\n"
                   for f in faces)


def write_inputs(out):
    """The files the import, open and install actions pick: an STL of one mesh with two shells, a ZIP holding it, the
    fixture preset, SVG and plugin."""
    stl = os.path.join(out, "two_shells.stl")
    with open(stl, "w") as f:
        f.write("solid two_shells\n" + box(0, 10) + box(20, 10) + "endsolid two_shells\n")
    with zipfile.ZipFile(os.path.join(out, "two_shells.zip"), "w") as z:
        z.write(stl, "two_shells.stl")
    shutil.copy(os.path.join(FIXTURES, "mcp_import_pla.json"), out)
    shutil.copy(os.path.join(FIXTURES, "star.svg"), out)
    shutil.copy(os.path.join(PLUGINS, "mcp_tools_fixture", "mcp_tools_fixture.py"), out)
    for folder in ("stl_multi", "drc_multi"):
        os.makedirs(os.path.join(out, folder), exist_ok=True)


# ---- States: each set up afresh by calls before its action ----

RESTORE = {"match": "restore", "button": "no"}  # the prompt to restore unsaved items after a start


def empty(run):
    run.call("dialog_auto_answer", rules=[RESTORE])
    if run.state().get("recording"):
        run.call("recording_stop", silent=True)
    run.call("project_new", discard_changes=True, preset_changes="discard")
    run.call("view_set", tab="prepare", canvas="3d")
    while run.slots() > 1:
        run.call("filament_delete")
    run.call("settings_set_mode", mode="expert")
    run.call("app_wait", **{"for": "idle"})


def two_cubes(run):
    """Two 20 mm cubes side by side on the plate, the first selected."""
    empty(run)
    for x in (100, 156):
        run.call("object_select", none=True)
        run.call("object_add_primitive", shape="cube", silent=True, size=[20, 20, 20], position=[x, BED_CENTRE, 10])
    run.call("object_select", objects=[0])


def sliced(run):
    two_cubes(run)
    run.wait(run.call("plate_slice"))


def assembled(run):
    """The two cubes assembled into one object of two parts, selected."""
    two_cubes(run)
    run.call("object_assemble", objects=[0, 1])
    run.call("object_select", objects=[0])


def part_selected(run):
    """The assembled object with its second part selected."""
    assembled(run)
    run.call("object_select", parts=[[0, 1]])


def offset_parts(run):
    """One object of two parts apart along x, y and z, selected: a mirror across any axis moves them."""
    empty(run)
    for position in ([100, 110, 10], [156, 150, 10]):
        run.call("object_select", none=True)
        run.call("object_add_primitive", shape="cube", silent=True, size=[20, 20, 20], position=position)
    run.call("object_assemble", objects=[0, 1])
    run.call("object_transform", object=0, part=1, translate=[0, 0, 10])
    run.call("object_select", objects=[0])


def two_plates(run):
    """The two cubes, the second on a second plate, which is current."""
    two_cubes(run)
    run.call("plate_add")
    run.call("object_transform", object=1, plate=1)
    run.call("plate_select", plate=1)


def sliced_plates(run):
    """The two plates, both sliced, the second current."""
    two_plates(run)
    run.wait(run.call("plate_slice", plates="all"))
    run.call("plate_select", plate=1)


def two_shells(run):
    """One object whose mesh is two shells, selected."""
    empty(run)
    run.call("project_import", paths=[run.path("two_shells.stl")])
    run.call("object_select", objects=[0])


def undone(run):
    """The two cubes with the second's addition undone."""
    two_cubes(run)
    run.call("undo")


def saved_project(run):
    """An empty project, after the two cubes were saved as load_me.3mf in the output folder."""
    two_cubes(run)
    run.call("project_save", path=run.path("load_me.3mf"), overwrite=True)
    run.call("project_new", discard_changes=True)


def recording(run):
    empty(run)
    run.call("recording_start", save_path=run.path("stopped.mkv"), overwrite=True)


STATES = {"empty": empty, "two_cubes": two_cubes, "sliced": sliced, "assembled": assembled, "part_selected": part_selected,
          "two_plates": two_plates, "sliced_plates": sliced_plates, "offset_parts": offset_parts, "two_shells": two_shells,
          "undone": undone,
          "saved_project": saved_project, "recording": recording}


# ---- Answers ----

def picker(name):
    """A rule for the native file or folder picker the action opens: it returns `name` in the output folder."""
    return ("picker", name)


def button(title, label):
    """Press `label` on the app's dialog whose title holds `title`, once it is open."""
    return ("button", title, label)


def typed(title, cls, value, label="ok"):
    """Type `value` into the `cls` control of the dialog titled `title`, then press `label`."""
    return ("typed", title, cls, value, label)


def answer(run, answers):
    """Carries out the row's answers that come after the call: the dialog buttons and typed values."""
    for a in answers:
        if a[0] == "picker":
            continue
        title = a[1]
        deadline = time.time() + 30
        while time.time() < deadline and not any(title.lower() in d.get("title", "").lower() for d in run.state()["dialogs"]):
            time.sleep(0.3)
        run.seen += [d["title"] for d in run.state()["dialogs"] if d["title"] not in run.seen]
        if a[0] == "typed":
            run.call("widget_set_value", target={"class": a[2], "within": a[1]}, value=a[3], submit=False)
        run.call("dialog_answer", dialog={"title": title}, button=a[-1])


# ---- Expectations ----

def objects(fn):
    return ("object_list", fn)


def plates(fn):
    return ("plate_list", fn)


def app_state(fn):
    return ("app_state", fn)


def window(title):
    """The action opened the window or dialog titled `title`, which it had not shown before."""
    return ("window", lambda r: (any(title.lower() in t.lower() for t in r.new_windows()), r.new_windows()))


def written(name):
    return ("file", lambda r: (r.file(name) is not None, os.listdir(r.out)))


def plates_in(name, plates):
    """The sliced file `name` holds the G-code of `plates` (numbered from 1, as the archive names them) and of no other."""
    def check(r):
        if r.file(name) is None:
            return False, os.listdir(r.out)
        with zipfile.ZipFile(r.path(name)) as z:
            held = sorted(n for n in z.namelist() if n.startswith("Metadata/plate_") and n.endswith(".gcode"))
        return held == [f"Metadata/plate_{p}.gcode" for p in plates], held
    return ("file", check)


def written_each(folder, extension, count):
    def check(r):
        found = [f for f in os.listdir(r.path(folder)) if f.lower().endswith(extension)]
        return len(found) == count, found
    return ("file", check)


# Why a row that opens a file picker is shown by the picker it opened and not by the file it would load.
PATHLESS = "dialog_auto_answer cannot give this picker a file, so its rule cancels it and nothing is loaded"


def picked(unchanged):
    """The action opened its file picker, which the row's rule answered, and with nothing chosen `unchanged` holds."""
    def check(r):
        uses = [rule.get("uses", 0) for rule in r.state().get("auto_answer_rules", []) if rule.get("path")]
        return uses == [1] and unchanged(r), (uses, r.names())
    return ("picker", check)


def external(why):
    return ("external", why)


def refused(code, why, nothing_opened=False):
    """Refused with `code`; with `nothing_opened`, also before any dialog, with nothing uploaded."""
    return ("refused", code, why, nothing_opened)


def near(a, b, tolerance=1.0):
    return abs(a - b) <= tolerance


def centre(o):
    box_ = o["bbox"]
    return [(box_["min"][i] + box_["max"][i]) / 2 for i in range(3)]


def camera(elevation=None, azimuth=None):
    def check(r):
        cam = r.state().get("camera") or {}
        ok = (elevation is None or near(cam.get("elevation", 999), elevation, 2)) and \
             (azimuth is None or near(cam.get("azimuth", 999), azimuth, 2) or near(abs(cam.get("azimuth", 999)), abs(azimuth), 2) and
              abs(azimuth) == 180)
        return ok, cam
    return app_state(check)


def part_centres(r):
    return [centre(p) for p in r.objects()[0]["parts"]]


def mirrored_along(r, axis):
    """Whether the parts' centres moved along `axis` alone, as a mirror of their object across that axis moves them, and
    the object stays mirrored."""
    after = part_centres(r)
    moved = [any(not near(a[i], b[i], 0.01) for a, b in zip(r.before, after)) for i in range(3)]
    return moved == [i == axis for i in range(3)] and r.objects()[0]["instances"][0]["mirrored"] is True


def part_types(r):
    return sorted(p["type"] for p in r.objects()[0]["parts"])


def heights(r):
    return [round(o["bbox"]["size"][2]) for o in r.objects()]


def call(tool, /, **args):
    return (tool, args)


def row(key, state, expect, *, value=None, answers=(), pre=(), post=(), before=None, after=None, why=""):
    """`after`, when given, reads the app once the row's post steps have run: (ok, detail)."""
    return key, {"state": state, "expect": expect, "value": value, "answers": tuple(answers), "pre": tuple(pre),
                 "post": tuple(post), "before": before, "after": after, "why": why}


CUBE_OFF_PLATE = call("object_transform", object=1, position=[-150, BED_CENTRE, 10])
LIFTED = call("object_transform", object=0, translate=[0, 0, 15], drop_to_bed=False)
TWO_SLOTS = call("filament_add")
ORDER_NAMES = (call("object_rename", object=0, name="b_cube"), call("object_rename", object=1, name="a_cube"))
# Three objects in an order that is neither ascending nor descending by height or by size, and whose order by height
# is the reverse of their order by size: an action that sorts by the other key, or reverses an order it finds sorted
# (ObjectList::order_objects()), leaves another order.
ORDER_SIZES = (call("object_select", none=True),
               call("object_add_primitive", shape="cube", silent=True, size=[10, 10, 30], position=[BED_CENTRE, 200, 15]),
               call("object_transform", object=0, size=[20, 20, 20]), call("object_transform", object=1, size=[40, 40, 10]))
ORDER_FILAMENTS = (TWO_SLOTS, call("settings_set", target="object", object=0, values={"extruder": 2}))
SHAPES = {"cube": "Cube", "cylinder": "Cylinder", "sphere": "Sphere", "cone": "Cone", "disc": "Disc", "torus": "Torus"}
# The objects each handy model adds to an empty project, as its files name them.
HANDY = {"orca_cube": ["OrcaCube_v2.drc", "OrcaPlug_v2.drc"],
         "orcasliced_combo": ["Orca Head", "OrcaCube_v2.drc", "OrcaPlug_v2.drc", "Orca Belly", "Orca Pedestal"],
         "orca_badge": ["OrcaSlicer Badge Horizontal", "OrcaSlicer Badge Vertical", "OrcaSlicer Badge Logo"],
         "orca_tolerance_test": ["OrcaToleranceTest.drc"], "3dbenchy": ["3DBenchy.drc"], "cali_cat": ["calicat.drc"],
         "autodesk_fdm_test": ["ksr_fdmtest_v4.drc"], "voron_cube": ["Voron_Design_Cube_v7.drc"],
         "stanford_bunny": ["Stanford_Bunny.drc"], "orca_string_hell": ["Orca_stringhell.drc"]}
# The question a handy model asks once it is loaded, which the test answers so that no setting changes.
HANDY_ASKS = {"orca_string_hell": [("button", "Suggestion", "no")]}
CALIBRATIONS = {"temperature": ("Temperature calibration", "Nozzle temperature test"),
                "max_volumetric": ("Max volumetric speed test", "Max volumetric speed test"),
                "pressure_advance": ("PA Calibration", "Pressure Advance Test"),
                "flow_ratio": ("Flow Ratio Calibration", "Orca YOLO Flow Calibration"),
                "retraction": ("Retraction", "Retraction"),
                "cornering": ("Cornering test", "Cornering test"),
                "input_shaping_freq": ("Input shaping Frequency test", "Input shaping Frequency test"),
                "input_shaping_damp": ("Input shaping Damp test", "Input shaping Damping test"),
                "vfa": ("VFA test", "VFA test")}


def dialog_action(key, title, why=""):
    """A Help or window action whose job is to show one of the app's dialogs, which the test then closes."""
    return row(key, "empty", window(title), answers=[button(title, "close")], why=why)


ROWS = dict([
    # ---- Slice & Export ----
    row("slice_and_preview", "two_cubes", plates(lambda r: (r.plates()[0]["sliced"] and r.state()["tab"] == "preview",
                                                            (r.plates()[0]["sliced"], r.state()["tab"])))),
    row("slice_all", "two_plates", plates(lambda r: (all(p["sliced"] for p in r.plates()) and len(r.plates()) == 2,
                                                     [p["sliced"] for p in r.plates()]))),
    row("export_gcode", "sliced", written("plate.gcode"), answers=[picker("plate.gcode")]),
    row("export_stl", "two_cubes", written("model.stl"), answers=[picker("model.stl")]),
    row("export_3mf", "two_cubes", written("model.3mf"), answers=[picker("model.3mf")]),
    # From two sliced plates, the second current: the current plate's G-code alone, and both plates'.
    row("export_sliced_file", "sliced_plates", plates_in("plate.gcode.3mf", [2]), answers=[picker("plate.gcode.3mf")]),
    row("export_all_sliced_file", "sliced_plates", plates_in("all.gcode.3mf", [1, 2]), answers=[picker("all.gcode.3mf")]),
    # On the seeded X1 Carbon, which prints through the Bambu network, the action opens the dialog that sends to the
    # printer's storage, which messages a printer: the row makes it a print host with no address first, and its post
    # step discards that, so later rows see the X1 Carbon as it was.
    row("send_gcode", "sliced", refused("no_print_host", "the X1 Carbon, made a print host with no address, has no host to "
                                        "upload to, and the test uploads to no printer", nothing_opened=True),
        pre=[call("settings_set", type="printer", values={"bbl_use_printhost": True, "host_type": "octoprint", "print_host": ""})],
        post=[call("settings_set", type="printer", reset_all=True)],
        after=lambda r: (lambda v, u: (v == {"bbl_use_printhost": "0"} and u == [], (v, u)))(
            checks.values(r.client, ["bbl_use_printhost"], "printer"), checks.unsaved(r.client, "printer"))),
    # ---- Commands ----
    row("go_to_layer", "sliced", plates(lambda r: (lambda v: (v.get("layer_count", 0) > 0 and
                                                              near((v.get("layers") or [0, 0])[1], v.get("layer_count", 0) / 2, 2), v))(
        r.call("plate_slice_stats", include=["view"]).get("view") or {})), value={"percent": 50}),
    row("go_to_tab", "two_cubes", app_state(lambda r: (r.state()["tab"] == "preview", r.state()["tab"])), value={"tab": "preview"}),
    row("load_project", "saved_project", objects(lambda r: (len(r.objects()) == 2 and
                                                            r.state()["project"]["path"].endswith("load_me.3mf"),
                                                            (r.names(), r.state()["project"]))),
        answers=[picker("load_me.3mf")]),
    row("save_project_as", "two_cubes", written("saved_as.3mf"), answers=[picker("saved_as.3mf")]),
    row("undo", "two_cubes", objects(lambda r: (len(r.objects()) == 1, r.names()))),
    row("redo", "undone", objects(lambda r: (len(r.objects()) == 2, r.names()))),
    row("toggle_run_script_panel", "empty",
        ("widget", lambda r: (r.canvas_height() not in (None, r.before), (r.before, r.canvas_height()))),
        before=lambda r: r.canvas_height(), post=[call("tool_call", tool="script_toggle_panel", args={})],
        why="the Run Script panel is kept from automation, so widget_list reads the 3D view it docks under"),
    row("open_action_replay", "empty", window("Action Replay"),
        post=[call("keyboard_press", keys="Escape", target={"label": "Action Replay"})]),
    dialog_action("open_preferences", "Preferences"),
    row("take_screenshot", "two_cubes", ("file", lambda r: (bool(r.reply.get("path")) and os.path.isfile(r.reply["path"]) and
                                                           os.path.dirname(r.reply["path"]) == r.out, r.reply))),
    # ---- Mode ----
    row("mode_simple", "empty", app_state(lambda r: (r.state()["mode"] == "simple", r.state()["mode"]))),
    row("mode_advanced", "empty", app_state(lambda r: (r.state()["mode"] == "advanced", r.state()["mode"]))),
    row("mode_expert", "empty", app_state(lambda r: (r.state()["mode"] == "expert", r.state()["mode"])),
        pre=[call("settings_set_mode", mode="simple")]),
    row("toggle_developer_mode", "empty",
        ("preferences_get", lambda r: (r.preference("developer_mode") == (not r.before), (r.before, r.preference("developer_mode")))),
        before=lambda r: r.preference("developer_mode"), post=[call("tool_call", tool="settings_toggle_developer_mode", args={})]),
    # ---- Calibration ----
    *[row("calib_" + test, "empty", app_state(lambda r, name=name: (r.state()["project"]["name"] == name and len(r.objects()) > 0,
                                                                   (r.state()["project"], r.names()))),
          answers=[button(title, "ok")]) for test, (title, name) in CALIBRATIONS.items()],
    # ---- View ----
    row("view_top", "two_cubes", camera(elevation=90), pre=[call("view_set", camera="front")]),
    row("view_bottom", "two_cubes", camera(elevation=-90), pre=[call("view_set", camera="front")]),
    row("view_front", "two_cubes", camera(elevation=0, azimuth=-90), pre=[call("view_set", camera="top")]),
    row("view_rear", "two_cubes", camera(elevation=0, azimuth=90), pre=[call("view_set", camera="top")]),
    row("view_left", "two_cubes", camera(elevation=0, azimuth=180), pre=[call("view_set", camera="top")]),
    row("view_right", "two_cubes", camera(elevation=0, azimuth=0), pre=[call("view_set", camera="top")]),
    row("view_iso", "two_cubes", camera(elevation=45, azimuth=-135), pre=[call("view_set", camera="top")]),
    row("view_default", "two_cubes", camera(elevation=45, azimuth=-90), pre=[call("view_set", camera="top")]),
    row("view_fit_bed", "two_cubes", app_state(lambda r: (lambda cam: (cam.get("zoom", 0) < r.before and
                                                                       near(cam["look_at"][0], BED_CENTRE, 5), (r.before, cam)))(
        r.state()["camera"])), pre=[call("view_set", camera="fit_selection")], before=lambda r: r.state()["camera"]["zoom"]),
    row("view_toggle_perspective", "two_cubes",
        app_state(lambda r: (r.state()["view"]["projection"] != r.before, (r.before, r.state()["view"]["projection"]))),
        before=lambda r: r.state()["view"]["projection"], post=[call("view_set", projection="toggle")]),
    row("reset_window_layout", "two_cubes",
        app_state(lambda r: (r.state()["view"]["sidebar_collapsed"] is False, r.state()["view"])),
        pre=[call("view_set", sidebar_collapsed=True)]),
    # ---- Object ----
    row("obj_delete", "two_cubes", objects(lambda r: ([o["id"] for o in r.objects()] == [r.before], r.objects())),
        before=lambda r: r.objects()[1]["id"]),
    row("obj_delete_all", "two_cubes", objects(lambda r: (r.objects() == [], r.names()))),
    row("select_all", "two_cubes", app_state(lambda r: (r.state()["selection"]["objects"] == [0, 1], r.state()["selection"])),
        pre=[call("object_select", none=True)]),
    row("deselect_all", "two_cubes", app_state(lambda r: (r.state()["selection"]["empty"] is True, r.state()["selection"]))),
    *[row("obj_mirror_" + axis, "offset_parts", objects(lambda r, i=i: (mirrored_along(r, i), (r.before, part_centres(r)))),
          before=part_centres) for i, axis in enumerate("xyz")],
    row("obj_split_objects", "two_shells", objects(lambda r: (len(r.objects()) == 2, r.names()))),
    row("obj_split_parts", "two_shells", objects(lambda r: (len(r.objects()) == 1 and len(r.objects()[0]["parts"]) == 2,
                                                            [len(o["parts"]) for o in r.objects()]))),
    row("obj_center", "two_cubes", objects(lambda r: (near(centre(r.objects()[0])[0], BED_CENTRE) and
                                                      near(centre(r.objects()[0])[1], BED_CENTRE), centre(r.objects()[0]))),
        pre=[call("object_transform", object=0, position=[60, 60, 10])]),
    row("obj_drop", "assembled", objects(lambda r: (near(r.objects()[0]["bbox"]["min"][2], 0, 0.01), r.objects()[0]["bbox"])),
        pre=[LIFTED]),
    row("obj_fit_volume", "two_cubes", objects(lambda r: (r.objects()[0]["bbox"]["size"][0] > 200, r.objects()[0]["bbox"]))),
    row("obj_instances_up", "two_cubes", objects(lambda r: (len(r.objects()[0]["instances"]) == 2,
                                                            len(r.objects()[0]["instances"])))),
    row("obj_instances_down", "two_cubes", objects(lambda r: (len(r.objects()[0]["instances"]) == 1,
                                                              len(r.objects()[0]["instances"]))),
        pre=[call("object_set_instances", object=0, count=2), call("object_select", objects=[0])]),
    row("obj_arrange", "two_cubes", objects(lambda r: (len(r.objects()) == 2 and all(o["plate"] is not None for o in r.objects()),
                                                       [(o["name"], o["plate"]) for o in r.objects()])), pre=[CUBE_OFF_PLATE]),
    row("obj_orient", "two_cubes", objects(lambda r: (near(r.objects()[0]["bbox"]["size"][2], 20, 0.5), r.objects()[0]["bbox"])),
        pre=[call("object_transform", object=0, rotate=[45, 0, 0]), call("object_select", objects=[0])]),
    *[row("obj_type_" + action, "part_selected", objects(lambda r, t=t: (part_types(r) == sorted(["part", t]), part_types(r))))
      for action, t in (("negative", "negative"), ("modifier", "modifier"), ("support_blocker", "support_blocker"),
                        ("support_enforcer", "support_enforcer"), ("precise_seam", "precise_seam"))],
    row("obj_type_part", "part_selected", objects(lambda r: (part_types(r) == ["part", "part"], part_types(r))),
        pre=[call("part_change_type", parts=[[0, 1]], type="modifier"), call("object_select", parts=[[0, 1]])]),
    row("obj_rename", "two_cubes", objects(lambda r: (r.names()[0] == "Renamed by its action", r.names())),
        answers=[typed("Renaming", "wxTextCtrl", "Renamed by its action")]),
    row("obj_set_instances", "two_cubes", objects(lambda r: (len(r.objects()[0]["instances"]) == 3,
                                                             len(r.objects()[0]["instances"]))),
        answers=[typed("Copies of the selected object", "wxSpinCtrl", 3)]),
    row("obj_assemble", "two_cubes", objects(lambda r: (len(r.objects()) == 1 and len(r.objects()[0]["parts"]) == 2,
                                                        [len(o["parts"]) for o in r.objects()])),
        pre=[call("object_select", all=True)]),
    row("obj_instances_to_objects", "two_cubes", objects(lambda r: (len(r.objects()) == 3 and
                                                                    all(len(o["instances"]) == 1 for o in r.objects()), r.names())),
        pre=[call("object_set_instances", object=0, count=2), call("object_select", objects=[0])]),
    row("obj_order_by_name", "two_cubes", objects(lambda r: (r.names() == ["a_cube", "b_cube"], r.names())), pre=ORDER_NAMES),
    row("obj_order_by_filament", "two_cubes", objects(lambda r: ([o["filament"] for o in r.objects()] == [1, 2],
                                                                 [o["filament"] for o in r.objects()])), pre=ORDER_FILAMENTS),
    row("obj_order_by_height", "two_cubes", objects(lambda r: (heights(r) == [10, 20, 30], heights(r))), pre=ORDER_SIZES),
    row("obj_order_by_size", "two_cubes", objects(lambda r: (heights(r) == [30, 20, 10], heights(r))), pre=ORDER_SIZES),
    # ---- Add Primitive, Add Handy models ----
    *[row("add_primitive_" + shape, "empty", objects(lambda r, n=name: (len(r.objects()) == 1 and n in r.names()[0], r.names())),
          answers=[button(name, "ok")]) for shape, name in SHAPES.items()],
    row("add_primitive_text", "empty", app_state(lambda r: ((r.state().get("gizmo") or {}).get("name") == "Text" and
                                                            len(r.objects()) == 1, (r.state().get("gizmo"), r.names()))),
        post=[call("keyboard_press", keys="Escape", target=CANVAS)]),
    row("add_primitive_svg", "empty", picked(lambda r: r.objects() == []), answers=[picker("star.svg")],
        why=PATHLESS + " (the SVG gizmo reads the picker's GetPaths(), which a path answer leaves empty)"),
    *[row("add_handy_" + model, "empty", objects(lambda r, n=names: (r.names() == n, r.names())), answers=HANDY_ASKS.get(model, ()))
      for model, names in HANDY.items()],
    # ---- Plate ----
    row("plate_add", "two_cubes", plates(lambda r: ([p["objects"] for p in r.plates()] == [2, 0], [p["objects"] for p in r.plates()]))),
    row("plate_duplicate", "two_cubes", plates(lambda r: (len(r.plates()) == 2 and r.plates()[1]["objects"] == 2,
                                                          [p["objects"] for p in r.plates()]))),
    # The plate goes and its objects stay, off every plate (PartPlateList::delete_plate()).
    row("plate_delete", "two_plates", plates(lambda r: (len(r.plates()) == 1 and [o["plate"] for o in r.objects()] == [0, None],
                                                        (len(r.plates()), [o["plate"] for o in r.objects()])))),
    row("plate_rename", "two_cubes", plates(lambda r: (r.plates()[0]["name"] == "Plate by its action", r.plates()[0]["name"])),
        answers=[typed("Edit Plate Name", "TextInput", "Plate by its action")]),
    row("plate_toggle_lock", "two_cubes", plates(lambda r: (r.plates()[0]["locked"] is True, r.plates()[0]["locked"]))),
    row("plate_goto", "two_plates", plates(lambda r: (r.state()["plates"]["current"] == 0, r.state()["plates"]))),
    # ---- Printer, Filament, Presets ----
    row("sync_ams", "two_cubes", refused("no_printer", "no printer is connected, and the test connects to none")),
    row("filament_add", "empty", app_state(lambda r: (r.slots() == 2, r.slots()))),
    row("filament_delete", "empty", app_state(lambda r: (r.slots() == 1, r.slots())), pre=[TWO_SLOTS]),
    dialog_action("preset_bundle", "Preset Bundle"),
    row("sync_presets", "empty", refused("not_signed_in", "nothing is signed in, and the test signs in to no account")),
    # ---- Import ----
    row("import_file", "empty", picked(lambda r: r.objects() == []), answers=[picker("two_shells.stl")],
        why=PATHLESS + " (a multi-select picker)"),
    row("import_zip_archive", "empty", objects(lambda r: (len(r.objects()) == 1, r.names())),
        answers=[picker("two_shells.zip"), button("Archive preview", "ok")]),
    row("import_configs", "empty", picked(lambda r: IMPORTED_PRESET not in [
        i.get("name") for i in r.call("preset_list", type="filament", query=IMPORTED_PRESET).get("items", [])]),
        answers=[picker("mcp_import_pla.json")], why=PATHLESS + " (a multi-select picker)"),
    # ---- Export ----
    row("export_stl_multi", "two_cubes", written_each("stl_multi", ".stl", 2), answers=[picker("stl_multi")]),
    row("export_drc_single", "two_cubes", written("model.drc"), answers=[picker("model.drc")]),
    row("export_drc_multi", "two_cubes", written_each("drc_multi", ".drc", 2), answers=[picker("drc_multi")]),
    row("export_toolpaths_obj", "sliced", written("toolpaths.obj"), answers=[picker("toolpaths.obj")],
        pre=[call("view_set", tab="preview")]),
    row("export_config", "empty", window("Export Preset Bundle"), answers=[button("Export Preset Bundle", "cancel")],
        why="the dialog's kinds and presets are the person's to pick, so the test closes it"),
    # ---- Help ----
    dialog_action("help_keyboard_shortcuts", "Keyboard Shortcuts"),
    dialog_action("help_setup_wizard", "Setup Wizard", why="the wizard installs printers on Finish, so the test closes it"),
    row("help_open_config_folder", "empty", external("it opens the configuration folder in the system's file manager")),
    row("help_migrate_filament_presets", "empty", window("Migrate Filament Presets"),
        answers=[button("Migrate Filament Presets", "ok")]),
    dialog_action("help_troubleshoot", "Troubleshoot Center"),
    dialog_action("help_network_test", "Network Test", why="its tests reach the network, so the test closes it"),
    # Once Start is pressed the OrcaSliced Combo replaces the empty project, and the benchmark's own window, which times
    # the views and slices, stays open beside the main window (run_scene_benchmark()).
    row("help_benchmark_3d_scene", "empty", objects(lambda r: (lambda shown: (
        len(r.objects()) > 1 and any("Orca" in n for n in r.names()) and "Benchmark 3D Scene" in shown, (r.names(), shown)))(
        [w["title"] for w in r.call("window_list").get("items", []) if not w.get("modal")])),
        answers=[button("Benchmark 3D Scene", "Start")],
        post=[call("keyboard_press", keys="Escape", target={"label": "Benchmark 3D Scene"})]),
    row("help_tip_of_the_day", "empty", ("widget", lambda r: ("Daily Tips" in r.imgui_windows(), r.imgui_windows()))),
    row("help_check_updates", "empty", ("window", lambda r: (r.new_windows() != [], r.new_windows())),
        answers=[button("", "ok")], why="the dialog says whether a newer version exists, which depends on the day"),
    dialog_action("help_about", "About Orca Slicer"),
    row("open_wiki", "empty", external("it opens the wiki in the system's browser")),
    row("open_youtube", "empty", external("it opens the YouTube channel in the system's browser")),
    # ---- Plugins ----
    row("open_plugins", "empty", window("Plugins"), post=[call("keyboard_press", keys="Escape", target={"label": "Plugins"})]),
    # The side-loaded plugin is listed only once the refresh has found it.
    row("refresh_plugins", "empty", ("plugin_list", lambda r: (r.before is False and SIDE_LOADED in r.plugin_names(),
                                                               (r.before, r.plugin_names()))),
        before=lambda r: side_load(r)),
    row("install_plugin", "empty", external("it opens the plugin hub in the system's browser")),
    row("install_local_plugin", "empty", ("plugin_list", lambda r: (LOCAL_PLUGIN in r.plugin_names(), r.plugin_names())),
        answers=[picker("mcp_tools_fixture.py")]),
    # ---- Commands ----
    row("new_project", "two_cubes", objects(lambda r: (r.objects() == [], r.names())), answers=[button("Save", "no")]),
    row("save_project", "two_cubes", written("saved.3mf"), answers=[picker("saved.3mf")]),
    row("start_recording", "empty", app_state(lambda r: (lambda rec: (bool(rec) and os.path.dirname(rec.get("path", "")) == r.out,
                                                                      rec))(r.state().get("recording"))),
        answers=[button("Start Recording", "Start recording")], post=[call("recording_stop", silent=True)]),
    row("stop_recording", "recording", app_state(lambda r: (r.state().get("recording") is None and r.file("stopped.mkv") is not None,
                                                            (r.state().get("recording"), os.listdir(r.out))))),
])


def side_load(run):
    """Copies the web fixture plugin into the plugins folder, where only a refresh finds it."""
    dest = os.path.join(run.datadir, "orca_plugins", "mcp_web_fixture")
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(PLUGINS, "mcp_web_fixture", "mcp_web_fixture.py"), dest)
    return SIDE_LOADED in run.plugin_names()


def use_capture_folder(run):
    """Points Start Recording's folder, where Take Screenshot saves too, at the output folder: the dialog's Change...
    picker takes it, and a recording started and stopped there makes the dialog keep it."""
    run.call("dialog_auto_answer", rules=[RESTORE, {"match": ".", "button": "ok", "path": run.out}])
    run.call("recording_start")
    run.call("widget_click", target={"label": "Change...", "within": "Start Recording"})
    run.call("dialog_answer", dialog={"title": "Start Recording"}, button="Start recording")
    stopped = run.call("recording_stop", silent=True)
    run.call("dialog_auto_answer", rules=[RESTORE])
    return os.path.dirname(stopped.get("path", "")) == run.out, stopped


def journal_step(run, key):
    """The action_run step replay_export writes for the action of `key`, or None."""
    steps = run.call("replay_export", clear=True)["replay"]["steps"]
    return next((s for s in steps if s.get("tool") == "action_run" and s.get("args", {}).get("id") == action_id(key)), None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="output folder (default: a new temporary one)")
    ap.add_argument("--only", nargs="+", metavar="KEY", help="run these actions alone")
    args = ap.parse_args()
    out = os.path.abspath(args.out) if args.out else tempfile.mkdtemp(prefix="every-action-")
    os.makedirs(out, exist_ok=True)
    write_inputs(out)
    with open(os.path.join(DATA, "speed_dial_actions.json"), encoding="utf-8") as f:
        actions = json.load(f)
    with open(os.path.join(DATA, "speed_dial_action_calls.json"), encoding="utf-8") as f:
        fixture = {c["key"]: c for c in json.load(f)}

    client, session = orca_mcp.session_client()
    client.call("app_wait", **{"for": "ready"})
    run = Run(client, out, session["datadir"])
    checks, shown_otherwise = [], []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else f": {str(detail)[:600]}"), flush=True)

    keys = [a["key"] for a in actions]
    check("every action has a row", set(keys) <= set(ROWS), sorted(set(keys) - set(ROWS)))
    check("every row is an action", set(ROWS) <= set(keys), sorted(set(ROWS) - set(keys)))
    check("every action has a call in the fixture", set(keys) <= set(fixture), sorted(set(keys) - set(fixture)))
    commands, offset = {}, 0
    while True:
        page = client.call("action_search", group="", limit=200, offset=offset)
        commands.update({a["id"]: a.get("command") for a in page.get("items", [])})
        offset = page.get("next_offset")
        if not offset:
            break
    empty(run)
    check("Start Recording's folder is the output folder", *use_capture_folder(run))

    for key in [k for k in keys if not args.only or k in args.only]:
        r = ROWS.get(key)
        if r is None:
            continue
        name = f"{key} ({r['state']})"
        try:
            STATES[r["state"]](run)
            for tool, a in r["pre"]:
                run.wait(run.call(tool, **a))
            run.call("app_wait", **{"for": "idle"})
            run.before = r["before"](run) if r["before"] else None
            run.windows = [w["title"] for w in run.call("window_list").get("items", [])]
            run.seen = []
            pickers = [{"match": ".", "button": "ok", "path": run.path(a[1]), "overwrite": True}
                       for a in r["answers"] if a[0] == "picker"]
            run.call("dialog_auto_answer", rules=[RESTORE] + pickers)
            run.call("replay_export", clear=True)
            kind = r["expect"][0]
            try:
                first = run.call("action_run", id=action_id(key), **({"args": r["value"]} if r["value"] else {}))
            except orca_mcp.McpError as e:
                if kind == "refused":
                    code = json.loads(str(e).rpartition("\n")[2] or "{}").get("code")
                    check(f"{name} is refused as {r['expect'][1]}", code == r["expect"][1], str(e))
                    check(f"{name}: and the journal keeps no step for it", journal_step(run, key) is None)
                    if r["expect"][3]:
                        state = run.state()
                        check(f"{name}: refused before any dialog, with nothing uploaded",
                              not state.get("dialogs") and state.get("uploads") == [], (state.get("dialogs"), state.get("uploads")))
                    shown_otherwise.append((key, "refused", r["expect"][2]))
                else:
                    check(f"{name} runs", False, str(e))
                continue
            if kind == "refused":
                check(f"{name} is refused as {r['expect'][1]}", False, first)
                continue
            answer(run, r["answers"])
            cancelled = any(a[0] == "button" and a[-1] == "cancel" for a in r["answers"])
            try:
                run.reply = run.wait(first)
            except orca_mcp.McpError as e:
                run.reply = {"error": str(e)}
                if not cancelled:
                    check(f"{name} runs", False, str(e))
                    continue
            run.call("app_wait", **{"for": "idle", "timeout_s": 50})
            if kind == "external":
                check(f"{name} runs, handing over to another program", run.reply.get("ran") == action_id(key), run.reply)
                shown_otherwise.append((key, "external", r["expect"][1]))
            else:
                ok, detail = r["expect"][1](run)
                check(f"{name} does its row's job ({kind})", ok, detail)
                if not cancelled:
                    check(f"{name}: its reply names the action it ran", run.reply.get("ran") == action_id(key), run.reply)
            step = journal_step(run, key)
            spec = fixture.get(key, {})
            given = step.get("args", {}).get("args", {}) if step else None
            wanted = {spec["input"]: list(r["value"].values())[0]} if spec.get("input") else {}
            check(f"{name}: the journal keeps its action_run with the arguments its command journals",
                  step is not None and given == wanted, (step, wanted))
            check(f"{name}: its action names the fixture's command", commands.get(action_id(key)) == spec.get("command"),
                  (commands.get(action_id(key)), spec.get("command")))
            if r["why"]:
                shown_otherwise.append((key, kind, r["why"]))
        except orca_mcp.McpError as e:
            check(f"{name} runs", False, str(e))
        finally:
            for d in run.state().get("dialogs", []):
                try:
                    run.call("dialog_answer", button="close")
                except orca_mcp.McpError:
                    pass
            for tool, a in r["post"]:
                try:
                    run.wait(run.call(tool, **a))
                except orca_mcp.McpError as e:
                    print(f"  after {key}: {tool} failed: {e}", flush=True)
            if r["after"]:
                check(f"{name}: after its post steps", *r["after"](run))

    empty(run)
    print("\nActions shown by a refusal or as handing over, and rows that read less than a change of their own:")
    for key, kind, why in shown_otherwise:
        print(f"  {key}: {kind}: {why}")
    failed = [c for c in checks if not c[1]]
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
