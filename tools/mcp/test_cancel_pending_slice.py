#!/usr/bin/env python3
"""Checks that a cancel stops a slice that is still waiting to start.

    tools/mcp/orca_mcp.py launch --headless
    tools/mcp/test_cancel_pending_slice.py

A slice requested right after a setting change first waits for the update the change scheduled
(half a second), and only then starts. A cancel sent during that wait must keep it from starting:
the slice reports its plate cancelled, the Prepare tab stays shown (a slice that starts shows the
Preview), and the plate stays unsliced, so its G-code export is refused as plate_not_sliced. A slice
made afterwards is the control: it shows the Preview, slices the plate and exports. Exits 0 when
every check passes.
"""

import os
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import refusal  # noqa: E402

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "3DBenchy.drc")


def main():
    client, _ = orca_mcp.session_client()
    gcode = os.path.join(tempfile.mkdtemp(prefix="orca-mcp-"), "plate.gcode")
    checks = []

    def check(name, ok, detail):
        checks.append((name, ok, detail))

    def tab():
        return client.call("app_state").get("tab")

    def sliced():
        return client.call("plate_list")["items"][0]["sliced"]

    client.call("dialog_auto_answer", rules=[{"match": "unsaved changes", "button": "no"}])
    client.call("project_new", discard_changes=True)
    client.call("project_import", paths=[os.path.abspath(MODEL)])
    client.call("view_set", tab="prepare")
    client.call("app_wait", **{"for": "idle"})
    start = (tab(), sliced())
    check("the slice starts from Prepare, unsliced", start == ("prepare", False), start)

    client.call("settings_set", type="process", values={"sparse_infill_density": "35%"})
    box = {}
    worker = threading.Thread(target=lambda: box.update(result=client.call("plate_slice", _timeout=600)))
    worker.start()
    time.sleep(0.1)
    cancel = client.call("job_cancel")
    worker.join(600)
    result = client.await_job(box["result"])
    check("job_cancel reports the waiting slice", cancel.get("was_running") is True, cancel)
    check("the slice reports its plate cancelled",
          result.get("status") == "done" and result.get("plates") == [{"index": 0, "outcome": "cancelled"}], result)
    shown = tab()
    check("the cancel came before the slice started", shown == "prepare", f"the {shown} tab is shown")
    client.call("dialog_auto_answer", rules=[])

    # Once the scheduled update has run, the plate must still have no result to export.
    client.call("app_wait", **{"for": "idle"})
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
    check("the plate stays unsliced", not problems, "; ".join(problems))

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
    check("a slice afterwards runs and exports", not problems, "; ".join(problems))

    failures = 0
    for name, ok, detail in checks:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
