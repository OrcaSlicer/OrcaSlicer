# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Twistify"
# description = "Twists, tapers, and wobbles every layer's slice polygons as a function of Z (demo)."
# author = "OrcaSlicer"
# version = "0.02"
# type = "slicing-pipeline"
# ///
"""Twistify -- twist/taper/wobble any model at slice time.

At Step.posSlice, every layer's sliced surfaces are transformed by a similarity
about the object's bounding-box center as a function of Z -- edited IN PLACE
through the host geometry classes (ExPolygon.rotate/scale/translate). Each
surface is rotated about the center, then (if tapering) translated to the
origin, uniformly scaled, and translated back, so the taper stays centered on
the object instead of drifting toward the coordinate origin. An optional X
wobble is applied last. After the per-region edits, layer.make_slices()
re-derives the layer's merged islands so overhang/bridge/skirt/support stay
coherent. The split slice loop runs make_perimeters() right after the hook, so
the transform cascades into perimeters, infill, and the final G-code -- the
preview corkscrews and the print keeps correct walls/infill/flow.

Because we edit geometry in place, surface types are preserved automatically
(no per-surface type carry needed), and no numpy is required --
rotate/scale/translate are host methods. Parameters come from self.get_config()
(see _params below), which the host seeds from get_default_config(). The first
object layer is untouched (z_rel = 0), so bed adhesion is unaffected.
"""
import math
import json
import orca

_DEFAULTS = {
    "twist_deg_per_mm": 1.0,
    "taper_per_mm":     0.0,
    "wobble_ampl_mm":   0.0,
    "wobble_period_mm": 20.0,
    "min_scale":        0.05,
}


def _params(self):
    try:
        src = json.loads(self.get_config())
    except (AttributeError, TypeError):
        src = {}
    out = {}
    for key, default in _DEFAULTS.items():
        try:
            out[key] = float(src[key])
        except (KeyError, TypeError, ValueError):
            out[key] = default
    return out


def _is_identity(p):
    return p["twist_deg_per_mm"] == 0.0 and p["taper_per_mm"] == 0.0 and p["wobble_ampl_mm"] == 0.0


def _layer_params(z_rel, mm_to_scaled, p):
    """(angle_rad, scale, x_offset_scaled) for one layer. Exact identity at z_rel == 0."""
    theta = math.radians(p["twist_deg_per_mm"] * z_rel)
    s = max(p["min_scale"], 1.0 + p["taper_per_mm"] * z_rel)
    ox = 0.0
    if p["wobble_ampl_mm"] != 0.0 and p["wobble_period_mm"] > 0.0:
        ox = p["wobble_ampl_mm"] * math.sin(2.0 * math.pi * z_rel / p["wobble_period_mm"]) * mm_to_scaled
    return theta, s, ox


class Twistify(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Twistify"
    
    def get_default_config(self):
        return _DEFAULTS

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.posSlice or ctx.object is None:
            return orca.ExecutionResult.success()

        p = _params(self)
        if _is_identity(p):
            return orca.ExecutionResult.success("Twistify: identity parameters, nothing to do")

        mm_to_scaled = 1.0 / orca.slicing.unscale(1)

        layers = ctx.object.layers()
        if not layers:
            return orca.ExecutionResult.success("Twistify: object has no layers")

        # Twist/taper axis = the object's bounding-box center (scaled coords, same frame
        # as the slice polygons), so each object on the plate transforms about its own
        # center. Keep the float center for translate-to-origin/back around scale(), and
        # a rounded-to-Point center for rotate() (which takes an integer Point).
        min_x, min_y, max_x, max_y = ctx.object.bounding_box()
        cx = (min_x + max_x) / 2.0
        cy = (min_y + max_y) / 2.0
        center = orca.host.Point(int(round(cx)), int(round(cy)))
        z0 = float(layers[0].print_z)  # z_rel = 0 on the first layer -> footprint untouched

        layers_touched = 0
        for layer in layers:
            if ctx.cancelled():
                break
            z_rel = float(layer.print_z) - z0
            theta, s, ox = _layer_params(z_rel, mm_to_scaled, p)
            if theta == 0.0 and s == 1.0 and ox == 0.0:
                continue  # exact identity (always the first layer)

            edited = False
            for region in layer.regions():
                for surface in region.slices.surfaces:
                    ex = surface.expolygon
                    ex.rotate(theta, center)   # rotate about the object center (in place)
                    if s != 1.0:
                        # scale() scales about the coordinate ORIGIN, so re-center the
                        # geometry on the origin first and translate back after, making
                        # this a true similarity transform about the object's center.
                        ex.translate(-cx, -cy)
                        ex.scale(s)
                        ex.translate(cx, cy)
                    if ox != 0.0:
                        ex.translate(ox, 0.0)   # wobble in X
                    edited = True
            if edited:
                # Re-derive the merged islands from the twisted region slices.
                layer.make_slices()
                layers_touched += 1

        name = ctx.object.model_object().name or "object"
        return orca.ExecutionResult.success(
            f"Twistify: transformed {layers_touched} layer(s) of '{name}' "
            f"(twist {p['twist_deg_per_mm']} deg/mm, taper {p['taper_per_mm']}/mm, "
            f"wobble {p['wobble_ampl_mm']} mm)")


@orca.plugin
class TwistifyPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Twistify)
