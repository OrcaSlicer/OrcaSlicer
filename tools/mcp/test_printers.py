#!/usr/bin/env python3
"""printer_status and the job commands with no printer connected, and a printer's extruders as settings_set and
plate_set_settings set them, as the test_printers entry of tools/mcp/test-entries/windows_web.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_printers.py

It selects printer presets only and sends nothing to any printer. Exits 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import BAMBU_PLA, X1C, X1C_STD, Checks, dialogs, refusal, run, start  # noqa: E402

H2D, H2D_06 = "Bambu Lab H2D 0.4 nozzle", "Bambu Lab H2D 0.6 nozzle"
H2D_STD, H2D_PLA = "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D"
A1_MINI = "Bambu Lab A1 mini 0.4 nozzle"
ER20, ER20_STD, GENERIC_PLA = "Eryone ER20 0.4 nozzle", "0.20mm Standard @Eryone ER20", "Generic PLA @System"
# What printer_install installs each printer start() does not know from.
MODELS = {H2D: {"vendor": "BBL", "model": "Bambu Lab H2D", "nozzles": ["0.4"]},
          ER20: {"vendor": "Eryone", "model": "Eryone ER20", "nozzles": ["0.4"]}}
NO_PRINTER = {"connected": False}
H2D_START = (H2D, "Standard,Standard")  # the printer preset and its extruders' nozzle flow types


def start_on(client, printer, process, filaments):
    """start() on a printer checks.PRINTERS does not list, installed first with printer_install where the folder
    lacks it."""
    run(client, "project_new", discard_changes=True, preset_changes="discard")
    listed = client.call("preset_list", type="printer", include_uninstalled=True, query=printer, limit=500)["items"]
    if not any(i["name"] == printer and i.get("installed", True) for i in listed):
        run(client, "printer_install", models=[MODELS[printer]], preset_changes="discard")
    return start(client, printer=printer, process=process, filaments=filaments)


def setting(client, key):
    """settings_get's item for one printer setting."""
    return client.call("settings_get", type="printer", keys=[key])["items"][0]


def plate_setting(client, key):
    return client.call("plate_get_settings", keys=[key])["items"][0]


def printer_state(client):
    """The printer preset selected, and the nozzle flow type of each extruder, joined by commas."""
    return client.call("app_state")["presets"]["printer"], setting(client, "nozzle_volume_type")["value"]


def modified(client):
    """Whether the selected printer preset has unsaved changes."""
    return any(item.get("modified") for item in client.call("preset_list", type="printer")["items"] if item.get("selected"))


def refused_with(t, name, client, tool, words, **args):
    """Checks that `tool` is refused with text that holds each of `words`."""
    try:
        reply = client.call(tool, **args)
    except orca_mcp.McpError as e:
        text, _details = refusal(e)
        missing = [w for w in words if w not in text]
        return t.check(name, not missing, f"the text does not hold {missing}: {text}")
    return t.check(name, False, f"not refused: {reply}")


def printer_commands(client, t):
    t.equal("start: the X1 Carbon with its default process and filament", start(client, printer=X1C),
            {"printer": X1C, "process": X1C_STD, "filaments": [BAMBU_PLA]})
    # The guard: no job command is called while a printer is connected.
    status = client.call("printer_status")
    if not t.equal("no printer is connected", status, NO_PRINTER):
        print(f"printer_status found {status.get('name')!r} ({status.get('model')!r}); no job command is called", flush=True)
        return
    for command in ("print_pause", "print_resume", "print_stop"):
        t.refused(f"{command}: refused", lambda: client.call(command), code="no_printer_connected")
        t.equal(f"{command}: printer_status still reads no printer", client.call("printer_status"), NO_PRINTER)
        t.equal(f"{command}: no dialog", dialogs(client), [])


