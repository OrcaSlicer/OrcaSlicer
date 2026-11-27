# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "MCP Tools Broken Fixture"
# description = "Declares an operation, then fails its load, for tools/mcp/test_plugin_tools.py."
# author = "OrcaSlicer contributors"
# version = "1.0.0"
# ///
"""A plugin whose load fails after it declared an operation the person granted.

tools/mcp/test_plugin_tools.py side-loads it with "measure" granted: its capability's default
configuration raises while the plugin loads, so the load fails and the operation must never be offered.
"""
import orca


class Broken(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP tools broken fixture"

    def get_default_config(self):
        raise RuntimeError("this fixture's load fails here on purpose")

    def execute(self):
        return orca.ExecutionResult.success("never runs")


def measure(args):
    return {"never": True}


@orca.plugin
class BrokenPackage(orca.base):
    def register_capabilities(self):
        orca.register_operation("measure", "Never offered.", None, measure, changes=False)
        orca.register_capability(Broken)
