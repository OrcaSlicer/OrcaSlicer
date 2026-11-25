"""Checks that opening a project loads the settings it was saved with, whatever the load-behaviour preference says.
Its design is the test_project_open_settings entry of tools/mcp/test-entries/windows_web.md.

Launches OrcaSlicer on a fresh data folder, seeded with the default printer, whose Preferences say to load the
geometry of a project only (project_load_behaviour load_geometry_only, which a person picks under Load behavior),
imports a cube, sets the process's layer_height to 0.28, saves the project, starts a new one that drops the presets'
changes, which gives back the 0.2 of 0.20mm Standard @MyKlipper, and opens the saved file as a project: the process
then has the project's layer_height again, and the preference is unchanged.
Run it as python tools/mcp/test_project_open_settings.py [--bin PATH] [--datadir PATH].
"""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, free_port, harness, presets  # noqa: E402
from orca_mcp import DEFAULT_PRINTER, OrcaMcp, app_env, seed_datadir, write_conf  # noqa: E402

CUBE = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tests", "data", "20mm_cube.obj")
TOKEN = "project-open-settings-test-token"
LAYER_HEIGHT = 0.28
# The layer height of the seeded process, 0.20mm Standard @MyKlipper.
PRESET_LAYER_HEIGHT = 0.2
# The test's named checks; check() records one and prints it.
t = Checks()
check = t.check


def layer_height(orca):
    """The process's layer_height being edited, as a number."""
    items = orca.call("settings_get", type="process", keys=["layer_height"])["items"]
    return float(items[0]["value"])


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    datadir = os.path.abspath(extra[extra.index("--datadir") + 1]) if "--datadir" in extra else tempfile.mkdtemp(prefix="oos-")
    port = free_port()
    env = app_env(dict(os.environ, ORCA_MCP_SESSION=os.path.join(datadir, "session.json"), ORCA_MCP_TOKEN=TOKEN), datadir)
    try:
        # The preference a person sets under Preferences > Load behavior: a project's geometry only.
        seed_datadir(datadir, DEFAULT_PRINTER)
        conf = os.path.join(datadir, "OrcaSlicer.conf")
        with open(conf, encoding="utf-8") as f:
            seeded = json.load(f)
        seeded["app"]["project_load_behaviour"] = "load_geometry_only"
        write_conf(conf, seeded)
        launched = harness(env, "launch", "--datadir", datadir, "--port", str(port), *binary)
        if launched.returncode != 0:
            print(launched.stdout, launched.stderr)
            return 1
        orca = OrcaMcp(port=port, token=TOKEN)

        selected = presets(orca)
        check("the app starts on the default printer",
              selected == {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}, selected)
        preference = orca.call("preferences_get", keys=["project_load_behaviour"])["items"]
        check("the app loads a project's geometry only", len(preference) == 1 and preference[0]["key"] == "project_load_behaviour"
              and preference[0]["value"] == "load_geometry_only", preference)
        orca.call("project_import", paths=[CUBE])
        orca.call("settings_set", values={"layer_height": LAYER_HEIGHT})
        check("the process is set to the layer height the project keeps", layer_height(orca) == LAYER_HEIGHT, layer_height(orca))
        project = os.path.join(datadir, "layer_height.3mf")
        orca.call("project_save", path=project, overwrite=True)
        orca.call("project_new", discard_changes=True, preset_changes="discard")
        check("a new project prints with the preset's layer height", layer_height(orca) == PRESET_LAYER_HEIGHT,
              layer_height(orca))

        opened = orca.call("project_open", path=project, discard_changes=True)
        check("the project opens with its object", len(opened.get("names", [])) == 1, opened)
        check("the opened project prints with the layer height it was saved with", layer_height(orca) == LAYER_HEIGHT,
              layer_height(orca))
        preference = orca.call("preferences_get", keys=["project_load_behaviour"])["items"]
        check("the preference is as it was", preference[0]["value"] == "load_geometry_only", preference)
    finally:
        harness(env, "stop")
        if "--datadir" not in extra:
            shutil.rmtree(datadir, ignore_errors=True)

    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
