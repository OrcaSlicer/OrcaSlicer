#!/usr/bin/env python3
"""Checks that the routes to one function accept and refuse the same states, in the same words.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_operation_routes.py

A Speed Dial command (run with run_action), the tool that does the same thing and the plugin API's
binding run one operation of GUI/Operations, so each pair here gets one answer: a project saved under
a .gcode.3mf name; an export of a project with no objects (a .3mf is written, an STL and a sliced file
are refused); a layer asked of a Preview that has none; an arrangement from the assemble view and with
a gizmo open; a preset for a mixed-colour filament slot; and a hidden printer selected by name through
the plugin API, as the select_preset tool selects one. The last two need the Bambu Lab X1 Carbon among
the printers, which the harness's datadir has. Run it on a freshly launched app: the Preview keeps the
layers of the last slice, and a hidden preset stays shown until the app restarts. Exits 0 when every
check passes.
"""

import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orca_mcp  # noqa: E402

FIXTURE = os.path.join(HERE, "tests", "fixtures", "ops_presets.py")
PRINTER = "Bambu Lab X1 Carbon 0.4 nozzle"


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

    def command(name, param=None):
        args = {"id": "orca_command:%s:orca" % name}
        if param is not None:
            args["param"] = param
        return answer("run_action", **args)

    def plugin_api(**args):
        code = "import ops_presets\nresult = ops_presets.preset_api(args)"
        return client.call("run_script", filename=FIXTURE, code=code, args=args)["result"]

    def offsets():
        return [o["instances"][0]["offset"][:2] for o in client.call("list_objects")["plates"][0]["objects"]]

    client.call("new_project", discard_changes=True, preset_changes="discard")
    client.call("set_view", tab="prepare")

    # ---- A project with no objects ----
    code_c, text_c = command("save_project_as", path("cmd.gcode.3mf"))
    code_t, text_t = answer("save_project", path=path("tool.gcode.3mf"))
    check("the command refuses a .gcode.3mf name", code_c == "sliced_file_name", (code_c, text_c))
    check("the tool refuses a .gcode.3mf name", code_t == "sliced_file_name", (code_t, text_t))
    check("the tool's refusal is the command's, with its hint after it",
          code_c == code_t == "sliced_file_name" and text_t.startswith(text_c) and "sliced_3mf" in text_t, (text_c, text_t))
    check("neither wrote a file", not os.path.exists(path("cmd.gcode.3mf")) and not os.path.exists(path("tool.gcode.3mf")))

    code_c, text_c = command("export_3mf", path("cmd.3mf"))
    code_t, text_t = answer("export", format="3mf", path=path("tool.3mf"))
    check("the command writes a .3mf of a project with no objects", code_c is None and os.path.exists(path("cmd.3mf")), text_c)
    check("the tool writes a .3mf of a project with no objects", code_t is None and os.path.exists(path("tool.3mf")), text_t)
    for fmt, name, ext in (("stl", "export_stl", ".stl"), ("sliced_3mf", "export_sliced_file", "_sliced.gcode.3mf")):
        code_c, text_c = command(name, path("cmd" + ext))
        code_t, text_t = answer("export", format=fmt, path=path("tool" + ext))
        check("the command refuses %s of a project with no objects" % fmt, code_c == "no_objects", (code_c, text_c))
        check("the tool refuses %s of a project with no objects" % fmt, code_t == "no_objects", (code_t, text_t))
        check("in the same words (%s)" % fmt, text_c == text_t, (text_c, text_t))
        check("neither wrote a file (%s)" % fmt, not os.path.exists(path("cmd" + ext)) and not os.path.exists(path("tool" + ext)))

    code_c, text_c = command("go_to_layer", "50")
    code_t, text_t = answer("set_view", layer=1)
    check("the command refuses a layer of a Preview with none", code_c == "no_layers", (code_c, text_c))
    check("the tool refuses a layer of a Preview with none", code_t == "no_layers", (code_t, text_t))
    check("in the same words (layer)", text_c == text_t, (text_c, text_t))
    client.call("set_view", tab="prepare")

    # ---- Two cubes on one spot ----
    for _ in range(2):
        client.call("select_objects", none=True)
        client.call("run_action", id="orca_command:add_primitive_cube:orca", param="20 20 20")
    client.call("wait", **{"for": "idle"})
    before = offsets()
    client.call("set_view", canvas="assemble")
    code, reply = answer("arrange")
    state = client.call("get_app_state")
    check("the tool arranges from the assemble view", code is None and reply.get("off_plate") == [], reply)
    check("and the arrangement moved a cube", offsets() != before, (before, offsets()))
    check("and the Prepare view is shown afterwards", state["selection"].get("canvas") == "3d", state["selection"])
    client.call("set_view", canvas="3d")
    client.call("select_objects", objects=[0])
    opened = client.call("click", target={"gizmo": "Move"})
    code, reply = answer("arrange")
    state = client.call("get_app_state")
    check("the tool arranges with the Move gizmo open", opened.get("active") is True and code is None, (opened, reply))
    check("and the gizmo stays open", state.get("gizmo") is not None, state.get("gizmo"))
    if state.get("gizmo") is not None:
        client.call("click", target={"gizmo": "Move"})

    # ---- A mixed-colour filament slot, and a hidden printer ----
    client.call("new_project", discard_changes=True, preset_changes="discard")
    client.call("select_preset", type="printer", name=PRINTER, preset_changes="discard")
    while len(client.call("get_app_state")["presets"]["filaments"]) < 2:
        client.call("run_action", id="orca_command:filament_add:orca")
    slot_preset = client.call("get_app_state")["presets"]["filaments"][0]
    blocked = client.call("click", target={"label": "Add Mixed Filament"})
    if blocked.get("status") == "blocked_by_dialog":
        client.call("answer_dialog", button="OK")
        client.await_job(blocked)
    flags = client.call("get_settings", scope="project", keys=["filament_is_mixed"])["items"][0]["value"].split(",")
    check("a mixed filament was added", "1" in flags, flags)
    if "1" in flags:
        slot = flags.index("1") + 1
        code_t, text_t = answer("select_preset", type="filament", name=slot_preset, filament_slot=slot)
        raised = plugin_api(editor="filament", form="begin_update", slot=slot, name=slot_preset, commit=True).get("raised", {})
        check("the tool refuses a mixed-colour slot as one", code_t == "mixed_filament_slot", (code_t, text_t))
        check("the plugin API refuses it with a ValueError in the same words",
              raised.get("type") == "ValueError" and raised.get("message") == text_t, (raised, text_t))
        client.call("click", target={"label": "Remove last mixed filament"})

    listed = client.call("list_presets", type="printer", include_hidden=True, limit=500)["items"]
    hidden = [p["name"] for p in listed if p.get("visible") is False and p.get("compatible", True) is not False]
    # A printer of the same family first: its process and filament suit the one selected.
    hidden.sort(key=lambda name: not name.startswith("Bambu Lab X1"))
    check("a printer is still hidden (else restart the app)", bool(hidden), len(listed))
    if hidden:
        got = plugin_api(editor="printer", form="begin_update", name=hidden[0], commit=True)
        client.call("wait", **{"for": "idle"})
        selected = client.call("get_app_state")["presets"]["printer"]
        check("the plugin API shows a hidden printer and selects it", "raised" not in got and selected == hidden[0], (got, selected))
        client.call("select_preset", type="printer", name=PRINTER, preset_changes="discard")

    while len(client.call("get_app_state")["presets"]["filaments"]) > 1:
        client.call("run_action", id="orca_command:filament_delete:orca")
    client.call("new_project", discard_changes=True, preset_changes="discard")

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + json.dumps(detail, default=str)[:600]))
    print("%d of %d checks passed" % (len(checks) - len(failed), len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
