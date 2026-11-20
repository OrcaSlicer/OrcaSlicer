"""What the sweep's Python tests use to apply the read-back rule of tools/mcp/TESTS.md: named checks with the test's
exit code, scenarios that fail on their own, refusals asserted by their code or words, waits that fail when they run
out of time, and readers that give the app's state in a fixed shape."""

import json
import time
import traceback

import orca_mcp


class Checks:
    """Named checks, each printed as `ok <name>` or `FAIL <name>: <detail>`. A failed check lets the test go on."""

    def __init__(self):
        self.count = 0
        self.failed = []

    def check(self, name, ok, detail=""):
        self.count += 1
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"), flush=True)
        if not ok:
            self.failed.append(name)
        return ok

    def equal(self, name, got, want):
        return self.check(name, got == want, f"got {got!r}, expected {want!r}")

    def near(self, name, got, want, tolerance):
        """`got` within `tolerance` of `want`, both numbers or lists of numbers of one length."""
        if isinstance(want, (list, tuple)):
            ok = isinstance(got, (list, tuple)) and len(got) == len(want) and all(
                abs(g - w) <= tolerance for g, w in zip(got, want))
        else:
            ok = isinstance(got, (int, float)) and abs(got - want) <= tolerance
        return self.check(name, ok, f"got {got!r}, expected {want!r} to {tolerance}")

    def refused(self, name, call, code=None, words=None):
        """Checks that `call()` is refused with the code `code` and with text that holds `words`, each when given,
        and returns the refusal's text; a call that goes through fails the check and returns None."""
        try:
            reply = call()
        except orca_mcp.McpError as e:
            text, details = refusal(e)
            problems = []
            if code is not None and details.get("code") != code:
                problems.append(f"code {details.get('code')!r}, expected {code!r}")
            if words is not None and words not in text:
                problems.append(f"the text does not hold {words!r}")
            self.check(name, not problems, "; ".join(problems) + f": {text}")
            return text
        self.check(name, False, f"not refused: {reply}")
        return None

    def scenario(self, name, fn):
        """Runs one scenario. An exception fails it under its name, and the next scenario still runs."""
        print(f"--- {name}", flush=True)
        try:
            fn()
        except Exception as e:  # a scenario that breaks off is one failure; the others still run
            self.check(f"{name}: runs to its end", False, f"{type(e).__name__}: {e}")
            traceback.print_exc()

    def exit_code(self):
        print(f"{self.count - len(self.failed)} of {self.count} checks passed", flush=True)
        return 1 if self.failed else 0


def refusal(error):
    """A tool error's text and the details the server puts on its last line as JSON ({} when it has none)."""
    text = str(error)
    head, _, last = text.rpartition("\n")
    try:
        details = json.loads(last)
    except ValueError:
        return text, {}
    return (head, details) if isinstance(details, dict) else (text, {})


def run(client, tool, timeout_s=600, **args):
    """Calls `tool` and follows a job it replies with to its end. A dialog that holds the call raises at once,
    naming it, since nothing answers a dialog the test does not expect; a test answers the dialogs it expects
    with client.call() and dialog_answer."""
    reply = client.call(tool, **args)
    deadline = time.monotonic() + timeout_s
    while isinstance(reply, dict) and "job" in reply and reply.get("status") in ("running", "blocked_by_dialog"):
        dialogs = reply.get("dialogs") or client.call("app_state").get("dialogs", [])
        # An auto-answer rule presses its dialog's button just after the dialog opens, and the reply can come
        # before the press: a dialog still open a moment later is one nothing answers.
        if (reply.get("status") == "blocked_by_dialog" or dialogs) and \
                client.call("app_wait", **{"for": "no_dialog", "timeout_s": 2}).get("satisfied") is not True:
            dialogs = client.call("app_state").get("dialogs", []) or dialogs
            titles = [d.get("title") for d in dialogs]
            raise orca_mcp.McpError(f"{tool} is held by the dialog {titles}: {dialogs}", reply)
        if time.monotonic() > deadline:
            raise TimeoutError(f"{tool} still runs after {timeout_s} s")
        reply = client.call("app_wait", **{"for": "job", "job": reply["job"], "timeout_s": 10})
    return reply


