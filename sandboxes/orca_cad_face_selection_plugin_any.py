# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "CAD Face Selection Demo"
# description = "Watch native Design-tab face selections through the Python host API."
# author = "OrcaSlicer contributors"
# version = "0.1.0"
# ///
"""Run once to start watching, again to stop. Requires the CAD face-selection API."""

import orca

PAGE = """<!doctype html><html><head><style>
body { background: var(--orca-bg, #fff); color: var(--orca-fg, #222);
       font: 14px sans-serif; margin: 20px; }
pre { white-space: pre-wrap; }
</style></head><body>
<h3>Selected CAD face</h3>
<p>Pick a face in the Design tab. Run the plugin again to stop watching.</p>
<pre id="face">Waiting for the plugin…</pre>
<script>
let ready = false;
orca.onMessage(message => {
  ready = true;
  document.getElementById('face').textContent = message.face === null
    ? 'No CAD face selected.' : JSON.stringify(message.face, null, 2);
});
// Wait for both the page and the Python window handle to exist before the initial snapshot.
const startup = setInterval(() => {
  if (ready) clearInterval(startup);
  else orca.postMessage({command: 'snapshot'});
}, 250);
</script></body></html>"""


class FaceWatcher(orca.script.ScriptPluginCapabilityBase):
    window = None
    subscription = None

    def get_name(self):
        return "Watch CAD face selection (start/stop)"

    def execute(self):
        if not hasattr(orca.host, "cad"):
            return orca.ExecutionResult.failure(
                orca.PluginResult.RecoverableError, "This build does not include the CAD face-selection API."
            )
        if self.subscription is not None and self.subscription.is_active():
            self.subscription.close()
            self.subscription = None
            if self.window is not None:
                self.window.close()
            self.window = None
            return orca.ExecutionResult.success("CAD face watcher stopped.")
        self.window = orca.host.ui.create_window(
            html=PAGE, title="CAD face selection", width=560, height=620,
            on_message=self.on_message, on_close=self.on_close,
        )
        self.subscription = orca.host.cad.on_face_selection_changed(self.show_face)
        return orca.ExecutionResult.success("CAD face watcher started.")

    def on_message(self, message):
        if message.get("command") == "snapshot":
            self.show_face(orca.host.cad.selected_face())

    def show_face(self, face):
        window = self.window
        if window is None or not window.is_open():
            return
        properties = (
            "body_index", "face_index", "selection_revision", "centroid",
            "normal", "area", "is_planar", "body_to_design",
        )
        window.post({"face": None if face is None else {key: getattr(face, key) for key in properties}})

    def on_close(self):
        if self.subscription is not None:
            self.subscription.close()
        self.subscription = None
        self.window = None

    def on_unload(self):
        self.on_close()


@orca.plugin
class CadFaceSelectionDemo(orca.base):
    def register_capabilities(self):
        orca.register_capability(FaceWatcher)
