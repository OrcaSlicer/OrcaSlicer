#!/usr/bin/env python3
"""Runs the MCP sweep: every replay and Python test under tools/mcp, each on a freshly launched OrcaSlicer, several
at a time.

    tools/mcp/run_sweep.py [--bin PATH] [-j N] [--only NAME ...] [--skip NAME ...] [--out DIR] [--history FILE]
                           [--headless] [--wayland] [--list]

The tests are smoke_test.json, tests/*.json and test_*.py; sweep.json lists each with what it needs, and a test it
does not list stops the run before anything starts. -j N (or --jobs N) runs N tests at once, as a build's -j runs N
compile jobs; it defaults to half the cores, up to six. Each test runs in a folder of its own under --out, with its
own datadir, port, session file ($ORCA_MCP_SESSION), temporary folder and empty working folder, so instances running
side by side share nothing. The longest tests (by their last passing time in --history) start first. Tests that hold
the same lock never overlap, and exclusive tests run after the rest, one at a time with nothing else running.
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
    """Every test under `mcp_dir`, by name (its file's stem), with its path relative to `mcp_dir`."""
    found = {"smoke_test": "smoke_test.json"}
    for pattern in (os.path.join("tests", "*.json"), "test_*.py"):
        for path in sorted(glob.glob(os.path.join(mcp_dir, pattern))):
            name, rel = os.path.splitext(os.path.basename(path))[0], os.path.relpath(path, mcp_dir)
            # A test is known by its name alone, so a second file of one name would never run.
            if name in found:
                raise ValueError(f"{found[name]} and {rel} are both the test {name}")
            found[name] = rel
    return found


def manifest_problems(manifest, discovered, mcp_dir):
    problems = [f"{name}: not in sweep.json" for name in discovered if name not in manifest]
    problems += [f"{name}: no such file {e['path']}" for name, e in manifest.items()
                 if not os.path.isfile(os.path.join(mcp_dir, e["path"]))]
    return problems


def order(names, manifest, history):
    """(parallel, exclusive), each longest first by `history`; a test without a time comes first, as it may be long."""
    ranked = sorted(names, key=lambda n: (n in history, -history.get(n, 0.0)))
    return ([n for n in ranked if not manifest[n]["exclusive"]], [n for n in ranked if manifest[n]["exclusive"]])


def pick_next(pending, manifest, held):
    return next((n for n in pending if not held.intersection(manifest[n]["locks"])), None)


@dataclass
class RunContext:
    mcp_dir: str
    binary: str
    run_dir: str
    port: int
    python: str
    platform: str
    headless: bool = False
    wayland: bool = False

    @property
    def datadir(self):
        return os.path.join(self.run_dir, "datadir")

    @property
    def out_dir(self):
        return os.path.join(self.run_dir, "out")

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
    path = os.path.join(ctx.mcp_dir, e["path"])
    if path.endswith(".json"):
        return [ctx.python, os.path.join(ctx.mcp_dir, "orca_mcp.py"), "run-replay", path, "--out", ctx.out_dir]
    return [ctx.python, path, *_fill(e["args"], ctx)]


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


def run_test(name, e, ctx):
    """Runs one test in `ctx.run_dir`: its setup, a launch when it needs the app, the test, then a stop of what it
    launched. Returns its record; `exit` is the test's exit code, or "setup_failed", "launch_failed" or "timeout".
    A step that runs out of time is ended as run_step() ends it."""
    shutil.rmtree(ctx.run_dir, ignore_errors=True)
    for d in (ctx.datadir, ctx.out_dir, ctx.tmp, ctx.cwd):
        os.makedirs(d)
    base_env = test_env(e, ctx, os.environ)
    rec = {"name": name, "seconds": 0.0, "launch_seconds": None, "exit": None, "log": os.path.join(ctx.run_dir, "test.log")}
    start = time.monotonic()
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
    """Hands out ports from FIRST_PORT up, passing over any something already listens on."""

    def __init__(self):
        self.next = FIRST_PORT

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
    try:
        with open(path, encoding="utf-8") as f:
            return {k: float(v) for k, v in json.load(f).items()}
    except (OSError, ValueError, AttributeError):
        return {}


def write_history(path, history):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(history.items())), f, indent=1)


