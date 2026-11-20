#!/usr/bin/env python3
"""Checks run_sweep.py without the app: its building blocks, and its whole run on a stand-in sweep of small scripts.

    python tools/mcp/test_run_sweep.py

entry() fills a manifest entry's defaults and refuses an unknown key, an unknown launch kind, and `exclusive` or
`locks` without a `why`. manifest_problems() names a test the manifest leaves out and an entry whose file is gone,
and the shipped sweep.json lists every test there is. order() splits the tests into the pool and the serial phase,
each longest first with those without a time ahead. pick_next() passes over a test whose lock another holds. The
commands launch on the job's data folder with --cad when asked and fill {bin} and {datadir}; on Linux the app is an
X11 client unless --wayland asks for a native Wayland one, --headless is added only when the run asks for it, and
both flags are refused off Linux and together. Two tests of one job share its data folder and nothing else. Two
test files with one name are refused, a test whose run raises is recorded as an error, one that runs out of time is
ended so that what it started ends too, the history file reads its earlier form and takes a bare file name, and -j
takes a whole number from 1 up. The stand-in sweep runs main() on four scripts that launch nothing, and checks each
run's record, the folders it keeps, the serial mark it learns, --list and --forget. Exits 0 when every check passes.
The checks are asserts, so a Python that runs with its asserts off (-O, or PYTHONOPTIMIZE, which the sweep's runner
passes on) fails the test before any of them.
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_sweep  # noqa: E402
from run_sweep import RunContext, entry  # noqa: E402

MCP_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULTS = {"launch": "app", "cad": False, "setup": [], "args": [], "env": {}, "exclusive": False, "locks": [],
            "timeout_s": 900, "why": ""}


def ok(name):
    print(f"ok - {name}", flush=True)


def asserts_live():
    """Whether Python runs the asserts every other check is, printing the check's line: with them off every check
    would pass without checking anything."""
    if sys.flags.optimize > 0:
        print(f"FAIL - the asserts are live: Python runs with its asserts off (optimize level {sys.flags.optimize}, "
              "from -O or PYTHONOPTIMIZE)")
        return False
    ok("the asserts are live")
    return True


def refused(raw):
    try:
        entry(raw)
    except ValueError:
        return True
    return False


def check_entry():
    assert entry({"path": "x.json"}) == {"path": "x.json", **DEFAULTS}, entry({"path": "x.json"})
    for raw in ({"path": "x", "lanch": "app"}, {"path": "x", "launch": "maybe"}, {"path": "x", "exclusive": True},
                {"path": "x", "locks": ["p"]}):
        assert refused(raw), raw
    ok("entry() fills the defaults and refuses an unknown key or launch kind, and exclusive or locks without why")


def check_manifest_problems():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as mcp_dir:
        open(os.path.join(mcp_dir, "smoke_test.json"), "w").close()
        manifest = {"smoke_test": entry({"path": "smoke_test.json"}),
                    "gone": entry({"path": os.path.join("tests", "gone.json")})}
        problems = run_sweep.manifest_problems(manifest, {"smoke_test": "smoke_test.json",
                                                          "a": os.path.join("tests", "a.json")}, mcp_dir)
    assert len(problems) == 2, problems
    assert any(p.startswith("a:") and "not in sweep.json" in p for p in problems), problems
    assert any(p.startswith("gone:") and "no such file" in p for p in problems), problems
    ok("manifest_problems() names a test sweep.json leaves out and an entry whose file is gone")


def check_order():
    manifest = {n: entry({"path": n + ".json"}) for n in "abce"}
    manifest["d"] = entry({"path": "d.json", "exclusive": True, "why": "reads the focus"})
    history = {"times": {"a": 10.0, "b": 20.0, "d": 5.0, "e": 30.0}, "serial": {"e": "2026-11-20"}}
    got = run_sweep.order(["a", "b", "c", "d", "e"], manifest, history)
    assert got == (["c", "b", "a"], ["e", "d"]), got
    ok("order() runs untimed tests first, then the longest, and puts the exclusive and the serial-marked ones in the "
       "serial phase, longest first")


def check_pick_next():
    lock = {"locks": ["port-13694"], "why": "fixed port"}
    manifest = {"x": entry({"path": "x.py", **lock}), "y": entry({"path": "y.py", **lock}),
                "z": entry({"path": "z.py"})}
    assert run_sweep.pick_next(["x", "y", "z"], manifest, {"port-13694"}) == "z"
    assert run_sweep.pick_next(["x", "y"], manifest, {"port-13694"}) is None
    assert run_sweep.pick_next(["y", "z"], manifest, set()) == "y"
    ok("pick_next() passes over a test whose lock is held")


ROOT = os.path.join(tempfile.gettempdir(), "orca-sweep-check")


def context(platform, headless=False, wayland=False, test="run", job="job-1"):
    return RunContext(mcp_dir=MCP_DIR, binary=os.path.join(ROOT, "bin", "orca-slicer"),
                      run_dir=os.path.join(ROOT, test), job_dir=os.path.join(ROOT, job), port=14101, python="python3",
                      platform=platform, headless=headless, wayland=wayland)


def check_launch_commands():
    cad = entry({"path": os.path.join("tests", "cad_doc.json"), "cad": True})
    ctx = context("linux")
    assert ctx.datadir == os.path.join(ROOT, "job-1", "datadir"), ctx.datadir
    launch = run_sweep.launch_command(cad, ctx)
    assert launch == ["python3", os.path.join(MCP_DIR, "orca_mcp.py"), "launch", "--bin", ctx.binary,
                      "--datadir", ctx.datadir, "--port", "14101", "--cad"], launch
    assert run_sweep.launch_command(cad, context("linux", wayland=True)) == launch
    # A headless launch is an X11 client on the compositor it starts, which its own launch sets.
    assert run_sweep.launch_command(cad, context("linux", headless=True)) == launch + ["--headless"]
    assert "--headless" not in run_sweep.launch_command(cad, context("darwin"))
    assert run_sweep.launch_command(entry({"path": "test_plugin_tools.py", "launch": "own"}), context("darwin")) is None
    ok("the launch runs on the job's data folder, with --cad when asked and --headless only when the run asks for it")


def check_launch_env():
    # An X11 client on the desktop by default: a Wayland compositor restores a minimized window only by
    # activating the app and reports no position, so the checks that need either are reported skipped there.
    assert run_sweep.launch_env(context("linux"), {"PATH": "p"}) == {"PATH": "p", "GDK_BACKEND": "x11"}
    assert run_sweep.launch_env(context("linux", wayland=True), {"PATH": "p"}) == {"PATH": "p", "GDK_BACKEND": "wayland"}
    assert run_sweep.launch_env(context("linux", headless=True), {"PATH": "p"}) == {"PATH": "p"}
    assert run_sweep.launch_env(context("darwin"), {"PATH": "p"}) == {"PATH": "p"}
    ok("the launch runs the app as an X11 client on Linux unless --wayland, and leaves the backend alone headless "
       "and off Linux")


def check_test_commands():
    mac = context("darwin")
    replay = entry({"path": os.path.join("tests", "cad_doc.json"), "cad": True})
    assert run_sweep.test_command(replay, mac) == ["python3", os.path.join(MCP_DIR, "orca_mcp.py"), "run-replay",
                                                   os.path.join(MCP_DIR, "tests", "cad_doc.json"), "--out", mac.out_dir]
    bridge = entry({"path": "test_stdio_bridge.py", "args": ["--bin", "{bin}"]})
    assert run_sweep.test_command(bridge, mac) == ["python3", os.path.join(MCP_DIR, "test_stdio_bridge.py"),
                                                   "--bin", mac.binary]
    assert run_sweep.setup_command(bridge, mac) is None
    web = entry({"path": "test_web_ui.py", "setup": ["--install", "{datadir}"]})
    assert run_sweep.setup_command(web, mac) == ["python3", os.path.join(MCP_DIR, "test_web_ui.py"),
                                                 "--install", os.path.join(ROOT, "job-1", "datadir")]
    ok("a replay runs into the test's own folder, and the commands fill {bin} and {datadir}, the job's data folder")


def check_env():
    ctx = context("darwin")
    env = run_sweep.test_env(entry({"path": "t.py", "env": {"ORCA_MCP_REPLY_BUDGET_MS": "2000"}}), ctx, {"PATH": "p"})
    assert env["ORCA_MCP_SESSION"] == ctx.session == os.path.join(ctx.run_dir, "session.json"), env
    assert env["TMPDIR"] == env["TMP"] == env["TEMP"] == ctx.tmp, env
    assert env["ORCA_MCP_REPLY_BUDGET_MS"] == "2000" and env["PATH"] == "p", env
    ok("a test runs with its own session file and temporary folder, and the entry's variables")


def check_shared_datadir():
    a, b = context("darwin", test="test_a"), context("darwin", test="test_b")
    assert a.datadir == b.datadir, (a.datadir, b.datadir)
    for field in ("session", "tmp", "cwd", "out_dir"):
        ours, theirs = getattr(a, field), getattr(b, field)
        assert ours != theirs and ours.startswith(a.run_dir + os.sep) and theirs.startswith(b.run_dir + os.sep), field
    ok("two tests of one job share its data folder and have their own session file, temporary, working and output "
       "folders")


def check_discover_duplicates():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as mcp_dir:
        os.makedirs(os.path.join(mcp_dir, "tests"))
        for path in ("test_x.py", os.path.join("tests", "test_x.json")):
            open(os.path.join(mcp_dir, path), "w").close()
        try:
            run_sweep.discover(mcp_dir)
        except ValueError as e:
            assert "test_x" in str(e), e
        else:
            raise AssertionError("two tests named test_x were taken as one")
    ok("discover() refuses two test files with one name, which would run as one")


def check_run_error():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        blocker = os.path.join(root, "file")
        open(blocker, "w").close()
        ctx = RunContext(mcp_dir=MCP_DIR, binary="orca-slicer", run_dir=os.path.join(blocker, "run"),
                         job_dir=os.path.join(root, "job"), port=14101, python=sys.executable, platform=sys.platform)
        rec = run_sweep.guarded_run_test("t", entry({"path": "t.py"}), ctx)
    assert rec["name"] == "t" and rec["exit"] == "error" and rec.get("error"), rec
    assert run_sweep.missing(["a", "t"], [{"name": "a"}]) == ["t"], run_sweep.missing(["a", "t"], [{"name": "a"}])
    ok("a test whose run raises is recorded as an error, and a test with no record is named")


def alive(pid):
    if os.name == "nt":
        out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout
        return str(pid) in out
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


STAND_IN = """
import subprocess, sys, time
child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
open(sys.argv[1], "w").write(str(child.pid))
try:
    time.sleep(120)
