#!/usr/bin/env python3
"""Checks how orca_mcp.py attaches to a running OrcaSlicer and calls commands, without the app.
Its design is the test_orca_mcp_attach entry of tools/mcp/test-entries/runtime.md.

    python tools/mcp/test_orca_mcp_attach.py

default_datadir() takes a portable data_dir beside the binary (beside the .app on macOS) and
otherwise the platform's configuration folder; it resolves a symlinked binary on Linux, as the app
does through /proc/self/exe, and takes it as given on Windows, as the app does, and on macOS, as the
stdio bridge does (skipped where no symlink can be made). from_datadir() refuses a datadir without
mcp/server.json, or whose server.json is not JSON or names no port, naming the file and giving the
--mcp advice; and one whose server.json names a port where nothing listens or where another server
answers (HTTP 401, a reply that is not JSON), also chaining the cause. call_command(), against a
stubbed call(), fails at once on a dialog, follows a running job, prefixes its errors with the
command's name, chaining the original, and returns the command's whole reply. seed_datadir() keeps a fresh
datadir's login in its own token file, opens its main window 1600 x 900 at the top left of the work area, seeds the
default printer, the Generic Klipper Printer 0.4 nozzle, and leaves a reused datadir's conf as it is, returning
whether it seeded. launch(), with the binary, the display, the process and the calls stubbed out, records every
preference preferences_get lists in mcp-baseline.json in a datadir it seeds, and nothing in one that had a conf.
ORCA_MCP_CALL_LOG gets a line per call, and reply_budget_s() reads ORCA_MCP_REPLY_BUDGET_MS as the app does. Exits 0
when every check passes.
The folders the checks make are removed when they end. The checks are asserts, so a Python that runs with its
asserts off (-O, or PYTHONOPTIMIZE, which the sweep's runner passes on) fails the test before any of them.
"""

import argparse
import contextlib
import http.server
import io
import json
import os
import socket
import sys
import tempfile
import threading
import types
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import asserts_live, ok, read_conf  # noqa: E402
from orca_mcp import McpError, OrcaMcp  # noqa: E402

_ROOT = None  # the temporary folder main() makes, which holds every folder the checks make


def scratch(prefix):
    """A new folder for a check, removed with the rest when main() returns."""
    return tempfile.mkdtemp(prefix=prefix, dir=_ROOT)


def same_path(a, b):
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


def raised(fn, *needles):
    """Runs fn, which must raise McpError whose message contains every needle; returns the error."""
    try:
        fn()
    except McpError as e:
        for needle in needles:
            assert needle in str(e), f"{needle!r} not in {str(e)!r}"
        return e
    raise AssertionError("no McpError raised")


def raises(fn, *needles):
    """Like raised(), returning the message."""
    return str(raised(fn, *needles))


def binary_in(folder):
    """Where the OrcaSlicer binary of an install in `folder` lives."""
    if sys.platform == "darwin":
        return os.path.join(folder, "OrcaSlicer.app", "Contents", "MacOS", "OrcaSlicer")
    return os.path.join(folder, "orca-slicer.exe" if os.name == "nt" else "orca-slicer")


def check_default_datadir():
    install = scratch("orca-attach-portable-")
    os.makedirs(os.path.join(install, "data_dir"))
    assert same_path(orca_mcp.default_datadir(binary_in(install)), os.path.join(install, "data_dir"))
    ok("default_datadir() takes the portable data_dir beside the binary")

    home = scratch("orca-attach-home-")
    no_portable = binary_in(scratch("orca-attach-installed-"))
    if sys.platform == "win32":
        cases = [({"APPDATA": home}, os.path.join(home, "OrcaSlicer"))]
    elif sys.platform == "darwin":
        cases = [({"HOME": home}, os.path.join(home, "Library", "Application Support", "OrcaSlicer"))]
    else:
        xdg = os.path.join(home, "xdg")
        cases = [({"XDG_CONFIG_HOME": xdg, "HOME": home}, os.path.join(xdg, "OrcaSlicer")),
                 ({"XDG_CONFIG_HOME": "", "HOME": home}, os.path.join(home, ".config", "OrcaSlicer"))]
    for env, expected in cases:
        with mock.patch.dict(os.environ, env):
            for exe in (None, no_portable):
                got = orca_mcp.default_datadir(exe)
                assert same_path(got, expected), f"{sys.platform} {env} exe={exe}: {got} != {expected}"
    ok(f"default_datadir() falls back to the {sys.platform} configuration folder")


