# MCP harness

`orca_mcp.py` launches OrcaSlicer with its MCP server and talks to it, from a shell or from
Python. It uses only the standard library. `docs/HLSD/mcp-server.md` covers the server, and
`docs/HLSD/mcp-tools.md` its commands.

## Commands

| Command | What it does |
|---|---|
| `launch [--headless [kwin\|mutter] \| --xvfb] [--cad] [--unrestricted-scripts] [--printer "<model>@<nozzle>"] [--vendor V] [--filament F] [--datadir DIR] [--port N] [--bin PATH] [--print-config] [-- app args]` | Start OrcaSlicer with `--mcp`, wait until it is ready, print the session |
| `call TOOL ['<json args>'] [--wait]` | Call a tool and print its JSON result; `--wait` follows a returned job to its result |
| `tools` | List the tools, with the first line of each description |
| `widgets ['<json args>']` | Print the `widget_list` tree |
| `screenshot OUT [--target main\|canvas\|dialog]` | Save a full-size PNG, replacing `OUT` if it exists |
| `run-replay FILE [--out DIR]` | Run a replay in the launched instance and print the result; exit 0 when it passes |
| `stop` | Stop the app and its private display, and remove the session |

`launch` creates a fresh temporary datadir unless you pass `--datadir`. It seeds the datadir with
an `OrcaSlicer.conf` that has a finished setup wizard and one printer and filament (by default the
Custom vendor's Generic Klipper Printer 0.4 nozzle with Generic PLA, so a first start copies no other vendor's
profiles; `--vendor V --printer "<model>@<nozzle>"` seeds another, its preset named as the vendor's profiles name
it, with `--filament` or the model's first default filament), with update checks and daily tips turned off, and with the Orca Cloud login kept
in a token file in the datadir (`use_encrypted_token_file`) instead of the system's secret store, so the
instance starts signed out and never reads your login: on macOS that read would stop a newly built app at
a keychain prompt. A seeded datadir's main window opens 1600×900 at the top left of the screen's work area
(on macOS below the menu bar), so it fits on a 1920×1080 screen, instances started side by side stay on
screen, and the tests' views and screenshots all have one size; the tests that size the window ask for
the same 1600×900. `--cad` starts the app with
`--cad`, which turns the Design (CAD) tab on for that run, and with it its commands, and
`--unrestricted-scripts` starts it with `--unrestricted-scripts`, which lets Python scripts
(`script_run`) run unrestricted for that run. Neither writes the datadir's `OrcaSlicer.conf`, and a later
launch without them has the settings the datadir holds. The token comes from
`ORCA_MCP_TOKEN` or is generated, and it reaches the app through the environment, not the command
line. On Linux, `launch` also sets `SSL_CERT_FILE`, so a fresh datadir doesn't prompt for a CA
bundle, and if that prompt does come up anyway, as when the file it names is gone, `launch` answers it
with Yes, as it waits for the app to be ready: an unattended launch has no person to answer it. It answers No
to the prompt to restore unsaved items, which opens when the last app on the datadir was ended with a project
unsaved, so a launch always starts on an empty project. On
Windows, it gives the instance its own WebView2 profile in the datadir, apart from the one
your own OrcaSlicer uses, and finds `build-clang\src\Release\orca-slicer.exe` as well; run the
commands as `python tools/mcp/orca_mcp.py …`. On macOS, it finds the app bundle's binary under
`build/arm64`, `build/x86_64`, `build/universal` or `build`: the bundle the CMake target builds in
`src/Release`, or the one `build_release_macos.sh` assembles in `OrcaSlicer`. The app opens on the
desktop and takes focus while it starts, and needs no CA-bundle setting.

On Linux, `--headless` and `--xvfb` keep the app off your desktop, on a private screen of
`--width`×`--height` (default 1600×1000), and run it as an X11 client (`GDK_BACKEND=x11`). Use
`--headless` on a machine with a desktop. It starts a headless Wayland compositor on a session bus of
its own and runs the app on that compositor's XWayland. The app draws with the GPU's OpenGL driver,
as on your desktop, and the compositor is a window manager, so focus, stacking and popup lists
behave as they do there; dialogs show in screenshots and recordings, and the private bus keeps the
D-Bus services the app starts, such as the desktop portals, out of your session. It takes KWin
(`kwin_wayland --virtual`), else mutter (`mutter --headless`; test required: not yet run on a GNOME
machine, so check the window, input, screenshots and the OpenGL driver the app's log reports before
relying on it); `--headless kwin` or `--headless mutter` names one. The compositor's output goes to
`<datadir>/mcp-display.log`. When neither is installed or starts, `--headless` says so and falls
back to Xvfb. `--xvfb` is for a machine without a desktop, such as a CI runner or a server. It
starts Xvfb on a free display between `:90` and `:119`, one of its own even when other launches
start at the same moment, as a sweep's jobs do. Xvfb draws OpenGL in software, and runs Openbox
as its window manager when Openbox is installed, as Chromium's tests do under Xvfb. Without a window
manager, a popup list, such as a combo box's drop-down or the Print button's menu, closes as soon as
it opens, and `launch` says so. Xvfb runs with `-noreset`, and its pointer is parked in the bottom-right
corner. In the middle of the screen, the pointer would raise tooltips over whatever opens there, and
they would show in screenshots. The app's output goes to `<datadir>/mcp-launch.log`. `--timeout`
(default 120 s) limits how long `launch` waits for `ready`.

## Session file

`launch` writes `{port, token, pid, datadir, display_server, display_server_pid, display_env,
display}` to `$ORCA_MCP_SESSION`, or to `~/.cache/orca-mcp/session.json` if that is unset.
`display_server` is `kwin`, `mutter`, `xvfb` or null (your desktop), and `display_env` holds the
variables the app was started with to reach it (null for one removed), which `app_env()` applies to
another process. The file is mode 0600, and every other command
reads it. To run several instances at once, give each its own `ORCA_MCP_SESSION` and `--port`.

## From Python

```python
import os, sys
sys.path.insert(0, "tools/mcp")
from orca_mcp import OrcaMcp, McpError

orca = OrcaMcp(port=13619, token=os.environ["ORCA_MCP_TOKEN"])
orca.call("project_import", paths=["/abs/path/part.stl"])
result = orca.await_job(orca.call("plate_slice"))    # follows a job envelope to the end
orca.call("app_wait", **{"for": "idle"})             # "for" is a Python keyword
info, pngs = orca.images("window_screenshot", target="canvas")
```

`call()` returns the tool's JSON result and raises `McpError` when the tool reports an error. The server
rounds a reply's numbers for an agent to read (4 decimals, or 3 significant digits where that keeps more);
`orca.exact_numbers = True` asks every later call for them whole, for a check that computes with them.
`call_with_text()` also returns the text blocks after the JSON one, such as `widget_list`'s tree.
`images()` also returns the PNG bytes of the image blocks. `call_raw()` returns the MCP result
unchanged. With `ORCA_MCP_CALL_LOG` set to a file, every call appends a JSON line with its tool, its
time in milliseconds and whether it succeeded, which shows where a test spends its time.

To drive an OrcaSlicer that is already running with the MCP server on (`--mcp`, or "MCP server"
under AI features in Preferences), attach to it through its datadir instead of a port and token:

```python
orca = OrcaMcp.from_datadir()                        # the datadir the app uses without --datadir
orca = OrcaMcp.from_datadir("/abs/datadir")          # an app started with --datadir /abs/datadir
design = orca.call_command("design_describe")             # a Design command (the Design tab on)
orca.call_command("construct_helix", radius=10, pitch=3, height=30)
```

`from_datadir()` reads the port and token from `<datadir>/mcp/server.json`, which the server writes
when it starts. Without a datadir it takes `default_datadir()`: `%APPDATA%\OrcaSlicer` on Windows,
`~/Library/Application Support/OrcaSlicer` on macOS, `$XDG_CONFIG_HOME/OrcaSlicer` (or
`~/.config/OrcaSlicer`) on Linux. A portable install keeps its datadir in `data_dir` beside the
binary (beside the `.app` on macOS) and is found only when that datadir is given, or the binary as
`from_datadir(exe=...)`; a symlink to the binary is resolved on Linux, as the app resolves its own,
and taken as it is on Windows, as the app takes it. On macOS it is taken as it is, as the stdio
bridge (`McpStdioBridge.cpp`) takes it; whether the app itself resolves a symlinked binary there is
unverified, which matters only for a symlinked binary. When the file is missing, or a ping to the
port it names fails (nothing listens there, or another server answers), `from_datadir()` raises
`McpError` saying so, with the failure as its cause.

`call_command(tool, **args)` calls `tool_call {tool, args}`, follows a running job to its end and
returns the command's reply. A call whose first
reply says a dialog blocks it fails at once, naming the dialog, rather than waiting on it. A call that
first replied `running` and opens a dialog later is waited on up to the timeout (600 s), since `app_wait`
reports only that its job is still running. Every `McpError` it raises starts with the command's name
(`"solid_extrude: ..."`) and has the original error as its cause. `tool_list {group: "Design"}` lists the
Design commands with what each does, and `tool_describe {tool}` gives one's manual, its arguments and examples among it.

## The skill's list of actions

`gen_skill_reference.py <skills-checkout> [--check]` writes
`skills/orcaslicer-mcp-skills/references/actions.md` in a checkout of the orcaslicer-mcp-skills repository:
the commands' Speed Dial actions section by section, from the command reference
`tests/data/command_catalog.json` (`docs/HLSD/mcp-server.md`, "Making a feature reachable"), and before them
what the reference says of particular commands: the Design commands with one form, which needs its value,
and the commands of actions the tool list does not show. Outside the Design tab a section is a Speed Dial
section (an action's `group`, or the `action_group` of a command's own action), and each action has a row
with its key, its title, the command and arguments it runs (`project_export {"format": "gcode"}` for Export
G-code) and its description. In the Design section a command has a row of its own when it has an action of
its own (`has_own_action`), and each Design menu item folded into a command follows it as a row with the
command's key, the item's title and the arguments that open its tool (`sketch_pattern` with
`{"type": "linear"}` for Linear array). It needs no running app. With `--check` it writes
nothing, prints the difference and exits 1 when the file is not what it would write. When a command
changed, regenerate the reference first: run `slic3rutils_tests "[Reference]"` with
`ORCA_UPDATE_COMMAND_REFERENCE=1` in the environment.

## Smoke test

`smoke_test.json` is an end-to-end replay that exercises the main paths, on a fresh datadir or one earlier tests
used, since it selects its own presets:

- an auto-answered startup or unsaved-changes prompt;
- loading, a setting change, slicing and a G-code export;
- a dialog that an action opens and the replay answers;
- screenshots of the dialog and the preview.

```bash
tools/mcp/orca_mcp.py launch --headless
tools/mcp/orca_mcp.py run-replay tools/mcp/smoke_test.json --out /tmp/orca-smoke
tools/mcp/orca_mcp.py stop
```

Without the harness, `orca-slicer --datadir DIR --run tools/mcp/smoke_test.json
--outputdir /tmp/orca-smoke` runs the replay and exits. On Linux, set `SSL_CERT_FILE` (for
example to `/etc/ssl/certs/ca-certificates.crt`) as the harness does; otherwise a prompt for the CA
bundle blocks startup before the replay can run. The exit code is 0 when it passes, 1
when a step fails and 2 when the replay is invalid, cannot be read or is neither a `.json` nor a `.py`
file. The run leaves `result.json` and the screenshots in the `--outputdir` folder, or without it in a
new `orca_action_replay/replay-<timestamp>` folder in the temporary folder (`%TEMP%` on Windows,
`<tmp>/orcaslicer_<uid>` elsewhere). Files that a step or a script saves without a path go to the
`--outputdir` folder too, or without it to `<datadir>/automation/out`. A `--run` launch is never handed to a window that is already open, whatever
the single-instance preference says, and it is refused with an action or `--downward-check`, which start
no GUI.

A `.py` file given to `--run` runs as a Python script (`script_run`) instead, exiting 0 when it
finishes and 1 when it raises, is cancelled or runs out of time; its traceback goes to the log. A
script's own file access is held to OrcaSlicer's folders, and it starts no programs or threads and reaches
no network, unless scripts run unrestricted: start the app with `--unrestricted-scripts` for one run (as
`orca_mcp.py launch --unrestricted-scripts` does), or a person turns the setting on in Preferences. No tool
can turn either on.

A `.py` file runs as Python runs it (`python file.py`), with the whole plugin API: `__name__` is
`"__main__"`, `__file__` and `sys.argv` name it, the modules beside it import (afresh each run, so an edit
shows), and the packages its PEP 723 block lists are installed first. It may open windows and progress
dialogs, which close when the run ends, and its threads and pool tasks call the tools too. The run ends once
its code and the non-daemon threads and pool tasks it started are done:

```python
# /// script
# dependencies = ["numpy"]
# ///
import queue, threading
import orca
import helper                                    # helper.py beside this file

def main():
    progress = orca.host.ui.create_progress_dialog("Measuring", "Starting", 3)
    for step in range(3):
        progress.update(step + 1, helper.describe(step))
    # A window's callbacks run on the UI thread and call no tools; they hand the work to the script.
    pages = queue.Queue()
    window = orca.host.ui.create_window("<button onclick='window.orca.postMessage({go: 1})'>Go</button>",
                                        on_message=pages.put)
    pages.get()                                  # the person pressed Go
    worker = threading.Thread(target=lambda: print(orca.host.tools.call("app_state")["ready"]))
    worker.start()                               # waited for before the run ends

if __name__ == "__main__":
    main()
```

A window that stays open is closed with the run, so a script that wants the person to work in it waits, as
the `pages.get()` above does. While a modal it opened is up, tool calls that change the project are refused,
so interactive scripts use modeless windows. `docs/HLSD/mcp-server.md` ("Python scripts") has the rules for
overlapping runs, imports, pools, `atexit` and Stop.

## Running one test by hand

Each sweep test also runs on its own and prints `ok` or `FAIL` for each check, exiting 1 when one failed. Most
run against an app `orca_mcp.py launch` started, as they run in a job's app in the sweep; `test_orca_mcp_attach.py`
and `test_run_sweep.py` need no app, and the tests `sweep.json` marks `"launch": "own"` start apps of their own.
What each test checks, and how it decides, is its entry under `test-entries/` (`TESTS.md` has the index). Running
one by hand needs what the sweep gives it:

- Launch the app with `--cad` for the tests that use the Design tab, as the sweep launches each job's app: the
  `test_cad_*` tests, `test_call_forms.py`, `test_set_view_camera.py` and `test_speed_dial_open.py`.
- An English UI. The GUI tests find widgets, menu paths and verbs by their English names. The Design tab's GUI
  tests (`test_cad_sketching.py`, `test_cad_context_menu.py` and `test_cad_commands.py`) exit 2 before their first
  check on another language, or with the Design tab off, and say how to fix it.
- Launch the app from a folder that holds no `.py` files: the app's working directory is on Python's module path,
  and a file there can shadow a standard module that `script_run` needs.
- The tests that start apps of their own take the binary as `--bin PATH`: `test_runtime_lifecycle.py`,
  `test_install_printers.py`, `test_install_filaments.py`, `test_plugin_commands.py`, `test_plugin_tools.py`,
  `test_project_open_settings.py` and `test_sidebar_floating.py`. `test_stdio_bridge.py [--bin PATH]` runs against a
  launched app and takes the binary too, for the checks in which its bridge starts an app when none answers.
- Scenarios, rungs or checks named on a test's command line run alone, in that order, where the test takes them
  (`test_gizmos.py`, `test_runtime_lifecycle.py [--bin PATH] [CHECK ...]` and the `test_cad_*` tests; each test's
  docstring lists them). `test_cad_sketching.py selfcheck-recovery` runs only when named: it fails on purpose with
  the mouse button held, the offer asked for and a tool armed, to show that the rungs after a failed one start
  clean.
- `test_cad_failed_job.py` and `test_export_running_job.py` run in the sweep with the reply budget capped,
  `ORCA_MCP_REPLY_BUDGET_MS` 4000 and 20000 (`sweep.json`'s `env`). Give the app and the test the same, or they
  take four to eight minutes and about a minute.
- `test_gui_tools.py` and `test_filaments.py` open popup lists, which close as soon as they open under Xvfb with no
  window manager: on Linux run them with `--headless`, or with `--xvfb` where Openbox is installed.
- `test_recording.py`'s encodes need an ffmpeg with libx265 and libx264 (`ORCA_FFMPEG`, or `ffmpeg` on `PATH`);
  without one, the checks that need it are skipped. Its recordings stay in `--out` (a new temporary folder without
  it): look at the videos, or at the frames `ffmpeg -i session.mkv -vf fps=1 frame%02d.png` extracts, for the
  caption, the pointer and the dialog.
- `test_web_ui.py` needs its fixture plugin side-loaded into the data folder first:

  ```bash
  tools/mcp/test_web_ui.py --install /tmp/orca-mcp-test
  tools/mcp/orca_mcp.py launch --datadir /tmp/orca-mcp-test
  tools/mcp/test_web_ui.py
  ```

- The Design tab tests, for example:

  ```bash
  tools/mcp/orca_mcp.py launch --cad
  tools/mcp/test_cad_sketching.py
  tools/mcp/test_cad_context_menu.py curves label
  tools/mcp/test_cad_commands.py words select
  ```

The app logs each file picker an auto-answer rule answers in `<datadir>/log/debug_*.log.0`, with the reason when
it cancels one a rule gave a path.

## The sweep

`run_sweep.py` runs every sweep test, the `test_*.py` files `sweep.json` lists, on several jobs at once, each job's
tests in one app on one data folder. It exits 0 when all of them pass and no OrcaSlicer crash report appeared
(macOS). How it shares apps and folders, orders the tests, runs a failed test again alone and keeps folders, and the
keys of a `sweep.json` entry, are in `TESTS.md`, "How the sweep decides".

```bash
tools/mcp/run_sweep.py
tools/mcp/run_sweep.py -j 2 --only test_objects test_files
tools/mcp/run_sweep.py --skip test_recording --keep --out /tmp/orca-sweep
tools/mcp/run_sweep.py --list
```

`--only` and `--skip` pick tests by name, and `-j N` (or `--jobs N`) runs N jobs at once, half the cores up to six
by default. `--out` is the folder for the jobs' data folders, each test's run and `results.jsonl`; a passing run's
folder is deleted as soon as it passes, unless `--keep`. `--list` prints the order, the serial phase with why each
test is in it, and exits; it needs no display, and with `--shuffle` it names the seed first.

On Linux the runner opens each app on your desktop as an X11 client (`GDK_BACKEND=x11`), so a sweep
leaves its windows on your screen while it runs, and a window manager un-minimizes a window on request
and reports its position. `--wayland` runs them as native Wayland clients instead, as a Wayland session
does: the compositor then owns positions, minimization and activation, so a check that needs a window
moved, or a minimized one brought back, is reported SKIP and does not fail its test. `--headless` runs
them on a private compositor instead, as `orca_mcp.py launch --headless` does, which is what a machine
without a desktop needs. `--xvfb` runs each job's app on a private Xvfb display of its own, with
Openbox as its window manager, as `orca_mcp.py launch --xvfb` does. OpenGL is drawn in software, so
`--headless` is the better pick wherever KWin is installed; without Openbox, the tests that open a
popup list, `test_gui_tools.py` and `test_filaments.py`, fail under `--xvfb`. Each flag is Linux-only,
and only one of the three can be given. Without any of them the run needs an X display, and `--wayland` needs a Wayland session;
`--list` needs none.

CI runs the sweep on every Build all run (`.github/workflows/mcp_sweep.yml`): it downloads the run's
x86_64 Linux AppImage, extracts it, and runs `run_sweep.py --xvfb --bin squashfs-root/AppRun` with
Openbox and ffmpeg installed. A failed run uploads its `--out` folder as the `mcp-sweep-linux-x86_64`
artifact.
