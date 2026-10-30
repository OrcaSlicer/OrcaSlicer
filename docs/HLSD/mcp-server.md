# MCP Server

## Why it exists

The MCP server lets an AI agent drive a running OrcaSlicer through the Model Context Protocol.
The same tools serve two audiences. A developer's agent reproduces a GitHub issue: it loads the
files, picks presets, changes settings, clicks through the GUI and slices. It also takes
screenshots before and after a fix and replays the steps from a script file. A user can drive
OrcaSlicer from Claude Code, Codex, Cursor or Claude Desktop.

The server runs inside the GUI process, because most of what an agent reproduces is GUI
behaviour, and reuses the Speed Dial catalog, so a feature added to the palette reaches agents and
scripts too. It is off by default; until it is first turned on, `McpService` does not exist, so
there is no thread, socket, modal-dialog hook, ImGui item recording or synthetic input state. The code is in
`src/slic3r/GUI/Mcp/`; the protocol, transport, threading core, script runner and operation logic
don't use wx, so they can be unit-tested.

```
MCP client ──POST /mcp──▶ McpHttpServer ──▶ McpProtocol ──▶ McpCore ──▶ UiInvoker ──▶ UI thread
stdio client ──▶ orca-slicer --mcp-stdio ──┘                   │
                                                          JobRegistry, Journal
```

`McpService`, owned by `GUI_App`, owns all of it except the bridge, plus the `ToolRegistry` and
the `ModalTracker`.

## Transport

The endpoint is `http://127.0.0.1:<port>/mcp`, bound to loopback. The default port is 13619,
since 13618 is the OAuth callback server. It is stateless Streamable HTTP: each POST carries one
JSON-RPC message. A request gets `200 application/json`. A notification or a response gets `202`
with no body. There is no SSE stream and no `Mcp-Session-Id`. Other methods get 405 with
`Allow: POST`, and other paths get 404. Batches get `-32600`, and so does an `id` that is neither
a string nor an integer.

`McpHttpServer` runs every socket on one Boost.Beast I/O thread, with a 30 s read deadline, a
120 s keep-alive idle timeout and an 8 MiB body cap. Each complete request runs on its own thread,
because a handler can block for a tool's whole reply budget. A connection carries one request at a
time, so threads never outnumber connections. The server takes 64 connections plus an overflow
reserve of 8. At the cap, a new connection evicts the one waiting longest on its client (idle or
still sending), never one whose request is being handled, and uses a reserve place only when all
are dispatched, so a recovery call gets through even when stalled clients hold the connections.
The listening and accepted sockets are made non-inheritable (`FD_CLOEXEC`, or no
`HANDLE_FLAG_INHERIT` on Windows). Otherwise a child process, such as a web view helper or a
post-processing script, would keep the port bound after the server closes it, and new connections
would queue on a socket nobody accepts.

Before any JSON is parsed, `validate_request()` checks path (404), method (405), a `Host` of
`127.0.0.1:<port>` or `localhost:<port>` against DNS rebinding (403), a loopback `Origin` when one
is sent (403), the bearer token in constant time (401 with `WWW-Authenticate`), `Content-Type:
application/json` (415) and the size cap (413). No CORS headers are sent, so browser preflights fail.

`McpProtocol` picks one of two protocol eras for each message.

- **Handshake era.** `initialize` echoes the client's version if it is 2025-11-25 or 2025-06-18,
  and answers 2025-11-25 otherwise. Then come `notifications/initialized`, `ping`, `tools/list`
  and `tools/call`. A present `MCP-Protocol-Version` header must be supported (400).
- **2026-07-28.** A request whose `params._meta` carries
  `io.modelcontextprotocol/protocolVersion` is served without a handshake. `server/discover`
  lists versions, capabilities and instructions. The `MCP-Protocol-Version`, `Mcp-Method` and
  `Mcp-Name` headers must match the body (400, `-32020`); `Mcp-Name` is needed for `tools/call`,
  `prompts/get` and `resources/read`. An unsupported version gets 400 `-32022` with the supported
  list. Results carry `resultType: "complete"`, and list results carry `ttlMs` and `cacheScope`.

A tool result is one compact JSON text block plus any image blocks; no tool declares an
`outputSchema`, so no `structuredContent` is sent. Tool failures, invalid arguments included, are
`isError` results the model can act on; an unknown tool is a JSON-RPC `-32602` error naming close
matches. `validate_arguments()` checks arguments against the tool's schema first. Before it,
`normalize_arguments()` reads what agents commonly send in place of the schema's form, for calls and
script steps alike, so the journal records the schema's form. An argument under a name the tool does
not take is read as one it does, when the call leaves that one out and the value fits it: `ref` as `target`, `key` as `keys`, `text`
as `value`, `action` as `id` or `button`, `object` as `objects`, `plate` as `plates`, `filter` as
`query`, `settings` as `values`, `target` as `root`. A value in another JSON form is converted: a
number, boolean, array or object written as a string, an index as a string where a name may also go
(`"0"` for a plate), a single item for an array. A value outside its argument's enum moves to the
one other argument whose enum takes it, when the call leaves that out: `set_view`'s `view: "front"`
is its `camera`. Each enum lists only its own values, since listing a camera direction under `view`
too had agents give it there beside a `camera`. Anything that still does not fit is left for the
validator to report. The bench's transcripts are where these come from: each was an agent's first
guess, many times over, and each cost a failed call and a retry.

The server `instructions` (under the 4096 characters clients keep) give the workflow: `get_state`,
act, then check the result with the reading tools, keeping screenshots for what those do not report.
They say to prefer typed tools over clicks and never retry a call that returned a job, point to
`get_settings`' meta and search for what a setting does, say how a gizmo opens and is driven (and that
`gizmo_file` only stands in for a file picker inside one, since its name draws agents to it for any
gizmo work), that the camera turns with `set_view` rather than a drag, and which indices are 0-based. Clients that defer tools load only their names and the
instructions at first, so the instructions are how an agent without a skill learns which tool to
load.

## Security, token and discovery

The bearer token is 32 random bytes as 64 hex characters in `<datadir>/mcp/token`: directory 0700
and file created 0600 with `O_CREAT|O_EXCL` on POSIX, the per-user ACL of `%APPDATA%` on Windows.
It stays out of AppConfig, whose writes and backups use default permissions. `ORCA_MCP_TOKEN`
(preferred, as it stays out of the process list) or `--mcp-token` overrides it for one run;
`get_state.mcp.token_source` says which is in force. Every start of the endpoint reads the file, so
deleting it and turning the server off and on in Preferences issues a new token. While
serving, `<datadir>/mcp/server.json` holds `{url, port, pid, version, token}` with the token in
use, so the stdio bridge and the harness connect without the launch environment. It is written
owner-only via an exclusive temporary file and a rename (`write_private_file()`), and removed on
stop, but only by the process its `pid` names: windows that share a configuration folder share the
file, and one that stops must not hide another's server from the stdio bridge.

## Lifecycle

`GUI_App::on_init_inner()` starts the service once AppConfig is loaded. That is before the first
startup dialog, such as the TLS prompt or the setup wizard, so an agent can answer those dialogs.
`--mcp` or the `mcp_enabled` preference opens the endpoint, while `--mcp-script` alone, or opening the
Run Script dialog, only builds the catalog. `get_state.ready` stays false until `post_init()` calls `mark_ready()`, the Prepare
canvas has loaded OpenGL and the main window has built its tab pages, and tools that need the main
window fail with "still starting". Starting on Home, the canvas loads OpenGL at idle after
`post_init()` (MainFrame's GL prebuild), and before that a 3MF save would draw its thumbnail with no
GL functions loaded. The same idle queue builds the tab pages (`MainFrame::prebuild_pending()`); a
page's web view registers its `wx` script message handler with a script it waits for in a nested
loop, and on GTK calls that came in meanwhile have left that wait unfinished for good, so that no web
view made afterwards gets `window.wx` and a dialog page's own buttons do nothing. So `mark_ready()`
polls every 100 ms until both are done. A hidden canvas that can't get a GL context loads OpenGL on
its first draw instead, and the pages build only while no input arrives, so after 10 s ready no
longer waits for either. Becoming ready also starts a command-line script. Notifications don't depend on this: one pushed or updated before the first
ImGui frame has built the font waits for its first render to measure its text.

The service stops, idempotently, at the start of `MainFrame::shutdown()` when the app closes
(before panels are destroyed) and on `wxEVT_END_SESSION` (after which macOS calls `exit()`).
`McpService::stop()` wakes every waiting call with "shutting down" (`UiInvoker::shutdown()`,
`JobRegistry::wake_all()`), refuses new calls, closes the endpoint and removes the discovery file,
drops every auto-answer rule, clears `SyntheticInput`, removes the menu capture and turns ImGui
recording off. It frees
nothing: a call may still be on the UI thread's stack inside a nested event loop, using the
inspector and the modal tracker, and job and request threads may still be returning.
A stopped service never starts again. `GUI_App` holds the service in an `McpServicePtr`, whose
deleter (`McpServiceDeleter`) is the only way to destroy one: the destructor is private. The
deleter stops the service, waits with `McpService::drain()` until no request, call or job thread
is inside it, and frees it; a call that never returns would make freeing a crash, so after 10 s the
service is left in place instead. `GUI_App::destroy_mcp_service()` runs it from `OnExit()`, and
from `CleanUp()` after a failed initialization, outside any event loop, so no closure can be
running. HTTP request threads reach the protocol through a shared `RequestGate`, which `drain()`
closes, so a late request gets 503 rather than a freed object. `McpCore`'s own destructor waits for
every call and job thread to leave, however long that takes; its owner drains first, so it returns
at once.

A language change runs `MainFrame::shutdown()` from `recreate_GUI()`, which only suspends the
service: the transport stays up, the UI generation is bumped so every GUI ref goes stale, UI calls
fail with a request to retry, and `ready` is false until `recreate_GUI()` resumes it. A UI closure
posted before the rebuild refuses to run after it ("not run, safe to retry").

Several OrcaSlicer windows may run at once, and those that share a configuration folder share the
`mcp_enabled` preference and the port. The bind is exclusive (`SO_REUSEADDR` stays off on
Windows, and POSIX refuses a second listener), so the first window to start serves and agents,
over HTTP or stdio, reach it. A later window that wants to serve finds the port taken by the
process the discovery file names (`discovery_held_by_other()`): it reports no error, Preferences
says another window is serving agents, and `GUI_App` tries the port every 3 s, so when the serving
window closes, one of the others takes over and writes the discovery file. A window that cannot
bind for any other reason reports it and stops trying. Windows meant to serve side by side need
their own configuration folders (`--datadir`) or `--mcp-port`; agents then pick one by its URL, or
the stdio bridge by `--datadir`. The server makes no attempt to route between windows.

Preferences turns the server on with `GUI_App::start_mcp_service()` and off with
`GUI_App::stop_mcp_endpoint()`, which stops the takeover retries and calls
`McpService::stop_serving()` to close only the endpoint and remove the discovery file. The
catalog, running jobs and a launch script carry on, because the call that opened Preferences may
itself be an MCP call still on the stack. A new port reopens the endpoint through
`GUI_App::restart_mcp_endpoint()`, and a port set in Preferences replaces `--mcp-port` for the rest
of the session. Closing the endpoint drops the session's auto-answer rules, and a client's rules
that a call still running registers afterwards are refused, so a person using the app afterwards
never has a prompt answered for them. Closing doesn't wait for in-flight requests, which
may be waiting on the UI thread that closes it; their replies are dropped. A bind failure is
logged, shown as a notification and in Preferences, and a start from Preferences never reruns a
command-line script. The modal tracker is created with the service and picks up modal dialogs that
are already open, such as Preferences when the server is turned on from there.

## Threading

Tool handlers never run on the UI thread. Anything that touches wx goes through
`ToolContext::run_ui()`. It posts a closure through `UiInvoker` to a `UiDispatcher`, then waits.
The dispatcher is `wxApp::CallAfter` in the app and a queue the test drains in tests. A closure
still queued after 10 s is abandoned without running, and the call fails saying it was not run and
is safe to retry. Once a closure has started it may use the caller's stack (the handler's captures),
so the caller waits for it to finish, through shutdown too; the closure ends when the UI thread
unwinds out of it. `ToolContext::observe_ui()` runs a closure under the inspect guard whatever the
tool's class: operations poll their progress with it, and `settle()`, the round trip a tool makes
after a change so queued events are handled, uses it and ignores a refusal, since the change is
made already.

`GuardClass` decides what may run when:

| Class | Tools | Runs behind a modal or inside another call |
|---|---|---|
| `Inspect` | get_state, wait, get_log, list tools, get_settings, screenshot, ui_snapshot, start_recording, stop_recording, encode_recording | yes |
| `Recover` | dialog_respond, dialog_auto_answer, GUI input aimed at the topmost modal | yes |
| `Mutate` | everything else | no |

The closure checks the guards on the UI thread, since the app can change between posting and
running. A closing app or a stopped service fails the call with "shutting down". A frame being
recreated fails it with "retry". A mutate call is refused while a modal dialog is open, and the
refusal lists the dialogs. It is also refused when it would run inside another UI call's nested
event loop: a modal, `ProgressDialog::Update` or `wxYield`. The GUI input tools (`ui_click`,
`ui_input`, `ui_key`) and `dialog_respond` run as recover calls. Once a target resolves outside the
topmost modal, they apply the mutate checks themselves (`UiInvoker::require_mutate()`), along with
operation admission (`McpCore::admit()`). `cancel` is a recover call too, so neither a dialog the
operation opened nor another call's nested loop keeps it out.

