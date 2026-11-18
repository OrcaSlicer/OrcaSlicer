"""Checks a printer's extruders as plate_set_settings and settings_set set them, on printers the sweep does not seed.

The sweep's instance has one printer of one extruder, so this test launches OrcaSlicer itself, twice, each time on a
fresh data folder. With the Bambu Lab H2D 0.4 nozzle (vendor BBL), a printer of two extruders, it sets a plate's
filament grouping, filament_map_mode "Manual" and then a filament_map, and reads each back with plate_get_settings; it
sets the nozzle flow type of the second extruder, as its card in the sidebar picks it, and reads it back with
settings_get. On it, settings_set checks every value of a call before it changes anything: a value that does not parse
leaves the printer preset and the flow types as they were, a nozzle_diameter that selects another variant's preset goes
with nozzle_volume_type alone, and a flow type that preset does not offer is refused before the switch; the switch asks
about the printer's unsaved changes, which reset_all discards first. With the Bambu Lab A1 mini 0.4 nozzle then
selected, whose nozzle card offers no flow type to pick, a switch to its 0.2 nozzle preset with a flow type is refused
before the switch. With the Eryone ER20 0.4 nozzle (vendor Eryone),
a printer of one extruder whose Extruders field the Printer settings let a person change (single extruder multi-material
is off), it sets extruders_count to 2 and reads two extruders back. It selects printer presets only and connects to no
printer.
Run it as python tools/mcp/test_printer_extruders.py [--bin PATH] [--datadir PATH].
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from orca_mcp import McpError, OrcaMcp, app_env  # noqa: E402

TOKEN = "printer-extruders-test-token"
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[:400]), flush=True)


def free_port():
    """A port nothing listens on now, so that this test runs beside the others of a sweep."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def harness(env, *args):
    return subprocess.run([sys.executable, os.path.join(HERE, "orca_mcp.py"), *args], env=env, capture_output=True, text=True,
                          timeout=300)


def refusal(orca, tool, **args):
    """The message of the error `tool` replies, or None when it runs."""
    try:
        orca.call(tool, **args)
    except McpError as e:
        return str(e)
    return None


def setting(orca, key, **args):
    """settings_get's item for one printer setting."""
    return orca.call("settings_get", type="printer", keys=[key], **args)["items"][0]


def plate_setting(orca, key):
    return orca.call("plate_get_settings", keys=[key])["items"][0]


def launched(datadir, vendor, printer, filament, binary):
    """An instance on a fresh `datadir` whose one printer is `printer` ("<model>@<nozzle>") of `vendor`: its client, and
    the environment that stops it."""
    port = free_port()
    env = app_env(dict(os.environ, ORCA_MCP_SESSION=os.path.join(datadir, "session.json"), ORCA_MCP_TOKEN=TOKEN), datadir)
    started = harness(env, "launch", "--datadir", datadir, "--port", str(port), "--vendor", vendor, "--printer", printer,
                      "--filament", filament, *binary)
    if started.returncode != 0:
        print(started.stdout, started.stderr)
        return None, env
    return OrcaMcp(port=port, token=TOKEN), env


def printer_state(orca):
    """The printer preset selected, and the nozzle flow type of each extruder."""
    return orca.call("app_state")["presets"]["printer"], setting(orca, "nozzle_volume_type")["value"]


def modified(orca):
    """Whether the selected printer preset has unsaved changes."""
    return any(item.get("modified") for item in orca.call("preset_list", type="printer")["items"] if item.get("selected"))


