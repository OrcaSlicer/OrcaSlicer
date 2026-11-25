#!/usr/bin/env python3
"""Checks that a call with exactly an action's arguments asks as a person's pick does, that one with a path does not,
and that a person's pick of a view action or Deselect All runs in place, as its menu item did.
Its design is the test_person_forms entry of tools/mcp/test-entries/commands.md.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_person_forms.py

project_save with a path writes the file without a picker. project_save {save_as: true} and project_export {format:
gcode} open their file pickers, which a dialog_auto_answer rule cancels: the rule says it answered a picker for each,
the save is an error, and nothing is written where either picker starts (the project's folder, or the folder the app
remembers for its last export). preset_export always opens the Export Preset Bundle dialog (it has no silent form): on a
folder it replies blocked_by_dialog, a cancelled dialog ends the call in an error with nothing written to the folder,
and silent is refused. view_set's tab refusal is the operation's, with its code.

Picks from the Speed Dial, opened with Space on the 3D view and searched by title as a person types: View: Top turns
the camera, Deselect All on the Preview tab clears the Prepare selection without leaving the tab, and Go to layer takes
its percentage from the second phase. None of them starts a job or shows a notice, while a pick of Slice and Preview,
the last one, made the same way on the same project, does both.

Auto-Arrange and Select All Objects, on a plate of three cubes: object_arrange {} and action_run of its action wait
and reply as the command does (the settings in force, what was left off the plates); the journal keeps the call with
the settings in force, and action_run of the action with its own arguments, whose exported replay runs. object_select
{all: true} and action_run of its action reply with the selection; in the assemble view the call selects there and
leaves the view shown, and with the Measure gizmo open there it is refused as gizmo_open. A pick of either runs in
place: Auto-Arrange puts a cube moved off the plate back on it, and Select All Objects selects the three, in Prepare,
which it shows when picked in the assemble view. A replay cannot expect a failed step, which is why this is a script.
Exits 0 when every check passes.
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks as shared  # noqa: E402
import orca_mcp  # noqa: E402

START = {"printer": shared.KLIPPER, "process": shared.KLIPPER_STD, "filaments": [shared.GENERIC_PLA]}
CUBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "OrcaCube_v2.drc")
# The prompts of the save and export pickers ("Save file as", "Save G-code file as:"), which have no title until shown.
PICKER = "save"
# Where the three cubes of the arrange and select checks stand on the bed, and where one goes off it: left of the bed,
# where no plate is.
CUBES_X = (60, 128, 196)
OFF_PLATE = [-150, 128, 10]


def last_output_dir(session):
    """The folder the app remembers for its last export (app.last_export_path), where the G-code picker starts; None when
    it has none. On Windows the app writes a checksum line after the config file's JSON, so only the JSON is read."""
    try:
        with open(os.path.join(session["datadir"], "OrcaSlicer.conf"), encoding="utf-8") as f:
            text = f.read()
        return json.loads(text[:text.rfind("}") + 1]).get("app", {}).get("last_export_path") or None
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return None


