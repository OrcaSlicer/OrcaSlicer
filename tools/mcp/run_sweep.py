#!/usr/bin/env python3
"""Runs the MCP sweep: every Python test in tools/mcp, on several jobs at once, each job running its tests one after
another in one app on one data folder.

    tools/mcp/run_sweep.py [--bin PATH] [-j N] [--only NAME ...] [--skip NAME ...] [--out DIR] [--history FILE]
                           [--keep] [--headless | --xvfb | --wayland] [--shuffle [SEED]] [--list]

The tests are the test_*.py files in tools/mcp; a replay, such as smoke_test.json, is not one. sweep.json lists each
test with what it needs, and a test it does not list, or an entry whose file is not a test, stops the run before
anything starts. -j N (or --jobs N) runs N jobs at once, as a build's -j runs N compile jobs; it defaults to half the
cores, up to six. A job keeps one data folder and one app for all its tests, as a person keeps the OrcaSlicer they
opened that morning: the first launch seeds the folder, and the app, launched with --cad and --unrestricted-scripts,
runs the job's tests one after another. Before each test the runner checks that the app answers, and launches it
again on the folder when it does not; when sweep.json marks the test fresh; when the test has a setup step, which
prepares the folder for the launch; and when the test's variables differ from those the app was launched with. A
test that runs past its time limit has the app stopped, so that the next test gets a new one, and a job's app is
stopped when the job has no tests left. A test marked own starts apps of its own on folders of its own and leaves
the job's app as it is; with --headless or --xvfb, they run on a private display the runner starts for the test. The
app's session file ($ORCA_MCP_SESSION) is in the job's folder; each test has a folder of its own under --out for its
temporary folder and empty working folder. The longest tests (by their last passing time in --history) start first,
and tests that hold the same lock never overlap. --shuffle SEED orders them at random from SEED instead and names the
seed on the first line, so that an order that fails can be run again; --shuffle alone picks a seed.

The serial tests, those sweep.json marks exclusive, run after the rest, one at a time with nothing else running, on a
job of their own with an app of its own. A test that fails while others run runs again in that phase, alone, in a
new app on the data folder it failed on, so that a failure that follows from what an earlier test left fails again
and one that follows from the load passes. A test that passes alone counts as passed.

A passing run's folder is deleted as soon as it passes. A failed run's folder is kept with a copy of the app's log
from its job's data folder, also when the test then passes alone; the data folder of a job one of whose tests
failed is kept too, and the other data folders are deleted at the end. --keep keeps everything. results.jsonl
records each run of each test with its job, the tests that ran before it on that job's data folder, and how long the
launch took when the run launched the job's app.

On Linux the runner opens each app on the desktop as an X11 client, where a window manager un-minimizes a window on
request and reports its position, which a Wayland compositor does not; --wayland runs them as native Wayland clients
instead, as a Wayland session runs the app, and --headless runs them on a private compositor instead of on the
desktop, as a machine without one needs. --xvfb runs each job's app on a private Xvfb display of its own, which draws
OpenGL in software, with Openbox as its window manager where Openbox is installed.
Exits 0 when every test passes and no OrcaSlicer crash report appeared.
"""

import argparse
import contextlib
import copy
import glob
import json
import os
import random
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass

MCP_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULTS = {"launch": "app", "setup": [], "args": [], "env": {}, "exclusive": False, "locks": [], "timeout_s": 900,
            "why": ""}
# "app" runs in its job's app and "fresh" in a new one the runner launches before it; "own" starts apps of its own
# and "none" needs no app.
APP_KINDS = ("app", "fresh")
LAUNCH_KINDS = (*APP_KINDS, "own", "none")
FIRST_PORT = 14100  # clear of the fixed ports tests serve on themselves (13692-13694)
SETUP_TIMEOUT_S = 300
LAUNCH_TIMEOUT_S = 300
STOP_TIMEOUT_S = 120
ANSWER_TIMEOUT_S = 20  # how long the job's app has to answer the call that checks it before a test
END_GRACE_S = 30  # how long a test that ran out of time has to close what it started
GONE_TIMEOUT_S = 60  # how long the app's processes have to end after its stop before they are killed
# The longest --out on Windows: a test that starts its own app makes its data folders under its temporary folder,
# <out>/<test>-alone/tmp, and the app copies profile files up to about 105 characters below a data folder, which
# must stay within MAX_PATH (260), or the copy stops and the app opens its Setup Wizard.
MAX_OUT_WINDOWS = 80
SERIAL_JOB = "serial"
NEW_SEED = object()  # --shuffle given without a seed: parse_args() picks one