def checked_before_changed(orca):
    """settings_set checks the whole call before it picks a variant or a flow type in the sidebar."""
    before = printer_state(orca)
    for values in ({"nozzle_diameter": "0.6,0.6", "printable_height": "not a height"},
                   {"nozzle_volume_type": {"index": 1, "value": "High Flow"}, "printable_height": "not a height"}):
        message = refusal(orca, "settings_set", type="printer", values=values)
        check("a call with a value that does not parse changes nothing: " + ", ".join(values),
              message is not None and printer_state(orca) == before, (message, printer_state(orca)))
    message = refusal(orca, "settings_set", type="printer", values={"nozzle_diameter": "0.6,0.6", "printable_height": 300})
    check("another key beside a variant switch is refused, to be set in a second call",
          message is not None and "second call" in message and printer_state(orca) == before, (message, printer_state(orca)))
    message = refusal(orca, "settings_set", type="printer",
                      values={"nozzle_diameter": "0.2,0.2", "nozzle_volume_type": {"index": 1, "value": "High Flow"}})
    check("a flow type the 0.2 nozzle preset does not offer is refused before the switch",
          message is not None and "extruder 2" in message and printer_state(orca) == before, (message, printer_state(orca)))
    applied = orca.call("settings_set", type="printer",
                        values={"nozzle_diameter": "0.6,0.6", "nozzle_volume_type": {"index": 1, "value": "High Flow"}})
    after = printer_state(orca)
    check("a variant switch with a flow type its preset offers sets both",
          after[0] == "Bambu Lab H2D 0.6 nozzle" and after[1].split(",")[1] == "High Flow", (applied, after))
    orca.call("settings_set", type="printer", values={"nozzle_diameter": "0.4,0.4"})
    orca.call("settings_set", type="printer", values={"nozzle_volume_type": before[1]})
    check("the 0.4 nozzle preset and its flow types are back", printer_state(orca) == before, printer_state(orca))


def unsaved_changes(orca):
    """A variant switch asks about the printer's unsaved changes, as the sidebar's does; reset_all discards them first."""
    orca.call("settings_set", type="printer", values={"printable_height": 300})
    reply = orca.call("settings_set", type="printer", values={"nozzle_diameter": "0.6,0.6"})
    check("a variant switch with unsaved printer changes asks about them", reply.get("status") == "blocked_by_dialog",
          reply)
    if reply.get("status") == "blocked_by_dialog":
        orca.call("dialog_answer", button="Discard")
        orca.await_job(reply)
    state = orca.call("app_state")
    check("discarded, the 0.6 nozzle preset is selected with no unsaved change and nothing asks",
          state["presets"]["printer"] == "Bambu Lab H2D 0.6 nozzle" and not state["dialogs"] and not modified(orca),
          (state["presets"]["printer"], state["dialogs"]))
    orca.call("settings_set", type="printer", values={"nozzle_diameter": "0.4,0.4"})
    orca.call("settings_set", type="printer", values={"printable_height": 300})
    reply = orca.call("settings_set", type="printer", reset_all=True, values={"nozzle_diameter": "0.6,0.6"})
    state = orca.call("app_state")
    check("with reset_all the switch asks nothing", reply.get("status") is None and not state["dialogs"] and
          state["presets"]["printer"] == "Bambu Lab H2D 0.6 nozzle" and not modified(orca), (reply, state["dialogs"]))
    orca.call("settings_set", type="printer", values={"nozzle_diameter": "0.4,0.4"})


def no_flow_type_to_pick(orca):
    """The Bambu Lab A1 mini, whose nozzle card offers no flow type to pick: a variant switch with a flow type the
    new preset's extruder does not have is refused before the switch."""
    orca.call("preset_select", type="printer", name="Bambu Lab A1 mini 0.4 nozzle", preset_changes="discard")
    before = printer_state(orca)
    check("the printer is the Bambu Lab A1 mini 0.4 nozzle", before[0] == "Bambu Lab A1 mini 0.4 nozzle", before)
    message = refusal(orca, "settings_set", type="printer", values={"nozzle_diameter": "0.2", "nozzle_volume_type": "High Flow"})
    check("a flow type the 0.2 nozzle preset's card offers none of is refused before the switch",
          message is not None and "extruder 1" in message and "no nozzle flow type to pick" in message and
          printer_state(orca) == before, (message, printer_state(orca)))


