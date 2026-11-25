#!/usr/bin/env python3
"""Checks that the callers of the operations layer make no owner call an operation stands for.

The Speed Dial's command runners, the MCP tools and the plugin API's bindings change the app through
src/slic3r/GUI/Operations (docs/HLSD/mcp-server.md, "Operations"). This script bans, in their source
files, the owner methods those operations call, by method name. A ban is on a name, so it also meets
calls that belong to another function: a banned call that stays is listed in
scripts/operation_calls_baseline.txt, by file and exact source line, under the reason it stays.

The check fails on a banned call that is not listed, and on a listed line that is no longer in the
source, so the list shrinks, or is edited in plain sight.

    python3 scripts/check_operation_calls.py           # check
    python3 scripts/check_operation_calls.py --print   # print every banned call found, as baseline lines

It does not catch a rule written again through calls that are not banned, such as a check made of
getters; the rule in the design doc and review cover that.
"""

import argparse
import collections
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = "scripts/operation_calls_baseline.txt"

# The callers: the command runners, the tools of every place, the Design commands among them, and the plugin API.
CALLERS = ["src/slic3r/GUI/NativeCommands.cpp", "src/slic3r/GUI/Mcp/Tools/*.cpp", "src/slic3r/plugin/host/*.cpp"]

# Owner methods an operation calls, by the operation that stands for them.
BANNED = {
    "start_gcode_export": ["export_gcode_to"],
    "import_models": ["load_files", "import_zip_archive", "preview_zip_archive"],
    "open_project": ["load_project"],
    "new_project": ["new_project"],
    "save_project_as": ["save_project"],
    "export_sliced_3mf": ["export_gcode_3mf"],
    "export_model_3mf": ["export_core_3mf"],
    "export_stl": ["export_stl"],
    "export_toolpaths_obj": ["export_toolpaths_to_obj"],
    "select_plate": ["select_plate"],
    "set_plate_locked": ["lock_plate", "set_plate_locked"],
    "delete_plate": ["delete_plate"],
    "duplicate_plate": ["duplicate_plate"],
    "rename_plate": ["rename_plate"],
    "start_arrange": ["set_prepare_state", "arrange"],
    "start_slice": ["request_slice"],
    "show_layer_range": ["SetHigherValue", "SetLowerValue", "show_layer_range"],
    "select_preset": ["select_preset"],
    "assign_filament_preset": ["set_filament_preset", "select_filament_preset"],
    "import_presets": ["import_configs"],
    "set_instance_count": ["set_number_of_copies"],
    "delete_objects": ["delete_from_model_and_list", "delete_all_objects_from_model", "remove_selected"],
    "rename_object": ["rename_item"],
    "separate_instances": ["instances_to_separated_object", "instances_to_separated_objects", "split_instances"],
    # assemble_objects calls ObjectList::merge(), a name too common to ban (a bounding box's merge()).
    "split_object": ["split_object", "split_volume"],
    "change_part_type": ["set_volume_type"],
    "order_objects": ["order_objects"],
    "mirror_objects": ["mirror"],
    "center_objects": ["center_selection", "do_center_plate"],
    "scale_to_fit": ["scale_selection_to_fit_print_volume"],
    "drop_objects": ["drop_selection"],
    "orient_objects": ["orient"],
    "add_primitive": ["load_shape_object"],
    "set_objects_printable": ["toggle_printable_state", "update_printable_state", "set_printable"],
    "set_objects_auto_drop": ["set_auto_drop", "toggle_auto_drop"],
    "fix_objects": ["fix_through_cgal", "fix_all_through_cgal"],
    # clone_objects calls Selection::clone(), a name too common to ban (ConfigOption::clone()).
    "clone_objects": ["clone_selection"],
    "reload_parts": ["reload_parts_from_disk", "reload_from_disk", "reload_all_from_disk"],
    "replace_part": ["replace_part_with_file", "replace_with_stl"],
    # The Print button's handlers run the print dialogs through it, as its Print and Send open them.
    "open_print_dialog": ["send_gcode_legacy", "send_to_printer"],
    # The Device tab's job buttons send the same commands; the operation adds the checks a call needs without the tab.
    "send_job_command": ["command_task_pause", "command_task_resume", "command_task_abort"],
    "sync_presets": ["restart_sync_user_preset", "start_sync_user_preset"],
    "add_filament_slot": ["add_filament"],
    "delete_filament_slot": ["delete_filament", "change_filament"],
    "add_mixed_filament": ["ensure_mixed_filament", "add_mixed_filament"],
    # uninstall_printers, and the Printer Selection dialog's Confirm, apply the set through it too (apply_install_set).
    "install_printers": ["apply_vendor_config"],
    # uninstall_filaments writes the filaments section through it too.
    "install_filaments": ["set_section"],
    # The Preferences dialog's controls run these effects through apply_preference(), as set_preferences does.
    "set_preferences": ["set_visible_page_count", "set_max_recent_count", "update_filaments_area_height"],
    # The Plugins window's switches, Delete and Unsubscribe, and Reload and Reinstall run these through the operations.
    "set_plugin_enabled": ["load_plugin", "unload_plugin", "set_capability_enabled"],
    "uninstall_plugin": ["delete_plugin", "delete_and_unsubscribe_cloud_plugin"],
    "reload_plugin": ["download_and_install_cloud_plugin"],
    # The sync dialog's Synchronize now runs the sync through the operation, and a drop in the object list a drag.
    "sync_filaments_from_ams": ["sync_ams_list", "apply_ams_sync"],
    "reorder_objects": ["move_object"],
    # The gizmo commands open their gizmo through it; a gizmo icon's click stays the GUI tools' (UiInspector.cpp).
    "open_gizmo": ["open_gizmo"],
    "cut_object": ["apply_cut_object_to_model", "perform_with_plane"],
    "lay_on_face": ["flattening_rotate"],
    "the undo snapshot of an operation": ["take_snapshot"],
}