def jobs_count(value):
    """-j's value: a whole number of tests to run at once, from 1 up."""
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
                    metavar="N", help="tests to run at once, as a build's -j (default: %(default)s, half the cores "
                                      "up to six)")
    ap.add_argument("--only", nargs="+", metavar="NAME", help="run only these tests")
    ap.add_argument("--skip", nargs="+", metavar="NAME", default=[], help="leave these tests out")
    ap.add_argument("--out", help="folder for each test's run (default: a new orca-sweep-<time> one in the temporary folder)")
    ap.add_argument("--history", default=os.path.expanduser(os.path.join("~", ".cache", "orca-mcp", "sweep-times.json")),
                    help="each test's last passing time, which orders the next run (default: %(default)s)")
    ap.add_argument("--headless", action="store_true",
                    help="run each app on a private compositor instead of on the desktop (Linux)")
    ap.add_argument("--wayland", action="store_true",
                    help="run each app as a native Wayland client instead of as an X11 client (Linux)")
    ap.add_argument("--list", action="store_true", help="print the order the tests would run in and exit")
    return ap.parse_args(argv)


def main(argv=None):
    a = parse_args(argv)

    problems = option_problems(a, sys.platform)
    manifest = load_manifest(os.path.join(MCP_DIR, "sweep.json"))
    try:
        problems += manifest_problems(manifest, discover(MCP_DIR), MCP_DIR)
    except ValueError as e:
        problems.append(str(e))
    problems += [f"{n}: no such test" for n in (a.only or []) + a.skip if n not in manifest]
    if not a.list:  # a listing launches nothing, so it needs no display
        problems += session_problems(a, os.environ, sys.platform)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 2
    history = read_history(a.history)
    parallel, exclusive = order([n for n in manifest if (not a.only or n in a.only) and n not in a.skip],
                                manifest, history)
    if a.list:
        for n in parallel:
            print(f"{n:45} {history.get(n, 0):7.1f}s")
        for n in exclusive:
            print(f"{n:45} {history.get(n, 0):7.1f}s  exclusive: {manifest[n]['why']}")
        return 0

    sys.path.insert(0, MCP_DIR)
    import orca_mcp
    binary = orca_mcp.find_binary(a.bin)
    out = os.path.abspath(a.out or os.path.join(tempfile.gettempdir(), "orca-sweep-" + time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    print(f"{len(parallel) + len(exclusive)} tests, {a.jobs} at a time, into {out}", flush=True)

    crashes_before = crash_reports()
    cond = threading.Condition()
    ports, results, held, pending, stopping = Ports(), [], set(), list(parallel), threading.Event()

    def run(name, port):
        ctx = RunContext(mcp_dir=MCP_DIR, binary=binary, run_dir=os.path.join(out, name), port=port,
                         python=sys.executable, platform=sys.platform, headless=a.headless, wayland=a.wayland)
        rec = guarded_run_test(name, manifest[name], ctx)
        with cond:
            results.append(rec)
            status = "pass" if rec["exit"] == 0 else f"FAIL ({rec['exit']})"
            print(f"{name:45} {status:20} {rec['seconds']:7.1f}s", flush=True)

    def worker():
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
                port = ports.take()
            try:
                run(name, port)
            finally:
                with cond:
                    held.difference_update(manifest[name]["locks"])
                    cond.notify_all()

    started = time.monotonic()
    threads = [threading.Thread(target=worker) for _ in range(a.jobs)]
    try:
        for t in threads:
            t.start()
        for t in threads:
            while t.is_alive():
                t.join(0.5)
        for name in exclusive:
            if stopping.is_set():
                break
            run(name, ports.take())
    except KeyboardInterrupt:
        # The running tests get the interrupt too; their workers stop what they launched and take no more.
        stopping.set()
        with cond:
            cond.notify_all()
        for t in threads:
            t.join()
        print("interrupted", file=sys.stderr)
    wall = time.monotonic() - started

    with open(os.path.join(out, "results.jsonl"), "w", encoding="utf-8") as f:
        for rec in results:
            f.write(json.dumps(rec) + "\n")
    history.update({r["name"]: r["seconds"] for r in results if r["exit"] == 0})
    write_history(a.history, history)

    failed = [r for r in results if r["exit"] != 0]
    unrecorded = [] if stopping.is_set() else missing(parallel + exclusive, results)
    crashes = sorted(crash_reports() - crashes_before)
    for r in failed:
        print(f"FAIL {r['name']} ({r['exit']}): {r['log']}")
        if r.get("error"):
            print(r["error"].rstrip())
    for n in unrecorded:
        print(f"FAIL {n}: never recorded")
    for c in crashes:
        print(f"crash report: {c}")
    print(f"{len(results) - len(failed)} passed, {len(failed)} failed; wall {wall / 60:.1f} min, "
          f"tests {sum(r['seconds'] for r in results) / 60:.1f} min")
    return 0 if not failed and not unrecorded and not crashes and not stopping.is_set() else 1


if __name__ == "__main__":
    sys.exit(main())
