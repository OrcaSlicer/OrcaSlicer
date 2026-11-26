"""Checks a plugin's calls into the MCP tools and the operations it offers, in the running app.
Its design is the test_plugin_tools entry of tools/mcp/test-entries/commands.md.

Launches OrcaSlicer on a fresh data folder with the Design tab on (--cad) and
tests/plugins/mcp_tools_fixture side-loaded with the tools permission, tests/plugins/mcp_tools_ungranted without it,
tests/plugins/mcp_tools_declaring, which asks for it as it loads, and tests/plugins/mcp_tools_broken, whose load
fails after declaring an operation: over MCP, then once without MCP,
running a replay with --run, so the plugins' calls go to the runtime started without its endpoint. The first launch
also checks that the plugin is sent the NewProject event when project_new or its Speed Dial action starts a project,
answers the Plugin permission request prompt a call without the tools permission opens, No and then Yes, answers the
declaring plugin's load prompt, No and then Yes, turns the fixture's OrcaSlicer tools switch off and on again in the
Plugins dialog, with its Full access switch turned on and off between them, and ends by unticking the fixture there, as a person unloads it. Without MCP, the replay's own
auto-answer rule says Yes to the prompt the ungranted plugin's call opens.
Run it as python tools/mcp/test_plugin_tools.py [--bin PATH].
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import Checks, dialogs, free_port, harness, reached, until, web_view  # noqa: E402
from orca_mcp import McpError, OrcaMcp, app_env, find_binary  # noqa: E402

PLUGIN = "mcp_tools_fixture"
CAPABILITIES = ["MCP tools fixture call", "MCP tools fixture thread", "MCP tools fixture rule", "MCP tools fixture export",
                "MCP tools fixture handle", "MCP tools fixture record"]
BROKEN = "mcp_tools_broken"
UNGRANTED = "mcp_tools_ungranted"
UNGRANTED_CAPABILITY = "MCP tools ungranted call"
UNGRANTED_ACTION = f"plugin_script_action:{UNGRANTED_CAPABILITY}:{UNGRANTED}"
DECLARING = "mcp_tools_declaring"  # asks for the tools as it loads
DECLARING_CAPABILITY = "MCP tools declaring call"
# Each side-loaded plugin's name and capabilities, as its install state lists them.
SIDE_LOADED = {PLUGIN: ("MCP Tools Fixture", CAPABILITIES), UNGRANTED: ("MCP Tools Ungranted", [UNGRANTED_CAPABILITY]),
               DECLARING: ("MCP Tools Declaring", [DECLARING_CAPABILITY]),
               BROKEN: ("MCP Tools Broken Fixture", ["MCP tools broken fixture"])}
REFUSED = "not allowed to call OrcaSlicer's tools"  # the start of the refusal a plugin without the tools gets
# The titles of the prompt a call without the tools permission opens, and of the one a load's declaration opens.
RUNTIME_PROMPT = "Plugin permission request"
LOAD_PROMPT = "Plugin permissions"
TOOLS_NAMED = "OrcaSlicer's tools"  # what both prompts' message says the plugin asks to call
# The fixture's OrcaSlicer tools switch on the Plugins dialog's Permissions tab, and the label a click lands on, which
# has the class of every yes/no permission's switch.
TOOLS_SWITCH = "permission-tools-toggle"
TOOLS_SWITCH_LABEL = f"label.permission-thread-toggle:has(#{TOOLS_SWITCH})"
# The Permissions tab's Full access switch, by the label a click lands on, and the title of the confirmation turning it
# on opens.
FULL_ACCESS_LABEL = "label.permission-placeholder:has(#permissionFullAccess)"
FULL_ACCESS_PROMPT = "Full access"
# A script that finds the fixture's progress dialog among the loaded modules and moves its gauge.
GRAB_HANDLE = """
import sys
handle = next(m.HANDLE for m in list(sys.modules.values()) if getattr(m, "HANDLE", None) is not None)
result = handle.update(5)
"""
RULE = "MCP tools fixture never shown"  # the match of the auto-answer rule the fixture's "rule" script sets
UNRECORDED = {"ok": None, "error": "the script recorded nothing"}  # the outcome of a script that recorded none
TOKEN = "plugin-tools-test-token"
# The test's named checks; check() records one and prints it.
t = Checks()
check = t.check


def write_state(datadir, key, enabled=True, permissions=None):
    """Writes the install state of the side-loaded plugin `key`: `enabled`, each of its capabilities on, and
    `permissions` when given (none otherwise)."""
    name, capabilities = SIDE_LOADED[key]
    state = {"installed_from": "local", "installed_version": "1.0.0", "plugin_name": name, "enabled": enabled,
             "capabilities": [{capability: True} for capability in capabilities]}
    if permissions is not None:
        state["permissions"] = permissions
    with open(os.path.join(datadir, "orca_plugins", key, ".install_state.json"), "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)


def side_load(datadir, key, enabled=True, permissions=None):
    """Side-loads tests/plugins/<key> with the install state write_state() writes. Returns the plugin's storage folder,
    where orca.host.plugin.storage() puts its files, and its scripts their outcomes."""
    dest = os.path.join(datadir, "orca_plugins", key)
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(HERE, "tests", "plugins", key, key + ".py"), dest)
    write_state(datadir, key, enabled, permissions)
    return os.path.join(datadir, "orca_plugins", "plugin_data", key)


def install(datadir):
    """Side-loads the fixture enabled, with its threads and the tools granted; returns its storage folder."""
    return side_load(datadir, PLUGIN, permissions={"threading": ["thread"], "tools": True})


def read_json(path):
    """What the JSON file at `path` holds, or None while it is missing or still being written."""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def stored_permissions(datadir, key):
    """The permissions the plugin `key`'s install state holds ({} while it has none), or None while it does not read."""
    state = read_json(os.path.join(datadir, "orca_plugins", key, ".install_state.json"))
    return None if state is None else state.get("permissions", {})


