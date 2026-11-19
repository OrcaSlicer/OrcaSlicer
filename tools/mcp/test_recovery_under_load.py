#!/usr/bin/env python3
"""Checks that a dialog can still be answered when stalled clients hold every connection.

    tools/mcp/orca_mcp.py launch --headless
    tools/mcp/test_recovery_under_load.py

With Preferences open in a running job, 32 `app_wait` calls block on it (every blocking slot) and 32
more connections sit idle or half-sent, which fills the server's 64 connections. A `dialog_answer`
on a new connection must still close the dialog, and the waits must then return. Exits 0 when both
the idle and the half-sent variants pass.

The waits are seen holding every blocking slot before the holders open: app_wait and app_state are
observation calls that each take one of the server's 32 blocking slots, and with none free an
`app_state` replies `pending`. So does a wait that arrives while the test's own `app_state` holds
a slot; such a wait is sent again. An idle holder reads its ping's reply before the next one opens,
so the server has accepted it; the half-sent holders are given 1 s.
"""

import json
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

# The reply an observation call gets when every blocking slot is taken.
PENDING = {"status": "pending", "note": "Too many calls are waiting right now; try again shortly."}
SLOTS_LIMIT_S = 10  # from the start of the 32 waits until app_state finds no slot free


def post(port, token, body, timeout=120):
    """One request on its own connection; returns the parsed JSON-RPC reply."""
    data = json.dumps(body).encode()
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall((f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
               f"Accept: application/json, text/event-stream\r\nAuthorization: Bearer {token}\r\n"
               f"MCP-Protocol-Version: 2025-06-18\r\nContent-Length: {len(data)}\r\n\r\n").encode() + data)
    reply = b""
    while b"\r\n\r\n" not in reply:
        reply += s.recv(65536)
    head, _, rest = reply.partition(b"\r\n\r\n")
    length = int([l.split(b":")[1] for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")][0])
    while len(rest) < length:
        rest += s.recv(65536)
    s.close()
    return json.loads(rest[:length])


def call(port, token, tool, args):
    reply = post(port, token, {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    return json.loads(reply["result"]["content"][0]["text"]), reply["result"].get("isError", False)


def holder(port, token, half_sent):
    """A connection that occupies a place: idle after a ping, or stuck mid-request."""
    s = socket.create_connection(("127.0.0.1", port), timeout=120)
    if half_sent:
        s.sendall(f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: appl".encode())
    else:
        data = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
        s.sendall((f"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
                   f"Authorization: Bearer {token}\r\nMCP-Protocol-Version: 2025-06-18\r\nContent-Length: {len(data)}\r\n\r\n").encode() + data)
        s.recv(65536)
    return s


def wait_for_close(port, token, results):
    """One app_wait for the dialog to close. The test's own app_state, asked meanwhile, can hold the slot a wait
    arrives for, so a wait replied `pending` is sent again, as the note asks, for up to SLOTS_LIMIT_S s."""
    deadline = time.monotonic() + SLOTS_LIMIT_S
    while True:
        reply = call(port, token, "app_wait", {"for": "no_dialog", "timeout_s": 50})[0]
        if {k: reply.get(k) for k in PENDING} != PENDING or time.monotonic() > deadline:
            results.append(reply)
            return
        time.sleep(0.2)


def every_slot_taken(port, token):
    """Whether app_state, on a connection of its own every 0.2 s, replies that no blocking slot is free within
    SLOTS_LIMIT_S s."""
    deadline = time.monotonic() + SLOTS_LIMIT_S
    while time.monotonic() < deadline:
        state, _ = call(port, token, "app_state", {})
        if {k: state.get(k) for k in PENDING} == PENDING:
            return True
        time.sleep(0.2)
    return False


def run(port, token, half_sent):
    job, _ = call(port, token, "action_run", {"id": "orca_command:open_preferences:orca"})
    if job.get("status") != "blocked_by_dialog":
        return f"Preferences did not open: {job}"

    waits, results = [], []
    for _ in range(32):
        t = threading.Thread(target=wait_for_close, args=(port, token, results))
        t.start()
        waits.append(t)
    # A variant whose waits never take every slot still answers the dialog, so that its waits end before the next.
    taken = every_slot_taken(port, token)
    holders = [holder(port, token, half_sent) for _ in range(32)]
    time.sleep(1)

    started = time.time()
    answer, failed = call(port, token, "dialog_answer", {"button": "close"})
    if failed:
        return f"dialog_answer failed: {answer}"
    if answer.get("status") == "running":  # no blocking slot: the job runs on, follow it
        answer, failed = call(port, token, "app_wait", {"for": "job", "job": answer["job"], "timeout_s": 30})
    for t in waits:
        t.join(60)
    for s in holders:
        s.close()
    satisfied = sum(1 for r in results if r.get("satisfied"))
    print(f"  dialog answered in {time.time() - started:.1f} s; {satisfied}/32 waits saw it close")
    if not taken:
        return f"app_state found a blocking slot free for {SLOTS_LIMIT_S} s after the 32 waits started"
    return None if satisfied == 32 else f"only {satisfied} of 32 waits returned satisfied"


def main():
    _, session = orca_mcp.session_client()
    port, token = session["port"], session["token"]
    failures = 0
    for half_sent in (False, True):
        name = "half-sent requests" if half_sent else "idle connections"
        error = run(port, token, half_sent)
        print(("ok   " if error is None else "FAIL ") + f"recover with 32 blocked waits and 32 {name}" + ("" if error is None else f": {error}"))
        failures += error is not None
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
