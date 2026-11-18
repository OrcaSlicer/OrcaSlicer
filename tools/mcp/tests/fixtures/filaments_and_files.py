"""Files for the filaments_and_files replay, run with script_run:

    {"tool": "script_run", "args": {"filename": "${replay_dir}/fixtures/filaments_and_files.py",
                                    "code": "import filaments_and_files\nresult = filaments_and_files.export(args)",
                                    "args": {"name": "first.stl", "format": "stl", "objects": [0]}}}

A script's own files are held to OrcaSlicer's configuration folder unless scripts run unrestricted, so the files go in
<datadir>/mcp/filaments_and_files/, which every run of the sweep has to itself, and the functions that write or read a
file call the command that takes its path themselves. Each returns what it did as a dict, which the step's result keeps.
"""
import os
import shutil
import zipfile

import orca

call = orca.host.tools.call

# A cube's corners and its twelve triangles, each turned outward.
CORNERS = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
TRIANGLES = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]


def folder():
    """<datadir>/mcp/filaments_and_files, created when missing; the app's log is in <datadir>/log."""
    log = call("app_log", {"lines": 1})["file"]
    path = os.path.join(os.path.dirname(os.path.dirname(log)), "mcp", "filaments_and_files")
    os.makedirs(path, exist_ok=True)
    return path


def cube_stl(size):
    """An ASCII STL of a cube of `size` mm."""
    lines = ["solid cube"]
    for triangle in TRIANGLES:
        lines += ["  facet normal 0 0 0", "    outer loop"]
        lines += ["      vertex %g %g %g" % tuple(c * size for c in CORNERS[i]) for i in triangle]
        lines += ["    endloop", "  endfacet"]
    lines.append("endsolid cube")
    return "\n".join(lines) + "\n"


def export(args):
    """project_export into the folder: of args["format"] as the file args["name"], or with args["per_object"] into the
    subfolder args["name"], emptied first, of the objects args["objects"] when given. Returns the reply and the size of
    each file written, by its name."""
    path = os.path.join(folder(), args["name"])
    request = {"format": args["format"], "path": path}
    if args.get("per_object"):
        shutil.rmtree(path, ignore_errors=True)
        os.makedirs(path)
        request["per_object"] = True
    elif os.path.exists(path):
        os.remove(path)
    if "objects" in args:
        request["objects"] = args["objects"]
    reply = call("project_export", request)
    if args.get("per_object"):
        sizes = {name: os.path.getsize(os.path.join(path, name)) for name in sorted(os.listdir(path))}
    else:
        sizes = {args["name"]: os.path.getsize(path)}
    return {"reply": reply, "sizes": sizes}


def write_zip(args):
    """Writes the archive args["name"] of cubes, each entry of args["cubes"] ({entry: size in mm}) an STL, with a
    text file beside them that is no model. Returns its path."""
    path = os.path.join(folder(), args["name"])
    with zipfile.ZipFile(path, "w") as archive:
        for entry, size in args["cubes"].items():
            archive.writestr(entry, cube_stl(size))
        archive.writestr("readme.txt", "not a model\n")
    return {"path": path}


def write_project_zip(args):
    """Writes the project as it is as a 3MF and puts it alone into the archive args["name"] of the folder, as the entry
    project.3mf. Returns the archive's path."""
    three_mf = os.path.join(folder(), "project.3mf")
    call("project_export", {"format": "3mf", "path": three_mf, "overwrite": True})
    path = os.path.join(folder(), args["name"])
    with zipfile.ZipFile(path, "w") as archive:
        archive.write(three_mf, "project.3mf")
    return {"path": path}


def import_zip(args):
    """project_import of the archive args["name"] of the folder, with args["entries"] when given; returns the reply."""
    request = {"paths": [os.path.join(folder(), args["name"])]}
    if "entries" in args:
        request["entries"] = args["entries"]
    return call("project_import", request)
