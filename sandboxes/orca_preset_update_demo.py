# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# id = "preset-update-demo"
# name = "Preset Update Demo"
# description = "Browse and edit settings in process, printer, and filament presets."
# author = "OrcaSlicer contributors"
# version = "0.4.0"
# type = "script"
# ///
"""Interactive preset editor. Snapshots are read on the UI thread; edits run on execute's worker."""

import queue
import orca


PAGE = r'''<!doctype html><html><head><meta charset="utf-8"><style>
:root{color-scheme:light dark;--a:#087f78;--line:rgba(127,127,127,.25);--soft:rgba(127,127,127,.1)}
*{box-sizing:border-box}body{margin:0;font:13px/1.45 var(--orca-font,system-ui,sans-serif);display:flex;flex-direction:column;height:100vh;color:var(--orca-fg,#20242a);background:var(--orca-bg,#fafafa)}
header{padding:14px 18px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:12px}h1{font-size:16px;margin:0}.brand{color:var(--a);font-size:20px}select,input,textarea,button{font:inherit;color:inherit}select,input,textarea{background:var(--orca-bg,#fafafa);border:1px solid var(--line);border-radius:7px;padding:7px 9px}header select{margin-left:auto;min-width:140px}.bar{padding:10px 18px;display:flex;gap:10px;align-items:center;border-bottom:1px solid var(--line)}.bar select{min-width:220px}.bar label{font-size:11px;text-transform:uppercase;letter-spacing:.06em;opacity:.65}.badge{padding:3px 9px;border:1px solid var(--line);border-radius:20px;opacity:.7}.layout{display:grid;grid-template-columns:minmax(230px,42%) 1fr;min-height:0;flex:1}.list{border-right:1px solid var(--line);display:flex;flex-direction:column;min-height:0}.search{padding:9px;border-bottom:1px solid var(--line)}.search input{width:100%}#rows{overflow:auto;padding:4px}.row{width:100%;display:flex;gap:12px;padding:7px 9px;border:0;border-left:3px solid transparent;background:transparent;text-align:left;cursor:pointer;border-radius:3px}.row:hover,.row.sel{background:var(--soft)}.row.sel{border-left-color:var(--a)}.key{flex:1;font-family:ui-monospace,monospace;overflow:hidden;text-overflow:ellipsis}.val{max-width:45%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;opacity:.65;font-family:ui-monospace,monospace}.editor{padding:20px;display:flex;flex-direction:column;gap:10px;overflow:auto}.editor h2{font:600 15px ui-monospace,monospace;margin:0;overflow-wrap:anywhere}.muted{opacity:.65;font-size:11px;text-transform:uppercase;letter-spacing:.06em}.current{font-family:ui-monospace,monospace;background:var(--soft);padding:10px;border-radius:7px;overflow-wrap:anywhere;white-space:pre-wrap}textarea{min-height:88px;resize:vertical;font-family:ui-monospace,monospace}.actions{display:flex;gap:8px}button.action{padding:7px 15px;border-radius:7px;border:1px solid var(--line);background:transparent;cursor:pointer}button.primary{background:var(--a);border-color:var(--a);color:white}button:disabled{opacity:.45;cursor:default}.hint{margin-top:auto;padding:10px;border-left:3px solid var(--a);background:var(--soft);border-radius:4px}.status{padding:8px 18px;border-top:1px solid var(--line);opacity:.75}.error{color:#c33}.empty{padding:20px;opacity:.65}@media(max-width:650px){.layout{grid-template-columns:1fr 1fr}.bar{flex-wrap:wrap}.bar select{min-width:120px;flex:1}}
</style></head><body>
<header><span class="brand">◈</span><h1>Preset editor</h1><select id="kind"><option value="process">Process</option><option value="printer">Printer</option><option value="filament">Filament</option></select><button class="action" id="close">Close</button></header>
<div class="bar"><label id="slotlabel" hidden>Slot</label><select id="slot" hidden></select><label>Preset</label><select id="preset"></select><span class="badge" id="dirty"></span><button class="action" id="reload">Refresh</button></div>
<div class="layout"><section class="list"><div class="search"><input id="search" placeholder="Search settings…"></div><div id="rows"></div></section><section class="editor"><div class="muted">Selected setting</div><h2 id="key">Choose a setting</h2><div class="muted">Current value</div><div class="current" id="current">—</div><label class="muted" for="value">New serialized value</label><textarea id="value" disabled></textarea><div class="actions"><button class="action primary" id="apply" disabled>Apply change</button><button class="action" id="reset" disabled>Reset</button></div><div class="hint">Choose a category, preset and filament slot. Edit any listed key, then apply. Orca may ask to switch presets or resolve unsaved changes. Values stay unsaved until saved in Orca.</div></section></div><footer class="status" id="status">Loading presets…</footer>
<script>
const $=id=>document.getElementById(id),S={kind:'process',slot:1,preset:'',rows:[],selected:null,busy:true,notice:'',noticeError:false};
function send(command,extra={}){$('status').textContent='Working…';S.busy=true;controls();orca.postMessage({command,kind:S.kind,slot:S.slot,preset:$('preset').value,...extra})}
function controls(){ $('kind').disabled=S.busy;$('slot').disabled=S.busy;$('preset').disabled=S.busy;$('apply').disabled=S.busy||!S.selected;$('reload').disabled=S.busy }
function load(){send('state')}
function paint(m){const notice=S.notice,noticeError=S.noticeError;S.notice='';S.noticeError=false;S.busy=false;S.kind=m.kind;S.slot=m.slot||1;S.rows=m.rows||[];S.selected=S.rows.some(r=>r.k===S.selected)?S.selected:null;
 $('slotlabel').hidden=S.kind!=='filament';$('slot').hidden=S.kind!=='filament';$('slot').innerHTML=(m.slots||[]).map(x=>`<option value="${x.slot}">Slot ${x.slot} · ${esc(x.name||'Empty')}</option>`).join('');$('slot').value=S.slot;
 $('preset').innerHTML=(m.presets||[]).map(x=>`<option value="${esc(x)}">${esc(x)}</option>`).join('');$('preset').value=m.preset_name||'';$('dirty').textContent=m.dirty?'Modified':'Saved';
 const q=$('search').value.toLowerCase(),rows=S.rows.filter(r=>(r.k+' '+r.v).toLowerCase().includes(q));$('rows').innerHTML=rows.map(r=>`<button class="row ${S.selected===r.k?'sel':''}" data-key="${esc(r.k)}"><span class="key">${esc(r.k)}</span><span class="val">${esc(r.v)}</span></button>`).join('')||'<div class="empty">No matching settings</div>';
 const r=S.rows.find(x=>x.k===S.selected);$('key').textContent=r?r.k:'Choose a setting';$('current').textContent=r?r.v:'—';$('value').disabled=!r;$('value').value=r?r.v:'';$('reset').disabled=!r;controls();$('status').textContent=m.error||notice||m.message||`${S.rows.length} settings · changes remain unsaved`;$('status').className='status'+((m.error||noticeError)?' error':'') }
function esc(x){return String(x??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
$('kind').onchange=()=>{S.kind=$('kind').value;S.selected=null;$('preset').value='';load()};$('slot').onchange=()=>{S.slot=Number($('slot').value);S.selected=null;$('preset').value='';load()};$('preset').onchange=()=>{S.selected=null;load()};$('reload').onclick=load;$('close').onclick=()=>orca.close();$('search').oninput=()=>paint({kind:S.kind,slot:S.slot,rows:S.rows,presets:[...$('preset').options].map(o=>o.value),preset_name:$('preset').value});
$('rows').onclick=e=>{let b=e.target.closest('[data-key]');if(!b)return;S.selected=b.dataset.key;let r=S.rows.find(x=>x.k===S.selected);$('key').textContent=r.k;$('current').textContent=r.v;$('value').value=r.v;$('value').disabled=false;$('reset').disabled=false;$('apply').disabled=false;document.querySelectorAll('.row').forEach(x=>x.classList.toggle('sel',x===b))};$('reset').onclick=()=>{let r=S.rows.find(x=>x.k===S.selected);if(r)$('value').value=r.v};$('apply').onclick=()=>send('apply',{key:S.selected,value:$('value').value});orca.onMessage(m=>{if(m.command==='state')paint(m);else if(m.command==='refresh'){S.notice=m.error||m.message||'';S.noticeError=!!m.error;load()}});load();
</script></body></html>'''


