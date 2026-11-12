"""The undo snapshots the app took since the last call, read from its log, for the ops_*.json replays.

No tool lists the undo history, and Plater::priv::take_snapshot() logs every snapshot it takes
("Undo / Redo snapshot taken: <name>"), so this reads those lines and keeps how many it has seen in
<datadir>/mcp/ops_undo_mark.txt. The first call of a run sets the mark; each later one returns the names
of the snapshots taken since the previous call, in order. A replay calls it with
    {"tool": "run_script", "args": {"filename": "${replay_dir}/fixtures/ops_undo.py",
                                    "code": "import ops_undo\nresult = ops_undo.taken()"}}
log_lines(text) returns the last log lines that contain a text, for what only the log says (a picker a
rule cancelled, for one).
"""
import os

import orca

KEY = "Undo / Redo snapshot taken: "


def taken():
    log = orca.host.tools.call("get_log", {"lines": 1})["file"]
    with open(log, encoding="utf-8", errors="replace") as f:
        names = [line.split(KEY, 1)[1].split(", Undo / Redo stack memory")[0] for line in f if KEY in line]
    mark = os.path.join(os.path.dirname(os.path.dirname(log)), "mcp", "ops_undo_mark.txt")
    try:
        with open(mark) as f:
            seen = int(f.read())
    except (OSError, ValueError):
        seen = 0
    if seen > len(names):  # another log file: the app was restarted
        seen = 0
    with open(mark, "w") as f:
        f.write(str(len(names)))
    return {"taken": names[seen:], "count": len(names) - seen}


def log_lines(text, last=3):
    """The last `last` lines of the app's log that contain `text`, without their timestamps."""
    log = orca.host.tools.call("get_log", {"lines": 1})["file"]
    with open(log, encoding="utf-8", errors="replace") as f:
        lines = [line.rstrip("\n") for line in f if text in line]
    return [line.split("]:", 1)[-1] for line in lines[-last:]]
