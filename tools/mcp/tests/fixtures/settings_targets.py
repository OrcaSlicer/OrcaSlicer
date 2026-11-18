"""Checks over whole settings lists for the settings_targets replay, run with script_run:

    {"tool": "script_run", "args": {"filename": "${replay_dir}/fixtures/settings_targets.py",
                                    "code": "import settings_targets\nresult = settings_targets.listing(args)",
                                    "args": {"type": "process"}}}

A replay's expectations read single values, and these checks hold for every item of a list or compare two lists, so
each function reads what it needs and returns what it found as a dict, which the step's result keeps.
"""
import orca

call = orca.host.tools.call


def values(args):
    """settings_list for `args`, which comes whole: {key: value}."""
    return {item["key"]: item["value"] for item in call("settings_list", args)["items"]}


def listing(args):
    """settings_list for `args` with no limit: its total, how many items it holds and whether it has a next page."""
    reply = call("settings_list", args)
    return {"total": reply["total"], "items": len(reply["items"]), "next_offset": "next_offset" in reply}


def value_of(args):
    """The value of `args["key"]` in settings_list for the other arguments."""
    query = dict(args)
    key = query.pop("key")
    return {"value": values(query).get(key)}


def nested(args):
    """Whether the object list holds process settings with the process's values, and the region list the object list's
    settings but layer_height, whose item says a height range alone takes it."""
    process = values({"type": "process"})
    region_items = call("settings_list", {"type": "region"})["items"]
    objects = values({"type": "object"})
    regions = {item["key"]: item["value"] for item in region_items}
    layer_height = [item for item in region_items if item["key"] == "layer_height"]
    return {"object": len(objects), "region": len(regions), "process": len(process),
            "object_not_in_process": [key for key in objects if key not in process],
            "object_values_not_the_process": [key for key in objects if key in process and objects[key] != process[key]],
            "region_not_in_object": [key for key in regions if key != "layer_height" and key not in objects],
            "layer_height_targets": layer_height[0].get("targets") if layer_height else None}


def overrides(args):
    """settings_get with `args` (a target and no keys): {key: value} of what the target overrides, and the reply's
    naming of the target."""
    reply = call("settings_get", args)
    named = {key: reply[key] for key in ("objects", "parts", "height_range") if key in reply}
    return {"values": {item["key"]: item["value"] for item in reply["items"]}, "named": named}
