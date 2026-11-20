#!/usr/bin/env python3
"""Runs the MCP sweep: every Python test in tools/mcp, on several jobs at once, each job running its tests one after
another on one data folder.

    tools/mcp/run_sweep.py [--bin PATH] [-j N] [--only NAME ...] [--skip NAME ...] [--out DIR] [--history FILE]
                           [--keep] [--headless] [--wayland] [--list] [--forget NAME ...]

The tests are the test_*.py files in tools/mcp; a replay, such as smoke_test.json, is not one. sweep.json lists each
test with what it needs, and a test it does not list, or an entry whose file is not a test, stops the run before
anything starts. -j N (or --jobs N) runs N jobs at once, as a build's -j runs N compile jobs; it defaults to half the
cores, up to six. A job keeps one data folder for all its tests, as a person's OrcaSlicer keeps its own: the first
launch seeds it, and each test after launches a fresh app on the folder as the tests before left it. Each test also
has a folder of its own under --out for its session file ($ORCA_MCP_SESSION), temporary folder and empty working
folder, and a port of its own. The longest tests (by their last passing time in --history) start first, and tests
that hold the same lock never overlap.

The serial tests run after the rest, one at a time with nothing else running, on a job of their own: those
sweep.json marks exclusive, and those the history marks serial. A test that fails while others run runs again in
that phase, alone, on the data folder it failed on, so that a failure that follows from what an earlier test left
fails again and one that follows from the load passes. A test that passes alone counts as passed, and the history
marks it serial, so later sweeps run it alone from the start; --forget NAME clears the mark.

A passing run's folder is deleted as soon as it passes. A failed run's folder is kept with a copy of the app's log
from its job's data folder, also when the test then passes alone; the data folder of a job one of whose tests
failed is kept too, and the other data folders are deleted at the end. --keep keeps everything. results.jsonl records each run of each test with its job and the tests that ran before it
on that job's data folder.

On Linux the runner opens each app on the desktop as an X11 client, where a window manager un-minimizes a window on
request and reports its position, which a Wayland compositor does not; --wayland runs them as native Wayland clients
instead, as a Wayland session runs the app, and --headless runs them on a private compositor instead of on the
desktop, as a machine without one needs.
Exits 0 when every test passes and no OrcaSlicer crash report appeared.
"""

import argparse
import copy
import glob
import json
import os
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
DEFAULTS = {"launch": "app", "cad": False, "setup": [], "args": [], "env": {}, "exclusive": False, "locks": [],
            "timeout_s": 900, "why": ""}
LAUNCH_KINDS = ("app", "own", "none")
FIRST_PORT = 14100  # clear of the fixed ports tests serve on themselves (13692-13694)
SETUP_TIMEOUT_S = 300
LAUNCH_TIMEOUT_S = 300
STOP_TIMEOUT_S = 120
END_GRACE_S = 30  # how long a test that ran out of time has to close what it started
GONE_TIMEOUT_S = 60  # how long the app's processes have to end after its stop before they are killed
# The longest --out on Windows: a test that starts its own app makes its data folders under its temporary folder,
# <out>/<test>-alone/tmp, and the app copies profile files up to about 105 characters below a data folder, which
# must stay within MAX_PATH (260), or the copy stops and the app opens its Setup Wizard.
MAX_OUT_WINDOWS = 80
SERIAL_JOB = "serial"


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


def serial_reason(name, manifest, history):
    """Why `name` runs in the serial phase, or None when it runs among the others."""
    if manifest[name]["exclusive"]:
        return f"exclusive: {manifest[name]['why']}"
    if name in history["serial"]:
        return f"serial since {history['serial'][name]}: it passed only alone"
    return None


def order(names, manifest, history):
    """(pool, serial), each longest first by the history's times; a test without a time comes first, as it may be
    long. The serial tests are those sweep.json marks exclusive and those the history marks serial."""
    times = history["times"]
    ranked = sorted(names, key=lambda n: (n in times, -times.get(n, 0.0)))
    return ([n for n in ranked if not serial_reason(n, manifest, history)],
            [n for n in ranked if serial_reason(n, manifest, history)])


