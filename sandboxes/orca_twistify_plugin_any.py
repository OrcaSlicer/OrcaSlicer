# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Twistify"
# description = "Twists, tapers, and wobbles every layer's slice polygons as a function of Z (demo)."
# author = "SoftFever"
# version = "0.03"
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
(see _params below); the host seeds them from get_default_config() the first
time the plugin loads. The first object layer is untouched (z_rel = 0), so bed
adhesion is unaffected.

The plugin also ships its own configuration UI (has_config_ui/get_config_ui):
a self-contained page the Plugins dialog renders instead of the JSON editor,
drawing the transform it is about to apply as an isometric stack of layer
outlines. It reaches the host only through the injected window.orca bridge --
getConfig/saveConfig/restoreDefaults/getContext/onConfig/onTheme -- and styles
itself from the --orca-* theme variables the host hands the frame, so it
follows OrcaSlicer's light/dark theme.
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
    except (AttributeError, TypeError, ValueError):
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


# The configuration page. Self-contained on purpose: it runs in an iframe sandboxed into an opaque
# origin, so it has only the window.orca bridge and the --orca-* theme variables the host injects --
# no network, no same-origin access, no shared stylesheet. get_config_ui() substitutes _DEFAULTS for
# __ORCA_DEFAULTS__, so Python owns the values and the page adds only presentation.
_CONFIG_UI = """
<style>
  :root {
    --ink:      var(--orca-fg, #1f2429);
    --paper:    var(--orca-bg, #ffffff);
    --rule:     var(--orca-border, #d9dee3);
    --quiet:    var(--orca-muted, #6b7580);
    --live:     var(--orca-accent, #009688);
    --live-ink: var(--orca-accent-fg, #ffffff);
    --ui:       var(--orca-font, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif);
    --data:     ui-monospace, "Cascadia Mono", "SF Mono", Menlo, Consolas, monospace;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --ink:   var(--orca-fg, #e4e6e8);
      --paper: var(--orca-bg, #2b2d30);
      --rule:  var(--orca-border, #3d4043);
      --quiet: var(--orca-muted, #9aa0a6);
    }
  }

  * { box-sizing: border-box; }
  html, body { height: 100%; }
  body {
    margin: 0;
    background: var(--paper);
    color: var(--ink);
    font: 13px/1.45 var(--ui);
  }
  .panel { display: flex; flex-direction: column; height: 100%; }

  .head {
    display: flex; align-items: baseline; gap: 10px;
    padding: 12px 16px 10px;
    border-bottom: 1px solid var(--rule);
  }
  .mark {
    font: 10px/1 var(--data); letter-spacing: .14em; text-transform: uppercase;
    color: var(--live);
  }
  .head-note { font-size: 11px; color: var(--quiet); }

  /* The frame is as narrow as ~400px in a resized dialog and as wide as ~600px; below the
     breakpoint the drawing moves under the controls rather than squeezing both. */
  .work {
    flex: 1; min-height: 0; overflow: auto;
    display: grid; grid-template-columns: minmax(0, 1fr) 180px; gap: 16px;
    align-content: start; padding: 14px 16px;
  }
  @media (max-width: 470px) {
    .work { grid-template-columns: minmax(0, 1fr); }
    .plot { max-width: 210px; }
  }

  .rows { display: flex; flex-direction: column; gap: 12px; }
  .row { display: grid; grid-template-columns: 1fr auto; align-items: center; gap: 4px 8px; }
  .row-label {
    grid-column: 1;
    font: 10px/1 var(--data); letter-spacing: .12em; text-transform: uppercase;
    color: var(--quiet);
  }
  .row-figure { grid-column: 2; display: flex; align-items: baseline; gap: 4px; }
  .row-figure input {
    /* No spinners: the rail is the coarse control, this is the exact one, and the arrows would
       steal the width the value needs. */
    -webkit-appearance: textfield; appearance: textfield;
    width: 76px; padding: 3px 6px; text-align: right;
    font: 13px/1.2 var(--data); font-variant-numeric: tabular-nums;
    color: var(--ink); background: transparent;
    border: 1px solid transparent; border-radius: 3px;
  }
  .row-figure input::-webkit-outer-spin-button,
  .row-figure input::-webkit-inner-spin-button { -webkit-appearance: none; margin: 0; }
  .row-figure input:hover { border-color: var(--rule); }
  .row-figure input:focus { outline: none; border-color: var(--live); }
  .unit { font: 10px/1 var(--data); color: var(--quiet); min-width: 26px; }

  /* The rail carries a detent at the value that means "no deformation", so neutral is a position
     you can feel for rather than a number you have to read. */
  .rail { grid-column: 1 / -1; position: relative; height: 16px; }
  .rail[data-detent]::before {
    content: ""; position: absolute; left: var(--detent); top: 3px;
    width: 1px; height: 10px; background: var(--rule);
  }
  .rail input {
    -webkit-appearance: none; appearance: none;
    position: relative; width: 100%; height: 16px; margin: 0; background: transparent;
  }
  .rail input::-webkit-slider-runnable-track { height: 2px; background: var(--rule); border-radius: 1px; }
  .rail input::-webkit-slider-thumb {
    -webkit-appearance: none; width: 11px; height: 11px; margin-top: -4.5px;
    border: none; border-radius: 50%; background: var(--live); cursor: grab;
  }
  .rail input:focus { outline: none; }
  .rail input:focus-visible { outline: 2px solid var(--live); outline-offset: 1px; border-radius: 3px; }
  .rail input:disabled::-webkit-slider-thumb { background: var(--quiet); cursor: default; }

  .readout {
    margin-top: 14px; padding-top: 10px; border-top: 1px solid var(--rule);
    font-size: 11px; color: var(--quiet); min-height: 32px;
  }
  .readout b { color: var(--ink); font-weight: 600; font-family: var(--data); font-variant-numeric: tabular-nums; }

  .plot { margin: 0; display: flex; flex-direction: column; gap: 6px; }
  .plot svg { width: 100%; height: auto; display: block; }
  .plot figcaption { font: 10px/1.4 var(--data); color: var(--quiet); }
  /* SVG colours live here rather than in stroke="" attributes: var() is not reliable in a
     presentation attribute, and this way the drawing re-themes with the rest of the page. */
  .ring, .helix, .datum { fill: none; }
  .ring  { stroke: var(--live); stroke-width: 1; }
  .helix { stroke: var(--live); stroke-width: 1; stroke-opacity: .5; stroke-dasharray: 2 2; }
  .datum { stroke: var(--quiet); stroke-width: 1; stroke-opacity: .7; }
  .tick  { stroke: var(--rule); stroke-width: 1; }
  .plot text { fill: var(--quiet); font: 8px var(--data); }

  .foot {
    display: flex; align-items: center; gap: 10px;
    padding: 10px 16px; border-top: 1px solid var(--rule);
  }
  .state { flex: 1; font-size: 11px; color: var(--quiet); }
  .state.dirty { color: var(--ink); }
  button {
    font: 12px/1 var(--ui); padding: 6px 14px; border-radius: 4px; cursor: pointer;
    border: 1px solid var(--rule); background: transparent; color: var(--ink);
  }
  button.go { border-color: var(--live); background: var(--live); color: var(--live-ink); }
  button:disabled { opacity: .45; cursor: default; }
  button:focus-visible { outline: 2px solid var(--live); outline-offset: 2px; }

  @media (prefers-reduced-motion: reduce) { * { transition: none !important; } }
</style>

<div class="panel">
  <header class="head">
    <span class="mark">Twistify</span>
    <span class="head-note">Applied to every layer as it is sliced</span>
  </header>

  <div class="work">
    <div>
      <div class="rows" id="rows"></div>
      <p class="readout" id="readout"></p>
    </div>

    <figure class="plot">
      <svg id="plot" viewBox="0 0 180 208" aria-hidden="true"></svg>
      <figcaption id="plotNote"></figcaption>
    </figure>
  </div>

  <footer class="foot">
    <span class="state" id="state"></span>
    <button type="button" id="restore">Restore defaults</button>
    <button type="button" id="save" class="go">Save</button>
  </footer>
</div>

<script>
(function () {
  var DEFAULTS = __ORCA_DEFAULTS__;

  // Presentation only. `neutral` marks the value at which the control contributes no deformation;
  // rows without one (period, floor) are shape parameters, not amounts.
  var FIELDS = [
    { key: "twist_deg_per_mm", label: "Twist", unit: "\\u00b0/mm", min: -20, max: 20, step: 0.1, dp: 1, neutral: 0,
      hint: "Rotation added per millimetre of height." },
    { key: "taper_per_mm", label: "Taper", unit: "/mm", min: -0.05, max: 0.05, step: 0.001, dp: 3, neutral: 0,
      hint: "Scale added per millimetre. Negative narrows the model as it rises." },
    { key: "wobble_ampl_mm", label: "Wobble", unit: "mm", min: 0, max: 10, step: 0.1, dp: 1, neutral: 0,
      hint: "How far each layer slides in X, from centre to peak." },
    { key: "wobble_period_mm", label: "Wobble period", unit: "mm", min: 1, max: 120, step: 1, dp: 0,
      hint: "Height of one full wobble cycle." },
    { key: "min_scale", label: "Scale floor", unit: "\\u00d7", min: 0.01, max: 1, step: 0.01, dp: 2,
      hint: "Taper never shrinks a layer past this, so a tall model cannot collapse." }
  ];

  // The preview simulates this much height. It never reads the real model, so the caption says so
  // rather than implying the drawing is to scale with the plate.
  var Z_SPAN = 40;
  var Z_STEP = 2;

  var form = {};
  var stored = {};
  var context = (window.orca && window.orca.getContext) ? window.orca.getContext() : { scope: "global" };
  var savedNoteTimer = 0;

  function num(value, fallback) {
    var n = parseFloat(value);
    return isFinite(n) ? n : fallback;
  }

  function merged(config) {
    var out = {};
    Object.keys(DEFAULTS).forEach(function (key) {
      out[key] = num(config ? config[key] : undefined, DEFAULTS[key]);
    });
    return out;
  }

  function isDirty() {
    return Object.keys(DEFAULTS).some(function (key) { return form[key] !== stored[key]; });
  }

  // --- the transform, exactly as _layer_params() applies it -------------------------------------

  function layerAt(z) {
    var theta = form.twist_deg_per_mm * z * Math.PI / 180;
    var scale = Math.max(form.min_scale, 1 + form.taper_per_mm * z);
    var shift = 0;
    if (form.wobble_ampl_mm !== 0 && form.wobble_period_mm > 0)
      shift = form.wobble_ampl_mm * Math.sin(2 * Math.PI * z / form.wobble_period_mm);
    return { theta: theta, scale: scale, shift: shift };
  }

  function deforms() {
    return form.twist_deg_per_mm !== 0 || form.taper_per_mm !== 0 || form.wobble_ampl_mm !== 0;
  }

  // --- drawing ----------------------------------------------------------------------------------

  var ISO_X = 0.866, ISO_Y = 0.5;     // isometric: plan outlines stacked by height
  var ORIGIN_X = 108, ORIGIN_Y = 182; // the z = 0 outline's centre, in viewBox units
  var PLAN = 20;                      // half-width of the sample contour
  var Z_UNITS = 3.3;                  // viewBox units per millimetre of height
  var MM = 1.5;                       // viewBox units per millimetre of wobble travel
  var REACH = 62;                     // how far sideways the drawing may run before it is scaled down

  // Plan-view corners of the layer at `z`, before projection.
  function planOutline(z) {
    var layer = layerAt(z);
    var half = PLAN * layer.scale;
    var cos = Math.cos(layer.theta), sin = Math.sin(layer.theta);
    return [[-half, -half], [half, -half], [half, half], [-half, half]].map(function (corner) {
      return [corner[0] * cos - corner[1] * sin + layer.shift * MM,
              corner[0] * sin + corner[1] * cos];
    });
  }

  function points(plan, z, fit) {
    return plan.map(function (p) {
      var x = ORIGIN_X + (p[0] - p[1]) * ISO_X * fit;
      var y = ORIGIN_Y + (p[0] + p[1]) * ISO_Y * fit - z * Z_UNITS;
      return x.toFixed(1) + "," + y.toFixed(1);
    }).join(" ");
  }

  function draw() {
    var svg = [];
    var layers = [];
    var reach = 0;
    for (var z = 0; z <= Z_SPAN + 0.001; z += Z_STEP) {
      var plan = planOutline(z);
      layers.push({ z: z, plan: plan });
      plan.forEach(function (p) { reach = Math.max(reach, Math.abs(p[0] - p[1]) * ISO_X); });
    }
    // A wide taper or a big wobble would run out of the frame, so scale down to fit rather than
    // clipping. Z keeps its scale, so the ruler stays true.
    var fit = reach > REACH ? REACH / reach : 1;

    // Z ruler: the drawing is a height plot, so it gets an axis.
    for (var mark = 0; mark <= Z_SPAN; mark += 10) {
      var y = ORIGIN_Y - mark * Z_UNITS;
      svg.push('<line class="tick" x1="24" y1="' + y.toFixed(1) + '" x2="30" y2="' + y.toFixed(1) + '"/>');
      svg.push('<text x="20" y="' + (y + 3).toFixed(1) + '" text-anchor="end">' + mark + '</text>');
    }
    svg.push('<text x="30" y="' + (ORIGIN_Y - Z_SPAN * Z_UNITS - 11).toFixed(1) +
             '" text-anchor="end">mm</text>');

    // The stack, faint at the bed and full strength at the top: the deformation grows with Z.
    var trace = [];
    layers.forEach(function (layer) {
      var ring = points(layer.plan, layer.z, fit);
      svg.push('<polygon class="ring" points="' + ring + '" stroke-opacity="' +
               (0.18 + 0.82 * layer.z / Z_SPAN).toFixed(2) + '"/>');
      trace.push(ring.split(" ")[1]);
    });

    // One corner followed all the way up: on a twisted model, the helix the walls will run in.
    svg.push('<polyline class="helix" points="' + trace.join(" ") + '"/>');

    // The datum: the first layer is never transformed, so it doubles as the reference outline.
    svg.push('<polygon class="datum" points="' + points(layers[0].plan, 0, fit) + '"/>');

    document.getElementById("plot").innerHTML = svg.join("");
    document.getElementById("plotNote").textContent =
      deforms() ? "Sample contour, " + Z_SPAN + " mm of Z" : "Sample contour, undeformed";
  }

  // --- readout ----------------------------------------------------------------------------------

  function describe() {
    if (!deforms())
      return "No deformation. Twistify passes every layer through unchanged.";

    var top = layerAt(Z_SPAN);
    var parts = [];
    if (form.twist_deg_per_mm !== 0)
      parts.push("<b>" + (form.twist_deg_per_mm * Z_SPAN).toFixed(0) + "\\u00b0</b> of rotation");
    if (form.taper_per_mm !== 0)
      parts.push("<b>" + top.scale.toFixed(2) + "\\u00d7</b> scale" +
                 (top.scale === form.min_scale ? " (at the floor)" : ""));
    if (form.wobble_ampl_mm !== 0)
      parts.push("<b>\\u00b1" + form.wobble_ampl_mm.toFixed(1) + "</b> mm of wobble");
    return "At " + Z_SPAN + " mm above the first layer: " + parts.join(", ") + ".";
  }

  function setReadout(html) { document.getElementById("readout").innerHTML = html; }

  // Whatever the pointer or the keyboard is on explains itself; with neither, report the totals.
  function restoreReadout() {
    var id = document.activeElement ? document.activeElement.id : "";
    for (var i = 0; i < FIELDS.length; i++)
      if (id === "n-" + FIELDS[i].key || id === "r-" + FIELDS[i].key)
        return setReadout(FIELDS[i].hint);
    setReadout(describe());
  }

  function refreshState() {
    var dirty = isDirty();
    document.getElementById("save").disabled = context.readOnly || !dirty;

    if (savedNoteTimer)
      return;   // the "Saved" note owns the line until it expires
    var state = document.getElementById("state");
    state.textContent = context.readOnly ? "This configuration is read-only."
                      : dirty ? "Unsaved changes" : "";
    state.classList.toggle("dirty", dirty && !context.readOnly);
  }

  function announce(message) {
    var state = document.getElementById("state");
    state.textContent = message;
    state.classList.remove("dirty");
    clearTimeout(savedNoteTimer);
    savedNoteTimer = setTimeout(function () { savedNoteTimer = 0; refreshState(); }, 2200);
  }

  // --- controls ---------------------------------------------------------------------------------

  function detent(field) {
    if (field.neutral === undefined) return "";
    return ((field.neutral - field.min) / (field.max - field.min) * 100).toFixed(2) + "%";
  }

  function clamp(field, value) { return Math.min(field.max, Math.max(field.min, value)); }

  // `echo` rewrites the number field with the clamped, formatted value. Off while the user is still
  // typing: "-" and "0.0" are valid partial input, and rewriting them mid-keystroke fights the
  // typist. The drawing still follows every keystroke.
  function apply(field, value, echo) {
    form[field.key] = clamp(field, value);
    if (echo)
      document.getElementById("n-" + field.key).value = form[field.key].toFixed(field.dp);
    document.getElementById("r-" + field.key).value = form[field.key];
    draw();
    // A control explains itself while you approach it, but once a value moves what matters is the
    // total it adds up to.
    setReadout(describe());
    refreshState();
  }

  function buildRows() {
    var host = document.getElementById("rows");
    FIELDS.forEach(function (field) {
      var row = document.createElement("div");
      row.className = "row";
      // The number input is the labelled, keyboard-reachable control; the rail is the same value by
      // pointer, so it stays out of the tab order and the accessibility tree.
      row.innerHTML =
        '<label class="row-label" for="n-' + field.key + '">' + field.label + '</label>' +
        '<span class="row-figure">' +
          '<input id="n-' + field.key + '" type="number" step="' + field.step + '" ' +
                 'min="' + field.min + '" max="' + field.max + '">' +
          '<span class="unit">' + field.unit + '</span>' +
        '</span>' +
        '<span class="rail"' + (field.neutral === undefined ? "" : ' data-detent style="--detent:' + detent(field) + '"') + '>' +
          '<input id="r-' + field.key + '" type="range" step="' + field.step + '" ' +
                 'min="' + field.min + '" max="' + field.max + '" tabindex="-1" aria-hidden="true">' +
        '</span>';
      host.appendChild(row);

      var number = row.querySelector("#n-" + field.key);
      var range = row.querySelector("#r-" + field.key);
      number.addEventListener("input", function () { apply(field, num(number.value, form[field.key]), false); });
      number.addEventListener("change", function () { apply(field, num(number.value, DEFAULTS[field.key]), true); });
      range.addEventListener("input", function () { apply(field, num(range.value, form[field.key]), true); });

      [number, range].forEach(function (input) {
        input.addEventListener("focus", function () { setReadout(field.hint); });
        input.addEventListener("mouseenter", function () { setReadout(field.hint); });
        input.addEventListener("blur", restoreReadout);
        input.addEventListener("mouseleave", restoreReadout);
      });
    });
  }

  function load(config) {
    stored = merged(config);
    form = merged(config);
    FIELDS.forEach(function (field) {
      document.getElementById("n-" + field.key).value = form[field.key].toFixed(field.dp);
      document.getElementById("r-" + field.key).value = form[field.key];
    });
    draw();
    restoreReadout();
    refreshState();
    labelRestore();
    setInputsEnabled(!context.readOnly);
  }

  // "Restore defaults" writes the plugin's own defaults globally, but in a preset it drops that
  // preset's override and falls back to the global configuration. Say which one the button does.
  function labelRestore() {
    var restore = document.getElementById("restore");
    var preset = context.scope === "preset";
    restore.textContent = preset ? "Use global settings" : "Restore defaults";
    restore.title = preset ? "Discard this preset's override and use the global configuration"
                           : "Replace the saved configuration with Twistify's defaults";
    restore.disabled = context.readOnly || (preset && !context.hasPresetOverride);
  }

  function setInputsEnabled(enabled) {
    FIELDS.forEach(function (field) {
      document.getElementById("n-" + field.key).disabled = !enabled;
      document.getElementById("r-" + field.key).disabled = !enabled;
    });
  }

  buildRows();
  load(window.orca ? window.orca.getConfig() : {});

  document.getElementById("save").addEventListener("click", function () {
    if (!window.orca) return;
    window.orca.saveConfig(form);
  });
  document.getElementById("restore").addEventListener("click", function () {
    if (window.orca) window.orca.restoreDefaults();
  });

  if (window.orca && window.orca.onConfig) {
    var first = true;
    window.orca.onConfig(function (config) {
      // Fires once immediately (already handled above) and then on every host save: reload from
      // what was stored, never from what was typed.
      if (first) { first = false; return; }
      if (window.orca.getContext) context = window.orca.getContext();
      load(config);
      announce("Saved");
    });
  }
  // No theme handler needed: every colour on the page, the drawing included, resolves through the
  // --orca-* variables the host rewrites in place.
})();
</script>
"""


class Twistify(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Twistify"

    def get_default_config(self):
        return _DEFAULTS

    def has_config_ui(self):
        return True

    def get_config_ui(self):
        return _CONFIG_UI.replace("__ORCA_DEFAULTS__", json.dumps(_DEFAULTS))

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