def entry(raw):
    """A manifest entry with its defaults filled; ValueError on an unknown key, an unknown launch kind, or
    `exclusive` or `locks` without the `why` that keeps a test apart."""
    unknown = sorted(set(raw) - set(DEFAULTS) - {"path"})
    if unknown:
        raise ValueError(f"unknown key {', '.join(unknown)}")
    if "path" not in raw:
        raise ValueError("no path")
    e = {**copy.deepcopy(DEFAULTS), **raw}
    if e["launch"] not in LAUNCH_KINDS:
        raise ValueError(f"launch is {e['launch']!r}, not one of {', '.join(LAUNCH_KINDS)}")
    if (e["exclusive"] or e["locks"]) and not e["why"]:
        raise ValueError("exclusive or locks without why")
    return e


def load_manifest(path):
    with open(path, encoding="utf-8") as f:
        tests = json.load(f)["tests"]
    manifest = {}
    for name, raw in tests.items():
        try:
            manifest[name] = entry(raw)
        except ValueError as e:
            raise ValueError(f"{path}: {name}: {e}") from None
    return manifest


def discover(mcp_dir):
    """Every test in `mcp_dir`, the test_*.py files, by name (its file's stem), with its path relative to `mcp_dir`."""
    return {os.path.splitext(os.path.basename(path))[0]: os.path.relpath(path, mcp_dir)
            for path in sorted(glob.glob(os.path.join(mcp_dir, "test_*.py")))}


def manifest_problems(manifest, discovered, mcp_dir):
    """What keeps the manifest from listing exactly the tests `discovered`: a test it leaves out, an entry whose file
    is missing, and one whose file is not a test, which the runner could not run."""
    tests = {os.path.normcase(os.path.normpath(path)) for path in discovered.values()}
    problems = [f"{name}: not in sweep.json" for name in discovered if name not in manifest]
    for name, e in manifest.items():
        if not os.path.isfile(os.path.join(mcp_dir, e["path"])):
            problems.append(f"{name}: no such file {e['path']}")
        elif os.path.normcase(os.path.normpath(e["path"])) not in tests:
            problems.append(f"{name}: {e['path']} is not a test; the tests are the test_*.py files")
    return problems


def order(names, manifest, times, seed=None):
    """(pool, serial), each longest first by the history's `times`, a test without a time first, as it may be long; or,
    given a `seed`, each in the random order random.Random(seed) gives the names, which is the same order every time
    for the same names and seed. The serial tests are those sweep.json marks exclusive."""
    if seed is None:
        ranked = sorted(names, key=lambda n: (n in times, -times.get(n, 0.0)))
    else:
        ranked = sorted(names)
        random.Random(seed).shuffle(ranked)
    return ([n for n in ranked if not manifest[n]["exclusive"]], [n for n in ranked if manifest[n]["exclusive"]])


def pick_next(pending, manifest, held):
    return next((n for n in pending if not held.intersection(manifest[n]["locks"])), None)


@dataclass
class RunContext:
    mcp_dir: str
    binary: str
    run_dir: str  # the test's own folder
    job_dir: str  # its job's folder, which holds the data folder and the session file of the app the job's tests share
    port: int  # the port the job's app serves on when this run launches it
    python: str
    platform: str
    headless: bool = False
    xvfb: bool = False
    wayland: bool = False

    @property
    def datadir(self):
        return os.path.join(self.job_dir, "datadir")

    @property
    def session(self):
        """The job's app's session file, which its launch writes and the tests that run in it read."""
        return os.path.join(self.job_dir, "session.json")

    @property
    def tmp(self):
        return os.path.join(self.run_dir, "tmp")

    @property
    def app_tmp(self):
        """The temporary folder of the job's app, which outlives the test that launched it and that test's folder."""
        return os.path.join(self.job_dir, "app-tmp")

    @property
    def app_cwd(self):
        """The working folder of the job's app, which outlives the test that launched it and that test's folder: an
        app launched with --datadir keeps the folder it was launched in, and once that folder is deleted every call
        that reads it fails, such as a preset reload. Empty, as `cwd` is."""
        return os.path.join(self.job_dir, "app-cwd")

    @property
    def cwd(self):
        # Empty: a .py file in the folder a script runs from shadows the modules script_run needs.
        return os.path.join(self.run_dir, "cwd")


