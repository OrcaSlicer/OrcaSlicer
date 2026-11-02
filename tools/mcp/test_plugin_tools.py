"""Checks a plugin's calls into the MCP tools and the operations it offers, in the running app.

Launches OrcaSlicer on a fresh data folder with tests/plugins/mcp_tools_fixture side-loaded: over MCP
without the plugin's tools permission, then with it granted (and tests/plugins/mcp_tools_broken, whose load
fails after declaring an operation), then once without MCP, running a script with --mcp-script, so the
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
from orca_mcp import OrcaMcp, find_binary, write_conf  # noqa: E402

FIXTURE = os.path.join(HERE, "tests", "plugins", "mcp_tools_fixture")
PLUGIN = "mcp_tools_fixture"
CAPABILITIES = ["MCP tools fixture call", "MCP tools fixture thread", "MCP tools fixture rule", "MCP tools fixture export",
                "MCP tools fixture handle"]
BROKEN = "mcp_tools_broken"
# A script that finds the fixture's handle among the loaded modules and tries one of its methods.
GRAB_HANDLE = """
import sys
handle = next(m.HANDLE for m in list(sys.modules.values()) if getattr(m, "HANDLE", None) is not None)
try:
    handle.update(5)
    result = "allowed"
except RuntimeError as e:
    result = str(e)
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


def allow_python(datadir):
    """Turns the Python scripts setting on in the data folder's config, as only a person can in the app."""
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    with open(conf, encoding="utf-8") as f:
        text = f.read()
    data = json.loads(text[:text.rfind("}") + 1])
    data.setdefault("app", {})["mcp_python_scripts"] = "true"
    write_conf(conf, data)


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


def run_script(orca, plugin_dir, name, capability):
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
        refused = run_script(orca, plugin_dir, "call", CAPABILITIES[0])
        check("a plugin without the tools permission is refused", refused.get("ok") is False and
              "tools permission" in refused.get("error", ""), refused)
        described = orca.call_raw("api_describe", {"tool": f"plugin.{PLUGIN}.measure"})
        check("api_describe describes the plugin's granted operation", not described.get("isError") and
              "\"a\"" in described["content"][0]["text"], described)
        called = orca.call_raw("api_call", {"tool": f"plugin.{PLUGIN}.measure", "args": {"a": 2}})
        check("api_call runs the plugin's operation", not called.get("isError") and
              json.loads(called["content"][0]["text"]) == {"twice": 4}, called)
        listed = [t["name"] for t in orca.request("tools/list")["tools"]]
        check("the operation is not in tools/list", f"plugin.{PLUGIN}.measure" not in listed)
        harness(env, "stop")
        time.sleep(3)

        # --- With the tools permission granted, Python scripts allowed, and a plugin whose load fails. ---
        plugin_dir = install(datadir, tools=True)
        install_broken(datadir)
        allow_python(datadir)
        orca = launch(env, datadir, extra)
        called = run_script(orca, plugin_dir, "call", CAPABILITIES[0])
        check("a plugin with the tools permission calls a tool from its Script worker", called == {"ok": True, "result": True}, called)
        threaded = run_script(orca, plugin_dir, "thread", CAPABILITIES[1])
        check("a thread its Script worker starts calls a tool too", threaded == {"ok": True, "result": True}, threaded)
        rule = run_script(orca, plugin_dir, "rule", CAPABILITIES[2])
        check("an auto-answer rule a plugin sets is owned by the plugin", rule == {"ok": True, "result": "plugin"}, rule)
        exported = run_script(orca, plugin_dir, "export", CAPABILITIES[3])
        check("a plugin is refused export_script", exported.get("ok") is False and "agent's session" in exported.get("error", ""),
              exported)
        journal = orca.call("export_script")["script"]["steps"]
        check("a plugin's calls are not in the agent's journal", not any(s["tool"] == "dialog_auto_answer" for s in journal),
              journal)
        broken = orca.call_raw("api_describe", {"tool": f"plugin.{BROKEN}.measure"})
        check("a plugin whose load failed offers none of its granted operations", broken.get("isError"), broken)
        held = run_script(orca, plugin_dir, "handle", CAPABILITIES[4])
        check("the plugin opens a progress dialog and keeps its handle", held.get("ok") is True, held)
        grab = orca.await_job(orca.call("run_python", code=GRAB_HANDLE))
        check("a script that finds a plugin's handle is refused its methods", "is for plugins" in str(grab.get("result")), grab)

        # --- The plugin unloads, as a person unticks it in the Plugins dialog. ---
        rules = lambda: json.dumps(orca.call("get_state").get("auto_answer_rules"))
        check("the auto-answer rule the plugin set lasts while it is loaded", RULE in rules(), rules())
        orca.call("run_action", id="orca_command:open_plugins:orca")
        untick = orca.call_raw("ui_click", {"target": {"css": f'label.plugin-checkbox:has(input[data-plugin-key="{PLUGIN}"])',
                                                       "within": "Plugins"}})
        check("the Plugins dialog unticks the plugin", not untick.get("isError"), untick)
        withdrawn = lambda: orca.call_raw("api_describe", {"tool": f"plugin.{PLUGIN}.measure"}).get("isError")
        check("an unloaded plugin's operation is withdrawn", wait_for(withdrawn, 15))
        check("and the auto-answer rule it set ends with it", wait_for(lambda: RULE not in rules(), 15), rules())
        harness(env, "stop")
        time.sleep(3)

        # --- Without MCP: a launch script runs the plugin's script, whose calls reach the catalog-only service. ---
        install(datadir, tools=True)  # enabled again: unticking it saved it disabled
        steps = os.path.join(datadir, "plugin_rule.json")
        with open(steps, "w", encoding="utf-8") as f:
            json.dump({"orca_script": 1, "name": "a plugin's rule without MCP", "steps": [
                {"tool": "run_action", "args": {"id": f"plugin_script_action:{CAPABILITIES[2]}:{PLUGIN}"}},
                {"tool": "wait", "args": {"for": "dialog", "timeout_s": 10}}]}, f)
        out = os.path.join(plugin_dir, "out_rule.json")
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([binary, "--datadir", datadir, "--mcp-script", steps], env=env, timeout=600)
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
