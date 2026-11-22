#!/usr/bin/env python3
"""Checks printing and sending through a print host: printer_send's Speed Dial action (send_gcode), printer_send with
plates, print_start and printer_test_host, read back through app_state.uploads, the open dialogs and an OctoPrint stub
host. The design is tools/mcp/test-entries/windows_web.md, test_send_gcode.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_send_gcode.py

Starts an OctoPrint-compatible stub host on a free loopback port, starts on the Generic Klipper Printer 0.4 nozzle,
which sends through a print host as its own route, and reads the printer agents off, as start() leaves them: with
them on, the Print button routes through an agent instead of the print host and could reach a printer. The
print-host values are set on the printer as unsaved changes, so the preset's saved ones play no part. With no
address, the sends and the host test are refused. With the stub's address, the host test answers; before slicing,
print_start is refused, and Print all and Send all are refused as the Print button offers neither; once sliced,
print_start and printer_send open the host's upload dialog, which the test cancels. Then each send runs the action,
sets the file name with widget_set_value and presses Upload with dialog_answer:
- the stub holds its answer once it has the whole file, until the row reads uploading at 99, then answers 200:
  the row reads completed, and the stub got the file without print=true;
- the replay exported from that send runs and uploads again;
- the stub holds its answer the same way, then answers 500: the row never reads completed, ends as an error naming
  HTTP 500, and the error dialog the queue opens closes with dialog_answer;
- the stub answers 500 at once: the row still ends as an error rather than staying queued.
Last, the host test fails on a port nothing listens on. Exits 0 when every check passes.
"""

import http.server
import json
import os
import re
import socket
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, dialogs, plates, start, undo_steps, unsaved,  # noqa: E402
                    until, wait)

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "OrcaCube_v2.drc")
SEND = "orca_command:send_gcode:orca"
UPLOAD_DIALOG = "Send G-code to printer host"
# The longest the stub holds an answer, for a test that never opens its gate.
HOLD_LIMIT_S = 120.0
ENDED = ("completed", "error", "cancelled")
# The undo steps a change of the current plate logs: Plater::select_plate() takes one, and PartPlateList::select_plate()
# another when the plate changes (as test_files has it).
SELECTED = ["select partplate!", "select partplate!"]


class StubHost:
    """An OctoPrint-compatible print host: it answers the version check, and takes a file posted to
    /api/files/local, answering `status` once it has read the whole request and its gate is open. The gate starts
    open; a test closes it with gate.clear() to catch an upload in flight, and opens it with gate.set()."""

    def __init__(self):
        self.status = 200
        self.gate = threading.Event()
        self.gate.set()
        self.uploads = []  # {"filename", "print"} of each file posted
        self.version_checks = 0  # the GET /api/version requests answered
        stub = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"  # answers curl's Expect: 100-continue

            def log_message(self, *args):
                pass

            def reply(self, status, body):
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def read_body(self):
                if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                    body = b""
                    while True:
                        size = int(self.rfile.readline().split(b";")[0], 16)
                        chunk = self.rfile.read(size + 2)[:size]
                        if size == 0:
                            return body
                        body += chunk
                return self.rfile.read(int(self.headers.get("Content-Length", 0)))

            def do_GET(self):
                if self.path.startswith("/api/version"):
                    stub.version_checks += 1
                    self.reply(200, {"api": "0.1", "server": "1.10.0", "text": "OctoPrint 1.10.0"})
                else:
                    self.reply(404, {"error": "not found"})

            def do_POST(self):
                body = self.read_body()
                if not self.path.startswith("/api/files/local"):
                    return self.reply(404, {"error": "not found"})
                name = re.search(rb'filename="([^"]*)"', body)
                printing = re.search(rb'name="print"\r\n\r\n(\w+)', body)
                stub.uploads.append({"filename": name.group(1).decode() if name else None,
                                     "print": printing.group(1).decode() if printing else None})
                stub.gate.wait(HOLD_LIMIT_S)
                if stub.status == 200:
                    self.reply(200, {"done": True})
                else:
                    self.reply(stub.status, {"error": "The stub host refuses the file."})

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