def wait(client, condition, timeout_s=30, job=None):
    """app_wait for `condition` ("idle", "dialog", "no_dialog", "ready" or "job"); raises TimeoutError when the
    wait runs out of time, which app_wait itself reports as an answer."""
    args = {"for": condition, "timeout_s": timeout_s}
    if job is not None:
        args["job"] = job
    reply = client.call("app_wait", **args)
    if condition == "job":
        if reply.get("status") in ("running", "blocked_by_dialog"):
            raise TimeoutError(f"job {job} still runs after {timeout_s} s")
    elif reply.get("satisfied") is not True:
        raise TimeoutError(f"app_wait {condition} did not hold within {timeout_s} s")
    return reply


# The default printer's presets, which a data folder orca_mcp.py launch seeds selects, and the Bambu Lab X1 Carbon
# 0.4 nozzle's, for a test of a Bambu Lab printer's own behaviour.
KLIPPER, KLIPPER_STD, GENERIC_PLA = "MyKlipper 0.4 nozzle", "0.20mm Standard @MyKlipper", "Generic PLA @System"
X1C, X1C_STD, BAMBU_PLA = "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C"
# The printers start() knows: what printer_install installs each from, and its default process and filament.
PRINTERS = {KLIPPER: ({"vendor": "Custom", "model": "Generic Klipper Printer", "nozzles": ["0.4"]}, KLIPPER_STD, GENERIC_PLA),
            X1C: ({"vendor": "BBL", "model": "Bambu Lab X1 Carbon", "nozzles": ["0.4"]}, X1C_STD, BAMBU_PLA)}


def presets(client):
    """The selected presets as {printer, process, filaments}, the filaments one per slot."""
    p = client.call("app_state")["presets"]
    return {"printer": p["printer"], "process": p["process"], "filaments": list(p["filaments"])}


def start(client, printer=KLIPPER, process=None, filaments=None):
    """Puts the app in a test's starting state on a data folder earlier tests used, and returns presets(): a new
    project with its changes and the presets' unsaved changes discarded, then `printer` selected, installed first
    with printer_install when the folder lacks it, then `process` (default: the printer's default process), then one
    filament slot per name in `filaments` (default: the printer's default filament), the mixed filament slots an
    earlier test left deleted first. Each switch discards unsaved preset changes. A printer PRINTERS does not list
    needs its process and filaments given."""
    install, default_process, default_filament = PRINTERS.get(printer, (None, None, None))
    process = process or default_process
    filaments = list(filaments or [default_filament])
    if process is None or None in filaments:
        raise ValueError(f"start() knows no defaults for {printer!r}: give its process and filaments")
    run(client, "project_new", discard_changes=True, preset_changes="discard")
    if presets(client)["printer"] != printer:
        listed = client.call("preset_list", type="printer", include_uninstalled=True, query=printer, limit=500)["items"]
        if install and not any(i["name"] == printer and i.get("installed", True) for i in listed):
            run(client, "printer_install", models=[install], preset_changes="discard")
        if presets(client)["printer"] != printer:
            run(client, "preset_select", type="printer", name=printer, preset_changes="discard")
    if presets(client)["process"] != process:
        run(client, "preset_select", type="process", name=process, preset_changes="discard")

    # With one physical filament the sidebar hides the mixed filaments and deletes none, so a mix an earlier test left
    # beside one physical slot goes once a slot is added.
    rows = filament_rows(client)
    if any(row["mixed"] for row in rows) and sum(not row["mixed"] for row in rows) < 2:
        run(client, "filament_add")
    # Last to first, so the numbers of the mixed slots still to go hold.
    for slot in reversed([row["filament_slot"] for row in filament_rows(client) if row["mixed"]]):
        run(client, "filament_delete", filament_slot=slot)
    while len(filament_rows(client)) > len(filaments):
        run(client, "filament_delete")
    while len(filament_rows(client)) < len(filaments):
        run(client, "filament_add")
    for slot, name in enumerate(filaments, 1):
        if presets(client)["filaments"][slot - 1] != name:
            run(client, "preset_select", type="filament", name=name, filament_slot=slot, preset_changes="discard")
    return presets(client)


