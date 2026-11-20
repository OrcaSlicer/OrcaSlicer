# Runtime, command line and harness

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_command_line`
`test_command_line.py` · Python, 10 checks

**Tests:** OrcaSlicer's own command line. Each case starts the app as the test's child on a new data folder of its own (`launch: own`, `port-13694` lock, and the fixed port 13693 for a second window). It covers refused legacy options, `--run`'s exit codes and output folders for a replay and a script, `--run` refused beside a CLI action, `--mcp-port` opening the server, `--unrestricted-scripts` and `--cad` holding for one run only, `--run` beside a window already open, and single-instance hand-over per data folder.

**Starts from:** Data folders of its own, a new one per case, never a job's shared folder: `seed_datadir()` with the default printer, the Generic Klipper Printer 0.4 nozzle, with preset sync off and the update check sent to a port nothing answers, then the case's own conf values. No check reads a preset, and nothing here is a Bambu Lab printer's own. Its first start copies the Custom vendor's and the Orca Filament Library's profiles only (a Klipper folder was ready about 8 s after the launch on Windows, measured), well inside the 120 s limits.

**Leaves:** Nothing: each case's folder is deleted when the case ends, retried while WebView2's processes let go of its files.

**Decides by:** Exits 1 when any check prints FAIL. Each check collects problems from exit codes, stderr text, files on disk and a few MCP reads.
- `refused_options`: `--mcp-run`, `--mcp-run-out`, `--mcp-token` and `--no-mcp` each exit non-zero within 10 s, with "Invalid option" on stderr. The data folder holds only `OrcaSlicer.conf`.
- `run_invalid_file`: the exit code is exactly 2 for a missing `.json` and for a `.txt`.
- `run_replay_folders`, `run_script_output_folder`: each run exits 0. `result.json` is in `r.out`, or only in `--outputdir`. Exactly one `.png` lands in `--outputdir` or in `<datadir>/automation/out`, none in the other, and `<datadir>/mcp/out` does not exist.
- `run_refused_without_gui`: the exit code is -2 or 254, stderr has "--run cannot be combined with <option>", and the data folder holds only the conf and `result.json`.
- Serving (`mcp_port_opens_endpoint`, `cad_flag`, the window checks): `app_wait {for: ready}` is satisfied on the expected port, and `mcp/server.json` names this process's pid and that port.
- `unrestricted_scripts_flag`: exit 0 with the flag and exactly 1 without it. The conf's `python_scripts_unrestricted` is absent or false after exit.
- `cad_flag`: `tool_search {group: Design}` returns at least one name with `--cad`. Without it, it returns none, or the refusal "Unknown group: Design;" whose details leave Design out. `app_state` has no `cad_note`. File > Quit of the `--cad` run ends with exit code 0 (a kill or another code is a problem, since the conf is written as the app exits), and only then the conf's `enable_cad_feature` is read: absent or false.
- `run_beside_open_window`: the run beside the first window is given a replay whose one step is `app_state` expecting `/mcp/serving` eq false. The second process exits 0 and writes `result.json` in its `--outputdir`, and that `result.json` has `passed: true`. The first window serves on 13694, so a run that went into it would fail its expect; exit code 0, as opposed to -1 for a hand-over, shows the same from the other side. Then two `--run`s started together both exit 0 and write `result.json`.
- `window_per_data_folder`: a start with a model exits -1 or 255. Off Linux, the first window's `app_state.objects` reaches n within 30 s. Windows also tries the folder in upper case, the exe's path in upper case, the exe by a relative path and the folder's 8.3 name. A window on a second folder serves on 13693.

**Not checked:**
- Outside `cad_flag`, the exit code `App.quit()` returns is dropped, so a crash or hang on File > Quit fails no other check.
- `refused_options` accepts any non-zero code, and `run_invalid_file` any cause of exit 2. `run_refused_without_gui` says the refusal's reason goes to `result.json` but never reads it. `run_replay_folders` checks that `result.json` exists, not what it says.
- `unrestricted_scripts_flag` takes exit 1 without the flag as the refusal. Any other script failure also exits 1, but the same script passed with the flag.
- `run_beside_open_window` reads nothing in the first window after the run; what shows the run stayed out of it is the run's own `result.json` and exit code.
- On Linux, `window_per_data_folder` checks only the exit code, not that the first window loaded the model. The Windows-only spellings never run elsewhere, and the check stops at the first spelling that fails.
- `mcp_port_opens_endpoint` does not check whether `--mcp-port` writes `mcp_enabled` to the conf.
- The limits are fixed wall-clock times: 10 s for a refusal, 120 s for a run or for ready, 45 s to exit, 30 s for a hand-over. A slow first start (profile copy, virus scan) fails spuriously, but none of these limits can turn a failure into a pass.

**Strength:** Strong — exit codes, where files land and conf values are checked exactly, the conf only after a clean exit. The gaps are the Quit outcome outside `cad_flag` and Linux's exit-code-only hand-over.

## `test_runtime_lifecycle`
`test_runtime_lifecycle.py` · Python, 6 checks

**Tests:** How the automation runtime behaves while the app closes and while the server is toggled. File > Quit is clicked with a long `script_run` job, with a recording running, and with a call stuck in one uninterruptible `time.sleep`. Other checks cover a `--run` with the endpoint never opened, switching the server off and on from Preferences during a script, and the GUI instruments going off with the server. `launch: own` with the `port-13694` lock; the two checks whose job outlives its reply set `ORCA_MCP_REPLY_BUDGET_MS=4000` for their app.

**Starts from:** Data folders of its own, a new one per check, never a job's shared folder: `seed_datadir()` with the default printer, the Generic Klipper Printer 0.4 nozzle, with preset sync off and the update check sent to a port nothing answers. No check reads a preset, and nothing here is a Bambu Lab printer's own. The checks that read the log read only this folder's `log`, which no other test writes.

**Leaves:** Nothing: each folder is deleted when its check ends, after the app is closed as a person closes it or, past its exit watchdog, killed.

**Decides by:** Exits 1 when any check prints FAIL.
- `exit_with_job_running`, `exit_with_recording`: after File > Quit the process is gone within 20 s with exit code 0. No line under `<datadir>/log` contains "did not return", and a missing log folder is a failure. `exit_with_job_running` also requires `script_run` to reply `status: running` once the 4 s budget is spent.
- `exit_with_recording`: the script gives no reply within 3 s and the `.mkv` appears within 10 s. After exit the file must parse as finished: a known Segment size that ends at the end of the file, a Duration above 0 and at least one Cluster.
- `exit_without_endpoint`: the process exits 0 within 60 s. `result.json` has `passed: true` for a replay that expects `/mcp/serving` eq false, and the log has no "did not return".
- `server_off_and_on_during_a_call`: the job ends `finished` with the result exactly `[false, true]`. `app_state.mcp.port` is 13694 before, and the same after.
- `exit_with_stuck_call`: exit 0 within 25 s of Quit, which is under the 30 s exit watchdog, and the log has a "did not return" line.
- `instruments_off_with_the_server`: while the server serves, the log has "Automation GUI instruments on" and not "... off". "... off" appears within 30 s of the switch.

**Not checked:**
- `server_off_and_on_during_a_call` reads `mcp.serving` from inside the app, through `orca.host.tools`. It never checks that the port stopped answering while off. The dropped-connection count is printed but not asserted, so a switch that flips the flag but leaves the listener open passes.
- The three "no 'did not return'" checks would still pass if the log wording changed. Only `exit_with_stuck_call`, which requires the line, catches a wording change, and only when it runs in the same pass.
- `exit_with_job_running` reads nothing about what became of the job beyond the exit code and the missing log line.
- `exit_with_recording` checks the container only: not the frames, and not whether Duration matches the time recorded.
- `instruments_off_with_the_server` swallows any error from the script and kills the app, so it says nothing about exit.
- The 20 s and 25 s exit limits and the 10 s wait for the video file are wall-clock, so a loaded machine fails spuriously. On a slow machine, `exit_with_recording`'s "no reply within 3 s" holds trivially, but the file checks after it still catch a recording that never ran.

**Strength:** Strong — exit codes, log lines and the recording's container are checked exactly. The server switch is checked only through the app's own report.

## `test_stdio_bridge`
`test_stdio_bridge.py` · Python, 20 checks (one printed only when a pid is found)

**Tests:** `orca-slicer --mcp-stdio` in four runs. Against the sweep-launched instance, on the job's data folder, it covers the handshake, a notification, `tools/list`, `tools/call` in the 2025-06-18 and 2026-07-28 protocol eras, and errors; `ORCA_MCP_TOKEN` is removed, so the bridge must take the token from `mcp/server.json`. A second run against the same instance sends a 2026-07-28 request with no handshake at all. Offline (an empty datadir and a free port), the bridge answers from its own catalog without starting the app. On a new datadir, a tool call starts OrcaSlicer on the bridge's port.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle, through the harness client, before the bridge runs; no check reads what it sets, and nothing here is a Bambu Lab printer's own. The online checks read the app's readiness, its tool list and the discovery file `<datadir>/mcp/server.json`, which the app writes at each launch. The tool list is the app's own catalog: a plugin an earlier test left in the folder (`test_web_ui`'s fixture) adds actions, not tools, so the offline catalog can equal it. The offline run uses a new empty folder; the launch run a new folder seeded by `seed_datadir()` with the default printer, whose first start copies the Custom vendor's and the Orca Filament Library's profiles only, well inside the bridge's 60 s wait.

**Leaves:** Nothing in the job's folder beyond `start()`'s empty project: the bridge only reads it. The offline and launch folders are deleted, and the app the launch run started is stopped.

**Decides by:** Exits 1 when any check prints FAIL. Every line the bridge writes to stdout is kept, in order, as one reply; checks read a reply by its JSON-RPC `id`, and the lines as a list.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
- Online, the lines in: `initialize` (id 1), the `notifications/initialized` notification, `tools/list` (2), `app_state` (3), a malformed line, a `tools/call` of `app_state` whose `_meta` names 2026-07-28 (4), an unknown tool (5).
  - "the notification gets no reply": the bridge writes exactly six lines, with the ids 1, 2, 3, null, 4 and 5 in that order. A reply to the notification would be a seventh line, or a second with id null.
  - "malformed input is a parse error": the line with id null is the error -32700.
  - `initialize` returns `protocolVersion` "2025-06-18". `tools/list` contains `app_state` and `widget_list`. `app_state` through the bridge has `ready: true`. "a 2026-07-28 request after a 2025-06-18 handshake is served": reply 4 has `resultType: "complete"`. An unknown tool gives -32602.
- Without a handshake: a bridge run whose only line is the 2026-07-28 `tools/call` of `app_state` (id 1). "a 2026-07-28 request is served without a handshake": the bridge writes exactly one line, id 1, with `resultType: "complete"` and content whose `app_state` has `ready: true`.
- Offline, the lines in: `initialize` (1), the notification, `tools/list` (2), `ping` (3), `resources/list` (4), a message with `"jsonrpc": 2` (id 5), `ping` (6).
  - "offline: the notification gets no reply": exactly six lines, with the ids 1, 2, 3, 4, null and 6 in that order.
  - "offline: a jsonrpc that is not a string is an invalid request": the line with id null is the error -32600.
  - `initialize` returns the client's version. `tools/list` is exactly equal to the online list. `ping` returns `{}`. `resources/list` gives -32601. "offline: the bridge answers after a malformed message": the ping after the invalid request returns `{}`. The empty datadir stays empty.
- Launch: `app_state.mcp.port` and the discovery file's `port` both equal the bridge's port, and the started pid is gone within 10 s.

**Not checked:**
- The online `tools/list` check only requires two names. Full equality is checked only offline, against the online list.
- The bridge's exit code and stderr are not read, so a crash after it wrote its replies passes.
- The launch section does not wait for ready itself: the bridge waits for an app it started to report ready before it forwards the first call, which is what the launch checks rely on. "the started app stops" prints nothing when the discovery file names no pid, but the port checks fail in that case.
- The bridge waits up to 60 s for the server of the app it starts and up to 120 s more for it to be ready, and each bridge run is capped at 120 s, so a slow first start fails spuriously.

**Strength:** Partial — every reply line is counted and matched to its request, error codes and the offline catalog are checked exactly, and the no-handshake request is sent with no handshake, but the bridge's exit code and stderr are not read, so a crash after its replies passes.

## `test_orca_mcp_attach`
`test_orca_mcp_attach.py` · Python, 19 checks (assert-based; stops at the first failure)

**Tests:** The harness client in `orca_mcp.py`, without the app (`launch: none`). It covers `default_datadir()` (a portable `data_dir`, the per-OS configuration folder, a symlinked binary), the refusals of `OrcaMcp.from_datadir()`, `call_command()` against a stubbed `call()`, the line `ORCA_MCP_CALL_LOG` gets per call, how `reply_budget_s()` reads its variable, and the conf `seed_datadir()` writes, its default printer included.

**Starts from:** No app and no data folder of a job: scratch folders under a temporary root of its own, and stub servers on loopback ports the system gives out.

**Leaves:** Nothing: the root is deleted at the end.

**Decides by:** A failed `assert`, or a missing `McpError`, raises and exits non-zero. Each passing group prints "ok - ...".
- "the asserts are live", before any other check: when Python runs with its asserts off (`sys.flags.optimize` above 0, from `-O` or `PYTHONOPTIMIZE`, which the runner passes on from its own environment), the test prints a FAIL line saying so and exits 1.
- `from_datadir()` is tried with no file, a file that is not JSON, one with no port, a stale port, an HTTP 401 and a 200 reply that is not JSON (the last two from a local stub server). Each error names the file and port, gives the "--mcp" advice and chains its cause; the stale port's cause is an `OSError`.
- `call_command()`: a `blocked_by_dialog` reply fails at once after one call, naming the dialog, with the prefix "extrude: ". A plain reply comes back whole from exactly `("tool_call", 600, {...})`, and a running job is followed with `app_wait`. Errors start with the command's name and chain the original.
- Call log: one JSONL line per call with `tool`, `ok` and `ms`. An HTTP 500 adds a second line with `ok: false`.
- `reply_budget_s()`: each of eight values gives exactly 45.0 or 2.0.
- `seed_datadir()`: `use_encrypted_token_file` is true, and `window_mainframe` is "12; 34; 1600; 900; 0" with the origin mocked. A reused conf parses back exactly as it was.
- "seed_datadir() seeds the default printer": with `DEFAULT_PRINTER`, `models` is exactly `[{"model": "Generic Klipper Printer", "nozzle_diameter": "0.4", "vendor": "Custom"}]`, `filaments` is `["Generic PLA @System"]`, and `presets.machine` is `MyKlipper 0.4 nozzle`, the printer preset's own name, which is not the model's.

**Not checked:**
- It leaves out the parts of `orca_mcp.py` the sweep's verdicts and setup depend on: `run-replay`'s exit code from `passed`, `launch` (the session file, `wait_ready`, answering the certificate prompt, and `--vendor` with `--printer`, which seeds another printer such as the X1 Carbon), `stop`, `find_binary`, `app_env`, the headless and Xvfb display start, and `await_job`'s timeout.
- `from_datadir()` is tested only on refusals. The success path, a ping that answers, never runs.
- The symlink check prints "skip" and passes wherever `os.symlink` fails, which is the usual case on Windows without Developer Mode or admin rights.
- `work_area_origin()` is mocked. The real call is checked only for non-negative ints, and it returns (0, 0) off macOS.
- The `reply_budget_s()` table belongs to this test. The app's parser has its own table in `tests/automation/test_mcp_core.cpp`, and nothing checks that the two agree.

**Strength:** Strong — the functions it covers are checked by exact value and error text, and the test refuses to run with its asserts off. It covers the attach-and-call half of `orca_mcp.py`, not launch, stop or run-replay.

## `test_run_sweep`
`test_run_sweep.py` · Python, 23 checks (assert-based)

**Tests:** The building blocks of `run_sweep.py`, without the app (`launch: none`), and its whole run, `main()`, on a
stand-in sweep of small scripts. It covers manifest entries and their validation, test discovery, ordering by
history into the pool and the serial phase, picking around held locks, the launch, test and setup commands and their
environments, each job's shared data folder, the Linux display options, the timeout kill, recording a run that
raises, the history file with its serial marks, `--forget`, `--list`, `--keep` and `-j`. It also checks, last, that
the shipped `sweep.json` lists exactly the tests on disk.

**Starts from:** No app. Each check that writes makes its own folder under the temporary folder. The stand-in sweep
runs `main()` with `mcp_dir` set to a folder of its own that holds its `sweep.json` and its scripts, and with
`--out` and `--history` in that folder, so it never reads the person's history file.

**Leaves:** Nothing in the job's data folder; its own folders are under its temporary folder, which the runner
deletes with the test's folder when it passes.

**Decides by:** A failed `assert` raises and exits non-zero.
- "the asserts are live", before any other check: when Python runs with its asserts off (`sys.flags.optimize` above
  0, from `-O` or `PYTHONOPTIMIZE`, which the runner passes on from its own environment), the test prints a FAIL line
  saying so and exits 1.
- Manifest: `entry()` equals the defaults table exactly, and refuses an unknown key, an unknown launch kind, and
  `exclusive` or `locks` without `why`. `discover()` of a folder that holds `test_a.py`, `test_b.py`,
  `smoke_test.json`, `checks.py`, `test_notes.md`, `tests/cad_doc.json` and `tests/test_nested.py` is exactly
  `{test_a: "test_a.py", test_b: "test_b.py"}`: the tests are the `test_*.py` files beside `sweep.json`, and a replay
  is none. `manifest_problems()` gives exactly three problems for an unlisted test, a missing file and an entry whose
  file is `smoke_test.json`, which is not a test. The shipped manifest has no problems; this check runs last, so that
  a `sweep.json` behind the files on disk fails it without stopping the checks of the runner before it.
- Scheduling: with times `a` 10 s, `b` 20 s, none for `c`, `d` exclusive at 5 s and `e` marked serial at 30 s,
  `order()` returns `(["c", "b", "a"], ["e", "d"])`: the pool longest first with a test that has no time ahead, and
  the serial phase longest first. `pick_next()` passes over a test whose lock is held.
- Commands: the argv of `launch_command` is exact, with `--datadir` the job's data folder (`<job>/datadir`), `--cad`
  when asked and `--headless` only when the run asks. `launch_env` sets `GDK_BACKEND` to x11, to wayland, or not at
  all for headless and off Linux. `test_command` is exact for a script with no arguments and for one whose `{bin}` it
  fills, and `setup_command` fills `{datadir}` with the job's data folder. `test_env` sets the test's own session
  file, `TMPDIR`, `TMP`, `TEMP` and the entry's variables. Two `RunContext`s of one job and two tests share `datadir`
  and differ in `session`, `tmp` and `cwd`, each under its own test's folder.
- Timeout: `run_step` raises `TimeoutExpired` after 3 s, and the stand-in's child is gone within 10 s.
- Error: `guarded_run_test` records `exit: "error"` with a traceback, and `missing()` names a test with no record.
- History file: a file of the earlier flat form, `{"a": 1.5}`, reads as times `{a: 1.5}` and no marks. The form
  `{"times": {"a": 1.5}, "serial": {"e": "2026-11-20"}}` reads and writes back unchanged, through a bare file name
  too. A missing file, and one that is not JSON, read as no times and no marks.
- Options: `-j 0`, `-2` and `x` exit 2, `--forget` of a test `sweep.json` does not list exits 2, and the option and
  session problems hold per platform.

**Scenario: a stand-in sweep**

The stand-in folder's `sweep.json` lists four scripts, each `launch: none` with `args: ["{datadir}"]`. Each appends
its name to `visits.txt` in the data folder it is given, and holds a file named for itself in a shared folder while
it runs:

- `test_slow` sleeps 2 s and exits 0.
- `test_load` sleeps 1 s and exits 1 when another stand-in's file is there, 0 when none is: it fails beside another
  test and passes alone.
- `test_broken` writes `log/broken.log` into its data folder and exits 1.
- `test_quick` is `exclusive` and exits 0.

With no history the pool keeps the manifest's order, so `test_slow` and `test_load` start together at `-j 2`.

| # | Step | Exit | Also checked |
|---|---|---|---|
| 1 | `main(["-j", "2", "--keep", ...])` | 1 | `results.jsonl` has 6 records: `test_slow` 0, `test_load` 1 then 0 with `phase: "alone"` on the job it failed on, `test_broken` 1 then 1 alone on its job, `test_quick` 0 with `phase: "serial"` on the serial job. Each record's `after` lists the tests before it on its job, and each job's `visits.txt` lists that job's records in order. The output has a line `test_load` … `passed alone` and a FAIL line for `test_broken`. The history has times for `test_slow`, `test_load` and `test_quick`, none for `test_broken`, and marks only `test_load` serial. With `--keep`, every test's folder and every job's data folder is there |
| 2 | `main(["--list", ...])` | 0 | the pool lists `test_broken`, which has no time, then `test_slow`; the serial phase lists `test_load` with its mark and `test_quick` with its `why` |
| 3 | `main(["-j", "2", ...])`, a new `--out` | 1 | `test_load` runs once, in the serial phase, and passes. The folders of `test_slow`, `test_load` and `test_quick` are gone; `test_broken`'s folder is kept with its `test.log` and a copy of `broken.log`, and so is its job's data folder; the other jobs' data folders are gone. The history keeps `test_load`'s mark, since it passed alone |
| 4 | `main(["--forget", "test_load", ...])` | 0 | the history has no marks and its times are as after 3; nothing runs |
| 5 | `main(["--list", ...])` | 0 | `test_load` is in the pool again |
| 6 | `main(["-j", "2", ...])`, a new `--out` | 1 | `test_load`, after `test_broken` on its job and beside `test_slow`, fails, then passes alone: its records are `pool` 1 and `alone` 0, the folder of its failed run is kept with its `test.log` and that of its passing run is gone, and the history marks it serial again |

**Not checked:**
- The launch path of a real app: launching on a job's data folder, the stop, and the wait for the app's processes
  and its WebView2 ones to be gone before the next test. The stand-ins launch nothing; the full sweep runs that path.
- Locks held across threads in `main()`, and an interrupt (Ctrl-C) during a run.
- The copy of the app's own log: the stand-in writes a file where the app writes its log, so the check covers the
  copy, not that the app's log is where the runner looks.
- The macOS crash-report verdict, and `Ports.take()`.
- The timeout check runs only the host's branch: `taskkill /T` on Windows; elsewhere SIGINT, with the stand-in's own
  `finally` killing its child. The stand-in has 3 s to write its pid file, so on a slow machine a missing file, or an
  interrupt before its `try`, fails the check spuriously. The stand-in sweep's `test_load` relies on the same kind of
  margin: it looks for the others' files 1 s into a 2 s neighbour.
- The default `-j` is compared with a copy of the source's own formula.

**Strength:** Strong — the pieces it covers are pinned to exact values, the whole run is checked on stand-ins down
to each record, folder and history mark, and the test refuses to run with its asserts off. What only a real app
exercises is left to the sweep itself.

## `test_runtime_calls`
`test_runtime_calls.py` · Python, 49 checks (4 only when the snapshot found the item they use)

**Tests:** How the automation runtime takes calls, in five scenarios on one app, run in this order. Each is a `Checks.scenario`: one that raises fails as "<scenario>: runs to its end", and the next still runs.
- Recovery under load: Preferences is opened as a running `action_run` job. Then 32 `app_wait {for: no_dialog}` calls take the server's 32 blocking slots, and 32 more connections sit idle (first variant) or half-sent (second variant), filling the 64-connection cap. A `dialog_answer` on a new connection must still close the dialog and release the waits.
- A slice cancelled while it waits: a `plate_slice` requested right after a `settings_set` first waits about 0.5 s for the update the change scheduled. A `job_cancel` sent during that wait must keep the slice from starting, and the plate stays unsliced. A slice made afterwards is the control.
- The Run Script panel out of reach: the panel is not listed by `widget_list` or picked by an untargeted `webview_run_javascript` or `keyboard_press`. While it is up, a window screenshot and an agent's `script_run` still work. Then `replay_run` runs `smoke_test.json`, the example of the replay format, and a nested inline replay.
- Refs whose items changed: an ImGui ref taken from `widget_list` still works after its item's label changes, and the exported session finds each item and passes. Two refs are tried. The Text gizmo's font button, whose label is the current font, is used for two picks. A Texture Displacement layer row "Layer 2" is clicked after the first layer is removed, when that row is "Layer 1".
- The UI job worker's admission: an arrange holds the UI job worker. It is started three ways: `object_arrange {rotate: true}`, `action_run obj_arrange`, and a click on the Arrange options panel's ImGui button. While it runs, changes and canvas input from MCP are refused and reads answer; `job_cancel` stops it, and a change is admitted afterwards. The bed is filled with copies of three cubes so the arrange takes long.

Recovery runs first, while no call holds a blocking slot or a connection, as at launch: a call an earlier scenario left running, such as an arrange starter's on a failed run, would hold one, and the waits would no longer fill the slots. The admission runs last, so that the arrange settings it stores and the Arrange options panel it opens meet no other scenario.

**Starts from:** Each scenario starts with `start(client)` on the Generic Klipper Printer 0.4 nozzle, and checks it. No check reads a preset beyond that, and nothing here is a Bambu Lab printer's own. Then:
- Recovery under load: Preferences opens and closes alike whatever an earlier test stored, and the server's slots and connections belong to this app alone.
- A slice cancelled while it waits: the printer's process slices the Benchy (measured: the control slice's outcome is `ok`). The setting it changes, `sparse_infill_density`, is an unsaved change of `start()`'s process, so its stored value plays no part. A rule that answers No to "unsaved changes" is set before `start()` and cleared after the cancel.
- The Run Script panel out of reach: on Prepare. The panel is hidden, as at every launch, since no scenario before this one shows it: the app builds the panel only when it is first shown, and applies its place from the saved `window_layout` then. The web views and the untargeted script are compared with this app's own reads before the panel shows, so a web view a plugin an earlier test left adds (`test_web_ui`'s fixture) is on both sides of the comparison. The smoke replay cannot call `start()`, so it sets its start with steps of its own: rules that answer No to "unsaved changes" and to "restore", which end with the replay; `project_new {silent: true}`, which needs the empty project this scenario's `start()` made, since on an app that holds unsaved changes it is refused; and three `preset_select` steps, each with `preset_changes: "discard"`, of the start's printer, process and slot 1 filament.
- Refs whose items changed: on Prepare. Three rules are set first, OK to "Uniform scale" and to "text volume is empty" and No to "unsaved changes", then `replay_export {clear: true}`, so that the exported session starts with `start()`'s own calls and its replay from the same new project. The rules are cleared when the scenario ends: an MCP client's rules last until the server stops. The cube comes from the Add Cube action with its size dialog answered by a rule; its size, which follows the bed or a size a person saved, decides no check. The Text gizmo's font button shows the font of the style it opens with: the gizmo stores its styles on closing without their unsaved changes, so a font a run picked never reaches the conf, and the first pick changes the label (measured: after a run, the stored style's font is the one it had). The Texture Displacement gizmo opens on one layer whatever textures earlier runs imported; its list of recent textures is not read.
- The UI job worker's admission: the Klipper's 250×250 mm bed is what the three cubes' fills cover with copies: measured on it, each starter's arrange still ran past the 2 s the refusals wait. The arrange settings the starters use are set by the test's own calls rather than taken from the folder: the fills pass `spacing: 0` and `rotate: false`, the first starter `rotate: true`, and `object_arrange` stores what it is given, as the panel does, so the action and the panel's button then arrange with rotation on.

**Leaves:** Measured, after one run on a new data folder and a second on the same folder:
- In the conf's `arrange` section, `min_object_distance_fff` 0 and `enable_rotation_fff` true, with `allow_multi_materials_on_same_plate` true written beside them. These are the settings of every later arrange on the job that names neither spacing nor rotation, as a person's last choice in the panel stays; the tests after it set the arrange settings their checks depend on.
- The Run Script panel's place in the conf's `window_layout`, saved with the panel hidden.
- Under `<datadir>/automation/out`, one of each per run: the main window's capture, and the nested replay's `replay-<time>/result.json` (its inner `replay_run` names no `out_dir`).
- The imported height maps copied into `<datadir>/textures/displacement`, one per import: `heightmap.png`, and `heightmap (2).png` from the exported session's replay; a second run adds `heightmap (3).png` and `heightmap (4).png`. The conf's `texture_displacement.recent_textures` names each import, the newest first: `heightmap (2).png/heightmap.png` after one run, `heightmap (4).png/heightmap (3).png/heightmap (2).png/heightmap.png` after two.
- The Text gizmo's styles in the conf (`font`, `font:1` to `font:5`), as it stores them on closing.
- The bed full of cube copies, an unsaved project, which the next `start()` discards. No auto-answer rule is left in force.
- The G-code, the smoke replay's files, and the exported session with its replay's files go to temporary folders of the test's own.

**Decides by:** Exits 1 when any check prints FAIL. Each scenario's first check is its start, "<scenario>: the start is the Klipper printer with one Generic PLA slot", with the scenario named `recovery`, `cancel`, `Run Script panel`, `refs` or `admission`: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.

**Scenario: recovery under load** (runs first)

A variant fails when `action_run` does not reply `blocked_by_dialog`, the waits are not seen holding every blocking slot, `dialog_answer` returns `isError`, or fewer than 32 waits return `satisfied: true`. A socket error ends the scenario, which fails it.
- Every blocking slot taken: `app_wait` and `app_state` are observation calls, which run on the request thread, are never jobs, and each take one of the server's 32 blocking slots (`inspect_tool`, `McpCore::run`, `blocking_slots` 32). After the 32 waits start, the test calls `app_state` on a connection of its own every 0.2 s, up to 10 s, until it replies `status: "pending"` with the note "Too many calls are waiting right now; try again shortly.", the reply an observation call gets when no slot is free. A variant whose waits have not taken every slot within the 10 s fails; this read takes the place of a fixed sleep. An `app_state` that finds a slot free holds it while it runs, so a wait that arrives then is itself replied `pending`: each wait is sent again every 0.2 s while it is, for up to 10 s, as the note asks, and one still `pending` after that counts as a wait that did not return `satisfied: true`.
- The holders: an idle holder sends a `ping` and reads its reply before the next one opens, so the server has accepted each of the 32. A half-sent holder sends part of a request's headers; the test gives the 32 of them 1 s.
- "recover with 32 blocked waits and 32 idle connections"
- "recover with 32 blocked waits and 32 half-sent requests"

**Scenario: a slice cancelled while it waits**

- After `project_import` of 3DBenchy and `view_set {tab: "prepare"}`, "the slice starts from Prepare, unsliced" reads `app_state.tab` as `prepare` and `plate_list` plate 0 `sliced: false`.
- "job_cancel reports the waiting slice": `was_running is True`.
- "the slice reports its plate cancelled": `status` `done` and `plates` exactly `[{index: 0, outcome: "cancelled"}]`.
- "the cancel came before the slice started": `app_state.tab` is still `prepare`. A slice that starts shows the Preview tab (`Ops::start_slice` selects it), and the call skips the start when the cancel count changed during its wait or before the start that follows it. A cancel that came after the slice started therefore fails this check instead of passing as an ordinary cancel. A cancel that lands after the wait ends but before the start is honoured the same way, and passes: it too is a cancel of a slice still waiting to start.
- "the plate stays unsliced": `plate_list` gives plate 0 `sliced: false`, and `project_export {format: gcode, path}` is refused with the code `plate_not_sliced` and writes no file at the path.
- Control, "a slice afterwards runs and exports": `plate_slice` gives plate 0 the outcome `ok`, `app_state.tab` is `preview`, `plate_list` gives plate 0 `sliced: true`, and the same `project_export` writes the file (it exists, larger than 0 bytes). This shows that the tab check sees a slice that starts, and that the export refusal was about the slice.

**Scenario: the Run Script panel out of reach**

- Run Script panel shown: "the Run Script panel shows", within 10 s of the toggle, the 3D view's height in `widget_list` (its `name=OrcaSceneCanvas` line) is less than before the toggle, as the panel docks under it. The test then gives its page 1 s.
- Run Script panel: the number of `webview` words in `widget_list` is unchanged, and untargeted `webview_run_javascript "1 + 1"` on Prepare gives the same outcome string as before the panel showed. `keyboard_press Escape`'s `to` contains "canvas", the screenshot returns a `path`, and `script_run` ends `finished`. The panel is then hidden again.
- Replays: `smoke_test.json` and a nested inline `replay_run` both return `passed: true`. The smoke replay passes when every call returns no error and its matchers hold: `project_import` `/added` = 1; `settings_get wall_loops` `/items/0/value` = `"3"` after `settings_set`; `plate_slice` `/status` = `done` and `/plates/0/outcome` = `ok`; for the Preferences dialog its action opens, `app_wait {for: dialog}` `/satisfied` = true and `widget_list` of the dialog for "splash" `/nodes` ≥ 1, `dialog_answer` must find a dialog titled "Preferences", and the action's job must then end without an error.

**Scenario: refs whose items changed**

A layer row is an item `widget_list` lists named exactly `Layer N` (its thumbnail is `Layer N image`), N its place in the list counted from 1; the open layer has no row. `object_list`'s `painted.texture` gives each layer's `slot`.
- Snapshot: `widget_list` lists a `Combo_Font` item and a `Layer 2` row. Opening the gizmo makes the first layer, slot 0, which the import textures, and `add_texture_layer()` takes the first free slot, so the two added are 1 and 2, slot 2 open; the layers are listed by slot, so the `Remove` of index 0 is slot 0's.
- Font: the first `widget_set_value` returns `selected == "HarmonyOS Sans SC"`. The second, through the same ref, returns a `selected` that starts with the label first shown.
- "the Remove leaves the second layer's row as Layer 1": after the `Remove` of index 0, `object_list` gives the slots `[1, 2]`, and `widget_list` lists a row `Layer 1` and none named `Layer 2` (slot 2 is open).
- "the old ref clicks the renamed row": after `widget_click` on the ref taken for `Layer 2`, `object_list` still gives the slots `[1, 2]` (the click removed nothing), and `widget_list` lists a row `Layer 2` and none named `Layer 1`. The click opened slot 1, the layer the ref was taken for, and slot 2 became the row. A ref that resolved to nothing, to the open layer or to a Remove button fails one of these.
- Replay: the exported session replays with `passed: true`.

**Scenario: the UI job worker's admission** (runs last)

Each starter gets the same seven checks, named after it (`object_arrange`, `action_run`, `widget_click`):
- Refusals: "object_transform refused", "keyboard_press Delete refused" and "GL toolbar click refused" each need the call to raise with "Busy: job" in its text.
- "object_list answers": the call succeeds.
- Cancel: "job_cancel stops it" needs `was_running` and `job` to be truthy. "the call reports it cancelled" needs the starter's call to raise with "cancelled" in its text.
- "a change is admitted afterwards": `object_transform` succeeds.

**Not checked:**
- Recovery under load: no tool reports the server's connections, so the half-sent holders are assumed accepted after 1 s. On a slow machine some may not have reached the server, and the half-sent variant then runs below the cap.
- Recovery under load: at the cap, the server makes room by evicting the oldest idle connection; failing that, it accepts up to 8 more into an overflow reserve (`McpHttpServer.hpp`). The test cannot tell the two apart, so broken eviction still passes, and whether a holder was evicted is not read.
- Recovery under load: when `dialog_answer` replies `running`, the follow-up `app_wait`'s error flag is dropped. That the dialog closed is inferred only from the 32 waits.
- Recovery under load: if the server's connection cap grows, the holders no longer fill it and the test still passes. A larger blocking-slot count fails instead: the 32 waits leave a slot free, and `app_state` answers rather than replying `pending`.
- A slice cancelled while it waits: the cancel goes out 0.1 s after the slice request starts on its thread. Too late, the slice starts and "the cancel came before the slice started" fails; too early, before the slice registered, the slice runs to `ok` and "the slice reports its plate cancelled" fails. Either is a spurious failure on a slow or loaded machine; neither passes.
- A slice cancelled while it waits: the scheduled update itself, after the cancel: only that it did not start a slice.
- The Run Script panel out of reach: `time.sleep(1.0)` stands in for the page having loaded. The panel is confirmed up and its web view exists with it, so the check that counts web views holds; whether a script could reach a page that has not loaded is not tried.
- The Run Script panel out of reach: the screenshot is checked only for a returned `path`; it is never compared.
- The Run Script panel out of reach: the smoke replay's G-code export, its `view_set {tab: preview, camera: iso}` and its three screenshots only have to return no error: the G-code is never read, the tab and camera are not read back, and the files are never compared. The presets its `preset_select` steps select are not read back, and nothing shows that its auto-answer rules answered a prompt; in an app `orca_mcp.py launch` started, neither answers one (measured: their `uses` is 0).
- Refs whose items changed: the exported replay has no expects, so `passed` means every exported call found a target and returned no error. A locator that resolves to the wrong existing item, such as the row's image button, still passes.
- Refs whose items changed: a live click on the row's image button would also open slot 1 (a retextured layer becomes the active one), with the layer picker open; the picker is not looked for.
- Refs whose items changed: `startswith(shown)` on the second pick is looser than the known label.
- The UI job worker's admission: `time.sleep(2.0)` stands in for "the arrange is running". On a slow machine the arrange may not have started (the refusal checks fail); on a fast one it may already be done (the refusals and `job_cancel` fail). Both are spurious failures; neither can make it pass wrongly.
- The UI job worker's admission: setup never checks that the bed was filled, or how many objects there are.
- The UI job worker's admission: no call is checked for its effect. Positions are never read, so neither the admitted `object_transform` nor whether the cancelled arrange left the objects in place is verified. `object_list` is checked only for not raising.

**Strength:** Partial — the cancelled slice, with its control slice, the hiding of the Run Script panel, confirmed up, and every refusal and cancel of the arrange are read exactly, and the live ref is read back by which layer it opened. But the recovery's half-sent variant fills the connection cap on trust, and the exported session and the smoke replay are checked only by passing.

## `test_export_running_job`
`test_export_running_job.py` · Python, 13 checks

**Tests:** What `replay_export` does with a call whose job outlives its reply. A `replay_run` has one step that waits 3 s past the reply budget. An export taken while it runs leaves it out and names it in `left_out_running`. After it ends, it is exported as an `await: false` step that a later `app_wait` refers to, and that replay runs. A call blocked on Preferences stays in an export while the dialog is open. The sweep sets `ORCA_MCP_REPLY_BUDGET_MS=20000` for both the app and the test: on a machine running other apps, Preferences took 12 to 15 s to open (measured), past a 10 s budget.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle; no check reads a preset, and nothing here is a Bambu Lab printer's own. The journal the exports read is this app's, cleared by `replay_export {clear: true}` after the start's own calls.

**Leaves:** Nothing stored: the process's `wall_loops` of 3 is an unsaved change, Preferences is closed without one, and the replays' files go to a temporary folder of the test's own.

**Decides by:** Exits 1 when any check prints FAIL.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
- While running: `replay_run` replies `status: running`. The export taken then has exactly the steps `["settings_set"]`, and `left_out_running` is exactly `[{tool: replay_run, job}]`.
- After: the job returns `passed: true`. The export's steps are exactly `["replay_run", "settings_set", "app_wait"]`. Step 0 has `await: false` and `save_as: "job1"`, step 2's `job` is `"${jobs.job1}"`, and there is no `left_out_running`.
- Dialog: opening Preferences with `action_run` replies `blocked_by_dialog`. An export with the dialog open has exactly `["action_run"]` and no `left_out_running`.
- Re-run: the exported file replays with `passed: true`.

**Not checked:**
- The exported steps' arguments are not compared: the inline replay and `out_dir` of `replay_run`, and `wall_loops: 3`.
- The docstring's "a job that fails after its reply is dropped" case is left to the C++ unit tests.
- Timing: between the 20 s reply and the job's end there are about 3 s for `settings_set` and the first export. On a loaded machine the job may end first, and the "leaves it out" checks fail spuriously. Preferences must also open within the 20 s budget for "Preferences blocks its call", as the docstring says. The test reads the budget from the same environment the app gets, so an app that ignored the cap would finish the replay before replying, which fails the test rather than passing it.

**Strength:** Strong — every export is compared with an exact list of steps, and the reference fields are checked by value.
