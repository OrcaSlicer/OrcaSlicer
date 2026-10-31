# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "MCP Web Fixture"
# description = "Pages with every kind of control, for the MCP server's web page tests (tools/mcp/test_web_ui.py)."
# author = "OrcaSlicer contributors"
# version = "1.0.0"
# ///
"""The pages tools/mcp/test_web_ui.py drives through the MCP server's GUI tools.

Side-load it into a test data directory (test_web_ui.py --install DATADIR does) and run its scripts
from run_action: "MCP web fixture dialog" opens PAGE as a modal plugin dialog and writes what the
page submits to stderr, "MCP web fixture window" opens it as a modeless window, and "MCP web fixture
panel" docks a small page beside the 3D view (the Prepare tab). The dialog script's configuration UI (CONFIG_UI) is shown by
the Plugins dialog in its sandboxed frame. Each page logs what its controls saw into <pre id="log">,
which the test reads back. "MCP web fixture thread" starts a thread, for which the host asks the
threading permission in a native message box.
"""
import json
import sys
import threading

import orca

PAGE = r"""
<style>
  .card { cursor: pointer; border: 1px solid var(--orca-border); padding: 4px; margin: 4px 0; }
  .fancy input { opacity: 0; position: absolute; }
  .fancy { display: inline-block; padding: 2px 6px; border: 1px solid var(--orca-border); }
  #cover { position: relative; display: inline-block; }
  #veil { position: absolute; left: 0; top: 0; width: 100%; height: 100%; background: rgba(0, 0, 0, .3); }
  #scroller { height: 60px; overflow: auto; border: 1px solid var(--orca-border); }
  .gone { display: none; }
  canvas { border: 1px solid var(--orca-border); }
</style>
<h1 id="top">Fixture</h1>
<p>Some <b>bold</b> text in one paragraph.</p>
<label for="name">Name</label> <input id="name" placeholder="your name">
<label>Count <input id="count" type="number" min="0" max="10" value="3"></label>
<select id="colour" aria-label="Colour"><option>Red</option><option selected>Green</option><option>Blue</option></select>
<label><input id="agree" type="checkbox"> I agree</label>
<label class="fancy"><input id="fancy" type="checkbox"> Fancy</label>
<input type="radio" name="size" id="small" checked><label for="small">Small</label>
<input type="radio" name="size" id="large"><label for="large">Large</label>
<input id="level" type="range" min="0" max="100" value="20" aria-label="Level">
<div id="notes" contenteditable="true" aria-label="Notes">editable</div>
<input id="controlled" aria-label="Controlled">
<div class="card" id="card1" onclick="log('card1')"><span>Card one</span></div>
<div class="card" id="card2"><span>Card two</span></div>
<div class="opt unselected" onclick="log('opt a')">Opt A</div>
<div class="opt non-selected" onclick="log('opt c')">Opt C</div>
<div class="opt unSelected" onclick="log('opt d')">Opt D</div>
<div class="opt is-selected" onclick="log('opt b')">Opt B</div>
<div id="hscroll" style="width: 120px; overflow-x: auto; white-space: nowrap"><button id="leftmost" onclick="log('leftmost')">Leftmost</button><span style="display: inline-block; width: 1600px"></span></div>
<button id="off" disabled>Disabled</button>
<span id="cover"><button id="under" onclick="log('under')">Under</button><span id="veil"></span></span>
<button id="ok" onclick="log('ok')">OK</button>
<button id="slow">Slow</button>
<table><tr><th>Name</th><th>Size</th></tr><tr><td>Alpha</td><td>1</td></tr><tr><td>Beta</td><td>2</td></tr></table>
<table id="matrix"><thead><tr><th>from/to</th><th>1</th><th>2</th></tr></thead>
  <tbody><tr><th>1</th><td><input value="0"></td><td><input id="m12" value="91"></td></tr></tbody></table>
<div class="gone"><button>Hidden</button></div>
<div id="scroller"><p>Scroll me</p><div style="height: 300px"></div><button id="deep" onclick="log('deep')">Deep</button></div>
<p id="deeptext">Deep</p>
<input id="upload" type="file" aria-label="Upload">
<input id="watched" aria-label="Watched">
<input id="ro" readonly value="fixed" aria-label="Fixed">
<fieldset disabled><legend><input type="checkbox" id="unlock" aria-label="Unlock"></legend><input id="locked" aria-label="Locked"></fieldset>
<input id="ron" type="number" readonly value="5" aria-label="Fixed number">
<form id="f3" onsubmit="event.preventDefault(); log('f3 submitted')"><input id="need" required aria-label="Needed"></form>
<button class="twin" onclick="log('twin a')">Twin</button> <button class="twin" onclick="log('twin b')">Twin</button>
<form id="f2" onsubmit="event.preventDefault(); log('f2 submitted by ' + (event.submitter ? event.submitter.value : 'none'))">
  <input id="first" aria-label="First"><input id="second" aria-label="Second">
  <button id="go" value="go-value" onclick="log('go clicked')">Go</button>
</form>
<canvas id="pad" width="80" height="40" aria-label="Pad"></canvas>
<canvas id="cap" width="80" height="40" aria-label="Capture pad"></canvas>
<canvas id="slowpad" width="80" height="40" aria-label="Slow pad"
        onpointerdown="var t = Date.now(); while (Date.now() - t < 1500) {}" onpointerup="log('slow pad up')"></canvas>
<div id="host"><button id="slotted">Slotted</button></div>
<a id="plain">Plain anchor</a>
<iframe id="frame" srcdoc="<button id='inner' onclick='parent.log(&quot;inner&quot;)'>Inner</button>" style="height: 40px"></iframe>
<a id="jump" href="#bottom">Jump</a> <a id="up" href="#top">Up</a>
<button id="later" onclick="setTimeout(function () { var b = document.createElement('button'); b.id = 'made'; b.textContent = 'Made later'; document.body.appendChild(b); log('later ran'); }, 10)">Later</button>
<button id="closer">Close through wx</button>
<button id="closenow">Close at once</button>
<button id="stallcancel" onclick="var t = Date.now(); while (Date.now() - t < 5000) {}">Cancel</button>
<button id="slowshow" onclick="setTimeout(function () { var b = document.createElement('button'); b.textContent = 'Shown late'; b.onclick = function () { log('shown late clicked'); }; document.body.appendChild(b); }, 3500)">Slow show</button>
<button id="submit" onclick="orca.submit({name: document.getElementById('name').value, colour: document.getElementById('colour').value})">Submit</button>
<button id="nothing">Nothing</button>
<select id="extras" aria-label="Extras"><option>One</option></select>
<button id="addextra" onclick="var o = document.createElement('option'); o.text = 'Two'; document.getElementById('extras').add(o)">Add extra</button>
<a id="tokenlink" href="#orca-token=fixture-secret&amp;view=2">Token link</a>
<pre id="log"></pre>
<div id="bottom">Bottom</div>
<script>
  function log(s) { document.getElementById('log').textContent += s + '\n'; }
  document.getElementById('card2').addEventListener('click', function () { log('card2'); });
  // A control scrolled out to the left of its scroller, which a click scrolls back.
  document.getElementById('hscroll').scrollLeft = 1000;
  // Plain text with a hidden handler, repeating the Deep button's label.
  document.getElementById('deeptext').addEventListener('click', function () { log('deep text'); });
  document.getElementById('name').addEventListener('change', function (e) { log('name change ' + e.target.value); });
  document.getElementById('colour').addEventListener('change', function (e) { log('colour ' + e.target.value); });
  document.getElementById('agree').addEventListener('change', function (e) { log('agree ' + e.target.checked); });
  document.getElementById('fancy').addEventListener('change', function (e) { log('fancy ' + e.target.checked); });
  document.getElementById('watched').addEventListener('keyup', function (e) { log('keyup sees ' + e.target.value); });
  document.getElementById('slow').addEventListener('click', function () {
    var t = Date.now(); while (Date.now() - t < 3000) {} log('slow done');
  });
  var pad = document.getElementById('pad'), moves = 0, shifted = 0;
  pad.addEventListener('pointerdown', function (e) { moves = shifted = 0; log('pad down ' + Math.round(e.offsetX) + ',' + Math.round(e.offsetY)); });
  pad.addEventListener('pointermove', function (e) { if (e.buttons) { moves++; if (e.shiftKey) shifted++; } });
  pad.addEventListener('pointerup', function (e) {
    log('pad up ' + Math.round(e.offsetX) + ',' + Math.round(e.offsetY) + ' after ' + moves + ' moves');
    if (shifted)
      log('pad shifted ' + shifted);
  });
  // The colour picker's spectrum: capture on press, sample only while captured.
  var cap = document.getElementById('cap'), samples = 0;
  cap.addEventListener('pointerdown', function (e) { cap.setPointerCapture(e.pointerId); samples = 1; });
  cap.addEventListener('pointermove', function (e) { if (cap.hasPointerCapture(e.pointerId)) samples++; });
  cap.addEventListener('lostpointercapture', function () { log('cap samples ' + samples); });
  // A reference to the channel kept before the click, as some pages do.
  var channel = window.wx;
  document.getElementById('closer').addEventListener('click', function () {
    channel.postMessage(JSON.stringify({channel: 'orca', kind: 'close', data: null}));
  });
  // A bound function kept from before: the message goes out at once, not held, so the dialog can close
  // before the click's reply.
  var post = window.wx.postMessage.bind(window.wx);
  document.getElementById('closenow').addEventListener('click', function () {
    post(JSON.stringify({channel: 'orca', kind: 'close', data: null}));
  });
  var root = document.getElementById('host').attachShadow({mode: 'open'});
  root.innerHTML = '<button id="shadowbtn">Shadow</button><slot></slot>';
  root.getElementById('shadowbtn').addEventListener('click', function () { log('shadow'); });
  // A framework-style controlled input: its own state follows input events only.
  (function () {
    var el = document.getElementById('controlled');
    var proto = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
    var tracked = '';
    Object.defineProperty(el, 'value', {get: function () { return proto.get.call(this); },
                                        set: function (v) { tracked = v; proto.set.call(this, v); }});
    el.addEventListener('input', function () {
      if (proto.get.call(el) !== tracked) { tracked = proto.get.call(el); log('controlled ' + tracked); }
    });
  })();
</script>
"""

