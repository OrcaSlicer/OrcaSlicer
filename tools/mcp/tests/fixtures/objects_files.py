"""Model files for the objects_state and objects_files replays, run with script_run:

    {"tool": "script_run", "args": {"filename": "${replay_dir}/fixtures/objects_files.py",
                                    "code": "import objects_files\nresult = objects_files.import_cube(args)",
                                    "args": {"name": "open.stl", "size": 20, "holed": true}}}

A script's own files are held to OrcaSlicer's configuration folder unless scripts run unrestricted, so the files go in
<datadir>/mcp/objects_files/, which every run of the sweep has to itself, and the functions that need a file's path
import it themselves. Each returns what it did as a dict, which the step's result keeps.
"""
import os
import time

import orca

call = orca.host.tools.call

# The corners of a cube and its twelve triangles, each turned outward; the last is the one a holed cube lacks.
CORNERS = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
TRIANGLES = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]


def folder():
    """<datadir>/mcp/objects_files, created when missing; the app's log is in <datadir>/log."""
    log = call("app_log", {"lines": 1})["file"]
    path = os.path.join(os.path.dirname(os.path.dirname(log)), "mcp", "objects_files")
    os.makedirs(path, exist_ok=True)
    return path


def write_cube(args):
    """Writes an ASCII STL of a cube of args["size"] mm as args["name"], without one triangle when args["holed"],
    which leaves three open edges. Returns its path."""
    size = float(args.get("size", 20))
    triangles = TRIANGLES[:-1] if args.get("holed") else TRIANGLES
    path = os.path.join(folder(), args["name"])
    lines = ["solid cube"]
    for triangle in triangles:
        lines += ["  facet normal 0 0 0", "    outer loop"]
        lines += ["      vertex %g %g %g" % tuple(c * size for c in CORNERS[i]) for i in triangle]
        lines += ["    endloop", "  endfacet"]
    lines.append("endsolid cube")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return {"path": path}


def import_cube(args):
    """Writes the cube write_cube() writes and imports it with project_import; returns the path and the reply."""
    path = write_cube(args)["path"]
    return {"path": path, "import": call("project_import", {"paths": [path]})}


def repair_notices():
    """The notifications Fix Model's menu row ends in (the object list's fix_through_cgal()), and the open dialogs.
    The object info notification of a selected object, which offers Fix Model, is not one."""
    state = call("app_state", {})
    ends = ("has been repaired", "have been repaired", "needed no repair", "Failed to repair", "Repairing was canceled")
    notices = [n["text"] for n in state["notifications"] if any(end in n["text"] for end in ends)]
    return {"notices": notices, "dialogs": [d.get("title") for d in state["dialogs"]]}


def mesh_errors(index):
    """Object `index`'s mesh errors as object_list gives them."""
    listing = call("object_list", {})
    rows = [row for plate in listing["plates"] for row in plate["objects"]] + listing.get("outside", [])
    return next(row["mesh_errors"] for row in rows if row["index"] == index)


def wait_repaired(args):
    """Waits up to 30 s for object args["object"] to have no open edge and for Fix Model's notification, since the
    command of a menu row that a mouse_gesture picks runs after the gesture's reply, and its progress dialog lets
    calls in while it repairs, before the notification is pushed; returns its mesh errors and repair_notices()."""
    deadline = time.monotonic() + 30
    errors = mesh_errors(args["object"])
    notices = repair_notices()
    while (errors["open_edges"] > 0 or not notices["notices"]) and time.monotonic() < deadline:
        time.sleep(0.2)
        errors = mesh_errors(args["object"])
        notices = repair_notices()
    return {"mesh_errors": errors, **notices}


def remove(args):
    """Deletes the file args["name"] of the folder; returns whether it was there."""
    path = os.path.join(folder(), args["name"])
    there = os.path.exists(path)
    if there:
        os.remove(path)
    return {"path": path, "removed": there}


def replace_part(args):
    """part_replace of part args["part"] of object args["object"] with the file args["name"] of the folder."""
    path = os.path.join(folder(), args["name"])
    return call("part_replace", {"object": args["object"], "part": args["part"], "path": path})


def part_row(index, part):
    """Part `part` of object `index` as object_list gives it."""
    listing = call("object_list", {})
    rows = [row for plate in listing["plates"] for row in plate["objects"]] + listing.get("outside", [])
    return next(row for row in rows if row["index"] == index)["parts"][part]


def replace_keeps(args):
    """part_replace of part args["part"] of object args["object"] with the file args["path"]; returns the reply, and
    whether the part kept its type and filament and how far it moved, from object_list before and after."""
    before = part_row(args["object"], args["part"])
    reply = call("part_replace", {"object": args["object"], "part": args["part"], "path": args["path"]})
    after = part_row(args["object"], reply["parts"][0]["part"])
    return {"reply": reply, "same_id": after["id"] == before["id"],
            "same_type": after["type"] == before["type"], "type": after["type"],
            "same_filament": after.get("filament") == before.get("filament"), "filament": after.get("filament"),
            "moved": max(abs(a - b) for a, b in zip(after["position"], before["position"]))}


def object_count():
    """How many objects object_list lists, on the plates and off them."""
    listing = call("object_list", {})
    return {"count": sum(len(plate["objects"]) for plate in listing["plates"]) + len(listing.get("outside", []))}
