#!/usr/bin/env python3
"""Checks sending G-code to a print host with the Speed Dial's send_gcode, read back through get_state.uploads.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_send_gcode.py

Starts an OctoPrint-compatible stub host on a free loopback port. The command first refuses a printer that
prints through the Bambu network (when the app starts on a Bambu printer) and one without a print host. Then
the printer points at the stub, a cube is sliced, and each send runs send_gcode, sets the file name with
ui_input and presses Upload with dialog_respond:
- the stub answers 200 a few seconds after it has the whole file: meanwhile the row reads uploading at 99,
  then completed, and the stub got the file without print=true;
- the script exported from that send replays and uploads again;
- the stub answers 500 after the same pause: the row never reads completed, ends as an error naming
  HTTP 500, and the error dialog the queue opens closes with dialog_respond;
- the stub answers 500 at once: the row still ends as an error rather than staying queued.
Exits 0 when every check passes.
"""

import http.server
import json
import os
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "OrcaCube_v2.drc")
SEND = "orca_command:send_gcode:orca"
PAUSE_S = 3.0


class StubHost:
    """An OctoPrint-compatible print host: it answers the version check, and takes a file posted to
    /api/files/local, answering `status` `delay` seconds after it has read the whole request."""

    def __init__(self):
        self.status = 200
        self.delay = 0.0
        self.uploads = []  # {"filename", "print"} of each file posted
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
                time.sleep(stub.delay)
                if stub.status == 200:
                    self.reply(200, {"done": True})
                else:
                    self.reply(stub.status, {"error": "The stub host refuses the file."})

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


def uploads(client):
    return client.call("get_state").get("uploads", [])


def send(client, filename):
    """Sends the current plate as `filename` through the command's dialog and returns the new row's id."""
    before = len(uploads(client))
    blocked = client.call("run_action", id=SEND)
    if blocked.get("status") != "blocked_by_dialog":
        raise orca_mcp.McpError(f"send_gcode did not open its dialog: {blocked}", blocked)
    client.call("ui_input", target={"class": "wxTextCtrl", "within": "Send G-code to printer host"}, value=filename, submit=False)
    client.call("dialog_respond", button="Upload")
    client.await_job(blocked)
    return before + 1


def follow(client, row_id, timeout=120):
    """Polls the row until it is done; returns every (state, progress) read and the last row."""
    seen, deadline = [], time.time() + timeout
    while time.time() < deadline:
        rows = [r for r in uploads(client) if r["id"] == row_id]
        if rows:
            row = rows[0]
            seen.append((row["state"], row["progress"]))
            if row["state"] in ("completed", "error", "cancelled"):
                return seen, row
        time.sleep(0.2)
    return seen, None


def refused(client, needle):
    try:
        opened = client.call("run_action", id=SEND)
    except orca_mcp.McpError as e:
        return needle in str(e)
    if opened.get("status") == "blocked_by_dialog":
        client.call("dialog_respond", button="Cancel")
        client.await_job(opened)
    return False


def close_error_dialog(client):
    state = client.call("wait", **{"for": "dialog", "timeout_s": 10})
    dialogs = client.call("get_state")["dialogs"]
    if not state.get("satisfied") or not dialogs:
        return None
    client.call("dialog_respond", button="ok")
    client.call("wait", **{"for": "no_dialog", "timeout_s": 10})
    return dialogs[-1]


def main():
    client, _session = orca_mcp.session_client()
    stub = StubHost()
    work = tempfile.mkdtemp(prefix="orca-send-gcode-")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    client.call("dialog_auto_answer", rules=[{"match": "unsaved changes", "button": "no"}])
    client.call("new_project", discard_changes=True)
    client.call("open_files", paths=[os.path.abspath(MODEL)])
    client.call("wait", **{"for": "idle"})

    found = client.call("list_actions", query="Send G-code")
    check("list_actions finds the command", any(a["id"] == SEND for a in found["actions"]), found)

    # ---- Refusals ----
    if client.call("get_state")["presets"]["printer"].startswith("Bambu Lab"):
        client.call("set_settings", scope="printer", values={"bbl_use_printhost": False})
        check("a Bambu network printer is refused", refused(client, "Print plate"))
        client.call("set_settings", scope="printer", values={"bbl_use_printhost": True})
    # Plain G-code, so the dialog proposes a .gcode name (a .gcode.3mf would make it confirm the suffix).
    client.call("set_settings", scope="printer", values={"host_type": "octoprint", "print_host": "", "use_3mf": False})
    check("a printer without a print host is refused", refused(client, "no print host"))

    client.call("set_settings", scope="printer", values={"print_host": f"127.0.0.1:{stub.port}"})
    sliced = client.call("slice")
    check("the cube slices", sliced["plates"][0]["outcome"] == "ok", sliced)

    # ---- The host answers 200 after a pause ----
    client.call("export_script", clear=True)
    stub.status, stub.delay = 200, PAUSE_S
    seen, row = follow(client, send(client, "mcp_send_ok.gcode"))
    check("the upload completes", row is not None and row["state"] == "completed" and row["progress"] == 100, row)
    check("the row reads uploading at 99 while the host has yet to answer", ("uploading", 99) in seen, seen)
    check("the row names the file and the host", row is not None and row["filename"] == "mcp_send_ok.gcode"
          and str(stub.port) in row["host"], row)
    check("the stub got the file, not to print", stub.uploads[-1:] == [{"filename": "mcp_send_ok.gcode", "print": "false"}],
          stub.uploads)

    # ---- The exported send replays ----
    path = os.path.join(work, "send.json")
    exported = client.call("export_script", path=path, overwrite=True)
    # Each step that acts on the dialog follows a wait for it, since the replayed command may open it late.
    check("the send exports as run_action, ui_input and dialog_respond, each after a wait for the dialog, and the "
          "wait for the command",
          [s["tool"] + ("(dialog)" if s["tool"] == "wait" and s["args"]["for"] == "dialog" else "")
           for s in exported["script"]["steps"]] ==
          ["run_action", "wait(dialog)", "ui_input", "wait(dialog)", "dialog_respond", "wait"],
          exported["script"])
    count = len(uploads(client))
    replay = client.await_job(client.call("run_script", path=path, out_dir=os.path.join(work, "replay")))
    check("the script replays", replay.get("passed") is True, replay)
    _seen, row = follow(client, count + 1)
    check("and uploads again", row is not None and row["state"] == "completed" and len(stub.uploads) == 2, (row, stub.uploads))

    # ---- The host answers 500 after a pause ----
    stub.status, stub.delay = 500, PAUSE_S
    seen, row = follow(client, send(client, "mcp_send_refused.gcode"))
    check("a refused upload ends as an error", row is not None and row["state"] == "error" and "HTTP 500" in row.get("error", ""), row)
    check("its row never reads completed", all(s != "completed" for s, _p in seen), seen)
    check("it reads uploading at 99 until the host answers", ("uploading", 99) in seen, seen)
    dialog = close_error_dialog(client)
    check("the queue's error dialog closes with dialog_respond", dialog is not None and not client.call("get_state")["dialogs"], dialog)

    # ---- The host answers 500 at once ----
    stub.delay = 0.0
    _seen, row = follow(client, send(client, "mcp_send_fast.gcode"))
    check("an upload refused at once ends as an error", row is not None and row["state"] == "error", row)
    close_error_dialog(client)

    # Leave an empty project behind, so a script run next on this app does not meet its unsaved changes.
    client.call("new_project", discard_changes=True)
    client.call("dialog_auto_answer", rules=[])
    stub.server.shutdown()
    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
