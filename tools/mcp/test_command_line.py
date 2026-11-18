"""Checks OrcaSlicer's command line: the options it refuses, what --run does with its file and its folders, where --run
is refused, what --mcp-port opens, the two flags that hold for one run, a run beside a window already open, and a
window on another data folder.

Each check starts the built app as this script's child, on a fresh data folder with a short path under the temporary
folder, seeded as orca_mcp.py launch seeds one, with preset sync off and the update check sent to the loopback's
discard port. Only a process's parent can read its exit code on every system. The replays and scripts a check runs are
written by the check into a temporary folder of its own. An app that serves is closed as a person closes it, by a click
on the File menu's Quit item that widget_list lists; an app still running when its check is done is killed with its
child processes.

- refused_options: --mcp-run r.json, --mcp-run-out d, --mcp-token t and --no-mcp each exit non-zero within 10 s with
  "Invalid option" on stderr, and the data folder holds only the seeded OrcaSlicer.conf, which a GUI that started
  would have added its log and cache folders to.
- run_invalid_file: --run missing.json exits 2, and so does --run a.txt, whose text is a valid replay.
- run_replay_folders: --run r.json, a replay whose one step is app_state, exits 0 with result.json in r.out beside
  it; with --outputdir D, a folder that does not exist yet, it exits 0, D holds result.json, and r.out is not created.
- run_script_output_folder: --run s.py, whose code takes a screenshot with no path, exits 0 with one .png in D when
  given --outputdir D, and without it in <datadir>/automation/out, while <datadir>/mcp/out does not exist.
- run_refused_without_gui: --run s.py beside --export-3mf out.3mf model.stl, and beside --downward-check, each exit
  with CLI_INVALID_PARAMS within 10 s, stderr says "--run cannot be combined with" the other option, and the data
  folder holds only OrcaSlicer.conf and the refusal's result.json.
- mcp_port_opens_endpoint: --mcp-port P alone, with mcp_enabled false in the conf, serves on P; --mcp-port 0 alone,
  with mcp_port 13694 in the conf, serves on 13694. A server counts as this app's when its discovery file names the
  app's pid.
- unrestricted_scripts_flag: --unrestricted-scripts --run r.py, where r.py reads a file the test wrote in a temporary
  folder that is neither the data folder, the resources folder nor the script's own, exits 0, and the conf's
  python_scripts_unrestricted is absent or false after exit; without the flag the same run exits 1.
- cad_flag: --cad --mcp on a fresh data folder: tool_search {group: Design} finds commands and app_state has no
  cad_note; after exit the conf's enable_cad_feature is absent or false. A second start without the flag on the same
  folder finds no Design command.
- run_beside_open_window: with single_instance true in the conf and a first window serving on the same data folder,
  --run r.json runs in a second process, exits 0 and writes its result.json, and the first window has loaded no
  object; then, with that window closed, two --run r.json processes started together on that folder both exit 0.
- window_per_data_folder: with single_instance true in both confs and a first window serving on one data folder, a
  start on that folder with a model exits -1, having handed its command line over, and the first window loads the
  model (on Linux, which hands it over on the session bus a headless run may lack, only the exit code is checked);
  on Windows so does a start on the folder spelled in upper case, on its 8.3 short name where the volume keeps one,
  from the exe's path in upper case, and from the exe's own folder by a relative path, the first window then holding
  one object per hand-over; a window started on a second data folder serves beside the first.

Prints one line per check and exits 1 when one fails. Checks named on the command line run alone, in that order. Run it
as python tools/mcp/test_command_line.py [--bin PATH] [CHECK ...].
"""
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from orca_mcp import DEFAULT_PRINTER, McpError, OrcaMcp, app_env, find_binary, seed_datadir, write_conf  # noqa: E402