def _fill(args, ctx):
    return [a.replace("{bin}", ctx.binary).replace("{datadir}", ctx.datadir) for a in args]


def launch_command(ctx):
    """The launch of the job's app. Every test that runs in it has the Design tab and Python scripts without the
    permission checks, whichever test launched it."""
    cmd = [ctx.python, os.path.join(ctx.mcp_dir, "orca_mcp.py"), "launch", "--bin", ctx.binary, "--datadir",
           ctx.datadir, "--port", str(ctx.port), "--cad", "--unrestricted-scripts"]
    if ctx.headless:
        cmd.append("--headless")
    if ctx.xvfb:
        cmd.append("--xvfb")
    return cmd


def stop_command(ctx):
    return [ctx.python, os.path.join(ctx.mcp_dir, "orca_mcp.py"), "stop"]


def setup_command(e, ctx):
    return [ctx.python, os.path.join(ctx.mcp_dir, e["path"]), *_fill(e["setup"], ctx)] if e["setup"] else None


def test_command(e, ctx):
    return [ctx.python, os.path.join(ctx.mcp_dir, e["path"]), *_fill(e["args"], ctx)]


def test_env(e, ctx, base):
    """The environment the test runs with. A test that runs in the job's app finds it through the job's session file;
    any other has a session file of its own in its folder, so that an app it launches never takes the job's."""
    session = ctx.session if e["launch"] in APP_KINDS else os.path.join(ctx.run_dir, "session.json")
    return {**base, "ORCA_MCP_SESSION": session, "TMPDIR": ctx.tmp, "TMP": ctx.tmp, "TEMP": ctx.tmp, **e["env"]}


def launch_env(ctx, env):
    """The environment the launch runs with: the test's, with the display backend the run asked for. An
    app left to pick its own on a Wayland desktop connects with Wayland, so the run's choice overrides
    one an entry sets: an X11 client on the desktop, or a native Wayland one with --wayland. A launch
    with --headless or --xvfb is an X11 client on the display it starts, which its own launch sets."""
    if ctx.platform.startswith("linux") and not (ctx.headless or ctx.xvfb):
        return {**env, "GDK_BACKEND": "wayland" if ctx.wayland else "x11"}
    return env


@contextlib.contextmanager
def own_display(e, ctx):
    """The private display a test that starts its own apps runs them on when the run keeps the apps off the desktop
    (--headless or --xvfb): yields the variables the test's environment takes (None removes one), or None for a
    test that is not marked own or a run on the desktop, and stops the display on leaving. The apps reach it through
    the environment they inherit, whether they start with orca_mcp.py launch or run the binary themselves, so the
    test needs no flag of its own; they share it, as they would share the desktop. A compositor's log is in the
    test's folder."""
    if e["launch"] != "own" or not (ctx.headless or ctx.xvfb):
        yield None
        return
    import orca_mcp
    args = argparse.Namespace(headless="auto" if ctx.headless else None, xvfb=ctx.xvfb, width=orca_mcp.SCREEN_SIZE[0],
                              height=orca_mcp.SCREEN_SIZE[1])
    try:
        _, env, pid = orca_mcp.start_display(args, ctx.run_dir)
    except SystemExit as stopped:  # no Xvfb, or no free display: the run is recorded as an error
        raise RuntimeError(f"the private display for the test's apps did not start: {stopped}") from None
    try:
        yield env
    finally:
        orca_mcp.stop_group(pid)


def run_step(cmd, timeout, **popen):
    """Runs `cmd` to its exit code. One still running after `timeout` s is ended so that what it started ends
    too, and TimeoutExpired raised: on Windows its process tree is ended; elsewhere it is interrupted, as Ctrl-C
    would, so its own cleanup closes what it started, and it is killed if it outlives END_GRACE_S."""
    proc = subprocess.Popen(cmd, **popen)
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
        else:
            proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=END_GRACE_S)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        raise


