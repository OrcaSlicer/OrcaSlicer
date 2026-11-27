#!/usr/bin/env python3
"""Checks preferences_get and preferences_set against the Preferences dialog.
Its design is the test_preferences entry of tools/mcp/test-entries/commands.md.

    tools/mcp/orca_mcp.py launch --datadir <a scratch folder>
    tools/mcp/test_preferences.py

On each of the dialog's tabs (window_open, then widget_list of the dialog), the controls named after a key are the keys
preferences_get lists for that tab. The MCP server's switch and port read as the dialog shows them: on, and the port this
session's launch gave. preferences_set writes a checkbox, a choice and a number in one call, and each reads back from
preferences_get and from its control in the dialog opened again. A preference read at the next start says so in
preferences_get and in the reply. A call with a good key and a refused one (the language) changes nothing, and so does a
value no choice has. The backup period runs the backup timer only while Auto backup is on, read from the timer's log
line. The downloads folder takes a folder this test makes in its own temporary folder, which preferences_get and the
dialog's row then show, and refuses one that does not exist; while the dialog is open a call cannot write it, and its
Browse button, answered with another such folder, shows that folder in the open dialog's row. Hide login side panel,
whose control has an effect in the app (Ops::apply_preference()): the Home page hides its login side panel as soon as
preferences_set writes it, without the dialog, and shows it again when it is written back. Last, preset sync and stealth
mode are written on and off again: with no account signed in, turning sync on starts no sync, and stealth mode signs out
of no account.

It starts from the Generic Klipper Printer and the baseline's preferences, and first sets the two that its write puts
back to the app's defaults away from them. It writes only preferences whose effect stays inside the app it drives; it
signs in to nothing and needs no network. Exits 0 when every check passes.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks as shared  # noqa: E402
import orca_mcp  # noqa: E402
from orca_mcp import McpError  # noqa: E402

TABS = ["general", "files", "behaviour", "control", "graphics", "online", "developer"]
# A widget_list line of a control named after a key: its kind, its label and value, its name and its state.
CONTROL = re.compile(r'^\s*\[w\d+\] (\w+)(?: "(?P<label>[^"]*)")?(?: = "(?P<value>[^"]*)")? \([^)]*\) name=(?P<name>\S+)(?P<rest>.*)$')
# The home page's login side panel, as display reads it; "missing" where the page has no such element.
LOGIN_PANEL = "(function () { var e = document.getElementById('LeftBoard'); return e ? getComputedStyle(e).display : 'missing'; })()"
# What the backup timer logs when its period changes.
TIMER_LINE = "set_interval exit, and new interval is:"
START = {"printer": shared.KLIPPER, "process": shared.KLIPPER_STD, "filaments": [shared.GENERIC_PLA]}
# The preferences the checks start from: the two the test sets away from the app's defaults, to which its write puts
# them back, and those the baseline gives, a new data folder's, which the checks write from.
AWAY = {"auto_arrange": False, "auto_calculate_flush": "color change"}
STARTING = {**AWAY, "opengl_fps_cap": 0, "enable_multi_machine": False, "backup_interval": 10, "hide_login_side_panel": False,
            "sync_user_preset": False, "stealth_mode": False}

# The test's named checks; check() records one and prints it.
t = shared.Checks()
check = t.check


def refusal(client, tool, **args):
    """The text of the error `tool` replies, or None when it runs."""
    try:
        client.call(tool, **args)
    except McpError as e:
        return str(e)
    return None


def dialog_listing(client, tab, key=None):
    """The widget_list text of the Preferences dialog opened on `tab`, which it closes again."""
    args = {"window": "preferences", "tab": tab}
    if key:
        args["key"] = key
    opened = client.call("window_open", **args)
    shared.wait(client, "dialog")
    _, texts = client.call_with_text("widget_list", target="dialog", max_nodes=2000)
    client.call("dialog_answer", dialog={"title": "Preferences"}, button="close")
    if isinstance(opened, dict) and opened.get("job"):
        shared.wait(client, "job", job=opened["job"])
    return "\n".join(texts)


def labels(listing):
    """The first static texts of a widget_list listing, to show what a dialog's rows read when a check fails."""
    return [line.strip() for line in listing.splitlines() if "wxStaticText" in line][:8]


def dialog_controls(client, tab, key=None):
    """{name: (label, value, rest)} of the Preferences dialog's controls named after a key, opened on `tab`."""
    controls = {}
    for line in dialog_listing(client, tab, key).splitlines():
        m = CONTROL.match(line)
        if m:
            controls[m.group("name")] = (m.group("label"), m.group("value"), m.group("rest"))
    return controls


def items_by_key(client, **args):
    return {item["key"]: item for item in client.call("preferences_get", **args)["items"]}


