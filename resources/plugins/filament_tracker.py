# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Filament Tracker"
# description = "Keeps an inventory of your spools and subtracts the filament each print uses."
# author = "Unified Slicer"
# version = "0.1.0"
# ///
"""Filament Tracker.

  * A dock panel lists your spools: brand, material, colour, linked filament preset, how much is
    left, and cost. Add, edit, weigh (gross weight minus empty-spool weight) or archive them.
  * Each filament slot (1..N) of the current project can be assigned a spool. Slots whose filament
    preset matches a spool's linked preset are assigned automatically.
  * Every G-code export or upload is captured as a job with the grams used per slot (read from the
    G-code's "; filament used [g]" footer). Mark a job Printed to subtract it from the assigned
    spools, Failed to subtract only the part printed, or Discard it. Printed jobs can be undone.
  * Optional: subtract automatically on every upload to a printer.
  * Low-stock warning when a job needs more than a spool has left.

Data lives in filament_tracker_data/spools.json next to this file, so it survives updates.
"""
from __future__ import annotations

import hashlib
import json
import re
import threading
import time
import uuid
from pathlib import Path

try:
    import orca  # type: ignore
except Exception:
    orca = None

DATA_DIR = Path(__file__).resolve().parent / "filament_tracker_data"
DB_FILE = DATA_DIR / "spools.json"
_lock = threading.RLock()

DEFAULT_DB = {"version": 1, "spools": [], "slots": {}, "jobs": [],
              "options": {"auto_deduct_on_upload": False, "low_stock_g": 50}}


# ------------------------------------------------------------------------------------------------
# Storage
# ------------------------------------------------------------------------------------------------

def load_db() -> dict:
    with _lock:
        try:
            db = json.loads(DB_FILE.read_text(encoding="utf-8"))
        except Exception:
            db = json.loads(json.dumps(DEFAULT_DB))
        for k, v in DEFAULT_DB.items():
            db.setdefault(k, json.loads(json.dumps(v)))
        for k, v in DEFAULT_DB["options"].items():
            db["options"].setdefault(k, v)
        return db


def save_db(db: dict) -> None:
    with _lock:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        tmp = DB_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(db, indent=1), encoding="utf-8")
        tmp.replace(DB_FILE)


def _num(v, default=0.0) -> float:
    try:
        return float(v)
    except Exception:
        return default


def find_spool(db: dict, spool_id: str) -> dict | None:
    return next((s for s in db["spools"] if s["id"] == spool_id), None)


def upsert_spool(db: dict, data: dict) -> dict:
    spool = find_spool(db, data.get("id") or "") if data.get("id") else None
    if spool is None:
        spool = {"id": uuid.uuid4().hex[:12], "created": int(time.time()), "archived": False}
        db["spools"].append(spool)
    for key in ("name", "brand", "material", "color", "preset", "notes"):
        if key in data:
            spool[key] = str(data[key] or "").strip()
    for key in ("total_g", "remaining_g", "tare_g", "cost_per_kg", "diameter"):
        if key in data:
            spool[key] = round(_num(data[key]), 2)
    spool.setdefault("total_g", 1000.0)
    spool.setdefault("remaining_g", spool["total_g"])
    spool.setdefault("tare_g", 0.0)
    spool.setdefault("diameter", 1.75)
    if not spool.get("name"):
        spool["name"] = " ".join(x for x in (spool.get("brand", ""), spool.get("material", ""), spool.get("color", "")) if x) or "Spool"
    return spool


def weigh_spool(db: dict, spool_id: str, gross_g: float) -> None:
    spool = find_spool(db, spool_id)
    if spool is None:
        raise ValueError("Spool not found.")
    spool["remaining_g"] = round(max(0.0, gross_g - _num(spool.get("tare_g"))), 2)


# ------------------------------------------------------------------------------------------------
# G-code parsing
# ------------------------------------------------------------------------------------------------

_FOOTER_KEYS = {
    "grams": re.compile(r"^;\s*filament used \[g\]\s*=\s*(.+)$"),
    "mm": re.compile(r"^;\s*filament used \[mm\]\s*=\s*(.+)$"),
    "presets": re.compile(r"^;\s*filament_settings_id\s*=\s*(.+)$"),
    "types": re.compile(r"^;\s*filament_type\s*=\s*(.+)$"),
    "colours": re.compile(r"^;\s*filament_colour\s*=\s*(.+)$"),
    "cost": re.compile(r"^;\s*filament cost\s*=\s*(.+)$"),
}