def main():
    client, session = orca_mcp.session_client()
    work = tempfile.mkdtemp(prefix="orca-person-forms-")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def picker_uses():
        for rule in client.call("app_state").get("auto_answer_rules", []):
            if rule.get("match") == PICKER:
                return rule.get("uses", 0)
        return None

    def written_since(folders, since):
        """The files in `folders` written at or after `since`."""
        out = []
        for folder in folders:
            for name in os.listdir(folder):
                if os.path.getmtime(os.path.join(folder, name)) >= since:
                    out.append(os.path.join(folder, name))
        return out

    def last_job():
        """The number of the newest job app_state lists; every call of this script is one."""
        return max([int(j["id"][1:]) for j in client.call("app_state").get("jobs", []) if j.get("id", "").startswith("j")] or [0])

    def settled():
        shared.wait(client, "idle")
        return client.call("app_state")

    def job_notices(state):
        """The notices a person's pick that runs as a job shows: "Started: <title>", then "Finished: <title>"."""
        return [n["text"] for n in state.get("notifications", []) if "Started" in n.get("text", "")
                or "Finished" in n.get("text", "")]

    def ran_in_place(name, command, state, since):
        # A pick started as a job is listed under its command, beside the input calls that made the pick.
        jobs = [j for j in state.get("jobs", []) if int(j["id"][1:]) > since and j.get("tool") == command]
        check(f"{name} starts no {command} job", jobs == [], jobs)
        notes = job_notices(state)
        check(f"{name} shows no Started or Finished notice", notes == [], notes)

    def outside():
        """The objects object_list lists off every plate."""
        return client.call("object_list").get("outside", [])

    def cube_off_plate(name):
        """Moves a cube off the plate, so that the arrangement `name` checks has something to put back."""
        client.call("object_transform", object=2, position=OFF_PLATE)
        check(f"before {name}, a cube is off the plate", len(outside()) == 1, outside())

    def refusal_code(error):
        """The code of a refusal, whose details are on the line after its text; None for another error."""
        try:
            return json.loads(str(error).rpartition("\n")[2]).get("code")
        except (ValueError, AttributeError):
            return None

    shared.wait(client, "ready", 50)
    presets = shared.start(client)
    check("starts from the Generic Klipper Printer", presets == START, presets)
    client.call("project_import", paths=[os.path.abspath(CUBE)])
    client.call("object_transform", object=0, size=[20, 20, 20])
    sliced = client.call("plate_slice")
    check("the plate slices", sliced.get("status") == "done", sliced.get("status"))

    # ---- A path skips the picker; the project's folder is then where both pickers start ----
    client.call("dialog_auto_answer", rules=[{"match": "restore", "button": "no"}, {"match": PICKER, "button": "cancel"}])
    before = picker_uses()
    path = os.path.join(work, "saved.3mf")
    client.call("project_save", path=path)
    check("project_save with a path writes the file", os.path.isfile(path), os.listdir(work))
    check("with no picker", picker_uses() == before, f"{before} -> {picker_uses()}")

    # ---- The actions' arguments open the pickers, which the rule cancels ----
    last = last_output_dir(session)
    folders = [work] + ([last] if last and os.path.isdir(last) else [])
    since = time.time()
    try:
        saved = client.call("project_save", save_as=True)
        check("a cancelled Save Project As is an error", False, saved)
    except orca_mcp.McpError as e:
        check("a cancelled Save Project As is an error", True, str(e))
    after_save = picker_uses()
    check("Save Project As opened its picker", after_save == before + 1, f"{before} -> {after_save}")
    client.call("project_export", format="gcode")
    after_export = picker_uses()
    check("Export G-code opened its picker", after_export == after_save + 1, f"{after_save} -> {after_export}")
    check("nothing was written where the pickers start", written_since(folders, since) == [],
          {"folders": folders, "written": written_since(folders, since)})

    # ---- preset_export opens its dialog whatever it is given ----
    client.call("dialog_auto_answer", rules=[])
    folder = os.path.join(work, "bundle")
    os.makedirs(folder)
    blocked = client.call("preset_export", path=folder)
    dialogs = blocked.get("dialogs", [])
    check("preset_export on a folder is blocked by its dialog", blocked.get("status") == "blocked_by_dialog", blocked.get("status"))
    check("titled Export Preset Bundle", bool(dialogs) and "Export Preset Bundle" in dialogs[-1].get("title", ""), dialogs)
    client.call("dialog_answer", dialog={"title": "Export Preset Bundle"}, button="cancel")
    try:
        ended = client.await_job(blocked)
        check("a cancelled Export Preset Bundle is an error", False, ended)
    except orca_mcp.McpError as e:
        check("a cancelled Export Preset Bundle is an error", True, str(e))
    check("with nothing written to the folder", os.listdir(folder) == [], os.listdir(folder))
    try:
        refused = client.call("preset_export", silent=True)
        check("preset_export refuses silent", False, refused)
    except orca_mcp.McpError as e:
        check("preset_export refuses silent", "silent" in str(e), str(e))

    # ---- view_set's tab refusal is the operation's ----
    try:
        client.call("view_set", tab="no_such_page")
        check("view_set refuses a tab no page has", False)
    except orca_mcp.McpError as e:
        check("view_set refuses a tab no page has, in the refusal's words", str(e).startswith('No tab has the id "no_such_page".'),
              str(e))
        check("with the refusal's code", "no_such_tab" in json.dumps(e.payload), e.payload)

    # ---- Picks from the Speed Dial run in place ----
    # A pick an earlier test made in this app can have left its Started or Finished notice, which fades about 10 s
    # after it shows (a regular notice); the picks here must show none of their own.
    try:
        shared.until(lambda: not job_notices(client.call("app_state")), "no Started or Finished notice shows")
        check("before the picks, no Started or Finished notice shows", True)
    except TimeoutError as e:
        check("before the picks, no Started or Finished notice shows", False, str(e))
    client.call("view_set", tab="prepare", camera="front")
    since = last_job()
    shared.dial_pick(client, "View: Top")
    state = settled()
    check("a pick of View: Top turns the camera", state.get("camera", {}).get("elevation", 0) >= 85, state.get("camera"))
    ran_in_place("View: Top", "view_set", state, since)

    client.call("object_select", objects=[0])
    client.call("view_set", tab="preview")
    since = last_job()
    shared.dial_pick(client, "Deselect All")
    state = settled()
    check("a pick of Deselect All on the Preview tab stays there", state.get("tab") == "preview", state.get("tab"))
    ran_in_place("Deselect All", "object_select", state, since)
    client.call("view_set", tab="prepare")
    check("and the Prepare selection is clear", client.call("app_state").get("selection", {}).get("empty") is True,
          client.call("app_state").get("selection"))

    # Go to layer's second phase takes the percentage typed in the search field, and Enter runs it.
    client.call("view_set", tab="preview")
    since = last_job()
    shared.dial_pick(client, "Go to layer (percent)")
    client.call("widget_set_value", target={"web": "Search actions"}, value="50", submit=False)
    client.call("keyboard_press", keys="Enter", target={"web": "Search actions"})
    state = settled()
    stats = client.call("plate_slice_stats", include=["view"]).get("view") or {}
    count = stats.get("layer_count", 0)
    top = (stats.get("layers") or [0, 0])[1]
    check("a pick of Go to layer at 50 shows half of the layers", count > 0 and abs(top - (round((count - 1) / 2) + 1)) <= 1,
          {"layers": stats.get("layers"), "layer_count": count})
    ran_in_place("Go to layer", "view_set", state, since)

    # ---- Auto-Arrange and Select All Objects: a call is the command's, with its reply; a pick runs in place ----
    client.call("project_new", discard_changes=True, preset_changes="discard")
    client.call("view_set", tab="prepare")
    for _ in CUBES_X:
        client.call("object_select", none=True)
        client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    for o, x in enumerate(CUBES_X):
        client.call("object_transform", object=o, position=[x, 128, 10])
    check("three cubes on the plate", client.call("object_list").get("total") == 3 and outside() == [], outside())

    client.call("replay_export", clear=True)
    cube_off_plate("object_arrange {}")
    arranged = client.await_job(client.call("object_arrange"))
    check("object_arrange {} replies with the settings in force and what it left off the plates",
          "settings" in arranged and arranged.get("off_plate") == [], arranged)
    check("and not with ran", "ran" not in arranged, arranged)
    check("and puts the cube back on the plate", outside() == [], outside())
    step = (client.call("replay_export", clear=True)["replay"]["steps"] or [{}])[-1]
    check("and the journal keeps it with the settings in force", step.get("tool") == "object_arrange"
          and "spacing" in step.get("args", {}), step)

    # action_run names the action it ran in ran, whatever the command replies; the menu item replied level besides.
    cube_off_plate("action_run of Auto-Arrange")
    arranged = client.await_job(client.call("action_run", id="orca_command:obj_arrange:orca"))
    check("action_run of Auto-Arrange replies with the settings in force and what it left off the plates",
          "settings" in arranged and arranged.get("off_plate") == [], arranged)
    check("and not with the menu item's level", "level" not in arranged, arranged)
    check("and with ran, the action's id", arranged.get("ran") == "orca_command:obj_arrange:orca", arranged)
    check("and puts the cube back on the plate", outside() == [], outside())
    # action_run takes the action with its own arguments alone, none here, so the journal keeps those in place of the
    # settings the command records, and the step replays.
    exported = client.call("replay_export", clear=True)["replay"]
    step = (exported["steps"] or [{}])[-1]
    check("and the journal keeps the action's call", step.get("tool") == "action_run"
          and step.get("args") == {"id": "orca_command:obj_arrange:orca", "args": {}}, step)
    exported["steps"] = [s for s in exported["steps"] if s.get("tool") in ("object_transform", "action_run")]
    replayed = client.await_job(client.call("replay_run", replay=exported))
    check("which replays: the cube moved off the plate again and put back", replayed.get("passed") is True and outside() == [],
          {"replay": replayed, "outside": outside()})

    cube_off_plate("a pick of Auto-Arrange")
    since = last_job()
    shared.dial_pick(client, "Auto-Arrange")
    # The pick returns while the arrangement runs.
    state = settled()
    ran_in_place("Auto-Arrange", "object_arrange", state, since)
    check("a pick of Auto-Arrange puts the cube back on the plate", outside() == [], outside())

    client.call("object_select", none=True)
    client.call("replay_export", clear=True)
    chosen = client.call("object_select", all=True)
    check("object_select {all: true} replies with the selection, the three cubes", chosen.get("objects") == [0, 1, 2], chosen)
    check("and not with ran", "ran" not in chosen, chosen)
    # The journal keeps the call as it was made, and a replay of that step selects the three from an empty selection.
    exported = client.call("replay_export", clear=True)["replay"]
    step = (exported["steps"] or [{}])[-1]
    check("and the journal keeps the call as it was made", step.get("tool") == "object_select" and step.get("args") == {"all": True},
          step)
    client.call("object_select", none=True)
    exported["steps"] = [step]
    replayed = client.await_job(client.call("replay_run", replay=exported))
    selected = client.call("app_state").get("selection", {}).get("objects")
    check("which replays: the replay passes and selects the three cubes from an empty selection",
          replayed.get("passed") is True and selected == [0, 1, 2], {"replay": replayed, "selection": selected})
    client.call("object_select", none=True)
    chosen = client.call("action_run", id="orca_command:select_all:orca")
    check("action_run of Select All Objects replies with the selection, the three cubes", chosen.get("objects") == [0, 1, 2],
          chosen)
    check("and not with the menu item's level", "level" not in chosen, chosen)
    check("and with ran, the action's id", chosen.get("ran") == "orca_command:select_all:orca", chosen)

    client.call("view_set", canvas="assemble")
    client.call("object_select", none=True)
    chosen = client.call("object_select", all=True)
    # app_state reads the assemble view's selection only while that view is shown, else Prepare's.
    selection = client.call("app_state").get("selection", {})
    check("in the assemble view object_select {all: true} selects the three there",
          chosen.get("canvas") == "assemble" and chosen.get("objects") == [0, 1, 2], chosen)
    check("and the assemble view stays shown, with the three selected",
          selection.get("canvas") == "assemble" and selection.get("objects") == [0, 1, 2], selection)

    # The assemble view has gizmos of its own; Measure keeps its selection, as Place on face does in Prepare.
    client.call("view_set", canvas="assemble")
    client.call("object_select", objects=[0])
    measure = {"gizmo": "Measure", "canvas": "assemble"}
    opened = client.call("widget_click", target=measure)
    gizmo = client.call("app_state").get("gizmo") or {}
    check("the Measure gizmo opens in the assemble view", gizmo == {"name": "Measure", "canvas": "assemble"}, (opened, gizmo))
    try:
        reply = client.call("object_select", all=True)
        check("with Measure open in the assemble view, object_select {all: true} is refused", False, reply)
    except orca_mcp.McpError as e:
        check("with Measure open in the assemble view, object_select {all: true} is refused as gizmo_open",
              refusal_code(e) == "gizmo_open", str(e))
    selection = client.call("app_state").get("selection", {})
    check("and the assemble view's selection stays", selection.get("canvas") == "assemble" and selection.get("objects") == [0],
          selection)
    if (client.call("app_state").get("gizmo") or {}).get("canvas") == "assemble":
        client.call("view_set", canvas="assemble")
        client.call("widget_click", target=measure)
    client.call("view_set", canvas="3d")

    client.call("object_select", none=True)
    since = last_job()
    shared.dial_pick(client, "Select All Objects")
    state = settled()
    ran_in_place("Select All Objects", "object_select", state, since)
    check("a pick of Select All Objects selects the three cubes", state.get("selection", {}).get("objects") == [0, 1, 2],
          state.get("selection"))

    # Where the pick and the call differ: picked while the assemble view is shown, Select All Objects shows Prepare and
    # selects there, as its menu item did, where the call selects in the assemble view.
    client.call("object_select", none=True)
    client.call("view_set", canvas="assemble")
    client.call("object_select", none=True)
    since = last_job()
    shared.dial_pick(client, "Select All Objects")
    state = settled()
    ran_in_place("Select All Objects in the assemble view", "object_select", state, since)
    check("a pick of Select All Objects in the assemble view shows Prepare and selects the three there",
          state.get("selection", {}).get("canvas") == "3d" and state.get("selection", {}).get("objects") == [0, 1, 2],
          state.get("selection"))
    client.call("view_set", canvas="3d")

    # The control for the absences above: a pick of Slice and Preview, opened and searched the same way on the same
    # project, starts its slice as a job, listed under its command and announced as Started, then Finished.
    since = last_job()
    shared.dial_pick(client, "Slice and Preview")
    state = settled()
    jobs = [j for j in state.get("jobs", []) if int(j["id"][1:]) > since and j.get("tool") == "plate_slice"]
    check("a pick that runs as a job is listed as one", jobs != [], state.get("jobs"))
    check("and shows its Started or Finished notice", job_notices(state) != [], state.get("notifications"))

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