def pick_next(pending, manifest, held):
    return next((n for n in pending if not held.intersection(manifest[n]["locks"])), None)


@dataclass
class RunContext:
    mcp_dir: str
    binary: str
    run_dir: str  # the test's own folder
    job_dir: str  # its job's folder, which holds the data folder the job's tests share
    port: int
    python: str
    platform: str
    headless: bool = False
    wayland: bool = False

    @property
    def datadir(self):
        return os.path.join(self.job_dir, "datadir")

    @property
    def session(self):
        return os.path.join(self.run_dir, "session.json")

    @property
    def tmp(self):
        return os.path.join(self.run_dir, "tmp")

    @property
    def cwd(self):
        # Empty: a .py file in the folder a script runs from shadows the modules script_run needs.
        return os.path.join(self.run_dir, "cwd")


def _fill(args, ctx):
    return [a.replace("{bin}", ctx.binary).replace("{datadir}", ctx.datadir) for a in args]


def launch_command(e, ctx):
    if e["launch"] != "app":
        return None
    cmd = [ctx.python, os.path.join(ctx.mcp_dir, "orca_mcp.py"), "launch", "--bin", ctx.binary, "--datadir",
           ctx.datadir, "--port", str(ctx.port)]
    if e["cad"]:
        cmd.append("--cad")
    if ctx.headless:
        cmd.append("--headless")
    return cmd


def setup_command(e, ctx):
    return [ctx.python, os.path.join(ctx.mcp_dir, e["path"]), *_fill(e["setup"], ctx)] if e["setup"] else None


def test_command(e, ctx):
    return [ctx.python, os.path.join(ctx.mcp_dir, e["path"]), *_fill(e["args"], ctx)]


def test_env(e, ctx, base):
    return {**base, "ORCA_MCP_SESSION": ctx.session, "TMPDIR": ctx.tmp, "TMP": ctx.tmp, "TEMP": ctx.tmp, **e["env"]}


def launch_env(ctx, env):
    """The environment the launch runs with: the test's, with the display backend the run asked for. An
    app left to pick its own on a Wayland desktop connects with Wayland, so the run's choice overrides
    one an entry sets: an X11 client on the desktop, or a native Wayland one with --wayland. A headless
    launch is an X11 client on the compositor it starts, which its own launch sets."""
    if ctx.platform.startswith("linux") and not ctx.headless:
        return {**env, "GDK_BACKEND": "wayland" if ctx.wayland else "x11"}
    return env


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


def run_test(name, e, ctx):
    """Runs one test in `ctx.run_dir` on its job's data folder: its setup, a launch when it needs the app, the test,
    then a stop of what it launched and a wait for the app's processes to end. Returns its record; `exit` is the
    test's exit code, or "setup_failed", "launch_failed" or "timeout". A step that runs out of time is ended as
    run_step() ends it. A test that fails keeps a copy of the app's log."""
    shutil.rmtree(ctx.run_dir, ignore_errors=True)
    for d in (ctx.tmp, ctx.cwd):
        os.makedirs(d)
    os.makedirs(ctx.datadir, exist_ok=True)
    base_env = test_env(e, ctx, os.environ)
    rec = {"name": name, "seconds": 0.0, "launch_seconds": None, "exit": None, "log": os.path.join(ctx.run_dir, "test.log")}
    start, began = time.monotonic(), time.time()
    with open(rec["log"], "w", encoding="utf-8") as log:
        def step(label, cmd, timeout, env=None):
            log.write(f"--- {label}: {shlex.join(cmd)}\n")
            log.flush()
            return run_step(cmd, timeout, cwd=ctx.cwd, env=base_env if env is None else env, stdin=subprocess.DEVNULL,
                            stdout=log, stderr=subprocess.STDOUT)

        launch, failed = launch_command(e, ctx), "setup_failed"
        try:
            setup = setup_command(e, ctx)
            if setup and step("setup", setup, SETUP_TIMEOUT_S) != 0:
                rec["exit"] = failed
                return rec
            if launch:
                failed, launched = "launch_failed", time.monotonic()
                code = step("launch", launch, LAUNCH_TIMEOUT_S, launch_env(ctx, base_env))
                rec["launch_seconds"] = round(time.monotonic() - launched, 1)
                if code != 0:
                    rec["exit"] = failed
                    return rec
            failed = "timeout"
            rec["exit"] = step("test", test_command(e, ctx), e["timeout_s"])
        except subprocess.TimeoutExpired as timeout:
            log.write(f"--- timed out after {timeout.timeout} s\n")
            rec["exit"] = failed
        finally:
            # launch writes the session file before it waits for the app, so a launch that failed is stopped too.
            if launch and os.path.exists(ctx.session):
                try:
                    step("stop", [ctx.python, os.path.join(ctx.mcp_dir, "orca_mcp.py"), "stop"], STOP_TIMEOUT_S)
                except subprocess.TimeoutExpired:
                    log.write("--- stop timed out\n")
            if launch:
                wait_gone(ctx.datadir, log)
            if rec["exit"] != 0:
                keep_app_log(ctx, began)
            rec["seconds"] = round(time.monotonic() - start, 1)
    return rec


