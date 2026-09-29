# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Dock Panel Demo"
# description = "Opens a dockable panel beside the 3D view that lists the objects on the plate."
# author = "OrcaSlicer"
# version = "0.0.1"
# ///
"""Dock Panel Demo -- orca.host.ui.create_dock_panel().

Run it from the Plugins dialog. It opens an HTML panel docked on the right of the 3D view, in the
same dock area as the sidebar. Drag its caption to dock it on another side (or float it, where the
platform allows), hide it from the page and run the plugin again to bring it back, or close it with
its close button or from the page.

  page   --orca.postMessage({command: 'refresh'})-->  plugin.on_message()
  page   --orca.postMessage({command: 'hide'})-->     plugin.on_message() -> panel.hide()
  page   --orca.close()-->                            panel closes, plugin.on_close()
  plugin --panel.post({command: 'objects', ...})-->   page (orca.onMessage)
"""
import orca

PAGE = """
<style>
  body { margin: 0; padding: 12px; font-size: 13px; }
  h3 { margin: 0 0 4px; font-size: 14px; }
  .note { margin: 0 0 12px; color: var(--orca-muted); font-size: 12px; }
  .actions { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 12px; }
  .actions button.quiet { background: transparent; color: var(--orca-fg); border-color: var(--orca-border); }
  table { width: 100%; border-collapse: collapse; }
  td.count { text-align: right; font-variant-numeric: tabular-nums; }
  #status { margin-top: 10px; color: var(--orca-muted); font-size: 12px; }
</style>

<h3>Objects on the plate</h3>
<p class="note">Docked beside the 3D view. Drag the caption to move it.</p>

<div class="actions">
  <button type="button" id="refresh">Refresh</button>
  <button type="button" id="hide" class="quiet">Hide</button>
  <button type="button" id="close" class="quiet">Close</button>
</div>

<table>
  <thead><tr><th>Name</th><th>Parts</th><th>Copies</th></tr></thead>
  <tbody id="rows"></tbody>
</table>
<p id="status">Waiting for the plugin...</p>

<script>
(function () {
  function text(value) {
    var span = document.createElement("span");
    span.textContent = value;
    return span.innerHTML;
  }

  function render(message) {
    var rows = document.getElementById("rows");
    var status = document.getElementById("status");
    if (message.error) {
      rows.innerHTML = "";
      status.textContent = message.error;
      return;
    }
    rows.innerHTML = message.objects.map(function (object) {
      return "<tr><td>" + text(object.name) + "</td><td class=\\"count\\">" + object.volumes +
             "</td><td class=\\"count\\">" + object.instances + "</td></tr>";
    }).join("");
    status.textContent = message.objects.length + " object(s), refreshed " + new Date().toLocaleTimeString();
  }

  orca.onMessage(function (message) {
    if (message && message.command === "objects")
      render(message);
  });

  document.getElementById("refresh").addEventListener("click", function () {
    orca.postMessage({ command: "refresh" });
  });
  document.getElementById("hide").addEventListener("click", function () {
    orca.postMessage({ command: "hide" });
  });
  document.getElementById("close").addEventListener("click", function () {
    orca.close();
  });

  orca.postMessage({ command: "refresh" });
})();
</script>
"""


def plate_objects():
    try:
        model = orca.host.model()
    except RuntimeError as error:
        return {"command": "objects", "error": str(error)}
    return {
        "command": "objects",
        "objects": [
            {"name": obj.name or "(unnamed)", "volumes": obj.volume_count(), "instances": obj.instance_count()}
            for obj in model.objects()
        ],
    }


class DockPanelDemo(orca.script.ScriptPluginCapabilityBase):
    panel = None

    def get_name(self):
        return "Dock Panel Demo"

    def execute(self):
        # The capability instance lives as long as the plugin, so a second run finds the open panel.
        if self.panel is not None and self.panel.is_open():
            self.panel.show()
            return orca.ExecutionResult.success("Dock Panel Demo is already open.")
        self.panel = orca.host.ui.create_dock_panel(
            html=PAGE,
            title="Dock Panel Demo",
            width=320,
            height=480,
            on_message=self.on_message,
            on_close=self.on_close,
            dock="right",
        )
        return orca.ExecutionResult.success("Dock Panel Demo opened.")

    # Called on the UI thread when the page posts.
    def on_message(self, message):
        command = (message or {}).get("command")
        if command == "refresh":
            self.panel.post(plate_objects())
        elif command == "hide":
            self.panel.hide()

    def on_close(self):
        self.panel = None


@orca.plugin
class DockPanelDemoPlugin(orca.base):
    def register_capabilities(self):
        orca.register_capability(DockPanelDemo)
