"""Checks plugin_list, plugin_install_local, plugin_enable, plugin_reload, plugin_uninstall and plugin_refresh, that a
person's pick of Refresh Plugins runs a refresh of its own, and that the Plugins window shows the rows plugin_list gives.
Its design is the test_plugin_commands entry of tools/mcp/test-entries/commands.md.

It launches OrcaSlicer itself on a fresh data folder, signed in to nothing, and works only on the fixture plugins of
tests/plugins, which it installs itself: mcp_tools_fixture from a copy of its .py file outside the plugins folder, and
then, once uninstalled, side-loaded again; mcp_web_fixture side-loaded, its folder copied into the plugins folder, for
plugin_refresh to find. Nothing is installed from the network, and no plugin that came from the cloud is touched. The
launch is stopped on every path out, and the data folder removed unless it was given.
Run it as python tools/mcp/test_plugin_commands.py [--bin PATH] [--datadir PATH].
"""
import json
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import Checks, launched, reached, refusal as error_of, speed_dial, until, web_view  # noqa: E402
from orca_mcp import McpError  # noqa: E402

TOKEN = "plugin-commands-test-token"
PLUGINS = os.path.join(HERE, "tests", "plugins")
INSTALLED = "MCP Tools Fixture"  # mcp_tools_fixture's name, from its manifest
CAPABILITY = "MCP tools fixture call"  # one of its capabilities
SIDE_LOADED = "MCP Web Fixture"  # mcp_web_fixture's name
REFRESHED = "Plugins refreshed."  # the notification a refresh ends with (GUI_App::refresh_plugins())
RESCAN = "Rescanning plugins..."  # the log line of each rescan of the plugins folder
# The test's named checks; check() records one and prints it.
t = Checks()
check = t.check


def answered(orca, reply):
    """`reply`, followed to its end; a permission dialog the call opened is answered Yes, as the person who called it
    allows the plugin what it asks."""
    while isinstance(reply, dict) and reply.get("status") == "blocked_by_dialog":
        titles = [d.get("title", "") for d in reply.get("dialogs", [])]
        print("  answering the dialog the call opened:", titles, flush=True)
        orca.call("dialog_answer", button="yes")
        reply = orca.call("app_wait", **{"for": "job", "job": reply["job"], "timeout_s": 45})
    return orca.await_job(reply)


def call(orca, tool, **args):
    return answered(orca, orca.call(tool, **args))


def refusal(orca, tool, **args):
    """The text and details of the error `tool` replies, or None when it runs."""
    try:
        call(orca, tool, **args)
    except McpError as e:
        return error_of(e)
    return None


def rows(orca):
    return orca.call("plugin_list")["plugins"]


def row_named(orca, name):
    return next((row for row in rows(orca) if row["name"] == name), None)


def window_rows(orca):
    """The Plugins window's rows: each plugin's key and whether its switch is on, read from its page."""
    code = ("JSON.stringify(Array.from(document.querySelectorAll('.plugin-row')).map(function (r) {"
            " var box = r.querySelector('input.plugin-checkbox-input:not(.capability-checkbox-input)');"
            " return {key: r.dataset.pluginKey, enabled: !!(box && box.checked)}; }))")
    view = web_view(orca, "PluginsDialog/index.html")
    if view is None:
        return None
    reply = orca.call("webview_run_javascript", code=code, target=view)
    result = reply.get("result")
    return json.loads(result) if isinstance(result, str) else result


