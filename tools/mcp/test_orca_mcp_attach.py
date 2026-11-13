#!/usr/bin/env python3
"""Checks how orca_mcp.py attaches to a running OrcaSlicer and calls commands, without the app.

    python tools/mcp/test_orca_mcp_attach.py

default_datadir() takes a portable data_dir beside the binary (beside the .app on macOS) and
otherwise the platform's configuration folder; it resolves a symlinked binary on Linux, as the app
does through /proc/self/exe, and takes it as given on Windows, as the app does, and on macOS, as the
stdio bridge does (skipped where no symlink can be made). from_datadir() refuses a datadir without
mcp/server.json, or whose server.json is not JSON or names no port, naming the file and giving the
--mcp advice; and one whose server.json names a port where nothing listens or where another server
answers (HTTP 401, a reply that is not JSON), also chaining the cause. call_command(), against a
stubbed call(), fails at once on a dialog, follows a running job, prefixes its errors with the
command's name, chaining the original, and returns the command's whole reply. Exits 0 when every check passes.
The folders the checks make are removed when they end.
"""

import http.server
import json
import os
import socket
import sys
import tempfile
import threading
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from orca_mcp import McpError, OrcaMcp  # noqa: E402

_ROOT = None  # the temporary folder main() makes, which holds every folder the checks make


def scratch(prefix):
    """A new folder for a check, removed with the rest when main() returns."""
    return tempfile.mkdtemp(prefix=prefix, dir=_ROOT)


def ok(name):
    print(f"ok - {name}")


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
    message = raises(lambda: client.call_command("extrude", distance=5), "Save?")
    assert message.startswith("extrude: "), message
    assert len(calls) == 1, calls
    ok("call_command() fails at once on a dialog, naming it")

    calls = stub(client, {"result": {"ok": True}})
    assert client.call_command("extrude", distance=5) == {"result": {"ok": True}}
    assert calls == [("call_tool", 600, {"tool": "extrude", "args": {"distance": 5}})], calls
    ok("call_command() calls call_tool and returns the command's whole reply")

    calls = stub(client, {"status": "running", "job": "j"}, {"result": 7})
    assert client.call_command("sweep", sketch=1, path=0) == {"result": 7}
    assert calls[1][0] == "wait" and calls[1][2]["job"] == "j", calls
    ok("call_command() follows a running job to its reply")

    refused = McpError("kernel refused the hole")
    stub(client, refused)
    error = raised(lambda: client.call_command("hole"), "kernel refused the hole")
    assert str(error).startswith("hole: "), error
    assert error.__cause__ is refused, repr(error.__cause__)
    stub(client, {"status": "running", "job": "j"}, McpError("the job failed"))
    message = raises(lambda: client.call_command("hole"), "the job failed")
    assert message.startswith("hole: "), message
    ok("call_command() starts every error with the command's name, with the original as its cause")


def main():
    global _ROOT
    with tempfile.TemporaryDirectory(prefix="orca-attach-") as root:
        _ROOT = root
        check_default_datadir()
        check_symlinked_binary()
        check_from_datadir()
        check_call_command()
    return 0


if __name__ == "__main__":
    sys.exit(main())
