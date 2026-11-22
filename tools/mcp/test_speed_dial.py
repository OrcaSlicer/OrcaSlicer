#!/usr/bin/env python3
"""The Speed Dial's actions as action_list and action_run give them, as the test_speed_dial entry of
tools/mcp/test-entries/commands.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_speed_dial.py [--out DIR]

action_list lists every action of tests/data/speed_dial_actions.json with the command
tests/data/speed_dial_action_calls.json gives it. Then ROWS runs a few actions through action_run, each standing for
one way an action runs: a person's form that opens a dialog, one that runs in place, a window opened, fixed arguments
that choose what the command does, a value collected, a file written, a refusal, and a value its command's schema
refuses, whose refusal names tool_describe for the command. Before each, its state is set up
afresh by calls (STATES), and its dialog is answered as the row says. For each that ran, the reply's ran is the
action's id, and the step replay_export writes for the call is an action_run of that id with the arguments its command
journals: the value under the action's input token for Go to tab, and none for the others (a step with the id alone
replays the same, since the action supplies its fixed arguments). A refused action has neither: action_run adds ran
only when the command ran, and a failed call is not exported.

It starts from the Generic Klipper Printer (checks.start()). Take Screenshot saves in Start Recording's folder, which
the test points at its output folder (a new temporary folder unless --out names one) through the Start Recording dialog
before the actions run. Exits 0 when every check passes.
"""

import argparse
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import checks  # noqa: E402
import orca_mcp  # noqa: E402

DATA = os.path.join(HERE, "..", "..", "tests", "data")
PREFIX, SUFFIX = "orca_command:", ":orca"
BED_CENTRE = 125.0  # the middle of the Generic Klipper Printer's 250 mm bed
START = {"printer": checks.KLIPPER, "process": checks.KLIPPER_STD, "filaments": [checks.GENERIC_PLA]}
ACTIONS = 129  # the actions of speed_dial_actions.json
RESTORE = {"match": "restore", "button": "no"}  # the prompt to restore unsaved items after a start


def action_id(key):
    return PREFIX + key + SUFFIX


def names(client):
    """The objects' names, in object_list's order of plates, then those outside every plate."""
    listing = client.call("object_list", limit=500)
    return [o["name"] for p in listing["plates"] for o in p["objects"]] + [o["name"] for o in listing.get("outside", [])]


# ---- States: each set up afresh by calls before its action ----

def empty(client):
    client.call("dialog_auto_answer", rules=[RESTORE])
    if client.call("app_state").get("recording"):
        client.call("recording_stop", silent=True)
    client.call("project_new", discard_changes=True, preset_changes="discard")
    client.call("view_set", tab="prepare", canvas="3d")
    while len(checks.filament_rows(client)) > 1:
        client.call("filament_delete")
    client.call("settings_set_mode", mode="expert")
    checks.wait(client, "idle")


def two_cubes(client):
    """Two 20 mm cubes side by side on the plate, the first selected."""
    empty(client)
    for x in (100, 156):
        client.call("object_select", none=True)
        client.call("object_add_primitive", shape="cube", silent=True, size=[20, 20, 20], position=[x, BED_CENTRE, 10])
    client.call("object_select", objects=[0])


STATES = {"empty": empty, "two_cubes": two_cubes}


# ---- What each row reads back after its action ----

def calibration(client, t, name, out, reply):
    project = client.call("app_state")["project"]
    t.equal(f"{name}: the project is named after the test", project["name"], "Retraction")
    count = len(names(client))
    t.check(f"{name}: the project has at least 1 object", count >= 1, f"{count} objects")


def combo(client, t, name, out, reply):
    t.equal(f"{name}: the objects are the combo's, in order", names(client),
            ["Orca Head", "OrcaCube_v2.drc", "OrcaPlug_v2.drc", "Orca Belly", "Orca Pedestal"])
    t.equal(f"{name}: no dialog is open", checks.dialogs(client), [])


def rear_view(client, t, name, out, reply):
    cam = client.call("app_state")["camera"]
    t.near(f"{name}: the camera's elevation", cam.get("elevation"), 0, 2)
    t.near(f"{name}: the camera's azimuth", cam.get("azimuth"), 90, 2)


def preview_tab(client, t, name, out, reply):
    t.equal(f"{name}: the tab is the Preview", client.call("app_state")["tab"], "preview")


def screenshot(client, t, name, out, reply):
    path = reply.get("path") or ""
    t.equal(f"{name}: the screenshot is in the output folder", os.path.normcase(os.path.dirname(path)),
            os.path.normcase(out))
    size = os.path.getsize(path) if os.path.isfile(path) else None
    t.check(f"{name}: the screenshot has bytes", bool(size), f"{path}: {size}")


def row(key, state, expect=None, *, value=None, journal=None, dialog=None, title_exact=True, pre=(), refused=None, hint=None):
    """One action's row: the state it runs from, the calls `pre` makes on that state, the value it is given, the
    dialog it opens as (title, button to press), and what it reads back (`expect`), or the code it is refused with and
    the hint its refusal gives, when it gives one. `journal` is the inner args its action_run step keeps."""
    return {"key": key, "state": state, "expect": expect, "value": value, "journal": journal or {}, "dialog": dialog,
            "title_exact": title_exact, "pre": tuple(pre), "refused": refused, "hint": hint}


