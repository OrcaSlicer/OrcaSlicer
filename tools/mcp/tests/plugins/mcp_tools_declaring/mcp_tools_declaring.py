# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "MCP Tools Declaring"
# description = "Asks for the MCP tools as it loads, for tools/mcp/test_plugin_tools.py."
# author = "OrcaSlicer contributors"
# version = "1.0.0"
# ///
"""The plugin tools/mcp/test_plugin_tools.py loads to answer its load-time tools prompt.

It asks for the tools as it registers, with orca.request_permissions(tools=True), and for nothing else. Its
script "MCP tools declaring call" calls app_state and writes what happened to out_declaring.json in the
plugin's storage folder: {"ok", "code", "call"}, the call's `ready` or the error text.
"""
import json
import os

import orca


class CallTheTools(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools declaring call"

    def execute(self):
        try:
            out = {"ok": True, "code": None, "call": orca.host.tools.call("app_state").get("ready")}
        except Exception as e:
            details = getattr(e, "details", None)
            out = {"ok": False, "code": details.get("code") if isinstance(details, dict) else None, "call": str(e)}
        with open(os.path.join(orca.host.plugin.storage(), "out_declaring.json"), "w", encoding="utf-8") as f:
            json.dump(out, f)
        return orca.ExecutionResult.success("declaring")


@orca.plugin
class DeclaringPackage(orca.base):
    def register_capabilities(self):
        orca.request_permissions(tools=True)
        orca.register_capability(CallTheTools)