class PresetUpdateDemo(orca.script.ScriptPluginCapabilityBase):
    window = None
    _commands = None
    _cancelled = False

    def get_name(self):
        return "Preset editor demo"

    def execute(self):
        if self.window is not None and self.window.is_open():
            return orca.ExecutionResult.success("Preset editor is already open.")
        # The queue exists before the window so on_cancelled() can always unblock this worker, even
        # when a forced unload destroys the window without firing on_close.
        commands = queue.Queue()
        self._commands = commands
        try:
            if self._cancelled:
                return orca.ExecutionResult.success("Preset editor closed.")
            self.window = orca.host.ui.create_window(
                title="Preset editor", html=PAGE, width=900, height=620,
                on_message=self._on_message,
                on_close=self._on_close,
            )
            while True:
                message = commands.get()
                if self._cancelled or message.get("command") == "close":
                    return orca.ExecutionResult.success("Preset editor closed.")
                try:
                    notice = {"command": "refresh", "message": self._apply(message)}
                except Exception as error:
                    notice = {"command": "refresh", "error": str(error)}
                if self.window is not None and self.window.is_open():
                    self.window.post(notice)
        except Exception as error:
            return orca.ExecutionResult.failure(orca.PluginResult.RecoverableError,
                                                "Preset editor failed: %s" % error)
        finally:
            if self._commands is commands:
                self._commands = None
            self.window = None

    # Both UI callbacks run on the wx main thread with the GIL held.
    def _on_message(self, message):
        message = dict(message) if isinstance(message, dict) else {}
        if message.get("command") == "apply":
            self._enqueue(message)
            return
        try:
            state = self._snapshot(message)
        except Exception as error:
            state = {"command": "state", "kind": message.get("kind", "process"),
                     "slot": message.get("slot", 1), "rows": [], "presets": [], "slots": [],
                     "preset_name": "", "dirty": False, "error": str(error)}
        window = self.window
        if window is not None and window.is_open():
            window.post(state)

    def _on_close(self):
        self._enqueue({"command": "close"})

    def on_cancelled(self):
        # Called before the unload drain. Enqueueing a close lets a worker parked in get() exit; a
        # callback that arrives after the worker already left sees no queue and is harmless.
        self._cancelled = True
        self._enqueue({"command": "close"})

    def _enqueue(self, message):
        commands = self._commands
        if commands is not None:
            commands.put(message)

    def _apply(self, message):
        kind = message.get("kind", "process")
        if kind not in ("process", "printer", "filament"):
            raise ValueError("Unknown preset category")
        name = message.get("preset") or ""
        if not isinstance(name, str):
            raise ValueError("Preset name must be a string")
        key, value = message.get("key"), message.get("value")
        if not isinstance(key, str) or not key or not isinstance(value, str):
            raise ValueError("Setting key and serialized value required")
        editor = getattr(orca.host.preset, kind)
        if kind == "filament":
            slot = int(message.get("slot", 1))
            if slot < 1:
                raise ValueError("Filament slot numbers are 1-based")
            transaction = editor.begin_update(slot, name)
        else:
            transaction = editor.begin_update(name)
        transaction.set(key, value)
        result = transaction.commit()
        status = "Set %s on %s. Changes remain unsaved." % (key, result.target_name)
        if result.selection_changed:
            status += " Orca switched the active preset."
        if kind == "filament" and len(result.shared_slots) > 1:
            status += " Shared by filament slots: %s." % ", ".join(str(s) for s in result.shared_slots)
        return status

    # Read-only snapshot. Runs on the UI thread so no live Preset or PresetBundle object is read
    # from the Script worker, and never calls begin_update() or another blocking preset operation.
    def _snapshot(self, message):
        kind = message.get("kind", "process")
        if kind not in ("process", "printer", "filament"):
            raise ValueError("Unknown preset category")
        try:
            slot = int(message.get("slot", 1))
        except (TypeError, ValueError):
            slot = 1
        requested = message.get("preset") or ""
        if not isinstance(requested, str):
            requested = ""
        bundle = orca.host.preset_bundle()
        slots = []
        if kind == "filament":
            assigned_names = list(bundle.current_filament_preset_names())
            presets = list(bundle.current_filament_presets())
            slots = [{"slot": index + 1,
                      "name": presets[index].name if presets[index] is not None
                              else (assigned_names[index] or None)}
                     for index in range(len(presets))]
            if not 1 <= slot <= len(presets):
                slot = 1
            preset = bundle.filaments.find_preset(requested) if requested else None
            if preset is None and presets:
                preset = presets[slot - 1]
            available = list(bundle.filaments.preset_names())
        else:
            collection = bundle.prints if kind == "process" else bundle.printers
            available = list(collection.preset_names())
            preset = collection.find_preset(requested) if requested else None
            if preset is None:
                preset = (bundle.current_process_preset() if kind == "process"
                          else bundle.current_printer_preset())
        rows = []
        if preset is not None:
            for key in sorted(preset.config_keys()):
                value = preset.config_value(key)
                rows.append({"k": key, "v": "" if value is None else str(value)})
        return {"command": "state", "kind": kind, "slot": slot, "slots": slots,
                "presets": available, "preset_name": preset.name if preset is not None else "",
                "dirty": bool(preset.is_dirty) if preset is not None else False, "rows": rows}


@orca.plugin
class PresetUpdateDemoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(PresetUpdateDemo)
