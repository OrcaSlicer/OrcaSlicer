#!/usr/bin/env python3
"""Drive OrcaSlicer's MCP server from Python or a shell, for development and tests.

Standard library only. Two halves:

* ``OrcaMcp``: a minimal client for the server's HTTP endpoint (``call(tool, **args)``), which
  ``OrcaMcp.from_datadir()`` attaches to an OrcaSlicer already running with the server on.
* A command line that launches OrcaSlicer for an agent or a test and talks to it::

    tools/mcp/orca_mcp.py launch --headless         # fresh datadir on a private headless desktop (Linux), wait until ready
    tools/mcp/orca_mcp.py launch --xvfb             # on a private Xvfb display, for a Linux machine without a desktop
    tools/mcp/orca_mcp.py launch --cad              # with the Design (CAD) tab on, for its commands (search_tools group Design)
    tools/mcp/orca_mcp.py launch --unrestricted-scripts  # with Python scripts (run_script) unrestricted
    tools/mcp/orca_mcp.py call get_app_state
    tools/mcp/orca_mcp.py call open_files '{"paths": ["/abs/model.stl"]}'
    tools/mcp/orca_mcp.py screenshot /tmp/shot.png --target canvas
    tools/mcp/orca_mcp.py widgets
    tools/mcp/orca_mcp.py run-replay tools/mcp/smoke_test.json --out /tmp/smoke
    tools/mcp/orca_mcp.py stop

``launch`` records the instance (port, token, pid, datadir) in a session file
(``$ORCA_MCP_SESSION`` or ``~/.cache/orca-mcp/session.json``) that the other commands read.
"""

import argparse
import base64
import json
import os
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

DEFAULT_PORT = 13619
SESSION_FILE = os.environ.get("ORCA_MCP_SESSION", os.path.expanduser("~/.cache/orca-mcp/session.json"))
PROTOCOL = "2025-11-25"

# A finished setup with one printer, so a fresh datadir opens straight into the main window.
DEFAULT_PRINTER = ("BBL", "Bambu Lab X1 Carbon", "0.4", "Bambu PLA Basic @BBL X1C")


class McpError(RuntimeError):
    """A tool returned isError, or the server answered with a JSON-RPC error."""

    def __init__(self, message, payload=None):
        super().__init__(message)
        self.payload = payload


