#!/usr/bin/env python3
"""Checks what replay_export does with a call whose job outlived its reply.
Its design is the test_export_running_job entry of tools/mcp/test-entries/runtime.md.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_export_running_job.py

A replay_run whose one step is a script that runs until this test writes its go file replies with its running job
once the reply budget is spent: 45 s, or the cap ORCA_MCP_REPLY_BUDGET_MS sets for the app (the sweep sets 20 s, as a
shorter cap can end the call opening Preferences before a slow or busy machine has opened it). The go file is a gate
the test releases once it has exported, so the job runs for as long as the export while it runs needs. An export
while it runs leaves it out and names it in left_out_running; an export after it succeeded has it as an await:false
step that the later app_wait refers to, and that replay runs. A call blocked on a dialog stays in the replay while
the dialog is open, since the steps after it answer the dialog. A job that fails after its reply is dropped; no tool
fails that late on demand, so the unit tests in test_mcp_results.cpp cover it. Takes about a minute with the full
budget, half a minute with a 20 s one. Exits 0 when every check passes.
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks as shared  # noqa: E402
import orca_mcp  # noqa: E402

START = {"printer": shared.KLIPPER, "process": shared.KLIPPER_STD, "filaments": [shared.GENERIC_PLA]}
# Runs until the file args["go"] exists, which the test writes when it is done with the running job; the deadline only
# ends a run the test never released.
GATE = """
import os
import time
deadline = time.monotonic() + 300
while not os.path.exists(args["go"]):
    if time.monotonic() > deadline:
        raise TimeoutError("the test never wrote the go file")
    time.sleep(0.1)
"""


def main():
    client, _session = orca_mcp.session_client()
    work = tempfile.mkdtemp(prefix="orca-export-running-")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def tools(replay):
        return [s["tool"] for s in replay["steps"]]

    presets = shared.start(client)
    check("the start is the Klipper printer with one Generic PLA slot", presets == START, presets)
    # The exports below read the journal from here on, without the start's own calls.
    client.call("replay_export", clear=True)

    # ---- A job past the reply budget, exported while it runs and after it ends ----
    start = time.time()
    go = os.path.join(work, "go")
    slow = {"orca_replay_version": "0.2.0", "name": "slow",
            "steps": [{"tool": "script_run", "args": {"code": GATE, "args": {"go": go}}}]}
    started = client.call("replay_run", _timeout=120, replay=slow, out_dir=os.path.join(work, "slow"))
    check("the slow replay replies with its running job", started.get("status") == "running",
          f"{started.get('status')} after {time.time() - start:.1f} s")
    job = started.get("job")
    client.call("settings_set", values={"wall_loops": 3})
    during = client.call("replay_export")
    check("an export while it runs leaves it out", tools(during["replay"]) == ["settings_set"], tools(during["replay"]))
    check("and names it in left_out_running", during.get("left_out_running") == [{"tool": "replay_run", "job": job}],
          during.get("left_out_running"))

    open(go, "w").close()
    done = client.await_job(started)  # a journaled app_wait on the job
    check("the job then succeeds", done.get("passed") is True, done)
    path = os.path.join(work, "after.json")
    after = client.call("replay_export", path=path, overwrite=True)
    steps = after["replay"]["steps"]
    check("an export after it ends has it as a step", tools(after["replay"]) == ["replay_run", "settings_set", "app_wait"],
          tools(after["replay"]))
    check("replayed with await:false", steps[0].get("await") is False and steps[0].get("save_as") == "job1", steps[0])
    check("the app_wait refers to it", steps[2]["args"].get("job") == "${jobs.job1}", steps[2])
    check("nothing is left out", "left_out_running" not in after, after.get("left_out_running"))

    # ---- A call blocked on a dialog stays while the dialog is open ----
    client.call("replay_export", clear=True)
    blocked = client.call("action_run", id="orca_command:open_preferences:orca")
    check("Preferences blocks its call", blocked.get("status") == "blocked_by_dialog", blocked.get("status"))
    open_export = client.call("replay_export")
    check("an export with the dialog open keeps the call", tools(open_export["replay"]) == ["action_run"],
          tools(open_export["replay"]))
    check("and leaves nothing out", "left_out_running" not in open_export, open_export.get("left_out_running"))
    client.call("dialog_answer", button="close")
    client.await_job(blocked)

    # ---- The replay exported after the slow job runs; its go file is still there ----
    replay = client.await_job(client.call("replay_run", _timeout=120, path=path, out_dir=os.path.join(work, "replay")))
    check("the exported replay runs", replay.get("passed") is True,
          json.dumps([(s["tool"], s["ok"]) for s in replay.get("steps", [])]))

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