PORT = 13694        # the port the checks that serve listen on, and the conf's mcp_port where one is seeded
GIVEN_PORT = 13693  # the port --mcp-port gives in mcp_port_opens_endpoint and to window_per_data_folder's second window
TOKEN = "command-line-test-token"
REFUSE_LIMIT_S = 10    # a refused command line ends before any GUI starts
RUN_LIMIT_S = 120      # from the start of a --run until the process is gone, a first start's profile copy included
READY_LIMIT_S = 120
EXIT_LIMIT_S = 45      # from the click on Quit, past the app's 30 s exit watchdog
INVALID_PARAMS = (-2, 254)  # CLI_INVALID_PARAMS as Windows and POSIX report it
HANDED_OVER = (-1, 255)     # a start that handed its command line to a window, as Windows and POSIX report it
MODEL = os.path.join(HERE, "..", "..", "resources", "handy_models", "OrcaCube_v2.drc")
DROPPED = (McpError, OSError, http.client.HTTPException)

REPLAY = {"orca_replay_version": "0.2.0", "name": "the app's state", "steps": [{"tool": "app_state"}]}
SCREENSHOT_SCRIPT = """
import orca
orca.host.tools.call("window_screenshot", {"include_image": False})
"""

results = []


def check(name, problems, evidence):
    results.append(not problems)
    print(("PASS " if not problems else "FAIL ") + name + "  " + ("; ".join(problems) if problems else evidence), flush=True)


def remove(folder):
    """Removes a temporary folder, retrying while the app's helper processes (WebView2) let go of its files."""
    for _ in range(20):
        shutil.rmtree(folder, ignore_errors=True)
        if not os.path.exists(folder):
            return
        time.sleep(0.5)


def exit_code(code):
    """The process's exit code as a signed number: Windows reports a negative one as its unsigned 32-bit value."""
    return code - (1 << 32) if code is not None and code >= (1 << 31) else code


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def read_conf(datadir):
    """The app section of the data folder's OrcaSlicer.conf, which ends with a checksum line after the JSON."""
    with open(os.path.join(datadir, "OrcaSlicer.conf"), encoding="utf-8") as f:
        text = f.read()
    return json.loads(text[:text.rfind("}") + 1]).get("app", {})


def is_off(value):
    return value in (None, False, "false", "0")


class Work:
    """A check's temporary folder, short enough for the first start's profile copy: the data folder is its `data`
    subfolder, launched from with no .py file in it, and the check's files go in `files`."""

    def __init__(self, **app):
        self.root = tempfile.mkdtemp(prefix="ocl-")
        self.datadir = os.path.join(self.root, "data")
        self.files = os.path.join(self.root, "files")
        os.makedirs(self.files)
        seed_datadir(self.datadir, DEFAULT_PRINTER)
        conf = os.path.join(self.datadir, "OrcaSlicer.conf")
        with open(conf, encoding="utf-8") as f:
            data = json.load(f)
        data["app"].update({"sync_user_preset": False, "version_check_url": "http://127.0.0.1:9/", **app})
        write_conf(conf, data)
        self.apps = []

    def file(self, name, text=None):
        path = os.path.join(self.files, name)
        return write(path, text) if text is not None else path

    def start(self, binary, *args, name="app"):
        app = App(binary, self.datadir, self.root, name, args)
        self.apps.append(app)
        return app

    def only_conf_in_datadir(self, also=()):
        """What the data folder holds besides OrcaSlicer.conf and `also`, as a problem, or None."""
        extra = sorted(set(os.listdir(self.datadir)) - {"OrcaSlicer.conf", *also})
        return f"the data folder gained {extra}, so the GUI started" if extra else None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        for app in self.apps:
            app.close()
        remove(self.root)


