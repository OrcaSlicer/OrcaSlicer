#!/usr/bin/env python3
"""Checks that the routes to one function accept and refuse the same states, in the same words.
Its design is the test_operation_routes entry of tools/mcp/test-entries/commands.md.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_operation_routes.py

A command, a Speed Dial action that runs it (with action_run) and the plugin API's binding run one
operation of GUI/Operations, so each pair here gets one answer: a layer asked of a Preview that has none
(the Go to layer action and view_set); a preset for a mixed-colour filament slot; and a hidden printer
selected by name through the plugin API, as preset_select selects one. It also checks what a command
refuses or does in those states alone: a project saved under a .gcode.3mf name; an export of a project
with no objects (a .3mf is written, an STL and a sliced file are refused); an arrangement from the
assemble view and with a gizmo open. It starts from the Generic Klipper Printer with one filament slot,
in a new project, which leaves the Preview no layers. The hidden printer is the first one still hidden,
one of that printer's other nozzles where one is left to show: a hidden preset once shown stays shown
until the app restarts, so in an app where an earlier run of this test showed one, it picks another.
Exits 0 when every check passes.
"""

import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import checks as shared  # noqa: E402
import orca_mcp  # noqa: E402

FIXTURE = os.path.join(HERE, "tests", "fixtures", "ops_presets.py")
START = {"printer": shared.KLIPPER, "process": shared.KLIPPER_STD, "filaments": [shared.GENERIC_PLA]}
# The middle of the Generic Klipper Printer's 250 mm bed.
BED_CENTRE = 125


def refusal(error):
    """(code, text) of a failed call: an operation's refusal has its details, the code among them, on
    the line after its text. The code is "" when the error is not an operation's."""
    text, _, tail = str(error).rpartition("\n")
    try:
        return json.loads(tail).get("code", ""), text
    except (ValueError, AttributeError):
        return "", str(error)


