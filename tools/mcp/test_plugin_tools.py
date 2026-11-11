"""Checks a plugin's calls into the MCP tools and the operations it offers, in the running app.

Launches OrcaSlicer on a fresh data folder with tests/plugins/mcp_tools_fixture side-loaded: over MCP
without the plugin's tools permission, then with it granted (and tests/plugins/mcp_tools_broken, whose load
fails after declaring an operation), then once without MCP, running a replay with --mcp-run, so the
plugin's calls go to the catalog-only service. The second launch ends by unticking the fixture in the Plugins
dialog, as a person unloads it. Run it as python tools/mcp/test_plugin_tools.py [--bin PATH].
The permission's own prompt opens while the app starts, before MCP can answer it, so the first launch has
the plugin not ask (ORCA_MCP_TOOLS_FIXTURE_ASK=0) and the second grants it in the plugin's saved state; the
prompt itself is covered by the unit tests.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from orca_mcp import OrcaMcp, find_binary  # noqa: E402

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
PORT = 13692
TOKEN = "plugin-tools-test-token"
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[:400]), flush=True)


def install(datadir, tools):
    """Side-loads the fixture enabled, with its operation and threads granted and the tools permission as given."""
    dest = os.path.join(datadir, "orca_plugins", PLUGIN)
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(FIXTURE, PLUGIN + ".py"), dest)
    with open(os.path.join(dest, ".install_state.json"), "w", encoding="utf-8") as f:
        json.dump({"installed_from": "local", "installed_version": "1.0.0", "plugin_name": "MCP Tools Fixture", "enabled": True,
                   "capabilities": [{name: True} for name in CAPABILITIES],
                   "permissions": {"threading": ["thread"], "tools": tools, "operations": ["measure"]}}, f, indent=2)
    # Where orca.host.plugin.storage() puts the plugin's files, and the scripts their outcomes.
    return os.path.join(datadir, "orca_plugins", "plugin_data", PLUGIN)


def install_broken(datadir):
    """Side-loads the plugin whose load fails, with its operation granted."""
    dest = os.path.join(datadir, "orca_plugins", BROKEN)
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(HERE, "tests", "plugins", BROKEN, BROKEN + ".py"), dest)
    with open(os.path.join(dest, ".install_state.json"), "w", encoding="utf-8") as f:
        json.dump({"installed_from": "local", "installed_version": "1.0.0", "plugin_name": "MCP Tools Broken Fixture", "enabled": True,
                   "capabilities": [{"MCP tools broken fixture": True}], "permissions": {"operations": ["measure"]}}, f, indent=2)


def wait_for(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.5)
    return predicate()


def harness(env, *args):
    return subprocess.run([sys.executable, os.path.join(HERE, "orca_mcp.py"), *args], env=env, capture_output=True, text=True,
                          timeout=300)


def run_plugin_script(orca, plugin_dir, name, capability):
    """Runs one of the fixture's scripts and returns what it recorded in its storage folder."""
    out = os.path.join(plugin_dir, f"out_{name}.json")
    if os.path.exists(out):
        os.remove(out)
    orca.call("run_action", id=f"plugin_script_action:{capability}:{PLUGIN}")
    deadline = time.time() + 60
    while time.time() < deadline:
        if os.path.exists(out):
            try:
                with open(out, encoding="utf-8") as f:
                    return json.load(f)
            except ValueError:
                pass  # still being written
        time.sleep(0.5)
    return {"ok": None, "error": "the script recorded nothing"}


def launch(env, datadir, extra):
    r = harness(env, "launch", "--datadir", datadir, "--port", str(PORT), *extra)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise SystemExit(1)
    return OrcaMcp(port=PORT, token=TOKEN)


