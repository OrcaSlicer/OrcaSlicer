"""Checks a plugin's calls into the MCP tools and the operations it offers, in the running app.

Launches OrcaSlicer on a fresh data folder with the Design tab on (--cad) and
tests/plugins/mcp_tools_fixture side-loaded, and tests/plugins/mcp_tools_broken, whose load fails after declaring an
operation: over MCP, then once without MCP, running a replay with --run, so the plugin's calls go to the
runtime started without its endpoint. The first launch also checks that the plugin is sent the NewProject event when
project_new or its Speed Dial action starts a project, and ends by unticking the fixture in the Plugins dialog, as a
person unloads it.
Run it as python tools/mcp/test_plugin_tools.py [--bin PATH].
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import until  # noqa: E402
from orca_mcp import OrcaMcp, app_env, find_binary  # noqa: E402

FIXTURE = os.path.join(HERE, "tests", "plugins", "mcp_tools_fixture")
PLUGIN = "mcp_tools_fixture"
CAPABILITIES = ["MCP tools fixture call", "MCP tools fixture thread", "MCP tools fixture rule", "MCP tools fixture export",
                "MCP tools fixture handle"]
BROKEN = "mcp_tools_broken"
# A script that finds the fixture's progress dialog among the loaded modules and moves its gauge.
GRAB_HANDLE = """
import sys
handle = next(m.HANDLE for m in list(sys.modules.values()) if getattr(m, "HANDLE", None) is not None)
result = handle.update(5)
"""
RULE = "MCP tools fixture never shown"  # the match of the auto-answer rule the fixture's "rule" script sets
TOKEN = "plugin-tools-test-token"
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[:400]), flush=True)


def install(datadir):
    """Side-loads the fixture enabled, with its threads granted."""
    dest = os.path.join(datadir, "orca_plugins", PLUGIN)
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(FIXTURE, PLUGIN + ".py"), dest)
    with open(os.path.join(dest, ".install_state.json"), "w", encoding="utf-8") as f:
        json.dump({"installed_from": "local", "installed_version": "1.0.0", "plugin_name": "MCP Tools Fixture", "enabled": True,
                   "capabilities": [{name: True} for name in CAPABILITIES],
                   "permissions": {"threading": ["thread"]}}, f, indent=2)
    # Where orca.host.plugin.storage() puts the plugin's files, and the scripts their outcomes.
    return os.path.join(datadir, "orca_plugins", "plugin_data", PLUGIN)


def install_broken(datadir):
    """Side-loads the plugin whose load fails."""
    dest = os.path.join(datadir, "orca_plugins", BROKEN)
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(HERE, "tests", "plugins", BROKEN, BROKEN + ".py"), dest)
    with open(os.path.join(dest, ".install_state.json"), "w", encoding="utf-8") as f:
        json.dump({"installed_from": "local", "installed_version": "1.0.0", "plugin_name": "MCP Tools Broken Fixture", "enabled": True,
                   "capabilities": [{"MCP tools broken fixture": True}]}, f, indent=2)


def free_port():
    """A port nothing listens on now, so that this test runs beside the others of a sweep."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def reached(fn, what, timeout_s=30):
    """Whether fn() comes true within `timeout_s`, for a check to report rather than to stop the test."""
    try:
        return bool(until(fn, what, timeout_s))
    except TimeoutError:
        return False


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


def harness(env, *args):
    return subprocess.run([sys.executable, os.path.join(HERE, "orca_mcp.py"), *args], env=env, capture_output=True, text=True,
                          timeout=300)


