# Testing and troubleshooting printer agents

*Owns evidence grades, the automated and manual verification passes, and
the open-defect register. Every "not hardware-verified" note elsewhere in
this guide resolves to a grade defined here.*

This page describes how to assess a printer-agent change without treating
source inspection as a hardware result. Use the
[manual checklist](reference/manual-checklist.html) for a repeatable live-printer
pass, and use the [capability matrix](reference/capability-matrix.md) to decide
which cases apply to the agent being changed.

## Evidence grades

Keep these grades separate in reviews and release notes.

- Source-inspected - the current implementation was read. It establishes
  intended behavior, not printer compatibility.
- Automated - a targeted test ran. It covers its inputs and assertions, not a
  printer, firmware version, or network failure that it does not model.
- Hardware-verified - the stated behavior was observed on a named class of
  live printer. Record the model, firmware, configuration, and result with the
  test evidence.

Do not call a capability supported by hardware solely because the code compiles or
a unit test passes. Conversely, a hardware observation should not be generalized
to every Moonraker-family printer without checking its configuration.

## Carried hardware evidence

Prior hardware sessions verified direct Moonraker-family connection, live
WebSocket status, and jog on both a Qidi/Moonraker printer and a generic
Moonraker box. The jog moved the printer but could leave relative positioning
active. This evidence establishes the Moonraker-base and Qidi grades in the
capability matrix. It was carried into this rewrite and was not rerun here.

It does not establish those behaviors for Creality or Snapmaker, and it does
not cover Qidi-specific filament discovery, box mapping, or print wrappers.

## Build and automated tests

Run the smallest relevant test target first, then broaden the run if the change
crosses shared agent, plug-in, or Device-tab code. Set
`<configured-build-dir>` to the CMake build tree that was already configured for
the compiler, generator, and build type you intend to use. Do not replace it
with the source directory or assume a `build` subdirectory exists.

```powershell
cmake --build <configured-build-dir> --config RelWithDebInfo --target slic3rutils_tests
cmake --build <configured-build-dir> --config RelWithDebInfo --target printer_agent_plugin_tests
ctest --test-dir <configured-build-dir>/tests/libslic3r --output-on-failure
```

`--config RelWithDebInfo` is needed for multi-config generators such as Visual
Studio. Omit it only when the configured generator is single-config and its
build type was selected at configure time. Parallel-build options belong to the
generator: for example, pass `--parallel 6` to CMake when the generator
supports it, or use the generator's own trailing arguments only when that
generator documents them. Do not combine a changed working directory, a
generator-specific flag, and an assumed build-tree layout in one command.

On Windows, start from an MSVC developer environment. A shell without the MSVC
include paths can fail in dependencies before it compiles Orca code, with errors
such as `C1083: Cannot open include file: 'stddef.h'`, `'time.h'`, or `'cstdint'`.
Those signatures are environment failures, not evidence against the agent change.

If a machine exhausts MSVC precompiled-header memory, use the documented lower
parallelism command:

```powershell
cmake --build <configured-build-dir> --config RelWithDebInfo --target slic3rutils_tests --parallel 6
```

Errors such as `C3859: Failed to create virtual memory for PCH` and `C1076:
internal heap limit reached` are machine-specific resource failures. If a build
appears hung and file operations are blocked, inspect for idle `cl.exe` processes
holding locks before changing source.

Relevant automated coverage includes:

- `tests/slic3rutils/test_qidi_printer_agent.cpp` validates malformed and null
  Qidi slot responses without throwing.
- `tests/slic3rutils/test_printer_agent.cpp` checks the public printer-agent
  surface, including filament-sync mode exposure.
- `tests/slic3rutils/test_printer_agent_plugin.cpp` exercises plug-in
  registration, replacement, conflict handling, and deregistration.

Do not present a historic test count, failure count, or skipped-test count as the
current state. Run the command above and attach its own output when a current
result is needed.