class App:
    """One start of the app, its output in files beside the data folder."""

    def __init__(self, binary, datadir, root, name, args):
        self.datadir = datadir
        self.stderr_path = os.path.join(root, name + ".stderr.txt")
        env = app_env(dict(os.environ, ORCA_MCP_TOKEN=TOKEN), datadir)
        with open(os.path.join(root, name + ".stdout.txt"), "w") as out, open(self.stderr_path, "w") as err:
            self.proc = subprocess.Popen([binary, "--datadir", datadir, *args], env=env, cwd=datadir, stdout=out, stderr=err)
        self.started = time.monotonic()
        self.orca = None

    def wait(self, limit):
        """The exit code once the process is gone within `limit` s of its start, else None after killing it."""
        try:
            return exit_code(self.proc.wait(timeout=max(0.0, limit - (time.monotonic() - self.started))))
        except subprocess.TimeoutExpired:
            self.kill()
            return None

    def stderr(self):
        with open(self.stderr_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def serve(self, port):
        """Waits until this app's server answers on `port` and returns "", or why it did not. The discovery file
        names the pid that wrote it, which tells this app from another one on the port."""
        orca = OrcaMcp(port, TOKEN)
        deadline = time.monotonic() + READY_LIMIT_S
        while time.monotonic() < deadline and self.proc.poll() is None:
            try:
                if not orca.call("app_wait", _timeout=15, **{"for": "ready", "timeout_s": 5}).get("satisfied"):
                    continue
            except DROPPED:
                time.sleep(0.5)
                continue
            try:
                with open(os.path.join(self.datadir, "mcp", "server.json"), encoding="utf-8") as f:
                    info = json.load(f)
            except (OSError, ValueError) as e:
                return f"a server is ready on port {port}, but this app wrote no discovery file ({e})"
            if info.get("pid") != self.proc.pid or info.get("port") != port:
                return f"a server is ready on port {port}, but the discovery file says {info}"
            self.orca = orca
            return ""
        if self.proc.poll() is not None:
            return f"the app exited with code {exit_code(self.proc.returncode)} before it served on {port}"
        return f"the app did not serve on {port} in {READY_LIMIT_S} s"

    def quit(self):
        """Closes the app with File > Quit, as a person does, and returns its exit code, or None when it was killed.
        The click's reply may not arrive, since the server closes with the app."""
        if self.orca is not None and self.proc.poll() is None:
            try:
                _, texts = self.orca.call_with_text("widget_list", _timeout=10, query="Quit")
                found = re.search(r'\[(\w+)\] menu_item "Quit"', "".join(texts))
                if found:
                    self.orca.call_raw("widget_click", {"target": found.group(1)}, timeout=10)
            except DROPPED:
                pass
        try:
            return exit_code(self.proc.wait(timeout=EXIT_LIMIT_S))
        except subprocess.TimeoutExpired:
            self.kill()
            return None

    def kill(self):
        if self.proc.poll() is None:
            if os.name == "nt":
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True)
            else:
                self.proc.kill()
            self.proc.wait()

    def close(self):
        if self.proc.poll() is None:
            if self.orca is not None:
                self.quit()
            self.kill()


def refused_options(binary):
    problems = []
    for label, args in (("--mcp-run r.json", lambda work: ["--mcp-run", work.file("r.json", json.dumps(REPLAY))]),
                        ("--mcp-run-out d", lambda work: ["--mcp-run-out", work.file("d")]),
                        ("--mcp-token t", lambda work: ["--mcp-token", "t"]),
                        ("--no-mcp", lambda work: ["--no-mcp"])):
        with Work() as work:
            app = work.start(binary, *args(work))
            code = app.wait(REFUSE_LIMIT_S)
            if code is None:
                problems.append(f"{label}: still running {REFUSE_LIMIT_S} s after its start")
            elif code == 0:
                problems.append(f"{label}: exit code 0")
            if "Invalid option" not in app.stderr():
                problems.append(f"{label}: no \"Invalid option\" on stderr")
            if (gui := work.only_conf_in_datadir()) is not None:
                problems.append(f"{label}: {gui}")
    check("refused_options", problems, "each exits non-zero within 10 s with \"Invalid option\" and no GUI")


def run_invalid_file(binary):
    problems = []
    with Work() as work:
        missing = work.file("missing.json")
        code = work.start(binary, "--run", missing, name="missing").wait(RUN_LIMIT_S)
        if code != 2:
            problems.append(f"--run missing.json: exit code {code}")
        text = work.file("a.txt", json.dumps(REPLAY))
        code = work.start(binary, "--run", text, name="txt").wait(RUN_LIMIT_S)
        if code != 2:
            problems.append(f"--run a.txt: exit code {code}")
    check("run_invalid_file", problems, "--run missing.json and --run a.txt each exit 2")