# Its Content-Security-Policy forbids eval, which the page agent doesn't need and webview_eval may
# (where the port holds injected scripts to the page's policy).
PANEL = r"""
<meta http-equiv="Content-Security-Policy" content="script-src 'unsafe-inline'">
<h3>Fixture panel</h3>
<button id="ping" onclick="document.getElementById('log').textContent += 'ping\n'">Ping</button>
<canvas id="dock" width="60" height="30" aria-label="Dock pad" onpointerup="document.getElementById('ups').textContent += 'dock up\n'"></canvas>
<pre id="log"></pre>
<pre id="ups"></pre>
"""

# Rewrite replaces every control with a new one, and Reload the frame's whole document, so refs to the
# old ones go stale either way.
CONFIG_UI = r"""
<div id="ui"></div>
<button id="reload" onclick="location.reload()">Reload</button>
<button class="twin" onclick="note('twin 1')">Refresh</button> <button class="twin" onclick="note('twin 2')">Refresh</button>
<button id="break" onclick="breakFor(3000)">Break</button>
<button id="mute" onclick="mute()">Mute</button>
<button id="stall" onclick="stallSoon()">Stall soon</button>
<pre id="log"></pre>
<script>
  function render(config) {
    document.getElementById('ui').innerHTML =
      '<label>Speed <input id="speed" type="number"></label>' +
      '<label><input id="enabled" type="checkbox"> Enabled</label>' +
      '<button id="save">Save</button> <button id="rewrite">Rewrite</button>';
    document.getElementById('speed').value = config.speed || 0;
    document.getElementById('enabled').checked = !!config.enabled;
    document.getElementById('save').onclick = function () {
      window.orca.saveConfig(read());
      document.getElementById('log').textContent += 'saved\n';
    };
    document.getElementById('rewrite').onclick = function () { render(read()); };
  }
  function read() {
    return {speed: Number(document.getElementById('speed').value), enabled: document.getElementById('enabled').checked};
  }
  function note(s) { document.getElementById('log').textContent += s + '\n'; }
  document.addEventListener('pointerup', function () { note('frame up'); });
  // For a while every page agent call here fails (it reads styles to walk the page), as on a page
  // that breaks what the agent uses.
  function breakFor(ms) {
    var styles = window.getComputedStyle;
    window.getComputedStyle = function () { throw new Error('styles broken by the page'); };
    setTimeout(function () { window.getComputedStyle = styles; }, ms);
  }
  // From then on the frame answers none of the page's relayed requests, as a frame that hangs on its
  // own does (a busy frame here shares the page's thread, so it would hold the page too). The bridge
  // answers only messages from `parent`, a replaceable property, so a stand-in turns it away.
  function mute() {
    window.parent = {};
  }
  // Busy for 4 s from 3 s on, time enough to leave a press here first.
  function stallSoon() {
    setTimeout(function () { var t = Date.now(); while (Date.now() - t < 4000) {} }, 3000);
  }
  render(window.orca.getConfig() || {});
</script>
"""