def checked_before_changed(client, t):
    """settings_set checks the whole call before it picks a variant or a flow type in the sidebar."""
    refused_with(t, "a value that does not parse is refused naming it", client, "settings_set",
                 ["Invalid value for printable_height"], type="printer",
                 values={"nozzle_volume_type": {"index": 1, "value": "High Flow"}, "printable_height": "not a height"})
    t.equal("the unparsed call changed nothing", printer_state(client), H2D_START)
    refused_with(t, "another key beside a variant switch is refused, to be set in a second call", client, "settings_set",
                 ["Set printable_height in a second call"], type="printer",
                 values={"nozzle_diameter": "0.6,0.6", "printable_height": 300})
    t.equal("the call with another key changed nothing", printer_state(client), H2D_START)
    refused_with(t, "a flow type the 0.2 nozzle preset does not offer is refused before the switch", client, "settings_set",
                 ["extruder 2 with the printer preset Bambu Lab H2D 0.2 nozzle", "not High Flow"], type="printer",
                 values={"nozzle_diameter": "0.2,0.2", "nozzle_volume_type": {"index": 1, "value": "High Flow"}})
    t.equal("the refused switch changed nothing", printer_state(client), H2D_START)
    applied = client.call("settings_set", type="printer",
                          values={"nozzle_diameter": "0.6,0.6", "nozzle_volume_type": {"index": 1, "value": "High Flow"}})
    t.equal("a variant switch with a flow type its preset offers applies both", applied["applied"],
            {"nozzle_diameter": "0.6,0.6", "nozzle_volume_type": "Standard,High Flow"})
    t.equal("the 0.6 nozzle preset is selected with the second extruder High Flow", printer_state(client),
            (H2D_06, "Standard,High Flow"))
    client.call("settings_set", type="printer", values={"nozzle_diameter": "0.4,0.4"})
    client.call("settings_set", type="printer", values={"nozzle_volume_type": "Standard,Standard"})
    t.equal("the 0.4 nozzle preset and its flow types are back", printer_state(client), H2D_START)


def unsaved_changes(client, t):
    """A variant switch asks about the printer's unsaved changes, as the sidebar's does; reset_all discards them first."""
    client.call("settings_set", type="printer", values={"printable_height": 300})
    reply = client.call("settings_set", type="printer", values={"nozzle_diameter": "0.6,0.6"})
    t.equal("a variant switch with unsaved printer changes asks about them", reply.get("status"), "blocked_by_dialog")
    if reply.get("status") == "blocked_by_dialog":
        client.call("dialog_answer", button="Discard")
        client.await_job(reply)
    state = client.call("app_state")
    t.equal("discarded, the 0.6 nozzle preset is selected with no unsaved change and nothing asks",
            (state["presets"]["printer"], state["dialogs"], modified(client)), (H2D_06, [], False))
    client.call("settings_set", type="printer", values={"nozzle_diameter": "0.4,0.4"})
    client.call("settings_set", type="printer", values={"printable_height": 300})
    reply = client.call("settings_set", type="printer", reset_all=True, values={"nozzle_diameter": "0.6,0.6"})
    state = client.call("app_state")
    t.equal("with reset_all the switch asks nothing",
            (reply.get("status"), state["presets"]["printer"], state["dialogs"], modified(client)), (None, H2D_06, [], False))
    client.call("settings_set", type="printer", values={"nozzle_diameter": "0.4,0.4"})


