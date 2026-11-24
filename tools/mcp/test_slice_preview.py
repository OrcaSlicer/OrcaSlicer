#!/usr/bin/env python3
"""Slicing, layers and the Preview, as the test_slice_preview entry of tools/mcp/test-entries/slice_preview.md designs
them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_slice_preview.py [scenario ...]

The plates the slicing calls and actions slice, an empty project and a plate that fails validation among them; the
statistics a slice reports; the Preview's tables, legend and layers, on a plate it shows, on one it has not shown and
on a G-code file opened for viewing; a legend row clicked, canvas captures taken right after a layer or view type
change, and view_set's refusals of the Preview's fields; height range modifiers, variable layer heights and a plate's
layer G-codes, each read in the exported G-code and kept by a 3MF; the commands that take a plate index; the display
switches and the sidebar of view_set; and app_state's account, network plug-in and configuration folder. After every
step the test reads the state back through another tool and compares it with the values the entry writes out. It runs
on the default printer, the Generic Klipper Printer 0.4 nozzle, in an app and on a data folder earlier tests used:
start() puts them in the baseline before its first scenario. It exits 0 when every check passes.
Naming scenarios (by the method names of SlicePreviewTest.SCENARIOS) runs those alone, each after the scenario it goes
on from where SlicePreviewTest.AFTER names one, for work on one of them.
"""

import json
import math
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (FLUSH_MULTIPLIER, GENERIC_PLA, KLIPPER, KLIPPER_STD, SLOT_COLOURS, Checks, add_slot,  # noqa: E402
                    filament_rows, obj_facts, run, slot_colours, start, until, wait)

ROW = re.compile(r'^"(.*)" (\d+)( \*)?( L)?( S)?$')
CUBE = {"shape": "cube", "size": [20, 20, 20]}
UNDO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures", "ops_undo.py")
SLICE_AND_PREVIEW = "orca_command:slice_and_preview:orca"
SLICE_ALL = "orca_command:slice_all:orca"
GO_TO_LAYER = "orca_command:go_to_layer:orca"
SPIRAL = "Spiral mode only works"
SAVE = "unsaved changes. Would you like to save"
SPIRAL_ERROR = 'Please select "By object" print sequence to print multiple objects in spiral vase mode.'
SELECT_PLATE = "select partplate!"
PLATE_STRING = 'plates is "all", "current" or a plate index'
# The Klipper process's travel speed, the top of the speed colour range while travel shows.
TRAVEL_SPEED = 350


def P(*rows):
    """Plates written as the entry writes them: '"" 1 * S' is an unnamed current plate with one object whose slice
    is up to date."""
    out = []
    for row in rows:
        m = ROW.match(row)
        assert m, row
        out.append({"name": m.group(1), "current": bool(m.group(3)), "locked": bool(m.group(4)),
                    "objects": int(m.group(2)), "sliced": bool(m.group(5))})
    return out


def seconds(text):
    """'1h 2m 3s' as seconds."""
    units = {"d": 86400, "h": 3600, "m": 60, "s": 1}
    return sum(int(n) * units[u] for n, u in re.findall(r"(\d+)\s*([dhms])", text))


def gcode(path):
    """An exported G-code file, as a printer outside the Bambu network has it written: its header (total layer
    number, max_z_height and filament, between HEADER_BLOCK_START and HEADER_BLOCK_END) with the estimate in seconds
    of the '; estimated printing time (normal mode) = ' line after the configuration block; and its layers, one per
    block a whole line ';LAYER_CHANGE' opens, each with z (';Z:'), height (the block's first ';HEIGHT:'), its tool
    lines, the lines after its ';PAUSE_PRINT' and ';CUSTOM_GCODE' lines, and per feature type (';TYPE:') the XY length
    of its extruding G1 moves."""
    header, layers = {}, []
    in_header = False
    layer = feature = after = None
    x = y = None
    relative = False
    move = re.compile(r"^G([0-3])(?:\s|$)")
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if after is not None:
                layer[after].append(line)
                after = None
            if line == "; HEADER_BLOCK_START":
                in_header = True
                continue
            if line == "; HEADER_BLOCK_END":
                in_header = False
                continue
            if in_header:
                if line.startswith("; total layer number:"):
                    header["total layer number"] = int(line.split(":", 1)[1])
                elif line.startswith("; max_z_height:"):
                    header["max_z_height"] = line.split(":", 1)[1].strip()
                elif line.startswith("; filament:"):
                    header["filament"] = line.split(":", 1)[1].strip()
                continue
            if line.startswith("; estimated printing time (normal mode) = "):
                header["estimate"] = seconds(line.split("=", 1)[1])
                continue
            if line == ";LAYER_CHANGE":
                layer = {"z": None, "height": None, "tools": [], "pauses": [], "customs": [], "features": {}}
                layers.append(layer)
                continue
            if line == "G90":
                relative = False
            elif line == "G91":
                relative = True
            if layer is None:
                m = move.match(line)
                if m:
                    x, y = position(line, x, y, relative)
                continue
            if line.startswith(";Z:"):
                layer["z"] = float(line.split(":", 1)[1])
            elif line.startswith(";HEIGHT:") and layer["height"] is None:
                # The layer's own height follows its LAYER_CHANGE; a bridge restates its own thickness later on.
                layer["height"] = float(line.split(":", 1)[1])
            elif line.startswith(";TYPE:"):
                feature = line.split(":", 1)[1].strip()
            elif line in ("T0", "T1"):
                layer["tools"].append(line)
            elif line == ";PAUSE_PRINT":
                after = "pauses"
            elif line == ";CUSTOM_GCODE":
                after = "customs"
            m = move.match(line)
            if m:
                nx, ny = position(line, x, y, relative)
                e = word(line, "E")
                if m.group(1) == "1" and feature is not None and e is not None and e > 0 and x is not None and \
                        (nx, ny) != (x, y):
                    layer["features"][feature] = layer["features"].get(feature, 0.0) + math.hypot(nx - x, ny - y)
                x, y = nx, ny
    return {"header": header, "layers": layers}


def word(line, letter):
    m = re.search(r"(?:^|\s)" + letter + r"(-?[\d.]+)", line.split(";", 1)[0])
    return float(m.group(1)) if m else None


def position(line, x, y, relative):
    nx, ny = word(line, "X"), word(line, "Y")
    if relative:
        return (x + nx if nx is not None and x is not None else x), (y + ny if ny is not None and y is not None else y)
    return (nx if nx is not None else x), (ny if ny is not None else y)


def toolpaths_z(path):
    """The lowest and highest vertex z of an OBJ."""
    zs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                zs.append(float(line.split()[3]))
    return (min(zs), max(zs)) if zs else (None, None)


