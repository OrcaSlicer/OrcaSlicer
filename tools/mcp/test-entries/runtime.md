# Runtime, command line and harness

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_command_line`
`test_command_line.py` · Python, 10 checks

**Tests:** OrcaSlicer's own command line. Each case starts the app as the test's child on a fresh data folder (`launch: own`, `port-13694` lock, and the fixed port 13693 for a second window). It covers refused legacy options, `--run`'s exit codes and output folders for a replay and a script, `--run` refused beside a CLI action, `--mcp-port` opening the server, `--unrestricted-scripts` and `--cad` holding for one run only, `--run` beside a window already open, and single-instance hand-over per data folder.

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
`test_stdio_bridge.py` · Python, 19 checks (one printed only when a pid is found)

**Tests:** `orca-slicer --mcp-stdio` in four runs. Against the sweep-launched instance it covers the handshake, a notification, `tools/list`, `tools/call` in the 2025-06-18 and 2026-07-28 protocol eras, and errors; `ORCA_MCP_TOKEN` is removed, so the bridge must take the token from `mcp/server.json`. A second run against the same instance sends a 2026-07-28 request with no handshake at all. Offline (an empty datadir and a free port), the bridge answers from its own catalog without starting the app. On a fresh datadir, a tool call starts OrcaSlicer on the bridge's port.

**Decides by:** Exits 1 when any check prints FAIL. Every line the bridge writes to stdout is kept, in order, as one reply; checks read a reply by its JSON-RPC `id`, and the lines as a list.
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
- The launch section does not wait for ready, by design, since a startup dialog may be up. "the started app stops" prints nothing when the discovery file names no pid, but the port checks fail in that case.
- The bridge waits up to 60 s for the app it starts, and each bridge run is capped at 120 s, so a slow first start fails spuriously.

**Strength:** Partial — every reply line is counted and matched to its request, error codes and the offline catalog are checked exactly, and the no-handshake request is sent with no handshake, but the bridge's exit code and stderr are not read, so a crash after its replies passes.

## `test_orca_mcp_attach`
`test_orca_mcp_attach.py` · Python, 18 checks (assert-based; stops at the first failure)

**Tests:** The harness client in `orca_mcp.py`, without the app (`launch: none`). It covers `default_datadir()` (a portable `data_dir`, the per-OS configuration folder, a symlinked binary), the refusals of `OrcaMcp.from_datadir()`, `call_command()` against a stubbed `call()`, the line `ORCA_MCP_CALL_LOG` gets per call, how `reply_budget_s()` reads its variable, and the conf `seed_datadir()` writes.

**Decides by:** A failed `assert`, or a missing `McpError`, raises and exits non-zero. Each passing group prints "ok - ...".
- "the asserts are live", before any other check: when Python runs with its asserts off (`sys.flags.optimize` above 0, from `-O` or `PYTHONOPTIMIZE`, which the runner passes on from its own environment), the test prints a FAIL line saying so and exits 1.
- `from_datadir()` is tried with no file, a file that is not JSON, one with no port, a stale port, an HTTP 401 and a 200 reply that is not JSON (the last two from a local stub server). Each error names the file and port, gives the "--mcp" advice and chains its cause; the stale port's cause is an `OSError`.
- `call_command()`: a `blocked_by_dialog` reply fails at once after one call, naming the dialog, with the prefix "extrude: ". A plain reply comes back whole from exactly `("tool_call", 600, {...})`, and a running job is followed with `app_wait`. Errors start with the command's name and chain the original.
- Call log: one JSONL line per call with `tool`, `ok` and `ms`. An HTTP 500 adds a second line with `ok: false`.
- `reply_budget_s()`: each of eight values gives exactly 45.0 or 2.0.
- `seed_datadir()`: `use_encrypted_token_file` is true, and `window_mainframe` is "12; 34; 1600; 900; 0" with the origin mocked. A reused conf parses back exactly as it was.

**Not checked:**
- It leaves out the parts of `orca_mcp.py` the sweep's verdicts and setup depend on: `run-replay`'s exit code from `passed`, `launch` (the session file, `wait_ready`, answering the certificate prompt), `stop`, `find_binary`, `app_env`, the headless and Xvfb display start, and `await_job`'s timeout.
- `from_datadir()` is tested only on refusals. The success path, a ping that answers, never runs.
- The symlink check prints "skip" and passes wherever `os.symlink` fails, which is the usual case on Windows without Developer Mode or admin rights.
- `work_area_origin()` is mocked. The real call is checked only for non-negative ints, and it returns (0, 0) off macOS.
- The `reply_budget_s()` table belongs to this test. The app's parser has its own table in `tests/automation/test_mcp_core.cpp`, and nothing checks that the two agree.

**Strength:** Strong — the functions it covers are checked by exact value and error text, and the test refuses to run with its asserts off. It covers the attach-and-call half of `orca_mcp.py`, not launch, stop or run-replay.

## `test_run_sweep`
`test_run_sweep.py` · Python, 14 checks (assert-based)

**Tests:** The building blocks of `run_sweep.py`, without the app (`launch: none`). It covers manifest entries and their validation, test discovery, ordering by history, picking around held locks, the launch, test and setup commands and their environments, the Linux display options, the timeout kill, recording a run that raises, `--history` and `-j`. It also checks that the shipped `sweep.json` lists exactly the tests on disk.

**Decides by:** A failed `assert` raises and exits non-zero.
- "the asserts are live", before any other check: when Python runs with its asserts off (`sys.flags.optimize` above 0, from `-O` or `PYTHONOPTIMIZE`, which the runner passes on from its own environment), the test prints a FAIL line saying so and exits 1.
- Manifest: `entry()` equals the defaults table exactly, and refuses an unknown key, an unknown launch kind, and `exclusive` or `locks` without `why`. `manifest_problems()` names an unlisted test and a missing file, `discover()` refuses two files with one stem, and the shipped manifest has no problems.
- Scheduling: `order()` returns `(["c","b","a"], ["d"])`, and `pick_next()` passes over a test whose lock is held.
- Commands: the argv of `launch_command` is exact, with `--cad` when asked and `--headless` only when the run asks. `launch_env` sets `GDK_BACKEND` to x11, to wayland, or not at all for headless and off Linux. `test_command` is exact for a replay and a script, and `setup_command` fills `{datadir}`. `test_env` sets the session file, `TMPDIR`, `TMP`, `TEMP` and the entry's variables.
- Timeout: `run_step` raises `TimeoutExpired` after 3 s, and the stand-in's child is gone within 10 s.
- Error and options: `guarded_run_test` records `exit: "error"` with a traceback, and `missing()` names a test with no record. History round-trips through a bare file name. `-j 0`, `-2` and `x` exit 2, and the option and session problems hold per platform.

**Not checked:**
- `main()` is never run. That leaves the worker pool, locks held across threads, exclusive tests running after the rest, and the final verdict untested: failing on a non-zero exit, an unrecorded test, a new macOS crash report or an interrupt. Writing history only for passes is untested too.
- `run_test`'s normal path is not run: the `setup_failed`, `launch_failed` and `timeout` labels, the stop after a launch (a failed one included), and the per-test folders. Nor are `Ports.take()`, `crash_reports()`, `--only`, `--skip` and `--list`.
- A replay passes when `orca_mcp.py run-replay` exits 0, which it does only when `passed` is true. That rule lives in `orca_mcp.py`, and neither harness test covers it.
- The timeout check runs only the host's branch: `taskkill /T` on Windows; elsewhere SIGINT, with the stand-in's own `finally` killing its child. The stand-in has 3 s to write its pid file, so on a slow machine a missing file, or an interrupt before its `try`, fails the check spuriously.
- The default `-j` is compared with a copy of the source's own formula.

**Strength:** Strong — the pieces it covers are pinned to exact values, and the test refuses to run with its asserts off. The scheduler and the verdict in `main()` are untested.

## `test_recovery_under_load`
`test_recovery_under_load.py` · Python, 2 checks

**Tests:** Preferences is opened as a running `action_run` job. Then 32 `app_wait {for: no_dialog}` calls take the server's 32 blocking slots, and 32 more connections sit idle (first variant) or half-sent (second variant), filling the 64-connection cap. A `dialog_answer` on a new connection must still close the dialog and release the waits.

**Decides by:** A variant fails when `action_run` does not reply `blocked_by_dialog`, the waits are not seen holding every blocking slot, `dialog_answer` returns `isError`, or fewer than 32 waits return `satisfied: true`. A socket error crashes the script, which also exits non-zero.
- Every blocking slot taken: `app_wait` and `app_state` are observation calls, which run on the request thread, are never jobs, and each take one of the server's 32 blocking slots (`inspect_tool`, `McpCore::run`, `blocking_slots` 32). After the 32 waits start, the test calls `app_state` on a connection of its own every 0.2 s, up to 10 s, until it replies `status: "pending"` with the note "Too many calls are waiting right now; try again shortly.", the reply an observation call gets when no slot is free. A variant whose waits have not taken every slot within the 10 s fails; this read takes the place of a fixed sleep. An `app_state` that finds a slot free holds it while it runs, so a wait that arrives then is itself replied `pending`: each wait is sent again every 0.2 s while it is, for up to 10 s, as the note asks, and one still `pending` after that counts as a wait that did not return `satisfied: true`.
- The holders: an idle holder sends a `ping` and reads its reply before the next one opens, so the server has accepted each of the 32. A half-sent holder sends part of a request's headers; the test gives the 32 of them 1 s.
- "recover with 32 blocked waits and 32 idle connections"
- "recover with 32 blocked waits and 32 half-sent requests"

**Not checked:**
- No tool reports the server's connections, so the half-sent holders are assumed accepted after 1 s. On a slow machine some may not have reached the server, and the half-sent variant then runs below the cap.
- At the cap, the server makes room by evicting the oldest idle connection; failing that, it accepts up to 8 more into an overflow reserve (`McpHttpServer.hpp`). The test cannot tell the two apart, so broken eviction still passes, and whether a holder was evicted is not read.
- When `dialog_answer` replies `running`, the follow-up `app_wait`'s error flag is dropped. That the dialog closed is inferred only from the 32 waits.
- If the server's connection cap grows, the holders no longer fill it and the test still passes. A larger blocking-slot count fails instead: the 32 waits leave a slot free, and `app_state` answers rather than replying `pending`.

**Strength:** Partial — the recovery is checked exactly and the blocked waits are seen holding every slot before the answer, but the connection cap is filled on trust in the half-sent variant.

## `test_worker_admission`
`test_worker_admission.py` · Python, 21 checks (7 per starter)

**Tests:** An arrange holds the UI job worker. It is started three ways: `object_arrange {rotate: true}`, `action_run obj_arrange`, and a click on the Arrange options panel's ImGui button. While it runs, changes and canvas input from MCP are refused and reads answer; `job_cancel` stops it, and a change is admitted afterwards. The bed is filled with copies of three cubes so the arrange takes long.

**Decides by:** Exits 1 when any check prints FAIL. Each starter gets the same seven checks:
- Refusals: "object_transform refused", "keyboard_press Delete refused" and "GL toolbar click refused" each need the call to raise with "Busy: job" in its text.
- "object_list answers": the call succeeds.
- Cancel: "job_cancel stops it" needs `was_running` and `job` to be truthy. "the call reports it cancelled" needs the starter's call to raise with "cancelled" in its text.
- "a change is admitted afterwards": `object_transform` succeeds.

**Not checked:**
- `time.sleep(2.0)` stands in for "the arrange is running". On a slow machine the arrange may not have started (the refusal checks fail); on a fast one it may already be done (the refusals and `job_cancel` fail). Both are spurious failures; neither can make it pass wrongly.
- Setup never checks that the bed was filled, or how many objects there are.
- No call is checked for its effect. Positions are never read, so neither the admitted `object_transform` nor whether the cancelled arrange left the objects in place is verified.
- `object_list` is checked only for not raising.

**Strength:** Strong — every refusal and the cancel are asserted for each starter. Only the effects on object positions go unread.

## `test_cancel_pending_slice`
`test_cancel_pending_slice.py` · Python, 6 checks

**Tests:** A `plate_slice` requested right after a `settings_set` first waits about 0.5 s for the update the change scheduled. A `job_cancel` sent during that wait must keep the slice from starting, and the plate stays unsliced. A slice made afterwards is the control.

**Decides by:** Exits 1 when any check prints FAIL.
- Start: after `project_import` of 3DBenchy and `view_set {tab: "prepare"}`, "the slice starts from Prepare, unsliced" reads `app_state.tab` as `prepare` and `plate_list` plate 0 `sliced: false`.
- "job_cancel reports the waiting slice": `was_running is True`.
- "the slice reports its plate cancelled": `status` `done` and `plates` exactly `[{index: 0, outcome: "cancelled"}]`.
- "the cancel came before the slice started": `app_state.tab` is still `prepare`. A slice that starts shows the Preview tab (`Ops::start_slice` selects it), and the call skips the start when the cancel count changed during its wait or before the start that follows it. A cancel that came after the slice started therefore fails this check instead of passing as an ordinary cancel. A cancel that lands after the wait ends but before the start is honoured the same way, and passes: it too is a cancel of a slice still waiting to start.
- "the plate stays unsliced": `plate_list` gives plate 0 `sliced: false`, and `project_export {format: gcode, path}` is refused with the code `plate_not_sliced` and writes no file at the path.
- Control, "a slice afterwards runs and exports": `plate_slice` gives plate 0 the outcome `ok`, `app_state.tab` is `preview`, `plate_list` gives plate 0 `sliced: true`, and the same `project_export` writes the file (it exists, larger than 0 bytes). This shows that the tab check sees a slice that starts, and that the export refusal was about the slice.

**Not checked:**
- The cancel goes out 0.1 s after the slice request starts on its thread. Too late, the slice starts and "the cancel came before the slice started" fails; too early, before the slice registered, the slice runs to `ok` and "the slice reports its plate cancelled" fails. Either is a spurious failure on a slow or loaded machine; neither passes.
- The scheduled update itself, after the cancel: only that it did not start a slice.

**Strength:** Strong — the cancelled outcome, the waiting case and the unsliced plate are each read exactly, with a control slice that shows the reads can see the opposite.

## `test_export_running_job`
`test_export_running_job.py` · Python, 12 checks

**Tests:** What `replay_export` does with a call whose job outlives its reply. A `replay_run` has one step that waits 3 s past the reply budget. An export taken while it runs leaves it out and names it in `left_out_running`. After it ends, it is exported as an `await: false` step that a later `app_wait` refers to, and that replay runs. A call blocked on Preferences stays in an export while the dialog is open. The sweep sets `ORCA_MCP_REPLY_BUDGET_MS=10000` for both the app and the test.

**Decides by:** Exits 1 when any check prints FAIL.
- While running: `replay_run` replies `status: running`. The export taken then has exactly the steps `["settings_set"]`, and `left_out_running` is exactly `[{tool: replay_run, job}]`.
- After: the job returns `passed: true`. The export's steps are exactly `["replay_run", "settings_set", "app_wait"]`. Step 0 has `await: false` and `save_as: "job1"`, step 2's `job` is `"${jobs.job1}"`, and there is no `left_out_running`.
- Dialog: opening Preferences with `action_run` replies `blocked_by_dialog`. An export with the dialog open has exactly `["action_run"]` and no `left_out_running`.
- Re-run: the exported file replays with `passed: true`.

**Not checked:**
- The exported steps' arguments are not compared: the inline replay and `out_dir` of `replay_run`, and `wall_loops: 3`.
- The docstring's "a job that fails after its reply is dropped" case is left to the C++ unit tests.
- Timing: between the 10 s reply and the job's end there are about 3 s for `settings_set` and the first export. On a loaded machine the job may end first, and the "leaves it out" checks fail spuriously. Preferences must also open within the 10 s budget for "Preferences blocks its call", as the docstring says. The test reads the budget from the same environment the app gets, so an app that ignored the cap would finish the replay before replying, which fails the test rather than passing it.

**Strength:** Strong — every export is compared with an exact list of steps, and the reference fields are checked by value.

## `test_action_replay_hidden`
`test_action_replay_hidden.py` · Python, 14 checks

**Tests:** That the Action Replay window and the Run Script panel stay out of reach of the GUI tools. Neither is listed by `widget_list` or picked by an untargeted `webview_run_javascript` or `keyboard_press`. While they are up, a window screenshot, an agent's `script_run`, the smoke replay and a nested inline replay still work.

**Decides by:** Exits 1 when any check prints FAIL.
- Action Replay is up: "Action Replay opens", within 10 s of `action_run open_action_replay`, `window_list` lists a window titled `Action Replay` with `hidden: true`; a wait that runs out fails it. The test then gives its page 1 s.
- Action Replay: the `widget_list` tree has no line of the role `window` or `dialog` titled `Action Replay`, and, the control, it words the main window as such a line: `window "<title>" (MainFrame)`. `window_screenshot` returns a `path`. Untargeted `webview_run_javascript "1 + 1"` gives the same outcome string as before the window opened; with two web views shown it would get the "Several web views are shown" refusal.
- "a key without a target never goes to Action Replay's web view": with Action Replay raised again on Prepare, `keyboard_press Escape`'s reply has no `page` (a key that reached a page replies with what the page did) and its `to` holds `canvas`: the focus left in a window the tools don't see is no target, so the key goes to the shown 3D view (`UiInspector::key_target`).
- Run Script panel shown: "the Run Script panel shows", within 10 s of the toggle, the 3D view's height in `widget_list` (its `canvas "OrcaSceneCanvas"` line) is less than before the toggle, as the panel docks under it. The test then gives its page 1 s.
- Run Script panel: the number of `webview` words in `widget_list` is unchanged, and untargeted JS on Prepare gives the same outcome as before. `keyboard_press`'s `to` contains "canvas", the screenshot returns a `path`, and `script_run` ends `finished`.
- Replays: `smoke_test.json` and a nested inline `replay_run` both return `passed: true`.

**Not checked:**
- `time.sleep(1.0)` stands in for each page having loaded. The windows are confirmed up and their web views exist with them, so the checks that count web views hold; whether a script could reach a page that has not loaded is not tried.
- The screenshots are checked only for a returned `path`; they are never compared.

**Strength:** Strong — the hiding checks run with each window confirmed up, the window absence has its control in the same tree, and the key's destination is read from the reply's own fields.

## `test_list_windows`
`test_list_windows.py` · Python, 16 checks (4 only when the Plugins window has a ref)

**Tests:** `window_list`. The main window comes first. The Plugins window, which doesn't block the app, is listed with a ref that `window_screenshot`, `widget_list` and `keyboard_press` take. Preferences is listed last as modal and is gone once closed. Action Replay is listed as hidden, with no ref.

**Decides by:** Exits 1 when any check prints FAIL.
- Page: `total == len(items)` and `offset == 0`.
- Main window: the first item has `kind: "main"`, a `ref`, `modal: false`, and width and height above 0.
- Plugins: it is found by title within 15 s, with `modal: false`, a `ref` and `web: true`. A screenshot by its ref has a `path`, a width above 0, and a size different from the main window's. `widget_list` by its ref has "Plugins" on the first line and no `MainFrame`. `keyboard_press Escape` by its ref removes it from the list within 10 s.
- Preferences: it has `modal: true` and a `ref`, is last in the list, and is absent after `dialog_answer close`.
- Action Replay: `hidden: true`, and no `ref` key.

**Not checked:**
- The Plugins screenshot is checked only for a size unlike the main window's. It is not compared with the size `window_list` gives for Plugins, and its pixels are never compared.
- "the modal dialog comes last" runs after Plugins is closed. If the list then holds only the main window and Preferences, "last" only means "not first"; I could not confirm what else the list holds at that point.
- Action Replay is left open at the end, and its other fields are not checked.

**Strength:** Strong — flags, refs, order and removal are checked by exact value. Only the screenshot is checked loosely.

## `test_ref_locator_export`
`test_ref_locator_export.py` · Python, 7 checks (4 only when the snapshot found the item they use)

**Tests:** That an ImGui ref taken from `widget_list` still works after its item's label changes, and that the exported session finds each item and passes. Two refs are tried. The Text gizmo's font button, whose label is the current font, is used for two picks. A Texture Displacement layer row "Layer 2" is clicked after the first layer is removed, when that row is "Layer 1".

**Reads:** A layer row is an item `widget_list` lists named exactly `Layer N` (its thumbnail is `Layer N image`), N its place in the list counted from 1; the open layer has no row. `object_list`'s `painted.texture` gives each layer's `slot`.

**Decides by:** Exits 1 when any check prints FAIL.
- Snapshot: `widget_list` lists a `Combo_Font` item and a `Layer 2` row. Opening the gizmo makes the first layer, slot 0, which the import textures, and `add_texture_layer()` takes the first free slot, so the two added are 1 and 2, slot 2 open; the layers are listed by slot, so the `Remove` of index 0 is slot 0's.
- Font: the first `widget_set_value` returns `selected == "HarmonyOS Sans SC"`. The second, through the same ref, returns a `selected` that starts with the label first shown.
- "the Remove leaves the second layer's row as Layer 1": after the `Remove` of index 0, `object_list` gives the slots `[1, 2]`, and `widget_list` lists a row `Layer 1` and none named `Layer 2` (slot 2 is open).
- "the old ref clicks the renamed row": after `widget_click` on the ref taken for `Layer 2`, `object_list` still gives the slots `[1, 2]` (the click removed nothing), and `widget_list` lists a row `Layer 2` and none named `Layer 1`. The click opened slot 1, the layer the ref was taken for, and slot 2 became the row. A ref that resolved to nothing, to the open layer or to a Remove button fails one of these.
- Replay: the exported session replays with `passed: true`.

**Not checked:**
- The exported replay has no expects, so `passed` means every exported call found a target and returned no error. A locator that resolves to the wrong existing item, such as the row's image button, still passes.
- A live click on the row's image button would also open slot 1 (a retextured layer becomes the active one), with the layer picker open; the picker is not looked for.
- `startswith(shown)` on the second pick is looser than the known label.

**Strength:** Partial — the live ref is read back by which layer it opened, but the exported replay is checked only by passing.

## The example replay

`smoke_test.json` is the example of the replay format, which `test_action_replay_hidden` runs as its input.

### `smoke_test`
`smoke_test.json` · replay, 17 steps, 5 expects, 0 optional

**Tests:** The server's main paths end to end: auto-answer rules, a new project, Benchy imported, a process setting written and read back, a slice, a G-code export, the Preferences dialog opened by an action and answered by the replay, and screenshots of the dialog, the Preview and the main window.

**Decides by:** Fails when a call errors or one of its 6 matchers misses.
- Import: `project_import` `/added` = 1.
- Setting: `settings_get wall_loops` `/items/0/value` = `"3"` after `settings_set`.
- Slice: `plate_slice` `/status` = `done`, `/plates/0/outcome` = `ok`.
- Dialog: `app_wait {for: dialog}` `/satisfied` = true; `widget_list` of the dialog for "splash" `/nodes` ≥ 1; `dialog_answer` must find a dialog titled "Preferences"; the action's job must then end without error.

**Not checked:**
- `project_export`: only that it does not error; the G-code file is never read, so neither its content nor `wall_loops = 3` in it is checked.
- `view_set {tab: preview, camera: iso}` has no expect; tab and camera are not read back.
- The three `window_screenshot` files are saved and never compared.
- The auto-answer rules: nothing shows a prompt was raised or answered.

**Strength:** Smoke — the setting round trip and the slice outcome are checked, but export, view and screenshots only have to not error.