def two_extruders(client, t):
    """The Bambu Lab H2D: settings_set's checks and the variant switch, a plate's filament grouping, and the second
    extruder's nozzle flow type."""
    t.equal("start: the H2D with two filaments", start_on(client, H2D, H2D_STD, [H2D_PLA, H2D_PLA]),
            {"printer": H2D, "process": H2D_STD, "filaments": [H2D_PLA, H2D_PLA]})
    # The conf remembers each printer preset's flow types (nozzle_volume_types), which a variant switch restores, so a
    # run before on the folder may have left High Flow on the second extruder of the 0.4 and 0.6 presets, as this one
    # does: both are set to Standard first.
    for diameter in ("0.6,0.6", "0.4,0.4"):
        client.call("settings_set", type="printer", values={"nozzle_diameter": diameter, "nozzle_volume_type": "Standard,Standard"})
    t.equal("start: both extruders' nozzle flow type is Standard", printer_state(client), H2D_START)
    checked_before_changed(client, t)
    unsaved_changes(client, t)

    mode = plate_setting(client, "filament_map_mode")
    t.check("a new plate groups its filaments Auto For Flush, and its dialog offers the manual grouping",
            mode["value"] == "Auto For Flush" and "Manual" in mode["meta"]["offered"], mode)
    applied = client.call("plate_set_settings", values={"filament_map_mode": "Manual"})["applied"]
    t.equal("plate_set_settings sets the grouping Manual", applied, {"filament_map_mode": "Manual"})
    mode = plate_setting(client, "filament_map_mode")
    t.equal("plate_get_settings reads the Manual grouping back", (mode["value"], mode["effective"]), ("Manual", "Manual"))
    applied = client.call("plate_set_settings", values={"filament_map": [2, 1]})["applied"]
    t.equal("plate_set_settings sets the filament map", applied, {"filament_map": [2, 1]})
    fmap = plate_setting(client, "filament_map")
    t.equal("plate_get_settings reads the filament map back", (fmap["value"], fmap["effective"]), ([2, 1], [2, 1]))
    refused_with(t, "a filament map naming an extruder the printer lacks is refused", client, "plate_set_settings",
                 ["extruder 3"], values={"filament_map": [1, 3]})
    t.equal("the refused map left the plate's as it was", plate_setting(client, "filament_map")["value"], [2, 1])
    client.call("plate_set_settings", values={"filament_map_mode": None})
    t.equal("null takes the plate back to the project's grouping", plate_setting(client, "filament_map_mode")["value"], None)

    volume = setting(client, "nozzle_volume_type")
    t.equal("the printer reads a nozzle flow type per extruder", (volume["meta"].get("variants"), volume["value"]),
            (["extruder 1", "extruder 2"], "Standard,Standard"))
    applied = client.call("settings_set", type="printer", values={"nozzle_volume_type": {"index": 1, "value": "High Flow"}})
    t.equal("settings_set sets the second extruder's nozzle flow type", applied["applied"],
            {"nozzle_volume_type": "Standard,High Flow"})
    t.equal("settings_get reads the second extruder's nozzle flow type back", setting(client, "nozzle_volume_type")["value"],
            "Standard,High Flow")
    refused_with(t, "a nozzle flow type the extruder's card does not offer is refused", client, "settings_set",
                 ["extruder 1 offers", "not TPU High Flow"], type="printer",
                 values={"nozzle_volume_type": {"index": 0, "value": "TPU High Flow"}})
    refused_with(t, "the extruder count is refused while single extruder multi-material is on", client, "settings_set",
                 ["single extruder multimaterial"], type="printer", values={"extruders_count": 3})
    client.call("settings_set", type="printer", reset_all=True)


def no_flow_type_to_pick(client, t):
    """The Bambu Lab A1 mini, whose nozzle card offers no flow type to pick: a variant switch with a flow type the
    new preset's extruder does not have is refused before the switch."""
    run(client, "preset_select", type="printer", name=A1_MINI, preset_changes="discard")
    t.equal("the printer is the Bambu Lab A1 mini 0.4 nozzle, its extruder Standard", printer_state(client),
            (A1_MINI, "Standard"))
    refused_with(t, "a flow type the 0.2 nozzle preset's card offers none of is refused before the switch", client,
                 "settings_set", ["extruder 1", "no nozzle flow type to pick"], type="printer",
                 values={"nozzle_diameter": "0.2", "nozzle_volume_type": "High Flow"})
    t.equal("the refused switch changed nothing", printer_state(client), (A1_MINI, "Standard"))


def extruder_count(client, t):
    """The Eryone ER20: two extruders in place of one."""
    t.equal("start: the Eryone ER20 with its default process and filament", start_on(client, ER20, ER20_STD, [GENERIC_PLA]),
            {"printer": ER20, "process": ER20_STD, "filaments": [GENERIC_PLA]})
    t.equal("the printer has one extruder", setting(client, "nozzle_diameter")["value"], "0.4")
    applied = client.call("settings_set", type="printer", values={"extruders_count": 2})
    t.equal("settings_set sets the extruder count", applied["applied"], {"extruders_count": 2})
    diameter = setting(client, "nozzle_diameter")
    t.equal("settings_get reads two extruders back", (diameter["value"], diameter["meta"].get("variants")),
            ("0.4,0.4", ["extruder 1", "extruder 2"]))
    client.call("settings_set", type="printer", reset_all=True)
    t.equal("reset_all takes the printer back to one extruder", setting(client, "nozzle_diameter")["value"], "0.4")


def main():
    client, _session = orca_mcp.session_client()
    t = Checks()
    t.scenario("printer commands", lambda: printer_commands(client, t))
    t.scenario("two extruders", lambda: two_extruders(client, t))
    t.scenario("no flow type to pick", lambda: no_flow_type_to_pick(client, t))
    t.scenario("extruder count", lambda: extruder_count(client, t))
    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
