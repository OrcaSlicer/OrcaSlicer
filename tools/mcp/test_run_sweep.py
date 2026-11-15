#!/usr/bin/env python3
"""Checks run_sweep.py's manifest, ordering and commands, without the app.

    python tools/mcp/test_run_sweep.py

entry() fills a manifest entry's defaults and refuses an unknown key, an unknown launch kind, and
`exclusive` or `locks` without a `why`. manifest_problems() names a test the manifest leaves out and
an entry whose file is gone, and the shipped sweep.json lists every test there is. order() puts the
longest tests first, those without a time before them, and the exclusive ones apart. pick_next()
passes over a test whose lock another holds. The commands launch with --cad when asked and fill {bin} and
{datadir}; on Linux the app is an X11 client unless --wayland asks for a native Wayland one, --headless is
added only when the run asks for it, and both flags are refused off Linux and together. A replay runs into
the test's own folder; a test's environment has its own session file and temporary folder. Two test files
with one name are refused, a test whose run raises is recorded as an error, one that runs out of time is
ended so that what it started ends too, --history takes a bare file name, and -j or --jobs takes a whole
number from 1 up. Exits 0 when every check passes.
"""

import contextlib
import os
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
    print(f"ok - {name}")


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
    manifest = {n: entry({"path": n + ".json"}) for n in "abc"}
    manifest["d"] = entry({"path": "d.json", "exclusive": True, "why": "reads the focus"})
    got = run_sweep.order(["a", "b", "c", "d"], manifest, {"a": 10.0, "b": 100.0})
    assert got == (["c", "b", "a"], ["d"]), got
    ok("order() runs untimed tests first, then the longest, and keeps the exclusive ones apart")


def check_pick_next():
    lock = {"locks": ["port-13694"], "why": "fixed port"}
    manifest = {"x": entry({"path": "x.py", **lock}), "y": entry({"path": "y.py", **lock}),
                "z": entry({"path": "z.py"})}
    assert run_sweep.pick_next(["x", "y", "z"], manifest, {"port-13694"}) == "z"
    assert run_sweep.pick_next(["x", "y"], manifest, {"port-13694"}) is None
    assert run_sweep.pick_next(["y", "z"], manifest, set()) == "y"
    ok("pick_next() passes over a test whose lock is held")


def context(platform, headless=False, wayland=False):
    root = os.path.join(tempfile.gettempdir(), "orca-sweep-check")
    return RunContext(mcp_dir=MCP_DIR, binary=os.path.join(root, "bin", "orca-slicer"),
                      run_dir=os.path.join(root, "run"), port=14101, python="python3", platform=platform,
                      headless=headless, wayland=wayland)


def check_commands():
    cad = entry({"path": os.path.join("tests", "cad_doc.json"), "cad": True})
    ctx = context("linux")
    launch = run_sweep.launch_command(cad, ctx)
    assert launch[:8] == ["python3", os.path.join(MCP_DIR, "orca_mcp.py"), "launch", "--bin", ctx.binary,
                          "--datadir", ctx.datadir, "--port"], launch
    # An X11 client on the desktop by default: a Wayland compositor restores a minimized window only by
    # activating the app and reports no position, so the checks that need either are reported skipped there.
    assert launch[-3:] == ["--port", "14101", "--cad"], launch
    assert run_sweep.launch_env(ctx, {"PATH": "p"}) == {"PATH": "p", "GDK_BACKEND": "x11"}
    wayland = context("linux", wayland=True)
    assert run_sweep.launch_command(cad, wayland)[-3:] == ["--port", "14101", "--cad"], wayland
    assert run_sweep.launch_env(wayland, {"PATH": "p"}) == {"PATH": "p", "GDK_BACKEND": "wayland"}
    # A headless launch is an X11 client on the compositor it starts, which its own launch sets.
    headless = context("linux", headless=True)
    assert run_sweep.launch_command(cad, headless)[-4:] == ["--port", "14101", "--cad", "--headless"], headless
    assert run_sweep.launch_env(headless, {"PATH": "p"}) == {"PATH": "p"}
    assert run_sweep.test_command(cad, ctx) == ["python3", os.path.join(MCP_DIR, "orca_mcp.py"), "run-replay",
                                                os.path.join(MCP_DIR, "tests", "cad_doc.json"), "--out", ctx.out_dir]
    assert "--headless" not in run_sweep.launch_command(cad, context("darwin"))
    assert run_sweep.launch_env(context("darwin"), {"PATH": "p"}) == {"PATH": "p"}

    mac = context("darwin")
    bridge = entry({"path": "test_stdio_bridge.py", "args": ["--bin", "{bin}"]})
    assert run_sweep.test_command(bridge, mac) == ["python3", os.path.join(MCP_DIR, "test_stdio_bridge.py"),
                                                   "--bin", mac.binary]
    assert run_sweep.launch_command(entry({"path": "test_plugin_tools.py", "launch": "own"}), mac) is None
    assert run_sweep.setup_command(bridge, mac) is None
    web = entry({"path": "test_web_ui.py", "setup": ["--install", "{datadir}"]})
    assert run_sweep.setup_command(web, mac) == ["python3", os.path.join(MCP_DIR, "test_web_ui.py"),
                                                 "--install", mac.datadir]
    ok("the commands launch with --cad when asked and fill {bin} and {datadir}, as X11 clients unless --wayland, "
       "with --headless only when the run asks for it")


def check_env():
    ctx = context("darwin")
    env = run_sweep.test_env(entry({"path": "t.py", "env": {"ORCA_MCP_REPLY_BUDGET_MS": "2000"}}), ctx, {"PATH": "p"})
    assert env["ORCA_MCP_SESSION"] == ctx.session == os.path.join(ctx.run_dir, "session.json"), env
    assert env["TMPDIR"] == env["TMP"] == env["TEMP"] == ctx.tmp, env
    assert env["ORCA_MCP_REPLY_BUDGET_MS"] == "2000" and env["PATH"] == "p", env
    ok("a test runs with its own session file and temporary folder, and the entry's variables")


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
        ctx = RunContext(mcp_dir=MCP_DIR, binary="orca-slicer", run_dir=os.path.join(blocker, "run"), port=14101,
                         python=sys.executable, platform=sys.platform)
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
    with tempfile.TemporaryDirectory(prefix="orca-sweep-check-") as root:
        here = os.getcwd()
        os.chdir(root)
        try:
            run_sweep.write_history("times.json", {"a": 1.0})
            assert run_sweep.read_history("times.json") == {"a": 1.0}
        finally:
            os.chdir(here)
    ok("--history takes a bare file name")


def check_jobs_option():
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
    ok("-j N and --jobs N set how many tests run at once, a whole number from 1 up")


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


def main():
    check_entry()
    check_manifest_problems()
    check_order()
    check_pick_next()
    check_commands()
    check_env()
    check_discover_duplicates()
    check_run_error()
    check_timeout_ends_children()
    check_history_file()
    check_jobs_option()
    check_display_options()
    check_shipped_manifest()
    return 0


if __name__ == "__main__":
    sys.exit(main())