def tools_grant(datadir, key=PLUGIN):
    """The tools permission the plugin `key`'s install state holds: True, False, or None while it holds none or does not
    read."""
    return (stored_permissions(datadir, key) or {}).get("tools")


def lines_of(path):
    """How many lines a file has, 0 while it does not exist."""
    try:
        with open(path, encoding="utf-8") as f:
            return len(f.readlines())
    except OSError:
        return 0


def canvas_height(orca):
    """The height of the 3D view as widget_list gives it (its name=OrcaSceneCanvas line), or None."""
    _, texts = orca.call_with_text("widget_list", include_menus=False, include_imgui=False, include_web=False)
    for line in "\n".join(texts).splitlines():
        if 'name=OrcaSceneCanvas' in line:
            return int(line.rsplit("x", 1)[1])
    return None


def run_plugin_script(orca, plugin_dir, name, capability, plugin=PLUGIN):
    """Runs one of a plugin's scripts and returns what it recorded in its storage folder (out_<name>.json)."""
    out = os.path.join(plugin_dir, f"out_{name}.json")
    if os.path.exists(out):
        os.remove(out)
    orca.call("action_run", id=f"plugin_script_action:{capability}:{plugin}")
    try:
        return until(lambda: read_json(out), f"the {name} script records its outcome", timeout_s=60)
    except TimeoutError:
        return UNRECORDED


def tools_switch(orca, view):
    """The OrcaSlicer tools switch on the Permissions tab of `view`, the Plugins dialog's page: {checked, disabled},
    or None."""
    if view is None:
        return None
    code = (f"JSON.stringify((function () {{ var b = document.getElementById('{TOOLS_SWITCH}');"
            " return b ? {checked: b.checked, disabled: b.disabled} : null; })())")
    result = orca.call("webview_run_javascript", code=code, target=view).get("result")
    return json.loads(result) if isinstance(result, str) else result


def click_in_plugins(orca, css):
    """Clicks the element `css` names on the Plugins dialog's page; the reply, which a dialog it opens may hold."""
    return orca.call_raw("widget_click", {"target": {"css": css, "within": "Plugins"}})


def prompts_open(orca):
    """Whether a permission prompt is open (the Plugins dialog itself may be listed)."""
    return any(d["title"] in (RUNTIME_PROMPT, LOAD_PROMPT) for d in dialogs(orca))


def prompt_messages(orca, title):
    """The messages of the open dialogs titled `title`."""
    return [d.get("message") or "" for d in dialogs(orca) if d["title"] == title]


def await_prompt(orca, title):
    """The messages of the dialogs titled `title` once one is open, waited for up to 30 s ([] when none opens). The
    app_wait for a dialog returns at once while another dialog, such as the Plugins dialog, is open, so the title is then
    waited for in app_state.dialogs."""
    orca.call_raw("app_wait", {"for": "dialog", "timeout_s": 30})
    reached(lambda: prompt_messages(orca, title), f"a dialog titled {title} opens")
    return prompt_messages(orca, title)


