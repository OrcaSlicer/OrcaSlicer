#!/usr/bin/env python3
"""The filament list, as the test_filaments entry of tools/mcp/test-entries/filaments_settings.md designs it.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_filaments.py

Exits 0 when every check passes.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, SLOT_COLOURS, Checks, add_slot, filament_rows,  # noqa: E402
                    g_text, presets, run, slot_colours, slots, start, wait)

B = GENERIC_PLA
X = "FilAr PLA Blanco Calido"
M = "Generic PLA Matte @System"
S = "Bambu Support For PLA @System"
A = "Bambu Support for ABS @System"
G = "Bambu PETG Basic @System"
H = "Bambu PETG HF @System"
LETTER = {B: "B", X: "X", M: "M", S: "S", A: "A", G: "G", H: "H"}
# The filaments the test shows by name, which it installs and uninstalls before its first scenario.
HIDDEN = [X, M, S, A, G, H]
# The name each preset's selector shows, which a pick clicks.
SHOWN = {B: "Generic PLA", X: "FilAr PLA Blanco Calido", M: "Generic PLA Matte", S: "Bambu Support For PLA",
         A: "Bambu Support for ABS", G: "Bambu PETG Basic", H: "Bambu PETG HF"}
GREYS = ["#111111", "#222222", "#333333"]
SENTINEL = 777
PROMPT = "Transfer or discard changes"
FLUSH = {"All": "all", "Color": "color change", "None": "disabled"}
# The states' short forms, as the entry writes them.
COLOURS = {"#111111;#222222;#333333": "c3", "#111111;#FFF8E7;#333333": "cX"}
MATRICES = {"0,777,777,777,0,777,777,777,0": "S3", "0,230,777,700,0,700,777,230,0": "Sup"}
CUBE = {"shape": "cube", "size": [20, 20, 10], "name": "cube", "position": [100, 100, 0]}
CYLINDER = {"shape": "cylinder", "size": [20, 10], "name": "cylinder", "position": [160, 100, 0]}
PART_NOTE = 'A part\'s value (source "part", or in parts) applies only inside that part\'s volume.'
# The mixed filaments' scenarios: their start, the colours of slots 2 and 3 where a mix's colour is read, and the
# titles of a mix's dialogs.
MIXED_START = {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [B]}
RED, BLUE = "#FF0000", "#0000FF"
HEX = re.compile(r"^#[0-9A-F]{6}$")
EDIT, ADD = "Edit Mixed Filament", "Add Mixed Filament"

# The plugin API call a row makes, run with script_run: `body` sets `result`, and an exception the API raises is
# returned as {raised: {type, message}}.
API = """
import orca
f = orca.host.preset.filament
def tx(t):
    return {"target_name": t.target_name, "selection_changed": t.selection_changed, "slot_number": t.slot_number}
def done(r):
    return {"target_name": r.target_name, "selection_changed": r.selection_changed,
            "slot_assignment_changed": r.slot_assignment_changed, "shared_slots": list(r.shared_slots),
            "keys": list(r.keys), "dirty": r.dirty}
try:
%s
except Exception as e:
    result = {"raised": {"type": type(e).__name__, "message": str(e)}}
"""


def args_of(*args):
    return ", ".join(repr(a) for a in args)


def commit_form(*args):
    """begin_update(*args).commit()"""
    return f"    result = done(f.begin_update({args_of(*args)}).commit())"


def begin_form(*args):
    """begin_update(*args), not committed"""
    return f"    result = tx(f.begin_update({args_of(*args)}))"


def update_form(*args):
    """update(*args), which returns nothing"""
    return f"    f.update({args_of(*args)})\n    result = 'returned'"


def set_commit_form(slot, key, value):
    """t = begin_update(slot); t.set(key, value); t.commit()"""
    return f"    t = f.begin_update({slot!r})\n    t.set({key!r}, {value!r})\n    result = done(t.commit())"


def commit(name, selection=True, assignment=True, shared=(1,), keys=(), dirty=False):
    return {"target_name": name, "selection_changed": selection, "slot_assignment_changed": assignment,
            "shared_slots": list(shared), "keys": list(keys), "dirty": dirty}


def raised(kind, message):
    return {"raised": {"type": kind, "message": message}}


def canceled(name, slot):
    return raised("RuntimeError", f'Switching the edited filament preset to "{name}" for filament slot {slot} was '
                                  f"canceled; neither the slot assignment nor any setting was changed")


def numbers(text):
    return [float(v) for v in text.split(",")]


def is_r2(matrix):
    """Slot 2's row and column of three slots worked out by the app: entries 1, 3, 5 and 7 above 0 and other than
    the sentinel, entries 2 and 6 the sentinel, the diagonal 0."""
    try:
        m = numbers(matrix)
    except ValueError:
        return False
    return (len(m) == 9 and all(m[i] == 0 for i in (0, 4, 8)) and all(m[i] == SENTINEL for i in (2, 6)) and
            all(m[i] > 0 and m[i] != SENTINEL for i in (1, 3, 5, 7)))


def four(matrix):
    """Entries 1, 3, 5 and 7: slot 2's row and column."""
    m = numbers(matrix)
    return [m[1], m[3], m[5], m[7]]


class FilamentsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Reads ----

    def multi_bed_types(self):
        return self.c.call("settings_get", type="printer", keys=["support_multi_bed_types"],
                           include_meta=False)["items"][0]["value"]

    def project_values(self, *keys):
        items = self.c.call("project_get_settings", keys=list(keys), include_meta=False)["items"]
        return {i["key"]: i["value"] for i in items}

    def slots(self):
        return slots(self.c)

    def matrix(self):
        """filament_list's flush_volumes_matrix, written as the project stores it: `0,777,777,0`."""
        return ",".join(g_text(x) for x in self.c.call("filament_list")["flush_volumes_matrix"])

    def dialogs(self, message=False):
        """The open dialogs as {title, buttons}, with their message too where `message` asks for it."""
        return [dict({"title": d.get("title"), "buttons": [b.get("label") for b in d.get("buttons", [])]},
                     **({"message": d.get("message", "")} if message else {}))
                for d in self.c.call("app_state").get("dialogs", [])]

    def state(self):
        """The raw state: each slot's preset, the tab's preset and whether it is modified, each slot's
        slow_down_layer_time, the colours, the matrix and the open dialogs' titles."""
        app = self.c.call("app_state")
        presets = app["presets"]["filaments"]
        tab = [(i["name"], bool(i.get("modified"))) for i in self.c.call("preset_list", type="filament", limit=500)["items"]
               if i.get("selected")]
        edits = [self.c.call("settings_get", type="filament", filament_slot=n, keys=["slow_down_layer_time"],
                             include_meta=False)["items"][0]["value"] for n in range(1, len(presets) + 1)]
        return {"presets": presets, "tab": tab, "edits": edits, "colours": slot_colours(self.c), "matrix": self.matrix(),
                "dialogs": [d.get("title") for d in app.get("dialogs", [])]}

    @staticmethod
    def text(state):
        """The state written as the entry writes it: `B X B | B* | 4,4,4 | cX | R2`, with the open dialogs after it."""
        tab = " ".join(LETTER.get(name, name) + ("*" if modified else "") for name, modified in state["tab"])
        matrix = MATRICES.get(state["matrix"]) or ("R2" if is_r2(state["matrix"]) else state["matrix"])
        out = " | ".join([" ".join(LETTER.get(p, p) for p in state["presets"]), tab, ",".join(state["edits"]),
                          COLOURS.get(state["colours"], state["colours"]), matrix])
        return out + (f" | dialogs {state['dialogs']}" if state["dialogs"] else "")

    def state_is(self, label, want):
        state = self.state()
        self.t.equal(f"{label}: the state", self.text(state), want)
        return state

    def filaments(self):
        """Each object as its filament and, with several parts, its parts' filaments: `cube 1 (1, 2) · cylinder 2`."""
        listing = self.c.call("object_list", limit=500, detail=True)
        objects = sorted([o for p in listing["plates"] for o in p["objects"]] + listing.get("outside", []),
                         key=lambda o: o["index"])
        return " · ".join(f"{o['name']} {o['filament']}" + (
            f" ({', '.join(str(p['filament']) for p in o['parts'])})" if len(o["parts"]) > 1 else "") for o in objects)

    def item(self, key, target, **where):
        """One item of settings_get as {value, effective, source, parts}, fields the reply leaves out absent, and the
        reply's note."""
        reply = self.c.call("settings_get", target=target, keys=[key], include_meta=False, **where)
        item = reply["items"][0]
        return {k: item[k] for k in ("value", "effective", "source", "parts") if k in item}, reply.get("note")

    def project(self):
        """The filament list's and the project's own settings, `colours | multiplier | matrix | plate type | tower`,
        with plate_list's plate 0 added where it disagrees with project_get_settings."""
        listed = self.c.call("filament_list")
        v = self.project_values("curr_bed_type", "wipe_tower_x", "wipe_tower_y")
        tower = f"{g_text(v['wipe_tower_x'].split(',')[0])},{g_text(v['wipe_tower_y'].split(',')[0])}"
        plate = self.c.call("plate_list", limit=500)["items"][0]
        out = {"colours": ";".join(row["colour"] for row in listed["filaments"]),
               "multiplier": ",".join(g_text(x) for x in listed["flush_multiplier"]),
               "matrix": ",".join(g_text(x) for x in listed["flush_volumes_matrix"]),
               "bed": v["curr_bed_type"], "tower": tower}
        plate_tower = ",".join(g_text(x) for x in plate.get("wipe_tower") or [])
        if (plate.get("curr_bed_type"), plate_tower) != (out["bed"], tower):
            out["plate_list"] = f"{plate.get('curr_bed_type')} {plate_tower}"
        return out

    def project_is(self, label, colours, multiplier, matrix, bed, tower, matrix_holds=None):
        """project() compared whole; a matrix the app works out is given as a name and checked by `matrix_holds`."""
        got = self.project()
        if matrix_holds is not None and matrix_holds(got["matrix"]):
            got = dict(got, matrix=matrix)
        text = " | ".join([got["colours"], got["multiplier"], got["matrix"], got["bed"], got["tower"]])
        if "plate_list" in got:
            text += f" | plate_list {got['plate_list']}"
        self.t.equal(f"{label}: the project's settings", text, " | ".join([colours, multiplier, matrix, bed, tower]))
        return got

    def grams(self):
        """Each slot's model_g, flush_g and tower_g, and the plate's filament_changes, from plate_slice_stats."""
        summary = self.c.call("plate_slice_stats", plate=0)["summary"]
        return ({f["slot"]: (f["model_g"], f["flush_g"], f["tower_g"]) for f in summary["filaments"]},
                summary["filament_changes"])

    def rows(self):
        return filament_rows(self.c)

    def row(self, slot):
        return self.rows()[slot - 1]

    def mixes(self):
        """The mixed rows as the entry writes them: `4 = 2,3 @ 50,50, bottom 3 top 2, per part`, joined by `·`. A
        gradient whose curve is not linear is written with it, which no row expects."""
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

    def mixed_is(self, label, want_slots, want_mixes):
        self.t.equal(f"{label}: the slots", self.slots(), want_slots)
        self.t.equal(f"{label}: the mixes", self.mixes(), want_mixes)

    def on(self, name):
        """The filament of the object named `name`, from object_list."""
        listing = self.c.call("object_list", limit=500)
        found = [o for p in listing["plates"] for o in p["objects"] if o["name"] == name]
        found += [o for o in listing.get("outside", []) if o["name"] == name]
        return found[0]["filament"] if len(found) == 1 else found

    # ---- Calls ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def idle(self):
        wait(self.c, "idle")

    def pick(self, slot, preset):
        self.c.call("widget_click", target={"class": "PlaterPresetComboBox", "index": slot})
        return self.c.call("widget_click", target={"row": SHOWN[preset]})

    def tool(self, slot, name, changes=None):
        args = {"type": "filament", "name": name, "filament_slot": slot}
        if changes:
            args["preset_changes"] = changes
        return self.c.call("preset_select", **args)

    def api(self, body):
        return self.c.call("script_run", code=API % body)

    @staticmethod
    def script_result(reply):
        if isinstance(reply, dict) and reply.get("status") == "finished":
            return reply.get("result")
        return {"script": reply}

    def close_dialogs(self):
        """Closes what a call left open against the entry, so that the next case starts clean."""
        for _ in range(5):
            if not self.c.call("app_state").get("dialogs"):
                return
            self.c.call("dialog_answer", button="cancel")
            wait(self.c, "no_dialog")

    def quiet(self, label, reply):
        """A call that opens nothing: its reply is not held by a dialog, and the wait for idle is met."""
        held = isinstance(reply, dict) and reply.get("status") in ("blocked_by_dialog", "running")
        self.t.check(f"{label}: no dialog opened", not held, reply)
        if held:
            self.close_dialogs()
            self.c.await_job(reply, timeout=60)
        self.idle()
        return reply

    def prompt(self, label, reply, answer, offers=("Transfer", "Discard", "Save"), lacks=()):
        """A call the prompt holds: it replies blocked_by_dialog, the prompt is the only dialog with the buttons it
        offers, the answer closes it and the wait for the call's job is met. Returns the job's result."""
        self.t.equal(f"{label}: the call replies blocked_by_dialog", reply.get("status"), "blocked_by_dialog")
        if reply.get("status") == "running":
            wait(self.c, "dialog")
        dialogs = self.dialogs()
        buttons = dialogs[0]["buttons"] if dialogs else []
        ok = ([d["title"] for d in dialogs] == [PROMPT] and all(b in buttons for b in offers) and
              not any(b in buttons for b in lacks))
        self.t.check(f"{label}: the prompt alone, offering {', '.join(offers)}" +
                     (f" and not {', '.join(lacks)}" if lacks else ""), ok, dialogs)
        if "job" not in reply:
            return reply
        self.c.call("dialog_answer", button=answer)
        try:
            result = wait(self.c, "job", 30, job=reply["job"])
        except TimeoutError as e:
            self.t.check(f"{label}: the wait for the call's job is met", False, e)
            self.close_dialogs()
            result = self.c.await_job(reply, timeout=60)
        except orca_mcp.McpError as e:
            result = {"ERROR": str(e)}
        self.idle()
        return result

    def tool_reply(self, label, reply, **want):
        """The reply's selected, shown, discarded and modified, each as `want` gives it (None: absent)."""
        self.t.equal(f"{label}: the reply", {k: reply.get(k) for k in want}, want)

    def held(self, label, reply, title):
        """A call the dialog titled `title` holds: the reply waits on it, and it is the one dialog open, with OK and
        Cancel. Returns the open dialogs with their message."""
        self.t.equal(f"{label}: the call waits on a dialog", reply.get("status"), "blocked_by_dialog")
        shown = self.dialogs(message=True)
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

    def cube(self, name, x, y=100):
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20], name=name, position=[x, y, 0])

    def save_delete_open(self, label, name, slot, want_slots):
        """Adds a cube, saves the project to `name`, deletes mixed filament `slot`, and opens the file again. The cube is
        there so that the open shows no warning, which the app gives for a project file that holds no object."""
        self.cube("kept", 100)
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        self.run("filament_delete", filament_slot=slot)
        self.t.equal(f"{label}: after the delete", self.slots(), want_slots)
        self.run("project_open", path=path, discard_changes=True)
        self.idle()
        self.t.equal(f"{label}: the cube is back", self.on("kept"), 1)

    # ---- Arranging ----

    def start(self, name, filaments):
        """The scenario's start(): a new project on the Klipper printer with one slot per preset of `filaments`, and
        the baseline's Auto-flush All, which the slot scenarios' volumes follow, read back."""
        self.t.equal(f"{name}: the start's presets", start(self.c, filaments=filaments),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": list(filaments)})
        self.flush_reads(name, "All")
        self.t.equal(f"{name}: the start's slots", self.slots(), ",".join(["0"] * len(filaments)))

    def flush_reads(self, label, which):
        got = self.c.call("preferences_get", keys=["auto_calculate_flush"])["items"][0]["value"]
        self.t.equal(f"{label}: Auto-flush reads {which}", got, FLUSH[which])

    def flush_preference(self, label, which):
        self.run("preferences_set", values={"auto_calculate_flush": FLUSH[which]})
        self.flush_reads(label, which)

    def drop_edits(self):
        """Drops the tab's unsaved changes through a slot that holds the preset the tab edits."""
        state = self.state()
        if not state["tab"] or not state["tab"][0][1]:
            return
        name = state["tab"][0][0]
        slot = state["presets"].index(name) + 1 if name in state["presets"] else 1
        reply = self.c.call("settings_set", type="filament", filament_slot=slot, reset_all=True)
        if isinstance(reply, dict) and reply.get("status") == "blocked_by_dialog":
            self.c.call("dialog_answer", button="Discard")
            self.c.await_job(reply, timeout=60)

    def arrange(self, label, presets, dirty, want):
        """The state a case starts from, checked as the row's start."""
        self.drop_edits()
        for slot, name in enumerate(presets, 1):
            self.run("preset_select", type="filament", name=name, filament_slot=slot, preset_changes="discard")
        self.run("settings_set", type="filament", filament_slot=1, reset_all=True)
        n = len(presets)
        for slot, colour in enumerate(GREYS[:n], 1):
            self.run("filament_set_colour", filament_slot=slot, colour=colour)
        # After the colours, whose change recalculates the matrix.
        self.run("filament_set_flushing", flush_volumes_matrix=[0 if i == j else SENTINEL for i in range(n) for j in range(n)])
        if dirty:
            self.run("settings_set", type="filament", filament_slot=1, values={"slow_down_layer_time": 9})
        self.idle()
        self.state_is(f"{label}: the start", want)

    def mixed_start(self, label, colours=False):
        """The mixed filaments' start: three physical slots of Generic PLA, slots 2 and 3 added in the baseline's
        colours, or red and blue with `colours`."""
        self.t.equal(f"{label}: the start's presets", start(self.c), MIXED_START)
        for colour in (RED, BLUE) if colours else SLOT_COLOURS[1:3]:
            add_slot(self.c, colour)
        self.t.equal(f"{label}: three physical slots", self.slots(), "0,0,0")
        self.t.equal(f"{label}: of Generic PLA", [(r.get("preset"), r.get("type")) for r in self.rows()],
                     [(B, "PLA")] * 3)
        if colours:
            self.t.equal(f"{label}: slots 2 and 3 red and blue", [r["colour"] for r in self.rows()[1:]], [RED, BLUE])

    # ---- Scenarios ----

    def hidden(self):
        s = "hidden filaments"
        self.start(s, [B])
        self.arrange(s, [B], False, "B | B | 4 | #111111 | 0")
        queries = {X: "FilAr PLA Blanco Calido", M: "Generic PLA Matte", S: "Bambu Support For PLA",
                   A: "Bambu Support for ABS"}

        def found(query, **args):
            return {i["name"]: i for i in self.c.call("preset_list", type="filament", query=query, limit=500,
                                                         **args)["items"]}

        listed = {LETTER[p]: (p in found(q), found(q, include_uninstalled=True).get(p, {}).get("installed", "absent"))
                  for p, q in queries.items()}
        self.t.equal(f"{s} 1: hidden, and listed with include_uninstalled as not installed", listed,
                     {"X": (False, False), "M": (False, False), "S": (False, False), "A": (False, False)})
        self.state_is(f"{s} 1", "B | B | 4 | #111111 | 0")
        for step, preset, want in [(2, X, "X | X | 4 | #FFF8E7 | 0"), (3, M, "M | M | 4 | #FFF8E7 | 0"),
                                   (4, S, "S | S | 8 | #FFF8E7 | 0"), (5, A, "A | A | 12 | #FFF8E7 | 0")]:
            reply = self.quiet(f"{s} {step}", self.tool(1, queries[preset]))
            self.tool_reply(f"{s} {step}", reply, selected=preset, shown=preset)
            self.state_is(f"{s} {step}", want)
        reply = self.quiet(f"{s} 6", self.tool(1, "Generic PLA"))
        self.tool_reply(f"{s} 6", reply, selected=B, shown=None)
        self.state_is(f"{s} 6", "B | B | 4 | #FFF8E7 | 0")
        shown = {LETTER[p]: found(q).get(p, {"absent": True}).get("installed", "no field") for p, q in queries.items()}
        self.t.equal(f"{s} 7: each listed with no installed field", shown,
                     {"X": "no field", "M": "no field", "S": "no field", "A": "no field"})
        self.state_is(f"{s} 7", "B | B | 4 | #FFF8E7 | 0")

    def one_slot_pla(self):
        s = "one slot, X"
        self.start(s, [B])
        clean, dirty = "B | B | 4 | #111111 | 0", "B | B* | 9 | #111111 | 0"
        after1, after4 = "X | X | 4 | #FFF8E7 | 0", "X | X* | 9 | #FFF8E7 | 0"
        after6 = "B | B* | 9 | #FFF8E7 | 0"
        transferred = {"selected": X, "modified": {"filament": {"name": X, "keys": ["slow_down_layer_time"]}},
                       "discarded": None}
        discarded = {"selected": X, "discarded": {"filament": {"name": B, "keys": ["slow_down_layer_time"]}},
                     "modified": None}

        def case(row, is_dirty):
            label = f"{s} {row}"
            self.arrange(label, [B], is_dirty, dirty if is_dirty else clean)
            return label

        label = case(1, False)
        self.quiet(label, self.pick(1, X))
        self.state_is(label, after1)
        label = case(2, False)
        self.tool_reply(label, self.quiet(label, self.tool(1, X)), selected=X, shown=None, discarded=None,
                        modified=None)
        self.state_is(label, after1)
        label = case(3, False)
        result = self.script_result(self.quiet(label, self.api(commit_form(1, X))))
        self.t.equal(f"{label}: the commit", result, commit(X))
        self.state_is(label, after1)
        for row, answer, want in [(4, "Transfer", after4), (5, "Discard", after1), (6, "cancel", after6)]:
            label = case(row, True)
            self.prompt(label, self.pick(1, X), answer)
            self.state_is(label, want)
        label = case(7, True)
        self.tool_reply(label, self.quiet(label, self.tool(1, X, "transfer")), **transferred)
        self.state_is(label, after4)
        label = case(8, True)
        self.tool_reply(label, self.quiet(label, self.tool(1, X, "discard")), **discarded)
        self.state_is(label, after1)
        cancelled = {"selected": B, "modified": {"filament": {"name": B, "keys": ["slow_down_layer_time"]}},
                     "discarded": None}
        for row, answer, want, reply in [(9, "Transfer", after4, transferred), (10, "Discard", after1, discarded),
                                         (11, "cancel", after6, cancelled)]:
            label = case(row, True)
            result = self.prompt(label, self.tool(1, X), answer)
            self.tool_reply(f"{label}: the job's result", result, **reply)
            self.state_is(label, want)
        for row, answer, want, result in [(12, "Transfer", after4, commit(X, dirty=True)),
                                          (13, "Discard", after1, commit(X)),
                                          (14, "cancel", dirty, canceled(X, 1))]:
            label = case(row, True)
            got = self.script_result(self.prompt(label, self.api(commit_form(1, X)), answer))
            self.t.equal(f"{label}: the commit", got, result)
            self.state_is(label, want)

    def one_slot_support(self):
        """What the filament's type changes, every row from unsaved changes: the prompt without Transfer, the refused
        Transfer ahead, Discard ahead and the cancel by each route. The clean assignments and Discard answered by the
        tool or the api run the PLA scenario's code with another preset."""
        s = "one slot, A"
        self.start(s, [B])
        dirty = "B | B* | 9 | #111111 | 0"
        after1 = "A | A | 12 | #111111 | 0"
        no_transfer = {"offers": ("Discard", "Save"), "lacks": ("Transfer",)}

        def case(row):
            label = f"{s} {row}"
            self.arrange(label, [B], True, dirty)
            return label

        for row, answer, want in [(1, "Discard", after1), (2, "cancel", dirty)]:
            label = case(row)
            self.prompt(label, self.pick(1, A), answer, **no_transfer)
            self.state_is(label, want)
        label = case(3)
        self.t.refused(f"{label}: refused", lambda: self.tool(1, A, "transfer"), code="transfer_not_offered")
        self.state_is(label, dirty)
        label = case(4)
        self.tool_reply(label, self.quiet(label, self.tool(1, A, "discard")), selected=A,
                        discarded={"filament": {"name": B, "keys": ["slow_down_layer_time"]}})
        self.state_is(label, after1)
        label = case(5)
        result = self.prompt(label, self.tool(1, A), "cancel", **no_transfer)
        self.tool_reply(f"{label}: the job's result", result, selected=B)
        self.state_is(label, dirty)
        label = case(6)
        got = self.script_result(self.prompt(label, self.api(commit_form(1, A)), "cancel", **no_transfer))
        self.t.equal(f"{label}: the commit", got, canceled(A, 1))
        self.state_is(label, dirty)

    def one_slot_forms(self):
        s = "one slot, no preset named"
        self.start(s, [B])
        clean, dirty = "B | B | 4 | #111111 | 0", "B | B* | 9 | #111111 | 0"
        after2 = "B | B* | 8 | #111111 | 0"
        rows = [(1, False, commit_form(1), clean, commit(B, selection=False, assignment=False)),
                (2, False, update_form(1, "slow_down_layer_time", "8"), after2, "returned"),
                (3, False, update_form("slow_down_layer_time", "8"), after2, "returned"),
                (4, True, commit_form(1), dirty, commit(B, selection=False, assignment=False, dirty=True)),
                (5, True, update_form(1, "slow_down_layer_time", "8"), after2, "returned"),
                (6, True, update_form("slow_down_layer_time", "8"), after2, "returned"),
                (7, True, commit_form(1, "Generic PLA"), dirty,
                 commit(B, selection=False, assignment=False, dirty=True))]
        for row, is_dirty, body, want, result in rows:
            label = f"{s} {row}"
            self.arrange(label, [B], is_dirty, dirty if is_dirty else clean)
            got = self.script_result(self.quiet(label, self.api(body)))
            self.t.equal(f"{label}: what the call returns", got, result)
            self.state_is(label, want)

    def auto_flush(self):
        """The three routes meet in Plater::priv::filament_preset_picked(), where the preference is read: row 1 runs
        each route into it, and every other row one branch behind it, by the tool or the colour picker's change."""
        s = "Auto-flush"
        self.start(s, [B] * 3)
        start = "B B B | B | 4,4,4 | c3 | S3"
        # (row, preference, the preset the tool puts in slot 2 or None for the colour picker's change, colours,
        # matrix, whether its four entries are row 1's)
        table = [(2, "All", M, "c3", "R2", False), (3, "All", S, "c3", "Sup", False),
                 (4, "All", None, "cX", "R2", True), (5, "Color", X, "cX", "R2", True), (6, "Color", M, "c3", "S3", False),
                 (7, "Color", S, "c3", "S3", False), (8, "Color", None, "cX", "R2", True),
                 (9, "None", X, "cX", "S3", False), (10, "None", M, "c3", "S3", False),
                 (11, "None", S, "c3", "S3", False), (12, "None", None, "cX", "S3", False)]

        # Row 1's All is the baseline's, read back by the scenario's start.
        routes = []
        for route in ("pick", "tool", "api"):
            label = f"{s} 1, All, X by {route}"
            self.arrange(label, [B, B, B], False, start)
            if route == "pick":
                self.quiet(label, self.pick(2, X))
            elif route == "tool":
                self.quiet(label, self.tool(2, X))
            else:
                got = self.script_result(self.quiet(label, self.api(commit_form(2, X))))
                self.t.equal(f"{label}: the commit", got, commit(X, shared=[2]))
            tab = "X" if route == "api" else "B"
            matrix = self.state_is(label, f"B X B | {tab} | 4,4,4 | cX | R2")["matrix"]
            # Entries 1 and 7 go into X, entries 3 and 5 out of it.
            entries = four(matrix) if is_r2(matrix) else None
            self.t.check(f"{label}: the volumes into X above those out of it",
                         entries is not None and min(entries[0], entries[3]) > max(entries[1], entries[2]), matrix)
            routes.append(entries)
        self.t.check(f"{s} 1: the three routes give the same four entries",
                     routes[0] is not None and all(e == routes[0] for e in routes), routes)
        row1 = routes[0]

        which = "All"
        for row, preference, preset, colours, matrix, same in table:
            if preference != which:
                which = preference
                self.flush_preference(f"{s} {row}", which)
            what = "the colour picker's change" if preset is None else f"{LETTER[preset]} by tool"
            label = f"{s} {row}, {which}, {what}"
            self.arrange(label, [B, B, B], False, start)
            if preset is None:
                self.run("filament_set_colour", filament_slot=2, colour="#FFF8E7")
                self.idle()
                want = f"B B B | B | 4,4,4 | {colours} | {matrix}"
            else:
                self.quiet(label, self.tool(2, preset))
                edits = "4,8,4" if preset == S else "4,4,4"
                want = f"B {LETTER[preset]} B | B | {edits} | {colours} | {matrix}"
            got = self.state_is(label, want)["matrix"]
            if same:
                # The Klipper printer's minimum flush is 0 whichever preset a slot holds, so B left in slot 2 by the
                # colour change gives what X gives.
                entries = four(got) if is_r2(got) else None
                self.t.check(f"{label}: the four entries are row 1's", row1 is not None and entries == row1,
                             (entries, row1))

    def three_slots_dirty(self):
        s = "three slots, unsaved changes"
        self.start(s, [B] * 3)
        start, start_m = "B B B | B* | 9,9,9 | c3 | S3", "B M B | B* | 9,4,9 | c3 | S3"
        after1 = "B X B | B* | 9,4,9 | cX | R2"
        kept = {"selected": X, "modified": {"filament": {"name": B, "keys": ["slow_down_layer_time"]}},
                "discarded": None}
        label = f"{s} 1"
        self.arrange(label, [B, B, B], True, start)
        self.quiet(label, self.pick(2, X))
        self.state_is(label, after1)
        for row, changes in [(2, None), (3, "transfer"), (4, "discard")]:
            label = f"{s} {row}"
            self.arrange(label, [B, B, B], True, start)
            self.tool_reply(label, self.quiet(label, self.tool(2, X, changes)), **kept)
            self.state_is(label, after1)
        offers = {"offers": ("Transfer", "Discard")}
        for row, answer, want, result in [
                (5, "Transfer", "B X B | X* | 4,9,4 | cX | R2", commit(X, shared=[2], dirty=True)),
                (6, "Discard", "B X B | X | 4,4,4 | cX | R2", commit(X, shared=[2])),
                (7, "cancel", start, canceled(X, 2))]:
            label = f"{s} {row}"
            self.arrange(label, [B, B, B], True, start)
            got = self.script_result(self.prompt(label, self.api(commit_form(2, X)), answer, **offers))
            self.t.equal(f"{label}: the commit", got, result)
            self.state_is(label, want)
        for row, body in [(8, commit_form(2)), (9, update_form(2, "slow_down_layer_time", "8"))]:
            label = f"{s} {row}"
            self.arrange(label, [B, M, B], True, start_m)
            got = self.script_result(self.prompt(label, self.api(body), "cancel", **offers))
            self.t.equal(f"{label}: raised", got, canceled(M, 2))
            self.state_is(label, start_m)

    def three_slots_forms(self):
        s = "three slots, other forms"
        self.start(s, [B] * 3)
        start, start_m = "B B B | B | 4,4,4 | c3 | S3", "B M B | B | 4,4,4 | c3 | S3"
        label = f"{s} 1"
        self.arrange(label, [B, B, B], False, start)
        self.quiet(label, self.c.call("widget_set_value", target={"class": "PlaterPresetComboBox", "index": 2},
                                      value="FilAr PLA Blanco Calido"))
        self.state_is(label, "B X B | B | 4,4,4 | cX | R2")
        label = f"{s} 2"
        self.arrange(label, [B, B, B], False, start)
        got = self.script_result(self.quiet(label, self.api(begin_form(2, X))))
        self.t.equal(f"{label}: the transaction", got, {"target_name": X, "selection_changed": True, "slot_number": 2})
        self.state_is(label, "B X B | X | 4,4,4 | cX | R2")
        after5 = "B M B | M* | 4,8,4 | c3 | S3"
        rows = [(3, commit_form(2, "Generic PLA Matte"), "B M B | M | 4,4,4 | c3 | S3",
                 commit(M, assignment=False, shared=[2])),
                (4, commit_form(2), "B M B | M | 4,4,4 | c3 | S3", commit(M, assignment=False, shared=[2])),
                (5, set_commit_form(2, "slow_down_layer_time", "8"), after5,
                 commit(M, assignment=False, shared=[2], keys=["slow_down_layer_time"], dirty=True)),
                (6, update_form(2, "slow_down_layer_time", "8"), after5, "returned"),
                (7, update_form("slow_down_layer_time", "8"), "B M B | B* | 8,4,8 | c3 | S3", "returned")]
        for row, body, want, result in rows:
            label = f"{s} {row}"
            self.arrange(label, [B, M, B], False, start_m)
            got = self.script_result(self.quiet(label, self.api(body)))
            self.t.equal(f"{label}: what the call returns", got, result)
            self.state_is(label, want)

    def refusals(self):
        s = "three slots, refusals"
        self.start(s, [B] * 3)
        start = "B B B | B | 4,4,4 | c3 | S3"
        out_of_range = ("Filament slot %d is out of range; slot numbers are 1-based and this printer has 3 filament "
                        "slot(s)")
        not_offered = 'The selector of filament slot 2 does not offer "No Such Filament".'
        incompatible = '"Generic PLA @MyToolChanger" is not compatible with the selected printer.'

        def case(row):
            label = f"{s} {row}"
            self.arrange(label, [B, B, B], False, start)
            return label

        def api_raises(row, body, want):
            label = case(row)
            got = self.script_result(self.quiet(label, self.api(body)))
            self.t.equal(f"{label}: raised", got, want)
            self.state_is(label, start)

        label = case(1)
        self.t.refused(f"{label}: refused", lambda: self.tool(5, X), code="no_such_slot",
                       words="Filament slot 5 does not exist (the slots are 1..3)")
        self.state_is(label, start)
        api_raises(2, commit_form(5, X), raised("IndexError", out_of_range % 5))
        api_raises(3, commit_form(0, X), raised("IndexError", out_of_range % 0))
        api_raises(4, update_form(5, "slow_down_layer_time", "8"), raised("IndexError", out_of_range % 5))
        label = case(5)
        text = self.t.refused(f"{label}: refused", lambda: self.tool(2, "No Such Filament"),
                              code="preset_not_offered", words=not_offered)
        self.t.check(f"{label}: the refusal starts with its reason", (text or "").startswith(not_offered), text)
        self.state_is(label, start)
        api_raises(6, commit_form(2, "No Such Filament"), raised("ValueError", not_offered))
        label = case(7)
        text = self.t.refused(f"{label}: refused", lambda: self.tool(2, "Generic PLA @MyToolChanger"),
                              code="preset_incompatible", words=incompatible)
        self.t.check(f"{label}: the refusal starts with its reason", (text or "").startswith(incompatible), text)
        self.state_is(label, start)
        api_raises(8, commit_form(2, "Generic PLA @MyToolChanger"), raised("ValueError", incompatible))
        label = case(9)
        got = self.script_result(self.quiet(label, self.api(begin_form("FilAr PLA Blanco Calido"))))
        self.t.check(f"{label}: raised ValueError",
                     isinstance(got, dict) and got.get("raised", {}).get("type") == "ValueError" and
                     got["raised"].get("message", "").startswith(
                         'begin_update on the filament editor takes (slot_number, preset_name="")'), got)
        self.state_is(label, start)

        def names(query, **args):
            return {i["name"]: i for i in self.c.call("preset_list", type="filament", query=query, limit=500,
                                                         **args)["items"]}

        label = case(10)
        first, second = names("Bambu PETG"), names("Bambu PETG", include_uninstalled=True)
        self.t.equal(f"{label}: hidden, then listed as not installed",
                     {LETTER[p]: (p in first, second.get(p, {}).get("installed", "absent")) for p in (G, H)},
                     {"G": (False, False), "H": (False, False)})
        self.state_is(label, start)
        label = case(11)
        reply = self.quiet(label, self.tool(2, "Bambu PETG Basic"))
        self.tool_reply(label, reply, selected=G, shown=G)
        self.state_is(label, "B G B | B | 4,12,4 | c3 | R2")
        self.t.check(f"{label}: preset_list lists G", G in names("Bambu PETG Basic"), names("Bambu PETG Basic"))
        label = case(12)
        got = self.script_result(self.quiet(label, self.api(commit_form(2, H))))
        self.t.equal(f"{label}: nothing raised, and the slot assigned",
                     {k: got.get(k) for k in ("raised", "slot_assignment_changed")} if isinstance(got, dict) else got,
                     {"raised": None, "slot_assignment_changed": True})
        self.state_is(label, "B H B | H | 4,10,4 | c3 | R2")
        self.t.check(f"{label}: preset_list lists H", H in names("Bambu PETG HF"), names("Bambu PETG HF"))

    def add_objects(self):
        self.run("object_add_primitive", **CUBE)
        self.run("object_add_primitive", **CYLINDER)
        self.run("part_add", object=0, type="part", shape="cube", size=[5, 5, 5])
        self.idle()

    def slice_ok(self, label):
        reply = self.run("plate_slice")
        self.t.equal(f"{label}: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        return self.grams()

    def objects_and_parts(self):
        s = "filaments of objects and parts"
        self.start(s, [B, B])
        self.add_objects()
        self.t.equal(f"{s}: the start's filaments", self.filaments(), "cube 1 (1, 1) · cylinder 1")
        one = {"value": "1", "effective": "1", "source": "object"}
        two = {"value": "2", "effective": "2", "source": "object"}
        cube_start = dict(one, parts=[{"part": 1, "value": "1"}])
        self.t.equal(f"{s}: the start's cylinder", self.item("extruder", "object", object=1)[0], one)
        self.t.equal(f"{s}: the start's cube", self.item("extruder", "object", object=0)[0], cube_start)
        grams, changes = self.slice_ok(f"{s}: the control")
        control = grams
        self.t.check(f"{s}: the control prints with slot 1 alone",
                     grams[1][0] > 0 and grams[2] == (0, 0, 0) and changes == 0, (grams, changes))

        reply = self.run("settings_set", target="object", object=1, values={"extruder": 2})
        self.t.equal(f"{s} 1: the reply", (reply.get("applied", {}).get("extruder"),
                                           [o.get("index") for o in reply.get("objects", [])]), ("2", [1]))
        self.t.equal(f"{s} 1", self.filaments(), "cube 1 (1, 1) · cylinder 2")
        self.t.equal(f"{s} 1: the cylinder", self.item("extruder", "object", object=1)[0], two)
        self.t.equal(f"{s} 1: the cube", self.item("extruder", "object", object=0)[0], cube_start)
        self.run("undo", history="project")
        self.t.equal(f"{s} 2", self.filaments(), "cube 1 (1, 1) · cylinder 1")
        self.t.equal(f"{s} 2: the cylinder", self.item("extruder", "object", object=1)[0], one)
        self.run("redo", history="project")
        self.t.equal(f"{s} 3", self.filaments(), "cube 1 (1, 1) · cylinder 2")
        self.t.equal(f"{s} 3: the cylinder", self.item("extruder", "object", object=1)[0], two)

        reply = self.run("settings_set", target="part", object=1, part=0, values={"extruder": 1})
        self.t.equal(f"{s} 4: the reply", (reply.get("applied", {}).get("extruder"), reply.get("note")),
                     ("1", "A single-part object's filament is the object's; it was set there."))
        self.t.equal(f"{s} 4", self.filaments(), "cube 1 (1, 1) · cylinder 1")
        self.t.equal(f"{s} 4: the cylinder", self.item("extruder", "object", object=1)[0], one)

        part_two = {"value": "2", "effective": "2", "source": "part"}
        cube_part_two = dict(one, parts=[{"part": 1, "value": "2"}])
        reply = self.run("settings_set", target="part", object=0, part=1, values={"extruder": 2})
        parts = [{"object": p.get("object"), "part": p.get("part")} for p in reply.get("parts", [])]
        self.t.equal(f"{s} 5: the reply", (reply.get("applied", {}).get("extruder"), parts, reply.get("note")),
                     ("2", [{"object": 0, "part": 1}], None))
        self.t.equal(f"{s} 5", self.filaments(), "cube 1 (1, 2) · cylinder 1")
        self.t.equal(f"{s} 5: the cube's part", self.item("extruder", "part", object=0, part=1),
                     (part_two, PART_NOTE))
        self.t.equal(f"{s} 5: the cube", self.item("extruder", "object", object=0), (cube_part_two, PART_NOTE))

        grams, _ = self.slice_ok(f"{s} 6")
        step6 = grams
        self.t.check(f"{s} 6: the cube's part prints with slot 2", grams[2][0] > 0 and control[2][0] == 0,
                     (grams, control))
        self.t.equal(f"{s} 6", self.filaments(), "cube 1 (1, 2) · cylinder 1")

        reply = self.run("settings_set", target="part", object=0, part=1, values={"extruder": None})
        self.t.equal(f"{s} 7: the reply", reply.get("applied", {}).get("extruder"), "1")
        self.t.equal(f"{s} 7", self.filaments(), "cube 1 (1, 1) · cylinder 1")
        self.t.equal(f"{s} 7: the cube's part", self.item("extruder", "part", object=0, part=1)[0],
                     {"value": "1", "effective": "1", "source": "part"})

        self.run("settings_set", target="part", object=0, part=1, values={"extruder": 2})
        self.run("settings_set", target="object", object=1, values={"extruder": 2})
        self.t.equal(f"{s} 8", self.filaments(), "cube 1 (1, 2) · cylinder 2")
        self.t.equal(f"{s} 8: the cube's part", self.item("extruder", "part", object=0, part=1)[0], part_two)
        self.t.equal(f"{s} 8: the cylinder", self.item("extruder", "object", object=1)[0], two)

        reply = self.run("settings_set", target="object", object=0, values={"extruder": 2, "wall_loops": 5})
        self.t.equal(f"{s} 9: the reply", reply.get("applied"), {"extruder": "2", "wall_loops": "5"})
        self.t.equal(f"{s} 9", self.filaments(), "cube 2 (2, 2) · cylinder 2")
        self.t.equal(f"{s} 9: the cube", self.item("extruder", "object", object=0)[0], two)
        self.t.equal(f"{s} 9: the cube's walls", self.item("wall_loops", "object", object=0)[0],
                     {"value": "5", "effective": "5", "source": "object"})
        self.run("undo", history="project")
        self.t.equal(f"{s} 10", self.filaments(), "cube 1 (1, 2) · cylinder 2")
        self.t.equal(f"{s} 10: the cube", self.item("extruder", "object", object=0)[0], cube_part_two)
        self.t.equal(f"{s} 10: the cube's walls", self.item("wall_loops", "object", object=0)[0],
                     {"value": None, "effective": "3", "source": "global"})

        grams, changes = self.slice_ok(f"{s} 11")
        self.t.check(f"{s} 11: both slots print, the cylinder with slot 2 too",
                     grams[2][0] > step6[2][0] and grams[1][0] > 0 and changes > 0, (grams, changes, step6))
        self.t.equal(f"{s} 11", self.filaments(), "cube 1 (1, 2) · cylinder 2")

    def merged(self):
        s = "one slot merged into another"
        self.start(s, [B] * 3)
        for slot, colour in enumerate(("#FF0000", "#00FF00", "#0000FF"), 1):
            self.run("filament_set_colour", filament_slot=slot, colour=colour)
        self.cube("on_two", 80, 128)
        self.cube("on_three", 176, 128)
        self.run("settings_set", target="object", object=0, values={"extruder": 2})
        self.run("settings_set", target="object", object=1, values={"extruder": 3})
        self.idle()
        self.t.equal(f"{s}: the start's filaments", self.filaments(), "on_two 2 · on_three 3")
        self.t.equal(f"{s}: the start's colours", slot_colours(self.c), "#FF0000;#00FF00;#0000FF")

        reply = self.run("filament_delete", filament_slot=2, merge_into=1)
        self.idle()
        self.t.equal(f"{s} 1: the reply", {k: reply.get(k) for k in ("filament_slot", "merged_into", "filament_slots")},
                     {"filament_slot": 2, "merged_into": 1, "filament_slots": 2})
        self.t.equal(f"{s} 1", self.slots(), "0,0")
        self.t.equal(f"{s} 1: the slot after the deleted one moved down, with its objects", self.filaments(),
                     "on_two 1 · on_three 2")
        self.t.equal(f"{s} 1: and its colour", slot_colours(self.c), "#FF0000;#0000FF")
        self.t.equal(f"{s} 1: the presets", presets(self.c)["filaments"], [B, B])

    def save_and_reopen(self, name):
        path = os.path.join(self.folder, name)
        self.run("project_save", path=path, overwrite=True)
        # The new project drops the printer's unsaved edit, which project_open would otherwise ask about; the file
        # brings it back with the printer settings it keeps.
        self.run("project_new", discard_changes=True, preset_changes="discard")
        self.run("project_open", path=path)
        self.idle()

    def project_settings(self):
        s = "the slot colours, the flushing and the project's own settings"
        self.start(s, [B, M])
        # The plate type selector shows for a printer with several plate types: an unsaved edit of the printer
        # preset, which the next start() discards.
        self.run("settings_set", type="printer", values={"support_multi_bed_types": True})
        self.t.equal(f"{s}: the printer has several plate types", self.multi_bed_types(), "1")
        self.add_objects()
        self.run("settings_set", target="part", object=0, part=1, values={"extruder": 2})
        self.run("settings_set", target="object", object=1, values={"extruder": 2})
        self.run("filament_set_colour", filament_slot=1, colour="#111111")
        self.run("filament_set_colour", filament_slot=2, colour="#222222")
        self.run("filament_set_flushing", flush_volumes_matrix=[0, 777, 777, 0], flush_multiplier=1)
        self.run("project_set_settings", values={"curr_bed_type": "Cool Plate", "wipe_tower_x": {"index": 0, "value": 60},
                                                 "wipe_tower_y": {"index": 0, "value": 150}})
        self.idle()
        self.t.equal(f"{s}: the start's filaments", self.filaments(), "cube 1 (1, 2) · cylinder 2")
        self.t.equal(f"{s}: the start's filament presets", presets(self.c)["filaments"], [B, M])
        self.project_is(f"{s}: the start", "#111111;#222222", "1", "0,777,777,0", "Cool Plate", "60,150")

        listed = self.c.call("filament_list")
        self.t.equal(f"{s} 1: no mixed slot", [row["mixed"] for row in listed["filaments"]], [False, False])
        self.t.equal(f"{s} 1: no purge mode to choose", (listed.get("prime_volume_mode"), "prime_volume_modes" in listed),
                     ("Default", False))
        items = {i["key"]: i for i in self.c.call("project_get_settings", keys=["curr_bed_type"])["items"]}
        offered = items["curr_bed_type"]["meta"].get("offered", [])
        self.t.check(f"{s} 1: the plate types offered include Cool Plate and Textured PEI Plate",
                     "Cool Plate" in offered and "Textured PEI Plate" in offered, offered)
        self.project_is(f"{s} 1", "#111111;#222222", "1", "0,777,777,0", "Cool Plate", "60,150")

        reply = self.run("filament_set_colour", filament_slot=1, colour="#ffffff")
        self.t.equal(f"{s} 2: the reply", reply, {"filament_slot": 1, "colour": "#FFFFFF", "changed": True})

        def into_white(matrix):
            m = numbers(matrix)
            return len(m) == 4 and m[0] == 0 and m[3] == 0 and SENTINEL not in (m[1], m[2]) and m[2] > m[1]

        self.project_is(f"{s} 2", "#FFFFFF;#222222", "1", "0,<a>,<b>,0", "Cool Plate", "60,150", into_white)

        self.run("filament_set_flushing", flush_volumes_matrix=[0, 777, 777, 0])
        self.run("filament_set_colour", filament_slot=2, colour="#000000")
        self.run("filament_set_flushing", flush_multiplier=0.7, flush_volumes_matrix={"index": 1, "value": 400})
        self.run("project_set_settings", values={"curr_bed_type": "Textured PEI Plate"})

        def c_worked_out(matrix):
            m = numbers(matrix)
            return len(m) == 4 and m[0] == 0 and m[1] == 400 and m[3] == 0 and m[2] not in (SENTINEL, 400)

        pei = "Textured PEI Plate"
        self.project_is(f"{s} 3", "#FFFFFF;#000000", "0.7", "0,400,<c>,0", pei, "60,150", c_worked_out)
        reply = self.run("filament_set_flushing", flush_volumes_matrix=[0, 400, 300, 0])
        self.t.check(f"{s} 4: the reply applied the matrix", "flush_volumes_matrix" in reply.get("applied", {}), reply)
        self.project_is(f"{s} 4", "#FFFFFF;#000000", "0.7", "0,400,300,0", pei, "60,150")
        reply = self.run("project_set_settings", values={"wipe_tower_x": {"index": 0, "value": 150},
                                                         "wipe_tower_y": {"index": 0, "value": 180}})
        self.t.equal(f"{s} 5: the reply has no note", reply.get("note"), None)
        after = ("#FFFFFF;#000000", "0.7", "0,400,300,0", pei, "150,180")
        self.project_is(f"{s} 5", *after)
        self.run("project_set_settings", values={"wipe_tower_x": {"index": 0, "value": 40}})
        self.project_is(f"{s} 6", "#FFFFFF;#000000", "0.7", "0,400,300,0", pei, "40,180")
        self.run("undo", history="project")
        self.project_is(f"{s} 7: the move was one undo step", *after)

        refusals = [
            (8, "project_set_settings", {"values": {"flush_multiplier": 0.5}}, "belongs to the filament list"),
            (9, "filament_set_flushing", {"flush_multiplier": 0.5, "flush_volumes_matrix": {"index": 0, "value": 10}},
             "flush_volumes_matrix[0] (nozzle 1, from slot 1 to slot 1) must be 0: no flush from a filament to itself"),
            (10, "filament_set_flushing", {"prime_volume_mode": "Saving"},
             "prime_volume_mode: this printer has no purge mode to choose"),
            (11, "filament_set_colour", {"filament_slot": 2, "colour": "white"}, 'Slot 2\'s colour "white" is not #RRGGBB'),
            (12, "project_set_settings", {"values": {"wipe_tower_x": {"index": 0, "value": 300}}},
             "The prime tower of plate 0 would be off the plate"),
        ]
        for step, tool, args, words in refusals:
            self.t.refused(f"{s} {step}: refused", lambda: self.c.call(tool, **args), words=words)
            self.project_is(f"{s} {step}", *after)
        self.t.refused(f"{s} 8: the refusal names filament_set_flushing",
                       lambda: self.c.call("project_set_settings", values={"flush_multiplier": 0.5}),
                       words="filament_set_flushing")
        read = self.c.call("project_get_settings", keys=["filament_colour"])
        self.t.equal(f"{s} 8: project_get_settings points filament_colour to filament_list",
                     (read.get("items"), read.get("unknown_keys"), read.get("type_of")),
                     ([], ["filament_colour"], {"filament_colour": "filament_list"}))
        self.t.equal(f"{s} 8: the slots", self.slots(), "0,0")
        self.t.equal(f"{s} 10: the purge mode", self.c.call("filament_list").get("prime_volume_mode"), "Default")

        # The Klipper printer purges a filament change into the prime tower, so the purges are in tower_g.
        grams13, changes13 = self.slice_ok(f"{s} 13")
        self.t.check(f"{s} 13: each slot prints and purges into the tower, nothing flushed outside it, with filament "
                     f"changes", all(grams13[slot][0] > 0 and grams13[slot][1] == 0 and grams13[slot][2] > 0
                                     for slot in (1, 2)) and changes13 > 0, (grams13, changes13))
        self.project_is(f"{s} 13", *after)

        self.run("filament_set_flushing", flush_volumes_matrix={"index": 1, "value": 200})
        grams14, changes14 = self.slice_ok(f"{s} 14")
        self.project_is(f"{s} 14", "#FFFFFF;#000000", "0.7", "0,200,300,0", pei, "150,180")
        self.t.check(f"{s} 14: slot 2 purges less into the tower", grams14[2][2] < grams13[2][2], (grams14, grams13))
        self.t.check(f"{s} 14: the objects print as after 13, with as many filament changes",
                     all(abs(grams14[slot][0] - grams13[slot][0]) <= 0.01 for slot in (1, 2)) and
                     changes14 == changes13, (grams14, changes14, grams13, changes13))

        self.run("filament_set_flushing", flush_multiplier=1.4)
        grams15, _ = self.slice_ok(f"{s} 15")
        after15 = ("#FFFFFF;#000000", "1.4", "0,200,300,0", pei, "150,180")
        self.project_is(f"{s} 15", *after15)
        self.t.check(f"{s} 15: each slot purges more into the tower",
                     all(grams15[slot][2] > grams14[slot][2] for slot in (1, 2)), (grams15, grams14))

        self.save_and_reopen("filaments.3mf")
        label = f"{s} 16: the project file keeps them"
        self.project_is(label, *after15)
        self.t.equal(f"{label}: the filaments", self.filaments(), "cube 1 (1, 2) · cylinder 2")
        self.t.equal(f"{label}: the presets", presets(self.c)["filaments"], [B, M])
        self.t.equal(f"{label}: the slots", self.slots(), "0,0")
        self.t.equal(f"{label}: the printer's plate types", self.multi_bed_types(), "1")

    @staticmethod
    def red_blue(colour):
        return int(colour[1:3], 16), int(colour[5:7], 16)

    def adding(self):
        s = "adding and reading"
        self.mixed_start(s)
        reply = self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.t.equal(f"{s} 1: the reply", reply, {"added": True, "filament_slot": 4})
        self.mixed_is(f"{s} 1", "0,0,0,1", "4 = 2,3 @ 50,50")
        row = self.row(4)
        self.t.check(f"{s} 1: the mix has a colour and no preset or type of its own",
                     bool(HEX.match(row.get("colour", ""))) and "preset" not in row and "type" not in row, row)
        self.t.equal(f"{s} 1: the physical slots keep their preset", [r.get("preset") for r in self.rows()[:3]], [B] * 3)
        self.t.equal(f"{s} 1: a preset entry per slot, the mixed slot's too",
                     len(self.c.call("app_state")["presets"]["filaments"]), 4)
        self.t.equal(f"{s} 1: the matrix is 4 x 4", len(self.c.call("filament_list")["flush_volumes_matrix"]), 16)

        reply = self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.t.equal(f"{s} 2: a mix the project has is not added again", reply, {"added": False, "filament_slot": 4})
        self.mixed_is(f"{s} 2", "0,0,0,1", "4 = 2,3 @ 50,50")

        reply = self.run("filament_add_mixed", components=[1, 2, 3], ratios=[20, 30, 50])
        self.t.equal(f"{s} 3: the reply", reply, {"added": True, "filament_slot": 5})
        after3 = ("0,0,0,1,1", "4 = 2,3 @ 50,50 · 5 = 1,2,3 @ 20,30,50")
        self.mixed_is(f"{s} 3", *after3)

        self.t.refused(f"{s} 4: components without ratios are refused",
                       lambda: self.c.call("filament_add_mixed", components=[2, 3]), code="components_and_ratios")
        self.mixed_is(f"{s} 4", *after3)

        reply = self.run("filament_add")
        self.t.equal(f"{s} 5: a physical slot goes before the mixes", reply.get("filament_slot"), 4)
        after5 = ("0,0,0,0,1,1", "5 = 2,3 @ 50,50 · 6 = 1,2,3 @ 20,30,50")
        self.mixed_is(f"{s} 5", *after5)

        self.save_delete_open(f"{s} 6", "mixes.3mf", 6, "0,0,0,0,1")
        self.mixed_is(f"{s} 6: the project file keeps its mixes", *after5)

    def no_preset(self):
        s = "a mixed slot has no preset or colour of its own"
        self.mixed_start(s)
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        unchanged = ("0,0,0,1", "4 = 2,3 @ 50,50")
        self.mixed_is(f"{s}: the start", *unchanged)
        code = "mixed_filament_slot"

        self.t.refused(f"{s} 1: settings_get", lambda: self.c.call(
            "settings_get", type="filament", filament_slot=4, keys=["filament_cost"]), code=code)
        self.mixed_is(f"{s} 1", *unchanged)
        self.t.refused(f"{s} 2: settings_set", lambda: self.c.call(
            "settings_set", type="filament", filament_slot=4, values={"filament_cost": 99}), code=code)
        saved = self.c.call("settings_get", type="filament", filament_slot=2, compare=":saved")
        self.t.equal(f"{s} 2: the preset slots 1 to 3 hold has no unsaved change", saved.get("items"), [])
        self.mixed_is(f"{s} 2", *unchanged)
        self.t.refused(f"{s} 3: settings_list", lambda: self.c.call(
            "settings_list", type="filament", filament_slot=4), code=code)
        self.mixed_is(f"{s} 3", *unchanged)
        self.t.refused(f"{s} 4: preset_save", lambda: self.c.call(
            "preset_save", type="filament", filament_slot=4, name="MCP Mixed Slot"), code=code)
        users = [p.get("name") for p in self.c.call("preset_list", type="filament", source="user", limit=500)["items"]]
        self.t.check(f"{s} 4: no preset was saved", "MCP Mixed Slot" not in users, users)
        self.mixed_is(f"{s} 4", *unchanged)

        items = self.c.call("preset_list", type="filament", query="Generic PLA", limit=500)["items"]
        used = [i.get("filament_slots") for i in items if i.get("name") == B]
        self.t.equal(f"{s} 5: the mixed slot does not use its first component's preset", used, [[1, 2, 3]])
        self.mixed_is(f"{s} 5", *unchanged)

        self.t.refused(f"{s} 6: another colour", lambda: self.c.call(
            "filament_set_colour", filament_slot=4, colour="#00FF00"), words="Slot 4 is a mixed filament")
        own = self.row(4)["colour"]
        self.t.check(f"{s} 6: the mix's colour is not set", own != "#00FF00", own)
        self.mixed_is(f"{s} 6", *unchanged)
        reply = self.run("filament_set_colour", filament_slot=4, colour=own.lower())
        self.t.equal(f"{s} 7: its own colour, in any case, changes nothing", reply.get("changed"), False)
        self.mixed_is(f"{s} 7", *unchanged)

        self.t.refused(f"{s} 8: a mixed filament's flushing volume", lambda: self.c.call(
            "filament_set_flushing", flush_volumes_matrix={"index": 3, "value": 50}),
            words="belongs to a mixed filament, which is never flushed")
        entry = self.c.call("filament_list")["flush_volumes_matrix"][3]
        self.t.check(f"{s} 8: entry 3 is not set", entry != 50, entry)
        self.mixed_is(f"{s} 8", *unchanged)

    def changing(self):
        s = "changing a mix"
        self.mixed_start(s, colours=True)
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.cube("on_mix", 100)
        self.run("settings_set", target="object", object=0, values={"extruder": 4})
        self.mixed_is(f"{s}: the start", "0,0,0,1", "4 = 2,3 @ 50,50")
        self.t.equal(f"{s}: the start: the cube on the mix", self.on("on_mix"), 4)

        def edit(**args):
            return self.run("filament_edit_mixed", filament_slot=4, **args)

        reply = edit(components=[2, 3], ratios=[70, 30])
        self.t.equal(f"{s} 1: the reply", reply, {"filament_slot": 4, "changed": True})
        self.mixed_is(f"{s} 1", "0,0,0,1", "4 = 2,3 @ 70,30")
        red, blue = self.red_blue(self.row(4)["colour"])
        self.t.check(f"{s} 1: the blend has more red than blue", red > blue, self.row(4)["colour"])
        self.t.equal(f"{s} 1: the cube stays on the mix", self.on("on_mix"), 4)

        reply = edit(components=[2, 3], ratios=[70, 30])
        self.t.equal(f"{s} 2: the same mix changes nothing", reply.get("changed"), False)
        self.mixed_is(f"{s} 2", "0,0,0,1", "4 = 2,3 @ 70,30")

        edit(components=[2, 3], ratios=[30, 70])
        self.mixed_is(f"{s} 3", "0,0,0,1", "4 = 2,3 @ 30,70")
        red, blue = self.red_blue(self.row(4)["colour"])
        self.t.check(f"{s} 3: the blend has more blue than red", blue > red, self.row(4)["colour"])
        self.t.equal(f"{s} 3: the cube stays on the mix", self.on("on_mix"), 4)

        edit(components=[1, 2, 3], ratios=[20, 30, 50])
        after4 = ("0,0,0,1", "4 = 1,2,3 @ 20,30,50")
        self.mixed_is(f"{s} 4: the slot keeps its number", *after4)
        self.t.equal(f"{s} 4: the cube stays on the mix", self.on("on_mix"), 4)

        for step, args, code in [
                (5, {"filament_slot": 2, "components": [2, 3], "ratios": [50, 50]}, "not_mixed_filament"),
                (6, {"filament_slot": 9, "components": [1, 2, 3], "ratios": [20, 30, 50]}, "no_such_slot"),
                (7, {"filament_slot": 4, "components": [2, 3]}, "components_and_ratios"),
                (8, {"filament_slot": 4, "components": [2, 2], "ratios": [50, 50]}, "duplicate_component")]:
            self.t.refused(f"{s} {step}: refused", lambda: self.c.call("filament_edit_mixed", **args), code=code)
            self.mixed_is(f"{s} {step}", *after4)
            self.t.equal(f"{s} {step}: the cube stays on the mix", self.on("on_mix"), 4)

    def gradient(self):
        s = "a gradient, through the dialogs"
        self.mixed_start(s)
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.mixed_is(f"{s}: the start", "0,0,0,1", "4 = 2,3 @ 50,50")

        reply = self.c.call("filament_edit_mixed", filament_slot=4)
        self.held(f"{s} 1", reply, EDIT)
        self.mixed_is(f"{s} 1", "0,0,0,1", "4 = 2,3 @ 50,50")

        self.set_in_dialog(EDIT, gradient=True, gradient_direction=1, per_part_gradient=True)
        self.click(EDIT, "OK")
        self.t.equal(f"{s} 2: the job's reply", self.finish(f"{s} 2", reply), {"filament_slot": 4, "changed": True})
        self.mixed_is(f"{s} 2", "0,0,0,1", "4 = 2,3 @ 50,50, bottom 3 top 2, per part")

        self.run("filament_edit_mixed", filament_slot=4, components=[3, 2], ratios=[50, 50])
        self.mixed_is(f"{s} 3: a gradient stays for two", "0,0,0,1", "4 = 3,2 @ 50,50, bottom 2 top 3, per part")

        self.run("filament_edit_mixed", filament_slot=4, components=[1, 2, 3], ratios=[20, 30, 50])
        after4 = ("0,0,0,1", "4 = 1,2,3 @ 20,30,50")
        self.mixed_is(f"{s} 4: a gradient goes for three", *after4)

        reply = self.c.call("filament_edit_mixed", filament_slot=4)
        self.held(f"{s} 5", reply, EDIT)
        self.click(EDIT, "Cancel")
        self.t.equal(f"{s} 5: the job's reply", self.finish(f"{s} 5", reply), {"filament_slot": 4, "changed": False})
        self.mixed_is(f"{s} 5", *after4)

        reply = self.c.call("filament_add_mixed")
        self.held(f"{s} 6", reply, ADD)
        self.mixed_is(f"{s} 6", *after4)
        self.set_in_dialog(ADD, gradient=True)
        self.click(ADD, "OK")
        self.t.equal(f"{s} 7: the job's reply", self.finish(f"{s} 7", reply), {"added": True, "filament_slot": 5})
        after7 = ("0,0,0,1,1", "4 = 1,2,3 @ 20,30,50 · 5 = 1,2 @ 50,50, bottom 1 top 2")
        self.mixed_is(f"{s} 7", *after7)

        reply = self.c.call("filament_add_mixed")
        self.held(f"{s} 8", reply, ADD)
        self.click(ADD, "Cancel")
        self.t.equal(f"{s} 8: the job's reply", self.finish(f"{s} 8", reply), {"added": False})
        self.mixed_is(f"{s} 8", *after7)

        self.save_delete_open(f"{s} 9", "gradient.3mf", 5, "0,0,0,1")
        self.mixed_is(f"{s} 9: the project file keeps the gradient", *after7)

    def deleting(self):
        s = "deleting or merging a filament that a mix mixes"
        self.mixed_start(s)
        self.run("filament_add_mixed", components=[1, 2], ratios=[50, 50])
        self.run("filament_add_mixed", components=[2, 3], ratios=[50, 50])
        self.cube("on_one", 70)
        self.cube("on_mix", 130)
        self.run("settings_set", target="object", object=1, values={"extruder": 4})
        start_state = ("0,0,0,1,1", "4 = 1,2 @ 50,50 · 5 = 2,3 @ 50,50")
        self.mixed_is(f"{s}: the start", *start_state)
        self.t.equal(f"{s}: the start: the cubes' filaments", (self.on("on_one"), self.on("on_mix")), (1, 4))

        reply = self.c.call("filament_delete", filament_slot=1, merge_into=4)
        shown = self.held(f"{s} 1", reply, "Warning")
        self.t.check(f"{s} 1: the question says the mix uses the filament",
                     bool(shown) and "uses this physical filament" in shown[0]["message"], shown)
        self.c.call("dialog_answer", button="Cancel")
        self.t.refused(f"{s} 1: the job", lambda: self.c.await_job(reply, timeout=60), code="merge_cancelled")
        self.t.equal(f"{s} 1: no dialog is left open", self.dialogs(), [])
        self.mixed_is(f"{s} 1", *start_state)
        self.t.equal(f"{s} 1: the cube stays on filament 1", self.on("on_one"), 1)

        self.t.refused(f"{s} 2: the merge answered no ahead", lambda: self.c.call(
            "filament_delete", filament_slot=1, merge_into=4, break_mix=False), code="mixed_uses_slot")
        self.t.equal(f"{s} 2: no dialog opened", self.dialogs(), [])
        self.mixed_is(f"{s} 2", *start_state)

        reply = self.run("filament_delete", filament_slot=1, merge_into=4, break_mix=True)
        self.t.equal(f"{s} 3: the reply", {k: reply.get(k) for k in ("filament_slot", "merged_into", "broken_mixed")},
                     {"filament_slot": 1, "merged_into": 3, "broken_mixed": [3]})
        self.t.equal(f"{s} 3: no dialog opened", self.dialogs(), [])
        after3 = ("0,0,1,1", "3 = 0,1 @ 50,50, broken · 4 = 1,2 @ 50,50")
        self.mixed_is(f"{s} 3", *after3)
        self.t.equal(f"{s} 3: the cubes are on the mix", (self.on("on_one"), self.on("on_mix")), (3, 3))

        reply = self.c.call("plate_slice")
        self.t.equal(f"{s} 4: the slice of an object on a broken mix ends in an error",
                     [(p.get("index"), p.get("outcome"), p.get("error")) for p in reply.get("plates", [])],
                     [(0, "error", "A mixed filament has a deleted or mismatched component")])
        self.mixed_is(f"{s} 4", *after3)

        reply = self.run("filament_edit_mixed", filament_slot=3, components=[1, 2], ratios=[50, 50])
        self.t.equal(f"{s} 5: an edit repairs it", reply.get("changed"), True)
        self.mixed_is(f"{s} 5", "0,0,1,1", "3 = 1,2 @ 50,50 · 4 = 1,2 @ 50,50")

        reply = self.run("filament_delete", filament_slot=2)
        self.t.equal(f"{s} 6: the reply names the broken mixes", reply.get("broken_mixed"), [2, 3])
        self.t.equal(f"{s} 6: no dialog opened", self.dialogs(), [])
        after6 = ("0,1,1", "2 = 1,0 @ 50,50, broken · 3 = 1,0 @ 50,50, broken")
        self.mixed_is(f"{s} 6", *after6)

        self.t.refused(f"{s} 7: the last physical slot", lambda: self.c.call("filament_delete", filament_slot=1),
                       code="last_filament_slot")
        self.mixed_is(f"{s} 7", *after6)

        self.t.refused(f"{s} 8: a mix beside one physical slot", lambda: self.c.call("filament_delete", filament_slot=2),
                       code="mixed_list_hidden")
        self.mixed_is(f"{s} 8", *after6)

    def prepare(self):
        """What the checks read beyond each scenario's start(): the filaments the test shows by name hidden from the
        selectors, and the baseline's Simple mode, whose prompt lists slow_down_layer_time, read back. An uninstall
        that changes something shows every filament as the installed set says, hiding one an earlier test showed for
        the app's session, so the install first makes it change something whatever an earlier test left."""
        s = "the start"
        self.t.equal(f"{s}: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [B]})
        self.run("filament_install", names=HIDDEN)
        reply = self.run("filament_uninstall", names=HIDDEN)
        self.t.equal(f"{s}: the filaments shown by name, installed first, are uninstalled",
                     (sorted(reply.get("uninstalled", [])), reply.get("not_installed")), (sorted(HIDDEN), None))
        self.t.equal("Simple mode", self.c.call("app_state").get("mode"), "simple")

    def run_all(self):
        for name, scenario in [("the start", self.prepare),
                               ("hidden filaments", self.hidden), ("one slot, X", self.one_slot_pla),
                               ("one slot, A", self.one_slot_support),
                               ("one slot, no preset named", self.one_slot_forms),
                               ("Auto-flush", self.auto_flush), ("three slots, unsaved changes", self.three_slots_dirty),
                               ("three slots, other forms", self.three_slots_forms),
                               ("three slots, refusals", self.refusals),
                               ("filaments of objects and parts", self.objects_and_parts),
                               ("one slot merged into another", self.merged),
                               ("the project's own settings", self.project_settings),
                               ("adding and reading", self.adding), ("no preset of its own", self.no_preset),
                               ("changing a mix", self.changing), ("a gradient", self.gradient),
                               ("deleting or merging", self.deleting)]:
            self.t.scenario(name, scenario)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-filaments-") as folder:
        return FilamentsTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
