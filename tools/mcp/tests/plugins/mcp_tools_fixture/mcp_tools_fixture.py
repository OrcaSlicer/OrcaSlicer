# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "MCP Tools Fixture"
# description = "Calls the MCP tools from a plugin and offers an operation, for tools/mcp/test_plugin_tools.py."
# author = "OrcaSlicer contributors"
# version = "1.0.0"
# ///
"""The plugin tools/mcp/test_plugin_tools.py drives to check a plugin's calls into the MCP tools.

Side-load it into a test data directory (test_plugin_tools.py does) and run its scripts from run_action.
Each script writes what happened to out_<name>.json in the plugin's storage folder, which the test reads back:
{"ok": true, "result": ...} or {"ok": false, "error": "...", "details": ...}. It asks for the tools
permission unless ORCA_MCP_TOOLS_FIXTURE_ASK is 0. "call" calls get_state,
"thread" calls it from a thread the script starts, "rule" registers an auto-answer rule, "export" asks for
export_script, which a plugin is refused, and "handle" opens a progress dialog and keeps its handle in
HANDLE, for a script to be refused its methods. The plugin also offers the operation "measure", which
doubles its argument a.
"""
import json
import os
import threading

import orca


def record(name, action):
    try:
        outcome = {"ok": True, "result": action()}
    except orca.host.tools.ToolError as e:
        outcome = {"ok": False, "error": str(e), "details": e.details}
    except Exception as e:
        outcome = {"ok": False, "error": f"{type(e).__name__}: {e}"}
    with open(os.path.join(orca.host.plugin.storage(), f"out_{name}.json"), "w", encoding="utf-8") as f:
        json.dump(outcome, f)
    return orca.ExecutionResult.success(name)


def call_from_thread():
    seen = {}

    def work():
        try:
            seen["ready"] = orca.host.tools.call("get_state").get("ready")
        except Exception as e:
            seen["error"] = f"{type(e).__name__}: {e}"

    worker = threading.Thread(target=work)
    worker.start()
    worker.join()
    if "error" in seen:
        raise RuntimeError(seen["error"])
    return seen["ready"]


class CallTool(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools fixture call"

    def execute(self):
        return record("call", lambda: orca.host.tools.call("get_state").get("ready"))


class CallFromThread(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools fixture thread"

    def execute(self):
        return record("thread", call_from_thread)


class SetRule(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools fixture rule"

    def execute(self):
        rules = {"rules": [{"match": "^MCP tools fixture never shown$", "button": "cancel"}]}
        return record("rule", lambda: orca.host.tools.call("dialog_auto_answer", rules).get("scope"))


class ExportScript(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools fixture export"

    def execute(self):
        return record("export", lambda: orca.host.tools.call("export_script"))


# A handle only the plugin's own calls may use; a script that finds it here is refused its methods.
HANDLE = None


class HoldHandle(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools fixture handle"

    def execute(self):
        global HANDLE

        def create():
            global HANDLE
            HANDLE = orca.host.ui.create_progress_dialog("MCP tools fixture", "Held for the test.", 100, 0)
            return HANDLE.id

        return record("handle", create)


def measure(args):
    return {"twice": args.get("a", 0) * 2}


@orca.plugin
class FixturePackage(orca.base):
    def register_capabilities(self):
        # The prompt for it opens while the app starts, before MCP can answer it, so the test that checks a
        # plugin without the permission sets ORCA_MCP_TOOLS_FIXTURE_ASK=0 and the plugin does not ask.
        if os.environ.get("ORCA_MCP_TOOLS_FIXTURE_ASK", "1") != "0":
            orca.request_permissions(tools=True)
        orca.register_operation("measure", "Doubles a.", {"type": "object", "properties": {"a": {"type": "number"}}}, measure,
                                changes=False)
        for capability in (CallTool, CallFromThread, SetRule, ExportScript, HoldHandle):
            orca.register_capability(capability)