def asks_once(messages):
    """Whether `messages`, await_prompt()'s, are one prompt's, and it names OrcaSlicer's tools."""
    return len(messages) == 1 and TOOLS_NAMED in messages[0]


def answer(orca, title, button):
    """Presses `button` on the dialog titled `title`, which must be the topmost; whether the press was not refused."""
    return not orca.call_raw("dialog_answer", {"button": button, "dialog": {"title": title}}).get("isError")


def run_and_prompt(orca, plugin_dir, name, capability, plugin):
    """Runs a plugin's script whose call is refused for the tools permission, and waits for both what it records and the
    Plugin permission request prompt the refused call queues: the queue may show the prompt before or after the script
    writes its outcome. Returns the outcome and await_prompt()'s messages."""
    outcome = run_plugin_script(orca, plugin_dir, name, capability, plugin)
    return outcome, await_prompt(orca, RUNTIME_PROMPT)


def refused_ungranted(outcome):
    """Whether the ungranted plugin's script recorded what a call without the tools permission gets: the catalog read,
    and the call refused as a ToolError whose text names the permission and how a plugin declares it."""
    return (outcome.get("listed") is True and outcome.get("described") == "app_state" and outcome.get("ok") is False and
            outcome.get("error_type") == "ToolError" and outcome.get("code") == "tools_not_allowed" and
            REFUSED in str(outcome.get("call")) and "request_permissions(tools=True)" in str(outcome.get("call")))


def plugin_row(orca, key):
    """The plugin `key`'s row in plugin_list, or None."""
    return next((r for r in orca.call("plugin_list")["plugins"] if r["key"] == key), None)


def load_declaring(orca, button):
    """Switches the declaring plugin on and, once its load's prompt is up, answers it with `button`. Returns the prompt's
    messages (await_prompt()'s), whether the answer was not refused, the load's end (its job's result, or the McpError
    that ended the load or the 45 s wait for it), and whether no permission prompt is open after it."""
    try:
        load = orca.call("plugin_enable", plugin=DECLARING, enabled=True)
    except McpError as e:
        load = e
    asked = await_prompt(orca, LOAD_PROMPT)
    answered = answer(orca, LOAD_PROMPT, button)
    try:
        load = orca.await_job(load, timeout=45)  # an McpError is no job envelope, so it comes back as it is
    except McpError as e:
        load = e
    settled = reached(lambda: not prompts_open(orca), "no permission prompt is open")
    return asked, answered, load, settled


def load_ended(load):
    """Whether load_declaring()'s end of the load is the load's end without error: no McpError, and no reply that the
    load is still running or held by a dialog (one without a job id, which await_job() hands back as it is)."""
    return not isinstance(load, McpError) and load.get("status") not in ("running", "blocked_by_dialog")


def load_failed(orca, load):
    """Whether load_declaring()'s load ended in failure: its job's result is the error of a plugin that did not load, or
    plugin_list shows the plugin neither loaded nor still loading."""
    row = plugin_row(orca, DECLARING)
    unloaded = row is not None and not row.get("enabled") and row.get("status") != "Loading"
    return (isinstance(load, McpError) and "not_loaded" in str(load)) or unloaded


def recording_written(plugin_dir):
    """Whether the fixture's recording left a file: fixture.mkv, or the MP4 it is encoded to when ffmpeg is there."""
    return any(os.path.isfile(p) and os.path.getsize(p) > 0
               for p in (os.path.join(plugin_dir, "fixture.mkv"), os.path.join(plugin_dir, "fixture.mp4")))


def launch(env, datadir, extra):
    port = free_port()
    r = harness(env, "launch", "--datadir", datadir, "--port", str(port), *extra)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise SystemExit(1)
    return OrcaMcp(port=port, token=TOKEN)


