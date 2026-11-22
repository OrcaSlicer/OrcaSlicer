#!/usr/bin/env python3
"""Checks what the object commands made from menu items refuse, and that each refusal says its reason.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_menu_items_refusals.py

Set as Individual Objects on an object of one instance is refused, saying it has one instance. Select All (object_select
{all: true}) selects every object while the Move gizmo is open, which lets it, and is refused while the Place on face
gizmo is open, which keeps the selection. object_delete with no target and nothing selected is refused as
nothing_selected and deletes nothing; object_rename with two objects selected is refused as one_object_only; and
object_delete of an object the project lacks names its index. object_select with an empty list leaves the selection,
and object_delete by index leaves the view shown (the assemble view).

A selection of a kind a command does not take: object_rename with an instance selected is refused as wrong_selection,
and a pick of Rename from the Speed Dial (Space on the 3D view, the search field, a click on its row) says to select
one object or part, as its menu item did, and opens no dialog. A pick of Set as Individual Objects with a part selected
separates that part's instance alone. A pick of Delete with an object's last solid part selected shows the object
list's message, as the menu item did, where object_delete of that part by index is refused as last_solid_part; object_delete
with no arguments on both solid parts of an object with a modifier deletes the second, shows that message for the first,
and replies with the part deleted and, under kept, the part that stayed. A cube cut
with the Cut gizmo: object_delete of a half by index is refused as cut_object; with no arguments it asks the object
list's question (blocked_by_dialog), and cancelled, the call is told nothing was deleted; a pick of Delete asks it too,
and cancelled shows no notice.

Split: object_split of a mesh of one shell is refused as one_shell, and a pick of Split to Objects on it says it is one
piece; a split into objects that would leave a piece floating above the bed is refused with the Auto-Drop question
(auto_drop_question) unless keep_z answers it, while a pick of Split to Objects asks it. A pick of Change Type with an
object selected, and of Assemble with one object selected, say what to select, in their menu items' words.

Placement: object_mirror with no axis is refused by its schema, and object_center and object_add_primitive on a plate the
project lacks as no_such_plate. A pick of Mirror X with two objects selected says Mirror takes one instance, a pick of Drop to Bed on an
object on the bed says it already rests there, and a pick of Cube with an object selected says to deselect it, as Add
Primitive does, while object_add_primitive with a size adds the object all the same.

The object list's rows, where they and the 3D selection disagree, picked as the rows read them: Assemble on the two
instance rows of an object assembles the two, and with another object's row all three; on every part row of an object it
says to select two or more objects. Change Type to Part on an object's part and modifier rows makes the modifier a part,
and Change Type to Negative Part on every solid part row says the last solid part keeps its type. Split to Objects on two
instance rows, and Split to Parts on every part row, say they take one row. object_auto_orient of a plate with no
objects is refused as no_objects_to_orient and turns nothing, and its reply leaves out an object on a locked plate.
Named objects, or a plate, of which nothing is printable (an Assembly of a cube made not printable) are refused as
not_printable and turn no other object, while a pick of Auto-Orient on that object turns the other object, as its menu item's
job did, and names it.

The plates: a pick of Duplicate Plate is one undo step, which Undo takes back alone. The Rename Plate action run with
its name dialog cancelled is told the plate was not renamed (not_renamed), while a pick of it, cancelled, shows
nothing, as its menu item did. plate_delete, plate_duplicate, plate_rename, plate_lock and plate_select of a plate the
project lacks are refused as no_such_plate, each leaving the plates as they were; a rename of a plate to the name it
has changes neither the current plate nor the undo history.

A refusal is an error a replay cannot expect, which is why this is a script. Exits 0 when every check passes.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks as shared  # noqa: E402
import orca_mcp  # noqa: E402

START = {"printer": shared.KLIPPER, "process": shared.KLIPPER_STD, "filaments": [shared.GENERIC_PLA]}
UNDO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures", "ops_undo.py")


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def refused(name, tool, args, expect, code=None):
        """Calls `tool` and checks that it fails with an error whose text holds `expect` and, when `code` is given,
        whose details carry that refusal code; returns the error's text."""
        try:
            reply = client.call(tool, **args)
            check(name, False, reply)
            return ""
        except orca_mcp.McpError as e:
            check(name, expect in str(e), str(e))
            if code is not None:
                # A refusal's details, its code among them, are on the line after its text.
                try:
                    given = json.loads(str(e).rpartition("\n")[2]).get("code")
                except (ValueError, AttributeError):
                    given = None
                check(name + f", as {code}", given == code, str(e))
            return str(e)

    def total():
        return client.call("object_list").get("total")

    def selected():
        return client.call("app_state").get("selection", {}).get("objects")

    def gizmo():
        return (client.call("app_state").get("gizmo") or {}).get("name")

    def settled():
        shared.wait(client, "idle")
        return client.call("app_state")

    def notices(state):
        return [n.get("text", "") for n in state.get("notifications", [])]

    def rule_uses(match):
        """How many dialogs the auto-answer rule `match` answered."""
        for rule in client.call("app_state").get("auto_answer_rules", []):
            if rule.get("match") == match:
                return rule.get("uses", 0)
        return None

    def uses_after(match, before):
        """The uses of the auto-answer rule `match` once they differ from `before`, waiting up to 30 s; `before` when
        they never change. The object list posts its messages (show_error() opens them from CallAfter), so one can open
        after the call or the pick has replied and the app has read idle."""
        try:
            return shared.until(lambda: rule_uses(match) != before and rule_uses(match), f"the rule {match!r} answers")
        except TimeoutError:
            return rule_uses(match)

    def close_notice(words):
        """Closes the notice whose text holds `words`, as a click on its close button does, so that a later pick that
        shows the same text is seen to show it: the same text pushed again only moves the open notice to the end of the
        list (NotificationManager::activate_existing()). Then checks that app_state.notifications holds no notice with
        that text, waiting up to 30 s.

        widget_list lists each notification as the ImGui window "!!Ntfctn<id>" with its lines as text nodes. Its close
        glyph and the large invisible button over it, " ", have labels the listing cleans to nothing and no ## name, so
        neither has a ref; the locator {imgui: "", window: ...} matches the window's unlabelled items in the order they
        are recorded, and render_close_button() submits the invisible button last. A notice that has already faded (a
        regular notice fades about 10 s after it shows) leaves nothing to close."""
        _, texts = client.call_with_text("widget_list", include_menus=False, include_web=False)
        lines, window = {}, None
        for line in "\n".join(texts).splitlines():
            header = re.match(r'\s*\[-\] imgui_window "(!!Ntfctn\d+)"', line)
            text = re.match(r'\s*\[-\] text "(.*)" @', line)
            if header:
                window = header.group(1)
                lines[window] = []
            elif text and window is not None:
                lines[window].append(text.group(1))
            else:
                window = None
        clicked = []
        for window in [w for w, held in lines.items() if words in " ".join(held)]:
            locator = {"imgui": "", "window": window}
            try:
                client.call("widget_click", target=locator)
                clicked.append((window, "the one unlabelled item"))
            except orca_mcp.McpError as e:
                count = re.match(r"(\d+) ImGui items match", str(e))
                try:
                    if count is None:
                        raise e
                    client.call("widget_click", target=dict(locator, index=int(count.group(1)) - 1))
                    clicked.append((window, f"item {count.group(1)} of {count.group(1)}"))
                except orca_mcp.McpError as failed:
                    clicked.append((window, str(failed)))
        try:
            shared.until(lambda: not any(words in text for text in notices(client.call("app_state"))),
                         f"no notice holds {words!r}")
            check("the earlier notice is closed", True)
        except TimeoutError:
            check("the earlier notice is closed", False, (clicked, notices(client.call("app_state"))))

    shared.wait(client, "ready", 50)
    presets = shared.start(client)
    check("starts from the Generic Klipper Printer", presets == START, presets)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    client.call("object_select", none=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    client.call("object_transform", object=0, position=[90, 100, 10])
    client.call("object_transform", object=1, position=[140, 100, 10])
    check("two cubes", client.call("object_list").get("total") == 2)

    # ---- Set as Individual Objects says why it does nothing ----
    client.call("object_select", objects=[0])
    refused("Set as Individual Objects on an object of one instance says it has one instance", "action_run",
            {"id": "orca_command:obj_instances_to_objects:orca"}, "has one instance")
    check("and leaves two objects", client.call("object_list").get("total") == 2)

    # ---- Select All with a gizmo open ----
    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Move"})
    check("the Move gizmo is open", gizmo() == "Move", gizmo())
    client.call("object_select", all=True)
    check("Select All with the Move gizmo open selects every object", selected() == [0, 1], selected())
    client.call("widget_click", target={"gizmo": "Move"})

    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Flatten"})
    check("the Place on face gizmo is open", gizmo() == "Flatten", gizmo())
    refused("Select All with the Place on face gizmo open is refused", "object_select", {"all": True}, "gizmo", "gizmo_open")
    check("and the selection stays", selected() == [0], selected())
    client.call("widget_click", target={"gizmo": "Flatten"})

    # ---- Targets: none and nothing selected, two for a command of one, an index the project lacks ----
    client.call("object_select", none=True)
    refused("object_delete with no target and nothing selected is refused", "object_delete", {}, "Nothing is selected",
            "nothing_selected")
    check("and deletes nothing", total() == 2, total())
    client.call("object_select", objects=[0, 1])
    refused("object_rename with two objects selected is refused", "object_rename", {"name": "x"}, "one object or part",
            "one_object_only")
    names = [o["name"] for o in client.call("object_list")["plates"][0]["objects"]]
    check("and renames neither", "x" not in names, names)
    refused("object_delete of an object the project lacks names its index", "object_delete", {"objects": [9]}, "object index 9")
    check("and deletes nothing", total() == 2, total())

    # ---- object_select with an empty list leaves the selection; a call that names its targets keeps the tab ----
    client.call("object_select", objects=[0])
    try:
        client.call("object_select", objects=[])
        check("object_select with an empty list leaves the selection", selected() == [0], selected())
    except orca_mcp.McpError as e:
        check("object_select with an empty list leaves the selection", False, str(e))
    client.call("view_set", canvas="assemble")
    client.call("object_delete", objects=[1])
    shown = client.call("app_state").get("selection", {}).get("canvas")
    check("object_delete by index leaves the assemble view shown", shown == "assemble", shown)
    client.call("view_set", canvas="3d")

    # ---- A selection of a kind Rename does not take: an instance, which a pick of Rename refuses as its menu item did ----
    client.call("object_set_instances", object=0, count=2)
    client.call("object_select", instances=[[0, 1]])
    refused("object_rename with an instance selected is refused", "object_rename", {"name": "x"}, "Select one object or part first",
            "wrong_selection")
    client.call("dialog_auto_answer", rules=[{"match": "Renaming", "button": "cancel"}])
    before = rule_uses("Renaming")
    shared.dial_pick(client, "Rename")
    state = settled()
    check("a pick of Rename with an instance selected says to select one object or part",
          any("Select one object or part first" in text for text in notices(state)), notices(state))
    check("and opens no Rename dialog", rule_uses("Renaming") == before, f"{before} -> {rule_uses('Renaming')}")
    check("and leaves the instance selected", state.get("selection", {}).get("instances") == [[0, 1]], state.get("selection"))
    names = [o["name"] for o in client.call("object_list", detail=True)["plates"][0]["objects"]]
    check("and renames nothing", "x" not in names, names)

    # ---- A pick of Set as Individual Objects with a part selected separates that part's instance alone ----
    client.call("project_new", discard_changes=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    client.call("part_add", object=0, type="part", shape="cube", size=[5, 5, 5])
    client.call("object_set_instances", object=0, count=3)
    offsets = [i["offset"] for i in client.call("object_list", detail=True)["plates"][0]["objects"][0]["instances"]]
    client.call("object_select", parts=[[0, 1]])
    shared.dial_pick(client, "Set as Individual Objects")
    settled()
    objects = client.call("object_list", detail=True)["plates"][0]["objects"]
    check("a pick of Set as Individual Objects with a part selected makes one object", len(objects) == 2, len(objects))
    check("and the object keeps its other two instances", len(objects[0]["instances"]) == 2, len(objects[0]["instances"]))
    check("and the new object is the part's instance, instance 0",
          len(objects) == 2 and objects[1]["instances"][0]["offset"] == offsets[0], (offsets, objects[-1]["instances"]))

    # ---- A pick of Delete with an object's last solid part selected: the object list says why, as for the menu item ----
    client.call("project_new", discard_changes=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    client.call("part_add", object=0, type="modifier", shape="cube", size=[5, 5, 5])
    refused("object_delete of an object's last solid part alone is refused", "object_delete", {"parts": [[0, 0]]},
            "last solid part", "last_solid_part")
    client.call("object_select", parts=[[0, 0]])
    client.call("dialog_auto_answer", rules=[{"match": "last solid part", "button": "ok"}])
    before = rule_uses("last solid part")
    shared.dial_pick(client, "Delete Selected")
    state = settled()
    uses = uses_after("last solid part", before)
    check("a pick of Delete with the last solid part selected shows the object list's message", uses == before + 1,
          f"{before} -> {uses}")
    check("and no notice of its own", not any("solid part" in text for text in notices(state)), notices(state))
    check("and deletes nothing", len(client.call("object_list", detail=True)["plates"][0]["objects"][0]["parts"]) == 2)

    # ---- object_delete {} on both solid parts of an object with a modifier: the object list deletes the second and
    # refuses the first, the last solid part, in its message; the reply names the part that went and the one that stayed ----
    client.call("project_new", discard_changes=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    x, y, _ = client.call("object_list", detail=True)["plates"][0]["objects"][0]["instances"][0]["offset"]
    client.call("part_add", object=0, type="part", shape="cube", size=[5, 5, 5], position=[x + 12, y, 2.5])
    client.call("part_add", object=0, type="modifier", shape="cube", size=[5, 5, 5])
    parts = client.call("object_list", detail=True)["plates"][0]["objects"][0]["parts"]
    check("an object of two solid parts and a modifier", [p["type"] for p in parts] == ["part", "part", "modifier"], parts)
    client.call("object_select", parts=[[0, 0], [0, 1]])
    client.call("dialog_auto_answer", rules=[{"match": "last solid part", "button": "ok"}])
    before = rule_uses("last solid part")
    reply = client.call("object_delete")
    if reply.get("status") in ("running", "blocked_by_dialog"):
        reply = client.await_job(reply)
    settled()
    uses = uses_after("last solid part", before)
    check("object_delete {} on both solid parts shows the object list's message for the first", uses == before + 1, f"{before} -> {uses}")
    left = client.call("object_list", detail=True)["plates"][0]["objects"][0]["parts"]
    check("and deletes the second alone", [p["id"] for p in left] == [parts[0]["id"], parts[2]["id"]], left)
    check("and its reply names the part that went, as it was, and no other",
          [(p.get("part"), p.get("part_id")) for p in reply.get("parts", [])] == [(1, parts[1]["id"])], reply)
    check("and the part asked for that stayed, under kept, where it is now",
          [(p.get("part"), p.get("part_id")) for p in reply.get("kept", {}).get("parts", [])] == [(0, parts[0]["id"])], reply)
    client.call("dialog_auto_answer", rules=[])

    # ---- A cut object: refused by index, asked about with no arguments, and a pick's cancelled question shows nothing ----
    client.call("dialog_auto_answer", rules=[])
    client.call("project_new", discard_changes=True)
    client.call("view_set", tab="prepare", camera="top")
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Cut"})
    client.call("widget_set_value", target={"imgui_name": "move_Z"}, value=10)
    client.call("widget_click", target={"imgui": "Add connectors"})
    client.call("mouse_gesture", at={"object": 0, "spot": "top"}, check="point")
    client.call("widget_click", target={"imgui": "Confirm connectors"})
    client.call("widget_click", target={"imgui": "Perform cut"})
    shared.wait(client, "idle")
    check("the cube is cut in two", total() == 2, total())
    refused("object_delete of a cut object by index is refused with the question Delete asks", "object_delete", {"objects": [0]},
            "part of a cut object", "cut_object")
    client.call("object_select", objects=[0])
    asked = client.call("object_delete")
    check("object_delete with no arguments asks about the cut object", asked.get("status") == "blocked_by_dialog", asked)
    if asked.get("status") == "blocked_by_dialog":
        client.call("dialog_answer", button="cancel")
        try:
            client.await_job(asked)
            check("and a cancelled question deletes nothing, which the call is told", False)
        except orca_mcp.McpError as e:
            check("and a cancelled question deletes nothing, which the call is told", "Nothing was deleted" in str(e), str(e))
    check("and both halves stay", total() == 2, total())
    client.call("object_select", objects=[0])
    client.call("dialog_auto_answer", rules=[{"match": "which is a part of", "button": "cancel"}])
    before = rule_uses("which is a part of")
    shared.dial_pick(client, "Delete Selected")
    state = settled()
    uses = uses_after("which is a part of", before)
    check("a pick of Delete on a cut object asks the object list's question", uses == before + 1, f"{before} -> {uses}")
    check("and, cancelled, shows no notice", not any("Nothing was deleted" in text for text in notices(state)), notices(state))
    check("and deletes nothing", total() == 2, total())

    # ---- Split: a mesh of one shell is one piece, and a split into objects that would ask the Auto-Drop question ----
    client.call("dialog_auto_answer", rules=[])
    client.call("project_new", discard_changes=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
    refused("object_split of a mesh of one shell is refused, saying so", "object_split", {"object": 0, "into": "objects"},
            "one piece", "one_shell")
    check("and splits nothing", total() == 1, total())
    client.call("object_select", objects=[0])
    shared.dial_pick(client, "Split to Objects")
    state = settled()
    check("a pick of Split to Objects on it says it is one piece", any("one piece" in text for text in notices(state)), notices(state))
    check("and splits nothing", total() == 1, total())

    def floating_pair():
        """A new project with one object: a cube on the bed and a cube part above it, which a split into objects leaves
        floating."""
        client.call("project_new", discard_changes=True)
        client.call("object_add_primitive", shape="cube", size=[20, 20, 20])
        x, y, _ = client.call("object_list", detail=True)["plates"][0]["objects"][0]["instances"][0]["offset"]
        client.call("part_add", object=0, type="part", shape="cube", size=[10, 10, 10], position=[x, y, 40])

    floating_pair()
    refused("object_split that would ask the Auto-Drop question is refused with the question", "object_split",
            {"object": 0, "into": "objects"}, "Auto-Drop", "auto_drop_question")
    check("and splits nothing", total() == 1, total())
    reply = client.call("object_split", object=0, into="objects", keep_z=True)
    check("with keep_z true it splits into two objects", reply.get("object_count") == 2, reply)
    lowest = sorted(o["bbox"]["min"][2] for o in client.call("object_list", detail=True)["plates"][0]["objects"])
    check("and the piece above the bed keeps its height", len(lowest) == 2 and lowest[1] > 20, lowest)

    floating_pair()
    client.call("object_select", objects=[0])
    client.call("dialog_auto_answer", rules=[{"match": "Auto-Drop", "button": "no"}])
    before = rule_uses("Auto-Drop")
    shared.dial_pick(client, "Split to Objects")
    settled()
    uses = uses_after("Auto-Drop", before)
    check("a pick of Split to Objects asks the Auto-Drop question", uses == before + 1, f"{before} -> {uses}")
    lowest = sorted(o["bbox"]["min"][2] for o in client.call("object_list", detail=True)["plates"][0]["objects"])
    check("and, answered No, the pieces drop", len(lowest) == 2 and lowest[1] < 1, lowest)
    client.call("dialog_auto_answer", rules=[])

    # ---- Change Type and Assemble picked with a selection their menu items refused, in the items' words ----
    client.call("object_select", objects=[0])
    shared.dial_pick(client, "Change Type to Modifier")
    state = settled()
    check("a pick of Change Type with an object selected says to select a part", any("Select a part first" in text for text in notices(state)),
          notices(state))
    client.call("object_select", objects=[0])
    shared.dial_pick(client, "Assemble")
    state = settled()
    check("a pick of Assemble with one object selected says to select two or more",
          any("Select two or more objects first" in text for text in notices(state)), notices(state))
    check("and assembles nothing", total() == 2, total())

    # ---- Mirror needs its axis, a plate the project lacks is named, and picks refused as their menu items were ----
    refused("object_mirror with no axis is refused by its schema", "object_mirror", {"objects": [0]}, "axis")
    refused("object_center on a plate the project lacks is refused", "object_center", {"plate": 9}, "plate", "no_such_plate")
    refused("object_add_primitive on a plate the project lacks is refused", "object_add_primitive",
            {"shape": "cube", "size": [10, 10, 10], "plate": 9}, "plate", "no_such_plate")
    check("and adds nothing", total() == 2, total())
    client.call("object_select", objects=[0, 1])
    shared.dial_pick(client, "Mirror X")
    state = settled()
    check("a pick of Mirror X with two objects selected says it takes one instance",
          any("one instance" in text for text in notices(state)), notices(state))
    mirrored = [o["instances"][0]["mirrored"] for o in client.call("object_list", detail=True)["plates"][0]["objects"]]
    check("and mirrors nothing", mirrored == [False, False], mirrored)
    client.call("object_select", objects=[0])
    shared.dial_pick(client, "Drop to Bed")
    state = settled()
    check("a pick of Drop to Bed on an object on the bed says it already rests there",
          any("already rests on the bed" in text for text in notices(state)), notices(state))
    client.call("object_select", objects=[0])
    shared.dial_pick(client, "Cube")
    state = settled()
    check("a pick of Cube with an object selected says to deselect it, as Add Primitive does",
          any("Deselect all objects" in text for text in notices(state)), notices(state))
    check("and adds nothing", total() == 2, total())
    client.call("object_select", objects=[0])
    added = client.call("object_add_primitive", shape="cube", size=[10, 10, 10], name="added")
    check("object_add_primitive with a size adds the object while another is selected", total() == 3 and added.get("object") == 2,
          added)

    # ---- Picks on rows selected in the object list, where its rows and the 3D selection disagree ----
    def cube(name, instances=1, part=None):
        """A 20 mm cube named `name` with `instances` instances and, with `part` ("part" or "modifier"), a 5 mm cube
        part of that type beside its first one, resting on the bed; returns its index."""
        index = client.call("object_add_primitive", shape="cube", size=[20, 20, 20], name=name)["object"]
        if part:
            x, y, _ = client.call("object_list", detail=True)["plates"][0]["objects"][index]["instances"][0]["offset"]
            client.call("part_add", object=index, type=part, shape="cube", size=[5, 5, 5], position=[x + 12, y, 2.5])
        if instances > 1:
            client.call("object_set_instances", object=index, count=instances)
        return index

    def list_rows(*paths):
        """Selects the object list's rows `paths` (their names from the plate down, joined by >), as a click on the
        first and Ctrl+clicks on the others do."""
        for i, path in enumerate(paths):
            client.call("widget_click", target={"row": "Plate 1>" + path}, **({"modifiers": "ctrl"} if i else {}))

    def objects_named():
        return [(o["name"], len(o["parts"])) for o in client.call("object_list", detail=True)["plates"][0]["objects"]]

    def part_types(index):
        return [p["type"] for p in client.call("object_list", detail=True)["plates"][0]["objects"][index]["parts"]]

    client.call("project_new", discard_changes=True)
    cube("pair", instances=2)
    cube("other")
    list_rows("pair>Instances>Instance 1", "pair>Instances>Instance 2")
    shared.dial_pick(client, "Assemble")
    settled()
    check("a pick of Assemble on the two instance rows of an object assembles the two", objects_named() == [("other", 1), ("Assembly", 2)],
          objects_named())

    client.call("project_new", discard_changes=True)
    cube("pair", instances=2)
    cube("other")
    list_rows("pair>Instances>Instance 1", "pair>Instances>Instance 2", "other")
    shared.dial_pick(client, "Assemble")
    settled()
    check("and on those rows and another object's row, all three, none lost", objects_named() == [("Assembly", 3)], objects_named())

    # The object list keeps part rows to one object ("Selection conflicts"), so every part row of one is the most it holds.
    client.call("project_new", discard_changes=True)
    cube("left", part="part")
    cube("right", part="part")
    # Assemble with one object selected, above, showed the same text.
    close_notice("Select two or more objects first")
    list_rows("left>left", "left>Generic-Cube")
    shared.dial_pick(client, "Assemble")
    state = settled()
    check("a pick of Assemble on every part row of an object says to select two or more objects, as its menu item did",
          any("Select two or more objects first" in text for text in notices(state)), notices(state))
    check("and assembles nothing", objects_named() == [("left", 2), ("right", 2)], objects_named())

    client.call("project_new", discard_changes=True)
    cube("mixed", part="modifier")
    list_rows("mixed>mixed", "mixed>Generic-Cube")
    shared.dial_pick(client, "Change Type to Part")
    settled()
    check("a pick of Change Type to Part on an object's part and modifier rows makes the modifier a part",
          part_types(0) == ["part", "part"], part_types(0))

    client.call("project_new", discard_changes=True)
    cube("solid", part="part")
    list_rows("solid>solid", "solid>Generic-Cube")
    shared.dial_pick(client, "Change Type to Negative Part")
    state = settled()
    check("a pick of Change Type to Negative Part on every solid part row says the last solid part keeps its type",
          any("last solid object part cannot be changed" in text for text in notices(state)), notices(state))
    check("and changes nothing", part_types(0) == ["part", "part"], part_types(0))

    # Split reads the object list's one selected row.
    client.call("project_new", discard_changes=True)
    cube("twice", instances=2, part="part")
    client.call("dialog_auto_answer", rules=[{"match": "Auto-Drop", "button": "no"}])
    list_rows("twice>Instances>Instance 1", "twice>Instances>Instance 2")
    shared.dial_pick(client, "Split to Objects")
    state = settled()
    check("a pick of Split to Objects on two instance rows says it takes one",
          any("one object or part at a time" in text for text in notices(state)), notices(state))
    check("and splits nothing", total() == 1, total())
    client.call("project_new", discard_changes=True)
    cube("parts", part="part")
    # Split to Objects on two instance rows, above, showed the same text.
    close_notice("one object or part at a time")
    list_rows("parts>parts", "parts>Generic-Cube")
    shared.dial_pick(client, "Split to Parts")
    state = settled()
    check("a pick of Split to Parts on every part row of an object says it takes one",
          any("one object or part at a time" in text for text in notices(state)), notices(state))
    check("and splits nothing", len(client.call("object_list", detail=True)["plates"][0]["objects"][0]["parts"]) == 2)
    client.call("dialog_auto_answer", rules=[])

    # ---- Auto-Orient of a plate with no objects, and of objects one of which is on a locked plate ----
    client.call("project_new", discard_changes=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 10])
    client.call("object_transform", object=0, rotate=[0, 90, 0], mode="absolute")
    turned = client.call("object_list", detail=True)["plates"][0]["objects"][0]["instances"][0]["rotation_deg"]
    client.call("plate_add")
    refused("object_auto_orient of an empty plate is refused, saying it has no objects", "object_auto_orient", {"plates": [1]},
            "no objects", "no_objects_to_orient")
    now = client.call("object_list", detail=True)["plates"][0]["objects"][0]["instances"][0]["rotation_deg"]
    check("and turns no object elsewhere", now == turned, (turned, now))
    client.call("plate_select", plate=1)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 10], plate=1)
    client.call("object_transform", object=1, rotate=[0, 90, 0], mode="absolute")
    client.call("plate_lock", plate=0, locked=True)
    reply = client.call("object_auto_orient", objects=[0, 1])
    named = [o["index"] for o in reply.get("objects", [])]
    check("object_auto_orient names only what it turned, not an object on a locked plate", named == [1], reply)
    client.call("plate_lock", plate=0, locked=False)

    # ---- Auto-Orient of named objects none of which it can turn: an Assembly of a cube made not printable ----
    client.call("project_new", discard_changes=True)
    client.call("plate_add")
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20], plate=1, name="off")
    client.call("widget_click", target={"row": "Plate 2>off", "column": "Printable"})
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20], plate=1, name="mate")
    client.call("object_assemble", objects=[0, 1])
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20], plate=0, name="edge")
    client.call("object_transform", object=1, rotate=[45, 0, 0], mode="absolute")

    def edge_rotation():
        return client.call("object_list", detail=True)["plates"][0]["objects"][0]["instances"][0]["rotation_deg"]

    on_plate = client.call("object_list", detail=True)["plates"][1]["objects"]
    check("an Assembly of a cube that is not printable stands on plate 1, not printable",
          [(o["name"], o["instances"][0]["printable"]) for o in on_plate] == [("Assembly", False)], on_plate)
    before = edge_rotation()
    refused("object_auto_orient of an object that is not printable is refused, saying why", "object_auto_orient",
            {"objects": [0]}, "not printable", "not_printable")
    check("and turns no other object", edge_rotation() == before, (before, edge_rotation()))
    refused("object_auto_orient of a plate whose only object is not printable is refused, saying why", "object_auto_orient",
            {"plates": [1]}, "not printable", "not_printable")
    check("and turns no other object", edge_rotation() == before, (before, edge_rotation()))
    # With the object that is not printable selected, the job turns every other object, as the menu item's did, and the
    # reply names what it turned.
    client.call("object_select", objects=[0])
    reply = client.call("action_run", id="orca_command:obj_orient:orca")
    check("an Auto-Orient pick of an object that is not printable names the other object, which the job turned",
          [o["index"] for o in reply.get("objects", [])] == [1] and edge_rotation() != before, (reply, edge_rotation()))

    # ---- The plates: Duplicate Plate is one undo step, and Rename Plate's dialog cancelled renames nothing ----
    client.call("dialog_auto_answer", rules=[])
    client.call("project_new", discard_changes=True)
    client.call("object_add_primitive", shape="cube", size=[20, 20, 20], name="kept")
    shared.wait(client, "idle")

    def plates():
        return client.call("plate_list").get("total")

    def snapshots():
        """The undo snapshots the app took since the last call (tests/fixtures/ops_undo.py)."""
        return client.call("script_run", filename=UNDO, code="import ops_undo\nresult = ops_undo.taken()")["result"]

    snapshots()
    shared.dial_pick(client, "Duplicate Plate")
    settled()
    check("a pick of Duplicate Plate adds a plate", plates() == 2, plates())
    taken = snapshots()
    check("and is one undo step", taken.get("count") == 1, taken)
    client.call("undo", history="project")
    check("which undo takes back, and that alone", (plates(), total()) == (1, 1), (plates(), total()))

    client.call("dialog_auto_answer", rules=[{"match": "Edit Plate Name", "button": "cancel"}])
    before = rule_uses("Edit Plate Name")
    refused("Rename Plate's action with its dialog cancelled says the plate was not renamed", "action_run",
            {"id": "orca_command:plate_rename:orca"}, "not renamed", "not_renamed")
    check("and its dialog was asked", rule_uses("Edit Plate Name") == before + 1, f"{before} -> {rule_uses('Edit Plate Name')}")
    before = rule_uses("Edit Plate Name")
    shared.dial_pick(client, "Rename Plate")
    state = settled()
    uses = uses_after("Edit Plate Name", before)
    check("a pick of Rename Plate asks for the name", uses == before + 1, f"{before} -> {uses}")
    check("and, cancelled, shows no notice", not any("renamed" in text for text in notices(state)), notices(state))
    name = client.call("plate_list")["items"][0]["name"]
    check("and renames nothing", name == "", name)

    refused("plate_delete of a plate the project lacks is refused", "plate_delete", {"plate": 9}, "Plate 9 does not exist",
            "no_such_plate")
    check("and deletes no plate", plates() == 1, plates())

    def plate_state():
        """Each plate's name, lock and whether it is the current one."""
        return [(p["name"], p["locked"], p["current"]) for p in client.call("plate_list")["items"]]

    before = plate_state()
    for tool, args in (("plate_duplicate", {"plate": 9}), ("plate_rename", {"plate": 9, "name": "nine"}),
                       ("plate_lock", {"plate": 9}), ("plate_select", {"plate": 9})):
        refused(f"{tool} of a plate the project lacks is refused", tool, args, "Plate 9 does not exist", "no_such_plate")
        check(f"and {tool} leaves the plates as they were", plate_state() == before, (before, plate_state()))

    # A rename to the name a plate has already changes nothing: not the current plate, not the undo history.
    client.call("plate_add")
    client.call("plate_rename", plate=1, name="second")
    client.call("plate_select", plate=0)
    before = plate_state()
    snapshots()
    reply = client.call("plate_rename", plate=1, name="second")
    check("a rename of plate 1 to the name it has replies plate 1 and its name",
          (reply.get("plate"), reply.get("name")) == (1, "second"), reply)
    check("and leaves plate 0 the current one", plate_state() == before, (before, plate_state()))
    taken = snapshots()
    check("and takes no undo snapshot", taken.get("count") == 0, taken)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