def app_processes(datadir):
    """The ids of the processes whose command line names `datadir`: an app launched on it, and the WebView2
    processes it started, whose profile orca_mcp.py puts in it."""
    if os.name == "nt":
        script = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and "
                  "$_.CommandLine.IndexOf($env:SWEEP_DATADIR, [StringComparison]::OrdinalIgnoreCase) -ge 0 } | "
                  "ForEach-Object { $_.ProcessId }")
        out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True,
                             text=True, env={**os.environ, "SWEEP_DATADIR": datadir}, timeout=60).stdout
        pids = out.split()
    else:
        out = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True, text=True, timeout=60).stdout
        pids = [line.split(None, 1)[0] for line in out.splitlines() if datadir in line]
    return [int(p) for p in pids if p.isdigit() and int(p) != os.getpid()]


def wait_gone(datadir, log, timeout=GONE_TIMEOUT_S):
    """Waits until no process names `datadir`, so that the next test's app is the only one on the folder; what is
    left after `timeout` s is killed. Notes in `log` what it waited for."""
    started = time.monotonic()
    left = app_processes(datadir)
    while left and time.monotonic() - started < timeout:
        time.sleep(1)
        left = app_processes(datadir)
    if left:
        log.write(f"--- killing what is left on the data folder after {timeout} s: {left}\n")
        for pid in left:
            if os.name == "nt":
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
            else:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
    elif time.monotonic() - started >= 1:
        log.write(f"--- waited {time.monotonic() - started:.1f} s for the app's processes to end\n")


def keep_app_log(ctx, since):
    """Copies into the test's folder the app logs in its job's data folder written since `since` (a time.time()),
    which the next tests on the folder would bury among their own."""
    for path in glob.glob(os.path.join(ctx.datadir, "log", "*")):
        try:
            if os.path.isfile(path) and os.path.getmtime(path) >= since - 1:
                shutil.copy2(path, ctx.run_dir)
        except OSError:
            pass


class JobApp:
    """What the runner knows of a job's app: `env`, the entry's variables it was launched with, or None when the
    runner left no app of its own running on the job's folder. Only the job's worker uses it, one test at a time."""

    def __init__(self):
        self.env = None


def stepper(log, cwd, env):
    """step(label, cmd, timeout, env=None), which notes `cmd` in `log` and runs it to its exit code in `cwd` with
    run_step(), its output in `log`, with `env` unless the step names its own."""
    def step(label, cmd, timeout, step_env=None):
        log.write(f"--- {label}: {shlex.join(cmd)}\n")
        log.flush()
        return run_step(cmd, timeout, cwd=cwd, env=env if step_env is None else step_env, stdin=subprocess.DEVNULL,
                        stdout=log, stderr=subprocess.STDOUT)
    return step


def app_answers(session):
    """Whether the app the session file `session` names answers an MCP call; False when there is no session file."""
    try:
        with open(session, encoding="utf-8") as f:
            s = json.load(f)
        import orca_mcp
        orca_mcp.OrcaMcp(s["port"], s["token"]).call("app_state", _timeout=ANSWER_TIMEOUT_S)
        return True
    except Exception:  # whatever way the call fails (no app, another one on the port, a hang), it did not answer
        return False


def stop_app(app, ctx, step, log):
    """Stops the job's app with `orca_mcp.py stop` when its session file is there, which a launch writes before it
    waits for the app, so a launch that failed is stopped too; then waits until no process names the data folder,
    so that the next app is the only one on it."""
    if os.path.exists(ctx.session):
        try:
            step("stop", stop_command(ctx), STOP_TIMEOUT_S, {**os.environ, "ORCA_MCP_SESSION": ctx.session})
        except subprocess.TimeoutExpired:
            log.write("--- stop timed out\n")
    wait_gone(ctx.datadir, log)
    app.env = None


