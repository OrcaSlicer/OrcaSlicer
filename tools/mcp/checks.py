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
        if reply.get("status") == "blocked_by_dialog" or dialogs:
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
    """What G-code text holds: `layers`, the number on its `; total layer number: N` line; `labels`, how many ids its
    `; model label id:` line lists; and `config`, the `; key = value` lines of its configuration block as
    {key: value}. A line the text lacks gives None."""
    layers, labels, config, block = None, None, {}, False
    for line in lines:
        line = line.rstrip("\r\n")
        if layers is None and line.startswith("; total layer number:"):
            layers = int(line.split(":", 1)[1])
        elif labels is None and line.startswith("; model label id:"):
            labels = len([i for i in line.split(":", 1)[1].split(",") if i.strip()])
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


def slots(client):
    """The filament slots as filament_is_mixed from project_get_settings, one digit per slot ("0,0,1": three slots,
    the third a mixed filament), as test_plates reads them."""
    return client.call("project_get_settings", keys=["filament_is_mixed"])["items"][0]["value"]
