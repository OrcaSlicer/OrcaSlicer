"""Checks how OrcaSlicer's automation behaves while the app closes, and while its server is switched off and on
during a call.
Its design is the test_runtime_lifecycle entry of tools/mcp/test-entries/runtime.md.

Each check launches OrcaSlicer on a fresh data folder of its own, with the MCP server on unless it says
otherwise. The app is started here, as orca_mcp.py launch starts it, but as this script's child: only a process's parent
can read its exit code on every system. It is closed as a person closes it, by a click on the File menu's Quit item that
widget_list lists; the click's reply may not arrive, since the server closes with the app. orca_mcp.py stop would not
do: on Windows it kills the process.

- exit_with_job_running: a script_run whose code sleeps in 0.2 s steps for 120 s replies running once its reply budget
  is spent, 4 s with ORCA_MCP_REPLY_BUDGET_MS set for the app, and the app is closed. It passes when the process exits
  with code 0, which an exit the app's 30 s exit watchdog forces is not, and no line of the log under <datadir>/log
  contains "did not return". A process still running 60 s after the click hangs.
- exit_with_recording: a script_run whose code starts a recording through orca.host.tools into a temporary folder and
  then sleeps as above; 3 s later the app is closed. It passes as the first does, and when the recording's file is a
  finished one: the writer puts the header down at once and patches the Segment's size and the Duration only as it
  closes the file (MkvWriter.cpp), so the file is read for a known Segment size that ends at the file's end, a Duration
  above 0 and at least one Cluster.
- exit_without_endpoint: the app is started with the server off and --run on a replay whose one step is
  app_state, expecting mcp.serving false. It passes when the process exits with code 0 within 120 s, the replay's
  result.json says passed (so the endpoint stayed off) and the log has no line containing "did not return".
- server_off_and_on_during_a_call: a script_run whose code opens Preferences, switches the server off with the widget
  named mcp_enabled, keeps app_state's mcp.serving, switches it on, reads it again, closes the dialog and returns
  both values. Closing the endpoint drops every connection, the one waiting for the run's job too, so the code waits
  until the test holds its job before it switches, and the test reconnects and reads the job with app_wait {for: "job"}. It
  passes when the job finished, the values are false then true, and the server answers on the port it had before.
- exit_with_stuck_call: as exit_with_job_running, but the code is one time.sleep(120), a call no stop interrupts, so
  the run is still inside it when the app closes, and its exit holds the runtime's 10 s drain. It passes when the
  process exits with code 0 and the log has a line containing "did not return": the app left the runtime in place
  under the call rather than free it. A missing line means the runtime was freed under the call or the call was
  interrupted, a change to explain.
- instruments_off_with_the_server: a script_run whose code opens Preferences, switches the server off with the widget
  named mcp_enabled and closes the dialog, so the run ends with no endpoint serving and nothing else driving the GUI.
  It passes when the log said "Automation GUI instruments on" while the server served and says "Automation GUI
  instruments off" within 30 s of the script's call ending. No server is left to close the app through, so the
  process is killed.

Prints one line per check and exits 1 when one fails. Checks named on the command line run alone, in
that order. Run it as python tools/mcp/test_runtime_lifecycle.py [--bin PATH] [CHECK ...].
"""
import http.client
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import until  # noqa: E402
from orca_mcp import DEFAULT_PRINTER, McpError, OrcaMcp, app_env, find_binary, seed_datadir, write_conf  # noqa: E402

PORT = 13694
# The checks whose run must outlive its reply budget cap it, so the reply comes in 4 s rather than 45 s.
BUDGET_CAP = {"ORCA_MCP_REPLY_BUDGET_MS": "4000"}
TOKEN = "runtime-lifecycle-test-token"
# From the click on Quit until the process is gone: past the app's 30 s exit watchdog, so that only a hang fails it and
# an exit the watchdog forces is read, and fails by its exit code (it ends the process with EXIT_FAILURE).
EXIT_HANG_S = 60
RUN_EXIT_LIMIT_S = 120  # from the start of a --run, a first start on a new folder, until the process is gone
# What AutomationRuntimeDeleter logs when a call has not returned by the time the runtime goes. Only these words are
# matched: the rest of the line may be worded otherwise.
STUCK = "did not return"
# What a call over a closing or closed endpoint can end in: the server's refusal, a refused or dropped connection, or a
# reply cut off.
DROPPED = (McpError, OSError, http.client.HTTPException)

