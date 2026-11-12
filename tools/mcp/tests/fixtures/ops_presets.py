"""Helpers for the ops_select_preset and ops_assign_filament_preset replays, run with run_script:

    {"tool": "run_script", "args": {"filename": "${replay_dir}/fixtures/ops_presets.py",
                                    "code": "import ops_presets\nresult = ops_presets.probe()"}}

A run whose filename is an existing file imports the modules beside it, so the replays share one copy of each
helper. Each returns what it saw as a dict, which the step's result keeps. A call into the plugin API that raises
is reported by its exception's type and message rather than failing the step.
"""
import orca

try:
    import ops_undo
except ImportError:  # ops_undo.py reads the undo snapshots from the log; without it they are not reported
    ops_undo = None

call = orca.host.tools.call

# Filament colours and a flushing volume no preset or calculation gives, so a probe shows what a call wrote.
SENTINEL_COLOURS = ["#111111", "#222222", "#333333", "#444444"]
SENTINEL_FLUSH = 777


def _name(preset):
    return preset.name + (" (modified)" if preset.is_dirty else "")


def _undo():
    return ops_undo.taken()["taken"] if ops_undo is not None else "ops_undo.py missing"


def probe():
    """What a call left: the slot mapping, the preset each tab edits, the three filament colour settings that
    PlaterPresetComboBox::update_ams_color() writes, the project scope's colours and flushing volumes, the open
    dialogs, the project's dirty flag and the undo snapshots taken since the last probe or arrange."""
    bundle = orca.host.preset_bundle()
    state = call("get_app_state", {})
    project = call("get_settings", {"scope": "project", "keys": ["filament_colour", "flush_volumes_matrix"],
                                    "include_meta": False})
    return {
        "slots": bundle.current_filament_preset_names(),
        "filament_tab": _name(bundle.filaments.edited_preset()),
        "process_tab": _name(bundle.prints.edited_preset()),
        "printer_tab": _name(bundle.printers.edited_preset()),
        "filament_colour": bundle.full_config_value("filament_colour"),
        "filament_colour_type": bundle.full_config_value("filament_colour_type"),
        "filament_multi_colour": bundle.full_config_value("filament_multi_colour"),
        "project": {item["key"]: item["value"] for item in project["items"]},
        "dialogs": [dialog.get("title") for dialog in state["dialogs"]],
        "project_dirty": state["project"]["dirty"],
        "undo_taken": _undo(),
    }


def drop_filament_edits():
    """Drops the unsaved changes of the filament preset the tab edits. set_settings reaches the filament tab
    through a slot (slot 1 by default) and switches the tab to that slot's preset first, which asks about
    the changes, so it is given the slot that holds the edited preset; when no slot holds it, the question
    that opens is answered with Discard."""
    bundle = orca.host.preset_bundle()
    edited = bundle.filaments.edited_preset()
    if not edited.is_dirty:
        return
    names = bundle.current_filament_preset_names()
    slot = names.index(edited.name) + 1 if edited.name in names else 1
    reply = call("set_settings", {"scope": "filament", "filament_slot": slot, "reset_all": True})
    if isinstance(reply, dict) and reply.get("status") == "blocked_by_dialog":
        call("answer_dialog", {"button": "Discard"})
        call("wait", {"for": "job", "job": reply["job"]})


def arrange_filaments(args):
    """The state a filament case starts from. args: slots (each slot's preset, by the name its selector shows;
    the slot count must already match), tab_slot (1-based: the filament tab edits that slot's preset, default
    1), dirty (an unsaved slow_down_layer_time on the preset the tab edits). Colours and flushing volumes are
    set to the sentinels. The slot presets are assigned with select_preset and the tab switched with
    set_settings filament_slot, both after the edits are dropped, so no prompt opens."""
    bundle = orca.host.preset_bundle()
    wanted = args["slots"]
    if len(bundle.current_filament_preset_names()) != len(wanted):
        raise RuntimeError("the project has %d slots, the case wants %d"
                           % (len(bundle.current_filament_preset_names()), len(wanted)))
    drop_filament_edits()
    for slot, name in enumerate(wanted, 1):
        call("select_preset", {"type": "filament", "name": name, "filament_slot": slot})
    call("set_settings", {"scope": "filament", "filament_slot": args.get("tab_slot", 1), "reset_all": True})
    n = len(wanted)
    matrix = [0 if i == j else SENTINEL_FLUSH for i in range(n) for j in range(n)]
    call("set_settings", {"scope": "project", "values": {"filament_colour": SENTINEL_COLOURS[:n],
                                                         "flush_volumes_matrix": matrix}})
    if args.get("dirty"):
        call("set_settings", {"scope": "filament", "values": {"slow_down_layer_time": 9}})
    call("wait", {"for": "idle"})
    _undo()
    return probe()


def arrange_presets(args):
    """The state a printer or process case starts from. args: printer, process (names), dirty ("process": an
    unsaved wall_loops; "printer": unsaved printer notes). Every preset's edits are dropped first."""
    drop_filament_edits()
    for scope in ("process", "printer"):
        call("set_settings", {"scope": scope, "reset_all": True})
    bundle = orca.host.preset_bundle()
    if bundle.printers.edited_preset().name != args["printer"]:
        call("select_preset", {"type": "printer", "name": args["printer"], "preset_changes": "discard"})
    if bundle.prints.edited_preset().name != args["process"]:
        call("select_preset", {"type": "process", "name": args["process"], "preset_changes": "discard"})
    dirty = args.get("dirty")
    if dirty == "process":
        call("set_settings", {"scope": "process", "values": {"wall_loops": 5}})
    elif dirty == "printer":
        call("set_settings", {"scope": "printer", "values": {"printer_notes": "MCP pin"}})
    call("wait", {"for": "idle"})
    _undo()
    return probe()


def _transaction(t):
    return {"target_name": t.target_name, "selection_changed": t.selection_changed, "slot_number": t.slot_number,
            "category": t.category}


def _update_result(r):
    return {"target_name": r.target_name, "selection_changed": r.selection_changed,
            "slot_assignment_changed": r.slot_assignment_changed, "shared_slots": list(r.shared_slots),
            "keys": list(r.keys), "dirty": r.dirty}


def preset_api(args):
    """One call into orca.host.preset.<editor> (process, printer or filament). args: form "begin_update" with
    slot and name as given (either may be left out), then set [key, value] and commit when asked; or form
    "update" with slot when given, key and value."""
    editor = getattr(orca.host.preset, args["editor"])
    out = {"call": args}
    try:
        if args["form"] == "begin_update":
            positional = [args[k] for k in ("slot", "name") if k in args]
            t = editor.begin_update(*positional)
            out["begin_update"] = _transaction(t)
            if "set" in args:
                t.set(args["set"][0], args["set"][1])
            if args.get("commit"):
                out["commit"] = _update_result(t.commit())
        else:
            positional = [args[k] for k in ("slot", "key", "value") if k in args]
            editor.update(*positional)
            out["update"] = "returned"
    except Exception as e:
        out["raised"] = {"type": type(e).__name__, "message": str(e)}
    return out
