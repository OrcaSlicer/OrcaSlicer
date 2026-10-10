# Experimental Bambu network helper

The Windows Bambu helper lets native Arm64 OrcaSlicer call an unchanged x64 or
x86 networking DLL in a matching executable running under Windows emulation.
It is opt-in at both build time and runtime. Without either opt-in, the existing
in-process DLL loader and agent behavior are unchanged.

## Boundary and capabilities

`BBLNetworkPlugin` keeps its function-pointer interface, but resolves native
proxy functions when the helper is active. `BBLPrinterAgent` and
`BBLCloudServiceAgent` retain their existing JSON and filament-ID conversions.
The actual module handle, STL objects, callbacks, and agent pointer live only in
the helper. The native agent handle is a local token, never a DLL pointer.

The prototype accepts only the **02.08.04** ABI generation. The helper checks the
DLL's reported version and debug/release consistency before advertising exports.
Older generations are deliberately rejected, avoiding a struct-layout or
calling-signature mismatch. `Api.def` is the explicit export allow-list and the
handshake capability manifest. Missing or unsupported functions resolve to null.

Supported operations include agent configuration, telemetry disabling, discovery,
LAN connections, messaging, subscription management, selection, binding, basic
account/login operations, and the five print/upload entry points. Account support
is not complete: cloud metadata, presets, firmware queries, publishing, and
MakerWorld APIs are outside the manifest. The helper is primarily a LAN experiment.

**Camera playback through BambuSource and the FileTransferModule file-management
interface are unsupported.** No module handle is fabricated to satisfy those
interfaces. Startup permits networking without BambuSource in helper mode; it
still requires that module in ordinary in-process mode. The download request
selects the Windows package rather than the native Arm package in helper mode.
Use a helper executable matching the downloaded DLL, normally x64.

## RPC and threading

A duplex local named pipe carries little-endian 32-bit lengths followed by UTF-8
JSON. Frames are capped at 16 MiB, and the inbound request queue is capped at 1024.
The protocol version is separate from the DLL ABI version. Strings, structs, and
output arguments have explicit value encodings; no STL layout, pointer value,
function address, or closure is sent across the pipe.

Each endpoint has a reader and dispatcher. The reader remains active while a
request waits for its response. Print/upload and binding calls execute on at most
four helper job workers so ordinary calls can run during an upload. A dispatcher
waiting for a reverse response can also pump nested requests. This supports
callbacks issued directly by an export and by DLL-owned worker threads.

Registered callbacks have session-scoped IDs and remain alive until replaced or
unregistered. Job callbacks remain registered until the synchronous job export
returns. Cancellation and wait-decision callbacks are reverse RPC requests, so
the DLL receives their actual return values. If the client disconnects, callback
adapters return cancellation=true and wait=false. Exceptions never escape a
callback into the proprietary DLL.

`QueueOnMainFn` cannot transport an arbitrary C++ closure. Its adapter stores the
closure in a helper-local queue and runs it on a dedicated helper thread. Existing
Orca GUI callbacks continue to schedule their UI changes through `CallAfter`.
The helper disables and drains its closure queue before destroying an idle agent.
An explicit destroy during a job is rejected; closing the session cancels the job
and terminates the helper through its Windows job object.

Ordinary RPC calls and reverse callbacks have 30-second deadlines; binding has
five minutes and print/upload jobs have 24 hours. Expiry or pipe failure disconnects
the session and fails outstanding calls. Calls are never replayed. A lost print
response can mean the printer accepted the job: verify its status before submitting
again. Restart networking to establish a new session after failure.

The pipe name uses cryptographic randomness, its DACL grants access only to the
current user's SID, and remote pipe clients are rejected. The parent verifies the
connected client PID against the child it launched. The helper path must be
absolute; process creation uses an explicit executable path without a shell or
inherited handles. A kill-on-close job prevents orphan helper processes. DLL
loading uses its absolute path and restricted dependency-search flags. Print
files are shared by path rather than copied through RPC; they must remain present
until their job finishes. Credentials are transient RPC values, not diagnostic
log output.

## Building and enabling on Windows

Build the helper separately from Orca so it can use a different architecture:

```powershell
cmake -S src/slic3r/Utils/BambuBridge -B build/bambu-helper -A x64
cmake --build build/bambu-helper --config Release
```

For an actual 32-bit DLL, substitute `-A Win32`. Use MSVC and a release runtime
compatible with the DLL's C++ ABI. MinGW builds check the Windows API and transport
code, but its STL ABI is incompatible with Bambu's MSVC DLL. Arm64EC is unnecessary:
the helper runs under Windows' ordinary x64/x86 emulation. Windows 11 on Arm
supports both; Windows 10 on Arm only supports x86.

Configure Orca's normal Arm64 build with
`-DORCA_BAMBU_NETWORK_BRIDGE=ON`, then launch it from a shell with the helper path:

```powershell
$env:ORCA_BAMBU_HELPER = (Resolve-Path build/bambu-helper/Release/orca-bambu-network-helper.exe).Path
& 'C:/path/to/Arm64/OrcaSlicer.exe'
```

Install/select a supported 02.08.04 networking plugin through Orca's existing
plugin controls. The helper uses the DLL file selected by those controls. Start
with a LAN printer; use the usual printer ID, IP address, and access code.
Remove `ORCA_BAMBU_HELPER` to use the ordinary loader again. The experimental
client is usable on an x64 Orca build too, allowing process-boundary debugging
before moving to Arm64.

## Verification

Portable framing, argument/output conversion, Unicode, persistent callbacks,
cancellation, nested RPC, DLL-thread callbacks, and disconnect behavior are tested
without Orca's dependency build:

```sh
cmake -S src/slic3r/Utils/BambuBridge -B build/bridge-tests -DBAMBU_BRIDGE_TESTS_ONLY=ON
cmake --build build/bridge-tests
ctest --test-dir build/bridge-tests -C Release --output-on-failure
```

The same tests also belong to the normal `slic3rutils_tests` suite. Windows has a
fake DLL and a smoke executable that use the actual helper launch, named pipe,
proxy table, and MSVC ABI. This fixture never contacts a printer:

```powershell
cmake -S src/slic3r/Utils/BambuBridge -B build/bridge-smoke -A x64 -DBAMBU_BRIDGE_WINDOWS_SMOKE=ON
cmake --build build/bridge-smoke --config Release
ctest --test-dir build/bridge-smoke -C Release --output-on-failure
```

The experiment workflow runs the portable tests and Windows process smoke test.
The fixture does not validate Bambu's undocumented threading requirements or DLL
behavior under Arm emulation. Hardware verification must include discovery,
connect/disconnect, incoming status messages, upload progress, cancellation,
print submission, plugin reload, helper termination mid-upload, and application
shutdown. Verify that normal printer commands remain responsive during upload,
that selected print files remain available, and that failed sessions do not
submit a job twice. Camera and file-management features are not acceptance
criteria for this prototype.
