#!/usr/bin/env python3
"""Checks what export_script does with a call whose job outlived its reply.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_export_running_job.py

A run_script whose one step waits 48 s replies with its running job after the 45 s budget. An export
while it runs leaves it out and names it in left_out_running; an export after it succeeded has it as
an await:false step that the later wait refers to, and that script replays. A call blocked on a
dialog stays in the script while the dialog is open, since the steps after it answer the dialog.
A job that fails after its reply is dropped; no tool fails that late on demand, so the unit tests
in test_mcp_results.cpp cover it. Takes about two minutes. Exits 0 when every check passes.
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def main():
    client, _session = orca_mcp.session_client()
    work = tempfile.mkdtemp(prefix="orca-export-running-")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def tools(script):
        return [s["tool"] for s in script["steps"]]

    client.call("export_script", clear=True)

    # ---- A job past the reply budget, exported while it runs and after it ends ----
    start = time.time()
    slow = {"orca_script": 1, "name": "slow", "steps": [{"tool": "wait", "args": {"for": "dialog", "timeout_s": 48}}]}
    started = client.call("run_script", _timeout=120, script=slow, out_dir=os.path.join(work, "slow"))
    check("the slow script replies with its running job", started.get("status") == "running",
          f"{started.get('status')} after {time.time() - start:.1f} s")
    job = started.get("job")
    client.call("set_settings", values={"wall_loops": 3})
    during = client.call("export_script")
    check("an export while it runs leaves it out", tools(during["script"]) == ["set_settings"], tools(during["script"]))
    check("and names it in left_out_running", during.get("left_out_running") == [{"tool": "run_script", "job": job}],
          during.get("left_out_running"))

    done = client.await_job(started)  # a journaled wait on the job
    check("the job then succeeds", done.get("passed") is True, done)
    path = os.path.join(work, "after.json")
    after = client.call("export_script", path=path, overwrite=True)
    steps = after["script"]["steps"]
    check("an export after it ends has it as a step", tools(after["script"]) == ["run_script", "set_settings", "wait"],
          tools(after["script"]))
    check("replayed with await:false", steps[0].get("await") is False and steps[0].get("save_as") == "job1", steps[0])
    check("the wait refers to it", steps[2]["args"].get("job") == "${jobs.job1}", steps[2])
    check("nothing is left out", "left_out_running" not in after, after.get("left_out_running"))

    # ---- A call blocked on a dialog stays while the dialog is open ----
    client.call("export_script", clear=True)
    blocked = client.call("run_action", id="orca_command:open_preferences:orca")
    check("Preferences blocks its call", blocked.get("status") == "blocked_by_dialog", blocked.get("status"))
    open_export = client.call("export_script")
    check("an export with the dialog open keeps the call", tools(open_export["script"]) == ["run_action"],
          tools(open_export["script"]))
    check("and leaves nothing out", "left_out_running" not in open_export, open_export.get("left_out_running"))
    client.call("dialog_respond", button="close")
    client.await_job(blocked)

    # ---- The script exported after the slow job replays ----
    replay = client.await_job(client.call("run_script", _timeout=120, path=path, out_dir=os.path.join(work, "replay")))
    check("the exported script replays", replay.get("passed") is True,
          json.dumps([(s["tool"], s["ok"]) for s in replay.get("steps", [])]))

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
