#!/usr/bin/env python3
"""printer_status, print_pause, print_resume and print_stop with no printer connected, on the Bambu Lab X1 Carbon 0.4
nozzle, as the test_printer_commands entry of tools/mcp/test-entries/windows_web.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_printer_commands.py

The X1 Carbon, which start() installs when the data folder lacks it, is chosen for what the folder may keep: on the
Klipper printer, a machine of the Orca printer agent an earlier test stored is selected when it is the agent's only
one, and printer_status then reads it connected. A data folder is not cut off from the account it may be signed in to,
so the test reads printer_status first and goes on only when it says no printer is connected: otherwise it fails,
naming the printer it found, and calls nothing else. Then each job command is refused no_printer_connected, and
printer_status still reads no printer and no dialog is open. It sends nothing to any printer, and exits 0 when every
check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import BAMBU_PLA, X1C, X1C_STD, Checks, dialogs, start  # noqa: E402

NO_PRINTER = {"connected": False}


def main():
    client, _session = orca_mcp.session_client()
    t = Checks()
    t.equal("start: the X1 Carbon with its default process and filament", start(client, printer=X1C),
            {"printer": X1C, "process": X1C_STD, "filaments": [BAMBU_PLA]})

    # The guard: no job command is called while a printer is connected.
    status = client.call("printer_status")
    if not t.equal("no printer is connected", status, NO_PRINTER):
        print(f"printer_status found {status.get('name')!r} ({status.get('model')!r}); the test calls nothing else", flush=True)
        return t.exit_code()

    for command in ("print_pause", "print_resume", "print_stop"):
        t.refused(f"{command}: refused", lambda: client.call(command), code="no_printer_connected")
        t.equal(f"{command}: printer_status still reads no printer", client.call("printer_status"), NO_PRINTER)
        t.equal(f"{command}: no dialog", dialogs(client), [])
    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
