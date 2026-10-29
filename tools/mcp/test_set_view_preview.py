#!/usr/bin/env python3
"""Checks what set_view does to the Preview of a sliced cube where a script cannot look.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_set_view_preview.py

Picking a view type is the legend combo's pick: a feature type hidden by a click on its legend row
shows again when Line Type is picked, and the pick is written to the app config for the "Last used"
preview default. The refusals: legend rows before a slice, a height above the top layer, layer with
layer_z, an unknown legend row (named with the rows the print has), a value that is not true or
false, and a silent-mode estimate the print does not have; a refused call changes nothing. Exits 0
when every check passes.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def main():
    client, session = orca_mcp.session_client()
    conf = os.path.join(session["datadir"], "OrcaSlicer.conf")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def refusal(**args):
        try:
            client.call("set_view", **args)
            return ""
        except orca_mcp.McpError as e:
            return str(e)

    def legend():
        return client.call("get_preview", include=["view"])["view"]["legend"]

    def last_view_type(expected):
        # The app writes its config at an idle moment, so wait for it.
        value = None
        for _ in range(50):
            try:
                with open(conf, encoding="utf-8") as f:
                    text = f.read()
                # The JSON is followed by a checksum line on Windows.
                value = json.loads(text[:text.rfind("}") + 1])["app"].get("preview_last_view_type")
            except (OSError, ValueError, KeyError):
                pass
            if value == expected:
                break
            time.sleep(0.1)
        return value

    client.call("dialog_auto_answer", rules=[{"match": "Uniform scale", "button": "ok"}])
    client.call("new_project", discard_changes=True)
    check("legend rows need a slice", "slice first" in refusal(legend={"travel": False}))
    client.call("run_action", id="orca_command:add_primitive_cube:orca")
    client.call("wait", **{"for": "idle"})
    sliced = client.await_job(client.call("slice"))["plates"][0]
    client.call("set_view", view_type="feature_type", layer_range=[1, 128])

    # A click on the legend row and get_preview's legend are one state. The snapshot draws the legend
    # first, since a window the desktop hides may not have drawn it since the slice.
    client.call("ui_snapshot", query="Sparse infill")
    client.call("ui_click", target={"imgui_name": "Sparse infill"})
    check("a legend row's click hides its feature type", legend()["features"]["Sparse infill"] is False)
    client.call("set_view", view_type="speed")
    check("the pick is remembered", last_view_type("speed") == "speed", last_view_type("speed"))
    check("another view type keeps it hidden", legend()["features"]["Sparse infill"] is False)
    client.call("set_view", view_type="feature_type")
    check("picking Line Type shows it again", legend()["features"]["Sparse infill"] is True)
    check("Line Type is remembered", last_view_type("feature_type") == "feature_type", last_view_type("feature_type"))

    reply = client.call("set_view", legend={"Sparse infill": False, "travel": True}, layer_z=1.05)
    check("layer_z between two tops shows up to the upper one", reply.get("layers") == [1, 6, 128] and reply.get("z") == [0.2, 1.2], reply)
    check("the legend reply gives each row's state", reply.get("legend") == {"Sparse infill": False, "travel": True}, reply)

    message = refusal(layer_z=80)
    check("a height above the top layer is refused", "above the top layer (25.6 mm)" in message, message)
    check("layer with layer_z is refused", "not both" in refusal(layer=3, layer_z=1.0))
    message = refusal(view_type="speed", legend={"Infill": False})
    check("an unknown row is refused with the print's rows", "Sparse infill" in message and "travel" in message, message)
    check("a refused call leaves the view type", client.call("get_state")["preview"]["view_type"] == "feature_type")
    check("a value that is not true or false is refused", "true (show) or false (hide)" in refusal(legend={"travel": "yes"}))
    if "silent_time_s" not in sliced:
        message = refusal(time_mode="silent")
        check("silent mode without its own estimate is refused", "no separate silent-mode estimate" in message, message)
    check("normal mode is always offered", client.call("set_view", time_mode="normal").get("time_mode") == "normal")

    client.call("dialog_auto_answer", rules=[])
    client.call("new_project", discard_changes=True)
    failures = 0
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
