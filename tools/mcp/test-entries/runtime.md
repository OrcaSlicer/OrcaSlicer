# Runtime, command line and harness

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_runtime_lifecycle`
`test_runtime_lifecycle.py` · Python, 6 checks

**Tests:** How the automation runtime behaves while the app closes and while the server is toggled. File > Quit is clicked with a long `script_run` job, with a recording running, and with a call stuck in one uninterruptible `time.sleep`. Other checks cover a `--run` with the endpoint never opened, switching the server off and on from Preferences during a script, and the GUI instruments going off with the server. `launch: own`; the two checks whose job outlives its reply set `ORCA_MCP_REPLY_BUDGET_MS=4000` for their app.

**Starts from:** Data folders of its own, a new one per check, never a job's shared folder: `seed_datadir()` with the default printer, the Generic Klipper Printer 0.4 nozzle, with preset sync off and the update check sent to a port nothing answers. No check reads a preset, and nothing here is a Bambu Lab printer's own. The checks that read the log read only this folder's `log`, which no other test writes.

**Leaves:** Nothing: each folder is deleted when its check ends, after the app is closed as a person closes it or, past its exit watchdog, killed.

**Decides by:** Exits 1 when any check prints FAIL. Each check's app is ready once `app_wait {for: ready}` is satisfied on 13694 and the discovery file names this process, waited for up to 120 s.
- `exit_with_job_running`, `exit_with_recording`: after File > Quit the process exits with code 0, which an exit the app's 30 s exit watchdog forces is not (it ends the process with `EXIT_FAILURE`); one still running 60 s after the click fails as a hang. No line under `<datadir>/log` contains "did not return", and a missing log folder is a failure. `exit_with_job_running` also requires `script_run` to reply `status: running` once the 4 s budget is spent.
- `exit_with_recording`: the script gives no reply within 3 s and the `.mkv` appears within 30 s. After exit the file must parse as finished: a known Segment size that ends at the end of the file, a Duration above 0 and at least one Cluster.
- `exit_without_endpoint`: the process exits 0 within 120 s of its start. `result.json` has `passed: true` for a replay that expects `/mcp/serving` eq false, and the log has no "did not return".
- `server_off_and_on_during_a_call`: the job, followed with `app_wait` across the dropped connections for up to 90 s, ends `finished` with the result exactly `[false, true]`. `app_state.mcp.port` is 13694 before, and the same after.
- `exit_with_stuck_call`: exit 0 after Quit, as above, though its exit holds the runtime's 10 s drain, and the log has a "did not return" line.
- `instruments_off_with_the_server`: while the server serves, the log has "Automation GUI instruments on" and not "... off". "... off" appears within 30 s of the script's call ending.

**Not checked:**
- `server_off_and_on_during_a_call` reads `mcp.serving` from inside the app, through `orca.host.tools`. It never checks that the port stopped answering while off. The dropped-connection count is printed but not asserted, so a switch that flips the flag but leaves the listener open passes.
- The three "no 'did not return'" checks would still pass if the log wording changed. Only `exit_with_stuck_call`, which requires the line, catches a wording change, and only when it runs in the same pass.
- `exit_with_job_running` reads nothing about what became of the job beyond the exit code and the missing log line.
- `exit_with_recording` checks the container only: not the frames, and not whether Duration matches the time recorded.
- `instruments_off_with_the_server` swallows any error from the script and kills the app, so it says nothing about exit.
- How long the app takes to close is bounded only by the 60 s that marks a hang: a close that takes 25 s and ends with code 0 passes, and only the exit code tells a close the watchdog forced. On a slow machine, `exit_with_recording`'s "no reply within 3 s" holds trivially, but the file checks after it still catch a recording that never ran.

**Strength:** Strong — exit codes, log lines and the recording's container are checked exactly. The server switch is checked only through the app's own report.

## `test_stdio_bridge`
`test_stdio_bridge.py` · Python, 20 checks (one printed only when a pid is found)

**Tests:** `orca-slicer --mcp-stdio` in four runs. Against the sweep-launched instance, on the job's data folder, it covers the handshake, a notification, `tools/list`, `tools/call` in the 2025-06-18 and 2026-07-28 protocol eras, and errors; `ORCA_MCP_TOKEN` is removed, so the bridge must take the token from `mcp/server.json`. A second run against the same instance sends a 2026-07-28 request with no handshake at all. Offline (an empty datadir and a free port), the bridge answers from its own catalog without starting the app. On a new datadir, a tool call starts OrcaSlicer on the bridge's port, and that app lists the same tools as the offline bridge.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle, through the harness client, before the bridge runs; no check reads what it sets, and nothing here is a Bambu Lab printer's own. The online checks read the app's readiness, its tool list and the discovery file `<datadir>/mcp/server.json`, which the app writes at each launch. The job's app runs with the CAD feature on (`--cad`), which adds the Design commands' index to `tool_list`'s description, so the offline catalog, built without it, is compared with the app the launch run starts, which runs without it too, rather than with the job's app. A plugin an earlier test left in a folder (`test_web_ui`'s fixture) adds actions, not tools. The offline run uses a new empty folder; the launch run a new folder seeded by `seed_datadir()` with the default printer, whose first start copies the Custom vendor's and the Orca Filament Library's profiles only, well inside the bridge's 60 s wait.

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
  - `initialize` returns the client's version. `ping` returns `{}`. `resources/list` gives -32601. "offline: the bridge answers after a malformed message": the ping after the invalid request returns `{}`. The empty datadir stays empty.
- Launch, the lines in: `initialize` (1), a `tools/call` of `app_state` (2), which starts the app, and `tools/list` (3), which the started app answers. `app_state.mcp.port` and the discovery file's `port` both equal the bridge's port. "offline: tools/list matches that of the app the bridge starts": the offline `tools/list` is exactly equal to reply 3's. The started pid is gone within 30 s of its stop.

**Not checked:**
- The online `tools/list` check only requires two names: the job's app, with the CAD feature on, is not compared whole with any other list. Full equality is checked only between the offline bridge and the app the launch run starts.
- The bridge's exit code and stderr are not read, so a crash after it wrote its replies passes.
- The launch section does not wait for ready itself: the bridge waits for an app it started to report ready before it forwards the first call, which is what the launch checks rely on. "the started app stops" prints nothing when the discovery file names no pid, but the port checks fail in that case.
- The bridge waits up to 60 s for the server of the app it starts and up to 120 s more for it to be ready, and each bridge run is capped at 120 s, so a slow first start fails spuriously.

**Strength:** Partial — every reply line is counted and matched to its request, error codes and the offline catalog are checked exactly, and the no-handshake request is sent with no handshake, but the bridge's exit code and stderr are not read, so a crash after its replies passes.

## `test_orca_mcp_attach`
`test_orca_mcp_attach.py` · Python, 21 checks (assert-based; stops at the first failure)

**Tests:** The harness client in `orca_mcp.py`, without the app (`launch: none`). It covers `default_datadir()` (a portable `data_dir`, the per-OS configuration folder, a symlinked binary), the refusals of `OrcaMcp.from_datadir()`, `call_command()` against a stubbed `call()`, the line `ORCA_MCP_CALL_LOG` gets per call, how `reply_budget_s()` reads its variable, the conf `seed_datadir()` writes, its default printer included, and whether it says it seeded; and the baseline file `launch` records in a data folder it seeds, which `checks.start()` resets the preferences to.

**Starts from:** No app and no data folder of a job: scratch folders under a temporary root of its own, and stub servers on loopback ports the system gives out.

**Leaves:** Nothing: the root is deleted at the end.

**Decides by:** A failed `assert`, or a missing `McpError`, raises and exits non-zero. Each passing group prints "ok - ...".
- "the asserts are live", before any other check: when Python runs with its asserts off (`sys.flags.optimize` above 0, from `-O` or `PYTHONOPTIMIZE`, which the runner passes on from its own environment), the test prints a FAIL line saying so and exits 1.
- `from_datadir()` is tried with no file, a file that is not JSON, one with no port, a stale port, an HTTP 401 and a 200 reply that is not JSON (the last two from a local stub server). Each error names the file and port, gives the "--mcp" advice and chains its cause; the stale port's cause is an `OSError`.
- `call_command()`: a `blocked_by_dialog` reply fails at once after one call, naming the dialog, with the prefix "extrude: ". A plain reply comes back whole from exactly `("tool_call", 600, {...})`, and a running job is followed with `app_wait`. Errors start with the command's name and chain the original.
- Call log: one JSONL line per call with `tool`, `ok` and `ms`. An HTTP 500 adds a second line with `ok: false`.
- `reply_budget_s()`: each of eight values gives exactly 45.0 or 2.0.
- `seed_datadir()`: `use_encrypted_token_file` is true, and `window_mainframe` is "12; 34; 1600; 900; 0" with the origin mocked. A reused conf parses back exactly as it was. It returns exactly `True` for the new folder and exactly `False` for the reused one.
- "seed_datadir() seeds the default printer": with `DEFAULT_PRINTER`, `models` is exactly `[{"model": "Generic Klipper Printer", "nozzle_diameter": "0.4", "vendor": "Custom"}]`, `filaments` is `["Generic PLA @System"]`, and `presets.machine` is `MyKlipper 0.4 nozzle`, the printer preset's own name, which is not the model's.
- The baseline: `launch` runs with `find_binary`, the display start, `subprocess.Popen`, `wait_ready` and `OrcaMcp.call` stubbed, the call answering `app_state` with no dialogs and `preferences_get` with three items, one of them marked refused. On a new folder, `mcp-baseline.json` is exactly `{"preferences": {...}}` with all three keys and their values, and `preferences_get` was called once; on a folder that has a conf, there is no `mcp-baseline.json` and `preferences_get` was not called.

**Not checked:**
- It leaves out the parts of `orca_mcp.py` the sweep's verdicts and setup depend on: `run-replay`'s exit code from `passed`, `launch` beyond the baseline it records (the session file, `wait_ready`, answering the certificate prompt, and `--vendor` with `--printer`, which seeds another printer such as the X1 Carbon), `stop` and its wait for the app to exit, `find_binary`, `app_env`, the headless and Xvfb display start, and `await_job`'s timeout.
- The baseline's values come from the stub, so whether the app's `preferences_get` lists every preference `start()` resets is the app's, not checked here.
- `from_datadir()` is tested only on refusals. The success path, a ping that answers, never runs.
- The symlink check prints "skip" and passes wherever `os.symlink` fails, which is the usual case on Windows without Developer Mode or admin rights.
- `work_area_origin()` is mocked. The real call is checked only for non-negative ints, and it returns (0, 0) off macOS.
- The `reply_budget_s()` table belongs to this test. The app's parser has its own table in `tests/automation/test_mcp_core.cpp`, and nothing checks that the two agree.

**Strength:** Strong — the functions it covers are checked by exact value and error text, and the test refuses to run with its asserts off. It covers the attach-and-call half of `orca_mcp.py` and the seeding of a data folder, not the rest of launch, stop or run-replay.

## `test_run_sweep`
`test_run_sweep.py` · Python, 25 checks (assert-based; 1 only on Linux with Xvfb installed)

**Tests:** The building blocks of `run_sweep.py`, without the app (`launch: none`), and its whole run, `main()`, on
two stand-in sweeps: one of small scripts that launch nothing, and one whose scripts run in a stand-in app that a
stand-in `orca_mcp.py` launches and stops. It covers manifest entries and their launch kinds, test discovery,
ordering by history or by a `--shuffle` seed into the pool and the serial phase, picking around held locks, the
launch, stop, test and setup commands and their environments, each job's shared data folder, its app's session
file and the working folder its app runs in, the Linux display options and the private display of a test that
starts its own apps, the timeout kill, recording a run that raises, the history file, `--list`, `--keep`, `--shuffle`
and `-j`, and when the runner launches a job's app and stops it. It also checks, last, that the shipped `sweep.json`
lists exactly the tests on disk.

**Starts from:** No app. Each check that writes makes its own folder under the temporary folder. Each stand-in sweep
runs `main()` with `mcp_dir` set to a folder of its own that holds its `sweep.json` and its scripts, and with
`--out` and `--history` in that folder, so it never reads the person's history file. The stand-in app sweep's
`--bin` is the Python that runs the test, and the longest `--out` the runner takes on Windows is set to the
length of its `--out` while it runs, since the stand-in app copies no profiles.

**Leaves:** Nothing in the job's data folder; its own folders are under its temporary folder, which the runner
deletes with the test's folder when it passes.

**Decides by:** A failed `assert` raises and exits non-zero.
- "the asserts are live", before any other check: when Python runs with its asserts off (`sys.flags.optimize` above
  0, from `-O` or `PYTHONOPTIMIZE`, which the runner passes on from its own environment), the test prints a FAIL line
  saying so and exits 1.
- Manifest: `entry()` equals the defaults table exactly, takes each of the launch kinds `app`, `fresh`, `own` and
  `none`, and refuses an unknown key (`lanch`, and `cad` whether true or false), an unknown launch kind, and
  `exclusive` or `locks` without `why`. `discover()` of a folder that holds `test_a.py`, `test_b.py`,
  `smoke_test.json`, `checks.py`, `test_notes.md`, `tests/cad_doc.json` and `tests/test_nested.py` is exactly
  `{test_a: "test_a.py", test_b: "test_b.py"}`: the tests are the `test_*.py` files beside `sweep.json`, and a replay
  is none. `manifest_problems()` gives exactly three problems for an unlisted test, a missing file and an entry whose
  file is `smoke_test.json`, which is not a test. The shipped manifest has no problems; this check runs last, so that
  a `sweep.json` behind the files on disk fails it without stopping the checks of the runner before it.
- Scheduling: with times `a` 10 s, `b` 20 s, none for `c`, and `d` at 5 s and `e` at 30 s both exclusive,
  `order()` returns `(["c", "b", "a"], ["e", "d"])`: the pool longest first with a test that has no time ahead, and
  the serial phase longest first. With a seed, 20 tests, one of them exclusive, and no history: the seed 123456 gives
  the same order whether the names come in order or reversed, the exclusive test alone in the serial phase and the
  other 19 in the pool, not in name order; the seeds 1 to 5 give more than one order. `pick_next()` passes over a
  test whose lock is held.
- Commands: the argv of `launch_command` is exact, with `--datadir` the job's data folder (`<job>/datadir`),
  `--cad --unrestricted-scripts` on every platform, the same with `--wayland`, and `--headless` or `--xvfb` added
  only when the run asks. `stop_command` is exactly `orca_mcp.py stop`. `launch_env` sets `GDK_BACKEND` to x11, to
  wayland, or not at all with `--headless` or `--xvfb` and off Linux. `test_command` is exact for a script with no
  arguments and for one whose `{bin}` it fills; `setup_command` is None for an entry without a setup and fills
  `{datadir}` with the job's data folder. `test_env` gives a test of kind `app` or `fresh` the job's session file,
  `<job>/session.json`, and one of kind `own` or `none` a session file in its own folder, and sets `TMPDIR`, `TMP`
  and `TEMP` to its temporary folder and the entry's variables. Two `RunContext`s of one job and two tests share
  `datadir`, `session` and `app_cwd`, the working folder of the job's app, which is `<job>/app-cwd` and under neither
  test's folder; they differ in `tmp` and `cwd`, each under its own test's folder.
- Own display: `own_display` yields None for a test of kind `app`, and for one of kind `own` in a run with neither
  `--headless` nor `--xvfb`. On Linux with Xvfb installed, `run_test` of a stand-in script of kind `own`, in a run
  with `--xvfb`, exits 0, and the script, which writes its environment to a file, saw `DISPLAY` a display from `:90`
  to `:119` whose socket was there, `GDK_BACKEND` x11 and no `WAYLAND_DISPLAY`; the test's log names that display,
  and once `run_test` returns its socket is gone. Elsewhere this part prints a `SKIP` line saying why.
- Timeout: `run_step` raises `TimeoutExpired` after 3 s, and the stand-in's child is gone within 10 s.
- Error: `guarded_run_test` records `exit: "error"` with a traceback, and `missing()` names a test with no record.
- History file: a file of the earlier flat form, `{"a": 1.5}`, reads as times `{a: 1.5}`. The times `{a: 1.5}`
  write as `{"times": {"a": 1.5}}` and read back unchanged, through a bare file name too. A missing file, and one
  that is not JSON, read as no times.
- Options: `-j 3` and `--jobs 2` set the jobs, and the default is the source's formula; `-j 0`, `-2` and `x` exit 2.
  `--shuffle` is None when not given, 123456 when given that, and a whole number it picks when given alone.
  `option_problems` takes none or one of `--headless`, `--xvfb` and `--wayland` on Linux, and refuses each of them off
  Linux and every pair of them on Linux. `session_problems` refuses a run on the desktop without a `DISPLAY` and a
  `--wayland` one without a `WAYLAND_DISPLAY`, and asks neither of a `--headless` or `--xvfb` run or of one off
  Linux.

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
| 1 | `main(["-j", "2", "--keep", ...])` | 1 | `results.jsonl` has 6 records: `test_slow` 0, `test_load` 1 then 0 with `phase: "alone"` on the job it failed on, `test_broken` 1 then 1 alone on its job, `test_quick` 0 with `phase: "serial"` on the serial job. Each record's `after` lists the tests before it on its job, and each job's `visits.txt` lists that job's records in order. The output has a line `test_load` … `passed alone` and a FAIL line for `test_broken`. The history has times for `test_slow`, `test_load` and `test_quick`, none for `test_broken`. With `--keep`, every test's folder and every job's data folder is there |
| 2 | `main(["--list", ...])` | 0 | the pool lists `test_broken`, which has no time, then `test_slow` and `test_load`: a test that passed only alone runs among the others again, with no note on its line; the serial phase lists `test_quick` with its `why` |
| 3 | `main(["--list", "--shuffle", "7", ...])`, twice, then `main(["--list", "--shuffle", ...])` | 0 | the first line is `shuffled with seed 7`, the next three are `test_broken`, `test_slow` and `test_load` in any order, then `test_quick`; the second run prints the same; without a seed the first line names the seed picked |
| 4 | `main(["-j", "2", ...])`, a new `--out` | 1 | `test_load`, after `test_broken` on its job and beside `test_slow`, fails, then passes alone: its records are `pool` 1 and `alone` 0, the folder of its failed run is kept with its `test.log`, and that of its passing run is gone. The folders of `test_slow` and `test_quick` are gone; `test_broken`'s folder is kept with its `test.log` and a copy of `broken.log`, and so is its job's data folder; the other jobs' data folders are gone |

**Scenario: a stand-in app sweep**

The stand-in folder holds an `orca_mcp.py` of its own. Its `launch` starts a stand-in app, an HTTP server that
answers every call with an empty result and has the data folder on its command line, as the app has, writes the
session file the runner names, and notes `launch <pid> <flags> budget=<ORCA_MCP_REPLY_BUDGET_MS> cwd=<folder>` in
`apps.txt` in the data folder, where `<folder>` is the launch's working folder, which the stand-in app keeps as the
app launched with `--datadir` does; its `stop` ends the app and notes `stop <pid>`. `sweep.json` lists twelve
scripts, each with `args: ["{datadir}"]`, in this order: `test_first`; `test_own`, `launch: own`; `test_second`;
`test_fresh`, `launch: fresh`; `test_hang`, which sleeps past its `timeout_s` of 3; `test_after`; `test_kill`, which
ends the app and waits until it no longer answers; `test_revived`; `test_budget`, with `ORCA_MCP_REPLY_BUDGET_MS`
4000 in its `env`; `test_setup`, with a setup step; `test_fails`, which exits 1; and `test_serial`, `exclusive`.
Each notes in `visits.txt` its name, the pid of the app its session file names (or `-` when there is none) and that
session file; its setup run notes `<name>-setup`.

`main(["-j", "1", "--keep", "--bin", <python>, ...])` exits 1, and:
- Job 1's `apps.txt` is eight launches, each followed by the stop of the same pid, eight different apps A to H. The
  budget is `-` for the first four, 4000 for the fifth and `-` for the last three, and every launch has
  `--cad --unrestricted-scripts` first. Every launch's working folder is job 1's `app-cwd`, compared with both
  paths resolved, since the temporary folder can be reached through a link (`/var` on macOS): the app never runs in
  a test's folder, which the runner deletes when that test passes.
- Job 1's `visits.txt` is exactly: `test_first` A, `test_own` with no app and a session file in its own folder,
  `test_second` A, `test_fresh` B, `test_hang` B, `test_after` C, `test_kill` C, `test_revived` D, `test_budget` E,
  `test_setup-setup` with no app, `test_setup` F, `test_fails` F, and then the runs alone, `test_hang` G and
  `test_fails` H; every other line names the job's session file.
- The records with a `launch_seconds` are exactly those of `test_first`, `test_fresh`, `test_after`, `test_revived`,
  `test_budget` and `test_setup` in the pool, `test_hang` and `test_fails` alone and `test_serial`. The records that
  did not exit 0 are exactly `test_hang`, `timeout` in the pool and alone, and `test_fails`, 1 in the pool and alone.
- The serial job's `apps.txt` is one launch, in the serial job's `app-cwd`, and the stop of its pid, and its
  `visits.txt` is `test_serial` with that pid and the serial job's session file. Neither job's session file is
  left, and no app the sweep launched is still running.

**Not checked:**
- A real app's launch and stop: `orca_mcp.py launch` seeding the data folder, recording its baseline and waiting for
  ready, and the WebView2 processes the runner waits for after the stop. The stand-in app has the data folder on its
  command line, so the wait for the app's processes runs, on it alone; the full sweep runs the rest.
- `app_answers()` meets an app that answers and one that is gone, never one that hangs.
- What an app does once its working folder is deleted: the check pins the folder the runner launches the app in,
  not the app's calls that read it. The stand-in sweep with `--keep` deletes no test's folder, so it could not
  show that failure; the full sweep, whose passing tests' folders go, does.
- Locks held across threads in `main()`, and an interrupt (Ctrl-C) during a run.
- The private display of a test marked `own` with `--headless`, which starts a compositor, and the run recorded as
  an error when its display does not start.
- The copy of the app's own log: the stand-in writes a file where the app writes its log, so the check covers the
  copy, not that the app's log is where the runner looks.
- The macOS crash-report verdict, and `Ports.take()`.
- A `--shuffle` order is checked only as the same for one seed and different across seeds, not as any given order.
- The timeout check runs only the host's branch: `taskkill /T` on Windows; elsewhere SIGINT, with the stand-in's own
  `finally` killing its child. The stand-in has 3 s to write its pid file, so on a slow machine a missing file, or an
  interrupt before its `try`, fails the check spuriously. The stand-in sweep's `test_load` relies on the same kind of
  margin: it looks for the others' files 1 s into a 2 s neighbour.
- The default `-j` is compared with a copy of the source's own formula.

**Strength:** Strong — the pieces it covers are pinned to exact values, the whole run is checked on stand-ins down
to each record, folder, history time and app launch, and the test refuses to run with its asserts off. What only a
real app exercises is left to the sweep itself.

## `test_runtime_calls`
`test_runtime_calls.py` · Python, 49 checks (4 only when the snapshot found the item they use)

**Tests:** How the automation runtime takes calls, in five scenarios on one app, run in this order. Each is a `Checks.scenario`: one that raises fails as "<scenario>: runs to its end", and the next still runs.
- Recovery under load: Preferences is opened as a running `action_run` job. Then 32 `app_wait {for: no_dialog}` calls take the server's 32 blocking slots, and 32 more connections sit idle (first variant) or half-sent (second variant), filling the 64-connection cap. A `dialog_answer` on a new connection must still close the dialog and release the waits.
- A slice cancelled while it waits: a `plate_slice` requested right after a `settings_set` first waits about 0.5 s for the update the change scheduled. A `job_cancel` sent during that wait, as soon as `app_state` lists the slice's job running, must keep the slice from starting, and the plate stays unsliced. A slice made afterwards is the control.
- The Run Script panel out of reach: the panel is not listed by `widget_list` or picked by an untargeted `webview_run_javascript` or `keyboard_press`. While it is up, a window screenshot and an agent's `script_run` still work. Then `replay_run` runs `smoke_test.json`, the example of the replay format, and a nested inline replay.
- Refs whose items changed: an ImGui ref taken from `widget_list` still works after its item's label changes, and the exported session finds each item and passes. Two refs are tried. The Text gizmo's font button, whose label is the current font, is used for two picks, the first to a bundled font other than the one shown. A Texture Displacement layer row "Layer 2" is clicked after the first layer is removed, when that row is "Layer 1".
- The UI job worker's admission: an arrange holds the UI job worker. It is started three ways: `object_arrange {rotate: true}`, `action_run obj_arrange`, and a click on the Arrange options panel's ImGui button. While it runs, which the test sees by a change that moves nothing being refused as busy, changes and canvas input from MCP are refused and reads answer; `job_cancel` stops it, and a change is admitted afterwards. The bed is filled with copies of three cubes so the arrange takes long.

Recovery runs first, before a call of this test can be left running: one a later scenario leaves, such as an arrange starter's on a failed run, would hold a blocking slot or a connection, and the waits would no longer fill the slots. The admission runs last, so that the arrange settings it stores and the Arrange options panel it opens meet no other scenario.

**Starts from:** Each scenario starts with `start(client)` on the Generic Klipper Printer 0.4 nozzle, and checks it. No check reads a preset beyond that, and nothing here is a Bambu Lab printer's own. Then:
- Recovery under load: Preferences opens and closes alike whatever an earlier test stored. The tests before this one on the job's app have ended, and their connections closed with their processes.
- A slice cancelled while it waits: the printer's process slices the Benchy (measured: the control slice's outcome is `ok`). The setting it changes, `sparse_infill_density`, is an unsaved change of `start()`'s process, so its stored value plays no part.
- The Run Script panel out of reach: on Prepare, with the panel hidden, as `start()` leaves them. The panel docks under the 3D view: when an earlier test built it, `start()`'s default window layout docked it again, and when the app builds it here, at its first showing, it takes its place from the saved `window_layout`. The web views and the untargeted script are compared with this app's own reads before the panel shows, so a web view a plugin an earlier test left adds (`test_web_ui`'s fixture) is on both sides of the comparison. The smoke replay cannot call `start()`, so it sets its start with steps of its own: rules that answer No to "unsaved changes" and to "restore", which end with the replay; `project_new {silent: true}`, which needs the empty project this scenario's `start()` made, since on an app that holds unsaved changes it is refused; and three `preset_select` steps, each with `preset_changes: "discard"`, of the start's printer, process and slot 1 filament.
- Refs whose items changed: on Prepare. `replay_export {clear: true}` comes first, so that the exported session starts with `start()`'s own calls and its replay from the same new project. Three rules, OK to "Uniform scale" and to "text volume is empty" and No to "unsaved changes", are set after `start()`, which clears the rules. The cube comes from the Add Cube action with its size dialog answered by a rule; its size, which follows the bed or a size a person saved, decides no check. The Text gizmo's font button shows the font of the style it opens with, which an earlier test's picks may have changed: the first pick is the first of HarmonyOS Sans SC and NanumGothic, both bundled with OrcaSlicer, that the label shown does not start with, so that it changes the label. The Texture Displacement gizmo opens on one layer whatever textures earlier runs imported; its list of recent textures is not read.
- The UI job worker's admission: the Klipper's 250×250 mm bed is what the three cubes' fills cover with copies, so that each starter's arrange runs on well past the refusals made once it holds the project (measured: each still ran past 2 s from its start). The arrange settings the starters use are set by the test's own calls rather than taken from the folder: the fills pass `spacing: 0` and `rotate: false`, the first starter `rotate: true`, and `object_arrange` stores what it is given, as the panel does, so the action and the panel's button then arrange with rotation on.

**Leaves:** Measured, after one run on a new data folder and a second on the same folder:
- In the conf's `arrange` section, `min_object_distance_fff` 0 and `enable_rotation_fff` true, with `allow_multi_materials_on_same_plate` true written beside them. These are the settings of every later arrange on the job that names neither spacing nor rotation, as a person's last choice in the panel stays; the tests after it set the arrange settings their checks depend on.
- The Run Script panel's place in the conf's `window_layout`, saved with the panel hidden.
- Under `<datadir>/automation/out`, one per run: the main window's capture. In `orca_action_replay` in the temporary folder, one per run: the nested replay's `replay-<time>/result.json` (its inner `replay_run` names no `out_dir`).
- The imported height maps copied into `<datadir>/textures/displacement`, one per import: `heightmap.png`, and `heightmap (2).png` from the exported session's replay; a second run adds `heightmap (3).png` and `heightmap (4).png`. The conf's `texture_displacement.recent_textures` names each import, the newest first: `heightmap (2).png/heightmap.png` after one run, `heightmap (4).png/heightmap (3).png/heightmap (2).png/heightmap.png` after two.
- The Text gizmo's styles in the conf (`font`, `font:1` to `font:5`), as it stores them on closing.
- The bed full of cube copies, an unsaved project, which the next `start()` discards. No auto-answer rule is left in force: the last scenario's `start()` clears those the refs scenario sets.
- The G-code, the smoke replay's files, and the exported session with its replay's files go to temporary folders of the test's own.

**Decides by:** Exits 1 when any check prints FAIL. Each scenario's first check is its start, "<scenario>: the start is the Klipper printer with one Generic PLA slot", with the scenario named `recovery`, `cancel`, `Run Script panel`, `refs` or `admission`: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.

**Scenario: recovery under load** (runs first)

A variant fails when `action_run` does not reply `blocked_by_dialog`, the waits are not seen holding every blocking slot, `dialog_answer` returns `isError`, or fewer than 32 waits return `satisfied: true`. A socket error ends the scenario, which fails it.
- Every blocking slot taken: `app_wait` and `app_state` are observation calls, which run on the request thread, are never jobs, and each take one of the server's 32 blocking slots (`inspect_tool`, `McpCore::run`, `blocking_slots` 32). After the 32 waits start, the test calls `app_state` on a connection of its own every 0.2 s, up to 30 s, until it replies `status: "pending"` with the note "Too many calls are waiting right now; try again shortly.", the reply an observation call gets when no slot is free. A variant whose waits have not taken every slot within the 30 s fails; this read takes the place of a fixed sleep. An `app_state` that finds a slot free holds it while it runs, so a wait that arrives then is itself replied `pending`: each wait is sent again every 0.2 s while it is, for up to 30 s, as the note asks, and one still `pending` after that counts as a wait that did not return `satisfied: true`.
- The holders: an idle holder sends a `ping` and reads its reply before the next one opens, so the server has accepted each of the 32. A half-sent holder sends part of a request's headers and gets no reply that would show the server took it, so the test gives the 32 of them a fixed 1 s.
- "recover with 32 blocked waits and 32 idle connections"
- "recover with 32 blocked waits and 32 half-sent requests"

**Scenario: a slice cancelled while it waits**

- After `project_import` of 3DBenchy and `app_wait {for: idle}`, "the slice starts from Prepare, unsliced" reads `app_state.tab` as `prepare` and `plate_list` plate 0 `sliced: false`.
- The cancel: `plate_slice` is called on a thread of its own, and `job_cancel` goes out as soon as `app_state.jobs`, read every 0.02 s, lists a `plate_slice` job `running`. A job not listed within 30 s fails the scenario.
- "job_cancel reports the waiting slice": `was_running is True`.
- "the slice reports its plate cancelled": `status` `done` and `plates` exactly `[{index: 0, outcome: "cancelled"}]`.
- "the cancel came before the slice started": `app_state.tab` is still `prepare`. A slice that starts shows the Preview tab (`Ops::start_slice` selects it), and the call skips the start when the cancel count changed during its wait or before the start that follows it. A cancel that came after the slice started therefore fails this check instead of passing as an ordinary cancel. A cancel that lands after the wait ends but before the start is honoured the same way, and passes: it too is a cancel of a slice still waiting to start.
- "the plate stays unsliced": `plate_list` gives plate 0 `sliced: false`, and `project_export {format: gcode, path}` is refused with the code `plate_not_sliced` and writes no file at the path.
- Control, "a slice afterwards runs and exports": `plate_slice` gives plate 0 the outcome `ok`, `app_state.tab` is `preview`, `plate_list` gives plate 0 `sliced: true`, and the same `project_export` writes the file (it exists, larger than 0 bytes). This shows that the tab check sees a slice that starts, and that the export refusal was about the slice.

**Scenario: the Run Script panel out of reach**

- Run Script panel shown: "the Run Script panel shows", within 30 s of the toggle, the 3D view's height in `widget_list` (its `name=OrcaSceneCanvas` line) is less than before the toggle, as the panel docks under it. The test then gives its page a fixed 1 s: no tool sees the panel's page, so its load cannot be waited for.
- Run Script panel: the number of `webview` words in `widget_list` is unchanged, and untargeted `webview_run_javascript "1 + 1"` on Prepare gives the same outcome string as before the panel showed. `keyboard_press Escape`'s `to` contains "canvas", the screenshot returns a `path`, and `script_run` ends `finished`. The panel is then hidden again.
- Replays: `smoke_test.json` and a nested inline `replay_run` both return `passed: true`. The smoke replay passes when every call returns no error and its matchers hold: `project_import` `/added` = 1; `settings_get wall_loops` `/items/0/value` = `"3"` after `settings_set`; `plate_slice` `/status` = `done` and `/plates/0/outcome` = `ok`; for the Preferences dialog its action opens, `app_wait {for: dialog}` `/satisfied` = true and `widget_list` of the dialog for "splash" `/nodes` ≥ 1, `dialog_answer` must find a dialog titled "Preferences", and the action's job must then end without an error.

**Scenario: refs whose items changed**

A layer row is an item `widget_list` lists named exactly `Layer N` (its thumbnail is `Layer N image`), N its place in the list counted from 1; the open layer has no row. `object_list`'s `painted.texture` gives each layer's `slot`.
- Snapshot: `widget_list` lists a `Combo_Font` item and a `Layer 2` row. Opening the gizmo makes the first layer, slot 0, which the import textures, and `add_texture_layer()` takes the first free slot, so the two added are 1 and 2, slot 2 open; the layers are listed by slot, so the `Remove` of index 0 is slot 0's.
- Font: "the ref picks another font", the first `widget_set_value`, returns `selected` equal to the font picked, the first of HarmonyOS Sans SC and NanumGothic that the label shown does not start with. The second, through the same ref, returns a `selected` that starts with the label first shown.
- "the Remove leaves the second layer's row as Layer 1": after the `Remove` of index 0, `object_list` gives the slots `[1, 2]`, and `widget_list` lists a row `Layer 1` and none named `Layer 2` (slot 2 is open).
- "the old ref clicks the renamed row": after `widget_click` on the ref taken for `Layer 2`, `object_list` still gives the slots `[1, 2]` (the click removed nothing), and `widget_list` lists a row `Layer 2` and none named `Layer 1`. The click opened slot 1, the layer the ref was taken for, and slot 2 became the row. A ref that resolved to nothing, to the open layer or to a Remove button fails one of these.
- Replay: the exported session replays with `passed: true`.

**Scenario: the UI job worker's admission** (runs last)

Each starter's call runs on a thread of its own. The checks start once `object_transform {object: 0, translate: [0, 0, 0]}`, sent every 0.1 s for up to 30 s, is refused with "Busy: job" in its text, the sign that the arrange's job holds the project; a starter whose arrange does not within the 30 s goes on to the checks, which then fail on what they meet. Each starter gets the same seven checks, named after it (`object_arrange`, `action_run`, `widget_click`):
- Refusals: "object_transform refused", "keyboard_press Delete refused" and "GL toolbar click refused" each need the call to raise with "Busy: job" in its text.
- "object_list answers": the call succeeds.
- Cancel: "job_cancel stops it" needs `was_running` and `job` to be truthy. "the call reports it cancelled" needs the starter's call to raise with "cancelled" in its text.
- "a change is admitted afterwards": `object_transform` succeeds.

**Not checked:**
- Recovery under load: no tool reports the server's connections, so the half-sent holders are assumed accepted after the fixed 1 s. On a slow machine some may not have reached the server, and the half-sent variant then runs below the cap.
- Recovery under load: at the cap, the server makes room by evicting the oldest idle connection; failing that, it accepts up to 8 more into an overflow reserve (`McpHttpServer.hpp`). The test cannot tell the two apart, so broken eviction still passes, and whether a holder was evicted is not read.
- Recovery under load: when `dialog_answer` replies `running`, the follow-up `app_wait`'s error flag is dropped. That the dialog closed is inferred only from the 32 waits.
- Recovery under load: if the server's connection cap grows, the holders no longer fill it and the test still passes. A larger blocking-slot count fails instead: the 32 waits leave a slot free, and `app_state` answers rather than replying `pending`.
- A slice cancelled while it waits: the cancel races the slice's half-second wait, a race the app's timing decides: it goes out as soon as the slice's job is listed, a round trip of `app_state` and one of `job_cancel` after the request. Too late, the slice starts and "the cancel came before the slice started" fails; too early, before the slice's operation began, `job_cancel` finds no operation and "job_cancel reports the waiting slice" fails. Either is a spurious failure on a slow or loaded machine; neither passes.
- A slice cancelled while it waits: the scheduled update itself, after the cancel: only that it did not start a slice.
- The Run Script panel out of reach: the fixed 1 s stands in for the page having loaded, since the panel's page is hidden from every tool. The panel is confirmed up and its web view exists with it, so the check that counts web views holds; whether a script could reach a page that has not loaded is not tried.
- The Run Script panel out of reach: the screenshot is checked only for a returned `path`; it is never compared.
- The Run Script panel out of reach: the smoke replay's G-code export, its `view_set {tab: preview, camera: iso}` and its three screenshots only have to return no error: the G-code is never read, the tab and camera are not read back, and the files are never compared. The presets its `preset_select` steps select are not read back, and nothing shows that its auto-answer rules answered a prompt; in an app `orca_mcp.py launch` started, neither answers one (measured: their `uses` is 0).
- Refs whose items changed: the exported replay has no expects, so `passed` means every exported call found a target and returned no error. A locator that resolves to the wrong existing item, such as the row's image button, still passes.
- Refs whose items changed: a live click on the row's image button would also open slot 1 (a retextured layer becomes the active one), with the layer picker open; the picker is not looked for.
- Refs whose items changed: `startswith(shown)` on the second pick is looser than the known label.
- The UI job worker's admission: an arrange that ends before the checks after its first busy refusal are made, on a very fast machine, fails the refusals and `job_cancel` spuriously, and so does one that never holds the project within 30 s; neither can make it pass wrongly. The probe is admitted while the arrange has not started, and moves nothing.
- The UI job worker's admission: setup never checks that the bed was filled, or how many objects there are.
- The UI job worker's admission: no call is checked for its effect. Positions are never read, so neither the admitted `object_transform` nor whether the cancelled arrange left the objects in place is verified. `object_list` is checked only for not raising.

**Strength:** Partial — the cancelled slice, with its control slice, the hiding of the Run Script panel, confirmed up, and every refusal and cancel of the arrange are read exactly, and the live ref is read back by which layer it opened. But the recovery's half-sent variant fills the connection cap on trust, and the exported session and the smoke replay are checked only by passing.

## `test_export_running_job`
`test_export_running_job.py` · Python, 13 checks

**Tests:** What `replay_export` does with a call whose job outlives its reply. A `replay_run` has one step, a `script_run` whose script runs until the test writes its go file: a gate the test releases once it has exported while the job runs, so the job ends only then, whatever the machine's speed. An export taken while it runs leaves it out and names it in `left_out_running`. After it ends, it is exported as an `await: false` step that a later `app_wait` refers to, and that replay runs. A call blocked on Preferences stays in an export while the dialog is open. The sweep sets `ORCA_MCP_REPLY_BUDGET_MS=20000` for the app, so a running job replies after 20 s rather than 45 s; a shorter budget can end the call opening Preferences before it opens: on a machine running other apps, Preferences took 12 to 15 s to open (measured), past a 10 s budget. The runner launches an app of its own for the test, since its variables differ from those of the job's app.

**Starts from:** `start(client)` on the Generic Klipper Printer 0.4 nozzle; no check reads a preset, and nothing here is a Bambu Lab printer's own. The journal the exports read is this app's, cleared by `replay_export {clear: true}` after the start's own calls.

**Leaves:** Nothing stored: the process's `wall_loops` of 3 is an unsaved change, Preferences is closed without one, and the go file and the replays' files go to a temporary folder of the test's own.

**Decides by:** Exits 1 when any check prints FAIL.
- Start: `start()` returns `{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`.
- While running: `replay_run` replies `status: running`. The export taken then has exactly the steps `["settings_set"]`, and `left_out_running` is exactly `[{tool: replay_run, job}]`. Then the test writes the go file.
- After: the job returns `passed: true`. The export's steps are exactly `["replay_run", "settings_set", "app_wait"]`. Step 0 has `await: false` and `save_as: "job1"`, step 2's `job` is `"${jobs.job1}"`, and there is no `left_out_running`.
- Dialog: opening Preferences with `action_run` replies `blocked_by_dialog`. An export with the dialog open has exactly `["action_run"]` and no `left_out_running`.
- Re-run: the exported file replays with `passed: true`; its script finds the go file there and ends at once.

**Not checked:**
- The exported steps' arguments are not compared: the inline replay and `out_dir` of `replay_run`, and `wall_loops: 3`.
- The docstring's "a job that fails after its reply is dropped" case is left to the C++ unit tests.
- Timing: Preferences must open within the reply budget for "Preferences blocks its call", as the docstring says; on a slower open the call replies `running`, and the check fails spuriously. The slow job's reply waits the whole budget, whatever it is, since the job cannot end before the go file.

**Strength:** Strong — every export is compared with an exact list of steps, and the reference fields are checked by value.