ROWS = [
    row("calib_retraction", "empty", calibration, dialog=("Retraction", "ok")),
    row("add_handy_orcasliced_combo", "empty", combo),
    row("help_keyboard_shortcuts", "empty", dialog=("Keyboard Shortcuts", "close"), title_exact=False),
    row("view_rear", "two_cubes", rear_view, pre=[("view_set", {"camera": "top"})]),
    row("go_to_tab", "two_cubes", preview_tab, value={"tab": "preview"}, journal={"tab": "preview"}),
    row("take_screenshot", "two_cubes", screenshot),
    row("sync_presets", "empty", refused="not_signed_in"),
    # The value the action collects, which view_set's layer_percent minimum of 0 refuses: a schema's refusal.
    row("go_to_layer", "empty", value={"percent": -1}, refused="invalid_argument",
        hint='tool_describe {tool: "view_set"} gives its parameters'),
]


def listed_commands(client):
    """{action id: command} for every action action_list lists, read page by page."""
    commands, offset = {}, 0
    while True:
        page = client.call("action_list", limit=200, offset=offset)
        commands.update({a["id"]: a.get("command") for a in page.get("items", [])})
        offset = page.get("next_offset")
        if not offset:
            return commands


def use_capture_folder(client, out):
    """Points Start Recording's folder, where Take Screenshot saves too, at the output folder: the dialog's Change...
    picker takes it, and a recording started and stopped there makes the dialog keep it. Returns the folder of the
    recording it stopped."""
    client.call("dialog_auto_answer", rules=[RESTORE, {"match": ".", "button": "ok", "path": out}])
    client.call("recording_start")
    client.call("widget_click", target={"label": "Change...", "within": "Start Recording"})
    client.call("dialog_answer", dialog={"title": "Start Recording"}, button="Start recording")
    stopped = client.call("recording_stop", silent=True)
    client.call("dialog_auto_answer", rules=[RESTORE])
    return os.path.normcase(os.path.dirname(stopped.get("path", "")))


def journal_step(client, key):
    """The action_run step replay_export writes for the action of `key`, or None."""
    steps = client.call("replay_export", clear=True)["replay"]["steps"]
    return next((s for s in steps if s.get("tool") == "action_run" and s.get("args", {}).get("id") == action_id(key)), None)


def run_row(client, t, out, r):
    key = r["key"]
    name = f"{key} ({r['state']})"
    STATES[r["state"]](client)
    for tool, args in r["pre"]:
        checks.run(client, tool, **args)
    checks.wait(client, "idle")
    client.call("replay_export", clear=True)
    call = {"id": action_id(key), **({"args": r["value"]} if r["value"] else {})}
    if r["refused"]:
        details = {}

        def refused_call():
            try:
                return client.call("action_run", **call)
            except orca_mcp.McpError as e:
                details.update(checks.refusal(e)[1])
                raise

        t.refused(f"{name}: refused as {r['refused']}", refused_call, code=r["refused"])
        if r["hint"]:
            t.equal(f"{name}: its refusal names tool_describe for the command", details.get("hint"), r["hint"])
            t.equal(f"{name}: the tab is still Prepare", client.call("app_state")["tab"], "prepare")
        t.check(f"{name}: the journal keeps no step for it", journal_step(client, key) is None)
        return
    first = client.call("action_run", **call)
    if r["dialog"]:
        title, button = r["dialog"]
        checks.wait(client, "dialog", 30)
        shown = [d["title"] for d in checks.dialogs(client)]
        opened = title in shown if r["title_exact"] else any(title in s for s in shown)
        t.check(f"{name}: its dialog {title!r} opens", opened, shown)
        client.call("dialog_answer", dialog={"title": title}, button=button)
    reply = client.await_job(first)
    checks.wait(client, "idle", 50)
    if r["expect"]:
        r["expect"](client, t, name, out, reply)
    t.equal(f"{name}: its reply names the action it ran", reply.get("ran"), action_id(key))
    step = journal_step(client, key)
    given = step.get("args", {}).get("args", {}) if step else None
    t.check(f"{name}: the journal keeps its action_run with the arguments its command journals",
            step is not None and given == r["journal"], (step, r["journal"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="output folder (default: a new temporary one)")
    args = ap.parse_args()
    out = os.path.abspath(args.out) if args.out else tempfile.mkdtemp(prefix="speed-dial-")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(DATA, "speed_dial_actions.json"), encoding="utf-8") as f:
        keys = [a["key"] for a in json.load(f)]
    with open(os.path.join(DATA, "speed_dial_action_calls.json"), encoding="utf-8") as f:
        fixture = {c["key"]: c["command"] for c in json.load(f)}

    client, _session = orca_mcp.session_client()
    checks.wait(client, "ready", 50)
    t = checks.Checks()
    t.equal("starts from the Generic Klipper Printer", checks.start(client), START)
    t.check("every action has a call in the fixture", len(keys) == ACTIONS and set(keys) <= set(fixture),
            (len(keys), sorted(set(keys) - set(fixture))))
    listed = listed_commands(client)
    wrong = {k: (listed.get(action_id(k)), fixture.get(k)) for k in keys if listed.get(action_id(k)) != fixture.get(k)}
    t.check("every action is listed with its command", len(keys) == ACTIONS and not wrong, wrong)
    empty(client)
    t.equal("Start Recording's folder is the output folder", use_capture_folder(client, out), os.path.normcase(out))
    for r in ROWS:
        t.scenario(r["key"], lambda r=r: run_row(client, t, out, r))
        for _ in checks.dialogs(client):
            try:
                client.call("dialog_answer", button="close")
            except orca_mcp.McpError:
                pass
    empty(client)
    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
