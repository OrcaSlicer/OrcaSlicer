#!/usr/bin/env python3
"""The presets of the three types, as the test_presets entry of tools/mcp/test-entries/presets.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_presets.py

Exits 0 when every check passes.
"""

import json
import os
import re
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, SLOT_COLOURS, Checks, add_slot, refusal, rule_uses,  # noqa: E402
                    run, slots, start, until, wait)

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures")

KLIPPER_06 = "MyKlipper 0.6 nozzle"
KLIPPER_08 = "MyKlipper 0.8 nozzle"
MARLIN = "MyMarlin 0.4 nozzle"
STD = KLIPPER_STD
OPT = "0.16mm Optimal @MyKlipper"
PLA = GENERIC_PLA
# A printer of the Custom vendor that PLA does not fit, with five extruders, and the Generic PLA of its own that takes
# PLA's place.
TOOLCHANGER = "MyToolChanger 0.4 nozzle"
TOOLCHANGER_STD = "0.20mm Standard @MyToolChanger"
TOOLCHANGER_PLA = "Generic PLA @MyToolChanger"
# The printer's Extruder page's Retraction group, in the tab's order (publishable_printer_retraction_options()).
RETRACTION_KEYS = ["retraction_length", "retract_restart_extra", "retraction_speed", "deretraction_speed",
                   "retraction_minimum_travel", "retract_when_changing_layer", "wipe", "wipe_distance",
                   "retract_before_wipe", "retract_after_wipe"]
# The entry's terms for the presets it names often.
TERMS = {"Klipper": KLIPPER, "Klipper 0.6": KLIPPER_06, "Klipper 0.8": KLIPPER_08, "Marlin": MARLIN,
         "ToolChanger": TOOLCHANGER, "Std": STD, "Opt": OPT, "TC Std": TOOLCHANGER_STD, "PLA": PLA,
         "TC PLA": TOOLCHANGER_PLA}
SHORT = {name: term for term, name in TERMS.items()}
START = {"printer": KLIPPER, "process": STD, "filaments": [PLA]}

# The user filament keep and discard saves in slot 2.
CALIB_PLA = "MCP Calib PLA"
# The presets this test saves, which a run that failed on the data folder may have left: a process child before its
# base, since a preset others inherit from cannot be deleted.
OWN_FILAMENTS = ("MCP Test PLA", "MCP Test PLA 2", "MCP Test PETG", "MCP Export PLA", "MCP Imported PLA",
                 "MCP Print PLA", CALIB_PLA)
OWN_PROCESSES = ("MCP Delete Child", "MCP Delete Base")
OWN_PRINTER = "MCP Test Printer"
# The presets the hidden presets scenario and the listing need hidden, which an earlier test may have installed or
# shown.
HIDDEN_PRINTERS = [{"vendor": "Custom", "model": "Generic Klipper Printer", "nozzles": ["0.2", "0.6", "0.8"]},
                   {"vendor": "Custom", "model": "Generic Marlin Printer", "nozzles": ["0.4"]}]

DEVELOPER_MODE = "orca_command:toggle_developer_mode:orca"
PROMPT = "Transfer or discard changes"
SPIRAL = "Spiral mode only works"
# A calibration test's call drops the project's and the presets' unsaved changes unless its step says otherwise.
DISCARD = {"discard_changes": True, "preset_changes": "discard"}
TEMPERATURE_KEYS = ["nozzle_temperature", "nozzle_temperature_initial_layer"]


def times(value, n):
    """The entry's `v×n`: n entries, each v, joined by commas."""
    return ",".join([str(value)] * n)


def group_size(pages, page, group):
    """How many settings settings_list_pages' `pages` gives `group` of `page`, None where it has no such group."""
    for listed in pages:
        if listed["page"] == page:
            for g in listed["groups"]:
                if g["group"] == group:
                    return g["settings"]
    return None


def same_number(a, b):
    try:
        return float(a) == float(b)
    except ValueError:
        return a == b


class Every:
    """The entry's "every entry": a value with one entry per extruder or variant, each entry `value`, whatever their
    number. Every("0") equals "0,0"."""

    def __init__(self, value):
        self.value = str(value)

    def __eq__(self, other):
        if not isinstance(other, str) or not other:
            return False
        return all(same_number(entry, self.value) for entry in other.split(","))

    def __repr__(self):
        return f"every entry {self.value}"


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
    """'Klipper | Std[wall_loops 5] | PLA' as {printer, process, filaments}: each preset (name, {key: value}), the
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


def named_selection(client):
    """selection() as the calibration scenarios compare it: the printer and the process by name, and each slot's
    filament with its unsaved edits. The process and printer keys a calibration test changes depend on the values
    the shipped profiles already hold, so they are read one by one with value() instead."""
    presets = client.call("app_state")["presets"]
    return {"printer": presets["printer"], "process": presets["process"],
            "filaments": [(name, unsaved(client, "filament", slot)) for slot, name in
                          enumerate(presets["filaments"], 1)]}


def named(text):
    """A selection the entry writes, as named_selection() gives it: the printer's and the process's edits dropped."""
    sel = parse_selection(text)
    return {"printer": sel["printer"][0], "process": sel["process"][0], "filaments": sel["filaments"]}


def pla(edits):
    """The selection `Klipper | Std | PLA`, as named_selection() gives it, with PLA's unsaved keys and values
    `edits`."""
    return {"printer": KLIPPER, "process": STD, "filaments": [(PLA, dict(edits))]}


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
    """The heights of a G-code file's `;Z:` lines, the line the Klipper's G-code writes at each layer change, in
    order."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return [float(line.split(":", 1)[1]) for line in f if line.startswith(";Z:")]


def gcode_layers(path):
    """The lines of an exported G-code file after its first `;LAYER_CHANGE` line, the line the Klipper's G-code
    writes at each layer change."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    first = next(i for i, line in enumerate(lines) if line.startswith(";LAYER_CHANGE"))
    return lines[first + 1:]


def in_a_row(values):
    """`values` with repeats in a row dropped."""
    return [v for i, v in enumerate(values) if i == 0 or v != values[i - 1]]


def flow_offset(name):
    """The number a flow ratio object's name ends in, "m" for minus: flowrate_m0.01 is -0.01."""
    m = re.fullmatch(r"flowrate_(m?)([0-9.]+)", name)
    assert m, name
    return -float(m.group(2)) if m.group(1) else float(m.group(2))