def run_replay_folders(binary):
    problems = []
    with Work() as work:
        replay = work.file("r.json", json.dumps(REPLAY))
        beside = os.path.join(work.files, "r.out")
        code = work.start(binary, "--run", replay, name="beside").wait(RUN_LIMIT_S)
        if code != 0:
            problems.append(f"--run r.json: exit code {code}")
        if not os.path.isfile(os.path.join(beside, "result.json")):
            problems.append("--run r.json: no result.json in r.out")
        shutil.rmtree(beside, ignore_errors=True)
        given = os.path.join(work.root, "given", "D")
        code = work.start(binary, "--run", replay, "--outputdir", given, name="given").wait(RUN_LIMIT_S)
        if code != 0:
            problems.append(f"--run r.json --outputdir D: exit code {code}")
        if not os.path.isfile(os.path.join(given, "result.json")):
            problems.append("--run r.json --outputdir D: no result.json in D")
        if os.path.exists(beside):
            problems.append("--run r.json --outputdir D: r.out was created")
    check("run_replay_folders", problems, "result.json in r.out, and with --outputdir in D alone")


def pngs(folder):
    try:
        return [n for n in os.listdir(folder) if n.lower().endswith(".png")]
    except OSError:
        return []


def run_script_output_folder(binary):
    problems = []
    with Work() as work:
        script = work.file("s.py", SCREENSHOT_SCRIPT)
        given = os.path.join(work.root, "D")
        code = work.start(binary, "--run", script, "--outputdir", given, name="given").wait(RUN_LIMIT_S)
        if code != 0:
            problems.append(f"--run s.py --outputdir D: exit code {code}")
        if len(pngs(given)) != 1:
            problems.append(f"--run s.py --outputdir D: D holds {pngs(given)}")
        default = os.path.join(work.datadir, "automation", "out")
        if pngs(default):
            problems.append(f"--run s.py --outputdir D: <datadir>/automation/out holds {pngs(default)}")
        code = work.start(binary, "--run", script, name="default").wait(RUN_LIMIT_S)
        if code != 0:
            problems.append(f"--run s.py: exit code {code}")
        if len(pngs(default)) != 1:
            problems.append(f"--run s.py: <datadir>/automation/out holds {pngs(default)}")
        if os.path.exists(os.path.join(work.datadir, "mcp", "out")):
            problems.append("--run s.py: <datadir>/mcp/out exists")
    check("run_script_output_folder", problems, "the screenshot in D with --outputdir, else in <datadir>/automation/out")


def run_refused_without_gui(binary):
    problems = []
    for other, extra in (("--export-3mf", ["--export-3mf", "out.3mf", "model.stl"]), ("--downward-check", ["--downward-check"])):
        with Work() as work:
            script = work.file("s.py", SCREENSHOT_SCRIPT)
            app = work.start(binary, "--run", script, *extra)
            code = app.wait(REFUSE_LIMIT_S)
            if code not in INVALID_PARAMS:
                problems.append(f"--run s.py {other}: exit code {code}")
            if f"--run cannot be combined with {other}" not in app.stderr():
                problems.append(f"--run s.py {other}: stderr says {app.stderr().strip()[:200]!r}")
            # A refused command line records its reason in result.json, which goes to the folder it runs
            # in: the data folder here. A GUI would leave more than that.
            if (gui := work.only_conf_in_datadir({"result.json"})) is not None:
                problems.append(f"--run s.py {other}: {gui}")
    check("run_refused_without_gui", problems, "each exits CLI_INVALID_PARAMS naming --run, with no GUI")


def mcp_port_opens_endpoint(binary):
    problems = []
    with Work(mcp_enabled=False) as work:
        app = work.start(binary, "--mcp-port", str(GIVEN_PORT))
        if (why := app.serve(GIVEN_PORT)):
            problems.append(f"--mcp-port {GIVEN_PORT}: {why}")
        app.quit()
    with Work(mcp_enabled=False, mcp_port=str(PORT)) as work:
        app = work.start(binary, "--mcp-port", "0")
        if (why := app.serve(PORT)):
            problems.append(f"--mcp-port 0: {why}")
        app.quit()
    check("mcp_port_opens_endpoint", problems, f"--mcp-port {GIVEN_PORT} serves there, --mcp-port 0 on the conf's {PORT}")


