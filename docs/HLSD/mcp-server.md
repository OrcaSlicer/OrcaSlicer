# MCP Server

## Why it exists

The MCP server lets an AI agent drive a running OrcaSlicer through the Model Context Protocol.
The same tools serve two audiences. A developer's agent reproduces a GitHub issue: it loads the
files, picks presets, changes settings, clicks through the GUI and slices. It also takes
screenshots before and after a fix and replays the steps from a replay file. A user can drive
OrcaSlicer from Claude Code, Codex, Cursor or Claude Desktop.

The server runs inside the GUI process, because most of what an agent reproduces is GUI
behaviour, and reuses the Speed Dial catalog, so a feature added to the palette reaches agents,
replays and scripts too. It is off by default; until it is first turned on, `McpService` does not exist, so
there is no thread, socket, modal-dialog hook, ImGui item recording or synthetic input state. The code is in
`src/slic3r/GUI/Mcp/`; the protocol, transport, threading core, replay runner and operation logic
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
`outputSchema`, so no `structuredContent` is sent. The descriptions say what each reply holds instead:
Claude Code shows its model a tool's description and input schema but never its output schema, and
fails a call whose structured content does not match that schema, so a schema would teach its agents
nothing and could turn a call that already ran into an error. Clients that prefer structured content
over the content blocks would also drop the images and `list_widgets`'s tree. Tool failures, invalid
arguments included, are `isError` results the model can act on; an unknown tool is a JSON-RPC
`-32602` error naming close matches. `validate_arguments()` checks arguments against the tool's
schema first. Before it,
`normalize_arguments()` reads what agents commonly send in place of the schema's form, for calls and
replay steps alike, so the journal records the schema's form. An argument under a name the tool does
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

The server `instructions` (under the 4096 characters clients keep) give the workflow: `get_app_state`,
act, then check the result with the reading tools, keeping screenshots for what those do not report.
They say to prefer typed tools over clicks and never retry a call that returned a job, point to
`get_settings`' meta and search for what a setting does, say how a gizmo opens and is driven (and that
`pick_gizmo_file` only stands in for a file picker inside one, since its name draws agents to it for any
gizmo work), that the camera turns with `set_view` rather than a drag, and which indices are 0-based.
They also point to `search_tools {query}` as the one search across every catalog an agent calls into:
the tools, the operations plugins offer (which `tools/list` never shows, so nothing else tells an agent
they exist), Speed Dial actions, settings, the Python API and the Design tab's CAD tools. An agent rarely
knows which catalog answers "how do I add a brim", and each catalog has its own search with its own
arguments. Clients that defer tools load only their names and the
instructions at first, so the instructions are how an agent without a skill learns which tool to
load.

## Security, token and discovery

The bearer token is 32 random bytes as 64 hex characters in `<datadir>/mcp/token`: directory 0700
and file created 0600 with `O_CREAT|O_EXCL` on POSIX, the per-user ACL of `%APPDATA%` on Windows.
It stays out of AppConfig, whose writes and backups use default permissions. `ORCA_MCP_TOKEN`
(preferred, as it stays out of the process list) or `--mcp-token` overrides it for one run;
`get_app_state.mcp.token_source` says which is in force. Every start of the endpoint reads the file, so
deleting it and turning the server off and on in Preferences issues a new token. While
serving, `<datadir>/mcp/server.json` holds `{url, port, pid, version, token}` with the token in
use, so the stdio bridge and the harness connect without the launch environment. It is written
owner-only via an exclusive temporary file and a rename (`write_private_file()`), and removed on
stop, but only by the process its `pid` names: windows that share a configuration folder share the
file, and one that stops must not hide another's server from the stdio bridge.

## Lifecycle

