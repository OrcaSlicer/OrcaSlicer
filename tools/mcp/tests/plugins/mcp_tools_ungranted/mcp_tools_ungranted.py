# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "MCP Tools Ungranted"
# description = "Calls the MCP tools without permission to, for tools/mcp/test_plugin_tools.py."
# author = "OrcaSlicer contributors"
# version = "1.0.0"
# ///
"""The plugin tools/mcp/test_plugin_tools.py side-loads without the tools permission.

It declares no permissions. Its script "MCP tools ungranted call" reads the catalog through
orca.host.tools.list() and describe(), which need no permission, then calls app_state, which does, and
writes what happened to out_ungranted.json in the plugin's storage folder: {"listed", "described", "ok",
"error_type", "code", "call"}. It offers the operation "echo", which returns its arguments and calls no tool.
"""
import json
import os

import orca


def outcome():
    out = {}
    try:
        out["listed"] = any(c["name"] == "app_state" for c in orca.host.tools.list())
    except Exception as e:
        out["listed"] = f"{type(e).__name__}: {e}"
    try:
        out["described"] = orca.host.tools.describe("app_state").get("key")
    except Exception as e:
        out["described"] = f"{type(e).__name__}: {e}"
    try:
        out["call"] = orca.host.tools.call("app_state").get("ready")
        out.update(ok=True, error_type=None, code=None)
    except Exception as e:
        details = getattr(e, "details", None)
        out.update(ok=False, error_type=type(e).__name__, code=details.get("code") if isinstance(details, dict) else None,
                   call=str(e))
    with open(os.path.join(orca.host.plugin.storage(), "out_ungranted.json"), "w", encoding="utf-8") as f:
        json.dump(out, f)
    return orca.ExecutionResult.success("ungranted")


class CallWithoutPermission(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools ungranted call"

    def execute(self):
        return outcome()


def echo(args):
    return args


@orca.plugin
class UngrantedPackage(orca.base):
    def register_capabilities(self):
        orca.register_operation("echo", "Returns its arguments.", {"type": "object", "properties": {"a": {"type": "number"}}},
                                echo, changes=False)
        orca.register_capability(CallWithoutPermission)