def _split_list(value: str) -> list[str]:
    value = value.strip()
    if value.startswith('"'):
        return [x.strip().strip('"') for x in re.findall(r'"[^"]*"', value)]
    sep = ";" if ";" in value else ","
    return [x.strip() for x in value.split(sep)]


def parse_gcode_usage(path: Path) -> dict:
    """Reads the per-filament usage Orca writes at the head and tail of a G-code file."""
    found: dict[str, list[str]] = {}
    size = path.stat().st_size
    with path.open("rb") as fh:
        chunks = [fh.read(256 * 1024)]
        if size > 512 * 1024:
            fh.seek(size - 512 * 1024)
        chunks.append(fh.read())
    for chunk in chunks:
        for raw in chunk.decode("utf-8", "replace").splitlines():
            if not raw.startswith(";"):
                continue
            for key, rx in _FOOTER_KEYS.items():
                m = rx.match(raw)
                if m and key not in found:
                    found[key] = _split_list(m.group(1))
    grams = [_num(x) for x in found.get("grams", [])]
    n = len(grams)
    pad = lambda lst: (lst + [""] * n)[:n]
    return {"grams": grams, "mm": [_num(x) for x in found.get("mm", [])][:n] or [0.0] * n,
            "presets": pad(found.get("presets", [])), "types": pad(found.get("types", [])),
            "colours": pad(found.get("colours", []))}