def run_test(name, e, ctx, app, new_app=False, stop_after=False):
    """Runs one test in `ctx.run_dir` on its job's data folder: its setup, then the test. A test that runs in the
    job's app (kind "app" or "fresh") first gets a working one: a new app when `new_app`, when it is marked fresh,
    when its setup prepares the folder for the launch (the job's app is stopped before the setup), when its
    variables differ from those the job's app was launched with, and when the job's app does not answer; otherwise
    it runs in the app `app` records. Its app is stopped after it when it runs out of time, so that the next test
    gets a new one, and when `stop_after`. A test marked own runs with own_display()'s display, if any. Returns its
    record; `exit` is the test's exit code, or "setup_failed", "launch_failed" or "timeout", and `launch_seconds` how
    long the launch took when the run launched the app. A step that runs out of time is ended as run_step() ends it.
    A test that fails keeps a copy of the app's log."""
    shutil.rmtree(ctx.run_dir, ignore_errors=True)
    for d in (ctx.tmp, ctx.cwd):
        os.makedirs(d)
    os.makedirs(ctx.datadir, exist_ok=True)
    base_env = test_env(e, ctx, os.environ)
    rec = {"name": name, "seconds": 0.0, "launch_seconds": None, "exit": None, "log": os.path.join(ctx.run_dir, "test.log")}
    start, began = time.monotonic(), time.time()
    with open(rec["log"], "w", encoding="utf-8") as log, own_display(e, ctx) as display_env:
        if display_env:
            log.write(f"--- the test's apps run on the private display {display_env['DISPLAY']}\n")
            base_env = {k: v for k, v in {**base_env, **display_env}.items() if v is not None}
        step = stepper(log, ctx.cwd, base_env)
        in_app, failed = e["launch"] in APP_KINDS, "setup_failed"
        setup = setup_command(e, ctx)
        renew = in_app and (new_app or e["launch"] == "fresh" or setup is not None or app.env != e["env"])
        try:
            if renew:
                stop_app(app, ctx, step, log)
            if setup and step("setup", setup, SETUP_TIMEOUT_S) != 0:
                rec["exit"] = failed
                return rec
            if in_app and not renew and not app_answers(ctx.session):
                log.write("--- the job's app does not answer\n")
                stop_app(app, ctx, step, log)
                renew = True
            if renew:
                failed, launched = "launch_failed", time.monotonic()
                os.makedirs(ctx.app_tmp, exist_ok=True)
                os.makedirs(ctx.app_cwd, exist_ok=True)
                tmp = {"TMPDIR": ctx.app_tmp, "TMP": ctx.app_tmp, "TEMP": ctx.app_tmp}
                code = stepper(log, ctx.app_cwd, base_env)("launch", launch_command(ctx), LAUNCH_TIMEOUT_S,
                                                           launch_env(ctx, {**base_env, **tmp}))
                rec["launch_seconds"] = round(time.monotonic() - launched, 1)
                if code != 0:
                    stop_app(app, ctx, step, log)
                    rec["exit"] = failed
                    return rec
                app.env = copy.deepcopy(e["env"])
            failed = "timeout"
            rec["exit"] = step("test", test_command(e, ctx), e["timeout_s"])
        except subprocess.TimeoutExpired as timeout:
            log.write(f"--- timed out after {timeout.timeout} s\n")
            rec["exit"] = failed
            if in_app:
                stop_app(app, ctx, step, log)
        finally:
            if in_app and stop_after and (app.env is not None or os.path.exists(ctx.session)):
                stop_app(app, ctx, step, log)
            if rec["exit"] != 0:
                keep_app_log(ctx, began)
            rec["seconds"] = round(time.monotonic() - start, 1)
    return rec


def guarded_run_test(name, e, ctx, app, new_app=False, stop_after=False):
    """run_test(), with a run that raises recorded as an error, its traceback kept in `error`, rather than lost
    with its worker."""
    try:
        return run_test(name, e, ctx, app, new_app, stop_after)
    except Exception:  # whatever goes wrong running one test, the sweep records it and goes on
        return {"name": name, "seconds": 0.0, "launch_seconds": None, "exit": "error",
                "log": os.path.join(ctx.run_dir, "test.log"), "error": traceback.format_exc()}


def missing(names, results):
    """The tests among `names` that have no record in `results`."""
    done = {r["name"] for r in results}
    return [n for n in names if n not in done]


def crash_reports():
    """The OrcaSlicer crash reports macOS keeps; an empty set elsewhere."""
    if sys.platform != "darwin":
        return set()
    return set(glob.glob(os.path.join(os.path.expanduser("~/Library/Logs/DiagnosticReports"), "OrcaSlicer*")))