def main():
    extra = sys.argv[1:]
    binary = find_binary(extra[extra.index("--bin") + 1] if "--bin" in extra else None)
    datadir = tempfile.mkdtemp(prefix="orca-plugin-tools-")
    # The CA bundle too, so the certificate prompt never comes up: the --run launch below starts the
    # binary itself, and with no endpoint there nothing could answer that prompt.
    env = app_env(dict(os.environ, ORCA_MCP_SESSION=os.path.join(datadir, "session.json"), ORCA_MCP_TOKEN=TOKEN), datadir)
    try:
        # --- Over MCP: the plugin's operation and its tool calls, beside a plugin whose load fails. ---
        plugin_dir = install(datadir)
        ungranted_dir = side_load(datadir, UNGRANTED)
        declaring_dir = side_load(datadir, DECLARING, enabled=False)  # loaded only when the test switches it on
        side_load(datadir, BROKEN)
        # The Design tab on, so that tool_list lists its commands.
        orca = launch(env, datadir, ["--cad", *extra])
        operation_name = f"plugin.{PLUGIN}.measure"
        listed_plugins = lambda: [i["name"] for i in orca.call("tool_list", group="plugins")["items"]]
        check("tool_list lists the plugin's operation", operation_name in listed_plugins(), listed_plugins())
        scripts = {f"plugin_script_action:{capability}:{PLUGIN}" for capability in CAPABILITIES}
        fixture_actions = lambda: {a["id"] for a in orca.call("action_list", section="MCP Tools Fixture", limit=200)["items"]}
        check("action_list lists the fixture's six scripts as its Speed Dial actions",
              reached(lambda: fixture_actions() == scripts, "the fixture's script actions are listed"), sorted(fixture_actions()))
        described = orca.call_raw("tool_describe", {"tool": operation_name})
        operation = {} if described.get("isError") else json.loads(described["content"][0]["text"])
        check("tool_describe describes the plugin's operation, its parameter a among it",
              [p.get("name") for p in operation.get("parameters", [])] == ["a"] and operation.get("listed") is False and
              operation.get("access", {}).get("open_world") is True, described)
        check("the operation is in the Plugins group, its description saying a plugin offers it",
              operation.get("group") == "Plugins" and operation.get("description", "").startswith("Offered by a plugin."), operation)
        called = orca.call_raw("tool_call", {"tool": f"plugin.{PLUGIN}.measure", "args": {"a": 2}})
        check("tool_call runs the plugin's operation", not called.get("isError") and
              json.loads(called["content"][0]["text"]) == {"twice": 4}, called)
        listed = [t["name"] for t in orca.request("tools/list")["tools"]]
        check("the operation is not in tools/list", f"plugin.{PLUGIN}.measure" not in listed)
        called = run_plugin_script(orca, plugin_dir, "call", CAPABILITIES[0])
        check("a plugin calls a tool from its Script worker", called == {"ok": True, "result": True}, called)
        threaded = run_plugin_script(orca, plugin_dir, "thread", CAPABILITIES[1])
        check("a thread its Script worker starts calls a tool too", threaded == {"ok": True, "result": True}, threaded)
        # The agent's own dialog_auto_answer, which clears only the rules MCP clients set: the journal keeps this call,
        # the control for the plugin's call of the same tool, which it must not keep.
        orca.call("dialog_auto_answer", rules=[])
        rule = run_plugin_script(orca, plugin_dir, "rule", CAPABILITIES[2])
        check("an auto-answer rule a plugin sets is owned by the plugin", rule == {"ok": True, "result": "plugin"}, rule)
        exported = run_plugin_script(orca, plugin_dir, "export", CAPABILITIES[3])
        check("a plugin is refused replay_export", exported.get("ok") is False and "agent's session" in exported.get("error", ""),
              exported)
        # --- A plugin without the tools permission: it reads the catalog and offers operations, but a call is refused and
        # asks the person, in the Plugin permission request prompt. A No is not remembered; a Yes lets the next call through.
        ungranted, asked = run_and_prompt(orca, ungranted_dir, "ungranted", UNGRANTED_CAPABILITY, UNGRANTED)
        check("a plugin without the tools permission reads the catalog but is refused a call, as a ToolError",
              refused_ungranted(ungranted), ungranted)
        check("and is asked in the Plugin permission request prompt, which names OrcaSlicer's tools", asks_once(asked),
              dialogs(orca))
        check("a No closes the prompt and stores no grant",
              answer(orca, RUNTIME_PROMPT, "no") and reached(lambda: not prompts_open(orca), "the prompt closes") and
              tools_grant(datadir, UNGRANTED) is not True, (stored_permissions(datadir, UNGRANTED), dialogs(orca)))
        ungranted, asked = run_and_prompt(orca, ungranted_dir, "ungranted", UNGRANTED_CAPABILITY, UNGRANTED)
        check("a No is not remembered: its next call is refused and asks again",
              refused_ungranted(ungranted) and asks_once(asked), (ungranted, dialogs(orca)))
        check("a Yes stores the grant",
              answer(orca, RUNTIME_PROMPT, "yes") and
              reached(lambda: tools_grant(datadir, UNGRANTED) is True, "the install state holds tools true"),
              (stored_permissions(datadir, UNGRANTED), dialogs(orca)))
        allowed = run_plugin_script(orca, ungranted_dir, "ungranted", UNGRANTED_CAPABILITY, UNGRANTED)
        check("which lets its next call through", allowed.get("ok") is True and allowed.get("call") is True, allowed)
        check("with no dialog open after it", not dialogs(orca), dialogs(orca))
        write_state(datadir, UNGRANTED)
        echoed = orca.call_raw("tool_call", {"tool": f"plugin.{UNGRANTED}.echo", "args": {"a": 1}})
        check("the operation it offers runs without the grant, asking nothing", not echoed.get("isError") and
              json.loads(echoed["content"][0]["text"]) == {"a": 1} and not prompts_open(orca), (echoed, dialogs(orca)))
        write_state(datadir, UNGRANTED, permissions={"full_access": True})
        full = run_plugin_script(orca, ungranted_dir, "ungranted", UNGRANTED_CAPABILITY, UNGRANTED)
        check("full access alone lets it call, asking nothing", full.get("ok") is True and full.get("call") is True and
              not prompts_open(orca), (full, dialogs(orca)))
        write_state(datadir, UNGRANTED)

        # --- The load prompt of a plugin that declares the tools, answered in its real dialog. Before the Plugins dialog
        # is first opened: it is modal, and plugin_enable is refused while a modal dialog is open. ---
        asked, answered, load, settled = load_declaring(orca, "no")
        check("a plugin that declares the tools asks for them in its Plugin permissions prompt", asks_once(asked),
              (asked, dialogs(orca)))
        check("a No fails its load and stores no grant",
              answered and settled and load_failed(orca, load) and tools_grant(datadir, DECLARING) is not True,
              (load, plugin_row(orca, DECLARING), stored_permissions(datadir, DECLARING), dialogs(orca)))
        orca.call("plugin_enable", plugin=DECLARING, enabled=False)
        asked, answered, load, settled = load_declaring(orca, "yes")
        check("switched on again, it asks again", asks_once(asked), (asked, dialogs(orca)))
        check("and a Yes loads it and stores the grant",
              answered and load_ended(load) and settled and tools_grant(datadir, DECLARING) is True,
              (load, stored_permissions(datadir, DECLARING), dialogs(orca)))
        allowed = run_plugin_script(orca, declaring_dir, "declaring", DECLARING_CAPABILITY, DECLARING)
        check("which lets the plugin call the tools, asking nothing",
              allowed.get("ok") is True and allowed.get("call") is True and not prompts_open(orca), (allowed, dialogs(orca)))
        # The menu items with no argument form, each with what it does as its manual's description says it.
        commands = {i["name"]: i for i in orca.call("tool_list")["items"]}
        for menu_item in ("settings_toggle_developer_mode", "script_toggle_panel"):
            manual = orca.call("tool_describe", tool=menu_item)
            check(f"tool_list lists {menu_item} with what it does", commands.get(menu_item, {}).get("does") == manual.get("description"),
                  (commands.get(menu_item), manual.get("description")))
        check("tool_describe describes a menu item's command, which takes no arguments",
              manual.get("listed") is False and manual.get("parameters") == [], manual)
        objects = orca.call("tool_list", group="objects")
        check("a group the tool list shows whole lists nothing, in any case, and says so",
              objects.get("items") == [] and "tools/list" in objects.get("note", ""), objects)
        mesh = [i["name"] for i in orca.call("script_list_api", within="orca.host.TriangleMesh", limit=200)["items"]]
        check("script_list_api lists a class's members", "orca.host.TriangleMesh.face_normals" in mesh, mesh)
        normals = orca.call("script_describe_api", name="orca.host.TriangleMesh.face_normals")
        check("script_describe_api gives an entry's documentation", bool(normals.get("doc")), normals)
        unknown = orca.call_raw("tool_list", {"group": "commandz"})
        check("a group no command has is refused, naming the groups there are",
              unknown.get("isError") and '"code":"invalid_argument"' in unknown["content"][0]["text"] and
              "Design" in unknown["content"][0]["text"], unknown)
        # It shows the Run Script panel, which docks under the 3D view and so makes it shorter; the second call hides it
        # again. The panel is kept from automation, so widget_list reads the 3D view instead.
        orca.call("view_set", tab="prepare")
        heights = [canvas_height(orca)]

        def height_comes(holds):
            """Whether the 3D view's height, read until it does, comes to pass `holds`; heights[-1] is the last read."""
            def read():
                heights.append(canvas_height(orca))
                return heights[-1] is not None and holds(heights[-1])
            return reached(read, "the 3D view's height changes")

        shown_before = heights[-1]
        ran = orca.call_raw("tool_call", {"tool": "script_toggle_panel"})
        shorter = shown_before is not None and height_comes(lambda h: h < shown_before)
        shown_after = heights[-1]
        check("tool_call runs a menu item's command", not ran.get("isError") and shorter, (ran, shown_before, shown_after))
        orca.call_raw("tool_call", {"tool": "script_toggle_panel"})
        taller = shown_after is not None and height_comes(lambda h: h > shown_after)
        check("and a second call hides it", taller, (shown_after, heights[-1]))
        design_tab = orca.call("app_state").get("design_tab")
        check("the Design tab is on (launch --cad)", design_tab is True, design_tab)
        design = {i["name"]: i for i in orca.call("tool_list", group="design")["items"]}
        check("tool_list lists the Design commands under their headings", design.get("solid_extrude", {}).get("heading") == "Solid",
              design.get("solid_extrude"))
        journal = orca.call("replay_export")["replay"]["steps"]
        answers = [s for s in journal if s["tool"] == "dialog_auto_answer"]
        check("a plugin's calls are not in the agent's journal",
              len(answers) == 1 and answers[0].get("args", {}).get("rules") == [], answers)
        broken_row = plugin_row(orca, BROKEN)
        check("the plugin whose load failed is listed with its error",
              broken_row is not None and "this fixture's load fails here on purpose" in (broken_row.get("error") or ""), broken_row)
        broken = orca.call_raw("tool_describe", {"tool": f"plugin.{BROKEN}.measure"})
        check("and offers none of its operations", broken.get("isError") is True and
              broken["content"][0]["text"].startswith(f"Unknown tool: plugin.{BROKEN}.measure") and
              '"code":"invalid_argument"' in broken["content"][0]["text"], broken)
        # A project started by project_new or by its Speed Dial action tells plugins, as File > New Project does.
        started = lambda: lines_of(os.path.join(plugin_dir, "new_projects.txt"))
        before = started()
        orca.call("project_new", discard_changes=True)
        check("project_new sends plugins the NewProject event",
              reached(lambda: started() == before + 1, "the plugin records a NewProject event"), started())
        orca.call("action_run", id="orca_command:new_project:orca")
        check("and so does its action, new_project",
              reached(lambda: started() == before + 2, "the plugin records a second NewProject event"), started())
        held = run_plugin_script(orca, plugin_dir, "handle", CAPABILITIES[4])
        # The script gives create_progress_dialog this title.
        titles = [w.get("title") for w in orca.call("window_list").get("items", [])]
        check("the plugin opens a progress dialog and keeps its handle", held.get("ok") is True and "MCP tools fixture" in titles,
              (held, titles))
        grab = orca.await_job(orca.call("script_run", code=GRAB_HANDLE))
        check("a script that finds a plugin's handle can use its methods", grab.get("result") is True, grab)

        # --- The fixture's OrcaSlicer tools switch, turned off and on again on the Plugins dialog's Permissions tab. ---
        rules = lambda: json.dumps(orca.call("app_state").get("auto_answer_rules"))
        check("the fixture's auto-answer rule is in force while it holds the tools", RULE in rules(), rules())
        orca.call("action_run", id="orca_command:open_plugins:orca")
        # The row's name, which the page lists as an element (the row itself is not one); a click on it selects the row.
        row = f'div.plugin-row[data-plugin-key="{PLUGIN}"] span.plugin-name-text'
        check("the Plugins dialog selects the fixture's row",
              reached(lambda: not click_in_plugins(orca, row).get("isError"), "the fixture's row is clicked"))
        shown = click_in_plugins(orca, "#permissionsTab")
        group = click_in_plugins(orca, 'details.permission-group[data-group="process"] > summary')
        check("and shows its Permissions tab with the Execution group open", not shown.get("isError") and not group.get("isError"),
              (shown, group))
        page = web_view(orca, "PluginsDialog/index.html")
        on = {"checked": True, "disabled": False}
        off = {"checked": False, "disabled": False}
        check("the OrcaSlicer tools switch starts on, as the install state does",
              reached(lambda: tools_switch(orca, page) == on, "the tools switch reads on") and tools_grant(datadir) is True,
              (tools_switch(orca, page), tools_grant(datadir)))
        clicked = click_in_plugins(orca, TOOLS_SWITCH_LABEL)
        check("turning it off asks nothing and stores false",
              not clicked.get("isError") and reached(lambda: tools_grant(datadir) is False, "the install state holds tools false") and
              reached(lambda: tools_switch(orca, page) == off, "the switch reads off") and not prompts_open(orca),
              (clicked, tools_grant(datadir), tools_switch(orca, page), dialogs(orca)))
        check("and ends the fixture's auto-answer rule", reached(lambda: RULE not in rules(), "the fixture's rule ends"), rules())
        refused, asked = run_and_prompt(orca, plugin_dir, "call", CAPABILITIES[0], PLUGIN)
        check("the fixture's next call is refused", refused.get("ok") is False and REFUSED in refused.get("error", ""), refused)
        check("and asks in the Plugin permission request prompt, over the Plugins dialog", asks_once(asked), dialogs(orca))
        check("whose No stores nothing",
              answer(orca, RUNTIME_PROMPT, "no") and reached(lambda: not prompts_open(orca), "the prompt closes") and
              tools_grant(datadir) is False, (tools_grant(datadir), dialogs(orca)))
        # Full access alone, with the switch off: it grants the tools, and turning it off withdraws them as the switch does.
        full_access = lambda: (stored_permissions(datadir, PLUGIN) or {}).get("full_access")
        confirming = lambda: any(d["title"] == FULL_ACCESS_PROMPT for d in dialogs(orca))
        clicked = click_in_plugins(orca, FULL_ACCESS_LABEL)
        confirmation = await_prompt(orca, FULL_ACCESS_PROMPT)
        check("turning full access on asks in its confirmation, which names the tools",
              not clicked.get("isError") and len(confirmation) == 1 and
              "OrcaSlicer tools" in " ".join(confirmation[0].split()),  # the dialog wraps its message at any space
              (clicked, confirmation))
        check("whose Yes stores full access, the tools grant still false",
              answer(orca, FULL_ACCESS_PROMPT, "yes") and reached(lambda: not confirming(), "the confirmation closes") and
              reached(lambda: full_access() is True, "the install state holds full access") and tools_grant(datadir) is False,
              (stored_permissions(datadir, PLUGIN), dialogs(orca)))
        rule = run_plugin_script(orca, plugin_dir, "rule", CAPABILITIES[2])
        check("which lets the fixture call the tools again and set its rule, asking nothing",
              rule == {"ok": True, "result": "plugin"} and RULE in rules() and not prompts_open(orca), (rule, rules(), dialogs(orca)))
        clicked = click_in_plugins(orca, FULL_ACCESS_LABEL)
        check("turning full access off asks nothing and stores false, the tools switch usable again",
              not clicked.get("isError") and reached(lambda: full_access() is False, "the install state holds full access false") and
              reached(lambda: tools_switch(orca, page) == off, "the switch reads off") and not confirming() and
              not prompts_open(orca), (clicked, stored_permissions(datadir, PLUGIN), tools_switch(orca, page), dialogs(orca)))
        check("and, with the switch off too, ends the fixture's rule", reached(lambda: RULE not in rules(), "the fixture's rule ends"),
              rules())
        refused, asked = run_and_prompt(orca, plugin_dir, "call", CAPABILITIES[0], PLUGIN)
        check("and refuses its next call, asking again",
              refused.get("ok") is False and REFUSED in refused.get("error", "") and asks_once(asked), (refused, dialogs(orca)))
        check("whose No closes the prompt",
              answer(orca, RUNTIME_PROMPT, "no") and reached(lambda: not prompts_open(orca), "the prompt closes"), dialogs(orca))
        clicked = click_in_plugins(orca, TOOLS_SWITCH_LABEL)
        check("turning the switch on asks nothing and stores true",
              not clicked.get("isError") and reached(lambda: tools_grant(datadir) is True, "the install state holds tools true") and
              reached(lambda: tools_switch(orca, page) == on, "the switch reads on") and not prompts_open(orca),
              (clicked, tools_grant(datadir), tools_switch(orca, page), dialogs(orca)))
        allowed = run_plugin_script(orca, plugin_dir, "call", CAPABILITIES[0])
        check("the fixture calls the tools again, asking nothing", allowed == {"ok": True, "result": True} and
              not prompts_open(orca), (allowed, dialogs(orca)))
        for name in ("fixture.mkv", "fixture.mp4"):
            if os.path.exists(os.path.join(plugin_dir, name)):
                os.remove(os.path.join(plugin_dir, name))
        recorded = run_plugin_script(orca, plugin_dir, "record", CAPABILITIES[5])
        check("and starts a recording under its scope opened again",
              recorded.get("ok") is True and (recorded.get("result") or {}).get("status") != "running" and
              reached(lambda: recording_written(plugin_dir), "the fixture's recording is written"), recorded)
        rule = run_plugin_script(orca, plugin_dir, "rule", CAPABILITIES[2])
        check("and sets its auto-answer rule again", rule == {"ok": True, "result": "plugin"}, rule)

        # --- The plugin unloads, as a person unticks it in the Plugins dialog. ---
        check("the auto-answer rule the plugin set lasts while it is loaded", RULE in rules(), rules())
        orca.call("action_run", id="orca_command:open_plugins:orca")
        untick = orca.call_raw("widget_click", {"target": {"css": f'label.plugin-checkbox:has(input[data-plugin-key="{PLUGIN}"])',
                                                       "within": "Plugins"}})
        check("the Plugins dialog unticks the plugin", not untick.get("isError"), untick)
        def withdrawn():
            gone = orca.call_raw("tool_describe", {"tool": operation_name})
            text = gone["content"][0]["text"] if gone.get("isError") else ""
            return (operation_name not in listed_plugins() and text.startswith(f"Unknown tool: {operation_name}") and
                    '"code":"invalid_argument"' in text)
        check("an unloaded plugin's operation is withdrawn", reached(withdrawn, "the plugin's operation is withdrawn"))
        check("and the auto-answer rule it set ends with it",
              reached(lambda: RULE not in rules(), "the plugin's auto-answer rule ends"), rules())
        # stop returns once the app has exited, so that the --run launch below is the only app on the folder.
        harness(env, "stop")

        # --- Without MCP: a launch replay runs the plugins' scripts, whose calls reach the runtime without its endpoint. The
        # ungranted plugin's refused call queues its prompt, which only the replay's own rule can answer, and the two waits
        # keep the replay running while the prompt opens and is answered; the rule ends with the replay. ---
        install(datadir)  # enabled again: unticking it saved it disabled
        write_state(datadir, UNGRANTED)
        steps = os.path.join(datadir, "plugin_rule.json")
        with open(steps, "w", encoding="utf-8") as f:
            json.dump({"orca_replay_version": "0.2.0", "name": "plugins' calls without MCP", "steps": [
                {"tool": "dialog_auto_answer", "args": {"rules": [{"match": f"^{RUNTIME_PROMPT}", "button": "yes"}]}},
                {"tool": "action_run", "args": {"id": f"plugin_script_action:{CAPABILITIES[2]}:{PLUGIN}"}},
                {"tool": "action_run", "args": {"id": UNGRANTED_ACTION}},
                {"tool": "app_wait", "args": {"for": "dialog", "timeout_s": 30}},
                {"tool": "app_wait", "args": {"for": "no_dialog", "timeout_s": 30}}]}, f)
        out = os.path.join(plugin_dir, "out_rule.json")
        ungranted_out = os.path.join(ungranted_dir, "out_ungranted.json")
        for stale in (out, ungranted_out):
            if os.path.exists(stale):
                os.remove(stale)
        subprocess.run([binary, "--datadir", datadir, "--run", steps], env=env, timeout=600)
        offline = read_json(out) or UNRECORDED
        check("with MCP off, a plugin's auto-answer rule is the plugin's", offline == {"ok": True, "result": "plugin"}, offline)
        offline_refused = read_json(ungranted_out) or UNRECORDED
        check("and a plugin without the tools permission is refused",
              offline_refused.get("ok") is False and offline_refused.get("code") == "tools_not_allowed", offline_refused)
        # Only a Yes to the prompt writes the grant, and with the MCP server off only the replay's rule answers it.
        check("and asked, in the prompt the replay's rule says Yes to", tools_grant(datadir, UNGRANTED) is True,
              stored_permissions(datadir, UNGRANTED))
    finally:
        if os.path.exists(os.path.join(datadir, "session.json")):
            harness(env, "stop")
        shutil.rmtree(datadir, ignore_errors=True)

    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