def main():
    extra = sys.argv[1:]
    binary = find_binary(extra[extra.index("--bin") + 1] if "--bin" in extra else None)
    datadir = tempfile.mkdtemp(prefix="orca-plugin-tools-")
    env = dict(os.environ, ORCA_MCP_SESSION=os.path.join(datadir, "session.json"), ORCA_MCP_TOKEN=TOKEN)
    try:
        # --- Without the tools permission: the plugin loads, its calls are refused, its operation is offered. ---
        plugin_dir = install(datadir, tools=False)
        orca = launch(dict(env, ORCA_MCP_TOOLS_FIXTURE_ASK="0"), datadir, extra)
        refused = run_plugin_script(orca, plugin_dir, "call", CAPABILITIES[0])
        check("a plugin without the tools permission is refused", refused.get("ok") is False and
              "tools permission" in refused.get("error", ""), refused)
        described = orca.call_raw("search_tools", {"tool": f"plugin.{PLUGIN}.measure"})
        check("search_tools describes the plugin's granted operation", not described.get("isError") and
              "\"a\"" in described["content"][0]["text"], described)
        called = orca.call_raw("call_tool", {"tool": f"plugin.{PLUGIN}.measure", "args": {"a": 2}})
        check("call_tool runs the plugin's operation", not called.get("isError") and
              json.loads(called["content"][0]["text"]) == {"twice": 4}, called)
        listed = [t["name"] for t in orca.request("tools/list")["tools"]]
        check("the operation is not in tools/list", f"plugin.{PLUGIN}.measure" not in listed)
        harness(env, "stop")
        time.sleep(3)

        # --- With the tools permission granted, Python scripts allowed, and a plugin whose load fails. ---
        plugin_dir = install(datadir, tools=True)
        install_broken(datadir)
        orca = launch(env, datadir, ["--python-scripts"] + extra)
        called = run_plugin_script(orca, plugin_dir, "call", CAPABILITIES[0])
        check("a plugin with the tools permission calls a tool from its Script worker", called == {"ok": True, "result": True}, called)
        threaded = run_plugin_script(orca, plugin_dir, "thread", CAPABILITIES[1])
        check("a thread its Script worker starts calls a tool too", threaded == {"ok": True, "result": True}, threaded)
        rule = run_plugin_script(orca, plugin_dir, "rule", CAPABILITIES[2])
        check("an auto-answer rule a plugin sets is owned by the plugin", rule == {"ok": True, "result": "plugin"}, rule)
        exported = run_plugin_script(orca, plugin_dir, "export", CAPABILITIES[3])
        check("a plugin is refused export_replay", exported.get("ok") is False and "agent's session" in exported.get("error", ""),
              exported)
        # search_tools's one search: an operation with its schema, a tool with its summary (its schema is in
        # tools/list), and beside them the other catalogs' best matches.
        found = orca.call("search_tools", query="fixture measure")
        operation = next((i for i in found["items"] if i["name"] == f"plugin.{PLUGIN}.measure"), {})
        check("search_tools's search finds the plugin's operation with its schema", "input_schema" in operation, found)
        found = orca.call("search_tools", query="slice plate")
        tool = next((i for i in found["items"] if i["name"] == "slice"), {})
        check("search_tools's search names a tool with its summary", tool.get("summary") and "input_schema" not in tool, found)
        found = orca.call("search_tools", query="brim width")
        check("search_tools's search finds settings", any(s["key"] == "brim_width" for s in found.get("settings", {}).get("items", [])),
              found)
        found = orca.call("search_tools", query="arrange")
        check("search_tools's search finds Speed Dial actions", any("arrange" in a["id"] for a in found.get("actions", {}).get("items", [])),
              found)
        found = orca.call("search_tools", query="mesh normals")
        check("search_tools's search finds the Python API", found.get("python", {}).get("items", [{}])[0].get("name") ==
              "orca.host.TriangleMesh.face_normals", found)
        found = orca.call("search_tools", query="extrude")
        check("search_tools's search finds the Design tab's CAD tools", found.get("cad", {}).get("items"), found)
        found = orca.call("search_tools", query="?")
        check("a query with no words to search for matches nothing", found.get("total") == 0 and "names" not in found, found)
        journal = orca.call("export_replay")["replay"]["steps"]
        check("a plugin's calls are not in the agent's journal", not any(s["tool"] == "auto_answer_dialogs" for s in journal),
              journal)
        broken = orca.call_raw("search_tools", {"tool": f"plugin.{BROKEN}.measure"})
        check("a plugin whose load failed offers none of its granted operations", broken.get("isError"), broken)
        held = run_plugin_script(orca, plugin_dir, "handle", CAPABILITIES[4])
        check("the plugin opens a progress dialog and keeps its handle", held.get("ok") is True, held)
        grab = orca.await_job(orca.call("run_script", code=GRAB_HANDLE))
        check("a script that finds a plugin's handle can use its methods", grab.get("result") is True, grab)

        # --- The plugin unloads, as a person unticks it in the Plugins dialog. ---
        rules = lambda: json.dumps(orca.call("get_app_state").get("auto_answer_rules"))
        check("the auto-answer rule the plugin set lasts while it is loaded", RULE in rules(), rules())
        orca.call("run_action", id="orca_command:open_plugins:orca")
        untick = orca.call_raw("click", {"target": {"css": f'label.plugin-checkbox:has(input[data-plugin-key="{PLUGIN}"])',
                                                       "within": "Plugins"}})
        check("the Plugins dialog unticks the plugin", not untick.get("isError"), untick)
        withdrawn = lambda: orca.call_raw("search_tools", {"tool": f"plugin.{PLUGIN}.measure"}).get("isError")
        check("an unloaded plugin's operation is withdrawn", wait_for(withdrawn, 15))
        check("and the auto-answer rule it set ends with it", wait_for(lambda: RULE not in rules(), 15), rules())
        harness(env, "stop")
        time.sleep(3)

        # --- Without MCP: a launch replay runs the plugin's script, whose calls reach the catalog-only service. ---
        install(datadir, tools=True)  # enabled again: unticking it saved it disabled
        steps = os.path.join(datadir, "plugin_rule.json")
        with open(steps, "w", encoding="utf-8") as f:
            json.dump({"orca_replay": 1, "name": "a plugin's rule without MCP", "steps": [
                {"tool": "run_action", "args": {"id": f"plugin_script_action:{CAPABILITIES[2]}:{PLUGIN}"}},
                {"tool": "wait", "args": {"for": "dialog", "timeout_s": 10}}]}, f)
        out = os.path.join(plugin_dir, "out_rule.json")
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([binary, "--datadir", datadir, "--mcp-run", steps], env=env, timeout=600)
        try:
            with open(out, encoding="utf-8") as f:
                offline = json.load(f)
        except (OSError, ValueError) as e:
            offline = {"ok": None, "error": str(e)}
        check("with MCP off, a plugin's auto-answer rule is the plugin's", offline == {"ok": True, "result": "plugin"}, offline)
    finally:
        harness(env, "stop")
        time.sleep(2)
        shutil.rmtree(datadir, ignore_errors=True)

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
