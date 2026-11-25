#!/usr/bin/env python3
"""How the MCP runtime takes calls, in five parts on one app, as the test_runtime_calls entry of
tools/mcp/test-entries/runtime.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_runtime_calls.py

A part that raises fails under its name, and the next part still runs. Exits 0 when every check passes.
"""

import json
import os
import re
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, RUN_SCRIPT_TOGGLE, Checks, refusal, start,  # noqa: E402
                    until, wait)

HERE = os.path.dirname(os.path.abspath(__file__))
START = {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}
START_CHECK = "the start is the Klipper printer with one Generic PLA slot"


# ---- Recovery under load ----

# The reply an observation call gets when every blocking slot is taken.
PENDING = {"status": "pending", "note": "Too many calls are waiting right now; try again shortly."}
# From the start of the 32 waits until app_state finds no slot free, and how long a wait replied pending is sent again.
SLOTS_LIMIT_S = 30
# No tool reports the server's connections, and a half-sent holder gets no reply that would show the server took it,
# so the half-sent holders are given this long to reach the server before the dialog is answered.
HALF_SENT_GRACE_S = 1


def post(port, token, body, timeout=120):
    """One request on its own connection; returns the parsed JSON-RPC reply."""
    data = json.dumps(body).encode()
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall((f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
               f"Accept: application/json, text/event-stream\r\nAuthorization: Bearer {token}\r\n"
               f"MCP-Protocol-Version: 2025-06-18\r\nContent-Length: {len(data)}\r\n\r\n").encode() + data)
    reply = b""
    while b"\r\n\r\n" not in reply:
        reply += s.recv(65536)
    head, _, rest = reply.partition(b"\r\n\r\n")
    length = int([l.split(b":")[1] for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")][0])
    while len(rest) < length:
        rest += s.recv(65536)
    s.close()
    return json.loads(rest[:length])


def raw_call(port, token, tool, args):
    """A tool call on a connection of its own: (its result, whether it is an error)."""
    reply = post(port, token, {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    return json.loads(reply["result"]["content"][0]["text"]), reply["result"].get("isError", False)


def holder(port, token, half_sent):
    """A connection that occupies a place: idle after a ping, or stuck mid-request."""
    s = socket.create_connection(("127.0.0.1", port), timeout=120)
    if half_sent:
        s.sendall(f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: appl".encode())
    else:
        data = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
        s.sendall((f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
                   f"Authorization: Bearer {token}\r\nMCP-Protocol-Version: 2025-06-18\r\nContent-Length: {len(data)}\r\n\r\n").encode() + data)
        s.recv(65536)
    return s


def pending(reply):
    """Whether an observation call's reply is the one it gets when no blocking slot is free."""
    return {k: reply.get(k) for k in PENDING} == PENDING


def wait_for_close(port, token, results):
    """One app_wait for the dialog to close, its last reply added to `results`. The test's own app_state, asked
    meanwhile, can hold the slot a wait arrives for, so a wait replied `pending` is sent again, as the note asks, for
    up to SLOTS_LIMIT_S s."""
    last = {}

    def answered():
        last["reply"] = raw_call(port, token, "app_wait", {"for": "no_dialog", "timeout_s": 50})[0]
        return not pending(last["reply"])

    try:
        until(answered, "a blocking slot for the wait", SLOTS_LIMIT_S, interval_s=0.2)
    except TimeoutError:  # still pending: a wait that did not see the dialog close
        pass
    results.append(last["reply"])


def every_slot_taken(port, token):
    """Whether app_state, on a connection of its own every 0.2 s, replies that no blocking slot is free within
    SLOTS_LIMIT_S s."""
    try:
        return until(lambda: pending(raw_call(port, token, "app_state", {})[0]), "app_state replied pending",
                     SLOTS_LIMIT_S, interval_s=0.2)
    except TimeoutError:
        return False


def recover(port, token, half_sent):
    """One variant: None when the dialog is answered and every wait sees it close, else what went wrong."""
    job, _ = raw_call(port, token, "action_run", {"id": "orca_command:open_preferences:orca"})
    if job.get("status") != "blocked_by_dialog":
        return f"Preferences did not open: {job}"

    waits, results = [], []
    for _ in range(32):
        t = threading.Thread(target=wait_for_close, args=(port, token, results))
        t.start()
        waits.append(t)
    # A variant whose waits never take every slot still answers the dialog, so that its waits end before the next.
    taken = every_slot_taken(port, token)
    # An idle holder has read its ping's reply, so the server took it; a half-sent one shows nothing.
    holders = [holder(port, token, half_sent) for _ in range(32)]
    if half_sent:
        time.sleep(HALF_SENT_GRACE_S)

    started = time.time()
    answer, failed = raw_call(port, token, "dialog_answer", {"button": "close"})
    if failed:
        return f"dialog_answer failed: {answer}"
    if answer.get("status") == "running":  # no blocking slot: the job runs on, follow it
        answer, failed = raw_call(port, token, "app_wait", {"for": "job", "job": answer["job"], "timeout_s": 30})
    for t in waits:
        t.join(SLOTS_LIMIT_S + 60)
    for s in holders:
        s.close()
    satisfied = sum(1 for r in results if r.get("satisfied"))
    print(f"  dialog answered in {time.time() - started:.1f} s; {satisfied}/32 waits saw it close")
    if not taken:
        return f"app_state found a blocking slot free for {SLOTS_LIMIT_S} s after the 32 waits started"
    return None if satisfied == 32 else f"only {satisfied} of 32 waits returned satisfied"


def recovery_under_load(client, session, t):
    t.equal(f"recovery: {START_CHECK}", start(client), START)
    for half_sent in (False, True):
        name = "half-sent requests" if half_sent else "idle connections"
        error = recover(session["port"], session["token"], half_sent)
        t.check(f"recover with 32 blocked waits and 32 {name}", error is None, error)


# ---- A slice cancelled while it waits to start ----

BENCHY = os.path.join(HERE, "..", "..", "resources", "handy_models", "3DBenchy.drc")


def cancel_waiting_slice(client, t):
    gcode = os.path.join(tempfile.mkdtemp(prefix="orca-mcp-"), "plate.gcode")

    def tab():
        return client.call("app_state").get("tab")

    def sliced():
        return client.call("plate_list")["items"][0]["sliced"]

    def slice_job_running():
        return any(j.get("tool") == "plate_slice" and j.get("state") == "running"
                   for j in client.call("app_state").get("jobs", []))

    t.equal(f"cancel: {START_CHECK}", start(client), START)
    client.call("project_import", paths=[os.path.abspath(BENCHY)])
    wait(client, "idle")
    t.equal("the slice starts from Prepare, unsliced", (tab(), sliced()), ("prepare", False))

    client.call("settings_set", type="process", values={"sparse_infill_density": "35%"})
    box = {}
    worker = threading.Thread(target=lambda: box.update(result=client.call("plate_slice", _timeout=600)))
    worker.start()
    # The slice waits about half a second for the update the change scheduled, and the cancel must land in that wait,
    # a race the app's timing decides: it goes out as soon as app_state lists the slice's job running.
    until(slice_job_running, "app_state lists the plate_slice job running", interval_s=0.02)
    cancel = client.call("job_cancel")
    worker.join(600)
    result = client.await_job(box["result"])
    t.check("job_cancel reports the waiting slice", cancel.get("was_running") is True, cancel)
    t.check("the slice reports its plate cancelled",
            result.get("status") == "done" and result.get("plates") == [{"index": 0, "outcome": "cancelled"}], result)
    shown = tab()
    t.check("the cancel came before the slice started", shown == "prepare", f"the {shown} tab is shown")

    # Once the scheduled update has run, the plate must still have no result to export.
    wait(client, "idle")
    problems = []
    if sliced() is not False:
        problems.append("plate_list gives plate 0 sliced")
    try:
        reply = client.call("project_export", format="gcode", path=gcode)
        problems.append(f"the G-code export went through: {reply}")
    except orca_mcp.McpError as e:
        text, details = refusal(e)
        if details.get("code") != "plate_not_sliced":
            problems.append(f"the G-code export was refused as {details.get('code')!r}: {text}")
    if os.path.exists(gcode):
        problems.append("the refused export wrote its file")
    t.check("the plate stays unsliced", not problems, "; ".join(problems))

    # The control: a slice that starts shows the Preview and leaves a plate that exports.
    problems = []
    control = client.await_job(client.call("plate_slice", _timeout=600))
    if [p.get("outcome") for p in control.get("plates", [])] != ["ok"]:
        problems.append(f"the slice replied {control}")
    if (shown := tab()) != "preview":
        problems.append(f"the {shown} tab is shown")
    if sliced() is not True:
        problems.append("plate_list gives plate 0 unsliced")
    client.call("project_export", format="gcode", path=gcode)
    if not (os.path.isfile(gcode) and os.path.getsize(gcode) > 0):
        problems.append("the export wrote no G-code")
    t.check("a slice afterwards runs and exports", not problems, "; ".join(problems))


# ---- The Run Script panel out of reach of the GUI tools ----

SHOW_LIMIT_S = 30  # from the action that opens a window until it is seen up
# No tool sees the Run Script panel's page, which is hidden from them, so its load cannot be waited for: once the panel
# is seen up, its page is given this long before the checks that it stays out of reach.
PAGE_LOAD_S = 1.0


def run_script_hidden(client, t):
    def eval_outcome():
        try:
            return "result: " + str(client.call("webview_run_javascript", code="1 + 1").get("result"))
        except orca_mcp.McpError as e:
            return "error: " + str(e)

    def webview_nodes():
        _snap, texts = client.call_with_text("widget_list")
        return len(re.findall(r"\bwebview\b", texts[0] if texts else ""))

    def canvas_height():
        """The 3D view's height, from its line in widget_list, or None when it is not listed."""
        _snap, texts = client.call_with_text("widget_list")
        found = re.search(r'canvas [^\n]*name=OrcaSceneCanvas[^\n]*@-?\d+,-?\d+ \d+x(\d+)', "\n".join(texts))
        return int(found.group(1)) if found else None

    t.equal(f"Run Script panel: {START_CHECK}", start(client), START)

    # The Run Script panel, docked under the 3D view of Prepare.
    nodes_before = webview_nodes()
    prepare_eval = eval_outcome()
    height = canvas_height()
    client.call("action_run", id=RUN_SCRIPT_TOGGLE)
    shown = height is not None
    if shown:
        try:
            until(lambda: (canvas_height() or height) < height, "the 3D view shorter", SHOW_LIMIT_S, interval_s=0.2)
        except TimeoutError:
            shown = False
    t.check("the Run Script panel shows", shown,
            f"the 3D view is {canvas_height()} high {SHOW_LIMIT_S} s after the toggle, {height} before")
    time.sleep(PAGE_LOAD_S)
    t.check("widget_list lists no node of the Run Script panel", webview_nodes() == nodes_before,
            f"{nodes_before} / {webview_nodes()}")
    t.check("webview_run_javascript without a target behaves as before the panel", eval_outcome() == prepare_eval,
            f"{prepare_eval} / {eval_outcome()}")
    key = client.call("keyboard_press", keys="Escape")
    t.check("a key without a target reaches the 3D view", "canvas" in str(key.get("to", "")).lower(), key)
    t.check("a window screenshot works with the panel shown",
            bool(client.call("window_screenshot", target="main").get("path")))
    python = client.await_job(client.call("script_run", code="print('agent run')"))
    t.check("an agent's script_run finishes while the panel is shown", python.get("status") == "finished", python)
    client.call("action_run", id=RUN_SCRIPT_TOGGLE)  # hidden again

    out = tempfile.mkdtemp(prefix="orca-runner-")
    smoke = client.await_job(client.call("replay_run", path=os.path.join(HERE, "smoke_test.json"), out_dir=out))
    t.check("replay_run still runs the smoke test", smoke.get("passed") is True, smoke.get("failed_step"))

    nested = {
        "orca_replay_version": "0.2.0",
        "name": "nested",
        "steps": [{"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "app_state"}]}},
                   "expect": {"/passed": {"eq": True}}}],
    }
    result = client.await_job(client.call("replay_run", replay=nested, out_dir=os.path.join(out, "nested")))
    t.check("a replay nesting an inline replay_run passes", result.get("passed") is True, result)


# ---- Refs whose items changed, and the session exported with them ----

# Bundled with OrcaSlicer, so every system lists them. The first pick takes the first that is not the font shown, so
# that it changes the button's label whatever font an earlier test left the gizmo's style on.
FONTS = ("HarmonyOS Sans SC", "NanumGothic")
HEIGHTMAP = os.path.join(HERE, "tests", "fixtures", "heightmap.png")


def snapshot_ref(client, pattern):
    _, texts = client.call_with_text("widget_list")
    found = re.search(pattern, "\n".join(texts))
    return found.groups() if found else None


def layer_rows(client):
    """The names of the layer rows widget_list lists, "Layer N", leaving out the thumbnails ("Layer N image"). A layer
    row is named by its place in the list counted from 1; the open layer has no row."""
    _, texts = client.call_with_text("widget_list")
    return re.findall(r"name=(Layer \d+) (?!image)", "\n".join(texts))


def layer_slots(client):
    """Each texture layer's slot, from object_list's painted.texture."""
    return [layer["slot"] for layer in client.call("object_list", detail=True)["plates"][0]["objects"][0]["parts"][0]["painted"]["texture"]]


def refs_after_change(client, t):
    # The exported session starts with the start's own calls, so its replay starts from the same new project.
    client.call("replay_export", clear=True)
    t.equal(f"refs: {START_CHECK}", start(client), START)
    client.call("dialog_auto_answer", rules=[{"match": "Uniform scale", "button": "ok"},
                                             {"match": "unsaved changes", "button": "no"},
                                             {"match": "text volume is empty", "button": "ok"}])
    client.call("action_run", id="orca_command:add_primitive_cube:orca")
    wait(client, "idle")

    # One ref to the font button, used for two picks.
    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Text"})
    wait(client, "idle")
    font = snapshot_ref(client, r'\[(g\d+)\] imgui "([^"]*)"[^\n]*name=Combo_Font')
    t.check("the snapshot lists the font button", font is not None, "no font button")
    if font:
        ref, shown = font
        pick = next(f for f in FONTS if not shown.startswith(f))
        first = client.call("widget_set_value", target=ref, value=pick)
        wait(client, "idle")
        back = client.call("widget_set_value", target=ref, value=shown)
        wait(client, "idle")
        t.check("the ref picks another font", first.get("selected") == pick, first)
        t.check("the same ref picks the first font back", back.get("selected", "").startswith(shown), back)

    # A ref to the second layer's row, clicked once the first layer is gone and the row is "Layer 1"
    # (the third layer, the open one, has no row). Opening the gizmo makes the first layer, slot 0, which the
    # import textures; each layer added takes the first free slot, so the two added are 1 and 2, slot 2 open.
    # The layers are listed by slot, so the Remove of index 0 is slot 0's.
    client.call("object_select", objects=[0])
    client.call("widget_click", target={"gizmo": "Texture Displacement"})
    client.call("gizmo_pick_file", action="texture_import", path=HEIGHTMAP)
    client.call("widget_click", target={"imgui_name": "add_layer"})
    client.call("widget_click", target={"imgui_name": "add_layer"})
    row = snapshot_ref(client, r'\[(g\d+)\][^\n]*name=Layer 2 (?!image)')
    t.check("the snapshot lists the second layer's row", row is not None, "no Layer 2 row")
    if row:
        client.call("widget_click", target={"imgui_name": "Remove", "index": 0})
        slots, rows = layer_slots(client), layer_rows(client)
        t.check("the Remove leaves the second layer's row as Layer 1",
                slots == [1, 2] and "Layer 1" in rows and "Layer 2" not in rows, (slots, rows))
        # The click opens slot 1, the layer the ref was taken for, and slot 2 becomes the row, "Layer 2". A ref
        # that resolved to nothing, to the open layer or to a Remove button leaves other slots or rows.
        clicked = client.call("widget_click", target=row[0])
        slots, rows = layer_slots(client), layer_rows(client)
        t.check("the old ref clicks the renamed row",
                slots == [1, 2] and "Layer 2" in rows and "Layer 1" not in rows, (clicked, slots, rows))
    # Its icon closes it: in a small window the object's info notification covers the panel's Close.
    client.call("widget_click", target={"gizmo": "Texture Displacement"})
    wait(client, "idle")

    out_dir = tempfile.mkdtemp(prefix="orca-mcp-")
    path = os.path.join(out_dir, "session.json")
    client.call("replay_export", path=path, overwrite=True)
    replay = client.await_job(client.call("replay_run", path=path, out_dir=out_dir))
    failed = [s for s in replay.get("steps", []) if not s.get("ok")]
    t.check("the exported session replays", replay.get("passed") is True, failed[:1])


# ---- The UI job worker's admission while an arrange runs ----

# object_arrange goes first: the rotation it saves, as the panel's checkbox does, makes the others slow too.
STARTERS = {
    "object_arrange": ("object_arrange", {"rotate": True}),
    "action_run": ("action_run", {"id": "orca_command:obj_arrange:orca"}),
    "widget_click": ("widget_click", {"target": {"imgui": "Arrange", "window": "Arrange options"}}),
}


def new_client():
    """A client of the same app, for a call made on a thread of its own."""
    c, _ = orca_mcp.session_client()
    c.initialize()
    return c


def attempt(c, tool, **args):
    try:
        return True, c.call(tool, **args)
    except orca_mcp.McpError as e:
        return False, str(e)


def worker_admission(c, t):
    def holds_project():
        """Whether a running job's operation holds the project: a change that moves nothing is refused as busy."""
        ok, reply = attempt(c, "object_transform", object=0, translate=[0, 0, 0])
        return not ok and "Busy: job" in reply

    c.call("app_wait", **{"for": "ready", "timeout_s": 50})
    t.equal(f"admission: {START_CHECK}", start(c), START)
    # Copies of three cubes fill the bed, so that an arrange with rotation runs for many seconds.
    for size in [[6, 6, 6], [4, 9, 5], [7, 3, 5]]:
        c.call("object_select", none=True)
        c.call("object_add_primitive", shape="cube", size=size)
    for o in range(3):
        c.call("object_arrange", fill_object=o, spacing=0, rotate=False)

    for name, (tool, args) in STARTERS.items():
        if name == "widget_click" and c.call("widget_list", query="Arrange options", include_menus=False).get("nodes", 0) == 0:
            c.call("widget_click", target={"gltoolbar": "arrange"})
        out = {}

        def arrange():
            out["reply"] = attempt(new_client(), tool, **args)

        starter = threading.Thread(target=arrange)
        starter.start()
        # The checks start once the arrange holds the project; when it never does, they fail on what they meet.
        try:
            until(holds_project, f"{name}: the arrange holds the project", interval_s=0.1)
        except TimeoutError as e:
            print(f"  {e}", flush=True)
        ok, reply = attempt(c, "object_transform", object=0, translate=[1, 0, 0])
        t.check(name + ": object_transform refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "object_list", limit=1)
        t.check(name + ": object_list answers", ok, reply)
        ok, reply = attempt(c, "keyboard_press", keys="Delete")
        t.check(name + ": keyboard_press Delete refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "widget_click", target={"gltoolbar": "orient"})
        t.check(name + ": GL toolbar click refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "job_cancel")
        t.check(name + ": job_cancel stops it", bool(ok and reply.get("was_running") and reply.get("job")), reply)
        starter.join(300)
        ok, reply = out.get("reply", (True, None))
        t.check(name + ": the call reports it cancelled", not ok and "cancelled" in reply, reply)
        ok, reply = attempt(c, "object_transform", object=0, translate=[-1, 0, 0])
        t.check(name + ": a change is admitted afterwards", ok, reply)


def main():
    client, session = orca_mcp.session_client()
    t = Checks()
    t.scenario("recovery under load", lambda: recovery_under_load(client, session, t))
    t.scenario("a slice cancelled while it waits", lambda: cancel_waiting_slice(client, t))
    t.scenario("the Run Script panel out of reach", lambda: run_script_hidden(client, t))
    t.scenario("refs whose items changed", lambda: refs_after_change(client, t))
    t.scenario("the UI job worker's admission", lambda: worker_admission(client, t))
    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