class Ports:
    """Hands out ports from `first` up, passing over any something already listens on."""

    def __init__(self, first=FIRST_PORT):
        self.next = first

    def take(self):
        while True:
            port, self.next = self.next, self.next + 1
            with socket.socket() as s:
                try:
                    s.bind(("127.0.0.1", port))
                    return port
                except OSError:
                    continue


def read_history(path):
    """The history file's times: each test's last passing time in seconds, from the file's `times`. The earlier form,
    a flat map of times, reads as its times; a file that is missing or is not JSON reads as none."""
    try:
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
        return {k: float(v) for k, v in raw.get("times", raw).items()}
    except (OSError, ValueError, AttributeError, TypeError):
        return {}


def write_history(path, times):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"times": dict(sorted(times.items()))}, f, indent=1)


def jobs_count(value):
    """-j's value: a whole number of jobs to run at once, from 1 up."""
    try:
        n = int(value)
    except ValueError:
        n = 0
    if n < 1:
        raise argparse.ArgumentTypeError(f"{value!r} is not a whole number from 1 up")
    return n


def option_problems(a, platform):
    """What --headless, --xvfb and --wayland can't do: each names a Linux display, and each picks the one
    the apps run on, so giving two asks for two things at once."""
    if not (a.headless or a.xvfb or a.wayland):
        return []
    if not platform.startswith("linux"):
        return ["--headless, --xvfb and --wayland run only on Linux; elsewhere each app opens on the desktop"]
    if a.headless + a.xvfb + a.wayland > 1:
        return ["--headless, --xvfb and --wayland each pick the display the apps run on, so give only one"]
    return []