def plates(client):
    """Each plate as {name, current, locked, objects}, objects being how many stand on it."""
    return [{"name": p["name"], "current": p["current"], "locked": p["locked"], "objects": p["objects"]}
            for p in client.call("plate_list", limit=500)["items"]]


def objects(client):
    """(on_plates, outside): the objects on the plates as (plate, name) pairs in sorted order, and the names of the
    objects outside every plate, sorted."""
    listing = client.call("object_list", limit=500)
    on = sorted((p["index"], o["name"]) for p in listing["plates"] for o in p["objects"])
    return on, sorted(o["name"] for o in listing.get("outside", []))


# ---- Objects: the readers of test-entries/objects.md, shared by test_objects and test_object_structure ----

import math  # noqa: E402
import re  # noqa: E402


def scene(client):
    """Each object of object_list in index order, its entry whole, with its instances gathered from every plate and
    from outside, in index order, each given its plate (-1 outside). object_list lists an object under each plate it
    has instances on, with that plate's instances alone."""
    listing = client.call("object_list", limit=500)
    found = {}
    groups = [(p["index"], p["objects"]) for p in listing["plates"]] + [(-1, listing.get("outside", []))]
    for plate, entries in groups:
        for entry in entries:
            obj = found.setdefault(entry["index"], {**entry, "instances": []})
            obj["instances"] += [{**inst, "plate": plate} for inst in entry["instances"]]
    for obj in found.values():
        obj["instances"].sort(key=lambda inst: inst["index"])
    return [found[i] for i in sorted(found)]


def selection(client):
    """app_state's selection as {objects, instances, parts}, parts absent when whole instances are selected."""
    sel = client.call("app_state").get("selection", {})
    out = {"objects": sel.get("objects", []), "instances": sel.get("instances", [])}
    if "parts" in sel:
        out["parts"] = sel["parts"]
    return out


def corner(client, plate):
    """Plate `plate`'s front left corner: its origin from plate_list, [x, y]."""
    return client.call("plate_list", limit=500)["items"][plate]["origin"]


def rect(client, plate):
    """Plate `plate`'s rectangle (x0, y0, x1, y1), from its origin and size in plate_list."""
    item = client.call("plate_list", limit=500)["items"][plate]
    (x, y), (w, d) = item["origin"], item["size"]
    return x, y, x + w, y + d


PRINTING = re.compile(r"^; printing object (.*) id:(\d+) copy (\d+)\s*$")


def gcode_prints(path):
    """What a G-code file prints, from its `; printing object <name> id:<n> copy <c>` comments: [(name, copies)] in
    the order each name first prints, copies being the number of different (id, copy) pairs printed under it."""
    printed = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PRINTING.match(line)
            if m:
                printed.setdefault(m.group(1), set()).add((m.group(2), m.group(3)))
    return [(name, len(pairs)) for name, pairs in printed.items()]


def square(instance):
    """An instance's square: its bbox in x and y, (x0, y0, x1, y1)."""
    box = instance["bbox"]
    return box["min"][0], box["min"][1], box["max"][0], box["max"][1]


def apart(a, b):
    """Two squares do not overlap (touching edges do not overlap)."""
    return a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1]


def inside(sq, rectangle):
    """A square lies within a rectangle (x0, y0, x1, y1), such as rect(p)."""
    return sq[0] >= rectangle[0] and sq[1] >= rectangle[1] and sq[2] <= rectangle[2] and sq[3] <= rectangle[3]


def gap(a, b):
    """The distance between two squares: along the axis on which they are apart, corner to corner when they are apart
    on both; 0 when they overlap."""
    dx = max(b[0] - a[2], a[0] - b[2], 0)
    dy = max(b[1] - a[3], a[1] - b[3], 0)
    return math.hypot(dx, dy)