def _file_md5(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ------------------------------------------------------------------------------------------------
# Jobs
# ------------------------------------------------------------------------------------------------

def auto_assign_slots(db: dict, presets: list[str]) -> None:
    """Assign a spool to every empty slot whose filament preset matches a spool's linked preset."""
    for idx, preset in enumerate(presets, start=1):
        if not preset or db["slots"].get(str(idx)):
            continue
        match = [s for s in db["spools"] if not s.get("archived") and s.get("preset") == preset and s.get("remaining_g", 0) > 0]
        if match:
            match.sort(key=lambda s: s.get("remaining_g", 0))  # use up the emptiest first
            db["slots"][str(idx)] = match[0]["id"]


def add_job(db: dict, usage: dict, name: str, host: str, md5: str) -> dict | None:
    if not any(g > 0 for g in usage["grams"]):
        return None
    for job in db["jobs"]:
        if job.get("md5") == md5 and job["status"] == "pending":
            return None  # same file captured twice (export + upload)
    auto_assign_slots(db, usage["presets"])
    job = {"id": uuid.uuid4().hex[:12], "time": int(time.time()), "name": name, "host": host, "md5": md5,
           "grams": usage["grams"], "presets": usage["presets"], "types": usage["types"],
           "colours": usage["colours"], "status": "pending", "deducted": {}}
    db["jobs"].insert(0, job)
    del db["jobs"][200:]
    return job


def low_stock_warnings(db: dict, job: dict) -> list[str]:
    out = []
    for idx, grams in enumerate(job["grams"], start=1):
        spool = find_spool(db, db["slots"].get(str(idx), ""))
        if grams > 0 and spool is not None and spool.get("remaining_g", 0) < grams:
            out.append("Slot %d: %s has %.0f g left, this print needs %.0f g." %
                       (idx, spool["name"], spool.get("remaining_g", 0), grams))
    return out


def complete_job(db: dict, job_id: str, fraction: float = 1.0) -> dict:
    job = next((j for j in db["jobs"] if j["id"] == job_id), None)
    if job is None:
        raise ValueError("Job not found.")
    if job["status"] in ("printed", "failed"):
        raise ValueError("This job was already subtracted.")
    fraction = min(1.0, max(0.0, fraction))
    deducted: dict[str, float] = {}
    for idx, grams in enumerate(job["grams"], start=1):
        spool = find_spool(db, db["slots"].get(str(idx), ""))
        if spool is None or grams <= 0:
            continue
        used = round(grams * fraction, 2)
        spool["remaining_g"] = round(max(0.0, spool.get("remaining_g", 0) - used), 2)
        deducted[spool["id"]] = round(deducted.get(spool["id"], 0) + used, 2)
    job["deducted"] = deducted
    job["status"] = "printed" if fraction >= 1.0 else "failed"
    job["fraction"] = fraction
    return job


def undo_job(db: dict, job_id: str) -> None:
    job = next((j for j in db["jobs"] if j["id"] == job_id), None)
    if job is None:
        raise ValueError("Job not found.")
    for spool_id, grams in job.get("deducted", {}).items():
        spool = find_spool(db, spool_id)
        if spool is not None:
            spool["remaining_g"] = round(spool.get("remaining_g", 0) + grams, 2)
    job["deducted"] = {}
    job["status"] = "pending"


def stats(db: dict) -> dict:
    active = [s for s in db["spools"] if not s.get("archived")]
    value = sum(_num(s.get("remaining_g")) / 1000.0 * _num(s.get("cost_per_kg")) for s in active)
    used = sum(sum(j.get("deducted", {}).values()) for j in db["jobs"])
    return {"spools": len(active), "remaining_g": round(sum(_num(s.get("remaining_g")) for s in active), 1),
            "value": round(value, 2), "used_g": round(used, 1)}


# ------------------------------------------------------------------------------------------------
# Panel
# ------------------------------------------------------------------------------------------------

PAGE = r"""
<style>
  body { margin: 0; padding: 10px; font-size: 12px; }
  h3 { margin: 10px 0 6px; font-size: 13px; }
  .muted { color: var(--orca-muted); }
  .row { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }
  .spool { border: 1px solid var(--orca-border); border-radius: 6px; padding: 6px 8px; margin: 6px 0; }
  .spool.low { border-color: #d9822b; }
  .sw { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--orca-border); display: inline-block; }
  .bar { height: 5px; background: var(--orca-border); border-radius: 3px; margin-top: 4px; overflow: hidden; }
  .bar > i { display: block; height: 100%; background: var(--orca-accent, #009688); }
  .grow { flex: 1; }
  input, select { font-size: 12px; }
  input.n { width: 64px; }
  button.quiet { background: transparent; color: var(--orca-fg); border-color: var(--orca-border); }
  table { width: 100%; border-collapse: collapse; }
  td { padding: 2px 3px; vertical-align: top; }
  #form { display: none; border: 1px solid var(--orca-border); border-radius: 6px; padding: 8px; }
  #form label { display: block; margin: 4px 0 1px; }
  #form input, #form select { width: 100%; box-sizing: border-box; }
  #status { margin-top: 8px; }
</style>
<div class="row"><b class="grow">Filament Tracker</b><span id="stats" class="muted"></span></div>

<h3>Slots</h3>
<table id="slots"></table>

<h3 class="row"><span class="grow">Jobs</span></h3>
<table id="jobs"></table>
<label class="row muted"><input type="checkbox" id="auto"> Subtract automatically on upload to a printer</label>

<h3 class="row"><span class="grow">Spools</span><button id="add">Add spool</button></h3>
<div id="form">
  <input type="hidden" id="f_id">
  <label>Brand</label><input id="f_brand">
  <label>Material</label><input id="f_material" placeholder="PLA, PETG, ABS…">
  <label>Colour</label><div class="row"><input id="f_color" class="grow" placeholder="Black"><input type="color" id="f_hex" style="width:36px"></div>
  <label>Filament preset (optional)</label><select id="f_preset"></select>
  <div class="row">
    <div class="grow"><label>Net weight (g)</label><input id="f_total" type="number" value="1000"></div>
    <div class="grow"><label>Remaining (g)</label><input id="f_remaining" type="number" value="1000"></div>
  </div>
  <div class="row">
    <div class="grow"><label>Empty spool (g)</label><input id="f_tare" type="number" value="0"></div>
    <div class="grow"><label>Cost per kg</label><input id="f_cost" type="number" value="0"></div>
  </div>
  <label>Notes</label><input id="f_notes">
  <div class="row" style="margin-top:6px"><button id="save">Save</button><button class="quiet" id="cancel">Cancel</button></div>
</div>
<div id="spools"></div>
<label class="row muted"><input type="checkbox" id="showArch"> Show archived</label>
<p id="status" class="muted"></p>

<script>
(function () {
  var state = { spools: [], slots: {}, jobs: [], presets: [], current: [], options: {} };
  var $ = function (id) { return document.getElementById(id); };
  function esc(v) { var s = document.createElement("span"); s.textContent = v == null ? "" : String(v); return s.innerHTML; }
  function send(cmd, extra) { var m = extra || {}; m.command = cmd; orca.postMessage(m); }
  function g(v) { return (Math.round(v * 10) / 10) + " g"; }
  function spoolById(id) { for (var i = 0; i < state.spools.length; i++) if (state.spools[i].id === id) return state.spools[i]; return null; }
  function swatch(c) { return '<span class="sw" style="background:' + esc(c && c[0] === "#" ? c : "transparent") + '"></span>'; }

  function renderSlots() {
    var n = Math.max(state.current.length, 1), rows = "";
    for (var i = 1; i <= n; i++) {
      var opts = '<option value="">(none)</option>';
      state.spools.forEach(function (s) {
        if (s.archived) return;
        opts += '<option value="' + s.id + '"' + (state.slots[String(i)] === s.id ? " selected" : "") + ">" +
                esc(s.name) + " · " + g(s.remaining_g) + "</option>";
      });
      rows += "<tr><td>" + i + '</td><td class="muted">' + esc(state.current[i - 1] || "") +
              '</td><td><select data-slot="' + i + '">' + opts + "</select></td></tr>";
    }
    $("slots").innerHTML = rows;
    Array.prototype.forEach.call($("slots").querySelectorAll("select"), function (el) {
      el.onchange = function () { send("assign", { slot: el.dataset.slot, spool: el.value }); };
    });
  }

  function renderJobs() {
    var rows = "";
    state.jobs.slice(0, 15).forEach(function (j) {
      var total = j.grams.reduce(function (a, b) { return a + b; }, 0);
      var when = new Date(j.time * 1000).toLocaleString();
      var act = j.status === "pending"
        ? '<button data-a="done" data-id="' + j.id + '">Printed</button> <button class="quiet" data-a="fail" data-id="' + j.id + '">Failed…</button> <button class="quiet" data-a="drop" data-id="' + j.id + '">Discard</button>'
        : (j.status === "discarded" ? '<span class="muted">discarded</span>' :
           '<span class="muted">' + j.status + "</span> <button class=\"quiet\" data-a=\"undo\" data-id=\"" + j.id + '">Undo</button>');
      rows += "<tr><td>" + esc(j.name) + '<br><span class="muted">' + esc(when) + " · " + g(total) + "</span></td><td>" + act + "</td></tr>";
    });
    $("jobs").innerHTML = rows || '<tr><td class="muted">Export or send a print to capture a job.</td></tr>';
    Array.prototype.forEach.call($("jobs").querySelectorAll("button"), function (b) {
      b.onclick = function () {
        if (b.dataset.a === "fail") {
          var pc = prompt("How much printed before it failed? (percent)", "50");
          if (pc === null) return;
          send("complete", { id: b.dataset.id, fraction: Math.max(0, Math.min(100, parseFloat(pc) || 0)) / 100 });
        } else if (b.dataset.a === "done") send("complete", { id: b.dataset.id, fraction: 1 });
        else if (b.dataset.a === "drop") send("discard", { id: b.dataset.id });
        else if (b.dataset.a === "undo") send("undo", { id: b.dataset.id });
      };
    });
  }

  function renderSpools() {
    var html = "", low = state.options.low_stock_g || 50, showArch = $("showArch").checked;
    state.spools.forEach(function (s) {
      if (s.archived && !showArch) return;
      var pct = s.total_g > 0 ? Math.max(0, Math.min(100, 100 * s.remaining_g / s.total_g)) : 0;
      html += '<div class="spool' + (s.remaining_g < low ? " low" : "") + '"><div class="row">' + swatch(s.hex) +
        '<b class="grow">' + esc(s.name) + (s.archived ? " (archived)" : "") + "</b><span>" + g(s.remaining_g) + "</span></div>" +
        '<div class="muted">' + esc([s.material, s.brand, s.preset].filter(Boolean).join(" · ")) + "</div>" +
        '<div class="bar"><i style="width:' + pct + '%"></i></div>' +
        '<div class="row" style="margin-top:4px"><button class="quiet" data-a="edit" data-id="' + s.id + '">Edit</button>' +
        '<button class="quiet" data-a="weigh" data-id="' + s.id + '">Weigh</button>' +
        '<button class="quiet" data-a="arch" data-id="' + s.id + '">' + (s.archived ? "Restore" : "Archive") + "</button>" +
        '<button class="quiet" data-a="del" data-id="' + s.id + '">Delete</button></div></div>';
    });
    $("spools").innerHTML = html || '<p class="muted">No spools yet. Add one to start tracking.</p>';
    Array.prototype.forEach.call($("spools").querySelectorAll("button"), function (b) {
      b.onclick = function () {
        var s = spoolById(b.dataset.id);
        if (b.dataset.a === "edit") openForm(s);
        else if (b.dataset.a === "weigh") {
          var w = prompt("Weight of the spool on a scale, in grams (empty spool is " + (s.tare_g || 0) + " g):", "");
          if (w !== null && w !== "") send("weigh", { id: s.id, gross: parseFloat(w) });
        } else if (b.dataset.a === "arch") send("archive", { id: s.id, archived: !s.archived });
        else if (b.dataset.a === "del" && confirm("Delete " + s.name + "?")) send("delete", { id: s.id });
      };
    });
  }

  function openForm(s) {
    s = s || {};
    $("f_id").value = s.id || "";
    $("f_brand").value = s.brand || ""; $("f_material").value = s.material || ""; $("f_color").value = s.color || "";
    $("f_hex").value = s.hex || "#808080";
    $("f_total").value = s.total_g != null ? s.total_g : 1000; $("f_remaining").value = s.remaining_g != null ? s.remaining_g : 1000;
    $("f_tare").value = s.tare_g || 0; $("f_cost").value = s.cost_per_kg || 0; $("f_notes").value = s.notes || "";
    var opts = '<option value="">(none)</option>';
    state.presets.forEach(function (p) { opts += '<option' + (p === s.preset ? " selected" : "") + ">" + esc(p) + "</option>"; });
    $("f_preset").innerHTML = opts;
    $("form").style.display = "block";
  }

  $("add").onclick = function () { openForm(null); };
  $("cancel").onclick = function () { $("form").style.display = "none"; };
  $("save").onclick = function () {
    send("save_spool", { spool: { id: $("f_id").value, brand: $("f_brand").value, material: $("f_material").value,
      color: $("f_color").value, hex: $("f_hex").value, preset: $("f_preset").value, total_g: $("f_total").value,
      remaining_g: $("f_remaining").value, tare_g: $("f_tare").value, cost_per_kg: $("f_cost").value, notes: $("f_notes").value } });
    $("form").style.display = "none";
  };
  $("showArch").onchange = renderSpools;
  $("auto").onchange = function () { send("options", { auto_deduct_on_upload: $("auto").checked }); };

  orca.onMessage(function (m) {
    if (!m) return;
    if (m.command === "state") {
      state = m;
      $("auto").checked = !!(m.options && m.options.auto_deduct_on_upload);
      $("stats").textContent = m.stats.spools + " spools · " + g(m.stats.remaining_g) + (m.stats.value ? " · " + m.stats.value.toFixed(2) + " value" : "");
      renderSlots(); renderJobs(); renderSpools();
    } else if (m.command === "status") $("status").textContent = m.text;
  });
  send("hello");
})();
</script>
"""


def _filament_preset_names() -> list[str]:
    try:
        return sorted(set(orca.host.preset_bundle().filaments.preset_names()))
    except Exception:
        return []


def _current_filaments() -> list[str]:
    try:
        return list(orca.host.preset_bundle().current_filament_preset_names())
    except Exception:
        return []


class _Panel:
    post = None

    @classmethod
    def push(cls, text: str | None = None) -> None:
        if cls.post is None:
            return
        db = load_db()
        current = _current_filaments()
        if current:
            before = dict(db["slots"])
            auto_assign_slots(db, current)
            if db["slots"] != before:
                save_db(db)
        msg = {"command": "state", "spools": db["spools"], "slots": db["slots"], "jobs": db["jobs"],
               "options": db["options"], "stats": stats(db), "presets": _filament_preset_names(), "current": current}
        try:
            cls.post(msg)
            if text:
                cls.post({"command": "status", "text": text})
        except Exception:
            pass


def _notify(text: str) -> None:
    try:
        orca.host.ui.push_notification(text="Filament Tracker: " + text)
    except Exception:
        pass


def handle_message(m: dict) -> str | None:
    cmd = m.get("command")
    db = load_db()
    text = None
    if cmd == "hello":
        return None
    if cmd == "save_spool":
        data = dict(m.get("spool") or {})
        spool = upsert_spool(db, data)
        if data.get("hex"):
            spool["hex"] = str(data["hex"])
        text = "Saved %s." % spool["name"]
    elif cmd == "weigh":
        weigh_spool(db, m["id"], _num(m.get("gross")))
        text = "Updated remaining weight."
    elif cmd == "archive":
        spool = find_spool(db, m["id"])
        if spool:
            spool["archived"] = bool(m.get("archived"))
            if spool["archived"]:
                db["slots"] = {k: v for k, v in db["slots"].items() if v != spool["id"]}
    elif cmd == "delete":
        db["spools"] = [s for s in db["spools"] if s["id"] != m["id"]]
        db["slots"] = {k: v for k, v in db["slots"].items() if v != m["id"]}
    elif cmd == "assign":
        if m.get("spool"):
            db["slots"][str(m["slot"])] = m["spool"]
        else:
            db["slots"].pop(str(m["slot"]), None)
    elif cmd == "complete":
        job = complete_job(db, m["id"], _num(m.get("fraction"), 1.0))
        text = "Subtracted %.1f g." % sum(job["deducted"].values())
    elif cmd == "undo":
        undo_job(db, m["id"])
        text = "Restored the filament for that job."
    elif cmd == "discard":
        for j in db["jobs"]:
            if j["id"] == m["id"] and j["status"] == "pending":
                j["status"] = "discarded"
    elif cmd == "options":
        for k in ("auto_deduct_on_upload", "low_stock_g"):
            if k in m:
                db["options"][k] = m[k]
    save_db(db)
    return text


if orca is not None:

    class FilamentTrackerPanel(orca.script.ScriptPluginCapabilityBase):
        panel = None

        def get_name(self):
            return "Filament Tracker"

        def execute(self):
            if self.panel is not None and self.panel.is_open():
                self.panel.show()
                return orca.ExecutionResult.success("Filament Tracker is already open.")
            self.panel = orca.host.ui.create_dock_panel(
                html=PAGE, title="Filament Tracker", width=340, height=640,
                on_message=self.on_message, on_close=self.on_close, dock="right")
            _Panel.post = self.panel.post
            return orca.ExecutionResult.success("Filament Tracker opened.")

        def on_message(self, message):
            try:
                text = handle_message(message or {})
            except Exception as e:
                text = "Error: %s" % e
            _Panel.push(text)

        def on_close(self):
            self.panel = None
            _Panel.post = None

    class FilamentTrackerCapture(orca.slicing.SlicingPipelineCapabilityBase):
        """Captures each exported or uploaded G-code as a job."""

        def get_name(self):
            return "Filament Tracker: capture print jobs"

        def execute(self, ctx):
            if ctx.step != orca.slicing.Step.psGCodePostProcess or not ctx.gcode_path:
                return orca.ExecutionResult.success()
            try:
                path = Path(ctx.gcode_path)
                usage = parse_gcode_usage(path)
                db = load_db()
                name = Path(ctx.output_name or ctx.gcode_path).name
                host = ctx.host or "File"
                job = add_job(db, usage, name, host, _file_md5(path))
                if job is not None:
                    for w in low_stock_warnings(db, job):
                        _notify(w)
                    if host != "File" and db["options"].get("auto_deduct_on_upload"):
                        complete_job(db, job["id"], 1.0)
                    save_db(db)
                    _Panel.push()
            except Exception as e:
                return orca.ExecutionResult.success("Filament Tracker could not read the G-code: %s" % e)
            return orca.ExecutionResult.success()

    @orca.plugin
    class FilamentTrackerPlugin(orca.base):
        def register_capabilities(self):
            orca.register_capability(FilamentTrackerPanel)
            orca.register_capability(FilamentTrackerCapture)