def check_symlinked_binary():
    # /usr/local/bin/orca-slicer -> /opt/orca/orca-slicer, each folder with a data_dir of its own.
    real, linked = scratch("orca-attach-real-"), scratch("orca-attach-link-")
    real_exe, link = binary_in(real), binary_in(linked)
    for exe in (real_exe, link):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
    open(real_exe, "w").close()
    for folder in (real, linked):
        os.makedirs(os.path.join(folder, "data_dir"))
    try:
        os.symlink(real_exe, link)
    except (OSError, NotImplementedError) as e:
        print(f"skip - default_datadir() with a symlinked binary: no symlink can be made here ({e})")
        return
    if sys.platform == "darwin":
        # As McpStdioBridge.cpp's _NSGetExecutablePath; whether the app's own lookup (the main
        # bundle's) resolves a symlinked binary is unverified.
        expected, how = linked, "takes a symlinked binary as given, as the stdio bridge does"
    elif sys.platform == "win32":
        expected, how = linked, "takes a symlinked binary as given, as the app does"
    else:
        expected, how = real, "resolves a symlinked binary, as the app does"
    expected = os.path.join(expected, "data_dir")
    got = orca_mcp.default_datadir(link)
    assert same_path(got, expected), f"{sys.platform}: {got} != {expected}"
    ok(f"default_datadir() {how} on {sys.platform}")


def datadir_with(text, prefix):
    """A datadir whose mcp/server.json holds `text`; returns it and that file's path."""
    datadir = scratch(prefix)
    os.makedirs(os.path.join(datadir, "mcp"))
    path = os.path.join(datadir, "mcp", "server.json")
    with open(path, "w") as f:
        f.write(text)
    return datadir, path


def datadir_naming(port, prefix):
    """A datadir whose mcp/server.json names `port`; returns it and that file's path."""
    return datadir_with(json.dumps({"url": f"http://127.0.0.1:{port}/mcp", "port": port, "pid": 0, "version": "0", "token": "t"}),
                        prefix)


def check_from_datadir():
    empty = scratch("orca-attach-empty-")
    raises(lambda: OrcaMcp.from_datadir(empty), os.path.join(empty, "mcp", "server.json"), "--mcp")
    ok("from_datadir() without server.json names the file and says to start with --mcp")

    # "cannot read the port" is the unreadable file's own error: a file read some other way (its
    # url's port, say) would end at the ping instead, whose error also names the file.
    for text in ("{not json", json.dumps({"url": "http://127.0.0.1:1/mcp", "token": "t"})):
        datadir, path = datadir_with(text, "orca-attach-malformed-")
        raises(lambda: OrcaMcp.from_datadir(datadir), "cannot read the port", path, "--mcp")
    ok("from_datadir() with a server.json that is not JSON, or names no port, says it cannot read the port, naming the file")

    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    stale, path = datadir_naming(port, "orca-attach-stale-")
    error = raised(lambda: OrcaMcp.from_datadir(stale), path, str(port), "--mcp")
    assert isinstance(error.__cause__, OSError), repr(error.__cause__)
    ok("from_datadir() with a stale server.json names the file and its port, with the refusal as the cause")

    check_port_taken(401, b"Unauthorized", "HTTP 401")
    check_port_taken(200, b"<html>not MCP</html>", "Expecting value")


def check_port_taken(status, body, cause):
    """from_datadir() on a port where another server answers HTTP `status` with `body`: the error names
    the file and the port, gives the --mcp advice, and holds and chains the cause (`cause` is in its message)."""
    with Listener(status, body) as port:
        datadir, path = datadir_naming(port, "orca-attach-taken-")
        error = raised(lambda: OrcaMcp.from_datadir(datadir), path, str(port), cause, "--mcp")
        assert error.__cause__ is not None, f"HTTP {status}: no cause"
    ok(f"from_datadir() on a port where another server answers HTTP {status} {body.decode()} names the file and the cause")


class Listener:
    """A local HTTP server, on a free port, that answers every POST with `status` and `body`; the
    context gives its port and shuts it down and closes it on leaving."""

    def __init__(self, status, body):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
                self.send_response(status)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self.server.server_address[1]

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def stub(client, *replies):
    """Replaces client.call with one that hands out `replies` in turn (raising an exception reply)
    and returns the list of calls it saw."""
    calls, queue = [], list(replies)

    def call(tool, /, _timeout=None, **args):
        calls.append((tool, _timeout, args))
        reply = queue.pop(0)
        if isinstance(reply, Exception):
            raise reply
        return reply

    client.call = call
    return calls


