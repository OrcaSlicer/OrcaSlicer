#!/usr/bin/env python3
"""Checks that a cancel stops a slice that is still waiting to start.

    tools/mcp/orca_mcp.py launch --xvfb
    tools/mcp/test_cancel_pending_slice.py

A slice requested right after a setting change first waits for the update the change scheduled
(half a second), and only then starts. A cancel sent during that wait must keep it from starting:
the slice reports its plate cancelled. Exits 0 when it does.
"""

import os
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "3DBenchy.drc")


def main():
    client, _ = orca_mcp.session_client()
    client.call("dialog_auto_answer", rules=[{"match": "unsaved changes", "button": "no"}])
    client.call("new_project", discard_changes=True)
    client.call("open_files", paths=[os.path.abspath(MODEL)])
    client.call("wait", **{"for": "idle"})

    client.call("set_settings", scope="print", values={"sparse_infill_density": "35%"})
    box = {}
    worker = threading.Thread(target=lambda: box.update(result=client.call("slice", _timeout=600)))
    worker.start()
    time.sleep(0.1)
    cancel = client.call("cancel")
    worker.join(600)
    result = client.await_job(box["result"])
    client.call("dialog_auto_answer", rules=[])
    # Once the scheduled update has run, the plate must still have no result to export.
    client.call("wait", **{"for": "idle"})
    try:
        client.call("export", format="gcode", path=os.path.join(tempfile.mkdtemp(prefix="orca-mcp-"), "plate.gcode"))
        unsliced = False
    except orca_mcp.McpError:
        unsliced = True

    failures = 0
    for name, ok, detail in [
        ("cancel reports the waiting slice", cancel.get("was_running") is True, cancel),
        ("the slice reports its plate cancelled", result["plates"][0]["outcome"] == "cancelled", result),
        ("the plate stays unsliced", unsliced, "a G-code export succeeded"),
    ]:
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
