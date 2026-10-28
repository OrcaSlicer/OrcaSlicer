# MCP harness

`orca_mcp.py` launches OrcaSlicer with its MCP server and talks to it, from a shell or from
Python. It uses only the standard library. `docs/HLSD/mcp-server.md` covers the server, and
`.claude/skills/orca-mcp/SKILL.md` covers the agent workflow.

## Commands

| Command | What it does |
|---|---|
| `launch [--xvfb] [--printer "<model>@<nozzle>"] [--vendor V] [--filament F] [--datadir DIR] [--port N] [--bin PATH] [--print-config] [-- app args]` | Start OrcaSlicer with `--mcp`, wait until it is ready, print the session |
| `call TOOL ['<json args>'] [--wait]` | Call a tool and print its JSON result; `--wait` follows a returned job to its result |
| `tools` | List the tools, with the first line of each description |
| `snapshot ['<json args>']` | Print the `ui_snapshot` tree |
| `screenshot OUT [--target window\|canvas\|dialog]` | Save a full-size PNG, replacing `OUT` if it exists |
| `run-script FILE [--out DIR]` | Run a script in the launched instance and print the result; exit 0 when it passes |
| `stop` | Stop the app and its Xvfb, and remove the session |

`launch` creates a fresh temporary datadir unless you pass `--datadir`. It seeds the datadir with
an `OrcaSlicer.conf` that has a finished setup wizard and one printer and filament (Bambu Lab X1
Carbon 0.4 by default), with update checks and daily tips turned off. The token comes from
`ORCA_MCP_TOKEN` or is generated, and it reaches the app through the environment, not the command
line. On Linux, `launch` also sets `SSL_CERT_FILE`, so a fresh datadir doesn't prompt for a CA
bundle. On Windows, it gives the instance its own WebView2 profile in the datadir, apart from the one
your own OrcaSlicer uses, and finds `build-clang\src\Release\orca-slicer.exe` as well; run the
commands as `python tools/mcp/orca_mcp.py …`. On macOS, it finds the app bundle's binary under
`build/arm64`, `build/x86_64`, `build/universal` or `build`: the bundle the CMake target builds in
`src/Release`, or the one `build_release_macos.sh` assembles in `OrcaSlicer`. The app opens on the
desktop and takes focus while it starts, and needs no CA-bundle setting. `--xvfb` is Linux only. It
starts Xvfb on a free display between `:90`
and `:119`, with `--width`×`--height` (default 1600×1000), and sets `GDK_BACKEND=x11`. Xvfb runs with `-noreset`,
and its pointer is parked in the bottom-right corner. In the middle of the screen, the pointer would
raise tooltips over whatever opens there, and they would show in screenshots. The app's output goes to
`<datadir>/mcp-launch.log`. `--timeout` (default 120 s) limits how long `launch` waits for
`ready`.

## Session file

`launch` writes `{port, token, pid, datadir, xvfb_pid, display}` to `$ORCA_MCP_SESSION`, or to
`~/.cache/orca-mcp/session.json` if that is unset. The file is mode 0600, and every other command
reads it. To run several instances at once, give each its own `ORCA_MCP_SESSION` and `--port`.

## From Python

```python
import os, sys
sys.path.insert(0, "tools/mcp")
from orca_mcp import OrcaMcp, McpError

orca = OrcaMcp(port=13619, token=os.environ["ORCA_MCP_TOKEN"])
orca.call("open_files", paths=["/abs/path/part.stl"])
result = orca.await_job(orca.call("slice"))          # follows a job envelope to the end
orca.call("wait", **{"for": "idle"})                 # "for" is a Python keyword
info, pngs = orca.images("screenshot", target="canvas")
```

`call()` returns the tool's JSON result and raises `McpError` when the tool reports an error.
`call_with_text()` also returns the text blocks after the JSON one, such as `ui_snapshot`'s tree.
`images()` also returns the PNG bytes of the image blocks. `call_raw()` returns the MCP result
unchanged.

## Smoke test

`smoke_test.json` is an end-to-end script that exercises the main paths against a fresh datadir:

- an auto-answered startup or unsaved-changes prompt;
- loading, a setting change, slicing and a G-code export;
- a dialog that an action opens and the script answers;
- screenshots of the dialog and the preview.

```bash
tools/mcp/orca_mcp.py launch --xvfb
tools/mcp/orca_mcp.py run-script tools/mcp/smoke_test.json --out /tmp/orca-smoke
tools/mcp/orca_mcp.py stop
```

Without the harness, `orca-slicer --datadir DIR --mcp-script tools/mcp/smoke_test.json
--mcp-script-out /tmp/orca-smoke` runs the script and exits. On Linux, set `SSL_CERT_FILE` (for
example to `/etc/ssl/certs/ca-certificates.crt`) as the harness does; otherwise a prompt for the CA
bundle blocks startup before the script can run. The exit code is 0 when it passes, 1
when a step fails and 2 when the script is invalid. The run leaves `result.json` and the
screenshots in the output directory. `tests/script_basic.json` is the smallest such check: load,
change a setting, slice and capture.

`tests/gizmos.json` drives every gizmo through the GUI tools, the way a person does: each one's icon,
its panel's fields, combos, toggles and links, and clicks or strokes at points of the model, then
checks what it left on the model (`list_objects`), with an undo and redo and a canvas screenshot per
gizmo. It uses the fixtures beside it (`tests/fixtures/star.svg`, `tests/fixtures/heightmap.png`).
Run it at a second window size through `run_script`'s `vars`:

```bash
tools/mcp/orca_mcp.py call run_script '{"path": "/abs/tools/mcp/tests/gizmos.json", "out_dir": "/tmp/gizmos-small", "vars": {"window_size": [1100, 760]}}' --wait
```

`test_stdio_bridge.py [--bin PATH]` checks `orca-slicer --mcp-stdio` against the launched instance:
the handshake, notifications, tool calls in both protocol eras, and errors.
`test_recovery_under_load.py` fills the server's connections with 32 blocked `wait` calls and 32
idle or half-sent connections while a dialog is open, and checks that `dialog_respond` on a new
connection still closes it. `test_cancel_pending_slice.py` cancels a slice while it still waits for
the update a setting change scheduled, and checks that it never starts. `test_ref_locator_export.py`
uses ImGui refs after their items changed (the Text gizmo's font button, whose label shows the font,
and a Texture Displacement layer row renamed by its place when a layer before it goes) and checks
that the exported session replays.