def unrestricted_scripts_flag(binary):
    problems = []
    outside = tempfile.mkdtemp(prefix="ocl-out-")
    try:
        target = write(os.path.join(outside, "outside.txt"), "outside")
        with Work() as work:
            script = work.file("r.py", f"""
with open({target!r}, encoding="utf-8") as f:
    if f.read() != "outside":
        raise RuntimeError("the file read back otherwise")
""")
            code = work.start(binary, "--unrestricted-scripts", "--run", script, name="flag").wait(RUN_LIMIT_S)
            if code != 0:
                problems.append(f"--unrestricted-scripts --run r.py: exit code {code}")
            value = read_conf(work.datadir).get("python_scripts_unrestricted")
            if not is_off(value):
                problems.append(f"the conf's python_scripts_unrestricted is {value!r} after exit")
            code = work.start(binary, "--run", script, name="no-flag").wait(RUN_LIMIT_S)
            if code != 1:
                problems.append(f"--run r.py without the flag: exit code {code}")
    finally:
        remove(outside)
    check("unrestricted_scripts_flag", problems,
          "the script read outside with the flag, the setting stayed off, and without the flag it exited 1")


def design_commands(orca):
    """How many commands tool_search lists in the group Design, and "" or why the answer was not one. With the Design
    tab off no command has that group, and tool_search refuses it as unknown, naming the groups there are; that
    refusal counts as none, and any other error is a problem."""
    try:
        return len(orca.call("tool_search", group="Design").get("names", [])), ""
    except McpError as e:
        # The refusal's details, on the line after its text, list the groups: places' headings, some with a comma.
        text, _, details = str(e).rpartition("\n")
        try:
            groups = json.loads(details).get("groups")
        except (ValueError, AttributeError):
            groups = None
        if text.startswith("Unknown group: Design;") and isinstance(groups, list) and "Design" not in groups:
            return 0, ""
        return 0, f"tool_search {{group: Design}} failed: {str(e)[:300]}"


def cad_flag(binary):
    problems = []
    found = 0
    with Work(mcp_port=str(PORT)) as work:
        app = work.start(binary, "--cad", "--mcp", name="flag")
        if (why := app.serve(PORT)):
            problems.append(f"--cad --mcp: {why}")
        else:
            found, error = design_commands(app.orca)
            if error:
                problems.append(f"--cad --mcp: {error}")
            elif found == 0:
                problems.append("--cad --mcp: tool_search finds no Design command")
            note = app.orca.call("app_state").get("cad_note")
            if note is not None:
                problems.append(f"--cad --mcp: app_state has cad_note {note!r}")
        app.quit()
        value = read_conf(work.datadir).get("enable_cad_feature")
        if not is_off(value):
            problems.append(f"the conf's enable_cad_feature is {value!r} after exit")
        app = work.start(binary, "--mcp", name="no-flag")
        if (why := app.serve(PORT)):
            problems.append(f"--mcp: {why}")
        else:
            count, error = design_commands(app.orca)
            if error:
                problems.append(f"--mcp without --cad: {error}")
            elif count != 0:
                problems.append(f"--mcp without --cad: tool_search finds {count} Design commands")
        app.quit()
    check("cad_flag", problems, f"{found} Design commands with --cad, none without, enable_cad_feature stayed off")


