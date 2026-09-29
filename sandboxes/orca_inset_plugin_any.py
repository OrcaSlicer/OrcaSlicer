# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Inset Every Slice"
# description = "Insets every layer's slices by 1mm at the Slice boundary (demo)."
# author = "OrcaSlicer"
# version = "0.02"
# type = "slicing-pipeline"
# ///
"""Inset Every Slice -- a small, WORKING SlicingPipeline sample plugin.

At Step.posSlice, for every layer/region of the sliced object, this shrinks each
sliced surface by INSET_MM using a real polygon offset (ExPolygon.offset) and
writes the result back with SurfaceCollection.set(). After the per-region edits,
layer.make_slices() re-derives the layer's merged islands (lslices) so
overhang/bridge detection, skirt/brim and support stay coherent with the inset
geometry. At Step.posSlice the split slice loop runs make_perimeters() right after
the hook, so the change cascades into perimeters, infill and the final G-code
-- the toolpath preview shrinks.

ExPolygon.offset() is a correct inward offset for any contour (it is Clipper
under the hood), and it naturally handles holes.
A surface may split into several islands or vanish when shrunk; both are handled.

No numpy required: the whole edit is expressed with the host geometry classes.
"""
import json

import orca

_DEFAULTS = {
    "inset_mm": 1.0,   # inward offset applied to every slice
}


def _inset_mm(self):
    try:
        return float(json.loads(self.get_config())["inset_mm"])
    except (AttributeError, TypeError, ValueError, KeyError):
        return _DEFAULTS["inset_mm"]


class InsetEverySlice(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Inset Every Slice"

    def get_default_config(self):
        return _DEFAULTS

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.posSlice or ctx.object is None:
            return orca.ExecutionResult.success()

        inset_mm = _inset_mm(self)
        if inset_mm <= 0.0:
            return orca.ExecutionResult.success("Inset: zero inset, nothing to do")

        # Millimeters -> scaled integer units via the *live* scale (never hardcode 1e6).
        inset_scaled = int(round(inset_mm / orca.slicing.unscale(1)))

        regions_touched = 0
        for layer in ctx.object.layers():
            if ctx.cancelled():
                break
            layer_touched = False
            for region in layer.regions():
                surfaces = region.slices.surfaces
                if not surfaces:
                    continue

                # Group the inward-offset geometry by surface type so each type is
                # preserved when written back (set() tags all its expolygons one type).
                by_type = {}
                for surface in surfaces:
                    shrunk = surface.expolygon.offset(-inset_scaled)  # [ExPolygon], may be empty
                    if shrunk:
                        by_type.setdefault(surface.surface_type, []).extend(shrunk)

                if not by_type:
                    continue  # every surface collapsed: leave the region untouched this demo

                # Rebuild the collection type-by-type: first set(), then append() the rest.
                items = list(by_type.items())
                first_type, first_expolys = items[0]
                region.slices.set(first_expolys, first_type)
                for st, expolys in items[1:]:
                    region.slices.append(expolys, st)
                regions_touched += 1
                layer_touched = True
            if layer_touched:
                # Re-derive the merged islands from the inset region slices.
                layer.make_slices()

        return orca.ExecutionResult.success(f"inset applied to {regions_touched} region(s)")


@orca.plugin
class InsetEverySlicePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(InsetEverySlice)
