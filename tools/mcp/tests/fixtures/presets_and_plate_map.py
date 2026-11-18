"""Checks over whole preset lists for the presets_and_plate_map replay, run with script_run:

    {"tool": "script_run", "args": {"filename": "${replay_dir}/fixtures/presets_and_plate_map.py",
                                    "code": "import presets_and_plate_map\nresult = presets_and_plate_map.listed(args)",
                                    "args": {"type": "filament", "filament_type": "PLA", "vendor": "Bambu Lab"}}}

A replay's expectations read single values, and these checks hold for every item of a list, so each function reads
every page of a preset_list and returns what it found as a dict, which the step's result keeps.
"""
import orca

call = orca.host.tools.call


def items(args):
    """Every item preset_list gives for `args`, page by page."""
    out = []
    while True:
        reply = call("preset_list", dict(args, limit=500, offset=len(out)))
        out += reply["items"]
        if not reply["items"] or len(out) >= reply["total"]:
            return out


def listed(args):
    """preset_list for `args`: how many items, their names, the names of those that do not match its vendor,
    filament_type or source, or lack a field their type gives (a filament its vendor and filament_type, a system
    printer its vendor, printer_model and nozzle), and how many are the Template vendor's."""
    found = items(args)
    fields = {"filament": ["vendor", "filament_type"], "printer": ["vendor", "printer_model", "nozzle"]}.get(args["type"], [])
    filters = [key for key in ("vendor", "filament_type") if key in args]
    others = []
    for item in found:
        matches = all(str(item.get(key, "")).lower() == str(args[key]).lower() for key in filters)
        source = args.get("source")
        if source == "system":
            matches = matches and item.get("system", False)
        elif source == "user":
            matches = matches and item.get("user", False) and not item.get("project", False)
        elif source == "project":
            matches = matches and item.get("project", False)
        if args["type"] == "filament" or item.get("system", False):
            matches = matches and all(key in item for key in fields)
        if not matches:
            others.append(item["name"])
    return {"count": len(found), "names": [item["name"] for item in found], "others": others,
            "template": sum(1 for item in found if str(item.get("vendor", "")).lower() == "template")}


def fitting(args):
    """The filaments that fit the printer preset args["printer"], against those that fit the selected printer, installed
    or not: how many each lists, how many only one of them lists, and how many of the first are marked incompatible."""
    other = items({"type": "filament", "printer": args["printer"], "include_uninstalled": True})
    selected = items({"type": "filament", "include_uninstalled": True})
    other_names = {item["name"] for item in other}
    selected_names = {item["name"] for item in selected}
    return {"other": len(other), "selected": len(selected), "only_other": len(other_names - selected_names),
            "only_selected": len(selected_names - other_names),
            "marked_incompatible": sum(1 for item in other if item.get("compatible") is False)}


def refused(args):
    """Calls args["tool"] with args["args"]: the error's message when it is refused, else the reply."""
    try:
        return {"refused": False, "reply": call(args["tool"], args["args"])}
    except Exception as e:
        return {"refused": True, "message": str(e)}
