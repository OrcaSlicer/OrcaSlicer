#!/usr/bin/env python3
"""Drive OrcaSlicer's MCP server from Python or a shell, for development and tests.

Standard library only. Two halves:

* ``OrcaMcp``: a minimal client for the server's HTTP endpoint (``call(tool, **args)``).
* A command line that launches OrcaSlicer for an agent or a test and talks to it::

    tools/mcp/orca_mcp.py launch --xvfb             # fresh datadir, wait until ready, print client config
    tools/mcp/orca_mcp.py call get_state
    tools/mcp/orca_mcp.py call open_files '{"paths": ["/abs/model.stl"]}'
    tools/mcp/orca_mcp.py screenshot /tmp/shot.png --target canvas
    tools/mcp/orca_mcp.py snapshot
    tools/mcp/orca_mcp.py run-script tools/mcp/smoke_test.json --out /tmp/smoke
    tools/mcp/orca_mcp.py stop

``launch`` records the instance (port, token, pid, datadir) in a session file
(``$ORCA_MCP_SESSION`` or ``~/.cache/orca-mcp/session.json``) that the other commands read.
"""

import argparse
import base64
import json
import os
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


class OrcaMcp:
    def __init__(self, port=DEFAULT_PORT, token="", host="127.0.0.1", timeout=120):
        self.url = f"http://{host}:{port}/mcp"
        self.port = port
        self.token = token
        self.timeout = timeout
        self._id = 0

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

    def call(self, tool, _timeout=None, **args):
        """Call a tool and return its JSON result; raise McpError when the tool reports an error.

        A call that returns a job envelope (running / blocked_by_dialog) is returned as is."""
        result = self.call_raw(tool, args, timeout=_timeout)
        text = next((c["text"] for c in result.get("content", []) if c.get("type") == "text"), "")
        if result.get("isError"):
            raise McpError(text, result)
        try:
            return json.loads(text) if text else {}
        except json.JSONDecodeError:
            return {"text": text}

    def images(self, tool, **args):
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
    candidates += [os.path.join(repo, "build", "src", "Release", "orca-slicer"),
                   os.path.join(repo, "build", "package", "bin", "orca-slicer"),
                   shutil.which("orca-slicer") or ""]
    for c in candidates:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    sys.exit("orca-slicer not found; pass --bin")


def seed_datadir(datadir, printer):
    vendor, model, nozzle, filament = printer
    os.makedirs(datadir, exist_ok=True)
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    if os.path.exists(conf):
        return
    with open(conf, "w") as f:
        json.dump({"firstguide": {"finish": True},
                   "models": [{"model": model, "nozzle_diameter": nozzle, "vendor": vendor}],
                   "filaments": [filament],
                   "presets": {"machine": f"{model} {nozzle} nozzle"},
                   "app": {"check_for_updates": False, "show_daily_tips": False}}, f, indent=2)


def start_xvfb(width, height):
    for display in range(90, 120):
        if not os.path.exists(f"/tmp/.X11-unix/X{display}") and not os.path.exists(f"/tmp/.X{display}-lock"):
            proc = subprocess.Popen(["Xvfb", f":{display}", "-screen", "0", f"{width}x{height}x24", "-nolisten", "tcp"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            time.sleep(0.7)
            return f":{display}", proc.pid
    sys.exit("no free X display for Xvfb")


def launch(args):
    token = os.environ.get("ORCA_MCP_TOKEN") or base64.b16encode(os.urandom(32)).decode().lower()
    datadir = os.path.abspath(args.datadir or tempfile.mkdtemp(prefix="orca-mcp-"))
    printer = DEFAULT_PRINTER
    if args.printer:
        model, _, nozzle = args.printer.partition("@")
        printer = (args.vendor, model, nozzle or "0.4", args.filament or DEFAULT_PRINTER[3])
    seed_datadir(datadir, printer)

    env = dict(os.environ, ORCA_MCP_TOKEN=token)
    xvfb_pid = None
    if args.xvfb:
        display, xvfb_pid = start_xvfb(args.width, args.height)
        env.pop("WAYLAND_DISPLAY", None)
        env.update(DISPLAY=display, GDK_BACKEND="x11")
    if sys.platform.startswith("linux"):
        # A static OpenSSL asks at startup which CA bundle to trust unless told.
        for bundle in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"):
            if os.path.exists(bundle):
                env.setdefault("SSL_CERT_FILE", bundle)
                break
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
    for key in ("pid", "xvfb_pid"):
        pid = s.get(key)
        if pid:
            try:
                os.killpg(pid, signal.SIGTERM)
            except ProcessLookupError:
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
    p.add_argument("--timeout", type=int, default=120)
    p.add_argument("--print-config", action="store_true", help="print client setup commands")
    p.add_argument("extra", nargs="*", help="extra orca-slicer arguments after --")

    sub.add_parser("stop", help="stop the launched instance")

    p = sub.add_parser("call", help="call a tool and print its JSON result")
    p.add_argument("tool")
    p.add_argument("args", nargs="?", default="{}")
    p.add_argument("--wait", action="store_true", help="follow a returned job to its result")

    sub.add_parser("tools", help="list tools")
    sub.add_parser("snapshot", help="print the ui_snapshot tree")

    p = sub.add_parser("screenshot", help="save a screenshot")
    p.add_argument("out")
    p.add_argument("--target", default="window")

    p = sub.add_parser("run-script", help="run an MCP script in the launched instance")
    p.add_argument("script")
    p.add_argument("--out", help="output directory")

    args = ap.parse_args()
    if args.cmd == "launch":
        return launch(args)
    if args.cmd == "stop":
        return stop(args)
    client, _ = session_client()
    try:
        if args.cmd == "call":
            result = client.call(args.tool, **json.loads(args.args))
            if args.wait:
                result = client.await_job(result)
            print(json.dumps(result, indent=2))
        elif args.cmd == "tools":
            for t in client.tools():
                print(f"{t['name']:20} {t.get('description', '').splitlines()[0][:100]}")
        elif args.cmd == "snapshot":
            print(client.call("ui_snapshot").get("tree", ""))
        elif args.cmd == "screenshot":
            out = os.path.abspath(args.out)
            print(json.dumps(client.call("screenshot", target=args.target, save_path=out, overwrite=True, include_image=False), indent=2))
        elif args.cmd == "run-script":
            a = {"path": os.path.abspath(args.script)}
            if args.out:
                a["out_dir"] = os.path.abspath(args.out)
            result = client.await_job(client.call("run_script", **a), timeout=3600)
            print(json.dumps(result, indent=2))
            return 0 if result.get("passed") else 1
    except McpError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
