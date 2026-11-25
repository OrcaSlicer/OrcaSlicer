#!/usr/bin/env python3
"""Checks that the filament, preset, export and import commands refuse with an error where they cannot run.
Its design is the test_additions_refusals entry of tools/mcp/test-entries/commands.md.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_additions_refusals.py

preset_sync while signed out is refused, and so is filament_sync_ams, as its Speed Dial action and with each mode, with
no printer connected. view_set of a layer on a plate with no slice is refused, and leaves the legend's fold and the
G-code window, given beside it, as they were. project_export of each object as its own STL, as its Speed Dial action
and with a folder, is refused for a project with no objects, and of the toolpaths as an OBJ while the Preview shows
none; a pick of either action from the Speed Dial (Space on the 3D view, the search field, a click on its row) shows
the refusal as a notice and opens no dialog. project_import of an entry a ZIP archive lacks is refused with the names
the archive has, and imports nothing (the archive is written by tests/fixtures/filaments_and_files.py, in the
configuration folder).

On a cube with a modifier and two height ranges, settings_set of layer_height on the modifier, and settings_get of it,
are refused naming where it is set (an object or a height range); settings_set of a null layer_height on a height range
is refused, the range always having one; settings_get of a height range the cube lacks is refused with the ranges it
has; and settings_get of the filament settings with target object is refused, a target's settings being process
settings. None of them changes the settings. A height range's own settings show on its row of the object list (the
row's note in widget_list says "has settings", as the icon that resets them shows), set by settings_set and gone when
a null leaves the range none, and the list's selection stays as it was. With a G-code file of no settings open, the
sidebar is off, and view_set of it is refused.

filament_delete of the only filament slot is refused. With a mixed filament of slots 1 and 2, a merge of slot 1 into
the mixed filament is refused where break_mix answers Merge with's question no ahead; and a merge of the mixed slot is
refused, a mixed filament being deleted but not merged. filament_add once
the project holds as many filament slots as it can is refused, and a pick of Add Filament Slot then says so, as its menu
item did. The mixed filament is left, as a person would keep it.

object_order with an order that leaves out an object of the plate is refused, naming it, and leaves the order as it was.

A refusal is an error a replay cannot expect, which is why this is a script. Exits 0 when every check passes.
"""

import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks as shared  # noqa: E402
import orca_mcp  # noqa: E402

START = {"printer": shared.KLIPPER, "process": shared.KLIPPER_STD, "filaments": [shared.GENERIC_PLA]}
FILES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures", "filaments_and_files.py")
# A G-code file with no settings in it, which the app shows without its sidebar.
GCODE = """G90
M82
G28
G1 Z0.2 F3000
G1 X50 Y50 F6000
G1 X100 Y50 E5 F1200
G1 X100 Y100 E10
G1 X50 Y100 E15
"""
CUBE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models",
                                     "OrcaCube_v2.drc"))