def check_call_command():
    client = OrcaMcp(1, "t")
    calls = stub(client, {"status": "blocked_by_dialog", "job": "j", "dialogs": [{"title": "Save?"}]})
    message = raises(lambda: client.call_command("solid_extrude", distance=5), "Save?")
    assert message.startswith("solid_extrude: "), message
    assert len(calls) == 1, calls
    ok("call_command() fails at once on a dialog, naming it")

    calls = stub(client, {"result": {"ok": True}})
    assert client.call_command("solid_extrude", distance=5) == {"result": {"ok": True}}
    assert calls == [("tool_call", 600, {"tool": "solid_extrude", "args": {"distance": 5}})], calls
    ok("call_command() calls tool_call and returns the command's whole reply")

    calls = stub(client, {"status": "running", "job": "j"}, {"result": 7})
    assert client.call_command("solid_sweep", sketch=1, path=0) == {"result": 7}
    assert calls[1][0] == "app_wait" and calls[1][2]["job"] == "j", calls
    ok("call_command() follows a running job to its reply")

    refused = McpError("kernel refused the hole")
    stub(client, refused)
    error = raised(lambda: client.call_command("solid_hole"), "kernel refused the hole")
    assert str(error).startswith("solid_hole: "), error
    assert error.__cause__ is refused, repr(error.__cause__)
    stub(client, {"status": "running", "job": "j"}, McpError("the job failed"))
    message = raises(lambda: client.call_command("solid_hole"), "the job failed")
    assert message.startswith("solid_hole: "), message
    ok("call_command() starts every error with the command's name, with the original as its cause")


def check_call_log():
    log = os.path.join(scratch("orca-attach-log-"), "calls.jsonl")
    reply = json.dumps({"jsonrpc": "2.0", "id": 1, "result": {"content": [{"type": "text", "text": "{}"}]}}).encode()
    with mock.patch.dict(os.environ, {"ORCA_MCP_CALL_LOG": log}):
        with Listener(200, reply) as port:
            OrcaMcp(port, "t").call("app_state")
        with open(log, encoding="utf-8") as f:
            lines = [json.loads(line) for line in f]
        assert len(lines) == 1, lines
        assert lines[0]["tool"] == "app_state" and lines[0]["ok"] is True and lines[0]["ms"] >= 0, lines
        with Listener(500, b"boom") as port:
            raises(lambda: OrcaMcp(port, "t").call("app_state"), "HTTP 500")
        with open(log, encoding="utf-8") as f:
            lines = [json.loads(line) for line in f]
        assert len(lines) == 2 and lines[1]["ok"] is False, lines
    ok("ORCA_MCP_CALL_LOG gets one line per call with its tool, time and whether it succeeded")


def check_reply_budget():
    for value, budget in ((None, 45.0), ("2000", 2.0), ("90000", 45.0), ("0", 45.0), ("-5", 45.0), ("1.5", 45.0),
                          (" 2000", 45.0), ("abc", 45.0)):
        env = {} if value is None else {"ORCA_MCP_REPLY_BUDGET_MS": value}
        with mock.patch.dict(os.environ, env):
            if value is None:
                os.environ.pop("ORCA_MCP_REPLY_BUDGET_MS", None)
            got = orca_mcp.reply_budget_s()
        assert got == budget, f"{value!r}: {got} != {budget}"
    ok("reply_budget_s() is 45 s, or less where ORCA_MCP_REPLY_BUDGET_MS caps it, read as the app reads it")


