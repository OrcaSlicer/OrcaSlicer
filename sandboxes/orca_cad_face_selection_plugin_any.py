# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "CAD Geometry Selection Demo"
# description = "Inspect native CAD picks, face boundaries, curves and topology through the Python API."
# author = "OrcaSlicer contributors"
# version = "0.2.0"
# ///
"""Run once to start watching, again to stop. Requires the CAD geometry-selection API."""

import orca

PAGE = """<!doctype html><html><head><style>
body { background: var(--orca-bg, #fff); color: var(--orca-fg, #222);
       font: 14px sans-serif; margin: 20px; }
pre { white-space: pre-wrap; }
</style></head><body>
<h3>Selected CAD geometry</h3>
<p>Pick a face, edge, vertex or whole body in the Design tab. Run the plugin again to stop watching.</p>
<pre id="face">Waiting for the plugin…</pre>
<script>
let ready = false;
orca.onMessage(message => {
  ready = true;
  document.getElementById('face').textContent = message.selection === null
    ? 'No CAD geometry selected.' : JSON.stringify(message.selection, null, 2);
});
// Wait for both the page and the Python window handle to exist before the initial snapshot.
const startup = setInterval(() => {
  if (ready) clearInterval(startup);
  else orca.postMessage({command: 'snapshot'});
}, 250);
</script></body></html>"""


class GeometryWatcher(orca.script.ScriptPluginCapabilityBase):
    window = None
    subscription = None

    def get_name(self):
        return "Inspect CAD geometry (start/stop)"

    def execute(self):
        if not hasattr(orca.host, "cad") or not hasattr(orca.host.cad, "selected_shape"):
            return orca.ExecutionResult.failure(
                orca.PluginResult.RecoverableError, "This build does not include the CAD geometry-selection API."
            )
        if self.subscription is not None and self.subscription.is_active():
            self.subscription.close()
            self.subscription = None
            if self.window is not None:
                self.window.close()
            self.window = None
            return orca.ExecutionResult.success("CAD geometry watcher stopped.")
        self.window = orca.host.ui.create_window(
            html=PAGE, title="CAD geometry selection", width=560, height=620,
            on_message=self.on_message, on_close=self.on_close,
        )
        self.subscription = orca.host.cad.on_selection_changed(self.show_selection)
        return orca.ExecutionResult.success("CAD geometry watcher started.")

    def on_message(self, message):
        if message.get("command") == "snapshot":
            self.show_selection(orca.host.cad.selected_shape())

    def show_selection(self, selection):
        window = self.window
        if window is None or not window.is_open():
            return
        if selection is None:
            window.post({"selection": None})
            return
        keys = ("kind", "body_index", "face_index", "edge_index", "vertex_index",
                "edge_indices", "selection_revision", "body_to_design")
        report = {key: getattr(selection, key) for key in keys}
        shape = selection.shape
        report["shape_type"] = shape.type.name
        report["orientation"] = shape.orientation
        try:
            if shape.type == orca.host.cad.ShapeType.Face:
                frame = shape.local_frame
                report.update(surface_type=shape.surface_type, area=shape.area,
                              centroid=shape.centroid, uv_bounds=shape.uv_bounds,
                              periodic=shape.periodic,
                              frame=None if frame is None else {
                                  key: getattr(frame, key) for key in ("origin", "x_axis", "y_axis", "normal")
                              })
                report["loops"] = [{
                    "is_outer": loop.is_outer, "is_complete": loop.is_complete,
                    "edges": [{"curve_type": edge.shape.curve_type,
                               "orientation": edge.shape.orientation,
                               "is_seam": edge.is_seam, "is_degenerate": edge.is_degenerate}
                              for edge in loop.edges],
                } for loop in shape.boundary_loops()]
            elif shape.type == orca.host.cad.ShapeType.Edge:
                report["curve_type"] = shape.curve_type
                if not shape.is_degenerate:
                    report["parameter_range"] = shape.parameter_range
                    report["sample_points"] = shape.sample_curve(9)
            elif shape.type == orca.host.cad.ShapeType.Vertex:
                report["point"] = shape.vertex_point
            else:
                report["topology_counts"] = {
                    kind.name: len(shape.subshapes(kind)) for kind in (
                        orca.host.cad.ShapeType.Face, orca.host.cad.ShapeType.Wire,
                        orca.host.cad.ShapeType.Edge, orca.host.cad.ShapeType.Vertex,
                    )
                }
        except (ValueError, orca.host.cad.GeometryError) as error:
            report["geometry_error"] = str(error)
        window.post({"selection": report})

    def on_close(self):
        if self.subscription is not None:
            self.subscription.close()
        self.subscription = None
        self.window = None

    def on_unload(self):
        self.on_close()


@orca.plugin
class CadGeometrySelectionDemo(orca.base):
    def register_capabilities(self):
        orca.register_capability(GeometryWatcher)
