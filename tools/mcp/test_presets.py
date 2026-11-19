#!/usr/bin/env python3
"""The presets of the three types, as the test_presets entry of tools/mcp/test-entries/presets.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_presets.py

Selecting presets by the names their selectors show, hidden ones included; listing them with filters; what a
setting is and where it lives; saving, overwriting, reverting and deleting presets; the unsaved-changes prompt of a
preset switch and of a new project, answered ahead or in the dialog, through the tools and through the plugin API;
a preset kept inside the project; exporting and importing preset files; a printer variant picked by its nozzle
diameter; a preference changed in its dialog; and that the presets selected and an unsaved edit are what the G-code
is sliced with. After every step it reads the selection back, each preset with its unsaved edits, and compares it
whole with the state the entry writes out. It runs on the harness's default printer, the Bambu Lab X1 Carbon 0.4,
on a freshly launched app and data folder, and exits 0 when every check passes.
"""

import json
import os
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import Checks, refusal, run, wait  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures")

X1C = "Bambu Lab X1 Carbon 0.4 nozzle"
X1E = "Bambu Lab X1E 0.4 nozzle"
STD = "0.20mm Standard @BBL X1C"
OPT = "0.16mm Optimal @BBL X1C"
PLA = "Bambu PLA Basic @BBL X1C"
HS = "Generic PLA High Speed @BBL X1C"
# The entry's terms for the presets it names often.
TERMS = {"X1C": X1C, "X1C 0.6": "Bambu Lab X1 Carbon 0.6 nozzle", "X1E": X1E, "A1M": "Bambu Lab A1 mini 0.4 nozzle",
         "Std": STD, "Opt": OPT, "PLA": PLA, "HS": HS}
SHORT = {name: term for term, name in TERMS.items()}

SIMPLE_MODE = "orca_command:mode_simple:orca"
DEVELOPER_MODE = "orca_command:toggle_developer_mode:orca"
PROMPT = "Transfer or discard changes"
SPIRAL = "Spiral mode only works"


# ---- The selection, written as the entry writes it ----

def split_outside(text, sep):
    """`text` split at each `sep` that stands outside brackets and quotes."""
    parts, depth, quoted, start, i = [], 0, False, 0, 0
    while i < len(text):
        ch = text[i]
        if ch == '"':
            quoted = not quoted
        elif not quoted and ch == "[":
            depth += 1
        elif not quoted and ch == "]":
            depth -= 1
        elif not quoted and depth == 0 and text.startswith(sep, i):
            parts.append(text[start:i].strip())
            i += len(sep)
            start = i
            continue
        i += 1
    parts.append(text[start:].strip())
    return parts


def preset_term(term):
    """'Std[wall_loops 5, printer_notes "MCP pin"]' as (name, {key: value}), a term read through TERMS."""
    name, bracket, edits = term.partition("[")
    name = name.strip()
    out = {}
    if bracket:
        for pair in split_outside(edits[:-1], ", "):
            key, _, value = pair.partition(" ")
            out[key] = value[1:-1] if value.startswith('"') else value
    return TERMS.get(name, name), out


def parse_selection(text):
    """'X1C | Std[wall_loops 5] | PLA' as {printer, process, filaments}: each preset (name, {key: value}), the
    filaments one per slot."""
    printer, process, filaments = split_outside(text, " | ")
    return {"printer": preset_term(printer), "process": preset_term(process),
            "filaments": [preset_term(f) for f in split_outside(filaments, ", ")]}


def selection_text(sel):
    """A selection written as the entry writes it, for a failure's detail."""
    def term(preset):
        name, edits = preset
        quoted = [f'{k} "{v}"' if " " in v else f"{k} {v}" for k, v in sorted(edits.items())]
        return SHORT.get(name, name) + (f"[{', '.join(quoted)}]" if quoted else "")
    return " | ".join([term(sel["printer"]), term(sel["process"]), ", ".join(term(f) for f in sel["filaments"])])


def value(client, type, key, preset=None, slot=None):
    """One setting's value as OrcaSlicer writes it: of the preset the tab edits, or of the saved preset named, or of
    a filament slot's."""
    args = {"type": type, "keys": [key], "include_meta": False}
    if preset is not None:
        args["preset"] = preset
    if slot is not None:
        args["filament_slot"] = slot
    return client.call("settings_get", **args)["items"][0]["value"]


def unsaved(client, type, slot=None):
    """The unsaved edits of the preset the tab edits, each key once with its whole value."""
    args = {"type": type, "compare": ":saved", "limit": 500}
    if slot is not None:
        args["filament_slot"] = slot
    keys = []
    for item in client.call("settings_get", **args)["items"]:
        if item["key"] not in keys:
            keys.append(item["key"])
    return {key: value(client, type, key, slot=slot) for key in keys}


def selection(client):
    """The printer, the process and each slot's filament from app_state.presets, each with its unsaved edits."""
    presets = client.call("app_state")["presets"]
    return {"printer": (presets["printer"], unsaved(client, "printer")),
            "process": (presets["process"], unsaved(client, "process")),
            "filaments": [(name, unsaved(client, "filament", slot)) for slot, name in
                          enumerate(presets["filaments"], 1)]}


def listed(client, type, **filters):
    """Every item preset_list gives for the filters, page after page, each whole."""
    items = []
    while True:
        page = client.call("preset_list", type=type, limit=500, offset=len(items), **filters)
        items += page["items"]
        if not page["items"] or len(items) >= page["total"]:
            return items


def dialogs(client):
    """The open dialogs as {title, buttons}, a button by its label."""
    return [{"title": d.get("title"), "buttons": [b.get("label") for b in d.get("buttons", [])]}
            for d in client.call("app_state").get("dialogs", [])]


def plugin_code(body):
    """Code for script_run that runs `body` against the plugin API and returns what it sets as `result`, or what the
    API raised as {raised: {type, message}}."""
    indented = "\n".join("    " + line for line in body.splitlines())
    return ("import orca\ntry:\n" + indented + "\nexcept Exception as e:\n"
            "    result = {'raised': {'type': type(e).__name__, 'message': str(e)}}\n")


def api_switch(type, name=None):
    """A switch through orca.host.preset.<type>.begin_update(name).commit(), returning the commit's target_name and
    selection_changed."""
    arg = "" if name is None else json.dumps(name)
    return (f"r = orca.host.preset.{type}.begin_update({arg}).commit()\n"
            "result = {'target_name': r.target_name, 'selection_changed': r.selection_changed}")


def gcode_config(path):
    """The `; key = value` lines between `; CONFIG_BLOCK_START` and `; CONFIG_BLOCK_END` of a G-code file, quotes
    around a value removed."""
    config, inside = {}, False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line == "; CONFIG_BLOCK_START":
                inside = True
            elif line == "; CONFIG_BLOCK_END":
                break
            elif inside and line.startswith("; ") and " = " in line:
                key, _, val = line[2:].partition(" = ")
                if len(val) >= 2 and val[0] == val[-1] == '"':
                    val = val[1:-1]
                config[key.strip()] = val
    return config