## Manual hardware verification

Use a small, disposable model and a printer that can safely accept the actions.
The checklist groups the work in the order below.

1. Confirm the printer accepts its configured URL and API key, then select it in
   the Device tab. Verify a fresh status update, not merely a successful connect
   return code.
2. Observe temperatures, targets, fan state, print state, filename, progress,
   elapsed time, and axis homing while the printer changes state.
3. Exercise safe idle controls first: home, bed and nozzle temperature, and a
   harmless G-code command. Verify the printer's action as well as the UI
   response.
4. Send a small file without starting it, then start a small print. Once the
   print is active, pause it, confirm the printer and UI both enter a paused
   state, resume it, and confirm both return to printing. Cancel only after
   observing an active or paused print, then confirm that the printer stops and
   the UI leaves that state. Cancel an upload and retry a missing input so that
   failure handling is observed too.
5. For Moonraker command workers, send three harmless, uniquely marked commands
   while a proxy, network shaper, or request log introduces or records latency.
   Pass only if the printer-side log records the markers in the same order they
   were queued. Record the latency method and the observed order.
6. For a material system, verify populated slots, empty slots, material, colour,
   refresh after a change, and cleanup when the system is absent or filament is
   removed. The latter must remove obsolete slot or material data from the UI.
   Do not infer write support from read support.
7. For Qidi, use a compatible single-nozzle slice and Send it while the Device
   tab has no reported nozzle diameter or type. Pass only if Send proceeds past
   preflight without `PrintStatusNozzleDataInvalid`; record any reported
   diameter/type and any mismatch message separately. This checks the intended
   tolerance for missing identity data, not that a mismatched known nozzle is
   safe.
8. Verify the camera only on hardware that advertises or implements it. Check
   that frames advance, switching printers starts the newly selected camera, and
   closing or changing the view does not leave misleading stale output. For
   Snapmaker, also test immediately before and after 300 seconds in an
   uninterrupted view. The expected renewal result is unknown until hardware
   evidence exists. Swap agents and shut down the app after the camera cases to
   exercise teardown around the detached callback's raw-`this` lifetime risk.
9. For Moonraker thumbnails, test a reused filename after its thumbnail changes,
   responses that use `thumbnail_path` and `relative_path`, and a thumbnail in a
   subdirectory below the G-code root. Record the endpoint payload and displayed
   image for each case.
10. Disconnect and reconnect the printer, then confirm that new status messages
    still reach the UI. A reconnect completion alone is insufficient evidence.
11. Test network and response failures for discovery, status, G-code, upload,
    and print start. For each operation, exercise HTTP 401, 404, and 500,
    invalid JSON, and a refused socket with a controlled proxy or test server.
    Each case must fail clearly or offer a retry, without a crash or a false
    success. Record the operation, injected failure, UI result, and any retry.

For Moonraker, record whether the thumbnail endpoint returns the response shape
the agent expects. That response has not yet been verified across a live
Moonraker deployment.

## Troubleshooting by symptom

### Connection appears successful but the Device tab stays stale

Treat status freshness as the connection result. Enable `ORCA_NETWORK_DEBUG` and
look for a new `parse_json: dev_id=` entry after the connection or reconnection.
The unresolved reconnect-delivery problem can complete the second connection
without delivering any new parsed messages. Capture an instrumented second
connection before changing dispatch or message-delay logic, because both remain
plausible causes.

Check identity too. One path can use a bare IP address while another uses
`host:port`; configuring a port can therefore create two machine objects. Do not
diagnose a duplicate as a printer-agent failure until the identities are
compared.

### A control reports success but the printer did not change

First establish that the command has a documented translation in the capability
matrix. Unsupported commands are deliberately rejected rather than silently
accepted. For Moonraker, queued controls are asynchronous, so wait for the
printer-side result and capture the request or log before concluding it was lost.