def check_seed_datadir():
    fresh = scratch("orca-seed-")
    with mock.patch.object(orca_mcp, "work_area_origin", return_value=(12, 34)):
        seeded = orca_mcp.seed_datadir(fresh, orca_mcp.DEFAULT_PRINTER)
    app = read_conf(fresh)["app"]
    assert app.get("use_encrypted_token_file") is True, app
    ok("seed_datadir() keeps a fresh datadir's login in its own token file, not the system's secret store")
    assert orca_mcp.WINDOW_SIZE == (1600, 900), orca_mcp.WINDOW_SIZE
    assert app.get("window_mainframe") == "12; 34; 1600; 900; 0", app
    x, y = orca_mcp.work_area_origin()
    assert isinstance(x, int) and isinstance(y, int) and x >= 0 and y >= 0, (x, y)
    ok("seed_datadir() opens a fresh datadir's main window 1600 x 900 at the top left of the work area")
    conf = read_conf(fresh)
    assert conf["models"] == [{"model": "Generic Klipper Printer", "nozzle_diameter": "0.4", "vendor": "Custom"}], conf
    assert conf["filaments"] == ["Generic PLA @System"], conf
    # The printer preset's own name, which is not the model's.
    assert conf["presets"] == {"machine": "MyKlipper 0.4 nozzle"}, conf
    ok("seed_datadir() seeds the default printer")

    reused = scratch("orca-seed-reused-")
    orca_mcp.write_conf(os.path.join(reused, "OrcaSlicer.conf"), {"app": {"use_encrypted_token_file": False}})
    kept = orca_mcp.seed_datadir(reused, orca_mcp.DEFAULT_PRINTER)
    conf = read_conf(reused)
    assert conf == {"app": {"use_encrypted_token_file": False}}, conf
    ok("seed_datadir() leaves a reused datadir's conf as it is, its login where the conf keeps it")
    assert seeded is True and kept is False, (seeded, kept)
    ok("seed_datadir() returns True when it seeds a datadir and False when the datadir has a conf")


def check_launch_baseline():
    """launch() with the binary, the display, the app's process and its calls stubbed out, on a new datadir and on
    one that has a conf."""
    items = [{"key": "auto_slice_after_change", "value": False}, {"key": "language", "value": "en", "refused": True},
             {"key": "default_page", "value": 1}]
    calls = []

    def call(self, tool, /, _timeout=None, **args):
        calls.append(tool)
        return {"app_state": {"dialogs": []}, "preferences_get": {"items": items}}[tool]

    def launched(datadir):
        """Runs launch() on `datadir` and returns where its baseline file would be."""
        args = argparse.Namespace(datadir=datadir, filament=None, printer=None, vendor="BBL", bin=None, port=1,
                                  cad=True, unrestricted_scripts=True, extra=[], timeout=1, print_config=False,
                                  headless=None, xvfb=False, width=1600, height=1000)
        session = os.path.join(scratch("orca-launch-session-"), "session.json")
        with mock.patch.object(orca_mcp, "SESSION_FILE", session), \
                mock.patch.object(orca_mcp, "find_binary", lambda explicit: "orca-slicer"), \
                mock.patch.object(orca_mcp, "start_display", lambda args, datadir: (None, None, None)), \
                mock.patch.object(orca_mcp.subprocess, "Popen", lambda *a, **k: types.SimpleNamespace(pid=4242)), \
                mock.patch.object(orca_mcp, "wait_ready", lambda client, timeout: True), \
                mock.patch.object(OrcaMcp, "call", call), contextlib.redirect_stdout(io.StringIO()):
            orca_mcp.launch(args)
        return os.path.join(datadir, orca_mcp.BASELINE_FILE)

    baseline = launched(os.path.join(scratch("orca-launch-fresh-"), "datadir"))
    with open(baseline, encoding="utf-8") as f:
        recorded = json.load(f)
    # Every preference, a refused one too: start() leaves out what preferences_get marks refused when it reads them.
    want = {"preferences": {"auto_slice_after_change": False, "language": "en", "default_page": 1}}
    assert recorded == want, recorded
    assert calls.count("preferences_get") == 1, calls

    calls.clear()
    reused = scratch("orca-launch-reused-")
    orca_mcp.write_conf(os.path.join(reused, "OrcaSlicer.conf"), {"app": {}})
    baseline = launched(reused)
    assert not os.path.exists(baseline), os.listdir(reused)
    assert "preferences_get" not in calls, calls
    ok("launch() records every preference preferences_get lists in mcp-baseline.json in a datadir it seeds, and "
       "nothing in one that had a conf")


def main():
    global _ROOT
    if not asserts_live():
        return 1
    with tempfile.TemporaryDirectory(prefix="orca-attach-") as root:
        _ROOT = root
        check_seed_datadir()
        check_launch_baseline()
        check_default_datadir()
        check_symlinked_binary()
        check_from_datadir()
        check_call_command()
        check_call_log()
        check_reply_budget()
    return 0


if __name__ == "__main__":
    sys.exit(main())