class SlicePreviewTest:
    SCENARIOS = ["nothing", "routes", "not_shown", "statistics", "tables", "layers", "legend_click", "height_ranges",
                 "variable_height", "layer_gcode", "plate_index", "view_switches", "app_info"]
    # The scenarios that go on from the plates and slice the one before them leaves, which runs first when one of them
    # is named alone.
    AFTER = {"not_shown": "routes", "layers": "tables"}

    def __init__(self, client, session, folder):
        self.c = client
        self.t = Checks()
        self.session = session
        self.folder = folder
        self.files = 0
        # Plate 1's estimate in the slice the routes end with, which the plate the Preview has not shown goes on from.
        self.time_1 = None

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def action(self, id, args=None):
        return run(self.c, "action_run", id=id, **({"args": args} if args is not None else {}))

    def undo(self):
        self.run("undo", history="project")

    def redo(self):
        self.run("redo", history="project")

    def idle(self):
        wait(self.c, "idle")

    def path(self, ext):
        self.files += 1
        return os.path.join(self.folder, f"f{self.files}.{ext}")

    def plates(self):
        return [{"name": p["name"], "current": p["current"], "locked": p["locked"], "objects": p["objects"],
                 "sliced": p["sliced"]} for p in self.c.call("plate_list", limit=500)["items"]]

    def plates_are(self, label, *rows):
        self.t.equal(f"{label}: the plates", self.plates(), P(*rows))

    def objects(self):
        """(on, outside): the objects on the plates as {plate, name, filament, offset} sorted by plate and name, and
        the names of the objects outside every plate."""
        listing = self.c.call("object_list", limit=500, detail=True)
        on = sorted(({"plate": p["index"], "name": o["name"], "filament": o["filament"],
                      "offset": o["instances"][0]["offset"]} for p in listing["plates"] for o in p["objects"]),
                    key=lambda o: (o["plate"], o["name"]))
        return on, sorted(o["name"] for o in listing.get("outside", []))

    def objects_are(self, label, on, outside=()):
        """`on` as (plate, name, filament) triples."""
        got, out = self.objects()
        self.t.equal(f"{label}: the objects", ([(o["plate"], o["name"], o["filament"]) for o in got], out),
                     (sorted(on), sorted(outside)))
        return got

    def shown(self):
        state = self.c.call("app_state")
        preview = state.get("preview")
        if preview is not None:
            preview = {k: preview.get(k) for k in ("plate", "layers", "z", "layer_count", "view_type")}
        return {"tab": state.get("tab"), "current": state.get("plates", {}).get("current"), "preview": preview}

    def tab_is(self, label, tab):
        self.t.equal(f"{label}: the tab", self.shown()["tab"], tab)

    def preview_is(self, label, tab="preview", **fields):
        """The tab and the fields of app_state's preview named; preview=None for none."""
        got = self.shown()
        self.t.equal(f"{label}: the tab", got["tab"], tab)
        if "preview" in fields:
            self.t.equal(f"{label}: the Preview's layers", got["preview"], fields["preview"])
            return
        preview = got["preview"] or {}
        if got["preview"] is None:
            self.t.check(f"{label}: the Preview shows layers", False, "preview is null")
            return
        for key, want in fields.items():
            if key == "z":
                self.t.near(f"{label}: the Preview's z", preview.get("z"), want, 1e-4)
            else:
                self.t.equal(f"{label}: the Preview's {key}", preview.get(key), want)

    def layers_shown(self, label, first, last, **fields):
        """Shown layers first–last of a 0.2 mm cube, z 0.2·first to 0.2·last."""
        self.preview_is(label, layers=[first, last], z=[round(0.2 * first, 4), round(0.2 * last, 4)], **fields)

    def reply_layers(self, label, reply, layers, z=None):
        self.t.equal(f"{label}: the reply's layers", reply.get("layers"), layers)
        if z is not None:
            self.t.near(f"{label}: the reply's z", reply.get("z"), z, 1e-4)

    def slice_is(self, label, reply, outcomes, layers=100):
        """A slice's reply: status done, the plates' (index, outcome) in order, and every ok plate's layers, an
        exact count or (lowest, highest) with None for no bound."""
        self.t.equal(f"{label}: the slice is done", reply.get("status"), "done")
        self.t.equal(f"{label}: the slice's plates", [(p.get("index"), p.get("outcome")) for p in reply.get("plates", [])],
                     outcomes)
        for p in reply.get("plates", []):
            if p.get("outcome") != "ok":
                continue
            got = p.get("layers")
            if isinstance(layers, tuple):
                lo, hi = layers
                ok = isinstance(got, int) and (lo is None or got >= lo) and (hi is None or got <= hi)
                self.t.check(f"{label}: plate {p.get('index')}'s layers", ok, f"got {got}, expected {layers}")
            else:
                self.t.equal(f"{label}: plate {p.get('index')}'s layers", got, layers)

    def plate_of(self, reply, index):
        return next((p for p in reply.get("plates", []) if p.get("index") == index), {})

    def no_file(self, label, path):
        self.t.check(f"{label}: no file at the path", not os.path.exists(path), path)

    def export(self, label, path=None):
        path = path or self.path("gcode")
        reply = self.run("project_export", format="gcode", path=path, overwrite=True)
        self.t.equal(f"{label}: the export", reply.get("export"), "ok")
        return gcode(path)

    def snapshots(self):
        """The undo snapshots the app took since the last call, the plate selections' left out."""
        taken = self.c.call("script_run", filename=UNDO, code="import ops_undo\nresult = ops_undo.taken()")["result"]
        return [name for name in taken["taken"] if name != SELECT_PLATE]

    def slots(self, label, n):
        """Deletes filament slots from the last until one is left, then adds slots with add_slot until the project has
        n, slot k coloured SLOT_COLOURS[k-1], the colour a new app gives it: the purge between two slots, and so the
        prime tower's depth, follows their colours."""
        for _ in range(len(filament_rows(self.c)) - 1):
            self.run("filament_delete")
        for slot in range(2, n + 1):
            add_slot(self.c, SLOT_COLOURS[slot - 1])
        self.t.equal(f"{label}: {n} filament slots, coloured as a new app colours them",
                     [row.get("colour", "").upper() for row in filament_rows(self.c)],
                     [colour.upper() for colour in SLOT_COLOURS[:n]])

    def rule_uses(self, match):
        for rule in self.c.call("app_state").get("auto_answer_rules", []):
            if rule.get("match") == match:
                return rule.get("uses", 0)
        return None

    def cube(self, **args):
        return self.run("object_add_primitive", **CUBE, **args)

    def save_and_reopen(self, name):
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.run("project_open", path=path)
        self.idle()

    def start(self, name):
        self.run("project_new", discard_changes=True)
        self.run("view_set", tab="prepare")
        self.idle()
        self.plates_are(f"{name}: the start", '"" 0 *')
        self.preview_is(f"{name}: the start", tab="prepare", preview=None)

    # ---- Scenarios ----

    def nothing(self):
        s = "nothing to slice"
        self.start(s)
        words = self.t.refused(f"{s} 1: a layer range is refused", lambda: self.c.call("view_set", layer_range=[2, 5]),
                               code="no_layers")
        self.plates_are(f"{s} 1", '"" 0 *')
        self.preview_is(f"{s} 1", preview=None)
        self.t.refused(f"{s} 2: a height is refused", lambda: self.c.call("view_set", layer_z=5.0), code="no_layers",
                       words=words)
        self.t.refused(f"{s} 2: a share is refused", lambda: self.c.call("view_set", layer_percent=50),
                       code="no_layers", words=words)
        self.plates_are(f"{s} 2", '"" 0 *')
        self.tab_is(f"{s} 2", "preview")
        self.run("view_set", tab="prepare")
        reply = self.action(SLICE_AND_PREVIEW)
        self.slice_is(f"{s} 3", reply, [(0, "empty")])
        self.t.equal(f"{s} 3: the reply's action", reply.get("ran"), SLICE_AND_PREVIEW)
        self.plates_are(f"{s} 3", '"" 0 *')
        self.tab_is(f"{s} 3", "preview")
        self.slice_is(f"{s} 4", self.action(SLICE_ALL), [(0, "empty")])
        self.plates_are(f"{s} 4", '"" 0 *')
        self.slice_is(f"{s} 5: the current plate", self.run("plate_slice"), [(0, "empty")])
        self.slice_is(f"{s} 5: all plates", self.run("plate_slice", plates="all"), [(0, "empty")])
        self.plates_are(f"{s} 5", '"" 0 *')
        path = self.path("gcode")
        self.t.refused(f"{s} 6: the export is refused", lambda: self.c.call("project_export", format="gcode", path=path),
                       code="no_objects")
        self.plates_are(f"{s} 6", '"" 0 *')
        self.no_file(f"{s} 6", path)
        self.cube(position=[90, 90, 0])
        self.cube(position=[150, 90, 0])
        self.run("plate_set_settings", plate=0, values={"spiral_mode": True})
        self.plates_are(f"{s} 7", '"" 2 *')

        def failed(label, reply):
            self.slice_is(label, reply, [(0, "error")])
            error = self.plate_of(reply, 0).get("error") or ""
            self.t.check(f"{label}: the plate's error", SPIRAL_ERROR in error, error)

        failed(f"{s} 8", self.run("plate_slice"))
        self.t.equal(f"{s} 8: nothing slices", self.c.call("app_state").get("slicing"), False)
        self.plates_are(f"{s} 8", '"" 2 *')
        failed(f"{s} 9: Slice and Preview", self.action(SLICE_AND_PREVIEW))
        failed(f"{s} 9: Slice All Plates", self.action(SLICE_ALL))
        self.plates_are(f"{s} 9", '"" 2 *')
        self.t.refused(f"{s} 10: the export is refused", lambda: self.c.call("project_export", format="gcode", path=path),
                       code="slice_has_errors")
        sliced_file = self.path("gcode.3mf")
        self.t.refused(f"{s} 10: the sliced file is refused",
                       lambda: self.c.call("project_export", format="sliced_3mf", path=sliced_file),
                       code="sliced_file_not_written")
        self.plates_are(f"{s} 10", '"" 2 *')
        self.no_file(f"{s} 10: the G-code", path)
        self.no_file(f"{s} 10: the sliced file", sliced_file)
        for name, call in [("a layer", lambda: self.c.call("view_set", layer=10)),
                           ("a layer range", lambda: self.c.call("view_set", layer_range=[2, 5])),
                           ("a height", lambda: self.c.call("view_set", layer_z=5.0)),
                           ("Go to layer", lambda: self.c.call("action_run", id=GO_TO_LAYER, args={"percent": 50}))]:
            self.t.refused(f"{s} 11: {name} is refused", call, code="no_layers")
            self.plates_are(f"{s} 11: after {name}", '"" 2 *')
            self.preview_is(f"{s} 11: after {name}", preview=None)

    def routes(self):
        s = "slicing by plate and by route"
        self.start(s)
        self.cube(name="a")
        self.run("plate_add")
        item = self.c.call("plate_list", limit=500)["items"][1]
        middle = [item["origin"][0] + item["size"][0] / 2, item["origin"][1] + item["size"][1] / 2]
        self.cube(name="b", position=middle + [0])
        self.run("plate_select", plate=0)
        self.plates_are(f"{s} 0", '"" 1 *', '"" 1')
        got = self.objects_are(f"{s} 0", [(0, "a", 1), (1, "b", 1)])
        b = [o for o in got if o["name"] == "b"]
        self.t.near(f"{s} 0: b stands at plate 1's middle", b[0]["offset"][:2] if b else None, middle, 0.001)
        self.snapshots()
        self.slice_is(f"{s} 1", self.run("plate_slice"), [(0, "ok")])
        self.plates_are(f"{s} 1", '"" 1 * S', '"" 1')
        self.tab_is(f"{s} 1", "preview")
        self.t.equal(f"{s} 1: a slice of the current plate takes no snapshot", self.snapshots(), [])
        self.run("view_set", tab="prepare")
        self.slice_is(f"{s} 2", self.run("plate_slice", plates=1), [(1, "ok")])
        self.plates_are(f"{s} 2: the plate named is made current", '"" 1 S', '"" 1 * S')
        self.snapshots()
        self.run("object_transform", object=0, translate=[5, 0, 0])
        self.idle()
        self.plates_are(f"{s} 3: a move makes its own plate's slice out of date", '"" 1', '"" 1 * S')
        self.t.equal(f"{s} 3: the snapshots", self.snapshots(), ["Transform object"])
        self.run("plate_select", plate=0)
        path = self.path("gcode")
        self.snapshots()
        self.t.refused(f"{s} 4: the export is refused", lambda: self.c.call("project_export", format="gcode", path=path),
                       code="plate_not_sliced")
        self.plates_are(f"{s} 4", '"" 1 *', '"" 1 S')
        self.no_file(f"{s} 4", path)
        self.t.equal(f"{s} 4: a refused export takes no snapshot", self.snapshots(), [])
        self.t.refused(f"{s} 5: the statistics are refused", lambda: self.c.call("plate_slice_stats"),
                       words="Plate 0 has no up-to-date slice")
        self.plates_are(f"{s} 5", '"" 1 *', '"" 1 S')
        self.slice_is(f"{s} 6", self.run("plate_slice", plates="all"), [(0, "ok"), (1, "ok")])
        self.plates_are(f"{s} 6: Slice All leaves the last plate current", '"" 1 S', '"" 1 * S')
        self.snapshots()
        self.run("view_set", tab="prepare")
        self.run("object_transform", object=0, translate=[5, 0, 0])
        self.run("object_transform", object=1, translate=[5, 0, 0])
        self.idle()
        self.slice_is(f"{s} 7: the current plate alone", self.action(SLICE_AND_PREVIEW), [(1, "ok")])
        self.plates_are(f"{s} 7", '"" 1', '"" 1 * S')
        self.t.equal(f"{s} 7: the snapshots", self.snapshots(), ["Transform object", "Transform object"])
        reply = self.action(SLICE_ALL)
        self.slice_is(f"{s} 8", reply, [(0, "ok"), (1, "ok")])
        self.time_1 = self.plate_of(reply, 1).get("time_s")
        self.plates_are(f"{s} 8", '"" 1 S', '"" 1 * S')
        g = self.export(f"{s} 9")
        self.t.equal(f"{s} 9: the G-code's layers", (g["header"].get("total layer number"), len(g["layers"])), (100, 100))
        self.plates_are(f"{s} 9", '"" 1 S', '"" 1 * S')

    def statistics(self):
        s = "slice statistics"
        self.start(s)
        self.slots(s, 2)
        self.cube(name="left", position=[90, 90, 0])
        self.plates_are(f"{s}: the start", '"" 1 *')
        self.objects_are(f"{s}: the start", [(0, "left", 1)])
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 1", reply, [(0, "ok")])
        plate = self.plate_of(reply, 0)
        self.t.near(f"{s} 1: max_z", plate.get("max_z"), 20.0, 0.001)
        self.t.equal(f"{s} 1: filament changes", plate.get("filament_changes"), 0)
        self.t.check(f"{s} 1: no extruder changes", "extruder_changes" not in plate, plate)
        rows = plate.get("filaments", [])
        self.t.equal(f"{s} 1: the slots listed", [r.get("slot") for r in rows], [1, 2])
        if len(rows) == 2:
            one, two = rows
            self.t.check(f"{s} 1: slot 1's model grams", one["model_g"] > 0, one)
            self.t.equal(f"{s} 1: slot 1's support, flush and tower grams",
                         (one["support_g"], one["flush_g"], one["tower_g"]), (0, 0, 0))
            self.t.equal(f"{s} 1: slot 1's grams are its model's", one["g"], one["model_g"])
            self.t.equal(f"{s} 1: slot 2 prints nothing",
                         [two[k] for k in ("m", "g", "model_g", "support_g", "flush_g", "tower_g")], [0] * 6)
            self.t.equal(f"{s} 1: the grams are slot 1's", plate.get("filament_g"), one["g"])
        self.plates_are(f"{s} 1", '"" 1 * S')
        g = self.export(f"{s} 2")
        h = g["header"]
        self.t.equal(f"{s} 2: the header", (h.get("total layer number"), h.get("max_z_height"), h.get("filament")),
                     (100, "20.00", "1"))
        self.t.near(f"{s} 2: the estimate is the slice's", h.get("estimate"), plate.get("time_s"), 1)
        self.t.equal(f"{s} 2: the layers", len(g["layers"]), 100)
        self.t.check(f"{s} 2: layer n at z 0.2·n, 0.2 high",
                     all(abs(l["z"] - 0.2 * (n + 1)) < 1e-3 and abs(l["height"] - 0.2) < 1e-3
                         for n, l in enumerate(g["layers"])), [(l["z"], l["height"]) for l in g["layers"][:3]])
        self.t.equal(f"{s} 2: no tool line", [l["tools"] for l in g["layers"] if l["tools"]], [])
        self.plates_are(f"{s} 2", '"" 1 * S')
        self.cube(name="right", position=[150, 90, 0])
        reply = self.c.call("keyboard_press", target={"name": "OrcaSceneCanvas"}, keys="2")
        self.t.equal(f"{s} 3: the key goes to the canvas", reply.get("to"), "wxGLCanvas")
        self.idle()
        self.plates_are(f"{s} 3", '"" 2 *')
        self.objects_are(f"{s} 3", [(0, "left", 1), (0, "right", 2)])
        self.run("object_select", none=True)
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 4", reply, [(0, "ok")])
        plate = self.plate_of(reply, 0)
        self.t.equal(f"{s} 4: filament changes", plate.get("filament_changes"), 100)
        rows = plate.get("filaments", [])
        for r in rows:
            self.t.check(f"{s} 4: slot {r.get('slot')}'s model and tower grams", r["model_g"] > 0 and r["tower_g"] > 0, r)
            self.t.equal(f"{s} 4: slot {r.get('slot')}'s flush and support grams: the purge goes into the prime tower",
                         (r["flush_g"], r["support_g"]), (0, 0))
        self.t.near(f"{s} 4: the grams are the slots'", plate.get("filament_g"), sum(r["g"] for r in rows), 0.02)
        self.plates_are(f"{s} 4", '"" 2 * S')
        g = self.export(f"{s} 5")
        h = g["header"]
        self.t.equal(f"{s} 5: the header's slots, in either order, and layers",
                     (sorted((h.get("filament") or "").split(",")), h.get("total layer number")), (["1", "2"], 100))
        self.t.near(f"{s} 5: the estimate is the slice's", h.get("estimate"), plate.get("time_s"), 1)
        tools = [l["tools"] for l in g["layers"]]
        self.t.check(f"{s} 5: one tool line in each of the 100 layers",
                     len(tools) == 100 and all(len(t) == 1 for t in tools), tools)
        self.t.check(f"{s} 5: no two layers in a row on one tool",
                     all(a != b for a, b in zip(tools, tools[1:])), tools)
        stats = self.c.call("plate_slice_stats", include=["objects"])
        rows = stats.get("objects", [])
        self.t.equal(f"{s} 6: the objects' rows", [(r.get("name"), r.get("object"), r.get("instance")) for r in rows],
                     [("left", 0, 0), ("right", 1, 0)])
        self.t.check(f"{s} 6: each object's grams", all(r.get("g", 0) > 0 for r in rows), rows)
        self.plates_are(f"{s} 6", '"" 2 * S')

    def tables(self):
        s = "the Preview's tables"
        self.start(s)
        self.slots(s, 1)
        self.cube(name="cube")
        self.slice_is(f"{s}: the start", self.run("plate_slice"), [(0, "ok")])
        self.plates_are(f"{s}: the start", '"" 1 * S')
        reply = self.run("view_set", view_type="feature_type", legend={"travel": False}, layer_range=[1, 100])
        self.reply_layers(f"{s}: the start", reply, [1, 100, 100])

        st = self.c.call("plate_slice_stats", include=["features", "layers", "objects", "view"], limit=5)
        sm, ft, ly, ob, vw = (st.get(k) or {} for k in ("summary", "features", "layers", "objects", "view"))
        self.t.equal(f"{s} 1: the plate", st.get("plate"), 0)
        self.t.equal(f"{s} 1: summary layers", sm.get("layers"), 100)
        for key in ("filament_g", "travel_mm", "travel_time_s"):
            self.t.check(f"{s} 1: summary {key}", (sm.get(key) or 0) > 0, sm)
        wall = ft.get("Outer wall", {})
        self.t.check(f"{s} 1: Outer wall", wall.get("time_s", 0) > 0 and 0 < wall.get("percent", 0) < 100 and
                     wall.get("g", 0) > 0, wall)
        travel = ft.get("Travel", {})
        self.t.check(f"{s} 1: Travel's mm", travel.get("mm", 0) > 0, travel)
        self.t.near(f"{s} 1: Travel's time is the summary's", travel.get("time_s"), sm.get("travel_time_s"), 0.5)
        self.t.check(f"{s} 1: the shares add up to at most 101", sum(r.get("percent", 0) for r in ft.values()) <= 101,
                     {k: r.get("percent") for k, r in ft.items()})
        self.t.equal(f"{s} 1: the layers' paging", (ly.get("total"), ly.get("next_offset"), len(ly.get("items", []))),
                     (100, 5, 5))
        for i, item in enumerate(ly.get("items", [])):
            self.t.check(f"{s} 1: layer item {i}",
                         item.get("layer") == i + 1 and abs(item.get("z", 0) - 0.2 * (i + 1)) < 1e-6 and
                         abs(item.get("height", 0) - 0.2) < 1e-6 and item.get("time_s", 0) > 0 and item.get("g", 0) > 0,
                         item)
        self.t.equal(f"{s} 1: the objects", [(r.get("name"), r.get("object"), r.get("instance")) for r in ob],
                     [("cube", 0, 0)])
        self.t.check(f"{s} 1: the object's grams", bool(ob) and ob[0].get("g", 0) > 0, ob)
        self.t.equal(f"{s} 1: the view", (vw.get("view_type"), vw.get("time_mode"), vw.get("layers"),
                                          vw.get("layer_count")), ("feature_type", "normal", [1, 100], 100))
        legend = vw.get("legend", {})
        features, options = legend.get("features", {}), legend.get("options", {})
        self.t.equal(f"{s} 1: the legend", (features.get("Outer wall"), features.get("Sparse infill"),
                                            options.get("travel")), (True, True, False))
        ranges = vw.get("ranges", {})
        height = ranges.get("height", [None, None])
        self.t.check(f"{s} 1: the height range holds 0.2", height[0] <= 0.2 <= height[1], height)
        self.t.check(f"{s} 1: the flow and layer time ranges' tops",
                     ranges.get("volumetric_flow_rate", [0, 0])[1] > 0 and ranges.get("layer_time_linear", [0, 0])[1] > 0,
                     ranges)
        self.t.check(f"{s} 1: the speed range's top is below the travel speed",
                     ranges.get("speed", [0, TRAVEL_SPEED])[1] < TRAVEL_SPEED, ranges.get("speed"))

        st = self.c.call("plate_slice_stats", include=["layer_features"], offset=99)
        items = (st.get("layers") or {}).get("items", [])
        self.t.equal(f"{s} 2: one item", len(items), 1)
        if items:
            item = items[0]
            self.t.check(f"{s} 2: layer 100", item.get("layer") == 100 and abs(item.get("z", 0) - 20.0) < 1e-6 and
                         abs(item.get("height", 0) - 0.2) < 1e-6 and
                         item.get("features", {}).get("Top surface", 0) > 0, item)
        self.t.equal(f"{s} 2: no next page, features or view",
                     ["next_offset" in (st.get("layers") or {}), "features" in st, "view" in st], [False, False, False])

        reply = self.run("view_set", view_type="feature_type", legend={"Sparse infill": False, "travel": True}, layer_z=5.0)
        self.t.equal(f"{s} 3: the reply's legend", reply.get("legend"), {"Sparse infill": False, "travel": True})
        self.reply_layers(f"{s} 3", reply, [1, 25, 100], [0.2, 5.0])
        vw = self.c.call("plate_slice_stats", include=["view"]).get("view") or {}
        legend = vw.get("legend", {})
        self.t.equal(f"{s} 4: the legend", (legend.get("features", {}).get("Sparse infill"),
                                            legend.get("features", {}).get("Outer wall"),
                                            legend.get("options", {}).get("travel")), (False, True, True))
        self.t.equal(f"{s} 4: the layers", vw.get("layers"), [1, 25])
        self.t.equal(f"{s} 4: a travel shown counts in the speed range", (vw.get("ranges", {}).get("speed") or [0, 0])[1],
                     TRAVEL_SPEED)
        self.run("view_set", view_type="feature_type", legend={"travel": False})
        vw = self.c.call("plate_slice_stats", include=["view"]).get("view") or {}
        legend = vw.get("legend", {})
        self.t.equal(f"{s} 5: Line Type shows every feature type", (legend.get("features", {}).get("Sparse infill"),
                                                                     legend.get("options", {}).get("travel")),
                     (True, False))
        self.t.check(f"{s} 5: the speed range's top is below the travel speed",
                     (vw.get("ranges", {}).get("speed") or [0, TRAVEL_SPEED])[1] < TRAVEL_SPEED,
                     vw.get("ranges", {}).get("speed"))
        self.run("view_set", layer=5)
        self.layers_shown(f"{s} 6", 1, 5, plate=0, layer_count=100, view_type="feature_type")
        self.run("view_set", view_type="fan_speed")
        self.layers_shown(f"{s} 7", 1, 5, view_type="fan_speed")
        st = self.c.call("plate_slice_stats", include=["view"])
        vw = st.get("view") or {}
        self.t.equal(f"{s} 7: the statistics' view", (vw.get("view_type"), vw.get("layers"), "features" in st),
                     ("fan_speed", [1, 5], False))

    def not_shown(self):
        s = "a plate the Preview has not shown"
        # The routes' two plates, both sliced by Slice All Plates, plate 1 current.
        time_1 = self.time_1
        self.plates_are(f"{s}: the start", '"" 1 S', '"" 1 * S')
        self.run("view_set", tab="prepare")
        self.run("plate_select", plate=0)
        self.run("view_set", tab="preview")
        self.idle()
        self.plates_are(f"{s} 1", '"" 1 * S', '"" 1 S')
        self.preview_is(f"{s} 1", plate=0, layer_count=100)
        self.run("view_set", tab="prepare")
        self.run("plate_select", plate=1)
        self.plates_are(f"{s} 2", '"" 1 S', '"" 1 * S')
        self.preview_is(f"{s} 2", tab="prepare", preview=None)
        for step, args, note in [(3, {}, "has not loaded this plate's toolpaths"), (4, {"plate": 0}, "show plate 0 there")]:
            st = self.c.call("plate_slice_stats", **args)
            plate = args.get("plate", 1)
            self.t.equal(f"{s} {step}: the statistics", (st.get("plate"), (st.get("summary") or {}).get("layers"),
                                                         st.get("view")), (plate, 100, None))
            self.t.check(f"{s} {step}: the note", note in (st.get("note") or ""), st.get("note"))
            self.plates_are(f"{s} {step}", '"" 1 S', '"" 1 * S')
        self.run("view_set", tab="preview")
        self.idle()
        self.preview_is(f"{s} 5", plate=1)
        st = self.c.call("plate_slice_stats", include=["view"])
        self.t.equal(f"{s} 5: the statistics' view", ((st.get("view") or {}).get("layer_count"), "note" in st),
                     (100, False))
        self.plates_are(f"{s} 5", '"" 1 S', '"" 1 * S')
        path = self.path("gcode")
        g = self.export(f"{s} 6", path)
        self.t.equal(f"{s} 6: the header's layers", g["header"].get("total layer number"), 100)
        self.t.near(f"{s} 6: the estimate is plate 1's", g["header"].get("estimate"), time_1, 1)
        self.plates_are(f"{s} 6", '"" 1 S', '"" 1 * S')
        uses = self.rule_uses(SAVE)
        self.run("project_open", path=path, discard_changes=True)
        self.idle()
        self.plates_are(f"{s} 7: the plate shows the file without being sliced", '"" 0 *')
        self.t.equal(f"{s} 7: the save question was answered once", self.rule_uses(SAVE), (uses or 0) + 1)
        self.preview_is(f"{s} 7", layer_count=100)
        st = self.c.call("plate_slice_stats", include=["features", "layers", "view"])
        self.t.equal(f"{s} 7: the statistics' layers",
                     ((st.get("summary") or {}).get("layers"), (st.get("layers") or {}).get("total"),
                      (st.get("view") or {}).get("layer_count")), (100, 100, 100))
        self.t.near(f"{s} 7: the time is the file's", (st.get("summary") or {}).get("time_s"),
                    g["header"].get("estimate"), 1)
        self.t.check(f"{s} 7: Outer wall's time", ((st.get("features") or {}).get("Outer wall") or {}).get("time_s", 0) > 0,
                     st.get("features"))
        self.run("project_new", discard_changes=True)
        self.plates_are(f"{s} 8", '"" 0 *')
        self.t.equal(f"{s} 8: the Preview", self.shown()["preview"], None)

    def layers(self):
        s = "the Preview's layers"
        # The tables' cube, whose slice the Preview shows.
        self.plates_are(f"{s}: the start", '"" 1 * S')
        self.reply_layers(f"{s}: the start", self.run("view_set", layer_range=[1, 100]), [1, 100, 100])
        self.layers_shown(f"{s}: the start", 1, 100)

        def go(percent):
            return self.action(GO_TO_LAYER, {"percent": percent})

        for step, percent, last in [(1, 0, 1), (2, 50, 51), (3, 100, 100), (4, 25.5, 26)]:
            reply = go(percent)
            self.reply_layers(f"{s} {step}", reply, [1, last, 100], [0.2, round(0.2 * last, 4)])
            self.t.equal(f"{s} {step}: the reply's action", reply.get("ran"), GO_TO_LAYER)
            self.layers_shown(f"{s} {step}", 1, last)
        for step, args, words in [(5, {"percent": 150}, "layer_percent must be <= 100"),
                                  (6, {"percent": -5}, "layer_percent must be >= 0"),
                                  (7, {"percent": "abc"}, "layer_percent must be of type number"),
                                  (8, None, "takes exactly its percent")]:
            call_args = {"id": GO_TO_LAYER, **({"args": args} if args is not None else {})}
            self.t.refused(f"{s} {step}: refused", lambda: self.c.call("action_run", **call_args), words=words)
            self.layers_shown(f"{s} {step}", 1, 26)
        steps = [(9, lambda: self.run("view_set", layer_range=[20, 60]), [20, 60]),
                 (10, lambda: go(80), [20, 80]),
                 (11, lambda: go(10), [11, 11])]
        for step, call, (first, last) in steps:
            self.reply_layers(f"{s} {step}", call(), [first, last, 100])
            self.layers_shown(f"{s} {step}", first, last)
        self.run("view_set", layer=1)
        self.reply_layers(f"{s} 12", self.run("view_set", layer=50), [1, 50, 100])
        self.layers_shown(f"{s} 12: layer shows from layer 1", 1, 50)
        self.reply_layers(f"{s} 13", self.run("view_set", layer=9999), [1, 100, 100])
        self.layers_shown(f"{s} 13", 1, 100)
        self.reply_layers(f"{s} 14", self.run("view_set", layer_range=[5, 20]), [5, 20, 100])
        self.layers_shown(f"{s} 14", 5, 20)
        self.t.refused(f"{s} 15: refused", lambda: self.c.call("view_set", layer_range=[5]),
                       words="layer_range must be [first, last]")
        self.layers_shown(f"{s} 15", 5, 20)
        self.reply_layers(f"{s} 16", self.run("view_set", layer_z=5.0), [1, 25, 100], [0.2, 5.0])
        self.layers_shown(f"{s} 16", 1, 25)
        self.reply_layers(f"{s} 17", self.run("view_set", layer_range=[20, 25]), [20, 25, 100])
        path = self.path("obj")
        self.run("project_export", format="toolpaths_obj", path=path, overwrite=True)
        lo, hi = toolpaths_z(path)
        self.t.check(f"{s} 17: the toolpaths written are the layers shown",
                     lo is not None and lo >= 3.6 and hi <= 5.2 and hi >= 4.8, (lo, hi))
        self.layers_shown(f"{s} 17", 20, 25)
        self.reply_layers(f"{s} 18", self.run("view_set", layer_range=[1, 100]), [1, 100, 100])
        path = self.path("obj")
        reply = self.run("project_export", format="toolpaths_obj", path=path, overwrite=True)
        self.t.equal(f"{s} 18: the reply's bytes", reply.get("bytes"),
                     os.path.getsize(path) if os.path.exists(path) else "no file")
        mtl = os.path.splitext(os.path.basename(path))[0] + ".mtl"
        self.t.check(f"{s} 18: the material library is written beside it",
                     os.path.exists(os.path.join(self.folder, mtl)), mtl)
        facts = obj_facts(path)
        self.t.equal(f"{s} 18: the OBJ's first line and material library", (facts["first"], facts["mtllib"]),
                     ("# G-Code Toolpaths", f"mtllib ./{mtl}"))
        self.t.check(f"{s} 18: the OBJ has vertices and faces", facts["v"] > 0 and facts["f"] > 0, facts)
        lo, _ = toolpaths_z(path)
        self.t.check(f"{s} 18: the toolpaths start at the first layer", lo is not None and lo <= 0.2, lo)
        self.t.near(f"{s} 18: the OBJ's highest vertex, the top layer plus half its height", facts["top"], 20.1, 0.01)
        self.layers_shown(f"{s} 18", 1, 100)
        self.run("view_set", tab="prepare")
        self.reply_layers(f"{s} 19", self.run("view_set", layer=40), [1, 40, 100])
        self.layers_shown(f"{s} 19", 1, 40)
        self.run("view_set", tab="prepare")
        self.reply_layers(f"{s} 20", go(30), [1, 31, 100])
        self.layers_shown(f"{s} 20", 1, 31)
        self.run("project_new", discard_changes=True)
        self.plates_are(f"{s} 21: after project_new", '"" 0 *')
        self.t.refused(f"{s} 21: a layer is refused", lambda: self.c.call("view_set", layer=10), code="no_layers")
        self.t.refused(f"{s} 21: Go to layer is refused",
                       lambda: self.c.call("action_run", id=GO_TO_LAYER, args={"percent": 50}), code="no_layers")
        self.plates_are(f"{s} 21: after the refusals", '"" 0 *')
        self.preview_is(f"{s} 21", preview=None)

    def legend(self):
        return ((self.c.call("plate_slice_stats", include=["view"]).get("view") or {}).get("legend") or {}).get(
            "features", {})

    def legend_click(self):
        s = "a legend row clicked"
        self.start(s)
        self.slots(s, 1)
        self.t.refused(f"{s} 1: legend rows need a slice", lambda: self.c.call("view_set", legend={"travel": False}),
                       words="slice first")
        self.plates_are(f"{s} 1", '"" 0 *')
        self.preview_is(f"{s} 1", preview=None)
        self.cube()
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 2", reply, [(0, "ok")])
        self.t.check(f"{s} 2: the slice has no silent-mode estimate", "silent_time_s" not in self.plate_of(reply, 0),
                     self.plate_of(reply, 0))
        # The pick an earlier test or scenario left in the configuration is any view type, so the checked picks start
        # from Line Type.
        self.reply_layers(f"{s} 2", self.run("view_set", view_type="feature_type", layer_range=[1, 100]), [1, 100, 100])
        self.t.equal(f"{s} 2: Line Type is kept", self.conf("preview_last_view_type", "feature_type"), "feature_type")
        # The snapshot draws the legend first, since a window the desktop hides may not have drawn it since the slice.
        self.c.call("widget_list", query="Sparse infill")
        self.run("widget_click", target={"imgui_name": "Sparse infill"})
        self.t.equal(f"{s} 3: a click on its row hides Sparse infill", self.legend().get("Sparse infill"), False)
        self.run("view_set", view_type="speed")
        self.t.equal(f"{s} 4: the pick is kept", self.conf("preview_last_view_type", "speed"), "speed")
        self.t.equal(f"{s} 4: another view type keeps it hidden", self.legend().get("Sparse infill"), False)
        self.layers_shown(f"{s} 4", 1, 100, view_type="speed")
        self.run("view_set", view_type="feature_type")
        self.t.equal(f"{s} 5: Line Type shows it again", self.legend().get("Sparse infill"), True)
        self.t.equal(f"{s} 5: Line Type is kept", self.conf("preview_last_view_type", "feature_type"), "feature_type")

        # Each change is captured twice. One replay runs the calls back to back, which seldom leaves the app an idle
        # frame between a change and its first capture to draw the change before the capture does. The legend click
        # left ImGui's highlight on its row until the pointer moves, which on a busy machine can come between two
        # captures, so the pointer first moves to the empty space above the plate, as a person's does after a click.
        # The slice's notification goes first, so that its fade is not among the captures.
        self.slice_notice_gone()
        self.run("mouse_gesture", action="move", x=430, y=80)
        out_dir = os.path.join(self.folder, "captures")
        os.makedirs(out_dir, exist_ok=True)
        changes = [{"layer_range": [1, 40]}, {"view_type": "speed"}, {"layer_range": [64, 64]}, {"view_type": "height"},
                   {"layer_range": [20, 100]}, {"view_type": "fan_speed"}, {"layer_range": [1, 100]},
                   {"view_type": "feature_type"}]
        steps = []
        for i, change in enumerate(changes):
            steps.append({"tool": "view_set", "args": change})
            steps += [{"tool": "window_screenshot", "args": {"target": "canvas", "save_path": f"${{out_dir}}/{i}{shot}.png",
                                                              "overwrite": True, "include_image": False}}
                      for shot in "ab"]
        ran = self.run("replay_run", replay={"orca_replay_version": "0.2.0", "name": "captures after view changes",
                                             "steps": steps}, out_dir=out_dir)
        if self.t.check(f"{s} 6: the capture replay passes", ran.get("passed") is True, ran):
            def png(name):
                with open(os.path.join(out_dir, name), "rb") as f:
                    return f.read()
            a = [png(f"{i}a.png") for i in range(len(changes))]
            b = [png(f"{i}b.png") for i in range(len(changes))]
            stale = [c for c, first, second in zip(changes, a, b) if first != second]
            unchanged = [c for c, first, before in zip(changes[1:], a[1:], b) if first == before]
            self.t.equal(f"{s} 6: a capture right after a change shows it settled", stale, [])
            self.t.equal(f"{s} 6: every change shows in the capture", unchanged, [])
        self.layers_shown(f"{s} 6", 1, 100, view_type="feature_type")

        self.reply_layers(f"{s} 7", self.run("view_set", layer_z=1.05), [1, 6, 100], [0.2, 1.2])
        self.layers_shown(f"{s} 7: a height between two tops shows up to the upper one", 1, 6)
        self.reply_layers(f"{s} 8", self.run("view_set", layer_range=[60, 20]), [60, 60, 100], [12.0, 12.0])
        self.layers_shown(f"{s} 8: a range given backwards shows its first layer alone", 60, 60)
        # L switches the layer slider's one-layer mode, which start() does not reset: the second press comes whatever
        # step 9 meets.
        self.c.call("keyboard_press", keys="L")
        try:
            self.reply_layers(f"{s} 9", self.run("view_set", layer_range=[2, 5]), [2, 5, 100], [0.4, 0.4])
            self.layers_shown(f"{s} 9: one-layer mode shows the range's first layer", 2, 2)
        finally:
            self.c.call("keyboard_press", keys="L")
        self.layers_shown(f"{s} 10: leaving one-layer mode shows every layer", 1, 100, view_type="feature_type")

        refusals = [
            ("a height above the top layer", {"layer_z": 80}, ["above the top layer (20.0 mm)"]),
            ("layer with layer_z", {"layer": 3, "layer_z": 1.0}, ["not both"]),
            ("an unknown row, named with the print's rows", {"view_type": "speed", "legend": {"Infill": False}},
             ["Sparse infill", "travel"]),
            ("a value that is not true or false", {"legend": {"travel": "yes"}}, ["true (show) or false (hide)"]),
            ("silent mode without its own estimate", {"time_mode": "silent"}, ["no separate silent-mode estimate"]),
        ]
        for name, args, words in refusals:
            text = self.t.refused(f"{s} 11: {name} is refused", lambda: self.c.call("view_set", **args), words=words[0])
            for more in words[1:]:
                self.t.check(f"{s} 11: {name}: the refusal names {more}", text is not None and more in text, text)
            self.layers_shown(f"{s} 11: after {name}", 1, 100, view_type="feature_type")
            self.t.equal(f"{s} 11: after {name}: the legend", self.legend().get("Sparse infill"), True)
        self.t.equal(f"{s} 12: normal mode is always offered", self.run("view_set", time_mode="normal").get("time_mode"),
                     "normal")

    def ranges(self):
        objects = self.c.call("object_list", limit=500, detail=True)["plates"][0]["objects"]
        return objects[0].get("layer_config_ranges") if objects else "no object"

    def height_ranges(self):
        s = "height range modifiers"
        self.start(s)
        self.slots(s, 1)
        self.cube()
        self.t.equal(f"{s}: the cube has no ranges", self.ranges(), None)

        def R(lo, hi, **values):
            return {"min_z": lo, "max_z": hi, "values": values}

        after1 = [R(0, 10, layer_height="0.2", extruder="0"), R(10, 20, layer_height="0.2", extruder="0")]
        after3 = [R(0, 10, layer_height="0.2", extruder="0", sparse_infill_density="40%"),
                  R(10, 20, layer_height="0.1", extruder="0")]
        after4 = [after3[0], R(10, 20, layer_height="0.2", extruder="0")]

        def ranges_are(label, want):
            self.t.equal(f"{label}: the ranges", self.ranges(), want)

        reply = self.run("object_set_height_ranges", object=0, ranges=[{"min_z": 10, "max_z": 20}, {"min_z": 0, "max_z": 10}])
        self.t.near(f"{s} 1: the reply's height", reply.get("height"), 20, 0.01)
        self.t.equal(f"{s} 1: the reply's ranges, sorted", reply.get("layer_config_ranges"), after1)
        self.t.check(f"{s} 1: no warnings", "warnings" not in reply, reply.get("warnings"))
        ranges_are(f"{s} 1", after1)

        def on_range(lo, hi, values):
            return self.c.call("settings_set", target="height_range", object=0, min_z=lo, max_z=hi, values=values)

        reply = on_range(0, 10, {"sparse_infill_density": "40%"})
        self.t.equal(f"{s} 2: the reply", (reply.get("applied", {}).get("sparse_infill_density"), reply.get("height_range")),
                     ("40%", {"min_z": 0, "max_z": 10}))
        ranges_are(f"{s} 2", [after3[0], after1[1]])
        item = self.c.call("settings_get", target="object", object=0, keys=["sparse_infill_density"])["items"][0]
        self.t.equal(f"{s} 2: a range's setting is not the object's", item.get("value"), None)
        reply = on_range(10, 20, {"layer_height": 0.1})
        self.t.equal(f"{s} 3: the reply", (reply.get("applied", {}).get("layer_height"), reply.get("height_range")),
                     ("0.1", {"min_z": 10, "max_z": 20}))
        ranges_are(f"{s} 3", after3)
        self.undo()
        ranges_are(f"{s} 4: one step each", after4)
        self.redo()
        self.run("object_set_height_ranges", object=0, ranges=[{"min_z": 10, "max_z": 20}, {"min_z": 0, "max_z": 10}])
        ranges_are(f"{s} 5: the ranges a call names again keep their settings", after3)
        self.undo()
        ranges_are(f"{s} 6: the same ranges took no undo step", after4)
        self.redo()
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 7", reply, [(0, "ok")], layers=(149, 151))
        count = self.plate_of(reply, 0).get("layers")
        ranges_are(f"{s} 7", after3)
        g = self.export(f"{s} 8")
        layers = g["layers"]
        self.t.check(f"{s} 8: 0.2 mm layers up to 10 mm",
                     all(abs(l["height"] - 0.2) < 1e-3 for l in layers if l["z"] <= 10.0 + 1e-3),
                     [(l["z"], l["height"]) for l in layers if l["z"] <= 10.0 + 1e-3 and abs(l["height"] - 0.2) >= 1e-3])
        self.t.check(f"{s} 8: 0.1 mm layers from 10.2 mm",
                     all(abs(l["height"] - 0.1) < 1e-3 for l in layers if l["z"] >= 10.2 - 1e-3),
                     [(l["z"], l["height"]) for l in layers if l["z"] >= 10.2 - 1e-3 and abs(l["height"] - 0.1) >= 1e-3])
        self.t.equal(f"{s} 8: as many layers as the slice", len(layers), count)
        at6 = [l for l in layers if abs(l["z"] - 6.0) < 1e-3]
        # Compared with step 18's control, the same cube sliced without ranges.
        dense = at6[0]["features"].get("Sparse infill", 0) if at6 and layers[29:30] == at6[:1] else 0
        ranges_are(f"{s} 8", after3)
        refusals = [
            (9, lambda: self.c.call("object_set_height_ranges", object=0,
                                    ranges=[{"min_z": 0, "max_z": 15}, {"min_z": 12, "max_z": 20}]), "overlap"),
            (10, lambda: on_range(0, 10, {"layer_height": 0.5}), "layer_height 0.5 is outside this printer's 0.08-0.32"),
            (11, lambda: on_range(0, 10, {"extruder": 9}), "Filament 9 does not exist"),
            (12, lambda: on_range(0, 10, {"enable_support": True}), "not on a height range"),
            (13, lambda: self.c.call("object_set_height_ranges", object=0, ranges=[{"min_z": 5, "max_z": 5}]),
             "A height range needs 0 <= min_z < max_z"),
        ]
        for step, call, words in refusals:
            self.t.refused(f"{s} {step}: refused", call, words=words)
            ranges_are(f"{s} {step}", after3)
        self.slots(s, 2)
        after14 = [after3[0], R(10, 20, layer_height="0.1", extruder="2")]
        reply = on_range(10, 20, {"extruder": 2})
        self.t.equal(f"{s} 14: the reply", (reply.get("applied", {}).get("extruder"), reply.get("height_range")),
                     ("2", {"min_z": 10, "max_z": 20}))
        # A tower worked out for the range's 0.1 mm layers is too deep to fit behind its default place on this bed.
        self.run("project_set_settings", values={"wipe_tower_y": {"index": 0, "value": 150}})
        self.t.equal(f"{s} 14: the prime tower moved forward", self.c.call("plate_list", limit=500)["items"][0].get(
            "wipe_tower"), [165, 150])
        ranges_are(f"{s} 14", after14)
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 15", reply, [(0, "ok")], layers=(149, 151))
        plate = self.plate_of(reply, 0)
        rows = plate.get("filaments", [])
        self.t.check(f"{s} 15: one filament change, to slot 2",
                     plate.get("filament_changes") == 1 and len(rows) == 2 and rows[1].get("model_g", 0) > 0, plate)
        g = self.export(f"{s} 15")
        tools = [(round(l["z"], 3), t) for l in g["layers"] for t in l["tools"]]
        self.t.equal(f"{s} 15: one tool line, T1, in the first layer above 10 mm", tools, [(10.1, "T1")])
        ranges_are(f"{s} 15", after14)
        self.save_and_reopen("ranges.3mf")
        ranges_are(f"{s} 16: the project file keeps them", after14)
        reply = self.run("object_set_height_ranges", object=0, ranges=[])
        self.t.equal(f"{s} 17: the reply", reply.get("layer_config_ranges"), [])
        ranges_are(f"{s} 17", None)
        self.slice_is(f"{s} 18", self.run("plate_slice"), [(0, "ok")])
        g = self.export(f"{s} 18")
        layers = g["layers"]
        self.t.check(f"{s} 18: 100 layers of 0.2 mm", len(layers) == 100 and all(abs(l["height"] - 0.2) < 1e-3 for l in layers),
                     len(layers))
        self.t.equal(f"{s} 18: no tool line", [l["tools"] for l in layers if l["tools"]], [])
        L = layers[29]["features"].get("Sparse infill", 0) if len(layers) >= 30 else 0
        self.t.check(f"{s} 18: the control's layer 30 has sparse infill", L > 0, L)
        self.t.check(f"{s} 8: layer 30 has 40 % infill", L > 0 and dense >= 1.8 * L, (dense, L))
        ranges_are(f"{s} 18", None)

    def profile(self):
        objects = self.c.call("object_list", limit=500, detail=True)["plates"][0]["objects"]
        return objects[0].get("layer_height_profile") if objects else "no object"

    def variable_height(self):
        s = "variable layer height"
        self.start(s)
        self.slots(s, 1)
        self.run("object_add_handy_model", model="3dbenchy")
        self.slice_is(f"{s}: the start", self.run("plate_slice"), [(0, "ok")], layers=240)
        self.t.equal(f"{s}: the Benchy has no profile", self.profile(), None)

        def vlh(**args):
            return self.run("object_set_variable_layer_height", object=0, **args)

        def profile_is(label, **want):
            got = self.profile()
            if not want:
                self.t.equal(f"{label}: no profile", got, None)
                return got
            if not isinstance(got, dict):
                self.t.check(f"{label}: a profile", False, got)
                return {}
            for key, test in want.items():
                ok = test(got.get(key)) if callable(test) else got.get(key) == test
                self.t.check(f"{label}: the profile's {key}", ok, got)
            return got

        reply = vlh(action="adaptive")
        self.t.near(f"{s} 1: the reply's height", reply.get("height"), 48, 0.01)
        self.t.check(f"{s} 1: no warnings", "warnings" not in reply, reply.get("warnings"))
        got = profile_is(f"{s} 1", points=lambda v: v > 250, min_height=lambda v: 0.08 <= v < 0.2,
                         max_height=lambda v: 0.2 < v <= 0.32)
        points = got.get("points", 0)
        self.tab_is(f"{s} 1", "prepare")
        vlh(action="adaptive", quality=0)
        profile_is(f"{s} 2", points=lambda v: v > points)
        vlh(action="adaptive", quality=1)
        profile_is(f"{s} 3", points=lambda v: v < points, max_height=0.32)
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 4", reply, [(0, "ok")], layers=(None, None))
        count = self.plate_of(reply, 0).get("layers")
        self.t.check(f"{s} 4: a layer count other than 240", count != 240, count)
        g = self.export(f"{s} 4")
        heights = [l["height"] for l in g["layers"]]
        self.t.equal(f"{s} 4: as many layers as the slice", len(heights), count)
        self.t.check(f"{s} 4: heights from 0.08 to 0.32", all(0.08 - 1e-3 <= h <= 0.32 + 1e-3 for h in heights),
                     sorted(set(heights)))
        self.t.check(f"{s} 4: more than one height", len(set(heights)) > 1, sorted(set(heights)))
        self.t.near(f"{s} 4: the last layer's z", g["layers"][-1]["z"] if g["layers"] else None, 48, 0.3)
        profile_is(f"{s} 4", points=lambda v: v < points, max_height=0.32)
        vlh(action="smooth", radius=10, keep_min=True)
        profile_is(f"{s} 5", max_height=lambda v: v < 0.32)
        self.undo()
        profile_is(f"{s} 6: smooth is one undo step", max_height=0.32)
        self.redo()
        reply = vlh(action="reset")
        self.t.check(f"{s} 7: the reply has no profile", "layer_height_profile" not in reply, reply)
        profile_is(f"{s} 7")
        vlh(action="reset")
        self.undo()
        profile_is(f"{s} 8: the second reset took no step", max_height=lambda v: v < 0.32)
        vlh(action="reset")
        profile_is(f"{s} 9")
        self.t.refused(f"{s} 10: refused", lambda: self.c.call("object_set_variable_layer_height", object=0,
                                                               action="smooth", quality=0.3),
                       words="quality goes with action adaptive")
        profile_is(f"{s} 10")
        self.run("view_set", canvas="assemble")
        self.t.refused(f"{s} 11: refused", lambda: self.c.call("object_set_variable_layer_height", object=0,
                                                               action="adaptive"),
                       words="Leave the assemble view first")
        self.run("view_set", canvas="3d")
        profile_is(f"{s} 11")
        self.run("object_set_height_ranges", object=0, ranges=[{"min_z": 0, "max_z": 20}, {"min_z": 20, "max_z": 48}])
        self.run("settings_set", target="height_range", object=0, min_z=0, max_z=20, values={"layer_height": 0.12})
        self.run("settings_set", target="height_range", object=0, min_z=20, max_z=48, values={"layer_height": 0.28})
        ranges = [{"min_z": 0, "max_z": 20, "values": {"layer_height": "0.12", "extruder": "0"}},
                  {"min_z": 20, "max_z": 48, "values": {"layer_height": "0.28", "extruder": "0"}}]
        self.t.equal(f"{s} 12: the ranges", self.ranges(), ranges)
        profile_is(f"{s} 12")
        reply = vlh(action="smooth")
        self.t.check(f"{s} 13: the reply warns", any("decides its layer heights" in w for w in reply.get("warnings", [])),
                     reply.get("warnings"))
        profile_is(f"{s} 13: smooth starts from the ranges' heights", min_height=0.12, max_height=0.28)
        vlh(action="adaptive", quality=0)
        profile_is(f"{s} 14", min_height=0.12)
        self.t.equal(f"{s} 14: the ranges stay", self.ranges(), ranges)
        self.save_and_reopen("profile.3mf")
        profile_is(f"{s} 15: the project file keeps the profile", min_height=0.12, points=lambda v: v > 100)
        reply = self.run("plate_slice")
        self.slice_is(f"{s} 16", reply, [(0, "ok")], layers=(172, None))
        count = self.plate_of(reply, 0).get("layers")
        g = self.export(f"{s} 16")
        heights = [l["height"] for l in g["layers"]]
        self.t.equal(f"{s} 16: as many layers as the slice", len(heights), count)
        self.t.check(f"{s} 16: heights from 0.12 to at most 0.28",
                     bool(heights) and abs(min(heights) - 0.12) < 1e-3 and max(heights) <= 0.28 + 1e-3,
                     sorted(set(heights)))
        profile_is(f"{s} 16", min_height=0.12, points=lambda v: v > 100)

    def codes(self, label, want, layers=False, reply=None):
        """plate 0's layer_gcode from plate_list, compared whole with `want` (without layer unless `layers`), and a
        reply's codes, with layer."""
        def strip(codes):
            return [{k: v for k, v in c.items() if k != "color"} for c in codes]
        got = self.c.call("plate_list", limit=500)["items"][0]["layer_gcode"]
        expected = want if layers else [{k: v for k, v in c.items() if k != "layer"} for c in want]
        self.t.equal(f"{label}: the plate's codes", strip(got), expected)
        if reply is not None:
            self.t.equal(f"{label}: the reply's codes", strip(reply.get("layer_gcode", [])), want)
        return got

    def layer_gcode(self):
        s = "layer G-codes"
        self.start(s)
        self.slots(s, 2)
        self.cube()

        def C(layer, type, z, gcode=None, filament=None):
            code = {"layer": layer, "type": type, "z": z}
            if gcode is not None:
                code["gcode"] = gcode
            if filament is not None:
                code["filament"] = filament
            return code

        def lg(**args):
            return self.c.call("plate_set_custom_gcode", **args)

        def not_slicing(label, tab):
            state = self.c.call("app_state")
            self.t.equal(f"{label}: the tab and slicing", (state.get("tab"), state.get("slicing")), (tab, False))

        self.t.refused(f"{s} 1: refused", lambda: lg(add=[{"layer": 26, "type": "pause"}]),
                       words="has no up-to-date slice to place codes on")
        not_slicing(f"{s} 1: a refused call starts no slice", "prepare")
        self.codes(f"{s} 1", [])
        self.slice_is(f"{s} 2", self.run("plate_slice"), [(0, "ok")])
        self.run("view_set", tab="prepare")
        self.codes(f"{s} 2", [])
        after3 = [C(26, "pause", 5.2), C(51, "custom", 10.2, "M104 S215")]
        reply = lg(add=[{"layer": 26, "type": "pause"}, {"z": 10.1, "type": "custom", "gcode": "M104 S215"}])
        self.idle()
        self.t.equal(f"{s} 3: the reply", (reply.get("plate"), reply.get("layers"), reply.get("changed")), (0, 100, True))
        self.t.check(f"{s} 3: the reply's note", "slice it again" in (reply.get("note") or ""), reply.get("note"))
        self.codes(f"{s} 3", after3, reply=reply)
        state = self.c.call("app_state")
        self.t.equal(f"{s} 3: the tab and the project", (state.get("tab"), state.get("project", {}).get("dirty")),
                     ("preview", True))
        self.plates_are(f"{s} 3: a code makes the slice out of date", '"" 1 *')
        reply = lg(add=[{"z": 5.2, "type": "pause"}])
        self.t.equal(f"{s} 4: the plate stays editable while the Preview shows it", reply.get("changed"), False)
        self.codes(f"{s} 4", after3, reply=reply)
        after5 = [C(51, "custom", 10.2, "M104 S220")]
        reply = lg(remove=[{"z": 5.2}], add=[{"layer": 51, "type": "custom", "gcode": "M104 S220"}])
        self.t.equal(f"{s} 5: changed", reply.get("changed"), True)
        self.codes(f"{s} 5", after5, reply=reply)
        refusals = [
            (6, {"add": [{"layer": 101, "type": "pause"}]}, "layer must be 1..100"),
            (7, {"add": [{"layer": 51, "type": "pause"}]}, "layer 51 has a custom code; remove it first"),
            (8, {"remove": [{"layer": 30}]}, "No code on layer 30"),
            (9, {"add": [{"layer": 30, "type": "change_filament", "filament": 9}]}, "filament must be 1..2"),
            (10, {"plate": 1, "add": [{"layer": 30, "type": "pause"}]},
             "Plate 1 does not exist (the project has plate 0 only)"),
        ]
        for step, args, words in refusals:
            self.t.refused(f"{s} {step}: refused", lambda: lg(**args), words=words)
            self.codes(f"{s} {step}", after5)
        self.run("view_set", tab="prepare")
        self.t.refused(f"{s} 11: refused", lambda: lg(add=[{"layer": 30, "type": "pause"}]),
                       words="has no up-to-date slice to place codes on")
        not_slicing(f"{s} 11", "prepare")
        self.codes(f"{s} 11", after5)
        self.slice_is(f"{s} 12", self.run("plate_slice"), [(0, "ok")])
        after12 = [C(26, "pause", 5.2), C(51, "custom", 10.2, "M104 S220"), C(76, "change_filament", 15.2, filament=2)]
        reply = lg(add=[{"layer": 26, "type": "pause"}, {"layer": 76, "type": "change_filament", "filament": 2}])
        got = self.codes(f"{s} 12", after12, reply=reply)
        colours = slot_colours(self.c).split(";")
        change = [c for c in got if c.get("type") == "change_filament"]
        self.t.equal(f"{s} 12: the filament change's colour is slot 2's", change[0].get("color") if change else None,
                     colours[1] if len(colours) > 1 else colours)
        self.save_and_reopen("codes.3mf")
        self.codes(f"{s} 13: the project file keeps them", after12)
        self.plates_are(f"{s} 13: the plate is not sliced", '"" 1 *')
        self.slice_is(f"{s} 14", self.run("plate_slice"), [(0, "ok")])
        self.codes(f"{s} 14", after12, layers=True)
        g = self.export(f"{s} 14")
        layers = g["layers"]
        self.t.equal(f"{s} 14: the pauses", [(n + 1, l["pauses"]) for n, l in enumerate(layers) if l["pauses"]],
                     [(26, ["PAUSE"])])
        self.t.equal(f"{s} 14: the custom G-codes", [(n + 1, l["customs"]) for n, l in enumerate(layers) if l["customs"]],
                     [(51, ["M104 S220"])])
        self.t.equal(f"{s} 14: the tool lines", [(n + 1, l["tools"]) for n, l in enumerate(layers) if l["tools"]],
                     [(76, ["T1"])])
        reply = lg(clear=True)
        self.t.equal(f"{s} 15: changed", reply.get("changed"), True)
        self.codes(f"{s} 15", [], reply=reply)
        self.slice_is(f"{s} 16", self.run("plate_slice"), [(0, "ok")])
        g = self.export(f"{s} 16")
        self.t.equal(f"{s} 16: no pause, custom G-code or tool line",
                     [n + 1 for n, l in enumerate(g["layers"]) if l["pauses"] or l["customs"] or l["tools"]], [])
        self.codes(f"{s} 16", [])

    def plate_index(self):
        s = "a plate index"
        self.start(s)
        self.cube()
        self.run("plate_add")
        self.run("plate_add")
        self.run("plate_select", plate=1)
        start = ('"" 1', '"" 0 *', '"" 0')
        self.plates_are(f"{s}: the start", *start)
        self.snapshots()
        listing = self.c.call("object_list", limit=500, detail=True)
        O = listing["plates"][0]["objects"][0]["instances"][0]["offset"]
        path = self.path("stl")

        def unchanged(label):
            self.plates_are(label, *start)
            got, out = self.objects()
            self.t.equal(f"{label}: the cube stays", ([(o["plate"], o["name"]) for o in got], out), ([(0, "Cube")], []))
            self.t.near(f"{label}: the cube's offset", got[0]["offset"] if got else None, O, 1e-6)
            self.tab_is(label, "prepare")
            self.no_file(label, path)

        def missing(plate):
            return f"Plate {plate} does not exist (the project has plates 0..2)"

        for plate in (5, -1):
            self.t.refused(f"{s} 1: plate_slice of plate {plate}", lambda: self.c.call("plate_slice", plates=plate),
                           code="no_such_plate", words=missing(plate))
            unchanged(f"{s} 1: plate {plate}")
        for plate in (5, -1):
            self.t.refused(f"{s} 2: the export of plate {plate}",
                           lambda: self.c.call("project_export", format="stl", path=path, plates=plate),
                           code="no_such_plate", words=missing(plate))
            unchanged(f"{s} 2: plate {plate}")
        # A G-code export checks the plate itself, before the checks it makes on the plate named.
        gcode_path = self.path("gcode")
        self.t.refused(f"{s} 2: the G-code export of plate 5",
                       lambda: self.c.call("project_export", format="gcode", path=gcode_path, plates=5),
                       code="no_such_plate", words=missing(5))
        unchanged(f"{s} 2: the G-code export")
        self.no_file(f"{s} 2: the G-code export", gcode_path)
        for plate in (5, -1):
            self.t.refused(f"{s} 3: the arrange of plate {plate}", lambda: self.c.call("object_arrange", plates=plate),
                           words=missing(plate))
            unchanged(f"{s} 3: plate {plate}")
        for name, call in [("plate_slice", lambda: self.c.call("plate_slice", plates="abc")),
                           ("project_export", lambda: self.c.call("project_export", format="stl", path=path, plates="abc")),
                           ("object_arrange", lambda: self.c.call("object_arrange", plates="abc"))]:
            self.t.refused(f"{s} 4: {name} of \"abc\"", call, words=PLATE_STRING)
            unchanged(f"{s} 4: {name}")
        refusals = [
            (5, "plate_get_settings", lambda: self.c.call("plate_get_settings", plate=5, keys=["curr_bed_type"]),
             missing(5)),
            (6, "object_list", lambda: self.c.call("object_list", plate=5), missing(5)),
            (7, "object_transform", lambda: self.c.call("object_transform", object=0, plate=5), missing(5)),
            (8, "plate_slice_stats", lambda: self.c.call("plate_slice_stats", plate=5), missing(5)),
            (9, "plate_set_custom_gcode",
             lambda: self.c.call("plate_set_custom_gcode", plate=5, add=[{"layer": 1, "type": "pause"}]), missing(5)),
        ]
        for step, name, call, words in refusals:
            text = self.t.refused(f"{s} {step}: {name}", call, words=words)
            if step == 7:
                self.t.check(f"{s} 7: the refusal says how to add a plate",
                             text is not None and "action_run plate_add makes one" in text, text)
            unchanged(f"{s} {step}")
        self.t.equal(f"{s} 10: no refused call took an undo snapshot", self.snapshots(), [])
        unchanged(f"{s} 10")
        reply = self.run("project_export", format="stl", path=path, plates=2)
        self.t.check(f"{s} 11: written", reply.get("bytes", 0) > 0 and os.path.exists(path), reply)
        after11 = ('"" 1', '"" 0', '"" 0 *')
        self.plates_are(f"{s} 11: the plate named is made current", *after11)
        reply = self.c.call("plate_get_settings", plate=0)
        self.t.equal(f"{s} 12: the reply's plate", reply.get("plate"), 0)
        self.plates_are(f"{s} 12: reading a plate leaves the current one", *after11)
        reply = self.c.call("object_list", plate=0, detail=True)
        self.t.equal(f"{s} 13: plate 0 alone", ([p["index"] for p in reply.get("plates", [])],
                                               [o["name"] for p in reply.get("plates", []) for o in p["objects"]],
                                               "outside" in reply), ([0], ["Cube"], False))
        self.plates_are(f"{s} 13", *after11)
        self.run("object_arrange", plates=0)
        self.idle()
        self.plates_are(f"{s} 14: the plate arranged is made current", '"" 1 *', '"" 0', '"" 0')
        items = self.c.call("plate_list", limit=500)["items"]
        (x0, y0), (w, d) = items[0]["origin"], items[0]["size"]
        box = self.c.call("object_list", limit=500, detail=True)["plates"][0]["objects"][0]["instances"][0]
        bb = box["bbox"]
        self.t.check(f"{s} 14: the cube lies inside plate 0", bb["min"][0] >= x0 and bb["min"][1] >= y0 and
                     bb["max"][0] <= x0 + w and bb["max"][1] <= y0 + d, (bb, (x0, y0, w, d)))
        arranged = box["offset"]
        self.run("plate_select", plate=2)
        self.slice_is(f"{s} 15", self.run("plate_slice", plates=0), [(0, "ok")])
        self.plates_are(f"{s} 15", '"" 1 * S', '"" 0', '"" 0')
        self.tab_is(f"{s} 15", "preview")
        self.t.refused(f"{s} 16: refused", lambda: self.c.call("object_arrange", plates=2),
                       words="Plate 2 has no objects to arrange")
        self.plates_are(f"{s} 16", '"" 1 * S', '"" 0', '"" 0')
        self.tab_is(f"{s} 16", "prepare")
        self.run("object_transform", object=0, plate=2)
        self.idle()
        self.plates_are(f"{s} 17: plate 0 stays current", '"" 0 *', '"" 0', '"" 1')
        got, _ = self.objects()
        o2 = items[2]["origin"]
        want = [arranged[0] + o2[0] - x0, arranged[1] + o2[1] - y0]
        self.t.equal(f"{s} 17: the cube on plate 2", [o["plate"] for o in got], [2])
        self.t.near(f"{s} 17: the cube's offset", got[0]["offset"][:2] if got else None, want, 0.001)

    def slice_notice_gone(self):
        """Waits until the Slice complete notification a slice leaves has gone: it shows for 3 s and then fades for 2, and
        a capture taken while it fades differs from the next. The wait goes by what widget_list lists, not by time, so a
        machine that draws slowly waits as long as it needs. The notification counts down only while the main window is
        active, so on a desktop where another window is it stays as it is; after 15 s the wait goes on, since a
        notification that stays draws the same in every capture."""
        def gone():
            return self.c.call("widget_list", query="Slice complete", include_menus=False).get("nodes") == 0
        try:
            until(gone, "the Slice complete notification gone", timeout_s=15, interval_s=0.25)
        except TimeoutError:
            pass

    def capture(self, label):
        """The canvas's PNG bytes and width once two captures in a row, taken back to back, are byte-identical. A
        capture whose reply has `approximate` or a `note` fails the check at once, and so do 30 s without two alike."""
        path = os.path.join(self.folder, "capture.png")
        got = {"data": None, "width": None, "flawed": None}

        def settled():
            reply = self.c.call("window_screenshot", target="canvas", include_image=False, save_path=path, overwrite=True)
            if reply.get("approximate") or reply.get("note"):
                got["flawed"] = reply
                return True
            with open(reply["path"], "rb") as f:
                data = f.read()
            same = data == got["data"]
            got.update(data=data, width=reply.get("width"))
            return same

        try:
            until(settled, "two captures in a row agree", interval_s=0)
        except TimeoutError as e:
            self.t.check(f"{label}: two captures in a row agree", False, str(e))
            return got["data"], got["width"]
        if got["flawed"] is not None:
            self.t.check(f"{label}: the capture", False, got["flawed"])
            return None, got["flawed"].get("width")
        return got["data"], got["width"]

    def conf(self, key, expected):
        """The app section's value of `key` in the configuration file, read until it is `expected`, since the app writes
        the file at an idle moment; when 30 s pass first, the value read last, which the caller's check then shows."""
        path = os.path.join(self.session["datadir"], "OrcaSlicer.conf")
        seen = {}

        def holds():
            try:
                with open(path, encoding="utf-8") as f:
                    text = f.read()
                seen["value"] = json.loads(text[:text.rfind("}") + 1])["app"].get(key)
            except (OSError, ValueError, KeyError):  # read while the app writes it
                pass
            return "value" in seen and seen["value"] == expected

        try:
            until(holds, f"{key} in OrcaSlicer.conf is {expected!r}", interval_s=0.1)
        except TimeoutError:
            pass
        return seen.get("value")

    def view_switches(self):
        s = "view switches"
        self.start(s)
        self.run("object_add_primitive", shape="sphere", size=[30], name="ball")
        self.run("object_select", objects=[0])
        # The iso direction lowered to 20 degrees above the bed, from where the sphere's overhang shows.
        self.run("view_set", camera="iso", orbit=[0, -25])
        self.idle()
        W =self.c.call("app_state")["view"]["sidebar_width"]
        self.t.check(f"{s}: the start width is at least 390", W >= 390, W)
        START = {"gridlines", "axes", "navigator", "gcode_window"}
        SWITCHES = ["labels", "overhangs", "outline", "wireframe", "gridlines", "axes", "navigator", "gcode_window"]

        def V(on=START, projection="perspective", section=False, sidebar_collapsed=False, legend_collapsed=False,
              sidebar_width=W):
            view = {k: k in on for k in SWITCHES}
            view.update(projection=projection, section={"on": section}, sidebar_collapsed=sidebar_collapsed,
                        legend_collapsed=legend_collapsed, sidebar_width=sidebar_width)
            return view

        def view_is(label, want):
            self.t.equal(f"{label}: the view", self.c.call("app_state")["view"], want)

        view_is(f"{s}: the start", V())
        C0, w0 = self.capture(f"{s}: C0")

        def capture_is(label, same, base=None):
            base = C0 if base is None else base
            data, _ = self.capture(label)
            if same:
                self.t.check(f"{label}: the capture is the start's", data == base, "the captures differ")
            else:
                self.t.check(f"{label}: the capture differs from the start's", data != base, "the captures are the same")

        def switch(step, name, key, default):
            on = (START | {name}) if not default else (START - {name})
            reply = self.run("view_set", **{name: not default})
            self.t.equal(f"{s} {step}: {name} {not default}: the reply", reply.get(name), not default)
            view_is(f"{s} {step}: {name} {not default}", V(on=on))
            capture_is(f"{s} {step}: {name} {not default}", False)
            if key:
                self.t.equal(f"{s} {step}: {name} {not default}: kept", self.conf(key, not default), not default)
            reply = self.run("view_set", **{name: default})
            self.t.equal(f"{s} {step}: {name} {default}: the reply", reply.get(name), default)
            view_is(f"{s} {step}: {name} {default}", V())
            if key:
                self.t.equal(f"{s} {step}: {name} {default}: kept", self.conf(key, default), default)

        switch(1, "labels", "show_labels", False)
        for name, key in [("overhangs", "show_overhang"), ("outline", "show_outline"), ("wireframe", None)]:
            switch(3, name, key, False)
        switch(4, "gridlines", "show_plate_gridlines", True)
        for name, key in [("axes", "show_axes"), ("navigator", "show_3d_navigator")]:
            switch(5, name, key, True)
        three = ("gridlines", "axes", "navigator")
        reply = self.run("view_set", **{k: False for k in three})
        self.t.equal(f"{s} 6: the reply", [reply.get(k) for k in three], [False] * 3)
        view_is(f"{s} 6", V(on=START - set(three)))
        reply = self.run("view_set", **{k: True for k in three})
        self.t.equal(f"{s} 7: the reply", [reply.get(k) for k in three], [True] * 3)
        view_is(f"{s} 7", V())
        capture_is(f"{s} 7", True)
        reply = self.run("view_set", projection="orthographic")
        self.t.equal(f"{s} 8: the reply", reply.get("projection"), "orthographic")
        view_is(f"{s} 8", V(projection="orthographic"))
        self.t.equal(f"{s} 8: the camera", self.c.call("app_state")["camera"]["type"], "ortho")
        capture_is(f"{s} 8", False)
        reply = self.run("view_set", projection="perspective")
        self.t.equal(f"{s} 9: the reply", reply.get("projection"), "perspective")
        view_is(f"{s} 9", V())
        self.t.equal(f"{s} 9: the camera", self.c.call("app_state")["camera"]["type"], "perspective")
        reply = self.run("view_set", section={"on": True})
        self.t.equal(f"{s} 10: the reply", reply.get("section", {}).get("on"), True)
        view_is(f"{s} 10", V(section=True))
        self.tab_is(f"{s} 10", "prepare")
        capture_is(f"{s} 10", False)
        reply = self.run("view_set", section={"on": False})
        self.t.equal(f"{s} 11: the reply", reply.get("section", {}).get("on"), False)
        view_is(f"{s} 11", V())
        capture_is(f"{s} 11", True)

        def width_is(label, compare):
            _, width = self.capture(label)
            self.t.check(f"{label}: the capture's width", width is not None and w0 is not None and compare(width, w0),
                         (width, w0))

        reply = self.run("view_set", sidebar_collapsed=True)
        self.t.equal(f"{s} 12: the reply", reply.get("sidebar_collapsed"), True)
        view_is(f"{s} 12", V(sidebar_collapsed=True))
        width_is(f"{s} 12: wider", lambda a, b: a > b)
        reply = self.run("view_set", sidebar_collapsed=False)
        self.t.equal(f"{s} 13: the reply", reply.get("sidebar_collapsed"), False)
        view_is(f"{s} 13", V())
        width_is(f"{s} 13: as wide", lambda a, b: a == b)
        reply = self.run("view_set", sidebar_width=W + 150)
        self.t.equal(f"{s} 14: the reply", reply.get("sidebar_width"), W + 150)
        view_is(f"{s} 14", V(sidebar_width=W + 150))
        width_is(f"{s} 14: narrower", lambda a, b: a < b)
        self.run("view_set", layout="default")
        D = self.c.call("app_state")["view"]["sidebar_width"]
        self.t.check(f"{s} 15: the default width is at least 390", D >= 390, D)
        view_is(f"{s} 15", V(sidebar_width=D))
        width_is(f"{s} 15: as wide", lambda a, b: a == b)
        self.run("view_set", sidebar_width=W + 150)
        reply = self.run("view_set", sidebar_width="default")
        self.t.equal(f"{s} 16: the reply", reply.get("sidebar_width"), D)
        view_is(f"{s} 16: the default width is the default layout's", V(sidebar_width=D))
        self.slice_is(f"{s} 17", self.run("plate_slice", preview=True), [(0, "ok")], layers=(149, 151))
        view_is(f"{s} 17", V(sidebar_width=D))
        self.tab_is(f"{s} 17", "preview")
        self.slice_notice_gone()
        C1, _ = self.capture(f"{s}: C1")
        reply = self.run("view_set", legend_collapsed=True)
        self.t.equal(f"{s} 18: the reply", reply.get("legend_collapsed"), True)
        view_is(f"{s} 18", V(legend_collapsed=True, sidebar_width=D))
        capture_is(f"{s} 18", False, C1)
        reply = self.run("view_set", legend_collapsed=False)
        self.t.equal(f"{s} 19: the reply", reply.get("legend_collapsed"), False)
        view_is(f"{s} 19", V(sidebar_width=D))
        capture_is(f"{s} 19", True, C1)
        for value in (False, True):
            reply = self.run("view_set", gcode_window=value)
            self.t.equal(f"{s} 20: gcode_window {value}: the reply", reply.get("gcode_window"), value)
            view_is(f"{s} 20: gcode_window {value}", V(on=START if value else START - {"gcode_window"}, sidebar_width=D))
            self.t.equal(f"{s} 20: gcode_window {value}: kept", self.conf("show_gcode_window", value), value)
        reply = self.run("view_set", view_type="speed", legend_collapsed=True)
        self.t.equal(f"{s} 21: the reply", reply.get("legend_collapsed"), True)
        view_is(f"{s} 21: a fold asked with a view type stays", V(legend_collapsed=True, sidebar_width=D))
        self.run("view_set", view_type="feature_type")
        view_is(f"{s} 22: a view type picked alone unfolds the legend", V(sidebar_width=D))

    def app_info(self):
        s = "the app's account"
        self.start(s)
        state =self.c.call("app_state")
        self.t.equal(f"{s} 1: the account", state.get("account"), {"signed_in": False, "bambu_signed_in": False})
        self.t.equal(f"{s} 1: the network plug-in", state.get("network_plugin"),
                     {"installed": False, "loaded": False, "version": "", "error": ""})
        norm = lambda p: os.path.normcase(os.path.normpath(p))  # noqa: E731
        folder = state.get("config_folder") or ""
        self.t.equal(f"{s} 1: the configuration folder is the datadir", norm(folder), norm(self.session["datadir"]))
        log = self.c.call("app_log", lines=1).get("file") or ""
        self.t.equal(f"{s} 2: the log lies in the configuration folder's log folder", norm(os.path.dirname(log)),
                     norm(os.path.join(folder, "log")))

    def starts_from(self):
        s = "the start"
        self.t.equal(f"{s}: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        # The purge of the two-colour slices, and so the depth of their prime tower, follows the slots' colours and the
        # flush multiplier.
        listed = self.c.call("filament_list")
        self.t.equal(f"{s}: the slot's colour and the flush multiplier",
                     (listed["filaments"][0]["colour"].upper(), listed["flush_multiplier"]),
                     (SLOT_COLOURS[0].upper(), [FLUSH_MULTIPLIER]))
        self.run("dialog_auto_answer", rules=[{"match": SPIRAL, "button": "yes"}, {"match": SAVE, "button": "No"}])

    def run_all(self, only=()):
        self.t.scenario("the start", self.starts_from)
        wanted = set(only) | {self.AFTER[name] for name in only if name in self.AFTER}
        for name in self.SCENARIOS:
            if not only or name in wanted:
                self.t.scenario(name, getattr(self, name))
        return self.t.exit_code()


def main():
    client, session = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-slice-") as folder:
        return SlicePreviewTest(client, session, folder).run_all(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