# ---- Project files: the readers of test-entries/files.md, for test_files ----

import io  # noqa: E402
import os  # noqa: E402
import zipfile  # noqa: E402

UNDO_LINE = "Undo / Redo snapshot taken: "
RECENT = re.compile(r"^Open the recent project (.*)\.$")
PLATE_GCODE = re.compile(r"^Metadata/plate_\d+\.gcode$")


def normal(path):
    """A path as the project readers compare it: os.path.normcase of os.path.abspath, None for "" (no file)."""
    return os.path.normcase(os.path.abspath(path)) if path else None


def project(client):
    """app_state's project as {name, file, dirty}, file being its path normalised by normal()."""
    p = client.call("app_state")["project"]
    return {"name": p["name"], "file": normal(p["path"]), "dirty": p["dirty"]}


def meshes(client):
    """Each object of object_list, plate by plate and then those outside, as {name, offset, size, filament, parts}:
    its first instance's offset x and y, its bbox size, its filament, and each part as {id, type, triangles,
    mesh_volume}."""
    listing = client.call("object_list", limit=500)
    entries = [o for p in listing["plates"] for o in p["objects"]] + listing.get("outside", [])
    return [{"name": o["name"], "offset": o["instances"][0]["offset"][:2], "size": o["bbox"]["size"],
             "filament": o.get("filament"),
             "parts": [{"id": p["id"], "type": p["type"], "triangles": p["triangles"], "mesh_volume": p["mesh_volume"]}
                       for p in o["parts"]]}
            for o in entries]


def values(client, keys, type="process"):
    """The settings `keys` of the preset of `type` being edited, as {key: value}, each value a string as settings_get
    gives it."""
    reply = client.call("settings_get", type=type, keys=list(keys), include_meta=False)
    return {item["key"]: item["value"] for item in reply["items"]}


def unsaved(client, type):
    """The keys, sorted, in which the preset of `type` being edited differs from that preset as it is stored."""
    reply = client.call("settings_get", type=type, compare=":saved", limit=500)
    return sorted(item["key"] for item in reply["items"])


def recent(client):
    """The files of the Recent Projects actions, normalised by normal(), read from each one's description ("Open the
    recent project <path>.")."""
    items = client.call("action_search", group="Recent Projects", limit=100)["items"]
    return [normal(m.group(1)) for m in (RECENT.match(i.get("description", "")) for i in items) if m]


def undo_steps(client):
    """The names of the undo steps the app logged (its log's lines `Undo / Redo snapshot taken: <name>`) since this
    reader's previous call for `client`, or since the app started on the first. A check calls it just before its
    step, which sets the mark, and checks what the call after the step gives."""
    log = client.call("app_log", lines=1)["file"]
    with open(log, encoding="utf-8", errors="replace") as f:
        names = [line.split(UNDO_LINE, 1)[1].split(", Undo / Redo stack memory")[0].rstrip("\r\n")
                 for line in f if UNDO_LINE in line]
    file, seen = getattr(client, "undo_mark", (None, 0))
    start = seen if file == log and seen <= len(names) else 0
    client.undo_mark = (log, len(names))
    return names[start:]


def dialogs(client):
    """The open dialogs from app_state, the topmost last, as {title, message}."""
    return [{"title": d.get("title"), "message": d.get("message")} for d in client.call("app_state").get("dialogs", [])]


def sliced_flags(client):
    """Whether each plate is sliced, from plate_list, in plate order."""
    return [p["sliced"] for p in client.call("plate_list", limit=500)["items"]]


def layer_gcode(client):
    """Plate 0's layer G-code from plate_list, each code as {z, type}."""
    return [{"z": g["z"], "type": g["type"]} for g in client.call("plate_list", limit=500)["items"][0]["layer_gcode"]]


