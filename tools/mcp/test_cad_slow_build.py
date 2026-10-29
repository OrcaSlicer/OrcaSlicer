#!/usr/bin/env python3
"""Checks that OrcaSlicer stays inspectable while a long Design-tab rebuild runs.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_slow_build.py

cad_call thread, sweep and loft rebuild as their cards do, off the UI thread behind the progress
dialog (DesignPanel::recompute_guarded). While a long thread builds: get_state answers, a screenshot
of the main window succeeds, ui_snapshot lists the progress dialog and a screenshot of that window
captures it (not on Windows, where the dialog is the system's task dialog), and another cad_call is refused rather than run inside the rebuild. The thread then
lands as one feature. Exits 0 when all hold.
"""

import os
import re
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

# An M8 thread tapped 60 mm deep into a 30 mm square block: 48 turns, several seconds of OCCT pipe and
# boolean work (about 8 s on an M2 Mac).
BLOCK = {"width": 30, "height": 30, "distance": 60}
THREAD = {"plane": "XY", "internal": True, "designation": "M8", "length": 60}


def progress_dialog_ref(client):
    """The ref of the rebuild's progress dialog (titled "Design") in ui_snapshot, or None."""
    _, texts = client.call_with_text("ui_snapshot")
    for line in "\n".join(texts).splitlines():
        m = re.match(r'\[(w\d+)\] (?:window|dialog) "Design" \((\w+)\)', line)
        if m and "Progress" in m.group(2):
            return m.group(1)
    return None


def main():
    client, _ = orca_mcp.session_client()
    out = tempfile.mkdtemp(prefix="orca-mcp-slow-")
    client.call("new_project", discard_changes=True)
    client.call("cad_call", method="new_design", params={"discard_changes": True})
    client.call("cad_call", method="extrude", params=BLOCK)

    box = {}

    def build():
        try:
            box["result"] = client.call("cad_call", _timeout=600, method="thread", params=THREAD)
        except orca_mcp.McpError as e:
            box["error"] = str(e)

    worker = threading.Thread(target=build)
    started = time.time()
    worker.start()

    # On Windows the progress dialog is the system's task dialog, on a thread of its own: no wx window
    # ui_snapshot lists. There the probes start once the build has run for 2 s, past the dialog's delay.
    native_dialog = sys.platform == "win32"
    probed, state_ms, window_shot, dialog_shot, refused = False, None, None, None, None
    while worker.is_alive() and time.time() - started < 300:
        time.sleep(0.5)
        if not probed:
            ref = None if native_dialog else progress_dialog_ref(client)
            if ref is not None or (native_dialog and time.time() - started > 2):
                probed = True
                t0 = time.time()
                client.call("get_state")
                state_ms = (time.time() - t0) * 1000
                window_shot = client.call("screenshot", target="window", include_image=False,
                                          save_path=os.path.join(out, "window.png"))
                if ref is not None:
                    dialog_shot = client.call("screenshot", target=ref, include_image=False,
                                              save_path=os.path.join(out, "progress.png"))
                try:
                    client.call("cad_call", method="describe_scene")
                    refused = False
                except orca_mcp.McpError as e:
                    refused = str(e)
    worker.join(600)
    seconds = time.time() - started
    result = client.await_job(box.get("result")) if "result" in box else None
    scene = client.call("cad_call", method="describe_scene")["result"]
    threads = [f for f in scene["features"] if f["type"] == "Thread"]

    print(f"build took {seconds:.1f} s; screenshots in {out}")
    failures = 0
    checks = [
        ("the thread call succeeds", result is not None and result.get("result", {}).get("ok") is True, box),
        ("get_state answers during the build", state_ms is not None, state_ms),
        ("a window screenshot succeeds during the build", bool(window_shot and window_shot.get("width")), window_shot),
        ("another cad_call is refused during the build", bool(refused) and "Busy" in refused, refused),
        ("one thread feature and one body", len(threads) == 1 and len(scene["bodies"]) == 1, scene["features"]),
    ]
    if not native_dialog:
        checks[1:1] = [
            ("the progress dialog is listed while it builds", dialog_shot is not None, "no window \"Design\" in ui_snapshot"),
            ("a screenshot of the progress dialog captures it",
             bool(dialog_shot and dialog_shot.get("width")) and dialog_shot.get("width") < (window_shot or {}).get("width", 0), dialog_shot),
        ]
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