def run_plugin_script(orca, plugin_dir, name, capability):
    """Runs one of the fixture's scripts and returns what it recorded in its storage folder."""
    out = os.path.join(plugin_dir, f"out_{name}.json")
    if os.path.exists(out):
        os.remove(out)
    orca.call("action_run", id=f"plugin_script_action:{capability}:{PLUGIN}")

    def recorded():
        try:
            with open(out, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):  # not written yet, or still being written
            return None

    try:
        return until(recorded, f"the {name} script records its outcome", timeout_s=60)
    except TimeoutError:
        return {"ok": None, "error": "the script recorded nothing"}


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
        install_broken(datadir)
        # The Design tab on, so that its commands are among tool_search's commands.
        orca = launch(env, datadir, ["--cad", *extra])
        described = orca.call_raw("tool_search", {"tool": f"plugin.{PLUGIN}.measure"})
        check("tool_search describes the plugin's operation", not described.get("isError") and
              "\"a\"" in described["content"][0]["text"], described)
        operation = {} if described.get("isError") else json.loads(described["content"][0]["text"])
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
        # tool_search's one search: a command the tool list does not show (an operation, a menu item) with
        # its schema, a listed tool with its summary (its schema is in tools/list), and beside them the other
        # catalogs' best matches.
        found = orca.call("tool_search", query="fixture measure")
        operation = next((i for i in found["items"] if i["name"] == f"plugin.{PLUGIN}.measure"), {})
        check("tool_search's search finds the plugin's operation with its schema",
              operation.get("listed") is False and "input_schema" in operation, found)
        found = orca.call("tool_search", query="slice plate")
        tool = next((i for i in found["items"] if i["name"] == "plate_slice"), {})
        check("tool_search's search names a listed tool with its summary",
              tool.get("listed") is True and tool.get("summary") and "input_schema" not in tool, found)
        found = orca.call("tool_search", query="brim width")
        check("tool_search's search finds settings", any(s["key"] == "brim_width" for s in found.get("settings", {}).get("items", [])),
              found)
        found = orca.call("tool_search", query="developer mode")
        command = next((i for i in found["items"] if i["name"] == "settings_toggle_developer_mode"), {})
        check("tool_search's search finds a menu item's command among the commands, with its schema",
              command.get("listed") is False and "input_schema" in command, found)
        check("and does not list its Speed Dial action among the actions",
              not any("toggle_developer_mode" in a["id"] for a in found.get("actions", {}).get("items", [])), found)
        found = orca.call("tool_search", query="fixture")
        check("tool_search's search finds the Speed Dial's other actions",
              any("fixture" in a.get("title", "").lower() for a in found.get("actions", {}).get("items", [])), found)
        described = orca.call_raw("tool_search", {"tool": "script_toggle_panel"})
        item = {} if described.get("isError") else json.loads(described["content"][0]["text"])
        check("tool_search describes a menu item's command", item.get("listed") is False and "input_schema" in item, described)
        names = [item.get("name") for item in orca.call("tool_search", group="objects").get("items", [])]
        check("tool_search's group is any case, a place's heading", "object_arrange" in names, names)
        unknown = orca.call_raw("tool_search", {"group": "commandz"})
        check("a group no command has is refused, naming the groups there are",
              unknown.get("isError") and "Objects" in unknown["content"][0]["text"], unknown)
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
        found = orca.call("tool_search", query="mesh normals")
        check("tool_search's search finds the Python API", found.get("python", {}).get("items", [{}])[0].get("name") ==
              "orca.host.TriangleMesh.face_normals", found)
        design_tab = orca.call("app_state").get("design_tab")
        check("the Design tab is on (launch --cad)", design_tab is True, design_tab)
        found = orca.call("tool_search", query="extrude")
        extrude = next((i for i in found["items"] if i["name"] == "extrude"), {})
        check("tool_search's search finds a Design command among the commands, with its schema",
              extrude.get("group") == "Design" and extrude.get("listed") is False and "input_schema" in extrude, found)
        check("and in no section of its own", "cad" not in found, found)
        found = orca.call("tool_search", query="?")
        check("a query with no words to search for matches nothing", found.get("total") == 0 and "names" not in found, found)
        journal = orca.call("replay_export")["replay"]["steps"]
        answers = [s for s in journal if s["tool"] == "dialog_auto_answer"]
        check("a plugin's calls are not in the agent's journal",
              len(answers) == 1 and answers[0].get("args", {}).get("rules") == [], answers)
        broken_row = next((r for r in orca.call("plugin_list")["plugins"] if r["key"] == BROKEN), None)
        check("the plugin whose load failed is listed with its error",
              broken_row is not None and "this fixture's load fails here on purpose" in (broken_row.get("error") or ""), broken_row)
        broken = orca.call_raw("tool_search", {"tool": f"plugin.{BROKEN}.measure"})
        check("and offers none of its operations", broken.get("isError") is True and
              broken["content"][0]["text"].startswith(f"Unknown tool: plugin.{BROKEN}.measure"), broken)
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

        # --- The plugin unloads, as a person unticks it in the Plugins dialog. ---
        rules = lambda: json.dumps(orca.call("app_state").get("auto_answer_rules"))
        check("the auto-answer rule the plugin set lasts while it is loaded", RULE in rules(), rules())
        orca.call("action_run", id="orca_command:open_plugins:orca")
        untick = orca.call_raw("widget_click", {"target": {"css": f'label.plugin-checkbox:has(input[data-plugin-key="{PLUGIN}"])',
                                                       "within": "Plugins"}})
        check("the Plugins dialog unticks the plugin", not untick.get("isError"), untick)
        withdrawn = lambda: orca.call_raw("tool_search", {"tool": f"plugin.{PLUGIN}.measure"}).get("isError")
        check("an unloaded plugin's operation is withdrawn", reached(withdrawn, "the plugin's operation is withdrawn"))
        check("and the auto-answer rule it set ends with it",
              reached(lambda: RULE not in rules(), "the plugin's auto-answer rule ends"), rules())
        # stop returns once the app has exited, so that the --run launch below is the only app on the folder.
        harness(env, "stop")

        # --- Without MCP: a launch replay runs the plugin's script, whose calls reach the runtime without its endpoint. ---
        install(datadir)  # enabled again: unticking it saved it disabled
        steps = os.path.join(datadir, "plugin_rule.json")
        with open(steps, "w", encoding="utf-8") as f:
            json.dump({"orca_replay_version": "0.2.0", "name": "a plugin's rule without MCP", "steps": [
                {"tool": "action_run", "args": {"id": f"plugin_script_action:{CAPABILITIES[2]}:{PLUGIN}"}},
                {"tool": "app_wait", "args": {"for": "dialog", "timeout_s": 10}}]}, f)
        out = os.path.join(plugin_dir, "out_rule.json")
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([binary, "--datadir", datadir, "--run", steps], env=env, timeout=600)
        try:
            with open(out, encoding="utf-8") as f:
                offline = json.load(f)
        except (OSError, ValueError) as e:
            offline = {"ok": None, "error": str(e)}
        check("with MCP off, a plugin's auto-answer rule is the plugin's", offline == {"ok": True, "result": "plugin"}, offline)
    finally:
        if os.path.exists(os.path.join(datadir, "session.json")):
            harness(env, "stop")
        shutil.rmtree(datadir, ignore_errors=True)

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