class FixtureDialog(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP web fixture dialog"

    def has_config_ui(self):
        return True

    def get_config_ui(self):
        return CONFIG_UI

    def get_default_config(self):
        return {"speed": 0, "enabled": False}

    def execute(self):
        # The modal dialog opens once this returns, from the host's next event loop turn.
        orca.host.ui.create_window(html=PAGE, title="MCP web fixture", width=760, height=900,
                                   style=orca.host.ui.WINDOW_MODAL, on_submit=self._on_submit)
        return orca.ExecutionResult.success("opened")

    def _on_submit(self, data):
        # The host tees stderr to <datadir>/log/python_*.log, where the test reads it.
        print("MCP web fixture submitted " + json.dumps(data, sort_keys=True), file=sys.stderr, flush=True)


class FixtureWindow(orca.script.ScriptPluginCapabilityBase):
    window = None

    def get_name(self):
        return "MCP web fixture window"

    def execute(self):
        if self.window is None or not self.window.is_open():
            self.window = orca.host.ui.create_window(html=PAGE, title="MCP web fixture window", width=760, height=900)
        return orca.ExecutionResult.success("opened")


class FixtureThread(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "MCP web fixture thread"

    def execute(self):
        # It runs on the UI thread, where the host asks for the threading permission in a modal native
        # message box; a refusal raises PermissionError, which the action's result reports.
        threading.Thread(target=lambda: None).start()
        return orca.ExecutionResult.success("thread started")


class FixturePanel(orca.script.ScriptPluginCapabilityBase):
    panel = None

    def get_name(self):
        return "MCP web fixture panel"

    def execute(self):
        if self.panel is None:
            self.panel = orca.host.ui.create_dock_panel(html=PANEL, title="MCP web fixture panel", width=260, height=200, dock="right")
        return orca.ExecutionResult.success("opened")


@orca.plugin
class FixturePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(FixtureDialog)
        orca.register_capability(FixtureWindow)
        orca.register_capability(FixtureThread)
        orca.register_capability(FixturePanel)