def two_extruders(orca):
    """The Bambu Lab H2D: settings_set's checks and the variant switch, a plate's filament grouping, and the second
    extruder's nozzle flow type."""
    printer = orca.call("app_state")["presets"]["printer"]
    check("the printer is the Bambu Lab H2D 0.4 nozzle", printer == "Bambu Lab H2D 0.4 nozzle", printer)
    orca.call("project_new", discard_changes=True, preset_changes="discard")
    orca.call("filament_add")
    checked_before_changed(orca)
    unsaved_changes(orca)

    mode = plate_setting(orca, "filament_map_mode")
    check("a new plate groups its filaments Auto For Flush, and its dialog offers the manual grouping",
          mode["value"] == "Auto For Flush" and "Manual" in mode["meta"]["offered"], mode)
    applied = orca.call("plate_set_settings", values={"filament_map_mode": "Manual"})["applied"]
    check("plate_set_settings sets the grouping Manual", applied == {"filament_map_mode": "Manual"}, applied)
    mode = plate_setting(orca, "filament_map_mode")
    check("plate_get_settings reads the Manual grouping back", mode["value"] == "Manual" and mode["effective"] == "Manual", mode)
    applied = orca.call("plate_set_settings", values={"filament_map": [2, 1]})["applied"]
    check("plate_set_settings sets the filament map", applied == {"filament_map": [2, 1]}, applied)
    fmap = plate_setting(orca, "filament_map")
    check("plate_get_settings reads the filament map back", fmap["value"] == [2, 1] and fmap["effective"] == [2, 1], fmap)
    message = refusal(orca, "plate_set_settings", values={"filament_map": [1, 3]})
    check("a filament map naming an extruder the printer lacks is refused", message and "extruder 3" in message, message)
    check("the refused map left the plate's as it was", plate_setting(orca, "filament_map")["value"] == [2, 1])
    orca.call("plate_set_settings", values={"filament_map_mode": None})
    check("null takes the plate back to the project's grouping", plate_setting(orca, "filament_map_mode")["value"] is None)

    volume = setting(orca, "nozzle_volume_type")
    check("the printer reads a nozzle flow type per extruder",
          volume["meta"].get("variants") == ["extruder 1", "extruder 2"] and len(volume["value"].split(",")) == 2, volume)
    first = volume["value"].split(",")[0]
    applied = orca.call("settings_set", type="printer", values={"nozzle_volume_type": {"index": 1, "value": "High Flow"}})
    check("settings_set sets the second extruder's nozzle flow type",
          applied["applied"]["nozzle_volume_type"] == first + ",High Flow", applied)
    value = setting(orca, "nozzle_volume_type")["value"]
    check("settings_get reads the second extruder's nozzle flow type back", value == first + ",High Flow", value)
    message = refusal(orca, "settings_set", type="printer", values={"nozzle_volume_type": {"index": 0, "value": "TPU High Flow"}})
    check("a nozzle flow type the extruder's card does not offer is refused", message and "extruder 1" in message, message)
    message = refusal(orca, "settings_set", type="printer", values={"extruders_count": 3})
    check("the extruder count is refused while single extruder multi-material is on",
          message and "single extruder multimaterial" in message, message)
    orca.call("settings_set", type="printer", reset_all=True)
    no_flow_type_to_pick(orca)


def extruder_count(orca):
    """The Eryone ER20: two extruders in place of one."""
    printer = orca.call("app_state")["presets"]["printer"]
    check("the printer is the Eryone ER20 0.4 nozzle", printer == "Eryone ER20 0.4 nozzle", printer)
    check("the printer has one extruder", setting(orca, "nozzle_diameter")["value"] == "0.4")
    applied = orca.call("settings_set", type="printer", values={"extruders_count": 2})
    check("settings_set sets the extruder count", applied["applied"] == {"extruders_count": 2}, applied)
    diameter = setting(orca, "nozzle_diameter")
    check("settings_get reads two extruders back",
          diameter["value"] == "0.4,0.4" and diameter["meta"].get("variants") == ["extruder 1", "extruder 2"], diameter)
    orca.call("settings_set", type="printer", reset_all=True)


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    given = "--datadir" in extra
    root = os.path.abspath(extra[extra.index("--datadir") + 1]) if given else tempfile.mkdtemp(prefix="ope-")
    runs = [("h2d", "BBL", "Bambu Lab H2D@0.4", "Bambu PLA Basic @BBL H2D", two_extruders),
            ("er20", "Eryone", "Eryone ER20@0.4", "Generic PLA @System", extruder_count)]
    try:
        for folder, vendor, printer, filament, run in runs:
            datadir = os.path.join(root, folder)
            orca, env = launched(datadir, vendor, printer, filament, binary)
            try:
                if orca is None:
                    check("the app starts with the " + printer, False)
                    continue
                run(orca)
            finally:
                harness(env, "stop")
                time.sleep(2)
    finally:
        if not given:
            shutil.rmtree(root, ignore_errors=True)

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