def run_beside_open_window(binary):
    problems = []
    with Work(single_instance=True) as work:
        replay = work.file("r.json", json.dumps(REPLAY))
        first = work.start(binary, "--mcp", "--mcp-port", str(PORT), name="first")
        if (why := first.serve(PORT)):
            return check("run_beside_open_window", [f"the first window: {why}"], "")
        out = os.path.join(work.root, "second")
        code = work.start(binary, "--run", replay, "--outputdir", out, name="second").wait(RUN_LIMIT_S)
        if code != 0:
            problems.append(f"--run r.json beside the first window: exit code {code}")
        if not os.path.isfile(os.path.join(out, "result.json")):
            problems.append("--run r.json beside the first window: no result.json")
        objects = first.orca.call("app_state").get("objects")
        if objects != 0:
            problems.append(f"the first window has {objects} objects")
        first.quit()
        both = [(os.path.join(work.root, f"together{i}"), work.start(binary, "--run", replay, "--outputdir",
                                                                      os.path.join(work.root, f"together{i}"),
                                                                      name=f"together{i}")) for i in (1, 2)]
        for i, (folder, app) in enumerate(both, 1):
            code = app.wait(RUN_LIMIT_S)
            if code != 0:
                problems.append(f"run {i} of two started together: exit code {code}")
            if not os.path.isfile(os.path.join(folder, "result.json")):
                problems.append(f"run {i} of two started together: no result.json")
    check("run_beside_open_window", problems,
          "the run beside an open window exited 0 there and loaded nothing into it; two together both exited 0")


def short_path(path):
    """The 8.3 spelling Windows gives an existing path, or None where the volume keeps no short names."""
    import ctypes
    buffer = ctypes.create_unicode_buffer(1024)
    length = ctypes.windll.kernel32.GetShortPathNameW(path, buffer, len(buffer))
    return buffer.value if 0 < length < len(buffer) and buffer.value.lower() != path.lower() else None


def window_per_data_folder(binary):
    problems = []
    with Work(single_instance=True) as first_work, Work(single_instance=True) as second_work:
        first = first_work.start(binary, "--mcp", "--mcp-port", str(PORT), name="first")
        if (why := first.serve(PORT)):
            return check("window_per_data_folder", [f"the first window: {why}"], "")
        # Each start hands the first window its command line, whichever way it spells the exe and the folder.
        starts = [("the first window's data folder", binary, first_work.datadir)]
        if os.name == "nt":
            starts += [("the folder in upper case", binary, first_work.datadir.upper()),
                       ("the exe's path in upper case", binary.upper(), first_work.datadir),
                       ("the exe by a relative path", os.path.join(".", os.path.basename(binary)), first_work.datadir)]
            if (short := short_path(first_work.datadir)):
                starts.append(("the folder's 8.3 name", binary, short))
        for n, (what, exe, folder) in enumerate(starts, 1):
            # A relative exe is found from the folder of the process that starts it. With a folder part, as
            # .\orca-slicer.exe, it is not looked for on PATH, which may hold an installed OrcaSlicer's alias.
            here = os.getcwd()
            os.chdir(os.path.dirname(os.path.abspath(binary)))
            try:
                app = App(exe, folder, first_work.root, f"same{n}", (MODEL,))
            finally:
                os.chdir(here)
            first_work.apps.append(app)
            code = app.wait(RUN_LIMIT_S)
            if code not in HANDED_OVER:
                problems.append(f"a start with a model on {what}: exit code {code}")
                break
            if sys.platform != "linux":
                deadline = time.monotonic() + 30
                while (objects := first.orca.call("app_state").get("objects")) != n and time.monotonic() < deadline:
                    time.sleep(0.5)
                if objects != n:
                    problems.append(f"the first window has {objects} objects after a start on {what} handed it a model")
                    break
        second = second_work.start(binary, "--mcp", "--mcp-port", str(GIVEN_PORT), name="second")
        if (why := second.serve(GIVEN_PORT)):
            problems.append(f"the window on the second data folder: {why}")
    check("window_per_data_folder", problems,
          "starts on the first window's data folder, however spelled, handed it the model; a window on a second one "
          "served beside it")


CHECKS = {f.__name__: f for f in (refused_options, run_invalid_file, run_replay_folders, run_script_output_folder,
                                  run_refused_without_gui, mcp_port_opens_endpoint, unrestricted_scripts_flag, cad_flag,
                                  run_beside_open_window, window_per_data_folder)}


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
              f"usage: python tools/mcp/test_command_line.py [--bin PATH] [CHECK ...]")
        return 2
    binary = find_binary(explicit)
    for name in argv or CHECKS:
        CHECKS[name](binary)
    failed = results.count(False)
    print(f"{len(results) - failed} of {len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