# Sleeps in 0.2 s steps: a stop interrupts a run at its next bytecode, which one long sleep would put off until it ends.
SLEEPER = """
import time
end = time.monotonic() + 120
while time.monotonic() < end:
    time.sleep(0.2)
"""
# One sleep, which no stop interrupts: the run is still inside its call when the app closes.
STUCK_SLEEPER = """
import time
time.sleep(120)
"""
RECORDER = """
import orca
orca.host.tools.call("recording_start", {"save_path": args["save_path"]})
""" + SLEEPER
# The run waits for the go file, which the test writes once it holds the run's job: the switch closes the connection a
# reply would come back on.
SWITCHER = """
import os
import time
import orca
tools = orca.host.tools
opened = tools.call("window_open", {"window": "preferences", "tab": "general", "key": "mcp_enabled"})
deadline = time.monotonic() + 60
while not os.path.exists(args["go"]):
    if time.monotonic() > deadline:
        raise TimeoutError("the test never wrote the go file")
    time.sleep(0.1)
switch = {"name": "mcp_enabled", "within": "Preferences"}
tools.call("widget_set_value", {"target": switch, "value": False})
off = tools.call("app_state")["mcp"]["serving"]
tools.call("widget_set_value", {"target": switch, "value": True})
on = tools.call("app_state")["mcp"]["serving"]
tools.call("dialog_answer", {"dialog": {"title": "Preferences"}, "button": "close"})
if opened.get("job"):
    tools.call("app_wait", {"for": "job", "job": opened["job"], "timeout_s": 30})
result = [off, on]
"""

# Leaves the server off: the run's reply is lost with the endpoint.
SWITCH_OFF = """
import orca
tools = orca.host.tools
opened = tools.call("window_open", {"window": "preferences", "tab": "general", "key": "mcp_enabled"})
tools.call("widget_set_value", {"target": {"name": "mcp_enabled", "within": "Preferences"}, "value": False})
tools.call("dialog_answer", {"dialog": {"title": "Preferences"}, "button": "close"})
if opened.get("job"):
    tools.call("app_wait", {"for": "job", "job": opened["job"], "timeout_s": 30})
"""
# What AutomationRuntime logs as it installs and removes the GUI instruments.
INSTRUMENTS_ON = "Automation GUI instruments on"
INSTRUMENTS_OFF = "Automation GUI instruments off"

# The ids of the Matroska elements a finished recording is read for.
SEGMENT = bytes.fromhex("18538067")
DURATION = bytes.fromhex("448988")  # the Duration id and its 8-byte payload size
CLUSTER = bytes.fromhex("1F43B675")

results = []


def check(name, problems, evidence):
    results.append(not problems)
    print(("PASS " if not problems else "FAIL ") + name + "  " + ("; ".join(problems) if problems else evidence), flush=True)


def remove(folder):
    """Removes a temporary folder, retrying for up to 30 s while the app's helper processes (WebView2) let go of its
    files; one still there after that is left."""
    def removed():
        shutil.rmtree(folder, ignore_errors=True)
        return not os.path.exists(folder)

    try:
        until(removed, f"{folder} removed")
    except TimeoutError:
        pass


def seed(datadir):
    """orca_mcp.py launch's fresh data folder, with preset sync off and the update check sent to the loopback's discard
    port, where nothing answers."""
    seed_datadir(datadir, DEFAULT_PRINTER)
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    with open(conf, encoding="utf-8") as f:
        data = json.load(f)
    data["app"].update({"sync_user_preset": False, "version_check_url": "http://127.0.0.1:9/"})
    write_conf(conf, data)