# ---- The switching rows: edit, type, name, how, selection after ----
# `how` is preset_changes ("transfer", "discard"), the prompt's answer when the call asks ("Transfer", "Discard",
# "cancel"), or None for a call with no edit to ask about.
ROWS = {
    1: (None, "process", OPT, None, "Klipper | Opt | PLA"),
    2: ("process", "process", OPT, "transfer", "Klipper | Opt[wall_loops 5] | PLA"),
    3: ("process", "process", OPT, "Discard", "Klipper | Opt | PLA"),
    4: ("process", "process", OPT, "cancel", "Klipper | Std[wall_loops 5] | PLA"),
    5: ("process", "process", STD, "cancel", "Klipper | Std[wall_loops 5] | PLA"),
    6: (None, "process", "No Such Process", None, "Klipper | Std | PLA"),
    7: (None, "process", "0.16mm Optimal", None, "Klipper | Std | PLA"),
    8: (None, "process", "0.20mm Standard @MyMarlin", None, "Klipper | Std | PLA"),
    9: ("printer", "printer", KLIPPER_08, "transfer", 'Klipper[printer_notes "MCP pin"] | Std | PLA'),
    10: ("printer", "printer", KLIPPER_08, "Discard", "Klipper 0.8 | Std | PLA"),
    11: ("printer", "printer", KLIPPER_08, "cancel", 'Klipper[printer_notes "MCP pin"] | Std | PLA'),
    12: ("process", "printer", KLIPPER_08, "transfer", "Klipper 0.8 | Std[wall_loops 5] | PLA"),
    13: ("process", "printer", KLIPPER_08, "discard", "Klipper 0.8 | Std | PLA"),
    14: ("process", "printer", KLIPPER_08, "Transfer", "Klipper 0.8 | Std[wall_loops 5] | PLA"),
    15: ("process", "printer", KLIPPER_08, "cancel", "Klipper 0.8 | Std | PLA"),
    16: (None, "printer", "Generic Klipper Printer", None, "Klipper | Std | PLA"),
}
# The rows refused, by code and words (None where the entry gives the code alone).
REFUSALS = {
    4: ("selection_cancelled", f'Selecting "{OPT}" was cancelled.'),
    5: ("selection_cancelled", None),
    6: ("preset_not_offered", 'The preset selector does not offer "No Such Process".'),
    7: ("preset_not_offered", 'The preset selector does not offer "0.16mm Optimal".'),
    8: ("preset_incompatible", '"0.20mm Standard @MyMarlin" is not compatible with the selected printer.'),
    9: ("transfer_not_offered", "The unsaved changes cannot be transferred: a printer's changes stay with it."),
    11: ("selection_cancelled", None),
    16: ("preset_not_offered", None),
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
        """Checks.refused, giving the refusal's text and its details ({} without any)."""
        caught = {}

        def wrapped():
            try:
                return call()
            except orca_mcp.McpError as e:
                caught["text"], caught["details"] = refusal(e)
                raise

        self.t.refused(label, wrapped, code=code, words=words)
        return caught.get("text", ""), caught.get("details", {})

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

    def select(self, type, name, slot=None, **args):
        if slot is not None:
            args["filament_slot"] = slot
        return self.run("preset_select", type=type, name=name, **args)

    def set(self, type, **args):
        return self.run("settings_set", type=type, **args)

    def scenario_start(self, name):
        self.t.equal(f"{name}: the start: the selection", start(self.c), START)
        state = self.c.call("app_state")
        self.t.equal(f"{name}: the start: one filament slot and no objects",
                     (len(state["presets"]["filaments"]), state["objects"]), (1, 0))
        self.t.equal(f"{name}: the start: Simple mode", state["mode"], "simple")
        self.t.equal(f"{name}: the start: Developer mode is off", self.preference("developer_mode"), False)

    # ---- Scenarios ----

    def without_remembered_presets(self):
        """remember_printer_config off, so that a printer switch keeps the process and filament that fit the new
        printer, whatever the data folder remembers it was last used with. The next start() turns it on
        again, as the baseline has it."""
        self.c.call("preferences_set", values={"remember_printer_config": False})

    def hidden(self):
        s = "hidden presets"
        self.scenario_start(s)
        self.without_remembered_presets()
        printers = {i["name"]: i for i in self.listed("printer")}
        self.t.equal(f"{s} 1: the Klipper listed selected, the hidden printers not",
                     (printers.get(KLIPPER, {}).get("selected"), KLIPPER_08 in printers, MARLIN in printers),
                     (True, False, False))
        self.selection_is(f"{s} 1", "Klipper | Std | PLA")
        reply = self.select("printer", KLIPPER_08)
        self.t.equal(f"{s} 2: the reply's shown", reply.get("shown"), KLIPPER_08)
        self.t.equal(f"{s} 2: no dialog opened", self.dialogs(), [])
        self.selection_is(f"{s} 2", "Klipper 0.8 | Std | PLA")
        self.select("printer", KLIPPER)
        self.selection_is(f"{s} 3", "Klipper | Std | PLA")
        reply = self.select("printer", MARLIN, preset_changes="discard")
        self.t.equal(f"{s} 4: the reply's shown", reply.get("shown"), MARLIN)
        self.selection_is(f"{s} 4", "Marlin | 0.20mm Standard @MyMarlin | PLA")
        self.select("printer", KLIPPER, preset_changes="discard")
        self.select("process", STD, preset_changes="discard")
        self.select("filament", PLA, slot=1)
        self.selection_is(f"{s} 5", "Klipper | Std | PLA")

    def listing(self):
        s = "listing presets"
        self.scenario_start(s)
        filters = {"vendor": "Generic", "filament_type": "PLA", "include_uninstalled": True}
        first = self.listed("filament", **filters)
        names = [i["name"] for i in first]
        self.t.check(f"{s} 1: PLA is listed and the ToolChanger's is not",
                     PLA in names and TOOLCHANGER_PLA not in names, names)
        self.t.equal(f"{s} 1: every item is a Generic PLA",
                     [i for i in first if (i.get("vendor"), i.get("filament_type")) != ("Generic", "PLA")], [])
        self.selection_is(f"{s} 1", "Klipper | Std | PLA")
        self.t.equal(f"{s} 2: the filters in lower case list the same",
                     self.names("filament", vendor="generic", filament_type="pla", include_uninstalled=True), names)
        self.selection_is(f"{s} 2", "Klipper | Std | PLA")
        more = self.listed("filament", include_incompatible=True, **filters)
        by_name = {i["name"]: i for i in more}
        self.t.check(f"{s} 3: every name of step 1 and more", set(names) < set(by_name), sorted(set(names) - set(by_name)))
        self.t.equal(f"{s} 3: the ToolChanger's PLA is listed incompatible",
                     by_name.get(TOOLCHANGER_PLA, {}).get("compatible"), False)
        self.t.equal(f"{s} 3: every item of step 1 is there without a compatible field",
                     [i for i in first if by_name.get(i["name"]) != i or "compatible" in i], [])
        self.t.equal(f"{s} 3: what step 1 leaves out does not fit the Klipper",
                     [n for n, i in by_name.items() if n not in names and i.get("compatible") is not False], [])
        self.selection_is(f"{s} 3", "Klipper | Std | PLA")
        printers = self.listed("printer", include_uninstalled=True)
        p = {i["name"]: i for i in printers}
        self.t.equal(f"{s} 4: the Klipper 0.2 nozzle is listed, not installed",
                     p.get("MyKlipper 0.2 nozzle", {}).get("installed"), False)
        self.t.equal(f"{s} 4: every system printer has a vendor, a model and a nozzle",
                     [i["name"] for i in printers if i.get("system") and
                      not all(i.get(k) for k in ("vendor", "printer_model", "nozzle"))], [])
        klipper = p.get(KLIPPER, {})
        self.t.equal(f"{s} 4: the Klipper item",
                     {k: klipper.get(k) for k in ("vendor", "printer_model", "nozzle", "selected")},
                     {"vendor": "Custom", "printer_model": "Generic Klipper Printer", "nozzle": "0.4", "selected": True})
        self.selection_is(f"{s} 4", "Klipper | Std | PLA")
        for_toolchanger = self.names("filament", printer=TOOLCHANGER, include_uninstalled=True)
        for_klipper = self.names("filament", include_uninstalled=True)
        self.t.equal(f"{s} 5: the ToolChanger's PLA fits it alone, and PLA the Klipper alone",
                     (TOOLCHANGER_PLA in for_toolchanger, TOOLCHANGER_PLA in for_klipper, PLA in for_toolchanger,
                      PLA in for_klipper), (True, False, False, True))
        self.selection_is(f"{s} 5", "Klipper | Std | PLA")
        items = {i["name"]: i for i in self.listed("filament", printer=TOOLCHANGER, include_uninstalled=True,
                                                   include_incompatible=True)}
        self.t.equal(f"{s} 6: compatibility is judged against the printer named",
                     (items.get(PLA, {}).get("compatible"),
                      TOOLCHANGER_PLA in items and "compatible" in items[TOOLCHANGER_PLA]), (False, False))
        self.selection_is(f"{s} 6", "Klipper | Std | PLA")
        self.t.refused(f"{s} 7: a printer filter on printers is refused",
                       lambda: self.c.call("preset_list", type="printer", printer=TOOLCHANGER),
                       words="printer lists the filaments or processes that fit a printer preset, not printers")
        self.selection_is(f"{s} 7", "Klipper | Std | PLA")
        self.t.refused(f"{s} 8: a filament type on processes is refused",
                       lambda: self.c.call("preset_list", type="process", filament_type="PLA"),
                       words="filament_type filters filaments (type filament)")
        self.selection_is(f"{s} 8", "Klipper | Std | PLA")

    def settings(self):
        s = "what a setting is"
        self.scenario_start(s)
        reply = self.c.call("settings_get", type="process", keys=["seam_slope_type", "no_such_key"])
        items = reply["items"]
        self.t.equal(f"{s} 1: one item", [i["key"] for i in items], ["seam_slope_type"])
        meta = items[0]["meta"] if items else {}
        self.t.check(f"{s} 1: the description speaks of the scarf joint", "scarf joint" in meta.get("description", ""),
                     meta.get("description"))
        self.t.equal(f"{s} 1: where it is and its second label",
                     (meta.get("where"), (meta.get("labels") or [None, None])[1]), ("Quality > Seam", "Contour"))
        self.t.equal(f"{s} 1: the unknown keys", reply.get("unknown_keys"), ["no_such_key"])
        self.selection_is(f"{s} 1", "Klipper | Std | PLA")
        pages = self.c.call("settings_list_pages", type="printer")["pages"]
        retraction = self.settings_pages(type="printer", where="Extruder > Retraction")
        size = group_size(pages, "Extruder", "Retraction")
        self.t.check(f"{s} 2: the Extruder page's Retraction group counts the settings listed there",
                     len(retraction) >= 1 and size == len(retraction), (size, len(retraction)))
        self.t.equal(f"{s} 2: retraction_length is listed there",
                     {i["key"]: i.get("meta", {}).get("where") for i in retraction}.get("retraction_length"),
                     "Extruder > Retraction")
        self.selection_is(f"{s} 2", "Klipper | Std | PLA")
        pages = self.c.call("settings_list_pages", type="process")["pages"]
        self.t.check(f"{s} 3: the Others page has a Brim group", group_size(pages, "Others", "Brim") is not None, pages)
        brim = [i["key"] for i in self.settings_pages(type="process", where="Others > Brim")]
        self.t.check(f"{s} 3: brim_type is listed there", "brim_type" in brim, brim)
        self.t.refused(f"{s} 3: settings_get takes no query", lambda: self.c.call("settings_get", query="seam"),
                       code="invalid_argument")
        counted = sum(g["settings"] for p in self.c.call("settings_list_pages", type="object")["pages"] for g in p["groups"])
        listed = self.settings_pages(type="object", include_meta=True)
        placed = [i["key"] for i in listed if i.get("meta", {}).get("where")]
        whole = self.c.call("settings_list", type="process", limit=1)["total"]
        self.t.check(f"{s} 3: an object's pages count the settings an object takes", counted == len(placed) and
                     len(listed) < whole, (counted, len(placed), len(listed), whole))
        self.selection_is(f"{s} 3", "Klipper | Std | PLA")
        reply = self.c.call("settings_get", keys=["nozzle_temperature", "layer_height", "nozzle_diameter",
                                                  "curr_bed_type", "no_such_key"], include_meta=False)
        self.t.equal(f"{s} 4: each key read from its type", [(i["key"], i["type"], i["value"]) for i in reply["items"]],
                     [("nozzle_temperature", "filament", times(220, 6)), ("layer_height", "process", "0.2"),
                      ("nozzle_diameter", "printer", "0.4")])
        self.t.equal(f"{s} 4: the unknown keys and where they are",
                     (reply.get("unknown_keys"), reply.get("type_of")),
                     (["curr_bed_type", "no_such_key"], {"curr_bed_type": "project"}))
        self.selection_is(f"{s} 4", "Klipper | Std | PLA")
        reply = self.c.call("settings_get", type="process", keys=["nozzle_temperature"])
        self.t.equal(f"{s} 5: no item, and the type that has it", (reply["items"], reply.get("type_of")),
                     ([], {"nozzle_temperature": "filament"}))
        self.selection_is(f"{s} 5", "Klipper | Std | PLA")
        self.t.refused(f"{s} 6: a filament key set as a process key is refused",
                       lambda: self.c.call("settings_set", values={"nozzle_temperature": 215}),
                       words='"nozzle_temperature" is a filament setting: settings_set takes it with type "filament"')
        self.t.equal(f"{s} 6: the nozzle temperature", self.value("filament", "nozzle_temperature"),
                     times(220, 6))
        self.selection_is(f"{s} 6", "Klipper | Std | PLA")
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
        self.selection_is(f"{s} 7", "Klipper | Std | PLA")
        meta = self.c.call("settings_get", type="process", keys=["fuzzy_skin"])["items"][0]["meta"]
        self.t.equal(f"{s} 8: the second value and its label", (meta["values"][1], meta["labels"][1]),
                     ("external", "Contour"))
        self.selection_is(f"{s} 8", "Klipper | Std | PLA")

    def settings_pages(self, **args):
        items = []
        while True:
            page = self.c.call("settings_list", offset=len(items), **args)
            items += page["items"]
            if not page["items"] or len(items) >= page.get("total", len(items)):
                return items

    def saving(self):
        s = "saving a filament"
        self.scenario_start(s)
        self.set("filament", values={"filament_cost": 21.5, "slow_down_layer_time": 6, "temperature_vitrification": 50})
        self.selection_is(f"{s} 1",
                          "Klipper | Std | PLA[filament_cost 21.5, slow_down_layer_time 6, temperature_vitrification 50]")
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
        self.t.equal(f"{s} 2: it fits the printer it was saved on",
                     sorted(p.strip().strip('"') for p in printers.split(";") if p.strip()), [KLIPPER])
        self.selection_is(f"{s} 2", "Klipper | Std | MCP Test PLA")
        self.t.equal(f"{s} 3: the user filaments of the names this test saves",
                     [n for n in self.names("filament", source="user") if n in OWN_FILAMENTS], ["MCP Test PLA"])
        self.t.equal(f"{s} 3: no system filament of the name",
                     self.names("filament", source="system", query="MCP Test PLA"), [])
        self.t.equal(f"{s} 3: no filament inside the project", self.names("filament", source="project"), [])
        self.selection_is(f"{s} 3", "Klipper | Std | MCP Test PLA")
        self.set("filament", values={"filament_cost": 22})
        self.run("preset_save", type="filament", name="MCP Test PLA")
        reply = self.c.call("settings_get", type="filament", compare=":parent",
                            keys=["filament_cost", "slow_down_layer_time", "nozzle_temperature"])
        self.t.equal(f"{s} 4: against PLA", (reply.get("right"), reply.get("total"),
                                             [(i["key"], i["value"], i["other"]) for i in reply["items"]]),
                     (PLA, 2, [("filament_cost", "22", "20"), ("slow_down_layer_time", "6", "4")]))
        self.selection_is(f"{s} 4", "Klipper | Std | MCP Test PLA")
        self.set("filament", values={"filament_cost": 23})
        reply = self.run("preset_save", type="filament", name="MCP Test PLA 2")
        self.t.equal(f"{s} 5: the reply's slots", reply.get("filament_slots"), [1])
        self.t.equal(f"{s} 5: MCP Test PLA is listed, not selected",
                     [i.get("selected", False) for i in self.listed("filament", query="MCP Test PLA")
                      if i["name"] == "MCP Test PLA"], [False])
        self.t.equal(f"{s} 5: MCP Test PLA's cost", self.value("filament", "filament_cost", preset="MCP Test PLA"), "22")
        self.selection_is(f"{s} 5", "Klipper | Std | MCP Test PLA 2")
        self.t.refused(f"{s} 6: saving over another preset is refused",
                       lambda: self.c.call("preset_save", type="filament", name="MCP Test PLA"),
                       words='Preset "MCP Test PLA" already exists; pass overwrite: true to replace it')
        self.t.equal(f"{s} 6: MCP Test PLA's cost", self.value("filament", "filament_cost", preset="MCP Test PLA"), "22")
        self.selection_is(f"{s} 6", "Klipper | Std | MCP Test PLA 2")
        reply = self.run("preset_save", type="filament", name="MCP Test PLA", overwrite=True)
        self.t.equal(f"{s} 7: the reply's selected", reply.get("selected"), True)
        self.t.equal(f"{s} 7: the cost, edited and saved",
                     (self.value("filament", "filament_cost"),
                      self.value("filament", "filament_cost", preset="MCP Test PLA")), ("23", "23"))
        self.selection_is(f"{s} 7", "Klipper | Std | MCP Test PLA")

    def reverting(self):
        s = "dropping and reverting edits"
        self.scenario_start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        self.set("process", values={"wall_loops": 7, "sparse_infill_density": "37%"})
        self.selection_is(f"{s} 1", "Klipper | Std[sparse_infill_density 37%, wall_loops 7] | MCP Test PLA")
        reply = self.set("process", reset_all=True, values={"wall_loops": 7})
        self.t.equal(f"{s} 2: the reply", (sorted(reply.get("reverted", [])), reply.get("applied")),
                     (["sparse_infill_density", "wall_loops"], {"wall_loops": "7"}))
        self.t.equal(f"{s} 2: the infill density", self.value("process", "sparse_infill_density"), "15%")
        self.selection_is(f"{s} 2", "Klipper | Std[wall_loops 7] | MCP Test PLA")
        reply = self.set("process", reset_all=True)
        self.t.equal(f"{s} 3: the reply's reverted", reply.get("reverted"), ["wall_loops"])
        self.t.equal(f"{s} 3: the wall loops", self.value("process", "wall_loops"), "3")
        self.selection_is(f"{s} 3", "Klipper | Std | MCP Test PLA")
        reply = self.set("process", reset_all=True)
        self.t.equal(f"{s} 4: the reply's reverted", reply.get("reverted"), [])
        self.selection_is(f"{s} 4", "Klipper | Std | MCP Test PLA")
        self.set("process", values={"wall_loops": 6})
        self.t.refused(f"{s} 5: a bad value fails before the reset",
                       lambda: self.c.call("settings_set", type="process", reset_all=True, values={"wall_loops": "six"}),
                       words="Invalid value for wall_loops")
        self.selection_is(f"{s} 5", "Klipper | Std[wall_loops 6] | MCP Test PLA")
        after6 = "Klipper | Std[sparse_infill_density 25%] | MCP Test PLA"
        self.set("process", reset_all=True, values={"wall_loops": 5, "sparse_infill_density": "25%"})
        reply = self.set("process", values={"wall_loops": None})
        self.t.equal(f"{s} 6: null takes the key back to its saved value",
                     (reply.get("applied"), reply.get("changed")), ({"wall_loops": "3"}, {"wall_loops": "3"}))
        self.selection_is(f"{s} 6", after6)
        self.t.refused(f"{s} 7: reset_to without a null is refused",
                       lambda: self.c.call("settings_set", type="process", values={"wall_loops": 4}, reset_to="parent"),
                       words="reset_to applies to the keys given as null")
        self.t.equal(f"{s} 7: the wall loops", self.value("process", "wall_loops"), "3")
        self.selection_is(f"{s} 7", after6)
        reply = self.set("filament", values={"filament_cost": None}, reset_to="parent")
        self.t.equal(f"{s} 8: the parent's cost", reply.get("applied"), {"filament_cost": "20"})
        self.t.equal(f"{s} 8: no difference from the parent",
                     self.c.call("settings_get", type="filament", compare=":parent", keys=["filament_cost"])["total"], 0)
        self.selection_is(f"{s} 8", "Klipper | Std[sparse_infill_density 25%] | MCP Test PLA[filament_cost 20]")
        reply = self.set("filament", values={"filament_cost": None})
        self.t.equal(f"{s} 9: the saved cost", reply.get("applied"), {"filament_cost": "23"})
        self.selection_is(f"{s} 9", after6)
        item = self.c.call("settings_get", type="process", keys=["outer_wall_speed"])["items"][0]
        self.t.equal(f"{s} 10: the variants, the one in use and the value",
                     (item["meta"].get("variants"), item["meta"].get("active"), item["value"]),
                     (["Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Extra High Flow"], [0],
                      times(120, 3)))
        self.selection_is(f"{s} 10", after6)
        reply = self.set("process", values={"outer_wall_speed": 250})
        self.t.equal(f"{s} 11: one value writes the entry in use", reply.get("applied"),
                     {"outer_wall_speed": "250,120,120"})
        after11 = "Klipper | Std[outer_wall_speed 250,120,120, sparse_infill_density 25%] | MCP Test PLA"
        self.selection_is(f"{s} 11", after11)
        self.t.refused(f"{s} 12: two values for three variants are refused",
                       lambda: self.c.call("settings_set", type="process", values={"outer_wall_speed": [1, 2]}),
                       words="outer_wall_speed holds 3 values, one per extruder variant")
        self.selection_is(f"{s} 12", after11)
        reply = self.set("process", values={"outer_wall_speed": {"index": 1, "value": 300}})
        self.t.equal(f"{s} 13: the reply", (reply.get("applied"), reply.get("changed")),
                     ({"outer_wall_speed": "250,300,120"}, {"outer_wall_speed": "250,300,120"}))
        after13 = "Klipper | Std[outer_wall_speed 250,300,120, sparse_infill_density 25%] | MCP Test PLA"
        self.selection_is(f"{s} 13", after13)
        reply = self.set("filament", values={"nozzle_temperature": {"index": 0, "value": 215}})
        self.t.equal(f"{s} 14: the reply's applied", reply.get("applied"), {"nozzle_temperature": "215," + times(220, 5)})
        reply = self.c.call("settings_get", type="filament", compare=":saved", keys=["nozzle_temperature"])
        self.t.equal(f"{s} 14: filaments are compared entry by entry",
                     (reply["total"], [(i.get("index"), i["value"], i["other"]) for i in reply["items"]]),
                     (1, [(0, "215", "220")]))
        self.selection_is(f"{s} 14", "Klipper | Std[outer_wall_speed 250,300,120, sparse_infill_density 25%] | "
                                     f"MCP Test PLA[nozzle_temperature 215,{times(220, 5)}]")
        reply = self.set("filament", reset_all=True)
        self.t.equal(f"{s} 15: the reply's changed", reply.get("changed"), {"nozzle_temperature": times(220, 6)})
        self.selection_is(f"{s} 15", after13)
        self.run("dialog_auto_answer", rules=[{"match": SPIRAL, "button": "yes"}])
        self.t.equal(f"{s} 16: the spiral rule has answered nothing yet", rule_uses(self.c, SPIRAL), 0)
        reply = self.answered_by_rule("settings_set", type="process", values={"spiral_mode": True})
        self.t.equal(f"{s} 16: the spiral mode question was asked once", rule_uses(self.c, SPIRAL), 1)
        changed = reply.get("changed", {})
        self.t.equal(f"{s} 16: the reply's applied", reply.get("applied"), {"spiral_mode": "1"})
        self.t.equal(f"{s} 16: the reply's changed", {k: changed.get(k) for k in
                                                      ("spiral_mode", "wall_loops", "top_shell_layers",
                                                       "sparse_infill_density")},
                     {"spiral_mode": "1", "wall_loops": "1", "top_shell_layers": "0", "sparse_infill_density": "0%"})
        edits = dict({"outer_wall_speed": "250,300,120"}, **changed)
        got = selection(self.c)
        self.t.check(f"{s} 16: the selection", got["printer"] == (KLIPPER, {}) and got["process"] == (STD, edits) and
                     got["filaments"] == [("MCP Test PLA", {})],
                     f"got {selection_text(got)}, expected the process's edits {edits}")
        self.run("dialog_auto_answer", rules=[])
        reply = self.set("process", reset_all=True)
        self.t.equal(f"{s} 17: the reply's spiral mode", reply.get("changed", {}).get("spiral_mode"), "0")
        self.selection_is(f"{s} 17", "Klipper | Std | MCP Test PLA")

    def not_selected(self):
        s = "reading presets that are not selected"
        self.scenario_start(s)
        after1 = "Klipper | Std[sparse_infill_density 25%, wall_loops 5] | PLA"
        reply = self.set("process", reset_all=True, values={"wall_loops": 5, "sparse_infill_density": "25%"})
        self.t.equal(f"{s} 1: the reply's changed", reply.get("changed"),
                     {"wall_loops": "5", "sparse_infill_density": "25%"})
        self.selection_is(f"{s} 1", after1)
        reply = self.c.call("settings_get", type="process", compare=":saved")
        self.t.equal(f"{s} 2: the unsaved edits", (reply.get("total"), reply.get("left"),
                                                   [(i["key"], i["value"], i["other"]) for i in reply["items"]]),
                     (2, f"{STD} (edited)", [("sparse_infill_density", "25%", "15%"), ("wall_loops", "5", "3")]))
        self.selection_is(f"{s} 2", after1)
        self.t.equal(f"{s} 3: Std as saved, and Opt's layer height",
                     (self.value("process", "wall_loops", preset=STD), self.value("process", "layer_height", preset=OPT)),
                     ("3", "0.16"))
        self.selection_is(f"{s} 3: reading Opt did not select it", after1)
        reply = self.c.call("settings_get", type="process", compare=OPT, keys=["layer_height", "wall_loops"])
        self.t.equal(f"{s} 4: against Opt", (reply.get("right"), [(i["key"], i["value"], i["other"])
                                                                  for i in reply["items"]]),
                     (OPT, [("layer_height", "0.2", "0.16"), ("wall_loops", "5", "3")]))
        self.selection_is(f"{s} 4", after1)

    # -- Switching a process or a printer --

    def row_start(self, label):
        """A row's start, as a scenario's, checked on the selection alone."""
        start(self.c)
        self.selection_is(f"{label}: the start", "Klipper | Std | PLA")

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
                _, details = self.refused(f"{label}: refused", lambda: self.c.call("preset_select", **args), code,
                                          words)
            else:
                reply = self.run("preset_select", **args)
            shown = self.dialogs()
        else:
            finish, shown = self.ask(label, "preset_select", how, **args)
            if code:
                _, details = self.refused(f"{label}: refused", finish, code, words)
            else:
                reply = finish()
        self.selection_is(label, after)
        if "[" not in after:
            self.t.equal(f"{label}: the wall loops", self.value("process", "wall_loops"), "3")
        return reply, shown, details

    def switching(self):
        s = "switching"
        self.scenario_start(s)
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
        reply, shown, _ = self.row(3)
        self.t.equal(f"{s} 3: the dialog that held it", shown, prompt)
        self.t.equal(f"{s} 3: the job's reply", parts(reply), (std_edit, None))
        self.row(4)
        _, shown, _ = self.row(5)
        self.t.equal(f"{s} 5: the dialog that held it", shown, prompt)
        self.row(6)
        _, _, details = self.row(7)
        self.t.equal(f"{s} 7: the similar names start with Opt", (details.get("similar") or [None])[0], OPT)
        self.row(8)
        self.row(9)
        reply, shown, _ = self.row(10)
        self.t.equal(f"{s} 10: the dialog that held it", shown, [{"title": PROMPT, "buttons": ["Discard", "Save"]}])
        self.t.equal(f"{s} 10: the job's reply's discarded", reply.get("discarded"),
                     {"printer": {"name": KLIPPER, "keys": ["printer_notes"]}})
        self.row(11)
        reply, shown, _ = self.row(12)
        self.t.equal(f"{s} 12: no dialog opened", shown, [])
        self.t.equal(f"{s} 12: the edit moved with the switch", reply.get("modified"), std_edit)
        reply, _, _ = self.row(13)
        self.t.equal(f"{s} 13: the reply's discarded", reply.get("discarded"), std_edit)
        _, shown, _ = self.row(14)
        self.t.equal(f"{s} 14: the dialog that held it", [d["buttons"] for d in shown], [["Transfer", "Discard", "Save"]])
        reply, _, _ = self.row(15)
        self.t.equal(f"{s} 15: closing the prompt does not stop the switch", reply.get("selected"), KLIPPER_08)
        self.row(16)

    def api_row(self, label, edit, type, name, answer):
        """A plugin API row: its start and `edit` as a switching row's, then begin_update(name).commit() of `type`,
        the prompt answered with `answer` (None: no dialog opens). Gives what the script returns and the dialogs
        that held it."""
        self.row_start(label)
        self.edit(edit)
        if answer is None:
            reply = self.run("script_run", code=plugin_code(api_switch(type, name)))
            shown = self.dialogs()
        else:
            finish, shown = self.ask(label, "script_run", answer, code=plugin_code(api_switch(type, name)))
            reply = finish()
        return self.script_result(reply), shown

    def api_as_row(self, label, n):
        """A plugin API row that is switching row n through begin_update(name).commit(), answered as that row is."""
        edit, type, name, how, _ = ROWS[n]
        return self.api_row(label, edit, type, name, how if how not in ("transfer", "discard") else None)

    def api_after(self, label, after):
        self.selection_is(label, after)
        if "[" not in after:
            self.t.equal(f"{label}: the wall loops", self.value("process", "wall_loops"), "3")

    def plugin_api(self):
        s = "the plugin API"
        self.scenario_start(s)
        prompt = [{"title": PROMPT, "buttons": ["Transfer", "Discard", "Save"]}]

        def commit(result):
            return result.get("target_name"), result.get("selection_changed"), result.get("raised")

        def raised(label, result, type, words):
            r = result.get("raised") or {}
            self.t.check(f"{label}: raises {type}", r.get("type") == type and (words is None or words in
                                                                              r.get("message", "")), result)

        result, _ = self.api_as_row(f"{s} 1", 1)
        self.t.equal(f"{s} 1: the commit", commit(result), (OPT, True, None))
        self.api_after(f"{s} 1", ROWS[1][4])
        result, shown = self.api_row(f"{s} 2", "process", "process", OPT, "Transfer")
        self.t.equal(f"{s} 2: the dialog that held it", shown, prompt)
        self.t.equal(f"{s} 2: the commit's target", commit(result)[::2], (OPT, None))
        self.api_after(f"{s} 2", "Klipper | Opt[wall_loops 5] | PLA")
        result, _ = self.api_as_row(f"{s} 3", 4)
        raised(f"{s} 3", result, "RuntimeError", REFUSALS[4][1])
        self.api_after(f"{s} 3", ROWS[4][4])
        for step, name in ((4, STD), (5, None)):
            result, shown = self.api_row(f"{s} {step}", "process", "process", name, None)
            self.t.equal(f"{s} {step}: no dialog opens", shown, [])
            self.t.equal(f"{s} {step}: the commit", commit(result), (STD, False, None))
            self.api_after(f"{s} {step}", "Klipper | Std[wall_loops 5] | PLA")
        for n in (7, 8):
            result, _ = self.api_as_row(f"{s} 6 (row {n})", n)
            raised(f"{s} 6 (row {n})", result, "ValueError", REFUSALS[n][1])
            self.api_after(f"{s} 6 (row {n})", ROWS[n][4])
        result, shown = self.api_row(f"{s} 7", None, "printer", KLIPPER_08, None)
        self.t.equal(f"{s} 7: no dialog opened", shown, [])
        self.t.equal(f"{s} 7: the commit", commit(result), (KLIPPER_08, True, None))
        self.api_after(f"{s} 7", "Klipper 0.8 | Std | PLA")
        self.row_start(f"{s} 8")
        result = self.plugin(f"orca.host.preset.process.begin_update(1, {json.dumps(OPT)})\nresult = 'returned'")
        raised(f"{s} 8: begin_update with a slot", result, "ValueError",
               "The slot_number form of begin_update() is only available on orca.host.preset.filament")
        self.selection_is(f"{s} 8: begin_update", "Klipper | Std | PLA")
        result = self.plugin("orca.host.preset.process.update(1, 'wall_loops', '4')\nresult = 'returned'")
        raised(f"{s} 8: update with a slot", result, "ValueError",
               "The slot_number form of update() is only available on orca.host.preset.filament")
        self.selection_is(f"{s} 8: update", "Klipper | Std | PLA")

    def carries(self):
        s = "what a switch carries"
        self.scenario_start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        self.set("process", values={"sparse_infill_density": "25%"})
        self.set("process", values={"wall_loops": 4})
        reply = self.select("process", OPT, preset_changes="transfer")
        self.t.equal(f"{s} 1: the reply", (reply.get("modified", {}).get("process", {}).get("keys"),
                                           "discarded" in reply), (["sparse_infill_density", "wall_loops"], False))
        self.selection_is(f"{s} 1", "Klipper | Opt[sparse_infill_density 25%, wall_loops 4] | MCP Test PLA")
        self.set("process", values={"seam_slope_type": "all"})
        reply = self.select("process", STD, preset_changes="transfer")
        self.t.equal(f"{s} 2: a setting Simple mode hides is dropped", reply.get("discarded"),
                     {"process": {"name": OPT, "keys": ["seam_slope_type"]}})
        self.selection_is(f"{s} 2", "Klipper | Std[sparse_infill_density 25%, wall_loops 4] | MCP Test PLA")
        self.set("filament", values={"slow_down_layer_time": 7})
        reply = self.select("process", OPT, preset_changes="discard")
        modified = reply.get("modified", {})
        self.t.equal(f"{s} 3: the reply",
                     (reply.get("discarded", {}).get("process", {}).get("keys"), "process" in modified,
                      modified.get("filament", {}).get("keys")),
                     (["sparse_infill_density", "wall_loops"], False, ["slow_down_layer_time"]))
        self.selection_is(f"{s} 3", "Klipper | Opt | MCP Test PLA[slow_down_layer_time 7]")
        self.set("filament", reset_all=True, values={"filament_type": "PETG"})
        self.run("preset_save", type="filament", name="MCP Test PETG")
        self.select("filament", "MCP Test PLA", slot=1)
        self.t.equal(f"{s} 4: MCP Test PETG's type", self.value("filament", "filament_type", preset="MCP Test PETG"),
                     "PETG")
        self.selection_is(f"{s} 4", "Klipper | Opt | MCP Test PLA")
        self.set("filament", values={"slow_down_layer_time": 8})
        reply = self.select("filament", "MCP Test PETG", slot=1, preset_changes="discard")
        self.t.equal(f"{s} 5: the reply", (reply.get("selected"), reply.get("discarded")),
                     ("MCP Test PETG", {"filament": {"name": "MCP Test PLA", "keys": ["slow_down_layer_time"]}}))
        self.selection_is(f"{s} 5", "Klipper | Opt | MCP Test PETG")

    def new_project(self):
        s = "a new project"
        self.scenario_start(s)
        edit = {"process": {"name": STD, "keys": ["wall_loops"]}}

        def std_modified():
            return [i.get("modified") for i in self.listed("process", query=STD) if i["name"] == STD]

        self.set("process", values={"wall_loops": 6})
        reply = self.run("project_new", discard_changes=True)
        self.t.equal(f"{s} 1: the reply", (reply.get("modified"), "discarded" in reply), (edit, False))
        self.t.equal(f"{s} 1: Std is listed modified", std_modified(), [True])
        self.selection_is(f"{s} 1", "Klipper | Std[wall_loops 6] | PLA")
        reply = self.run("project_new", discard_changes=True, preset_changes="keep")
        self.t.equal(f"{s} 2: the reply", (reply.get("modified"), "discarded" in reply), (edit, False))
        self.selection_is(f"{s} 2", "Klipper | Std[wall_loops 6] | PLA")
        reply = self.run("project_new", discard_changes=True, preset_changes="discard")
        self.t.equal(f"{s} 3: the reply", (reply.get("discarded"), "modified" in reply), (edit, False))
        self.t.equal(f"{s} 3: Std is listed without modified", std_modified(), [None])
        self.selection_is(f"{s} 3", "Klipper | Std | PLA")

    def inside_project(self):
        s = "a process kept inside the project"
        self.scenario_start(s)

        def saved():
            """The processes of the two names it saves, each read by its name: a user process of another test whose
            name holds "MCP" stays out."""
            return sorted((i["name"], i.get("project"), i.get("user"))
                          for name in ("MCP Project Process", "MCP Detached") for i in self.listed("process", query=name))

        self.select("filament", "MCP Test PLA", slot=1)
        reply = self.run("preset_save", type="process", name="MCP Project Process", inside_project=True)
        self.t.equal(f"{s} 1: the reply", (reply.get("project"), reply.get("user"), reply.get("inherits")),
                     (True, False, STD))
        self.t.equal(f"{s} 1: listed once, selected, inside the project",
                     [(i["name"], i.get("selected"), i.get("project"), i.get("user"))
                      for i in self.listed("process", query="MCP Project Process")],
                     [("MCP Project Process", True, True, False)])
        self.selection_is(f"{s} 1", "Klipper | MCP Project Process | MCP Test PLA")
        self.t.refused(f"{s} 2: a detached save is refused outside Developer mode",
                       lambda: self.c.call("preset_save", type="process", name="MCP Detached", detach=True),
                       words="Detach is offered only in Developer mode")
        self.t.equal(f"{s} 2: nothing saved", self.names("process", query="MCP Detached"), [])
        self.selection_is(f"{s} 2", "Klipper | MCP Project Process | MCP Test PLA")
        self.developer_mode()
        reply = self.run("preset_save", type="process", name="MCP Detached", detach=True)
        self.developer_mode()
        self.t.equal(f"{s} 3: the reply", (reply.get("project"), "inherits" in reply), (True, False))
        self.selection_is(f"{s} 3", "Klipper | MCP Detached | MCP Test PLA")
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20])
        path = os.path.join(self.folder, "inside.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.t.equal(f"{s} 4: presets inside a project leave with it", saved(), [])
        self.selection_is(f"{s} 4", "Klipper | Std | MCP Test PLA")
        self.run("project_open", path=path)
        wait(self.c, "idle")
        self.t.equal(f"{s} 5: the project's processes, the detached one made from the project's settings",
                     saved(), [("MCP Detached(inside.3mf)", None, True), ("MCP Project Process", True, False)])
        self.t.equal(f"{s} 5: MCP Test PLA is selected and not modified",
                     [(i.get("selected"), i.get("modified", False)) for i in self.listed("filament", query="MCP Test PLA")
                      if i["name"] == "MCP Test PLA"], [(True, False)])
        self.selection_is(f"{s} 5", "Klipper | MCP Detached(inside.3mf) | MCP Test PLA")

    def deleting(self):
        s = "deleting presets"
        self.scenario_start(s)
        self.select("filament", "MCP Test PLA", slot=1)
        self.t.refused(f"{s} 1: a system preset is refused",
                       lambda: self.c.call("preset_delete", type="filament", name=PLA),
                       words=f'"{PLA}" is a system preset')
        self.t.equal(f"{s} 1: PLA is still there", self.value("filament", "filament_type", preset=PLA), "PLA")
        self.selection_is(f"{s} 1", "Klipper | Std | MCP Test PLA")
        reply = self.run("preset_delete", type="filament", name="MCP Test PLA 2")
        self.t.equal(f"{s} 2: the reply", (reply.get("deleted"), reply.get("selected"), reply.get("filaments")),
                     ("MCP Test PLA 2", "MCP Test PLA", ["MCP Test PLA"]))
        self.t.equal(f"{s} 2: it is not listed", self.names("filament", query="MCP Test PLA 2"), [])
        self.selection_is(f"{s} 2", "Klipper | Std | MCP Test PLA")
        self.set("filament", values={"slow_down_layer_time": 11})
        self.t.refused(f"{s} 3: refused while the edited filament has unsaved changes",
                       lambda: self.c.call("preset_delete", type="filament", name="MCP Test PETG"),
                       words='The edited filament "MCP Test PLA" has unsaved changes')
        self.t.equal(f"{s} 3: MCP Test PETG is still listed", self.names("filament", query="MCP Test PETG"),
                     ["MCP Test PETG"])
        self.selection_is(f"{s} 3", "Klipper | Std | MCP Test PLA[slow_down_layer_time 11]")
        self.set("filament", reset_all=True)
        self.select("filament", "MCP Test PETG", slot=1)
        self.set("filament", values={"slow_down_layer_time": 11})
        reply = self.run("preset_delete", type="filament", name="MCP Test PETG")
        self.t.equal(f"{s} 4: the reply", (reply.get("selected"), reply.get("discarded")),
                     (PLA, {"filament": {"name": "MCP Test PETG", "keys": ["slow_down_layer_time"]}}))
        self.t.equal(f"{s} 4: it is not listed", self.names("filament", query="MCP Test PETG"), [])
        self.selection_is(f"{s} 4", "Klipper | Std | PLA")
        reply = self.run("preset_delete", type="filament", name="MCP Test PLA")
        self.t.equal(f"{s} 5: the reply's selected", reply.get("selected"), PLA)
        self.t.equal(f"{s} 5: no MCP Test filament of this test is left",
                     [n for n in self.names("filament", query="MCP Test") if n in OWN_FILAMENTS], [])
        self.selection_is(f"{s} 5", "Klipper | Std | PLA")
        self.developer_mode()
        reply = self.run("preset_save", type="process", name="MCP Delete Base", detach=True)
        self.developer_mode()
        self.t.equal(f"{s} 6: the reply", (reply.get("user"), "inherits" in reply), (True, False))
        self.selection_is(f"{s} 6", "Klipper | MCP Delete Base | PLA")
        self.set("process", values={"wall_loops": 2})
        reply = self.run("preset_save", type="process", name="MCP Delete Child")
        self.t.equal(f"{s} 7: the reply", (reply.get("inherits"), reply.get("selected")), ("MCP Delete Base", True))
        self.t.equal(f"{s} 7: the child's wall loops", self.value("process", "wall_loops", preset="MCP Delete Child"), "2")
        self.selection_is(f"{s} 7", "Klipper | MCP Delete Child | PLA")
        both = ["MCP Delete Base", "MCP Delete Child"]
        self.t.refused(f"{s} 8: a preset others inherit from is refused",
                       lambda: self.c.call("preset_delete", type="process", name="MCP Delete Base"),
                       words='"MCP Delete Base" cannot be deleted while other presets inherit from it')
        self.t.equal(f"{s} 8: both are listed", sorted(self.names("process", query="MCP Delete")), both)
        self.selection_is(f"{s} 8", "Klipper | MCP Delete Child | PLA")
        self.select("process", STD)
        self.set("process", values={"wall_loops": 4})
        self.t.refused(f"{s} 9: refused while the edited process has unsaved changes",
                       lambda: self.c.call("preset_delete", type="process", name="MCP Delete Child"),
                       words=f'The edited process "{STD}" has unsaved changes')
        self.t.equal(f"{s} 9: both are listed", sorted(self.names("process", query="MCP Delete")), both)
        self.selection_is(f"{s} 9", "Klipper | Std[wall_loops 4] | PLA")
        self.set("process", reset_all=True)
        reply = self.run("preset_delete", type="process", name="MCP Delete Child")
        self.t.equal(f"{s} 10: the reply", (reply.get("deleted"), reply.get("selected"), "discarded" in reply),
                     ("MCP Delete Child", STD, False))
        self.t.equal(f"{s} 10: the base alone is listed", self.names("process", query="MCP Delete"), ["MCP Delete Base"])
        self.selection_is(f"{s} 10", "Klipper | Std | PLA")
        self.select("process", "MCP Delete Base")
        self.set("process", values={"wall_loops": 5})
        reply = self.run("preset_delete", type="process", name="MCP Delete Base")
        # The tab's next visible process, which the user processes earlier tests left on the folder may be.
        p = reply.get("selected", "")
        self.t.check(f"{s} 11: the tab moves to another process", p not in ("", "MCP Delete Base"), reply)
        self.t.equal(f"{s} 11: the reply's discarded", reply.get("discarded"),
                     {"process": {"name": "MCP Delete Base", "keys": ["wall_loops"]}})
        self.t.equal(f"{s} 11: none is listed", self.names("process", query="MCP Delete"), [])
        self.selection_is(f"{s} 11", f"Klipper | {p} | PLA")
        self.select("process", STD)
        reply = self.run("preset_save", type="printer", name="MCP Test Printer")
        self.t.equal(f"{s} 12: the reply", (reply.get("user"), reply.get("inherits"), reply.get("selected")),
                     (True, KLIPPER, True))
        self.selection_is(f"{s} 12", "MCP Test Printer | Std | PLA")
        self.select("printer", KLIPPER)
        self.t.refused(f"{s} 13: a printer that is not selected is refused",
                       lambda: self.c.call("preset_delete", type="printer", name="MCP Test Printer"),
                       words='"MCP Test Printer" is not the selected printer')
        self.t.equal(f"{s} 13: it is still listed", self.names("printer", query="MCP Test Printer"), ["MCP Test Printer"])
        self.selection_is(f"{s} 13", "Klipper | Std | PLA")
        self.select("printer", "MCP Test Printer")
        reply = self.run("preset_delete", type="printer", name="MCP Test Printer")
        self.t.equal(f"{s} 14: the reply", (reply.get("selected"), "attached" in reply), (KLIPPER, False))
        self.t.equal(f"{s} 14: it is not listed", self.names("printer", query="MCP Test Printer"), [])
        self.selection_is(f"{s} 14", "Klipper | Std | PLA")

    def costs(self):
        """filament_cost of MCP Export PLA and MCP Imported PLA, "absent" for one not listed."""
        out = []
        for name in ("MCP Export PLA", "MCP Imported PLA"):
            if name in self.names("filament", query=name):
                out.append(self.value("filament", "filament_cost", preset=name))
            else:
                out.append("absent")
        return tuple(out)

    def exporting(self):
        s = "exporting and importing presets"
        self.scenario_start(s)
        folder = os.path.join(self.folder, "export")
        os.makedirs(folder, exist_ok=True)
        zip_path = os.path.join(folder, "Filament presets.zip")
        # The import fixture renamed, so that its own name, "MCP Import PLA", is never loaded.
        with open(os.path.join(FIXTURES, "mcp_import_pla.json"), encoding="utf-8") as f:
            preset = json.load(f)
        preset.update(name="MCP Imported PLA", filament_settings_id=["MCP Imported PLA"])
        fixture = os.path.join(self.folder, "imported_pla.json")
        with open(fixture, "w", encoding="utf-8") as f:
            json.dump(preset, f, indent=1)

        def step(n, want_costs, selection=True):
            self.t.equal(f"{s} {n}: the costs", self.costs(), want_costs)
            if selection:
                self.selection_is(f"{s} {n}", "Klipper | Std | PLA")

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
        # Read from app_state rather than with app_wait's dialog condition, which Export Preset Bundle, still open
        # under it, already meets.
        info = until(lambda: [d.get("message") for d in self.c.call("app_state").get("dialogs", [])
                              if "Info" in d.get("title", "")], "the Info dialog open")
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
        imported(8, "Imported presets: MCP Imported PLA.", path=fixture)
        self.t.equal(f"{s} 8: it inherits PLA",
                     [i.get("inherits") for i in self.listed("filament", query="MCP Imported PLA")], [PLA])
        step(8, ("25", "24"))
        imported(9, "Kept the existing presets: MCP Imported PLA.", path=fixture)
        imported(9, "Replaced presets: MCP Imported PLA.", path=fixture, on_conflict="overwrite")
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
        self.run("preset_delete", type="filament", name="MCP Imported PLA")
        self.t.equal(f"{s} 15: neither is left",
                     [n for n in self.names("filament", query="MCP ") if n in ("MCP Export PLA", "MCP Imported PLA")], [])

    def nozzle(self):
        s = "a nozzle diameter"
        self.scenario_start(s)
        self.without_remembered_presets()
        reply = self.set("printer", values={"nozzle_diameter": 0.6})
        self.t.equal(f"{s} 1: the reply's applied", reply.get("applied"), {"nozzle_diameter": "0.6"})
        self.t.equal(f"{s} 1: the nozzle and the variant",
                     (self.value("printer", "nozzle_diameter"), self.value("printer", "printer_variant")), ("0.6", "0.6"))
        self.selection_is(f"{s} 1", "Klipper 0.6 | Std | PLA")
        reply = self.set("printer", values={"nozzle_diameter": "0.4"})
        self.t.equal(f"{s} 2: the reply's applied", reply.get("applied"), {"nozzle_diameter": "0.4"})
        self.selection_is(f"{s} 2", "Klipper | Std | PLA")

        def grouping(label):
            items = {i["key"]: i for i in self.c.call("plate_get_settings",
                                                      keys=["filament_map_mode", "filament_map"])["items"]}
            mode, fmap = items.get("filament_map_mode", {}), items.get("filament_map", {})
            self.t.equal(f"{label}: the filament grouping",
                         (mode.get("value"), mode.get("meta", {}).get("offered"), fmap.get("effective")),
                         ("Auto For Flush", [], [1]))

        grouping(f"{s} 3")
        self.selection_is(f"{s} 3", "Klipper | Std | PLA")
        self.t.refused(f"{s} 4: a grouping mode on one extruder is refused",
                       lambda: self.c.call("plate_set_settings", values={"filament_map_mode": "Manual"}),
                       words="filament_map_mode groups the filaments onto the extruders of a printer that has several; "
                             "this one has one")
        grouping(f"{s} 4")
        self.selection_is(f"{s} 4", "Klipper | Std | PLA")

    def print(self):
        s = "what the print is sliced with"
        self.scenario_start(s)
        self.set("filament", values={"filament_cost": 23})
        self.run("preset_save", type="filament", name="MCP Print PLA")
        self.select("process", OPT)
        self.set("process", values={"wall_loops": 4})
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20])
        self.selection_is(f"{s} 1", "Klipper | Opt[wall_loops 4] | MCP Print PLA")
        reply = self.run("plate_slice")
        self.t.equal(f"{s} 2: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        path = os.path.join(self.folder, "print.gcode")
        self.run("project_export", format="gcode", path=path, overwrite=True)
        config = gcode_config(path)
        keys = ("printer_settings_id", "print_settings_id", "filament_settings_id", "layer_height", "wall_loops",
                "filament_cost")
        self.t.equal(f"{s} 2: the G-code's settings", {k: config.get(k) for k in keys},
                     {"printer_settings_id": KLIPPER, "print_settings_id": OPT, "filament_settings_id": "MCP Print PLA",
                      "layer_height": "0.16", "wall_loops": "4", "filament_cost": "23"})
        heights = gcode_heights(path)
        steps = [round(b - a, 4) for a, b in zip(heights, heights[1:])][:-1]
        self.t.check(f"{s} 2: the layers rise by 0.16", len(heights) > 2 and all(abs(d - 0.16) <= 0.001 for d in steps),
                     f"{len(heights)} heights, steps {sorted(set(steps))}")
        self.selection_is(f"{s} 2", "Klipper | Opt[wall_loops 4] | MCP Print PLA")

    def preference_dialog(self):
        s = "a preference changed in its dialog"
        self.scenario_start(s)
        reply = self.c.call("preferences_get", keys=["auto_arrange", "no_such_preference"])
        self.t.equal(f"{s} 1: auto_arrange, on as the start set it",
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

    # -- The Calibration menu's tests --

    def objects(self):
        """Each object as {name, height}, the height being its bbox's size in z."""
        listing = self.c.call("object_list", limit=500, detail=True)
        return [{"name": o["name"], "height": o["bbox"]["size"][2]} for p in listing["plates"] for o in p["objects"]]

    def object_filaments(self):
        """Each object as (name, the filament it prints with) from object_list."""
        listing = self.c.call("object_list", limit=500)
        return [(o["name"], o.get("filament")) for p in listing["plates"] for o in p["objects"]]

    def own(self, obj, key):
        """The object's own value of a process key."""
        return self.c.call("settings_get", target="object", object=obj, keys=[key])["items"][0]["value"]

    def project_name(self):
        return self.c.call("app_state")["project"]["name"]

    def named_selection_is(self, label, want):
        self.t.equal(f"{label}: the selection", named_selection(self.c), want)

    def slice(self, label, export=None):
        """plate_slice and a check that it ended ok; with `export`, the G-code written to that new file in the test's
        folder, whose path it returns."""
        reply = self.run("plate_slice")
        self.t.equal(f"{label}: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        if export is None:
            return None
        path = os.path.join(self.folder, export)
        self.run("project_export", format="gcode", path=path)
        return path

    def generate(self, label, args, params, project=None, names=None, heights=None, changes=None, sets=None,
                 result_keys=None):
        """Makes the test with `args`, discarding the project's and the presets' changes, and checks what the row
        fixes: the reply's params whole; project, read back from app_state; names, read back from object_list, with
        their heights to 0.01 where given; the filament's changes whole in the reply and in the selection; each key
        `sets` lists, read with value(); and result_keys."""
        t = self.t
        reply = self.run("calibration_generate_test", **DISCARD, **args)
        if params is not None:
            t.equal(f"{label}: params", reply.get("params"), params)
        if project is not None:
            t.equal(f"{label}: the reply's project", reply.get("project"), project)
            t.equal(f"{label}: the project's name", self.project_name(), project)
        if names is not None:
            t.equal(f"{label}: the reply's names", reply.get("names"), names)
            objects = self.objects()
            t.equal(f"{label}: the objects", [o["name"] for o in objects], names)
            if heights is not None:
                t.near(f"{label}: the objects' heights", [o["height"] for o in objects], heights, 0.01)
        if changes is not None:
            filament = reply.get("presets_changed", {}).get("filament", {})
            t.equal(f"{label}: the filament's changes", {k: filament.get(k) for k in ("name", "changes")},
                    {"name": PLA, "changes": changes})
            self.named_selection_is(label, pla(changes))
        for (type_, key), want in (sets or {}).items():
            t.equal(f"{label}: sets {type_} {key}", self.value(type_, key), want)
        if result_keys is not None:
            t.equal(f"{label}: result_keys", reply.get("result_keys"), result_keys)
        return reply

    def temperature(self):
        s = "temperature"
        t = self.t
        t.equal(f"{s} 1: the start", start(self.c), START)
        t.equal(f"{s} 1: no objects", self.objects(), [])

        reply = self.run("calibration_generate_test", test="temperature", start=252, end=232)
        t.equal(f"{s} 2: the reply's project", reply.get("project"), "Nozzle temperature test")
        t.equal(f"{s} 2: params", reply.get("params"), {"start": 250, "end": 230, "auto_scale": True})
        t.equal(f"{s} 2: warnings", reply.get("warnings"),
                ["start 252 °C rounded down to 250 °C, the tower's 5 °C blocks",
                 "end 232 °C rounded down to 230 °C, the tower's 5 °C blocks"])
        t.equal(f"{s} 2: the reply's names", reply.get("names"), ["temperature_tower.drc"])
        filament = reply.get("presets_changed", {}).get("filament", {})
        t.equal(f"{s} 2: the filament's changes", {k: filament.get(k) for k in ("name", "changes")},
                {"name": PLA, "changes": {"nozzle_temperature": times(250, 6),
                                          "nozzle_temperature_initial_layer": times(250, 6)}})
        t.equal(f"{s} 2: result_keys", reply.get("result_keys"), {"type": "filament", "keys": TEMPERATURE_KEYS})
        t.equal(f"{s} 2: modified", reply.get("modified", {}).get("filament", {}).get("keys"), TEMPERATURE_KEYS)
        t.equal(f"{s} 2: the project is dirty", self.c.call("app_state")["project"]["dirty"], True)
        after2 = named(f"Klipper | Std | PLA[nozzle_temperature {times(250, 6)}, "
                       f"nozzle_temperature_initial_layer {times(250, 6)}]")

        def as_after_2(label):
            t.equal(f"{label}: the project's name", self.project_name(), "Nozzle temperature test")
            objects = self.objects()
            t.equal(f"{label}: the objects", [o["name"] for o in objects], ["temperature_tower.drc"])
            t.near(f"{label}: the tower's height, five 10 mm blocks", [o["height"] for o in objects], [50], 0.01)
            t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.2")
            self.named_selection_is(label, after2)

        as_after_2(f"{s} 2")

        path = self.slice(f"{s} 3", "temperature.gcode")
        temps = [float(m.group(1)) for line in gcode_layers(path)
                 if (m := re.match(r"M104 S([0-9.]+)", line))]
        t.equal(f"{s} 3: the tower's temperatures, block by block", in_a_row([v for v in temps if v != 0]),
                [250, 245, 240, 235, 230])

        text, details = self.refused(f"{s} 4: refused while the presets are modified",
                                     lambda: self.c.call("calibration_generate_test", test="max_volumetric",
                                                         discard_changes=True),
                                     words="Presets are modified")
        t.check(f"{s} 4: the refusal names preset_changes", "preset_changes" in text, text)
        t.equal(f"{s} 4: the refusal's modified keys", details.get("modified", {}).get("filament", {}).get("keys"),
                TEMPERATURE_KEYS)
        as_after_2(f"{s} 4")

        _, details = self.refused(f"{s} 5: an argument the test does not take is refused",
                                  lambda: self.c.call("calibration_generate_test", test="temperature", step=5,
                                                      **DISCARD),
                                  words="step does not apply to temperature")
        t.equal(f"{s} 5: the arguments it takes", details.get("takes"), ["start", "end", "auto_scale", "test_model"])
        as_after_2(f"{s} 5")

        self.refused(f"{s} 6: a step past the range is refused",
                     lambda: self.c.call("calibration_generate_test", test="pressure_advance", start=0.01, end=0.05,
                                         step=0.051, **DISCARD),
                     words="pressure_advance needs start >= 0, step >= 0.001 and end >= start + step; "
                           "got start 0.01, end 0.05, step 0.051")
        as_after_2(f"{s} 6")

        self.refused(f"{s} 7: refused without discard_changes",
                     lambda: self.c.call("calibration_generate_test", test="retraction", preset_changes="discard"),
                     code="unsaved_changes")
        as_after_2(f"{s} 7")

    def other_tests(self):
        s = "other tests"
        t = self.t
        self.generate(f"{s}: the start", {"test": "temperature"}, {"start": 230, "end": 190, "auto_scale": True},
                      changes={"nozzle_temperature": times(230, 6), "nozzle_temperature_initial_layer": times(230, 6)})

        label = f"{s} 1"
        reply = self.generate(label, {"test": "max_volumetric", "end": 15}, {"start": 5, "end": 15, "step": 0.5},
                              project="Max volumetric speed test", names=["SpeedTestStructure.drc"], heights=[22],
                              changes={"filament_max_volumetric_speed": times(200, 6), "slow_down_layer_time": "0"},
                              sets={("filament", "filament_max_volumetric_speed"): times(200, 6),
                                    ("filament", "slow_down_layer_time"): "0", ("process", "spiral_mode"): "1"},
                              result_keys={"type": "filament", "keys": ["filament_max_volumetric_speed"]})
        t.equal(f"{label}: discarded", reply.get("discarded", {}).get("filament"), {"name": PLA, "keys": TEMPERATURE_KEYS})
        t.equal(f"{label}: the temperature is the preset's again", self.value("filament", "nozzle_temperature"),
                times(220, 6))
        self.slice(label)

        label = f"{s} 2"
        self.generate(label, {"test": "pressure_advance", "end": 0.06},
                      {"method": "tower", "start": 0, "end": 0.06, "step": 0.002}, project="Pressure Advance Test",
                      names=["tower_with_seam.drc"], changes={"slow_down_layer_time": "1"},
                      sets={("filament", "slow_down_layer_time"): "1"},
                      result_keys={"type": "filament", "keys": ["enable_pressure_advance", "pressure_advance"]})
        path = self.slice(label, "pressure_advance.gcode")
        # A Klipper printer's pressure advance command.
        ks = [float(m.group(1)) for line in gcode_layers(path)
              if (m := re.match(r"SET_PRESSURE_ADVANCE ADVANCE=([0-9.]+)", line))]
        t.near(f"{label}: the tower's pressure advance, band by band", in_a_row(ks),
               [round(0.002 * i, 6) for i in range(31)], 0.0001)

        self.generate(f"{s} 3", {"test": "pressure_advance", "method": "line", "print_numbers": False},
                      {"method": "line", "start": 0, "end": 0.1, "step": 0.002, "print_numbers": False},
                      project="Pressure Advance Test", names=["pressure_advance_test.drc"], changes={},
                      sets={("process", "wipe_inward"): "0", ("process", "retract_after_outer_wall"): "0"})

        pattern_changes = {"filament_retract_when_changing_layer": Every("0"), "filament_wipe": Every("0")}
        pattern_sets = {("filament", "filament_retract_when_changing_layer"): Every("0"),
                        ("filament", "filament_wipe"): Every("0"), ("printer", "wipe"): Every("0"),
                        ("printer", "retract_when_changing_layer"): Every("0")}
        label = f"{s} 4"
        self.generate(label, {"test": "pressure_advance", "method": "pattern", "accelerations": [1000, 2000],
                              "speeds": [50, 100]},
                      {"method": "pattern", "start": 0, "end": 0.08, "step": 0.005, "accelerations": [1000, 2000],
                       "speeds": [50, 100]},
                      project="Pressure Advance Test",
                      names=["pa_pattern_50_1000", "pa_pattern_100_1000", "pa_pattern_50_2000", "pa_pattern_100_2000"],
                      changes=pattern_changes, sets=pattern_sets,
                      result_keys={"type": "filament", "keys": ["enable_pressure_advance", "pressure_advance",
                                                                "adaptive_pressure_advance",
                                                                "adaptive_pressure_advance_model"]})
        self.slice(label)

        # The pattern's defaults are the first entries of Std's outer_wall_acceleration and outer_wall_speed.
        self.generate(f"{s} 5", {"test": "pressure_advance", "method": "pattern"},
                      {"method": "pattern", "start": 0, "end": 0.08, "step": 0.005, "accelerations": [3000],
                       "speeds": [120]},
                      project="Pressure Advance Test", names=["pa_pattern_120_3000"], changes=pattern_changes,
                      sets=pattern_sets)

        label = f"{s} 6"
        self.generate(label, {"test": "flow_ratio"}, {"method": "yolo", "top_surface_pattern": "archimedeanchords"},
                      project="Orca YOLO Flow Calibration",
                      names=["flowrate_0", "flowrate_0.01", "flowrate_0.02", "flowrate_0.03", "flowrate_0.04",
                             "flowrate_0.05", "flowrate_m0.01", "flowrate_m0.02", "flowrate_m0.03", "flowrate_m0.04",
                             "flowrate_m0.05"],
                      changes={}, sets={("process", "layer_height"): "0.2"},
                      result_keys={"type": "filament", "keys": ["filament_flow_ratio"]})
        self.flow_ratios(label, lambda m: (0.98 + m) / 0.98, "archimedeanchords")

        label = f"{s} 7"
        self.generate(label, {"test": "flow_ratio", "method": "yolo_perfectionist"},
                      {"method": "yolo_perfectionist", "top_surface_pattern": "archimedeanchords"},
                      project="Orca YOLO Flow Calibration - Perfectionist version",
                      names=["flowrate_0", "flowrate_0.005", "flowrate_0.01", "flowrate_0.015", "flowrate_0.02",
                             "flowrate_0.025", "flowrate_0.03", "flowrate_0.035", "flowrate_m0.005", "flowrate_m0.01",
                             "flowrate_m0.015", "flowrate_m0.02", "flowrate_m0.025", "flowrate_m0.03", "flowrate_m0.035",
                             "flowrate_m0.04"],
                      changes={})
        self.flow_ratios(label, lambda m: (0.98 + m) / 0.98)

        label = f"{s} 8"
        self.generate(label, {"test": "flow_ratio", "method": "pass1"},
                      {"method": "pass1", "top_surface_pattern": "archimedeanchords"}, project="Flowrate Test - Pass1",
                      names=["flowrate_0", "flowrate_10", "flowrate_15", "flowrate_20", "flowrate_5", "flowrate_m10",
                             "flowrate_m15", "flowrate_m20", "flowrate_m5"],
                      changes={})
        self.flow_ratios(label, lambda m: 1 + m / 100)

        label = f"{s} 9"
        self.generate(label, {"test": "flow_ratio", "method": "pass2", "top_surface_pattern": "monotonic"},
                      {"method": "pass2", "top_surface_pattern": "monotonic"}, project="Flowrate Test - Pass2",
                      names=["flowrate_0"] + [f"flowrate_m{i}" for i in range(1, 10)],
                      changes={})
        self.flow_ratios(label, lambda m: 1 + m / 100, "monotonic")

        label = f"{s} 10"
        self.generate(label, {"test": "retraction", "end": 1}, {"start": 0, "end": 1, "step": 0.1}, project="Retraction",
                      names=["retraction_tower.drc"], changes={}, sets={("printer", "use_firmware_retraction"): "0"},
                      result_keys={"type": "filament", "keys": ["filament_retraction_length"]})
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.2")
        path = self.slice(label, "retraction.gcode")
        self.retraction_bands(label, path)

        # One row for each way the VFA tower fits its end speed to the filament's volumetric speed (12 mm³/s on the
        # extruder variant in use), which allows the outer wall about 168 mm/s at 0.2 mm.
        vfa = {"project": "VFA test", "names": ["vfa.drc"], "changes": {"slow_down_layer_time": "0"},
               "sets": {("filament", "slow_down_layer_time"): "0", ("process", "spiral_mode"): "1",
                        ("process", "wall_loops"): "1", ("process", "top_shell_layers"): "0",
                        ("process", "bottom_shell_layers"): "1", ("process", "sparse_infill_density"): "0%",
                        ("process", "enable_overhang_speed"): Every("0")},
               "result_keys": {"type": "printer", "keys": ["resonance_avoidance", "min_resonance_avoidance_speed",
                                                           "max_resonance_avoidance_speed"]}}
        label = f"{s} 11"
        reply = self.generate(label, {"test": "vfa", "end": 160},
                              {"start": 40, "end": 160, "step": 10, "auto_scale": True, "auto_adjust": True}, **vfa)
        t.equal(f"{label}: no adjusted", reply.get("adjusted"), None)
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.2")

        label = f"{s} 12"
        reply = self.generate(label, {"test": "vfa"},
                              {"start": 40, "end": 200, "step": 10, "auto_scale": True, "auto_adjust": True}, **vfa)
        t.equal(f"{label}: adjusted", reply.get("adjusted"),
                {"layer_height": 0.16,
                 "reason": "the end speed (200 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s) at the "
                           "default layer height (0.2 mm); the layer height of a profile of this printer reaches it"})
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.16")
        self.slice(label)

        label = f"{s} 13"
        reply = self.generate(label, {"test": "vfa", "end": 400, "step": 20},
                              {"start": 40, "end": 380, "step": 20, "auto_scale": True, "auto_adjust": True}, **vfa)
        t.equal(f"{label}: adjusted", reply.get("adjusted"),
                {"end": 380, "layer_height": 0.08,
                 "reason": "the end speed (400 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s) even at "
                           "the smallest layer height of this printer's profiles; the end speed was lowered to what it "
                           "reaches"})
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.08")

        label = f"{s} 14"
        reply = self.generate(label, {"test": "vfa", "end": 400, "auto_adjust": False},
                              {"start": 40, "end": 400, "step": 10, "auto_scale": True, "auto_adjust": False}, **vfa)
        t.equal(f"{label}: no adjusted", reply.get("adjusted"), None)
        t.equal(f"{label}: warnings", reply.get("warnings"),
                ["the end speed (400 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s), which limits "
                 "the outer wall to about 168 mm/s: the blocks above print at that speed (auto_adjust and auto_scale "
                 "lower the layer height to reach it)"])
        t.equal(f"{label}: the tower's own layer height, half the nozzle", self.own(0, "layer_height"), "0.2")

        # The outer wall speed is the smaller of the Klipper's machine_max_speed_x and _y, and the accelerations its
        # machine_max_acceleration_extruding; the jerk is raised to at least 5, below the Klipper's own 9.
        shaping = {"changes": {"slow_down_layer_time": "0", "slow_down_min_speed": "0", "slow_down_for_layer_cooling": "0",
                               "enable_pressure_advance": Every("1"), "pressure_advance": Every("0")},
                   "sets": {("filament", "slow_down_layer_time"): "0", ("filament", "slow_down_min_speed"): "0",
                            ("filament", "slow_down_for_layer_cooling"): "0",
                            ("filament", "enable_pressure_advance"): Every("1"),
                            ("filament", "pressure_advance"): Every("0"),
                            ("printer", "machine_max_jerk_x"): "9", ("printer", "machine_max_jerk_y"): "9",
                            ("printer", "input_shaping_emit"): "0",
                            ("process", "spiral_mode"): "1", ("process", "wall_loops"): "1",
                            ("process", "top_shell_layers"): "0", ("process", "bottom_shell_layers"): "1",
                            ("process", "sparse_infill_density"): "0%", ("process", "enable_overhang_speed"): Every("0"),
                            ("process", "accel_to_decel_enable"): "1", ("process", "accel_to_decel_factor"): "100%",
                            ("process", "outer_wall_speed"): Every(500),
                            ("process", "default_acceleration"): Every(20000),
                            ("process", "outer_wall_acceleration"): Every(20000)}}
        label = f"{s} 15"
        self.generate(label, {"test": "input_shaping_freq", "start": 20, "end": 80},
                      {"start": 20, "end": 80, "start_y": 20, "end_y": 80, "damping": 0.15, "shaper_type": "Default",
                       "test_model": "ringing_tower"},
                      project="Input shaping Frequency test", names=["ringing_tower.drc"],
                      result_keys={"type": "printer", "keys": ["input_shaping_emit", "input_shaping_type",
                                                               "input_shaping_freq_x", "input_shaping_freq_y",
                                                               "input_shaping_damp_x", "input_shaping_damp_y"]},
                      **shaping)
        self.slice(label)

        label = f"{s} 16"
        self.generate(label, {"test": "input_shaping_damp", "freq_x": 48, "freq_y": 42, "test_model": "fast_tower"},
                      {"freq_x": 48, "freq_y": 42, "start": 0, "end": 0.4, "shaper_type": "Default",
                       "test_model": "fast_tower"},
                      project="Input shaping Damping test", names=["fast_tower_test.drc"], **shaping)
        self.slice(label)

        label = f"{s} 17"
        reply = self.generate(
            label, {"test": "cornering", "test_model": "scv_v2", "end": 25}, {"start": 1, "end": 25, "test_model": "scv_v2"},
            project="Cornering test", names=["SCV-V2.drc"],
            changes={"slow_down_layer_time": "0", "slow_down_min_speed": "0", "slow_down_for_layer_cooling": "0",
                     "filament_max_volumetric_speed": "200", "enable_pressure_advance": Every("1"),
                     "pressure_advance": Every("0")},
            sets={("printer", "machine_max_jerk_x"): "25", ("printer", "machine_max_jerk_y"): "25",
                  ("printer", "input_shaping_emit"): "1", ("printer", "input_shaping_type"): "Disable",
                  ("filament", "slow_down_layer_time"): "0", ("filament", "slow_down_min_speed"): "0",
                  ("filament", "slow_down_for_layer_cooling"): "0", ("filament", "filament_max_volumetric_speed"): "200",
                  ("filament", "enable_pressure_advance"): Every("1"), ("filament", "pressure_advance"): Every("0")},
            result_keys={"type": "printer", "keys": ["machine_max_jerk_x", "machine_max_jerk_y"]})
        t.equal(f"{label}: warnings", reply.get("warnings"), ["end 25 is above 20: high values may cause layer shifts"])
        self.slice(label)

    def flow_ratios(self, label, ratio, pattern=None):
        """Each object's own print_flow_ratio, `ratio` of the number its name ends in, and with `pattern` its own
        top_surface_pattern."""
        objects = self.objects()
        got = [float(self.own(i, "print_flow_ratio")) for i in range(len(objects))]
        self.t.near(f"{label}: each object's own flow ratio", got, [ratio(flow_offset(o["name"])) for o in objects],
                    0.0001)
        if pattern is not None:
            self.t.equal(f"{label}: each object's own top surface pattern",
                         [self.own(i, "top_surface_pattern") for i in range(len(objects))], [pattern] * len(objects))

    def retraction_bands(self, label, path):
        """The layers cut into bands at each `; Calib_Retraction_tower: ...` line: the bands' lengths in a row, and the
        lengths each length's bands restore."""
        bands = []
        for line in gcode_layers(path):
            m = re.match(r"; Calib_Retraction_tower: Z_HEIGHT: [0-9.]+, length:([0-9.]+)", line)
            if m:
                bands.append((float(m.group(1)), []))
                continue
            words = line.split(";")[0].split()
            if bands and len(words) == 3 and words[0] == "G1" and sorted(w[0] for w in words[1:]) == ["E", "F"]:
                e = float(next(w for w in words[1:] if w[0] == "E")[1:])
                if e > 0:
                    bands[-1][1].append(e)
        lengths = [round(0.1 * i, 6) for i in range(11)]
        self.t.near(f"{label}: the tower's retraction lengths, band by band", in_a_row([b[0] for b in bands]), lengths,
                    1e-9)
        restored = {}
        for length, deretractions in bands:
            restored.setdefault(round(length, 3), set()).update(round(e, 3) for e in deretractions)
        want = {length: {length} for length in lengths}
        want[0.0] = {0.8}  # the first layer's retraction, made before the tower's first line
        self.t.equal(f"{label}: the lengths each band's deretractions restore",
                     {k: sorted(v) for k, v in restored.items()}, {k: sorted(v) for k, v in want.items()})

    def keep_and_discard(self):
        s = "keep and discard"
        t = self.t
        t.equal(f"{s} 1: the start", start(self.c), START)
        t.equal(f"{s} 1: the slot added", add_slot(self.c, SLOT_COLOURS[1]), 2)
        self.set("filament", filament_slot=2, values={"filament_cost": 21})
        self.run("preset_save", type="filament", name=CALIB_PLA, filament_slot=2)
        self.select("filament", PLA, slot=1)
        self.set("filament", filament_slot=2, values={"filament_cost": 22})
        after1 = named("Klipper | Std | PLA, MCP Calib PLA[filament_cost 22]")
        self.named_selection_is(f"{s} 1", after1)
        saved_cost = lambda: self.value("filament", "filament_cost", preset=CALIB_PLA)  # noqa: E731
        t.equal(f"{s} 1: the saved cost", saved_cost(), "21")

        self.refused(f"{s} 2: keep is refused while the tab edits another slot's modified preset",
                     lambda: self.c.call("calibration_generate_test", test="retraction", discard_changes=True,
                                         preset_changes="keep"),
                     words='The filament tab edits "MCP Calib PLA", which has unsaved changes, and the test prints '
                           f'with filament slot 1 ("{PLA}")')
        self.named_selection_is(f"{s} 2", after1)
        t.equal(f"{s} 2: no objects", self.objects(), [])
        t.equal(f"{s} 2: the saved cost", saved_cost(), "21")

        reply = self.run("calibration_generate_test", test="retraction", **DISCARD)
        filament = reply.get("presets_changed", {}).get("filament", {})
        t.equal(f"{s} 3: the filament tab switched to slot 1",
                {k: filament.get(k) for k in ("name", "tab_switched_from")},
                {"name": PLA, "tab_switched_from": CALIB_PLA})
        t.equal(f"{s} 3: discarded", reply.get("discarded", {}).get("filament"),
                {"name": CALIB_PLA, "keys": ["filament_cost"]})
        t.equal(f"{s} 3: the reply's names", reply.get("names"), ["retraction_tower.drc"])
        t.equal(f"{s} 3: the objects", [o["name"] for o in self.objects()], ["retraction_tower.drc"])
        self.named_selection_is(f"{s} 3", named("Klipper | Std | PLA, MCP Calib PLA"))
        t.equal(f"{s} 3: slot 2's cost", self.value("filament", "filament_cost", slot=2), "21")

        self.set("filament", values={"slow_down_layer_time": 7})
        reply = self.run("calibration_generate_test", test="retraction", discard_changes=True, preset_changes="keep")
        t.equal(f"{s} 4: modified", reply.get("modified", {}).get("filament", {}).get("keys"), ["slow_down_layer_time"])
        t.equal(f"{s} 4: nothing discarded", reply.get("discarded"), None)
        self.named_selection_is(f"{s} 4", named("Klipper | Std | PLA[slow_down_layer_time 7], MCP Calib PLA"))

        reply = self.run("project_new", **DISCARD)
        t.check(f"{s} 5: discarded holds slow_down_layer_time",
                "slow_down_layer_time" in reply.get("discarded", {}).get("filament", {}).get("keys", []), reply)
        t.equal(f"{s} 5: nothing modified", reply.get("modified"), None)
        self.named_selection_is(f"{s} 5", named("Klipper | Std | PLA, MCP Calib PLA"))
        t.equal(f"{s} 5: slow_down_layer_time", self.value("filament", "slow_down_layer_time"), "4")

        temperatures = {key: times(240, 6) for key in TEMPERATURE_KEYS}
        reply = self.run("calibration_generate_test", test="temperature", filament_slot=2, start=240, end=220, **DISCARD)
        t.equal(f"{s} 6: the reply's project", reply.get("project"), "Nozzle temperature test")
        t.equal(f"{s} 6: params", reply.get("params"), {"start": 240, "end": 220, "auto_scale": True})
        t.equal(f"{s} 6: the filament's part of presets_changed", reply.get("presets_changed", {}).get("filament"),
                {"name": CALIB_PLA, "filament_slot": 2, "tab_switched_from": PLA, "changes": temperatures})
        after6 = named(f"Klipper | Std | PLA, MCP Calib PLA[nozzle_temperature {times(240, 6)}, "
                       f"nozzle_temperature_initial_layer {times(240, 6)}]")
        self.named_selection_is(f"{s} 6", after6)
        t.equal(f"{s} 6: the tower prints with slot 2", self.object_filaments(), [("temperature_tower.drc", 2)])

        path = self.slice(f"{s} 7", "slot2_temperature.gcode")
        temps = [float(m.group(1)) for line in gcode_layers(path) if (m := re.match(r"M104 S([0-9.]+)", line))]
        t.equal(f"{s} 7: slot 2's temperatures, block by block", in_a_row([v for v in temps if v != 0]),
                [240, 235, 230, 225, 220])

        self.run("filament_add_mixed", components=[1, 2], ratios=[50, 50])
        t.equal(f"{s} 8: the slots after the add", slots(self.c), "0,0,1")
        for bad, words in ((3, "Filament slot 3 is a mixed filament"),
                           (4, "filament_slot must be 1..2, the physical filaments")):
            self.refused(f"{s} 8: slot {bad} refused",
                         lambda bad=bad: self.c.call("calibration_generate_test", test="retraction", filament_slot=bad,
                                                     **DISCARD),
                         words=words)
            t.equal(f"{s} 8: slot {bad} leaves the tower", self.object_filaments(), [("temperature_tower.drc", 2)])
        self.run("filament_delete", filament_slot=3)
        t.equal(f"{s} 8: the slots after the delete", slots(self.c), "0,0")
        self.named_selection_is(f"{s} 8: before the discard", after6)
        self.run("project_new", **DISCARD)
        self.named_selection_is(f"{s} 8", named("Klipper | Std | PLA, MCP Calib PLA"))

        reply = self.run("filament_delete", filament_slot=2)
        t.equal(f"{s} 9: the slot's delete", reply.get("level"), "success")
        reply = self.run("preset_delete", type="filament", name=CALIB_PLA)
        t.equal(f"{s} 9: the preset's delete", reply.get("deleted"), CALIB_PLA)
        self.named_selection_is(f"{s} 9", named("Klipper | Std | PLA"))
        # Read by name: the filaments listed besides hold those earlier tests installed or saved.
        listed = {i["name"]: i for i in self.listed("filament")}
        t.equal(f"{s} 9: PLA listed not modified, MCP Calib PLA not listed",
                (PLA in listed, (listed.get(PLA) or {}).get("modified"), CALIB_PLA in listed), (True, None, False))

    def another_extruder(self):
        s = "another extruder"
        t = self.t
        t.equal(f"{s} 1: the start", start(self.c), START)
        for n in range(1, 5):
            t.equal(f"{s} 1: slot {n + 1} added", add_slot(self.c, SLOT_COLOURS[n]), n + 1)
        self.named_selection_is(f"{s} 1: before the switch", named("Klipper | Std | " + ", ".join(["PLA"] * 5)))
        self.without_remembered_presets()
        self.select("printer", TOOLCHANGER)
        self.named_selection_is(f"{s} 1", named("ToolChanger | TC Std | " + ", ".join(["TC PLA"] * 5)))
        t.equal(f"{s} 1: the nozzles", self.value("printer", "nozzle_diameter"), times(0.4, 5))
        self.extruder_page(f"{s} 1")
        t.equal(f"{s} 1: the Speed Dial has an action per extruder", self.dial("retraction_length"),
                [(f"orca_setting:retraction_length#{n}:5", f"Printer : Extruder {n + 1} : Retraction")
                 for n in range(5)])

        # Tool 2's nozzle differs from tool 1's, so what a test takes from its extruder tells the two apart.
        tool2_06 = {"nozzle_diameter": "0.4,0.6,0.4,0.4,0.4"}
        self.set("printer", values=tool2_06)
        reply = self.run("calibration_generate_test", test="max_volumetric", filament_slot=2, discard_changes=True,
                         preset_changes="keep")
        t.equal(f"{s} 2: the reply's project", reply.get("project"), "Max volumetric speed test")
        t.equal(f"{s} 2: params", reply.get("params"), {"start": 5, "end": 20, "step": 0.5})
        t.equal(f"{s} 2: the filament's part of presets_changed", reply.get("presets_changed", {}).get("filament"),
                {"name": TOOLCHANGER_PLA, "filament_slot": 2, "extruder": 2,
                 "changes": {"filament_max_volumetric_speed": times(200, 3), "slow_down_layer_time": "0"}})
        t.equal(f"{s} 2: the tower's own layer height and wall width, tool 2's 0.6 nozzle",
                (self.own(0, "layer_height"), self.own(0, "outer_wall_line_width")), ("0.48", "1.05"))
        t.equal(f"{s} 2: max_layer_height raised to the layer height", self.value("printer", "max_layer_height"),
                times(0.48, 5))
        t.equal(f"{s} 2: the tower prints with slot 2", self.object_filaments(), [("SpeedTestStructure.drc", 2)])

        path = self.slice(f"{s} 3", "tool2_max_volumetric.gcode")
        with open(path, encoding="utf-8", errors="replace") as f:
            tools = sorted({m.group(0) for line in f if (m := re.match(r"T\d+", line))})
        t.equal(f"{s} 3: the tools the G-code picks", tools, ["T1"])

        self.run("project_new", **DISCARD)
        self.set("printer", values=tool2_06)
        reply = self.run("calibration_generate_test", test="max_volumetric", discard_changes=True, preset_changes="keep")
        filament = reply.get("presets_changed", {}).get("filament", {})
        t.equal(f"{s} 4: the slot and its extruder", {k: filament.get(k) for k in ("filament_slot", "extruder")},
                {"filament_slot": 1, "extruder": 1})
        t.equal(f"{s} 4: the tower's own layer height and wall width, tool 1's 0.4 nozzle",
                (self.own(0, "layer_height"), self.own(0, "outer_wall_line_width")), ("0.32", "0.7"))
        t.equal(f"{s} 4: the tower prints with slot 1", self.object_filaments(), [("SpeedTestStructure.drc", 1)])

        t.equal(f"{s} 5: the start", start(self.c), START)
        self.hide_printers()
        printers = self.names("printer")
        t.equal(f"{s} 5: the Klipper listed and the ToolChanger not", (KLIPPER in printers, TOOLCHANGER in printers),
                (True, False))
        # After a printer of five extruders, the Klipper keeps no row for the four it lacks.
        self.extruder_page(f"{s} 5")
        t.equal(f"{s} 5: the Speed Dial has one action", self.dial("retraction_length"),
                [("orca_setting:retraction_length#0:5", "Printer : Extruder : Retraction")])
        t.equal(f"{s} 5: no setting on a numbered extruder page", self.numbered(), [])

    def extruder_page(self, label):
        """The entry's Extruder page check: the printer has one Extruder page, whose Retraction group holds the ten
        retraction keys, each placed there."""
        pages = self.c.call("settings_list_pages", type="printer")["pages"]
        self.t.equal(f"{label}: one Extruder page", [p["page"] for p in pages if p["page"].startswith("Extruder")],
                     ["Extruder"])
        self.t.equal(f"{label}: its Retraction group counts ten", group_size(pages, "Extruder", "Retraction"), 10)
        listed = self.settings_pages(type="printer", where="Extruder > Retraction")
        self.t.equal(f"{label}: the retraction keys, each on the Extruder page",
                     sorted((i["key"], i.get("meta", {}).get("where")) for i in listed),
                     sorted((key, "Extruder > Retraction") for key in RETRACTION_KEYS))

    def setting_actions(self):
        """Every item of the Speed Dial's listing with its settings, page after page."""
        items, offset = [], 0
        while offset is not None:
            page = self.c.call("action_list", include_settings=True, offset=offset, limit=200)
            items += page["items"]
            offset = page.get("next_offset")
        return items

    def dial(self, key):
        """The entry's dial(key): the Speed Dial's actions for the printer setting `key`, as (id, section)."""
        return sorted((a["id"], a.get("section")) for a in self.setting_actions()
                      if a["id"].startswith(f"orca_setting:{key}#"))

    def numbered(self):
        """The entry's numbered(): the Speed Dial's actions on a numbered extruder page, as (id, section)."""
        return [(a["id"], a.get("section")) for a in self.setting_actions()
                if re.match(r"Printer : Extruder \d+ : ", a.get("section") or "")]

    def hide_printers(self):
        """Hides every printer that is neither installed nor selected, the hidden printers among them, with the
        Klipper selected. A printer that preset_select shows stays shown until the presets are reloaded, and
        uninstalling a printer that is not installed reloads nothing. Installing the printers first makes the
        uninstall a change, whose reload hides every printer that is neither installed nor selected, whatever showed
        it. The install selects a nozzle it adds, so the Klipper is selected again in between. What the two change
        depends on the tests before, so their replies are shown but not checked."""
        print("     (installed:", self.run("printer_install", models=HIDDEN_PRINTERS, preset_changes="discard"),
              ")", flush=True)
        self.select("printer", KLIPPER, preset_changes="discard")
        print("     (uninstalled:", self.run("printer_uninstall", models=HIDDEN_PRINTERS, preset_changes="discard"),
              ")", flush=True)

    def setup(self):
        """The state the scenarios count on beyond the baseline, which an earlier test may have left changed: the
        hidden presets hidden, and no preset of the names this test saves."""
        start(self.c)
        self.hide_printers()
        for type, names in (("filament", OWN_FILAMENTS), ("process", OWN_PROCESSES)):
            for name in names:
                if name in self.names(type, query=name):
                    print(f"     (deleting the {type} {name!r} left on the folder)", flush=True)
                    self.run("preset_delete", type=type, name=name)
        if OWN_PRINTER in self.names("printer", query=OWN_PRINTER):
            print(f"     (deleting the printer {OWN_PRINTER!r} left on the folder)", flush=True)
            self.select("printer", OWN_PRINTER, preset_changes="discard")  # preset_delete takes the selected printer
            self.run("preset_delete", type="printer", name=OWN_PRINTER)

    def run_all(self):
        self.t.scenario("the start", self.setup)
        for name, scenario in [("hidden presets", self.hidden), ("listing presets", self.listing),
                               ("what a setting is", self.settings), ("saving a filament", self.saving),
                               ("dropping and reverting edits", self.reverting),
                               ("reading presets that are not selected", self.not_selected),
                               ("switching", self.switching), ("the plugin API", self.plugin_api),
                               ("what a switch carries", self.carries), ("a new project", self.new_project),
                               ("a process kept inside the project", self.inside_project),
                               ("deleting presets", self.deleting), ("exporting and importing presets", self.exporting),
                               ("a nozzle diameter", self.nozzle), ("what the print is sliced with", self.print),
                               ("a preference changed in its dialog", self.preference_dialog),
                               ("the temperature tower, and what is refused", self.temperature),
                               ("the other tests", self.other_tests), ("keep and discard", self.keep_and_discard),
                               ("another extruder, on the ToolChanger", self.another_extruder)]:
            self.t.scenario(name, scenario)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-presets-") as folder:
        return PresetsTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