def closed_port():
    """A loopback port nothing listens on: one the system gave out and that was closed again."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def current_plate(client):
    """The index of the current plate, from plate_list."""
    return next(i for i, p in enumerate(plates(client)) if p["current"])


def uploads(client):
    return client.call("app_state").get("uploads", [])


def send(client, filename):
    """Sends the current plate as `filename` through the action's dialog and returns the new row's id."""
    before = len(uploads(client))
    blocked = client.call("action_run", id=SEND)
    if blocked.get("status") != "blocked_by_dialog":
        raise orca_mcp.McpError(f"the send_gcode action did not open its dialog: {blocked}", blocked)
    client.call("widget_set_value", target={"class": "wxTextCtrl", "within": UPLOAD_DIALOG}, value=filename, submit=False)
    client.call("dialog_answer", button="Upload")
    client.await_job(blocked)
    return before + 1


def follow(client, row_id, stub=None):
    """Reads the row every 0.2 s until it ends (completed, error or cancelled), and returns every (state, progress)
    read and the last row, or None for the row when 120 s pass first. With `stub`, whose closed gate holds the host's
    answer, it first reads until the row is at (uploading, 99), the whole file sent and the answer still to come, or
    has ended, and then opens the gate."""
    seen = []

    def row_when(holds):
        rows = [r for r in uploads(client) if r["id"] == row_id]
        if not rows:
            return None
        seen.append((rows[0]["state"], rows[0]["progress"]))
        return rows[0] if holds(rows[0]) else None

    try:
        if stub is not None:
            try:
                until(lambda: row_when(lambda r: (r["state"], r["progress"]) == ("uploading", 99) or r["state"] in ENDED),
                      f"upload {row_id} reads uploading at 99", interval_s=0.2)
            finally:
                stub.gate.set()
        return seen, until(lambda: row_when(lambda r: r["state"] in ENDED), f"upload {row_id} ends", timeout_s=120,
                           interval_s=0.2)
    except TimeoutError as e:
        print(e, flush=True)
        return seen, None


def close_error_dialog(client):
    state = client.call("app_wait", **{"for": "dialog", "timeout_s": 30})
    open_dialogs = client.call("app_state")["dialogs"]
    if not state.get("satisfied") or not open_dialogs:
        return None
    client.call("dialog_answer", button="ok")
    client.call("app_wait", **{"for": "no_dialog", "timeout_s": 30})
    return open_dialogs[-1]


def main():
    client, _session = orca_mcp.session_client()
    stub = StubHost()
    work = tempfile.mkdtemp(prefix="orca-send-gcode-")
    t = Checks()

    def nothing_sent(name, call, code):
        """`call` is refused with `code`, and afterwards app_state.uploads is as before, the stub got no file and no
        dialog is open."""
        rows, files = uploads(client), len(stub.uploads)
        t.refused(f"{name}: refused", call, code=code)
        t.equal(f"{name}: nothing sent", (uploads(client), len(stub.uploads), dialogs(client)), (rows, files, []))

    def cancelled(name, tool, **args):
        """`tool` opens the host's upload dialog, which Cancel closes, and nothing is sent."""
        rows, files = uploads(client), len(stub.uploads)
        blocked = client.call(tool, **args)
        t.equal(f"{name}: held by its dialog", blocked.get("status"), "blocked_by_dialog")
        t.equal(f"{name}: the dialog is the host's upload", [d["title"] for d in dialogs(client)], [UPLOAD_DIALOG])
        client.call("dialog_answer", button="Cancel")
        if blocked.get("status") == "blocked_by_dialog":
            client.await_job(blocked)
        t.equal(f"{name}: cancelled, nothing sent", (uploads(client), len(stub.uploads), dialogs(client)), (rows, files, []))

    t.equal("start: the Klipper printer with its default process and filament", start(client),
            {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
    # With printer agents on, the Print button routes through an agent instead of the print host and could reach a
    # printer: the test stops before any print call unless the agents read off, as start() leaves them.
    agents = [p.get("value") for p in client.call("preferences_get", keys=["use_printer_agents"]).get("items", [])]
    if not t.equal("start: the printer agents are off", agents, [False]):
        raise SystemExit(f"use_printer_agents is {agents}: the print calls would reach a printer; the test sends nothing")
    client.call("dialog_auto_answer", rules=[{"match": "unsaved changes", "button": "no"}])
    client.call("project_import", paths=[os.path.abspath(MODEL)])
    wait(client, "idle")

    found = client.call("action_list", section="Slice & Export", limit=200)
    t.check("action_list lists the send_gcode action", any(a["id"] == SEND for a in found["items"]), found)

    # ---- No address ----
    # Plain G-code, so the dialog proposes a .gcode name (a .gcode.3mf would make it confirm the suffix).
    client.call("settings_set", type="printer", values={"host_type": "octoprint", "print_host": "", "use_3mf": False})
    nothing_sent("the action, with no address", lambda: client.call("action_run", id=SEND), "no_print_host")
    nothing_sent("printer_send {plates: 0}, with no address", lambda: client.call("printer_send", plates=0), "no_print_host")
    t.refused("printer_test_host, with no address: refused", lambda: client.call("printer_test_host"), code="no_print_host")
    t.equal("printer_test_host, with no address: no dialog", dialogs(client), [])
    # The values set are the Klipper printer's own, so the preset differs from its stored self only by what a refused
    # call would have changed.
    t.equal("the refused calls changed nothing in the printer preset", unsaved(client, "printer"), [])

    # ---- The stub's address, the cube not sliced ----
    client.call("settings_set", type="printer", values={"print_host": f"127.0.0.1:{stub.port}"})
    checked = stub.version_checks
    tested = client.call("printer_test_host")
    t.equal("printer_test_host answered by the stub", (tested.get("ok"), tested.get("message")),
            (True, "Connection to OctoPrint is working correctly."))
    t.equal("printer_test_host: the stub answered one more version check", stub.version_checks, checked + 1)
    t.equal("printer_test_host: no dialog", dialogs(client), [])
    nothing_sent("print_start, not sliced", lambda: client.call("print_start"), "not_sliced")
    nothing_sent("print_start {plates: \"all\"}", lambda: client.call("print_start", plates="all"), "all_plates_not_offered")
    nothing_sent("printer_send {plates: \"all\"}", lambda: client.call("printer_send", plates="all"), "all_plates_not_offered")

    # A plate named in a refused call: selected for the Print button's check, then the plate that was current put back,
    # as a refused export that names a plate does.
    client.call("plate_add")
    client.call("plate_select", plate=1)
    undo_steps(client)
    nothing_sent("print_start {plates: 0}, not sliced", lambda: client.call("print_start", plates=0), "not_sliced")
    t.equal("print_start {plates: 0}: plate 1 is current again", current_plate(client), 1)
    t.equal("print_start {plates: 0}: the undo steps, two changes of plate", undo_steps(client), SELECTED + SELECTED)
    client.call("plate_select", plate=0)

    # ---- Sliced ----
    sliced = client.call("plate_slice")
    t.equal("the cube slices", sliced["plates"][0]["outcome"], "ok")
    cancelled("print_start", "print_start")
    cancelled("printer_send {plates: 0}", "printer_send", plates=0)
    # The plate a call names stays selected; plate 0 stays current for the steps below.
    client.call("plate_select", plate=1)
    undo_steps(client)
    cancelled("printer_send {plates: 0}, plate 1 current", "printer_send", plates=0)
    t.equal("printer_send {plates: 0}: plate 0 is current", current_plate(client), 0)
    t.equal("printer_send {plates: 0}: the undo steps, one change of plate", undo_steps(client), SELECTED)

    # ---- The host answers 200 once the upload is caught in flight ----
    client.call("replay_export", clear=True)
    stub.status = 200
    stub.gate.clear()
    seen, row = follow(client, send(client, "mcp_send_ok.gcode"), stub)
    t.check("the upload completes", row is not None and row["state"] == "completed" and row["progress"] == 100, row)
    t.check("the row reads uploading at 99 while the host has yet to answer", ("uploading", 99) in seen, seen)
    t.check("the row names the file and the host", row is not None and row["filename"] == "mcp_send_ok.gcode"
            and str(stub.port) in row["host"], row)
    t.equal("the stub got the file, not to print", stub.uploads[-1:], [{"filename": "mcp_send_ok.gcode", "print": "false"}])

    # ---- The exported send replays ----
    path = os.path.join(work, "send.json")
    exported = client.call("replay_export", path=path, overwrite=True)
    # Each step that acts on the dialog follows an app_wait for it, since the replayed action may open it late.
    t.equal("the send exports as action_run, widget_set_value and dialog_answer, each after an app_wait for the dialog, "
            "and the app_wait for the action",
            [s["tool"] + ("(dialog)" if s["tool"] == "app_wait" and s["args"]["for"] == "dialog" else "")
             for s in exported["replay"]["steps"]],
            ["action_run", "app_wait(dialog)", "widget_set_value", "app_wait(dialog)", "dialog_answer", "app_wait"])
    count = len(uploads(client))
    replay = client.await_job(client.call("replay_run", path=path, out_dir=os.path.join(work, "replay")))
    t.check("the replay runs", replay.get("passed") is True, replay)
    _seen, row = follow(client, count + 1)
    t.check("and uploads again", row is not None and row["state"] == "completed" and len(stub.uploads) == 2, (row, stub.uploads))

    # ---- The host answers 500 once the upload is caught in flight ----
    stub.status = 500
    stub.gate.clear()
    seen, row = follow(client, send(client, "mcp_send_refused.gcode"), stub)
    t.check("a refused upload ends as an error", row is not None and row["state"] == "error" and "HTTP 500" in row.get("error", ""), row)
    t.check("its row never reads completed", all(s != "completed" for s, _p in seen), seen)
    t.check("it reads uploading at 99 until the host answers", ("uploading", 99) in seen, seen)
    dialog = close_error_dialog(client)
    t.check("the queue's error dialog closes with dialog_answer", dialog is not None and not client.call("app_state")["dialogs"], dialog)

    # ---- The host answers 500 at once ----
    _seen, row = follow(client, send(client, "mcp_send_fast.gcode"))
    t.check("an upload refused at once ends as an error", row is not None and row["state"] == "error", row)
    close_error_dialog(client)

    # ---- A port nothing listens on ----
    client.call("settings_set", type="printer", values={"print_host": f"127.0.0.1:{closed_port()}"})
    tested = client.call("printer_test_host")
    t.check("printer_test_host on a closed port fails, saying it could not connect",
            tested.get("ok") is False and str(tested.get("message", "")).startswith("Could not connect to OctoPrint"), tested)
    t.equal("printer_test_host on a closed port: no dialog", dialogs(client), [])
    stub.server.shutdown()
    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