class Instance:
    """A private OrcaSlicer on a fresh data folder, started as this script's child, its working folder that data folder,
    which holds no .py file to shadow a module script_run needs. The folder's name is short, so that the first start's
    profile copy stays within Windows' 260-character paths under a deep temporary folder. On leaving, an app still
    running is closed as a person closes it when the server on PORT is its own, then killed if it outlives its exit
    watchdog, and the folder is removed. `env` adds variables to the app's environment."""

    def __init__(self, binary, *args, serve=True, env=None):
        self.datadir = tempfile.mkdtemp(prefix="olc-")
        seed(self.datadir)
        child_env = app_env(dict(os.environ, ORCA_MCP_TOKEN=TOKEN, **(env or {})), self.datadir)
        cmd = [binary, "--datadir", self.datadir, *(["--mcp", "--mcp-port", str(PORT)] if serve else []), *args]
        with open(os.path.join(self.datadir, "mcp-launch.log"), "w") as out:
            self.proc = subprocess.Popen(cmd, env=child_env, cwd=self.datadir, stdout=out, stderr=subprocess.STDOUT)
        self.orca = OrcaMcp(PORT, TOKEN)
        self.serves_here = False
        try:
            self.not_ready = self._wait_ready(120) if serve else ""
        except BaseException:
            self.__exit__()
            raise

    def _wait_ready(self, timeout):
        """Why this instance's server is not ready on PORT, or "" once it is. The discovery file the server writes into
        the fresh data folder tells this app from another one that may hold the port."""
        def ready_or_exited():
            if self.proc.poll() is not None:
                return "exited"
            try:
                return self.orca.call("app_wait", _timeout=15, **{"for": "ready", "timeout_s": 5}).get("satisfied")
            except DROPPED:
                return False

        try:
            if until(ready_or_exited, f"a server ready on port {PORT}", timeout) == "exited":
                return f"the app exited with code {self.proc.returncode} before it was ready"
        except TimeoutError:
            return f"the app was not ready in {timeout} s"
        try:
            with open(os.path.join(self.datadir, "mcp", "server.json"), encoding="utf-8") as f:
                info = json.load(f)
        except (OSError, ValueError) as e:
            return f"a server is ready on port {PORT}, but this app wrote no discovery file ({e})"
        if info.get("pid") != self.proc.pid or info.get("port") != PORT:
            return f"a server is ready on port {PORT}, but this app's discovery file says {info}"
        self.serves_here = True
        return ""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.proc.poll() is None:
            # Never another app's: one that holds the port is not this test's to close.
            if self.serves_here:
                quit_app(self.orca)
            try:
                self.proc.wait(timeout=45)  # past the app's 30 s exit watchdog
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                print(f"NOTE the app on {self.datadir} did not close and was killed", flush=True)
        remove(self.datadir)


def quit_app(orca):
    """Clicks the File menu's Quit item, as a person closes the app, and says what came of the click: its reply, or why
    none came. The reply may not arrive, since the server closes with the app."""
    try:
        _, texts = orca.call_with_text("widget_list", _timeout=30, query="Quit")
    except DROPPED as e:
        return f"widget_list failed: {e}"
    found = re.search(r'\[(\w+)\] menu_item "Quit"', "".join(texts))
    if not found:
        return "widget_list lists no Quit menu item"
    try:
        reply = orca.call_raw("widget_click", {"target": found.group(1)}, timeout=30)
    except DROPPED as e:
        return f"the click's reply was lost ({type(e).__name__})"
    return "the click replied " + json.dumps(reply)[:300]


def log_lines_containing(datadir, text):
    """The lines of the app's log files under <datadir>/log that contain `text`, or None when there is no log."""
    folder = os.path.join(datadir, "log")
    try:
        paths = [p for p in (os.path.join(folder, n) for n in os.listdir(folder)) if os.path.isfile(p)]
    except OSError:
        return None
    if not paths:
        return None
    lines = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines += [line.strip() for line in f if text in line]
    return lines


def log_problems(datadir):
    stuck = log_lines_containing(datadir, STUCK)
    if stuck is None:
        return [f"no log under {os.path.join(datadir, 'log')} to read"]
    return ["the log says: " + line[:300] for line in stuck]


def close_as_a_person(app):
    """Closes the app with File > Quit and returns what is wrong with how the process ended, and the seconds it took:
    still running EXIT_HANG_S s after the click, or an exit code other than 0, which an exit the app's exit watchdog
    forces is not."""
    start = time.monotonic()
    clicked = quit_app(app.orca)
    try:
        code = app.proc.wait(timeout=max(0.0, EXIT_HANG_S - (time.monotonic() - start)))
    except subprocess.TimeoutExpired:
        return [f"still running {EXIT_HANG_S} s after Quit ({clicked})"], time.monotonic() - start
    took = time.monotonic() - start
    return ([] if code == 0 else [f"exit code {code} after {took:.1f} s ({clicked})"]), took


def call(orca, tool, **args):
    """A tool's reply, or {"error": ...} when the call failed."""
    try:
        return orca.call(tool, **args)
    except DROPPED as e:
        return {"error": f"{type(e).__name__}: {e}"}


def ebml_size(data, at):
    """The value of the EBML size field at data[at] and its length in bytes, (None, 0) when it is malformed."""
    if at >= len(data):
        return None, 0
    length = 1
    while length <= 8 and not data[at] & (0x80 >> (length - 1)):
        length += 1
    if length > 8 or at + length > len(data):
        return None, 0
    value = data[at] & (0xFF >> length)
    for b in data[at + 1:at + length]:
        value = value << 8 | b
    return value, length