# A call of a method: obj.name( or obj->name( . An operation is a free function (Ops::name), so the
# operations that share a name with an owner method are not met.
CALL = re.compile(r"(?:\.|->)\s*(" + "|".join(sorted({m for names in BANNED.values() for m in names})) + r")\s*\(")


def caller_files():
    def matches(pattern, name):
        paths = glob.glob(os.path.join(REPO, pattern))
        # A pattern that matches nothing would pass every check unseen, as after its files move.
        if not paths:
            sys.exit(f"{pattern} matches no file: correct {name} in {os.path.basename(__file__)}")
        return {os.path.relpath(path, REPO).replace(os.sep, "/") for path in paths}

    return sorted(set().union(*(matches(pattern, "CALLERS") for pattern in CALLERS)))


def banned_calls():
    """{file: [stripped source line, ...]} for every line that makes a banned call, in file order."""
    found = collections.OrderedDict()
    for rel in caller_files():
        with open(os.path.join(REPO, rel), encoding="utf-8") as f:
            for line in f:
                text = line.strip()
                if text.startswith("//"):
                    continue
                if CALL.search(text.split("//")[0]):
                    found.setdefault(rel, []).append(text)
    return found


def read_baseline():
    """{file: Counter of listed lines}. A [file] line starts a file's section, # starts a reason."""
    listed = collections.OrderedDict()
    current = None
    with open(os.path.join(REPO, BASELINE), encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            text = line.strip()
            if not text or text.startswith("#"):
                continue
            if text.startswith("[") and text.endswith("]") and "(" not in text:
                current = listed.setdefault(text[1:-1], collections.Counter())
            elif current is None:
                sys.exit(f"{BASELINE}:{number}: a source line before any [file] section")
            else:
                current[text] += 1
    return listed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--print", action="store_true", help="print every banned call found, as baseline lines")
    args = ap.parse_args()

    found = banned_calls()
    if args.print:
        for rel, lines in found.items():
            print(f"[{rel}]")
            for text in lines:
                print(text)
            print()
        return 0

    listed = read_baseline()
    problems = []
    for rel, lines in found.items():
        left = collections.Counter(lines)
        left.subtract(listed.get(rel, collections.Counter()))
        for text, count in left.items():
            if count > 0:
                problems.append(f"{rel}: a banned owner call that is not in {BASELINE}:\n    {text}")
    for rel, counts in listed.items():
        left = collections.Counter(counts)
        left.subtract(collections.Counter(found.get(rel, [])))
        for text, count in left.items():
            if count > 0:
                problems.append(f"{BASELINE}: [{rel}] lists a line the source no longer has:\n    {text}")
    if problems:
        print("\n".join(problems))
        print("\nA command runner, a tool or a plugin API binding changes the app through an operation of "
              "src/slic3r/GUI/Operations.\nCall the operation, or, where the call belongs to another function or to "
              f"a path that asks a person, list the line in {BASELINE} under its reason.")
        return 1
    calls = sum(len(lines) for lines in found.values())
    print(f"{calls} banned owner calls in {len(caller_files())} caller files, all listed in {BASELINE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