Moonraker jog is a special case. The current path can leave the printer in
relative positioning mode after a jog. Do not use it as a general verification
control until it is changed to save state, issue `G91` and the move, then restore
state with `SAVE_GCODE_STATE` and `RESTORE_GCODE_STATE`. Extruder-relative moves
use a separate `M83` path.

### A thumbnail is missing or belongs to an earlier print

The thumbnail lookup accepts both `thumbnail_path` and `relative_path`, but the
live endpoint response is not yet verified. The cache is keyed by filename, so
reusing a common name can retain the previous image. Test a distinct filename
before changing the lookup. Paths below the G-code root also need live coverage
for the `relative_path` fallback.

### Filament looks stale, blank, or does not follow an edit

Moonraker-family agents use pull-mode filament sync. Verify the pull request and
the resulting Device-tab update rather than expecting a subscription callback.
Read-side discovery does not establish load, unload, slot-setting, or Auto Refill
support. Happy Hare and AFC macro names are printer-side configuration; do not
guess them. A guessed macro can silently do nothing or issue the wrong action.

### A Python agent disappears or cannot be enabled

Check its agent ID first. A duplicate ID is rejected and the conflicting
capability is disabled rather than auto-promoted later, because automatic
promotion could change the active printer implementation without user intent.
Reload and teardown also need a live check: registration tests cover lifecycle
logic, but a plug-in can still be exposed to API drift or a teardown race in a
real session.

## Known defects and safeguards

- Reconnect delivery remains unresolved. Instrument the second connection before
  attempting a fix; the observed failure is stale data after a completed reconnect.
- Moonraker jog can leave relative mode active. Keep the future state-save and
  restore sequence together so the jog cannot affect later G-code positioning.
- Qidi's nozzle-data Send-preflight tolerance for unreported diameter and type
  has not received a hardware verification.
- A configured `host:port` can coexist with a bare-IP machine identity. This can
  duplicate devices and confuse selection.
- Moonraker thumbnail caching can show an old image when a filename is reused.
- Moonraker filament data can be stale, and its pull/read path does not provide
  safe generic write-side MMU operations.
- Duplicate plug-in agent IDs are rejected. There is no automatic fallback to a
  losing capability after the winner unloads.
- Plug-in implementations can drift from the Python printer-agent API. Treat an
  import or interface error as a plug-in compatibility issue until proved otherwise.
- Snapmaker camera callbacks can outlive their view during agent replacement or
  shutdown because the detached path retains a raw `this` pointer. Treat a crash
  or stale callback during those transitions as a source-derived use-after-free
  risk until the lifetime is made explicit.
- Snapmaker writes an IP-specific local camera HTML file below the application
  cache. The source contains no cleanup path, so residual files can accumulate
  for each unique printer IP. This is source-derived and was not reproduced.

## Not yet hardware-verified

- Moonraker command-worker FIFO behavior under recorded or injected network
  latency.
- Moonraker thumbnail responses: reused filenames, `thumbnail_path`,
  `relative_path`, and subdirectory paths.
- Snapmaker camera behavior on a live U1: frames, renewal across a long-open
  view including the 300-second boundary, switching between printers, agent
  replacement, and shutdown teardown.
- Whether Snapmaker's renewal cadence prevents a stale-frame interval. Do not
  shorten it as a workaround without resolving the printer-side behavior first.
- Creality CFS detection and preset scoring on a real printer.
- Qidi Send preflight when firmware omits nozzle diameter and type.
- Moonraker-family write-side MMU commands. They remain blocked on verified,
  printer-specific macro contracts.

## Background - source locations for maintainers

The Moonraker command worker, status stream, print path, and thumbnail
lookup live in `MoonrakerPrinterAgent`. Qidi maps its material box before routing
to the Moonraker base. Snapmaker adds the camera start request and its snapshot
page. Python agent registration and conflict handling live in
`NetworkAgentFactory`.