finally:
    child.kill()
    child.wait()
"""


def check_timeout_ends_children():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        script, pid_file = os.path.join(root, "stand_in.py"), os.path.join(root, "child.pid")
        with open(script, "w") as f:
            f.write(STAND_IN)
        try:
            run_sweep.run_step([sys.executable, script, pid_file], 3, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise AssertionError("the stand-in test ended before its timeout")
        with open(pid_file) as f:
            child = int(f.read())
        deadline = time.monotonic() + 10
        while alive(child) and time.monotonic() < deadline:
            time.sleep(0.1)
        assert not alive(child), f"the stand-in's child {child} outlived its timeout"
    ok("a test that runs out of time is ended so that what it started ends too")


def check_history_file():
    empty = {"times": {}, "serial": {}}
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        here = os.getcwd()
        os.chdir(root)
        try:
            with open("flat.json", "w") as f:
                json.dump({"a": 1.5}, f)
            assert run_sweep.read_history("flat.json") == {"times": {"a": 1.5}, "serial": {}}
            history = {"times": {"a": 1.5}, "serial": {"e": "2026-11-20"}}
            run_sweep.write_history("times.json", history)
            assert run_sweep.read_history("times.json") == history
            with open("times.json") as f:
                assert json.load(f) == history
            assert run_sweep.read_history("missing.json") == empty
            with open("broken.json", "w") as f:
                f.write("not json")
            assert run_sweep.read_history("broken.json") == empty
        finally:
            os.chdir(here)
    ok("the history file reads its earlier flat form as times, keeps times and serial marks through a bare file name, "
       "and reads as empty when missing or not JSON")


def check_options():
    assert run_sweep.parse_args(["-j", "3"]).jobs == 3
    assert run_sweep.parse_args(["--jobs", "2"]).jobs == 2
    assert run_sweep.parse_args([]).jobs == min(6, max(1, (os.cpu_count() or 2) // 2))
    for bad in ("0", "-2", "x"):
        try:
            with open(os.devnull, "w") as quiet, contextlib.redirect_stderr(quiet):
                run_sweep.parse_args(["-j", bad])
        except SystemExit as e:
            assert e.code == 2, (bad, e.code)
        else:
            raise AssertionError(f"-j {bad} was taken")
    ok("-j N and --jobs N set how many jobs run at once, a whole number from 1 up")


def check_display_options():
    def parse(*flags):
        return run_sweep.parse_args(list(flags))

    for flags in ([], ["--headless"], ["--wayland"]):
        assert run_sweep.option_problems(parse(*flags), "linux") == [], flags
    assert run_sweep.option_problems(parse("--headless", "--wayland"), "linux"), "--headless --wayland"
    for platform in ("darwin", "win32"):
        for flags in (["--headless"], ["--wayland"]):
            assert run_sweep.option_problems(parse(*flags), platform), (platform, flags)

    session = {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-1"}
    assert run_sweep.session_problems(parse(), session, "linux") == []
    assert run_sweep.session_problems(parse("--wayland"), session, "linux") == []
    assert run_sweep.session_problems(parse("--headless"), {}, "linux") == []
    assert run_sweep.session_problems(parse(), {"WAYLAND_DISPLAY": "wayland-1"}, "linux"), "no DISPLAY"
    assert run_sweep.session_problems(parse("--wayland"), {"DISPLAY": ":0"}, "linux"), "no WAYLAND_DISPLAY"
    assert run_sweep.session_problems(parse(), {}, "darwin") == []
    ok("--headless and --wayland are taken on Linux, refused elsewhere and together, and each backend needs "
       "its display")


def check_shipped_manifest():
    manifest = run_sweep.load_manifest(os.path.join(MCP_DIR, "sweep.json"))
    problems = run_sweep.manifest_problems(manifest, run_sweep.discover(MCP_DIR), MCP_DIR)
    assert problems == [], problems
    ok("sweep.json lists every test, and only tests that exist")


# ---- The stand-in sweep: four scripts that launch nothing, each given its job's data folder ----

# Each stand-in appends its name to visits.txt in the data folder it is given, and holds a file named for itself in
# the shared folder `running` while it runs: test_load fails while another stand-in's file is there.
SWEEP_STAND_IN = """
import os, sys, time
name = os.path.splitext(os.path.basename(__file__))[0]
datadir, running = sys.argv[1], os.path.join(os.path.dirname(os.path.abspath(__file__)), "running")
os.makedirs(datadir, exist_ok=True)
with open(os.path.join(datadir, "visits.txt"), "a") as f:
    f.write(name + "\\n")
