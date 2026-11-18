"""What the sweep's Python tests use to apply the read-back rule of tools/mcp/TESTS.md: named checks with the test's
exit code, scenarios that fail on their own, refusals asserted by their code or words, waits that fail when they run
out of time, and readers that give the app's state in a fixed shape."""

import json
import time
import traceback

import orca_mcp


class Checks:
    """Named checks, each printed as `ok <name>` or `FAIL <name>: <detail>`. A failed check lets the test go on."""

    def __init__(self):
        self.count = 0
        self.failed = []

    def check(self, name, ok, detail=""):
        self.count += 1
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"), flush=True)
        if not ok:
            self.failed.append(name)
        return ok

    def equal(self, name, got, want):
        return self.check(name, got == want, f"got {got!r}, expected {want!r}")

    def near(self, name, got, want, tolerance):
        """`got` within `tolerance` of `want`, both numbers or lists of numbers of one length."""
        if isinstance(want, (list, tuple)):
            ok = isinstance(got, (list, tuple)) and len(got) == len(want) and all(
                abs(g - w) <= tolerance for g, w in zip(got, want))
        else:
            ok = isinstance(got, (int, float)) and abs(got - want) <= tolerance
        return self.check(name, ok, f"got {got!r}, expected {want!r} to {tolerance}")

    def refused(self, name, call, code=None, words=None):
        """Checks that `call()` is refused with the code `code` and with text that holds `words`, each when given,
        and returns the refusal's text; a call that goes through fails the check and returns None."""
        try:
            reply = call()
        except orca_mcp.McpError as e:
            text, details = refusal(e)
            problems = []
            if code is not None and details.get("code") != code:
                problems.append(f"code {details.get('code')!r}, expected {code!r}")
            if words is not None and words not in text:
                problems.append(f"the text does not hold {words!r}")
            self.check(name, not problems, "; ".join(problems) + f": {text}")
            return text
        self.check(name, False, f"not refused: {reply}")
        return None

    def scenario(self, name, fn):
        """Runs one scenario. An exception fails it under its name, and the next scenario still runs."""
        print(f"--- {name}", flush=True)
        try:
            fn()
        except Exception as e:  # a scenario that breaks off is one failure; the others still run
            self.check(f"{name}: runs to its end", False, f"{type(e).__name__}: {e}")
            traceback.print_exc()

    def exit_code(self):
        print(f"{self.count - len(self.failed)} of {self.count} checks passed", flush=True)
        return 1 if self.failed else 0


def refusal(error):
    """A tool error's text and the details the server puts on its last line as JSON ({} when it has none)."""
    text = str(error)
    head, _, last = text.rpartition("\n")
    try:
        details = json.loads(last)
    except ValueError:
        return text, {}
    return (head, details) if isinstance(details, dict) else (text, {})


def run(client, tool, timeout_s=600, **args):
    """Calls `tool` and follows a job it replies with to its end. A dialog that holds the call raises at once,
    naming it, since nothing answers a dialog the test does not expect; a test answers the dialogs it expects
    with client.call() and dialog_answer."""
    reply = client.call(tool, **args)
    deadline = time.monotonic() + timeout_s
    while isinstance(reply, dict) and "job" in reply and reply.get("status") in ("running", "blocked_by_dialog"):
        dialogs = reply.get("dialogs") or client.call("app_state").get("dialogs", [])
        if reply.get("status") == "blocked_by_dialog" or dialogs:
            titles = [d.get("title") for d in dialogs]
            raise orca_mcp.McpError(f"{tool} is held by the dialog {titles}: {dialogs}", reply)
        if time.monotonic() > deadline:
            raise TimeoutError(f"{tool} still runs after {timeout_s} s")
        reply = client.call("app_wait", **{"for": "job", "job": reply["job"], "timeout_s": 10})
    return reply


def wait(client, condition, timeout_s=30, job=None):
    """app_wait for `condition` ("idle", "dialog", "no_dialog", "ready" or "job"); raises TimeoutError when the
    wait runs out of time, which app_wait itself reports as an answer."""
    args = {"for": condition, "timeout_s": timeout_s}
    if job is not None:
        args["job"] = job
    reply = client.call("app_wait", **args)
    if condition == "job":
        if reply.get("status") in ("running", "blocked_by_dialog"):
            raise TimeoutError(f"job {job} still runs after {timeout_s} s")
    elif reply.get("satisfied") is not True:
        raise TimeoutError(f"app_wait {condition} did not hold within {timeout_s} s")
    return reply


def plates(client):
    """Each plate as {name, current, locked, objects}, objects being how many stand on it."""
    return [{"name": p["name"], "current": p["current"], "locked": p["locked"], "objects": p["objects"]}
            for p in client.call("plate_list", limit=500)["items"]]


def objects(client):
    """(on_plates, outside): the objects on the plates as (plate, name) pairs in sorted order, and the names of the
    objects outside every plate, sorted."""
    listing = client.call("object_list", limit=500)
    on = sorted((p["index"], o["name"]) for p in listing["plates"] for o in p["objects"])
    return on, sorted(o["name"] for o in listing.get("outside", []))