def shown_value(item, control):
    """What `item`'s control shows, in preferences_get's terms: True or False, a choice's value, a number."""
    label, value, rest = control
    if item["kind"] == "checkbox":
        return "[checked]" in rest
    if item["kind"] == "choice":
        return next((c["value"] for c in item.get("choices", []) if c["label"] == value), value)
    if item["kind"] == "number":
        return float(value)
    return value


def log_lines(client):
    return client.call("app_log", lines=2000).get("lines", [])


def new_lines(before, after):
    """The lines of `after` past the last line of `before`."""
    start = after.index(before[-1]) + 1 if before and before[-1] in after else 0
    return after[start:]


def main():
    client, session = orca_mcp.session_client()
    presets = shared.start(client)
    check("starts from the Generic Klipper Printer", presets == START, presets)
    client.call("preferences_set", values=AWAY)
    read = {key: item["value"] for key, item in items_by_key(client, keys=list(STARTING)).items()}
    check("the preferences start as written", read == STARTING, read)
    scenarios(client, session)
    return t.exit_code()


def scenarios(client, session):
    # ---- the dialog's controls named after keys are the keys preferences_get lists, tab by tab ----
    every = client.call("preferences_get")
    check("preferences_get with no argument lists the dialog's preferences of every tab",
          {item["tab"] for item in every["items"]} == set(TABS) and every["total"] == len(every["items"]), every.get("total"))
    for tab in TABS:
        controls = dialog_controls(client, tab)
        listed = items_by_key(client, tab=tab)
        check(f"on {tab}, every control named after a key is a preference preferences_get lists",
              set(controls) <= set(listed), sorted(set(controls) - set(listed)))
        check(f"on {tab}, every preference preferences_get lists is a control of the tab",
              set(listed) <= set(controls), sorted(set(listed) - set(controls)))
        mismatched = [key for key in sorted(set(controls) & set(listed))
                      if listed[key]["kind"] in ("checkbox", "choice") and not listed[key].get("refused")
                      and shown_value(listed[key], controls[key]) != listed[key]["value"]]
        check(f"on {tab}, each checkbox and choice shows the value preferences_get gives", not mismatched, mismatched)

    # ---- the MCP server's switch and port, as the dialog shows them ----
    # The harness launches the app with --mcp and --mcp-port, which store neither: the switch is checked while the server
    # runs and the port field shows the port in use.
    server = items_by_key(client, keys=["mcp_enabled", "mcp_port"])
    controls = dialog_controls(client, "general", "mcp_enabled")
    check("preferences_get gives the MCP server's switch on while it runs, as its control shows it",
          server["mcp_enabled"]["value"] is True and "[checked]" in controls.get("mcp_enabled", ("", "", ""))[2],
          (server["mcp_enabled"], controls.get("mcp_enabled")))
    check("and its port the one this session's server is on, as its field shows it",
          server["mcp_port"]["value"] == session["port"] and controls.get("mcp_port", (None, None, None))[1] == str(session["port"]),
          (server["mcp_port"], controls.get("mcp_port"), session["port"]))

    # ---- a checkbox, a choice and a number written in one call, read back ----
    before = {"auto_arrange": False, "auto_calculate_flush": "color change", "opengl_fps_cap": 0}
    values = {"auto_arrange": True, "auto_calculate_flush": "all", "opengl_fps_cap": 45}
    reply = client.call("preferences_set", values=values)
    changed = {c["key"]: c for c in reply.get("changed", [])}
    check("preferences_set replies each key with its value before and after",
          set(changed) == set(values) and all(changed[k]["before"] == before[k] and changed[k]["after"] == values[k]
                                              for k in values), reply)
    after = items_by_key(client, keys=list(values))
    check("preferences_get reads the three values written", all(after[k]["value"] == values[k] for k in values),
          {k: after[k]["value"] for k in values})
    for key in values:
        controls = dialog_controls(client, after[key]["tab"], key)
        shown = shown_value(after[key], controls[key]) if key in controls else None
        check(f"the dialog opened again shows {key} as written", shown == values[key], (shown, values[key]))
    again = client.call("preferences_set", values={"auto_arrange": values["auto_arrange"]})
    check("a value already stored is left as it is and named unchanged",
          again.get("changed") == [] and again.get("unchanged") == ["auto_arrange"], again)

    # ---- a preference read at the next start says so ----
    # Multi device management's row says "(Requires restart)": the value is stored at once and read at the next start.
    multi = items_by_key(client, keys=["enable_multi_machine", "auto_arrange"])
    check("preferences_get marks a preference read at the next start, and not another",
          multi["enable_multi_machine"].get("restart_needed") is True and "restart_needed" not in multi["auto_arrange"], multi)
    try:
        reply = client.call("preferences_set", values={"enable_multi_machine": True, "auto_arrange": False})
    except McpError as e:
        reply = {"error": str(e)}
    changed = {c["key"]: c for c in reply.get("changed", [])}
    check("preferences_set's reply marks it too, and not the other key",
          changed.get("enable_multi_machine", {}).get("restart_needed") is True
          and "restart_needed" not in changed.get("auto_arrange", {"restart_needed": None}), reply)
    client.call("preferences_set", values={"enable_multi_machine": False, "auto_arrange": True})

    # ---- a refused key refuses the whole call ----
    text = refusal(client, "preferences_set", values={"auto_arrange": False, "language": "de"})
    check("a call with a good key and the language is refused, naming the language",
          text is not None and "language" in text and "preference_refused" in text, text)
    check("and the good key is unchanged", items_by_key(client, keys=["auto_arrange"])["auto_arrange"]["value"] is True)
    text = refusal(client, "preferences_set", values={"auto_arrange": False, "auto_calculate_flush": "sometimes"})
    check("a value no choice has is refused with the choice's values",
          text is not None and "no_such_choice" in text and "color change" in text, text)
    check("and nothing is written", items_by_key(client, keys=["auto_arrange"])["auto_arrange"]["value"] is True)
    text = refusal(client, "preferences_set", values={"opengl_fps_cap": 1000})
    check("a number outside its range is refused with the range", text is not None and "out_of_range" in text and "240" in text, text)
    text = refusal(client, "preferences_set", values={"python_scripts_unrestricted": True})
    check("the Python scripts' switch is no preference of the dialog", text is not None and "not_a_preference" in text, text)

    # ---- the backup period runs the backup timer only while Auto backup is on ----
    # The timer is read from the app's log: each change of it logs "set_interval exit, and new interval is: <s>"
    # (_BBS_Backup_Manager::set_interval()), and a period of 0 stops it, as the app's start sets it with the switch off.
    def timer_lines():
        lines = client.call("app_log", lines=2000).get("lines", [])
        return [line for line in lines if TIMER_LINE in line]

    def timer(line):
        return int(line.rsplit(":", 1)[1].strip())

    def periods_set_by(values):
        """The timer's periods the call `values` logged, in order, read once the app is idle."""
        before = timer_lines()
        client.call("preferences_set", values=values)
        shared.wait(client, "idle")
        after = timer_lines()
        start = after.index(before[-1]) + 1 if before and before[-1] in after else 0
        return [timer(line) for line in after[start:]]

    # Each period differs from the 10 the test starts from and from the one before it, so that each write changes it.
    off, on, both = 30, 31, 37
    client.call("preferences_set", values={"backup_switch": True})
    client.call("preferences_set", values={"backup_switch": False})
    logged = periods_set_by({"backup_interval": off})
    check("with Auto backup off, preferences_set backup_interval leaves the backup timer off", logged == [0], logged)
    client.call("preferences_set", values={"backup_switch": True})
    logged = periods_set_by({"backup_interval": on})
    check("with Auto backup on, the same write sets the timer to the period", logged == [on], logged)
    logged = periods_set_by({"backup_switch": False, "backup_interval": both})
    check("a call that turns Auto backup off and writes the period sees the switch it leaves: the timer is off, never the period",
          logged and all(period == 0 for period in logged), logged)

    # ---- the downloads folder takes the absolute path of an existing folder ----
    # The folder is one this test makes in its own temporary folder; the dialog's row shows it as the Browse button
    # leaves it, in the label beside the button.
    folder = tempfile.mkdtemp(prefix="downloads_")
    reply = client.call("preferences_set", values={"download_path": folder})
    changed = reply.get("changed", [])
    check("preferences_set download_path writes a folder that exists",
          [c["key"] for c in changed] == ["download_path"] and changed[0]["after"] == folder, reply)
    stored_path = items_by_key(client, keys=["download_path"])["download_path"]["value"]
    check("preferences_get reads the folder back", stored_path == folder, stored_path)
    listing = dialog_listing(client, "files", "download_path")
    check("the dialog opened again shows the folder in its downloads row", f'label "{folder}" (wxStaticText)' in listing,
          labels(listing))
    missing = folder + "-missing"
    text = refusal(client, "preferences_set", values={"download_path": missing})
    check("a folder that does not exist is refused, naming it", text is not None and "not_a_folder" in text and missing in text, text)
    stored_path = items_by_key(client, keys=["download_path"])["download_path"]["value"]
    check("and the stored folder is unchanged", stored_path == folder, stored_path)

    # While the Preferences dialog is open, a call that writes is refused before it runs (the dialog is modal), so only
    # the dialog's Browse button changes the folder an open dialog shows: answered with another folder, its row shows it.
    browsed = tempfile.mkdtemp(prefix="browsed_")
    opened = client.call("window_open", window="preferences", tab="files", key="download_path")
    shared.wait(client, "dialog")
    text = refusal(client, "preferences_set", values={"download_path": browsed})
    check("with the Preferences dialog open, preferences_set is refused before it writes",
          text is not None and "modal dialog is open" in text, text)

    def browse(path):
        """Clicks the open dialog's Browse with a rule answering its picker with `path`; returns the dialog's listing
        after it."""
        client.call("dialog_auto_answer", rules=[{"match": "Choose Download Directory", "button": "ok", "path": path}])
        client.call("widget_click", target={"name": "download_path", "within": "Preferences"})
        _, texts = client.call_with_text("widget_list", target="dialog", max_nodes=2000)
        return "\n".join(texts)

    try:
        # A folder that does not exist is a path the picker can't take, so the rule cancels it.
        missing_listing = browse(missing)
        missing_uses = [r.get("uses") for r in client.call("app_state")["auto_answer_rules"]]
        missing_stored = items_by_key(client, keys=["download_path"])["download_path"]["value"]
        listing = browse(browsed)
    finally:
        client.call("dialog_auto_answer", rules=[])
        client.call("dialog_answer", dialog={"title": "Preferences"}, button="close")
        if isinstance(opened, dict) and opened.get("job"):
            shared.wait(client, "job", job=opened["job"])
    check("a Browse rule naming a folder that does not exist answers the picker once", missing_uses == [1], missing_uses)
    check("and the open dialog's row still shows the folder", f'label "{folder}" (wxStaticText)' in missing_listing,
          labels(missing_listing))
    check("and preferences_get still reads the folder", missing_stored == folder, missing_stored)
    check("the open dialog's row shows the folder its Browse button picked", f'label "{browsed}" (wxStaticText)' in listing,
          labels(listing))
    stored_path = items_by_key(client, keys=["download_path"])["download_path"]["value"]
    check("and preferences_get reads that folder", stored_path == browsed, stored_path)

    # ---- an effect of a checkbox's control, without the dialog ----
    # Hide login side panel's control sends the cloud providers to the Home page (WebViewPanel::SendCloudProvidersInfo()),
    # which hides its login side panel; the effect runs through Ops::apply_preference() for the dialog and the call alike.
    client.call("preferences_set", values={"hide_login_side_panel": False})
    client.call("view_set", tab="home")

    def panel():
        try:
            return client.call("webview_run_javascript", code=LOGIN_PANEL).get("result")
        except McpError:
            return None

    # The page shows the panel with jQuery's show(), which leaves its display "block" (home.js).
    shown = shared.reached(lambda: panel() == "block", "the login side panel shows")
    check("the Home page shows its login side panel", shown, panel())
    client.call("preferences_set", values={"hide_login_side_panel": True})
    hidden = shared.reached(lambda: panel() == "none", "the login side panel is hidden")
    check("preferences_set hide_login_side_panel true hides it at once, without the dialog", hidden, panel())
    client.call("preferences_set", values={"hide_login_side_panel": False})
    shown_again = shared.reached(lambda: panel() == "block", "the login side panel shows again")
    check("and false shows it again", shown_again, panel())
    client.call("view_set", tab="prepare")

    # ---- preset sync and stealth mode, with no account signed in ----
    # Each is written on and off again, as its control does, and read back from preferences_get and from the dialog. Their
    # effects (GUI_App::start_sync_user_preset(), GUI_App::on_stealth_mode_enter()) act only for a signed-in account: the
    # log shows that turning sync on started no sync ("start_sync_service...") and that stealth mode ran its effect. Sync
    # goes first, while stealth mode is off, as the dialog leaves its checkbox enabled only then.
    for key, effect in (("sync_user_preset", None), ("stealth_mode", "logout: on_stealth_mode_enter")):
        for value in (True, False):
            before = log_lines(client)
            try:
                reply = client.call("preferences_set", values={key: value})
            except McpError as e:
                reply = {"error": str(e)}
            # What the write started is read once the app is idle, and a sync it started would be logged by then.
            shared.wait(client, "idle")
            logged = new_lines(before, log_lines(client))
            check(f"preferences_set {key} {value} writes it", [c["key"] for c in reply.get("changed", [])] == [key], reply)
            read = items_by_key(client, keys=[key])[key]["value"]
            control = dialog_controls(client, "online", key).get(key)
            check(f"and preferences_get and the dialog read {key} {value}",
                  read is value and control is not None and ("[checked]" in control[2]) is value, (read, control))
            check(f"and writing {key} {value} starts no preset sync", not any("start_sync_service" in line for line in logged),
                  [line for line in logged if "sync" in line][:5])
            if effect and value:
                check(f"and {key} on runs its control's effect", any(effect in line for line in logged), logged[-10:])


if __name__ == "__main__":
    sys.exit(main())