`GUI_App::on_init_inner()` starts the service once AppConfig is loaded. That is before the first
startup dialog, such as the TLS prompt or the setup wizard, so an agent can answer those dialogs.
`--mcp` or the `mcp_enabled` preference opens the endpoint, while `--mcp-run` alone, or opening
Action Replay or the Run Script panel, only builds the catalog. `get_app_state.ready` stays false until `post_init()` calls `mark_ready()`, the Prepare
canvas has loaded OpenGL and the main window has built its tab pages, and tools that need the main
window fail with "still starting". Starting on Home, the canvas loads OpenGL at idle after
`post_init()` (MainFrame's GL prebuild), and before that a 3MF save would draw its thumbnail with no
GL functions loaded. The same idle queue builds the tab pages (`MainFrame::prebuild_pending()`); a
page's web view registers its `wx` script message handler with a script it waits for in a nested
loop, and on GTK calls that came in meanwhile have left that wait unfinished for good, so that no web
view made afterwards gets `window.wx` and a dialog page's own buttons do nothing. So `mark_ready()`
polls every 100 ms until both are done. A hidden canvas that can't get a GL context loads OpenGL on
its first draw instead, and the pages build only while no input arrives, so after 10 s ready no
longer waits for either. Becoming ready also starts the `--mcp-run` file. Notifications don't depend on this: one pushed or updated before the first
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
catalog, running jobs and a launch run carry on, because the call that opened Preferences may
itself be an MCP call still on the stack. The port field shows the port in use, `--mcp-port`'s
included, and only an edit of it is saved and reopens the endpoint, through
`GUI_App::restart_mcp_endpoint()`: the field also commits when focus merely passes through it. A
port set in Preferences replaces `--mcp-port` for the rest of the session, also while the server is
off. Closing the endpoint drops the session's auto-answer rules, and a client's rules
that a call still running registers afterwards are refused, so a person using the app afterwards
never has a prompt answered for them. Closing doesn't wait for in-flight requests, which
may be waiting on the UI thread that closes it; their replies are dropped. A bind failure is
logged, shown as a notification and in Preferences, and a start from Preferences never reruns a
`--mcp-run` file. The modal tracker is created with the service and picks up modal dialogs that
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
made already. On Wayland a 3D view that a change shows, such as by a tab switch, gets its GL surface
from the map event GTK handles when its main loop next runs, and the next call's closure can run
before that, finding the view not on screen. So `settle()` also waits, up to a second, until every
shown canvas has its surface.

A window the compositor isn't drawing, as while the display is off, gets no frames: GTK lays a window
out and paints it only in a frame, and after each frame it shows it waits for the compositor's
go-ahead, which doesn't come. Waits on the UI then time out, a list can't scroll to a row and the 3D
views capture blank, none of it for the reason the call would give. `undrawn_window()` asks the main
window's frame clock for a frame and waits up to half a second for it to run, so only a call that
failed asks it: a `poll_ui()` timeout, a blank canvas capture and a row `click` can't place say the
window isn't being drawn when it isn't.

`GuardClass` decides what may run when:

| Class | Tools | Runs behind a modal or inside another call |
|---|---|---|
| `Inspect` | get_app_state, wait, get_log, list tools, get_settings, screenshot, list_widgets, start_recording, stop_recording, encode_recording | yes |
| `Recover` | answer_dialog, auto_answer_dialogs, GUI input aimed at the topmost modal | yes |
| `Mutate` | everything else | no |

The closure checks the guards on the UI thread, since the app can change between posting and
running. A closing app or a stopped service fails the call with "shutting down". A frame being
recreated fails it with "retry". A mutate call is refused while a modal dialog is open, and the
refusal lists the dialogs. It is also refused when it would run inside another UI call's nested
event loop: a modal, `ProgressDialog::Update` or `wxYield`. The GUI input tools (`click`,
`set_widget_value`, `press_keys`) and `answer_dialog` run as recover calls. Once a target resolves outside the
topmost modal, they apply the mutate checks themselves (`UiInvoker::require_mutate()`), along with
operation admission (`McpCore::admit()`). `cancel_job` is a recover call too, so neither a dialog the
operation opened nor another call's nested loop keeps it out.

`ToolDef::inline_call` decides where a call runs. Pure observation tools run on the request
thread. Every tool that can activate UI runs as a job on its own thread, recover tools that click
in dialogs included, since a click can open another modal (Preferences, Keyboard shortcuts, Edit).
`JobRegistry` names jobs `j1`, `j2`, …, records each job's tool and parent, allows 64 running and
keeps the last 64 results, plus finished jobs whose parent (a running replay) may still wait for
them, up to 1024 in all. The request thread waits up to `ToolDef::budget_ms` (45 s, under
Codex's 60 s tool timeout); after that the reply is `{status: "running", job, tool, note}` and the
job keeps going until `wait {for: "job"}` (at most 50 s per call) returns its result. The server
never retries a mutating call, and every envelope tells the model not to either.

**blocked_by_dialog.** A handler that opens a modal blocks in its nested loop, so every 1.5 s the
request thread posts a probe closure, which runs inside a wx dialog's loop and returns the
`ModalTracker` snapshot as JSON; no `wxDialog*` crosses threads. With a modal open, the reply is
`{status: "blocked_by_dialog", job, dialogs}`. Its note says how to answer the topmost dialog:
`answer_dialog` for a wx dialog or a native message box whose buttons are listed, a person for any
other dialog marked `native: true` (a picker, or any native dialog on macOS), which `answer_dialog`
can't press.

On macOS a file picker, folder picker or message box pauses wx's pending events while it is open
(`wxCFEventLoopPauseIdleEvents` in its `ShowModal()`), and `CallAfter` closures with them, so behind
one the probe goes unanswered and so does every `run_ui()` call. A probe unanswered within 1 s
therefore falls back to the snapshot `ModalTracker` publishes for any thread: if its topmost dialog is
native, the reply is `blocked_by_dialog` with those dialogs and a note that tools needing the UI
thread fail until the dialog closes. Otherwise the request keeps waiting. A `run_ui()` call that times
out with a native dialog on top fails naming it, instead of reporting a busy UI thread. The dispatcher
stays `CallAfter`: running closures some other way inside the Cocoa modal session would bypass wx's
pause and re-enter the app while the dialog is up, so on macOS native dialogs are answered ahead.
On Windows and Linux a native dialog's own loop runs wx's pending events, so the calls go on. On
Windows wx runs them from a message hook when such a loop, or a menu's, takes a message off the thread's
queue, and `CallAfter` alone wakes only wx's own loop, so the dispatcher also posts the UI thread a message
(`wxWakeUpMainThread()`). The loops of menus and system dialogs take any message of the thread, so a call
made while one is open is picked up at once instead of waiting for an unrelated message.

**Blocking slots.** A request that may block (an inline call, or a job call within its budget)
first takes one of 32 slots. Without one, a job call returns its `running` envelope at once and an
inline call returns `{status: "pending"}`, so at most 32 request threads ever block, well under the
64-connection cap.

### Operations

Slicing and G-code export finish in the background process. Their handlers call
`ToolContext::begin_operation()` in a short UI closure before starting the work; that fails if
another operation runs. An export starts in that same closure. A slice first waits for the
background update that a change made just before has scheduled, which validates the new state, and
then requests the slice. A `cancel_job` that comes while it waits moves on `Plater::slice_cancel_seq()`,
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
then becomes a running job, as for any call. `cancel_job` (or the call's own timeout, 30 minutes unless
the tool sets a shorter one, as `arrange` does) asks the job to stop, but `Worker::cancel()` does not
wait, so the call keeps the operation until the worker is idle and then fails as cancelled. `cancel_job`
stops only a job an MCP call claimed. The worker doesn't record who queued a job, so a job a person
starts while a call is still between its closures, after one that found the worker idle, is claimed
by that call too: its reply then waits for that job. The window is the call's own length, a
second or so for most calls and up to its poll timeout for a call that waits on the UI.

**Admission.** While an operation job runs, every other `Mutate` call is refused ("Busy: job j12 is
running an operation (slice, export, arrange or another background job)"), whichever client, replay or script
sends it. So an agent can't change the project under its own operation. While the worker runs a job
no call holds, a person's (typing in the Text gizmo) or one not yet claimed, changes are refused too
("Busy: a background job ..."); reads are not. The check, `McpCore::admit()`, runs when the call
arrives (operations only, since the worker lives on the UI thread) and again in each of its UI
closures, where operations start, so a call admitted just before an operation began still can't
change the project under it. Inline tools, recover tools answering the topmost modal and
`allowed_during_operation` tools (`cancel_job`) still run; the GUI input tools apply the same check once
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
that. `cancel_job` also drops a pending automatic reslice: it stops the delay timer,
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
so a replay or script waiting for idle doesn't wait on itself. Only the job registry, the `ModalTracker`
snapshot and the journal are shared between threads, each behind a mutex. All app state is read on
the UI thread.

## Tool catalog

A tool's name says what it does in the words the GUI uses, so a person's request and an agent's search for a
tool meet: `set_height_ranges` edits what the object list calls height ranges (Height Range Modifier),
`set_variable_layer_height` runs the Variable layer height panel, and `run_replay` runs what Action Replay calls
a replay file. Where the GUI has no name for the thing, the name is a plain verb and noun (`list_widgets`,
`answer_dialog`), and a word the GUI uses for something else is avoided: `run_script` would read as the Run
Script panel, which runs Python.

| Group | Tool | Purpose |
|---|---|---|
| App | `get_app_state` | version, platform and display backend, ready, window, the MCP endpoint, whether Python runs and Python scripts are allowed, whether the Design tab is on, tab, mode, project, presets, plates, objects, the selection, slicing, the Preview's view type, time mode and layer range, print host uploads, the open gizmo, dialogs, menus closed unseen, auto-answer rules, notifications, jobs |
| | `wait` | `ready`, `idle`, `dialog`, `no_dialog` or a `job` (default the job given, else `idle`); `timeout_s` up to 50 |
| | `get_log` | end of the current log file, filtered by minimum level |
| Speed Dial | `search_actions` | search the action catalog, paged |
| | `run_action` | run an action by id (or the name inside it), with its optional `param`; an error when the action did nothing; waits for a background job it starts (arrange, orient) |
| Project | `open_files` | load models, a project or G-code by absolute path, with no dialog |
| | `new_project` | start an empty project (`discard_changes` if it has unsaved changes); `changes` keeps or discards modified presets |
| | `generate_calibration_test` | a Calibration menu test with typed parameters and no dialog: a new project with the test model and the preset changes the test needs, printed with filament slot 1; refused while presets are modified unless `changes` keeps or discards them |
| | `save_project` | save the project as `.3mf` to a path (a `.gcode.3mf` path is refused: that is `export` `sliced_3mf`) |
| | `export` | `gcode`, `sliced_3mf`, `3mf` or `stl` to a path, by default the one its extension names; `gcode` is an export operation |
| Presets | `list_presets` | printer, process or filament presets with flags (system, user, inside the project, the preset inherited from), paged |
| | `select_preset` | select by name through the selector (filament per slot), showing a hidden preset first; `changes` transfers or discards unsaved changes without the prompt |
| | `get_settings` | values and metadata (description, settings page and group, enum labels, per-variant entries) by key, by page or group, or found by words matched in keys, labels and descriptions, of the edited or any saved preset, of an object's or part's overrides with the values they print with, of the project's own settings (plate type, slot colours, flushing volumes, purge mode, prime tower positions, mixed slots), of a plate's (plate type, print sequence, spiral vase, filament orders, name, lock), or of named preferences (read only); `compare` lists only what differs from another preset, the saved one or the parent |
| | `set_settings` | set values the way the settings tab does, all or none; one entry of a per-variant value; object and part overrides and filaments; the project's and a plate's own settings through their controls' and dialogs' code; `null` takes a preset's key back to its saved or parent value, `reset_all` drops a preset's unsaved changes first; replies with every setting that `changed` |
| | `save_preset` | save the preset being edited under a name, as a user preset or inside the project |
| | `delete_preset` | delete a user or project preset, selected or not, as the tab's Delete does (destructive) |
| Objects | `list_objects` | objects (filament, auto drop, overrides, brim ears, height range modifiers, variable layer height profile), instances (placement, mirrored, bounding box, canvas rectangle on request) and parts (type, filament, placement and bounding box on the bed, scaled and mesh volume, painted facets, text and SVG sources), paged |
| | `list_plates` | plates with the settings they print with and those they set themselves, objects, name, lock, slice state, prime tower position and layer G-codes, paged |
| | `select_objects` | select objects, instances or parts (indices checked against what the object has), all, or none |
| | `transform` | translate, rotate, scale or size one instance, relative or absolute, move it to a `position` or to another plate; or the same for one part (`volume`) |
| | `add_part` | add a part, negative part, modifier, support blocker or enforcer, or precise seam to an object: a shape at a size or a model file, optionally at a position |
| | `arrange` | arrange every unlocked plate or one plate, or fill an object's plate with instances or copies, with the Arrange options panel's settings; waits for the job |
| | `set_height_ranges` | replace an object's height range modifiers: Z ranges with their own layer height, filament and process settings |
| | `set_variable_layer_height` | the Variable layer height panel's Adaptive (with its quality), Smooth (radius, keep min) or Reset on an object |
| Slice | `slice` | current, one or all plates, then the Preview tab as after the Slice button; outcome for each plate, and for an `ok` one its time, filament totals with the flush, cost, layers, max Z, filament changes and grams per slot by use |
| | `cancel_job` | stop the running slice or G-code export, a pending automatic reslice, or a background job an MCP call started (optionally only a given job), or with `job` a running `encode_recording`; allowed during an operation |
| | `set_custom_gcode_at_layer` | add or remove a plate's pauses, custom G-code, custom templates and filament changes on its sliced layers, as the Preview layer slider's menu does |
| View | `set_view` | tab (Design included), 3D view (Prepare, preview, assemble), camera (a direction also as `view`; the Design view's on the Design tab) and `orbit` around what it looks at by degrees, preview layers (by number or height), color scheme as the legend's combo picks it, legend rows and time estimate, window size |
| | `screenshot` | window, the 3D view shown (the Design view on the Design tab), topmost dialog or one widget: a downscaled image and a full-size PNG |
| | `get_slice_stats` | a sliced plate's summary, time and grams per feature type, per-layer Z, height, time and grams (paged), grams per object instance, and the Preview's legend state and colour ranges |
| Recording | `start_recording` | record the main window with its modal dialogs, or the 3D view shown, into an MJPEG `.mkv`, with the current tool call as a caption and the pointer highlighted when asked for, and with `encode` an MP4 of it once stopped |
| | `stop_recording` | capture the last state, end the recording and report its file once written: length, frames, drops, the mean UI time of a capture, size and why it ended; the job of the encode `start_recording` asked for |
| | `encode_recording` | encode a recording into an H.265 or H.264 MP4 with the system ffmpeg; cancellable |
| GUI | `list_windows` | the open windows, modal or not, with refs for `screenshot`, `list_widgets` and the input tools, paged |
| | `list_widgets` | widgets, rows of lists, trees and tables, popups, menus, GL toolbar items, gizmos, the Prepare view's plates and plate icons, and ImGui items, with refs |
| | `click` | activate a ref or locator, click a row's cell or a plate icon, or click a point of a 3D canvas or custom-drawn widget; list the menu it opens, or run one of its items |
| | `set_widget_value` | set a widget's text, value, selection or checked state, or a row's checkbox |
| | `press_keys` | send a key chord to a target or the focused window, whose panels' key hooks see it first; list or run the menu it opens |
| | `mouse_gesture` | canvas gestures: move, down, up, click, drag, wheel, at pixels or at points of the model (the Design view at pixels); list or run a right click's menu; reports when a gesture moved the camera |
| | `pick_gizmo_file` | what the open gizmo's file pickers do, with a path: SVG change file and save as, texture import |
| | `answer_dialog` | press a button of the topmost wx dialog or native message box, or of its page for a web dialog |
| | `auto_answer_dialogs` | answer matching modals as they open (native ones unseen, web ones on their page), and one already open |
| | `run_javascript` | run JavaScript in a web view (the topmost dialog's while a modal is open), with a page element as `el`, and return the result |
| Replays and scripts | `run_replay` | run a replay file or inline replay, with results per step |
| | `export_replay` | write the session journal as a replay; calls whose job is still running are left out and listed |
| | `run_script` | run Python code against `orca.host`, calling tools through `orca.host.tools`; needs the Python setting (see Python scripts); cancellable |
| | `search_python_api` | search the `orca` package in the running build: names, signatures and documentation, paged |
| API | `search_tools` | look up any registered tool or plugin operation by name, list every name, or search every catalog by words |
| | `call_tool` | call a registered tool or plugin operation by name; runs and is journaled as that tool's own call |
| CAD | `list_cad_tools` | the Design tab's CAD tools, each with its access and one-line summary, narrowed by a query (CAD builds; answers with the CAD feature off too) |
| | `describe_cad_tool` | one CAD tool or a list: summary, details, params with `required`, how a failure shows and examples (CAD builds; answers with the CAD feature off too) |
| | `call_cad_tool` | run a CAD tool of the Design tab: sketches, features, reads and measurements, feature and body edits, undo, commit to plate (CAD builds, Design tab enabled) |

Each tool has a lane, the way it acts, set by `ToolRegistry::add()` from `tool_lane()`: core (the
session, recovery and Python), API (project data by index and name, JSON in and out) and UI (the GUI
itself). Every caller reaches every tool; the lane is a hint for the agent choosing one. It starts each
description (`[core]`, `[api]`, `[ui]`), orders `tools/list` (core, then API, then UI) and orders the
server instructions.

Each tool also has an access label, which `tools/list` sends as MCP's annotations. Clients act on it
differently: Codex asks a person before every call of a destructive tool (unless it is also read-only),
and its `writes` mode before every call of a tool that is not read-only; Claude Code asks by its own
permission rules whatever the label says, runs read-only calls in parallel and retries idempotent ones.
So the label marks what a call does by its nature, not everything a call could reach, which would put
a confirmation on calls an agent makes all the time.

`read_only`: the tool changes nothing in the app or the project. `screenshot` is read-only, since its
PNG replaces no file without `overwrite`; `export_replay`, which can clear the journal, is not.
`destructive`: every call deletes what a person made, with no undo: `delete_preset`, which takes the
preset's file and its cloud copy. A tool that can lose work only when an argument asks for it is not:
`new_project`, `generate_calibration_test` and `open_files` drop unsaved changes only with `discard_changes`, an output
replaces a file only with `overwrite: true`, and `encode_recording` deletes its source only with
`delete_source`. Nor is a tool that runs what its arguments name: the input tools (`click`, `set_widget_value`, `press_keys`, `mouse_gesture`), which press
whatever is on screen, a Delete button included, `run_action`, `call_tool`, `run_replay` and
`run_script`. What they run may be destructive, but the client sees both in the arguments, and the
label would ask on every call. `open_world` (`openWorldHint`): the tool can reach outside OrcaSlicer, to a
printer, the cloud account or the network: `save_preset` (user presets sync), `delete_preset` (the cloud
copy), `run_javascript` (the page's own reach), and through what they run `run_action` (`send_gcode`,
`sync_presets`), `call_tool`, `run_replay` and `run_script`, which has the app's access to the computer.
No client is known to ask on it. `idempotent`: the same call again changes nothing more.

The tools that take an object index (`set_settings` object and volume scopes, `transform`, `add_part`,
`set_height_ranges`, `set_variable_layer_height`) also take an optional `object_id`, and where they take a
part index a `volume_id`; `select_objects` takes `object_ids`, `instance_object_ids` and `volume_ids`, one per
entry of `objects`, `instances` and `volumes`: the `ObjectID` values `list_objects` reports as `id`, which `orca.host` shows
too. `check_expected_ids()` refuses the call before anything changes when the index names another object
or part now, so a replay or script addresses what it read. `list_objects`' description says what the ids are for
and names the tools that take them, so each property only points back at it. Ids are allocated per
process, so the journal leaves them out of those tools' calls (`without_expected_ids()`), through
`call_tool` and inline `run_replay` steps too, and a replay addresses by index; a `run_script` step's code
and `args`, and a plugin operation's own arguments, are kept as written.

Descriptions keep one order: what the tool does, the arguments that change it, `Reply:` with the
fields it has, then edge cases; a long one puts each case on a line of its own (the scopes of
`get_settings` and `set_settings`, `click`'s web pages). What several tools share is written once,
since every session loads the whole tool list: the locator grammar in `list_widgets`'s description,
which hands out the refs, each `target` giving two examples and pointing there; the paging shape, and
that a part is a `volume` in argument names, in the server instructions. A tool that takes one plate
calls it `plate`, an index; one that also takes `"current"` or `"all"` calls it `plates`.

Each tool family registers itself from its own `McpTools*.cpp` through `register_*_tools()`.
List tools take `limit` and `offset` and reply `{items, offset, total}` (`page()`), with `next_offset` while
more remain; `search_actions` names its items `actions`. The server instructions give that shape once, rather
than every description, and each `limit` states its default. `McpPaths.hpp` requires
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
its `layer_height` and `extruder`, and the list can be handed back to `set_height_ranges` unchanged. An
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

### set_height_ranges

`set_height_ranges` gives an object's whole set of height ranges (`ModelObject::layer_config_ranges`),
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

### set_variable_layer_height

`set_variable_layer_height` runs what the Variable layer height panel's Adaptive, Smooth and Reset buttons
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

### set_custom_gcode_at_layer

`set_custom_gcode_at_layer` places what the Preview layer slider's right-click menu adds at the start of a layer
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
`get_slice_stats` number layers; `z` takes the layer whose top is at or above it
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
datadir, not the project, so a replay or script meant to be rerun passes `overwrite`. A filament temperature
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
asks for more.

Without a `scope`, a search and named keys look through the process, filament and printer settings
being edited, since a question about an effect rarely knows which tab answers it (stringing is
retraction on the printer tab and its overrides on the filament's), and an agent naming a key seldom
knows which tab holds it; each item names its `scope`, the first of the three that has the key. Every
other read without a scope (`where`, the whole listing, `preset`, `compare`) reads the process, and
`set_settings` writes it: a write takes one scope, whose values apply together as one undo step, so it
never spreads over several by itself. A key the scope lacks is listed in `unknown_keys`, and `scope_of`
names the scope that has it (those three, then the project's) when another does; `set_settings`
refuses such a key with that scope named, so an agent that passed a filament key with no scope learns
where it belongs instead of reading "unknown".

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

`select_preset` finds the name in the selector the GUI picks from: the sidebar's filament combo of the
slot, or the tab's combo, by the label shown or the internal name. A selector lists the visible,
compatible presets, and a hidden one is a system filament or printer the setup wizard has not added,
which a fresh data folder has for all but a few. Such a name is shown first (`show_hidden_preset()`):
the preset becomes visible in memory and the selectors are rebuilt, as they show the selected preset and
the filaments an AMS holds, so the wizard's list in `OrcaSlicer.conf`, and what the next start shows,
stays as it is; the reply names it in `shown`. A printer shown this way also shows its model's default
filaments when no shown filament suits it, as adding the printer in the wizard does
(`PresetBundle::load_installed_filaments()`), so the switch lands on one of them rather than on the
default filament. A preset's own name wins over another's alias that reads the same. An incompatible preset is refused as such, since no selector lists it, and an unknown name with
the closest names.

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
preset, and `discarded`/`modified` as `select_preset`'s. A replay needs the preset to exist,
so replays create it first with `save_preset`.

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

The dialog's controls are named after their AppConfig keys, so `list_widgets` shows the key and
`set_widget_value {"target": {"name": "export_statistics", "within": "Preferences"}, "value": true}` changes one
through its own handler. The Speed Dial's `open_preferences` takes `<tab>[:<key>]` (`general`, `files`,
`behaviour`, `control`, `graphics`, `online` or `developer`) and opens the dialog there through `GUI_App::open_preferences()`,
scrolled to the key's control and focused; a key on another tab is shown on its own tab. Checkboxes,
number and text fields and the language combobox change this way. The other comboboxes (those of
`create_item_combobox()`, the log level, the login region and the network plug-in version) save from a
handler on their drop-down list, where a person's pick arrives, while `set_widget_value` sends the selection to the
combobox itself: it shows the new choice and saves nothing.

### Calibration tests

`generate_calibration_test` runs a Calibration menu test (temperature, max volumetric speed, pressure advance as tower,
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
`generate_calibration_test` refuses without `changes` and lists the modified keys: a previous test's settings would
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
the dialogs, and their descriptions point at `generate_calibration_test`.

### Design tab: `list_cad_tools`, `describe_cad_tool` and `call_cad_tool`

`call_cad_tool {tool, args}` is the one way into the Design tab, under the mutate guard. Its CAD
tools (`Mcp/McpToolsCad.cpp`) sit behind one generic tool rather than a schema each, so agents that
never open the Design tab pay nothing for them. One table, `kCadTools`, declares every CAD tool's
name, access and handler; `run_cad_tool()`, the busy gate and `cad_tool_table()` all read it.
`cad_tool_table()` is the table of record, built once from `tool_rows()` and `kCadTools`: one row
per CAD tool with a summary of one sentence (at most 120 characters), details, typed params with
units, defaults and `required` (true for a param without a default), its access, `failure`
(`error`, or `reply` with a `failure_note` for a tool that reports a rejected rebuild in its reply)
and examples.

Two inspect tools read the table, which needs no Design panel and no UI thread. `list_cad_tools
{query?}` gives each CAD tool's name, access and summary in the table's order, which groups them by
what they do, with `total` (every CAD tool, whatever the query), `ids` (how face and edge ids
expire) and `use`; `query` keeps the tools whose name, summary or details hold it, in any case.
`describe_cad_tool {tool}` gives the whole row of a name, or of each name of a list, once and in
the order asked, and `id_lifetime` when one of them takes or gives face or edge ids; an unknown
name fails with the close ones. Both answer with the CAD feature off too, and then add a `note` on
how a person turns it on. That preference lives in `AppConfig`, which Preferences may write
meanwhile, so it is read on the UI thread (`observe_ui`, `cad_feature_note()`), which the call waits
for up to the usual queue timeout of 10 s; when the UI does not answer by then, or refuses, the reply
goes out without the note, which is only advice.

`run_cad_tool()` runs the CAD tool on the UI thread, inside `call_cad_tool`'s mutate guard, and
refuses a call by its cause, in this order. The MCP server is up before the main window, so first
come `ui_plater()`'s "shutting down" and "still starting" refusals, as for any tool. Then the G-code
viewer, which has no Design tab (`-32001`); the CAD feature off (Preferences > Features; the main
window creates the Design page only at startup, so turning it on takes a restart); and a Design
panel that cannot be built (`-32001`: no page was made at startup, the page is mid-build, or its
build failed), whose message says to retry and then restart. The panel is otherwise built on the
first call, since in a headless session nobody clicks the tab; `get_app_state.design_tab` says
whether this session built it. Then come an unknown CAD tool (`-32601`, with the close names and a
pointer to `list_cad_tools`), a busy Design tab (`-32002`), and a `generation` that is not an
unsigned integer (`-32602`) or not the model's (`-32010`). Before the generation is read, the
project's recipe is loaded into an empty document (`DesignPanel::hydrate_from_model()`), as showing
the tab would.

Access decides what `DesignPanel::mcp_busy()` lets through. `read` CAD tools run under an open
feature card or sketch session, `sketch` CAD tools drive the live sketch session, and `change`
CAD tools are refused under either, as the GUI would not let the edit land there. Neither `change`
nor `sketch` CAD tools run while a body Move is open, since its session holds a body index that a
change could point at another body. Nothing runs while a rebuild's worker owns the document.

A CAD tool that fails is an error (an `isError` result with `code` `-32000`, `OCCT: …` for a kernel
exception) and leaves the design as it was, so `export_replay` drops it and a replay never repeats a
call that did nothing. That holds for argument errors and for a rebuild the kernel rejects alike.
The exceptions are the CAD tools whose `failure` is `reply` (`fillet`, `chamfer`, `boolean`,
`pattern`, `mirror`, `shell`, the surface tools, `import_step`, `set_variable` and others): they
report a rejected rebuild in their reply, `{ok: false, error}`, after the same rollback, so such a
call is journaled and replays the same rejection. `extrude`, `revolve`, the `hole` tools, `split`
and `sketch_add` fail as errors, so none of them fails two ways depending on its arguments. The
feature tools that fail as errors add through `add_feature()`: one checkpoint, a rebuild, and on
failure `CadDocument::rollback()`, which restores the checkpoint without leaving the refused recipe
on the redo stack and with the redo branch the checkpoint cleared, then "Edit rejected" on the
status line as for a refused GUI edit. Features made through `call_cad_tool` are named as the cards
name them ("Extrude 3"), from the panel's own counter.

`generation` guards face and edge ids within a session, but its value depends on the session's
history, so the journal records `call_cad_tool` without `args.generation` and a replay rebuilds the
same ids.

The plane-taking CAD tools (`sketch_begin`, `extrude`, `revolve`, the `hole` tools, `thread`,
`split`, `pattern`, `mirror`, `helix`, `project`) share one resolver, `cad_resolve_plane()`: `"XY"`,
`"XZ"` or `"YZ"` through `modeling_origin`, `{"datum": i}` for a Plane feature, or
`{"body": b, "face": f}` for a planar face. A face plane for sketching is
`CadDocument::plane_of_face()` (outward normal); for drilling it is
`CadDocument::inward_plane_of_face()`, the Hole card's plane (normal into the body, x along the
longest edge), and the hole or thread goes into that face's body. Their replies carry the `frame`
used. Because a sketch's base plane passes through `modeling_origin` like every feature's, a sketch
lands where the features drawn on the same plane land, and the coordinates in a replay are relative
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
because an exported replay leaves the failed call out and its next sketch call needs that session.
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
the ids every other CAD tool takes). A face's kind is its OCCT surface type, from
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

The hole tools drill a face plane's own body, or the body `body` names, else the last body, and
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
and inspection tools answer from inside that loop: `get_app_state`, `list_widgets` (the dialog is a
non-modal `wxProgressDialog` titled "Design") and `screenshot`, of the window or of the dialog's own
ref. On Windows the dialog is the system's task dialog, run on a thread of its own, so it is no wx
window and `list_widgets` does not list it; the other tools answer as elsewhere. Mutating calls,
another `call_cad_tool` among them, are refused by `UiInvoker` as running inside `call_cad_tool`, and
`mcp_busy()` refuses every CAD tool while a rebuild's worker owns the document, the GUI's own
rebuilds included. A call past the reply budget becomes a running job.

`undo` and `redo` are Edit > Undo and Redo with the Design tab shown (`menu_undo_redo()`, behind
the same `menu_can_undo_redo()` gate): the Design history, or inside a sketch session the sketch's
own, which takes off or puts back the last drawn shape. `run_action undo` stays Prepare's history.
They have sketch access, so they run in a sketch session, and like every changing CAD tool they are
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
`commit` is the one CAD tool that changes Prepare's model. It needs no refusal of its own:
`call_cad_tool`'s mutate guard keeps every CAD tool out of a modal dialog and out of another call's
nested event loop.

`new_design {discard_changes}` is New Design without its confirmation: `clear_document()`, which
also clears the Design undo history and leaves committed objects on the plate. A design with
features, bodies or variables needs `discard_changes: true`. `export_step {path, overwrite}` is
Export STEP… without its file dialog (`DesignPanel::export_step_to()`, shared with it): every
body as native B-rep at its Move-gizmo position, under the server's output-path rules.
`import_step`, `import_mesh` and `validate_against`'s `reference.step` take an absolute, existing
path, like every file the server reads.

`set_view {tab: "design"}` shows the Design tab, and while it is shown `camera` points the Design
view as its own controls do: a direction through `DesignCanvas::set_view()`, `default` its Home
view (iso, fitted), `fit_objects` its `fit_view()`, and `fit_selection` the pick's box, refused when
nothing is picked. The camera is applied in a second UI closure after `settle()`, because the tab
takes the shared camera only when its page shows (`DesignPanel::on_tab_shown()`). `fit_bed`, `view`
and the Preview fields are refused with the Design tab, since they show Prepare or Preview. The
Design view is a canvas target for `list_widgets`, the input tools and screenshots (see GUI automation and
Screenshots).

A change made only in the Design tab marks the project modified, as other project data outside
Prepare's undo stack does: `sync_recipe_to_model()` sets the plater's dirty flag besides the
backup's change mark. So `get_app_state.project.dirty`, the window title and `open_files` /
`new_project {discard_changes}` agree with the unsaved-changes prompt that Open and New Project
show. A project holding only a design loads no objects, and `Plater::load_project()` still names
it after its file, since a loaded recipe counts as a loaded project.

`tests/slic3rutils/test_mcp_cad.cpp` (`[Mcp][CAD]`) checks the CAD tool table against `kCadTools`
and its readers (`cad_list_tools()`, `cad_describe_tool()`), the CAD-off note, the plane resolver,
the rollbacks, and the CAD tools through their public seams
(`cad_sketch_extrude()`, `cad_sketch_call()`, `cad_query_topology()`, `cad_thread()` and the rest),
which take a `CadDocument` or a bare `DesignSketchTool` and need no Design panel. The replay
replays run on an instance that `tools/mcp/orca_mcp.py launch --cad` starts:
`tools/mcp/tests/cad_sketch_to_solid.json`, `cad_sketch_edit.json`, `cad_topology_targets.json`,
`cad_features.json`, `cad_doc.json` and `cad_canvas.json` (the Design view as a canvas target, its
key hook, and the dirty flag). `tools/mcp/test_cad_replay.py` records a session with
failed calls and `generation`s, exports it and replays it to the same features and mass
properties; `tools/mcp/test_cad_slow_build.py` checks the window during a guarded rebuild;
`tools/mcp/test_cad_failed_job.py` exports a session whose call failed after its reply budget
(a hole that misses a slow-to-rebuild coil), which leaves it out, and replays it.

## Speed Dial catalog

`search_actions` and `run_action` serve the Speed Dial's `ActionRegistry`: built-in commands, plates,
recent projects, plugin scripts and pages, and, on request, settings. Built-in commands are
`NativeCommand`s in `NativeCommands.cpp`, with three fields for automation that the palette never
shows: `description` (English, for agents), `param` (a `CommandParam {type, help}`, typed `None`,
`Percent`, `Tab`, `Path`, `Index` or `Text`) and `completion` (`Export` when the effect finishes in
the background after `run()` returns). `describe_commands()` fills them from one table, with a
key-prefix rule for calibration dialogs (pointing at `generate_calibration_test`), camera views and primitives, and `CommandAction` copies
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
| `install_local_plugin` | `.py`, `.whl` | `install_local_plugin_package(path, parent, message)`, the Plugins dialog's install: it still asks before replacing an installed plugin, in Orca's own "Overwrite Plugin" message dialog, which `answer_dialog` answers on every platform. Cancel makes the command an error saying that the plugin was not installed. A file that is not a `.py` or `.whl` is refused before that. A package that fails inspection or installation is an Error whose message is "Plugin installation failed:" and the reason, such as a `.py` without a name in its PEP 723 `[tool.orcaslicer.plugin]` table |

The six shape commands (`add_primitive_cube`, `_cylinder`, `_sphere`, `_cone`, `_disc`, `_torus`)
take a `Text` param, the size the Primitive size dialog asks for (`"40 40 10"`, `"90 4"`), and then
call `ObjectList::load_generic_subobject(type, INVALID, true, size)`, which skips the dialog. A param
with the wrong count of positive numbers is an Error result. With an object selected they add
nothing (`ObjectList::can_add_shape_object()`), and say so as an Error, in the palette too.

The object structure commands stand for the object list's context-menu items and the plate menu's
Order By. Like those items, they act on the object list's selected rows, which `select_objects` sets:

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
(`click`), checks presets with `set_widget_value` on their checkboxes, which are named after the preset, printer
or filament they stand for, presses OK with `answer_dialog`, and answers the result message. The command
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

The command then appears in the palette, in `search_actions` and `run_action`, and in replays.

When an agent needs structured arguments or results, or has to wait for the effect, add a typed
tool to the matching `McpTools*.cpp`. Use `inspect_tool()` for pure observation, which runs inline,
read-only and unjournaled. Use `mutate_tool()` for anything that changes the app; it runs as a
journaled job under the mutate guard and may be marked `destructive`, and a tool that reaches outside
the app sets `open_world` (see Tool catalog). Build the schema with
`object_schema()`, `prop()`, `enum_prop()` and `array_prop()`. It is validated before the handler
runs, so `arg_string()` and its siblings can read the arguments. Do all wx work in `ctx.run_ui()`,
and get the plater with `ui_plater()`, which fails until the main window is up. Throw `ToolFailure`
with a message the model can act on. Call `settle()` after a change so queued events are handled
before the reply, and page lists with `page()`. For work that finishes later, call
`ctx.begin_operation()`, wait with `poll_ui()`, and give the work an identity, as slicing and
export do. Work on the UI job worker needs none of that: the call that starts it holds it as its
operation (see Worker jobs). Describe the tool in English, including what the reply holds and when it is a job.

## GUI automation

`list_widgets` (`UiInspector`) returns a compact text tree of the shown top-level windows, each
followed by the popups that opened from it, the rows of their lists, the menus, the GL toolbar items
and gizmos, and each canvas's last-frame ImGui items, capped by `max_nodes` and filtered by `query`.
The tree is a second text block of the result, and each shown web page's elements are listed under
its web view (see Web pages below). Refs (`w`, `m`, `t`, `z`, `g`, `r`, `p`, `d` plus a number) are
never reused and carry the UI generation and a liveness check, so a
ref to a destroyed or rebuilt widget is reported stale. A field is named by the setting it edits:
its option key is read from the "parameter name" line of its tooltip, and its label comes from the
option's definition. Failing that, the label to its left on the same row names it, then its
tooltip. A control's state reads `[checked]` or `[unchecked]`, or `[mixed]` for a checkbox that shows
neither (a three-state one undetermined, Orca's `CheckBox` half-checked for an unset nullable
setting). An option of a selector Orca builds of buttons (`RadioGroup`, `TabCtrl`, `MultiSwitchButton`)
is a button that reads checked while it is the selected one (`selector_option()`), so `set_widget_value true`
selects it, and a `SwitchButton` with a label at each end has the one it is on as its value.

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
  icons below);
- `{web: "Next", kind, css, within, index}` for an element of a web page, by its label, kind or a CSS
  selector (see Web pages below).

`select_objects` selects objects, instances and parts and replies with the selection as `get_app_state.selection`
gives it (`selection_state()`): in `select_objects`'s own indices, the objects, the instances they are selected
in and, when the selection holds parts rather than whole instances, those parts. An agent reads it
before an operation that acts on the selection: `run_action` runs the object operations the catalog
has (delete, split, mirror, center, drop, instances, arrange, orient, change type, rename, assemble,
set as individual objects, order by), and `set_settings` with the object or volume scope edits their
settings and filament. `add_part` adds parts, modifiers and support shapes,
`set_height_ranges` sets height range modifiers, and `set_variable_layer_height` runs the Variable layer
height panel's buttons. `set_custom_gcode_at_layer` does what the Preview layer slider's right-click menu does.
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
canvas's idle handler does) before `list_widgets` lists them and before a click checks them, so an
item a `select_objects` just enabled is not read as disabled. A click on an ImGui item whose centre lies
under another ImGui window (a notification over a panel's button) is refused, naming that window.
`ImGuiWrapper::set_item_recording()` supplies the ImGui items (see below). `SyntheticInput` holds
a gesture's keys, buttons and pointer for code that reads hardware state through `get_key_state()`
and `get_mouse_state()`: the canvas, gizmos, and the object list's Ctrl/Shift multi-select. Mouse
modifiers take the names `press_keys` chords use, through `KeyChord::parse_modifier()`, so `ctrl` means
the shortcut modifier (Command on macOS) in both, and `rawctrl` the macOS Control key. Held
keys are released however the gesture ends; a `mouse_gesture` `down` keeps its button held, and a
`move` drags with it, until an `up`, another gesture (one that fails included; a request the server
rejects for its arguments never runs and changes nothing), the end of the replay or script that pressed it,
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
plater's three. `live_canvases()` lists the plater's, which `get_app_state.gizmo` and the idle checks
read, since the Design view has no gizmos; `ui_canvases()` adds the Design view for what `list_widgets`, the input
tools and screenshots reach: `list_widgets` lists its ImGui items, `click` and `press_keys` act on it,
and `mouse_gesture` takes `canvas: "design"` (`"current"` is the view shown, `shown_canvas()`). A click
there goes to the Design view's own handlers, so a left click picks a face, edge or body and a right
click opens the offer menu, which is listed or run like any other. Points of the model name
Prepare's objects, so on the Design view `mouse_gesture` takes pixels only; `call_cad_tool query_topology`
gives its faces.

### Windows

`list_windows` (`UiInspector::windows()`) lists the open windows without their widgets, in `list_widgets`'s
order: the main window, the windows that don't block in the order they were created, then the modal
stack, the topmost last. wx keeps top-level windows in creation order, not stacking order, so only the
modal stack orders by what is on top. Each window has the ref `list_widgets` gives its node, with the same
locator, so `screenshot` captures that window by it, `list_widgets` takes it as `root`, the input tools as
`target`, and the journal records the locator; its title is what `within` takes. A window says whether it
is modal and whether it shows a web view (outside a hidden pane), with its size and, where the platform
knows it (not Wayland, `window_positions_known()`), its place on screen. A native dialog of the modal
stack, which only `answer_dialog` reaches, is listed with `native` and no ref, whether or not wx reports
it shown. A window named `kHiddenFromAutomation` is listed by its title and kind with `hidden` and
nothing more: without a ref no tool acts on it, and an agent learns why a window it was told about is out
of its reach. The Python scripts window is the one modal window the tracker leaves off its stack, so it is
listed among the others, `modal` from `wxDialog::IsModal()`.

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
set each item's enabled and checked state as a person would see them. `click`, `mouse_gesture` and
`press_keys` arm a capture in the closure that delivers their input (`arm_menu()`). The first menu that
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
a menu that opens in a window where nothing is armed (a `call_cad_tool run_verb` whose offer opens later)
is closed unseen too. The last three such menus, with the tool whose window took them, are in
`get_app_state.closed_menus`.

A menu path is the item labels joined with `>`, as `GetItemLabelText()` gives them, matched without
case; a trailing `...` may be left out. A label repeated among its siblings is numbered from its
second time on (`Change Filament>Generic PLA #2`), so every listed path names one item, and the
marker the Change Filament list appends to the slot in use (" (current)", translated) is left out of
the numbering and the matching, so a path recorded with another slot in use still names the same
item. A path that names a submenu, a disabled item or nothing fails with the items to pick from, and
so does a `popup_item` when the input opened no menu (a right click on empty space while objects are
selected, in the Preview, or with a gizmo open). Paths are the UI language's text, as other locators
are. With `x`/`y`, `click` clicks that point of a custom-drawn widget (such as the Design tab's
feature tree) as well as of a 3D canvas; a native control, which acts on what the system tells it
rather than on wx mouse events, is refused.

MenuFactory builds the multi-selection, layer and assemble-view menus afresh for each popup.
`Plater::PopupMenu()` takes them back (`MenuFactory::take_transient()`) and frees them once the popup
has closed, for a person's right click as for MCP's. Their items' `wxEVT_UPDATE_UI` handlers stay bound
on the main frame, but under ids `wxNewId()` never hands out again, so they never run.

### Rows and popups

The rows of `wxDataViewCtrl`s (the object list, Compare presets, the upload queue and other data
views), of `wxListBox` and `wxCheckListBox` (the Add Settings checklist), of `wxListCtrl`s (the
printers a Bonjour or Creality scan finds), of Orca's `DropDown` lists, of `wxGrid` tables (the object
settings table) and of the Design tab's `DesignRowList`s are listed under their list as `row` nodes
(`list_rows()`): the first text cell is the label, the other text cells the value joined with ` | `, a
checkbox cell its checked state. A data view is walked through its model from the root, collapsed
branches included; a modal dialog's message reads its lists through a bounded `list_rows()`, which reads
only shown rows, up to its limit. A click on a list control's row selects it with `SetItemState()`, for
which the control sends `wxEVT_LIST_ITEM_SELECTED`, natively on Windows and from the generic control's
highlighting elsewhere; a double click sends `wxEVT_LIST_ITEM_ACTIVATED`. An object list row also says what it stands
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
is captured like any other. `set_widget_value` with true or false sets a checkbox cell through
`ChangeValue()`, as the toggle renderer does, which tells the owner. A list box row is selected with
`wxEVT_LISTBOX` and a checklist row ticked with `wxEVT_CHECKLISTBOX`; a drop-down row is picked with
`DropDown::ClickItem()`, which does what a release over it does; a table row's cell is made current
with `GoToCell()` (`wxEVT_GRID_SELECT_CELL`), and its cells are not edited: `set_settings` with the
object scope writes the same values. A Design list row is clicked at its centre, or at the action
icon whose tooltip `column` gives, through the list's own mouse handlers.

Popups (`wxPopupWindow`: the print button's menu, drop-down lists) report themselves top-level but
are children of the window they opened from. `list_widgets` lists each shown one as a `popup` right
after its window (`popups_of()`), and `root: "popup"` lists only them. A widget in a popup belongs
to that window (`owning_window()`), so it is inside the topmost modal for the input guards when its
popup opened from there, and its locator's `within` names that window. A screenshot of a widget in
a popup is refused: a screenshot of its window shows the popup.

### Plate icons

A plate and its icons in the Prepare view (delete, orient, arrange, lock, settings, name, move to
front, filament grouping, IDEX mode) are GL picking models, which a person reaches only by pointing at
them. `list_widgets` lists under the Prepare canvas a `plate_icon` node for each plate, named as the
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

`list_widgets` lists each ImGui window (its scroll range when it scrolls), then its items and text in
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

`set_widget_value` on an ImGui item acts by its kind and reads the value back, failing when the field shows
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
gizmo, which `click` waits for. A `gizmo` locator takes the name `get_app_state.gizmo` gives, or
the English title of its toolbar button where that differs (Support Painting for FdmSupports), in
any case; the reply uses the first, and an unknown name's error lists them. `get_app_state.gizmo`
names the open gizmo and its canvas. The
assemble view is entered and left with `set_view view` (leaving does what its Return button does;
entering returns once the canvas has loaded its objects, which waits for its GL surface on Wayland),
and `select_objects` and `list_objects` act on it while it is shown. Grabbers are not dragged: a gizmo's
panel fields or `transform` set those values. Pickers inside a gizmo go through `pick_gizmo_file`, whose
actions are the bodies of those pickers (`GLGizmoSVG::change_file()`, `save_svg_as()`,
`GLGizmoTextureDisplacement::import_texture()`).

`mouse_gesture` takes points of the model (`CanvasTarget`): `{object, instance, volume}` with `point` in
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
the camera, and the stroke paints nothing. `mouse_gesture` refuses a down or drag that starts there,
testing the start pixel's ray against the selected instance's parts, so a misaimed stroke fails
instead of silently turning the view under the screenshot the agent aimed from. The refusal names
the first pixel along the stroke that lands on the object, so a stroke swept in from outside it is
retried in one call rather than broken into clicks. Any gesture that
moves the camera anyway (a drag off the model outside the painters, a wheel) says so with
`camera_moved` in its reply, compared from the view matrix and zoom before and after.

Every point of a
gesture is resolved before its first event, and the journal keeps the model points, so a replay
replays at another window size or camera. A gesture's modifiers are pressed as a keyboard does: a
key-down event each before it and a key-up after, which is what switches Measure and Assembly to
point picking and the painters to their Shift and Alt modes.

### Dialogs and web views

`ModalTracker`, a `wxModalDialogHook`, sees every `ShowModal()`, native dialogs included, and
publishes the snapshot `[{title, message, class, native, buttons}]`, which other threads read. Read on
the UI thread, as `get_app_state`, the `blocked_by_dialog` probe, the guards' refusals and the input tools'
`dialogs` read it, the snapshot describes the open dialogs again first. So it shows a dialog as it is
now: text the dialog filled in once it showed (a printer's name, a list from a scan), a field typed in,
a page switched. Should describing fail, the last description stands. Only a native dialog that holds
the UI thread (macOS) leaves readers with the description published before it; its content does not
change. A dialog that can't be described is listed by its title, class, `native` and `web` flags, with no
message or buttons, so the guards still see it and a rule matches it by its title. Message boxes and file and folder pickers are native; a box's message is its text, and a
picker's is its prompt, such as "Choose Download Directory", since its title is often empty. Colour and
font choosers are not classified as native, because on GTK they are wx dialogs that `answer_dialog`
ends; on macOS and Windows they are OS choosers, best answered ahead with an `auto_answer_dialogs` rule.

A wx dialog's message reads it as a person does, so an agent can tell what the dialog asks and what
each button will do without a screenshot (`DialogReader`). Each shown child becomes a cell. Nested
dialogs, popups, web views and canvases are left out; a nested dialog is described on its own.
Controls are read through the readers `list_widgets` uses (`kind_of`, `value_of`, `checked_of`,
`selector_choices`, `list_rows`), so the two say the same about each one:

- Static text, html and custom widgets that show a label (`StaticLine` titles) read as their text, and
  a static box as its title, then its contents. A label that is the window's own name, when that is a
  name wx gives every window of a class, is no label: Windows shows the name a window was created with
  as its text, so a bare panel reads "panel".
- An editable field, spin or slider reads `[value]`. A read-only field reads as its text.
- A choice reads `[value ▾]`. Orca's `ComboBox` shows its placeholder, such as "Select Vendor", until
  something is picked.
- A checkbox or switch reads `[x] label`, `[ ] label`, or `[-] label` when it shows neither state (an
  unset nullable setting). A radio button reads `(*) label` or `( ) label`.
- A selector Orca builds of buttons (`RadioGroup`, `TabCtrl`, `MultiSwitchButton`), a `SwitchButton`
  with a label at each end, and a notebook's tabs read as one line, `(*) A | ( ) B`; an icon option
  reads as its tooltip. A notebook's tabs are followed by the page shown.
- A list reads one line per shown row, up to 25, then a line saying `list_widgets` lists the rest.
- A gauge reads `[45%]`.
- A disabled control adds ` (disabled)`. Disabled is judged between the control and its dialog, so a
  dialog under another modal does not read as all disabled.

Of the cells a toggle could take as its label, a control with its own mark (a labelled checkbox, a
selector) is never one.

A secret never shows: a field with password style, a setting field for a password, API key or token
(by the `parameter name` of its tooltip, as `printhost_password` and `printhost_apikey`), or one its
dialog names `kSecretField` (a printer's access code and pin, which those dialogs show in plain text)
reads `[******]`. A URL in a field loses its credentials (`redact_url()`), each word of the value on
its own, as does a print host's value without a scheme, `user:password@host`, which the hosts prefix
with `http://`.

A parent's cells are put in reading order by their rectangles: rows top to bottom, then left to right.
Two cells are on one row when either one's vertical centre lies within the other's span. A cell over
twice as tall as the other, such as a wrapped label or a column of text, shares its row only with a
cell beside its first line (its centre within the shorter cell's height of the taller one's top) or
centred on it. So a checkbox or a field at a wrapped label's first line shares its row, while a column
of text never pulls the label and field rows below that line into one. So the order the dialog made its widgets in does not
matter: a field made before the label above it reads after it, and PrintHostSend's radio boxes, made
before their labels, each meet their own label. In a row:

- A toggle with no label of its own (Orca's `CheckBox` and `RadioBox`) takes the cell to its right as
  its label: `[x] Remember my choice.`. With nothing there, it takes the topmost single-line cell to
  its left, as Preferences and CloneDialog lay their switches out: `Show splash screen | [x]`. Of a
  title and its hint stacked beside a switch (RecordingDialog), that is the title.
- Single-line cells side by side join with ` | `. A container whose children make one line counts as
  one cell, so `UnsavedChangesDialog`'s rows, each cell in a panel of its own, read
  `Sparse infill density | 15% | 20%`.
- A toggle nothing completes is named by what is its own: the setting it edits, its tooltip, else its
  window name in parentheses. The text beside it is not used, since the layout gave that to what it
  labels.

Widgets that overlap, as in a dialog not laid out yet, join nothing and keep their order.

The message is at most 2000 bytes; a longer one is cut there, where a character starts, and ends in
`...`. Once the
cells read pass that, only buttons are looked for: lists are read through a bounded `list_rows()`, which
reads only shown rows and stops at the cap. So describing a dialog costs its window count, plus each
bounded list's own enumeration of the level it reads (a flat data view's model lists all its children,
a pointer each), not the size of the text, values or rows it holds. The dialog's own text, which only
rule matching reads, is walked only for that. Text no wx control holds is not read: what custom widgets paint themselves (`SwitchBoard`, the
AMS mapping items, `RecenterDialog`'s explanation, Preferences' row labels) and icon buttons with no
label. Web dialogs are read through their page (below).

`buttons` lists the wx buttons a dialog shows, with `enabled: false` for a disabled one. The options of
a selector made of buttons (`RadioGroup`, `TabCtrl`, `MultiSwitchButton`) are not listed, since pressing
one only selects it. `answer_dialog`, and a rule, still find such an option by its label (its tooltip
for an icon option, in full even where a message cut at its cap shows only its start) and select it, and
`list_widgets` shows the selected option checked, under the same full label. `set_widget_value false` on the
selected one, or on a selected radio button or radio box, is refused, since an option is unselected by
selecting another. Other radio buttons and switches are set
with `click` or `set_widget_value true`, and notebook tabs with `click` on the tab.

`answer_dialog` presses the topmost wx dialog's button, so the dialog's own handler runs.
`press_dialog_button()` tells what it did:

- **Pressed:** the button was pressed.
- **Ended:** the dialog has no button with that id, so it ended with it, and `cancel` closes a dialog
  without a Cancel button. One whose Cancel or Close is on a page it does not show ends the same way.
- **Disabled:** the button is shown but disabled, so nothing is pressed or stood in for. The dialog
  disabled it on purpose: OK while a name is invalid, Send until a box is ticked, Cancel while a
  cancel is under way. `answer_dialog` refuses a disabled button, its message showing what the dialog
  still needs.
- **Hidden:** the button is only on a page the dialog does not show, such as a wizard's Finish, so
  nothing is pressed or stood in for, and `answer_dialog` refuses it. Buttons are looked up among the
  dialog's own windows, not a popup's or a nested dialog's.
- **Failed:** a native message box could not be pressed.

On Windows and Linux `answer_dialog` also presses an open native message box. The tracker lists the
buttons wx gives the box for its style (Yes and No or OK, and Cancel; not Help, which on Windows shows
help without closing the box), and a press goes to the OS's box by message. On Windows that is a
`WM_COMMAND` to a message box, or a `TDM_CLICK_BUTTON` to the task dialog wx shows where Windows has
one, found among the UI thread's windows by class and caption, the active one first. On GTK it is
`gtk_dialog_response()` on the dialog's widget. The box closes once its own loop runs again, which
`answer_dialog` waits up to 1 s for, so its reply says `closed`. A box that is no longer shown is
refused. This is how a plugin's permission prompt is answered. On macOS a message box lists no buttons,
since no call reaches one (above).

A picker, and on macOS any native dialog, can only be answered ahead with an `auto_answer_dialogs` rule:
a regex and a button. The regex is searched in three places:

- the title and message joined by a newline
- the title and the dialog's own text joined the same way. The own text is its static text and html,
  one per line in child order, cut and trimmed as the message is. It leaves the controls out, and
  `describe_dialog()` reads it in a walk of its own that the message's budget does not stop.
- the title alone, for a dialog with no own text (so `^Title$` matches a web dialog)

A rule written against a dialog's wording therefore matches however its controls read, and long values
or lists before that wording do not push it out. `Enter()` then returns that button's id without
showing the dialog, and logs it; a file or folder picker can only be cancelled, since any other answer
would return without a path.

A wx dialog that matches a rule opens, and the tracker presses the button once the dialog's modal loop
runs (`press_dialog_button()`, queued on the dialog), as `answer_dialog` does. Its buttons do the
dialog's work in their handlers (`UnsavedChangesDialog`'s Transfer and Discard, `CloneDialog`'s Fill),
which a returned id would skip. The rule counts as used once the press pressed the button or ended the
dialog. A button disabled by then leaves the dialog open, logged, and a modal opened above it before
then leaves it to `answer_dialog`.

A new rule also answers a matching dialog that is already open, as `answer_dialog` would, a message
box included where it can be pressed. The first rule that names one of its buttons decides: if that
button is disabled or hidden, the dialog stays open and the reply says `disabled: true` or
`hidden: true` (a hidden Cancel or Close ends the dialog, as above). A prompt posted around
the same time, such as the restore prompt after startup, is handled whether it opens before or after
the rule. A replay's or script's rules end with it, a client's call replaces its earlier rules, and
`get_app_state` lists the rules in force.

A dialog that hosts a web view is described with `web: true`; its controls are on its page, which
`list_widgets root=dialog` lists and the input tools drive like any widget (see Web pages below). It
has no text of its own, so a rule matches it by title. Its page is the one its shown web view holds; a web
view the dialog keeps hidden has no page to press on. `answer_dialog` looks a name or label up as a
wx button label first, then among the page's buttons and links, waiting up to 3 s for one to show
(each ask expires with those 3 s, so the page never presses one later), and clicks it there, hit
tested as a click is, so the page's own handler runs (`{pressed: true, web: true}`). With no such
button, `cancel` and `close` end the dialog as for any other, also when the page could not be asked
(busy for the 3 s, or its agent failing), while `ok`, `yes`, `no`, `apply` and ids need a wx button
with that id and are refused otherwise, listing the page's buttons, so a page's commit is never
skipped. A rule for a web dialog whose button names none of its shown wx buttons, by label,
standard name or id (`native_dialog_button()`), starts a web answer
(`start_web_answer()`): a UI-thread timer that every 250 ms, while the dialog is the topmost and the
rule in force, asks the page to press the button and stops at the first press. Its 10 s limit counts
only time a complete document is shown and restarts when the document changes, so a dialog's
bootstrap page replaced by its content is waited out; past it, or after 2 s with no web view shown,
`cancel` and `close` end the dialog natively and any other answer gives up, logged.

`run_javascript` runs JavaScript in a page through its agent, as a recover call: in a web view of the
topmost modal dialog whatever the dialog, in one under it never, and elsewhere with a mutate call's
checks. Without a target the topmost dialog's web view is used, else the only shown one; a web view's
locator names it by kind (`class: "webview"`), since its class differs per platform, and a page
element's ref or locator runs the code in its page with the element bound to `el`. The job thread
waits up to `timeout_s` (default 10) without the UI; a JSON result comes back parsed, and the call
reports `closed`, `navigated` and `dialogs` as the input tools do. The agent runs the code through
`eval`, which a page whose Content-Security-Policy forbids it may refuse ("…forbids eval"), where the
port holds injected scripts to the page's policy (WebView2 does not); the agent itself is installed
as inline code, so the input tools work on such a page either way.

### Web pages

Every web view of OrcaSlicer — its web dialogs (setup wizard, preset bundles, colour picker, flushing
volumes, Plugins), sign-in pages, the Home tab, and plugins' dialogs, windows, pages and dock panels —
is read and driven through the page agent, `resources/web/mcp/page_agent.js` (`WebPages.hpp`). No host
loads it. A call sends a light script asking `window.__orcaMcp` for the request; a page without the
agent, or with another version of it (a hash of its source), answers `{code: "install"}`, and the
request goes again with the source in front: inline as code, so a page whose CSP forbids `eval` takes
it, and as text for the frames it relays to. So every web view on every port is reached without a
change to its host, and a plugin's pages, which share one dialog, stylesheet and bridge script, need
nothing of their own. The agent is installed as a non-enumerable property, out of the page's own
enumeration of `window`.

Scripts go through `WebView::RunScriptAsync()`, whose completion holds only the callback: WebView2's
`ExecuteScript` handler, WKWebView's `evaluateJavaScript` block, WebKitGTK's `run_javascript`
callback. wx's own `RunScriptAsync()` keeps a plain pointer to the web view in its callback on every
port, and a page may close its dialog, destroying the web view, at any time. `PageCalls` start calls
on the UI thread and wait for them on the job thread, a destruction watcher ending the wait for a
web view that goes. Job and request threads hold a web view as a `WebHandle`, a number only the UI
thread maps back to it, never as a `wxWeakRef`: making or dropping one edits the window's list of
refs, which the UI thread edits at the same time.

What a page lists, after Playwright's accessibility snapshot: each element with a role as a kind
(button, link, text, checkbox, radio, choice with its options, slider, spin, tab, heading, row,
canvas, frame, file), its accessible name (ARIA, its label element, `alt`, `title`, its text), value,
id as name, written as the CSS selector that finds it (`#save`) so it is not taken for the `name`
attribute, state (checked, expanded, disabled, focused, required) and place; text outside them,
merged per block, as label lines that act through their block, so a click on text with a handler the
walk can't see is possible. Pages built from `div`s are read by what makes an element clickable:
an `onclick` attribute or jQuery handler, a pointer cursor, `tabindex`, a `btn`/`button` class name;
a `selected`/`checked` class word of its own or a camel-case one's end (`is-selected`,
`RegionSelected`, never `unselected` or `NotSelected`) gives a state. A field with no name of its own in a table cell, such as an entry of
the flushing volumes matrix, is named by its row's and its column's headers, `row / column`. Hidden
subtrees are left out, and so are off-canvas ones (entirely left of the page, as a closed sidebar
is) unless a scroller scrolled to the right holds them, which scrolls them back when acted on. A visually
hidden checkbox is listed as its label, and open shadow roots and same-origin frames are walked. A
`d` ref names its element by a key stable within the document and by the document's token, which a
navigation or reload changes, so a ref from before is stale ("the page changed"). Page lines have
their own budget, `max_web_nodes` (default 300), so a large page never pushes widgets listed after it
out of `max_nodes`. With a modal dialog open only the topmost dialog's pages are read, since no input
reaches the others, which get the note `behind a modal dialog`. `list_widgets` waits up to 2 s for a
page still loading, then reads every page at once, for up to 3 s together.

A locator `{web, kind, css, within, index}` matches the label exactly, else case aside, else as a
part of it (a card listed by all of its text); a text line only when no element matches. `css` is
matched through shadow roots and frames. Without `within`, a locator looks in the topmost dialog's
pages while a modal shows one, else in every shown page; `index` picks among the matches of all of
them. It waits up to 3 s for a match, since pages fill in after they load and a dialog's page loads
after the click that opened it has returned. A match while another candidate page has not answered
is refused, since that page may hold more matches and neither uniqueness nor `index` is then known;
`within` or a ref narrows the search. The journal records the page's own locator for the
element acted on (its id, else its label and kind, else a CSS path), with `within` and an `index`
among the matches of every candidate page and its configuration frames, as a replay searches them
(`journal_index()`); with no `index` when one of them was not heard from (silent, failing the search
or unable to run the agent) or the action took the element away, so a replay that finds several asks
for one rather than taking another. A screenshot of a page element is journalled as one of its web view, which it
captures. A web view's own ref is no page element: `click` and `set_widget_value` refuse it, pointing to
the page's elements, and `press_keys` sends to its page's focused element.

An action runs synchronously in one task of the page. The element is scrolled into view and hit
tested: one covered at its centre is refused naming what covers it, a disabled one and a file input
are refused. A click sends what a person's does — pointer and mouse over, down (focusing the element),
up, then `click` — so the browser's default actions follow: a label reaches its control, a link
navigates, a submit button submits its form with itself as the submitter. `set_widget_value` sets a value
through the element's own setter and sends `input` and `change`, which framework-controlled fields
follow; a choice is picked by its option's text, a checkbox through a click, editable text through
`insertText`. Keys go as `keydown`, `keypress`, the default action (text, Enter submitting the form
implicitly, Tab moving focus in the page's composed focus order through open shadow roots, their
slots and same-origin frames, past controls the browser has disabled (`:disabled`, so a disabled
fieldset's first legend keeps its controls), arrows in a radio group, Left/Right by a character or,
on a selection, to its start or end) and `keyup`; Ctrl is Command on macOS, as for widgets. A disabled
target is refused, and a read-only field takes no text. In editable content where the editing
command is unavailable or refuses (WebKit), the text goes in, or a character goes, at the selection
as the command would. `mouse_gesture` gestures send pointer events from a synthetic pointer whose capture
the agent emulates while it is pressed, as a canvas that captures the pointer (the colour picker's
spectrum) needs; a `down`'s button and modifiers stay pressed through later moves until its `up`, or
until any other gesture, click, `answer_dialog` or auto-answer press, a widget's included and even
one the tool then refuses, releases them where they were pressed. A move or an `up` keeps a press only on the agent
that holds it: a page's own, or one of its configuration frames', which are told apart, so an `up`
on a frame's page ends a press in the frame. Each agent has one synthetic pointer, which every
client's gestures share, as they share the widgets' one. The tools keep each press on record
(`WebPresses`), by web view, document and agent, from before its `down` is sent, as a down whose
reply never comes may still have pressed, until that agent confirms the release (a frame's through
its page's relay). One too busy to take it is asked again by the next gesture; the late request is
past its deadline and not run. Each press has its own id, which the page keeps with the press, and a
release names the press it ends, so it never ends a later press of the same agent or takes one off
record; a press whose down is not sent yet is neither
released nor ended by an up, either of which could overtake it; once sent, the page runs it before
anything sent after it. An up that fails or times out ends nothing: past its deadline the page does
not run it. The releases are asked all at once and get one second together, so a page that doesn't
answer holds up none of the others, and the call still names the dialog it works in before the
soft budget, past which an open dialog would read as what it waits on. Like the widgets' held
press, a page's belongs to the client, replay or script that made it: a replay's or script's ends with it,
a client's when the endpoint closes, every one when the service stops, and a client's down sent
after the endpoint closed is refused. These releases, and an auto-answer rule's before it presses a
button, are made on the UI thread without waiting (`release_web_presses()`) and with no deadline,
so a page busy then releases once free: a page's own confirmed release drops its record, and a
frame's, which its page forwards, is confirmed by the next gesture.
Keys and input leave a press held, as a person can type while holding the mouse. Only a press is
hit tested, so a drag split across calls may move
and end outside the element. Typed text goes in by code point, a character outside the BMP as one
key, and a delete takes a surrogate pair whole, also with the caret between its halves; arrows leave
a read-only number field as it is. Coordinates map between logical and CSS pixels by the device pixel ratio and the content
scale factor.

A page closes its dialog by a message to the host (`window.wx`, `chrome.webview`, a WebKit message
handler), which ends the modal loop and destroys the web view. So while an action, a replay or a script runs,
the page's messages are held, even through a reference to the channel kept from before, and posted in
order once it has returned. The tool then waits up to 1 s for the page to have posted them, lets wx
run their events, and looks for 300 ms at what followed: the web view destroyed or its dialog off the
modal stack (`closed: true`, with `dialogs` listing any still open under it, as the filament picker
is under the colour picker it opened), another modal (`dialogs`), or a change of page (`navigated:
true` and `url`). A change of page is told by the web view's URL, by the page's own URL before and
after the action (a fragment change, which WebView2 reports no URL for on a page set as HTML), and,
once the 300 ms have passed, by asking the page for its document's token, which a reload or a form
posted to the same URL changes; not by `IsBusy()`, which WebView2 also reports while a page only
works. Failing those, the reply's `page` says what the action changed on its page: the page outlines
itself before the action, a line per element (with its value, state and the notes `list_widgets`
gives it, such as a select's options) and per run of text, under an id the tool gives the action,
and is asked afterwards which lines it lists now that it did not (`added`) and which it no longer
lists (`removed`), counted as a multiset so an element rebuilt as it was, or text that moved, is no
change. Each configuration frame outlines and compares its own page through the relay: the page sends
it the outline request before the action runs, and the frame, whose page changes only by a message
or by loading another document, takes it before any message the action sends it. A frame that does
not answer, fails, or has loaded another document since is not compared, and `partial: true` says so;
then `changed` is given only when a line was found. Otherwise `changed: false` says the page lists
what it did before, which tells a click whose page ignored it (a validation it failed) from one that
worked, and `posted` how many messages the page itself sent the host meanwhile, to its plugin's
handler say. An outline is dropped once compared, and a page keeps at most 16, none past a minute. A
change that comes later is the next snapshot's to show.

A URL a reply shows (`navigated`'s, `run_javascript`'s, a web view's value in `list_widgets`) reads
`REDACTED` for the user info of its authority and for the value of each query or fragment parameter
one of whose name's words names a credential (`token`, `key`, `secret`, `password`, `auth`, `session`,
`signature`, `code` and the like; `redact_url()`), after OpenTelemetry's rule for URLs in telemetry.
A plugin's page may keep a secret in its URL, as a bridge token in the fragment, and a reply lands in
the agent's transcript. Words, not substrings, so `keyword` or `design` stay as they are. Navigation
is still told by the URLs as they are.

A request carries a deadline 100 ms before the tool stops waiting (`timeout_s`, 1 to 120, default
10), in the page's clock. A page busy past it refuses the request when it gets to it, and nothing was
done; a request that got no answer has an unknown outcome, said as such. A web call working in the
topmost dialog tells the job layer which dialog that is (`ToolContext::works_in_dialog()`, with the
dialog's serial: `ModalTracker` numbers each dialog as it opens and never reuses a number), so a page
slower than the soft budget is reported running, not `blocked_by_dialog`, while that dialog is the
topmost (`GuardState::top_dialog`), and blocked once any other is, above it or in its place.

A plugin's configuration UI is in a sandboxed `srcdoc` frame of the Plugins dialog, whose content the
page can't read. The frame's bridge (`resources/web/dialog/js/plugin-config-ui.js`) relays: the
agent posts the request with its source to the frame, which runs a copy of the agent and answers by
message. A call is repeated, in the same round, until every relayed frame has answered or a second
has passed; the last repeat says so, and a frame still silent then is listed as not answering. The
frame's elements are keyed `<frame>/<document>/<element>`.

The agent targets Safari 15 (the oldest macOS the build allows) and WebKitGTK 2.36: ES2017 syntax,
and feature detection with fallbacks for `requestSubmit` (the form's constraint validation runs
first, as an implicit submission's does), `PointerEvent`, `inert`, `insertText`, `CSS.escape` and
`InputEvent`.

Limits: a web view in a popup window is not read, which in Orca is the setting tooltip's help
(`MarkdownTip`, a transient popup); a dialog whose web view is hidden is answered as a wx dialog; a
page element's screenshot captures its whole web view, not a crop of the element; points in a frame
that a CSS transform or zoom scales are where the untransformed frame would put them; a page button
whose handler outlasts `answer_dialog`'s 3 s may still be running when `cancel` or `close` ends the
dialog natively; cross-origin frames other than plugin configuration UIs are listed but not read.

## Screenshots

`screenshot` captures on the UI thread and encodes off it. It has three targets.

- `canvas`: the visible 3D view, the Design view while the Design tab is shown, exactly as drawn,
  toolbars and ImGui panels included (the Design view's sketch overlay and status line too), on
  every platform. `GLCanvas3D::render_and_capture()` forces a frame and reads the back buffer just
  before the swap; it fails, saying why, when the canvas is hidden or mid-frame. That frame is often
  the first since a tool's change, yet it shows the change settled, since slider moves reach the
  toolpaths before they are drawn and the capture first draws any extra frames the change asked for:
  a capture right after `set_view` shows the layers its slider shows and the legend laid out.
- `window`: the main window's client area from `capture_native_window()`: its own pixels or, where
  the capture reads the screen (X11, macOS 14.4 and later), our windows over it as well, never another
  application's, with every visible 3D canvas composited in from its canvas capture, since window
  capture loses OpenGL content on several platforms.
- `dialog`: the topmost modal, captured like a window.
- a widget's ref or locator: its top-level window captured like a window (a window that is not
  modal, such as an object's settings, has no other target: `list_windows` gives its ref) and cropped to the widget, or the
  canvas capture for a 3D canvas. A tab is cropped to its page, which must be the one shown. The
  journal records the locator.

When the capture may differ from the screen, the result carries `approximate: true` and a `note`.

| Platform | Window capture | Approximate when |
|---|---|---|
| Windows | Windows Graphics Capture of the window, cropped to its client area; `PrintWindow(PW_CLIENTONLY \| PW_RENDERFULLCONTENT)` where that is unavailable | `PrintWindow` fails and the screen is copied instead |
| macOS | ScreenCaptureKit of our windows on the window's display, cropped to its client area (14.4 and later); `cacheDisplayInRect:toBitmapImageRep:` on the content view where that is unavailable or the window is not wholly on its display | always with `cacheDisplayInRect`: web views may be blank |
| Linux X11, Xvfb, XWayland | `XGetImage` of the top-level X window | part of it is off-screen or under another application's window |
| Linux Wayland | `gtk_widget_draw()` of the top-level window's child, which leaves out the client-side decoration's shadow, after `gtk_container_check_resize()` runs any relayout still pending, without which it draws nothing | always: web views may be blank |

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

On macOS 14.4 and later the capture comes from the window server through ScreenCaptureKit, which lets
a process capture its own windows without the Screen Recording permission
(`getCurrentProcessShareableContentWithCompletionHandler:`). A filter of the window's display with
OrcaSlicer's windows only gives what the screen shows of them in the client area, at the window's
backing scale, in sRGB: the window, web views and 3D views included, and over it our dialogs, wx
popups, AppKit's menus and tooltips, but no other application's window, so a covered window captures
whole. The filter names the application rather than its windows, so a menu or tooltip that opens later
is in it too. It leaves out the shown windows hidden from automation (Action Replay, a floating Run Script
panel), their wx
popups, and AppKit's windows over them: AppKit does not say which window opened a menu or tooltip, so
each of our windows that is not wx's goes with the topmost wx window below it that it overlaps, as on
Windows. The window behind such a window shows through. `SCScreenshotManager` takes one image per
capture; the UI thread waits up to 2 s for its completion handler, which runs on a ScreenCaptureKit
queue, even while a menu is tracking. The wait is the window server composing the image: about 25–35 ms
for OrcaSlicer's main window on a Retina display (6–7 ms for a plain window), so a recording reaches
about 12 frames a second however high its `fps`. The window server shows
what was last committed, so the capture first runs `displayIfNeeded` on our visible windows and flushes
Core Animation; without that, a change made earlier in the same turn of the event loop is missing.
Listing the content takes 10–20 ms, so a recording keeps it and a filter per display in
its sessions, and fetches them again when the left-out windows change or the window moves to a display
they do not list. The capture records the rectangles of our windows above the target
(`CGWindowListCopyWindowInfo`) in `Capture::covered`, where a screenshot's 3D views are not composited.
The pixels the window's rounded corners leave uncovered read black. The window server shows only the
part of a window on its display, and ScreenCaptureKit clamps what it captures to it, so a window not
wholly on its display is drawn from its views (`cacheDisplayInRect`), which leaves AppKit's windows out
and may leave web views blank, as it is before macOS 14.4 or with an SDK older than 14.4, and while the
window or the app is hidden or the window is on another Space. A failure or a timeout ends
ScreenCaptureKit captures for a recording, which draws views from then on rather than wait at every
frame; the third timeout in the run ends them for the run, as a macOS whose answers needed the waiting
UI thread would never answer. ScreenCaptureKit is weakly linked, since the deployment target (12.0)
predates it. A stream of the whole display (`SCStream`) would take the composing off the UI thread, but
holds several display-sized buffers for the recording's life, and each frame it hands over may predate a
change of the left-out windows or a move of the window it is cropped to.

On every platform, a canvas that can't be captured keeps whatever the window capture drew, and the
result is approximate. A canvas frame that reads back black (`is_blank()`, a stray pixel aside) has
not been drawn yet. This happens on Wayland for a moment after the canvas is first shown, while
GNOME's overview hides an XWayland window, and while the display is off. Such a capture is taken
again every 300 ms for up to 3 s, then returned as approximate, saying so, and saying the window isn't
being drawn when `undrawn_window()` finds it isn't. The composite takes the canvases of `ui_canvases()`,
the Design view included; an OpenGL view outside them has no frame capture, and a window capture
says it is left out. Minimized and hidden windows fail. X11 reads only the on-screen part of a window (`XGetImage` fails
for the rest), which leaves the remainder black. X11 keeps no backing store, so a part
covered by another window reads back black. OrcaSlicer's own popups and tooltips stacked above the
window are read and drawn in, as they appear on screen; another application's window leaves the
hole, and the result is approximate. Wayland draws them in too, over the 3D views: `capture_popups()` draws
each shown GTK popup window, a wx popup, a menu, a drop-down list or a tooltip, at its place on the
window it shows over. That window is the end of its chain of transient parents or, for a menu, which wx
shows attached to no widget, the one `popup_menu()` showed it from (`shown_menus()`). A wx popup has no
shadow and is drawn from its widget tree. A popup GTK placed is drawn whole, since GTK paints a tooltip's
background and border on the window rather than its content, then cut to its box of opaque pixels, which
leaves its shadow out, and blended over the window by its alpha, as a tooltip's background can be
see-through. The place comes from GDK's root coordinates, which on Wayland are not the screen's but keep a
popup in step with its window (`calculate_moved_to_rect_result()` and `create_simple_positioner()` in
GTK 3's `gdkwindow-wayland.c`): GDK puts a popup's origin at its offset from the content of the window it
shows over, plus that content's origin, less where the window GTK placed it relative to is in that
window. wx's menus are placed relative to the window `popup_menu()` was given, a submenu or a drop-down
menu relative to the widget it is attached to, a tooltip relative to the window it shows over, and wx's
own popups, which wx places with `gtk_window_move()`, at their offset from the content directly. Windows,
and macOS where it draws views, draw them in too, each where its screen position puts it on the window it
shows over. wx's popups (`popups_of()`) show over the window they opened from: Windows reads one from the
screen, where it shows topmost, and macOS draws its view as it draws a window's (`capture_native_popup()`),
while its window is on the screen: wx can still report a drop-down shown after its window has left it.
Windows also reads the system's popups from the screen (`capture_native_popups()`): menus, tooltips and the
lists of native drop-downs, top-level windows of the thread that showed them, and the drop-down lists and
context menus of web views, which WebView2 shows in captionless popup windows of its browser process owned
by the window that hosts the web view. A menu shows over the window it was shown from: one of wx's, found
among the menus `popup_menu()` shows (`shown_menus()`) by its `HMENU`, submenus included, as wx shows them
recursively; one the system shows itself, an edit control's or a native dialog's, over the window of the
thread's active menu (`GetGUIThreadInfo()`), unless a wx menu shows too, which leaves its origin unknown.
Another popup shows over the first of its owners (`GW_OWNER`) that is neither a popup nor hidden or, without
one, over the topmost of our windows below it that it overlaps (`EnumWindows` lists windows topmost first).
When that window is not a wx one, such as a native file dialog, or DWM cloaks it, captures do not draw it,
and the popup is left out with it. A popup read from the screen holds whatever the screen shows
there, so Windows leaves out one that DWM cloaks or that a window above it covers in part: another
application's, a window of ours that is not a popup, or a popup over another window or left out. Only a
popup over the same window that is drawn itself, and a layered window at constant alpha 0, do not count. AppKit
draws macOS's menus and tooltips in windows of its own, which only the window server shows, so drawing
views leaves them out. The full-size PNG goes to `save_path` or `<datadir>/mcp/out/OrcaSlicer-screenshot-<timestamp>.png`. The image
block is scaled to `max_dim` on the long edge (default 1280, clamped to 256–2000).
`include_image: false` returns only the path.

## Recording

`start_recording` records what OrcaSlicer shows into a video while an agent, a replay, a script or a person
works, and `encode_recording` turns it into an MP4 to attach to an issue. Recording has to stay light,
since it runs while the app works: frames are kept as JPEG in a Matroska file (MJPEG), and the H.265
encode, which costs far more, comes after it. There is no audio. `McpRecorder` holds at most one
recording; `get_app_state.recording` shows it while it runs.

**Capture.** The UI thread captures a frame per tick of a one-shot `wxTimer`, which fires inside
modal loops too. `start_recording` takes the first frame in its own UI closure: it fixes the video's
size (fit to `max_dim`, rounded down to even numbers for 4:2:0), and a failure fails the call before
any file exists. The `window` target is the main window as `screenshot` captures it, with the modal
stack (`ModalTracker::stack()`, outermost first, so in stacking order) composited at each dialog's
client offset, resampled to the main window's content scale off MSW. Modeless windows and native
dialogs are left out: `wxTopLevelWindows` is creation order, not stacking order. Where the capture
does not read the screen, popups, menus and tooltips (on macOS, wx's popups only) are drawn last, over
the dialogs, rather than into the window each shows over (`CaptureOptions::popups`), so a drop-down list
reaching past its dialog is not cut off, and the pointer follows a wx popup to its window. They are taken as
for screenshots, so on Windows a popup over a native dialog, or one partly covered by a window other than
popups over the same window, is left out, and so is one over a window the recording does not draw. A capture that
reads the screen (`Capture::from_screen`: X11, and macOS's window-server capture) already shows our
windows over the main window as they are, modeless ones and native pickers included (a native message box,
an in-process panel, should show too; OrcaSlicer raises none on demand on macOS to check), and nothing more
is composited; the frame decides, so a macOS recording that falls back to drawing views composites them
from then on. Popups reaching past the window are cut at its edge, as everywhere. X11 shows Action
Replay when it covers the window (a reparenting window manager's frame is searched for our window inside
it); macOS leaves it out. The capture records the
rectangles it drew another of our windows in (`Capture::covered`) and composites the 3D views
everywhere else, so they are not pasted over those windows (screenshots get this too). A macOS
window-server capture shows the 3D views as presented (`Capture::canvases_shown`), and a recording keeps
them so (`CaptureOptions::keep_shown_canvases`): it draws no canvas there. Wayland
never tells the app where its windows are, so there `top_level_offset_in()` centres each dialog on the
window GTK made it transient for, and that one on its own, as compositors place a dialog: KWin centres
its frame, title bar included, so the video shows the dialog half a title bar higher than the screen
does. The pointer follows the same placement (`drawn_shift()`). It follows wx's mouse events, which
AppKit's menu tracking and native dialogs do not send, so it is not drawn moving inside a menu. The
result's `note` is the last frame's: which of these applies, then what that frame's capture
approximated, so a macOS recording that fell back to drawing views says so. `capture_frame()` stores it
under the recorder's mutex, since `stop_recording` reports from another thread while a tick may be
capturing. The `canvas` target is `shown_canvas()`. A recording captures with
`CaptureOptions::hide_covering` off, so X11 never hides and re-shows Action Replay at each frame.

The costly part is a 3D view: `render_and_capture()` draws a frame, after any extra frames a change
asked for, and reads it back. A recording
keeps each canvas's last capture with `GLCanvas3D::presented_frames()`, which counts the frames the
canvas presented, and its size and the UI generation; while all three are unchanged, the frame on
screen is the kept one, so the canvas is drawn only when it changed. Each tick measures its capture
and starts the next `max(1000/fps, 3 x capture time)` after its own start, so recording takes at most
about a third of the UI thread wherever window capture is slow (macOS's window-server capture or view
drawing, X11 `XGetImage`). A
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
down. Frames are written in capture order through a reorder map; a failed encode is a skip (`failed`), and a
dropped frame has no sequence number, so the order never waits on a hole. `MkvWriter` writes the
Matroska file: one `V_MJPEG` track, SimpleBlocks with millisecond timestamps in Clusters of 5 s, and
the Segment, Cluster sizes and Duration written as unknown and patched on close, so a file never
closed (a crash) still reads up to its last frame. Matroska rather than AVI because frames keep their
real, uneven times; one file rather than a folder of JPEGs because it plays as it is.

**Ending.** `stop_recording` first captures one more frame in a UI closure, when the UI thread can run
it, so the last call's effect is in the video even if the timer has not ticked since. A recording ends on
`stop_recording`, after `max_seconds`, at 4 GiB, on a write error, when
the replay or script that started it ends (`run_replay_job()`, which names a file still being written after 30 s
as the result's `unfinished_recording`), when the endpoint closes for a client's
(`stop_serving()`; a run's carries on), and at exit. Ending from any thread sets a flag the next tick
reads, tells the sink to finish, and posts the timer and input filter's cleanup to the UI thread; the
sink drains its queue, writes the last frame again at the end time so the video lasts until then,
closes the file and calls back, on its own thread. So ending never needs a UI round trip: a run's
end waits for the file without deadlocking against `McpService::stop()`, which joins run threads on
the UI thread, and a step after the replay sees a closed file. At exit `stop()` waits 5 s for the
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
names the job, which only exists once the file is closed: an agent finds it in `get_app_state.jobs` and waits
for it by its id, as the command's description says.
Start Recording starts the service without its endpoint when the server is off, as Action Replay and the
Run Script panel do. The person's recording has its own owner (`kPersonOwner`), so neither a run's end nor the
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
that are not `Inspect` (mutate and recover tools, replay steps included) as the tool's translated
title and what it acts on (`caption_detail()`), with a mark: a ring while it runs, then a dot, red
with the failure's first line, from which `caption_error()` takes out every value the caption hides.
One caption shows at a time, the current call's (`CaptionBoard::current()`): of the calls that run or
linger (4 s after they end, then fading), the one started last, so a replay's steps replace one another
and a replay still running between them shows again once its step has faded. Clicks and keys are not
captioned. With `pointer`, a `wxEventFilter` sees every mouse and click-type command event, a person's
and the ones the GUI tools synthesize alike: mouse events move the pointer, presses add a ripple, and a
button, checkbox, toggle or radio command with no press on it just before is a click made without the
mouse (`click`), shown at the control's centre. Events in windows the frame doesn't show, and in
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
`printhost_apikey` is a plain text field) shows `•••` for its value; `set_widget_value` shows its value only
for a non-sensitive setting located by its key, since a field located by its label, or a ref not yet
resolved, may be a password box; and `press_keys` shows its chord, never its text.

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
(`ToolDef::cancellable`: `cancel_job {job}` stops it through `McpService::request_stop()`). On Windows it is
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

`get_slice_stats` reads what the Preview shows for a plate without selecting the plate. Its tables
come from the plate's `gcode_statistics_json()`, the serializer behind the slice reply's statistics
and the exported `statistics.json`, which sums the moves as the legend does: `summary` is the slice
reply's `plate_summary()` with the prepare and travel times and the travel distance; `features` has
each feature type's seconds, share of the normal-mode print time and grams, keyed by the English
`;FEATURE:` names (`ExtrusionEntity::role_to_string()`), and a `Travel` row (the shares leave out
retractions, wipes, Z moves, tool changes and the prepare time, as the legend's do); `layers`,
paged, numbers layers as the slider's value (G-code layer index + 1, which `set_view {layer}` and
`get_app_state.preview` use) and carries each one's Z, so tools that number layers by the slider's label
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
show it. `get_app_state.preview` is the same state without the legend, or null when the Preview holds no
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
sets the type the next toolpaths show. `legend` names rows as `get_slice_stats` lists them, in any case
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
sets with `set_widget_value` and whose Upload or Upload and Print button it presses with `answer_dialog`.
The plate is then sliced if needed, its G-code exported in the background process, and the upload
queued. Host settings are printer settings (`host_type`, `print_host`, `printhost_apikey`, and
`bbl_use_printhost` on a Bambu printer), written with `set_settings {scope: "printer"}`. Where the
Print button would not send to a print host, the command fails and says why: the printer prints
through the Bambu network or printer agents (`PresetBundle::use_bbl_network()`,
`use_printer_agents`), or has no `print_host`, the condition `MainFrame::can_send_gcode()` gives the
button. It is journaled like any `run_action`, so a replay uploads again.

`get_app_state.uploads` lists the session's rows of the upload queue (`PrintHostQueueDialog`), as the
queue window shows them: `id` (its ID column, from 1), `state` (`queued`, `uploading`, `error`,
`cancelling`, `cancelled` or `completed`), `progress` (percent), `host`, `filename`, and `error`, the
queue's message, when there is one. Uploads a person started are listed too, so an agent can wait
for any of them by polling. A failed upload also opens the queue's error dialog
(`PrintHostQueueDialog::on_error()`), which blocks changes until it is closed with
`answer_dialog`.

Two properties of the queue make its rows a reliable answer. A row reads `completed` only once the
host has accepted the file: while the bytes go out, `PrintHostJobQueue::sent_percent()` stops at 99,
and only `perform_job()`'s 100 after a successful `upload()` completes the row, so a host that
refuses the file after the last byte turns the row into an error without it reading completed on
the way. And rows are added on the UI thread: `PrintHostJobQueue::enqueue()` runs on the background
slicing thread, reads what the row shows (host, upload path, file size) before the upload thread can
remove the source file, and posts `append_job()` to the queue dialog itself, the handler the upload
thread's progress and error events go to, so the row exists before any of them is handled.

## Replays and the journal

```jsonc
{ "orca_replay": 1, "name": "repro #12345",
  "vars": {"model": "${replay_dir}/part.stl"},
  "steps": [
    {"tool": "open_files", "args": {"paths": ["${vars.model}"]}},
    {"tool": "slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}},
    {"tool": "run_action", "args": {"id": "orca_command:open_preferences:orca"}, "await": false, "save_as": "prefs"},
    {"tool": "answer_dialog", "args": {"dialog": {"title": "Preferences"}, "button": "close"}},
    {"tool": "wait", "args": {"for": "job", "job": "${jobs.prefs}"}},
    {"tool": "screenshot", "args": {"target": "canvas", "save_path": "${out_dir}/preview.png", "overwrite": true}}
  ]}
```

A replay is JSON with comments allowed. `run_replay` runs a file or an inline replay as a job whose
steps call tools as its children (`ToolContext::call_child()`), so steps are not journaled and a
replay's `wait {for: "idle"}` doesn't wait for the replay. An inline replay is the replay object or
its JSON text, comments included, so a caller holding a replay file's text passes it as read; the
journal records the text as the object it parses to. Every string is substituted recursively
with `${replay_dir}`, `${out_dir}`, `${env:NAME}`, `${vars.NAME}` and `${jobs.NAME}`; a string that
is just one placeholder becomes the referenced JSON value. A replay's own `run_replay` step
gives its nested replay as an object: the outer run splices placeholders into a text replay as plain
text, so a value holding a quote or a backslash, such as a Windows path, breaks its JSON. A step that returns a job is awaited,
up to `timeout_s` (default 120), before its `expect` is checked; a wait that times out with a dialog
open names the topmost one, since a step can't answer a dialog it opened. `"await": false` returns the job
at once and stores it under `save_as`, so later steps can answer a dialog it opened and then
`wait` for it. `expect` maps JSON pointers into the result to matchers (`eq ne gt ge lt le contains
regex exists empty`). A failed step or expectation stops the run unless the step is `optional`.
Auto-answer rules registered by a replay end with it. `vars` passed to `run_replay` override the
replay's. A step naming an unknown tool makes the replay invalid before anything runs; so does any
field of the wrong type, and broken JSON is reported with its line and column. The result, also
written to `<out_dir>/result.json`, is `{passed, name, failed_step, steps: [{index, tool, args, ok,
duration_ms, result | error, expect_failure, cancelled, reason}], out_dir, result_file}`. A failed
step's `reason` says why in parts a host can word: `cancelled`, `timeout {seconds}`, `expect {pointer,
op, expected, actual, exists}`, `expect_invalid`, `placeholder {text}`, `missing_job {name,
saved_by}` (the step that saves that job, when one does) or `tool` (the tool's own error). A replay
file's `out_dir` defaults to `<replay name>.out` beside it. An inline replay's defaults to
`<datadir>/mcp/out/replay-<timestamp>`, which is also its `${replay_dir}`.

The engine (`McpReplay.cpp`, wx-free) and the job around it (`run_replay_job()` in
`McpToolsReplay.cpp`) are shared by `run_replay`, `--mcp-run` and Action Replay. A run
stops before its next step, and stops waiting on a step's job, once the service stops or a stop was
asked for any job in its lineage (`McpService::request_stop()`), so a nested `run_replay` stops with
the run that holds it. Stop requests last the session, and `JobRegistry` records each job's ancestor
chain when it is created, so a descendant still reaches its root after finished ancestors are
forgotten. A step during which the stop came is failed as `cancelled` whatever it returned (a
cancelled slice still reports its plates) and ends the run, `optional` or not. The engine reports
each step as it starts and ends, and can run a single step with the jobs earlier runs saved.

`orca-slicer --mcp-run FILE [--mcp-run-out DIR]` runs a replay (`.json`) or a Python script (`.py`) once
the GUI is ready. It doesn't open the endpoint unless `--mcp` is also given. A replay writes `result.json`
and screenshots to the output directory, which defaults to `<replay name>.out` next to the replay. Then
it closes the app without prompting and exits 0 if the replay passed or the script finished, 1 if a step
failed or the script did not finish, and 2 if the file is invalid or can't be read. `OrcaSlicer.cpp`
resolves both paths against the original working directory, since the GUI changes it. wx ignores
`OnExit()`'s return value, so `GUI_Run()` returns `McpService::run_exit_code()` after `wxEntry()`.

### Action Replay

File › Action Replay… and the Speed Dial command `open_action_replay` (an id kept because exported
replays and agent sessions already call it) open `ActionReplayDialog`, where a person opens or writes a
replay file, reads its steps and replays it whole or one step at a time. It is a `WebViewHostDialog` page
(`resources/web/dialog/ActionReplay/`), modeless and one at a time (`MainFrame::show_action_replay()`).
It takes JSON replay files only. A `.py` file, picked with the picker's "All files" or dropped on the
window, brings up a notice that Python scripts run in the Run Script panel, with Open in Run Script
(`RunScriptPanel::open_file()`, which asks about the panel's unsaved changes first) and Cancel. The page
navigates nowhere but its own loads (`WebViewHostDialog::own_page_navigation()`); every other navigation is
vetoed, since it would hand the page's message handler to a page that isn't the app's. A file dropped on the
window is such a navigation, to that file (`WebViewHostDialog::dropped_file()`), handled after the event: a
`.json` through `load_file()` once the unsaved-changes question is answered, anything else ignored, and
nothing while a replay runs, as Open is unavailable then. The browser's own keys are off, so Ctrl+R and F5
don't reload the page back to its draft. The page's editor starts from a commented
draft and suggests the replay's keys, tool names, a tool's arguments and their allowed values, and
`${…}` placeholders; the tool part comes from the registry, which the dialog sends once the service is
ready, so the suggestions match the build. Save writes the text back to the file it was opened from, or
asks where to put a new replay file and offers the replay's name as the file name. A replay saved
somewhere new is that file's from then on: `${replay_dir}` and its output folder follow the file, as for
an opened one. Closing the window, or opening another replay, with unsaved edits asks first. The page
reports every edit to the host, which parses, runs and saves that one copy of the text, so the host can
ask and save even when the window's close box starts the close. Opening the window starts the service
without the endpoint, as `--mcp-run` does, so a person replays files with the server off.

**The editor.** Action Replay and the Run Script panel edit in Ace (`ace-builds` 1.45.2, BSD-3-Clause),
vendored unmodified in `resources/web/include/ace/` (its `src-min-noconflict` core, the JSON and Python
modes, the language tools and the search box) and set up by `resources/web/dialog/js/code_editor.js`.
The pages are `file://` URLs, so they load it with script tags, and nothing is loaded on demand: no
workers, no snippets, the default commands that would load a file the repository doesn't ship (the
settings menu, error navigation, go to line, the command palette, the mode select) removed, the
completers set to the page's own as an array, and the theme applied as an object built from the host's
theme tokens, never by name. Ace draws only the rows on screen, and brings indentation, bracket matching,
folding, search and replace, multiple cursors and its own undo. Its own text (the search box, the
suggestion list, the read-only note, folding and its ARIA labels) is translated: the host puts
`WebViewHostDialog::code_editor_strings()` under `code_editor` in the page's strings, keyed by Ace's
message keys, and `code_editor.js` hands them to `ace.config.setMessages()`. Each page reports the
chords its editor binds (`editor_keys`), which the main frame's menu rule reads (Keys, under Run Script
panel). Action Replay's language is a JSON mode of its own (`orca_replay`, `replay_editor.js`).

`McpService::start_replay_run()` runs the replay as a job of `run_replay`'s kind but outside the
session journal, since a person's run is not an agent's call. A thread the service owns follows the
job past a `running` or `blocked_by_dialog` reply to its result, posts it once, and is joined by
`stop()`. Step reports and the result reach the dialog through the service's dispatcher as
`PersonRun` events, which the dialog drops once it is gone. `PersonRun::stop()` latches, so a stop
asked before the job exists still counts, requests the stop for the job (`JobRegistry::request_stop()`)
and cancels its operation as `cancel_job` does. Every call the run and its nested replays make
then sees `ToolContext::cancelled()`, so a step that waits, such as `wait`, returns at once. `suspend()` and `stop()` stop every run first, while the main
window they run in is still the current one.

Before a run the dialog describes each step with `describe_step()` (`McpReplaySummary.cpp`, wx-free):
what it acts on, and flags for effects beyond the open project (writes a file outside the output
folder, changes presets, replaces the project, sends to a printer). It reads the
arguments after substitution, and nested replays as their run sees them: an inline one with the outer
replay's placeholders already applied, as `run_replay` substitutes the whole step first, and a file
with its own vars and output folder; a step whose effect can't be
read (a plugin action, a nested replay too deep or unreadable) gets every flag. Flags only inform. A
replacing step either checks unsaved changes itself (`new_project`, `open_files` of a project,
`generate_calibration_test`) or goes through the app's own save question (`close_with_confirm()`). When the project
has unsaved changes and the steps to run hold a step of the first kind, or one of the second kind
with a step that can answer a dialog, the dialog asks whether to save first. Yes saves; No lets the
run's replacing tools treat the run as `discard_changes: true` (`McpService::discard_authorized()`,
read from the job lineage when the tool checks), so changes are dropped only when a step really
replaces the project. The runner never answers the app's own question.

The window is named `kHiddenFromAutomation`: `list_widgets` and locators skip it, `list_windows` lists
it as hidden with no ref,
`run_javascript` never picks its web views, a key sent without a target never goes to the focus inside it, and on X11,
where a window capture reads the screen, it hides for the moment of the capture. Every prompt and
file dialog it opens is named so too, so no auto-answer rule a replay registers answers the person's
own question. Its own wording is
translated, from the structured step descriptions and failure reasons; a tool's own error, which
agents read in English, is shown as a detail under a translated lead. Tool titles are marked with
`L()` so the dialog can show them translated while `tools/list` keeps the English.

The session journal records client calls to journaled tools in order: every mutate tool, plus
`wait` and `screenshot`, which a replay needs. It keeps at most 5000 calls and 64 MiB of their
arguments (serialized), so a long CAD run's large `sketch_add` calls can't grow it without bound:
after each change it drops the oldest calls whose outcome is known until it is within both. A call
still waiting for its reply or its job keeps its place, so calls still waiting may hold it over for a
while; once they finish they are dropped like any other call, oldest first, and only while the
journal is still over a bound. GUI tools record locators instead of refs through
`ToolContext::set_journal_args()`. `export_replay` returns it, and with `path` writes it, through `Journal::to_replay()`; `clear`
starts a new recording. `Journal::to_replay()`
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
`running` is dropped like a call that failed at once, so a replay never repeats a call that did
nothing (a slow CAD rebuild that then fails). A call whose outcome is still open, because its job
is still running or it has not replied yet, is left out, and `export_replay`'s reply lists it in
`left_out_running: [{tool, job}]` (`job` is absent for a call that is not one); a later export
includes it once its job has succeeded. A call whose reply was `blocked_by_dialog` stays in the
replay while its job runs and whatever its job's result: the steps after it answer its dialog, and
a replay without it would fail at the first of them. A call whose job was still running when a later
call acted with a modal open (`answer_dialog`, or a recover tool's input while
`UiInvoker::open_dialogs()` lists one) stays the same way, whether it had replied yet or not, since that
dialog may be one its job opened; the journal can't tell whose dialog it was, so it keeps every such
job, as a step with `"await": false`. Its replayed reply can come before the dialog opens, so each
call that acted with a modal open is preceded in the replay by `wait {"for": "dialog"}`, expected to
be satisfied. Only the call's own job counts as its job: a
`wait` that timed out names the job it waits on in its reply, and stays a plain step.

## Python scripts

A Python script is a `.py` file or a code string that runs on the plugin interpreter as an embedded macro: the
main program of a run inside OrcaSlicer, with the whole plugin API (`orca.host`), without being packaged as a
plugin. A person runs one in the Run Script panel or with `--mcp-run file.py`; an agent sends one with
`run_script {code, args, timeout_s, filename}`; a plugin's code may call `run_script` too. A script that runs
under `python file.py` runs the same way here, and code written for a plugin's `execute()` (windows, progress,
presets, storage) runs from a script unchanged. A script is not a plugin: it gets no permission prompt and no
grants, is never in the Plugins list, and ends when its work ends, leaving nothing loaded.

**Entry contract.** `run_python_code()` (`plugin/PythonScriptRunner.cpp`) runs the code on the calling thread, the
MCP job that runs `run_script`, holding an interpreter lease and the GIL, as a session with one file work (Run
Script panel, below). The code is text, compiled as Python
compiles any code string, so a coding line in it changes nothing; a file's bytes are decoded first as Python reads
a source file, by its BOM or its PEP 263 declaration, else as UTF-8 (`decode_python_source()`). It runs as the main
program, in a `types.ModuleType("__main__")` of its own, with `args` bound and `orca` importable. A run is a *file
run* when `filename` names an existing file (a saved script's in the panel and `--mcp-run`'s do): `__file__` and
`sys.argv` (`[path]`) name it, its folder serves its imports, and its PEP 723 dependencies are installed first.
Any other name (the default `<run_script>`, a bare traceback name) is code sent as text, which gets none of that;
`filename` still names it in tracebacks. The value left in `result` is read once when the body returns; in code
sent as text, one that is not JSON (a dict with string keys, a list, a string, a finite number, a boolean or None,
nested) makes the run `raised`, while in a file run, where a standalone script may well have a variable of that
name, it is no result, and the output ends with a line saying it was not JSON. `SystemExit` with code 0 or None
finishes the run. Output is captured up to 256 KiB by a stream installed once over `sys.stdout` and `sys.stderr`
that routes writes by the run bound to the writing thread: the run's own thread, the threads it starts, its pool
tasks and its window callbacks; other threads print through. The outcome is `finished`, `raised` (with the
traceback), `cancelled` or `timed_out`, reported in a successful result, as `run_replay` reports a failed step.

**The run scope.** `PythonRunScope` (`plugin/PythonRunScope.hpp`) is the run's state that its threads, pool tasks
and window callbacks share: identity, file, tool bridge, the work it waits for, admission, and whether its UI is
closed. It is bound thread-locally (`ScopedPythonRun`, `current_python_run()`) to the run's thread, carried by the
interpreter's thread shim into the threads it starts, by a wrapper of `concurrent.futures.thread._WorkItem.run`
into its pool tasks, and captured by its window callbacks. Lock order: the GIL before the scope's mutex; nothing
holding the mutex waits for the GIL.

**One interpreter, many runs.** Runs overlap (an agent's beside a person's), so everything a run changes is keyed
by the run bound to the calling thread (`_run_id()`), in the helper module `_orca_run_capture` the runner installs
once (under a lock taken without the GIL, since its imports release the GIL). The interpreter's own `__main__`
module and the `sys` module get a `ModuleType` subclass (module `__class__` assignment): attribute access on
`__main__` goes to the bound run's module, so `typing.get_type_hints`, `dataclasses` and pickling find a script's
globals, and `sys.argv` to the bound run's own list, so every list operation and reassignment stays in the run;
code outside runs sees the app's. C code that reads the modules' dicts directly (`PyModule_GetDict`,
`PySys_GetObject`) sees the app's too. Routing `argv` costs every Python attribute read on `sys` about 70 ns. A
script that puts a plain class back is undone at the next run's entry, and one that replaces
`sys.modules["__main__"]` gets the original back when it leaves. No run changes `sys.path`: a finder in
`sys.meta_path`, just before the path finder and so after the built-in and frozen importers as `sys.path[0]` is
under `python file.py`, serves the threads of an active file run from the script's folder through `FileFinder`
(source, extension modules, sourceless `.pyc`, namespace packages), and the submodules of the packages it served
there, never those of a package found elsewhere, even one installed below the folder. It compiles source afresh
each run, never from or to a cached `.pyc`, so an edit shows on the next run whatever the file's timestamp. What it
served for a folder is evicted when the last active run from that folder ends, each module only while
`sys.modules` still holds the module it found (by its spec), never an extension module, which can't load twice,
nor one a plugin's load recorded as its own. `sys.modules` stays one cache: a script's `import utils` gets a `utils` the app or a plugin
already imported, and two overlapping scripts in different folders with a helper of the same name share whichever
came first, so helpers want distinct names. Modules a script imports through folders it adds to `sys.path` itself
stay cached, as Python's cache keeps them.

**Threads, pools and the end of a run.** Like `python file.py`, a run ends once its work is done: its code, the
non-daemon `threading.Thread`s it starts, its window callbacks while they run, and the pool tasks it submits. Its
code runs on a thread Python didn't start (an MCP job thread), which Python sees as a daemon dummy thread, so the
body runs with that thread's daemon flag cleared: as on `python file.py`'s main thread, a thread it starts is
non-daemon unless it says otherwise. Unlike there, `threading.main_thread()` is still the app's main thread, and
the run's own thread is named `Dummy-N`. The shim counts a thread on the run in the starting thread, before the OS
thread exists, so a thread started late,
even by another of the run's threads, is never missed; raw `_thread` threads are not waited for, as Python's exit
doesn't wait for them. A pool's workers are reused across runs, so they carry no run and are never waited for,
though the pool's initializer, which runs on a new worker before any task, runs inside the run whose thread started
the worker, as the task it was started for does (its binding and identity, one of its interrupt targets), counted
as the run's work from before the worker's thread starts until it returns, so the run waits for it even when that
task is cancelled. Once the run's admission has closed, such a worker doesn't start (a thread of the run gets the
same error as for any thread), since its initializer would run outside any run and identity. A work
item created from a thread of a run (a wrapped `_WorkItem.__init__`; `submit` creates exactly one per task it
accepts) gets a tag of that run, a C++ object that counts its task on the run and uncounts it once, and the
`_WorkItem.run` wrapper runs the whole item (the task, the future's completion and the done callbacks the pool calls
from there) inside the submitting run, whichever run started the worker, with the worker as one of its interrupt
targets; no `BaseException` gets past it, so neither a Stop nor a `SystemExit` in a callback, nor the stock `run`
refusing a future completed by hand, ends a worker other runs share. A done callback therefore runs in the run
that submitted the task, whoever added it; the callbacks of a future no pool runs (a bare `Future` another thread
completes), and those `cancel()` runs on the cancelling thread, run in that thread's own context, as in CPython. A
Stop in one done callback ends that future's later callbacks too, as any `BaseException` does in CPython's callback
loop. A future completed without its item running (cancelled, or failed by a broken pool) is uncounted by its
callback, the tag making sure only one of the two uncounts the item; the worker that later takes a cancelled item
still marks its future notified, which `wait()` and `as_completed()` need. A `submit` that raises after creating
its item (a wrapped `submit`) cancels it: the caller got no future, and the item may sit queued with no worker to
run it. The callback and that hand-off are in place before the count is taken, so no count is ever left without
them. The run waits in C++,
without the GIL, never through `Thread.join()`, whose interrupted wait marks a live thread stopped, and closes its
admission in the same locked step that sees nothing left: from then on a thread of the run can't start a thread
(it raises, as at interpreter shutdown), a submit from one raises, and a callback does nothing. Then it shuts down
the pools its own `__main__` code created (`ThreadPoolExecutor.__init__` records the creating frame: the run's
`__main__`, a module the finder served for a folder, or neither); a folder's pools are shut down when the modules
that made them are evicted, so one run never shuts down a pool another run from the folder still uses, and other
pools (`from library import POOL`) keep working. `atexit.register` called from a run's `__main__` code records on
the run, called from a folder's helper (at its import, say) records on the folder, and anything else (`logging`,
`multiprocessing`) registers with the process; `atexit.unregister` removes only from the caller's own
registrations, filed the same way. The run takes its functions once, as Python's exit does (one registered while
they run is not called), and calls them last first once it is quiet. A folder's teardown, when its last run ends,
first takes what the folder holds, evicts its modules and lists the pools they made, as host bookkeeping no
interrupt reaches, so a run that enters the folder while the rest runs imports its helpers afresh and keeps them and
their pools. A module that stays (a plugin's, an extension) has its `atexit` functions handed to the process's exit
there. Then, as Python ends a program, it runs the evicted modules' `atexit` functions, clears their globals (so
what they hold, cycles included, finalizes there, under the run's watcher, while their pools still work) and shuts
down the pools it listed. Each of these cleanup steps (the run's pools and `atexit` functions too) works through
what was taken piece by piece, removing a piece as it starts (an `atexit` function) or once it is done (a module's
globals, a pool), and the runner resumes the step after an interrupt, so an interrupt cuts only the piece it lands
in. A pool is shut down again on a resume even when its `_shutdown` flag is set, since an interrupt may have cut its
shutdown between setting the flag and waking its workers. The teardown
waits while a plugin's import is in progress (`PythonInterpreter::plugin_import_in_progress()`, since a plugin
records its modules only after its import returns), and then runs at the next entry from that folder, before that
run's code, on its thread, under its identity and watcher, with the output in its capture. A daemon thread still
running after the run sees the run's globals, and its evicted helpers', cleared, and is refused tool calls.

**Cancellation.** A watcher thread interrupts the run with `PyThreadState_SetAsyncExc` when `cancel_job {job}` stops
the job (`run_script` is `cancellable`) or the time budget (`timeout_s`, default 300 s; the panel's session has
none, since its Stop ends what runs) runs out, again every second until it ends. What it interrupts follows the run's phase,
which is set, and read before each interrupt, under the GIL: in *Body* (the code) and *Exit* (`atexit` functions,
the result's own code, the UI teardown, the namespace's finalizers, the folder's teardown, dropping what the script
left in its routes, its `sys.argv` or in place of `__main__`) the run's thread and its interrupt targets (its
threads, and the workers running its items or its pools' initializers); in *Threads* (the wait) the targets only;
in *Drain* (the host's bookkeeping: entering the run and its folder, taking the run's `atexit` functions, releasing
the folder and taking what its teardown evicts, detaching the run's routes, the output, closing the tools) nothing,
so no interrupt breaks the host's own bookkeeping, though a stop or time-out there is still recorded. The runner
switches to *Drain* for each piece of bookkeeping and back to *Exit* for the cleanup it hands over, so what the
script left finalizes only where a stop can still reach it. At the first stop or time-out it also cancels the
run's tool calls in flight (`PythonRunRequest::stop_calls`, the job's stop request), so a waiting tool returns at
once, marks the run stopped, so its queued pool tasks don't run, and closes its UI. Five seconds after a stop the
run stops waiting for its work and closes its admission anyway. Code blocked inside a C call, or a tool call that
can't be interrupted, holds the run until it returns. A pending interrupt is cleared before the thread runs
anything else.

**Dependencies.** A file run's PEP 723 block (`# /// script` … `# ///`) is found, after normalizing line ends, with
the PEP's reference expression, and read with `tomllib`, so quoted commas, multi-line arrays and comments read as
TOML says; a malformed block, two `script` blocks or a `dependencies` that isn't a list of strings make the run
`raised` before its code. Its dependencies are installed before the code, without the GIL, through
`plugin_loader::install_packages()` into the shared package folder plugins use; installs are serialized, so a
script's and a plugin's never write the folder at once. An install that fails makes the run `raised` before its
code. The install isn't covered by the watcher; uv's 120 s timeout bounds it.

**Identity.** A person's script runs under the audit identity `script`, an agent's under `mcp`
(`kScriptIdentityPerson`, `kScriptIdentityAgent`); `McpService::person_run()` tells them apart by the run's
lineage, since the panel's and `--mcp-run`'s jobs are marked as a person's (`as_person()`). Every audited
access is allowed to these identities and logged under them (reads at debug level), apart from the categorical
denies every plugin has (`is_denied_path()`): nothing contains the code, and the setting is the boundary. The
threads, pool tasks and window callbacks of a run carry its identity. A run owns what its calls leave behind, as a
replay does (`begin_run_scope()`): auto-answer rules, a recording and a held press end with it. The
identities are reserved: a plugin whose key equals one is refused before any of its code runs. A plugin's
`run_script` runs under the plugin's own identity and grants instead, with the same main-program environment, but
its windows, callbacks and storage stay the plugin's.

**The setting.** One switch, `mcp_python_scripts`, off by default, lets both a person's scripts and an agent's
`run_script` run; with it off they are refused (a plugin's `run_script` needs none). It is changed only in
`PythonScriptsDialog`, opened from Preferences › AI features › Python scripts, which is named
`kHiddenFromAutomation`: the UI tools never see it and `ModalTracker` leaves it off the stack, so no dialog tool or
auto-answer rule reaches it. No tool writes the key, and `get_app_state.python.scripts_allowed` reads it, so an
agent learns that `run_script` is off before it plans a run rather than from the refusal.

**The host surface and the bridge.** A script reaches the whole `orca.host`, as a plugin does: reads, `orca.host.ui`
(message boxes, progress dialogs, HTML windows, dock panels, notifications), the staged preset update, bundle
reload and the 3MF copy; nothing is plugin-only. `orca.host.plugin.storage()` is a script's own folder, the folder
of its file; code sent as text has none. `orca.host.tools.call(name, args)` and `run_replay(replay, vars)`, whose
replay is a dict or its JSON text (`plugin/host/PluginHostTools.cpp`), reach the tools from any thread of the run
while it lasts, through the run's bridge: its calls are `ToolContext::call_child()` of the `run_script` job, so
each has its tool's guards, admission and undo, and the GIL is released while it waits. At the end the bridge
closes, and the run waits for calls already inside it before its `ToolContext` goes. An error result raises
`orca.host.tools.ToolError` with its data as `details`. Code with no audit identity at all is refused.

**A script's UI.** What a script's run opens (windows, dock panels, progress dialogs, message boxes, notification
links) is registered under a run-unique owner (`script/<id>`; a plugin key never holds `/`), and closes with the
run: after its `atexit` functions and before its namespace is cleared, the run marks its UI closed and tears it
down on the UI thread, waiting for that (`PluginHostUi::close_run_windows()`), so none of its callbacks runs after
its globals go. At the first stop or time-out the watcher does the same at once (posted only while the app isn't
closing), which ends a modal loop the run waits in: a script's `ui.message` box is registered while it is shown and
ended with `EndModal`. From then on a creation from a thread of the run raises, and one already posted checks the
run on the UI thread and creates nothing, so nothing the run opens outlives it. A window callback (`on_message`,
`on_submit`, `on_close`, a notification's `on_click`) captures the run when it is made and runs inside it,
counted as active work; it runs on the UI thread, so it is refused tool calls (the tools wait for the UI thread),
and hands its work to the script's own code, through a `queue.Queue` say, which can call them. While a modal the
run opened is up, tool calls that change the project are refused by the modal guard, the run's own included, so
interactive macros use modeless windows. A script's dock panel keeps its saved layout under the script identity and
its title. A plugin's `run_script` leaves all of this to the plugin.

**Idle.** A `run_script` job doesn't count as busy for `wait {for: "idle"}` (`JobRegistry::set_idle()`); the
calls it makes, and what they start, do. A macro waiting for the person in its window would otherwise hold every
agent's idle wait, and a `tools/mcp` driver run from the panel against the same app would wait on its own run.

**Journal and replay.** `run_script` is journaled as one step holding its code, kept when the code failed
partway; its tool calls are not journaled. In a replay a `run_script` step's `code` is never
substituted, and a step whose outcome is not `finished` fails.

**The reference.** `search_python_api` serves the reference `python_api_reference()` builds from the running
build's `orca` package: `orca.host`, and the classes, events and pages plugins build on. It walks the
modules and lists each public class, function, method, property, named instance (such as
`orca.host.preset.process`) and constant (such as `orca.host.ui.PD_CAN_ABORT`) with its signature and
documentation. The signature lines pybind11 starts a docstring with become the signature, a class's are its
constructors, and an enum's values stay in its documentation rather than becoming entries, as they would
where a module exports them. The documentation is the bindings' own docstrings, a class's followed by what
its constructors say of their arguments; a constant can't carry one, so its documentation names the entries
whose documentation names it. The reference is only as good as the docstrings: a test requires every entry
to have documentation, every constructor that takes arguments to say what they are, and every parameter a
name (pybind11 names an unnamed one `arg0`). A query ranks the
entries as the settings search does (`query_words()`, `match_score()`); without one the whole package is
listed with each entry's first sentence. The reply says whether numpy is installed, which the mesh and
matrix arrays need and which the bundled Python lacks unless a file run's PEP 723 block installs it.

### Run Script panel

File › Run Script (the Global shortcut Ctrl+Shift+R, ⌘⇧R on macOS) and the Speed Dial command
`toggle_run_script` show or hide the Run Script panel (`RunScriptPanel`, page `resources/web/dialog/RunScript/`),
a dock pane of the Plater under the 3D view and the sidebar, where a person edits a `.py` file in `# %%` cells and
runs a cell, the whole file or a console line in a live session. Showing it selects Prepare when the current tab is
neither Prepare nor Preview; the menu item's check mark is whether it is on screen.

**Sessions.** `PythonSession` (`plugin/PythonSession.hpp`) is a `__main__`, folder, identity, output capture and
watcher that outlive one piece of work. The panel submits a `PythonWork` and reads `PythonSessionEvent`s from any
thread; `host_python_session()` hosts the session on one thread, under one interpreter lease and audit scope, and runs
the teardown a run has (threads, pools, `atexit`, its windows, its folder's helpers) once, at the session's end. A work
is a File (the whole script, run as `run_python_code()` runs code), a Cell (its lines, which start at the file line
`first_line`, so line numbers are the file's) or Console code; a Cell or Console work that ends in a bare expression
shows its value, as Python's prompt does, and binds it to `_`, and so does each cell of a File work whose last
statement is one (`cell_results`): `mark_cell_results()` makes that statement a call to `_orca_show_result`, which
the work's namespace binds to the capture module's `show_result()`, a C++ function, so the module keeps its lines,
cells, tracebacks and docstring, and neither `__import__` nor `sys.modules` stands between a cell and its value. A
value that is a dict or a list JSON holds as is (str keys, finite floats, strings UTF-8 holds, no tuples or
subclasses) comes as a Result event, compact JSON counted in the output's allowance; any other value, or one past the
allowance, is printed as its repr. The events
are Output, streamed as printed, with each
work and each stretch between works capped at 256 KiB and then the note `[output truncated at 256 KiB]`; Result; Cell, a
File work's cell whose lines start running, reported through `sys.monitoring` tool id 4, which the session claims only
around a File work and frees after it (when another owner holds the id the work runs without Cell events, and its
Finished says `cells_reported` false); and Finished, with the outcome, the traceback, the innermost line in the
session's file, whether the code started at all (`executed` is false for a syntax error or a failed dependency
install) and the count of variables, the names in `__main__` that don't start with `_`, aren't `args` and aren't
modules. A work's traceback quotes its `source`, the editor's text, for frames in the session's file, by rebuilding the
frame summaries, chained exceptions included, never through `linecache`, which every run, plugin and later traceback
shares; a traceback the session's own threads print reads the file on disk. A File work reports once the non-daemon
threads it started have ended, as `python file.py` ends; the threads of a Cell or Console work keep running in the
session, whose admission stays open until it ends. A session keeps the file it started with (`__file__`, `sys.argv`
and its folder), so saving somewhere new applies from the next session, and a file session installs its PEP 723
dependencies as its first work starts. `run_python_code()` is a session with one File work and its end asked up front,
with streaming, quoting, Cell events and cell results off and a stop closing every window of the run, which is how it
keeps the run described above.

**The session job.** `McpService::start_python_session()` hosts the panel's session as an unjournaled person's
`run_script` job (`run_python_session_job()`) on a thread the service owns, which `stop()` joins and
`stop_person_runs()` stops. The job is idle for `wait {for: "idle"}` as any `run_script` job is, so agents' and
plugins' runs overlap with it, sharing no state, tracebacks included. It owns what its calls leave behind for the
whole session, as a run does (`begin_run_scope()`): auto-answer rules, recordings, held presses. Between works its
rule layer is paused (`ModalTracker::set_paused()`; the rules listing shows it as paused), so a rule a cell
registered never answers the person's own dialogs between runs and answers again in the next one (a rule a thread of
the session registers between works starts paused, and leaves the dialog open then to the person too), and its held
presses are released as each work ends (`release_run_presses()`). Its `orca.host.tools` calls are child calls of a
`LineageNode`, a running idle job under the session's job that only parents calls: a stop requests the stop for the
node and cancels its operations (`cancel_operations_of()`), so every call in flight, inline ones such as `wait`
included, returns at once while the session goes on, and the next work after a stop starts once that cancelling is done,
under a renewed node. With Python
scripts off the job refuses before it hosts anything, and turning them off ends the session.

**Stop.** Stop interrupts what runs now: the work, and the session's threads and tasks; while the session is idle a
stop reaches its threads and tasks for five seconds (`kStopWindow`), and the session goes on. A stop moves the run
scope's stop generation on, so a pool task queued before it doesn't run (its future fails with "The task was stopped
before it ran") while one submitted later does, and ends the session's message boxes and modal windows on the UI
thread (`PluginHostUi::end_run_modals()`), so code waiting in them can be interrupted, leaving its modeless windows,
dock panels and callbacks open. A cancel or time-out of the job reaches everything until the end, and ends the
session. A Run all still waiting for the old session to end is dropped by Stop and reported stopped. Restart session, Close script, New, Open, a recent or dropped script, Run all (which runs in a session of
its own, as `python file.py` does), turning Python scripts off, quitting and a main-window rebuild end the session
and stop it, so it ends as a stopped run does: its threads get five seconds, its `atexit` functions run once, and
what the teardown runs can be interrupted, as a stop asked once the session is ending reaches its teardown even when
nothing else was live; then its windows close and its folder's helpers are evicted.

**Lifetime.** `RunScriptPanel` is owned by `GUI_App`, so the script's text, file, unsaved state, session and recent
scripts (AppConfig `run_script` › `recent`, newest first, at most 10) outlive the pane (`RunScriptPane`), which a
main-window rebuild (a language change) destroys: `MainFrame::shutdown()` calls `main_window_closing()`, which stops
and ends the session and stops reading its events, and the end of `GUI_App::recreate_GUI()` calls
`main_window_rebuilt()`, which builds the pane again, with an empty console, if it was shown. The pane is built the
first time it is shown and hidden at each start; its place and size are kept in the saved layout, and its close button
hides it (`add_dock_pane(..., hide_on_close)`). The page reports every edit as it happens, so New, Open, a recent or
dropped script, Close script and quitting ask Save / Don't save / Cancel against the host's current copy. While a work
runs the editor is read-only; threads alone don't lock it. The console keeps its last 2 MiB; its Clear button
empties it and leaves the session alone, keeping, during a run, the run's heading (in Run all, with the row of the cell
it runs now) for the output still to come. Run all's block holds a row per cell it reaches, folded to a heading that
shows the last line the cell printed, its result or its error; the cell a run stops in opens, and Expand all opens
every row. The page lays a JSON result out (an array or object stays on one line while it fits 72 characters, and a
result whose layout would be more than four times as long stays on one line), keeping its numbers and strings as
Python wrote them, in the editor's JSON colours, and counts what it shows toward the console's 2 MiB. Besides each cell's bar in the margin, the
toolbar's Cells menu draws dividers, header bands or shaded cells, and can tint the cell at the cursor, as Ace
full-line markers behind the text for the cells on screen; the choice is AppConfig `run_script` › `cell_style` and
`highlight_cell`. Ace has no cells, so the + in the margin a row below the last line, which adds a `# %% ` cell at
the end, is the page's own element, placed after each render. The divider between the editor and the console sets the
editor's share of the width, so a resized pane keeps its proportions; a drag stops at 220 px of editor and 200 px of
console, and its arrow keys, Home and End and double-click (back to 60%) work as a window splitter's. The arrows on its
grip hide a side, as does a drag that ends 48 px past a side's smallest width; a hidden side leaves a strip at its edge
that brings it back at its width: the editor's shows each cell's bar, the console's a dot for a run going, output not
seen, or a failed run. The console's side hides only while it shows the console, never the start page or the note that
Python is off. The share and the hidden side are AppConfig `run_script` › `split` and `hidden_side`, saved when a drag
ends rather than as it moves.

**Out of the tools' reach.** Like Action Replay's page, the page navigates nowhere but its own loads: a dropped
`.py` opens in the panel, and any other navigation is vetoed. The pane is named `kHiddenFromAutomation`, as are the frame it floats in
(`AuiMgr::CreateFloatingFrame()`) and every prompt and file dialog it opens. `skipped_by_walks()` keeps the walks of
`list_widgets`, locators and `run_javascript`'s search for a web view out of it, and a key sent without a target never
goes to the focus inside it, so a script never acts on the panel that runs it. `list_windows` lists the floating
frame as hidden, with no ref. Docked, the panel is part of the main
window and shows in its screenshots and recordings; floating, it is a window of its own and stays out of them, as the
person's own dialogs do.

**Keys.** The page handles its own keys: in the editor through Ace's commands, elsewhere through a document key
handler. The pane's web view has the browser's accelerator keys off, so Ctrl+R, Ctrl+Shift+R and F5 don't reload it.
While a code editor window (`CodeEditorWindow`: the panel or Action Replay) has the focus
(`MainFrame::focused_code_editor()`; on Windows the focus inside WebView2 is found with `::IsChild()` of
`::GetFocus()`), the main frame's key hook passes keys to the page past the global shortcuts, apart from the toggle's,
and a menu item whose shortcut the editor takes is disabled: a chord the page reported in `editor_keys` (Ace's own
bindings only, so a rebinding can't make it stale) or a key with no modifier but Shift. Save, Save As, Undo, Redo, Cut,
Copy, Paste, Select All and Run Script stay enabled, and Save and the Edit items are forwarded to that window's editor
(`forward_to_editor()`, `editor_clipboard()`), so one press acts once, on the text; a text field outside the editor
that has the focus (the console prompt) takes the forwarded Undo, Redo and Select All itself. A toggle the page asks for within
150 ms of one the main frame dispatched is the same press delivered twice and is dropped.

## Plugins and the tools

A plugin's code calls the tools through `orca.host.tools` once it holds the `tools` permission
(`PluginPermissions::tools`), which it declares with `request_permissions(tools=True)`. The person answers
it at load in its own prompt (`request_tools_permission()`), apart from the plugin's other permissions,
and a refusal leaves the plugin loading without it. With the permission a plugin can do what an agent can:
work in the GUI, answer dialogs, replace the project, write presets and files; the prompt says so. A plugin's
`run_script` is the plugin's own code: its identity and grants, its windows and storage, though it runs as a main
program as any script does.

A plugin's call waits for the UI thread, so it comes only from a thread nothing there waits for: a Script
worker, a Python thread one starts (the thread shim carries the mark) or MCP's own thread running a plugin
operation (`ScopedToolCallerThread`). A call from the UI thread, a slicing hook or another plugin callback
is refused before anything waits. `install_plugin_tool_caller()` routes a permitted plugin's calls. The UI
thread, which owns the service and runs the plugin's unload, admits each one: the plugin must still be
loaded and its scope not ended, and the service is started in its catalog-only mode when MCP is off; the
call then holds a lease the service's drain waits for. A caller that stops waiting because the app closes
marks its admission abandoned, so no lease is taken that nobody releases. The call runs as
`McpCore::call_plugin()`: a top-level job owned by the plugin
(`JobRegistry::owner()`), never journaled, whose children inherit the plugin
(`ToolContext::caller_plugin()`). The session tools are scoped to the plugin: `export_replay` is refused,
and `cancel_job {job}` and `wait {for: "job"}` act only on its own jobs (`require_own_job()`). What its calls
leave behind (auto-answer rules, a recording, a held press) belongs to the plugin's scope
(`begin_plugin_scope()`), which ends when the plugin unloads. From the moment the plugin manager unloads
it, no call of the plugin starts, inline or as a job, nor a child of one of its jobs, a call admitted just
before included (`JobRegistry::unloaded()`, which asks the plugin manager itself, since the scope ends later
from the UI thread). A job is checked once it is registered and again on its thread before its handler,
so it is either refused or registered while the plugin was loaded, where the unload finds it. A running
call of the plugin reads as cancelled at once (`ToolContext::cancelled()`), so a handler that waits or
polls ends; one that does neither (a screenshot, a settings change) runs to its end, as a call does that
started just before the unload. Ending the scope then stops the
plugin's running jobs and blocks new ones (`JobRegistry::block_owner()`) until it loads again; one still
running that would leave something behind (a rule, a recording, a press) is refused. The wait for admission also ends
when the plugins shut down, which on macOS's end of session happens without the app closing. The handlers raise no audit events,
so each call is logged as `[MCP plugin] plugin=… tool=… outcome=… paths=[…]`; the record is best effort,
since plugin code may write the log folder. A plugin's window callbacks run without its audit identity,
so their calls are refused.

**Plugin operations.** A plugin may offer operations to agents, replays and scripts with
`orca.register_operation(name, description, schema, handler, changes)` while it loads. The person grants
each one in its own prompt (`request_operation_permission()`, saved in `PluginPermissions::operations`);
a refused one is not offered. Granted operations are staged and offered once the plugin's load has committed
(`PluginOperations::activate()`, in one step under the lock an unload's withdrawal takes); any exit of a
load that did not commit, an exception included, drops them, and they are withdrawn when the plugin
unloads. An operation is reached as
`plugin.<plugin key>.<name>` through `call_tool`, `orca.host.tools.call` and replay steps, and never
appears in `tools/list`: `ToolRegistry::set_dynamic()` resolves those names (`ToolRegistry::lookup()`).
It runs as a job, under the mutate guard and journaled when it changes something, under the inspect guard
otherwise; its handler runs under the plugin's identity and grants, and each call is logged with the
plugin and the caller. A call is refused once the plugin has unloaded, by the plugin manager's state, and
by `PluginOperations::invoke()` when the operation found is no longer the one offered (withdrawn, or
offered again by a new load). The call's job is the caller's, so an operation already running when its
plugin unloads runs to its end; its own tool calls are refused from then on.

**`search_tools` and `call_tool`.** `search_tools` gives the schema of any registered tool or operation,
or lists every name; for `call_cad_tool` it points to `list_cad_tools` and `describe_cad_tool`, which list
and describe the Design tab's CAD tools.
Its search by words is the one search across catalogs: the best five tools and operations that match (more
with `limit`), a tool with the first sentence of its description, since `tools/list` has its schema, and an
operation with its schema, which nothing else lists; and beside them the best five of each other catalog with
its total and how to use it: Speed Dial
actions, the settings being edited (`search_settings()`, as `get_settings` searches without a scope), the
Python reference and the Design tab's CAD tools (scored on their details too, and noting when the Design
tab is off). Every catalog is ranked
the same way (`query_words()`, `match_score()`): a query word matches where a word of the name or text
starts with it, CamelCase parts included, and entries matching more words, then more of them in the name,
come first. Scores of different catalogs don't compare, so each keeps its own list. Actions, settings
and the Design tab's CAD tools are read on the UI thread: when it can't run the call (a native dialog holds
it, or it is busy) the search leaves those three out, and before the main window exists (the server starts
ahead of it) it leaves out the settings. The tools, operations and Python reference answer whatever the UI
thread is doing. `call_tool {tool, args}` is dispatched before it runs
(`resolve_api_call()`, in the protocol, in child calls and in the plugin caller): the target's arguments
are normalized and validated, and the call runs as the target's own, with its guard, mode, budget and
journal flag, so the journal records the target.

## stdio bridge

`orca-slicer --mcp-stdio [--datadir DIR] [--mcp-port N]` serves clients that speak only stdio,
such as Claude Desktop; `main` handles it before any GUI or CLI processing (`McpStdioBridge.cpp`).
It POSTs each line of JSON-RPC from stdin to the instance in the discovery file (or `--mcp-port`)
with the token and the headers `bridge_headers()` derives: `MCP-Protocol-Version` (negotiated, or
from `_meta`), `Mcp-Method`, and `Mcp-Name` for `tools/call`, `prompts/get` and `resources/read`.
Replies go to stdout; a 202 writes nothing.

The bridge starts OrcaSlicer only for a tool call, so an agent session that is registered user-wide
but never calls a tool never opens the app. While nothing answers, every other message is answered
by an `McpProtocol` of the bridge's own over `McpService::offline_catalog()`, given the headers the
server would get: the handshake, `ping`, `tools/list`, `server/discover`, notifications and unknown
methods get exactly the reply this build's server gives. That holds because these methods read
only the catalog and the server's details, and the server keeps no session. The catalog is held by
a service that is never started: registering a tool only describes it (a schema built from
constants and compile-time options), and its handler runs only when the tool is called, so
building it needs neither wx nor a window. A tool's registration must stay that way, never reading
app state; a unit test builds the catalog without the app.

A `tools/call` with nothing answering starts `orca-slicer --mcp` detached, with the bridge's
`--datadir` and `--mcp-port`, and waits up to 60 s. If that process exits within seconds, a running
instance without MCP took the launch over, and each tool call is answered with an error saying to
turn the server on in Preferences. The bridge does not start the app again after a failed start, so
a stuck or refused start never makes every call another launch and another wait, but each tool call
still looks for an instance; once one is found, a later tool call after it quits starts the app
again. The offline answering keeps the same containment as the HTTP transport: a message the server
fails on gets a JSON-RPC error, and the bridge keeps serving. A request that could not be sent, or was refused with 401 because the token changed, is sent once more
to the endpoint the discovery file names now: the app restarted, or its port or token changed in
Preferences. When nothing answers it, the app quit: the request is answered offline, or a tool call
starts the app again. A request that was sent but lost its reply is never sent again, since it may
have run. The client gets an error saying so, and later requests go to the new endpoint.

The bridge looks for an instance when it starts, at each tool call and when a forwarded request
fails; in between, its own build answers. An instance running when the bridge starts, possibly
another build sharing the configuration folder, answers everything, its tool list included. The
client keeps the tool list it got at the handshake, so an instance that comes up later, or replaces
the app mid-session, serves tool calls against that list. When it is another build, its server
still checks every call against its own catalog: a tool it lacks is a -32602 "Unknown tool" error,
arguments its schema rejects are an "Invalid arguments" result, and a protocol version it does not
support is a 400. Reconnecting the MCP server lists its tools. The bridge does not look for an
instance before each message it answers offline: the handshake follows the startup lookup at once,
and each lookup costs a connection attempt while nothing runs. A request gets 5 s to connect, 30 s to be sent and 120 s for its reply (a tool replies
within its 45 s budget, `wait` within 50 s), so a server that never replies can't hang the agent.
A line that is valid JSON but not one message object, such as a batch, gets an error reply.

## Configuration

The CLI options are in `CLIMiscConfigDef`, so the GUI still starts: `--mcp`, `--mcp-port N`,
`--mcp-token T`, `--mcp-run FILE`, `--mcp-run-out DIR` and `--mcp-stdio`. The AppConfig `app`
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

HTTP is the default because its URL stays the same whichever OrcaSlicer build runs, and nothing
runs on the agent's side: the stdio bridge is a process of its own for each agent session, and it
opens OrcaSlicer when the agent first calls a tool. The cost is that OrcaSlicer must be running with
the server on before the agent connects, and a registration holds the token: a new token (delete
the token file, then turn the server off and on) or a new port means copying the setup again. Stdio suits an agent that should open OrcaSlicer on demand, and apps
that can't connect over HTTP; its setup names this executable, so it breaks when the program moves.

## Testing

Unit tests are Catch2 cases tagged `[Mcp]` in `tests/slic3rutils/test_mcp_*.cpp`, run with
`slic3rutils_tests "[Mcp]"`. `test_mcp_protocol` covers both eras and argument validation;
`test_mcp_http` the endpoint rules and a live server (deadlines, eviction, stop); `test_mcp_token`
who owns the discovery file and when a port counts as held by another window; `test_mcp_core`
`UiInvoker`, `McpCore` and `JobRegistry` on a hand-stepped dispatcher (guards, abandoned calls,
jobs, blocked_by_dialog, admission, slots, idle, journal); `test_mcp_operations` the slice and
export outcomes; `test_mcp_replay` parsing, substitution, matchers and await; `test_mcp_ui` labels,
snapshot text, ref liveness, synthetic key state, a key's way through the hooks of its target's
parents (with a display), how ImGui items are labelled and named, value
matching and the model-target projection and surface check (`CanvasTarget`); `test_mcp_results`
what covers a canvas click (`imgui_window_at()`), the surface meshes of a modifier target, fresh GL
toolbar state and the auto-answer press (with a display), the slice reply's plate summary,
`get_slice_stats`'s feature, layer and object tables, the legend rows' names, the slider's layer for
a height, menu paths and listings, the UI activity hook, the journal's dropped listing clicks and
calls whose job failed or still runs (with a replay of the export), the
menu capture on real wx menus, row paths and their snapshot text, (with a display) the rows of
real trees, lists, checklists and tables, clicks and checkbox input on them, and popups' owning
windows, plate icon names and bed picking ids, an upload's percentage while its bytes go out, and
the `send_gcode` command's catalog entry;
`test_imgui_wrapper` the
ImGui recorder (named items, kinds, values, text, clipping) on frames drawn without OpenGL;
`test_mcp_web` a page's lines and their window pixels, web locators, chords as DOM keys per
platform, the page deadline, the request and install scripts, auto-answer matching and standard
button names, the journal's index across pages and frames, and (with a display) the web flag of a
dialog's description, a web dialog rule's wx buttons and web view handles;
`test_mcp_recording` the Matroska file (read back closed and unclosed), the sink's order, memory
budget, non-blocking push, finish, abort and failed frames, the video transform, overlays and frame
repeats, captions and what they hide, the ffmpeg command and listings, the files recordings and
encodes reserve, and the journal's recording paths; `test_mcp_tools` setting serialization; `test_mcp_config` the object and volume scopes' keys and
effective values, the names and places `save_preset` accepts, enum labels and descriptions, the settings search,
per-variant entries and their writes, preset comparison, `changed`, which unsaved changes a call discarded, which presets
`delete_preset` refuses or deletes with a custom printer, the project scope's slot colours,
flushing multipliers and matrix, and the plate scope's filament orders and layer ranges and
`list_plates`' layer numbers and layer G-codes, the preferences scope's keys, and `generate_calibration_test`'s arguments,
defaults, the dialogs' shared rules, VFA's auto-adjust and the parameters the journal records. End to end, `tools/mcp/orca_mcp.py` launches
the app on a private Xvfb display on Linux (with `-noreset`, and the pointer parked in a corner so no
tooltip lands in a screenshot), or on the desktop on macOS and Windows, and runs
`tools/mcp/smoke_test.json`. `tools/mcp/tests/replay_basic.json`
checks `--mcp-run` headless, `tools/mcp/tests/gizmos.json` every gizmo through the GUI tools,
`tools/mcp/tests/multicolor.json` object and part filaments and the project scope, `tools/mcp/tests/presets.json` saving,
reading and comparing presets, explaining settings, per-variant entries, `changed`, reverting keys, answering the
unsaved-changes prompts of a preset switch and a new project, deleting presets, exporting and importing
presets with a path, and reading a preference changed in the Preferences dialog,
`tools/mcp/tests/plates.json` the plate scope, `list_plates` and the filament slot commands,
`tools/mcp/tests/calibration.json` every calibration test and its preset answers,
`tools/mcp/tests/dialogs_canvas_input.json` auto-answers that run a button's handler and canvas
targets on negative parts and from below the bed, `tools/mcp/tests/slice_stats.json` the slice
reply's statistics on one and two colours, `tools/mcp/tests/preview.json` `get_slice_stats`,
`get_app_state.preview` and `set_view`'s preview arguments, `tools/mcp/tests/context_menus.json` context
and button menus listed and run without one ever showing, `tools/mcp/tests/rows.json` object list
rows, checklist rows, the object settings table and the print button's popup,
`tools/mcp/tests/recording.json` a recording with clicks, a dialog and a slice, stopped and encoded,
and one a replay ends, `tools/mcp/tests/plate_icons.json` plate icons (lock, settings, arrange, the plate menu, move to
front), `tools/mcp/tests/webview_modal.json` JavaScript in the preset bundle dialog and the setup
wizard, `tools/mcp/test_webview_modal.py` the web views `run_javascript` is refused in and a page that
closes its dialog, `tools/mcp/tests/web_dialogs.json` the preset bundle dialog, the colour picker
(hex field and spectrum) and the setup wizard's first steps through the GUI tools alone,
`tools/mcp/test_web_ui.py` every page control kind, action, refusal, locator, timeout and close of
the MCP web fixture plugin's dialog, window, dock panel and configuration UI (side-loaded with
`--install DATADIR`), `tools/mcp/test_set_view_preview.py` the
legend combo's semantics of `set_view {view_type}`, a canvas screenshot straight after a layer or
view type change, and the preview refusals of `set_view`,
`tools/mcp/test_export_running_job.py` an export during and after a job that outlived its reply,
`tools/mcp/test_send_gcode.py` `send_gcode` and `get_app_state.uploads` against a stub print host that
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
sizes, the object structure commands' params, the height ranges `set_height_ranges` accepts (order,
overlaps, bounds, keys), and `set_custom_gcode_at_layer`'s edits of a layer slider built in the test (by layer
and height, in-place edits, refusals that leave the slider as it was, the type names). On the app,
`tools/mcp/tests/scene.json` checks object structure and placement (transform on instances, plates and
parts, what `list_objects` reports of them, the selection `select_objects` and `get_app_state` report, shapes at a
size, arrange and Fill bed, `add_part`, the
object structure commands, then all of them on one scene through to a slice);
`tools/mcp/tests/layers.json` height range modifiers and variable layer height profiles (set, read
back, undone, sliced, refused, kept by a 3MF) and layer G-codes (placed by layer and height, edited in
place, refused, sliced, kept by a 3MF); and `tools/mcp/test_worker_admission.py` admission while an
arrange started three ways runs.