def unfinished_recording(path):
    """Why the Matroska file at `path` is not a finished recording, or "" when it is."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        return f"cannot read it ({e})"
    at = data.find(SEGMENT)
    if at < 0:
        return "it has no Segment"
    size, length = ebml_size(data, at + 4)
    if size is None:
        return "its Segment's size is malformed"
    if size == (1 << (7 * length)) - 1:  # all value bits set: 01 FF FF FF FF FF FF FF in the writer's 8 bytes
        return "its Segment's size is still unknown"
    if at + 4 + length + size != len(data):
        return f"its Segment ends at byte {at + 4 + length + size}, the file at {len(data)}"
    d = data.find(DURATION, at)
    if d < 0 or d + 11 > len(data):
        return "it has no Duration"
    duration = struct.unpack(">d", data[d + 3:d + 11])[0]
    if not duration > 0:
        return f"its Duration is {duration}"
    if data.find(CLUSTER, at) < 0:
        return "it has no Cluster"
    return ""


def exit_with_job_running(binary):
    name = "exit_with_job_running"
    with Instance(binary, env=BUDGET_CAP) as app:
        if app.not_ready:
            return check(name, [app.not_ready], "")
        reply = call(app.orca, "script_run", code=SLEEPER)
        if reply.get("status") != "running":
            return check(name, ["script_run did not reply running: " + json.dumps(reply)[:300]], "")
        problems, took = close_as_a_person(app)
        check(name, problems + log_problems(app.datadir),
              f"script_run replied running; exit code 0 {took:.1f} s after Quit; no \"{STUCK}\" in the log")


def exit_with_recording(binary):
    name = "exit_with_recording"
    folder = tempfile.mkdtemp(prefix="orca-lifecycle-recording-")
    video = os.path.join(folder, "session.mkv")
    try:
        with Instance(binary) as app:
            if app.not_ready:
                return check(name, [app.not_ready], "")
            # No reply within 3 s means the script runs on, as it should; the call goes on in the app.
            try:
                early = app.orca.call("script_run", _timeout=3, code=RECORDER, args={"save_path": video})
            except McpError as e:
                early = {"error": str(e)}
            except (OSError, http.client.HTTPException):
                early = None
            if early is not None:
                return check(name, ["the script ended within 3 s: " + json.dumps(early)[:300]], "")
            try:
                until(lambda: os.path.exists(video), "the recording's file", interval_s=0.2)
            except TimeoutError:
                return check(name, ["no recording was started"], "")
            problems, took = close_as_a_person(app)
            problems += log_problems(app.datadir)
            why = unfinished_recording(video)
            if why:
                problems.append("the recording is not finished: " + why)
            check(name, problems, f"exit code 0 {took:.1f} s after Quit; no \"{STUCK}\" in the log; the recording "
                                  f"({os.path.getsize(video)} bytes) is finished")
    finally:
        remove(folder)


def exit_without_endpoint(binary):
    name = "exit_without_endpoint"
    folder = tempfile.mkdtemp(prefix="orca-lifecycle-replay-")
    replay, out = os.path.join(folder, "state.json"), os.path.join(folder, "out")
    os.makedirs(out)
    with open(replay, "w", encoding="utf-8") as f:
        # The expect makes a run that opened the endpoint fail the replay, and with it the check.
        json.dump({"orca_replay_version": "0.2.0", "name": "the app's state without the server",
                   "steps": [{"tool": "app_state", "expect": {"/mcp/serving": {"eq": False}}}]}, f)
    try:
        with Instance(binary, "--run", replay, "--outputdir", out, serve=False) as app:
            start = time.monotonic()
            try:
                code = app.proc.wait(timeout=RUN_EXIT_LIMIT_S)
            except subprocess.TimeoutExpired:
                return check(name, [f"still running {RUN_EXIT_LIMIT_S} s after its start"], "")
            took = time.monotonic() - start
            problems = [] if code == 0 else [f"exit code {code}"]
            try:
                with open(os.path.join(out, "result.json"), encoding="utf-8") as f:
                    passed = json.load(f).get("passed")
            except (OSError, ValueError) as e:
                passed = f"unreadable ({e})"
            if passed is not True:
                problems.append(f"result.json's passed is {passed}")
            check(name, problems + log_problems(app.datadir),
                  f"exit code 0 {took:.1f} s after its start; result.json passed; no \"{STUCK}\" in the log")
    finally:
        remove(folder)


def server_off_and_on_during_a_call(binary):
    name = "server_off_and_on_during_a_call"
    with Instance(binary) as app:
        if app.not_ready:
            return check(name, [app.not_ready], "")
        before = call(app.orca, "app_state").get("mcp", {}).get("port")
        go = os.path.join(app.datadir, "go")
        # The call replies blocked_by_dialog once the Preferences dialog the run opened is up.
        reply = call(app.orca, "script_run", code=SWITCHER, args={"go": go})
        job = reply.get("job")
        if reply.get("status") not in ("running", "blocked_by_dialog") or not job:
            return check(name, ["script_run did not reply with a running job: " + json.dumps(reply)[:300]], "")
        open(go, "w").close()
        drops = []

        def ended():
            """The job's reply once it ended, or None while it runs or the server is away."""
            try:
                r = app.orca.call("app_wait", **{"for": "job", "job": job, "timeout_s": 10})
            except DROPPED as e:
                drops.append(e)
                return None
            return r if r.get("status") != "running" or r.get("job") != job else None

        try:
            done = until(ended, f"job {job} ended", 90)
        except TimeoutError:
            return check(name, [f"the job never finished, or the server never came back on port {PORT}"], "")
        after = call(app.orca, "app_state").get("mcp", {}).get("port")
        problems = []
        if done.get("status") != "finished":
            problems.append("the job did not finish: " + json.dumps(done)[:300])
        elif done.get("result") != [False, True]:
            problems.append(f"mcp.serving read {done.get('result')}, not [false, true]")
        if before != PORT or after != before:
            problems.append(f"the port was {before} before and {after} after")
        check(name, problems, f"job {job} finished; mcp.serving false then true; port {before} before and after; "
                              f"connections dropped while waiting: {len(drops)}")