def guarded_run_test(name, e, ctx):
    """run_test(), with a run that raises recorded as an error, its traceback kept in `error`, rather than lost
    with its worker."""
    try:
        return run_test(name, e, ctx)
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
    """The history file as {times, serial}: each test's last passing time in seconds, and the date each test the
    sweep runs alone was marked serial. The earlier form, a flat map of times, reads as times with no marks; a file
    that is missing or is not JSON reads as neither."""
    try:
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
        if "times" not in raw and "serial" not in raw:
            raw = {"times": raw}
        return {"times": {k: float(v) for k, v in raw.get("times", {}).items()},
                "serial": {k: str(v) for k, v in raw.get("serial", {}).items()}}
    except (OSError, ValueError, AttributeError, TypeError):
        return {"times": {}, "serial": {}}


def write_history(path, history):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"times": dict(sorted(history["times"].items())), "serial": dict(sorted(history["serial"].items()))},
                  f, indent=1)


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
    """What --headless and --wayland can't do: both name a Linux display, and --headless runs the app as
    an X11 client on the compositor it starts, so giving both asks for two things at once."""
    if not (a.headless or a.wayland):
        return []
    if not platform.startswith("linux"):
        return ["--headless and --wayland run only on Linux; elsewhere each app opens on the desktop"]
    if a.headless and a.wayland:
        return ["--headless runs each app as an X11 client on a private compositor, so it can't be given with --wayland"]
    return []


def session_problems(a, env, platform):
    """What the display backend the run asked for needs of the session it runs in: a Wayland client a
    WAYLAND_DISPLAY, and an X11 client a DISPLAY, which a headless run takes from its own compositor."""
    if not platform.startswith("linux") or a.headless:
        return []
    if a.wayland and not env.get("WAYLAND_DISPLAY"):
        return ["--wayland needs a Wayland session, and this one has no WAYLAND_DISPLAY"]
    if not a.wayland and not env.get("DISPLAY"):
        return ["no X display in this session; pass --headless to run each app on a private compositor"]
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
                    help="each test's last passing time, which orders the next run, and the tests it runs alone "
                         "(default: %(default)s)")
    ap.add_argument("--keep", action="store_true",
                    help="keep every test's folder and every job's data folder, not only those of failed tests")
    ap.add_argument("--forget", nargs="+", metavar="NAME",
                    help="clear these tests' serial marks in the history, so the next sweep runs them among the "
                         "others, and exit")
    ap.add_argument("--headless", action="store_true",
                    help="run each app on a private compositor instead of on the desktop (Linux)")
    ap.add_argument("--wayland", action="store_true",
                    help="run each app as a native Wayland client instead of as an X11 client (Linux)")
    ap.add_argument("--first-port", type=int, default=FIRST_PORT, metavar="PORT",
                    help="the first port an app serves on, for sweeps run side by side (default: %(default)s)")
    ap.add_argument("--list", action="store_true", help="print the order the tests would run in and exit")
    return ap.parse_args(argv)


