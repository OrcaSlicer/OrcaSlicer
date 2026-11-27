"""Files for test_additions_refusals.py, run with script_run:

    client.call("script_run", filename=".../fixtures/filaments_and_files.py",
                code="import filaments_and_files\nresult = filaments_and_files.write_zip(args)",
                args={"name": "cubes.zip", "cubes": {"a.stl": 10}})

A script's own files are held to OrcaSlicer's configuration folder unless scripts run unrestricted, so the files go in
<datadir>/mcp/filaments_and_files/, which every run of the sweep has to itself. Each function returns what it did as a
dict, which the run's result keeps.
"""
import os
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


def write_zip(args):
    """Writes the archive args["name"] of cubes, each entry of args["cubes"] ({entry: size in mm}) an STL, with a
    text file beside them that is no model. Returns its path."""
    path = os.path.join(folder(), args["name"])
    with zipfile.ZipFile(path, "w") as archive:
        for entry, size in args["cubes"].items():
            archive.writestr(entry, cube_stl(size))
        archive.writestr("readme.txt", "not a model\n")
    return {"path": path}
