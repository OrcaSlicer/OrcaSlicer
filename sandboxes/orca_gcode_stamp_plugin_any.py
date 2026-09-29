# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "G-code Stamp"
# description = "Stamps a comment line into the exported G-code at the post-process step (demo)."
# author = "OrcaSlicer"
# version = "0.01"
# type = "slicing-pipeline"
# ///
"""G-code Stamp -- the post-processing half of the slicing-pipeline plugin.

Post-processing is now a step of the slicing pipeline: Step.psGCodePostProcess.
It fires from the G-code export path AFTER the classic post_process scripts, on the
exported G-code file -- NOT from Print::process(). So unlike the geometry steps
(posSlice, posPerimeters, ...) there is no live slicing graph here: ctx.print and
ctx.object are None. Instead the context carries ctx.gcode_path (the working G-code
file on disk, edited IN PLACE), ctx.host ("File", "OctoPrint", ...) and
ctx.output_name (the final file name). self.get_config() still works.

This sample inserts a single comment line near the top of the file. Because the same
capability class can also implement the geometry steps, one plugin can transform slices
AND stamp the final G-code; a geometry-only plugin just returns success here.

The step may fire more than once per slice (file export and/or upload each run it on a
separate working copy), and its output is not reflected in the G-code preview -- the
viewer maps the pre-post-process file.
"""
import orca
import json

_DEFAULTS = {
    "stamp_text": "processed by the OrcaSlicer G-code Stamp plugin",
}


def _stamp_text(self):
    try:
        text = json.loads(self.get_config())["stamp_text"]
    except (AttributeError, TypeError, ValueError, KeyError):
        text = _DEFAULTS["stamp_text"]
    return str(text).replace("\n", " ").strip() or _DEFAULTS["stamp_text"]


class GCodeStamp(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "G-code Stamp"
    
    def get_default_config(self):
        return _DEFAULTS

    def execute(self, ctx):
        # Only act at the post-process seam; at every geometry step this is a no-op.
        if ctx.step != orca.slicing.Step.psGCodePostProcess:
            return orca.ExecutionResult.success()
        if not ctx.gcode_path:
            return orca.ExecutionResult.success("G-code Stamp: no gcode_path, nothing to do")

        comment = "; " + _stamp_text(self) + " (host=" + (ctx.host or "?") + ")\n"

        # Edit the exported G-code in place: keep the original first line first (some flavors
        # expect a specific leading line), then insert the stamp right after it.
        with open(ctx.gcode_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        insert_at = 1 if lines else 0
        lines.insert(insert_at, comment)
        with open(ctx.gcode_path, "w", encoding="utf-8") as f:
            f.writelines(lines)

        return orca.ExecutionResult.success(
            "G-code Stamp: stamped '" + (ctx.output_name or ctx.gcode_path) + "'")


@orca.plugin
class GCodeStampPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(GCodeStamp)