class Sweep:
    """One sweep's runs: which job and data folder each runs on, what ran before it there, and their records."""

    def __init__(self, a, manifest, mcp_dir, binary, out):
        self.a, self.manifest, self.mcp_dir, self.binary, self.out = a, manifest, mcp_dir, binary, out
        self.ports = Ports(a.first_port)
        self.results = []
        self.runs_on = {}  # job: the names of the runs on its data folder so far, in order
        self.lock = threading.Lock()

    def job_dir(self, job):
        return os.path.join(self.out, f"job-{job}")

    def run(self, name, job, phase):
        """Runs `name` on `job`'s data folder in `phase` ("pool", "serial" or "alone") and returns its record. A
        passing run's folder is deleted unless --keep."""
        run_dir = os.path.join(self.out, name + ("-alone" if phase == "alone" else ""))
        with self.lock:
            port = self.ports.take()
            after = list(self.runs_on.setdefault(job, []))
            self.runs_on[job].append(name)
        ctx = RunContext(mcp_dir=self.mcp_dir, binary=self.binary, run_dir=run_dir, job_dir=self.job_dir(job),
                         port=port, python=sys.executable, platform=sys.platform, headless=self.a.headless,
                         wayland=self.a.wayland)
        rec = {**guarded_run_test(name, self.manifest[name], ctx), "job": job, "phase": phase, "after": after}
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


def main(argv=None, mcp_dir=MCP_DIR):
    a = parse_args(argv)

    problems = option_problems(a, sys.platform)
    manifest = load_manifest(os.path.join(mcp_dir, "sweep.json"))
    problems += manifest_problems(manifest, discover(mcp_dir), mcp_dir)
    problems += [f"{n}: no such test" for n in (a.only or []) + a.skip + (a.forget or []) if n not in manifest]
    if not (a.list or a.forget):  # neither launches anything, so neither needs a display
        problems += session_problems(a, os.environ, sys.platform)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 2
    history = read_history(a.history)
    if a.forget:
        for n in a.forget:
            print(f"{n}: serial mark cleared" if history["serial"].pop(n, None) else f"{n}: had no serial mark")
        write_history(a.history, history)
        return 0
    pool, serial = order([n for n in manifest if (not a.only or n in a.only) and n not in a.skip], manifest, history)
    if a.list:
        for n in pool:
            print(f"{n:45} {history['times'].get(n, 0):7.1f}s")
        for n in serial:
            print(f"{n:45} {history['times'].get(n, 0):7.1f}s  {serial_reason(n, manifest, history)}")
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
    print(f"{len(pool) + len(serial)} tests on {a.jobs} jobs, {len(serial)} of them alone after the rest, into {out}",
          flush=True)

    crashes_before = crash_reports()
    sweep = Sweep(a, manifest, mcp_dir, binary, out)
    cond = threading.Condition()
    held, pending, again, stopping = set(), list(pool), [], threading.Event()

    def worker(job):
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

    started = time.monotonic()
    threads = [threading.Thread(target=worker, args=(job,)) for job in range(1, a.jobs + 1)]
    try:
        for t in threads:
            t.start()
        for t in threads:
            while t.is_alive():
                t.join(0.5)
        for name in serial:
            if stopping.is_set():
                break
            sweep.run(name, SERIAL_JOB, "serial")
        for name, job in sorted(again, key=lambda pair: pool.index(pair[0])):
            if stopping.is_set():
                break
            sweep.run(name, job, "alone")
    except KeyboardInterrupt:
        # The running tests get the interrupt too; their workers stop what they launched and take no more.
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
    history["times"].update({n: last[n]["seconds"] for n in passed})
    history["serial"].update({n: time.strftime("%Y-%m-%d") for n in alone})
    write_history(a.history, history)

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
        print(f"{n}: passed alone, so later sweeps run it alone (--forget {n} to undo)")
    for n in unrecorded:
        print(f"FAIL {n}: never recorded")
    for c in crashes:
        print(f"crash report: {c}")
    print(f"{len(passed)} passed ({len(alone)} alone), {len(failed)} failed; wall {wall / 60:.1f} min, "
          f"tests {sum(r['seconds'] for r in results) / 60:.1f} min")
    return 0 if not failed and not unrecorded and not crashes and not stopping.is_set() else 1


if __name__ == "__main__":
    sys.exit(main())
