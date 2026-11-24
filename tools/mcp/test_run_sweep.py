#!/usr/bin/env python3
"""Checks run_sweep.py without the app: its building blocks, and its whole run on a stand-in sweep of small scripts.

    python tools/mcp/test_run_sweep.py

entry() fills a manifest entry's defaults, takes the launch kind fresh, and refuses an unknown key (`cad` among them),
an unknown launch kind, and `exclusive` or `locks` without a `why`. discover() finds the test_*.py files beside
sweep.json and no replay. manifest_problems() names a test the manifest leaves out, an entry whose file is gone and
one whose file is not a test, and the shipped sweep.json lists every test there is. order() splits the tests into the
pool and the serial phase, each longest first with those without a time ahead, or in the order a --shuffle seed
gives, the same for the same seed. pick_next() passes over a test whose lock another holds. The app launches on the
job's data folder, always with --cad and --unrestricted-scripts, and the commands fill {bin} and {datadir}; on Linux
the app is an X11 client unless --wayland asks for a native Wayland one, --headless or --xvfb is added only when the
run asks for it, and the three flags are refused off Linux and any two of them together. Two tests of one job share
its data folder and its app's session file, and have their own temporary and working folders; a test that starts its
own apps has a session file of its own, and with --xvfb a private Xvfb display that is gone when it ends. A test
whose run raises is recorded as an error, one that runs out of time is ended so that what it started ends too, the
history file reads its earlier form and takes a bare file name, -j takes a whole number from 1 up, and --shuffle a
seed or none. The stand-in sweep runs main() on four scripts that launch nothing, and checks each run's record, the
folders it keeps, the times it records and --list. The stand-in app sweep runs main() on scripts that
run in a stand-in app, an HTTP server a stand-in orca_mcp.py launches and stops, and checks when the runner launches
and stops it. Exits 0 when every check passes. The checks are asserts, so a Python that runs with its asserts off
(-O, or PYTHONOPTIMIZE, which the sweep's runner passes on) fails the test before any of them.
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
from run_sweep import JobApp, RunContext, entry  # noqa: E402

MCP_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULTS = {"launch": "app", "setup": [], "args": [], "env": {}, "exclusive": False, "locks": [], "timeout_s": 900,
            "why": ""}


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
    assert entry({"path": "test_x.py"}) == {"path": "test_x.py", **DEFAULTS}, entry({"path": "test_x.py"})
    for kind in ("app", "fresh", "own", "none"):
        assert entry({"path": "x", "launch": kind})["launch"] == kind, kind
    for raw in ({"path": "x", "lanch": "app"}, {"path": "x", "launch": "maybe"}, {"path": "x", "exclusive": True},
                {"path": "x", "locks": ["p"]}, {"path": "x", "cad": True}, {"path": "x", "cad": False}):
        assert refused(raw), raw
    ok("entry() fills the defaults, takes the launch kinds app, fresh, own and none, and refuses an unknown key (cad "
       "among them) or launch kind, and exclusive or locks without why")


def check_manifest_problems():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as mcp_dir:
        for path in ("test_listed.py", "smoke_test.json"):
            open(os.path.join(mcp_dir, path), "w").close()
        manifest = {"test_listed": entry({"path": "test_listed.py"}), "gone": entry({"path": "test_gone.py"}),
                    "smoke_test": entry({"path": "smoke_test.json"})}
        problems = run_sweep.manifest_problems(manifest, {"test_listed": "test_listed.py", "test_a": "test_a.py"},
                                               mcp_dir)
    assert len(problems) == 3, problems
    assert any(p.startswith("test_a:") and "not in sweep.json" in p for p in problems), problems
    assert any(p.startswith("gone:") and "no such file" in p for p in problems), problems
    assert any(p.startswith("smoke_test:") and "not a test" in p for p in problems), problems
    ok("manifest_problems() names a test sweep.json leaves out, an entry whose file is gone and one whose file is not "
       "a test")


def check_order():
    manifest = {n: entry({"path": f"test_{n}.py"}) for n in "abc"}
    manifest.update({n: entry({"path": f"test_{n}.py", "exclusive": True, "why": "reads the focus"}) for n in "de"})
    got = run_sweep.order(["a", "b", "c", "d", "e"], manifest, {"a": 10.0, "b": 20.0, "d": 5.0, "e": 30.0})
    assert got == (["c", "b", "a"], ["e", "d"]), got
    ok("order() runs untimed tests first, then the longest, and puts the exclusive ones in the serial phase, longest "
       "first")

    many = {f"t{i:02}": entry({"path": f"test_t{i:02}.py"}) for i in range(20)}
    many["t00"] = entry({"path": "test_t00.py", "exclusive": True, "why": "reads the focus"})
    names, none = list(many), {}
    first = run_sweep.order(names, many, none, seed=123456)
    assert run_sweep.order(list(reversed(names)), many, none, seed=123456) == first, first
    assert first[1] == ["t00"] and sorted(first[0]) == names[1:], first
    assert first[0] != names[1:], first
    others = {tuple(run_sweep.order(names, many, none, seed=s)[0]) for s in range(1, 6)}
    assert len(others) > 1, others
    ok("order() with a seed shuffles the pool and keeps the serial phase apart, gives the same order for the same seed "
       "whatever order the names come in, and another order for another seed")


def check_pick_next():
    lock = {"locks": ["port-13694"], "why": "fixed port"}
    manifest = {"x": entry({"path": "x.py", **lock}), "y": entry({"path": "y.py", **lock}),
                "z": entry({"path": "z.py"})}
    assert run_sweep.pick_next(["x", "y", "z"], manifest, {"port-13694"}) == "z"
    assert run_sweep.pick_next(["x", "y"], manifest, {"port-13694"}) is None
    assert run_sweep.pick_next(["y", "z"], manifest, set()) == "y"
    ok("pick_next() passes over a test whose lock is held")


ROOT = os.path.join(tempfile.gettempdir(), "orca-sweep-check")


def context(platform, headless=False, xvfb=False, wayland=False, test="run", job="job-1"):
    return RunContext(mcp_dir=MCP_DIR, binary=os.path.join(ROOT, "bin", "orca-slicer"),
                      run_dir=os.path.join(ROOT, test), job_dir=os.path.join(ROOT, job), port=14101, python="python3",
                      platform=platform, headless=headless, xvfb=xvfb, wayland=wayland)


def check_launch_commands():
    ctx = context("linux")
    assert ctx.datadir == os.path.join(ROOT, "job-1", "datadir"), ctx.datadir
    launch = run_sweep.launch_command(ctx)
    assert launch == ["python3", os.path.join(MCP_DIR, "orca_mcp.py"), "launch", "--bin", ctx.binary,
                      "--datadir", ctx.datadir, "--port", "14101", "--cad", "--unrestricted-scripts"], launch
    assert run_sweep.launch_command(context("linux", wayland=True)) == launch
    # A launch with --headless or --xvfb is an X11 client on the display it starts, which its own launch sets.
    assert run_sweep.launch_command(context("linux", headless=True)) == launch + ["--headless"]
    assert run_sweep.launch_command(context("linux", xvfb=True)) == launch + ["--xvfb"]
    for platform in ("darwin", "win32"):
        mac_or_win = run_sweep.launch_command(context(platform))
        assert mac_or_win[-2:] == ["--cad", "--unrestricted-scripts"], mac_or_win
    assert run_sweep.stop_command(ctx) == ["python3", os.path.join(MCP_DIR, "orca_mcp.py"), "stop"]
    ok("the launch runs on the job's data folder, always with --cad and --unrestricted-scripts, and with --headless "
       "or --xvfb only when the run asks for it")


def check_launch_env():
    # An X11 client on the desktop by default: a Wayland compositor restores a minimized window only by
    # activating the app and reports no position, so the checks that need either are reported skipped there.
    assert run_sweep.launch_env(context("linux"), {"PATH": "p"}) == {"PATH": "p", "GDK_BACKEND": "x11"}
    assert run_sweep.launch_env(context("linux", wayland=True), {"PATH": "p"}) == {"PATH": "p", "GDK_BACKEND": "wayland"}
    assert run_sweep.launch_env(context("linux", headless=True), {"PATH": "p"}) == {"PATH": "p"}
    assert run_sweep.launch_env(context("linux", xvfb=True), {"PATH": "p"}) == {"PATH": "p"}
    assert run_sweep.launch_env(context("darwin"), {"PATH": "p"}) == {"PATH": "p"}
    ok("the launch runs the app as an X11 client on Linux unless --wayland, and leaves the backend alone with "
       "--headless or --xvfb and off Linux")


def check_test_commands():
    mac = context("darwin")
    objects = entry({"path": "test_objects.py"})
    assert run_sweep.test_command(objects, mac) == ["python3", os.path.join(MCP_DIR, "test_objects.py")]
    bridge = entry({"path": "test_stdio_bridge.py", "args": ["--bin", "{bin}"]})
    assert run_sweep.test_command(bridge, mac) == ["python3", os.path.join(MCP_DIR, "test_stdio_bridge.py"),
                                                   "--bin", mac.binary]
    assert run_sweep.setup_command(bridge, mac) is None
    web = entry({"path": "test_web_ui.py", "setup": ["--install", "{datadir}"]})
    assert run_sweep.setup_command(web, mac) == ["python3", os.path.join(MCP_DIR, "test_web_ui.py"),
                                                 "--install", os.path.join(ROOT, "job-1", "datadir")]
    ok("a test runs its script with the entry's arguments, and the commands fill {bin} and {datadir}, the job's data "
       "folder")


def check_env():
    ctx = context("darwin")
    for kind in ("app", "fresh"):
        env = run_sweep.test_env(entry({"path": "t.py", "launch": kind, "env": {"ORCA_MCP_REPLY_BUDGET_MS": "2000"}}),
                                 ctx, {"PATH": "p"})
        assert env["ORCA_MCP_SESSION"] == ctx.session == os.path.join(ctx.job_dir, "session.json"), (kind, env)
        assert env["TMPDIR"] == env["TMP"] == env["TEMP"] == ctx.tmp, env
        assert env["ORCA_MCP_REPLY_BUDGET_MS"] == "2000" and env["PATH"] == "p", env
    for kind in ("own", "none"):
        env = run_sweep.test_env(entry({"path": "t.py", "launch": kind}), ctx, {"PATH": "p"})
        assert env["ORCA_MCP_SESSION"] == os.path.join(ctx.run_dir, "session.json"), (kind, env)
    ok("a test in the job's app runs with the job's session file, one that starts its own apps with a session file of "
       "its own, and each with its own temporary folder and the entry's variables")


def check_shared_datadir():
    a, b = context("darwin", test="test_a"), context("darwin", test="test_b")
    assert a.datadir == b.datadir, (a.datadir, b.datadir)
    assert a.session == b.session, (a.session, b.session)
    assert a.app_cwd == b.app_cwd == os.path.join(ROOT, "job-1", "app-cwd"), (a.app_cwd, b.app_cwd)
    for ctx in (a, b):
        assert not ctx.app_cwd.startswith(ctx.run_dir + os.sep), (ctx.app_cwd, ctx.run_dir)
    for field in ("tmp", "cwd"):
        ours, theirs = getattr(a, field), getattr(b, field)
        assert ours != theirs and ours.startswith(a.run_dir + os.sep) and theirs.startswith(b.run_dir + os.sep), field
    ok("two tests of one job share its data folder, its app's session file and its app's working folder, outside "
       "either test's folder, and have their own temporary folder and working folder")


def check_discover():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as mcp_dir:
        os.makedirs(os.path.join(mcp_dir, "tests"))
        for path in ("test_b.py", "test_a.py", "smoke_test.json", "checks.py", "test_notes.md",
                     os.path.join("tests", "cad_doc.json"), os.path.join("tests", "test_nested.py")):
            open(os.path.join(mcp_dir, path), "w").close()
        found = run_sweep.discover(mcp_dir)
    assert found == {"test_a": "test_a.py", "test_b": "test_b.py"}, found
    ok("discover() finds the test_*.py files beside sweep.json, and no replay")


def check_run_error():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        blocker = os.path.join(root, "file")
        open(blocker, "w").close()
        ctx = RunContext(mcp_dir=MCP_DIR, binary="orca-slicer", run_dir=os.path.join(blocker, "run"),
                         job_dir=os.path.join(root, "job"), port=14101, python=sys.executable, platform=sys.platform)
        rec = run_sweep.guarded_run_test("t", entry({"path": "t.py"}), ctx, JobApp())
    assert rec["name"] == "t" and rec["exit"] == "error" and rec.get("error"), rec
    assert run_sweep.missing(["a", "t"], [{"name": "a"}]) == ["t"], run_sweep.missing(["a", "t"], [{"name": "a"}])
    ok("a test whose run raises is recorded as an error, and a test with no record is named")


ENV_WRITER = """
import json, os, sys
keys = ("DISPLAY", "GDK_BACKEND", "WAYLAND_DISPLAY")
seen = {k: os.environ.get(k) for k in keys}
seen["socket"] = os.path.exists("/tmp/.X11-unix/X" + (seen["DISPLAY"] or ":").lstrip(":"))
json.dump(seen, open(sys.argv[1], "w"))
"""


def check_own_display():
    with run_sweep.own_display(entry({"path": "t.py"}), context("linux", xvfb=True)) as env:
        assert env is None, env
    with run_sweep.own_display(entry({"path": "t.py", "launch": "own"}), context("linux")) as env:
        assert env is None, env
    name = "with --xvfb, a test that starts its own apps runs them on a private Xvfb display, gone when it ends"
    if not sys.platform.startswith("linux") or not shutil.which("Xvfb"):
        print(f"SKIP {name}: " + ("Xvfb runs only on Linux" if not sys.platform.startswith("linux")
                                  else "Xvfb is not installed"))
        ok("own_display() gives no display to a test in the job's app, nor to one that starts its own on the desktop")
        return
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        with open(os.path.join(root, "test_env.py"), "w") as f:
            f.write(ENV_WRITER)
        seen_file = os.path.join(root, "seen.json")
        ctx = RunContext(mcp_dir=root, binary="orca-slicer", run_dir=os.path.join(root, "run"),
                         job_dir=os.path.join(root, "job"), port=14101, python=sys.executable, platform="linux",
                         xvfb=True)
        e = entry({"path": "test_env.py", "launch": "own", "args": [seen_file]})
        rec = run_sweep.run_test("test_env", e, ctx, JobApp())
        assert rec["exit"] == 0, rec
        with open(seen_file) as f:
            seen = json.load(f)
        number = (seen["DISPLAY"] or ":").lstrip(":")
        assert number.isdigit() and 90 <= int(number) < 120, seen
        assert seen == {"DISPLAY": f":{number}", "GDK_BACKEND": "x11", "WAYLAND_DISPLAY": None, "socket": True}, seen
        with open(rec["log"]) as f:
            assert f"private display :{number}" in f.read(), rec["log"]
        assert not os.path.exists(f"/tmp/.X11-unix/X{number}"), f"Xvfb :{number} outlived the test"
    ok("own_display() gives no display to a test in the job's app, nor to one that starts its own on the desktop; "
       + name)


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
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        here = os.getcwd()
        os.chdir(root)
        try:
            with open("flat.json", "w") as f:
                json.dump({"a": 1.5}, f)
            assert run_sweep.read_history("flat.json") == {"a": 1.5}
            run_sweep.write_history("times.json", {"a": 1.5})
            assert run_sweep.read_history("times.json") == {"a": 1.5}
            with open("times.json") as f:
                assert json.load(f) == {"times": {"a": 1.5}}
            assert run_sweep.read_history("missing.json") == {}
            with open("broken.json", "w") as f:
                f.write("not json")
            assert run_sweep.read_history("broken.json") == {}
        finally:
            os.chdir(here)
    ok("the history file reads its earlier flat form as times, keeps the times through a bare file name, and reads as "
       "empty when missing or not JSON")


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

    assert run_sweep.parse_args([]).shuffle is None
    assert run_sweep.parse_args(["--shuffle", "123456"]).shuffle == 123456
    picked = run_sweep.parse_args(["--shuffle", "--list"])
    assert type(picked.shuffle) is int and picked.list, picked
    ok("--shuffle takes a seed, picks one when given none, and leaves the order alone when not given")


def check_display_options():
    def parse(*flags):
        return run_sweep.parse_args(list(flags))

    single = (["--headless"], ["--xvfb"], ["--wayland"])
    for flags in ([], *single):
        assert run_sweep.option_problems(parse(*flags), "linux") == [], flags
    for flags in (["--headless", "--wayland"], ["--headless", "--xvfb"], ["--xvfb", "--wayland"]):
        assert run_sweep.option_problems(parse(*flags), "linux"), flags
    for platform in ("darwin", "win32"):
        for flags in single:
            assert run_sweep.option_problems(parse(*flags), platform), (platform, flags)

    session = {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-1"}
    assert run_sweep.session_problems(parse(), session, "linux") == []
    assert run_sweep.session_problems(parse("--wayland"), session, "linux") == []
    assert run_sweep.session_problems(parse("--headless"), {}, "linux") == []
    assert run_sweep.session_problems(parse("--xvfb"), {}, "linux") == []
    assert run_sweep.session_problems(parse(), {"WAYLAND_DISPLAY": "wayland-1"}, "linux"), "no DISPLAY"
    assert run_sweep.session_problems(parse("--wayland"), {"DISPLAY": ":0"}, "linux"), "no WAYLAND_DISPLAY"
    assert run_sweep.session_problems(parse(), {}, "darwin") == []
    ok("--headless, --xvfb and --wayland are taken one at a time on Linux, refused elsewhere and in pairs, and each "
       "backend needs its display")


def check_shipped_manifest():
    manifest = run_sweep.load_manifest(os.path.join(MCP_DIR, "sweep.json"))
    problems = run_sweep.manifest_problems(manifest, run_sweep.discover(MCP_DIR), MCP_DIR)
    assert problems == [], problems
    ok("sweep.json lists every test, and nothing that is not a test file")


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
        times = run_sweep.read_history(history)
        assert sorted(times) == ["test_load", "test_quick", "test_slow"], times
        for name in STAND_INS:
            assert os.path.isdir(os.path.join(out, name)), name
        for j in {r["job"] for r in recs}:
            assert os.path.isdir(os.path.join(out, f"job-{j}", "datadir")), j
        ok("the stand-in sweep runs a test that fails beside others again alone on its job and counts it passed, runs "
           "the exclusive one alone, and records each run's job and what ran before it there")

        code, printed = sweep(mcp_dir, "--list", *common)
        lines = printed.splitlines()
        assert code == 0 and [line.split()[0] for line in lines] == \
            ["test_broken", "test_slow", "test_load", "test_quick"], (code, printed)
        assert len(lines[2].split()) == 2 and "runs alone" in lines[3], printed
        ok("--list keeps a test that passed only alone among the others, and puts the exclusive one in the serial "
           "phase with its why")

        code, printed = sweep(mcp_dir, "--list", "--shuffle", "7", *common)
        lines = printed.splitlines()
        assert code == 0 and lines[0] == "shuffled with seed 7", (code, printed)
        assert sorted(line.split()[0] for line in lines[1:4]) == ["test_broken", "test_load", "test_slow"], printed
        assert [line.split()[0] for line in lines[4:]] == ["test_quick"], printed
        assert sweep(mcp_dir, "--list", "--shuffle", "7", *common) == (code, printed)
        code, printed = sweep(mcp_dir, "--list", "--shuffle", *common)
        assert code == 0 and printed.startswith("shuffled with seed "), (code, printed)
        ok("--list --shuffle names the seed first, keeps the serial phase after the pool, and lists the same order for "
           "the same seed; --shuffle alone names the seed it picked")

        out = os.path.join(root, "second")
        code, printed = sweep(mcp_dir, "-j", "2", "--out", out, *common)
        assert code == 1, (code, printed)
        recs = records(out)
        load = [(r["phase"], r["exit"]) for r in recs if r["name"] == "test_load"]
        assert load == [("pool", 1), ("alone", 0)], (load, printed)
        assert os.path.isfile(os.path.join(out, "test_load", "test.log")), os.listdir(out)
        for name in ("test_load-alone", "test_slow", "test_quick"):
            assert not os.path.exists(os.path.join(out, name)), name
        assert os.path.isfile(os.path.join(out, "test_broken", "test.log"))
        assert os.path.isfile(os.path.join(out, "test_broken", "broken.log"))
        broken_job = next(r["job"] for r in recs if r["name"] == "test_broken")
        for j in {r["job"] for r in recs}:
            assert os.path.isdir(os.path.join(out, f"job-{j}")) == (j == broken_job), j
        ok("a later sweep runs a test that passed only alone among the others again, keeps the folder of its run "
           "beside them and deletes what passed, and keeps a failed test's folder with the app's log and its job's "
           "data folder")
    shutil.rmtree(ROOT, ignore_errors=True)


# ---- The stand-in app sweep: scripts that run in a stand-in app, which a stand-in orca_mcp.py launches and stops ----

# The stand-in app is an HTTP server that answers every MCP call with an empty result, with the data folder on its
# command line, as the app's is, so that the runner's wait for the app's processes waits for it. It keeps the working
# folder of its launch, as the app launched with --datadir does. Each launch and stop is noted in apps.txt in the data
# folder: "launch <pid> <flags> budget=<ORCA_MCP_REPLY_BUDGET_MS> cwd=<working folder>" and "stop <pid>".
FAKE_ORCA_MCP = r'''
import http.server, json, os, signal, subprocess, sys, time, urllib.request


class Answer(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])) or b"{}")
        reply = json.dumps({"jsonrpc": "2.0", "id": body.get("id"),
                            "result": {"content": [{"type": "text", "text": "{}"}]}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


def note(datadir, line):
    with open(os.path.join(datadir, "apps.txt"), "a") as f:
        f.write(line + "\n")


def answers(port):
    try:
        urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}/mcp", data=b"{}"), timeout=2)
        return True
    except OSError:
        return False


command, args = sys.argv[1], sys.argv[2:]
if command == "serve":
    http.server.HTTPServer(("127.0.0.1", int(args[0])), Answer).serve_forever()
elif command == "launch":
    datadir, port = args[args.index("--datadir") + 1], int(args[args.index("--port") + 1])
    os.makedirs(datadir, exist_ok=True)
    app = subprocess.Popen([sys.executable, os.path.abspath(__file__), "serve", str(port), datadir],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(os.environ["ORCA_MCP_SESSION"], "w") as f:
        json.dump({"port": port, "token": "", "pid": app.pid, "datadir": datadir}, f)
    flags = [a for a in args if a in ("--cad", "--unrestricted-scripts", "--headless")]
    note(datadir, " ".join(["launch", str(app.pid), *flags,
                            "budget=" + os.environ.get("ORCA_MCP_REPLY_BUDGET_MS", "-"), "cwd=" + os.getcwd()]))
    deadline = time.monotonic() + 30
    while not answers(port):
        if time.monotonic() > deadline:
            sys.exit("the stand-in app did not answer")
        time.sleep(0.1)
elif command == "stop":
    with open(os.environ["ORCA_MCP_SESSION"]) as f:
        s = json.load(f)
    try:
        os.kill(s["pid"], signal.SIGTERM)
    except OSError:
        pass
    note(s["datadir"], f"stop {s['pid']}")
    os.remove(os.environ["ORCA_MCP_SESSION"])
'''

# Each script notes in visits.txt in its data folder "<name> <pid of the app its session file names, or -> <session
# file>"; its setup run notes "<name>-setup ...". test_hang runs past its time limit, test_kill ends the app and waits
# until it no longer answers, and test_fails fails.
APP_STAND_IN = r'''
import json, os, signal, sys, time, urllib.request
name = os.path.splitext(os.path.basename(__file__))[0]
setup = sys.argv[1] == "--setup"
datadir, session = sys.argv[-1], os.environ["ORCA_MCP_SESSION"]
app = None
if os.path.exists(session):
    with open(session) as f:
        app = json.load(f)
os.makedirs(datadir, exist_ok=True)
with open(os.path.join(datadir, "visits.txt"), "a") as f:
    f.write(f"{name}{'-setup' if setup else ''} {app['pid'] if app else '-'} {session}\n")
if name == "test_hang" and not setup:
    time.sleep(120)
elif name == "test_kill":
    os.kill(app["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{app['port']}/mcp", data=b"{}"), timeout=2)
            time.sleep(0.1)
        except OSError:
            break
sys.exit(1 if name == "test_fails" else 0)
'''
APP_STAND_INS = {
    "test_first": {}, "test_own": {"launch": "own"}, "test_second": {}, "test_fresh": {"launch": "fresh"},
    "test_hang": {"timeout_s": 3}, "test_after": {}, "test_kill": {}, "test_revived": {},
    "test_budget": {"env": {"ORCA_MCP_REPLY_BUDGET_MS": "4000"}}, "test_setup": {"setup": ["--setup", "{datadir}"]},
    "test_fails": {}, "test_serial": {"exclusive": True, "why": "runs alone"},
}


def lines(path):
    with open(path) as f:
        return [line.split(" ", 2) for line in f.read().splitlines()]


def launch_notes(apps):
    """The (flags, budget, working folder) of each launch line of apps.txt; the folder is last, since it may hold
    spaces."""
    notes = []
    for a in apps:
        if a[0] == "launch":
            head, _, cwd = a[2].partition(" cwd=")
            *flags, budget = head.split()
            notes.append((flags, budget, cwd))
    return notes


def same_folder(a, b):
    """Whether two paths name one folder, links resolved: the temporary folder can be reached through one (/var on
    macOS), and a process's working folder names the real path."""
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def check_stand_in_app_sweep():
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        mcp_dir = os.path.join(root, "mcp")
        os.makedirs(mcp_dir)
        with open(os.path.join(mcp_dir, "orca_mcp.py"), "w") as f:
            f.write(FAKE_ORCA_MCP)
        tests = {}
        for name, raw in APP_STAND_INS.items():
            with open(os.path.join(mcp_dir, name + ".py"), "w") as f:
                f.write(APP_STAND_IN)
            tests[name] = {"path": name + ".py", "args": ["{datadir}"], **raw}
        with open(os.path.join(mcp_dir, "sweep.json"), "w") as f:
            json.dump({"tests": tests}, f)
        out = os.path.join(root, "apps")
        # The stand-in app copies no profiles, so the --out a sweep's long temporary folder gives is no trouble.
        longest = run_sweep.MAX_OUT_WINDOWS
        run_sweep.MAX_OUT_WINDOWS = len(out)
        try:
            code, printed = sweep(mcp_dir, "-j", "1", "--keep", "--bin", sys.executable, "--out", out,
                                  "--history", os.path.join(root, "history.json"), "--first-port", "14390")
        finally:
            run_sweep.MAX_OUT_WINDOWS = longest
        assert code == 1, (code, printed)
        job, serial = os.path.join(out, "job-1"), os.path.join(out, f"job-{run_sweep.SERIAL_JOB}")

        # A to F run the pool; G and H are the runs alone of test_hang and test_fails, which failed there.
        apps = lines(os.path.join(job, "datadir", "apps.txt"))
        launched = [a[1] for a in apps if a[0] == "launch"]
        assert [a[0] for a in apps] == ["launch", "stop"] * 8, apps
        assert [a[1] for a in apps if a[0] == "stop"] == launched and len(set(launched)) == 8, apps
        A, B, C, D, E, F, G, H = launched
        notes = launch_notes(apps)
        assert [budget for _, budget, _ in notes] == ["budget=-"] * 4 + ["budget=4000"] + ["budget=-"] * 3, apps
        for flags, _, cwd in notes:
            assert flags[:2] == ["--cad", "--unrestricted-scripts"], apps
            # Not the launching test's folder, which the runner deletes when that test passes.
            assert same_folder(cwd, os.path.join(job, "app-cwd")), (cwd, job)
        own_session = os.path.join(out, "test_own", "session.json")
        session = os.path.join(job, "session.json")
        assert lines(os.path.join(job, "datadir", "visits.txt")) == [
            ["test_first", A, session], ["test_own", "-", own_session], ["test_second", A, session],
            ["test_fresh", B, session], ["test_hang", B, session], ["test_after", C, session],
            ["test_kill", C, session], ["test_revived", D, session], ["test_budget", E, session],
            ["test_setup-setup", "-", session], ["test_setup", F, session], ["test_fails", F, session],
            ["test_hang", G, session], ["test_fails", H, session]], lines(os.path.join(job, "datadir", "visits.txt"))

        recs = {(r["name"], r["phase"]): r for r in records(out)}
        launches = {key for key, r in recs.items() if r["launch_seconds"] is not None}
        assert launches == {("test_first", "pool"), ("test_fresh", "pool"), ("test_after", "pool"),
                            ("test_revived", "pool"), ("test_budget", "pool"), ("test_setup", "pool"),
                            ("test_hang", "alone"), ("test_fails", "alone"), ("test_serial", "serial")}, launches
        exits = {key: r["exit"] for key, r in recs.items() if r["exit"] != 0}
        assert exits == {("test_hang", "pool"): "timeout", ("test_fails", "pool"): 1, ("test_hang", "alone"): "timeout",
                         ("test_fails", "alone"): 1}, exits

        serial_apps = lines(os.path.join(serial, "datadir", "apps.txt"))
        assert [a[0] for a in serial_apps] == ["launch", "stop"] and serial_apps[0][1] == serial_apps[1][1], serial_apps
        assert same_folder(launch_notes(serial_apps)[0][2], os.path.join(serial, "app-cwd")), serial_apps
        assert lines(os.path.join(serial, "datadir", "visits.txt")) == [
            ["test_serial", serial_apps[0][1], os.path.join(serial, "session.json")]]
        for d in (job, serial):
            assert not os.path.exists(os.path.join(d, "session.json")), d
        for pid in launched + [serial_apps[0][1]]:
            assert not alive(int(pid)), pid
        ok("each job's tests run in one app, launched with --cad and --unrestricted-scripts in the job's app-cwd "
           "folder and named by the job's session file; a new one comes for a test marked fresh, after a test that "
           "ran out of time, after one that ended the app, for a test whose variables differ, and after a setup; a "
           "test that starts its own apps leaves it alone; each job's app is stopped at its end, and a run alone has "
           "an app of its own")


def main():
    if not asserts_live():
        return 1
    check_entry()
    check_discover()
    check_manifest_problems()
    check_order()
    check_pick_next()
    check_launch_commands()
    check_launch_env()
    check_test_commands()
    check_env()
    check_shared_datadir()
    check_run_error()
    check_own_display()
    check_timeout_ends_children()
    check_history_file()
    check_options()
    check_display_options()
    check_stand_in_sweep()
    check_stand_in_app_sweep()
    # Last: it reads the folder as it is, so a sweep.json behind a file just added stops no check of the runner.
    check_shipped_manifest()
    return 0


if __name__ == "__main__":
    sys.exit(main())