def exit_with_stuck_call(binary):
    name = "exit_with_stuck_call"
    with Instance(binary, env=BUDGET_CAP) as app:
        if app.not_ready:
            return check(name, [app.not_ready], "")
        reply = call(app.orca, "script_run", code=STUCK_SLEEPER)
        if reply.get("status") != "running":
            return check(name, ["script_run did not reply running: " + json.dumps(reply)[:300]], "")
        problems, took = close_as_a_person(app)
        stuck = log_lines_containing(app.datadir, STUCK)
        if stuck is None:
            problems.append(f"no log under {os.path.join(app.datadir, 'log')} to read")
        elif not stuck:
            problems.append(f"no \"{STUCK}\" line in the log: the runtime was freed under the call, or the call was "
                            f"interrupted")
        line = " ".join(stuck[0].split())[:200] if stuck else ""
        check(name, problems, f"script_run replied running; exit code 0 {took:.1f} s after Quit; the log says: {line}")


def instruments_off_with_the_server(binary):
    name = "instruments_off_with_the_server"
    with Instance(binary) as app:
        if app.not_ready:
            return check(name, [app.not_ready], "")
        problems = []
        if not log_lines_containing(app.datadir, INSTRUMENTS_ON):
            problems.append(f"no \"{INSTRUMENTS_ON}\" line in the log while the server serves")
        if log_lines_containing(app.datadir, INSTRUMENTS_OFF):
            problems.append(f"the log says \"{INSTRUMENTS_OFF}\" while the server serves")
        try:
            app.orca.call("script_run", _timeout=30, code=SWITCH_OFF)
        except DROPPED:
            pass
        start = time.monotonic()
        try:
            until(lambda: log_lines_containing(app.datadir, INSTRUMENTS_OFF), f"\"{INSTRUMENTS_OFF}\" in the log")
        except TimeoutError:
            closed = log_lines_containing(app.datadir, "MCP server endpoint closed")
            problems.append(f"no \"{INSTRUMENTS_OFF}\" line in the log 30 s after the script's call ended "
                            f"(the endpoint {'closed' if closed else 'never closed'})")
        took = time.monotonic() - start
        app.proc.kill()
        app.proc.wait()
        check(name, problems, f"the log says \"{INSTRUMENTS_OFF}\" {took:.1f} s after the script's call ended")


CHECKS = {f.__name__: f for f in (exit_with_job_running, exit_with_recording, exit_without_endpoint,
                                  server_off_and_on_during_a_call, exit_with_stuck_call, instruments_off_with_the_server)}


def main():
    argv = sys.argv[1:]
    explicit = None
    if "--bin" in argv:
        i = argv.index("--bin")
        explicit = argv[i + 1] if i + 1 < len(argv) else None
        del argv[i:i + 2]
    unknown = [a for a in argv if a not in CHECKS]
    if unknown:
        print(f"unknown check {', '.join(unknown)}; the {len(CHECKS)} checks are {', '.join(CHECKS)}\n"
              f"usage: python tools/mcp/test_runtime_lifecycle.py [--bin PATH] [CHECK ...]")
        return 2
    binary = find_binary(explicit)
    print(f"binary: {binary}", flush=True)
    for name in argv or CHECKS:
        CHECKS[name](binary)
    failed = results.count(False)
    print(f"\n{len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