mine = os.path.join(running, name)
open(mine, "w").close()
code = 0
try:
    if name == "test_slow":
        time.sleep(2)
    elif name == "test_load":
        time.sleep(1)
        code = 1 if [n for n in os.listdir(running) if n != name] else 0
    elif name == "test_broken":
        os.makedirs(os.path.join(datadir, "log"), exist_ok=True)
        with open(os.path.join(datadir, "log", "broken.log"), "w") as f:
            f.write("what the app logged\\n")
        code = 1
finally:
    os.remove(mine)
sys.exit(code)
"""
STAND_INS = ("test_slow", "test_load", "test_broken", "test_quick")


def stand_in_folder(root):
    mcp_dir = os.path.join(root, "mcp")
    os.makedirs(os.path.join(mcp_dir, "running"))
    tests = {}
    for name in STAND_INS:
        with open(os.path.join(mcp_dir, name + ".py"), "w") as f:
            f.write(SWEEP_STAND_IN)
        tests[name] = {"path": name + ".py", "launch": "none", "args": ["{datadir}"]}
    tests["test_quick"].update(exclusive=True, why="runs alone")
    with open(os.path.join(mcp_dir, "sweep.json"), "w") as f:
        json.dump({"tests": tests}, f)
    return mcp_dir


def sweep(mcp_dir, *argv):
    """main() on the stand-in folder: its exit code and what it printed."""
    printed = io.StringIO()
    with contextlib.redirect_stdout(printed), contextlib.redirect_stderr(printed):
        code = run_sweep.main(list(argv), mcp_dir=mcp_dir)
    return code, printed.getvalue()


def records(out):
    with open(os.path.join(out, "results.jsonl"), encoding="utf-8") as f:
        return [json.loads(line) for line in f]


def visits(out, job):
    with open(os.path.join(out, f"job-{job}", "datadir", "visits.txt")) as f:
        return f.read().split()


def check_stand_in_sweep():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        mcp_dir, history = stand_in_folder(root), os.path.join(root, "history.json")
        common = ["--history", history, "--first-port", "14190"]

        out = os.path.join(root, "first")
        code, printed = sweep(mcp_dir, "-j", "2", "--keep", "--out", out, *common)
        assert code == 1, (code, printed)
        recs = records(out)
        got = sorted((r["name"], r["phase"], r["exit"]) for r in recs)
        assert got == sorted([("test_slow", "pool", 0), ("test_load", "pool", 1), ("test_broken", "pool", 1),
                              ("test_quick", "serial", 0), ("test_load", "alone", 0), ("test_broken", "alone", 1)]), got
        job = {(r["name"], r["phase"]): r["job"] for r in recs}
        assert job[("test_load", "alone")] == job[("test_load", "pool")], job
        assert job[("test_broken", "alone")] == job[("test_broken", "pool")], job
        assert job[("test_quick", "serial")] == run_sweep.SERIAL_JOB, job
        for j in {r["job"] for r in recs}:
            on_job = [r["name"] for r in recs if r["job"] == j]
            assert [r["after"] for r in recs if r["job"] == j] == [on_job[:i] for i in range(len(on_job))], (j, recs)
            assert visits(out, j) == on_job, (j, visits(out, j), on_job)
        lines = printed.splitlines()
        assert any(line.startswith("test_load") and "passed alone" in line for line in lines), printed
        assert any(line.startswith("FAIL test_broken") for line in lines), printed
        h = run_sweep.read_history(history)
        assert sorted(h["times"]) == ["test_load", "test_quick", "test_slow"], h
        assert list(h["serial"]) == ["test_load"], h
        for name in STAND_INS:
            assert os.path.isdir(os.path.join(out, name)), name
        for j in {r["job"] for r in recs}:
            assert os.path.isdir(os.path.join(out, f"job-{j}", "datadir")), j
        ok("the stand-in sweep runs a test that fails beside others again alone on its job, counts it passed and marks "
           "it serial, runs the exclusive one alone, and records each run's job and what ran before it there")

        code, printed = sweep(mcp_dir, "--list", *common)
        names = [line.split()[0] for line in printed.splitlines() if line.strip()]
        assert code == 0 and names == ["test_broken", "test_slow", "test_load", "test_quick"], (code, printed)
        assert "serial since" in printed.splitlines()[2] and "runs alone" in printed.splitlines()[3], printed
        ok("--list puts the serial-marked test in the serial phase with its mark, and the exclusive one with its why")

        out = os.path.join(root, "second")
        code, printed = sweep(mcp_dir, "-j", "2", "--out", out, *common)
        assert code == 1, (code, printed)
        recs = records(out)
        load = [r for r in recs if r["name"] == "test_load"]
        assert [(r["phase"], r["exit"]) for r in load] == [("serial", 0)], load
        for name in ("test_slow", "test_load", "test_quick"):
            assert not os.path.exists(os.path.join(out, name)), name
        assert os.path.isfile(os.path.join(out, "test_broken", "test.log"))
        assert os.path.isfile(os.path.join(out, "test_broken", "broken.log"))
        broken_job = next(r["job"] for r in recs if r["name"] == "test_broken")
        for j in {r["job"] for r in recs}:
            assert os.path.isdir(os.path.join(out, f"job-{j}")) == (j == broken_job), j
        h = run_sweep.read_history(history)
        assert list(h["serial"]) == ["test_load"], h
        ok("a later sweep runs the serial-marked test alone, deletes what passed, and keeps a failed test's folder with "
           "the app's log and its job's data folder")

        times = h["times"]
        out = os.path.join(root, "forget")
        code, printed = sweep(mcp_dir, "--forget", "test_load", "--out", out, *common)
        h = run_sweep.read_history(history)
        assert code == 0 and h == {"times": times, "serial": {}} and not os.path.exists(out), (code, printed, h)
        code, printed = sweep(mcp_dir, "--forget", "test_nobody", *common)
        assert code == 2, (code, printed)
        ok("--forget clears a serial mark and runs nothing, and refuses a test sweep.json does not list")

        code, printed = sweep(mcp_dir, "--list", *common)
        assert code == 0 and [line.split()[0] for line in printed.splitlines()][:3] == \
            ["test_broken", "test_slow", "test_load"] and "serial since" not in printed, printed
        ok("a test whose mark is cleared runs in the pool again")

        out = os.path.join(root, "third")
        code, printed = sweep(mcp_dir, "-j", "2", "--out", out, *common)
        load = [(r["phase"], r["exit"]) for r in records(out) if r["name"] == "test_load"]
        assert code == 1 and load == [("pool", 1), ("alone", 0)], (code, load, printed)
        assert os.path.isfile(os.path.join(out, "test_load", "test.log")), os.listdir(out)
        assert not os.path.exists(os.path.join(out, "test_load-alone")), os.listdir(out)
        assert list(run_sweep.read_history(history)["serial"]) == ["test_load"]
        ok("a test that passes alone keeps the folder of its run beside the others, and its passing run's goes")
    shutil.rmtree(ROOT, ignore_errors=True)


def main():
    if not asserts_live():
        return 1
    check_entry()
    check_manifest_problems()
    check_order()
    check_pick_next()
    check_launch_commands()
    check_launch_env()
    check_test_commands()
    check_env()
    check_shared_datadir()
    check_discover_duplicates()
    check_run_error()
    check_timeout_ends_children()
    check_history_file()
    check_options()
    check_display_options()
    check_shipped_manifest()
    check_stand_in_sweep()
    return 0


if __name__ == "__main__":
    sys.exit(main())