# The most filament slots a project holds (MAXIMUM_EXTRUDER_NUMBER, libslic3r.h).
MOST_SLOTS = 64


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def refused(name, tool, args, expect, code=None):
        """Calls `tool` and checks that it fails with an error whose text holds `expect` and, when `code` is given,
        whose details carry that refusal code; returns the error's details."""
        try:
            reply = client.call(tool, **args)
            check(name, False, reply)
            return {}
        except orca_mcp.McpError as e:
            check(name, expect in str(e), str(e))
            # A refusal's details, its code among them, are on the line after its text.
            try:
                details = json.loads(str(e).rpartition("\n")[2])
            except ValueError:
                details = {}
            if code is not None:
                check(name + f", as {code}", isinstance(details, dict) and details.get("code") == code, str(e))
            return details if isinstance(details, dict) else {}

    def slots():
        """How many filament slots the project has: app_state names each slot's preset."""
        return len(client.call("app_state")["presets"]["filaments"])

    def picked(title):
        """The notices and dialogs once a pick of `title` has run."""
        shared.dial_pick(client, title)
        shared.wait(client, "idle")
        state = client.call("app_state")
        return [n.get("text", "") for n in state.get("notifications", [])], state.get("dialogs", [])

    shared.wait(client, "ready", 50)
    presets = shared.start(client)
    check("starts from the Generic Klipper Printer", presets == START, presets)

    # ---- Syncing presets needs an account ----
    refused("preset_sync while signed out is refused", "preset_sync", {}, "Sign in", "not_signed_in")

    # ---- Syncing the filaments from the AMS needs a printer ----
    refused("filament_sync_ams with no printer connected is refused", "filament_sync_ams", {}, "Connect a printer", "no_printer")
    for mode in ("map", "overwrite"):
        refused(f"filament_sync_ams {{mode: {mode}}} with no printer connected is refused", "filament_sync_ams", {"mode": mode},
                "Connect a printer", "no_printer")

    # ---- A view_set refused for its layer leaves the legend's fold and the G-code window ----
    view = client.call("app_state")["view"]
    refused("view_set of a layer on a plate with no slice is refused", "view_set",
            {"gcode_window": not view["gcode_window"], "legend_collapsed": not view["legend_collapsed"], "layer": 3},
            "Slice the plate first", "no_layers")
    after = client.call("app_state")["view"]
    check("and leaves the G-code window and the legend's fold as they were",
          (after["gcode_window"], after["legend_collapsed"]) == (view["gcode_window"], view["legend_collapsed"]), (view, after))
    client.call("view_set", tab="prepare")

    # ---- Exports of a project with no objects, and of toolpaths the Preview does not show ----
    check("the project has no objects", client.call("object_list").get("total") == 0)
    refused("project_export of each object as an STL, as its action, is refused for a project with no objects", "project_export",
            {"format": "stl", "per_object": True}, "no objects", "no_objects")
    folder = tempfile.mkdtemp(prefix="per_object_")
    refused("project_export of each object as an STL into a folder is refused for a project with no objects", "project_export",
            {"format": "stl", "per_object": True, "path": folder}, "no objects", "no_objects")
    check("and writes no file", os.listdir(folder) == [], os.listdir(folder))
    refused("project_export of the toolpaths as an OBJ is refused while the Preview shows none", "project_export",
            {"format": "toolpaths_obj"}, "Preview", "no_toolpaths")
    notices, dialogs = picked("Export All Objects as STLs")
    check("a pick of Export All Objects as STLs on a project with no objects says it has none",
          "The project has no objects." in notices, notices)
    check("and opens no dialog", dialogs == [], dialogs)
    notices, dialogs = picked("Export Toolpaths as OBJ")
    check("a pick of Export Toolpaths as OBJ with no toolpaths in the Preview says so",
          any("The Preview shows no toolpaths" in text for text in notices), notices)
    check("and opens no dialog", dialogs == [], dialogs)

    # ---- An entry a ZIP archive lacks ----
    archive = client.call("script_run", filename=FILES, code="import filaments_and_files\nresult = filaments_and_files.write_zip(args)",
                          args={"name": "refusals.zip", "cubes": {"cube.stl": 10, "parts/block.stl": 15}})["result"]["path"]
    details = refused("project_import of an entry the ZIP archive lacks is refused", "project_import",
                      {"paths": [archive], "entries": ["cube.stl", "missing.stl"]}, "no model file missing.stl", "no_such_entry")
    check("with the model files the archive has", details.get("entries") == ["cube.stl", "parts/block.stl"], details)
    check("and imports nothing", client.call("object_list").get("total") == 0)

    # ---- Settings on a target that does not take them, a height range the object lacks, a type with a target ----
    client.call("project_import", paths=[CUBE])
    client.call("object_transform", object=0, size=[20, 20, 20])
    client.call("part_add", object=0, type="modifier", shape="cube", size=[5, 5, 5])
    client.call("object_set_height_ranges", object=0, ranges=[{"min_z": 0, "max_z": 5}, {"min_z": 5, "max_z": 10}])

    def overrides(**target):
        return {item["key"]: item["value"] for item in client.call("settings_get", **target)["items"]}

    def range_row(heights):
        """The object list's row of object 0's height range `heights` ("0-5"), as widget_list's tree lists it."""
        _, texts = client.call_with_text("widget_list", target="main", query="row", include_menus=False, include_web=False)
        rows = [line for text in texts for line in text.splitlines() if f"object 0 layer range {heights}" in line]
        return rows[0] if rows else ""

    def selected_rows():
        """The object list's selected rows, without the refs, which change at every listing."""
        _, texts = client.call_with_text("widget_list", target="main", query="row", include_menus=False, include_web=False)
        return [re.sub(r"^\[r\d+\] ", "", line.strip()) for text in texts for line in text.splitlines() if "selected]" in line]

    modifier = {"target": "part", "object": 0, "part": 1}
    lower = {"target": "height_range", "object": 0, "min_z": 0, "max_z": 5}
    before = overrides(**modifier)
    refused("settings_set of layer_height on a part is refused, naming the object and the height range", "settings_set",
            {**modifier, "values": {"layer_height": 0.1}}, "layer_height is set on an object or a height range, not on a part")
    check("and leaves the part's settings", overrides(**modifier) == before, overrides(**modifier))
    refused("settings_get of layer_height on a part is refused, naming where it is set", "settings_get",
            {**modifier, "keys": ["layer_height"]}, "layer_height is set on an object or a height range, not on a part")
    check("and leaves the part's settings too", overrides(**modifier) == before, overrides(**modifier))
    range_before = overrides(**lower)
    refused("settings_set of a null layer_height on a height range is refused", "settings_set",
            {**lower, "values": {"layer_height": None}}, "A height range keeps its layer_height")
    check("and leaves the range's settings", overrides(**lower) == range_before, (range_before, overrides(**lower)))

    # A height range's own settings show on its row, as a change in its panel shows them.
    shown = selected_rows()
    check("height range 0-5 has no settings of its own on its row", "has settings" not in range_row("0-5"), range_row("0-5"))
    client.call("settings_set", **lower, values={"wall_loops": 6})
    check("settings_set of wall_loops on it shows the row with settings", "has settings" in range_row("0-5"), range_row("0-5"))
    client.call("settings_set", **lower, values={"wall_loops": None})
    check("and a null that leaves the range none takes them away", "has settings" not in range_row("0-5"), range_row("0-5"))
    check("the object list's selection stays as it was", selected_rows() == shown, (shown, selected_rows()))
    details = refused("settings_get of a height range the object lacks is refused", "settings_get",
                      {"target": "height_range", "object": 0, "min_z": 0, "max_z": 7}, "object 0 has no height range 0-7; it has 0-5, 5-10")
    check("with the ranges it has", details.get("height_ranges") == [{"min_z": 0, "max_z": 5}, {"min_z": 5, "max_z": 10}], details)
    refused("settings_get of the filament settings with target object is refused", "settings_get",
            {"type": "filament", "target": "object", "object": 0}, "The filament settings have no target object")
    check("and the object's ranges stay", len(client.call("object_list", detail=True)["plates"][0]["objects"][0]["layer_config_ranges"]) == 2)
    client.call("project_new", discard_changes=True)

    # ---- The sidebar is off while a G-code file without its settings is shown ----
    gcode = os.path.join(tempfile.mkdtemp(prefix="no_settings_"), "lines.gcode")
    with open(gcode, "w", encoding="utf-8") as f:
        f.write(GCODE)
    client.call("project_open", path=gcode)
    view = client.call("app_state")["view"]
    refused("view_set of the sidebar while a G-code file without its settings is shown is refused", "view_set",
            {"sidebar_collapsed": not view["sidebar_collapsed"]}, "The sidebar is off while a G-code file is shown")
    check("and leaves the sidebar as it was", client.call("app_state")["view"]["sidebar_collapsed"] == view["sidebar_collapsed"])
    client.call("project_new", discard_changes=True)
    client.call("view_set", tab="prepare")

    # ---- Filament slots: the only one is not deleted, a merge answered no is not made, and a mixed one is not merged ----
    check("the project has one filament slot", slots() == 1, slots())
    refused("filament_delete of the only filament slot is refused", "filament_delete", {}, "only filament slot",
            "last_filament_slot")
    check("and deletes no slot", slots() == 1, slots())
    shared.add_slot(client, shared.SLOT_COLOURS[1])
    mixed = client.call("filament_add_mixed", components=[1, 2], ratios=[50, 50])
    check("a mixed filament of slots 1 and 2 is slot 3", mixed.get("filament_slot") == 3, mixed)
    refused("filament_delete of slot 1 merged into the mixed filament that mixes it, answered no, is refused",
            "filament_delete", {"filament_slot": 1, "merge_into": 3, "break_mix": False},
            "removes a filament that mixed filament mixes", "mixed_uses_slot")
    refused("filament_delete of the mixed filament merged into slot 1 is refused", "filament_delete",
            {"filament_slot": 3, "merge_into": 1}, "can be deleted but not merged", "mixed_filament_slot")
    check("and the two leave the three slots", slots() == 3, slots())

    # ---- No slot is added past the most a project holds ----
    # filament_add is what this part tests, the sidebar's Add up to the limit; the slots' colours play no part.
    for _ in range(MOST_SLOTS - slots()):
        client.call("filament_add")
    check(f"{MOST_SLOTS} filament slots", slots() == MOST_SLOTS, slots())
    refused("filament_add with as many filament slots as a project holds is refused", "filament_add", {}, "No more filament slots",
            "too_many_filaments")
    check("and adds no slot", slots() == MOST_SLOTS, slots())
    notices, dialogs = picked("Add Filament Slot")
    check("a pick of Add Filament Slot then says no more can be added", "No more filament slots can be added." in notices, notices)
    check("and adds no slot either", slots() == MOST_SLOTS, slots())

    # ---- An order of objects names each of the plate's objects once ----
    client.call("project_new", discard_changes=True)
    client.call("view_set", tab="prepare")
    # Named apart, so that an order that changed shows in the names.
    for name in ("first", "second"):
        client.call("object_add_primitive", shape="cube", size=[20, 20, 20], name=name)
        client.call("object_select", none=True)
    names = lambda: [o["name"] for o in client.call("object_list")["plates"][0]["objects"]]
    check("two cubes, first and second", names() == ["first", "second"], names())
    refused("object_order with an order that leaves out an object of the plate is refused, naming it", "object_order",
            {"plate": 0, "order": [1]}, "leaves out objects of plate 0: 0", "order_misses_object")
    check("and leaves the order as it was", names() == ["first", "second"], names())
    client.call("object_order", plate=0, order=[1, 0])
    check("a full order is taken", names() == ["second", "first"], names())

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
