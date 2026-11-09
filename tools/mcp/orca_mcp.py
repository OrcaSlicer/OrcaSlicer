#!/usr/bin/env python3
"""Drive OrcaSlicer's MCP server from Python or a shell, for development and tests.

Standard library only. Two halves:

* ``OrcaMcp``: a minimal client for the server's HTTP endpoint (``call(tool, **args)``), which
  ``OrcaMcp.from_datadir()`` attaches to an OrcaSlicer already running with the server on.
* A command line that launches OrcaSlicer for an agent or a test and talks to it::

    tools/mcp/orca_mcp.py launch --xvfb             # fresh datadir, wait until ready, print client config
    tools/mcp/orca_mcp.py launch --cad              # with the Design (CAD) tab on, for the CAD tools (list_cad_tools, call_cad_tool)
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

    def call_cad_tool(self, tool, /, **args):
        """Call one CAD tool of the Design tab through call_cad_tool and return its result.

        A running job is followed to its end. A call whose first reply is blocked_by_dialog fails at
        once, since a script that waited would wait on a dialog nobody answers. A call that first
        replied running and opens a dialog later is waited on up to await_job's timeout, since wait
        reports only that its job is still running. Every McpError's message starts "<tool>: ", with
        the original error as its cause. The tool name is positional-only, so a CAD tool's own
        argument named tool goes to that tool."""
        try:
            reply = self.call("call_cad_tool", _timeout=600, tool=tool, args=args)
            if reply.get("status") == "blocked_by_dialog":
                titles = ", ".join(f"\"{d.get('title', '')}\"" for d in reply.get("dialogs", []))
                raise McpError(f"blocked by a dialog: {titles} (job {reply.get('job')} goes on once it is answered)", reply)
            return self.await_job(reply)["result"]
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


def seed_datadir(datadir, printer, cad=False):
    """Seed a fresh datadir's OrcaSlicer.conf. A datadir that has one keeps it, except that cad turns
    the Design tab on there too, since the app creates the Design tab only at startup, when that
    setting is on."""
    vendor, model, nozzle, filament = printer
    os.makedirs(datadir, exist_ok=True)
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    if os.path.exists(conf):
        if cad:
            with open(conf, encoding="utf-8") as f:
                text = f.read()
            # The app ends the file with a checksum line after the JSON; a mismatch only logs.
            data = json.loads(text[:text.rfind("}") + 1])
            data.setdefault("app", {})["enable_cad_feature"] = True
            write_conf(conf, data)
        return
    app = {"check_for_updates": False, "show_daily_tips": False}
    if cad:
        app["enable_cad_feature"] = True
    write_conf(conf, {"firstguide": {"finish": True},
                      "models": [{"model": model, "nozzle_diameter": nozzle, "vendor": vendor}],
                      "filaments": [filament],
                      "presets": {"machine": f"{model} {nozzle} nozzle"},
                      "app": app})


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


def app_env(env, datadir, xvfb_display=None):
    """Adds to `env` what OrcaSlicer needs to start unattended with `datadir`: the Xvfb display when
    given one, the CA bundle on Linux and a WebView2 profile of its own on Windows."""
    if xvfb_display:
        env.pop("WAYLAND_DISPLAY", None)
        env.update(DISPLAY=xvfb_display, GDK_BACKEND="x11")
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
    if args.xvfb and not sys.platform.startswith("linux"):
        sys.exit("--xvfb runs only on Linux; elsewhere the app opens on the desktop")
    token = os.environ.get("ORCA_MCP_TOKEN") or base64.b16encode(os.urandom(32)).decode().lower()
    datadir = os.path.abspath(args.datadir or tempfile.mkdtemp(prefix="orca-mcp-"))
    printer = DEFAULT_PRINTER
    if args.printer:
        model, _, nozzle = args.printer.partition("@")
        printer = (args.vendor, model, nozzle or "0.4", args.filament or DEFAULT_PRINTER[3])
    seed_datadir(datadir, printer, args.cad)

    env = dict(os.environ, ORCA_MCP_TOKEN=token)
    display = xvfb_pid = None
    if args.xvfb:
        display, xvfb_pid = start_xvfb(args.width, args.height)
    app_env(env, datadir, display)
    cmd = [find_binary(args.bin), "--datadir", datadir, "--mcp", "--mcp-port", str(args.port)] + args.extra
    log = open(os.path.join(datadir, "mcp-launch.log"), "w")
    proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

    session = {"port": args.port, "token": token, "pid": proc.pid, "datadir": datadir,
               "xvfb_pid": xvfb_pid, "display": env.get("DISPLAY")}
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
    kill = os.kill if os.name == "nt" else os.killpg  # Windows has no process groups to signal
    for key in ("pid", "xvfb_pid"):
        pid = s.get(key)
        if pid:
            try:
                kill(pid, signal.SIGTERM)
            except OSError:  # already gone
                pass
    os.remove(SESSION_FILE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("launch", help="start OrcaSlicer with the MCP server")
    p.add_argument("--bin")
    p.add_argument("--datadir", help="data directory (default: a fresh temporary one)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--xvfb", action="store_true", help="run on a private Xvfb display (Linux)")
    p.add_argument("--width", type=int, default=1600)
    p.add_argument("--height", type=int, default=1000)
    p.add_argument("--printer", help="\"<model>@<nozzle>\" for a fresh datadir, e.g. \"Bambu Lab X1 Carbon@0.4\"")
    p.add_argument("--vendor", default="BBL")
    p.add_argument("--filament")
    p.add_argument("--cad", action="store_true", help="turn the Design (CAD) tab on, for the CAD tools (list_cad_tools, call_cad_tool)")
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
    p.add_argument("--target", default="window")

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