def default_datadir(exe=None):
    """The datadir OrcaSlicer uses without --datadir, as McpStdioBridge.cpp's default_datadir() picks it.

    A portable install keeps it in data_dir beside the binary `exe` (beside the .app on macOS), which
    is looked for only when `exe` is given; otherwise it is the platform's configuration folder. On
    Linux the binary is located as the app locates itself, through /proc/self/exe, which resolves
    symlinks, so a symlinked `exe` is resolved too; on Windows by GetModuleFileName, which leaves them.
    On macOS it follows the stdio bridge, whose _NSGetExecutablePath takes the path as given; whether
    the app itself resolves a symlinked binary there (its lookup goes through the main bundle) is
    unverified, which matters only for a symlinked binary."""
    if exe:
        exe = os.path.abspath(exe) if sys.platform in ("win32", "darwin") else os.path.realpath(exe)
        folder = os.path.dirname(exe)
        if sys.platform == "darwin":  # <folder>/OrcaSlicer.app/Contents/MacOS/<exe>
            for _ in range(3):
                folder = os.path.dirname(folder)
        portable = os.path.join(folder, "data_dir")
        if os.path.exists(portable):
            return portable
    if sys.platform == "win32":
        base = os.environ.get("APPDATA", ".")
    elif sys.platform == "darwin":
        base = os.path.join(os.path.expanduser("~"), "Library", "Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "OrcaSlicer")


class OrcaMcp:
    def __init__(self, port=DEFAULT_PORT, token="", host="127.0.0.1", timeout=120):
        self.url = f"http://{host}:{port}/mcp"
        self.port = port
        self.token = token
        self.timeout = timeout
        self._id = 0

    @classmethod
    def from_datadir(cls, datadir=None, exe=None):
        """A client of the OrcaSlicer already running on `datadir` (default: default_datadir(exe)).

        Its MCP server writes its port and token to <datadir>/mcp/server.json when it starts. Raises
        McpError when that file is missing, or when a ping to the port it names fails: nothing listens
        there any more, or something else does (another server, or an OrcaSlicer with another token),
        with the failure as the error's cause."""
        path = os.path.join(datadir or default_datadir(exe), "mcp", "server.json")
        advice = "start OrcaSlicer with --mcp (or turn on \"MCP server\" under AI features in its Preferences)"
        try:
            with open(path, encoding="utf-8") as f:
                info = json.load(f)
            port, token = int(info["port"]), info.get("token", "")
        except FileNotFoundError:
            raise McpError(f"{path} does not exist, so no OrcaSlicer serves MCP on that datadir; {advice}") from None
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise McpError(f"cannot read the port from {path} ({e}); {advice}") from None
        client = cls(port, token)
        # A port the file names may since have gone to anything, so whatever way the ping fails (refused,
        # timed out, an HTTP error, a reply that is not HTTP or not JSON), this datadir's server is not there.
        try:
            client.request("ping", timeout=10)
        except Exception as e:
            raise McpError(f"OrcaSlicer's MCP server does not answer on port {port}, which {path} names ({e}); "
                           f"{advice}") from e
        return client

    def request(self, method, params=None, timeout=None):
        self._id += 1
        body = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            body["params"] = params
        req = urllib.request.Request(
            self.url,
            data=json.dumps(body).encode(),
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json, text/event-stream",
                "Authorization": f"Bearer {self.token}",
                "MCP-Protocol-Version": PROTOCOL,
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                reply = json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            raise McpError(f"HTTP {e.code}: {e.read().decode(errors='replace')}") from None
        if "error" in reply:
            raise McpError(reply["error"].get("message", "error"), reply["error"])
        return reply.get("result", {})

    def initialize(self):
        return self.request("initialize", {"protocolVersion": PROTOCOL, "capabilities": {},
                                           "clientInfo": {"name": "orca_mcp.py", "version": "1"}})

    def tools(self):
        return self.request("tools/list")["tools"]

    def call_raw(self, tool, args=None, timeout=None):
        return self.request("tools/call", {"name": tool, "arguments": args or {}}, timeout=timeout)

    def call(self, tool, /, _timeout=None, **args):
        """Call a tool and return its JSON result; raise McpError when the tool reports an error.

        A call that returns a job envelope (running / blocked_by_dialog) is returned as is. The tool
        name is positional-only, so an argument named tool (call_tool's) goes to the tool."""
        return self.call_with_text(tool, _timeout, **args)[0]

    def call_with_text(self, tool, /, _timeout=None, **args):
        """Like call(), plus the text blocks after the JSON one (list_widgets's tree)."""
        result = self.call_raw(tool, args, timeout=_timeout)
        texts = [c["text"] for c in result.get("content", []) if c.get("type") == "text"]
        text = texts[0] if texts else ""
        if result.get("isError"):
            raise McpError(text, result)
        try:
            data = json.loads(text) if text else {}
        except json.JSONDecodeError:
            data = {"text": text}
        return data, texts[1:]

    def images(self, tool, /, **args):
        """Call a tool and return (json_result, [png bytes...]) for image content blocks."""
        result = self.call_raw(tool, args)
        if result.get("isError"):
            raise McpError(result["content"][0]["text"], result)
        data, imgs = {}, []
        for c in result.get("content", []):
            if c.get("type") == "text":
                data = json.loads(c["text"])
            elif c.get("type") == "image":
                imgs.append(base64.b64decode(c["data"]))
        return data, imgs

    def await_job(self, result, timeout=600):
        """Follow a job envelope until the job finishes and return its result."""
        deadline = time.time() + timeout
        while isinstance(result, dict) and result.get("status") in ("running", "blocked_by_dialog") and "job" in result:
            if time.time() > deadline:
                raise McpError(f"job {result['job']} did not finish in {timeout} s", result)
            result = self.call("wait", **{"for": "job", "job": result["job"], "timeout_s": 45})
        return result

    def call_command(self, tool, /, **args):
        """Call one command, a tool or one the tool list does not show (a Design command), through
        call_tool and return its whole reply; a Design command's result is the reply's "result".

        A running job is followed to its end. A call whose first reply is blocked_by_dialog fails at
        once, since a script that waited would wait on a dialog nobody answers. A call that first
        replied running and opens a dialog later is waited on up to await_job's timeout, since wait
        reports only that its job is still running. Every McpError's message starts "<tool>: ", with
        the original error as its cause. The command's name is positional-only, so its own argument
        named tool goes to the command."""
        try:
            reply = self.call("call_tool", _timeout=600, tool=tool, args=args)
            if reply.get("status") == "blocked_by_dialog":
                titles = ", ".join(f"\"{d.get('title', '')}\"" for d in reply.get("dialogs", []))
                raise McpError(f"blocked by a dialog: {titles} (job {reply.get('job')} goes on once it is answered)", reply)
            return self.await_job(reply)
        except McpError as e:
            raise McpError(f"{tool}: {e}", e.payload) from e

    def ready(self, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if self.call("wait", **{"for": "ready", "timeout_s": 5}).get("satisfied"):
                    return True
            except (McpError, OSError):
                time.sleep(0.5)
        return False


# ---- launching ------------------------------------------------------------------------------

def find_binary(explicit):
    candidates = [explicit] if explicit else []
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    exe = ".exe" if os.name == "nt" else ""
    if sys.platform == "darwin":
        # The CMake target's bundle, then the one build_release_macos.sh assembles; this machine's
        # architecture first.
        arches = sorted(("arm64", "x86_64"), key=lambda a: a != platform.machine()) + ["universal"]
        for tree in [os.path.join("build", a) for a in arches] + ["build"]:
            for bundle in (os.path.join("src", "Release", "OrcaSlicer.app"), os.path.join("OrcaSlicer", "OrcaSlicer.app")):
                candidates.append(os.path.join(repo, tree, bundle, "Contents", "MacOS", "OrcaSlicer"))
    # build_win.bat names a clang-cl tree build-clang.
    candidates += [os.path.join(repo, tree, "src", "Release", "orca-slicer" + exe) for tree in ("build", "build-clang")]
    candidates += [os.path.join(repo, "build", "package", "bin", "orca-slicer"),
                   shutil.which("orca-slicer") or ""]
    for c in candidates:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    sys.exit("orca-slicer not found; pass --bin")


def write_conf(conf, data):
    with open(conf, "w") as f:
        json.dump(data, f, indent=2)
        # On Windows the loader expects a line after the closing brace, where it keeps a checksum, and
        # fails to start the app when the file ends at the brace.
        f.write("\n")


def seed_datadir(datadir, printer, cad=False, unrestricted_scripts=False):
    """Seed a fresh datadir's OrcaSlicer.conf. A datadir that has one keeps it, except for the settings
    cad and unrestricted_scripts turn on, which only the file can: the app creates the Design tab only at
    startup, when its setting is on, and no tool lets Python scripts run unrestricted.

    A fresh datadir keeps the Orca Cloud login in its own token file (use_encrypted_token_file), not in the
    system's secret store, so the instance starts signed out and never reads the person's login: on macOS
    that read stops a newly built app at a keychain prompt that only a person can answer."""
    vendor, model, nozzle, filament = printer
    os.makedirs(datadir, exist_ok=True)
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    switches = {}
    if cad:
        switches["enable_cad_feature"] = True
    if unrestricted_scripts:
        switches["mcp_python_unrestricted"] = "true"
    if os.path.exists(conf):
        if switches:
            with open(conf, encoding="utf-8") as f:
                text = f.read()
            # The app ends the file with a checksum line after the JSON; a mismatch only logs.
            data = json.loads(text[:text.rfind("}") + 1])
            data.setdefault("app", {}).update(switches)
            write_conf(conf, data)
        return
    write_conf(conf, {"firstguide": {"finish": True},
                      "models": [{"model": model, "nozzle_diameter": nozzle, "vendor": vendor}],
                      "filaments": [filament],
                      "presets": {"machine": f"{model} {nozzle} nozzle"},
                      "app": {"check_for_updates": False, "show_daily_tips": False, "use_encrypted_token_file": True,
                              **switches}})


# The headless compositors --headless tries, in order, by the binary each needs.
COMPOSITORS = {"kwin": "kwin_wayland", "mutter": "mutter"}


def start_headless(compositor, width, height, datadir):
    """Start `compositor` headless with XWayland on a private session bus, and return the variables
    the app needs to run on it as an X11 client and the process group to stop, or None when it did
    not come up (its output is in <datadir>/mcp-display.log).

    Unlike Xvfb, the app draws with the GPU's OpenGL driver, and a window manager runs, so focus and
    stacking behave as on a desktop and popup lists stay open; with no window manager, GDK moves the
    focus to a drop-down list itself, which deactivates the main window and closes the list. The private bus keeps the D-Bus services the app starts, such as
    the desktop portals, out of the person's session. Only the compositor knows its XWayland display,
    so it starts a command that writes its environment to a file.

    Headless mutter is not tested yet: its command line and whether it renders on the GPU still need
    checking on a GNOME machine."""
    env_file = os.path.join(datadir, "mcp-display.env")
    if os.path.exists(env_file):
        os.remove(env_file)
    keys = ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XAUTHORITY")
    report = 'printf "%s\\n" ' + " ".join(f'"{k}=${k}"' for k in keys) + ' > "$0.tmp" && mv "$0.tmp" "$0"'
    socket = f"orca-mcp-{os.getpid()}"
    if compositor == "kwin":
        # KWin starts the command detached, outside the process group stop() ends, so the command
        # exits once it has written the file; XWayland stays up without it.
        cmd = ["kwin_wayland", "--virtual", "--width", str(width), "--height", str(height), "--socket", socket,
               "--xwayland", shlex.join(["sh", "-c", report, env_file])]
    else:
        # mutter quits when its command exits, so the command stays, in mutter's process group.
        cmd = ["mutter", "--headless", "--wayland", "--virtual-monitor", f"{width}x{height}", "--wayland-display", socket,
               "--", "sh", "-c", report + "; exec sleep infinity", env_file]
    with open(os.path.join(datadir, "mcp-display.log"), "w") as log:
        proc = subprocess.Popen(["dbus-run-session", "--"] + cmd, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
    deadline = time.time() + 30
    while not os.path.exists(env_file) and proc.poll() is None and time.time() < deadline:
        time.sleep(0.1)
    if os.path.exists(env_file):
        with open(env_file) as f:
            env = {k: v or None for k, _, v in (line.partition("=") for line in f.read().splitlines())}
        os.remove(env_file)
        if env.get("DISPLAY"):
            return dict(env, GDK_BACKEND="x11"), proc.pid
    stop_group(proc.pid)
    return None


def start_display(args, datadir):
    """The private display `launch` runs the app on, as (server name, variables for the app, process
    group to stop), or (None, None, None) for the desktop the harness runs in.

    --headless takes the first headless compositor that is installed and starts, and falls back to
    Xvfb when none does, as on a machine without a desktop; --xvfb takes Xvfb."""
    if (args.headless or args.xvfb) and not sys.platform.startswith("linux"):
        sys.exit("--headless and --xvfb run only on Linux; elsewhere the app opens on the desktop")
    if args.headless:
        explicit = args.headless != "auto"
        for name in [args.headless] if explicit else list(COMPOSITORS):
            missing = [b for b in (COMPOSITORS[name], "dbus-run-session") if not shutil.which(b)]
            if missing:
                if explicit:
                    sys.exit(f"--headless {name} needs {' and '.join(missing)}, which is not installed")
                continue
            if name == "mutter":
                print("headless mutter is not tested yet (test required): check the window, input, screenshots "
                      "and that get_log reports the GPU's OpenGL driver", file=sys.stderr)
            started = start_headless(name, args.width, args.height, datadir)
            if started:
                return (name,) + started
            message = f"{COMPOSITORS[name]} did not start; see {os.path.join(datadir, 'mcp-display.log')}"
            if explicit:
                sys.exit(message)
            print(message, file=sys.stderr)
        print("no headless compositor started, so the app runs on Xvfb: software OpenGL, no window manager",
              file=sys.stderr)
    if args.headless or args.xvfb:
        if not shutil.which("Xvfb"):
            sys.exit("Xvfb is not installed")
        display, pid = start_xvfb(args.width, args.height)
        return "xvfb", {"DISPLAY": display, "GDK_BACKEND": "x11", "WAYLAND_DISPLAY": None}, pid
    return None, None, None


def stop_group(pid, timeout=5, force=True):
    """End the process group `pid` leads: SIGTERM, then wait up to `timeout` s for it to go, and
    SIGKILL what is left when `force`."""
    try:
        os.killpg(pid, signal.SIGTERM)
    except OSError:  # already gone
        return
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            os.waitpid(pid, os.WNOHANG)  # reaps the leader when this process started it
        except ChildProcessError:
            pass
        try:
            os.killpg(pid, 0)
        except OSError:
            return
        time.sleep(0.1)
    if force:
        try:
            os.killpg(pid, signal.SIGKILL)
        except OSError:
            pass


def start_xvfb(width, height):
    for display in range(90, 120):
        if not os.path.exists(f"/tmp/.X11-unix/X{display}") and not os.path.exists(f"/tmp/.X{display}-lock"):
            proc = subprocess.Popen(["Xvfb", f":{display}", "-screen", "0", f"{width}x{height}x24", "-nolisten", "tcp", "-noreset"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            deadline = time.time() + 10
            while not os.path.exists(f"/tmp/.X11-unix/X{display}") and time.time() < deadline:
                time.sleep(0.1)
            park_pointer(f":{display}", width, height)
            return f":{display}", proc.pid
    sys.exit("no free X display for Xvfb")


def park_pointer(display, width, height):
    """Move Xvfb's pointer to the bottom-right corner, where it hovers over nothing.

    It starts in the middle of the screen, where it would raise tooltips over whatever opens there
    and put them in screenshots."""
    try:
        import ctypes
        import ctypes.util
        x11 = ctypes.cdll.LoadLibrary(ctypes.util.find_library("X11") or "libX11.so.6")
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XWarpPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                     ctypes.c_uint, ctypes.c_uint, ctypes.c_int, ctypes.c_int]
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        dpy = None
        for _ in range(50):  # the socket exists a moment before the server accepts clients
            dpy = x11.XOpenDisplay(display.encode())
            if dpy:
                break
            time.sleep(0.1)
        if not dpy:
            return
        x11.XWarpPointer(dpy, 0, x11.XDefaultRootWindow(dpy), 0, 0, 0, 0, width - 1, height - 1)
        x11.XFlush(dpy)
        x11.XCloseDisplay(dpy)
    except OSError:
        pass


def app_env(env, datadir, display_env=None):
    """Adds to `env` what OrcaSlicer needs to start unattended with `datadir`: the variables of the
    private display start_display() gave (None removes one), the CA bundle on Linux and a WebView2
    profile of its own on Windows."""
    for key, value in (display_env or {}).items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    if sys.platform.startswith("linux"):
        # A static OpenSSL asks at startup which CA bundle to trust unless told.
        for bundle in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"):
            if os.path.exists(bundle):
                env.setdefault("SSL_CERT_FILE", bundle)
                break
    if os.name == "nt":
        # The WebView2 profile otherwise lives in %LOCALAPPDATA%\OrcaSlicer, shared with the user's own OrcaSlicer.
        env.setdefault("WEBVIEW2_USER_DATA_FOLDER", os.path.join(datadir, "webview2"))
    return env


def launch(args):
    token = os.environ.get("ORCA_MCP_TOKEN") or base64.b16encode(os.urandom(32)).decode().lower()
    datadir = os.path.abspath(args.datadir or tempfile.mkdtemp(prefix="orca-mcp-"))
    printer = DEFAULT_PRINTER
    if args.printer:
        model, _, nozzle = args.printer.partition("@")
        printer = (args.vendor, model, nozzle or "0.4", args.filament or DEFAULT_PRINTER[3])
    seed_datadir(datadir, printer, args.cad, args.unrestricted_scripts)

    env = dict(os.environ, ORCA_MCP_TOKEN=token)
    binary = find_binary(args.bin)
    server, display_env, server_pid = start_display(args, datadir)
    app_env(env, datadir, display_env)
    cmd = [binary, "--datadir", datadir, "--mcp", "--mcp-port", str(args.port)] + args.extra
    log = open(os.path.join(datadir, "mcp-launch.log"), "w")
    proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

    session = {"port": args.port, "token": token, "pid": proc.pid, "datadir": datadir,
               "display_server": server, "display_server_pid": server_pid, "display_env": display_env,
               "display": env.get("DISPLAY")}
    os.makedirs(os.path.dirname(SESSION_FILE), exist_ok=True)
    with open(SESSION_FILE, "w") as f:
        json.dump(session, f)
    os.chmod(SESSION_FILE, 0o600)

    client = OrcaMcp(args.port, token)
    if not client.ready(args.timeout):
        sys.exit(f"OrcaSlicer did not become ready in {args.timeout} s; see {log.name}")
    url = f"http://127.0.0.1:{args.port}/mcp"
    print(json.dumps({**session, "url": url}, indent=2))
    if args.print_config:
        print(f"\nclaude mcp add --transport http orcaslicer {url} --header \"Authorization: Bearer {token}\"")
        print(f"codex mcp add orcaslicer --url {url} --bearer-token-env-var ORCA_MCP_TOKEN   # export ORCA_MCP_TOKEN={token}")


def session_client():
    try:
        with open(SESSION_FILE) as f:
            s = json.load(f)
    except OSError:
        sys.exit("no session; run launch first (or set ORCA_MCP_SESSION)")
    return OrcaMcp(s["port"], s["token"]), s


def stop(_args):
    _, s = session_client()
    if os.name == "nt":  # Windows has no process groups to signal
        try:
            os.kill(s["pid"], signal.SIGTERM)
        except OSError:  # already gone
            pass
    else:
        # The app goes first and is given time to close before its display does.
        stop_group(s["pid"], timeout=10, force=False)
        if s.get("display_server_pid"):
            stop_group(s["display_server_pid"])
    os.remove(SESSION_FILE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("launch", help="start OrcaSlicer with the MCP server")
    p.add_argument("--bin")
    p.add_argument("--datadir", help="data directory (default: a fresh temporary one)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    display = p.add_mutually_exclusive_group()
    display.add_argument("--headless", nargs="?", const="auto", choices=("auto", *COMPOSITORS),
                         help="run on a private headless desktop with XWayland (Linux): KWin, else mutter (not tested "
                              "yet), else Xvfb")
    display.add_argument("--xvfb", action="store_true",
                         help="run on a private Xvfb display (Linux), for a machine without a desktop")
    p.add_argument("--width", type=int, default=1600)
    p.add_argument("--height", type=int, default=1000)
    p.add_argument("--printer", help="\"<model>@<nozzle>\" for a fresh datadir, e.g. \"Bambu Lab X1 Carbon@0.4\"")
    p.add_argument("--vendor", default="BBL")
    p.add_argument("--filament")
    p.add_argument("--cad", action="store_true", help="turn the Design (CAD) tab on, for its commands (search_tools group Design)")
    p.add_argument("--unrestricted-scripts", action="store_true",
                   help="let Python scripts (run_script) run unrestricted, which no tool can turn on")
    p.add_argument("--timeout", type=int, default=120)
    p.add_argument("--print-config", action="store_true", help="print client setup commands")
    p.add_argument("extra", nargs="*", help="extra orca-slicer arguments after --")

    sub.add_parser("stop", help="stop the launched instance")

    p = sub.add_parser("call", help="call a tool and print its JSON result")
    p.add_argument("tool")
    p.add_argument("args", nargs="?", default="{}")
    p.add_argument("--wait", action="store_true", help="follow a returned job to its result")

    sub.add_parser("tools", help="list tools")
    p = sub.add_parser("widgets", help="print the list_widgets tree")
    p.add_argument("args", nargs="?", help="list_widgets arguments as JSON")

    p = sub.add_parser("screenshot", help="save a screenshot")
    p.add_argument("out")
    p.add_argument("--target", default="main")

    p = sub.add_parser("run-replay", help="run a replay in the launched instance")
    p.add_argument("replay")
    p.add_argument("--out", help="output directory")

    args = ap.parse_args()
    if args.cmd == "launch":
        return launch(args)
    if args.cmd == "stop":
        return stop(args)
    client, _ = session_client()
    try:
        if args.cmd == "call":
            result, texts = client.call_with_text(args.tool, **json.loads(args.args))
            if args.wait:
                result = client.await_job(result)
            print(json.dumps(result, indent=2))
            for t in texts:
                print(t)
        elif args.cmd == "tools":
            for t in client.tools():
                print(f"{t['name']:20} {t.get('description', '').splitlines()[0][:100]}")
        elif args.cmd == "widgets":
            a = json.loads(args.args) if getattr(args, "args", None) else {}
            data, texts = client.call_with_text("list_widgets", **a)
            print("\n".join(texts) or json.dumps(data, indent=2))
        elif args.cmd == "screenshot":
            out = os.path.abspath(args.out)
            print(json.dumps(client.call("screenshot", target=args.target, save_path=out, overwrite=True, include_image=False), indent=2))
        elif args.cmd == "run-replay":
            a = {"path": os.path.abspath(args.replay)}
            if args.out:
                a["out_dir"] = os.path.abspath(args.out)
            result = client.await_job(client.call("run_replay", **a), timeout=3600)
            print(json.dumps(result, indent=2))
            return 0 if result.get("passed") else 1
    except McpError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