def main():
    client, _session = orca_mcp.session_client()
    out = tempfile.mkdtemp(prefix="orca-routes-")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def path(name):
        return os.path.join(out, name)

    def answer(tool, **args):
        """(code, text) of a refused call, or (None, reply) of one that went through."""
        try:
            reply = client.call(tool, **args)
        except orca_mcp.McpError as e:
            return refusal(e)
        # No call here should stop at a dialog; one that does is reported, not waited on.
        if isinstance(reply, dict) and reply.get("status") == "blocked_by_dialog":
            return "blocked_by_dialog", json.dumps(reply.get("dialogs"))
        return None, client.await_job(reply)

    def action(name, **action_args):
        return answer("action_run", id="orca_command:%s:orca" % name, args=action_args)

    def plugin_api(**args):
        code = "import ops_presets\nresult = ops_presets.preset_api(args)"
        return client.call("script_run", filename=FIXTURE, code=code, args=args)["result"]

    def offsets():
        return [o["instances"][0]["offset"][:2] for o in client.call("object_list", detail=True)["plates"][0]["objects"]]

    presets = shared.start(client)
    check("starts from the Generic Klipper Printer", presets == START, presets)

    # ---- A project with no objects ----
    code, text = answer("project_save", path=path("project.gcode.3mf"))
    check("project_save refuses a .gcode.3mf name", code == "sliced_file_name", (code, text))
    check("with the hint after its refusal", "sliced_3mf" in text, text)
    check("and writes no file", not os.path.exists(path("project.gcode.3mf")))

    code, text = answer("project_export", format="3mf", path=path("model.3mf"))
    check("project_export writes a .3mf of a project with no objects", code is None and os.path.exists(path("model.3mf")), text)
    for fmt, ext in (("stl", ".stl"), ("sliced_3mf", ".gcode.3mf")):
        code, text = answer("project_export", format=fmt, path=path("model" + ext))
        check("project_export refuses %s of a project with no objects" % fmt, code == "no_objects", (code, text))
        check("and writes no file (%s)" % fmt, not os.path.exists(path("model" + ext)))

    code_c, text_c = action("go_to_layer", percent=50)
    code_t, text_t = answer("view_set", layer=1)
    check("the Go to layer action refuses a layer of a Preview with none", code_c == "no_layers", (code_c, text_c))
    check("view_set refuses a layer of a Preview with none", code_t == "no_layers", (code_t, text_t))
    check("in the same words (layer)", text_c == text_t, (text_c, text_t))
    client.call("view_set", tab="prepare")

    # ---- Two cubes on one spot ----
    for _ in range(2):
        client.call("object_select", none=True)
        client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    shared.wait(client, "idle")
    before = offsets()
    client.call("view_set", canvas="assemble")
    # object_arrange with no arguments, Auto-Arrange's, replies with what it left off the plates, as any call of it does.
    code, reply = answer("object_arrange")
    state = client.call("app_state")
    check("object_arrange arranges from the assemble view", code is None and reply.get("off_plate") == [], reply)
    check("and the arrangement moved a cube", offsets() != before, (before, offsets()))
    check("and the Prepare view is shown afterwards", state["selection"].get("canvas") == "3d", state["selection"])
    client.call("view_set", canvas="3d")
    # Both cubes on one spot, so that only an arrangement that runs can set them apart.
    for i in range(2):
        client.call("object_transform", object=i, position=[BED_CENTRE, BED_CENTRE, 10])
    together = offsets()
    check("the cubes are back on one spot", together == [[BED_CENTRE, BED_CENTRE]] * 2, together)
    client.call("object_select", objects=[0])
    opened = client.call("widget_click", target={"gizmo": "Move"})
    code, reply = answer("object_arrange")
    state = client.call("app_state")
    after = offsets()
    apart = len(after) == 2 and (abs(after[0][0] - after[1][0]) >= 20 or abs(after[0][1] - after[1][1]) >= 20)
    check("object_arrange arranges with the Move gizmo open", opened.get("active") is True and code is None and apart,
          (opened, reply, after))
    check("and the gizmo stays open", state.get("gizmo") is not None, state.get("gizmo"))
    if state.get("gizmo") is not None:
        client.call("widget_click", target={"gizmo": "Move"})

    # ---- A mixed-colour filament slot, and a hidden printer ----
    client.call("project_new", discard_changes=True, preset_changes="discard")
    shared.add_slot(client, shared.SLOT_COLOURS[1])
    slot_preset = client.call("app_state")["presets"]["filaments"][0]
    blocked = client.call("widget_click", target={"label": "Add Mixed Filament"})
    if blocked.get("status") == "blocked_by_dialog":
        client.call("dialog_answer", button="OK")
        client.await_job(blocked)
    flags = shared.slots(client).split(",")
    check("a mixed filament was added", "1" in flags, flags)
    if "1" in flags:
        slot = flags.index("1") + 1
        code_t, text_t = answer("preset_select", type="filament", name=slot_preset, filament_slot=slot)
        raised = plugin_api(editor="filament", form="begin_update", slot=slot, name=slot_preset, commit=True).get("raised", {})
        check("preset_select refuses a mixed-colour slot as one", code_t == "mixed_filament_slot", (code_t, text_t))
        check("the plugin API refuses it with a ValueError in the same words",
              raised.get("type") == "ValueError" and raised.get("message") == text_t, (raised, text_t))
        client.call("widget_click", target={"label": "Remove last mixed filament"})

    listed = client.call("preset_list", type="printer", include_uninstalled=True, limit=500)["items"]
    hidden = [p["name"] for p in listed if p.get("installed") is False and p.get("compatible", True) is not False]
    # Another nozzle of the selected printer first: its process and filament suit the one selected.
    hidden.sort(key=lambda name: not name.startswith("MyKlipper"))
    check("a printer is still hidden (else restart the app)", bool(hidden), len(listed))
    if hidden:
        got = plugin_api(editor="printer", form="begin_update", name=hidden[0], commit=True)
        shared.wait(client, "idle")
        selected = client.call("app_state")["presets"]["printer"]
        check("the plugin API shows a hidden printer and selects it", "raised" not in got and selected == hidden[0], (got, selected))
        # The data folder remembers the printer selected, which a later start of the app selects before any test's
        # start() runs, and the hidden printer is not installed there.
        client.call("preset_select", type="printer", name=shared.KLIPPER, preset_changes="discard")

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + json.dumps(detail, default=str)[:600]))
    print("%d of %d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