def session_problems(a, env, platform):
    """What the display backend the run asked for needs of the session it runs in: a Wayland client a
    WAYLAND_DISPLAY, and an X11 client a DISPLAY, which a run with --headless or --xvfb takes from the
    display each launch starts."""
    if not platform.startswith("linux") or a.headless or a.xvfb:
        return []
    if a.wayland and not env.get("WAYLAND_DISPLAY"):
        return ["--wayland needs a Wayland session, and this one has no WAYLAND_DISPLAY"]
    if not a.wayland and not env.get("DISPLAY"):
        return ["no X display in this session; pass --headless or --xvfb to run each app on a private display"]
    return []


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", help="orca-slicer binary (default: the one orca_mcp.py launch finds)")
    ap.add_argument("-j", "--jobs", type=jobs_count, default=min(6, max(1, (os.cpu_count() or 2) // 2)),
                    metavar="N", help="jobs to run at once, as a build's -j (default: %(default)s, half the cores "
                                      "up to six)")
    ap.add_argument("--only", nargs="+", metavar="NAME", help="run only these tests")
    ap.add_argument("--skip", nargs="+", metavar="NAME", default=[], help="leave these tests out")
    ap.add_argument("--out", help="folder for the jobs' data folders and each test's run (default: a new "
                                  "orca-sweep-<time> one in the temporary folder)")
    ap.add_argument("--history", default=os.path.expanduser(os.path.join("~", ".cache", "orca-mcp", "sweep-times.json")),
                    help="each test's last passing time, which orders the next run (default: %(default)s)")
    ap.add_argument("--keep", action="store_true",
                    help="keep every test's folder and every job's data folder, not only those of failed tests")
    ap.add_argument("--headless", action="store_true",
                    help="run each app on a private compositor instead of on the desktop (Linux)")
    ap.add_argument("--xvfb", action="store_true",
                    help="run each job's app on a private Xvfb display of its own instead of on the desktop: software "
                         "OpenGL, with Openbox as the window manager where it is installed (Linux)")
    ap.add_argument("--wayland", action="store_true",
                    help="run each app as a native Wayland client instead of as an X11 client (Linux)")
    ap.add_argument("--first-port", type=int, default=FIRST_PORT, metavar="PORT",
                    help="the first port an app serves on, for sweeps run side by side (default: %(default)s)")
    ap.add_argument("--shuffle", nargs="?", type=int, const=NEW_SEED, metavar="SEED",
                    help="order the tests at random from SEED instead of longest first, and name the seed on the "
                         "first line, so that the same order can be run again (default SEED: a new one)")
    ap.add_argument("--list", action="store_true", help="print the order the tests would run in and exit")
    a = ap.parse_args(argv)
    if a.shuffle is NEW_SEED:
        a.shuffle = random.randrange(1_000_000)
    return a


class Sweep:
    """One sweep's runs: which job, app and data folder each runs on, what ran before it there, and their records."""

    def __init__(self, a, manifest, mcp_dir, binary, out):
        self.a, self.manifest, self.mcp_dir, self.binary, self.out = a, manifest, mcp_dir, binary, out
        self.ports = Ports(a.first_port)
        self.results = []
        self.runs_on = {}  # job: the names of the runs on its data folder so far, in order
        self.apps = {}  # job: its JobApp
        self.lock = threading.Lock()

    def job_dir(self, job):
        return os.path.join(self.out, f"job-{job}")

    def context(self, job, run_dir, port):
        return RunContext(mcp_dir=self.mcp_dir, binary=self.binary, run_dir=run_dir, job_dir=self.job_dir(job),
                          port=port, python=sys.executable, platform=sys.platform, headless=self.a.headless,
                          xvfb=self.a.xvfb, wayland=self.a.wayland)

    def run(self, name, job, phase):
        """Runs `name` on `job`'s data folder in `phase` ("pool", "serial" or "alone") and returns its record. A run
        alone has a new app, stopped after it. A passing run's folder is deleted unless --keep."""
        run_dir = os.path.join(self.out, name + ("-alone" if phase == "alone" else ""))
        with self.lock:
            port = self.ports.take()
            after = list(self.runs_on.setdefault(job, []))
            self.runs_on[job].append(name)
            app = self.apps.setdefault(job, JobApp())
        alone = phase == "alone"
        rec = {**guarded_run_test(name, self.manifest[name], self.context(job, run_dir, port), app, new_app=alone,
                                  stop_after=alone),
               "job": job, "phase": phase, "after": after}
        if rec["exit"] == 0:
            status = "passed alone" if phase == "alone" else "pass"
        else:
            status = f"FAIL ({rec['exit']})" + (", again alone" if phase == "pool" else "")
        with self.lock:
            self.results.append(rec)
            print(f"{name:45} {status:28} {rec['seconds']:7.1f}s  job {job}", flush=True)
        if rec["exit"] == 0 and not self.a.keep:
            shutil.rmtree(run_dir, ignore_errors=True)
        return rec

    def end_job(self, job):
        """Stops `job`'s app when its session file says one is running, and waits until its processes are gone;
        what that prints goes to stop.log in the job's folder."""
        ctx = self.context(job, self.job_dir(job), 0)
        if not os.path.exists(ctx.session):
            return
        with self.lock:
            app = self.apps.setdefault(job, JobApp())
        with open(os.path.join(ctx.job_dir, "stop.log"), "a", encoding="utf-8") as log:
            stop_app(app, ctx, stepper(log, ctx.job_dir, os.environ), log)


def main(argv=None, mcp_dir=MCP_DIR):
    a = parse_args(argv)

    problems = option_problems(a, sys.platform)
    manifest = load_manifest(os.path.join(mcp_dir, "sweep.json"))
    problems += manifest_problems(manifest, discover(mcp_dir), mcp_dir)
    problems += [f"{n}: no such test" for n in (a.only or []) + a.skip if n not in manifest]
    if not a.list:  # a list launches nothing, so it needs no display
        problems += session_problems(a, os.environ, sys.platform)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 2
    times = read_history(a.history)
    pool, serial = order([n for n in manifest if (not a.only or n in a.only) and n not in a.skip], manifest, times,
                         a.shuffle)
    shuffled = "" if a.shuffle is None else f"shuffled with seed {a.shuffle}"
    if a.list:
        if shuffled:
            print(shuffled)
        for n in pool:
            print(f"{n:45} {times.get(n, 0):7.1f}s")
        for n in serial:
            print(f"{n:45} {times.get(n, 0):7.1f}s  exclusive: {manifest[n]['why']}")
        return 0

    sys.path.insert(0, MCP_DIR)
    import orca_mcp
    # Tests that neither launch an app nor name one run without a build.
    needs_app = any(manifest[n]["launch"] != "none" or "{bin}" in " ".join(manifest[n]["args"] + manifest[n]["setup"])
                    for n in pool + serial)
    binary = orca_mcp.find_binary(a.bin) if needs_app else (a.bin or "")
    out = os.path.abspath(a.out or os.path.join(tempfile.gettempdir(), "orca-sweep-" + time.strftime("%Y%m%d-%H%M%S")))
    if os.name == "nt" and needs_app and len(out) > MAX_OUT_WINDOWS:
        print(f"{out} is {len(out)} characters long; on Windows the data folders under --out must leave room for the "
              f"app's profile files within MAX_PATH, so give an --out of at most {MAX_OUT_WINDOWS}", file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)
    print(f"{len(pool) + len(serial)} tests on {a.jobs} jobs, {len(serial)} of them alone after the rest, into {out}"
          + (f", {shuffled}" if shuffled else ""), flush=True)

    crashes_before = crash_reports()
    sweep = Sweep(a, manifest, mcp_dir, binary, out)
    cond = threading.Condition()
    held, pending, again, stopping = set(), list(pool), [], threading.Event()

    def worker(job):
        try:
            while not stopping.is_set():
                with cond:
                    name = None
                    while pending and not stopping.is_set():
                        name = pick_next(pending, manifest, held)
                        if name:
                            break
                        cond.wait()
                    if not name:
                        return
                    pending.remove(name)
                    held.update(manifest[name]["locks"])
                try:
                    if sweep.run(name, job, "pool")["exit"] != 0:
                        with cond:
                            again.append((name, job))
                finally:
                    with cond:
                        held.difference_update(manifest[name]["locks"])
                        cond.notify_all()
        finally:
            sweep.end_job(job)

    started = time.monotonic()
    threads = [threading.Thread(target=worker, args=(job,)) for job in range(1, a.jobs + 1)]
    try:
        for t in threads:
            t.start()
        for t in threads:
            while t.is_alive():
                t.join(0.5)
        try:
            for name in serial:
                if stopping.is_set():
                    break
                sweep.run(name, SERIAL_JOB, "serial")
        finally:
            sweep.end_job(SERIAL_JOB)
        for name, job in sorted(again, key=lambda pair: pool.index(pair[0])):
            if stopping.is_set():
                break
            sweep.run(name, job, "alone")
    except KeyboardInterrupt:
        # The running tests get the interrupt too; their workers take no more and stop their jobs' apps.
        stopping.set()
        with cond:
            cond.notify_all()
        for t in threads:
            t.join()
        print("interrupted", file=sys.stderr)
    wall = time.monotonic() - started

    results = sweep.results
    with open(os.path.join(out, "results.jsonl"), "w", encoding="utf-8") as f:
        for rec in results:
            f.write(json.dumps(rec) + "\n")
    last = {r["name"]: r for r in results}  # each test's verdict is its last run's
    passed = [n for n, r in last.items() if r["exit"] == 0]
    alone = [n for n in passed if last[n]["phase"] == "alone"]
    failed = [r for r in last.values() if r["exit"] != 0]
    times.update({n: last[n]["seconds"] for n in passed})
    write_history(a.history, times)

    if not a.keep:
        # A test that passed alone keeps the folder of its run beside the others, which says why it failed there.
        kept = {r["job"] for r in results if r["name"] in {f["name"] for f in failed}}
        for job in sweep.runs_on:
            if job not in kept:
                shutil.rmtree(sweep.job_dir(job), ignore_errors=True)

    unrecorded = [] if stopping.is_set() else missing(pool + serial, results)
    crashes = sorted(crash_reports() - crashes_before)
    for r in failed:
        print(f"FAIL {r['name']} ({r['exit']}): {r['log']}")
        if r.get("error"):
            print(r["error"].rstrip())
    for n in alone:
        print(f"{n}: failed beside the others, then passed alone")
    for n in unrecorded:
        print(f"FAIL {n}: never recorded")
    for c in crashes:
        print(f"crash report: {c}")
    print(f"{len(passed)} passed ({len(alone)} alone), {len(failed)} failed; wall {wall / 60:.1f} min, "
          f"tests {sum(r['seconds'] for r in results) / 60:.1f} min")
    return 0 if not failed and not unrecorded and not crashes and not stopping.is_set() else 1


if __name__ == "__main__":
    sys.exit(main())
