#!/usr/bin/env python3
"""Checks that a session using ImGui refs whose items changed exports a replay that runs.

    tools/mcp/orca_mcp.py launch --headless
    tools/mcp/test_ref_locator_export.py

Two refs outlive a change to what they were taken by. The Text gizmo's font button shows the font,
so its label changes with every pick: one ref is used for two picks. Texture Displacement names its
collapsed layer rows by place: a ref to "Layer 2" is clicked after the first layer is removed, when
that row is "Layer 1". The click must open the layer the ref was taken for, which leaves the other
layer as the row "Layer 2". The exported replay must find each item by what it is at each step, so
replaying it passes. Exits 0 when every check passes.

A layer row is an item widget_list lists named exactly "Layer N" (its thumbnail is "Layer N image"), N its
place in the list counted from 1; the open layer has no row. object_list's painted.texture gives each
layer's slot.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

FONT = "HarmonyOS Sans SC"  # bundled with OrcaSlicer, so every system lists it
HEIGHTMAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures", "heightmap.png")


def snapshot_ref(client, pattern):
    _, texts = client.call_with_text("widget_list")
    found = re.search(pattern, "\n".join(texts))
    return found.groups() if found else None


def layer_rows(client):
    """The names of the layer rows widget_list lists, "Layer N", leaving out the thumbnails ("Layer N image")."""
    _, texts = client.call_with_text("widget_list")
    return re.findall(r"name=(Layer \d+) (?!image)", "\n".join(texts))


def layer_slots(client):
    return [layer["slot"] for layer in client.call("object_list")["plates"][0]["objects"][0]["parts"][0]["painted"]["texture"]]


def main():
    client, _ = orca_mcp.session_client()
    client.call("dialog_auto_answer", rules=[{"match": "Uniform scale", "button": "ok"},
                                             {"match": "unsaved changes", "button": "no"},
                                             {"match": "text volume is empty", "button": "ok"}])
    client.call("replay_export", clear=True)
    client.call("project_new", discard_changes=True)
    client.call("view_set", tab="prepare")
    client.call("action_run", id="orca_command:add_primitive_cube:orca")
    client.call("app_wait", **{"for": "idle"})
    checks = []

    # One ref to the font button, used for two picks.
    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Text"})
    client.call("app_wait", **{"for": "idle"})
    font = snapshot_ref(client, r'\[(g\d+)\] imgui "([^"]*)"[^\n]*name=Combo_Font')
    checks.append(("the snapshot lists the font button", font is not None, "no font button"))
    if font:
        ref, shown = font
        first = client.call("widget_set_value", target=ref, value=FONT)
        client.call("app_wait", **{"for": "idle"})
        back = client.call("widget_set_value", target=ref, value=shown)
        client.call("app_wait", **{"for": "idle"})
        checks.append(("the ref picks a font", first.get("selected") == FONT, first))
        checks.append(("the same ref picks the first font back", back.get("selected", "").startswith(shown), back))

    # A ref to the second layer's row, clicked once the first layer is gone and the row is "Layer 1"
    # (the third layer, the open one, has no row). Opening the gizmo makes the first layer, slot 0, which the
    # import textures; each layer added takes the first free slot, so the two added are 1 and 2, slot 2 open.
    # The layers are listed by slot, so the Remove of index 0 is slot 0's.
    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Texture Displacement"})
    client.call("gizmo_pick_file", action="texture_import", path=HEIGHTMAP)
    client.call("widget_click", target={"imgui_name": "add_layer"})
    client.call("widget_click", target={"imgui_name": "add_layer"})
    row = snapshot_ref(client, r'\[(g\d+)\][^\n]*name=Layer 2 (?!image)')
    checks.append(("the snapshot lists the second layer's row", row is not None, "no Layer 2 row"))
    if row:
        client.call("widget_click", target={"imgui_name": "Remove", "index": 0})
        slots, rows = layer_slots(client), layer_rows(client)
        checks.append(("the Remove leaves the second layer's row as Layer 1",
                       slots == [1, 2] and "Layer 1" in rows and "Layer 2" not in rows, (slots, rows)))
        # The click opens slot 1, the layer the ref was taken for, and slot 2 becomes the row, "Layer 2". A ref
        # that resolved to nothing, to the open layer or to a Remove button leaves other slots or rows.
        clicked = client.call("widget_click", target=row[0])
        slots, rows = layer_slots(client), layer_rows(client)
        checks.append(("the old ref clicks the renamed row",
                       slots == [1, 2] and "Layer 2" in rows and "Layer 1" not in rows, (clicked, slots, rows)))
    # Its icon closes it: in a small window the object's info notification covers the panel's Close.
    client.call("widget_click", target={"gizmo": "Texture Displacement"})
    client.call("app_wait", **{"for": "idle"})

    out_dir = tempfile.mkdtemp(prefix="orca-mcp-")
    path = os.path.join(out_dir, "session.json")
    client.call("replay_export", path=path, overwrite=True)
    replay = client.await_job(client.call("replay_run", path=path, out_dir=out_dir))
    failed = [s for s in replay.get("steps", []) if not s.get("ok")]
    checks.append(("the exported session replays", replay.get("passed") is True, failed[:1]))

    failures = 0
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
