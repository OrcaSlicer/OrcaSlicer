# LAN monitor SDK diagnostics

These tools compare the installed SDK's allocation, shared-agent LAN connections and isolated
worker processes. They use the existing Orca LAN pairing records.

## Build and run

Build with the same compiler ABI as the installed networking module. On Windows, use an x64
Visual Studio Release build with the dynamic MSVC runtime:

```powershell
cmake -S tools/lan_monitor_probe -B build/lan-monitor-probe -A x64
cmake --build build/lan-monitor-probe --config Release
py -3 tools/lan_monitor_probe/run_probe.py --mode shared --seconds 70
py -3 tools/lan_monitor_probe/run_probe.py --mode workers --seconds 900
py -3 tools/lan_monitor_probe/run_probe.py --mode workers --seconds 70 --restart-after 25
```

The shared test connects the first printer, waits 20 seconds, then connects the second without
disconnecting the first. The worker test starts one SDK process per printer. The restart test
terminates and replaces only the first diagnostic worker, while the second keeps updating.
Session generations discard output from the replaced worker.

The runner reads the saved pairing records for exactly two P1-family LAN printers (C11/C12).
The configured SDK version is the default; `--sdk-version` selects another installed version.
Use `--data-dir` for an alternate Orca data folder and `--worker` for another worker executable.
On macOS and Linux these paths must be supplied explicitly.

Pairing credentials travel through private stdin pipes, without command-line arguments or
configuration-file copies. Workers use isolated SDK working directories, reuse the existing
certificate bundle/cache, disable SDK telemetry, and send full-status requests only. Each worker
publishes connection events and merged job snapshots over stdout. The coordinator accepts only
the diagnostic's prefixed JSON records and excludes SDK console output from its reports.

Results are written under `build/lan-monitor-probe/<mode>-<timestamp>/result.json`. They include
per-printer report counts, freshness, connection state, job status, progress and remaining time.
A run passes when both final snapshots are fresh, both workers exit successfully and Orca's saved
selection is unchanged. Restart runs additionally require the uninterrupted peer's report count
to advance. Printer and account identifiers and access codes are excluded from result records;
job names remain available for checking which print is being monitored.

Workers own their SDK process for its entire lifetime. At the end they disconnect and exit as
a unit, allowing the OS to reclaim the SDK's process-global resources. The parent bounds shutdown
and kills only its own diagnostic children if they fail to exit. The main Orca process is independent
of these lifetimes.

## Allocation diagnostic

```powershell
& ./build/lan-monitor-probe/Release/lan_monitor_probe.exe `
    "$env:APPDATA/OrcaSlicer/plugins/bambu_networking_02.08.01.dll" `
    ./build/lan-monitor-probe/sdk-logs
```

This calls the SDK factory directly, independently of Orca's wrapper. It uses an isolated process
and exits after printing whether the returned handles are distinct.

| Exit code | Meaning |
|---|---|
| 0 | Three distinct handles were allocated; live validation is still required. |
| 2 | Invalid arguments, missing exports, loading failure or filesystem error. |
| 3 | The SDK factory returned an existing handle. |
| 4 | The SDK rejected an allocation. |

A shared handle establishes singleton allocation; it does not establish a single-printer limit.

## Verification

On Windows, SDKs `02.08.01.53` and `02.03.00.62` both return the first handle on the second
factory call. Live tests with the configured `02.03.00.62` SDK pass for both printers using one
shared agent for 70 seconds and using separate workers for 15 minutes. Restarting one worker
also passes: its peer keeps updating and the replacement reconnects. The saved Device selection
is unchanged in these runs. Switching Prepare/Preview tabs was not verified because desktop
access was unavailable.

The side-effect-free snapshot parser has Catch2 coverage in
`tests/slic3rutils/test_lan_monitor.cpp`, registered in the existing `slic3rutils_tests` suite.
It checks independent printer state, partial updates, numeric strings, invalid/overflowing values,
remaining-time conversion, unknown estimates and new-job resets.

The same tests can be built without the application:

```powershell
cmake -S tools/lan_monitor_probe -B build/lan-monitor-dev -A x64 -DLAN_MONITOR_BUILD_TESTS=ON
cmake --build build/lan-monitor-dev --config Release --target lan_monitor_snapshot_tests
ctest --test-dir build/lan-monitor-dev -C Release --output-on-failure
```

The application builds the same C++ worker as `orca-lan-monitor-worker`; Python is used only for
these developer diagnostics. Its application mode (`seconds: 0`) keeps running while stdin is open
and exits when the parent closes the pipe or sends `{"command":"stop"}`.

On Windows, `lan_camera_probe` and `run_camera_probe.py` validate two camera tunnels concurrently
using the installed `BambuSource.dll`. The 30-second test received 22 and 27 frames, respectively,
with no errors and unchanged saved Device selection. The first frame from each is saved under
`build/lan-camera-probe/` for inspection. Camera credentials use stdin only.

The camera diagnostic uses independent threads by default, matching the grid's separate players,
and records SDK diagnostic codes separately from actual camera read errors. A concurrent run
received 24 and 25 frames with no transport errors even though the second camera logged code `3`.
That diagnostic is recoverable; the grid uses the player's terminal playback result when deciding
to retry. This also preserves actual transport-failure and initialization-timeout retries.

With `ORCA_TOOLS=ON`, the main application build also provides the explicit
`lan_camera_decode_probe` target, which links the production video decoder:

```powershell
cmake --build build --config Release --target lan_camera_decode_probe
py -3 tools/lan_monitor_probe/run_camera_probe.py --worker build/src/dev-utils/Release/lan_camera_decode_probe.exe --seconds 60
```

It reports received and decoded frames, decode failures, near-black frames and the longest arrival
gap per camera. A one-minute Windows run received and decoded 56 frames from each printer, with
zero decode failures, zero near-black frames, no transport errors and a maximum gap of 1218 ms.
These counts compare frames received by the SDK with decoder output; they do not measure frames
discarded inside the printer or SDK. They also do not measure onscreen repainting.

Automated tests also cover stale telemetry, invalid IPC, independent retry backoff, authentication
failure, callbacks after replacement/removal, camera source isolation and full/delta merging.
GUI layout, Prepare/Preview switching, real printer power loss and application networking reload
still need interactive validation. Only Windows and networking SDK `02.03.00.62` have received live
printer testing; macOS and Linux remain untested.