def gcode_heights(path):
    """The heights of a G-code file's `; Z_HEIGHT:` lines, in order."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return [float(line.split(":", 1)[1]) for line in f if line.startswith("; Z_HEIGHT:")]


# ---- The switching rows: edit, type, name, how, selection after ----
# `how` is preset_changes ("transfer", "discard"), the prompt's answer when the call asks ("Transfer", "Discard",
# "cancel"), or None for a call with no edit to ask about.
ROWS = {
    1: (None, "process", OPT, None, "X1C | Opt | PLA"),
    2: ("process", "process", OPT, "transfer", "X1C | Opt[wall_loops 5] | PLA"),
    3: ("process", "process", OPT, "discard", "X1C | Opt | PLA"),
    4: ("process", "process", OPT, "Transfer", "X1C | Opt[wall_loops 5] | PLA"),
    5: ("process", "process", OPT, "Discard", "X1C | Opt | PLA"),
    6: ("process", "process", OPT, "cancel", "X1C | Std[wall_loops 5] | PLA"),
    7: ("process", "process", STD, "cancel", "X1C | Std[wall_loops 5] | PLA"),
    8: (None, "process", "No Such Process", None, "X1C | Std | PLA"),
    9: (None, "process", "0.16mm Optimal", None, "X1C | Std | PLA"),
    10: (None, "process", "0.20mm Standard @BBL A1", None, "X1C | Std | PLA"),
    11: (None, "printer", X1E, None, "X1E | Std | PLA"),
    12: ("printer", "printer", X1E, "transfer", 'X1C[printer_notes "MCP pin"] | Std | PLA'),
    13: ("printer", "printer", X1E, "discard", "X1E | Std | PLA"),
    14: ("printer", "printer", X1E, "Discard", "X1E | Std | PLA"),
    15: ("printer", "printer", X1E, "cancel", 'X1C[printer_notes "MCP pin"] | Std | PLA'),
    16: ("process", "printer", X1E, "transfer", "X1E | Std[wall_loops 5] | PLA"),
    17: ("process", "printer", X1E, "discard", "X1E | Std | PLA"),
    18: ("process", "printer", X1E, "Transfer", "X1E | Std[wall_loops 5] | PLA"),
    19: ("process", "printer", X1E, "cancel", "X1E | Std | PLA"),
    20: (None, "printer", "No Such Printer", None, "X1C | Std | PLA"),
    21: (None, "printer", "Bambu Lab X1 Carbon", None, "X1C | Std | PLA"),
}
# The rows refused, by code and words (None where the entry gives the code alone).
REFUSALS = {
    6: ("selection_cancelled", 'Selecting "0.16mm Optimal @BBL X1C" was cancelled.'),
    7: ("selection_cancelled", None),
    8: ("preset_not_offered", 'The preset selector does not offer "No Such Process".'),
    9: ("preset_not_offered", 'The preset selector does not offer "0.16mm Optimal".'),
    10: ("preset_incompatible", '"0.20mm Standard @BBL A1" is not compatible with the selected printer.'),
    12: ("transfer_not_offered", "The unsaved changes cannot be transferred: a printer's changes stay with it."),
    15: ("selection_cancelled", None),
    20: ("preset_not_offered", None),
    21: ("preset_not_offered", None),
}


class PresetsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def value(self, type, key, preset=None, slot=None):
        return value(self.c, type, key, preset, slot)

    def listed(self, type, **filters):
        return listed(self.c, type, **filters)

    def names(self, type, **filters):
        return [item["name"] for item in self.listed(type, **filters)]

    def dialogs(self):
        return dialogs(self.c)

    def selection_is(self, label, text, **placeholders):
        """selection() against the entry's row, whole. A name written <X> is checked by placeholders[X], a
        predicate on the name."""
        got, want = selection(self.c), parse_selection(text)

        def same(g, w):
            name = w[0]
            if name.startswith("<") and name.endswith(">"):
                return placeholders[name[1:-1]](g[0]) and g[1] == w[1]
            return g == w

        ok = (same(got["printer"], want["printer"]) and same(got["process"], want["process"]) and
              len(got["filaments"]) == len(want["filaments"]) and
              all(same(g, w) for g, w in zip(got["filaments"], want["filaments"])))
        return self.t.check(f"{label}: the selection", ok, f"got {selection_text(got)}, expected {text}")

    def refused(self, label, call, code=None, words=None):
        """Checks.refused, giving the refusal's details ({} without any) as well."""
        caught = {}

        def wrapped():
            try:
                return call()
            except orca_mcp.McpError as e:
                caught["details"] = refusal(e)[1]
                raise

        self.t.refused(label, wrapped, code=code, words=words)
        return caught.get("details", {})

    def ask(self, label, tool, answer, **args):
        """Makes a call the unsaved-changes prompt holds, reads the dialogs that hold it and answers with `answer`.
        Gives a function that waits for the call's job and returns its reply (raising what refuses it), and the
        dialogs."""
        reply = self.c.call(tool, **args)
        if not self.t.equal(f"{label}: a dialog holds the call", reply.get("status"), "blocked_by_dialog"):
            return (lambda: reply), self.dialogs()
        shown = self.dialogs()
        self.c.call("dialog_answer", button=answer)
        return (lambda: wait(self.c, "job", job=reply["job"])), shown

    def answered_by_rule(self, tool, **args):
        """A call whose dialog an auto-answer rule answers. The rule presses its button once the dialog's modal loop
        runs, so the call can reply blocked_by_dialog first; its job then ends once the rule has answered."""
        reply = self.c.call(tool, **args)
        if isinstance(reply, dict) and reply.get("job") and reply.get("status") in ("running", "blocked_by_dialog"):
            reply = wait(self.c, "job", job=reply["job"])
        return reply

    def plugin(self, body):
        """What `body` returns through the plugin API, run with script_run."""
        return self.script_result(self.run("script_run", code=plugin_code(body)))

    def script_result(self, reply):
        if reply.get("status") != "finished":
            raise RuntimeError(f"script_run ended {reply.get('status')}: {reply}")
        return reply["result"]

    def preference(self, key):
        return self.c.call("preferences_get", keys=[key])["items"][0]["value"]

    def developer_mode(self):
        self.run("action_run", id=DEVELOPER_MODE)

    def rule_uses(self, match):
        for rule in self.c.call("app_state").get("auto_answer_rules", []):
            if rule.get("match") == match:
                return rule.get("uses", 0)
        return None

    def select(self, type, name, slot=None, **args):
        if slot is not None:
            args["filament_slot"] = slot
        return self.run("preset_select", type=type, name=name, **args)

    def set(self, type, **args):
        return self.run("settings_set", type=type, **args)

    def wait_dialog(self, title, timeout_s=30):
        """The open dialogs whose title holds `title`, as dialog_answer's dialog.title matches one, once one is open;
        a wait that runs out of time fails. app_wait's dialog condition cannot serve where another dialog is open
        already."""
        deadline = time.monotonic() + timeout_s
        while True:
            found = [d for d in self.c.call("app_state").get("dialogs", []) if title in d.get("title", "")]
            if found:
                return found
            if time.monotonic() > deadline:
                raise TimeoutError(f"no dialog titled {title!r} opened within {timeout_s} s")
            time.sleep(0.2)

    def close_left_dialogs(self):
        """A dialog an earlier failure left open is closed, so that the next scenario can start; the failure has
        been reported where it happened."""
        for d in reversed(self.dialogs()):  # the topmost first
            print(f"     (closing the dialog {d['title']!r} left open)", flush=True)
            self.c.call("dialog_answer", dialog={"title": d["title"]}, button="cancel")

    def to_start(self):
        """project_new dropping every unsaved edit, then X1C, Std and PLA in slot 1 where others are selected."""
        self.close_left_dialogs()
        self.run("project_new", discard_changes=True, preset_changes="discard")
        presets = self.c.call("app_state")["presets"]
        if presets["printer"] != X1C:
            self.select("printer", X1C, preset_changes="discard")
        if self.c.call("app_state")["presets"]["process"] != STD:
            self.select("process", STD, preset_changes="discard")
        if self.c.call("app_state")["presets"]["filaments"][0] != PLA:
            self.select("filament", PLA, slot=1, preset_changes="discard")

    def start(self, name):
        self.to_start()
        self.selection_is(f"{name}: the start", "X1C | Std | PLA")
        state = self.c.call("app_state")
        self.t.equal(f"{name}: the start: one filament slot and no objects",
                     (len(state["presets"]["filaments"]), state["objects"]), (1, 0))
        self.t.equal(f"{name}: the start: Simple mode", state["mode"], "simple")
        self.t.equal(f"{name}: the start: Developer mode is off", self.preference("developer_mode"), False)

    # ---- Scenarios ----

    def hidden(self):
        s = "hidden presets"
        self.start(s)
        self.t.equal(f"{s} 1: the printers listed", self.names("printer"), [X1C])
        self.t.equal(f"{s} 1: the hidden filament is not listed", self.names("filament", query=HS), [])
        items = self.listed("filament", query=HS, include_uninstalled=True)
        self.t.equal(f"{s} 1: with include_uninstalled it is listed, not installed",
                     [(i["name"], i.get("installed")) for i in items], [(HS, False)])
        self.selection_is(f"{s} 1", "X1C | Std | PLA")
        reply = self.select("filament", HS, slot=1)
        self.t.equal(f"{s} 2: the reply's selected and shown", (reply.get("selected"), reply.get("shown")), (HS, HS))
        items = self.listed("filament", query=HS)
        self.t.equal(f"{s} 2: it is listed, selected, with no installed field",
                     [(i["name"], i.get("selected"), "installed" in i) for i in items], [(HS, True, False)])
        self.selection_is(f"{s} 2", "X1C | Std | HS")
        reply = self.select("filament", "Bambu PLA Basic", slot=1)
        self.t.equal(f"{s} 3: the reply's selected, and no shown", (reply.get("selected"), "shown" in reply),
                     (PLA, False))
        self.selection_is(f"{s} 3", "X1C | Std | PLA")
        reply = self.select("printer", X1E)
        self.t.equal(f"{s} 4: the reply's shown", reply.get("shown"), X1E)
        self.t.equal(f"{s} 4: no dialog opened", self.dialogs(), [])
        self.selection_is(f"{s} 4", "X1E | Std | PLA")
        self.select("printer", X1C)
        self.selection_is(f"{s} 5", "X1C | Std | PLA")
        reply = self.select("printer", TERMS["A1M"], preset_changes="discard")
        self.t.equal(f"{s} 6: the reply's shown", reply.get("shown"), TERMS["A1M"])

        def a1m_filament(name):
            system = [i.get("system") for i in self.listed("filament", query=name) if i["name"] == name]
            return name.endswith(" @BBL A1M") and system == [True]

        self.selection_is(f"{s} 6: slot 1 takes a system filament of the A1 mini",
                          "A1M | 0.20mm Standard @BBL A1M | <F>", F=a1m_filament)
        self.select("printer", X1C, preset_changes="discard")
        self.select("process", STD, preset_changes="discard")
        self.select("filament", PLA, slot=1)
        self.selection_is(f"{s} 7", "X1C | Std | PLA")

    def listing(self):
        s = "listing presets"
        self.start(s)
        a1m_pla = "Bambu PLA Basic @BBL A1M"
        x1c02 = "Bambu Lab X1 Carbon 0.2 nozzle"
        pla02 = "Bambu PLA Basic @BBL X1C 0.2 nozzle"
        filters = {"vendor": "Bambu Lab", "filament_type": "PLA", "include_uninstalled": True}
        first = self.listed("filament", **filters)
        names = [i["name"] for i in first]
        self.t.check(f"{s} 1: PLA is listed and the A1 mini's is not", PLA in names and a1m_pla not in names, names)
        self.t.equal(f"{s} 1: every item is a Bambu Lab PLA",
                     [i for i in first if (i.get("vendor"), i.get("filament_type")) != ("Bambu Lab", "PLA")], [])
        self.selection_is(f"{s} 1", "X1C | Std | PLA")
        self.t.equal(f"{s} 2: the filters in lower case list the same",
                     self.names("filament", vendor="bambu lab", filament_type="pla", include_uninstalled=True), names)
        self.selection_is(f"{s} 2", "X1C | Std | PLA")
        more = self.listed("filament", include_incompatible=True, **filters)
        by_name = {i["name"]: i for i in more}
        self.t.check(f"{s} 3: every name of step 1 and more", set(names) < set(by_name), sorted(set(names) - set(by_name)))
        self.t.equal(f"{s} 3: the A1 mini's PLA is listed incompatible",
                     by_name.get(a1m_pla, {}).get("compatible"), False)
        self.t.equal(f"{s} 3: every item of step 1 is there without a compatible field",
                     [i for i in first if by_name.get(i["name"]) != i], [])
        self.t.equal(f"{s} 3: what step 1 leaves out does not fit the X1C",
                     [n for n, i in by_name.items() if n not in names and i.get("compatible") is not False], [])
        self.selection_is(f"{s} 3", "X1C | Std | PLA")
        printers = self.listed("printer", include_uninstalled=True)
        p = {i["name"]: i for i in printers}
        self.t.equal(f"{s} 4: the X1C 0.2 nozzle is listed, not installed", p.get(x1c02, {}).get("installed"), False)
        self.t.equal(f"{s} 4: every system printer has a vendor, a model and a nozzle",
                     [i["name"] for i in printers if i.get("system") and
                      not all(i.get(k) for k in ("vendor", "printer_model", "nozzle"))], [])
        x1c = p.get(X1C, {})
        self.t.equal(f"{s} 4: the X1C item",
                     {k: x1c.get(k) for k in ("vendor", "printer_model", "nozzle", "selected")},
                     {"vendor": "BBL", "printer_model": "Bambu Lab X1 Carbon", "nozzle": "0.4", "selected": True})
        self.selection_is(f"{s} 4", "X1C | Std | PLA")
        for_02 = self.names("filament", printer=x1c02, include_uninstalled=True)
        for_x1c = self.names("filament", include_uninstalled=True)
        self.t.equal(f"{s} 5: the 0.2 nozzle's PLA fits it alone, and PLA the X1C alone",
                     (pla02 in for_02, pla02 in for_x1c, PLA in for_02, PLA in for_x1c), (True, False, False, True))
        self.selection_is(f"{s} 5", "X1C | Std | PLA")
        items = {i["name"]: i for i in self.listed("filament", printer=x1c02, include_uninstalled=True,
                                                   include_incompatible=True)}
        self.t.equal(f"{s} 6: compatibility is judged against the printer named",
                     (items.get(PLA, {}).get("compatible"), pla02 in items and "compatible" in items[pla02]),
                     (False, False))
        self.selection_is(f"{s} 6", "X1C | Std | PLA")
        self.t.refused(f"{s} 7: a printer filter on printers is refused",
                       lambda: self.c.call("preset_list", type="printer", printer=x1c02),
                       words="printer lists the filaments or processes that fit a printer preset, not printers")
        self.selection_is(f"{s} 7", "X1C | Std | PLA")
        self.t.refused(f"{s} 8: a filament type on processes is refused",
                       lambda: self.c.call("preset_list", type="process", filament_type="PLA"),
                       words="filament_type filters filaments (type filament)")
        self.selection_is(f"{s} 8", "X1C | Std | PLA")

    def settings(self):
        s = "what a setting is"
        self.start(s)
        reply = self.c.call("settings_get", type="process", keys=["seam_slope_type", "no_such_key"])
        items = reply["items"]
        self.t.equal(f"{s} 1: one item", [i["key"] for i in items], ["seam_slope_type"])
        meta = items[0]["meta"] if items else {}
        self.t.check(f"{s} 1: the description speaks of the scarf joint", "scarf joint" in meta.get("description", ""),
                     meta.get("description"))
        self.t.equal(f"{s} 1: where it is and its second label",
                     (meta.get("where"), (meta.get("labels") or [None, None])[1]), ("Quality > Seam", "Contour"))
        self.t.equal(f"{s} 1: the unknown keys", reply.get("unknown_keys"), ["no_such_key"])
        self.selection_is(f"{s} 1", "X1C | Std | PLA")
        items = self.c.call("settings_get", type="printer", query="ooze during long travel")["items"]
        self.t.equal(f"{s} 2: the first match", (items[0]["key"], items[0]["meta"].get("where")) if items else None,
                     ("retraction_length", "Extruder > Retraction"))
        self.selection_is(f"{s} 2", "X1C | Std | PLA")
        items = self.c.call("settings_get", query="ooze during long travel")["items"]
        self.t.equal(f"{s} 3: the first two matches, the filament's first",
                     [(i["key"], i["type"], i["value"]) for i in items[:2]],
                     [("filament_retraction_length", "filament", "nil,0.4"), ("retraction_length", "printer", "0.8,0.8")])
        metas = [(i["meta"].get("label"), i["meta"].get("description")) for i in items[:2]]
        self.t.check(f"{s} 3: the two share their label and description", len(metas) == 2 and metas[0] == metas[1],
                     metas)
        self.selection_is(f"{s} 3", "X1C | Std | PLA")
        reply = self.c.call("settings_get", keys=["nozzle_temperature", "layer_height", "nozzle_diameter",
                                                  "curr_bed_type", "no_such_key"], include_meta=False)
        self.t.equal(f"{s} 4: each key read from its type", [(i["key"], i["type"], i["value"]) for i in reply["items"]],
                     [("nozzle_temperature", "filament", "220,220"), ("layer_height", "process", "0.2"),
                      ("nozzle_diameter", "printer", "0.4")])
        self.t.equal(f"{s} 4: the unknown keys and where they are",
                     (reply.get("unknown_keys"), reply.get("type_of")),
                     (["curr_bed_type", "no_such_key"], {"curr_bed_type": "project"}))
        self.selection_is(f"{s} 4", "X1C | Std | PLA")
        reply = self.c.call("settings_get", type="process", keys=["nozzle_temperature"])
        self.t.equal(f"{s} 5: no item, and the type that has it", (reply["items"], reply.get("type_of")),
                     ([], {"nozzle_temperature": "filament"}))
        self.selection_is(f"{s} 5", "X1C | Std | PLA")
        self.t.refused(f"{s} 6: a filament key set as a process key is refused",
                       lambda: self.c.call("settings_set", values={"nozzle_temperature": 215}),
                       words='"nozzle_temperature" is a filament setting: settings_set takes it with type "filament"')
        self.t.equal(f"{s} 6: the nozzle temperature", self.value("filament", "nozzle_temperature"), "220,220")
        self.selection_is(f"{s} 6", "X1C | Std | PLA")
        group = self.settings_pages(type="process", where="Quality > Seam")
        whole = {i["key"]: i for i in self.settings_pages(type="process", include_meta=True)}
        in_group = sorted(k for k, i in whole.items() if i.get("meta", {}).get("where") == "Quality > Seam")
        keys = sorted(i["key"] for i in group)
        self.t.equal(f"{s} 7: the group lists exactly the keys there", keys, in_group)
        self.t.check(f"{s} 7: seam_position and seam_slope_type among them",
                     {"seam_position", "seam_slope_type"} <= set(keys), keys)
        self.t.equal(f"{s} 7: each item has a description, and the whole listing's value",
                     [i["key"] for i in group if not i.get("meta", {}).get("description") or
                      i.get("value") != whole.get(i["key"], {}).get("value")], [])
        self.selection_is(f"{s} 7", "X1C | Std | PLA")
        meta = self.c.call("settings_get", type="process", keys=["fuzzy_skin"])["items"][0]["meta"]
        self.t.equal(f"{s} 8: the second value and its label", (meta["values"][1], meta["labels"][1]),
                     ("external", "Contour"))
        self.selection_is(f"{s} 8", "X1C | Std | PLA")

    def settings_pages(self, **args):
        items = []
        while True:
            page = self.c.call("settings_list", offset=len(items), **args)
            items += page["items"]
            if not page["items"] or len(items) >= page.get("total", len(items)):
                return items

    def saving(self):
        s = "saving a filament"
        self.start(s)
        self.set("filament", values={"filament_cost": 21.5, "slow_down_layer_time": 6, "temperature_vitrification": 50})
        self.selection_is(f"{s} 1",
                          "X1C | Std | PLA[filament_cost 21.5, slow_down_layer_time 6, temperature_vitrification 50]")
        reply = self.run("preset_save", type="filament", name="MCP Test PLA")
        self.t.equal(f"{s} 2: the reply", {k: reply.get(k) for k in ("saved", "user", "project", "inherits", "selected",
                                                                     "filament_slots")},
                     {"saved": "MCP Test PLA", "user": True, "project": False, "inherits": PLA, "selected": True,
                      "filament_slots": [1]})
        items = self.listed("filament", query="MCP Test PLA")
        self.t.equal(f"{s} 2: listed once, selected, user, not modified, inheriting PLA",
                     [(i["name"], i.get("selected"), i.get("user"), i.get("modified", False), i.get("inherits"))
                      for i in items], [("MCP Test PLA", True, True, False, PLA)])
        self.t.equal(f"{s} 2: the saved preset's values",
                     [self.value("filament", k, preset="MCP Test PLA")
                      for k in ("filament_cost", "slow_down_layer_time", "temperature_vitrification")], ["21.5", "6", "50"])
        printers = self.value("filament", "compatible_printers", preset="MCP Test PLA")
        self.t.equal(f"{s} 2: it fits the printers of the preset it was saved from",
                     sorted(p.strip().strip('"') for p in printers.split(";") if p.strip()),
                     sorted([X1C, X1E, "Bambu Lab X1E 0.6 nozzle"]))
        self.selection_is(f"{s} 2", "X1C | Std | MCP Test PLA")
        self.t.equal(f"{s} 3: the user filaments", self.names("filament", source="user"), ["MCP Test PLA"])
        self.t.equal(f"{s} 3: no system filament of the name",
                     self.names("filament", source="system", query="MCP Test PLA"), [])
        self.t.equal(f"{s} 3: no filament inside the project", self.names("filament", source="project"), [])
        self.selection_is(f"{s} 3", "X1C | Std | MCP Test PLA")
        self.set("filament", values={"filament_cost": 22})
        self.run("preset_save", type="filament", name="MCP Test PLA")
        reply = self.c.call("settings_get", type="filament", compare=":parent",
                            keys=["filament_cost", "slow_down_layer_time", "nozzle_temperature"])
        self.t.equal(f"{s} 4: against PLA", (reply.get("right"), reply.get("total"),
                                             [(i["key"], i["value"], i["other"]) for i in reply["items"]]),
                     (PLA, 2, [("filament_cost", "22", "15.99"), ("slow_down_layer_time", "6", "4")]))
        self.selection_is(f"{s} 4", "X1C | Std | MCP Test PLA")
        self.set("filament", values={"filament_cost": 23})
        reply = self.run("preset_save", type="filament", name="MCP Test PLA 2")
        self.t.equal(f"{s} 5: the reply's slots", reply.get("filament_slots"), [1])
        self.t.equal(f"{s} 5: MCP Test PLA is listed, not selected",
                     [i.get("selected", False) for i in self.listed("filament", query="MCP Test PLA")
                      if i["name"] == "MCP Test PLA"], [False])
        self.t.equal(f"{s} 5: MCP Test PLA's cost", self.value("filament", "filament_cost", preset="MCP Test PLA"), "22")
        self.selection_is(f"{s} 5", "X1C | Std | MCP Test PLA 2")
        self.t.refused(f"{s} 6: saving over another preset is refused",
                       lambda: self.c.call("preset_save", type="filament", name="MCP Test PLA"),
                       words='Preset "MCP Test PLA" already exists; pass overwrite: true to replace it')
        self.t.equal(f"{s} 6: MCP Test PLA's cost", self.value("filament", "filament_cost", preset="MCP Test PLA"), "22")
        self.selection_is(f"{s} 6", "X1C | Std | MCP Test PLA 2")
        reply = self.run("preset_save", type="filament", name="MCP Test PLA", overwrite=True)
        self.t.equal(f"{s} 7: the reply's selected", reply.get("selected"), True)
        self.t.equal(f"{s} 7: the cost, edited and saved",
                     (self.value("filament", "filament_cost"),
                      self.value("filament", "filament_cost", preset="MCP Test PLA")), ("23", "23"))
        self.selection_is(f"{s} 7", "X1C | Std | MCP Test PLA")

    def reverting(self):
        s = "dropping and reverting edits"
        self.start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        self.set("process", values={"wall_loops": 7, "sparse_infill_density": "37%"})
        self.selection_is(f"{s} 1", "X1C | Std[sparse_infill_density 37%, wall_loops 7] | MCP Test PLA")
        reply = self.set("process", reset_all=True, values={"wall_loops": 7})
        self.t.equal(f"{s} 2: the reply", (sorted(reply.get("reverted", [])), reply.get("applied")),
                     (["sparse_infill_density", "wall_loops"], {"wall_loops": "7"}))
        self.t.equal(f"{s} 2: the infill density", self.value("process", "sparse_infill_density"), "15%")
        self.selection_is(f"{s} 2", "X1C | Std[wall_loops 7] | MCP Test PLA")
        reply = self.set("process", reset_all=True)
        self.t.equal(f"{s} 3: the reply's reverted", reply.get("reverted"), ["wall_loops"])
        self.t.equal(f"{s} 3: the wall loops", self.value("process", "wall_loops"), "2")
        self.selection_is(f"{s} 3", "X1C | Std | MCP Test PLA")
        reply = self.set("process", reset_all=True)
        self.t.equal(f"{s} 4: the reply's reverted", reply.get("reverted"), [])
        self.selection_is(f"{s} 4", "X1C | Std | MCP Test PLA")
        self.set("process", values={"wall_loops": 6})
        self.t.refused(f"{s} 5: a bad value fails before the reset",
                       lambda: self.c.call("settings_set", type="process", reset_all=True, values={"wall_loops": "six"}),
                       words="Invalid value for wall_loops")
        self.selection_is(f"{s} 5", "X1C | Std[wall_loops 6] | MCP Test PLA")
        after6 = "X1C | Std[sparse_infill_density 25%] | MCP Test PLA"
        self.set("process", reset_all=True, values={"wall_loops": 5, "sparse_infill_density": "25%"})
        reply = self.set("process", values={"wall_loops": None})
        self.t.equal(f"{s} 6: null takes the key back to its saved value",
                     (reply.get("applied"), reply.get("changed")), ({"wall_loops": "2"}, {"wall_loops": "2"}))
        self.selection_is(f"{s} 6", after6)
        self.t.refused(f"{s} 7: reset_to without a null is refused",
                       lambda: self.c.call("settings_set", type="process", values={"wall_loops": 3}, reset_to="parent"),
                       words="reset_to applies to the keys given as null")
        self.t.equal(f"{s} 7: the wall loops", self.value("process", "wall_loops"), "2")
        self.selection_is(f"{s} 7", after6)
        reply = self.set("filament", values={"filament_cost": None}, reset_to="parent")
        self.t.equal(f"{s} 8: the parent's cost", reply.get("applied"), {"filament_cost": "15.99"})
        self.t.equal(f"{s} 8: no difference from the parent",
                     self.c.call("settings_get", type="filament", compare=":parent", keys=["filament_cost"])["total"], 0)
        self.selection_is(f"{s} 8", "X1C | Std[sparse_infill_density 25%] | MCP Test PLA[filament_cost 15.99]")
        reply = self.set("filament", values={"filament_cost": None})
        self.t.equal(f"{s} 9: the saved cost", reply.get("applied"), {"filament_cost": "23"})
        self.selection_is(f"{s} 9", after6)
        item = self.c.call("settings_get", type="process", keys=["outer_wall_speed"])["items"][0]
        self.t.equal(f"{s} 10: the variants, the one in use and the value",
                     (item["meta"].get("variants"), item["meta"].get("active"), item["value"]),
                     (["Direct Drive Standard", "Direct Drive High Flow"], [0], "200,350"))
        self.selection_is(f"{s} 10", after6)
        reply = self.set("process", values={"outer_wall_speed": 250})
        self.t.equal(f"{s} 11: one value writes the entry in use", reply.get("applied"), {"outer_wall_speed": "250,350"})
        after11 = "X1C | Std[outer_wall_speed 250,350, sparse_infill_density 25%] | MCP Test PLA"
        self.selection_is(f"{s} 11", after11)
        self.t.refused(f"{s} 12: three values for two variants are refused",
                       lambda: self.c.call("settings_set", type="process", values={"outer_wall_speed": [1, 2, 3]}),
                       words="outer_wall_speed holds 2 values, one per extruder variant")
        self.selection_is(f"{s} 12", after11)
        reply = self.set("process", values={"outer_wall_speed": {"index": 1, "value": 300}})
        self.t.equal(f"{s} 13: the reply", (reply.get("applied"), reply.get("changed")),
                     ({"outer_wall_speed": "250,300"}, {"outer_wall_speed": "250,300"}))
        after13 = "X1C | Std[outer_wall_speed 250,300, sparse_infill_density 25%] | MCP Test PLA"
        self.selection_is(f"{s} 13", after13)
        reply = self.set("filament", values={"nozzle_temperature": {"index": 0, "value": 215}})
        self.t.equal(f"{s} 14: the reply's applied", reply.get("applied"), {"nozzle_temperature": "215,220"})
        reply = self.c.call("settings_get", type="filament", compare=":saved", keys=["nozzle_temperature"])
        self.t.equal(f"{s} 14: filaments are compared entry by entry",
                     (reply["total"], [(i.get("index"), i["value"], i["other"]) for i in reply["items"]]),
                     (1, [(0, "215", "220")]))
        self.selection_is(f"{s} 14", "X1C | Std[outer_wall_speed 250,300, sparse_infill_density 25%] | "
                                     "MCP Test PLA[nozzle_temperature 215,220]")
        reply = self.set("filament", reset_all=True)
        self.t.equal(f"{s} 15: the reply's changed", reply.get("changed"), {"nozzle_temperature": "220,220"})
        self.selection_is(f"{s} 15", after13)
        self.run("dialog_auto_answer", rules=[{"match": SPIRAL, "button": "yes"}])
        self.t.equal(f"{s} 16: the spiral rule has answered nothing yet", self.rule_uses(SPIRAL), 0)
        reply = self.answered_by_rule("settings_set", type="process", values={"spiral_mode": True})
        self.t.equal(f"{s} 16: the spiral mode question was asked once", self.rule_uses(SPIRAL), 1)
        changed = reply.get("changed", {})
        self.t.equal(f"{s} 16: the reply's applied", reply.get("applied"), {"spiral_mode": "1"})
        self.t.equal(f"{s} 16: the reply's changed", {k: changed.get(k) for k in
                                                      ("spiral_mode", "wall_loops", "top_shell_layers",
                                                       "sparse_infill_density")},
                     {"spiral_mode": "1", "wall_loops": "1", "top_shell_layers": "0", "sparse_infill_density": "0%"})
        edits = dict({"outer_wall_speed": "250,300"}, **changed)
        got = selection(self.c)
        self.t.check(f"{s} 16: the selection", got["printer"] == (X1C, {}) and got["process"] == (STD, edits) and
                     got["filaments"] == [("MCP Test PLA", {})],
                     f"got {selection_text(got)}, expected the process's edits {edits}")
        self.run("dialog_auto_answer", rules=[])
        reply = self.set("process", reset_all=True)
        self.t.equal(f"{s} 17: the reply's spiral mode", reply.get("changed", {}).get("spiral_mode"), "0")
        self.selection_is(f"{s} 17", "X1C | Std | MCP Test PLA")

    def not_selected(self):
        s = "reading presets that are not selected"
        self.start(s)
        after1 = "X1C | Std[sparse_infill_density 25%, wall_loops 5] | PLA"
        reply = self.set("process", reset_all=True, values={"wall_loops": 5, "sparse_infill_density": "25%"})
        self.t.equal(f"{s} 1: the reply's changed", reply.get("changed"),
                     {"wall_loops": "5", "sparse_infill_density": "25%"})
        self.selection_is(f"{s} 1", after1)
        reply = self.c.call("settings_get", type="process", compare=":saved")
        self.t.equal(f"{s} 2: the unsaved edits", (reply.get("total"), reply.get("left"),
                                                   [(i["key"], i["value"], i["other"]) for i in reply["items"]]),
                     (2, "0.20mm Standard @BBL X1C (edited)",
                      [("sparse_infill_density", "25%", "15%"), ("wall_loops", "5", "2")]))
        self.selection_is(f"{s} 2", after1)
        self.t.equal(f"{s} 3: Std as saved, and Opt's layer height",
                     (self.value("process", "wall_loops", preset=STD), self.value("process", "layer_height", preset=OPT)),
                     ("2", "0.16"))
        self.selection_is(f"{s} 3: reading Opt did not select it", after1)
        reply = self.c.call("settings_get", type="process", compare=OPT, keys=["layer_height", "wall_loops"])
        self.t.equal(f"{s} 4: against Opt", (reply.get("right"), [(i["key"], i["value"], i["other"])
                                                                  for i in reply["items"]]),
                     (OPT, [("layer_height", "0.2", "0.16"), ("wall_loops", "5", "2")]))
        self.selection_is(f"{s} 4", after1)

    # -- Switching a process or a printer --

    def row_start(self, label):
        """A row's start, as a scenario's, checked on the selection alone."""
        self.to_start()
        self.selection_is(f"{label}: the start", "X1C | Std | PLA")

    def edit(self, kind):
        if kind == "process":
            self.set("process", values={"wall_loops": 5})
        elif kind == "printer":
            self.set("printer", values={"printer_notes": "MCP pin"})

    def row(self, n):
        """Runs switching row n: its start, its edit and its call, then checks its selection and, where it shows no
        edit, the saved wall loops. Gives the reply (None for a refusal), the dialogs that held the call or that
        were open after it, and a refusal's details."""
        edit, type, name, how, after = ROWS[n]
        label = f"switching {n}"
        self.row_start(label)
        self.edit(edit)
        code, words = REFUSALS.get(n, (None, None))
        args = {"type": type, "name": name}
        reply, details = None, {}
        if how in (None, "transfer", "discard"):
            if how:
                args["preset_changes"] = how
            if code:
                details = self.refused(f"{label}: refused", lambda: self.c.call("preset_select", **args), code, words)
            else:
                reply = self.run("preset_select", **args)
            shown = self.dialogs()
        else:
            finish, shown = self.ask(label, "preset_select", how, **args)
            if code:
                details = self.refused(f"{label}: refused", finish, code, words)
            else:
                reply = finish()
        self.selection_is(label, after)
        if "[" not in after:
            self.t.equal(f"{label}: the wall loops", self.value("process", "wall_loops"), "2")
        return reply, shown, details

    def switching(self):
        s = "switching"
        self.start(s)
        prompt = [{"title": PROMPT, "buttons": ["Transfer", "Discard", "Save"]}]
        std_edit = {"process": {"name": STD, "keys": ["wall_loops"]}}
        opt_edit = {"process": {"name": OPT, "keys": ["wall_loops"]}}

        def parts(reply):
            return reply.get("discarded"), reply.get("modified")

        reply, shown, _ = self.row(1)
        self.t.equal(f"{s} 1: no dialog opened", shown, [])
        self.t.equal(f"{s} 1: no discarded and no modified", parts(reply), (None, None))
        reply, _, _ = self.row(2)
        self.t.equal(f"{s} 2: the reply", parts(reply), (None, opt_edit))
        reply, _, _ = self.row(3)
        self.t.equal(f"{s} 3: the reply", parts(reply), (std_edit, None))
        reply, shown, _ = self.row(4)
        self.t.equal(f"{s} 4: the dialog that held it", shown, prompt)
        self.t.equal(f"{s} 4: the job's reply", parts(reply), (None, opt_edit))
        reply, _, _ = self.row(5)
        self.t.equal(f"{s} 5: the job's reply", parts(reply), (std_edit, None))
        self.row(6)
        _, shown, _ = self.row(7)
        self.t.equal(f"{s} 7: the dialog that held it", shown, prompt)
        self.row(8)
        _, _, details = self.row(9)
        self.t.equal(f"{s} 9: the similar names start with Opt", (details.get("similar") or [None])[0], OPT)
        self.row(10)
        _, shown, _ = self.row(11)
        self.t.equal(f"{s} 11: no dialog opened", shown, [])
        self.row(12)
        reply, _, _ = self.row(13)
        self.t.equal(f"{s} 13: the reply's discarded", reply.get("discarded"),
                     {"printer": {"name": X1C, "keys": ["printer_notes"]}})
        _, shown, _ = self.row(14)
        self.t.equal(f"{s} 14: the dialog that held it", shown, [{"title": PROMPT, "buttons": ["Discard", "Save"]}])
        self.row(15)
        reply, shown, _ = self.row(16)
        self.t.equal(f"{s} 16: no dialog opened", shown, [])
        self.t.equal(f"{s} 16: the edit moved with the switch", reply.get("modified"), std_edit)
        reply, _, _ = self.row(17)
        self.t.equal(f"{s} 17: the reply's discarded", reply.get("discarded"), std_edit)
        _, shown, _ = self.row(18)
        self.t.equal(f"{s} 18: the dialog that held it", [d["buttons"] for d in shown], [["Transfer", "Discard", "Save"]])
        reply, _, _ = self.row(19)
        self.t.equal(f"{s} 19: closing the prompt does not stop the switch", reply.get("selected"), X1E)
        self.row(20)
        self.row(21)

    def api_row(self, label, n, name="row", answer="row"):
        """Plugin API row: as switching row n, through begin_update(name).commit(). The call asks where the row
        has an edit, and is answered as the row is, unless `answer` says otherwise (None: no dialog opens). Gives
        what the script returns and the dialogs that held it."""
        edit, type, row_name, how, after = ROWS[n]
        name = row_name if name == "row" else name
        answer = (how if how not in ("transfer", "discard") else None) if answer == "row" else answer
        self.row_start(label)
        self.edit(edit)
        if answer is None:
            reply = self.run("script_run", code=plugin_code(api_switch(type, name)))
            shown = self.dialogs()
        else:
            finish, shown = self.ask(label, "script_run", answer, code=plugin_code(api_switch(type, name)))
            reply = finish()
        return self.script_result(reply), shown

    def api_after(self, label, n, text=None):
        after = text or ROWS[n][4]
        self.selection_is(label, after)
        if "[" not in after:
            self.t.equal(f"{label}: the wall loops", self.value("process", "wall_loops"), "2")

    def plugin_api(self):
        s = "the plugin API"
        self.start(s)
        prompt = [{"title": PROMPT, "buttons": ["Transfer", "Discard", "Save"]}]

        def commit(result):
            return result.get("target_name"), result.get("selection_changed"), result.get("raised")

        def raised(label, result, type, words):
            r = result.get("raised") or {}
            self.t.check(f"{label}: raises {type}", r.get("type") == type and (words is None or words in
                                                                              r.get("message", "")), result)

        result, _ = self.api_row(f"{s} 1", 1)
        self.t.equal(f"{s} 1: the commit", commit(result), (OPT, True, None))
        self.api_after(f"{s} 1", 1)
        result, shown = self.api_row(f"{s} 2", 4)
        self.t.equal(f"{s} 2: the dialog that held it", shown, prompt)
        self.t.equal(f"{s} 2: the commit's target", commit(result)[::2], (OPT, None))
        self.api_after(f"{s} 2", 4)
        result, _ = self.api_row(f"{s} 3", 5)
        self.t.equal(f"{s} 3: the commit's target", commit(result)[::2], (OPT, None))
        self.api_after(f"{s} 3", 5)
        result, _ = self.api_row(f"{s} 4", 6)
        raised(f"{s} 4", result, "RuntimeError", REFUSALS[6][1])
        self.api_after(f"{s} 4", 6)
        for step, name in ((5, STD), (6, None)):
            result, shown = self.api_row(f"{s} {step}", 7, name=name, answer=None)
            self.t.equal(f"{s} {step}: no dialog opens", shown, [])
            self.t.equal(f"{s} {step}: the commit", commit(result), (STD, False, None))
            self.api_after(f"{s} {step}", 7, "X1C | Std[wall_loops 5] | PLA")
        for n in (8, 9, 10):
            result, _ = self.api_row(f"{s} 7 (row {n})", n)
            raised(f"{s} 7 (row {n})", result, "ValueError", REFUSALS[n][1])
            self.api_after(f"{s} 7 (row {n})", n)
        result, _ = self.api_row(f"{s} 8", 11)
        self.t.equal(f"{s} 8: the commit", commit(result), (X1E, True, None))
        self.api_after(f"{s} 8", 11)
        result, shown = self.api_row(f"{s} 9 (row 14)", 14)
        self.t.equal(f"{s} 9 (row 14): the dialog that held it", shown,
                     [{"title": PROMPT, "buttons": ["Discard", "Save"]}])
        self.t.equal(f"{s} 9 (row 14): the commit's target", commit(result)[::2], (X1E, None))
        self.api_after(f"{s} 9 (row 14)", 14)
        result, _ = self.api_row(f"{s} 9 (row 15)", 15)
        raised(f"{s} 9 (row 15)", result, "RuntimeError", None)
        self.api_after(f"{s} 9 (row 15)", 15)
        result, shown = self.api_row(f"{s} 10", 18)
        self.t.equal(f"{s} 10: the dialog that held it", [d["buttons"] for d in shown], [["Transfer", "Discard", "Save"]])
        self.t.equal(f"{s} 10: the commit's target", commit(result)[::2], (X1E, None))
        self.api_after(f"{s} 10", 18)
        result, _ = self.api_row(f"{s} 11", 19)
        self.t.equal(f"{s} 11: nothing raised", commit(result)[::2], (X1E, None))
        self.api_after(f"{s} 11", 19)
        for n in (20, 21):
            result, _ = self.api_row(f"{s} 12 (row {n})", n)
            raised(f"{s} 12 (row {n})", result, "ValueError", None)
            self.api_after(f"{s} 12 (row {n})", n)
        self.row_start(f"{s} 13")
        result = self.plugin(f"orca.host.preset.process.begin_update(1, {json.dumps(OPT)})\nresult = 'returned'")
        raised(f"{s} 13: begin_update with a slot", result, "ValueError",
               "The slot_number form of begin_update() is only available on orca.host.preset.filament")
        self.selection_is(f"{s} 13: begin_update", "X1C | Std | PLA")
        result = self.plugin("orca.host.preset.process.update(1, 'wall_loops', '3')\nresult = 'returned'")
        raised(f"{s} 13: update with a slot", result, "ValueError",
               "The slot_number form of update() is only available on orca.host.preset.filament")
        self.selection_is(f"{s} 13: update", "X1C | Std | PLA")

    def carries(self):
        s = "what a switch carries"
        self.start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        self.set("process", values={"sparse_infill_density": "25%"})
        self.set("process", values={"wall_loops": 4})
        reply = self.select("process", OPT, preset_changes="transfer")
        self.t.equal(f"{s} 1: the reply", (reply.get("modified", {}).get("process", {}).get("keys"),
                                           "discarded" in reply), (["sparse_infill_density", "wall_loops"], False))
        self.selection_is(f"{s} 1", "X1C | Opt[sparse_infill_density 25%, wall_loops 4] | MCP Test PLA")
        self.set("process", values={"seam_slope_type": "all"})
        reply = self.select("process", STD, preset_changes="transfer")
        self.t.equal(f"{s} 2: a setting Simple mode hides is dropped", reply.get("discarded"),
                     {"process": {"name": OPT, "keys": ["seam_slope_type"]}})
        self.selection_is(f"{s} 2", "X1C | Std[sparse_infill_density 25%, wall_loops 4] | MCP Test PLA")
        self.set("filament", values={"slow_down_layer_time": 7})
        reply = self.select("process", OPT, preset_changes="discard")
        modified = reply.get("modified", {})
        self.t.equal(f"{s} 3: the reply",
                     (reply.get("discarded", {}).get("process", {}).get("keys"), "process" in modified,
                      modified.get("filament", {}).get("keys")),
                     (["sparse_infill_density", "wall_loops"], False, ["slow_down_layer_time"]))
        self.selection_is(f"{s} 3", "X1C | Opt | MCP Test PLA[slow_down_layer_time 7]")
        self.set("filament", reset_all=True, values={"filament_type": "PETG"})
        self.run("preset_save", type="filament", name="MCP Test PETG")
        self.select("filament", "MCP Test PLA", slot=1)
        self.t.equal(f"{s} 4: MCP Test PETG's type", self.value("filament", "filament_type", preset="MCP Test PETG"),
                     "PETG")
        self.selection_is(f"{s} 4", "X1C | Opt | MCP Test PLA")
        self.set("filament", values={"slow_down_layer_time": 8})
        self.t.refused(f"{s} 5: no transfer to a filament of another type",
                       lambda: self.c.call("preset_select", type="filament", name="MCP Test PETG", filament_slot=1,
                                           preset_changes="transfer"),
                       code="transfer_not_offered",
                       words='The unsaved changes cannot be transferred: "MCP Test PETG" is a filament of another type.')
        self.selection_is(f"{s} 5", "X1C | Opt | MCP Test PLA[slow_down_layer_time 8]")
        reply = self.select("filament", "MCP Test PETG", slot=1, preset_changes="discard")
        self.t.equal(f"{s} 6: the reply", (reply.get("selected"), reply.get("discarded")),
                     ("MCP Test PETG", {"filament": {"name": "MCP Test PLA", "keys": ["slow_down_layer_time"]}}))
        self.selection_is(f"{s} 6", "X1C | Opt | MCP Test PETG")

    def new_project(self):
        s = "a new project"
        self.start(s)
        edit = {"process": {"name": STD, "keys": ["wall_loops"]}}

        def std_modified():
            return [i.get("modified") for i in self.listed("process", query=STD) if i["name"] == STD]

        self.set("process", values={"wall_loops": 6})
        reply = self.run("project_new", discard_changes=True)
        self.t.equal(f"{s} 1: the reply", (reply.get("modified"), "discarded" in reply), (edit, False))
        self.t.equal(f"{s} 1: Std is listed modified", std_modified(), [True])
        self.selection_is(f"{s} 1", "X1C | Std[wall_loops 6] | PLA")
        reply = self.run("project_new", discard_changes=True, preset_changes="keep")
        self.t.equal(f"{s} 2: the reply", (reply.get("modified"), "discarded" in reply), (edit, False))
        self.selection_is(f"{s} 2", "X1C | Std[wall_loops 6] | PLA")
        reply = self.run("project_new", discard_changes=True, preset_changes="discard")
        self.t.equal(f"{s} 3: the reply", (reply.get("discarded"), "modified" in reply), (edit, False))
        self.t.equal(f"{s} 3: Std is listed without modified", std_modified(), [None])
        self.selection_is(f"{s} 3", "X1C | Std | PLA")

    def inside_project(self):
        s = "a process kept inside the project"
        self.start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        reply = self.run("preset_save", type="process", name="MCP Project Process", inside_project=True)
        self.t.equal(f"{s} 1: the reply", (reply.get("project"), reply.get("user"), reply.get("inherits")),
                     (True, False, STD))
        self.t.equal(f"{s} 1: listed once, selected, inside the project",
                     [(i["name"], i.get("selected"), i.get("project"), i.get("user"))
                      for i in self.listed("process", query="MCP Project Process")],
                     [("MCP Project Process", True, True, False)])
        self.selection_is(f"{s} 1", "X1C | MCP Project Process | MCP Test PLA")
        self.t.refused(f"{s} 2: a detached save is refused outside Developer mode",
                       lambda: self.c.call("preset_save", type="process", name="MCP Detached", detach=True),
                       words="Detach is offered only in Developer mode")
        self.t.equal(f"{s} 2: nothing saved", self.names("process", query="MCP Detached"), [])
        self.selection_is(f"{s} 2", "X1C | MCP Project Process | MCP Test PLA")
        self.developer_mode()
        reply = self.run("preset_save", type="process", name="MCP Detached", detach=True)
        self.developer_mode()
        self.t.equal(f"{s} 3: the reply", (reply.get("project"), "inherits" in reply), (True, False))
        self.selection_is(f"{s} 3", "X1C | MCP Detached | MCP Test PLA")
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20])
        path = os.path.join(self.folder, "inside.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.t.equal(f"{s} 4: presets inside a project leave with it", self.names("process", query="MCP"), [])
        self.selection_is(f"{s} 4", "X1C | Std | MCP Test PLA")
        self.run("project_open", path=path)
        wait(self.c, "idle")
        self.t.equal(f"{s} 5: the project's processes, the detached one made from the project's settings",
                     sorted((i["name"], i.get("project"), i.get("user")) for i in self.listed("process", query="MCP")),
                     [("MCP Detached(inside.3mf)", None, True), ("MCP Project Process", True, False)])
        self.t.equal(f"{s} 5: MCP Test PLA is selected and not modified",
                     [(i.get("selected"), i.get("modified", False)) for i in self.listed("filament", query="MCP Test PLA")
                      if i["name"] == "MCP Test PLA"], [(True, False)])
        self.selection_is(f"{s} 5", "X1C | MCP Detached(inside.3mf) | MCP Test PLA")

    def deleting(self):
        s = "deleting presets"
        self.start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        self.t.refused(f"{s} 1: a system preset is refused",
                       lambda: self.c.call("preset_delete", type="filament", name=PLA),
                       words='"Bambu PLA Basic @BBL X1C" is a system preset')
        self.t.equal(f"{s} 1: PLA is still there", self.value("filament", "filament_type", preset=PLA), "PLA")
        self.selection_is(f"{s} 1", "X1C | Std | MCP Test PLA")
        reply = self.run("preset_delete", type="filament", name="MCP Test PLA 2")
        self.t.equal(f"{s} 2: the reply", (reply.get("deleted"), reply.get("selected"), reply.get("filaments")),
                     ("MCP Test PLA 2", "MCP Test PLA", ["MCP Test PLA"]))
        self.t.equal(f"{s} 2: it is not listed", self.names("filament", query="MCP Test PLA 2"), [])
        self.selection_is(f"{s} 2", "X1C | Std | MCP Test PLA")
        self.set("filament", values={"slow_down_layer_time": 11})
        self.t.refused(f"{s} 3: refused while the edited filament has unsaved changes",
                       lambda: self.c.call("preset_delete", type="filament", name="MCP Test PETG"),
                       words='The edited filament "MCP Test PLA" has unsaved changes')
        self.t.equal(f"{s} 3: MCP Test PETG is still listed", self.names("filament", query="MCP Test PETG"),
                     ["MCP Test PETG"])
        self.selection_is(f"{s} 3", "X1C | Std | MCP Test PLA[slow_down_layer_time 11]")
        self.set("filament", reset_all=True)
        self.select("filament", "MCP Test PETG", slot=1)
        self.set("filament", values={"slow_down_layer_time": 11})
        reply = self.run("preset_delete", type="filament", name="MCP Test PETG")
        self.t.equal(f"{s} 4: the reply", (reply.get("selected"), reply.get("discarded")),
                     (PLA, {"filament": {"name": "MCP Test PETG", "keys": ["slow_down_layer_time"]}}))
        self.t.equal(f"{s} 4: it is not listed", self.names("filament", query="MCP Test PETG"), [])
        self.selection_is(f"{s} 4", "X1C | Std | PLA")
        reply = self.run("preset_delete", type="filament", name="MCP Test PLA")
        self.t.equal(f"{s} 5: the reply's selected", reply.get("selected"), PLA)
        self.t.equal(f"{s} 5: no MCP Test filament is left", self.names("filament", query="MCP Test"), [])
        self.selection_is(f"{s} 5", "X1C | Std | PLA")
        self.developer_mode()
        reply = self.run("preset_save", type="process", name="MCP Delete Base", detach=True)
        self.developer_mode()
        self.t.equal(f"{s} 6: the reply", (reply.get("user"), "inherits" in reply), (True, False))
        self.selection_is(f"{s} 6", "X1C | MCP Delete Base | PLA")
        self.set("process", values={"wall_loops": 3})
        reply = self.run("preset_save", type="process", name="MCP Delete Child")
        self.t.equal(f"{s} 7: the reply", (reply.get("inherits"), reply.get("selected")), ("MCP Delete Base", True))
        self.t.equal(f"{s} 7: the child's wall loops", self.value("process", "wall_loops", preset="MCP Delete Child"), "3")
        self.selection_is(f"{s} 7", "X1C | MCP Delete Child | PLA")
        both = ["MCP Delete Base", "MCP Delete Child"]
        self.t.refused(f"{s} 8: a preset others inherit from is refused",
                       lambda: self.c.call("preset_delete", type="process", name="MCP Delete Base"),
                       words='"MCP Delete Base" cannot be deleted while other presets inherit from it')
        self.t.equal(f"{s} 8: both are listed", sorted(self.names("process", query="MCP Delete")), both)
        self.selection_is(f"{s} 8", "X1C | MCP Delete Child | PLA")
        self.select("process", STD)
        self.set("process", values={"wall_loops": 4})
        self.t.refused(f"{s} 9: refused while the edited process has unsaved changes",
                       lambda: self.c.call("preset_delete", type="process", name="MCP Delete Child"),
                       words='The edited process "0.20mm Standard @BBL X1C" has unsaved changes')
        self.t.equal(f"{s} 9: both are listed", sorted(self.names("process", query="MCP Delete")), both)
        self.selection_is(f"{s} 9", "X1C | Std[wall_loops 4] | PLA")
        self.set("process", reset_all=True)
        reply = self.run("preset_delete", type="process", name="MCP Delete Child")
        self.t.equal(f"{s} 10: the reply", (reply.get("deleted"), reply.get("selected"), "discarded" in reply),
                     ("MCP Delete Child", STD, False))
        self.t.equal(f"{s} 10: the base alone is listed", self.names("process", query="MCP Delete"), ["MCP Delete Base"])
        self.selection_is(f"{s} 10", "X1C | Std | PLA")
        self.select("process", "MCP Delete Base")
        self.set("process", values={"wall_loops": 5})
        reply = self.run("preset_delete", type="process", name="MCP Delete Base")
        p = reply.get("selected", "")
        system = [i.get("system") for i in self.listed("process", query=p) if i["name"] == p]
        self.t.check(f"{s} 11: the tab moves to a system process", p != "MCP Delete Base" and system == [True], reply)
        self.t.equal(f"{s} 11: the reply's discarded", reply.get("discarded"),
                     {"process": {"name": "MCP Delete Base", "keys": ["wall_loops"]}})
        self.t.equal(f"{s} 11: none is listed", self.names("process", query="MCP Delete"), [])
        self.selection_is(f"{s} 11", f"X1C | {p} | PLA")
        self.select("process", STD)
        reply = self.run("preset_save", type="printer", name="MCP Test Printer")
        self.t.equal(f"{s} 12: the reply", (reply.get("user"), reply.get("inherits"), reply.get("selected")),
                     (True, X1C, True))
        self.selection_is(f"{s} 12", "MCP Test Printer | Std | PLA")
        self.select("printer", X1C)
        self.t.refused(f"{s} 13: a printer that is not selected is refused",
                       lambda: self.c.call("preset_delete", type="printer", name="MCP Test Printer"),
                       words='"MCP Test Printer" is not the selected printer')
        self.t.equal(f"{s} 13: it is still listed", self.names("printer", query="MCP Test Printer"), ["MCP Test Printer"])
        self.selection_is(f"{s} 13", "X1C | Std | PLA")
        self.select("printer", "MCP Test Printer")
        reply = self.run("preset_delete", type="printer", name="MCP Test Printer")
        self.t.equal(f"{s} 14: the reply", (reply.get("selected"), "attached" in reply), (X1C, False))
        self.t.equal(f"{s} 14: it is not listed", self.names("printer", query="MCP Test Printer"), [])
        self.selection_is(f"{s} 14", "X1C | Std | PLA")

    def costs(self):
        """filament_cost of MCP Export PLA and MCP Import PLA, "absent" for one not listed."""
        out = []
        for name in ("MCP Export PLA", "MCP Import PLA"):
            if name in self.names("filament", query=name):
                out.append(self.value("filament", "filament_cost", preset=name))
            else:
                out.append("absent")
        return tuple(out)

    def exporting(self):
        s = "exporting and importing presets"
        self.start(s)
        folder = os.path.join(self.folder, "export")
        os.makedirs(folder, exist_ok=True)
        zip_path = os.path.join(folder, "Filament presets.zip")
        fixture = os.path.join(FIXTURES, "mcp_import_pla.json")

        def step(n, want_costs, selection=True):
            self.t.equal(f"{s} {n}: the costs", self.costs(), want_costs)
            if selection:
                self.selection_is(f"{s} {n}", "X1C | Std | PLA")

        def imported(n, message, **args):
            reply = self.run("preset_import", **args)
            self.t.equal(f"{s} {n}: the message", reply.get("message"), message)

        self.set("filament", values={"filament_cost": 25})
        self.developer_mode()
        reply = self.run("preset_save", type="filament", name="MCP Export PLA", detach=True)
        self.developer_mode()
        self.select("filament", PLA, slot=1)
        self.t.equal(f"{s} 1: no inherits", "inherits" in reply, False)
        step(1, ("25", "absent"))
        export = self.c.call("preset_export", path=folder)
        self.t.equal(f"{s} 2: the export is held by its dialog", export.get("status"), "blocked_by_dialog")
        within = "Export Preset Bundle"
        self.c.call("widget_click", target={"label": "Filament presets(.zip)", "within": within})
        self.c.call("widget_set_value", target={"name": "MCP Export PLA", "within": within}, value=True)
        ok = self.c.call("dialog_answer", dialog={"title": within}, button="ok")
        info = [d.get("message") for d in self.wait_dialog("Info")]
        self.t.equal(f"{s} 2: the Info dialog's message", info, ["Export successful"])
        self.c.call("dialog_answer", dialog={"title": "Info"}, button="ok")
        if isinstance(ok, dict) and ok.get("job"):
            wait(self.c, "job", job=ok["job"])
        done = wait(self.c, "job", job=export["job"]) if export.get("job") else export
        self.t.equal(f"{s} 2: the export's level", done.get("level"), "success")
        self.t.equal(f"{s} 2: no dialog is left", self.dialogs(), [])
        entries = {}
        if os.path.exists(zip_path):
            with zipfile.ZipFile(zip_path) as z:
                entries = {n: json.loads(z.read(n)) for n in z.namelist() if n.endswith("MCP Export PLA.json")}
        self.t.equal(f"{s} 2: the file holds MCP Export PLA with its cost",
                     [e.get("filament_cost") for e in entries.values()], [["25"]])
        step(2, ("25", "absent"))
        imported(3, "Kept the existing presets: MCP Export PLA.", path=zip_path)
        step(3, ("25", "absent"))
        self.run("preset_delete", type="filament", name="MCP Export PLA")
        imported(4, "Imported presets: MCP Export PLA.", path=zip_path)
        self.t.equal(f"{s} 4: it is listed as a user preset",
                     [i.get("user") for i in self.listed("filament", query="MCP Export PLA")], [True])
        step(4, ("25", "absent"))
        self.select("filament", "MCP Export PLA", slot=1)
        self.set("filament", values={"filament_cost": 30})
        self.run("preset_save", type="filament", name="MCP Export PLA")
        self.select("filament", PLA, slot=1)
        step(5, ("30", "absent"), selection=False)
        imported(6, "Kept the existing presets: MCP Export PLA.", path=zip_path)
        step(6, ("30", "absent"))
        imported(7, "Replaced presets: MCP Export PLA.", path=zip_path, on_conflict="overwrite")
        step(7, ("25", "absent"))
        imported(8, "Imported presets: MCP Import PLA.", path=fixture)
        self.t.equal(f"{s} 8: it inherits PLA",
                     [i.get("inherits") for i in self.listed("filament", query="MCP Import PLA")], [PLA])
        step(8, ("25", "24"))
        imported(9, "Kept the existing presets: MCP Import PLA.", path=fixture)
        imported(9, "Replaced presets: MCP Import PLA.", path=fixture, on_conflict="overwrite")
        step(9, ("25", "24"))
        missing = os.path.join(self.folder, "no_such_preset.json")
        self.t.refused(f"{s} 10: a missing file is refused", lambda: self.c.call("preset_import", path=missing),
                       words="file not found")
        step(10, ("25", "24"))
        self.t.refused(f"{s} 11: a picture is refused",
                       lambda: self.c.call("preset_import", path=os.path.join(FIXTURES, "heightmap.png")),
                       words="Not a preset file: give a .json, .zip, .orca_printer, .orca_filament, .orca_bundle or .gcode")
        step(11, ("25", "24"))
        not_preset = os.path.join(self.folder, "not_a_preset.json")
        with open(not_preset, "w", encoding="utf-8") as f:
            json.dump({"not": "a preset"}, f)
        self.t.refused(f"{s} 12: a file with no preset is refused", lambda: self.c.call("preset_import", path=not_preset),
                       code="no_presets_imported")
        self.t.equal(f"{s} 12: no dialog open", self.dialogs(), [])
        step(12, ("25", "24"))
        self.t.refused(f"{s} 13: overwrite is not an argument",
                       lambda: self.c.call("preset_import", path=fixture, overwrite=True),
                       words="unknown argument overwrite")
        step(13, ("25", "24"))
        self.t.refused(f"{s} 14: an unknown on_conflict is refused",
                       lambda: self.c.call("preset_import", path=fixture, on_conflict="merge"),
                       words="on_conflict must be one of")
        step(14, ("25", "24"))
        self.run("preset_delete", type="filament", name="MCP Export PLA")
        self.run("preset_delete", type="filament", name="MCP Import PLA")
        self.t.equal(f"{s} 15: none is left", self.names("filament", query="MCP "), [])

    def nozzle(self):
        s = "a nozzle diameter"
        self.start(s)
        reply = self.set("printer", values={"nozzle_diameter": 0.6})
        self.t.equal(f"{s} 1: the reply's applied", reply.get("applied"), {"nozzle_diameter": "0.6"})
        self.t.equal(f"{s} 1: the nozzle and the variant",
                     (self.value("printer", "nozzle_diameter"), self.value("printer", "printer_variant")), ("0.6", "0.6"))
        self.selection_is(f"{s} 1", "X1C 0.6 | 0.30mm Standard @BBL X1C 0.6 nozzle | HS")
        reply = self.set("printer", values={"nozzle_diameter": "0.4"})
        self.t.equal(f"{s} 2: the reply's applied", reply.get("applied"), {"nozzle_diameter": "0.4"})
        self.selection_is(f"{s} 2", "X1C | Std | PLA")

        def grouping(label):
            items = {i["key"]: i for i in self.c.call("plate_get_settings",
                                                      keys=["filament_map_mode", "filament_map"])["items"]}
            mode, fmap = items.get("filament_map_mode", {}), items.get("filament_map", {})
            self.t.equal(f"{label}: the filament grouping",
                         (mode.get("value"), mode.get("meta", {}).get("offered"), fmap.get("effective")),
                         ("Auto For Flush", [], [1]))

        grouping(f"{s} 3")
        self.selection_is(f"{s} 3", "X1C | Std | PLA")
        self.t.refused(f"{s} 4: a grouping mode on one extruder is refused",
                       lambda: self.c.call("plate_set_settings", values={"filament_map_mode": "Manual"}),
                       words="filament_map_mode groups the filaments onto the extruders of a printer that has several; "
                             "this one has one")
        grouping(f"{s} 4")
        self.selection_is(f"{s} 4", "X1C | Std | PLA")

    def print(self):
        s = "what the print is sliced with"
        self.start(s)
        self.set("filament", values={"filament_cost": 23})
        self.run("preset_save", type="filament", name="MCP Print PLA")
        self.select("process", OPT)
        self.set("process", values={"wall_loops": 3})
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20])
        self.selection_is(f"{s} 1", "X1C | Opt[wall_loops 3] | MCP Print PLA")
        reply = self.run("plate_slice")
        self.t.equal(f"{s} 2: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        path = os.path.join(self.folder, "print.gcode")
        self.run("project_export", format="gcode", path=path, overwrite=True)
        config = gcode_config(path)
        keys = ("printer_settings_id", "print_settings_id", "filament_settings_id", "layer_height", "wall_loops",
                "filament_cost")
        self.t.equal(f"{s} 2: the G-code's settings", {k: config.get(k) for k in keys},
                     {"printer_settings_id": X1C, "print_settings_id": OPT, "filament_settings_id": "MCP Print PLA",
                      "layer_height": "0.16", "wall_loops": "3", "filament_cost": "23"})
        heights = gcode_heights(path)
        steps = [round(b - a, 4) for a, b in zip(heights, heights[1:])][:-1]
        self.t.check(f"{s} 2: the layers rise by 0.16", len(heights) > 2 and all(abs(d - 0.16) <= 0.001 for d in steps),
                     f"{len(heights)} heights, steps {sorted(set(steps))}")
        self.selection_is(f"{s} 2", "X1C | Opt[wall_loops 3] | MCP Print PLA")

    def preference_dialog(self):
        s = "a preference changed in its dialog"
        self.start(s)
        reply = self.c.call("preferences_get", keys=["auto_arrange", "no_such_preference"])
        self.t.equal(f"{s} 1: auto_arrange, on by default",
                     ([(i["key"], i["value"]) for i in reply["items"]], reply.get("unknown_keys")),
                     ([("auto_arrange", True)], ["no_such_preference"]))
        self.t.refused(f"{s} 2: settings_set does not take a preference",
                       lambda: self.c.call("settings_set", values={"auto_arrange": False}),
                       words='Unknown setting "auto_arrange" for the process settings')
        self.t.equal(f"{s} 2: no dialog open", self.dialogs(), [])
        self.t.equal(f"{s} 2: auto_arrange", self.preference("auto_arrange"), True)
        self.t.refused(f"{s} 3: an unknown tab is refused",
                       lambda: self.c.call("window_open", window="preferences", tab="advanced"),
                       words="tab must be one of")
        self.t.equal(f"{s} 3: no dialog open", self.dialogs(), [])
        for n, tab, setting in ((4, "behaviour", False), (5, "general", True)):
            opened = self.c.call("window_open", window="preferences", tab=tab, key="auto_arrange")
            self.t.equal(f"{s} {n}: the call is held by the Preferences dialog",
                         (opened.get("status"), [d["title"] for d in self.dialogs()]),
                         ("blocked_by_dialog", ["Preferences"]))
            reply = self.c.call("widget_set_value", target={"name": "auto_arrange", "within": "Preferences"},
                                value=setting)
            self.t.equal(f"{s} {n}: the control's reply", reply.get("checked"), setting)
            self.c.call("dialog_answer", dialog={"title": "Preferences"}, button="close")
            if opened.get("job"):
                wait(self.c, "job", job=opened["job"])
            self.t.equal(f"{s} {n}: auto_arrange", self.preference("auto_arrange"), setting)

    def run_all(self):
        self.run("action_run", id=SIMPLE_MODE)
        for name, scenario in [("hidden presets", self.hidden), ("listing presets", self.listing),
                               ("what a setting is", self.settings), ("saving a filament", self.saving),
                               ("dropping and reverting edits", self.reverting),
                               ("reading presets that are not selected", self.not_selected),
                               ("switching", self.switching), ("the plugin API", self.plugin_api),
                               ("what a switch carries", self.carries), ("a new project", self.new_project),
                               ("a process kept inside the project", self.inside_project),
                               ("deleting presets", self.deleting), ("exporting and importing presets", self.exporting),
                               ("a nozzle diameter", self.nozzle), ("what the print is sliced with", self.print),
                               ("a preference changed in its dialog", self.preference_dialog)]:
            self.t.scenario(name, scenario)
        self.close_left_dialogs()
        self.c.call("dialog_auto_answer", rules=[])
        self.run("project_new", discard_changes=True, preset_changes="discard")
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-presets-") as folder:
        return PresetsTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