`ToolDef::inline_call` decides where a call runs. Pure observation tools run on the request
thread. Every tool that can activate UI runs as a job on its own thread, recover tools that click
in dialogs included, since a click can open another modal (Preferences, Keyboard shortcuts, Edit).
`JobRegistry` names jobs `j1`, `j2`, …, records each job's tool and parent, allows 64 running and
keeps the last 64 results, plus finished jobs whose parent (a running script) may still wait for
them, up to 1024 in all. The request thread waits up to `ToolDef::budget_ms` (45 s, under
Codex's 60 s tool timeout); after that the reply is `{status: "running", job, tool, note}` and the
job keeps going until `wait {for: "job"}` (at most 50 s per call) returns its result. The server
never retries a mutating call, and every envelope tells the model not to either.

**blocked_by_dialog.** A handler that opens a modal blocks in its nested loop, so every 1.5 s the
request thread posts a probe closure, which runs inside a wx dialog's loop and returns the
`ModalTracker` snapshot as JSON; no `wxDialog*` crosses threads. With a modal open, the reply is
`{status: "blocked_by_dialog", job, dialogs}`. Its note says how to answer the topmost dialog:
`dialog_respond` for a wx dialog, a person for one marked `native: true`, which `dialog_respond` can't
press.

On macOS a file picker, folder picker or message box pauses wx's pending events while it is open
(`wxCFEventLoopPauseIdleEvents` in its `ShowModal()`), and `CallAfter` closures with them, so behind
one the probe goes unanswered and so does every `run_ui()` call. A probe unanswered within 1 s
therefore falls back to the snapshot `ModalTracker` publishes for any thread: if its topmost dialog is
native, the reply is `blocked_by_dialog` with those dialogs and a note that tools needing the UI
thread fail until the dialog closes. Otherwise the request keeps waiting. A `run_ui()` call that times
out with a native dialog on top fails naming it, instead of reporting a busy UI thread. The dispatcher
stays `CallAfter`: running closures some other way inside the Cocoa modal session would bypass wx's
pause and re-enter the app while the dialog is up, and native dialogs can only be answered ahead
anyway.

**Blocking slots.** A request that may block (an inline call, or a job call within its budget)
first takes one of 32 slots. Without one, a job call returns its `running` envelope at once and an
inline call returns `{status: "pending"}`, so at most 32 request threads ever block, well under the
64-connection cap.

### Operations

Slicing and G-code export finish in the background process. Their handlers call
`ToolContext::begin_operation()` in a short UI closure before starting the work; that fails if
another operation runs. An export starts in that same closure. A slice first waits for the
background update that a change made just before has scheduled, which validates the new state, and
then requests the slice. A `cancel` that comes while it waits moves on `Plater::slice_cancel_seq()`,
so the slice never starts and reports its plates `cancelled`. `poll_ui()` then checks on the work
through `observe_ui()` every 200 ms, so a dialog that opens meanwhile doesn't stop the watch, until
the work ends or the deadline passes. The deadline is `slice.timeout_s` (default 30 minutes) for a
slice and 10 minutes for an export.

**Worker jobs.** Arrange, orient, Fill bed and the jobs of the Text, SVG and texture gizmos run on
the Plater's UI job worker, whatever started them: the typed `arrange`, `run_action obj_arrange`, a
GL toolbar item, an ImGui button such as the Arrange options panel's, or a gizmo control. `McpCore`
watches that worker through `WorkerHooks` (busy and cancel, both on the UI thread). A call that finds
the worker idle at the start of one of its UI closures and busy at the end of that closure, or of a
later one (a toolbar event or a render queued by the call starts the job between closures), has
started a worker job, and `McpCore::claim_worker()` makes that job the call's operation. The call's
own job then ends only once the worker is idle again, its finalization included, since the job holds
`ModelInstance` pointers until then. (`Worker::is_idle()` reads true from the moment the worker takes
the finalize message, so a dialog finalization opens, such as an arrange's geometry error, ends the
hold early; the modal guard keeps changes out until it closes.) Its reply waits within the reply budget and
then becomes a running job, as for any call. `cancel` (or the call's own timeout, 30 minutes unless
the tool sets a shorter one, as `arrange` does) asks the job to stop, but `Worker::cancel()` does not
wait, so the call keeps the operation until the worker is idle and then fails as cancelled. `cancel`
stops only a job an MCP call claimed. The worker doesn't record who queued a job, so a job a person
starts while a call is still between its closures, after one that found the worker idle, is claimed
by that call too: its reply then waits for that job. The window is the call's own length, a
second or so for most calls and up to its poll timeout for a call that waits on the UI.

**Admission.** While an operation job runs, every other `Mutate` call is refused ("Busy: job j12 is
running an operation (slice, export, arrange or another background job)"), whichever client or script
sends it. So an agent can't change the project under its own operation. While the worker runs a job
no call holds, a person's (typing in the Text gizmo) or one not yet claimed, changes are refused too
("Busy: a background job ..."); reads are not. The check, `McpCore::admit()`, runs when the call
arrives (operations only, since the worker lives on the UI thread) and again in each of its UI
closures, where operations start, so a call admitted just before an operation began still can't
change the project under it. Inline tools, recover tools answering the topmost modal and
`allowed_during_operation` tools (`cancel`) still run; the GUI input tools apply the same check once
their target is outside the topmost modal.

**Identity.** An operation must never take another's completion for its own. `Plater` numbers
slice requests (`slice_request_seq`, bumped through `request_slice()` by every user-facing slice
entry point, not by internal restarts) and background runs (`slice_run_seq`). It keeps a
`SliceRunRecord` per run with the request it ran for, its plate, whether it finished with a
result, and the project revision and request sequence at its finish. A slice request the validation
refused before running is recorded too, as a finished run carrying the validation error; other
updates validate as well, but only a refusal met by a slice request, or by Slice All moving to its
next plate, is recorded. So is a request `Plater::reslice()` turns away before validating (a plate
already refused, a broken mixed filament, missing plugins, a UI job that won't stop), with the
reason, so it is never taken for a cached result. `Plater` also notes each request that a later request reached while it was
still slicing (`slice_request_interrupted()`).
`Plater::project_revision()` rises with every undoable model edit, undo/redo, load, new project and
config change, and with every change of a plate's layer G-codes (the plater's handler of the layer
slider's ticks), but not when a plate is selected (`Plater::take_plate_selection_snapshot()`), since
Slice All selects each plate in turn. A ticket's own runs are those numbered after it began that
its request started (`SliceRun::for_request`); Slice All's later plates run for the same request,
while a reslice from a Preview switch or automatic slicing, or an export, merely runs under the
request's number and is not the ticket's. A run already slicing when
the request was made, which the request left running (it returns early, for instance while a gizmo
is in editing mode), is adopted as the ticket's own. A later request means someone sliced again:
if it came while the ticket's request was still slicing, the ticket is `superseded` at once,
without waiting for that slice; if it came after, the ticket's result stands and their slice isn't
waited for. Otherwise the ticket ends once nothing slices and it either saw slicing run or waited
1.5 s with nothing scheduled, and is `superseded` when the project changed, or another slice was
requested, during its own runs (or, with none, by the final poll). Each plate's last own run then
decides its outcome, whatever someone did to the plate after it finished: `ok`, `error` with the
run's message, or `cancelled`. A plate no own run reached is `empty`, `ok` when it was already
sliced, `error` with the latest validation message when the configuration was refused before the
request so no run started (`Plater::plate_validation_error()`), `cancelled` when the operation was
cancelled, or `skipped` (Slice All stops at a failing plate). Statistics come with an `ok` plate
only while the project is as the operation sliced it and nobody has sliced that plate since
(`plate_result_is_own()`). They are `plate_summary()` of the plate's `gcode_statistics_json()`, the
serializer of the exported `statistics.json`, so the reply agrees with that file and with the
legend: `filament_g` and `filament_m` count the flush, as the legend's Total Filament and the
G-code's per-filament totals do, and `filaments` lists every filament slot of the project
(`filaments[i]` is slot i + 1, zeros for slots the plate does not use) with its model, support,
flush and prime tower grams. The silent-mode time and extruder changes, which that file leaves
out, come from the result's estimates, and only when the printer has them. The summary is read in
the poll closure that decides the outcome, since a UI call of its own would wait behind the
preview's loading of the new result; the serializer's one pass over the moves is short next to
that. `cancel` also drops a pending automatic reslice: it stops the delay timer,
whose callback wx then never delivers, and moves on a generation that a zero-delay reslice already
queued with `CallAfter` checks. An export is numbered in `export_seq` once scheduled, so an
unchanged number means it never started; otherwise its `ExportRecord` ends `ok`, `failed`,
`cancelled` or `superseded`. A G-code export with a path is refused while slicing runs, and
`run_action export_gcode` needs a path: without one it would ask in a native file dialog that MCP
can't fill in. The
decision logic is in `McpOperations`, which uses neither wx nor `Plater`.

**Idle.** `wait {for: "idle"}` holds when no modal is open, nothing slices (`Plater::is_slicing_any()`:
the background process, Slice All, a scheduled background update or a pending automatic reslice)
and the UI job worker (arrange, orient) is idle, and no gizmo on any canvas reports
`GLGizmoBase::is_busy()`: work a gizmo started in the background whose result it has yet to show
(Simplify's calculation until `worker_finished()` has run, Texture Displacement's adjustment
preview). It also needs no job running other than the caller and its ancestors,
so a script waiting for idle doesn't wait on itself. Only the job registry, the `ModalTracker`
snapshot and the journal are shared between threads, each behind a mutex. All app state is read on
the UI thread.

## Tool catalog

| Group | Tool | Purpose |
|---|---|---|
| App | `get_state` | version, platform and display backend, ready, tab, mode, project, presets, plates, the Preview's view type, time mode and layer range, objects, slicing, print host uploads, the open gizmo, dialogs, menus closed unseen, notifications, jobs, window |
| | `wait` | `ready`, `idle`, `dialog`, `no_dialog` or a `job` (default the job given, else `idle`); `timeout_s` up to 50 |
| | `get_log` | end of the current log file, filtered by minimum level |
| Speed Dial | `list_actions` | search the action catalog, paged |
| | `run_action` | run an action by id (or the name inside it), with its optional `param`; an error when the action did nothing; waits for a background job it starts (arrange, orient) |
| Project | `open_files` | load models, a project or G-code by absolute path, with no dialog |
| | `new_project` | start an empty project (`discard_changes` if it has unsaved changes); `changes` keeps or discards modified presets |
| | `calibrate` | a Calibration menu test with typed parameters and no dialog: a new project with the test model and the preset changes the test needs, printed with filament slot 1; refused while presets are modified unless `changes` keeps or discards them |
| | `save_project` | save the project as `.3mf` to a path (a `.gcode.3mf` path is refused: that is `export` `sliced_3mf`) |
| | `export` | `gcode`, `sliced_3mf`, `3mf` or `stl` to a path, by default the one its extension names; `gcode` is an export operation |
| Presets | `list_presets` | printer, process or filament presets with flags (system, user, inside the project, the preset inherited from), paged |
| | `select_preset` | select by name through the selector (filament per slot); `changes` transfers or discards unsaved changes without the prompt |
| | `get_settings` | values and metadata (description, settings page and group, enum labels, per-variant entries) by key, by page or group, or found by words matched in keys, labels and descriptions, of the edited or any saved preset, of an object's or part's overrides with the values they print with, of the project's own settings (plate type, slot colours, flushing volumes, purge mode, prime tower positions, mixed slots), of a plate's (plate type, print sequence, spiral vase, filament orders, name, lock), or of named preferences (read only); `compare` lists only what differs from another preset, the saved one or the parent |
| | `set_settings` | set values the way the settings tab does, all or none; one entry of a per-variant value; object and part overrides and filaments; the project's and a plate's own settings through their controls' and dialogs' code; `null` takes a preset's key back to its saved or parent value, `reset_all` drops a preset's unsaved changes first; replies with every setting that `changed` |
| | `save_preset` | save the preset being edited under a name, as a user preset or inside the project |
| | `delete_preset` | delete a user or project preset, selected or not, as the tab's Delete does (destructive) |
| Objects | `list_objects` | objects (filament, auto drop, overrides, brim ears, height range modifiers, variable layer height profile), instances (placement, mirrored, bounding box, canvas rectangle on request) and parts (type, filament, placement and bounding box on the bed, scaled and mesh volume, painted facets, text and SVG sources), paged |
| | `list_plates` | plates with the settings they print with and those they set themselves, objects, name, lock, slice state, prime tower position and layer G-codes, paged |
| | `select` | select objects, instances or parts (indices checked against what the object has), all, or none |
| | `transform` | translate, rotate, scale or size one instance, relative or absolute, move it to a `position` or to another plate; or the same for one part (`volume`) |
| | `add_part` | add a part, negative part, modifier, support blocker or enforcer, or precise seam to an object: a shape at a size or a model file, optionally at a position |
| | `arrange` | arrange every unlocked plate or one plate, or fill an object's plate with instances or copies, with the Arrange options panel's settings; waits for the job |
| | `set_layer_ranges` | replace an object's height range modifiers: Z ranges with their own layer height, filament and process settings |
| | `set_layer_height_profile` | the Variable layer height panel's Adaptive (with its quality), Smooth (radius, keep min) or Reset on an object |
| Slice | `slice` | current, one or all plates, then the Preview tab as after the Slice button; outcome for each plate, and for an `ok` one its time, filament totals with the flush, cost, layers, max Z, filament changes and grams per slot by use |
| | `cancel` | stop the running slice or G-code export, a pending automatic reslice, or a background job an MCP call started (optionally only a given job), or with `job` a running `encode_recording`; allowed during an operation |
| | `set_layer_gcode` | add or remove a plate's pauses, custom G-code, custom templates and filament changes on its sliced layers, as the Preview layer slider's menu does |
| View | `set_view` | tab (Design included), 3D view (Prepare, preview, assemble), camera (a direction also as `view`; the Design view's on the Design tab) and `orbit` around what it looks at by degrees, preview layers (by number or height), color scheme as the legend's combo picks it, legend rows and time estimate, window size |
| | `screenshot` | window, the 3D view shown (the Design view on the Design tab), topmost dialog or one widget: a downscaled image and a full-size PNG |
| | `get_preview` | a sliced plate's summary, time and grams per feature type, per-layer Z, height, time and grams (paged), grams per object instance, and the Preview's legend state and colour ranges |
| Recording | `start_recording` | record the main window with its modal dialogs, or the 3D view shown, into an MJPEG `.mkv`, with the current tool call as a caption and the pointer highlighted when asked for, and with `encode` an MP4 of it once stopped |
| | `stop_recording` | capture the last state, end the recording and report its file once written: length, frames, drops, the mean UI time of a capture, size and why it ended; the job of the encode `start_recording` asked for |
| | `encode_recording` | encode a recording into an H.265 or H.264 MP4 with the system ffmpeg; cancellable |
| GUI | `ui_snapshot` | widgets, rows of lists, trees and tables, popups, menus, GL toolbar items, gizmos, the Prepare view's plates and plate icons, and ImGui items, with refs |
| | `ui_click` | activate a ref or locator, click a row's cell or a plate icon, or click a point of a 3D canvas or custom-drawn widget; list the menu it opens, or run one of its items |
| | `ui_input` | set a control's text, value, selection or checked state, or a row's checkbox |
| | `ui_key` | send a key chord to a target or the focused window, whose panels' key hooks see it first; list or run the menu it opens |
| | `ui_mouse` | canvas gestures: move, down, up, click, drag, wheel, at pixels or at points of the model (the Design view at pixels); list or run a right click's menu; reports when a gesture moved the camera |
| | `gizmo_file` | what the open gizmo's file pickers do, with a path: SVG change file and save as, texture import |
| | `dialog_respond` | press a button of the topmost wx dialog |
| | `dialog_auto_answer` | answer matching modals as they open (native ones unseen), and one already open |
| | `webview_eval` | run JavaScript in a web view, the setup wizard's and preset bundle dialogs' included while they are modal, and return the result |
| Scripts | `run_script` | run a script file or inline script, with results per step |
| | `export_script` | write the session journal as a script that can be replayed; calls whose job is still running are left out and listed |
| CAD | `cad_call` | a method of the Design tab: sketches, features, reads and measurements, feature and body edits, undo, commit to plate; `describe_tools` lists them (CAD builds, Design tab enabled) |

Each tool family registers itself from its own `McpTools*.cpp` through `register_*_tools()`.
List tools take `limit` and `offset` and report `total` and `next_offset`. `McpPaths.hpp` requires
absolute paths, since a relative one would resolve against `<datadir>/log`, the GUI's working
directory; an output's directory must exist, and an existing file is replaced only with
`overwrite: true`. Paths arrive and leave as UTF-8 text, and so does Orca's `data_dir()`;
`path_from_utf8()` and `path_to_utf8()` convert, through wide strings on Windows, where boost's
narrow conversion would use the ANSI code page.

### list_objects

`list_objects` describes the model through `object_info()` and `volume_info()` (McpToolsScene.cpp,
libslic3r only, so unit tests build them from a `Model`); the handler adds what the GUI knows: each
instance's plate from the plate registry and, on request, its canvas rectangle. Bounding boxes are
`ModelObject::instance_bounding_box()`, on the bed and of model parts only, which is what prints; the
Object manipulation panel's Size also counts modifiers and negative parts. A part's `position`,
`bbox` and `scaled_volume` (the mesh's volume times the determinant of its placement) are measured on
instance 0, like the object's `bbox`; its `rotation_deg` and `scale` are in the object's frame, the
panel's Object coordinates. `mirrored` says whether a placement is left-handed
(`is_left_handed()`), not along which axis, since the split of a mirror from a rotation is not unique
(mirrored in X and in Y is a half turn about Z). `filament` follows the object list's filament
column: an object's `extruder`, 1 when unset; a part's own, else its object's; a modifier's only when
it sets one, since a modifier without one leaves the filament of what it covers alone; none for
negative parts, support blockers and enforcers, and precise seams. A precise seam has `type:
"precise_seam"` and its `subtype` (`center`, `left`, `right`, `enforced`, `blocked`, `neutral`, the 3MF
names without their prefix). The `transform` reply on a part carries the same `volume_info`. An
object with height range modifiers lists them as `layer_config_ranges` (`layer_config_ranges_json()`),
each with its whole config serialized as `get_settings` serializes values, so a range always shows
its `layer_height` and `extruder`, and the list can be handed back to `set_layer_ranges` unchanged. An
object whose `layer_height_profile` has more than a flat bottom-to-top pair, which the object list marks
as a variable layer height, reports it as `layer_height_profile: {points, min_height, max_height}`
(`layer_height_profile_json()`): the number of (z, height) points and the extreme heights, to 0.001 mm.
The points themselves stay in the 3MF's `layer_heights_profile.txt`.

### transform

`position` is where the instance or part goes, as the Move panel shows it, the same move as an
absolute `translate` whatever `mode` says for rotate and scale; add_part takes the same name for a new
part's place.

On an instance, `transform` writes the `ModelInstance` directly: a relative rotation composes as
Euler angles, and a rotation or scale applies to that instance only, where the panel applies it to
every instance of the object. `size` sets mm along
the object's own axes (its model parts' box, unrotated, times the instance's scale; a single number
with two nulls keeps the proportions), and `plate` is one more relative move by the difference of the
two plates' origins, applied last. Then the bounding box is invalidated before
`ModelObject::ensure_on_bed()`, so a scale that grows an object drops it onto the bed instead of
leaving it sunk, and the instance is registered with the plate it is on now
(`PartPlateList::notify_instance_update()`, then `ObjectList::update_plate_values_for_items()`), as
`GLCanvas3D::do_move()` does for a person's moves. Slicing and `list_objects` read plates from that
registry.

On a part (`volume`), it takes the Object manipulation panel's path. It refuses the assemble view, an
open gizmo and an object with one part (which has no row of its own), selects the part's row in the
object list with nothing else selected, so the 3D view selects it on instance 0, takes one undo
snapshot, and runs `Selection::translate()` / `rotate()` along the bed's axes and `scale()` along the
part's own axes, each followed by `do_move("")` / `do_rotate("")` / `do_scale("")`. Those write the
part, drop the object when Auto Drop is on, register every instance with its plate and update the
object list. An absolute translate is a delta from the part's position on instance 0; an absolute
scale or a `size` (mesh box times the part's scale, in the object's frame) becomes factors on the
current scale, as the panel computes them, since `Selection::scale()` multiplies a part's scale
whatever the transformation type. A part's rotation is relative only.

### add_part

`add_part` runs what the object's context menu Add items run. It refuses the assemble view, an open
gizmo and an object with more than one instance, since adding a shape folds instance 0's rotation
and scale into the parts (`ObjectList::apply_object_instance_transfrom_to_all_volumes()`), which the
other instances would not follow. It selects the object's row, which the menu's functions act on,
then calls `ObjectList::load_generic_subobject()` with `interactive` false and the size, so neither
the Primitive size dialog nor the Add Modifier tips show, or, for a file,
`ObjectList::load_subobject(type, files, ...)`: the Load... item after its file dialog, which hands
back the reason a file failed to load instead of showing the error dialog. A STEP file still asks
for its mesh options when the preference says so, as it does a person (`blocked_by_dialog`). A size
is checked against `primitive_size_names()`, the fields the size dialog asks for; without one, the
shape gets the built-in size (10 % of the bed's larger side), never a size the user saved, so a
replay does not depend on the machine. The menu's function takes the undo snapshot ("Add
primitive", "Load Part" or "Load Modifier"; a file that fails to load leaves it, as in the GUI).
`position` then selects the new part and moves it along the Move panel's path
(`Selection::translate()`, `do_move("")`), all inside one `Plater::SingleSnapshot`, so the call is
one undo step. The new part is found by pointer, since `ModelObject::sort_volumes()` orders parts
by type. SVG files are refused: SVG parts come from the SVG gizmo, which places them where the
camera looks.

### arrange

`arrange` runs what the GUI runs: with `plates: "all"`, `set_prepare_state(PREPARE_STATE_DEFAULT)` and
`Plater::arrange()`, as the toolbar does; with a plate index or `"current"`, `select_plate()`,
`PREPARE_STATE_MENU` and `arrange()`, as the plate's Arrange icon does, without its
`last_arrange_job_is_finished()` flag, which a cancelled job never clears; with `fill`, the object's
row selected and `fill_bed_with_instances()` or `fill_bed_with_copies()`, which fill the object's own
plate. Each takes one "Arrange" undo snapshot. It refuses the assemble view, an open gizmo, a locked
or empty plate, `fill` with `plates`, and an object Fill bed refuses (`can_increase_instances()`).
`spacing`, `rotate`, `align_y` and `multi_material` go into the current print mode's
`GLCanvas3D::ArrangeSettings`, which both jobs read, by the panel's rules (`apply_arrange_args()`:
Align to Y axis stays off while rotation is on), and `GLCanvas3D::store_arrange_settings()` saves
spacing, rotation and the material rule under the keys the panel writes, so later arranges, the
panel and a replaying machine's app config all see them. The journal records the settings in force,
so a replay does not depend on the replaying machine's saved options.

The call waits for the worker (see Worker jobs), then reports the settings in force with `by_object`
(which mode's set was used), instances on no plate (`off_plate`, measured against the plate boxes,
since Fill bed leaves its new instances out of the plate registry), the plates an all-plates arrange
leaves alone (`skipped_plates`: locked, or printing in another sequence than the project, which
`ArrangeJob::prepare_all()` locks for the run), notifications the job raised, and a `note` when a
setting asked for did not apply (one plate in another sequence is arranged with automatic spacing).
An arrange sorts the model's objects by arrange order
(`PartPlateList::rebuild_plates_after_arrangement()`), so indices may change. After `timeout_s`
(default 300) the call cancels the job.

### set_layer_ranges

`set_layer_ranges` gives an object's whole set of height ranges (`ModelObject::layer_config_ranges`),
which a person builds with the object list's Height Range Modifier, its "+" buttons and the range
editors in the sidebar, one gesture and one undo step at a time. Each range starts from what the object
list gives a new one, `ObjectList::get_default_layer_config()` (the object's layer height, else the
process's, and filament 0, the object's), then takes its `values`, parsed strictly. The keys are those a
range's page and the object list's Add settings menus offer (`layer_range_option_keys()`): `layer_height`,
the per-region settings with a category, without support settings or, with one filament, per-feature
filaments and wipe options, and `extruder`, the range row's filament column. A `layer_height` given is
checked against the bounds the range editor enforces (`get_min_layer_height()` /
`get_max_layer_height()` for the range's filament: the printer's per-nozzle values indexed by filament
number, so the first serves a single-nozzle printer). Ranges may touch but not overlap: the slicer would
trim an overlap silently (`layer_height_profile_from_ranges()`), and from an agent it is a mistake. A
range above the object is kept, as the GUI keeps it, and slicing cuts it; the reply gives the object's
height. All of this is checked before anything changes, and a call that gives the ranges the object
already has changes nothing and takes no undo step.

Like `add_part`, it refuses the assemble view and an open gizmo and switches to Prepare. To change the
ranges it selects the object's row, so no Layers row and no sidebar range editor stays in use, then
takes one snapshot ("Edit height ranges"), writes the new ranges, rebuilds the Layers rows from the
model with `ObjectList::reload_layer_rows()` (shared with pasting ranges) and calls
`Plater::changed_object()`. A range editor that loses focus later reports an edit of a range that is
gone; `ObjectList::edit_layer_range()` then edits nothing rather than adding the range back without a
layer height. The dialog the menu shows when the process uses mixed colour sublayers becomes a
`warnings` entry, and so does a variable layer height profile on the object, which decides its layer
heights instead of the ranges' `layer_height` (`PrintObject::update_layer_height_profile()` falls back to
the ranges only without a valid profile).

### set_layer_height_profile

`set_layer_height_profile` runs what the Variable layer height panel's Adaptive, Smooth and Reset buttons
post: `GLCanvas3D::adaptive_layer_height_profile(quality)`, `smooth_layer_height_profile({radius,
keep_min})` and `reset_layer_height_profile()`, each with the GUI's undo snapshot ("Variable layer height -
Adaptive", "- Smooth all", "- Reset"), followed by a background update and the object list's variable
mark. The defaults are the panel's (quality 0.5, radius 5, keep min off), fixed rather than read from the
panel, so a replay does not depend on what a person last set there; an argument of another action is
refused. Like the panel, it works on the selected object: it refuses the assemble view and an open gizmo,
switches to Prepare, selects the object's row, and requires `Plater::can_layers_editing()`, the rule that
enables the toolbar item (FFF, the object above the bed, the layer editing shader loaded), naming the
condition that fails. It does not open the panel. Reset of an object without a profile changes nothing
and takes no undo step.

The panel's buttons act on the object its last frame picked (`LayersEditing::select_object()`), with
the profile that frame loaded, and a closed panel has no such frame. So each canvas function first picks
the selected object (`GLCanvas3D::_select_layers_editing_object()`), dropping what an earlier call left
while the panel is closed, and `LayersEditing::smooth_layer_height_profile()` loads the object's profile
with `PrintObject::update_layer_height_profile()` (the model's profile, or the one its height ranges give)
as the frame does. With the panel open both are what the frame did already. The Prepare canvas edits layer
heights only once it has drawn, since `GLCanvas3D::init()` creates the texture `is_layers_editing_allowed()`
checks; the call waits up to 15 s for it.

Adaptive keeps a height range's own `layer_height` inside the range (`layer_height_profile_adaptive()`),
so on an object whose ranges cover its height it gives the ranges' heights at any quality. Once an object
has a profile, the profile decides its layer heights, and the reply warns when it also has ranges; it
also passes on the warning the panel shows when the process mixes colours in sublayers. A profile is
stored per object, saved in the 3MF, and discarded by the slicer when it no longer ends at the object's
top (after a scale, for example).

### set_layer_gcode

`set_layer_gcode` places what the Preview layer slider's right-click menu adds at the start of a layer
(Add Pause, Add Custom G-code, Add Custom Template, Change Filament > Filament N) and removes them, on one
plate. The codes are a plate's `Model::plates_custom_gcodes` entry, saved in the 3MF
(`custom_gcode_per_layer.xml`) and read back by `list_plates` as `layer_gcode`, with the same type names
(`pause`, `custom`, `template`, `change_filament`, and `color_change` for an old project's colour change,
which the slider never adds). The slider is the source of truth: the plater's handler of its tick event
writes the slider's whole set over the plate's entry, and the slider is not refilled while the plate's slice
is invalid, so a code written to the model alone would vanish at the next tick event. The call therefore
edits the slider with its own functions (`IMSlider::add_code_as_tick()`, `add_custom_gcode()` and
`delete_tick()`, given an explicit layer instead of the active thumb) and hands the ticks over with
`GLCanvas3D::send_layer_ticks_changes()`, what each Preview frame does, then waits for the handler: the
plate's entry rewritten, its slice invalidated, the project dirty and its revision raised.

Codes sit on the layers of the plate's slice. `layer` is the slider's value plus one, as `set_view` and
`get_preview` number layers; `z` takes the layer whose top is at or above it
(`IMSlider::get_tick_from_value(z, true)`, the rule the slider uses for a stored height), so a code is
always stored at a layer's top, where the slider and the G-code generator agree on its layer. The reply
gives both for every code. A plate's layers are in the slider only while the Preview shows that plate's
result (`GCodeViewer::last_result_id()` against the plate's slice result), so when the plate's slice is
up to date the call shows the Preview and selects the plate as the Preview's plate toolbar does
(`Plater::select_sliced_plate()`), which loads its result; a plain plate switch leaves the Preview
showing the old plate. A plate whose slice is not up to date is refused unless the Preview still shows
its layers (it was edited and not sliced again since), as a person can go on editing there: opening the
Preview on such a plate would start a slice. The call waits for a pending update of the slicing state,
which may invalidate the plate, and a slice in progress refuses it; a plate that prints by object takes
no codes, and an empty plate has no layers.

The arguments apply in order: `clear`, then `remove`, then `add`, all checked on a copy of the slider's
ticks before it changes (`edit_layer_gcode()`), with the menus' rules: one code per layer, so a code of
another type on the layer is refused unless the call removes it, and a custom code or filament change on
a layer with one replaces it (Edit Custom G-code, Change Filament on the tick); a template only when the
printer's `template_custom_gcode` is set; a filament change only with two or more filaments, when the
slider offers it (the plate's objects on one filament, no spiral vase) and while the filament count is the
slice's, since the slider colours the change from the slice's filament list; custom G-code up to the
dialog's 1023 bytes. The check matters because the slider's own add turns any code on a layer into a
filament change. A call that changes nothing hands nothing over and leaves the slice valid; a pause on a
printer with an empty `machine_pause_gcode` is a warning. As in the GUI, the edit takes no undo step (the
undo archive does not hold the plates' codes), and the slider drops codes it can't keep when it next loads
the plate: filament changes once the objects use different filaments, every code on a plate printed by
object or across a spiral vase change. The journal records the plate the call resolved.

### Object and part settings

The object and volume scopes of `get_settings` and `set_settings` take the keys the object list
offers to add to an object, or to a part or modifier: `SettingsFactory::get_options()` less what
`SettingsFactory::is_improper_category()` hides with the current filament count, the rule of the
list's Add Settings menu. Keys a person cannot add there, such as the internal `print_extruder_id`,
the process-wide `print_sequence` or, with one filament, the per-feature filaments, are refused rather
than stored and ignored by slicing. Parts and modifiers take region keys only.

`extruder`, the object's or part's filament, is the one key outside that list. `set_settings` writes
it through `ObjectList::set_extruder_for_items()`, the Change Filament code without the selection,
so a call leaves the user's selection alone. An object's filament clears its parts' own; 0 or null
is Default, which is filament 1 on an object, the object's filament on a part and no filament
override on a modifier. A single-part object has no part row, so its volume 0 sets the object's
filament, and the reply notes it. Filament numbers (`extruder`, the per-feature and the support
filaments) past the project's slots are refused before anything changes, and the support filaments
refuse a mixed slot, as their combos do. A call is one undo step (`Plater::SingleSnapshot`).

In these scopes each `get_settings` item has `effective` and `source`, resolved as the object's
settings tab shows it: the part's override (`part`), else the object's (`object`), else the process
being edited (`process`), else the option's default (`default`). An `extruder` of 0 is no override.
An object's item also lists in `parts` the parts and modifiers that override the key; their values
apply only inside their own volume, and the reply's `note` says so.

### Saving presets

`save_preset` saves the printer, process or filament preset being edited, unsaved changes included,
through `Tab::save_preset()` with the name given, the call the Save preset dialog's OK leads to, so
no dialog opens and the saved preset is selected. A filament is first made the one the filament tab
edits for `filament_slot`, as `set_settings` does, and saving it moves every slot that used the old
preset to the new one, as in the GUI. The name is checked by the dialog's own
`check_preset_name()`, and its status decides: an invalid name is refused with the dialog's text,
and the name of another existing preset needs `overwrite`, while the selected preset's own name is
the plain Save. A new preset goes where the edited one is (user or inside the project) unless
`inside_project` says otherwise; an existing one stays where it is, since the dialog locks that
choice. `detach` is refused outside Developer mode, where the dialog has no such checkbox. For a
printer, the selected physical printer, if any, takes the dialog's default action
(`SavePresetDialog::update_physical_printers()` with `ChangePreset`). Preset files live in the
datadir, not the project, so a script meant to be rerun passes `overwrite`. A filament temperature
warning, the unresolved-plugin confirmation or the unsaved-changes prompt of a slot switch makes the
reply `blocked_by_dialog`.

There is no subset of edits to save. `set_settings {"reset_all": true, "values": {...}}` on a preset
scope first discards every unsaved change with `Tab::on_roll_back_value()`, the tab's undo button,
then applies the values in the same call, so `save_preset` keeps exactly those. The reply lists the
discarded keys in `reverted`. The values are parsed before the reset, so a bad one leaves the edits in
place. The object and volume scopes refuse `reset_all`; a null value removes an override there.

### Reading, comparing and changing preset values

`get_settings` reads the preset being edited (for a filament, the slot's preset, edited or not), or
with `preset` any saved preset of the scope, selected or not, without selecting it: the named preset
is read as saved, without the edits the tab holds. `preset` and `filament_slot` are exclusive, and a
name that matches no preset is refused with the closest names. `compare` lists only the keys, among
those named or matched, that differ from another preset: a saved preset by name, `:saved` (the read
preset as saved, so the list is its unsaved changes, what the Unsaved Changes dialog shows) or
`:parent` (the preset it inherits from, a system preset itself, what the tab's lock icons compare
against). `:` cannot be part of a preset name, so the two never clash with one. The key list is
`PresetCollection::dirty_options()` with the Compare presets dialog's rules: printer and filament
vectors are compared entry by entry (an item then carries `index`), a printer with fewer extruders is
compared from the other side, and both presets have one printer technology. Each item has `value`
(the preset read) and `other`; the bookkeeping `*_settings_id` keys are left out. `list_presets`
reports `inherits` and `project: true` for presets kept inside the project.

An enum's meta lists `labels`, the untranslated texts its combo shows, in the order of `values`, so
a person's wording maps to a value. A preset scope's whole listing (no `keys`, `query` or `where`) leaves
meta out unless `include_meta` asks for it: a page of a hundred keys with meta runs to about 50,000
characters, and an agent that lists everything is scanning values; the keys it then names or searches
for come with their meta.

What a setting does comes with its value. Meta has `description`, the option's whole untranslated
tooltip (what it does, when to change it, what it affects), and `where`, the page and group of its
row on the settings tab. `where` comes from the sidebar's settings index (`Search::SettingsIndex`),
which the settings tabs fill as they build their pages and the Speed Dial reads for its own setting
actions; a setting with no row on a page has none, and an object's or part's settings are placed on
the process tab. The help text sits in `get_settings` rather than in a tool of its own because that is
where agents look: in the bench, agents read settings with `get_settings` three to five times as often
as they called a separate explanation tool, and with the tooltips out of the meta they set a draft
shield without skirt loops and a support threshold measured from the wrong side, mistakes the same
agents avoided when the tooltip came with the value. The size stays bounded by the rule above: only
keys that are named, searched for or listed with their group carry meta.

`where` lists one group ("Strength > Walls") or a whole page of the scope with values and meta; a
name the scope has no page or group for is an error that lists them all, which is the map of the
tabs. `query` serves an agent that knows what a setting should do but not its name
(`rank_settings()`). A query word matches a word of a key, label or description that starts with it,
so "retract" finds "retraction" while "to" does not match inside "toolchange"; words under three
letters count only when the query has nothing longer, since most help texts have them. Settings
matching more of the query's words come first, then those matching more of them in the key or label.
With no weighting by how rare a word is, a generic word such as "reduce" counts as much as
"stringing", so the tool asks for a few distinctive words. A search returns the best 20 unless `limit`
asks for more. Without a `scope` it looks through the process, filament and printer settings being
edited, since a question about an effect rarely knows which tab answers it (stringing is retraction on
the printer tab and its overrides on the filament's), and each match names its `scope`.

Many process, filament and printer values hold one entry per extruder variant (an extruder type and
nozzle flow type, such as the X1C's "Direct Drive Standard" and "Direct Drive High Flow"), the machine
limits a normal and a silent mode entry per variant, and some printer values one entry per extruder.
For such a value with more than one entry, the meta lists `variants`, what each entry is for, and
`active`, the entry each extruder prints with (extruder 1 first), found as the slicer and the tab's
variant switch find it (`DynamicPrintConfig::get_index_for_extruder()` with the printer's
`extruder_type` and the project's `nozzle_volume_type`). The tab edits one variant at a time: its
field shows the variant in use and typing writes that entry. So a single value for such a key goes to
the entries in use, `set_settings` writes one entry as `{"key": {"index": i, "value": v}}`, and an
array writes them all. Either way the whole vector, with the entries replaced, goes through the tab's
transaction (`Tab::apply_preset_config_values()`), whose commit runs the key's handling as a field edit
does, and the other entries stay as they are. A value with another number of entries is refused, with
the entries and the one in use in the error and its details: written as given, it would collapse the
vector and leave the preset inconsistent with its variants. Values of one entry, and vectors that are not
per variant or per extruder, take a single value. Object and part overrides take whole values.

`set_settings` replies with `changed`: every key of the edited config (preset, object or part) whose
value differs after the call, including the settings the app adjusted in turn, such as spiral vase
setting one wall loop and no top shell layers once its dialog is confirmed, and the keys `reset_all`
reverted. Dependency order within one call is not resolved; a call can be split instead.

### Unsaved preset changes

On a preset scope, a `null` value takes the key back to the saved preset's value, as the key's undo
arrow does, and with `reset_to: "parent"` to the value of the preset it inherits from (the `:parent`
of `compare`), as its lock icon does. The value joins the call's other values in the tab's
transaction, so its handling runs as for a typed value. `reset_to` without a null value, and in the
object and volume scopes, is refused.

`Tab::select_preset()` asks about unsaved changes through `Tab::may_discard_current_dirty_preset()`:
for the preset it leaves, and for each modified preset a process or printer switch may replace (the
filament, or the process and filament). `select_preset`'s `changes` sets `Tab::m_dirty_preset_answer`
for the one switch, so every prompt is answered with the dialog's Transfer or Discard without
showing it, and the switch then does what it does after a person's answer. Transfer moves what the
dialog would list: the changed options a settings page shows in the current mode, so a setting the
mode hides is dropped, as in the GUI. The dialog offers no Transfer for a printer's own changes or
between filaments of different types (`Tab::can_transfer_changes_to()`); there `transfer` is refused
before anything changes. The GUI's outcomes stay as they are: a modified filament that suits the new
process keeps its changes whatever the answer. Without `changes` the prompt opens, as it does for a
person, and the reply is `blocked_by_dialog`. The message box that a printer switch shows when it drops
other extruder variants' values is shown only to a person. With several filament slots the sidebar does
not move the filament tab when a slot's filament changes (`Plater::priv::on_select_preset()`), so no
prompt opens there: the edits stay with the preset the tab edits, and the reply's `modified` lists them.

`new_project` without `changes`, or with `"keep"`, keeps modified presets for the new project, as
New Project's Keep does with every option. `"discard"` is the prompt's Discard,
`GUI_App::discard_current_preset_changes()`, called only when a preset is modified, since it reloads
the presets, which can ask about the printer's. Modified presets make the project dirty, so
`discard_changes` is needed too.

Both replies carry `discarded` and `modified` when presets had or have unsaved changes, each
`{type: {name, keys}}`: a change is discarded when the preset edited afterwards no longer holds its
value, and modified lists the unsaved changes left. Only presets that `list_presets` calls modified
count, with the keys of their preset type; the full comparison also lists bookkeeping keys that only
one of the two configs has.

### Deleting presets

`delete_preset` deletes a user preset or one kept inside the project, with its file and its cloud copy,
through the GUI's own delete, so no confirmation opens; it is marked destructive, so clients that gate
such tools ask first. What may be deleted is checked first, as the GUI offers and checks it: the tab
shows its Delete button only for user and project presets, refuses a base preset that other presets
inherit from (they would no longer load), and for a custom printer (a base printer that is not a system
one) asks before deleting the user presets made for it alone, which `delete_attached` answers. The
reply lists those in `attached`.

The selected preset (for a filament, the one the filament tab edits) goes through
`Tab::delete_preset()`, the Delete button, with `ask` false to skip the confirmations MCP has made
itself. The tab then selects the preset it inherited from, or the next one, through the normal
selection, so a printer or process switch can still ask about other presets' unsaved changes, and the
reply is then `blocked_by_dialog`; the deleted preset's own changes go with it. With a physical
printer selected the tab's Delete only takes the preset off that printer, so that case is refused. A
filament the tab does not edit is deleted as the Edit filament dialog deletes it
(`delete_unselected_filament_preset()`): the selection stays and the slots that used it take the
selected filament; the sidebar and tabs are refreshed as the dialog's opener does. The tab deletes
only its selected preset, so a process that is not selected is selected, deleted and the previous
process selected again, as a person would do it. A printer is not: switching printers recomputes the
process and the filaments, so the call is refused until the printer is selected. Both routes select
the preset being edited again, which would drop its unsaved changes (filament) or ask about them
(process), so they are refused while it has any.

The reply carries `selected`, the preset of that type selected afterwards, `filaments`, each slot's
preset, and `discarded`/`modified` as `select_preset`'s. A replayed script needs the preset to exist,
so scripts create it first with `save_preset`.

### Project settings

The project scope of `get_settings` and `set_settings` covers the settings of the project itself
(`PresetBundle::project_config`) that a person sets in the sidebar, on the canvas or in their dialogs:
`curr_bed_type`, `filament_colour`, `flush_volumes_matrix`, `flush_multiplier`, `prime_volume_mode`
and `wipe_tower_x`/`wipe_tower_y`, plus the read-only `filament_is_mixed` and, on a printer with
several extruders, the read-only `filament_map_mode` and `filament_map`. A generic config write would
skip what each control does after setting its value, so `set_settings` writes every key through that
control's own code. Everything is checked before anything changes, `null` is refused (project values
always exist), and `{"index": i, "value": v}` sets one slot's colour, one matrix cell or one plate's
tower position.

- `curr_bed_type` goes through the plate type selector (`Sidebar::set_bed_type_accord_combox()`, whose
  handler updates the project, the printer's remembered plate type and the plates on the global type).
  Only the types the selector lists are taken (`meta.offered`), and none while the selector is hidden
  for the printer.
- `filament_colour` is one `#RRGGBB` per slot in config order. Each changed slot goes through its
  colour button's code: `PlaterPresetComboBox::sync_colour_config()` and the queued
  `EVT_FILAMENT_COLOR_CHANGED`, which recalculates that slot's flushing volumes when the "Auto flush
  after changing" preference is on.
- `flush_volumes_matrix` and `flush_multiplier` go through `apply_flushing_volumes()`, the Flushing
  volumes dialog's OK split from the dialog, then the button's scene reload and update. The matrix is
  N x N per nozzle (row = from slot, column = to slot) in config order over every slot, as
  `project_config` and the G-code header hold it, not over the physical slots the dialog shows. The
  values are before the multiplier: the header prints the matrix multiplied by it, and the dialog
  shows and edits each entry multiplied by it, so its meta's `description` (the option has no tooltip)
  says so, since agents otherwise type the dialog's value as the stored one. Volumes are limited
  to `g_max_flush_volume` / multiplier as the dialog limits them; negative volumes and a non-zero
  diagonal are refused. A multiplier is one per nozzle (one value sets all) within the dialog's 0..3.
- `prime_volume_mode` is what the Purge mode dialog's OK writes, followed by the update it does. It is
  offered only while the Purge mode button shows (printers with several nozzles per extruder or fast
  purge), with the dialog's choices.
- `wipe_tower_x`/`wipe_tower_y` are one position per plate, in mm from the plate's origin, as a tower
  drag stores them: one undo step ("Move object", which takes the old positions along), then an
  update. A position off the plate is refused; one where the tower does not fit is re-placed by the
  scene reload, as after a drag, with its notification, and the reply's `note` says so.

Colours are applied in a first UI closure and the rest in a second after a settle, so a matrix given
in the same call is written after the recalculation the colour change triggered, and a matrix cell
given by index is set in the matrix that recalculation left. The recalculation also runs when a slot
is added and, depending on the preference, when a slot's preset changes, so a matrix is best set
last; a replay matches only on a datadir with the same preference.

A mixed filament is a virtual slot made of physical ones (the sidebar's mixed filament list); its
colour comes from its components and it is never flushed. `filament_is_mixed` tells which slots they
are, and their colours and matrix rows and columns are refused unless left as they are, as the
dialog keeps them. The filament grouping is set per plate in the Filament grouping dialog, and every
plate carries its own mode from the start, so a project-wide `filament_map_mode` or `filament_map`
would not change a slice: both are listed for reading only. Nothing here except the tower position
enters the undo stack, as in the GUI.

### Plate settings

The plate scope of `get_settings` and `set_settings` (`plate`, the current plate by default) holds what
the Plate Settings dialog sets on a plate: `curr_bed_type`, `print_sequence`, `spiral_mode`,
`first_layer_print_sequence` and `other_layers_print_sequence`, plus the plate's `name` and `locked`, which
its icons set. Values take the forms the dialog offers: an enum value, a boolean, a filament order that
lists every slot once (the dialog's drag canvas), and layer ranges `[{from, to, order}]` starting at layer
2 (the first layer has its own order), sorted and not overlapping, `to` left out for the dialog's "End".
`null` is the dialog's "Same as Global" or "Auto". The dialog's Auto/Customize choices are not separate
keys: an order given is Customize. `get_settings` lists each key with `value`, the plate's own (null where
it follows the global setting or Auto), and `effective`, what the plate prints with.

`set_settings` checks everything first, then selects the plate as its settings and name icons do and
passes the plate's values, changed by the call, to `Plater::apply_plate_settings()`, the dialog's OK
split from the dialog. It is called only when one of them changes, since the OK marks the project dirty.
The plate types are those the sidebar's selector offers (`meta.offered`), none while it is hidden. The
custom filament orders are refused while a slot is a mixed filament, where the dialog greys them out.
Turning spiral vase on asks the dialog's question about the settings it changes, so the reply can be
`blocked_by_dialog`. `locked` goes through `PartPlateList::lock_plate()` as the lock icon does, and only
when it differs, so setting it twice leaves the plate locked (the palette's `plate_toggle_lock` stays a
toggle). `name` goes through `Plater::rename_plate()`. The call is one undo step (`Plater::SingleSnapshot`;
the dialog itself takes none, the plate tab one per change). The journal records the plate the call
resolved, so a replay sets the same plate. The reply has `applied` (the plate's own values), `effective`
and `changed`. The plate tab's `skirt_start_angle` and the per-plate filament grouping are not in the
scope.

`list_plates` reports every plate with these values as it prints them, `overrides` (the keys it sets
itself), `current`, `objects` (`[object, instance]`, as `list_objects` places instances), `sliced` (an
up-to-date slice), `wipe_tower` (the project's tower position for the plate) and `layer_gcode`, the
plate's entry of `Model::plates_custom_gcodes`: the Preview slider's ticks, pauses, custom G-code,
templates and filament changes, each `{z, layer, type, filament, color, gcode}` by height. `z` is rounded
to the G-code's 0.001 mm. `layer` is the number the slider shows for the code, found by the slider's rule
for a stored height (the first layer at or above it, `IMSlider::get_tick_from_value()`) in the layers of
the plate's slice result, numbered and placed as the viewer loads them (`gcode_layer_zs()`); it is given
only while that slice is up to date and no slice is running, and it is left out for a code above the top
layer.

### Preferences

The preferences scope of `get_settings` reads the app configuration's `app` section, where the
Preferences dialog keeps its settings: `{"scope": "preferences", "keys": [...]}` returns each key's value as
stored (`"true"`, `"1"`, an enum's stored text) and `unknown_keys` for keys the section does not hold.
Keys are required, since the section also holds account and device state and nothing outside the dialog
lists which keys it shows. `set_settings` refuses the scope: a preference changes in the dialog, whose
handlers carry its effects (dark mode, the language, which recreates the GUI, the network plug-in, stealth
mode, render settings redrawn after the dialog closes, and this server's own `mcp_enabled` and `mcp_port`,
which close the endpoint the call came through). Writing the key would skip all of them.

The dialog's controls are named after their AppConfig keys, so `ui_snapshot` shows the key and
`ui_input {"target": {"name": "export_statistics", "within": "Preferences"}, "value": true}` changes one
through its own handler. The Speed Dial's `open_preferences` takes `<tab>[:<key>]` (`general`, `control`,
`graphics`, `online` or `developer`) and opens the dialog there through `GUI_App::open_preferences()`,
scrolled to the key's control and focused; a key on another tab is shown on its own tab. Checkboxes,
number and text fields and the language combobox change this way. The other comboboxes (those of
`create_item_combobox()`, the log level, the login region and the network plug-in version) save from a
handler on their drop-down list, where a person's pick arrives, while `ui_input` sends the selection to the
combobox itself: it shows the new choice and saves nothing.

### Calibration tests

`calibrate` runs a Calibration menu test (temperature, max volumetric speed, pressure advance as tower,
line or pattern, flow ratio in its four passes, retraction, VFA, cornering, input shaping frequency and
damping) one level below its dialog: it calls the same `Plater::calib_*` the dialog's OK calls, with
the `Calib_Params` the dialog would build, and never shows the dialog. The dialogs only collect and check
numbers, so their rules are shared with the tool in `libslic3r/calib.cpp`: `calib_default_params()` (the
values each dialog opens with), `check_calib_params()` (what each dialog refuses, as an enum, so each
dialog keeps its own message box and the tool states the rule with the values given),
`cornering_end_limits()`, `has_junction_deviation()`, `belt_temp_tower_asset()` and the VFA dialog's
auto-adjust, `fit_vfa_speed()` with `vfa_candidate_layer_heights()`. The input shaper types offered per
firmware come from `shaper_type_values()` beside the dialogs. A parameter the test does not take is
refused, so a misplaced argument never passes silently.

Two choices that only refill the dialogs' fields are not parameters: the pressure advance defaults follow
the printer's `extruder_type` (the dialog's Extruder type radio, which starts at Direct Drive whatever the
printer), and the temperature dialog's filament type is replaced by the range itself. Temperatures are
clamped to 155-500 °C and rounded down to multiples of 5, as the dialog's fields do when they lose focus,
with a warning naming the corrected range (the field's message box says 170). The input shaping Y range,
and the damping test's Y frequency, default to the X one; RepRapFirmware, whose dialogs show one range,
refuses them. VFA's Yes/No questions are answered Yes: the reply's `adjusted` gives the lowered layer
height or end speed, and a warning says when the upper blocks stay above the filament's limit.

Every test starts a new project, whose prompts the tool answers itself (`skip_confirm` on each
`calib_*`): a project with unsaved changes needs `discard_changes`, and modified presets need `changes`,
`"keep"` to run the test with them or `"discard"` to drop them first through
`GUI_App::discard_current_preset_changes()`. Unlike `new_project`, which keeps modified presets silently,
`calibrate` refuses without `changes` and lists the modified keys: a previous test's settings would
otherwise spoil this one. With `"discard"` the test's rules and defaults come from the saved printer
preset, which is what remains. The tests write their filament settings to the preset the filament tab
edits, while the test objects print with slot 1, so the tool first points the tab at slot 1's preset
(`edit_filament_slot()`, the switch `set_settings` uses for a slot), as a person must; the reply's
`presets_changed.filament` says `filament_slot: 1` and `tab_switched_from` when the tab moved. A tab that
edits another slot's modified preset is refused with `"keep"`. `run_action calib_*` keeps the GUI's
behaviour.

The reply has `test`, `project`, `params` (the values used, in the tool's units: max volumetric speed in
mm³/s, a pattern's default acceleration and speed read back from the process), `objects`, `names`,
`plates`, `presets_changed` (per preset type, the settings the test changed, as `set_settings`'
`changed`), `result_keys` (the scope and keys where the measured value belongs), `warnings`, `adjusted`,
and `discarded` and `modified` as `new_project` reports them. The journal records the call with its
effective parameters, defaults included, so a replay does not depend on the dialogs' defaults; VFA's end
speed is recorded as asked and a pattern's default acceleration and speed are left out, since both are
computed from the presets again on replay. To keep a measured value, the agent leaves the test with
`new_project {"discard_changes": true, "changes": "discard"}`, sets the value with `set_settings` and saves
it with `save_preset`; staying in the test project, `set_settings {"reset_all": true, "values": ...}` on
the preset's scope drops the test's settings there and sets the value in one call. The Speed Dial's `calib_*` commands still open
the dialogs, and their descriptions point at `calibrate`.

### Design tab: `cad_call`

`cad_call {method, params}` reaches the Design tab's control surface (`CAD/McpControl.cpp`), the
same methods the `ORCA_CAD_MCP` socket serves, as one generic tool rather than a schema per
method, so agents that never open the Design tab pay nothing for it. It needs the CAD feature on
(Preferences > Features), which the app reads only at startup. One table, `kCadMethods`, declares
every method's name, access and handler; the dispatcher, the busy gate and `describe_tools` all
read it. `describe_tools` is the parameter schema of record: one row per method with typed params,
units and defaults, and its access. `params.method` (a name or a list) and `params.query` (text in a
name or summary, any case) narrow the reply; an unknown name fails with the close ones.

Access decides what `DesignPanel::mcp_busy()` lets through. `read` methods run under an open
feature card or sketch session, `sketch` methods drive the live sketch session, and `change`
methods are refused under either, as the GUI would not let the edit land there. Neither `change`
nor `sketch` methods run while a body Move is open, since its session holds a body index that a
change could point at another body. Nothing runs while a rebuild's worker owns the document.

A method that fails is an error (`CadCallError` → `isError`) and leaves the design as it was, so
`export_script` drops it and a script never replays a call that did nothing. That holds for
argument errors and for a rebuild the kernel rejects alike. The exceptions are the older methods
`describe_tools` lists in `legacy_ok_false` (`fillet`, `chamfer`, `boolean`, `pattern`, `mirror`,
`shell`, the surface methods, `import_step`, `set_variable` and others): they report a rejected
rebuild in their reply, `{ok: false, error}`, after the same rollback, so such a call is journaled
and replays the same rejection. `extrude`, `revolve`, the `hole` methods, `split` and `sketch_add`
are not on that list, so none of them fails two ways depending on its arguments. The feature
methods off the list add through `add_feature()`: one checkpoint, a rebuild, and on failure
`CadDocument::rollback()`, which restores the checkpoint without leaving the refused recipe on the
redo stack and with the redo branch the checkpoint cleared, then "Edit rejected" on the status line
as for a refused GUI edit. Features made through `cad_call` are named as the cards name them
("Extrude 3"), from the panel's own counter.

`generation` guards face and edge ids within a session, but its value depends on the session's
history, so the journal records `cad_call` without it and a replay rebuilds the same ids.

The plane-taking methods (`sketch_begin`, `extrude`, `revolve`, the `hole` methods, `thread`,
`split`, `pattern`, `mirror`, `helix`, `project`) share one resolver, `cad_resolve_plane()`: `"XY"`,
`"XZ"` or `"YZ"` through `modeling_origin`, `{"datum": i}` for a Plane feature, or
`{"body": b, "face": f}` for a planar face. A face plane for sketching is
`CadDocument::plane_of_face()` (outward normal); for drilling it is
`CadDocument::inward_plane_of_face()`, the Hole card's plane (normal into the body, x along the
longest edge), and the hole or thread goes into that face's body. Their replies carry the `frame`
used. Because a sketch's base plane passes through `modeling_origin` like every feature's, a sketch
lands where the features drawn on the same plane land, and the coordinates in a script are relative
to the modeling origin on any printer.

`describe_scene` reads the document: the features, each with the fields an expression drives
(`bound`), the variables with their values (all null, with `variables_error`, when one does not
evaluate), the bodies with the feature that made them, whether their eye is on, the colour the
canvas draws and whether the Move gizmo placed them, the undo and redo depth, mate conflicts and the
document's `auto_close_loops`. `describe_feature {feature}` reads one feature as its card shows
it, from one per-type table, `feature_card()`: `params`, the `editable` ones named as the
`CadFeature` members (the names `set_feature_expr` binds) with the `ranges` of the card's spin
controls, `expr`, the structure the card keeps in `refs` (feature and body indices, face and edge
ids), a sketch's plane, entities and constraints (snake_case types, point references with a role,
angles in degrees), and `used_by` / `bodies_made`. `used_by` comes from the kernel's own reference
visitors, `CadDocument::for_each_feature_ref()` and, for a datum plane, `for_each_datum_ref()`,
the lists that deleting and moving a feature remap.

`set_feature {feature, fields}` (also read as `index` and `params` or `values`, the names agents
reach for, as `describe_feature` reads `index`) is a card's Confirm in edit mode: one checkpoint, then
`CadDocument::replace_feature()` with a copy whose fields `cad_apply_feature_fields()` checked
against the same table, so it takes only what the card edits (numbers within the card's range, and
its choices). A field an expression drives is refused, since the next rebuild would overwrite it,
and so is an end condition that needs a pick the feature does not hold. A rebuild the kernel
refuses fails the call after `rollback()`. Numeric fields go through
`CadDocument::set_numeric_field()`, the allow-list expressions use, so a literal edit and a binding
reach the same fields. An empty `expr` removes a binding (`set_feature_expr`) or a variable
(`set_variable`), as the Variables panel's Clear and Remove do; a binding on a field the evaluator
does not know, or removing one that is not there, is refused.

A sketch goes from a session to a solid in typed calls. `sketch_begin {plane}` opens the session
through `DesignPanel::begin_sketch_session()`, the code a plane or face pick runs, without the tool
menu that follows the click. `sketch_commit` is the sketch's ✓, whose commit handler adds the Sketch
feature and solves its dimensions; the reply reads the committed feature back: `sketch_index`, its
name and its loops. An empty sketch is refused before anything changes and the session stays open,
because an exported script leaves the failed call out and its next sketch call needs that session.
`sketch_describe {sketch}` describes a committed sketch as the open one is described, through
`DesignSketchTool::loop_report(entities)`. Its loop indices are those `extrude {loop}` takes: a
region lists the loops inside it under `holes`, and each of those is a loop of its own.
`extrude {sketch}` and `revolve {sketch}` are the cards' Confirm on a committed sketch. Extrude
takes the whole sketch (`add_extrude`) or, with `loop`, one region and the loops inside it, the list
a loop picked in the viewport hands the card (`add_extrude_entities`). Its `boolean: "auto"`, the
default with a sketch, joins the body the sketch touches (`CadDocument::body_touching_sketch()`),
else makes a new body, as the card's default does; `body` names the body to work on in every form.
Revolve's axis is a line of the sketch (`axis_entity`), else the sketch's only centerline, which the
card also offers first (`DesignPanel::default_revolve_axis_entity()`), else a plane axis.
`cad_sketch_extrude()` and `cad_sketch_revolve()` check every argument before the checkpoint and
return the call that adds the feature.

Inside the session, the sketch's own tools are reached without a pointer through
`cad_sketch_call()`, which needs only the live `DesignSketchTool` and checks every argument before
it changes anything. `sketch_add` takes, besides lines, arcs, circles and points, the shapes the
drawing tools build: a polygon (by its circumradius, or across the flats, the tool's inscribed and
circumscribed fits), a slot, an ellipse, an elliptical arc and a spline. Their fields are those
`sketch_describe` reads back (`radius`, `rminor`, `rotation`, `ctrl`); each is turned into the clicks
its tool takes and built by the tool's own builder and shape grouping
(`DesignSketchTool::add_shape_scripted()`), leaving the tool's sticky polygon and construction
settings alone. Angles are radians, or degrees under a `_deg` name. `sketch_fillet`,
`sketch_chamfer`, `sketch_trim` (with `extend`) and `sketch_transform` (move, rotate, scale, array,
polar array) run the edit tools' own apply steps through scripted entry points, so they keep the
constraint upkeep a click gets: a fillet arc bound coincident and tangent to its legs, constraints
an op invalidates released, array copies bound to their source. They leave the armed tool armed,
and are refused while a gesture or a value field is pending, which the tool's apply step would
take over. `sketch_trim` picks the entity nearest `at` within a `tolerance` in mm, where the
scissors' click uses a pixel radius. `sketch_constrain` is a CONSTRAIN bar button: with
`entities` (else the selection) the bar's planner, `plan_entity_constraint()` with
`write_plan_value()` in `SketchEngine`, shared with the panel, chooses the points, and the value it
would ask for defaults to the current one; with `refs` the references are taken as written, in the form
`sketch_constraints` and `describe_feature` report them (`{entity, role}` for a point, `{entity}` for
a whole entity, `"origin"`, `"x_axis"` and `"y_axis"` for the sketch's implicit references), which
also reaches the types the bar has no button for (distance, lock, point on line or object). A
constraint the sketch cannot solve with is rolled back and the call fails; a refused pick names the
planner's reason as a code. `sketch_unconstrain` is a constraint row's delete button; the later
indices move down. `sketch_constraints` reads the open sketch's constraints, or a committed one's
by `sketch`. Live sketch edits are not on the Design undo stack until the commit, as in the GUI.

`query_topology` reads a body from one enumeration (`faces_of()`, `edges_of()`, whose ordinals are
the ids every other method takes). A face's kind is its OCCT surface type, from
`GeometryEngine::surface_info()`, with its area and the numbers that define it: a cylinder's radius
and axis, a cone's apex, axis into its opening, half angle and the radii the face reaches, a
sphere's centre and radius, a torus's centre, axis and radii; `internal` marks a bore or a
countersink. Edges are lines, circles, ellipses, B-splines, Béziers or other. Filters narrow the
list they apply to: `kinds` (the surface's noun, `plane` or `cylinder`, standing for its kind),
`at_extreme` (the face on one side of the bounding box) and `direction`
(a planar face's normal) for faces; `edge_kinds`, `direction` (a line) and `convex` for edges. When
filters are given, a list none of its filters names is empty, or with `adjacency` holds the
neighbours of the other list's matches, so `{kinds: ["conical"], adjacency: true}` returns the cones
with their edges. Adjacency comes from `edge_faces()` (one face for a seam), and convexity from
`edge_convexity()`, OCCT's fillet classification (`ChFi3d::DefineConnectType`). `limit` and `offset`
page both lists alike; `face_count`/`edge_count` stay the body's totals and
`faces_total`/`edges_total` count what matched.

`measure` keeps `distance`, between the refs' representative points (a face's centroid, an edge's
middle sample), and adds `min_distance` with the closest points (`GeometryEngine::min_distance()`,
OCCT's `BRepExtrema_DistShapeShape`), and `parallel_gap` across two parallel planar faces. Each ref
may name its own body, and `{"face": f, "axis": true}` stands for a cylinder's axis over the face's
length, so a hole's height or spacing is one call.

The hole methods drill a face plane's own body, or the body `body` names, else the last body, and
take `at` instead of `x`/`y`: a world point projected onto the plane along its normal (the reply's
`off_plane` says how far), or a cylindrical face or circular edge whose axis the hole shares, which
replays on any printer. `cad_hole_placement()` resolves all of it before the checkpoint. `extrude
{face}` is the Extrude card's push/pull of a picked planar face (`add_extrude_face`), joined to that
face's body by default. `plane` is the Plane card's Confirm: every construction (offset, angle,
midplane, tangent, two edges, coincident) with its references as `{body, face}` or `{body, edge}`,
checked by `CadDocument::datum_plane_refusal()`, the card's own check, before the checkpoint; a
parameter the construction would ignore is refused.

`thread`, `sweep`, `loft` and `split` are their cards' Confirm. `thread` takes a cylindrical face or
a circular edge, as a pick sets the Thread card: `CadDocument::thread_target()`, which the card's
pick also runs, puts the thread on that cylinder's axis from its base, on that body, internal for a
bore. Without a `designation` the size is the standard nearest the cylinder,
`nearest_thread_standard()`, the card's inference: a bore is matched on the minor diameter, a boss
on the major, over ISO metric and UTS together, so a 5 mm bore infers 1/4-20 UNC. On a plane the
thread is placed as a hole is (`cad_hole_placement()`), needs `internal`, and either a designation or
a custom `diameter` and `pitch`; the depth is the 60 degree V's for the pitch and role unless given.
`sweep` takes a sketch or a Helix feature as its path, as the Sweep card lists both; along a helix
the kernel keeps the profile at a fixed angle to the helix axis (`SketchEngine::make_sweep` with a
binormal), as a spring's or a thread's wire is drawn. `helix` replies with its `start` and
`tangent`, where a profile is drawn across the curve. `loft` takes two or more sketches in order.
`split {plane}` is the Cut card's feature (`add_cut`) with the card's plane forms plus a face plane,
`offset`, `flip` and the kernel's `keep_upper`/`keep_lower`, which the card always leaves both on;
`split {face}` splits along a face's plane. A plane clear of the body is refused, since the kernel
would leave one piece empty. The reply names the pieces: the upper one keeps the body's index and
the lower one follows it.

Thread, sweep and loft rebuild as their cards do, through `DesignPanel::recompute_guarded()`: the
kernel works on a worker thread while the UI thread waits in `wxYield()` behind a progress dialog
that appears after 300 ms. A long thread or sweep takes seconds, and meanwhile the window paints
and inspection tools answer from inside that loop: `get_state`, `ui_snapshot` (the dialog is a
non-modal `wxProgressDialog` titled "Design") and `screenshot`, of the window or of the dialog's own
ref. On Windows the dialog is the system's task dialog, run on a thread of its own, so it is no wx
window and `ui_snapshot` does not list it; the other tools answer as elsewhere. Mutating calls are refused by `UiInvoker` as running inside `cad_call`, and the `ORCA_CAD_MCP`
socket's requests by `mcp_busy()`, since the worker owns the document. A call past the reply budget
becomes a running job; the socket waits on such a request past its 15 s queue timeout once it has
started.

`undo` and `redo` are Edit > Undo and Redo with the Design tab shown (`menu_undo_redo()`, behind
the same `menu_can_undo_redo()` gate): the Design history, or inside a sketch session the sketch's
own, which takes off or puts back the last drawn shape. `run_action undo` stays Prepare's history.
They have sketch access, so they run in a sketch session, and like every changing method they are
refused under an open card, Constrain mode or a pending body Move. The Design history is not saved
with a project.

The Feature tree's and Bodies list's edits are split from their handlers into index-taking
`DesignPanel` cores that the handlers call after resolving the selected row, so `feature_delete`,
`feature_move` (one place up or down), `feature_set_enabled`, `feature_rename`, `body_set` and
`body_delete` run the GUI's own code without selecting a row, and queue no face highlight of their
own. Each tree edit is one Design undo step; when the kernel refuses one (a consumer moved above
its input, a feature shown without the body it works on, a delete a later feature cannot survive),
the kernel has already put the features back, `CadDocument::abandon_checkpoint()` drops the
checkpoint and restores the redo branch it cleared, and the call fails with the kernel's reason.
A feature rename is its own undo step. `body_set {body, name?, color?, visible?}` checks every
argument before it changes anything; a body's name and colour are saved in the recipe and are not
in the undo history, and its visibility is view state that decides what `commit` sends.
`body_delete` deletes the feature that made the body (`CadBody::source_feature`). Deletes, moves and
hiding renumber features and bodies, so the replies carry the new counts and `generation`.

`commit {mode, replace}` is Commit to Plate: `DesignPanel::commit_to_plate()`, which the button
also runs, sends the visible bodies (a lone body even when hidden) at their Move-gizmo positions,
as one Prepare undo step, and shows Prepare. A body named in the Bodies list names its object, or
its part of an assembly; otherwise the design's labels ("Design Body", "Design Assembly") are used.
`mode` applies to this call only and leaves the split button and `design_commit_mode`, the user's
choice, as they are. `replace` lists Prepare objects to delete afterwards through the Object
List's delete path, under one outer snapshot so the commit and the deletions are a single undo
step. It refuses before anything changes: an index out of range or repeated, an object that is
part of a cut (deleting one asks the person first), an open gizmo, a design with no body, or every
body hidden. The reply gives the new objects' `list_objects` indices after the deletions.
`commit` is the one method that changes Prepare's model, so the `ORCA_CAD_MCP` socket refuses it:
the socket has no mutate guard, no modal check, and runs requests inside any nested event loop.

`new_design {discard_changes}` is New Design without its confirmation: `clear_document()`, which
also clears the Design undo history and leaves committed objects on the plate. A design with
features, bodies or variables needs `discard_changes: true`. `export_step {path, overwrite}` is
Export STEP… without its file dialog (`DesignPanel::export_step_to()`, shared with it): every
body as native B-rep at its Move-gizmo position, under the server's output-path rules. The
socket takes it with the same checks. `import_step`, `import_mesh` and `validate_against`'s
`reference.step` take an absolute, existing path, like every file the server reads.

`set_view {tab: "design"}` shows the Design tab, and while it is shown `camera` points the Design
view as its own controls do: a direction through `DesignCanvas::set_view()`, `default` its Home
view (iso, fitted), `fit_objects` its `fit_view()`, and `fit_selection` the pick's box, refused when
nothing is picked. The camera is applied in a second UI closure after `settle()`, because the tab
takes the shared camera only when its page shows (`DesignPanel::on_tab_shown()`). `fit_bed`, `view`
and the Preview fields are refused with the Design tab, since they show Prepare or Preview. The
Design view is a canvas target for the `ui_*` tools and screenshots (see GUI automation and
Screenshots).

A change made only in the Design tab marks the project modified, as other project data outside
Prepare's undo stack does: `sync_recipe_to_model()` sets the plater's dirty flag besides the
backup's change mark. So `get_state.project.dirty`, the window title and `open_files` /
`new_project {discard_changes}` agree with the unsaved-changes prompt that Open and New Project
show. A project holding only a design loads no objects, and `Plater::load_project()` still names
it after its file, since a loaded recipe counts as a loaded project.

`tests/slic3rutils/test_mcp_cad.cpp` (`[Mcp][CAD]`) checks the method table against
`describe_tools`, the plane resolver, the rollbacks, and the methods through their public seams
(`cad_sketch_extrude()`, `cad_sketch_call()`, `cad_query_topology()`, `cad_thread()` and the rest),
which take a `CadDocument` or a bare `DesignSketchTool` and need no Design panel. The replay
scripts run on an instance that `tools/mcp/orca_mcp.py launch --cad` starts:
`tools/mcp/tests/cad_sketch_to_solid.json`, `cad_sketch_edit.json`, `cad_topology_targets.json`,
`cad_features.json`, `cad_doc.json` and `cad_canvas.json` (the Design view as a canvas target, its
key hook, and the dirty flag). `tools/mcp/test_cad_replay.py` records a session with
failed calls and `generation`s, exports it and replays it to the same features and mass
properties; `tools/mcp/test_cad_slow_build.py` checks the window during a guarded rebuild;
`tools/mcp/test_cad_failed_job.py` exports a session whose call failed after its reply budget
(a hole that misses a slow-to-rebuild coil), which leaves it out, and replays it.

## Speed Dial catalog

`list_actions` and `run_action` serve the Speed Dial's `ActionRegistry`: built-in commands, plates,
recent projects, plugin scripts and pages, and, on request, settings. Built-in commands are
`NativeCommand`s in `NativeCommands.cpp`, with three fields for automation that the palette never
shows: `description` (English, for agents), `param` (a `CommandParam {type, help}`, typed `None`,
`Percent`, `Tab`, `Path`, `Index` or `Text`) and `completion` (`Export` when the effect finishes in
the background after `run()` returns). `describe_commands()` fills them from one table, with a
key-prefix rule for calibration dialogs (pointing at `calibrate`), camera views and primitives, and `CommandAction` copies
them into the `AppAction`. The palette keeps `input` for its picker and never passes a path.

`ActionRegistry::describe(Query)` searches id, title, group, source and description, including the
English aliases. It can filter by group, lists settings only when asked, sorts by group and title,
and returns one page. Each entry has `id`, the English `title` and `group`, `kind`, `description`,
`param {type, help}`, and `completes: "export"` for exports. Built-in ids look like
`orca_command:<key>:orca`. `run_action` also takes the key alone (`plate_add`) when only one action
has it, as agents and the tools' own messages write it. `run_action` calls
`ActionRegistry::run(id, param, record_stats=false)`, so agent runs never reorder the palette. An
unknown id returns up to three close ids, scored by edit distance against both the full id and its
key, a key that contains the request (`delete_all` in `obj_delete_all`) counting as closest. An action
with `completes_in_background` that gets a path runs as an export operation and completes when the
file is written.

Given a path, a command with a `Path` param skips its file dialog and calls an entry point that has
none. Without a param, it behaves as it does in the palette. The param is an absolute path, or, for
outputs, a JSON object string `{"path": "...", "overwrite": true}`. `path_param()` resolves it and
turns a bad path into an Error result.

| Command | Takes | Entry point |
|---|---|---|
| `load_project` | `.3mf` | `Plater::load_project(path)`; still asks about unsaved changes |
| `import_file` | model file | `Plater::load_files({path})` |
| `import_configs` | `.json`, `.zip`, `.orca_printer`, `.orca_filament`, `.orca_bundle`; `.gcode` | `MainFrame::import_configs(files, confirm)`; for G-code `MainFrame::load_config_file(path)` |
| `save_project_as` | output `.3mf` | `Plater::save_project(true, path)` |
| `export_gcode` | output `.gcode` | `Plater::export_gcode_to(path)`; completion `Export` |
| `export_sliced_file`, `export_all_sliced_file` | output `.gcode.3mf` | `Plater::export_gcode_3mf(all, path)` |
| `export_stl` | output `.stl` | `Plater::export_stl(..., path)` |
| `export_3mf` | output `.3mf` | `Plater::export_core_3mf(path)` |
| `export_stl_multi` | existing directory | `Plater::export_stl(..., multiple, dir)` |
| `add_primitive_svg` | `.svg` | `GLGizmoSVG::create_volume(path, type)`: a new object, or a part of the selected one; a job |
| `export_config` | existing directory | `ExportConfigsDialog(parent, dir)`: the dialog still opens, and its OK writes there |

The six shape commands (`add_primitive_cube`, `_cylinder`, `_sphere`, `_cone`, `_disc`, `_torus`)
take a `Text` param, the size the Primitive size dialog asks for (`"40 40 10"`, `"90 4"`), and then
call `ObjectList::load_generic_subobject(type, INVALID, true, size)`, which skips the dialog. A param
with the wrong count of positive numbers is an Error result. With an object selected they add
nothing (`ObjectList::can_add_shape_object()`), and say so as an Error, in the palette too.

The object structure commands stand for the object list's context-menu items and the plate menu's
Order By. Like those items, they act on the object list's selected rows, which `select` sets:

| Command | Param | Entry point | Refused when |
|---|---|---|---|
| `obj_type_part`, `_negative`, `_modifier`, `_support_blocker`, `_support_enforcer`, `_precise_seam` | none | `ObjectList::set_volume_type(type, true, false)` | a selected row is not a part; the object's last solid part; a text or SVG part to a support or seam type |
| `obj_rename` | Text: the name | `ObjectList::rename_item(item, name)` | not one object or part row; characters a file name cannot hold |
| `obj_set_instances` | Index: 1-1000 | `Plater::set_number_of_copies(num)` | no object selected; 0 (use `obj_delete`); a cut object; an unprintable instance |
| `obj_assemble` | none | `ObjectList::merge(true)` | fewer than two object rows; a cut object |
| `obj_instances_to_objects` | none | `ObjectList::split_instances()` | no object with more than one instance selected |
| `obj_order_by_name`, `_filament`, `_height`, `_size` | Text: `all` for every plate | `ObjectList::order_objects(order, all)` | no objects to order; a gizmo is open |

Change Type checks the rows itself: with no part row selected, `set_volume_type()` falls back to the
3D selection, which for a selected object holds every part, a change no menu offers. That function
returns why it changed nothing and shows it only for the menu (`show_errors`), so the rule stays in
one place. `rename_item(item, name)` and `set_number_of_copies(num)` are the work of the Rename and
Set number of instances items without their dialogs, which the items keep; without a param, the
commands open those dialogs. Order By keeps the menu's toggle: choosing the order the objects already
have reverses it.

A command whose effect its message does not carry sets `AppActionRunResult::reply`, a JSON object
whose fields `run_action` adds to its reply and the palette never shows. `obj_order_by_*` reply with
the sorted plates' objects in their new order (`order: [{object, name, plate}]`) and `descending`,
`obj_assemble` with the new object's `object` and `name`, `obj_instances_to_objects` with the
`objects` the instances are in, and `obj_set_instances` with `object` and `instances`.

Commands that still open a dialog, such as `export_config`, `plate_rename` or `send_gcode` (see
Print host uploads), return `blocked_by_dialog`, and the agent answers the dialog; the plate scope sets
a name without one.

`filament_add` and `filament_delete` are the filament list's + and - buttons and a slot's Delete menu item
(`Sidebar::add_filament()` and `Sidebar::delete_filament()`, then the slot counter). They run where the
buttons show, for printers that take several filaments through one extruder, and not on a G-code file;
elsewhere, and at the slot limit or with one slot left, they return Info. A new slot goes after the
physical slots, so mixed filaments move up by one with the objects on them; the message names the new
slot. `filament_delete` takes a 1-based slot, the last physical one without it, and remaps the objects on
it as the GUI does. A mixed filament is refused (an Error): it is deleted from the mixed filament list.

`import_configs` with a path imports presets as the Import Configs dialog does, through
`MainFrame::import_configs()`, the dialog's work split from its file picker and result message: the files
go to `PresetBundle::import_presets()`, and the presets are reloaded and the sidebar refreshed. A preset
file (`.json`) is imported alone, a `.zip` or bundle (`.orca_printer`, `.orca_filament`, `.orca_bundle`)
with every preset inside it. The dialog asks about each preset that exists already; the command answers
that question itself, never with "to all", so it is asked about every such preset and the message can name
them all: the param `{"path": ..., "on_conflict": "overwrite"}` replaces them, and by default (`"skip"`) they
are kept. `overwrite`, which other path params use for an output file, is refused here rather than read
with that other meaning. The message lists the presets imported, replaced and kept, found by comparing the
preset names before and after; a call that changes nothing is an Error with the dialog's hint about adding
the printer first. A `.gcode` still loads the settings it was sliced with through
`MainFrame::load_config_file(path)`.

`export_config` with a directory still opens the Export Preset Bundle dialog, given that directory: its
OK writes there instead of asking for a folder. The agent picks the kind by clicking its label
(`ui_click`), checks presets with `ui_input` on their checkboxes, which are named after the preset, printer
or filament they stand for, presses OK with `dialog_respond`, and answers the result message. The command
is an Error when the dialog is closed without exporting. Preset files are not exported without the
dialog: its archive functions read its own checkboxes.

A command that does nothing, such as an object command with nothing selected, returns its message
with `AppActionRunResult::done` false. The palette shows it at its level, Info for most, as it always
has; `run_action` replies `isError`. `obj_arrange` and `obj_orient` say why they did nothing (no
objects, or another background job running) and reset the prepare state a plate's Arrange or Auto
Rotate icon leaves behind, so they act on every plate as the toolbar does. Their job is the call's
operation (see Worker jobs), so `run_action` returns once it is done.

### Making a feature reachable

Most features need only a Speed Dial command:

1. Add it to `build_command_catalog()` with a key, title, group and runner. Give it a row in
   `describe_commands()` with an English description of what it does and what it returns.
2. If it opens a file dialog, give it a `Path` param with help text. Given a param, the runner
   resolves it with `path_param()` and calls an entry point that shows no dialog. It must not
   prompt; it returns an Error result instead.
3. If the effect finishes in the background after `run()` returns, set
   `CommandCompletion::Export` so that `run_action` waits for the file.

The command then appears in the palette, in `list_actions` and `run_action`, and in scripts.

When an agent needs structured arguments or results, or has to wait for the effect, add a typed
tool to the matching `McpTools*.cpp`. Use `inspect_tool()` for pure observation, which runs inline,
read-only and unjournaled. Use `mutate_tool()` for anything that changes the app; it runs as a
journaled job under the mutate guard and may be marked `destructive`. Build the schema with
`object_schema()`, `prop()`, `enum_prop()` and `array_prop()`. It is validated before the handler
runs, so `arg_string()` and its siblings can read the arguments. Do all wx work in `ctx.run_ui()`,
and get the plater with `ui_plater()`, which fails until the main window is up. Throw `ToolFailure`
with a message the model can act on. Call `settle()` after a change so queued events are handled
before the reply, and page lists with `page()`. For work that finishes later, call
`ctx.begin_operation()`, wait with `poll_ui()`, and give the work an identity, as slicing and
export do. Work on the UI job worker needs none of that: the call that starts it holds it as its
operation (see Worker jobs). Describe the tool in English, including what the reply holds and when it is a job.

## GUI automation

`ui_snapshot` (`UiInspector`) returns a compact text tree of the shown top-level windows, each
followed by the popups that opened from it, the rows of their lists, the menus, the GL toolbar items
and gizmos, and each canvas's last-frame ImGui items, capped by `max_nodes` and filtered by `query`.
The tree is a second text block of the result. Refs (`w`, `m`, `t`, `z`, `g`, `r`, `p` plus a number) are
never reused and carry the UI generation and a liveness check, so a
ref to a destroyed or rebuilt widget is reported stale. A field is named by the setting it edits:
its option key is read from the "parameter name" line of its tooltip, and its label comes from the
option's definition. Failing that, the label to its left on the same row names it, then its
tooltip.

Input tools take a `target`: a ref (also read written as `{ref: "w12"}`), or a locator, which is
stable across runs and is what the journal stores. A locator is one of:

- `{option: "layer_height"}` for a setting field;
- `{label, class, name, within, index}` for other widgets;
- `{tab: "General", within}` for a notebook page;
- `{menu: "File>Export>Export G-code"}`, `{gltoolbar: "arrange"}` or `{gizmo: "Move"}`;
- `{imgui: "label", imgui_name: "name", window, canvas, index}` for an ImGui item, by either or both;
- `{row: "Plate 1>Cube>Cylinder", column, within, index}` for a row of a list, tree, table or
  drop-down list (see Rows and popups below);
- `{plate_icon: "lock", plate: 0}` for a plate or one of its icons in the Prepare view (see Plate
  icons below).

`select` selects objects, instances and parts, after which `run_action` runs the object operations
the catalog has (delete, split, mirror, center, drop, instances, arrange, orient, change type, rename,
assemble, set as individual objects, order by) and `set_settings` with the object or volume scope
edits their settings and filament. `add_part` adds parts, modifiers and support shapes,
`set_layer_ranges` sets height range modifiers, and `set_layer_height_profile` runs the Variable layer
height panel's buttons. `set_layer_gcode` does what the Preview layer slider's right-click menu does.
The rest of the object menu is reached through a right click on the object in the 3D view, or on
its row of the object list, with `popup_item` (see Menus below).

A label that matches both a row's label and the one field it names resolves to the field.
`within` names a window other than the main one, whose title follows the project name and is left
out of locators. A locator that matches several elements needs `index`; the journal records one
(`UiInspector::locator_for()`) whenever the element it acted on was not the only match. Before
acting, a widget is scrolled into view, as a person would. On GTK this goes through the scrolled
window's adjustment, as the mouse wheel does, because `wxScrollHelper::Scroll()` leaves native
labels unpainted.

Activation prefers a widget's own dispatch: `wxEVT_MENU`, `GLToolbar::force_left_action()` (or
`force_right_action()` for a right click), `GLGizmosManager::click_toolbar_icon()`, a notebook's
`SetSelection()`, a native control's semantic event, or a custom widget's public API. Canvas
gestures and ImGui items get synthesized wx events with a render between steps. A right click on a
plain window gets the context menu event a person's would: on macOS wx makes it from the right press
the window's handlers pass on, elsewhere the system sends it, so the click sends it there. A GL toolbar item's
visibility and enabled state are brought up to date (`GLToolbar::update_items_state()`, what the
canvas's idle handler does) before `ui_snapshot` lists them and before a click checks them, so an
item a `select` just enabled is not read as disabled. A click on an ImGui item whose centre lies
under another ImGui window (a notification over a panel's button) is refused, naming that window.
`ImGuiWrapper::set_item_recording()` supplies the ImGui items (see below). `SyntheticInput` holds
a gesture's keys, buttons and pointer for code that reads hardware state through `get_key_state()`
and `get_mouse_state()`: the canvas, gizmos, and the object list's Ctrl/Shift multi-select. Mouse
modifiers take the names `ui_key` chords use, through `KeyChord::parse_modifier()`, so `ctrl` means
the shortcut modifier (Command on macOS) in both, and `rawctrl` the macOS Control key. Held
keys are released however the gesture ends; a `ui_mouse` `down` keeps its button held, and a
`move` drags with it, until an `up`, another gesture (one that fails included; a request the server
rejects for its arguments never runs and changes nothing), the end of the script that pressed it,
the closing of the endpoint for a client's press, or the service's stop. A client's gesture that
reaches the UI thread after its endpoint closed is refused. Keys without a target go to the
focused window inside the topmost modal, else that modal, else the focused window, else the 3D view
shown.

A key is delivered as a real key press is on every port: `wxEVT_CHAR_HOOK` goes to the target first
and travels up through its parents (`wxKeyEvent` propagates it) to the top-level window, or stops
at a dialog (`wxWS_EX_BLOCK_EVENTS`); only when no hook takes it, or the one that does allows the
next event (`DoAllowNextEvent()`), do `KEY_DOWN` and `CHAR` reach the target. So a panel that binds
its own hook sees the keys sent to its widgets, as the Design tab's does for Escape, Enter and its
tool shortcuts, and the main frame's hook still sees what none below it takes.

The Design tab's view is a `GLCanvas3D` of its own (`DesignCanvas::gl_canvas()`), outside the
plater's three. `live_canvases()` lists the plater's, which `get_state.gizmo` and the idle checks
read, since the Design view has no gizmos; `ui_canvases()` adds the Design view for what the `ui_*`
tools and screenshots reach: `ui_snapshot` lists its ImGui items, `ui_click` and `ui_key` act on it,
and `ui_mouse` takes `canvas: "design"` (`"current"` is the view shown, `shown_canvas()`). A click
there goes to the Design view's own handlers, so a left click picks a face, edge or body and a right
click opens the offer menu, which is listed or run like any other. Points of the model name
Prepare's objects, so on the Design view `ui_mouse` takes pixels only; `cad_call query_topology`
gives its faces.

### Menus

Every popup and context menu goes through `popup_menu()` (`SyntheticInput.hpp`), which is
`wxWindow::PopupMenu()` unless the MCP server has installed a capture
(`SyntheticInput::set_menu_capture()`). The capture is there because a native menu loop holds the UI
thread until a person closes the menu: on macOS it pauses wx's pending events, and with them every
`run_ui()` closure. So MCP never shows a menu. `UiInspector::on_menu()` takes a menu that opens in a call's window: from the start of each of
the call's UI closures (`UiInvoker`'s activity hook) until the first `wxEVT_IDLE` with nothing queued
after the closure ends, so a menu opened by an event the call queued (a 3D view posts its right click
to itself) or by a `CallAfter` chain is taken too. The window closes a second after its closure in any
case (`on_menu()` checks the age itself, so no idle is needed), so a stream of queued events can't
keep a person's menus from showing. Only menus of the event
loop the closure ran in are taken: a person's right click in a dialog the call opened, whose loop is
another, shows its menu, and a call working inside that dialog has a window of its own there.

The capture does what `wxWindowBase::PopupMenu()` does before it shows a menu: it sets the invoking
window and calls `UpdateUI()`, so the `wxEVT_UPDATE_UI` handlers MenuFactory binds on the main frame
set each item's enabled and checked state as a person would see them. `ui_click`, `ui_mouse` and
`ui_key` arm a capture in the closure that delivers their input (`arm_menu()`). The first menu that
opens goes to it: without `popup_item` it is listed in the reply as `popup_menu` (`items`, and the
`disabled` and `checked` ones) and nothing runs, as if the person pressed Escape; with one, that item
runs: a check item is toggled or a radio item checked, then `SendEvent()` on the submenu holding it,
which dispatches through that menu, its parents and the invoking window as a pick from a shown menu
does, and the reply says `ran`. It runs inside the `popup_menu()` frame, so `Plater::PopupMenu()`'s
deferral of slicing and error messages, and the popup position Add Text uses, are in effect. A
command that opens a dialog from the 3D view's queued right click leaves it open when the call
returns; the reply then lists it under `dialogs`. The call waits for that queued click by queuing a
`CallAfter` on the canvas behind it (one handler's queue is first in, first out) before it settles
and reads the capture. A later menu in the same call is closed unseen and counted (`menus_closed`);
a menu that opens in a window where nothing is armed (a `cad_call run_verb` whose offer opens later)
is closed unseen too. The last three such menus, with the tool whose window took them, are in
`get_state.closed_menus`.

A menu path is the item labels joined with `>`, as `GetItemLabelText()` gives them, matched without
case; a trailing `...` may be left out. A label repeated among its siblings is numbered from its
second time on (`Change Filament>Generic PLA #2`), so every listed path names one item, and the
marker the Change Filament list appends to the slot in use (" (current)", translated) is left out of
the numbering and the matching, so a path recorded with another slot in use still names the same
item. A path that names a submenu, a disabled item or nothing fails with the items to pick from, and
so does a `popup_item` when the input opened no menu (a right click on empty space while objects are
selected, in the Preview, or with a gizmo open). Paths are the UI language's text, as other locators
are. With `x`/`y`, `ui_click` clicks that point of a custom-drawn widget (such as the Design tab's
feature tree) as well as of a 3D canvas; a native control, which acts on what the system tells it
rather than on wx mouse events, is refused.

MenuFactory builds the multi-selection, layer and assemble-view menus afresh for each popup.
`Plater::PopupMenu()` takes them back (`MenuFactory::take_transient()`) and frees them once the popup
has closed, for a person's right click as for MCP's. Their items' `wxEVT_UPDATE_UI` handlers stay bound
on the main frame, but under ids `wxNewId()` never hands out again, so they never run.

### Rows and popups

The rows of `wxDataViewCtrl`s (the object list, Compare presets, the upload queue and other data
views), of `wxListBox` and `wxCheckListBox` (the Add Settings checklist), of Orca's `DropDown` lists,
of `wxGrid` tables (the object settings table) and of the Design tab's `DesignRowList`s are listed
under their list as `row` nodes (`list_rows()`): the first text cell is the label, the other text
cells the value joined with ` | `, a checkbox cell its checked state. A data view is walked through
its model from the root, collapsed branches included. An object list row also says what it stands
for by the indices `list_objects` uses (`plate 1`, `object 0 part 1, modifier`, `object 0 layer
range 0-2`). A table's rows are named by their `Name` cell (the object settings table's part rows
by the part), else by their row label; a sort indicator after a column title is left out, and a
table that hides its column labels and draws its titles as its first row is read with that row as
the titles. A drop-down row in a group has the group's label above it; the picked row is the checked
one. A list node lists the names its cells are picked by (`columns:`): header text, `#n` for a
column without one, and for the object list, which shows no header, `Name`, `Height`, `Printable`,
`Filament`, `Support`, `Color`, `Sinking` and `Editing`.

A row locator gives the labels from the top joined with `>` (an array when a label holds `>`), of
which it matches the last ones, so the plate above an object may be left out; the marker " (current)"
may be left out as in menu paths. Rows are found again by their labels on each use, since models
delete and rebuild their items; a ref stays alive while its list does and the path still names a
row, rows with the same labels being told apart by their order. The journal records the row's whole
path, its `column` and its window's `within`, with `index` when other rows match it. A row that
matches nothing gets the rows whose label holds it, or the shown ones; when no shown list has any,
the error says that the object list, the one agents look for most, is hidden until the sidebar's
Global/Objects switch (a `SwitchButton` the snapshot labels by the "Process" beside it) is on Objects.

A click acts as a person's click on the row's cell (`click_row()`): the cell is the `column` named,
else the row's first. On a data view, the branches above the row are expanded and the row scrolled
into view; the selection is set as the click would set it (Ctrl toggles the row, Shift selects the
shown rows from the current one, a right click selects an unselected row only), and the owner gets
one `wxEVT_DATAVIEW_SELECTION_CHANGED` for the row. The controls report a selection they are given
differently (macOS each change it makes on the way, GTK and the generic control none), so their own
reports are held back for the call and the one a click makes is sent. The synthetic pointer sits at the cell's centre
for the gesture, and the object list reads it through `get_mouse_state()` where it would read the
real pointer: on Windows and Linux its selection handler then acts on the cell under it (Printable
toggles printability, Sinking drops the object, Editing resets the row's settings). On macOS the
native list starts editing an editable icon cell instead, which the object list turns into the same
actions, so a click that names such a column sends `wxEVT_DATAVIEW_ITEM_EDITING_STARTED` too. A
double click sends `wxEVT_DATAVIEW_ITEM_ACTIVATED` (the object list frames the object, or opens the
filament editor on the Filament column), a right click `wxEVT_DATAVIEW_ITEM_CONTEXT_MENU`, whose menu
is captured like any other. `ui_input` with true or false sets a checkbox cell through
`ChangeValue()`, as the toggle renderer does, which tells the owner. A list box row is selected with
`wxEVT_LISTBOX` and a checklist row ticked with `wxEVT_CHECKLISTBOX`; a drop-down row is picked with
`DropDown::ClickItem()`, which does what a release over it does; a table row's cell is made current
with `GoToCell()` (`wxEVT_GRID_SELECT_CELL`), and its cells are not edited: `set_settings` with the
object scope writes the same values. A Design list row is clicked at its centre, or at the action
icon whose tooltip `column` gives, through the list's own mouse handlers.

Popups (`wxPopupWindow`: the print button's menu, drop-down lists) report themselves top-level but
are children of the window they opened from. `ui_snapshot` lists each shown one as a `popup` right
after its window (`popups_of()`), and `root: "popup"` lists only them. A widget in a popup belongs
to that window (`owning_window()`), so it is inside the topmost modal for the input guards when its
popup opened from there, and its locator's `within` names that window. A screenshot of a widget in
a popup is refused, since window captures leave popups out.

### Plate icons

A plate and its icons in the Prepare view (delete, orient, arrange, lock, settings, name, move to
front, filament grouping, IDEX mode) are GL picking models, which a person reaches only by pointing at
them. `ui_snapshot` lists under the Prepare canvas a `plate_icon` node for each plate, named as the
object list names it, then the icons of the current plate; the lock icon carries the plate's lock
state. A locator `{plate_icon, plate}` names any plate's icon by the picking action `PartPlate` gives
it: `plate` (the plate itself), `delete`, `orient`, `arrange`, `lock`, `settings`, `name`,
`move_to_front`, `filament_map`, `imex_mode`. The bed picking registry is the authority on what is
shown, since `PartPlate` registers only the icons it draws and hidden plates are inactive there: an
icon it does not offer (plate settings off, filament grouping without two nozzles, IDEX mode off) is
refused by name.

A click calls `GLCanvas3D::click_plate()` with the icon's picking id, the code the canvas's mouse
handler runs for the plate under the pointer, so undo snapshots, notifications and dialogs are the
GUI's. It is refused where the canvas takes no plate clicks: outside the Prepare view, while variable
layer height is being edited, and while a gizmo is open, where the canvas would not open the plate
menu. A left click on lock replies the lock state; on the plate itself, move to front and delete the
current plate. Arrange and orient start a job on the UI job worker, which the reply waits for
(`done`); `started: false` says the click started nothing, with a note when the arrange icon's own
flag is stuck (an arrange that was cancelled or failed leaves it set until another arrange finishes).
Settings and filament grouping open their dialog from an event posted to the plater, so the call
queues a `CallAfter` on the plater behind it and replies once it ran, with the open dialog under
`dialogs`; the name icon opens its dialog inside the click, which the call reports as
`blocked_by_dialog`. A right click opens the plate menu, posted to the canvas, or the IDEX icon's mode
menu, and either is captured as any other menu (Menus above).

### ImGui items and gizmos

The 3D canvases draw their panels with ImGui, which has no widgets to walk. ImGui's test-engine
hooks (`IMGUI_ENABLE_TEST_ENGINE`) report every item a frame adds while
`ImGuiContext::TestEngineHookItems` is set, which `ImGuiWrapper` does only while the service runs;
otherwise ImGui skips each hook with one branch. `ImGuiWrapper` keeps the items of each canvas's
last frame: id, label, window, rectangle and status flags, and whether the item lies outside the
visible part of its window. The hooks reach the recorder through `ImGuiContext::TestEngine`, so they
work without the app. Orca's copy of ImGui adds two hooks upstream lacks: `ItemValue`, through which
sliders, drags, inputs, combos and radio buttons report their kind, the value they show and whether
they take typed input (`ImGuiItemStatusFlags_Inputable`, upstream's flag), and `Text`, through which
`TextEx` reports text, kept apart from the items. A clipped widget returns before it reports its
label, so the string its id was hashed from (`GetID()`'s hook) stands in for it.

Some controls have no label, or no item at all: icon buttons drawn as a `Dummy` or an `Image` with a
hand-made click test (the painters' tools, the Text and SVG icons), links that are bold text with a
hover test, image buttons, combos with an empty label, options drawn as an empty `Selectable` and
text. Their code calls `ImGuiWrapper::record_named_item()` with an English name right after drawing
them: a real item is named in its own record, so its value stays on it; anything else gets a record
of its own at its rectangle, with the id the name hashes to in the current ID scope, so repeated
rows under `PushID` stay apart. Same-named ones in one scope can only be told apart by their order,
so their ids follow it: a target taken among them is not found once their number changes, and after
a reorder it names the control now in its place. Call sites give each control a name of its own
(Text's "Revert Depth", Texture's "Remove <texture>"). A synthesized click at the rectangle goes
through the code's own click test. Nothing about the controls changes for a person, and the call costs one branch while
recording is off.

`ui_snapshot` lists each ImGui window (its scroll range when it scrolls), then its items and text in
reading order. An item is listed by its shown label, or, for a field with none, by the text left of
it on its row, which only the first field after that text takes; its name is the one it was given or
its label's `##` part (`##cursor_radius` → `cursor_radius`). Orca's icon glyphs are not label text.
The locator is the label when no other item in the window shares it, else the name, else both;
text is never matched. A ref follows its item by id, or by name when the label shows a value that
changes (Text's font button); it takes the item's current name (a layer row renamed by its place),
and the locator recorded for a call is the one that finds the item then. Before acting on a clipped item, the inspector scrolls its window, and the
windows it is a child of, to bring it into view (`SetScrollFromPosY()`, then two frames). A list that
draws no item for rows out of view, such as Texture Displacement's library, is scrolled with the
mouse wheel over its window, as a person does.

`ui_input` on an ImGui item acts by its kind and reads the value back, failing when the field shows
something else (numbers compared at the precision it prints): a checkbox, radio button or named
toggle is clicked when its state differs; an input is clicked, its text selected and replaced,
then Return pressed; a multiline input gets Return only between lines; a drag or a slider that takes
typing is Ctrl+clicked first; a slider set through the number field beside it when it has one, as
Orca pairs them; a combo is opened and its option, by its text or its 0-based place, clicked in
its popup.

A gizmo is listed per canvas as its toolbar shows it (`GLGizmosManager::is_on_toolbar()`), and
on the Prepare canvas Simplify and SVG too while they can open (Simplify when activable, SVG on a
selected SVG part). Clicking one does what its icon does (`click_toolbar_icon()`): it opens, or
closes the open one; Text with no text selected starts a job that creates a text part and opens the
gizmo, which `ui_click` waits for. A `gizmo` locator takes the name `get_state.gizmo` gives, or
the English title of its toolbar button where that differs (Support Painting for FdmSupports), in
any case; the reply uses the first, and an unknown name's error lists them. `get_state.gizmo`
names the open gizmo and its canvas. The
assemble view is entered and left with `set_view view` (leaving does what its Return button does;
entering returns once the canvas has loaded its objects, which waits for its GL surface on Wayland),
and `select` and `list_objects` act on it while it is shown. Grabbers are not dragged: a gizmo's
panel fields or `transform` set those values. Pickers inside a gizmo go through `gizmo_file`, whose
actions are the bodies of those pickers (`GLGizmoSVG::change_file()`, `save_svg_as()`,
`GLGizmoTextureDisplacement::import_texture()`).

`ui_mouse` takes points of the model (`CanvasTarget`): `{object, instance, volume}` with `point` in
the part's mesh coordinates (`frame: "volume"`, mapped by `GLVolume::world_matrix()`, which follows
the assemble view's offset and explosion) or in the scene's (`"world"`), or `spot`, where a ray along
an axis through the part's displayed bounding box first meets it. A point is projected through the
camera the canvas was drawn with and converted to logical pixels. With `check: "surface"` (the
default) the point must lie on the part, above the bed and on the side the clipping plane keeps;
the ray through the whole pixel the click goes to must meet nothing before it (active parts, the
parts of the instance the open gizmo works on, which it draws itself while their scene entries are
inactive, and the plates), and must then meet the part near the point. A part the view does not
draw (`GLVolume::visible`, false for objects the assemble view left out) is neither a target nor in
the way. Other inactive parts are hidden by the gizmo, and modifiers and negative parts other than the
target are see-through, so neither counts; a modifier or negative part is a target like any part. The
plates count only for a ray looking down: from below the GUI draws them see-through and picks and
paints through them (`SceneRaycaster` raycasts the bed only while the camera looks down).
`check: "point"` only projects, for points off the surface such as a cut plane. Either way, a gesture
that starts at a model point under an ImGui window (a gizmo panel, a notification, an open popup) is
refused, naming it, since ImGui would take its events: `imgui_window_at()` repeats ImGui's
hovered-window search (`FindHoveredWindow()`) on the windows of a frame drawn just before. A pixel
given as `x`/`y` is the client's to aim and is not checked, except in a painting gizmo. A model
target takes `point` or `spot`, not both.

What a drag does off the model is the user's to choose: Preferences maps each button to rotate, pan
or nothing (`left_mouse_drag_action`, `middle_mouse_drag_action`, `right_mouse_drag_action`, rotate,
pan and pan by default), and the touchpad camera style turns the camera with Alt+move and pans with
Shift+move instead. So the tools never turn the camera with a drag. `set_view` `orbit` moves it around
what it looks at by degrees, yaw to its right and pitch up, placing it with `Camera::look_at()` from
its position so the result is the same on every install; it is how an agent looks from between the
named directions, which the bench showed agents otherwise reach by dragging the canvas edge on the
assumption that the left button rotates. A painting gizmo (`GLGizmoPainterBase`) takes a press only
where the cursor hits the object it paints; a press that misses falls through to the canvas and moves
the camera, and the stroke paints nothing. `ui_mouse` refuses a down or drag that starts there,
testing the start pixel's ray against the selected instance's parts, so a misaimed stroke fails
instead of silently turning the view under the screenshot the agent aimed from. The refusal names
the first pixel along the stroke that lands on the object, so a stroke swept in from outside it is
retried in one call rather than broken into clicks. Any gesture that
moves the camera anyway (a drag off the model outside the painters, a wheel) says so with
`camera_moved` in its reply, compared from the view matrix and zoom before and after.

Every point of a
gesture is resolved before its first event, and the journal keeps the model points, so a script
replays at another window size or camera. A gesture's modifiers are pressed as a keyboard does: a
key-down event each before it and a key-up after, which is what switches Measure and Assembly to
point picking and the painters to their Shift and Alt modes.

### Dialogs and web views

`ModalTracker`, a `wxModalDialogHook`, sees every `ShowModal()`, native dialogs included, and
publishes the snapshot `[{title, message, class, native, buttons}]`. Message boxes and file and folder
pickers are native; a picker's message is its prompt, such as "Choose Download Directory", since its
title is often empty. Colour and font choosers are not classified as native, because on GTK they are
wx dialogs that `dialog_respond` ends; on macOS and Windows they are OS choosers, best answered ahead
with a `dialog_auto_answer` rule. `dialog_respond` presses the
topmost wx dialog's button, so the dialog's own handler runs, or ends the dialog with that id when
no such button exists. A native dialog can only be answered ahead with a `dialog_auto_answer` rule:
a regex over title and message, and a button. `Enter()` then returns that button's id without
showing the dialog, and logs it; a file or folder picker can only be cancelled, since any other
answer would return without a path. A wx dialog that matches a rule opens, and the tracker presses
the button once the dialog's modal loop runs (`press_dialog_button()`, queued on the dialog), as
`dialog_respond` does: its buttons do the dialog's work in their handlers (`UnsavedChangesDialog`'s
Transfer and Discard, `CloneDialog`'s Fill), which a returned id would skip. A modal opened above it
before then leaves it to `dialog_respond`. A new rule also answers a
matching wx dialog that is already open. A prompt posted around the same time, such as the restore
prompt after startup, is handled whether it opens before or after the rule. A script's rules end
with the script, a client's call replaces its earlier rules, and `get_state` lists the rules in
force.

`webview_eval` runs JavaScript with `RunScriptAsync`, as a recover call. While a modal dialog is open
it runs only in a web view of the topmost one, and only when that dialog is the setup wizard
(`GuideFrame`) or a preset bundle dialog (`PresetBundleDialog`, `ExportPresetBundleDialog`), checked
by class: their pages have no typed route. Sign-in dialogs are never on that list, so no client reads
or fills a sign-in page, and neither are the flushing volumes and colour picker dialogs, whose values
`set_settings`'s project scope writes through the same apply code. A web view in a dialog under the
topmost one is refused, and any other gets a mutate call's checks. Without a target the web view of
the topmost dialog is used, else the only shown one. These dialogs have no wx buttons, so
`dialog_respond` only ends them: a page's work is committed through the page's own button. A web
view's locator names it by kind (`class: "webview"`), since its class differs per platform (WebKit,
WebView2). The result arrives as an event on the UI thread, in a `ScriptSink` that only the UI thread
creates and deletes, along with its weak reference to the web view. The job thread waits for the
result, up to `timeout_s` (default 10), without the UI; a JSON result comes back parsed. A page
closes its dialog with a message (`window.wx.postMessage`) the dialog answers by ending its modal
loop, which destroys the web view, while wx's script callback keeps a plain pointer to it. So the
script runs wrapped: messages the page posts while it runs are held and sent, in order, once it has
returned, and a script whose held messages closed its dialog replies its value with `closed: true`
once the dialog has left the modal stack. The sink also watches the web view's destruction, so a
dialog that goes away before the result by another route replies `closed: true` with a null result
instead of timing out.

## Screenshots

`screenshot` captures on the UI thread and encodes off it. It has three targets.

- `canvas`: the visible 3D view, the Design view while the Design tab is shown, exactly as drawn,
  toolbars and ImGui panels included (the Design view's sketch overlay and status line too), on
  every platform. `GLCanvas3D::render_and_capture()` forces a frame and reads the back buffer just
  before the swap; it fails, saying why, when the canvas is hidden or mid-frame.
- `window`: the main window's own pixels, never the screen, from `capture_native_window()`, with
  every visible 3D canvas composited in from its canvas capture, since window capture loses OpenGL
  content on several platforms.
- `dialog`: the topmost modal, captured like a window.
- a widget's ref or locator: its top-level window captured like a window (a window that is not
  modal, such as an object's settings, has no other target) and cropped to the widget, or the
  canvas capture for a 3D canvas. A tab is cropped to its page, which must be the one shown. The
  journal records the locator.

When the capture may differ from the screen, the result carries `approximate: true` and a `note`.

| Platform | Window capture | Approximate when |
|---|---|---|
| Windows | Windows Graphics Capture of the window, cropped to its client area; `PrintWindow(PW_CLIENTONLY \| PW_RENDERFULLCONTENT)` where that is unavailable | `PrintWindow` fails and the screen is copied instead |
| macOS | `cacheDisplayInRect:toBitmapImageRep:` on the content view | always: web views may be blank |
| Linux X11, Xvfb, XWayland | `XGetImage` of the top-level X window | part of it is off-screen or under another application's window |
| Linux Wayland | `gtk_widget_draw()` of the top-level window's child, which leaves out the client-side decoration's shadow | always: web views may be blank |

On Windows the capture never makes the window repaint. `PrintWindow` gets its image by repainting the
window and every child into its bitmap, and the screen shows that repaint as a flash of the half-drawn
window, at every frame of a recording. Windows Graphics Capture reads what DWM composed instead, web
views included, whether or not other windows cover it. A session waits for its first frame; after it,
Windows sends a frame only when the window changes, and a free-threaded frame pool hands each one to a
thread pool thread that keeps only the newest, so the pool always has a buffer free and no change is
dropped. A recording keeps its sessions open from frame to frame (`open_capture_sessions()`, passed in
`CaptureOptions::sessions`, released when it ends); any other capture opens and closes its own. The
capture leaves out the pointer and, where Windows allows it (Windows 11), the yellow border it draws
around a captured window; Windows 10 shows the border while a recording runs. A few pixels at the
corners Windows 11 rounds read black. A window Windows Graphics Capture cannot read falls back to
`PrintWindow` for the rest of the recording.

On every platform, a canvas that can't be captured keeps whatever the window capture drew, and the
result is approximate. A canvas frame that reads back black (`is_blank()`, a stray pixel aside) has
not been drawn yet. This happens on Wayland for a moment after the canvas is first shown, and while
GNOME's overview hides an XWayland window. Such a capture is taken again every 300 ms for up to 3 s,
then returned as approximate, saying so. The composite takes the canvases of `ui_canvases()`,
the Design view included; an OpenGL view outside them has no frame capture, and a window capture
says it is left out. Minimized and hidden windows fail. X11 reads only the on-screen part of a window (`XGetImage` fails
for the rest), which leaves the remainder black. X11 keeps no backing store, so a part
covered by another window reads back black. OrcaSlicer's own popups and tooltips stacked above the
window are read and drawn in, as they appear on screen; another application's window leaves the
hole, and the result is approximate. The full-size PNG goes to `save_path` or `<datadir>/mcp/out/OrcaSlicer-screenshot-<timestamp>.png`. The image
block is scaled to `max_dim` on the long edge (default 1280, clamped to 256–2000).
`include_image: false` returns only the path.

## Recording

`start_recording` records what OrcaSlicer shows into a video while an agent, a script or a person
works, and `encode_recording` turns it into an MP4 to attach to an issue. Recording has to stay light,
since it runs while the app works: frames are kept as JPEG in a Matroska file (MJPEG), and the H.265
encode, which costs far more, comes after it. There is no audio. `McpRecorder` holds at most one
recording; `get_state.recording` shows it while it runs.

**Capture.** The UI thread captures a frame per tick of a one-shot `wxTimer`, which fires inside
modal loops too. `start_recording` takes the first frame in its own UI closure: it fixes the video's
size (fit to `max_dim`, rounded down to even numbers for 4:2:0), and a failure fails the call before
any file exists. The `window` target is the main window as `screenshot` captures it, with the modal
stack (`ModalTracker::stack()`, outermost first, so in stacking order) composited at each dialog's
client offset, resampled to the main window's content scale off MSW. Modeless windows, popups and
native dialogs are left out: `wxTopLevelWindows` is creation order, not stacking order. On X11 the
capture reads the screen, so it already shows our windows over the main window as they are, the
script runner included when it covers it, and nothing more is composited (a reparenting window manager's
frame is searched for our window inside it); the capture records the
rectangles it drew another of our windows in (`Capture::covered`) and composites the 3D views
everywhere else, so they are not pasted over those windows (X11 screenshots get this too). On
Wayland, where the app can't know where its windows are, dialogs are not composited. The result's
`note` says which applies. The `canvas` target is `shown_canvas()`. A recording captures with
`CaptureOptions::hide_covering` off, so X11 never hides and re-shows the script runner at each frame.

The costly part is a 3D view: `render_and_capture()` draws a frame and reads it back. A recording
keeps each canvas's last capture with `GLCanvas3D::presented_frames()`, which counts the frames the
canvas presented, and its size and the UI generation; while all three are unchanged, the frame on
screen is the kept one, so the canvas is drawn only when it changed. Each tick measures its capture
and the next comes after `max(1000/fps, 3 x capture time)`, so recording takes at most about a third
of the UI thread wherever window capture is slow (macOS `cacheDisplayInRect`, X11 `XGetImage`). A
tick is skipped while the main window is minimized or being rebuilt, or no canvas is shown for
`canvas`. A frame whose pixels equal the last queued one (`memcmp`), under the same overlay, is not
queued, unless an overlay is animating (a ripple, a caption or the pointer fading) or a second has
passed (`repeats_last_frame()`), so an idle window costs nothing.

**Writing.** Only the capture runs on the UI thread. `RecordingSink` (wx-free) takes each frame as RGB
bytes plus its overlay, since wxImage reference counts are not atomic. One budget of 512 MB covers a
frame from `push()` until it is written: frames being encoded and encoded ones waiting their turn
count, and a frame that doesn't fit is dropped (`dropped`), never waited for. The budget is an atomic
counter, the queue's mutex is held only to push or pop, and only the sink's threads take the writer's
mutex, so a stalled disk never holds up the UI thread. Two detached encoder threads run
`render_frame()`: bilinear scaling into the video through one `FrameTransform` (scale and letterbox
offset) that the pixels and every overlay position share, the overlays, and JPEG through libjpeg
directly (`encode_jpeg()`, whose libjpeg error handler longjmps back into a frame holding only C
structs). They use nothing of wx, so a thread still running at exit touches nothing wx has torn
down. Frames are written in capture order through a reorder map; a failed encode is a skip, and a
dropped frame has no sequence number, so the order never waits on a hole. `MkvWriter` writes the
Matroska file: one `V_MJPEG` track, SimpleBlocks with millisecond timestamps in Clusters of 5 s, and
the Segment, Cluster sizes and Duration written as unknown and patched on close, so a file never
closed (a crash) still reads up to its last frame. Matroska rather than AVI because frames keep their
real, uneven times; one file rather than a folder of JPEGs because it plays as it is.

**Ending.** `stop_recording` first captures one more frame in a UI closure, when the UI thread can run
it, so the last call's effect is in the video even if the timer has not ticked since. A recording ends on
`stop_recording`, after `max_seconds`, at 4 GiB, on a write error, when
the script that started it ends (`run_script_job()`, which names a file still being written after 30 s
as the result's `unfinished_recording`), when the endpoint closes for a client's
(`stop_serving()`; a script's carries on), and at exit. Ending from any thread sets a flag the next tick
reads, tells the sink to finish, and posts the timer and input filter's cleanup to the UI thread; the
sink drains its queue, writes the last frame again at the end time so the video lasts until then,
closes the file and calls back, on its own thread. So ending never needs a UI round trip: a script's
end waits for the file without deadlocking against `McpService::stop()`, which joins script threads on
the UI thread, and a step after the script sees a closed file. At exit `stop()` waits 5 s for the
file, then drops what is still queued and leaves the sink to its threads, which own its state. A
recording counts as active until its file is closed, for the one-at-a-time rule and for encodes.

**Encoding up front.** `start_recording` takes the Start Recording command's encoding keys (`encode`,
`encode_preset`, `codec`, `crf`, `speed`, `profile`), checked by `apply_recording_overrides()` with its
defaults, the Balanced preset, and refuses `encode` without an ffmpeg before anything records. The
recording keeps `encode_recording`'s arguments (`RecordingOptions::encode`), and the `stop_recording`
that reports it, once its file is closed, starts the encode with `McpService::start_encode()`: a job of
its own, outside the journal (a replayed `start_recording` asks for it again), whose envelope the report
carries as `encode`, so the agent waits for the MP4 by that job's id. A recording that ended on its own
is encoded when the next `stop_recording` reports it; one nothing reports, a client's that ended with
the endpoint, is not encoded. An agent can still encode any recording later with `encode_recording`.

**From the app.** The Speed Dial commands `take_screenshot`, `start_recording` and `stop_recording`, which
the Help menu also has, give a person screenshots and recordings without an agent: a PNG of the main window,
and a recording of it for up to an hour. Both go in the person's Downloads folder unless they chose another.
Start Recording opens `RecordingDialog`, which splits its settings by the file they make. Record holds the
JPEG quality of the `.mkv` and a switch for each overlay, the pointer and the caption, both off unless the
person turns them on. Encode to MP4 holds whether ffmpeg encodes it once it ends, with a built-in preset or
codec, CRF, speed and profile; it is off and disabled when no ffmpeg is found, and says how to get one. A
rail joins the two stages and shows whether the second runs. The dialog remembers what it starts with in
AppConfig's `recording` section (`RecordingSettings`). The command's parameter overrides those settings for
one recording, and with `silent` starts at once without the dialog and remembers nothing; a silent start
that would encode without an ffmpeg is refused rather than recording unencoded. The encode is
`encode_recording` itself, run by `McpService::encode_when_closed()` as an unjournaled job once the
recording's file is closed (`McpRecorder::wait_closed()`), so it can be cancelled like any encode. No reply
names the job, which only exists once the file is closed: an agent finds it in `get_state.jobs` and waits
for it by its id, as the command's description says.
Start Recording starts the service without its endpoint when the server is off, as the Run Script dialog
does. The person's recording has its own owner (`kPersonOwner`), so neither a script's end nor the
endpoint closing ends it. Whatever ends it tells the person where it went, in a notification whose button
opens the folder: Stop Recording itself, which stops any recording, an agent's too, ending it as
`stopped_in_app`; or `RecordingOptions::ended`, which the recorder runs on the UI thread when an agent's
`stop_recording`, the time or size limit, or a write error ends it, and which starts the encode. A second
notification then names the MP4, or says why it could not be made.

**The REC pill.** While a recording runs, whoever started it, the main window's tab bar shows a REC pill
before the slice button, so the person always knows the app is being recorded: an outlined pill whose dot
pulses, filled red with a stop square under the pointer, and a click runs Stop Recording. The recorder shows
it once a recording starts and hides it once one ends (`McpRecorder::update_indicator()`), and shows it again
on the window a rebuild makes. It is `RecordingIndicator`, one of the tab bar's side tools, in the same place
on every platform: the window's content, which captures and full screen keep, where macOS's native title bar
is in neither. Recordings and screenshots show it as the person sees it, since captures show what the app
shows, so its pulsing dot is in the frames and a recording never sees an idle window: each frame in which the
dot changed is queued.

**Captions.** `McpCore` reports each call it runs to a `CallObserver`: when its handler starts, when
`set_journal_args()` replaces its arguments (refs become locators), and its final result (a job's,
worker job included). Calls refused before they run are not reported. The recorder captions calls
that are not `Inspect` (mutate and recover tools, script steps included) as the tool's translated
title and what it acts on (`caption_detail()`), with a mark: a ring while it runs, then a dot, red
with the failure's first line, from which `caption_error()` takes out every value the caption hides.
One caption shows at a time, the current call's (`CaptionBoard::current()`): of the calls that run or
linger (4 s after they end, then fading), the one started last, so a script's steps replace one another
and a script still running between them shows again once its step has faded. Clicks and keys are not
captioned. With `pointer`, a `wxEventFilter` sees every mouse and click-type command event, a person's
and the ones the GUI tools synthesize alike: mouse events move the pointer, presses add a ripple, and a
button, checkbox, toggle or radio command with no press on it just before is a click made without the
mouse (`ui_click`), shown at the control's centre. Events in windows the frame doesn't show, and in
windows hidden from automation, are ignored. Both are off unless asked for, `captions` and `pointer`
turning on one each; without the pointer no filter is installed.

The overlays annotate the app without competing with it. They are drawn at the app's own scale:
`overlay_scale()` is video pixels per app pixel at 100 %, from `em_unit()`, the content scale and the
capture-to-video transform's scale (the one the pixels are drawn with), at least 0.75 so a small video
stays readable; it travels with each frame's overlay. The caption sits at the bottom centre, where
subtitles are read, in the app's own label font (`Label::Body_14`) at the size the sidebar's labels
have, on a translucent charcoal plate; a title followed by a detail is quieter than the detail. It is at
most 40 % of the video's width (240 units in a small one), fitted by `fit_spans()`: the title first,
then a failure's error, then the detail, each cut with an ellipsis or left out when too little room is
left. The pointer is an arrow in an amber ring with nothing inside it, which reads on the light UI and
the dark bed alike; presses ripple out in the button's colour. Text is rasterized on the UI thread,
only when it changes, as a coverage mask (`wxMemoryDC`, white on black) the encoders colour and blend.

What a caption adds must not leak secrets, although the screen itself shows whatever its fields show.
A setting whose key names a password, API key, access code, token or secret (`is_sensitive_key()`;
`printhost_apikey` is a plain text field) shows `•••` for its value; `ui_input` shows its value only
for a non-sensitive setting located by its key, since a field located by its label, or a ref not yet
resolved, may be a password box; and `ui_key` shows its chord, never its text.

**Encoding.** The bundled FFmpeg only decodes, so `encode_recording` runs the system `ffmpeg`:
`ORCA_FFMPEG`, else `PATH`, else on macOS the Homebrew and MacPorts folders, which an app started from
Finder doesn't have on its `PATH`. Its path is never a tool argument, so a call can't run an arbitrary
program. The command keeps the recording's timestamps to the millisecond: variable frame rate (`-fps_mode vfr`,
`-vsync vfr` before ffmpeg 5.1) in the source's time base (`-enc_time_base demux` from 6.1, `-1` from 3.4;
without it they round to whole frames), the spelling chosen by `ffmpeg -version`, and for a git build,
whose version is unknown, each older spelling of the two in turn while ffmpeg refuses the last. It evens the size, converts the
full-range JPEG frames to the limited range players expect, encodes 4:2:0 with libx265 (tagged `hvc1` for
Apple's players) or libx264 at `crf`, at the encoder's `speed` preset and in `profile` (main10 encodes 10 bits
per sample), and writes MP4 with `+faststart`. Constant quality (CRF) suits a screen recording: the bitrate
falls to almost nothing while the window is still and rises for the motion, where a bitrate target would
spend it evenly; the video's length in the reply is
the MP4's movie header's (`mp4_duration()`); an ffmpeg without the encoder is refused, naming what is needed. It runs
without a console window, polled every 100 ms and terminated when the call is cancelled
(`ToolDef::cancellable`: `cancel {job}` stops it through `McpService::request_stop()`). On Windows it is
ended with `TerminateProcess()` on its handle and waited for on that handle, since `TerminateProcess()`
returns before the process is gone and boost's `terminate()` closes the handle right after it; a child not
gone within 5 s keeps its files, which the error names, and its source and destination stay reserved for
the session, since it may still be using them. `RecordingFiles`
keeps the files in use, compared by `path_compare_key()` (Unicode case-folded on Windows and macOS): the
recording until its file is closed, and each encode's source and destination. Encodes may share a source, but one with `delete_source` holds its source alone, and
nothing writes a file anything else uses. Output goes to a temporary file of the call's own, renamed
over the destination on success; the call's files (that, ffmpeg's log, and the listings it read) are
removed whatever the outcome, and no other call's. `delete_source` deletes
only a recording this session made.

## Preview data

`get_preview` reads what the Preview shows for a plate without selecting the plate. Its tables
come from the plate's `gcode_statistics_json()`, the serializer behind the slice reply's statistics
and the exported `statistics.json`, which sums the moves as the legend does: `summary` is the slice
reply's `plate_summary()` with the prepare and travel times and the travel distance; `features` has
each feature type's seconds, share of the normal-mode print time and grams, keyed by the English
`;FEATURE:` names (`ExtrusionEntity::role_to_string()`), and a `Travel` row (the shares leave out
retractions, wipes, Z moves, tool changes and the prepare time, as the legend's do); `layers`,
paged, numbers layers as the slider's value (G-code layer index + 1, which `set_view {layer}` and
`get_state.preview` use) and carries each one's Z, so tools that number layers by the slider's label
can be joined on Z. `objects` lists the G-code's object instances, which it names by object and
instance number; an entry gets the `list_objects` index only when one model object has that name.
The wx-free shaping (`preview_features()`, `preview_layers()`, `preview_objects()`) is unit tested.

`view` is the legend's state, read from the libvgcode viewer it draws from
(`GCodeViewer::get_viewer()`), so only for the plate the Preview has loaded (its
`last_result_id()` is the plate's result id; the Preview loads a plate only while shown, so after
another plate is selected it keeps the last one's): view type, time mode,
the layer slider's range with its Z, which feature types and options are visible, and each view
type's colour range. The viewer recomputes ranges at its next frame after a legend change, so the
closure draws the Preview first; the ranges span the whole print but count travels, wipes and Custom
extrusions only while those rows are shown. For another plate `view` is null and a `note` says how to
show it. `get_state.preview` is the same state without the legend, or null when the Preview holds no
toolpaths of the current plate.

The tool refuses while anything slices or exports (`Plater::is_slicing_any()`), since a worker may be
rewriting a result whatever its valid flag says, and for a plate without an up-to-date result. A
G-code file opened for viewing fills the current plate's result without marking the plate sliced;
that plate is read while the Preview shows it (`Plater::only_gcode_mode()`). Of a project opened from
a sliced 3MF, a plate the Preview has not shown keeps only the file's totals, without moves, so it is
refused with "slice again for layer data". The serializer's pass over the moves runs on the UI
thread, about 15 ms on a plate of 4 million moves.

`set_view` changes the Preview through the functions the legend's own controls call
(`GCodeViewer::pick_view_type()` for the combo, `toggle_extrusion_role_visibility()` and
`toggle_option_visibility()` for its rows, `set_time_mode()` for the mode button), so a call leaves
the Preview as the same clicks would. Picking a view type therefore shows every feature type again
when it is Line Type, and is written to the app config as `preview_last_view_type`, which the "Last
used" preview default restores. Before a slice there is no legend to pick from, and `view_type` only
sets the type the next toolpaths show. `legend` names rows as `get_preview` lists them, in any case
and with `_` or a space between words, and clicks a row only when its state differs. Hiding a row
the print lacks, a feature type or option some other print has, does nothing, since agents hiding
everything but one feature name rows from the whole list. The viewer keeps one visibility set, so feature types can be set
whatever the view type, and are applied after it. Every key, and the time mode, is checked before
the Preview changes. `time_mode: "silent"` needs a separate silent-mode estimate, which the legend
offers only when the two estimates read differently (`can_switch_time_mode()`). `layer_z` takes the
layer the slider gives a height, `IMSlider::get_tick_from_value(z, true)`: the first layer whose top
is at or above it, which is where a custom G-code at that height goes, so a `z` lands on the same
layer in every tool. Layer replies carry `layers` [first, last, count] and the `z` of first and
last.

## Print host uploads

Sending G-code to a print host (OctoPrint, Moonraker, PrusaLink, Duet and the others in
`PrintHost::get_print_host()`) goes through the GUI's own dialog. The Speed Dial command
`send_gcode` ("Send G-code to printer host", the dialog's title) calls `Plater::send_gcode_legacy()`,
which the Print button calls for such a printer: it opens the Send dialog, whose file name an agent
sets with `ui_input` and whose Upload or Upload and Print button it presses with `dialog_respond`.
The plate is then sliced if needed, its G-code exported in the background process, and the upload
queued. Host settings are printer settings (`host_type`, `print_host`, `printhost_apikey`, and
`bbl_use_printhost` on a Bambu printer), written with `set_settings {scope: "printer"}`. Where the
Print button would not send to a print host, the command fails and says why: the printer prints
through the Bambu network or printer agents (`PresetBundle::use_bbl_network()`,
`use_printer_agents`), or has no `print_host`, the condition `MainFrame::can_send_gcode()` gives the
button. It is journaled like any `run_action`, so a replay uploads again.

`get_state.uploads` lists the session's rows of the upload queue (`PrintHostQueueDialog`), as the
queue window shows them: `id` (its ID column, from 1), `state` (`queued`, `uploading`, `error`,
`cancelling`, `cancelled` or `completed`), `progress` (percent), `host`, `filename`, and `error`, the
queue's message, when there is one. Uploads a person started are listed too, so an agent can wait
for any of them by polling. A failed upload also opens the queue's error dialog
(`PrintHostQueueDialog::on_error()`), which blocks changes until it is closed with
`dialog_respond`.

Two properties of the queue make its rows a reliable answer. A row reads `completed` only once the
host has accepted the file: while the bytes go out, `PrintHostJobQueue::sent_percent()` stops at 99,
and only `perform_job()`'s 100 after a successful `upload()` completes the row, so a host that
refuses the file after the last byte turns the row into an error without it reading completed on
the way. And rows are added on the UI thread: `PrintHostJobQueue::enqueue()` runs on the background
slicing thread, reads what the row shows (host, upload path, file size) before the upload thread can
remove the source file, and posts `append_job()` to the queue dialog itself, the handler the upload
thread's progress and error events go to, so the row exists before any of them is handled.

## Scripts and the journal

```jsonc
{ "orca_script": 1, "name": "repro #12345",
  "vars": {"model": "${script_dir}/part.stl"},
  "steps": [
    {"tool": "open_files", "args": {"paths": ["${vars.model}"]}},
    {"tool": "slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}},
    {"tool": "run_action", "args": {"id": "orca_command:open_preferences:orca"}, "await": false, "save_as": "prefs"},
    {"tool": "dialog_respond", "args": {"dialog": {"title": "Preferences"}, "button": "close"}},
    {"tool": "wait", "args": {"for": "job", "job": "${jobs.prefs}"}},
    {"tool": "screenshot", "args": {"target": "canvas", "save_path": "${out_dir}/preview.png", "overwrite": true}}
  ]}
```

A script is JSON with comments allowed. `run_script` runs a file or an inline script as a job whose
steps call tools as its children (`ToolContext::call_child()`), so steps are not journaled and a
script's `wait {for: "idle"}` doesn't wait for the script. Every string is substituted recursively
with `${script_dir}`, `${out_dir}`, `${env:NAME}`, `${vars.NAME}` and `${jobs.NAME}`; a string that
is just one placeholder becomes the referenced JSON value. A step that returns a job is awaited,
up to `timeout_s` (default 120), before its `expect` is checked. `"await": false` returns the job
at once and stores it under `save_as`, so later steps can answer a dialog it opened and then
`wait` for it. `expect` maps JSON pointers into the result to matchers (`eq ne gt ge lt le contains
regex exists empty`). A failed step or expectation stops the run unless the step is `optional`.
Auto-answer rules registered by a script end with it. `vars` passed to `run_script` override the
script's. A step naming an unknown tool makes the script invalid before anything runs; so does any
field of the wrong type, and broken JSON is reported with its line and column. The result, also
written to `<out_dir>/result.json`, is `{passed, name, failed_step, steps: [{index, tool, args, ok,
duration_ms, result | error, expect_failure, cancelled, reason}], out_dir, result_file}`. A failed
step's `reason` says why in parts a host can word: `cancelled`, `timeout {seconds}`, `expect {pointer,
op, expected, actual, exists}`, `expect_invalid`, `placeholder {text}`, `missing_job {name,
saved_by}` (the step that saves that job, when one does) or `tool` (the tool's own error). A script
file's `out_dir` defaults to `<script name>.out` beside it. An inline script's defaults to
`<datadir>/mcp/out/script-<timestamp>`, which is also its `${script_dir}`.

The engine (`McpScript.cpp`, wx-free) and the job around it (`run_script_job()` in
`McpToolsScript.cpp`) are shared by `run_script`, `--mcp-script` and the Run Script dialog. A run
stops before its next step, and stops waiting on a step's job, once the service stops or a stop was
asked for any job in its lineage (`McpService::request_stop()`), so a nested `run_script` stops with
the run that holds it. Stop requests last the session, and `JobRegistry` records each job's ancestor
chain when it is created, so a descendant still reaches its root after finished ancestors are
forgotten. A step during which the stop came is failed as `cancelled` whatever it returned (a
cancelled slice still reports its plates) and ends the run, `optional` or not. The engine reports
each step as it starts and ends, and can run a single step with the jobs earlier runs saved.

`orca-slicer --mcp-script FILE [--mcp-script-out DIR]` runs a script once the GUI is ready. It
doesn't open the endpoint unless `--mcp` is also given. It writes `result.json` and screenshots to
the output directory, which defaults to `<script name>.out` next to the script. Then it closes the
app without prompting and exits 0 if the script passed, 1 if a step failed, and 2 if the script is
invalid. `OrcaSlicer.cpp` resolves both paths against the original working directory, since the
GUI changes it. wx ignores `OnExit()`'s return value, so `GUI_Run()` returns
`McpService::script_exit_code()` after `wxEntry()`.

### Script runner dialog

File › Run Script… and the Speed Dial command `open_script_runner` open `ScriptRunnerDialog`, where a
person opens or pastes a script, reads its steps and runs it whole or one step at a time. It is a
`WebViewHostDialog` page (`resources/web/dialog/ScriptRunner/`), modeless and one at a time
(`MainFrame::show_script_runner()`). Opening it starts the service without the endpoint, as
`--mcp-script` does, so a person runs scripts with the server off.

`McpService::start_script_run()` runs the script as a job of `run_script`'s kind but outside the
session journal, since a person's run is not an agent's call. A thread the service owns follows the
job past a `running` or `blocked_by_dialog` reply to its result, posts it once, and is joined by
`stop()`. Step reports and the result reach the dialog through the service's dispatcher as
`ScriptRun` events, which the dialog drops once it is gone. `ScriptRun::stop()` latches, so a stop
asked before the job exists still counts, requests the stop for the job (`JobRegistry::request_stop()`)
and cancels its operation as the `cancel` tool does. Every call the run and its nested scripts make
then sees `ToolContext::cancelled()`, so a step that waits, such as `wait`, returns at once. `suspend()` and `stop()` stop every run first, while the main
window they run in is still the current one.

Before a run the dialog describes each step with `describe_step()` (`McpScriptSummary.cpp`, wx-free):
what it acts on, and flags for effects beyond the open project (writes a file outside the output
folder, changes presets, replaces the project, sends to a printer). It reads the
arguments after substitution, and nested scripts as their run sees them: an inline one with the outer
script's placeholders already applied, as `run_script` substitutes the whole step first, and a file
with its own vars and output folder; a step whose effect can't be
read (a plugin action, a nested script too deep or unreadable) gets every flag. Flags only inform. A
replacing step either checks unsaved changes itself (`new_project`, `open_files` of a project,
`calibrate`) or goes through the app's own save question (`close_with_confirm()`). When the project
has unsaved changes and the steps to run hold a step of the first kind, or one of the second kind
with a step that can answer a dialog, the dialog asks whether to save first. Yes saves; No lets the
run's replacing tools treat the run as `discard_changes: true` (`McpService::discard_authorized()`,
read from the job lineage when the tool checks), so changes are dropped only when a step really
replaces the project. The runner never answers the app's own question.

The dialog's window is named `kHiddenFromAutomation`: `ui_snapshot` and locators skip it, web view
scripts never pick it, a key sent without a target never goes to the focus inside it, and on X11,
where a window capture reads the screen, it hides for the moment of the capture. Its own wording is
translated, from the structured step descriptions and failure reasons; a tool's own error, which
agents read in English, is shown as a detail under a translated lead. Tool titles are marked with
`L()` so the dialog can show them translated while `tools/list` keeps the English.

The session journal records client calls to journaled tools in order: every mutate tool, plus
`wait` and `screenshot`, which a replay needs. It keeps the last 5000 calls, dropping the oldest
call whose outcome is known; a call still waiting for its reply or its job keeps its place. GUI tools record locators instead of refs through
`ToolContext::set_journal_args()`. `export_script` returns it, and with `path` writes it, through `Journal::to_script()`; `clear`
starts a new recording. `Journal::to_script()`
drops failed calls and an input call that only listed the menu it opened when the next call repeats
it with a `popup_item` (otherwise it stays: its right click changed the selection), turns a call that
returned a job into a step with `"await": false` and
`"save_as": "jobN"`, points the `wait {for: "job"}` that followed at `${jobs.jobN}` (dropping waits
on jobs it never saw start) and rewrites screenshot paths to `${out_dir}/<file name>`. It moves a
recording's path there too (`start_recording` records the one it used), and an encode's `save_path`, and
an encode's source when an earlier `start_recording` recorded it; another source stays where it is.

A call that runs as a job is judged by its job's final result, not by its reply. `McpCore` tells the
journal the job's id when it starts it (`Journal::job_started()`) and its result when the handler
returns (`Journal::job_ended()`), before `JobRegistry::finish()`, so an export right after the
`wait` that saw the result already knows it. A job that fails after its reply came back as
`running` is dropped like a call that failed at once, so a script never replays a call that did
nothing (a slow CAD rebuild that then fails). A call whose outcome is still open, because its job
is still running or it has not replied yet, is left out, and `export_script`'s reply lists it in
`left_out_running: [{tool, job}]` (`job` is absent for a call that is not one); a later export
includes it once its job has succeeded. A call whose reply was `blocked_by_dialog` stays in the
script while its job runs and whatever its job's result: the steps after it answer its dialog, and
a replay without it would fail at the first of them. A call whose job was still running when a later
call acted with a modal open (`dialog_respond`, or a recover tool's input while
`UiInvoker::open_dialogs()` lists one) stays the same way, whether it had replied yet or not, since that
dialog may be one its job opened; the journal can't tell whose dialog it was, so it keeps every such
job, as a step with `"await": false`. Its replayed reply can come before the dialog opens, so each
call that acted with a modal open is preceded in the script by `wait {"for": "dialog"}`, expected to
be satisfied. Only the call's own job counts as its job: a
`wait` that timed out names the job it waits on in its reply, and stays a plain step.

## stdio bridge

`orca-slicer --mcp-stdio [--datadir DIR] [--mcp-port N]` serves clients that speak only stdio,
such as Claude Desktop; `main` handles it before any GUI or CLI processing (`McpStdioBridge.cpp`).
It POSTs each line of JSON-RPC from stdin to the instance in the discovery file (or `--mcp-port`)
with the token and the headers `bridge_headers()` derives: `MCP-Protocol-Version` (negotiated, or
from `_meta`), `Mcp-Method`, and `Mcp-Name` for `tools/call`, `prompts/get` and `resources/read`.
Replies go to stdout; a 202 writes nothing. When nothing answers, it starts `orca-slicer --mcp`
detached and waits up to 60 s. If that process exits within seconds, a running instance without
MCP took the launch over, and every request is answered with an error saying to turn the server
on in Preferences. A request that could not be sent, or was refused with 401 because the token
changed, is sent once more to the endpoint the discovery file names now: the app restarted, or its
port or token changed in Preferences. A request that was sent but lost its reply is never sent
again, since it may have run. The client gets an error saying so, and later requests go to the new
endpoint. A request gets 5 s to connect, 30 s to be sent and 120 s for its reply (a tool replies
within its 45 s budget, `wait` within 50 s), so a server that never replies can't hang the agent.
A line that is valid JSON but not one message object, such as a batch, gets an error reply.

## Configuration

The CLI options are in `CLIMiscConfigDef`, so the GUI still starts: `--mcp`, `--mcp-port N`,
`--mcp-token T`, `--mcp-script FILE`, `--mcp-script-out DIR` and `--mcp-stdio`. The AppConfig `app`
section holds `mcp_enabled` (false) and `mcp_port` (13619). Preferences › General › AI features
opens with a line on what agents can do and who can connect, then has five rows:

- **"MCP server"**: checked while the server runs. A dot and a line beside it give the state:
  "Running", "Off", "Not running" (enabled, but the start failed), that another window serves
  agents and this one takes over when it closes (refreshed every second while so), or the reason it
  could not start. A message from the last change, such as a rejected port, follows "Running".
- **"Server port"**.
- **"Server address"**: the URL, dimmed while the server is off, when it shows the port turning
  it on would use.
- **"Connection type"**: HTTP (the default) or stdio, for the setup the next row copies, with a
  line under it on what the choice means for the agent. It is not saved: it shapes only the copied
  text.
- **"Add to an agent"**: a picker, "Copy", and a line under them saying what to do with the copied
  text. An HTTP setup holds the URL and the token in force; a stdio setup runs
  `<executable> --mcp-stdio`, using the AppImage's own path inside an AppImage, and holds no token,
  since the bridge reads it from the discovery file:
  - Claude Code: `claude mcp add --transport http --scope user orcaslicer <url> --header "Authorization: Bearer <token>"`,
    or `claude mcp add --scope user orcaslicer -- "<executable>" --mcp-stdio`.
  - Codex: an `[mcp_servers.orcaslicer]` table for `~/.codex/config.toml`, with `url` and
    `http_headers`, since `codex mcp add` takes a token only from an environment variable, or with
    `command` and `args`.
  - opencode: an `mcp.orcaslicer` entry of type `remote` with `url` and `headers`, or of type
    `local` with `command`.
  - DeepSeek Harness: an `insert` row with a `@deepseek-ai/dsh-mcp-client` entry for
    `~/.dsh/cordis.patch.yml`, which every profile applies: `streamable-http` with `url` and
    `headers`, or `stdio` with `command` and `args`.
  - "Other apps": an `mcpServers.orcaslicer` entry, the shape most apps read: of type `http` with
    `url` and `headers`, or with `command` and `args` for apps that only take a command, such as
    Claude Desktop.

HTTP is the default because its URL stays the same whichever OrcaSlicer build runs, and starting an
agent session never starts OrcaSlicer: the stdio bridge launches the app as soon as a client
starts it, so a user-wide registration would open OrcaSlicer with every agent session. The cost is
that OrcaSlicer must be running with the server on before the agent connects, and a registration
holds the token: a new token (delete the token file, then turn the server off and on) or a new port
means copying the setup again. Stdio suits an agent that should open OrcaSlicer on demand, and apps
that can't connect over HTTP; its setup names this executable, so it breaks when the program moves.

## Testing

Unit tests are Catch2 cases tagged `[Mcp]` in `tests/slic3rutils/test_mcp_*.cpp`, run with
`slic3rutils_tests "[Mcp]"`. `test_mcp_protocol` covers both eras and argument validation;
`test_mcp_http` the endpoint rules and a live server (deadlines, eviction, stop); `test_mcp_token`
who owns the discovery file and when a port counts as held by another window; `test_mcp_core`
`UiInvoker`, `McpCore` and `JobRegistry` on a hand-stepped dispatcher (guards, abandoned calls,
jobs, blocked_by_dialog, admission, slots, idle, journal); `test_mcp_operations` the slice and
export outcomes; `test_mcp_script` parsing, substitution, matchers and await; `test_mcp_ui` labels,
snapshot text, ref liveness, synthetic key state, a key's way through the hooks of its target's
parents (with a display), how ImGui items are labelled and named, value
matching and the model-target projection and surface check (`CanvasTarget`); `test_mcp_results`
what covers a canvas click (`imgui_window_at()`), the surface meshes of a modifier target, fresh GL
toolbar state and the auto-answer press (with a display), the slice reply's plate summary,
`get_preview`'s feature, layer and object tables, the legend rows' names, the slider's layer for
a height, menu paths and listings, the UI activity hook, the journal's dropped listing clicks and
calls whose job failed or still runs (with a replay of the export), the
menu capture on real wx menus, row paths and their snapshot text, (with a display) the rows of
real trees, lists, checklists and tables, clicks and checkbox input on them, and popups' owning
windows, plate icon names and bed picking ids, an upload's percentage while its bytes go out, and
the `send_gcode` command's catalog entry;
`test_imgui_wrapper` the
ImGui recorder (named items, kinds, values, text, clipping) on frames drawn without OpenGL;
`test_mcp_recording` the Matroska file (read back closed and unclosed), the sink's order, memory
budget, non-blocking push, finish, abort and failed frames, the video transform, overlays and frame
repeats, captions and what they hide, the ffmpeg command and listings, the files recordings and
encodes reserve, and the journal's recording paths; `test_mcp_tools` setting serialization; `test_mcp_config` the object and volume scopes' keys and
effective values, the names and places `save_preset` accepts, enum labels and descriptions, the settings search,
per-variant entries and their writes, preset comparison, `changed`, which unsaved changes a call discarded, which presets
`delete_preset` refuses or deletes with a custom printer, the project scope's slot colours,
flushing multipliers and matrix, and the plate scope's filament orders and layer ranges and
`list_plates`' layer numbers and layer G-codes, the preferences scope's keys, and `calibrate`'s arguments,
defaults, the dialogs' shared rules, VFA's auto-adjust and the parameters the journal records. End to end, `tools/mcp/orca_mcp.py` launches
the app on a private Xvfb display on Linux (with `-noreset`, and the pointer parked in a corner so no
tooltip lands in a screenshot), or on the desktop on macOS and Windows, and runs
`tools/mcp/smoke_test.json`. `tools/mcp/tests/script_basic.json`
checks `--mcp-script` headless, `tools/mcp/tests/gizmos.json` every gizmo through the GUI tools,
`tools/mcp/tests/multicolor.json` object and part filaments and the project scope, `tools/mcp/tests/presets.json` saving,
reading and comparing presets, explaining settings, per-variant entries, `changed`, reverting keys, answering the
unsaved-changes prompts of a preset switch and a new project, deleting presets, exporting and importing
presets with a path, and reading a preference changed in the Preferences dialog,
`tools/mcp/tests/plates.json` the plate scope, `list_plates` and the filament slot commands,
`tools/mcp/tests/calibration.json` every calibration test and its preset answers,
`tools/mcp/tests/dialogs_canvas_input.json` auto-answers that run a button's handler and canvas
targets on negative parts and from below the bed, `tools/mcp/tests/slice_stats.json` the slice
reply's statistics on one and two colours, `tools/mcp/tests/preview.json` `get_preview`,
`get_state.preview` and `set_view`'s preview arguments, `tools/mcp/tests/context_menus.json` context
and button menus listed and run without one ever showing, `tools/mcp/tests/rows.json` object list
rows, checklist rows, the object settings table and the print button's popup,
`tools/mcp/tests/recording.json` a recording with clicks, a dialog and a slice, stopped and encoded,
and one a script ends, `tools/mcp/tests/plate_icons.json` plate icons (lock, settings, arrange, the plate menu, move to
front), `tools/mcp/tests/webview_modal.json` scripts in the preset bundle dialog and the setup
wizard, `tools/mcp/test_webview_modal.py` the dialogs scripts are refused in and a page that closes
its dialog, `tools/mcp/test_set_view_preview.py` the
legend combo's semantics of `set_view {view_type}` and its preview refusals,
`tools/mcp/test_export_running_job.py` an export during and after a job that outlived its reply,
`tools/mcp/test_send_gcode.py` `send_gcode` and `get_state.uploads` against a stub print host that
answers 200 or 500,
`tools/mcp/test_stdio_bridge.py` the stdio bridge, and
`tools/mcp/test_recovery_under_load.py` answering a dialog while stalled clients hold every
connection. See `tools/mcp/README.md`.

`test_mcp_scene` covers the scene and layer tools without the app: `transform`'s `size`, the shape
commands' size param, notifications updated before the first ImGui frame, `arrange`'s option rules,
worker-job admission on a stand-in worker (a job past the reply budget, a slow cancel and a timeout,
three ways to start one, recover input and a person's job), `list_objects`' descriptions of a `Model`
built in the test (placement and boxes on the bed, mirrors, filament fallback, precise seam subtypes,
height ranges, the summary of a variable layer height profile), `add_part`'s type names and shape
sizes, the object structure commands' params, the height ranges `set_layer_ranges` accepts (order,
overlaps, bounds, keys), and `set_layer_gcode`'s edits of a layer slider built in the test (by layer
and height, in-place edits, refusals that leave the slider as it was, the type names). On the app,
`tools/mcp/tests/scene.json` checks object structure and placement (transform on instances, plates and
parts, what `list_objects` reports of them, shapes at a size, arrange and Fill bed, `add_part`, the
object structure commands, then all of them on one scene through to a slice);
`tools/mcp/tests/layers.json` height range modifiers and variable layer height profiles (set, read
back, undone, sliced, refused, kept by a 3MF) and layer G-codes (placed by layer and height, edited in
place, refused, sliced, kept by a 3MF); and `tools/mcp/test_worker_admission.py` admission while an
arrange started three ways runs.