def gcode_facts(lines):
    """What G-code text holds: `layers`, the number on its `; total layer number: N` line; `labels`, how many
    `EXCLUDE_OBJECT_DEFINE` lines it has, one per object instance a Klipper printer's G-code names (0 when it has
    none); and `config`, the `; key = value` lines of its configuration block as {key: value}. A line the text lacks
    gives None."""
    layers, labels, config, block = None, 0, {}, False
    for line in lines:
        line = line.rstrip("\r\n")
        if layers is None and line.startswith("; total layer number:"):
            layers = int(line.split(":", 1)[1])
        elif line.startswith("EXCLUDE_OBJECT_DEFINE"):
            labels += 1
        elif line == "; CONFIG_BLOCK_START":
            block = True
        elif line == "; CONFIG_BLOCK_END":
            block = False
        elif block and line.startswith("; ") and " = " in line:
            key, _, value = line[2:].partition(" = ")
            config[key] = value
    return {"layers": layers, "labels": labels, "config": config}


def gcode(path):
    """gcode_facts() of a G-code file."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return gcode_facts(f)


def sliced(path):
    """A sliced file's (.gcode.3mf) G-code entries, Metadata/plate_<n>.gcode, as {name: gcode_facts()} in name
    order."""
    with zipfile.ZipFile(path) as archive:
        names = sorted(n for n in archive.namelist() if PLATE_GCODE.match(n))
        out = {}
        for name in names:
            with archive.open(name) as entry:
                out[name] = gcode_facts(io.TextIOWrapper(entry, encoding="utf-8", errors="replace"))
        return out


def obj_facts(path):
    """An OBJ file's first line, its mtllib line, its numbers of `v` and `f` lines and its highest vertex z, as
    {first, mtllib, v, f, top}."""
    first, mtllib, v, f, top = None, None, 0, 0, None
    with open(path, encoding="utf-8", errors="replace") as text:
        for line in text:
            if first is None:
                first = line.rstrip("\r\n")
            if line.startswith("mtllib "):
                mtllib = line.strip()
            elif line.startswith("v "):
                v += 1
                z = float(line.split()[3])
                top = z if top is None else max(top, z)
            elif line.startswith("f "):
                f += 1
    return {"first": first, "mtllib": mtllib, "v": v, "f": f, "top": top}


def reimport(client, path):
    """Imports a file into a new, empty project: project_new {discard_changes: true}, then project_import of the file.
    Returns the import's reply and meshes()."""
    run(client, "project_new", discard_changes=True)
    reply = run(client, "project_import", paths=[path])
    return reply, meshes(client)


def answer_all(client, checks, label, expected, timeout_s=30):
    """Answers OK, one at a time as each comes topmost, the dialogs a call opens, until it has answered one dialog for
    each (title, words in its message) of `expected`, in whatever order they come. Each wait for the next is app_wait
    {for: "dialog"}, and one that runs out raises TimeoutError. Then checks that no other dialog opened and that
    dialogs() is empty."""
    left, others = list(expected), []
    while left and len(others) < 5:
        wait(client, "dialog", timeout_s)
        shown = dialogs(client)
        if not shown:  # closed between the wait and the read
            continue
        top = shown[-1]
        match = next((e for e in left if e[0] == top["title"] and e[1] in (top["message"] or "")), None)
        if match:
            left.remove(match)
        else:
            others.append(top)
        client.call("dialog_answer", dialog={"title": top["title"]}, button="OK")
    checks.equal(f"{label}: no other dialog opened", others, [])
    checks.equal(f"{label}: no dialog is left open", dialogs(client), [])


def filament_rows(client):
    """filament_list's rows, one per filament slot in config order, the physical ones first."""
    return client.call("filament_list")["filaments"]


def slots(client):
    """The filament slots as the mixed flag of each filament_list row, one digit per slot ("0,0,1": three slots, the
    third a mixed filament), as test_plates reads them."""
    return ",".join("1" if row["mixed"] else "0" for row in filament_rows(client))


def slot_colours(client):
    """Each filament slot's colour from filament_list, joined by ";" as the project stores them."""
    return ";".join(row["colour"] for row in filament_rows(client))