def run(orca, datadir, scratch):
    fields = {"key", "name", "version", "installed", "enabled", "status", "source", "capabilities", "error"}
    before = rows(orca)
    check("no fixture plugin is installed yet", all(r["name"] not in (INSTALLED, SIDE_LOADED) for r in before), before)

    # --- plugin_install_local, and again with and without overwrite ---
    package = os.path.join(scratch, "mcp_tools_fixture.py")
    shutil.copy(os.path.join(PLUGINS, "mcp_tools_fixture", "mcp_tools_fixture.py"), package)
    installed = call(orca, "plugin_install_local", path=package)
    check("plugin_install_local installs the fixture", installed.get("level") == "success", installed)
    row = row_named(orca, INSTALLED)
    check("plugin_list finds it, installed", row is not None and row["installed"] and row["source"] == "local", row)
    # Read on the fixture's row, since a fresh folder lists no plugin before the install; the rows listed then, if any,
    # are held to the same fields.
    check("plugin_list lists the plugins with their fields", row is not None and fields <= set(row)
          and isinstance(before, list) and all(fields <= set(r) for r in before), (row, before))
    if row is None:
        return
    key = row["key"]

    again = refusal(orca, "plugin_install_local", path=package)
    check("installing it again is refused with the question the dialog asks", again is not None and
          "is already installed" in again[0] and again[1].get("code") == "plugin_installed", again)
    replaced = orca.call("plugin_install_local", path=package, overwrite=True)
    check("with overwrite true it is replaced, and nothing asks", replaced.get("status") not in ("blocked_by_dialog", "running") and
          replaced.get("level") == "success", replaced)
    check("and no dialog is open", orca.call("app_state").get("dialogs", []) == [], orca.call("app_state").get("dialogs"))

    # --- plugin_enable: the plugin and one of its capabilities, each read back ---
    on = call(orca, "plugin_enable", plugin=key, enabled=True)
    check("plugin_enable turns it on, the call waiting for its load", on.get("plugin", {}).get("enabled") is True and
          on["plugin"]["status"] == "Activated", on)
    check("plugin_list reads it on, with its capabilities", row_named(orca, INSTALLED)["enabled"] and
          CAPABILITY in [c["name"] for c in row_named(orca, INSTALLED)["capabilities"]], row_named(orca, INSTALLED))
    off = call(orca, "plugin_enable", plugin=key, enabled=False)
    check("plugin_enable turns it off", off.get("plugin", {}).get("enabled") is False and off.get("changed") is True, off)
    check("plugin_list reads it off, with no capabilities", not row_named(orca, INSTALLED)["enabled"] and
          row_named(orca, INSTALLED)["capabilities"] == [], row_named(orca, INSTALLED))
    capability_off = refusal(orca, "plugin_enable", plugin=key, capability=CAPABILITY, enabled=False)
    check("a capability of a plugin that is off is refused", capability_off is not None and
          capability_off[1].get("code") == "plugin_not_enabled", capability_off)
    call(orca, "plugin_enable", plugin=INSTALLED, enabled=True)
    check("plugin_enable names a plugin by its name too", row_named(orca, INSTALLED)["enabled"], row_named(orca, INSTALLED))
    cap_off = call(orca, "plugin_enable", plugin=key, capability=CAPABILITY, enabled=False)
    capability = lambda: next((c for c in row_named(orca, INSTALLED)["capabilities"] if c["name"] == CAPABILITY), {})
    check("plugin_enable turns a capability off", cap_off.get("changed") is True and capability().get("enabled") is False,
          (cap_off, capability()))
    cap_on = call(orca, "plugin_enable", plugin=key, capability=CAPABILITY, enabled=True)
    check("and on again", cap_on.get("changed") is True and capability().get("enabled") is True, (cap_on, capability()))
    empty = refusal(orca, "plugin_enable", plugin=key, capability="", enabled=False)
    check("an empty capability is refused", empty is not None and empty[1].get("code") == "empty_capability", empty)
    check("and the plugin stays on, with the capability", row_named(orca, INSTALLED)["enabled"] is True and
          capability().get("enabled") is True, row_named(orca, INSTALLED))
    unknown = refusal(orca, "plugin_enable", plugin=key, capability="No such capability", enabled=True)
    check("a capability it lacks is refused, naming its capabilities", unknown is not None and
          unknown[1].get("code") == "no_such_capability" and CAPABILITY in unknown[1].get("capabilities", []), unknown)
    nobody = refusal(orca, "plugin_enable", plugin="mcp_tools_fixtur", enabled=True)
    check("a plugin no key or name names is refused with the closest keys", nobody is not None and
          nobody[1].get("code") == "no_such_plugin" and key in nobody[1].get("similar", []), nobody)

    # --- plugin_reload ---
    reloaded = call(orca, "plugin_reload", plugin=key)
    check("plugin_reload loads it again, and it stays on", reloaded.get("reloaded") == key and reloaded.get("reinstalled") is False and
          reloaded.get("plugin", {}).get("enabled") is True, reloaded)
    cache = call(orca, "plugin_reload", plugin=key, clear_cache=True)
    check("and with clear_cache", cache.get("plugin", {}).get("enabled") is True, cache)

    # --- plugin_uninstall of the plugin while it runs ---
    folder = os.path.join(datadir, "orca_plugins")
    files = lambda: [os.path.join(r, f) for r, _, fs in os.walk(folder) for f in fs if f == "mcp_tools_fixture.py"]
    check("its file is in the plugins folder", files() != [], folder)
    # On this machine the removal of a single-file plugin just loaded now and then fails with "The request is not
    # supported" from the file system (delete_plugin_root(), which the row's Delete runs too); the error then names how
    # the plugin is left, and a later try, as a person's, removes it. Only that failure is tried again, until a try
    # removes it.
    removed, failed = {}, None
    try:
        removed = call(orca, "plugin_uninstall", plugin=key)
    except McpError as e:
        failed = error_of(e)
    if failed is not None and "Failed to delete plugin folder" in failed[0]:
        check("a removal that fails says how the plugin is left", "It is installed" in failed[0] and
              failed[1].get("code") == "not_uninstalled" and (failed[1].get("plugin") or {}).get("key") == key, failed)
        print("  plugin_uninstall failed to delete the plugin folder once; trying again", flush=True)

        def uninstalled():
            try:
                return call(orca, "plugin_uninstall", plugin=key)
            except McpError as e:
                if "Failed to delete plugin folder" in str(e):
                    return None
                raise

        try:
            removed = until(uninstalled, "plugin_uninstall removes the plugin's folder", interval_s=1)
        except (McpError, TimeoutError) as e:
            check("plugin_uninstall removes it on a later try", False, str(e))
    elif failed is not None:
        check("plugin_uninstall fails only with the known folder deletion error", False, failed)
    check("plugin_uninstall removes it, which was on", removed.get("uninstalled") == key and removed.get("unsubscribed") is False and
          "plugin" not in removed, removed)
    check("plugin_list no longer lists it", row_named(orca, INSTALLED) is None, rows(orca))
    check("and its file is gone", files() == [], files())
    gone = refusal(orca, "plugin_uninstall", plugin=key)
    check("uninstalling it again is refused: no plugin has that key", gone is not None and gone[1].get("code") == "no_such_plugin", gone)

    # --- Refresh Plugins picked in the Speed Dial as a person picks it (the pick of test_person_forms.py; action_run of
    # it is an automated call, which waits as plugin_refresh does): it returns at once and its refresh ends with
    # its "Plugins refreshed." notification. It goes before the calls below, as the notification manager shows that
    # text once for every refresh that ends with it. Each refresh rescans the plugins, which the log says
    # (PluginManager::rescan_plugins()). Two refreshes that overlap take turns: the unit case of Ops::RefreshRun shows
    # that, which needs two refreshes held at once.
    def log_lines():
        return orca.call("app_log", lines=2000).get("lines", [])

    def rescans_since(before):
        """The rescans the log has logged past the last line of `before`."""
        after = log_lines()
        start = after.index(before[-1]) + 1 if before and before[-1] in after else 0
        return sum(RESCAN in line for line in after[start:])

    def refreshed_notified():
        return any(n.get("text") == REFRESHED for n in orca.call("app_state").get("notifications", []))

    def newest_job():
        """The number of the newest job app_state lists."""
        return max([int(j["id"][1:]) for j in orca.call("app_state").get("jobs", []) if j.get("id", "").startswith("j")] or [0])

    check("no refresh has been notified yet", not refreshed_notified(), orca.call("app_state").get("notifications"))
    orca.call("view_set", tab="prepare")
    logged = log_lines()
    orca.call("keyboard_press", keys="space", target={"name": "OrcaSceneCanvas"})
    until(lambda: speed_dial(orca), "the Speed Dial shows")
    orca.call("widget_set_value", target={"web": "Search actions"}, value="Refresh Plugins", submit=False, timeout_s=30)
    since = newest_job()
    clicked = time.monotonic()
    picked = orca.call("widget_click", target={"web": "Refresh Plugins from Orcaslicer", "kind": "button"}, timeout_s=30)
    took = time.monotonic() - clicked
    # The pick runs the menu item's form in place (MenuItems::refresh_plugins()), which starts the refresh off the UI
    # thread and returns, its end told by the notification; the call's form, plugin_refresh, would run as a job and
    # wait for the refresh. So the pick starts no plugin_refresh job. The 30 s bound only catches a pick that hangs.
    refresh_jobs = [j for j in orca.call("app_state").get("jobs", []) if int(j["id"][1:]) > since and j.get("tool") == "plugin_refresh"]
    check("a person's pick of Refresh Plugins returns at once, starting the refresh in place, not as a job",
          picked.get("status") not in ("running", "blocked_by_dialog") and refresh_jobs == [] and took < 30,
          (picked.get("status"), refresh_jobs, took))
    check("and its refresh ends with its notification", reached(refreshed_notified, "the refresh's notification shows", 60),
          orca.call("app_state").get("notifications"))
    check("which came from a rescan of its own", rescans_since(logged) == 1, rescans_since(logged))

    # --- plugin_refresh finds a plugin side-loaded into the plugins folder, and replies once it has ---
    dest = os.path.join(folder, "mcp_web_fixture")
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(PLUGINS, "mcp_web_fixture", "mcp_web_fixture.py"), dest)
    check("plugin_list does not list the side-loaded plugin before a refresh", row_named(orca, SIDE_LOADED) is None, rows(orca))
    first = orca.call("plugin_refresh")
    waited = first.get("status") not in ("running", "blocked_by_dialog")
    refreshed = orca.await_job(first)
    check("plugin_refresh replies with the plugins once the refresh has ended", waited and
          SIDE_LOADED in [r["name"] for r in refreshed.get("plugins", [])], first)
    check("plugin_list lists it too", row_named(orca, SIDE_LOADED) is not None, rows(orca))

    # --- a second fixture side-loaded, and plugin_refresh's reply has it ---
    second = os.path.join(folder, "mcp_tools_fixture")
    os.makedirs(second, exist_ok=True)
    shutil.copy(os.path.join(PLUGINS, "mcp_tools_fixture", "mcp_tools_fixture.py"), second)
    check("plugin_list does not list the second side-loaded plugin before a refresh", row_named(orca, INSTALLED) is None, rows(orca))
    again = orca.call("plugin_refresh")
    waited = again.get("status") not in ("running", "blocked_by_dialog")
    refreshed = orca.await_job(again)
    check("plugin_refresh replies with the second plugin, from a rescan that began after it was asked", waited and
          INSTALLED in [r["name"] for r in refreshed.get("plugins", [])], again)

    # --- The Plugins window shows the rows plugin_list gives ---
    listed = {r["key"]: r["enabled"] for r in rows(orca)}
    orca.call("window_open", window="plugins")
    shown = None

    def read():
        nonlocal shown
        try:
            shown = window_rows(orca)
        except McpError as e:
            shown = str(e)
        return isinstance(shown, list) and len(shown) == len(listed)

    reached(read, "the Plugins window lists as many plugins as plugin_list")
    check("the Plugins window lists the same plugins, each switch as plugin_list says",
          isinstance(shown, list) and {r["key"]: r["enabled"] for r in shown} == listed, (shown, listed))


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    given = "--datadir" in extra
    root = os.path.abspath(extra[extra.index("--datadir") + 1]) if given else tempfile.mkdtemp(prefix="opc-")
    datadir = os.path.join(root, "d")
    scratch = os.path.join(root, "files")
    os.makedirs(scratch, exist_ok=True)
    try:
        with launched(datadir, binary, TOKEN) as orca:
            check("the app starts", orca is not None)
            if orca is not None:
                run(orca, datadir, scratch)
    finally:
        if not given:
            shutil.rmtree(root, ignore_errors=True)

    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
