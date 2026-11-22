# MCP Server

## Why it exists

The MCP server lets an AI agent drive a running OrcaSlicer through the Model Context Protocol.
The same tools serve two audiences. A developer's agent reproduces a GitHub issue: it loads the
files, picks presets, changes settings, clicks through the GUI and slices. It also takes
screenshots before and after a fix and replays the steps from a replay file. A user can drive
OrcaSlicer from Claude Code, Codex, Cursor or Claude Desktop.

The server runs inside the GUI process, because most of what an agent reproduces is GUI behaviour, and
reuses the Speed Dial catalog, so a feature added to the Speed Dial reaches agents, replays and scripts
too. It is off by default. The code is in `src/slic3r/GUI/Mcp/`. The core, which uses no wx (the command
catalog's type, the dispatch core, the hand-off to the UI thread, the job registry, the followed threads, the
journal, the paths, argument checking, the protocol and HTTP transport, what the server says of itself, the
replay model, Action Replay's summary of a replay, the setup a client copies, the stdio bridge and the token),
is the library `libslic3r_automation`, so it is unit-tested without the GUI (see Testing).

```
MCP client ──POST /mcp──▶ McpHttpServer ──▶ McpProtocol ──▶ McpCore ──▶ UiInvoker ──▶ UI thread
stdio client ──▶ orca-slicer --mcp-stdio ──┘                   │
                                                          JobRegistry, Journal
```

Two classes, both owned by `GUI_App`, hold all of it except the bridge. `AutomationRuntime` runs
commands for every front end: the MCP endpoint, Action Replay, the Run Script panel, plugins, a person's
commands from the Speed Dial and the command line's run. It owns the dispatch core (`McpCore`), the
jobs, the journal, the UI invoker, the `ModalTracker`, the recorder and the scopes a run, a plugin or a
client leaves state in. `McpEndpoint` is the MCP server and nothing else: the HTTP server, the protocol,
the request gate, the token, the discovery file and the takeover of a port another window holds. It
holds the runtime by reference and reaches its core through the protocol; the runtime does not know it.
Neither exists until something needs it: the runtime until a front end first asks for it, the endpoint
until the server is first turned on. Until then there is no thread, socket, modal-dialog hook, ImGui
item recording or synthetic input state. The commands the runtime runs are the process's command catalog
(`NativeCommands::instance()`, see Tool catalog), which startup builds without either.

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
over the content blocks would also drop the images and `widget_list`'s tree. Tool failures, invalid
arguments included, are `isError` results the model can act on; an unknown tool is a JSON-RPC
`-32602` error naming close matches, or the catalog that does have the name (see Tool catalog).
`validate_arguments()` checks arguments against the tool's schema first. Before it,
`normalize_arguments()` reads what agents commonly send in place of the schema's form, for calls and
replay steps alike, so the journal records the schema's form. An argument under a name the tool does
not take is read as one it does, when the call leaves that one out and the value fits it: `ref` as `target`, `key` as `keys`, `text`
as `value`, `action` as `id` or `button`, `object` as `objects`, `plate` as `plates`, `filter` as
`query`, `settings` as `values`, and `path`
as `save_path` where the tool has no `path` of its own. A value in another JSON form is converted: a
number, boolean, array or object written as a string, an index as a string where a name may also go
(`"0"` for a plate), a single item for an array, also where null may stand for the array. A
value outside its argument's enum moves to the one other argument whose enum takes it, when the call
leaves that out: `view_set`'s `canvas: "front"` is its `camera`. Each enum lists only its own values,
since listing a camera direction under both had agents give it to the wrong one. Anything that still
does not fit is left for the validator to report. The bench's transcripts are where these come from:
each was an agent's first guess, many times over, and each cost a failed call and a retry.

The server `instructions` (under the 4096 characters clients keep) give the workflow: `app_state`,
act, then check the result with the reading commands, keeping screenshots for what those do not report.
The rest is in three sections, by what the commands are for: running the session, acting on project data,
which an agent should prefer, and driving the GUI itself.
They say to prefer the commands that act on project data over clicks and never retry a call that returned a
job, that commands are named place first, then verb, and `tool_search {group}` lists a place's, that a call
with exactly a Speed Dial action's arguments may ask as a person's pick does while any other runs from its
arguments, point to `settings_get {query}` and its meta for what a setting does, say how a gizmo opens and is
driven (and that `gizmo_pick_file` only stands in for a file picker inside one, since its name draws agents to
it for any gizmo work), that the camera turns with `view_set` rather than a drag, and which indices are 0-based.
They also point to `tool_search {query}` as the one search across every catalog an agent calls into:
the commands, among them those `tools/list` never shows (the three menu items that take no arguments, the
Design tab's and the operations plugins offer, so nothing else tells an agent they exist), the Speed Dial's
other actions, settings and the Python API. An agent rarely
knows which catalog answers "how do I add a brim", and each catalog has its own search with its own
arguments. Clients that defer tools load only their names and the
instructions at first, so the instructions are how an agent without a skill learns which tool to
load.

## Security, token and discovery

The bearer token is 32 random bytes as 64 hex characters in `<datadir>/mcp/token`: directory 0700
and file created 0600 with `O_CREAT|O_EXCL` on POSIX, the per-user ACL of `%APPDATA%` on Windows.
It stays out of AppConfig, whose writes and backups use default permissions. `ORCA_MCP_TOKEN` overrides
it for one run, and stays out of the process list; `app_state.mcp.token_source` (`file` or
`ORCA_MCP_TOKEN`) says which is in force. Every start of the endpoint reads the file, so deleting it and
turning the server off and on in Preferences issues a new token. While
serving, `<datadir>/mcp/server.json` holds `{url, port, pid, version, token}` with the token in
use, so the stdio bridge and the harness connect without the launch environment. It is written
owner-only via an exclusive temporary file and a rename (`write_private_file()`), and removed on
stop, but only by the process its `pid` names: windows that share a configuration folder share the
file, and one that stops must not hide another's server from the stdio bridge.

## Lifecycle

`GUI_App::on_init_inner()` opens the endpoint once AppConfig is loaded when `--mcp`, `--mcp-port` or the
`mcp_enabled` preference asks for it (`GUI_App::open_mcp_endpoint()`, which starts the runtime first). That is
before the first startup dialog, such as the TLS prompt or the setup wizard, so an agent can answer those
dialogs. `--run` alone starts the runtime without the endpoint, and so do Action Replay, the Run
Script panel, a plugin's tool call and a person's recording or long command when the server is off. Each
front end gets the runtime from `GUI_App::automation()`, which starts it on first use and returns
nullptr while OrcaSlicer closes or the runtime stops; `automation_if_started()` returns it, stopping or
not, and never starts it. `app_state.ready` stays false until `post_init()` calls `mark_ready()`,
the Prepare canvas has loaded OpenGL and the main window has built its tab pages, and tools that need
the main window fail with "still starting". Starting on Home, the canvas loads OpenGL at idle after
`post_init()` (MainFrame's GL prebuild), and before that a 3MF save would draw its thumbnail with no GL
functions loaded. The same idle queue builds the tab pages (`MainFrame::prebuild_pending()`); a page's
web view registers its `wx` script message handler with a script it waits for in a nested loop, and on
GTK calls that came in meanwhile have left that wait unfinished for good, so that no web view made
afterwards gets `window.wx` and a dialog page's own buttons do nothing. So `mark_ready()` polls every
100 ms until both are done. A hidden canvas that can't get a GL context loads OpenGL on its first draw
instead, and the pages build only while no input arrives, so after 10 s ready no longer waits for
either. Becoming ready also starts the `--run` file, which `start_launch_run()` registers with
`on_ready()`. Notifications don't depend on this: one pushed or updated before the first ImGui frame has
built the font waits for its first render to measure its text.

A front end asks for what it uses. `AutomationRuntime::start()` brings up the dispatch core, the jobs,
the UI invoker, the modal tracker, the recorder and the list of presses held on web pages: all that a
person's recording or a long command a person picked from the Speed Dial needs, the recorder drawing the
modal dialogs from the tracker's stack. The GUI instruments are the UI inspector behind `widget_list`
and the input tools, the menu capture, ImGui item recording and, on Wayland, the window-state hooks
(`track_window_states()`). `enable_gui_instruments()` turns them on for the front ends that drive the
GUI: `McpEndpoint::open()` when it opens, the command line's run (`start_launch_run()`) and a plugin's
call as the UI thread admits it. A person's pick from the Speed Dial (`PersonContext`) does not. The log
reads `Automation runtime started` as the runtime starts and `Automation GUI instruments on` when they go
on, so a session that began with a person's recording shows the second line only once something drives
the GUI. ImGui items are recorded as a frame draws them, so turning the instruments on asks the shown 3D
view for a frame. `AutomationRuntime::inspector()` turns them on itself if they are off, so a command
never finds the inspector missing.

The instruments cost every frame, every popup menu and, on Wayland, every widget's visibility and focus
signals, so they are on only while something drives the GUI: the client scope is open (the endpoint
serves), a replay, a script's run or session, or a plugin has its scope open, or a window holds them.
Action Replay holds them while it is open and the Run Script panel while it is wanted on screen
(`hold_gui_instruments()`, `release_gui_instruments()`), so that they stay on between a window's runs and
its next run finds the 3D view's ImGui items recorded. When the last of these goes (the endpoint closes,
a run ends, a plugin unloads, a window lets go) the instruments come off, and the log reads `Automation
GUI instruments off`: the menu capture and its idle handler are removed, ImGui recording stops and drops
what it recorded, and the window-state hooks are removed with what they noted. The inspector itself stays,
with its refs, for a call that still holds it. A scope that begins while they are off turns them on again.
Scopes begin and end on job threads, so those changes are queued to the UI thread, where the state is read
again as each one runs.

Both stop, idempotently, at the start of `MainFrame::shutdown()` when the app closes (before panels are
destroyed) and on `wxEVT_END_SESSION` (after which macOS calls `exit()`): `GUI_App::stop_automation()`
closes the endpoint, then stops the runtime, and does nothing for one that was never made.
`McpEndpoint::close()` closes the socket, stops the takeover retries, removes the discovery file and
closes the runtime's client scope; requests in flight run on. `AutomationRuntime::stop()` wakes every
waiting call with "shutting down" (`UiInvoker::shutdown()`, `JobRegistry::wake_all()`), refuses new
calls, joins the threads of the followed calls (Threading), drops every auto-answer rule and, when the
GUI instruments are on, takes them down; nothing turns them on again. Neither frees anything: a call may still be on the UI
thread's stack inside a nested event loop, using the inspector and the modal tracker, and job and
request threads may still be returning. A stopped runtime never starts again.

`GUI_App` holds the runtime in an `AutomationRuntimePtr`, whose deleter (`AutomationRuntimeDeleter`) is
the only way to destroy one: the destructor is private. The deleter stops the runtime, waits with
`AutomationRuntime::drain()` until no lease is held and no call or job thread is inside the core, and
frees it; a call that never returns would make freeing a crash, so after 10 s the runtime is left in
place instead, and the log says "Automation runtime: a call did not return; leaving it in place". The
endpoint is held in a `std::unique_ptr` declared after the runtime, so it is destroyed first whichever
way `GUI_App` goes. `GUI_App::destroy_automation()` runs from `OnExit()`, and from `CleanUp()` after a
failed initialization, outside any event loop, so no closure can be running: it drops the Run Script
panel, calls `stop_automation()`, then waits up to 10 s with `McpEndpoint::drain()` for the request
threads to leave the protocol. HTTP request threads reach the protocol through a shared `RequestGate`,
which `drain()` closes, so a late request gets 503 rather than a freed object. When a request is still
inside after 10 s, both are left in place, since the protocol reaches the runtime's core, and the log
says so; otherwise the endpoint is freed, then the runtime goes through its deleter. `McpCore`'s own
destructor waits for every call and job thread to leave, however long that takes; its owner drains
first, so it returns at once.

A language change runs `MainFrame::shutdown()` from `recreate_GUI()`, which only suspends the
runtime: the endpoint stays open, the UI generation is bumped so every GUI ref goes stale, UI calls
fail with a request to retry, and `ready` is false until `recreate_GUI()` resumes it. A UI closure
posted before the rebuild refuses to run after it ("not run, safe to retry").

Several OrcaSlicer windows may run at once, and those that share a configuration folder share the
`mcp_enabled` preference and the port. The bind is exclusive (`SO_REUSEADDR` stays off on Windows, and
POSIX refuses a second listener), so the first window to start serves and agents, over HTTP or stdio,
reach it. A later window that wants to serve finds the port taken by the process the discovery file
names (`discovery_held_by_other()`): it reports no error, Preferences says another window is serving
agents, and the endpoint tries the port every 3 s, so when the serving window closes, one of the others
takes over and writes the discovery file. A window that cannot bind for any other reason reports it and
stops trying: a retry that fails otherwise is shown as a notification, through the function `GUI_App`
builds the endpoint with. Windows meant to serve side by side need their own configuration folders
(`--datadir`) or `--mcp-port`; agents then pick one by its URL, or the stdio bridge by `--datadir`. The
server makes no attempt to route between windows.

Preferences turns the server on with `GUI_App::open_mcp_endpoint()` and off with
`GUI_App::close_mcp_endpoint()`, which closes only the endpoint. The runtime, running jobs and a launch
run carry on, because the call that opened Preferences may itself be an MCP call still on the stack, and
turning the server on again opens the same endpoint, with the protocol it had, on the same port.
Preferences reads the endpoint through `GUI_App::mcp_endpoint()`, and `app_state.mcp` is the
endpoint's `describe()`: whether it serves and, while it does, its port, URL, token source, token file
and discovery file; in a session that never opened it, `{serving: false}`. The port field shows the port
in use, `--mcp-port`'s included, and only an edit of it is saved and reopens the endpoint, through
`GUI_App::restart_mcp_endpoint()`: the field also commits when focus merely passes through it. A port
set in Preferences replaces `--mcp-port` for the rest of the session, also while the server is off.
Closing the endpoint closes the runtime's client scope (`close_client_scope()`): a client's auto-answer
rules, recording and held presses end, and a client's rules that a call still running registers
afterwards are refused (`client_scope_open()`), so a person using the app afterwards never has a prompt
answered for them. Closing doesn't wait for in-flight requests, which may be waiting on the UI thread
that closes it; their replies are dropped. A bind failure is logged, shown as a notification and in
Preferences, and opening the endpoint from Preferences never reruns a `--run` file. The modal
tracker is created with the runtime and picks up modal dialogs that are already open, such as
Preferences when the server is turned on from there.

## Threading

Tool handlers never run on the UI thread. Anything that touches wx goes through
`ToolContext::run_ui()`. A command a person picks is the one call that runs there: its context, a
`PersonContext`, runs the closure in place (see Speed Dial catalog). For every other call `run_ui()`
posts a closure through `UiInvoker` to a `UiDispatcher`, then waits. The dispatcher is
`wxApp::CallAfter` in the app and a queue the test drains in tests. A closure
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
failed asks it: a `poll_ui()` timeout, a blank canvas capture and a row `widget_click` can't place say the
window isn't being drawn when it isn't.

`GuardClass` decides what may run when:

| Class | Tools | Runs behind a modal or inside another call |
|---|---|---|
| `Inspect` | app_state, app_wait, app_log, the searches and lists (tool_search, action_search, object_list, plate_list, preset_list, settings_list, printer_list_models, plugin_list, window_list), the reads (settings_get, plate_get_settings, project_get_settings, preferences_get, plate_slice_stats, script_search_api), window_screenshot, widget_list, replay_export, recording_start, recording_stop, recording_encode | yes |
| `Recover` | dialog_answer, dialog_auto_answer, job_cancel, GUI input aimed at the topmost modal, window_set | yes |
| `Mutate` | everything else | no |

The closure checks the guards on the UI thread, since the app can change between posting and
running. A closing app or a stopped runtime fails the call with "shutting down". A frame being
recreated fails it with "retry". A mutate call is refused while a modal dialog is open, and the
refusal lists the dialogs. It is also refused when it would run inside another UI call's nested
event loop: a modal, `ProgressDialog::Update` or `wxYield`. The GUI input tools (`widget_click`,
`widget_set_value`, `keyboard_press`) and `dialog_answer` run as recover calls. Once a target resolves outside the
topmost modal, they apply the mutate checks themselves (`UiInvoker::require_mutate()`), along with
operation admission (`McpCore::admit()`). `job_cancel` is a recover call too, so neither a dialog the
operation opened nor another call's nested loop keeps it out.

`NativeCommand::inline_call` decides where a call runs. Pure observation tools run on the request
thread. Every tool that can activate UI runs as a job on its own thread, recover tools that click
in dialogs included, since a click can open another modal (Preferences, Keyboard shortcuts, Edit).
`JobRegistry` names jobs `j1`, `j2`, …, records each job's tool and parent, allows 64 running and
keeps the last 64 results, plus finished jobs whose parent (a running replay) may still wait for
them, up to 1024 in all. The request thread waits up to `NativeCommand::budget_ms` (45 s, under
Codex's 60 s tool timeout); after that the reply is `{status: "running", job, tool, note}` and the
job keeps going until `app_wait {for: "job"}` (at most 50 s per call) returns its result. The server
never retries a mutating call, and every envelope tells the model not to either. The environment
variable `ORCA_MCP_REPLY_BUDGET_MS`, read when the runtime starts and logged when set, caps every
command's budget at that many milliseconds; it only lowers a budget, never raises one, and exists for
tests that need a call to outlive its budget without waiting 45 s for it.

**blocked_by_dialog.** A handler that opens a modal blocks in its nested loop, so every 1.5 s the
request thread posts a probe closure, which runs inside a wx dialog's loop and returns the
`ModalTracker` snapshot as JSON; no `wxDialog*` crosses threads. With a modal open, the reply is
`{status: "blocked_by_dialog", job, dialogs}`, once a second probe, posted at once, finds the same dialog on top:
an auto-answer rule presses a wx dialog's button from a closure queued as the dialog opens, which a probe posted
before the dialog opened runs ahead of, and the second probe runs after it. A wx dialog that press ended is
hidden but stays on top until its modal loop returns, which on a busy machine can come after the second probe,
so a hidden wx dialog on top holds no call. Its note says how to answer the topmost dialog:
`dialog_answer` for a wx dialog or a native message box whose buttons are listed, a person for any
other dialog marked `native: true` (a picker, or any native dialog on macOS), which `dialog_answer`
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

**Followed calls.** A run a person starts (Action Replay, the Run Script panel, `--run`) and the encode
of a person's recording once it ends outlive any reply, so the runtime follows each on a thread of its own:
`AutomationRuntime::follow()` takes a `FollowedCall`, calls its command and waits past a `running` or
`blocked_by_dialog` reply for the job's result. Its `ready` runs first on that thread and may complete the
arguments: the encode waits there for the recording's file, and `--run` reads its file there, off the UI
thread. `ended` gets the result; while the runtime stops, that is an error ("OrcaSlicer is shutting down") when
the call was refused or cut short within its reply budget, and nothing when the runtime stopped while the call's
job was being waited for. `stop` lets `stop_followed()` end
the call early; `suspend()` and `stop()` call it first, while the main window the runs act on is still the
current one. The threads are a `FollowedThreads`: a start joins the threads whose work has returned, and
`stop()` joins the rest once it has woken their waits. `follow()` may be called from any thread. A start and
the join take one lock, and the join refuses every start after it, so a call followed while the runtime stops
is refused or joined, never left running.

**The person mark.** A job a person's run makes is created with the person mark (`McpCore::call_person()`,
and `McpCore::submit()` for a long command a person picks), and the mark counts while the job is unfinished:
`JobRegistry::person_in()` finds it in a call's lineage, so a script under the run runs as the person's
(Identity, under Python scripts). An agent's call never has it, a call made right after a person's run
included.

### Operations

Slicing and G-code export finish in the background process. Their handlers call
`ToolContext::begin_operation()` in a short UI closure before starting the work; that fails if
another operation runs. An export starts in that same closure. A slice first waits for the
background update that a change made just before has scheduled, which validates the new state, and
then requests the slice. A `job_cancel` that comes while it waits moves on `Plater::slice_cancel_seq()`,
so the slice never starts and reports its plates `cancelled`. `poll_ui()` then checks on the work
through `observe_ui()` every 200 ms, so a dialog that opens meanwhile doesn't stop the watch, until
the work ends or the deadline passes. The deadline is `plate_slice`'s `timeout_s` (default 30 minutes)
for a slice and 10 minutes for an export.

**Worker jobs.** Arrange, orient, Fill bed and the jobs of the Text, SVG and texture gizmos run on
the Plater's UI job worker, whatever started them: `object_arrange` or `object_auto_orient`, called or
run as a Speed Dial action, a GL toolbar item, an ImGui button such as the Arrange options panel's, or a gizmo
control. `McpCore`
watches that worker through `WorkerHooks` (busy and cancel, both on the UI thread). A call that finds
the worker idle at the start of one of its UI closures and busy at the end of that closure, or of a
later one (a toolbar event or a render queued by the call starts the job between closures), has
started a worker job, and `McpCore::claim_worker()` makes that job the call's operation. The call's
own job then ends only once the worker is idle again, its finalization included, since the job holds
`ModelInstance` pointers until then. (`Worker::is_idle()` reads true from the moment the worker takes
the finalize message, so a dialog finalization opens, such as an arrange's geometry error, ends the
hold early; the modal guard keeps changes out until it closes.) Its reply waits within the reply budget and
then becomes a running job, as for any call. `job_cancel` (or the call's own timeout, 30 minutes unless
the tool sets a shorter one, as `object_arrange` does) asks the job to stop, but `Worker::cancel()` does not
wait, so the call keeps the operation until the worker is idle and then fails as cancelled. `job_cancel`
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
`allowed_during_operation` tools (`job_cancel`) still run; the GUI input tools apply the same check once
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
that. `job_cancel` also drops a pending automatic reslice: it stops the delay timer,
whose callback wx then never delivers, and moves on a generation that a zero-delay reslice already
queued with `CallAfter` checks. An export is numbered in `export_seq` once scheduled, so an
unchanged number means it never started; otherwise its `ExportRecord` ends `ok`, `failed`,
`cancelled` or `superseded`. A G-code export with a path is refused while slicing runs. Without a
path, `project_export {format: "gcode"}` is its Speed Dial action's call and asks in a native file
dialog, which a caller answers only with a `dialog_auto_answer` rule registered first. The
decision logic is in `Operations/RunStatus`, which uses neither wx nor `Plater` (see The operations
layer).

**Idle.** `app_wait {for: "idle"}` holds when no modal is open, nothing slices (`Plater::is_slicing_any()`:
the background process, Slice All, a scheduled background update or a pending automatic reslice)
and the UI job worker (arrange, orient) is idle, and no gizmo on any canvas reports
`GLGizmoBase::is_busy()`: work a gizmo started in the background whose result it has yet to show
(Simplify's calculation until `worker_finished()` has run, Texture Displacement's adjustment
preview). It also needs no job running other than the caller and its ancestors,
so a replay or script waiting for idle doesn't wait on itself. Only the job registry, the `ModalTracker`
snapshot and the journal are shared between threads, each behind a mutex. All app state is read on
the UI thread.

## The operations layer

Three interfaces ask the app to do things by name: the Speed Dial's actions, the MCP commands, and the
plugin API (`orca.host`). What one of them does is often what another does too: exporting G-code is a
Speed Dial action, a call of `project_export` and the action run through `action_run`. The checks, the
refusal and its wording, the undo snapshot and the order of the owner calls of such a function are in one
place, `src/slic3r/GUI/Operations/` (namespace `Slic3r::GUI::Ops`), and the three interfaces convert their
arguments and results around it. Where a dialog does the same work (the Clone dialog, the Preferences
dialog, the Plugins window, the Printer Selection and System Filaments dialogs, the AMS sync dialog, Fix
Model), the dialog calls the operation too, so a command and the dialog cannot differ. A command keeps
its key, arguments and reply, and a binding its signature; what they share is the rule.

**What is an operation.** An effect, or a rule about one owner's state that its own widgets also
apply, is a public method of the owner (`Plater`, `Sidebar`, `Tab`, `GCodeViewer`) or a function in
its header: `Tab::can_delete_preset()` is what the tab's Delete and the `preset_delete` command both
ask, and `locked_save_location()` what the Save preset dialog and `preset_save` both follow. What an
automation caller invokes by name is an operation, for every function that more than one interface
reaches and for every function a new command adds. A caller calls an operation to change something
and an owner to read. A tool that changes something through a function with one entry point is left
as it is: nothing can drift there until a second entry point appears, and then the rule applies to it.

**Shape.** An operation is a free function, `Outcome<Result> op(const Args&)`. `Args` and `Result`
are plain structs of bool, int, double, string, path, enum, optional and vector, with fields named
as the tool's arguments are: no JSON, no `wxString`, no pointer to a GUI object. An `Outcome` holds
the result or a `Refusal`:

| Field | What it holds |
|---|---|
| `kind` | `Precondition` (the caller can change something and call again), `Busy` (the same call works later) or `Failed` (it was tried and did not happen) |
| `code` | a stable id, such as `slicing_in_progress` |
| `msgid`, `args` | one English message, marked with `L()`, and the values of its `%1%` placeholders |
| `names` | names that help the caller: the candidates of an ambiguous name, the closest names to an unknown one |

The Speed Dial shows `translated()`. A tool and a Python exception carry `english()`, and a tool's
error also carries the code (`code`) and puts the names under the key it has always used (`similar`,
`presets`). `action_run` replies with the English text and the code too, where the command ran an
operation, so a command's refusal reads the same through every route. A caller may add a hint after
the text (the `project_export` tool's `app_wait {for:"idle"}`), never a second wording. A check of an argument's
form (a path that does not resolve, an argument its schema refuses, an extension the tool does not
write) stays with the caller that owns the argument.

**Undo.** An operation takes the undo snapshot its function takes, once, after its checks pass. A
tool may still make one call one undo step with `Plater::SingleSnapshot`, as `settings_set` does;
that is the tool's own contract.

**Threads and waits.** An operation runs on the UI thread and returns at once. It does not marshal
and does not wait for background work: it returns a ticket, and a status function, itself an
operation, says how the work stands (`start_gcode_export()` and `gcode_export_state()`;
`start_slice()` with `slice_status()` of `Operations/RunStatus`). The caller owns the loop, the
timeout and the cancel, and reaches the status function through its own way to the UI thread:
`ctx.run_ui()` and `poll_ui()` for a tool, nothing for the Speed Dial, `run_on_ui_thread()` for Python.
An operation adds no guard and refuses on the state of the project only; each interface keeps its
own guards.

**Dialogs.** An operation asks nothing of its own. An answer the owner lets a caller give is a field
of `Args`: whether unsaved project changes are refused or dropped (`UnsavedChanges`), whether a
preset's unsaved changes are transferred or dropped (`PresetChanges`). An owner's effect can still
open a dialog of its own (a STEP file's mesh options, the modified-presets question of
`Plater::load_project()`, the Transfer-or-discard prompt of `Tab::select_preset()` when no answer is
given). A call that stops there behaves as any call behind a dialog: a tool replies
`blocked_by_dialog`, and a plugin's call waits for the person. A path that asks a person keeps its
owner method: a command's person's form (a call with exactly one of its actions' arguments) is the asking
path and calls the owner.

**The plugin API.** Where an operation exists for what a binding does, the binding calls it and
takes its refusals, raised as the exception type the binding has always raised for that case. Its
way to the UI thread stays its own. `orca.host.preset.process` and `.printer`'s
`begin_update(preset_name)` resolve and select through `resolve_preset()` and `select_preset()`,
and `orca.host.preset.filament.begin_update(slot_number, preset_name)` resolves and assigns through
`resolve_preset()` and `assign_filament_preset()`, with one step of its own between the two: it
makes the filament tab edit the preset, since its transaction edits that preset.

**What the layer includes.** Owner headers, libslic3r and wx. Nothing from `GUI/Mcp`,
`NativeCommands.hpp` or `plugin/`, and neither nlohmann/json nor pybind11. The three interfaces
include it, and GUI code may. The dispatch core (`McpCore`, `CommandCatalog`, `JobRegistry`) includes
nothing from it, and `plugin/` includes nothing from `GUI/Mcp`.

| File | Operations | Reached by |
|---|---|---|
| `ExportOps` | `start_gcode_export`, `gcode_export_state` | `project_export` with `format` gcode |
| | `export_sliced_3mf`, `export_model_3mf`, `export_stl`, `export_toolpaths_obj` | `project_export` with the other formats |
| `ProjectOps` | `import_models`, `open_project`, `new_project`, `save_project_as` | `project_import`, `project_open`, `project_new`, `project_save` |
| | `set_instance_count` | `object_set_instances` |
| `ObjectOps` | `add_primitive`, `delete_objects`, `rename_object`, `separate_instances`, `split_object`, `assemble_objects` | `object_add_primitive`, `object_delete`, `object_rename` (and `object_add_primitive`'s `name`), `object_separate_instances`, `object_split`, `object_assemble` |
| | `order_objects`, `reorder_objects` | `object_order` with `by`, and with `order`, which makes the drops `ObjectList::OnDrop()` makes for a drag in the object list |
| | `drop_objects`, `mirror_objects`, `center_objects`, `scale_to_fit`, `orient_objects` | `object_drop`, `object_mirror`, `object_center`, `object_scale_to_fit`, `object_auto_orient` |
| | `set_objects_printable`, `set_objects_auto_drop` | `object_set_printable` (the object list's Printable cell sets the flag through the same `set_printable()`); `object_set_auto_drop` and the 3D view's Auto Drop |
| | `fix_objects`, `clone_objects`, `reload_parts` | `object_fix` and Fix Model; `object_clone` and the Clone dialog; `object_reload_from_disk` |
| | `change_part_type`, `replace_part` | `part_change_type`, `part_replace` |
| `PlateOps` | `select_plate` | `plate_select`, `plate_rename`; the exports of `ExportOps` that write their file at once, for a plate their caller names |
| | `set_plate_locked`, `delete_plate`, `duplicate_plate`, `rename_plate` | `plate_lock`, `plate_delete`, `plate_duplicate`, `plate_rename` |
| | `start_arrange` | `object_arrange` |
| | `start_slice` | `plate_slice` |
| | `show_layer_range` | `view_set`'s layer arguments and its Go to layer action |
| | `show_prepare_view`, `shown_prepare_canvas` | the commands that act on the Prepare view's selection: its canvas, shown and with its scene loaded |
| `PresetOps` | `resolve_preset`, `select_preset` | `preset_select`, the plugin API's `begin_update(preset_name)` |
| | `assign_filament_preset` | `preset_select`, the plugin API's `begin_update(slot_number, preset_name)` (the sidebar's selector calls the same owner effect, `Sidebar::select_filament_preset()`) |
| | `import_presets`, `sync_presets` | `preset_import`, `preset_sync` |
| | `add_filament_slot`, `delete_filament_slot`, `add_mixed_filament`, `edit_mixed_filament` | `filament_add`, `filament_delete`, `filament_add_mixed`, `filament_edit_mixed` |
| | `sync_filaments_from_ams` | `filament_sync_ams`; the AMS sync dialog applies its choice through the same `apply_ams_sync()` |
| `InstallOps` | `printer_models`, `install_printers`, `uninstall_printers` | `printer_list_models`, `printer_install`, `printer_uninstall` |
| | `install_filaments`, `uninstall_filaments` | `filament_install`, `filament_uninstall` |
| | `printer_profile_data`, `apply_install_set` | the Printer Selection and System Filaments dialogs, whose Confirm applies the installed set as the commands do |
| `PreferenceOps` | `preferences`, `set_preferences`, `apply_preference` | the table `preferences_get` lists and the Preferences dialog builds its rows from; `preferences_set`; each control's effect, run by the dialog and by `preferences_set` |
| `PluginOps` | `set_plugin_enabled`, `uninstall_plugin`, `reload_plugin` | `plugin_enable`, `plugin_uninstall`, `plugin_reload`, and the Plugins window's switches and row menu |
| | `start_plugin_refresh` | `plugin_refresh` and Refresh Plugins |
| `GizmoOps` | `open_gizmo` | `object_paint`, `object_measure`, `object_simplify`, `part_boolean`, and `object_cut`, `object_lay_on_face` and `object_set_brim_ears` called with only their target |
| | `cut_object`, `lay_on_face`, `set_brim_ears` | `object_cut`, `object_lay_on_face`, `object_set_brim_ears` with their acting arguments: what the gizmo's Perform cut, a click on a face and its stored ears do, through the gizmo's own functions |
| `PrinterOps` | `open_print_dialog` | `print_start`, `printer_send` and its Speed Dial action, which run the Print button's own handler for the route the button offers |
| | `test_print_host` | `printer_test_host`, the call the connection dialog's Test button makes |
| | `send_job_command` | `print_pause`, `print_resume`, `print_stop`, the commands of the Device tab's buttons |
| `RunStatus` | `slice_status`, `export_status`, `classify_plates` | the status rules of a slice and an export, free of wx and `Plater` |

**What every route gets.** An operation holds what every route of its function does, so these are
the same whichever interface asks:

- A project is not saved under a name ending in `.gcode.3mf`, which is a sliced file's name, since a
  project file holds no G-code.
- An STL, a sliced file and G-code need objects. A `.3mf` of the model does not: a project that
  holds only a Design-tab recipe has none.
- An arrangement shows the Prepare view first, runs with a gizmo open as the toolbar's does, and is
  refused while the job worker is busy.
- A slice is requested as the Slice button requests it, with no check of the printer's technology.
- A mixed-colour filament slot is refused as one, since it holds no preset of its own.
- A hidden preset that suits the printer is shown in the selectors and then selected.
- A printer or process selection that leaves another preset selected is a refusal, and that preset
  stays selected.
- A layer asked of a Preview that has none is a refusal: the layer slider has a range from the
  start, so what counts is whether a print gave it layers (`Ops::layer_count()`). It keeps the last
  slice's layers, so a plate that is no longer sliced can still have layers to show.

A check only one route makes stays with that route, as a step of its own before the call:

- `object_arrange` refuses a locked or an empty plate it was given by index; its Speed Dial action names no plate.
  Fill bed is a call's alone.
- The plugin API refuses a filament slot whose preset is empty or does not resolve, since its
  transaction edits that preset; `preset_select` only assigns one.

**Keeping it so.** Two checks, neither of which reads what a handler does:

- `NativeCommand::operations` names the operations a command reaches (`reaching()` where it is
  registered), and the command reference lists them. A unit test (`test_operations`) walks
  `NativeCommands::instance()` and requires every command outside the Design tab that is not read-only
  to name an operation or to be on the test's one exception list (`kExceptions`), with the reason: it
  runs what its arguments name, it drives the GUI, it changes the session and not the project, its
  function has one entry point, it runs its menu item's function (which adds a shape, opens a window or
  changes a setting of the app), or it calls the undo and redo of a history (`undo`, `redo`). An entry
  that names operations fails the test, so a new command has to say which it is.
- `scripts/check_operation_calls.py`, run by the `Check operation calls` workflow, bans in the callers' files
  (`NativeCommands.cpp`, the `McpTools*.cpp` files but the Design commands' `McpToolsCad.cpp`,
  `plugin/host/*.cpp`) the owner methods the operations call, by name. A banned call that stays, because
  another function uses the same owner method or the caller is an asking path, is listed in
  `scripts/operation_calls_baseline.txt` by file and exact source line, under its reason. The check fails on a
  banned call that is not listed and on a listed line the source no longer has.

Neither catches a rule written again through calls that are not banned, such as a check made of
getters. That is for review: a caller's handler converts, calls, and shapes the reply.

## Tool catalog

The command set, how its commands are named and grouped, what each does and takes, which menu items call it and
which calls open a dialog or a tool, is in [mcp-tools.md](mcp-tools.md), with the Design tab's commands in a
section of their own. A command's group is the heading of its place, the first word of its name, and
`tools/list` gives the places in the order that document lists them. Every command that runs from its
arguments is `listed`, so `tools/list` shows it. The menu items that take no arguments are not: `tool_search`
finds them and `tool_call` runs them, as it does the Design tab's commands, which no listing shows.

The commands are the process's command catalog (`CommandCatalog`, `NativeCommands::instance()`). Startup
builds it, with no automation runtime: `register_all_commands()` gathers each family of commands in a fixed
order of calls (app, api, project, config, preference, install, scene, layer, view, plugin, replay, python,
record, action, ui), each through its `register_*_tools(catalog)` in its `McpTools*.cpp`, gives each the
group of its place (`NativeCommands::place_heading()`, which reads the name's first word; a name with no
place throws), and sorts them stably by place: `tool_search`, `tool_call` and `app_state` first, as what an
agent starts with, then the places in the order above, each place's commands in their families' order. The
Design tab's commands come before Driving the GUI, in a CAD build with the CAD feature on
(`register_cad_commands()`; see Design tab), and the source of the operations plugins offer is added beside
them. A command is made with `inspect_tool()`, `mutate_tool()`, `record_tool()` or `recover_input_tool()`,
which build it through `long_command()` with its function unchanged, so it runs where its call does, or, for
one made from menu items, with `menu_command()`, which builds it through `ui_command()` with the items as its
actions; a menu item's function is in namespace `MenuItems` (`NativeCommandMenuItems.hpp`), beside the helpers
the items share in `NativeCommands.cpp`. `reaching()` names the operations a command reaches. A function
reaches the automation runtime through its call's context, `ToolContext::runtime()`: the core gives the
runtime that owns it (`McpCore::set_runtime()`), a person's pick starts the runtime without the endpoint, and
a core built on its own, as in a unit test, refuses, which ends the call in an error. Every key is unique
across the catalog.

Every command is called by its key: `tools/call`, `tool_call`, a replay step and a script's call reach every
command alike and journal it as its own call. `tools/list` and the editors' tool list
(`editor_tool_catalog()`) read the listed commands, in the catalog's order. `tool_search` reads every
command, then the plugin operations of the catalog's source (`search_commands()`; see Plugins and the tools),
so its one inventory finds an unlisted menu item's key or a Design command's as it finds a listed one's.

A command's actions, most of them its menu items, are what a person picks in the Speed Dial to run it (see
Speed Dial catalog). A call whose arguments are exactly those of one of its actions, a command's own action
being `{}`, is the person's form: it does what a person's pick of that action does, so it may ask in a dialog,
which an agent's call replies as `blocked_by_dialog`, or act on the selection. Any other call runs from its
arguments and opens nothing, and where a command's own function needs nothing more than an action gives,
`silent: true` (`Mcp::with_silent()`) runs it from its arguments without asking. So `project_export
{format: "gcode"}` asks for the file and `project_export {format: "gcode", path}` writes it;
`object_add_primitive {shape: "cube"}` asks for the size and `{shape: "cube", silent: true}` takes the built-in
one. The journal keeps `silent`, so a replay of such a call asks nothing either. A call of the person's form is
refused while a modal dialog is open or another call's nested loop runs (`UiInvoker::require_mutate()`), also
for a command whose guard lets it run behind dialogs, unless that form opens nothing: `recording_stop {}` stops
the recording wherever the command runs. `project_save`'s `path` is required in every call that is not one of
its actions', under `then`; `recording_start` given arguments records for up to an hour unless `max_seconds`
says otherwise, and its reply says the limit it runs under.

Two things have no silent form, so the caller answers their dialogs. `preset_export` always opens the
Export Preset Bundle dialog, on the folder `path` names when one is given. And the questions the app's
loader asks while it reads a file (a STEP file's mesh options, the AMF and CAD 3MF options, the names
clean-up, inches or metres, one object of several parts) open for `project_import`, `part_add` and
`part_replace` with a path as they do for a person: those calls open no file dialog, and where the loader
asks they reply `blocked_by_dialog`.

**Targets and the selection.** A command that acts on objects, instances or parts takes them in its
arguments, read by one resolver (`Mcp::resolve_targets()`, `McpTargets.hpp`): `objects`, `instances`
(`[[object, instance], ...]`) and `parts` (`[[object, part], ...]`), or for a command of one target
`object`, `instance` and `part`, each beside its expected ids (below). Every index is checked before
anything changes, and naming one target two ways, an instance or a part without its object, and empty lists
are refused. With none of them the targets are what is selected in the Prepare view
(`Ops::shown_prepare_canvas()` shows that view and loads its scene first): every instance of an object
selected is the object, and an instance or a part stands for its object only where the command says so.
Nothing selected is refused ("Nothing is selected: name the objects, or select them first.",
`nothing_selected`), so is a selection of a kind the command does not take (`wrong_selection`), and a command
of one target refuses a selection of several (`one_object_only`). Where a menu item acted on the object
list's own rows, a call that names no targets reads those rows too (`object_assemble`, `part_change_type`,
`object_split`, and `object_delete`'s person's form), since the list and the 3D selection disagree when
every instance, or every part, of an object is selected. The reply names what the command acted on
(`targets_reply()`: `objects`, `instances`, `parts`, each with its index, id and name), so a caller that gave
no target learns what the selection was, and a command that makes, removes or renumbers things replies with
their indices as they are afterwards. `object_delete` names what it deleted with the indices they had, and
under `kept` what it was asked for and did not delete, with its indices afterwards: its person's form leaves
the object list to delete, which goes from the last item and stops at one it refuses (an object's last solid
part) or asks about (a cut object), and `Ops::deleted_targets()` finds what went by identity, since the delete
moved the indices. A plate command with no `plate` acts on the current plate, and its
reply names it.

Each tool also has an access label, which `tools/list` sends as MCP's annotations. Clients act on it
differently: Codex asks a person before every call of a destructive tool (unless it is also read-only),
and its `writes` mode before every call of a tool that is not read-only; Claude Code asks by its own
permission rules whatever the label says, runs read-only calls in parallel and retries idempotent ones.
So the label marks what a call does by its nature, not everything a call could reach, which would put
a confirmation on calls an agent makes all the time.

`read_only`: the command changes nothing in the app or the project. `window_screenshot` is read-only,
since its PNG replaces no file without `overwrite`; `replay_export`, which can clear the journal, is not.
`destructive`: every call deletes what a person made, with no undo: `preset_delete`, which takes the
preset's file and its cloud copy, and `plugin_uninstall`, which removes the plugin's files. A command that
can lose work only when an argument asks for it is not: `project_new`, `calibration_generate_test` and
`project_open` drop unsaved changes only with `discard_changes`, and an output replaces a file only with
`overwrite: true`. Nor is `recording_encode`, which deletes only a recording this session made, once its
MP4 has replaced it, nor a command whose deletion has undo (`object_delete`, `plate_delete`). Nor is a
command that runs what its arguments name: the input commands (`widget_click`, `widget_set_value`,
`keyboard_press`, `mouse_gesture`), which press whatever is on screen, a Delete button included,
`action_run`, `tool_call`, `replay_run` and `script_run`. What they run may be destructive, but the client
sees both in the arguments, and the label would ask on every call. A command made from menu items is neither
read-only nor destructive. `open_world` (`openWorldHint`): the command can reach outside OrcaSlicer, to a
printer, the cloud account or the network: `preset_save` (user presets sync), `preset_delete` (the cloud
copy), `webview_run_javascript` (the page's own reach); of the commands made from menu items `printer_send`,
`preset_sync`, `filament_sync_ams`, `app_check_for_updates`, `app_open_web_page` and `plugin_refresh`;
`plugin_enable` and `plugin_reload`, which run a plugin's code and download a cloud plugin, and
`plugin_uninstall`, whose Unsubscribe reaches the account; and through what they run `action_run`,
`tool_call`, `replay_run` and `script_run`, which has the app's access to the computer. No client is known
to ask on it. `idempotent`: the same call again changes nothing more.

The commands that take targets take expected ids beside them (`target_props()`): `object_id` beside an
`object`, and where they take a part index a `part_id`, as `object_transform`, `part_add`,
`object_set_height_ranges`, `object_set_variable_layer_height` and `settings_set`'s object and part targets
do; and beside lists `object_ids`, `instance_object_ids` and `part_ids`, one per entry of `objects`,
`instances` and `parts`, as `object_select`, `object_delete` and the other commands of several targets do:
the `ObjectID` values `object_list` reports as `id`, which `orca.host` shows too. `check_expected_ids()`
refuses the call before anything changes when the index names another object
or part now, so a replay or script addresses what it read. `object_list`' description says what the ids are for
and names the commands that take them, so each property only points back at it. Ids are allocated per
process, so a command that takes them names them in its `id_args`, which `with_expected_ids()` reads from
its schema where the command is registered, and the core gives the journal its calls without them
(`without_expected_ids()`, which looks the command up in the catalog), through `tool_call` and inline
`replay_run` steps too, and a replay addresses by index; a `script_run` step's code and `args`, and a plugin
operation's own arguments, are kept as written.

Descriptions keep one order: what the tool does, the arguments that change it, `Reply:` with the
fields it has, then edge cases; a long one puts each case on a line of its own (the types and targets
of `settings_get` and `settings_set`, `widget_click`'s web pages). What several tools share is written once,
since every session loads the whole tool list: the locator grammar in `widget_list`'s description,
which hands out the refs, each `target` giving two examples and pointing there; the paging shape in
the server instructions.

An argument name means one thing across the tools, so what an agent learns from one tool holds for
the next. `target` is the window or element a UI tool acts on (a ref, a locator, or `main`, `dialog`
and the like), `canvas` a 3D view, `path` the file a tool is about, and `save_path` where a capture
writes its file beside its reply. So the point `view_set`'s camera looks at is `look_at`, which
`camera_state` reads back under the same name, and a drag's intermediate points are `waypoints`.
The main window is `main` wherever a tool names it, as `window_list` calls its kind; `window` is
another kind there. A part of an object is `part` in arguments and replies, the Objects panel's word
for what the model calls a volume, which leaves `mesh_volume` and `scaled_volume` to mean cubic
millimetres. A tool that takes one plate calls it `plate`, an index; one that also takes
`"current"` or `"all"` calls it `plates`. Each refuses an index the project lacks in the same words
(`no_such_plate()`), which give the index and the plates there are, so an agent learns the range from
any of them. Where a tool takes two unlike things of one word, each says
whose it is: `discard_changes` drops the project's unsaved changes, `preset_changes` keeps, transfers
or discards the presets'.

An agent calls into two catalogs by name: the commands (those `tools/list` shows, the three menu items it
does not, the Design tab's and the operations plugins offer) and the Speed Dial's actions, which
`action_run` runs.
`tool_search {query}` finds a name in either, and a command runs by its key through `tools/call` and
`tool_call`, with `tool_search` giving the schema of one `tools/list` does not show. Its description
names the Design commands while the catalog holds them (see Design tab), so an agent knows of them before
it searches. A name that misses
is answered with the closest names of the catalog asked and nothing else, unless the catalog leaves that
command out (`CommandCatalog::leave_out()`): then it says why. A catalog built with the CAD feature off
leaves out the Design commands, and says that the feature was off when OrcaSlicer started and how a person
turns it on. `CommandCatalog::unknown_command()` builds the error for every lookup of a command that
misses: `tools/call`, `tool_call`, `tool_search {tool}`, a replay step and a plugin's or script's call.
`action_run` gives the closest action ids, and says that `action_search` finds an action and
`tool_search` a command, since the name may be a command's that has no action; an action's key runs its
command. For a Design tab action that is not shown it says when the action is there instead, and that
`tool_call` runs its command from any tab, or, when the catalog leaves its command out, why.

Each tool family lives in its own `McpTools*.cpp`. List tools take `limit` and `offset` and reply
`{items, offset, total}` (`page()`; `object_list`, a tree, has `plates` and `outside` in place of
`items`), with `next_offset` while more remain: `action_search` too. The server instructions give that
shape once, rather than every description, and each `limit` states its default. `McpPaths.hpp` requires
absolute paths, since a relative one would resolve against `<datadir>/log`, the GUI's working
directory; an output's directory must exist, and an existing file is replaced only with
`overwrite: true`. Paths arrive and leave as UTF-8 text, and so does Orca's `data_dir()`;
`path_from_utf8()` and `path_to_utf8()` convert, through wide strings on Windows, where boost's
narrow conversion would use the ANSI code page.

### object_list

`object_list` lays the model out as the Objects panel does: `plates`, each `{index, name, objects}`
with the objects that have an instance on it, then `outside`, the objects with an instance on no
plate. `objects_by_plate()` builds it from `object_info()` and `volume_info()` (McpToolsScene.cpp,
libslic3r only, so unit tests build them from a `Model`) and a callback that says which plate an
instance is on. The handler's is `instance_plate()`: the plate registry's answer or, for an instance
the registry does not hold, as Fill bed leaves its new ones, the first plate the instance stands on,
so the copies that fill a plate are listed on it. `plate_list` counts a plate's objects the same
way. On request the handler adds each instance's canvas rectangle.

The reply differs from the panel in two places, both so that a row says all an agent needs to act
on it. A plate holds instances, not objects, so an object row lists the instances of its plate only,
and an object with instances on two plates has a row under each; the panel shows it once, under its
first instance's plate. And an object always lists its `parts`, even a single one, which the panel
folds into the object's row: painting, a part's filament and its settings address the part. An
object with several parts is what Assemble makes, and the reply has no other kind for it.

An object's `index` numbers it in the project and a part's in its object, whatever the plate, since
those are what the other tools take. `plate` lists one plate alone, without `outside`. `limit` and
`offset` page the objects listed, in the model's order, and `total` counts them, each once however
many plates it is on. A page lists its objects under every plate they are on, so the rows of one
object come together.

Bounding boxes are
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
names without their prefix). The `object_transform` reply on a part carries the same entry as
`part_info`. An object with height range modifiers lists them as `layer_config_ranges`
(`layer_config_ranges_json()`), each `{min_z, max_z, values}` with its whole config serialized as
`settings_get` serializes values, so a range always shows its `layer_height` and `extruder`; its `min_z`
and `max_z` are what `object_set_height_ranges` keeps a range by and what the target `height_range` of
the settings commands names it by. An object whose `layer_height_profile` has more than a flat bottom-to-top
pair, which the object list marks
as a variable layer height, reports it as `layer_height_profile: {points, min_height, max_height}`
(`layer_height_profile_json()`): the number of (z, height) points and the extreme heights, to 0.001 mm.
The points themselves stay in the 3MF's `layer_heights_profile.txt`.

An object also has `auto_drop`, `brim_ears` (how many), `overrides`, the keys of the settings it sets
itself (`settings_get` with the target `object` reads them), and `mesh_errors`, what the object list's
warning icon reads (`Ops::mesh_errors()`): `open_edges`, `manifold` (no open edge) and the counts the loader
or a repair left (`degenerate_facets`, `edges_fixed`, `facets_removed`, `facets_reversed`,
`backwards_edges`), which `object_fix` reports before and after its repair.

### object_transform

`position` is where the instance or part goes, as the Move panel shows it, the same move as an
absolute `translate` whatever `mode` says for rotate and scale; `part_add` takes the same name for a new
part's place. With no `object` it acts on the instance or part selected in the Prepare view, or on
instance 0 of the object selected.

On an instance, `object_transform` writes the `ModelInstance` directly: a relative rotation composes as
Euler angles, and a rotation or scale applies to that instance only, where the panel applies it to
every instance of the object. `size` sets mm along
the object's own axes (its model parts' box, unrotated, times the instance's scale; a single number
with two nulls keeps the proportions), and `plate` is one more relative move by the difference of the
two plates' origins, applied last. Then the bounding box is invalidated before
`ModelObject::ensure_on_bed()`, so a scale that grows an object drops it onto the bed instead of
leaving it sunk, and the instance is registered with the plate it is on now
(`PartPlateList::notify_instance_update()`, then `ObjectList::update_plate_values_for_items()`), as
`GLCanvas3D::do_move()` does for a person's moves. Slicing and `object_list` read plates from that
registry.

On a part (`part`), it takes the Object manipulation panel's path. It refuses the assemble view, an
open gizmo and an object with one part (which has no row of its own), selects the part's row in the
object list with nothing else selected, so the 3D view selects it on instance 0, takes one undo
snapshot, and runs `Selection::translate()` / `rotate()` along the bed's axes and `scale()` along the
part's own axes, each followed by `do_move("")` / `do_rotate("")` / `do_scale("")`. Those write the
part, drop the object when Auto Drop is on, register every instance with its plate and update the
object list. An absolute translate is a delta from the part's position on instance 0; an absolute
scale or a `size` (mesh box times the part's scale, in the object's frame) becomes factors on the
current scale, as the panel computes them, since `Selection::scale()` multiplies a part's scale
whatever the transformation type. A part's rotation is relative only.

### part_add

`part_add` runs what the object's context menu Add items run. It refuses the assemble view, an open
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
camera looks. With no `object` it adds to the object selected in the Prepare view, or to the object of the
selected instance or part.

### object_arrange

`object_arrange` runs what the GUI runs: with `plates: "all"`, `set_prepare_state(PREPARE_STATE_DEFAULT)` and
`Plater::arrange()`, as the toolbar does; with a plate index or `"current"`, `select_plate()`,
`PREPARE_STATE_MENU` and `arrange()`, as the plate's Arrange icon does, without its
`last_arrange_job_is_finished()` flag, which a cancelled job never clears; with `fill_object`, the object's
row selected and `fill_bed_with_instances()` or `fill_bed_with_copies()`, which fill the object's own
plate. Each takes one "Arrange" undo snapshot. An arrangement shows the Prepare view first and runs
with a gizmo open, as the toolbar's does (`Ops::start_arrange()`, which a person's pick of its action,
Auto-Arrange, calls too), and is refused while another job runs. Fill bed selects the object's row, so it
refuses the assemble view and an open gizmo as the commands that select do. The command also refuses a
locked or empty plate given by index, `fill_object` with `plates`, and an object Fill bed refuses
(`can_increase_instances()`).
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
(default 300) the call cancels the job. Called with no arguments, as its Speed Dial action, it arranges
every unlocked plate with the panel's saved settings, as the toolbar does, and an automated call of that form
waits and replies as any other call; a person's pick starts the arrangement and returns.

### object_set_height_ranges

`object_set_height_ranges` gives an object's whole set of height ranges (`ModelObject::layer_config_ranges`),
which a person builds with the object list's Height Range Modifier, its "+" buttons and the range
editors in the sidebar, one gesture and one undo step at a time. A range is `{min_z, max_z}`. One given with
the `min_z` and `max_z` of a range the object has keeps that range's settings; a new one starts from what the
object list gives a new range, `ObjectList::get_default_layer_config()` (the object's layer height, else the
process's, and filament 0, the object's). A range's settings are not this command's: `settings_get` and
`settings_set` read and write them with the target `height_range`, the range named by its `min_z` and
`max_z`, so they are set in one place with an object's and a part's. There they take the keys a range's page
and the object list's Add settings menus offer (`layer_range_option_keys()`, the `region` list of
`settings_list`, `layer_height` among them), and a `layer_height` given is checked against the bounds the
range editor enforces (`check_height_range_layer_height()`: `get_min_layer_height()` /
`get_max_layer_height()` for the range's filament, the printer's per-nozzle values indexed by filament
number, so the first serves a single-nozzle printer). Ranges may touch but not overlap: the slicer would
trim an overlap silently (`layer_height_profile_from_ranges()`), and from an agent it is a mistake. A
range above the object is kept, as the GUI keeps it, and slicing cuts it; the reply gives the object's
height and its ranges as `object_list` shows them. All of this is checked before anything changes, and a
call that gives the ranges the object already has changes nothing and takes no undo step. With no
`object` it acts on the object selected in the Prepare view.

Like `part_add`, it refuses the assemble view and an open gizmo and switches to Prepare. To change the
ranges it selects the object's row, so no Layers row and no sidebar range editor stays in use, then
takes one snapshot ("Edit height ranges"), writes the new ranges, rebuilds the Layers rows from the
model with `ObjectList::reload_layer_rows()` (shared with pasting ranges) and calls
`Plater::changed_object()`. A range editor that loses focus later reports an edit of a range that is
gone; `ObjectList::edit_layer_range()` then edits nothing rather than adding the range back without a
layer height. The dialog the menu shows when the process uses mixed colour sublayers becomes a
`warnings` entry, and so does a variable layer height profile on the object, which decides its layer
heights instead of the ranges' `layer_height` (`PrintObject::update_layer_height_profile()` falls back to
the ranges only without a valid profile).

### object_set_variable_layer_height

`object_set_variable_layer_height` runs what the Variable layer height panel's Adaptive, Smooth and Reset buttons
post: `GLCanvas3D::adaptive_layer_height_profile(quality)`, `smooth_layer_height_profile({radius,
keep_min})` and `reset_layer_height_profile()`, each with the GUI's undo snapshot ("Variable layer height -
Adaptive", "- Smooth all", "- Reset"), followed by a background update and the object list's variable
mark. The defaults are the panel's (quality 0.5, radius 5, keep min off), fixed rather than read from the
panel, so a replay does not depend on what a person last set there; an argument of another action is
refused. Like the panel, it works on the selected object, so it selects the object named (with none, the
object selected in the Prepare view): it refuses the assemble view and an open gizmo,
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

### plate_set_custom_gcode

`plate_set_custom_gcode` places what the Preview layer slider's right-click menu adds at the start of a layer
(Add Pause, Add Custom G-code, Add Custom Template, Change Filament > Filament N) and removes them, on one
plate. The codes are a plate's `Model::plates_custom_gcodes` entry, saved in the 3MF
(`custom_gcode_per_layer.xml`) and read back by `plate_list` as `layer_gcode`, with the same type names
(`pause`, `custom`, `template`, `change_filament`, and `color_change` for an old project's colour change,
which the slider never adds). The slider is the source of truth: the plater's handler of its tick event
writes the slider's whole set over the plate's entry, and the slider is not refilled while the plate's slice
is invalid, so a code written to the model alone would vanish at the next tick event. The call therefore
edits the slider with its own functions (`IMSlider::add_code_as_tick()`, `add_custom_gcode()` and
`delete_tick()`, given an explicit layer instead of the active thumb) and hands the ticks over with
`GLCanvas3D::send_layer_ticks_changes()`, what each Preview frame does, then waits for the handler: the
plate's entry rewritten, its slice invalidated, the project dirty and its revision raised.

Codes sit on the layers of the plate's slice. `layer` is the slider's value plus one, as `view_set` and
`plate_slice_stats` number layers; `z` takes the layer whose top is at or above it
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

### Settings of an object, a part or a height range

The settings commands take two arguments where a person picks two things (`settings_get`, `settings_set`):
`type`, which settings (printer, process or filament, as the preset commands have it), and `target`, where
they apply, named for the rows a person selects in the Process panel's Global and Objects modes: `global`,
the default, the preset being edited; `object`, an object or an assembly, named by `object`; `part`, a part
or a modifier of it, named by `object` and `part`; `height_range`, one of an object's height ranges, named by
`object`, `min_z` and `max_z`. With no `object` the target is the object or part selected in Prepare. A
target other than `global` holds process settings alone, since the app has no filament or printer setting on
an object (`TabPrintModel`), so with another `type` it is refused (`settings_request_error()`, which also
refuses an argument of another target and a part named by its object alone, before anything is read).

What a target takes is what its row offers to add (`target_options()`): an object, the keys the object
list offers to add to an object (`SettingsFactory::get_options()` less what
`SettingsFactory::is_improper_category()` hides with the current filament count, the rule of the list's Add
Settings menu); a part or a modifier, the region keys among them; a height range, `layer_height` and those
region keys (`layer_range_option_keys()`). `settings_list` with `type` `object` or `region` lists them. Keys
a person cannot add there, such as the internal `print_extruder_id`, the process-wide `print_sequence` or,
with one filament, the per-feature filaments, are refused rather than stored and ignored by slicing, and the
refusal says where the key is set (`untaken_setting_error()`: "layer_height is set on an object or a height
range, not on a part", or that it is set globally only).

`extruder`, the target's filament, is the one key outside those lists. `settings_set` writes it through
`ObjectList::set_extruder_for_items()`, the Change Filament code without the selection, so a call leaves
the user's selection alone. An object's filament clears its parts' own; 0 or null is Default, which is
filament 1 on an object, the object's filament on a part and no filament override on a modifier. A
single-part object has no part row, so its part 0 sets the object's filament, and the reply notes it.
Filament numbers (`extruder`, the per-feature and the support filaments) past the project's slots are
refused before anything changes, and the support filaments refuse a mixed slot, as their combos do. A
height range's `layer_height` is checked as its range editor checks it (see `object_set_height_ranges`). A
null value removes an override; `reset_to` and `reset_all` are refused on these targets. A call is one undo
step (`Plater::SingleSnapshot`).

Read on such a target, `settings_get` with no `keys` gives what the target overrides, and with `keys` or
`query` any setting it takes. Each item has `value`, the target's own (null where it has none), and
`effective` and `source`, resolved in the order the slicer applies them (`effective_setting()`): the height
range's own value (`height_range`), else the part's (`part`), else the object's (`object`), else the
process being edited (`global`), else the option's default (`default`). An `extruder` of 0 is no override.
An object's or a height range's item also lists in `parts` the parts and modifiers that override the key,
whose values apply only inside their own volume, and an object's or a part's lists in `height_ranges` the
ranges that override it between their heights. Inside a region modifier, a mesh or a painted one, that
sets none of the key, the slicer gives the region the value of the region the modifier overlaps
(`region_config_from_model_volume()` starts from it): a model part's, or that of a region modifier of
either kind before it in the object's list, since the slicer chains both kinds alike. Which volumes it
overlaps is not worked out, so where one of those sets the key the modifier's item gives no value as the
one it prints with: `effective` is null, `source` is `overlapped_part`, `object_effective` and
`object_source` are what the object's chain resolves to, and `parts` lists the volumes that set it. The
reply names the target (`objects` or `parts`, and `height_range`).

### Saving presets

`preset_save` saves the printer, process or filament preset being edited, unsaved changes included,
through `Tab::save_preset()` with the name given, the call the Save preset dialog's OK leads to, so
no dialog opens and the saved preset is selected. A filament is first made the one the filament tab
edits for `filament_slot`, as `settings_set` does, and saving it moves every slot that used the old
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

There is no subset of edits to save. `settings_set {"reset_all": true, "values": {...}}` on a preset
(the target `global`) first discards every unsaved change with `Tab::on_roll_back_value()`, the tab's
undo button, then applies the values in the same call, so `preset_save` keeps exactly those. The reply
lists the discarded keys in `reverted`. The values are parsed before the reset, so a bad one leaves the
edits in place. The other targets refuse `reset_all`; a null value removes an override there.

### Reading, comparing and changing preset values

`settings_get` reads the settings named (`keys`) or searched for (`query`), or with `compare` those that
differ from another preset, or what a target other than `global` overrides; a call with none of these is
refused, since `settings_list` lists all of a type's settings, so no call of `settings_get` is a second
way to list them. On the target `global` it reads the preset being edited (for a filament, the slot's
preset, edited or not), or with `preset` any saved preset of the type, selected or not, without selecting
it: the named preset
is read as saved, without the edits the tab holds. `preset` and `filament_slot` are exclusive, and a
name that matches no preset is refused with the closest names. `compare` lists only the keys, among
those named or matched, that differ from another preset: a saved preset by name, `:saved` (the read
preset as saved, so the list is its unsaved changes, what the Unsaved Changes dialog shows) or
`:parent` (the preset it inherits from, a system preset itself, what the tab's lock icons compare
against). `:` cannot be part of a preset name, so the two never clash with one. The key list is
`PresetCollection::dirty_options()` with the Compare presets dialog's rules: printer and filament
vectors are compared entry by entry (an item then carries `index`), a printer with fewer extruders is
compared from the other side, and both presets have one printer technology. Each item has `value`
(the preset read) and `other`; the bookkeeping `*_settings_id` keys are left out. `preset_list`
reports `inherits` and `project: true` for presets kept inside the project.

`settings_list` lists all the settings of a `type`, which has no default, in one reply, each with its
value and no meta: `process`, `printer` and `filament` read the preset being edited, a saved one with
`preset`, or a slot's with `filament_slot` (a process list runs to about 400 settings and 20 KB, a
printer's to 220 and 40 KB, most of it G-code blocks); `object` lists the process settings an object or an
assembly takes and `region` those a part, a modifier or a height range takes, whose `layer_height` item
says a height range alone takes it (`targets: ["height_range"]`), each with the process preset's value,
which an object or a region prints with unless it sets its own. `settings_list` takes no target: what an
object, a part or a height range overrides is `settings_get` on that target with no `keys`. With
`where`, or with `include_meta`, its items carry their meta and come 100 a page
(`settings_list_page()`): a page of a hundred keys with meta runs to about 50,000 characters, and an agent
that lists everything is scanning values; the keys it then names or searches for come with their meta.
An enum's meta lists `labels`, the untranslated texts its combo shows, in the order of `values`, so a
person's wording maps to a value.

What a setting does comes with its value. Meta has `description`, the option's whole untranslated
tooltip (what it does, when to change it, what it affects), and `where`, the page and group of its
row on the settings tab. `where` comes from the sidebar's settings index (`Search::SettingsIndex`),
which the settings tabs fill as they build their pages and the Speed Dial reads for its own setting
actions; a setting with no row on a page has none, and an object's or part's settings are placed on
the process tab. The help text sits in `settings_get` rather than in a command of its own because that is
where agents look: in the bench, agents read settings with the settings reads three to five times as often
as they called a separate explanation tool, and with the tooltips out of the meta they set a draft
shield without skirt loops and a support threshold measured from the wrong side, mistakes the same
agents avoided when the tooltip came with the value. The size stays bounded by the rule above: only
keys that are named, searched for or listed with their group carry meta.

`settings_list`'s `where` lists one group ("Strength > Walls") or a whole page of the type with values
and meta; a name the type has no page or group for is an error that lists them all, which is the map of
the tabs. `settings_get`'s `query` serves an agent that knows what a setting should do but not its name
(`rank_settings()`). A query word matches a word of a key, label or description that starts with it,
so "retract" finds "retraction" while "to" does not match inside "toolchange"; words under three
letters count only when the query has nothing longer, since most help texts have them. Settings
matching more of the query's words come first, then those matching more of them in the key or label.
With no weighting by how rare a word is, a generic word such as "reduce" counts as much as
"stringing", so the tool asks for a few distinctive words. A search returns the best 20 unless `limit`
asks for more.

Without a `type`, a search and named keys look through the process, filament and printer settings
being edited, since a question about an effect rarely knows which tab answers it (stringing is
retraction on the printer tab and its overrides on the filament's), and an agent naming a key seldom
knows which tab holds it; each item names its `type`, the first of the three that has the key. Every
other read without a type (`preset`, `compare`) reads the process, and `settings_set` writes it: a write
takes one type, whose values apply together, so it never spreads over several by itself. A key the type
lacks is listed in `unknown_keys`, and `type_of` names where it is a setting (another type, or `project`
for the project's own settings: `type_of_key()`); `settings_set` refuses such a key with that type named,
so an agent that passed a filament key with no type learns where it belongs instead of reading
"unknown".

Many process, filament and printer values hold one entry per extruder variant (an extruder type and
nozzle flow type, such as the X1C's "Direct Drive Standard" and "Direct Drive High Flow"), the machine
limits a normal and a silent mode entry per variant, and some printer values one entry per extruder.
For such a value with more than one entry, the meta lists `variants`, what each entry is for, and
`active`, the entry each extruder prints with (extruder 1 first), found as the slicer and the tab's
variant switch find it (`DynamicPrintConfig::get_index_for_extruder()` with the printer's
`extruder_type` and the project's `nozzle_volume_type`). The tab edits one variant at a time: its
field shows the variant in use and typing writes that entry. So a single value for such a key goes to
the entries in use, `settings_set` writes one entry as `{"key": {"index": i, "value": v}}`, and an
array writes them all. Either way the whole vector, with the entries replaced, goes through the tab's
transaction (`Tab::apply_preset_config_values()`), whose commit runs the key's handling as a field edit
does, and the other entries stay as they are. A value with another number of entries is refused, with
the entries and the one in use in the error and its details: written as given, it would collapse the
vector and leave the preset inconsistent with its variants. Values of one entry, and vectors that are not
per variant or per extruder, take a single value. The overrides of the other targets take whole values.

Three printer values are set as the app sets them, not typed into the tab. `extruders_count` goes alone,
as a change of the Printer settings' Extruders field does (`TabPrinter::change_extruders_count()`).
`nozzle_volume_type`, one per extruder, is each extruder's nozzle flow type as its card in the sidebar
picks it. A `nozzle_diameter` that another variant of the printer model has selects that variant's
printer preset, as the sidebar's nozzle diameter does, and may come with `nozzle_volume_type` alone
(other keys go in a second call); each flow type is checked against what the new preset's nozzle card
offers before anything changes, and read back after the picks (`pick_nozzles()`). The switch asks about
the printer's unsaved changes (`blocked_by_dialog`) unless `reset_all` discards them first; the discard
stays if the switch is not made.

`settings_set` replies with `changed`: every key of the edited config (preset, object, part or height
range) whose value differs after the call, including the settings the app adjusted in turn, such as
spiral vase setting one wall loop and no top shell layers once its dialog is confirmed, and the keys
`reset_all` reverted. A preset's change is not in the undo history; a target's is one undo step.
Dependency order within one call is not resolved; a call can be split instead.

### Unsaved preset changes

On a preset (the target `global`), a `null` value takes the key back to the saved preset's value, as the
key's undo arrow does, and with `reset_to: "parent"` to the value of the preset it inherits from (the
`:parent` of `compare`), as its lock icon does. The value joins the call's other values in the tab's
transaction, so its handling runs as for a typed value. `reset_to` without a null value, and on the other
targets, is refused.

`preset_select` finds the name in the selector the GUI picks from (`Ops::resolve_preset()`, which the
plugin API's `begin_update()` uses too): the sidebar's filament combo of the
slot, or the tab's combo, by the label shown or the internal name. It then selects the printer or process
preset (`Ops::select_preset()`), or assigns the filament to its slot as a pick in the slot's selector does
(`Ops::assign_filament_preset()`, `Sidebar::select_filament_preset()`). A selector lists the visible,
compatible presets, and a hidden one is a system filament or printer the setup wizard has not added,
which a fresh data folder has for all but a few: `preset_list` lists those only with
`include_uninstalled`, each with `installed: false`. Such a name is shown first (`show_hidden_preset()`):
the preset becomes visible in memory and the selectors are rebuilt, as they show the selected preset and
the filaments an AMS holds, so the wizard's list in `OrcaSlicer.conf`, and what the next start shows,
stays as it is; the reply names it in `shown`. A printer shown this way also shows its model's default
filaments when no shown filament suits it, as adding the printer in the wizard does
(`PresetBundle::load_installed_filaments()`), so the switch lands on one of them rather than on the
default filament. A preset's own name wins over another's alias that reads the same. An incompatible preset is refused as such, since no selector lists it, and an unknown name with
the closest names. A mixed-colour slot is refused as one (`mixed_filament_slot`), since it holds no
preset of its own. A printer or process selection that leaves another preset selected than the one
asked for is refused too (`other_preset_selected`), and that preset stays selected.

`Tab::select_preset()` asks about unsaved changes through `Tab::may_discard_current_dirty_preset()`:
for the preset it leaves, and for each modified preset a process or printer switch may replace (the
filament, or the process and filament). `preset_select`'s `preset_changes` sets `Tab::m_dirty_preset_answer`
for the one switch, so every prompt is answered with the dialog's Transfer or Discard without
showing it, and the switch then does what it does after a person's answer. Transfer moves what the
dialog would list: the changed options a settings page shows in the current mode, so a setting the
mode hides is dropped, as in the GUI. The dialog offers no Transfer for a printer's own changes or
between filaments of different types (`Tab::can_transfer_changes_to()`); there `transfer` is refused
before anything changes. The GUI's outcomes stay as they are: a modified filament that suits the new
process keeps its changes whatever the answer. Without `preset_changes` the prompt opens, as it does for a
person, and the reply is `blocked_by_dialog`. The message box that a printer switch shows when it drops
other extruder variants' values is shown only to a person. With several filament slots the sidebar does
not move the filament tab when a slot's filament changes (`Plater::priv::filament_preset_picked()`), so no
prompt opens there: the edits stay with the preset the tab edits, and the reply's `modified` lists them.

`project_new` without `preset_changes`, or with `"keep"`, keeps modified presets for the new project, as
New Project's Keep does with every option. `"discard"` is the prompt's Discard,
`GUI_App::discard_current_preset_changes()`, called only when a preset is modified, since it reloads
the presets, which can ask about the printer's. Modified presets make the project dirty, so
`discard_changes` is needed too.

Both replies carry `discarded` and `modified` when presets had or have unsaved changes, each
`{type: {name, keys}}`: a change is discarded when the preset edited afterwards no longer holds its
value, and modified lists the unsaved changes left. Only presets that `preset_list` calls modified
count, with the keys of their preset type; the full comparison also lists bookkeeping keys that only
one of the two configs has.

### Deleting presets

`preset_delete` deletes a user preset or one kept inside the project, with its file and its cloud copy,
through the GUI's own delete, so no confirmation opens; it is marked destructive, so clients that gate
such commands ask first. What may be deleted is checked first, as the GUI offers and checks it: the tab
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
preset, and `discarded`/`modified` as `preset_select`'s. A replay needs the preset to exist,
so replays create it first with `preset_save`.

### Project settings

`project_get_settings` and `project_set_settings` read and write the project's own settings for its
plates, which it stores outside any preset (`PresetBundle::project_config`), as a plate's are stored on
the plate: they are not print settings a preset or an object holds, so they have commands of their own
beside the settings commands. They are `curr_bed_type` and `wipe_tower_x`/`wipe_tower_y` and, on a
printer with several extruders, the read-only `filament_map_mode` and `filament_map`;
`project_get_settings` with no `keys` returns them all, each with its value and meta (`meta.offered`,
the choices this printer has, and `meta.read_only`). A generic config write would skip what each
control does after setting its value, so `project_set_settings` writes every key through that
control's own code. Everything is checked before anything changes, `null` is refused (project values
always exist), and `{"index": i, "value": v}` sets one plate's tower position.

- `curr_bed_type` goes through the plate type selector (`Sidebar::set_bed_type_accord_combox()`, whose
  handler updates the project, the printer's remembered plate type and the plates on the global type).
  Only the types the selector lists are taken (`meta.offered`), and none while the selector is hidden
  for the printer.
- `wipe_tower_x`/`wipe_tower_y` are one position per plate, in mm from the plate's origin, as a tower
  drag stores them: one undo step ("Move object", which takes the old positions along), then an
  update. A position off the plate is refused; one where the tower does not fit is re-placed by the
  scene reload, as after a drag, with its notification, and the reply's `note` says so.

The filament grouping is set per plate in the Filament grouping dialog, and every plate carries its
own mode from the start, so a project-wide `filament_map_mode` or `filament_map` would not change a
slice: both are listed for reading only, and a plate's are written with `plate_set_settings`. The same
`project_config` holds the filament list's state, its slot colours, mixed filaments and flushing, which
belongs to the filament commands (The filament list, below): a call that names one of those keys here
is refused, naming the command that reads or sets it. Nothing here except the tower position enters the
undo stack, as in the GUI.

### Plate settings

`plate_get_settings` and `plate_set_settings` (`plate`, the current plate by default) read and write
what the Plate Settings dialog sets on a plate and the plate stores (`PartPlate::m_config`):
`curr_bed_type`, `print_sequence`, `spiral_mode`, `first_layer_print_sequence` and
`other_layers_print_sequence`; and on a printer with several extruders what the plate's Filament grouping
dialog sets, `filament_map_mode` and, with the mode "Manual", `filament_map`, the extruder of each
filament (1-based). The plate's name and lock are commands of their own, `plate_rename` and `plate_lock`,
as a person sets them with the plate's icons. Values take the forms the dialog offers: an enum value, a
boolean, a filament order that
lists every slot once (the dialog's drag canvas), and layer ranges `[{from, to, order}]` starting at layer
2 (the first layer has its own order), sorted and not overlapping, `to` left out for the dialog's "End".
`null` is the dialog's "Same as Global" or "Auto". The dialog's Auto/Customize choices are not separate
keys: an order given is Customize. `plate_get_settings` lists each key with `value`, the plate's own (null
where it follows the global setting or Auto), `effective`, what the plate prints with, and meta
(`meta.offered`, the plate types or grouping modes this printer has); `unknown_keys` lists the keys a
plate does not have.

`plate_set_settings` checks everything first, then selects the plate as its settings icon does and
passes the plate's values, changed by the call, to `Plater::apply_plate_settings()`, the dialog's OK
split from the dialog. It is called only when one of them changes, since the OK marks the project dirty.
The plate types are those the sidebar's selector offers (`meta.offered`), none while it is hidden. The
custom filament orders are refused while a slot is a mixed filament, where the dialog greys them out.
Turning spiral vase on asks the dialog's question about the settings it changes, so the reply can be
`blocked_by_dialog`. The grouping mode and map are written as the Filament grouping dialog's OK writes
them (`PartPlate::set_filament_map_mode()`, `set_filament_maps()`). On a printer with a multi-nozzle
extruder, a manual grouping is refused where an extruder it gives a filament the plate prints has no nozzle
of the volume type that filament needs, the check that disables the dialog's OK on its Custom page
(`manual_map_nozzle_shortage()`, which both call). The call is one undo step
(`Plater::SingleSnapshot`; the dialog itself takes none, the plate tab one per change). The journal records
the plate the call resolved, so a replay sets the same plate. The reply has `applied` (the plate's own
values), `effective` and `changed`. The plate tab's `skirt_start_angle` is not among them.

`plate_rename` names a plate as its name icon does (`Ops::rename_plate()`, `Plater::rename_plate()`): a
plate other than the current one is made current first, and the name the plate has already changes
nothing. `plate_lock` locks or unlocks a plate as its lock icon does (`Ops::set_plate_locked()`,
`PartPlateList::lock_plate()`), and only when it differs, so `locked: true` twice leaves the plate locked;
without `locked` it toggles, as its Speed Dial action does. `plate_delete` and `plate_duplicate` take `plate` too
(`Ops::delete_plate()`, `Ops::duplicate_plate()`, the plate menu's functions with the plate given), and
reply with every plate afterwards; a duplicate is one undo step and is refused while slicing and at the
most plates a project holds. `plate_select` makes a plate current as a click on it does.

`plate_list` reports every plate with its `name`, `locked`, these values as it prints them, `overrides`
(the keys it sets itself), `current`, `objects` (how many objects have an instance on it; `object_list`
lists them), `origin` and `size` (its front left corner and its width and depth on the bed, in the bed
coordinates object positions are given in, so that a caller can place an object on a plate by numbers),
`sliced` (an up-to-date slice), `wipe_tower` (the project's tower position for the plate) and `layer_gcode`, the
plate's entry of `Model::plates_custom_gcodes`: the Preview slider's ticks, pauses, custom G-code,
templates and filament changes, each `{z, layer, type, filament, color, gcode}` by height. `z` is rounded
to the G-code's 0.001 mm. `layer` is the number the slider shows for the code, found by the slider's rule
for a stored height (the first layer at or above it, `IMSlider::get_tick_from_value()`) in the layers of
the plate's slice result, numbered and placed as the viewer loads them (`gcode_layer_zs()`); it is given
only while that slice is up to date and no slice is running, and it is left out for a code above the top
layer.

### Preferences

The Preferences dialog keeps the app's preferences in the app configuration's `app` section, and one
table says which they are: `Ops::preferences()` (`Operations/PreferenceOps`), a row for each control
named after the key it stores, with its tab (general, files, behaviour, control, graphics, online or
developer), section, label, kind (checkbox, choice, number or text), a choice's values and labels, a
number's range, the fallback the dialog shows where nothing is stored, whether the row is shown in this
run, whether its value is read only at the next start, and, where a call may not write it, the reason. The dialog builds its plain rows from the table and
the rest with a function of its own per key, so the dialog, `preferences_get` and `preferences_set` cannot
differ. A preference is kept by the app, not in a preset or a project, so the two commands are under The
app, apart from the settings commands.

`preferences_get` with no argument lists every preference the dialog shows, tab by tab in its order, each
with `key`, `tab`, `section`, `label` (in English), `kind`, `value` (what the dialog shows: true or false
for a checkbox, a number for a number field), a choice's `choices` (`{value, label}`), a number's `min` and
`max`, `stored: false` where nothing is stored and the value is the fallback, `restart_needed: true` for a
preference read at the next start (the rows the dialog marks "(Requires restart)": a write is stored at
once and changes nothing before then), and `refused` for a key `preferences_set` does not write. The
switch and the port of this server read as their controls show them (`Ops::shown_texts()`, which the
dialog's rows read too): the switch is on while the server runs and the port is the one it is on, also
where the session's `--mcp` and `--mcp-port` started it without storing either, so their `stored` says
only whether the configuration holds a value. `keys`, `query` and `tab` narrow the list together. A named key the
dialog does not show is read as the app configuration stores it, and one that is neither is listed under
`unknown_keys`: the section also holds account and device state, which is read only by name.

`preferences_set` writes `{key: value}` as the controls do (`Ops::set_preferences()`): a checkbox as true
or false, a choice as one of its values, a number within its range, and the downloads folder as the
absolute path of an existing folder. Every key and value is checked first, so a call with one refused key
changes nothing; then the changed keys are written together, the configuration is saved, and each key's
effect runs through `Ops::apply_preference()`, in the keys' order, which the dialog's controls call too (the
preview redrawn, the sidebar's lists, the settings' mode, the backup timer, the log level, the theme, the
preset sync started or stopped, stealth mode's sign-out of the cloud accounts). A key whose value is
already the one given runs nothing. Refused by name, with the table's reason: a key the dialog does not
show, and a key whose control opens a dialog, restarts OrcaSlicer, signs out or reaches outside it (the
language, the login region, the network plug-in, file associations, the mixed-temperature warning, and
this server's own `mcp_enabled` and `mcp_port`, which would end or move the connection the call came
through). The switch that lets Python scripts run unrestricted is no row of the table at all: only a person
turns it on. While the Preferences dialog is open, a call that changes something is refused, as the
dialog is modal, so the rows of an open dialog change only through its own controls. Not in the undo
history. The reply has `changed`, each key with `before` and `after` typed as `preferences_get` gives them,
`stored_before: false` where nothing was stored and `before` is the fallback the dialog showed, and
`restart_needed: true` for a key read at the next start; and `unchanged`.

A refused preference changes in the dialog, whose controls carry their effects. `window_open {"window":
"preferences", "tab": <tab>, "key": <key>}` opens the dialog there through `GUI_App::open_preferences()`,
scrolled to the key's control and focused; a key on another tab is shown on its own tab. The dialog's
controls are named after their keys, so `widget_list` shows the key and `widget_set_value {"target":
{"name": "export_statistics", "within": "Preferences"}, "value": true}` changes one through its own
handler. Checkboxes, number and text fields and the language combobox change this way. The other
comboboxes (those of `create_item_combobox()`, the log level, the login region and the network plug-in
version) save from a handler on their drop-down list, where a person's pick arrives, while
`widget_set_value` sends the selection to the combobox itself: it shows the new choice and saves nothing.

### Calibration tests

`calibration_generate_test` runs a Calibration menu test (`test`: temperature, max volumetric speed,
pressure advance as tower, line or pattern, flow ratio in its four passes, retraction, VFA, cornering, input
shaping frequency and damping) one level below its dialog: it calls the same `Plater::calib_*` the dialog's
OK calls, with the `Calib_Params` the dialog would build. Called with `test` alone, the call of its Speed
Dial action (one per menu item, whose ids are the items' ids: `calib_temperature` and the rest), it opens that
test's dialog, as the Calibration menu does (`blocked_by_dialog`), and the dialog's OK runs the test; any value
of the test, or `silent: true`, runs it without the dialog. The dialogs only collect and check numbers, so
their rules are shared with the command in `libslic3r/calib.cpp`: `calib_default_params()` (the values each
dialog opens with), `check_calib_params()` (what each dialog refuses, as an enum, so each dialog keeps its
own message box and the command states the rule with the values given), `cornering_end_limits()`,
`has_junction_deviation()`, `belt_temp_tower_asset()` and the VFA dialog's auto-adjust, `fit_vfa_speed()`
with `vfa_candidate_layer_heights()`. The input shaper types offered per firmware come from
`shaper_type_values()` beside the dialogs. A parameter the test does not take is refused, so a misplaced
argument never passes silently.

Two choices that only refill the dialogs' fields are not parameters: the pressure advance defaults follow
the printer's `extruder_type` (the dialog's Extruder type radio, which starts at Direct Drive whatever the
printer), and the temperature dialog's filament type is replaced by the range itself. Temperatures are
clamped to 155-500 °C and rounded down to multiples of 5, as the dialog's fields do when they lose focus,
with a warning naming the corrected range (the field's message box says 170). The input shaping Y range,
and the damping test's Y frequency, default to the X one; RepRapFirmware, whose dialogs show one range,
refuses them. VFA's Yes/No questions are answered Yes: the reply's `adjusted` gives the lowered layer
height or end speed, and a warning says when the upper blocks stay above the filament's limit.

Every test starts a new project, whose prompts the tool answers itself (`skip_confirm` on each
`calib_*`): a project with unsaved changes needs `discard_changes`, and modified presets need `preset_changes`,
`"keep"` to run the test with them or `"discard"` to drop them first through
`GUI_App::discard_current_preset_changes()`. Unlike `project_new`, which keeps modified presets silently,
`calibration_generate_test` refuses without `preset_changes` and lists the modified keys: a previous test's
settings would
otherwise spoil this one. With `"discard"` the test's rules and defaults come from the saved printer
preset, which is what remains.

A test prints with one filament, `filament_slot`, which the dialogs pick in their filament list
(`CalibFilamentSelector`): a physical filament, so a mixed slot is refused, and slot 1 when the call names
none, where a dialog starts on the filament the filament tab edits, a slot that `settings_set` moves as a
side effect. The extruder follows from the filament map, by the slicer's rule (`calib_filament_extruder()`,
shared with the dialogs; extruder 0 while an automatic map leaves it to slicing), and the two go to
`Calib_Params::filament_id` and `extruder_id`, which every `Plater::calib_*` reads. The tests write their
filament settings to the preset the filament tab edits, so the tool first points the tab at that slot's
preset (`edit_filament_slot()`, the switch `settings_set` uses for a slot), as a person must; the reply's
`presets_changed.filament` gives the `filament_slot`, the `extruder` (1-based) on a printer with several once
the filament map decides it, as the dialog's list labels it, and `tab_switched_from` when the tab moved.
Neither is in `params`, which the journal replays as arguments: the slot is an argument of its own, and
the default slot 1 does not depend on the app's state. A tab that edits another slot's modified preset is
refused with `"keep"`.

The reply has `test`, `project`, `params` (the values used, in the tool's units: max volumetric speed in
mm³/s, a pattern's default acceleration and speed read back from the process), `objects`, `names`,
`plates`, `presets_changed` (per preset type, the settings the test changed, as `settings_set`'
`changed`), `result_keys` (the `type` and `keys` where the measured value belongs), `warnings`,
`adjusted`, and `discarded` and `modified` as `project_new` reports them. The journal records the call with its
effective parameters, defaults included, so a replay does not depend on the dialogs' defaults; VFA's end
speed is recorded as asked and a pattern's default acceleration and speed are left out, since both are
computed from the presets again on replay. To keep a measured value, the agent leaves the test with
`project_new {"discard_changes": true, "preset_changes": "discard"}`, sets the value with `settings_set`
and saves it with `preset_save`; staying in the test project, `settings_set {"reset_all": true, "values":
...}` with the preset's `type` drops the test's settings there and sets the value in one call.

### Object commands

Each object command does one menu item's job, or a few items' with the argument that picks the variant, and
takes its targets as Tool catalog says, so a call names what to act on and a Speed Dial action works on the
selection, as its menu item does. What it does is an operation of `ObjectOps` (see The operations layer),
which the item's own GUI function calls where it has one, so a pick and a call cannot differ. A few have more
to say than their descriptions:

- `object_order` orders a plate's objects (the current plate, or `plate`, or with `all` every plate), which
  sets the object list and print order. With `by` (name, filament, height, size) it does what the plate
  menu's Order By does (`ObjectList::order_objects()`), ascending or with `descending`; called as its Speed
  Dial action, with `by` alone, it keeps Order By's toggle and reverses the order the objects already have,
  which a call with any other argument never does. With `order`, the objects of the plate in the order wanted
  (`Ops::reorder_objects()`), it makes the drops a drag in the object list makes (`ObjectList::OnDrop()`), so a
  call reaches what a person can only drag: `order` names each of the plate's objects once, by index, and the
  other plates' objects keep their order. One undo step; the reply gives the new order.
- `object_fix` repairs the meshes as Fix Model does (`ObjectList::fix_through_cgal()` through
  `Ops::fix_objects()`) with no progress dialog and no notification: the call waits for the repair, during
  which the window does not redraw, and `job_cancel {job}` cancels it. What was repaired before a cancel,
  a part of the object it stopped in among them, keeps its new mesh and is shown with it; the reply, in the
  error's details, says which object the cancel stopped. Each object's `mesh_errors` before and after are in
  the reply, as `object_list` gives them.
- `object_clone` makes copies as the Clone dialog does (`Ops::clone_objects()`, over `Selection::clone()`),
  which the dialog calls too; `arrange` defaults to the dialog's choice, the `auto_arrange` preference, and
  the call waits for the arrangement.
- `object_reload_from_disk` reloads parts from their files (`Ops::reload_parts()`, the work of
  `Plater::priv::reload_from_disk()`); a part whose file is gone is not asked for but listed under `missing`,
  and cut objects are refused, since a reload would replace a cut part with its whole file. `part_replace`
  replaces one part's mesh as Replace 3D file does (`Ops::replace_part()`), keeping its settings, type,
  filament, placement and, with the `keep_painting` preference on, its painting.
- `object_set_printable` sets the flag the object list's Printable cell toggles, and `object_set_auto_drop`
  the 3D view's Auto Drop, which only says whether the object drops when it is next moved: the call drops
  nothing.
- `object_split` into objects asks Split's Auto-Drop question as its Speed Dial action; a call answers it
  with `keep_z`, or is refused (`auto_drop_question`) where pieces would float. `object_delete` refuses what
  Delete would ask about (part of a cut object, an object's last solid part) with the question as its reason,
  and its Speed Dial action leaves those to the object list, which asks.

### The filament list

The sidebar's filament section holds the filament slots, the mixed filament list under them, and the
Flushing volumes and Purge mode buttons in its title bar. Their state is stored in `project_config`
beside the project's settings, but the app keeps it with the printer: `PresetBundle::export_selections()`
writes the slot colours, the mixed filaments and the flushing into the app configuration under the
printer, so they outlast a new project and a restart and come back when the printer is selected again,
and a saved project keeps a copy of its own (`s_project_options`). So the filament list has commands of
its own, in the Filaments group, rather than the project's.

`filament_list` reads it as the sidebar shows it: a row per slot in config order, the physical filaments
first and then the mixed ones (`filament_slot`, `mixed`, `colour`; a physical row's `preset` and `type`;
a mixed row's `components`, `ratios`, `gradient` and `broken`), then `flush_multiplier`,
`flush_volumes_matrix` and `prime_volume_mode`, with `prime_volume_modes` where the printer offers a
choice. A mixed row's components are as stored, a deleted one as 0; its ratios, in whole percentages as
the commands take them, and its gradient are as the Edit Mixed Filament dialog reads them
(`Sidebar::mixed_filament_result()`). The slicer reads `filament_mixed_gradient_range` as the first
component's share at the bottom and at the top, so a gradient is given as the components that lead
`bottom` and `top`, with `per_part` and whether its `curve` is the default straight line or a custom one.
`broken` is what the sidebar marks (`Sidebar::broken_mixed_filaments()`): a component deleted or past the
physical filaments, fewer than two, or components of different filament types; slicing an object on a
broken mix is refused.

A mixed slot's `filament_presets` entry is a copy of its first component's preset name
(`create_mixed_filament_from_result()`), which no reader takes for the slot's preset: a mixed slot has no
selector and no filament tab, so `filament_list` gives its row no preset, `preset_list`'s `filament_slots`
leaves mixed slots out, and `preset_select`, `settings_get`, `settings_set`, `settings_list` and
`preset_save` refuse a mixed slot (`mixed_filament_slot`), as the plugin API's preset editing does.

`filament_add`, `filament_delete`, `filament_add_mixed` and `filament_edit_mixed` change the slots as the
list does (`Ops::add_filament_slot()`, `Ops::delete_filament_slot()`, `Ops::add_mixed_filament()`,
`Ops::edit_mixed_filament()`, see Commands that replace a dialog). None of them is in the undo history, as
in the GUI.

- `filament_add_mixed` adds what the Add Mixed Filament dialog's OK adds with no gradient
  (`Sidebar::ensure_mixed_filament()`): `components`, two or more physical slots of one filament type,
  each named once, and `ratios`, whole percentages adding up to 100; a mix the project has already is not
  added again, and the reply names its slot.
- `filament_edit_mixed` changes a mix as the Edit Mixed Filament dialog's OK does
  (`Sidebar::apply_mixed_filament_edit()`, the OK split from the dialog): the slot keeps its number and the
  objects on it, its colour becomes the blend, and a gradient stays for a mix of two filaments and goes for
  more, which the dialog shows none for. An edit that names its filaments again repairs a broken mix.
- Without `components` and `ratios`, `filament_add_mixed` opens the Add Mixed Filament dialog, as the
  sidebar's button does, and `filament_edit_mixed` with only `filament_slot` opens the Edit Mixed Filament
  dialog on that mix, as a click on its swatch does; both reply `blocked_by_dialog`. This is how an agent
  sets a gradient, which has no argument: the dialog names its controls for `widget_set_value`
  (`filament_1` to `filament_3`, `gradient`, `gradient_direction`, whose item 0 puts the first filament at
  the bottom, and `per_part_gradient`), and its OK makes what it shows. The gradient curve is drawn, so
  only a drag changes it.
- `filament_delete` deletes a physical filament that a mixed filament mixes as the sidebar does, without a
  question: `remap_mixed_components_on_delete()` leaves a 0 in the mix's components, the mix is broken, and
  the reply's `broken_mixed` names the broken mixes until an edit repairs them. A merge into a mixed
  filament that mixes the slot asks first, as Merge with does (`Sidebar::change_filament()`), so the call
  replies `blocked_by_dialog`, unless `break_mix` answers ahead: true merges, false refuses
  (`mixed_uses_slot`). With one physical filament left the sidebar hides the mixed filament list and deletes
  nothing (`Sidebar::delete_filament()`), so a mix is refused then (`mixed_list_hidden`) until a slot is added.
- `filament_set_colour` sets one physical slot's `#RRGGBB` through its colour button's code:
  `PlaterPresetComboBox::sync_colour_config()` and the queued `EVT_FILAMENT_COLOR_CHANGED`, which
  recalculates that slot's flushing volumes when the "Auto flush after changing" preference is on. A mixed
  filament's colour is its components' blend and is refused unless left as it is. The recalculation also
  runs when a slot is added and, depending on the preference, when a slot's preset changes, so a matrix is
  best set after the colours; a replay matches only on a datadir with the same preference.
- `filament_set_flushing` writes what the Flushing volumes and Purge mode dialogs' OK write, through the
  code `project_set_settings` runs for its keys. `flush_volumes_matrix` and `flush_multiplier` go through
  `apply_flushing_volumes()`, the Flushing volumes dialog's OK split from the dialog, then the button's scene
  reload and update. The matrix is N x N per nozzle (row = from slot, column = to slot) in config order
  over every slot, as `project_config` and the G-code header hold it, not over the physical slots the
  dialog shows. The values are before the multiplier: the header prints the matrix multiplied by it, and
  the dialog shows and edits each entry multiplied by it, which `filament_list`'s description says, since
  agents otherwise type the dialog's value as the stored one. Volumes are limited to `g_max_flush_volume` /
  multiplier as the dialog limits them; negative volumes and a non-zero diagonal are refused, and a mixed
  filament's rows and columns, which are never flushed, are refused unless left as they are, as the dialog
  keeps them. A multiplier is one per nozzle (one value sets all) within the dialog's 0..3.
  `prime_volume_mode` is what the Purge mode dialog's OK writes, followed by the update it does, offered
  only while the Purge mode button shows (printers with several nozzles per extruder or fast purge), with
  the dialog's choices.

`filament_sync_ams` takes the filament list from the AMS of the printer selected on the Device tab. As its
Speed Dial action, with no arguments, it opens the sync dialog where the project has objects. With `mode` it
does what the dialog's Synchronize now does with that choice, on what the dialog first shows, without the dialog
(`Ops::sync_filaments_from_ams()`, which the dialog applies its choice through too, `Ops::apply_ams_sync()`):
`map` maps each of the project's filaments to a slot, as the dialog's Mapping first does, and takes a
project with objects; `overwrite` replaces the list with the slots' filaments. No printer, no AMS, no
filament, or a printer of another model is refused, saying so (`Ops::ams_sync_refusal()`). The reply lists
each slot's preset and colour afterwards, and the slots whose filament was unknown and mapped to a generic
preset.

### Installing printers and filaments

What a person installs is what the Printer Selection and System Filaments dialogs list: the printer models
of every vendor the app ships, and the system filaments. Installed is what the app configuration's models
and filaments sections name, which the presets' visibility follows (`Preset::set_visible_from_appconfig()`):
the selectors offer an installed printer or filament, and `preset_list` lists the others only with
`include_uninstalled`, each with `installed: false`. The dialogs' work is in `Operations/InstallOps`, which
their Confirm calls (`Ops::apply_install_set()`) and the commands call:

- `printer_list_models` lists every printer model with its vendor, nozzles and installed nozzles, from the
  data the Printer Selection dialog lists (`Ops::printer_profile_data()`). That loads every vendor into a
  preset bundle of its own, which takes a while, so it is kept in `<datadir>/cache/wizard_profile_data.json`
  while the vendors' versions match; the first call after the shipped profiles change can reply with a job.
- `printer_install` and `printer_uninstall` add or remove the named models' nozzles and keep the rest of the
  installed set, where the dialog writes the whole set (`Ops::install_printers()`, `Ops::uninstall_printers()`):
  a vendor's bundle is installed from the shipped profiles, the presets are reloaded and a newly added
  printer selected, as the dialog's Confirm does. A reload drops the presets' unsaved changes, so while there
  are some `preset_changes` says what becomes of them, and without it the call is refused. Removing the last
  installed model is refused, and a selected printer that is removed is replaced by another installed one. A
  call that changes nothing reloads nothing. The installed filaments are written back with the printers: a
  configuration with no filaments section, which installs every filament, gets one naming every system
  filament the selectors offer, so that none is hidden. The reload can install a printer's default
  filaments where no installed filament fits it, which the reply lists; where a vendor profile lists
  default filaments of its own (`VendorProfile::default_filaments`, which only an INI bundle fills, so none
  of the shipped JSON profiles does), it also swaps a @System filament for the vendor's own. Two failures
  come after a change: `vendors_not_installed`, where a vendor's profiles cannot be copied from the shipped
  ones, leaves the installed set as it was, but the profiles copied before it stay in the configuration
  folder and, with `preset_changes: discard`, the presets' unsaved changes are gone; `presets_not_reloaded`
  has written both sections and not reloaded the presets. Each error says so.
- `filament_install` and `filament_uninstall` add or remove names in the filaments section
  (`Ops::install_filaments()`, `Ops::uninstall_filaments()`) and apply it as the app's start does
  (`PresetBundle::load_installed_filaments()`), with no full reload, so the presets' unsaved changes are kept.
  A name is a preset's, or the short name the dialog's row shows, which stands for that filament on every
  installed printer it fits; `filament_install` also takes a vendor and a filament type. A filament a slot
  of the project uses is not uninstalled. A printer that no installed filament fits gets its default
  filaments again, as the next start would give them, and the reply lists them.

None of them is in the undo history: the installed set is the app's, not the project's.

### Plugin commands

The plugin commands do what the Plugins window does, by the plugin's key, through `Operations/PluginOps`,
which the window's switches and row menus call too:

- `plugin_list` lists the window's rows as data (`Ops::plugin_rows()`): key, name, version, whether a package
  is installed and the plugin enabled (loaded), status, source (local, subscribed, mine or orphaned), a loaded
  plugin's capabilities and its error.
- `plugin_enable` turns a plugin, or with `capability` one of its capabilities, on or off, as the row's
  switches do (`Ops::set_plugin_enabled()`). A plugin turned on is loaded, after a cloud plugin that is not
  installed is downloaded and installed, and the call waits for its load to end (up to five minutes,
  `Ops::load_plugin_and_wait()`); a plugin that asks for a permission while it loads asks in its permission
  dialog (`blocked_by_dialog`), which the caller answers.
- `plugin_reload` loads a plugin again after its files changed, as the row's Reload does, and with
  `clear_cache` as Delete cache and reload; a cloud plugin is installed again, as its Reinstall does
  (`Ops::reload_plugin()`).
- `plugin_uninstall` removes a plugin without asking, as the row's Delete does once confirmed, or for a cloud
  plugin subscribed to its Unsubscribe, which also ends the subscription in the account
  (`Ops::uninstall_plugin()`). It is labelled destructive: a person's plugin and its files go for good.
- `plugin_install_local` installs a `.py` or `.whl` file by path, as the window's Install local plugin
  does, without its file dialog. A plugin of that name already installed is replaced only with `overwrite:
  true`; without it the call is refused with the question the window asks. Its Speed Dial action, with no
  arguments, asks for the file and, before it replaces an installed plugin, in Orca's own "Overwrite Plugin"
  message dialog.
- `plugin_refresh` does what Refresh Plugins does: the installed plugins are rescanned and the plugin hub's
  list fetched again, off the UI thread (`Ops::start_plugin_refresh()`). Refreshes take turns: each call and
  each pick of Refresh Plugins is a run of its own, and one asked for while others wait or run begins once
  they have returned (`Ops::RefreshRun`), so the call's reply comes from a rescan that began after it was
  asked. The call waits for its own run to end and replies with the plugins as `plugin_list` gives them.
  `job_cancel` stops the wait, not the refresh. A person's pick starts its run and returns, and its own
  notification says how it went. A run that would begin while OrcaSlicer closes does not scan.

The work the window does behind a progress dialog (a download, an uninstall, a reload) runs off the UI
thread in a command too, with no dialog: the operation plans on the UI thread, does the work on a thread of
its own and finishes on the UI thread. However the operation ends, a refusal or a throw, the Plugins window is
brought up to date, and an error after a step that changed the plugin (a cloud download before a refused load,
an unload before a failed removal) says how the plugin is left and carries its row under `plugin`. An empty
`capability` of `plugin_enable` is refused (`empty_capability`): the operation reads no capability as the
plugin itself.

### Design tab: the Design commands

The Design tab's sketches, features, reads and measurements, feature and body edits and its commit to the
plate are commands of the catalog (`Mcp/McpToolsCad.cpp`) in the group `Design`, not listed, so agents that
never open the Design tab pay in `tools/list` for their names alone. The command set itself, how the commands
are named and what each takes, is in [mcp-tools.md](mcp-tools.md), The Design tab's commands. The names end
`tool_search`' description while the catalog holds the commands (`design_command_index()`): that they are the
Design tab's CAD commands and what those cover, that one is looked up before it is called, that a command
whose schema has `silent` does what its menu item does when called with no arguments unless `silent` is true,
so `interference {}` with a body selected opens the tool and `interference {silent: true}` returns the result,
and then every key under its heading, in the table's order: Document, Sketch, Solid, Surface, Construct,
Inspect, Bodies, History, and Menu items, the commands that take no arguments and do what their item in the
right-click menu does. The headings are the generator's (`SECTIONS` in `gen_offer_table.py`, `kDesignSections`
in the header), and a command's is its entry's `section`. So an agent learns without a search that the
commands exist, that they are CAD and where each works, and reads a command's description and schema only when
it looks it up. The description stays within 4096 characters, the most a client is known to keep of one. The
commands that open a tool have Speed Dial actions while the Design tab is shown (see Speed Dial catalog).
`register_all_commands()` adds them through `register_cad_commands()` only
when the catalog is built with the CAD feature on (`CatalogOptions::cad`, which startup takes in the editor
from the value in force: the `enable_cad_feature` preference, or `--cad` for the run). With it off the catalog
holds none, and records that it leaves them out (`leave_out_cad_commands()`), a folded item's id among them,
which names a Speed Dial action: `tool_call {tool: "hole"}` is an unknown command whose error says the feature
was off when OrcaSlicer started and how a person turns it on, and `app_state` carries `cad_note`, how a person
turns the feature on (`cad_note()`, in the editor while the catalog holds no Design command or the feature is
off). The catalog is sealed at startup and the main window makes the Design page only then, so turning the
feature on or off takes effect after a restart, but for the Speed Dial's Design tab actions, which go as soon
as it is turned off (see Speed Dial catalog); a window rebuilt for a language change in between makes the page
by the new preference, while the catalog keeps the commands it started with.

The commands' names, their variant arguments and those arguments' values use the industry's terminology, as
popular CAD programs such as Fusion 360, Onshape and SolidWorks use it, so that people and agents who know one
of them learn the commands quickly. Three nouns are the app's own, as it shows them everywhere: variable, body
and feature. No prefix means the 3D model: the commands that build and change the model's bodies (`extrude`,
`fillet`, `mirror`, `pattern`, `boolean`, `split`) and the two that take any geometry (`measure`,
`interference`). A prefix marks another place: `sketch_` the sketch, with the same verbs there
(`sketch_fillet`, `sketch_mirror`, `sketch_pattern`); `surface_` the commands that make sheet bodies;
`construct_` reference planes, axes and coordinate systems; `feature_` one entry of the history; `body_` one
body as an object, named and coloured, deleted, placed, described and measured, and `body_transform`, since
bare `object_transform` is Prepare's. A name is whole words, never an abbreviation.

One command does one operation: a menu item and the argument command for its job are one command, and an item
that only picks a variant is a way of calling its command. One argument picks the variant. `type` says which
kind of thing the command makes, or which way it makes it: a shape (`sketch_draw`, where a shape drawn more
than one way names the way, `rectangle_3_point`), a pattern's layout, a hole's kind, how a plane is defined.
`operation` says what the result does to the bodies already there, in the same four words in every command
that has it, the Extrude card's: `new_body`, `join`, `cut` and `intersect`
(`extrude`, `revolve`, `sweep` and `loft`; `boolean`'s `join`, `cut` and `intersect`; `mirror`'s
`new_body` and `join`), with `extrude`'s `auto`, which joins the body the profile touches. `extrude`'s has no
default in its schema: left out, it is `auto` with a sketch or a face and `new_body` without one; `boolean`'s
has none at all, so a call that gives other arguments and no `operation` is refused. No command has both, so
the normalizer reads a guessed `type` as a command's `operation` and the other way round, where the command
has no argument of that name and the value fits (`argument_aliases()`, beside `index` for `feature`):
`boolean {type: "cut", target: 0, tool: 1}` cuts. A feature's fields (`feature_describe`, `feature_edit`,
`feature_set_expression`) keep the engine's names (`hole_diameter`, `mate_offset`, `cut_offset`), which the
project file stores and expressions bind, but for the four that hold a variant these words name: `mode` is
read and written as `operation`, `mate_kind` and `hole_style` as `type`, and `pattern_circular` as `type`,
`linear` or `circular`. What is stored is the engine's. A reply key that carries a command's name carries
this one (`align_index`, `align_conflicts`), and a feature's type name (`Mate`, `Cut`, `SurfaceFill`,
`CoordSys`) is the app's name for it in the history.

One table, the Design tab's, declares every command: `kDesignTools` in `CAD/DesignOffer.hpp`, generated from
`scripts/CAD/tool_atlas.json`, which is also the right-click menu (`docs/HLSD/design-tab.md`). An entry with a
handler or a tool block is a command, unless it is a folded item (`is_design_command()`): its id is the key and
its name the title. Of the 127 entries, 88 are commands, in five kinds:

- A merged entry is a menu item and the argument command for its job as one entry under the command's name:
  the item's slot, key, handler (`action`), accepts, mode, name, icon and hint, with the command's tool block
  (47: `sketch_trim`, `extrude`, `feature_edit`). Called with no arguments it does what its menu item does.
- A menu-only command has a handler and no block (5: `rename_selected`, `zoom_to_selection`,
  `sketch_polygon_inscribed`, `sketch_polygon_circumscribed`, `sketch_constrain_selected`). It takes no
  argument at all (`additionalProperties: false`), `silent` included.
- An argument-only command has a block and no handler (36: `describe_design`, `sketch_draw`,
  `select_geometry`); all but `measure`, whose menu item has no GUI path, have no menu item (`row` -1).
- A folded item (`"fold": {"into": "sketch_pattern", "args": {"type": "linear"}}`) is a menu item whose tool
  is a variant of the command it names: calling that command with the item's arguments opens it, so Linear
  array is `sketch_pattern {type: "linear"}` and Color `body_set {}`. It is no command, and keeps its entry
  and its id, which the menu, the keys, the toolbar, its Speed Dial action and the GUI tests use. 37 items fold
  into eight commands: `sketch_draw` (24 shapes), `sketch_set_value` (the three value items, one handler under
  three labels, each `{}`), `sketch_transform` (3), `sketch_pattern` (2), `boolean` (2), and `thicken`,
  `pattern` and `body_set` (one each).
- A greyed item has a slot and neither a handler nor a block (`pat_curve`, `align_to`): shown disabled, and no
  command.

An entry with a handler and a block may also name the arguments that choose its own menu item (`args`):
`boolean`'s Join item is `{operation: "join"}`, `thicken`'s `{type: "face"}` and `pattern`'s
`{type: "linear"}`, so that each of a command's variants opens the same way. The generator refuses a table in
which a fold names no entry, another folded item or an entry with no block, a folded item has a block of its
own or another mode than its command's item, two items of a command have the same arguments under different
handlers, an item's arguments are its command's own, own `args` sit on an entry without a handler and a block
or are `{}`, or a command has no `section` (and a folded or greyed item has one).

An entry with a tool block has a silent form, whose function `kSilentForms` binds by the entry's id; a unit
test fails for a block with no function and a function with no block. The block gives the silent form's
access, a summary of one sentence (at most 120 characters), details, typed params with units, defaults and
limits, one to three examples and whether it reports a rejected rebuild in its reply (`ok_false`); the header
carries it as JSON text (`doc`). `register_cad_commands()` makes a
command of each entry that is one (`design_command()`, through `cad_command()`), in the table's order. It is
`read_only` and `idempotent` for a `read` block, and has the entry's icon. Its description is the summary, the
details, with `ok_false` how a rejected rebuild shows, and, for a command whose menu items a call's arguments
choose, those argument sets each with its items' names (`opening_calls()`), so that a search for "polar array"
or "rounded rectangle" finds `sketch_pattern` or `sketch_draw` (`tool_search` matches keys, titles and
descriptions, not schemas). A menu-only command's description is its hint and what its call does: what its
right-click menu item does, which may open a tool, start a sketch tool, act at once or tell the person why it
cannot, with the status line in the reply, and the mode it runs in, "only in an open sketch" or "only outside
a sketch" by its menu item. Its examples are the block's, each `{title, calls: [{tool, args}], needs?}` ending
with the command it documents, where an argument `{"from_call": i, "key": k}` is the value under `k` in the
result of call `i`, and a call that gives nothing else of a command that opens a tool with no arguments is
`{"silent": true}`. Its input
schema (`cad_input_schema()`) is built from the params: a type written `"a|b"` is the list of both, `min`,
`exclusive_min` and `max` are `minimum`, `exclusiveMinimum` and `maximum`, `enum` and `default` carry over,
the unit joins the description, and a param without a default is required. A param whose default is null is
one its function reads as left out when it is null (no limit, the open sketch), so its type also admits null,
and its enum holds null: `extrude {"sketch": null}` runs the silent form on its default rectangle, as
`extrude {"silent": true}` does. A param marked `"nullable": false` keeps its type, as its function reads a
null as a value given: `thread`'s `plane`, which a thread on a plane resolves as the plane named, and
`sketch_transform`'s `angle`, `scale` and `pivot` and `sketch_pattern`'s `count`, `angle` and `pivot`, which a
type they do not apply to refuses whenever they are present. A command whose call with no arguments opens a
tool goes through `with_silent()`: `silent` is a property, and what the silent form requires is under
`"if": {"minProperties": 1}, "then": {"required": [...]}`, so the call with no arguments, the person's form,
requires nothing (`fillet {}` with an edge selected opens the Fillet tool, while `fillet {radius: 2}` needs
`edges`). The description, the silent form's, says nothing of that call, so `silent`'s says what it does and
the mode it runs in, as that call's ("Called with no arguments, it runs only in an open sketch"): the argument
form may run in the other mode, as `sketch_text`'s does on the model and `sketch_project`'s in a sketch too.
A limit is the one the tool's function or the
kernel applies to every call that passes the param, so the schema refuses no call the function would run: a
distance that may be negative, or a depth a through hole ignores, has none. A command with a silent form also
takes `generation`, never required, whose description says how face and edge ids expire. Every schema refuses
an argument it does not name (`additionalProperties: false`), so a word the command does not take is refused as
an unknown argument rather than ignored, and the call never runs with a default its caller did not mean
(`helix {pich: 3}` is refused, not built with the default pitch); the validator names an unknown argument before
a missing required one. A param in radians that also takes degrees names both (`angle` and `angle_deg`), and
the normalizer reads `index` as `feature` and `params` or `values` as `fields` (`normalize_arguments()`), the
names `feature_describe`, `feature_edit` and the tree edits also read, before the schema is checked.

One rule decides what a call of a Design command does (`opened_item()`): **a call whose arguments are exactly
those that choose one of its menu items opens that item's tool for a person, as a pick of the item does; any other
call runs its silent form.** No arguments choose the entry's own item where it has a handler (`sketch_trim {}`,
`boolean {}`); where it has none, the items folded into it with `{}` (`body_set {}` the Color item,
`sketch_set_value {}` the first of the value items). An item's own or folded `args` choose that item:
`boolean {operation: "cut"}` opens the Subtract item, `sketch_draw {type: "circle_3_point"}` arms the 3-point
circle, and with entities selected `sketch_pattern {type: "circular"}` the Polar array. The arguments are
compared as JSON values after `normalize_arguments()`, so `boolean {type: "cut"}` opens it too, and anything
beside them, `generation` or `silent: true` among them, makes the call run. `silent: true` runs the silent
form with the defaults of what the call leaves out, beside the choosing arguments as beside none. A command has
`silent` where a call with no arguments opens a tool; `sketch_draw`, `sketch_transform` and `sketch_pattern` have
none, since a call of them with no arguments opens nothing, and their closed schemas refuse `silent` beside their
choosing arguments. So a call meant to run that gives only an item's arguments opens the tool, and its
reply says so.

A call that opens a tool, an agent's or a Speed Dial action's, is the person's form (`open_tool()`, on the
item's entry). It passes `cad_gate()` with `CadAccess::None`, whose only busy rule is the rebuild's, so the
recipe is loaded and the call refused while the model is being rebuilt, and runs the item's handler in place
(`DesignPanel::mcp_run_verb()`), as the menu item and the tab's keys do: an open card does not refuse it,
since each tool decides what that means to it. Before the handler runs, `open_refusal()` refuses it in two cases
(`wrong_mode`, `wrong_selection`). An entry runs only in the mode its menu item belongs to, where the right-click menu offers it
(`offered_in_mode()`, by the menu's `sketch_map_applies()`, for which a sketch in Constrain mode is open too): a
model tool such as Shell in an open sketch is refused with "This tool works on the model: finish or cancel the
sketch first" (`mode: "model"`), a sketch tool outside a sketch with "This tool works in a sketch: open a
sketch first" (`mode: "sketch"`); the person's message comes with a `hint` naming the commands that change the
mode: `sketch_create {plane}` and `sketch_edit {sketch}` (with their arguments, as `sketch_create {}` opens the plane
picker rather than a sketch), or `confirm_tool` and `cancel_tool`, which end a sketch or Constrain mode
alike, where the busy gate refuses `sketch_finish` and `sketch_cancel` in Constrain mode. No entry is offered in both modes, and the only keys read in both, the
Ctrl+Shift ones, are no entry's handler. The tab's keys take the sketch's entries only in Sketch mode, so in
Constrain mode they reach the model's while the menu offers the sketch's: there a command does what its menu
item does, not what its key does. And a handler that only the right-click menu reaches (`btn:`, `fly:`),
whose item the menu greys for a selection it does not accept, is refused for one with "This tool does not apply
to what is selected", its details naming the selection (`selection_kind`, `selection`) and the atlas ids of
those the item accepts (`accepts`). Only an item that accepts a kind the tab can report in its mode is greyed so
(`reported_kinds()`): Boolean's items take two bodies and Rib's a sketch line, which the tab never reports as
what is selected, so `boolean {}` opens the Boolean card as its toolbar button does, whatever is selected. It
never gives the item's refusal, which is about the document (no body yet) and which the menu shows only for an
item that takes the selection. A `key:` handler runs whatever is selected, as its key does. A call that is not a
person's pick, made while a row of the Feature list is selected and nothing is picked in the view, takes that
row as what four menu items act on (`feature_row` in the atlas): `feature_edit`, `rename_selected`,
`feature_delete` and `sketch_constrain_selected` are not refused for the selection then, and their handler says
itself what it cannot do with the row; `zoom_to_selection` acts on what is picked in the view. A person's pick
keeps the menu's rule, which greys those items for that selection. The reply is `{"result": {"interactive": true,
"tool", "applies", "modal", "selection_kind", "busy", "status"}}`: the command called, whether the item accepts
the selection, whether a dialog the handler opened held the call until it was answered (the atlas's `modal`
entries but Text, whose dialog is modeless: `DesignPanel::mcp_text_dialog_open()` tells it), the selection's
kind, `mcp_busy()`'s reason after the handler ran (a card, a dialog or a sketch now open) or null, and the text
of the tab's status line (`DesignPanel::mcp_status()`). It claims nothing about what the handler did, which may
be to open a card, arm a sketch tool, act at once, or say on the status line why it cannot (Loft with fewer
than two profile sketches). A modal dialog the handler opens holds the call, which the core reports as blocked by
it, as for `project_new {}`; Text's dialog is modeless, so `sketch_text {}` replies at once with a `busy` that
names it, and its own Cancel or Escape closes it. For a person's pick, a refusal is shown in the UI's language
where its text is a msgid, as these three refusals are. `describe_design` gives `selection`, `{kind, id}`, the
selection the right-click menu would be built for, by its `OfferSel` and its atlas id
(`kDesignSelectionIds`), `picked_plane`, the reference plane picked in the view, and `feature_row`, the
Feature list's selected row.

An agent finds them as it finds any command: `tool_search {group: "Design"}` lists their names, a query
finds them among the commands with their input schemas, and `tool_search {tool}` gives one's group,
`read_only`, description, input schema and examples. `tool_call {tool, args}` runs one, its arguments
validated against its schema, and replies `{"result": ...}`, the tool's own reply; a replay step calls one
by its name or through `tool_call`, and `orca_mcp.py`'s `call_command()` from Python. An unknown name is
answered as any unknown command is: with the closest key (`invalid_argument`), or, for a Design command's name
in a run without the Design commands, why they are left out (`design_unavailable`): in the editor, the CAD feature
was off at startup, and how a person turns it on; in the G-code viewer, which has no Design tab whatever the
preference says, that a person opens the project in the editor (`CatalogOptions::editor`).

A call runs its function on the UI thread: a person's pick in place, an automated call in one UI closure
under the mutate guard, then `settle()`. Before the silent form, `cad_gate()` refuses a call by its cause, in
this order. The MCP server is up before the main window, so first come `ui_plater()`'s "shutting down" and
"still starting" refusals, as for any tool. Then, each `design_unavailable`, the G-code viewer, which has no
Design tab; the CAD feature off (Preferences > Features); and a Design panel that cannot be built (no page was made
at startup, the page is mid-build, or its build failed), whose message says to retry and then restart. The
panel is otherwise built on the first call, since in a headless session nobody clicks the tab;
`app_state.design_tab` says whether this session has the page. Then come a busy Design tab (`design_busy`, whose
`hint` says that `confirm_tool` keeps what is open when its Confirm is on and `cancel_tool` discards it, and that a
rebuild ends by itself), and a `generation` that is not an integer of 0 or more (`invalid_argument`;
`cad_generation()` takes it stored signed too, as a script's Python integers arrive) or not the model's
(`stale_ids`). Before the
generation is read, the project's recipe is loaded into an empty document
(`DesignPanel::hydrate_from_model()`), as showing the tab would.

A silent form's access (`CadAccess`, from its block) decides what `DesignPanel::mcp_busy()` lets through.
Nothing runs while a rebuild's worker owns the document; otherwise `read` commands always run, and so do the
opening of a tool and the `ui` commands, `confirm_tool` and `cancel_tool`, which act on what is open and
change the document through it. `change` and `sketch` commands are refused under an open feature card, the
Text dialog, Constrain mode or an open body Move; in Constrain mode `sketch_finish` and `sketch_cancel` are
refused too, and the tab's own Confirm or Cancel, or `confirm_tool` and `cancel_tool`, end it. A card holds a
candidate built from the document as it was when it opened, which a change underneath would overwrite, or be
overwritten by, on Confirm; the Move holds a body index that a change could point at another body. A sketch
session lets `sketch` commands through, since driving it is their point, and refuses `change` ones, as the GUI
would not let the edit land there. `select_geometry` and `body_place` are `change` commands: a click in the
view means something else under a card, a sketch, Constrain mode, a Move or the Text dialog.

A Design command that fails is an error (an `isError` result). Its `code` is a word, as the other commands'
refusals have, that names the cause, so that an agent knows from the error alone whether to change the call or the
design:

- `invalid_argument`: an argument is wrong in itself, names nothing, or names an object whose kind or geometry the
  operation does not take (a face that is not planar, edges square to the sketch plane, a plane that misses the
  body). The caller changes the arguments, or which object they name.
- `precondition`: the arguments are right, but the state of what they name, of the design or of the session is not
  one the operation takes: no body yet, a body hidden or without a shape, a sketch open or none, a datum hidden, the
  feature already first, a tool open, nothing to undo. The caller changes the design or the session first.
- `rebuild_failed`: a rebuild reported failure, with the call's change (taken back) or at the earlier state a call
  reads (`body_describe {before}`); its message is the rebuild's reason, a kernel exception that
  `CadDocument::recompute()` caught included.
- `kernel_failure`: a CAD kernel error reached the command's guard, wherever it was raised; its message starts with
  `OCCT: `.
- `export_failed`: `export_step` could not transfer or write the STEP file.

The line between the first two is between geometry and state. Geometry is what the named objects are and where
they lie, which the call chose, even where a modeling step could make it fit; state is what the Design tab tracks
apart from the geometry (whether an object exists, is shown or has a shape, a feature's place in the list, what is
open or selected), which a call cannot choose. The silent forms throw `CadPrecondition` for state, `CadKernelFailure`
where OCCT reports it could not compute, and plain exceptions for the arguments, which `run_guarded()` and
`add_feature()` tell apart; a refusal of their own (a `ToolFailure`) keeps its code, and one that names none reads as
a plain exception does. The `mcp_` entry points of `DesignPanel` that only these commands call (`mcp_select()`,
`mcp_place_body()`, `mcp_place_body_on_face()`) throw `CadPrecondition` themselves for their state refusals, as
`select_geometry` does for a body the view takes no pick on (`DesignSketchTool::body_takes_pick()`: no shape shown,
hidden, or another body the only one picked). Whatever the cause, the design is as it was, so `replay_export` drops the call and a replay never repeats
a call that did nothing.
The exceptions are the silent forms whose block has `ok_false`, whose descriptions say so (`boolean`,
`mirror`, `shell`, `draft`, `rib`, the surface tools, `delete_face`, `import_step`, `set_variable` and others):
they report a rejected rebuild in their reply, `{ok: false, error}`, with no code, after the same rollback, so such
a call is journaled and replays the same rejection; their `ok: false` can mean more than that per command
(`sketch_finish` of a re-opened sketch that was not replaced). `extrude`, `revolve`, `sweep`, `loft`, `hole`, `thread`, `split`,
`fillet`, `chamfer`, `thicken`, `pattern`, `align` and `sketch_draw` fail as errors, so none of them fails two
ways depending on its arguments. The feature tools that fail as errors add through `add_feature()`: one
checkpoint, a rebuild, and on failure `CadDocument::rollback()`, which restores the checkpoint without leaving
the refused recipe on the redo stack and with the redo branch the checkpoint cleared, then "Edit rejected" on
the status line as for a refused GUI edit. Features made through a Design command are named as the cards
name them ("Extrude 3"), from the panel's own counter.

`generation` guards face and edge ids within a session, but its value depends on the session's
history, so the journal records a Design command's call without `generation`, and with `silent`, which it
adds to a call that ran when the call without `generation` would open a tool, so that the replay runs the
silent form rather than opening the tool; a replay rebuilds the same ids. A call that opens a tool is
journaled as it was given, and its replay opens the tool again.

The plane-taking commands (`sketch_create`, `extrude`, `revolve`, `hole`, `thread`, `split`, `pattern`,
`mirror`, `helix`, `sketch_project`, `sketch_text`, `sketch_insert_svg`) share one resolver,
`cad_resolve_plane()`: `"XY"`, `"XZ"` or `"YZ"` through `modeling_origin`, `{"datum": i}` for a Plane
feature, or `{"body": b, "face": f}` for a planar face. A face plane for sketching is
`CadDocument::plane_of_face()` (outward normal); for drilling it is
`CadDocument::inward_plane_of_face()`, the Hole card's plane (normal into the body, x along the
longest edge), and the hole or thread goes into that face's body. Their replies carry the `frame`
used. Because a sketch's base plane passes through `modeling_origin` like every feature's, a sketch
lands where the features drawn on the same plane land, and the coordinates in a replay are relative
to the modeling origin on any printer.

`describe_design` reads the document: the features, each with whether it is `suppressed` and the fields an
expression drives (`bound`), the variables with their values (all null, with `variables_error`, when one does
not evaluate), the bodies with the feature that made them, whether their eye is on, the colour the canvas
draws, whether they were moved and their `placement` (null where the model has the body, else three rows of
four numbers, the rotation and the translation in mm in the world frame), the undo and redo depth, the
Commit button's mode (`commit_mode`), the aligns that cannot hold with those before them (`align_conflicts`),
the document's `auto_close_loops`, and the selection (above). `feature_describe {feature}` reads one feature as
its card shows it, from one per-type table, `feature_card()`: `params`, the `editable` ones named as the
`CadFeature` members (the names `feature_set_expression` binds) but for the four variant fields, with the
`ranges` of the card's spin controls, `expr`, the structure the card keeps in `refs` (feature and body
indices, face and edge ids), a sketch's plane, entities and constraints (snake_case types, point references
with a role, angles in degrees), `used_by` / `bodies_made`, and, for imported Text or SVG art, how many
regions it has (`regions`), how many closed loops each region has, its outer loop plus its holes
(`region_loops`), and the area each region fills as placed, in mm² (`region_areas`). `used_by` comes from the
kernel's own reference visitors, `CadDocument::for_each_feature_ref()` and, for a datum plane,
`for_each_datum_ref()`, the lists that deleting and moving a feature remap.

A feature's body indices and face and edge ids are as of the feature: they index the model as it stood just
before it, which is how the feature stores them and how the rebuild replays them. A Plane's, an Axis's and a
Coordinate system's are the finished model's, where the engine reads them (`for_each_body_ref()`).
`feature_describe` says which in `ids_of` (`"before"` or `"finished"`), and lists in `bodies_before` the
bodies that existed just before the feature (`{index, name}`), which `CadDocument::recompute()` records as it
rebuilds (`CadDocument::bodies_before`, not saved): it is the last rebuild's, and null where that rebuild did
not reach the feature. `body_describe {body, before: <feature>}` reads a body as it stood just before that
feature, by replaying a copy of the history cut there (`cad_document_before()`, on
`CadDocument::history_before()`, which leaves out the undo and redo stacks and the display meshes), as the
cards do to list bodies when they edit. Those are the ids `feature_edit` takes in `refs`, for every feature
but a Plane, an Axis and a Coordinate system. A card that edits a feature reads a face or an edge picked again
the same way: the viewport's pick is fed the model before the feature while the card is open (not for a
Plane, an Axis or a Coordinate system), each of its bodies where the view draws the body it became (its
placement, matched by identity, as its visibility is), and the Hole and Thread cards read the picked face's
geometry from that model too.

`feature_edit {feature, fields, refs}` (also read as `index` and `params` or `values`, the names agents
reach for, as `feature_describe` reads `index`) is a card's Confirm in edit mode: one checkpoint, then
`CadDocument::replace_feature()` with a copy whose fields `cad_apply_feature_fields()` checked against the
same table, so it takes only what the card edits (numbers within the card's range, and its choices), and whose
references `cad_apply_feature_refs()` changed, under the names `feature_describe` reports. `refs` reaches what
the feature's card can pick again when it edits: a revolve's axis, a sweep's path, a loft's profiles, a
boolean's two bodies, a mirror's, split's or helix's plane, a rib's line, an align's coordinate systems, a
fillet's or chamfer's edges, a shell's, draft's or thicken's face, a hole's or thread's plane, the picks of a
plane, axis or coordinate system, the faces of a Delete Face, the target body. An extrude's profile and target
stay fixed, as its card keeps them. A feature index must name an earlier feature of the right type, a body
index or id one the model before the feature has, and `generation` guards the call. For a Text feature
`fields` takes the words, height and font (a face name, or the stored value `feature_describe` reports), with
bold and italic beside a face name, and outlines it again; for imported art, its position as well as its
scale. A field an expression drives is refused, since the next rebuild would overwrite it, and so is an end
condition that needs a pick the feature does not hold. A rebuild the kernel refuses fails the call after
`rollback()`. The reply is `changed`, each field and reference with its old and new value, and `plane`, the plane
the feature builds on, which `feature_describe` reports beside them, where it changed (a mirror's, a helix's or
a split's re-pointed). Numeric fields go
through `CadDocument::set_numeric_field()`, the allow-list expressions use, so a literal edit and a binding
reach the same fields. `feature_set_expression {feature, field, expr}` binds a field, and an empty `expr`
removes a binding, as `set_variable`'s removes a variable, as the Variables panel's Clear and Remove do; a
binding on a field the evaluator does not know, or removing one that is not there, is refused.

A sketch goes from a session to a solid in typed calls. `sketch_create {plane}` opens the session through
`DesignPanel::begin_sketch_session()`, the code a plane or face pick runs, without the tool menu that
follows the click; `sketch_create {}` is the Sketch menu item, which with nothing picked waits for a plane.
`sketch_finish` is the sketch's ✓, whose commit handler adds the Sketch feature and solves its dimensions; the
reply reads the committed feature back: `sketch_index`, its name and its loops. A new sketch left empty is
refused before anything changes and the session stays open, because an exported replay leaves the failed call
out and its next sketch call needs that session. `sketch_edit {sketch}` re-opens a committed sketch in the
session, as Edit on its row does (`DesignCanvas::edit_sketch()`, `DesignSketchTool::begin_edit()`), and the
`sketch_` commands then act on it. `sketch_finish` replaces it in place (`replace_feature()`), in one undo
step, rebuilds what is built on it and replies `replaced: true`; a replace the rebuild refuses, or a re-opened
sketch left empty, ends the edit with the design as it was and replies `ok: false` with `replaced: false`,
`sketch_index` and `error`, so the call is journaled and its replay ends the same edit. A click inside one of
the sketch's loops, the Text dialog (`sketch_text {}`) and Constrain mode (K) finish the sketch first, and a
re-opened one they cannot put back stops them there, with the reason on the status line, so nothing opens on
the sketch as it was. `sketch_cancel` drops
the changes without the dialog a person gets and resets the panel's edit state. `sketch_edit` is refused while
a sketch is open, and for text, imported art, a Project sketch and a sketch not drawn from entities, each with
its reason. An extrude of one loop keeps its own copy of that loop and does not follow the edit.
`sketch_describe {sketch}` describes a committed sketch as the open one is described, through
`DesignSketchTool::loop_report(entities)`. Its loop indices are those `extrude {loop}` and
`select_geometry {sketch, loop}` take: a region lists the loops inside it under `holes`, and each of those is
a loop of its own. `extrude {sketch}` and `revolve {sketch}` are the cards' Confirm on a committed sketch.
Extrude takes the whole sketch (`add_extrude`) or, with `loop`, one region and the loops inside it, the list
a loop picked in the viewport hands the card (`add_extrude_entities`); `body` names the body to work on in
every form. Revolve's axis is a line of the sketch (`axis_entity`), else the sketch's only centerline, which
the card also offers first (`DesignPanel::default_revolve_axis_entity()`), else a plane axis.
`cad_sketch_extrude()` and `cad_sketch_revolve()` check every argument before the checkpoint and return the
call that adds the feature. `surface_revolve` takes `axis_entity` and `flip` as `revolve` does, with its
checks (`cad_surface_revolve()`), and `feature_edit` edits both.

Inside the session, the sketch's own tools are reached without a pointer through
`cad_sketch_call()`, which needs only the live `DesignSketchTool` and checks every argument before
it changes anything. `sketch_draw {type, ...}` draws one of nineteen shapes from its fields, or a list of
shapes as `entities`, each with its `type`: `line`, `polyline`, `rectangle_2_point`, `rectangle_center`,
`rectangle_3_point`, `rectangle_rounded`, `circle_center`, `circle_2_point`, `circle_3_point`, `arc_3_point`,
`arc_center`, `arc_tangent` (from the end of `entity`, the line or arc it continues, one the sketch has or one
an item before it adds, the one added last when it is left out, leaving along its direction there), `slot`,
`slot_arc`, `ellipse`, `ellipse_arc`, `spline`, `polygon` (by its circumradius or across the flats, the tool's
inscribed and circumscribed fits) and `point`. Their fields are those `sketch_describe` reads back (`radius`,
`rminor`, `rotation`, `ctrl`); each shape is turned into the clicks its tool takes and built by the tool's own
builder and shape grouping (`DesignSketchTool::add_shape_scripted()`), so one undo takes it off whole, or a
polyline's last line only, as their tools leave them, leaving the tool's sticky polygon and construction
settings alone. Each shape carries the constraints its tool gives it, at any slant. Its constraints are
inferred exactly, so that none moves what is given; as exact inference records a relation only where rounding
leaves it exact, the ones the tool binds inside the shape are added where it left them out: a
`rectangle_3_point`'s and a square's right angles, an `arc_tangent`'s tangency to a line it leaves, a `slot`'s
tangencies and equal ends, a `slot_arc`'s equal ends, and a `rectangle_rounded`'s level and upright sides,
tangencies and equal corners, batched as the tool's inference batches them. A constraint between a shape and
the sketch's axes or what is already in the sketch, or between plain lines, is inferred only where it holds
exactly, so a square on the axes, whose sides lie a rounding step off them, is level only on a side lying
exactly on one, and right angles hold the others: as many constraints and degrees of freedom as its tool's,
some of another kind. Each item of `entities` carries its own fields, so a shape's field beside the list is
refused, and a `construction` beside it is each item's that gives none of its own. A refused `sketch_draw`
leaves the sketch exactly as it was. Angles are radians, or degrees
under a `_deg` name.
`sketch_fillet`, `sketch_chamfer`, `sketch_trim`, `sketch_extend`, `sketch_transform {type: move | rotate |
scale}` and `sketch_pattern {type: linear | circular}` run the edit tools' own apply steps through scripted
entry points, so they keep the constraint upkeep a click gets: a fillet arc bound coincident and tangent to its
legs, constraints an op invalidates released (Fillet and Chamfer let go of the Symmetric constraints on the two
ends they trim, Trim and Extend the line's), pattern copies bound to their source. They leave the armed tool
armed, and are refused while a gesture or a value field is pending, which the tool's apply step would take
over. `sketch_trim` and `sketch_extend` pick the entity nearest `at` within a `tolerance` in mm, where the
scissors' click uses a pixel radius. `sketch_mirror {axis}` mirrors about a line of the sketch, `"x_axis"` or
`"y_axis"` through the Mirror tool's own Confirm, which binds each end of a mirrored line or arc Symmetric to
its source's opposite end about the axis; `axis_a` and `axis_b`, two free points, bind nothing. `sketch_offset` of one
entity binds its copy Parallel or Concentric, as the Offset tool does (`DesignSketchTool::offset_scripted()`),
and that copy gets no inferred constraints. Both replies list the constraints added. `sketch_constrain` is a
CONSTRAIN bar button and `sketch_dimension {type: distance | distance_x | distance_y | angle | radius |
diameter}` the Dimension tool: with `entities` (else the selection) the bar's planner,
`plan_entity_constraint()` with `write_plan_value()` in `SketchEngine`, shared with the panel, chooses the
points, and the value it would ask for defaults to the current one; with `refs` the references are taken as
written, in the form `sketch_list_constraints` and `feature_describe` report them (`{entity, role}` for a
point, `{entity}` for a whole entity, `"origin"`, `"x_axis"` and `"y_axis"` for the sketch's implicit
references), which also reaches the types the bar has no button for (lock, point on line or object). A
constraint the sketch cannot solve with is rolled back and the call fails; a refused pick names the planner's
reason as a code. `sketch_delete_constraint` is a constraint row's delete button; the later indices move down.
`sketch_list_constraints` reads the open sketch's constraints, or a committed one's by `sketch`.
`sketch_project` with a sketch open, new or re-opened, adds a body's edges to it as construction entities,
which do not follow the body; with none open it adds a Project feature, a sketch on `plane` that follows the
body. `sketch_insert_svg {path, scale, position}` adds an SVG file's outlines to the open sketch as lines, as
Insert SVG does there (`add_imported_regions`), and with no sketch open makes an art feature on `plane`.
`sketch_text {text, font, bold, italic, height, plane, position}` makes a Text feature on the model: the
dialog's outline (`DesignPanel::mcp_outline_text()`) and push, one undo step, no dialog and no placement
step, a font left out being the face the Text dialog would start from. Both decide every refusal (a font not
installed, text or a file with no outline) before anything changes. Live sketch edits are not on the Design
undo stack until the commit, as in the GUI.

`body_describe` reads a body from one enumeration (`faces_of()`, `edges_of()`, whose ordinals are
the ids every other Design command takes), and with `vertices` its vertices (`{id, p}`, numbered as
`TopExp::MapShapes()` numbers them, as faces and edges are). A face's kind is its OCCT surface type, from
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
`faces_total`/`edges_total` count what matched. `fillet` and `chamfer` take edge ids in `edges`, or one of
the card's Edges choices, `"top"`, `"bottom"`, `"lateral"` or `"all"`, through the engine's face-group
overloads.

`measure` keeps `distance`, between the refs' representative points (a face's centroid, an edge's
middle sample), and adds `min_distance` with the closest points (`GeometryEngine::min_distance()`,
OCCT's `BRepExtrema_DistShapeShape`), and `parallel_gap` across two parallel planar faces. Each ref
may name its own body, and `{"face": f, "axis": true}` stands for a cylinder's axis over the face's
length, so a hole's height or spacing is one call.

`hole` drills a face plane's own body, or the body `body` names, else the last body, and takes `at` instead
of `x`/`y`: a world point projected onto the plane along its normal (the reply's `off_plane` says how far), or
a cylindrical face or circular edge whose axis the hole shares, which replays on any printer. Its `type` is
`simple`, `counterbore` or `countersink`, the seat for a screw head, a counterbore sized by `cbore_*` and a
countersink by `csink_*`, each refused beside another type, or by the standard with `designation` (a
clearance size such as M6, ISO 273 or ANSI), which gives the diameter in its
place. `cad_hole_placement()` resolves all of it before the checkpoint. `extrude {face}` is the Extrude card's
push/pull of a picked planar face (`add_extrude_face`), joined to that face's body by default.
`construct_plane` is the Plane card's Confirm: every construction (offset, angle, midplane, tangent, two
edges, coincident) with its references as `{body, face}` or `{body, edge}`, checked by
`CadDocument::datum_plane_refusal()`, the card's own check, before the checkpoint; a parameter the
construction would ignore is refused. `thicken {type: face}` offsets one face of a body into a plate and
`thicken {type: surface}` thickens a sheet body; left out, `type` is `face` when `face` is given. `pattern`'s
`type` is `linear`, `circular` or `curve`, the last along a sketch's `entity`, and what one type reads is
refused beside another: `spacing` and `dir` go with `linear`, `angle` with `circular`, `plane` with both,
`sketch` and `entity` with `curve`. `align {type, cs_a, cs_b}` is
the Mate card's Confirm, its five types `fastened` to `cylindrical`, and `list_align_types {cs_a, cs_b}` says
which fit that pair and why each other one does not, as the card dims them (`CadDocument::mate_options()`).

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
`split {face}` splits along a face's plane, and `split {}` is the Cut menu item. A plane clear of the body is
refused, since the kernel would leave one piece empty. The reply names the pieces: the upper one keeps the
body's index and the lower one follows it.

Thread, sweep and loft rebuild as their cards do, through `DesignPanel::recompute_guarded()`: the
kernel works on a worker thread while the UI thread waits in `wxYield()` behind a progress dialog
that appears after 300 ms. A long thread or sweep takes seconds, and meanwhile the window paints
and inspection tools answer from inside that loop: `app_state`, `widget_list` (the dialog is a
non-modal `wxProgressDialog` titled "Design") and `window_screenshot`, of the window or of the dialog's own
ref. On Windows the dialog is the system's task dialog, run on a thread of its own, so it is no wx
window and `widget_list` does not list it; the other tools answer as elsewhere. Mutating calls,
another Design command and `undo` among them, are refused by `UiInvoker` as running inside the
rebuilding call ("Busy: thread is still running"), and `mcp_busy()` refuses every Design command while
a rebuild's worker owns the document, the GUI's own rebuilds included. A call past the reply budget
becomes a running job.

`select_geometry` sets the selection a click would set, in the view and in the lists, so that a menu item's call
with no arguments, `zoom_to_selection` and `rename_selected` have something to act on: a body as a click on
its Bodies row selects it, a face, edges or a vertex of a body as a click in the view picks them, a reference
plane while a click could pick one (the Origin row shows the planes, or Sketch waits for its plane, where
picking it opens the sketch), a committed sketch's loop, numbered as `sketch_describe`'s `closed_loops` and
only while the view shows it (an extrude of the whole sketch hides its loops and an extrude of one loop hides
that loop, and a hidden loop is refused, naming the extrude), or a feature as a click on its Feature list
row; a call with nothing clears it. A vertex's id is its place among the body's vertices
(`cad_body_vertices()`). The reply is the selection as `describe_design` reports it, and the tab takes the
next call once it has replied. `confirm_tool` confirms what is open: the Text dialog as its OK does (a new
text goes into the design with its placement then open), else as Enter does (`tool_confirm()`), in the
order Confirm takes it: a value field, a body Move, a text or art placement, a feature card, a sketch,
Constrain mode; it is refused when nothing is open, while Sketch waits for its plane, while the Text dialog's
OK is off, with the dialog's size line as the reason, and while Confirm is off, with the status line as the
reason, and replies what was open, whether it took (`ok`), the feature added
or edited, what is still open and the status line. A card's Confirm that adds a feature whose rebuild fails
takes the feature back, and an add the card refuses, or that adds nothing, abandons its checkpoint: either way
no undo step is taken and the card stays open with the reason on the status line, as a person's Confirm
leaves it.
`cancel_tool` cancels what is open as the card's Cancel does (`tool_cancel()`), the Text dialog as its Cancel
does, an open sketch discarded without the dialog and a re-opened one kept as it was.
`body_place {body, move, rotate | face | reset}` is
the Bodies row's Move and Place on Face: where the body goes for Commit to Plate and Export STEP
(`m_body_xform`), turned about its centre as the Move / Rotate card turns it, a face laid on the bed as
`place_on_face()` lays it, or put back where the model has it. Like the tools it is outside the history, with
no undo step, and the project file does not keep it; a placement follows its body when the bodies are
renumbered, by the transient `uid` of the feature that made it (`CadFeature::uid`).

`undo` and `redo` are commands of The app, each with an action of its own (`McpToolsApp.cpp`), shared by the
Design tab and
the project, and act on the history of the tab shown, as Edit > Undo and Redo and the top bar's buttons
do: the Design tab's while `MainFrame::shown_design_panel()` is not null, else the project's.
`history`, `design` or `project`, names one for automation, which acts whatever tab is shown: a Design
command runs with Prepare in front, so a replay that drives the Design tab from there would otherwise
undo a project change. The Design history's is `cad_undo_redo()`: Edit > Undo and Redo with the
Design tab shown (`menu_undo_redo()`, behind the same `menu_can_undo_redo()` gate), the Design
history, or inside a sketch session the sketch's own, which takes off or puts back the last drawn
shape, past `cad_gate()` with sketch access, so it runs in a sketch session and is refused under an
open card, Constrain mode or a pending body Move, and during a rebuild. A refusal is the call's error
and never falls through to the other history. The project's is `Plater::undo()` and `redo()`, the Edit menu's,
which step past the snapshots that do not modify the project to the nearest one that does and load it, and leave
the active snapshot where it is when there is none, while a background job holds the model, or (undo) in the
assemble view for a record that is not a gizmo's. The call counts the active history's undo and redo items before
and after (`Plater::undo_redo_string_getter()`), which change only when the active snapshot moves, and is refused
with "Nothing was undone in the project's history" (or redone) when neither changed. Success means an entry of the
history was taken back or put back, as the Undo list shows it, not that the project differs afterwards: the plater
classes a snapshot by its type, and an action can leave one that changed nothing (renaming an object to the name it
has). That is an automated call's, `action_run` of its action
included: a person's pick from the Speed Dial does what the Edit menu's item does, and where the menu greys
that item it does nothing and shows nothing: on the Design tab's history through `menu_undo_redo()` where
`menu_can_undo_redo()` allows, and on the project's only where `Plater::can_undo()` (or `can_redo()`) holds,
so not from the Preview or while a job runs, since `Plater::undo()` itself acts wherever it is called. The reply is the action's, with `history` and, for the
Design tab's, `result`, `cad_undo_redo()`'s reply; with the CAD feature off, or in a build without CAD,
`design` fails ("The CAD feature is off"). Their examples, which call Design commands, are only in a
catalog that holds them. The Design history is not saved with a project.

The Feature tree's and Bodies list's edits are split from their handlers into index-taking
`DesignPanel` cores that the handlers call after resolving the selected row, so `feature_delete`,
`feature_move` (one place up or down), `feature_suppress`, `feature_rename`, `body_set` and
`body_delete` run the GUI's own code without selecting a row, and queue no face highlight of their
own. Each tree edit is one Design undo step; when the kernel refuses one (a consumer moved above
its input, a feature brought back without the body it works on, a delete a later feature cannot survive),
the kernel has already put the features back, `CadDocument::abandon_checkpoint()` drops the
checkpoint and restores the redo branch it cleared, and the call fails with the kernel's reason.
A feature rename is its own undo step. `feature_suppress {feature, suppressed}` leaves the feature out of
the rebuild, as its row's eye does, or brings it back. `body_set {body, name?, color?, visible?}` checks every
argument before it changes anything; a body's name and colour are saved in the recipe and are not
in the undo history, and its visibility is view state that decides what `commit_to_plate` sends.
`body_delete` deletes the feature that made the body (`CadBody::source_feature`). Deletes, moves and
suppressing renumber features and bodies, so the replies carry the new counts and `generation`.

`commit_to_plate {mode, replace}` is Commit to Plate: `DesignPanel::commit_to_plate()`, which the button
also runs, sends the visible bodies (a lone body even when hidden) where they are placed,
as one Prepare undo step, and shows Prepare. A body named in the Bodies list names its object, or
its part of an assembly; otherwise the design's labels ("Design Body", "Design Assembly") are used.
`mode` applies to this call only and leaves the split button and `design_commit_mode`, the user's
choice, as they are; given none, the call uses the button's, which `describe_design` reports as
`commit_mode`. `replace` lists Prepare objects to delete afterwards through the Object
List's delete path, under one outer snapshot so the commit and the deletions are a single undo
step. It refuses before anything changes: an index out of range or repeated, an object that is
part of a cut (deleting one asks the person first), an open gizmo, a design with no body, or every
body hidden. The reply gives the new objects' `object_list` indices after the deletions.
`commit_to_plate` is the one Design command that changes Prepare's model. It needs no refusal of its own:
the mutate guard a Design command's call runs under keeps it out of a modal dialog and out of another
call's nested event loop.

`new_design {discard_changes}` is New Design without its confirmation: `clear_document()`, which
also clears the Design undo history and leaves committed objects on the plate. A design with
features, bodies or variables needs `discard_changes: true`. `export_step {path, overwrite}` is
Export STEP… without its file dialog (`DesignPanel::export_step_to()`, shared with it): every
body as native B-rep where it is placed, under the server's output-path rules.
`import_step`, `import_mesh` and `body_compare`'s `reference.step` take an absolute, existing
path, like every file the server reads.

`view_set {tab: "design"}` shows the Design tab, and while it is shown `camera` points the Design
view as its own controls do: a direction through `DesignCanvas::set_view()`, `default` its Home
view (iso, fitted), `fit_objects` its `fit_view()`, `fit_selection` the pick's box, refused when
nothing is picked, and `sketch_normal` straight at the open sketch, refused with none open
(`DesignCanvas::view_normal_to_sketch()` does nothing without one). `look_at`, `orbit` and `zoom` act on the
Design camera as on Prepare's, since the tab swaps it into the plater's slot (see Camera, under GUI
automation). The camera is applied in a second UI closure after `settle()`, because the tab takes the shared
camera only when its page shows (`DesignPanel::on_tab_shown()`). With `tab: "design"`, `section {on, z,
keep}` sets the section view, a horizontal cut at world `z` (left out, the model's middle) that keeps the
lower or the upper half, and `origin_planes`, `world_axes`, `origin` and `bed` show or hide the origin helper
planes, the world axes, the Origin row's pickable planes and the bed, each through a setter by value; the
reply's `design` reads them back. They are refused during a rebuild, and without `tab: "design"`. `fit_bed`,
`canvas` and the Preview fields are refused with the Design tab, since they show Prepare or Preview. The
Design view is a canvas target for `widget_list`, the input tools and screenshots (see GUI automation and
Screenshots).

A change made only in the Design tab marks the project modified, as other project data outside
Prepare's undo stack does: `sync_recipe_to_model()` sets the plater's dirty flag besides the
backup's change mark. So `app_state.project.dirty`, the window title and `project_open` /
`project_new {discard_changes}` agree with the unsaved-changes prompt that Open and New Project
show. A project holding only a design loads no objects, and `Plater::load_project()` names it after
its file all the same, since a loaded recipe counts as a loaded project.

`tests/slic3rutils/test_mcp_cad.cpp` (`[Mcp][CAD]`) checks the Design commands as the catalog holds
them (one per command entry of the Design table, none for a folded or greyed item, a silent form for every
block and a block for every silent form, which entries have a Speed Dial action and an icon, the folded items'
actions, their schemas, descriptions, sections and access, and every example's calls against their commands' schemas), that
none is registered with the feature off, which call opens a tool and which runs, how a call is journaled, the
plane resolver, the rollbacks, and the tools through their public seams (`cad_sketch_extrude()`,
`cad_sketch_call()`, `cad_body_describe()`, `cad_thread()` and the rest), which take a `CadDocument` or a
bare `DesignSketchTool` and need no Design panel; `scripts/CAD/test_gen_offer_table.py` checks the
generator's refusals. `tools/mcp/test_cad_replays.py` runs on an instance that `tools/mcp/orca_mcp.py launch
--cad` starts, in parts: the replays `tools/mcp/tests/cad_sketch_to_solid.json`, `cad_sketch_edit.json`,
`cad_topology_targets.json`, `cad_features.json`, `cad_doc.json` and `cad_canvas.json` (the Design view as a canvas
target, its key hook, and the dirty flag), each with `replay_run`; a session recorded with failed calls and
`generation`s, exported and replayed to the same features and mass properties; the window during a guarded
rebuild; and every example, `undo`'s and `redo`'s too, each on a fresh design, failing an example when a call fails
or its result has `ok` false, an error, or is the person's form. `tools/mcp/test_cad_failed_job.py` exports a
session whose call failed after its reply budget (a hole that misses a slow-to-rebuild coil), which leaves it out,
and replays it.
`tools/mcp/test_cad_commands.py` checks in the running app what the commands' arguments do where the Design
panel's own functions answer: the words, `sketch_draw` against each shape's tool, `sketch_edit`, the
gaps' argument forms, `feature_edit`'s `refs`, `select_geometry`, `confirm_tool` and `cancel_tool`,
`body_place`, the Boolean card a call opens, and a folded item's action picked from the Speed Dial and
replayed. `tools/mcp/test_cad_sketching.py` and `test_cad_context_menu.py`
drive the Design tab through the GUI tools as a person does (canvas gestures,
typed values, the offer menu, file pickers answered by path rules) and read the result back with `read`
Design commands.

## Speed Dial catalog

`action_search` and `action_run` serve the Speed Dial's `ActionRegistry`, which holds what a person picks
there, its actions: the commands' actions, plates, recent projects, plugin scripts and pages, and, on request,
settings. A command's actions are those of the Speed Dial's actions that run that command, each with fixed
arguments. Most of them match a menu item; Go to tab... and Go to layer (percent) collect a value in the
Speed Dial and match none. A command's action is one of its `actions` (`NativeCommand::actions`, made with
`NativeCommands::action()`: `{key, title, group, icon, description, args, input?, input_arg?}`, which the
command reference writes), or the command's own action (`has_own_action`), whose key is the command's. A menu
item and the command that does its job are one command, so the items of one job are actions of one command,
each with the arguments that pick its variant: Mirror > Along X Axis is `object_mirror {axis: "x"}` under the
id `obj_mirror_x`, Export G-code is `project_export {format: "gcode"}` under `export_gcode`, and the nine view
directions are `view_set {camera}` actions. Eight commands, whose names are their menu items' ids, have an
action of their own: `plate_add`, `plate_delete`, `plate_duplicate`, `plate_rename`, `filament_add`,
`filament_delete`, `undo` and `redo`.

An action keeps what a person sees of its menu item: its id (`orca_command:<key>:orca`, the item's id as the
key, whatever its command is named), its title, its Speed Dial section (the item's `group`, or the command's
`action_group` for its own action, since a command's `group` is its place's heading and one command's actions
come from several sections: `export_gcode` is in Slice & Export, `export_stl_multi` in Export), its icon and
the value its second phase collects (`input`). A person's pins and run counts are stored under the id, so the
ids of the menu items a released build stored stay the ids of actions. `tests/data/speed_dial_actions.json`
holds the 129 actions (key, title, section, icon, input), and the unit test "every Speed Dial action of today
is still a command's action" finds each in the Speed Dial with that presentation and no other; "a menu item's id
is an action's key, not a command" holds that no menu item's id is a command's name.

The commands live in one `CommandCatalog` (`CommandCatalog.hpp`, which includes no wx), built once at startup
by `NativeCommands::build()` before the automation runtime starts, and sealed: from then on it never changes and
any thread reads it (`NativeCommands::instance()`, `find()`). A process that never calls `build()`, such as
the unit tests and the stdio bridge, gets it built on first use; nothing in the GUI process reads it before
startup builds it, the automation runtime included, which `GUI_App` starts after `build()`.
`NativeCommands::speed_dial_actions()` returns the actions a person picks whatever tab is shown, in the
catalog's order, a command's own action first and then each item of its actions, and `ActionRegistry` makes an
`AppAction` of each, once, at the snapshot: these actions depend on no tab or mode. `make_action()` makes it: a
`CommandAction` with the action's id, its title (`NativeCommands::action_title()`, translated in the command's
`title_context()`), its section and icon, the action's description as what automation reads, and its `args`
as the `AppAction`'s `preset`, so a pick runs the command with the action's arguments (`action_call_args()`)
through `NativeCommands::run()` with a person's context, with the MCP server on or off. An action shows an agent
no arguments, but an action that collects a value: that one takes the value alone, under its `input` token,
typed as the command's argument it goes to. `find_command_action()` maps an action's id to its command and
action.

The Design tab's actions are the exception: `speed_dial_actions()` leaves group `Design` out, and
`ActionRegistry::materialize_design_actions()`, which `materialize_live()` runs beside the plate and
recent-project families for `snapshot()`, `describe()` and `find_live()`, holds an action for each entry of the
Design table with a handler that is offered in the tab's mode, in the table's order (`Mcp::design_actions()`,
by `Mcp::offered_in_mode()`), while `MainFrame::shown_design_panel()` is not null and the CAD feature is on,
and none otherwise, so the Speed Dial has the sketch's actions in an open sketch, the model's outside one, and
no Design tab action on another tab, nor once the feature is turned off: the tab stays until the restart, but
`cad_gate()` would refuse every pick of its actions. A Design command with a handler of its own has its action
under its key (`orca_command:sketch_trim:orca`); a menu item folded into a Design command keeps its action
under the item's id (`orca_command:sk_array:orca`), one of the command's `actions`, with the item's title and
icon, and a pick runs the command with the item's arguments, as `tool_call {"tool": "sketch_pattern", "args":
{"type": "linear"}}` does. 89 Design tab actions in all: 52 commands' own and 37 folded items'. A Design
command's own action, unlike any other command's, takes its command's arguments. For a Design command, an
action's title is the entry's name translated with the `Design` context (`title_context()`), as the right-click
menu translates it, and its icon is the entry's. Every place that shows a person a command's title translates
it so: the Action Replay window's step and its summary line, a person's job notices ("Started", "Finished"),
the tool list of the replay and script editors, and a recording's caption of a call (`CaptionBoard` keeps the
context beside the title). A call of a command with exactly an action's arguments is titled as that action
wherever a person is shown it (`call_title()`, `action_with_args()`, which skips an action that collects a
value): a recording's caption (`McpRecorder::call_started()`, through `shown_title()`, which also titles
`action_run` of an action as that action) and an Action Replay step, whose description names the action
(`describe_step()`'s `what` has `action`, which `replay_what_text()` titles). Every Design entry's name is
marked for translation in that context (`gen_offer_table.py` writes it as `L_CONTEXT`), also an entry's with no
menu item, which has no Speed Dial action but is shown in these places. A pin of a Design tab action stays in
the stored favourites while its action is not live, and only leaves the bar that is shown
(`prune_favourites()`, `is_design_action_id()`, which goes by the Design table's entries with a handler, a
folded item's among them, so also when the CAD feature was off at startup). Every other pin with no live
action is dropped, a pin stored under an id the table does not have among them, and the pins around it are
kept. A build without CAD has no Design tab action, and drops such a pin as any other.
The bar's limit of ten is on what it shows: the first ten live pins in their stored order. A pin is refused
only while the bar shows ten (`add_favourite()`), so a hidden pin never fills it, and a live pin past the
tenth stays stored and comes onto the bar once a pin before it is unpinned. After each pin, unpin and reorder
the open Speed Dial's page is sent the bar as the registry then shows it
(`SpeedDialWebDialog::send_favourites()`, `ActionRegistry::favourites_bar()`), which replaces the page's own
after the click's instant change, so the page never shows fewer pins than the registry counts. Reordering
the bar moves the pins it shows among their own places, and every other pin keeps its place
(`reordered_favourites()`). Ordinary use stores at most thirty pins, ten for each of the three sets of live
actions the bar is shown with (the Design tab's model, a sketch, and every other tab); reading the stored
list cuts it to `ActionRegistry::kFavStoredLimit` (100), so a corrupt configuration cannot grow it.

### One function, JSON arguments

A command has one function, `run(args, context)`, which the Speed Dial, a menu and every automated caller
call. Its arguments are a JSON object that matches its `input_schema`, and every caller checks them first
with `Mcp::argument_error(command, args)`, which normalizes them as a tool's (`normalize_arguments()`) and
validates them. The context is a `ToolContext`, declared beside the catalog with the other types a command
is written in (`CommandTypes.hpp`: `ToolResult`, `ToolFailure`, `GuardClass`). It is the MCP core's for a
call, or a `Mcp::PersonContext` (`McpPersonContext.hpp`) for a person's pick, whose `person()` is true. A
person's pick runs in place on the UI thread, outside the runtime's dispatch core: it has no job, no journal
entry and no operation to hold, and its context reads as cancelled once OrcaSlicer is closing. It needs no MCP
endpoint, so every command's action runs with the server off. A pick starts the automation runtime only when
its command needs it: through its context (`runtime()`, `submit()`), as a long command does, or through
`GUI_App::automation()`, as a person's recording and the Action Replay and Run Script windows do.

Helpers wrap the function a command's author writes and place it on the thread it needs:

- `ui_command(key, title, group, schema, fn, completion)`: `fn(args)` runs on the UI thread and returns an
  `AppActionRunResult`, what a person's pick shows. Picked by a person, it runs in place, and the reply
  carries that result as its `notice`. Called by automation, it runs in one `ctx.run_ui()` closure, then
  `settle()`, and `from_run_result()` makes the reply: `{ran, level, message?}` with the fields of the
  result's `reply`; an error for an Error or Busy level and for a command that did nothing, worded from an
  operation's refusal in English with `{code, names?}` as details. A command made from menu items is one
  (`menu_command()`): `fn` runs the item its variant argument names, on the targets the call names, else on
  what is selected.
- `long_command(key, title, group, schema, fn, inline_call)`: `fn(args, ctx)` runs where its call does,
  on the job's thread for an automated call. Picked by a person it is started as a job and returns at
  once (`ctx.submit()`), since it must not hold the UI thread; a command with `inline_call` runs in place
  instead. A person's context submits it with `McpCore::submit()` on the runtime's core, started without
  the endpoint when the server is off: the job runs as a client's call does, under the same guards and
  operation admission, is not journaled, and its job carries the person mark (`JobRegistry::person_in()`), so
  a script it runs runs as the person's. The pick replies at once with the notice "Started:" and the
  command's title, or an Error when no job started (another operation holds the project, the app is
  closing); the job's result shows as a notification when it ends. So a pick of Slice and Preview or Slice
  All Plates, actions of `plate_slice`, starts the slice as a job, where a call waits for its result.
- `with_menu_item_forms(tool, person, ...)` and `with_person_form(tool, person, ...)`: a typed command whose
  actions are menu items that ask, or that act on what a person selected. Its person's forms are `{}` for its
  own action and the `args` of each item that collects no value (`Mcp::person_forms()`); a call that is one of
  them (`Mcp::person_form(command, args)`) runs `person` with those arguments on the UI thread, as
  `ui_command()` runs a function, and any other call is the command's own function. `with_person_form()` is
  for a command whose own function needs what its person's forms leave out (a path a dialog asks for):
  `Mcp::with_silent()` adds `silent` to the schema, which only `true` fits, and moves the own function's
  `required` list under `"if": {"not": {"enum": [<the person's forms>]}}, "then": {"required": [...]}` (or
  `"if": {"minProperties": 1}` where the only person's form is `{}`), which the validator reads as "required
  unless the call is a person's form"; it reads no other use of `if`. A call with `silent` is the own
  function, given `silent` with the other arguments, so the journal records the call as silent. Where a menu
  item does what the command does (the nine camera actions of `view_set`, Select All and Deselect All of
  `object_select`, Auto-Arrange, Auto-Orient, Refresh Plugins), its action is one of the command's `own_forms`:
  an automated call of it runs the command's own function and replies as any call,
  and a person's pick runs the same function in place, on the UI thread, showing what the item showed. A
  person's pick of an action that collects a value (Go to layer, Go to tab) runs in place too, given the
  action's call; an automated call of it is the command's own.

The Speed Dial and the menus call `NativeCommands::run(key, args)`: it checks the arguments, runs the command
with a person's context and returns the notice for `notify_result()`, or an Error holding the message when
the arguments are refused, the command refuses the call (`ToolFailure`) or it fails without a notice. Any
other exception is the command's defect, not an answer to show a person: `run()` logs it with the command's
key and lets it through to the application's handler (`GUI_App::OnExceptionInMainLoop()`), which treats it as
one from any other event handler, so running out of memory still ends the app there.

Two actions collect a value in the Speed Dial's second phase and have no dialog: Go to tab and Go to layer,
the actions `go_to_tab` and `go_to_layer` of `view_set`, whose `input` tokens are `tab` and `percent`. The page
sends the value under the token, and the action passes it on under its command's argument (`input_arg`:
`layer_percent` for `percent`; `tab` keeps its name), so `{"percent": 50}` runs `view_set {layer_percent:
50}`; a page id that a word of `view_set`'s `tab` names (`kTabWords`) is given as that word, the Device tab's
`monitor` as `device`. Go to Plate is the action `plate_goto` of `plate_select`, with `{plate: 0}`: the first
plate, as the menu item did when given none, and the plate family of the Speed Dial calls the command with each
plate.

### action_search and action_run

`ActionRegistry::describe(Query)` lists the actions, sorted by group and title, one page of them
(`{items, offset, total}`). It can filter by group, lists settings only when asked, and keeps the
actions whose id, title, group, source or description holds its `text`, including the English
aliases. Each entry has `id`, the English `title` and `group`, `kind`, `description`, `command` (the
catalog key) for a command's action, its `input_schema` when it takes arguments, and `completes: "export"`
for a command whose effect lands in the background. Built-in ids look like `orca_command:<key>:orca`.

`action_search` gives `describe()` no text. An agent's query is a few words ("delete object"),
which no action holds as one text, so `rank_actions()` (McpToolsActions.cpp) scores the listing with
the `match_score()` every other search uses: a word matches where a word of what the action is
called, or of its description, starts with it. An action is called by its key, its title and its
group, as the Speed Dial shows one by group and title: Delete Selected is the Object group's delete,
though its own title names no object. The actions matching more of the words come first, then those
matching more of them in what they are called, then the shorter key, which is the plainer action:
the action `obj_delete` before `obj_delete_all`. The key is scored without the `orca_command:`
and `:orca` around it, which would make "orca" and "command" match every built-in. `tool_search`
ranks its `actions` with the same function, so the query that found an action there finds it, and
the rest, here. Its `actions` are the actions that are no command (plates, recent projects, plugin
scripts and pages): a command's action carries its key (`describe()`'s `command`), and the command is among
`tool_search`' commands already, so it appears there once.

`action_run {id, args}` also takes the key alone (`plate_add`) when only one action has it, as agents and
the commands' own messages write it. It resolves the id on the UI thread with `ActionRegistry::find_live()`,
which first re-materialises the families whose membership follows live state (plates, recent projects, the
Design tab's actions; settings are not, being thousands), so the call after one that added a plate, opened a
sketch or switched the tab finds the actions of the app as it is then, and no longer one that is gone. The
Speed Dial's own picks run from the snapshot it was opened with. An unknown id returns up to three
close ids, scored by edit distance against both the full id and its key, a key that contains the request
(`delete_all` in the action id `obj_delete_all`) counting as closest, and says where to find an action or a
command (see Tool catalog), with code `invalid_argument`; the id or key of a Design tab action that is not live
says instead why (`unknown_action()`): that the action is there only while the Design tab is shown in its mode
with the CAD feature on, naming the call that runs its command from any tab, with a folded item's arguments
(`tool_call {"tool": "sketch_pattern", "args": {"type": "linear"}}`), as `wrong_mode`; with the feature turned off
while the app runs, which drops every Design action though their commands stay in the catalog, that a person
enables it and restarts; and in a run without the Design commands, why they are left out, as `tool_call` does.
Those two are `design_unavailable`.

A command's action takes no `args`, or exactly its own, and an action that collects a value takes that value
alone (`action_args_error()`); a Design command's own action takes its command's arguments. To run a command
with other arguments an agent calls the command. `action_run` gives the action's command what a person's pick
gives it (`action_call_args()`), so the action runs as a call with its arguments does: an action that asks in a
dialog asks, and the reply is `blocked_by_dialog`. `action_run` is an automated call, so for the actions that
are the command's own forms (the camera actions, Select All and Deselect All, Auto-Arrange, Auto-Orient, Refresh
Plugins) it replies as the command does. The
command runs through a `Mcp::ActionContext`, which is `action_run`'s own context except that a command rewriting
its journal entry does so inside `action_run`'s: the step is journaled as `action_run {"id": <the action's id>,
"args": <what action_run takes for the action>}` (`action_journal_args()`), so a recorded session replays
through `action_run` and the replay is never refused for arguments the action does not take; a Design command's
own action, which takes its command's arguments, keeps the command's rewrite. The reply's `ran` is the resolved
id, added only when the command ran. Any other action (a plugin's script or page, a plate's, a setting's, a
recent project's) takes no arguments, so `args` other than `{}` are refused, saying so; it runs through
`ActionRegistry::run(id, args, record_stats=false)`, so agent runs never reorder the Speed Dial, and replies as a
command does.

### Commands that replace a dialog

A call with exactly an action's arguments is the person's form and asks as the menu item does; any other call
runs from its arguments and opens nothing. Which actions ask, what each asks, the call that does the same without
asking, and the commands that open a dialog whatever their arguments are in [mcp-tools.md](mcp-tools.md),
When a call opens a dialog or a tool. A path is absolute, and an output takes
`overwrite: true` to replace a file that exists; `path_argument()` resolves `{"path", "overwrite"?}` for the menu items that take one and turns
a bad or missing path into an Error result.

`preset_import` with a path imports presets as the Import Configs dialog does, through
`Ops::import_presets()`, the dialog's work split from its file picker and result message: the files
go to `PresetBundle::import_presets()`, and the presets are reloaded and the sidebar refreshed. A preset
file (`.json`) is imported alone, a `.zip` or bundle (`.orca_printer`, `.orca_filament`, `.orca_bundle`)
with every preset inside it. The dialog asks about each preset that exists already; the command answers
that question itself, never with "to all", so it is asked about every such preset and the message can name
them all: `on_conflict: "overwrite"` replaces them, and by default (`"skip"`) they are kept. The schema has
no `overwrite`, which the output commands take for a file, so it is refused here rather than read with
that other meaning. The message lists the presets imported, replaced and kept, found by comparing the
preset names before and after; a call that changes nothing is an Error with the dialog's hint about adding
the printer first. A `.gcode` loads the settings it was sliced with through
`MainFrame::load_config_file(path)`.

A command made from menu items that refuses says why, each refusal in its own words (an `Ops::Refusal`, shown
in the UI's language to a person and read in English by a call): a pick shows it at its level, Info for most,
and an automated call replies `isError`. `filament_add` and `filament_delete` are the filament list's + and -
buttons and a slot's Delete and Merge with menu items (`Ops::add_filament_slot()`,
`Ops::delete_filament_slot()`):
they run where the buttons show, for printers that take several filaments through one extruder, and refuse
elsewhere, at the slot limit, and for the only physical slot. `filament_delete` deletes a mixed filament as the
mixed filament list does and refuses to merge one, which the list offers no Merge with for; a physical slot a
mixed filament mixes is deleted as the sidebar deletes it, leaving the mix broken (The filament list). A new
slot goes after the physical slots, so mixed filaments move up by one with the objects on them. `filament_delete` takes a 1-based `filament_slot`, the last physical one without it, and
`merge_into`, the slot its objects go to in place of the first. `preset_sync` refuses while no account is
signed in, and `filament_sync_ams` with no printer, no AMS or no filament.

A command whose effect its message does not carry sets `AppActionRunResult::reply`, a JSON object whose
fields an automated call's reply has and the Speed Dial never shows (`menu_reply()`): `object_order` replies with
the new order (`order: [{object, name, plate}]`) and `descending`, `object_assemble` with the new object's
`object` and `name`, `object_separate_instances` with the `objects` the instances are in, and
`object_set_instances` with `object` and `instances`. A refusal after a dialog the person answered or read,
which told them why, shows nothing more to the person, and a call learns it (`quiet_refusal()`): a cancelled
rename is `not_renamed`, a save that wrote nothing `project_not_saved`. `object_arrange {}` and
`object_auto_orient {}` say why they did nothing (no objects, or another background job running); their job
is the call's operation (see Worker jobs), so the call, and `action_run` of their actions, returns once it is
done.

### Making a feature reachable

A feature is a command, named for its place, then its verb (see [mcp-tools.md](mcp-tools.md)),
declared in the `McpTools*.cpp` of its place beside the commands of that place:

1. Build it with `inspect_tool()` for pure observation, which runs inline, read-only and unjournaled, or with
   `mutate_tool()` for anything that changes the app, which runs as a journaled job under the mutate guard;
   label it as Tool catalog says (`destructive`, `open_world`). Build the schema with `object_schema()`,
   `prop()`, `enum_prop()` and `array_prop()`, and its targets with `with_targets()`; it is validated before
   the handler runs, so `arg_string()` and its siblings can read the arguments. Describe it in English: what
   it does, its arguments, `Reply:` with what the reply holds, and when it is a job. Give it one to three
   `examples` and its key in `kPlaces` of the catalog test, and, where it is one, in that test's read-only or
   destructive set; a new place word needs a heading in `place_heading()`.
2. What it does goes into an operation of `GUI/Operations` (see The operations layer): its checks, its
   refusals and their wording, its undo snapshot. The handler reads its arguments, resolves its targets
   (`resolve_targets()`), calls the operation, throws `refused()` with the refusal or shapes the reply
   (`targets_reply()` for what it acted on), and the command names the operation through `reaching()`, or
   joins the exception list of `test_operations` with its reason. Throw `ToolFailure` with a message the
   model can act on for what is the command's own to check, such as the form of an argument. Do all wx work
   in `ctx.run_ui()`, get the plater with `ui_plater()`, which fails until the main window is up, call
   `settle()` after a change so queued events are handled before the reply, and page lists with `page()`.
   For work that finishes later, call `ctx.begin_operation()`, wait with `poll_ui()`, and give the work an
   identity, as slicing and export do. Work on the UI job worker needs none of that: the call that starts it
   holds it as its operation (see Worker jobs).
3. Where a menu item does the job, the item is an action of the command (`NativeCommands::action()`), with the
   arguments that pick its variant and the section, title and icon the item has, and the command takes its
   person's form for it (`menu_command()`, `with_menu_item_forms()` or `with_person_form()`). An item that
   asks keeps its owner's asking path for that form; the call with arguments must not prompt, and refuses
   instead. An item with no argument form is a command that is not `listed`.

The command then appears in `tools/list`, in `tool_search`, its actions in the Speed Dial, `action_search` and
`action_run`, and its calls in replays.

A new or changed command changes the command reference, `tests/data/command_catalog.json`: the
`catalog_reference()` (`CommandCatalog.hpp`) of the catalog a run with the Design tab holds, every added
command in the order it was added with its key, English title, group, description, schema, examples, actions,
whether it is listed and has an action of its own, its access labels, guard, completion and the operations it
names. It is where a reader outside the app sees every command, and the skill's list of the Speed Dial's
actions is written from it (`tools/mcp/gen_skill_reference.py`). The unit test "the checked-in command reference
matches the catalog" fails until it is regenerated: run `slic3rutils_tests "[Reference]"` with
`ORCA_UPDATE_COMMAND_REFERENCE=1` in the environment and check the file in. The test compares the file with
its line ends made LF, since a checkout may turn them into CRLF, and a build without CAD does not check it.
A plugin's operations are not in it: they are not the build's.

## GUI automation

`widget_list` (`UiInspector`) returns a compact text tree of the shown top-level windows, each
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
setting). Orca's `LabeledCheckBox`, a panel holding a `CheckBox` and its label, is one checkbox named
by its label, as a native one is: its parts are neither listed nor matched, and its check holds its
state, takes its click and says whether it is disabled (`check_part()`), since the panel itself stays
enabled. An option of a selector Orca builds of buttons (`RadioGroup`, `TabCtrl`, `MultiSwitchButton`)
is a button that reads checked while it is the selected one (`selector_option()`), so `widget_set_value true`
selects it, and a `SwitchButton` with a label at each end has the one it is on as its value.

The inspector, the menu capture, ImGui item recording and, on Wayland, the window-state hooks are the
GUI instruments, which are on while something drives the GUI (see Lifecycle); the inspector, once made,
stays. It is built from the modal tracker and the UI generation alone, not from the runtime.

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

`object_select` selects objects, instances and parts and replies with the selection as `app_state.selection`
gives it (`selection_state()`): in `object_select`'s own indices, the objects, the instances they are selected
in and, when the selection holds parts rather than whole instances, those parts. An agent reads it
before a command it calls with no targets, which acts on the selection (see Tool catalog): the object
commands (`object_delete`, `object_split`, `object_mirror`, `object_center`, `object_drop`,
`object_set_instances`, `object_auto_orient`, `part_change_type`, `object_rename`, `object_assemble`,
`object_separate_instances`, `object_order` and the others) take their targets by index too, and their
reply names what they acted on. `settings_set` with the target `object` or `part` edits their
settings and filament. `part_add` adds parts, modifiers and support shapes,
`object_set_height_ranges` sets height range modifiers, and `object_set_variable_layer_height` runs the Variable layer
height panel's buttons. `plate_set_custom_gcode` does what the Preview layer slider's right-click menu does.
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
canvas's idle handler does) before `widget_list` lists them and before a click checks them, so an
item a `object_select` just enabled is not read as disabled. A main menu item likewise gets the state a
person opening its menu sees before `widget_list` lists it and before a click checks it: the menu's
`wxEVT_UPDATE_UI` handlers run with the main frame as their source (`update_menu_ui()`), as the frame does
when a menu of its menu bar opens and `PopupMenu()` does for the menus Windows' top bar shows. Until then an
item keeps the state of the last time a person opened its menu, so View > Default View would still read
enabled on the Design tab, where the app greys it. An item under a greyed submenu entry (File > Recent
files with none) or menu bar title reads as disabled too, and a click on a disabled one is refused. A
click on an ImGui item whose centre lies under another ImGui window (a notification over a panel's
button) is refused, naming that window.
`ImGuiWrapper::set_item_recording()` supplies the ImGui items (see below). `SyntheticInput` holds
a gesture's keys, buttons and pointer for code that reads hardware state through `get_key_state()`
and `get_mouse_state()`: the canvas, gizmos, and the object list's Ctrl/Shift multi-select. Mouse
modifiers take the names `keyboard_press` chords use, through `KeyChord::parse_modifier()`, so `ctrl` means
the shortcut modifier (Command on macOS) in both, and `rawctrl` the macOS Control key. Held
keys are released however the gesture ends; a `mouse_gesture` `down` keeps its button held, and a
`move` drags with it, until an `up`, another gesture (one that fails included; a request the server
rejects for its arguments never runs and changes nothing), the end of the replay or script that pressed it,
the closing of the endpoint for a client's press, or the runtime's stop. A client's gesture that
reaches the UI thread after its endpoint closed is refused. Keys without a target go to the
focused window inside the topmost modal, else that modal, else the focused window, else the 3D view
shown. A key or a button press for a 3D view gives it the focus first (`focus_for_press()`), as a
real key comes from the window holding the focus and a real press activates the window under the
pointer. The panels' shortcut handling reads the focus (the Design tab leaves keys to a focused text
field), and the focus can be left in the field of a dialog that closed while the app was behind
another.

A key is delivered as a real key press is on every port: `wxEVT_CHAR_HOOK` goes to the target first
and travels up through its parents (`wxKeyEvent` propagates it) to the top-level window, or stops
at a dialog (`wxWS_EX_BLOCK_EVENTS`); only when no hook takes it, or the one that does allows the
next event (`DoAllowNextEvent()`), do `KEY_DOWN` and `CHAR` reach the target. So a panel that binds
its own hook sees the keys sent to its widgets, as the Design tab's does for Escape, Enter and its
tool shortcuts, and the main frame's hook still sees what none below it takes.

The Design tab's view is a `GLCanvas3D` of its own (`DesignCanvas::gl_canvas()`), outside the
plater's three. `live_canvases()` lists the plater's, which `app_state.gizmo` and the idle checks
read, since the Design view has no gizmos; `ui_canvases()` adds the Design view for what `widget_list`, the input
tools and screenshots reach: `widget_list` lists its ImGui items, `widget_click` and `keyboard_press` act on it,
and `mouse_gesture` takes `canvas: "design"` (`"current"` is the view shown, `shown_canvas()`). A click
there goes to the Design view's own handlers, so a left click picks a face, edge or body and a right
click opens the offer menu, which is listed or run like any other. Points of the model name
Prepare's objects, so on the Design view `mouse_gesture` takes pixels only; the `body_describe` command
gives its faces.

### Windows

`window_list` (`UiInspector::windows()`) lists the open windows without their widgets, in `widget_list`'s
order: the main window, the windows that don't block in the order they were created, then the modal
stack, the topmost last. wx keeps top-level windows in creation order, not stacking order, so only the
modal stack orders by what is on top. Each window has the ref `widget_list` gives its node, with the same
locator, so `window_screenshot` captures that window by it, `widget_list` lists it alone by it, the input tools take it, all as
`target`, and the journal records the locator; its title is what `within` takes. A window says whether it
is modal and whether it shows a web view (outside a hidden pane), with its size and, where the platform
knows it (not Wayland, `window_positions_known()`), its place on screen, its state (normal, maximized or
minimized) and whether it has a resize border (the main window always does). `UiInspector::window_entry()`
builds one window's entry, which `window_set` replies with too. A native dialog of the modal stack, which
only `dialog_answer` reaches, is listed with `native` and no ref, whether or not wx reports it shown. A window named `kHiddenFromAutomation` is listed by its title and kind with `hidden` and
nothing more: without a ref no tool acts on it, and an agent learns why a window it was told about is out
of its reach. The Python scripts window is the one modal window the tracker leaves off its stack, so it is
listed among the others, `modal` from `wxDialog::IsModal()`.

### Sizing and placing windows

`window_set` sizes, moves, maximizes, minimizes and raises a window `window_list` gives a ref: the main
window (`main`, the default), the topmost modal dialog (`dialog`), or the window a ref or locator is in,
so a widget's ref names its window and a replay's `{label: "Preferences"}` finds the dialog again. It
refuses a native dialog, which `dialog_answer` answers, a window named `kHiddenFromAutomation`, a popup,
which the window it opened from places, and a window no longer shown. On macOS a native dialog holds
the UI thread, so a call made while one is open waits on it as every call that needs the UI thread does
(`blocked_by_dialog`) and never reaches that refusal. A dialog that doesn't block is
hidden when it closes, so its ref still resolves; that refusal and the one for a stale ref point to
`window_list`. A window's geometry changes nothing in the project, so `window_set` is a recover call
allowed during an operation that applies no mutate checks: it runs behind a modal dialog, on it, or on
any other window. A floating dock panel is sized and moved through its own frame, so the dock manager
records its floating size and place as after a drag.

`window_request_refusal()` checks the whole request against the window's `WindowAbilities` before
anything changes, so a refused call leaves the window as it was: at least one change, whole-number size
and position, no `maximized` or `minimized` with a size or position (the system sets those), a state only
when the title bar offers it (always for the main window, else `wxMAXIMIZE_BOX` and `wxMINIMIZE_BOX`), a
modal dialog never minimized, since the app would stay blocked with nothing showing, and no position on
Wayland. A UI closure then applies it in a fixed order. A size or position restores a maximized or
minimized window first (`restore_window()`): out of the minimized state, then out of the maximized one,
since `Restore()` presents, so raises, the window on GTK and brings a window minimized while maximized
back maximized on Windows. It touches only the state the window is in, as GTK raises a mapped X11 window
it un-minimizes, Windows activates a normal window it un-maximizes, and a Wayland compositor answers an
un-maximize with the window's current size, which replaces a size asked for right after. GTK reports
the restore once the window manager or compositor has applied it (X11, Wayland), so there the call polls
until the window reads normal before it goes on, since a size given before that would be lost, or worked
out from the maximized window's decorations. On macOS a minimized window stays in the Dock until the
animation that brings it back ends, about half a second, and AppKit ignores a zoom until then, so a
window minimized while zoomed comes back zoomed; the wait for the window to settle takes it out of that
once it is back (`finish_restore()`), and it returns to the size and place it had before the zoom. A
size given meanwhile is kept, as it leaves the window no longer zoomed. Then come the size, held to the window's minimum and maximum (what its sizer
needs), the position (`wxSIZE_ALLOW_MINUS_ONE`: -1 is a place on a screen left of or above the primary
one), the state and the raise. A window without a resize border is resized all the same. Sizes and
positions are wx window coordinates, as `window_list` and `app_state` report them: physical pixels
on Windows, points on macOS, logical pixels on GTK. The argument normalizer reads `window_list`'s flat
`x`/`y` and `width`/`height` as `position` and `size`.

On Wayland a size is the window the compositor shows, which a window screenshot captures
(`window_size()`). GTK decorates OrcaSlicer's main window itself, as the window draws its own title bar,
and keeps margins around it for its shadow and resize border (45 pixels a side with Breeze). wx counts
those margins in the window's size after some allocations and not after others. It also adds decoration
sizes it reads only where GTK decorates a window the compositor would rather not
(`gdk_wayland_display_prefers_ssd()` false, as on Mutter), and counts none on KWin. So the size is read
from GTK's allocation less the margins, and `resize_window()` gives wx the size plus the decorations wx
takes off again, less a title bar GTK shows.

A window manager may apply a request later (X11), animate it (macOS) or change it, so the call polls every
50 ms (`poll_until()`, which returns null on timeout or cancellation instead of throwing) until two readings of the screen
rect and state in a row agree and show the state asked for, `normal` after a size or position. For the
main window it also waits, asking for a frame each poll, until the shown 3D view has drawn at its new size:
the camera's viewport, which only a frame sets, must match the canvas, or a `view_set` fit or a
`screen_rect` that follows would use the old size. The Design tab swaps its camera into the plater's slot
while it is shown, so the one check covers every 3D view. A minimized window and a hidden canvas draw no
frames, so neither is waited for. After 3 s (5 s for a minimize on Wayland, `state_change_timeout()`)
the call replies with what it read and a note. A call cancelled during the wait fails, and so does one
whose window closes, saying the request was applied.

The reply is the window's `window_list` entry, from the same `window_entry()` with a new ref, in
`window_set`'s own form: `title`, `kind`, `ref`, `size`, `position` (not on Wayland), `state` and
`resizable`, plus `scale` (`GetContentScaleFactor()`, screenshot pixels per window unit), `display` (the
work area of the display it is on, the primary one for a minimized window on none), `active` with
`raise`, and `adjusted`, the requested fields the result differs from, with a `note` on whether a size
limit or the window manager did it, or on what the wait did not see: the window holding still in the
state asked for, or the 3D view drawing at its size, with `undrawn_window()`'s reason when the window is
not being drawn (`add_window_notes()`). `app_state.displays` gives each display's work area, scale and
whether it is primary, so an agent can choose a position.

On Windows a change takes effect at once, but `raise` may only flash the taskbar button while another app
has the focus, since the OS locks the foreground; `active` says which happened. On macOS `maximized` is
the window's zoomed state, not full screen, and `raise` brings the window in front of the app's other
windows, but while another app is active it stays behind that app's windows and `active` reads false.
OrcaSlicer doesn't activate itself for it, which would take the keyboard from the person's app, and no
tool needs it active: a capture reads a covered window whole and input goes through wx. Under X11 the
window manager may adjust a request.

With no window manager, as under a bare Xvfb, the app does the window manager's part
(`WindowControl.cpp`); every EWMH window manager lists `_NET_WM_STATE` among the hints it takes, so its
absence says none runs. `maximized` remembers the normal rect, adds the maximized atoms to the window's
`_NET_WM_STATE`, which GDK reads back, so `IsMaximized()` follows, and fills the work area of the window's
display. `normal` takes the atoms out and puts the rect back. `minimized` unmaps the window, which GDK
reports as minimized, and wx's `Iconize(false)` maps it again. Mapping a window raises it, and nothing
keeps a dialog above the window it belongs to, while an X11 capture reads the screen. So after a restore
and after a `raise`, the windows transient for the window are raised over it, in the order they were
created.

Wayland has no request to place a window or to read where it is, and no portal offers one; only
compositor-private interfaces do (KWin's scripting, sway's IPC), and GNOME has none. So a call with
`position` is refused there, naming `GDK_BACKEND=x11`, under which the app runs through XWayland and every
field works. `window_list`, `window_set` and `app_state.window` leave positions out. Size,
maximize and normal work. GTK never reports a minimized window, but the compositor's `suspended` state
(xdg-shell 6) reaches it as a fully obscured visibility notify, which emission hooks installed with the
GUI instruments record on the window (`track_window_states()`). Hidden is not minimized: KWin hides a
minimized window at once, but also one on another virtual desktop and every window while the screens are
off, and Mutter hides any window it has not shown for 3 s. So a window reads minimized while it is hidden
and a minimize from `window_set` stands, which ends once the window shows, takes the focus or closes. A
minimize the person makes from the desktop reads `normal`, since nothing tells the app of it. The wait for
a minimize lasts 5 s on Wayland, as Mutter hides the window 3 s after it, and sway never minimizes a
window.

Wayland gives an app no request to un-minimize a window, only to activate it (`gtk_window_present()`
over xdg-activation), which `normal` and `raise` do. The compositor grants it only while the app may take
the focus. KWin grants it while no window is active, or when no click or key has gone elsewhere since the
app's own last input; MCP input goes through wx, not the compositor, so it counts as none. Mutter grants
it only while one of the app's windows has the focus. A refused `normal` replies `minimized`, with a note
that the person restores the window from the taskbar, and a refused `raise` replies `active: false`.

### Menus

Every popup and context menu goes through `popup_menu()` (`SyntheticInput.hpp`), which is
`wxWindow::PopupMenu()` unless the GUI instruments are on, which install a capture
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
set each item's enabled and checked state as a person would see them. `widget_click`, `mouse_gesture` and
`keyboard_press` arm a capture in the closure that delivers their input (`arm_menu()`). The first menu that
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
a menu that opens in a window where nothing is armed (a call that queues a menu to open later)
is closed unseen too. The last three such menus, with the tool whose window took them, are in
`app_state.closed_menus`.

A menu path is the item labels joined with `>`, each without the key the menu shows after it
(`MainFrame::menu_item_label()`), matched without case; a trailing `...` may be left out. That is the
label `GetItemLabelText()` gives, which drops a key written after a tab, but for a key the macOS menu
bar shows as plain text after ` - ` so that it does not take the key from text fields (`Edit>Undo`, not
`Edit>Undo - ⌘+Z`), which `MainFrame` drops by the label it made the item with. So a path is the same
on every platform. A label repeated among its siblings is numbered from its
second time on (`Change Filament>Generic PLA #2`), so every listed path names one item, and the
marker the Change Filament list appends to the slot in use (" (current)", translated) is left out of
the numbering and the matching, so a path recorded with another slot in use still names the same
item. A path that names a submenu, a disabled item or nothing fails with the items to pick from, and
so does a `popup_item` when the input opened no menu (a right click on empty space while objects are
selected, in the Preview, or with a gizmo open). An input to a 3D view is read once the view has run
it (a call queued behind it has run), however long that takes on a busy machine, up to a minute, and
until then the capture it armed takes the menu the input opens even past the call's menu window, so
the menu is never shown as a person's while the call still waits for it. Paths are the UI language's text, as other locators
are. With `x`/`y`, `widget_click` clicks that point of a custom-drawn widget (such as the Design tab's
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
for by the indices `object_list` uses (`plate 1`, `object 0 part 1, modifier`, `object 0 layer
range 0-2`), with `, has settings` where the row's settings of its own show as the icon that resets them
(a plate's, an object's, a part's or a height range's). A table's rows are named by their `Name` cell (the
object settings table's part rows
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
is captured like any other. `widget_set_value` with true or false sets a checkbox cell through
`ChangeValue()`, as the toggle renderer does, which tells the owner. A list box row is selected with
`wxEVT_LISTBOX` and a checklist row ticked with `wxEVT_CHECKLISTBOX`; a drop-down row is picked with
`DropDown::ClickItem()`, which does what a release over it does; a table row's cell is made current
with `GoToCell()` (`wxEVT_GRID_SELECT_CELL`), and its cells are not edited: `settings_set` with the
target `object` writes the same values. A Design list row is clicked at its centre, or at the action
icon whose tooltip `column` gives, through the list's own mouse handlers.

Popups (`wxPopupWindow`: the print button's menu, drop-down lists) report themselves top-level but
are children of the window they opened from. `widget_list` lists each shown one as a `popup` right
after its window (`popups_of()`), and `target: "popup"` lists only them. A widget in a popup belongs
to that window (`owning_window()`), so it is inside the topmost modal for the input guards when its
popup opened from there, and its locator's `within` names that window. A screenshot of a widget in
a popup is refused: a screenshot of its window shows the popup.

### Plate icons

A plate and its icons in the Prepare view (delete, orient, arrange, lock, settings, name, move to
front, filament grouping, IDEX mode) are GL picking models, which a person reaches only by pointing at
them. `widget_list` lists under the Prepare canvas a `plate_icon` node for each plate, named as the
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
`ImGuiContext::TestEngineHookItems` is set, which `ImGuiWrapper` does only while the GUI instruments
are on; otherwise ImGui skips each hook with one branch. `ImGuiWrapper` keeps the items of each canvas's
last frame: id, label, window, rectangle and status flags, whether the item lies outside the
visible part of its window, and whether it is the text field ImGui feeds keys to as the frame ends
(the active item, while `InputTextState` is its), which `widget_list` marks `[focused]` as it does
the focused window. A field that asks for the keyboard (`SetKeyboardFocusHere()`) takes it a frame
after the one that first draws it, and keys sent before then are lost, so a client types into it
once it shows focused. The hooks reach the recorder through `ImGuiContext::TestEngine`, so they
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

`widget_list` lists each ImGui window (its scroll range when it scrolls), then its items and text in
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

`widget_set_value` on an ImGui item acts by its kind and reads the value back, failing when the field shows
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
gizmo, which `widget_click` waits for. A `gizmo` locator takes the name `app_state.gizmo` gives, or
the English title of its toolbar button where that differs (Support Painting for FdmSupports), in
any case; the reply uses the first, and an unknown name's error lists them. `app_state.gizmo`
names the open gizmo and its canvas. The
assemble view is entered and left with `view_set canvas` (leaving does what its Return button does;
entering returns once the canvas has loaded its objects, which waits for its GL surface on Wayland),
and `object_select` and `object_list` act on it while it is shown. Grabbers are not dragged: a gizmo's
panel fields or `object_transform` set those values. Pickers inside a gizmo go through `gizmo_pick_file`, whose
actions are the bodies of those pickers (`GLGizmoSVG::change_file()`, `save_svg_as()`,
`GLGizmoTextureDisplacement::import_texture()`).

A command can also open a gizmo on a target it names (mcp-tools.md, The gizmo commands: opening or acting):
`Ops::open_gizmo()` shows the Prepare view, selects the target as a click on the model would, asks the gizmo
whether it can open, and opens it through `GLGizmosManager::open_gizmo()`, as its icon does; then it renders the
canvas twice, so that the panel's items exist for `widget_list` before the reply, as a click on the icon here does.
Opening is not a toggle: the gizmo already open on the same target is left open. A refused opening puts back the
selection it found, under `Plater::SuppressSnapshots`, so it leaves no undo step. The gizmos that open and close
with snapshots of their own (Cut, Brim Ears, the painting gizmos) record them as for a person. A command that acts
from arguments closes the open gizmo first, through `reset_all_gizmos()`, since a gizmo writes its state to the
model as it closes; the Brim Ears gizmo also reloads an object's ears on its next opening after
`object_set_brim_ears` writes them (`GLGizmoBrimEars::reload_on_next_opening()`), where it would otherwise show,
and write back, the ears it held.

`mouse_gesture` takes points of the model (`CanvasTarget`): `{object, instance, part}` with `point` in
the part's mesh coordinates (`frame: "part"`, mapped by `GLVolume::world_matrix()`, which follows
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
Shift+move instead. So the tools never turn the camera with a drag. `view_set` `orbit` moves it around
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

### Camera

`view_set` sets the camera of the 3D view shown. Prepare, Preview and the assemble view share the
plater's camera, and the Design tab swaps its own into that slot while it is shown
(`DesignCanvas::enter_viewport()`), so `Plater::get_camera()` is always the shown view's and one code
path serves all of them. `look_at` is the point the camera looks at, in mm in the view's world
coordinates, as `object_list` gives positions. On the Design tab these are the Design view's, the
frame the Design commands' world points use, where `describe_design`'s `modeling_origin` is the sketch
planes' origin. `Camera::set_target()` moves the camera parallel, keeping its direction and
distance. `zoom` is viewport pixels per mm at the target's depth, which are screenshot pixels: exact
everywhere in an orthographic view, and only at the target's depth in a perspective one
(`Camera::apply_projection()`). `Camera::set_zoom()` holds it between what the scene's size allows
and 250, and the reply then lists `zoom` under `adjusted`.

The fields apply in one UI closure in the order `camera`, `look_at`, `orbit`, `zoom`: a fit sets the
point the camera looks at and the zoom, which the given ones then replace. A tab switch, a new project or a load queues a
fit on the camera (`requires_zoom_to_bed`, `requires_zoom_to_volumes`, `requires_zoom_to_plate`) that
`GLCanvas3D::render()` runs at the view's next frame, and it would replace a `look_at`, `orbit` or `zoom` set
before that frame. So when any of the three is given, the call first polls (`poll_until()`, every
20 ms for up to 2 s) until no fit is pending, asking the shown view to draw each time. A `camera` makes
the same poll also wait until the shown view has drawn at its canvas's size (`view_drawn_at_size()`,
which `window_set` waits on too): a fit, which `default`, the `fit_*` cameras and the Design directions
make, uses the camera's viewport, which only a frame sets, so after a resize while another tab showed
it would fit the old size. After 2 s the call applies the fields anyway and says in a `note` which of
the two it did not see; a call cancelled meanwhile fails instead.

Whenever a camera field is given, the reply carries `camera_state`, read after a last `settle()`:
`type` (`ortho` or `perspective`, the Preference the `K` key toggles, which says whether `zoom` holds
everywhere), `look_at` and `position` in mm, `zoom`, `click_px_per_mm` (the zoom over the canvas's
scale, in the logical pixels `widget_click` and `mouse_gesture` take) and `elevation` and `azimuth` in
`orbit`'s convention. `camera_angles()` reads the angles for both, without the orbit's one-degree stop
short of vertical, so a camera straight above reads 90. The `camera` and `orbit` echoes stay beside it,
since replays check `/camera`. `app_state.camera` is the same `camera_state()` for the 3D view
shown, left out when none is shown.

### Dialogs and web views

`ModalTracker`, a `wxModalDialogHook`, sees every `ShowModal()`, native dialogs included, and
publishes the snapshot `[{title, message, class, native, buttons}]`, which other threads read. Read on
the UI thread, as `app_state`, the `blocked_by_dialog` probe, the guards' refusals and the input tools'
`dialogs` read it, the snapshot describes the open dialogs again first. The guards themselves only ask whether a
dialog is open: describing one reads every control, and done for every closure the UI thread runs, it would keep
a dialog's own events waiting while many calls queue. So it shows a dialog as it is
now: text the dialog filled in once it showed (a printer's name, a list from a scan), a field typed in,
a page switched. Should describing fail, the last description stands. Only a native dialog that holds
the UI thread (macOS) leaves readers with the description published before it; its content does not
change. A dialog that can't be described is listed by its title, class, `native` and `web` flags, with no
message or buttons, so the guards still see it and a rule matches it by its title. Message boxes and file and folder pickers are native; a box's message is its text, and a
picker's is its prompt, such as "Choose Download Directory", since its title is often empty. Colour and
font choosers are not classified as native, because on GTK they are wx dialogs that `dialog_answer`
ends; on macOS and Windows they are OS choosers, best answered ahead with an `dialog_auto_answer` rule.

A wx dialog's message reads it as a person does, so an agent can tell what the dialog asks and what
each button will do without a screenshot (`DialogReader`). Each shown child becomes a cell. Nested
dialogs, popups, web views and canvases are left out; a nested dialog is described on its own.
Controls are read through the readers `widget_list` uses (`kind_of`, `value_of`, `checked_of`,
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
- A list reads one line per shown row, up to 25, then a line saying `widget_list` lists the rest.
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
  its label: `[x] Run Python scripts unrestricted`. With nothing there, it takes the topmost single-line cell to
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
one only selects it. `dialog_answer`, and a rule, still find such an option by its label (its tooltip
for an icon option, in full even where a message cut at its cap shows only its start) and select it, and
`widget_list` shows the selected option checked, under the same full label. `widget_set_value false` on the
selected one, or on a selected radio button or radio box, is refused, since an option is unselected by
selecting another. Other radio buttons and switches are set
with `widget_click` or `widget_set_value true`, and notebook tabs with `widget_click` on the tab.

`dialog_answer` presses the topmost wx dialog's button, so the dialog's own handler runs.
`press_dialog_button()` tells what it did:

- **Pressed:** the button was pressed.
- **Ended:** the dialog has no button with that id, so it ended with it, and `cancel` closes a dialog
  without a Cancel button. One whose Cancel or Close is on a page it does not show ends the same way.
- **Disabled:** the button is shown but disabled, so nothing is pressed or stood in for. The dialog
  disabled it on purpose: OK while a name is invalid, Send until a box is ticked, Cancel while a
  cancel is under way. `dialog_answer` refuses a disabled button, its message showing what the dialog
  still needs.
- **Hidden:** the button is only on a page the dialog does not show, such as a wizard's Finish, so
  nothing is pressed or stood in for, and `dialog_answer` refuses it. Buttons are looked up among the
  dialog's own windows, not a popup's or a nested dialog's.
- **Failed:** a native message box could not be pressed.

On Windows and Linux `dialog_answer` also presses an open native message box. The tracker lists the
buttons wx gives the box for its style (Yes and No or OK, and Cancel; not Help, which on Windows shows
help without closing the box), and a press goes to the OS's box by message. On Windows that is a
`WM_COMMAND` to a message box, or a `TDM_CLICK_BUTTON` to the task dialog wx shows where Windows has
one, found among the UI thread's windows by class and caption, the active one first. On GTK it is
`gtk_dialog_response()` on the dialog's widget. The box closes once its own loop runs again, which
`dialog_answer` waits up to 1 s for, so its reply says `closed`. A box that is no longer shown is
refused. On macOS a message box lists no buttons, since no call reaches one (above).

A picker, and on macOS any native dialog, can only be answered ahead with an `dialog_auto_answer` rule:
a regex and a button, and for a file or folder picker a path. The regex is searched in three places:

- the title and message joined by a newline
- the title and the dialog's own text joined the same way. The own text is its static text and html,
  one per line in child order, cut and trimmed as the message is. It leaves the controls out, and
  `describe_dialog()` reads it in a walk of its own that the message's budget does not stop.
- the title alone, for a dialog with no own text (so `^Title$` matches a web dialog)

A rule written against a dialog's wording therefore matches however its controls read, and long values
or lists before that wording do not push it out. `Enter()` then returns that button's id without
showing the dialog, and logs it.

A picker is matched by its prompt, unanchored: one answered before it shows usually has no title, so
the text searched starts with the newline before the prompt, which a pattern anchored with `^` misses.
A rule without a path answers a file or folder picker only with cancel, since any other answer would
return without a path. A rule with `button: ok` and an absolute `path` answers one with that path
(`make_rule()` refuses a relative path, and a path with any other button): `answer_picker()` sets it on
the picker with `SetPath()` and returns `wxID_OK`, so the caller reads it from `GetPath()` as if a
person had chosen it and runs its own code from there. An open picker takes an existing file and a
folder picker an existing folder; a save picker takes a path as any output does (its folder must exist,
and an existing file is replaced only with `overwrite: true` on the rule). A path the picker can't take
cancels it, and the log says why, so nothing waits for a person; the rule counts as used either way. A
multi-select picker takes no path and is cancelled, since it fills `GetPaths()` from its own list,
never from `SetPath()`; for the same reason a single-select picker's caller that reads `GetPaths()` instead of
`GetPath()` gets an empty list from a path answer. wxGTK's own `SetPath()` also hands the path to the GTK
chooser, which loads the folder later in the main loop, and its folder picker's `SetPath()` stores nothing
else, so on GTK the path goes in through the base classes' setters (`wxDirDialogBase::SetPath()`,
`wxFileDialogBase::SetPath()`), which store it and start no folder load that could outlive the dialog. wxGTK's
folder picker reads its path from there. Its file picker would ask the GTK chooser, which never showed and has
no answer, but Orca's wxWidgets fork (`deps/wxWidgets/wxWidgets.cmake`) has its `wxFileDialog::ShowModal()`
record whether a `wxModalDialogHook` answered the call, with or without a path. While one did, `GetPath()`,
`GetFilename()` and `GetDirectory()` return what `SetPath()` stored and `GetPaths()` and `GetFilenames()` are
empty, as on Windows and macOS; the fork's header announces it with `wxHAS_GTK_FILEDLG_HOOK_ANSWER`, and a
build against a wxWidgets without it cancels a GTK file picker a path rule matches, saying so in the log.
GTK's own file chooser button, a `wxFilePickerCtrl` without `wxFLP_SAVE` or `wxFLP_USE_TEXTCTRL` (the SLA
import dialog's), opens its dialog itself, never through `ShowModal()`, so no rule answers it.

A save rule's path is the file's final name. The hook bypasses the picker's overwrite prompt, and `overwrite`
is checked against the path as given, while the picker's caller may still change what it returns: exporting a
plate's sliced file adds `.3mf` to a name without it (`Plater::export_gcode_3mf()`). A caller that refuses the
name and shows the picker again, as exporting G-code does for characters a FAT file system can't hold, is
answered with the same path again. A path rule answers pickers only, as they open: never a wx dialog or
message box it matches, nor a picker already open. `app_state.auto_answer_rules` and the tool's reply list a
rule's `path` and `overwrite`. The replay summary (`describe_step()`) flags a step that sets a path rule
outside the run's output folder as writing a file, since which picker the rule will answer, and whether that
picker saves, is not known until one opens.

A wx dialog that matches a rule opens, and the tracker presses the button once the dialog's modal loop
runs (`press_dialog_button()`, queued on the dialog), as `dialog_answer` does. Its buttons do the
dialog's work in their handlers (`UnsavedChangesDialog`'s Transfer and Discard, `CloneDialog`'s Fill),
which a returned id would skip. The rule counts as used once the press pressed the button or ended the
dialog. A button disabled by then leaves the dialog open, logged, and a modal opened above it before
then leaves it to `dialog_answer`.

A new rule also answers a matching dialog that is already open, as `dialog_answer` would, a message
box included where it can be pressed. The first rule that names one of its buttons decides: if that
button is disabled or hidden, the dialog stays open and the reply says `disabled: true` or
`hidden: true` (a hidden Cancel or Close ends the dialog, as above). A prompt posted around
the same time, such as the restore prompt after startup, is handled whether it opens before or after
the rule. A replay's or script's rules end with it, a client's call replaces its earlier rules, and
`app_state` lists the rules in force.

A dialog that hosts a web view is described with `web: true`; its controls are on its page, which
`widget_list target=dialog` lists and the input tools drive like any widget (see Web pages below). It
has no text of its own, so a rule matches it by title. Its page is the one its shown web view holds; a web
view the dialog keeps hidden has no page to press on. `dialog_answer` looks a name or label up as a
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

`webview_run_javascript` runs JavaScript in a page through its agent, as a recover call: in a web view of the
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
`ExecuteScript` handler, WKWebView's `evaluateJavaScript` block, WebKitGTK's `webview_run_javascript`
callback. wx's own `RunScriptAsync()` keeps a plain pointer to the web view in its callback on every
port, and a page may close its dialog, destroying the web view, at any time. `PageCalls` start calls
on the UI thread and wait for them on the job thread, a destruction watcher ending the wait for a
web view that goes. Job and request threads hold a web view as a `WebHandle`, a number only the UI
thread maps back to it, never as a `wxWeakRef`: making or dropping one edits the window's list of
refs, which the UI thread edits at the same time. A web view whose native view is not made yet, which WebView2
makes a moment after its window opens, holds the call's request and sends it once the view is there, trying every
100 ms for up to 10 s.

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
reaches the others, which get the note `behind a modal dialog`. `widget_list` waits up to 2 s for a
page still loading, then reads every page at once, for up to 3 s together.

A locator `{web, kind, css, within, index}` matches the label exactly, else case aside, else as a
part of it (a card listed by all of its text); a text line only when no element matches. `css` is
matched through shadow roots and frames. Without `within`, a locator looks in the topmost dialog's
pages while a modal shows one, else in every shown page; `index` picks among the matches of all of
them. It waits for a match up to 2 s short of the call's `timeout_s` (8 s of the default 10), but
at least 3 s or the whole of a shorter timeout, since pages fill in after they load and a dialog's
page loads after the click that opened it has returned, later still on a slow or busy machine. The
last search has what is left: 2 s once the timeout is 5 s or more. A
match while another candidate page has not answered is refused, since that page may hold more
matches and neither uniqueness nor `index` is then known;
`within` or a ref narrows the search. The journal records the page's own locator for the
element acted on (its id, else its label and kind, else a CSS path), with `within` and an `index`
among the matches of every candidate page and its configuration frames, as a replay searches them
(`journal_index()`); with no `index` when one of them was not heard from (silent, failing the search
or unable to run the agent) or the action took the element away, so a replay that finds several asks
for one rather than taking another. A screenshot of a page element is journalled as one of its web view, which it
captures. A web view's own ref is no page element: `widget_click` and `widget_set_value` refuse it, pointing to
the page's elements, and `keyboard_press` sends to its page's focused element.

An action runs synchronously in one task of the page. The element is scrolled into view and hit
tested: one covered at its centre is refused naming what covers it, a disabled one and a file input
are refused. A click sends what a person's does — pointer and mouse over, down (focusing the element),
up, then `widget_click` — so the browser's default actions follow: a label reaches its control, a link
navigates, a submit button submits its form with itself as the submitter. `widget_set_value` sets a value
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
until any other gesture, click, `dialog_answer` or auto-answer press, a widget's included and even
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
a client's when the endpoint closes, every one when the runtime stops, and a client's down sent
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
itself before the action, a line per element (with its value, state and the notes `widget_list`
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

A URL a reply shows (`navigated`'s, `webview_run_javascript`'s, a web view's value in `widget_list`) reads
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
whose handler outlasts `dialog_answer`'s 3 s may still be running when `cancel` or `close` ends the
dialog natively; cross-origin frames other than plugin configuration UIs are listed but not read.

## Screenshots

`window_screenshot` captures on the UI thread and encodes off it. It has three targets.

- `canvas`: the visible 3D view, the Design view while the Design tab is shown, exactly as drawn,
  toolbars and ImGui panels included (the Design view's sketch overlay and status line too), on
  every platform. `GLCanvas3D::render_and_capture()` forces a frame and reads the back buffer just
  before the swap; it fails, saying why, when the canvas is hidden or mid-frame. That frame is often
  the first since a tool's change, yet it shows the change settled, since slider moves reach the
  toolpaths before they are drawn and the capture first draws any extra frames the change asked for:
  a capture right after `view_set` shows the layers its slider shows and the legend laid out.
- `main`: the main window's client area from `capture_native_window()`: its own pixels or, where
  the capture reads the screen (X11, macOS 14.4 and later), our windows over it as well, never another
  application's, with every visible 3D canvas composited in from its canvas capture, since window
  capture loses OpenGL content on several platforms.
- `dialog`: the topmost modal, captured like a window.
- a widget's ref or locator: its top-level window captured like a window (a window that is not
  modal, such as an object's settings, has no other target: `window_list` gives its ref) and cropped to the widget, or the
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
is in it too. It leaves out the shown windows hidden from automation (a floating Run Script panel, the
person's own prompts), their wx
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
views leaves them out. The full-size PNG goes to `save_path` or `OrcaSlicer-screenshot-<timestamp>.png` in the output
folder (`output_dir()`: the `--outputdir` of a `--run` launch, else `<datadir>/automation/out`). The image
block is scaled to `max_dim` on the long edge (default 1280, clamped to 256–2000).
`include_image: false` returns only the path.

## Recording

`recording_start` records what OrcaSlicer shows into a video while an agent, a replay, a script or a person
works, and `recording_encode` turns it into an MP4 to attach to an issue. Recording has to stay light,
since it runs while the app works: frames are kept as JPEG in a Matroska file (MJPEG), and the H.265
encode, which costs far more, comes after it. There is no audio. `McpRecorder` holds at most one
recording; `app_state.recording` shows it while it runs.

The recorder starts with the runtime and needs none of the GUI instruments. It is built from the modal
tracker, whose stack it draws, the UI generation its kept canvas frames belong to, whether the main window is
being rebuilt, and a way to queue a function on the UI thread, not from the runtime. So a person's Start
Recording with the server off starts the runtime without the instruments: its captions, pointer and dialogs
are recorded all the same, and the person's own menus show as they do without automation.

**Capture.** The UI thread captures a frame per tick of a one-shot `wxTimer`, which fires inside
modal loops too. `recording_start` takes the first frame in its own UI closure: it fixes the video's
size (fit to `max_dim`, rounded down to even numbers for 4:2:0), and a failure fails the call before
any file exists. The `main` target is the main window as `window_screenshot` captures it, with the modal
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
under the recorder's mutex, since `recording_stop` reports from another thread while a tick may be
capturing. The `canvas` target is `shown_canvas()`. A recording captures with
`CaptureOptions::hide_covering` off, so X11 never hides and re-shows a floating Run Script panel at each frame.

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

**Ending.** `recording_stop` first captures one more frame in a UI closure, when the UI thread can run
it, so the last call's effect is in the video even if the timer has not ticked since. A recording ends on
`recording_stop`, after `max_seconds`, at 4 GiB, on a write error, when
the replay or script that started it ends (`run_replay_job()`, which names a file still being written after 30 s
as the result's `unfinished_recording`), when the endpoint closes for a client's
(`close_client_scope()`; a run's carries on), and at exit. Ending from any thread sets a flag the next tick
reads, tells the sink to finish, and posts the timer and input filter's cleanup to the UI thread; the
sink drains its queue, writes the last frame again at the end time so the video lasts until then,
closes the file and calls back, on its own thread. So ending never needs a UI round trip: a run's
end waits for the file without deadlocking against `AutomationRuntime::stop()`, which joins the followed calls'
threads on the UI thread, and a step after the replay sees a closed file. At exit `stop()` waits 5 s for the
file, then drops what is still queued and leaves the sink to its threads, which own its state. A
recording counts as active until its file is closed, for the one-at-a-time rule and for encodes.

**Encoding up front.** `recording_start` takes the Start Recording dialog's encoding keys (`encode`,
`encode_preset`, `codec`, `crf`, `speed`, `profile`, `keep_raw_video_file`), checked by
`apply_recording_overrides()` with its defaults, the Balanced preset, and refuses `encode` without an ffmpeg
before anything records. The encode deletes the `.mkv` once the MP4 is written unless `keep_raw_video_file`
keeps it, as every encode does (see Encoding). The recording keeps `recording_encode`'s arguments (`RecordingOptions::encode`), and the `recording_stop`
that reports it, once its file is closed, starts the encode with `start_encode()` (`McpToolsRecord.cpp`): a job of
its own, outside the journal (a replayed `recording_start` asks for it again), whose envelope the report
carries as `encode`, so the agent waits for the MP4 by that job's id. A recording that ended on its own
is encoded when the next `recording_stop` reports it; one nothing reports, a client's that ended with
the endpoint, is not encoded. An agent can still encode any recording later with `recording_encode`.

**From the app.** The Speed Dial actions Take Screenshot, Start Recording and Stop Recording (the actions
`take_screenshot`, `start_recording` and `stop_recording` of the commands `window_save_screenshot`,
`recording_start` and `recording_stop`), which the Help menu also has, give a person screenshots and
recordings without an agent: a PNG of the main window, and a recording of it for up to an hour. Both go in
the person's Downloads folder unless they chose another. `window_save_screenshot` takes no arguments and is
not listed; `recording_start {}` and `recording_stop {}` are the person's forms of the commands above
(`with_person_form()`, see Tool catalog): given arguments, or `silent: true`, they run from their
arguments, and `recording_start` then also records for up to an hour unless `max_seconds` says otherwise.
Start Recording opens `RecordingDialog`, which splits its settings by the file they make. Record holds the
JPEG quality of the `.mkv` and a switch for each overlay, the pointer and the caption, both off unless the
person turns them on. Encode to MP4 holds whether ffmpeg encodes it once it ends, with a built-in preset or
codec, CRF, speed and profile, and whether the `.mkv` stays beside the MP4 (off unless the person turns it
on); it is off and disabled when no ffmpeg is found, and says how to get one. A
rail joins the two stages and shows whether the second runs. The dialog remembers what it starts with in
AppConfig's `recording` section (`RecordingSettings`). The encode is
`recording_encode` itself, run by `McpRecorder::encode_when_closed()` as an unjournaled job that the runtime
follows (`follow()`) once the recording's file is closed (`McpRecorder::wait_closed()`, in the call's `ready`), so
it can be cancelled like any encode. No reply
names the job, which only exists once the file is closed: an agent finds it in `app_state.jobs` and waits
for it by its id, as the command's description says.
Start Recording starts the runtime without the endpoint when the server is off, as Action Replay and the
Run Script panel do. The person's recording has its own owner (`kPersonOwner`), so neither a run's end nor the
endpoint closing ends it. Whatever ends it tells the person where it went, in a notification whose button
opens the folder: Stop Recording itself, which stops any recording, an agent's too, ending it as
`stopped_in_app`; or `RecordingOptions::ended`, which the recorder runs on the UI thread when an agent's
`recording_stop`, the time or size limit, or a write error ends it, and which starts the encode. A second
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
mouse (`widget_click`), shown at the control's centre. Events in windows the frame doesn't show, and in
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
`printhost_apikey` is a plain text field) shows `•••` for its value; `widget_set_value` shows its value only
for a non-sensitive setting located by its key, since a field located by its label, or a ref not yet
resolved, may be a password box; and `keyboard_press` shows its chord, never its text.

**Encoding.** The bundled FFmpeg only decodes, so `recording_encode` runs the system `ffmpeg`:
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
(`NativeCommand::cancellable`: `job_cancel {job}` stops it through `JobRegistry::request_stop()`). On Windows it is
ended with `TerminateProcess()` on its handle and waited for on that handle, since `TerminateProcess()`
returns before the process is gone and boost's `terminate()` closes the handle right after it; a child not
gone within 5 s keeps its files, which the error names, and its source and destination stay reserved for
the session, since it may still be using them. `RecordingFiles`
keeps the files in use, compared by `path_compare_key()` (Unicode case-folded on Windows and macOS): the
recording until its file is closed, and each encode's source and destination. Encodes may share a source, but one
that deletes it holds it alone, and nothing writes a file anything else uses. Output goes to a temporary file of the
call's own, renamed over the destination on success; the call's files (that, ffmpeg's log, and the listings it
read) are removed whatever the outcome, and no other call's.

An encode is asked for to get the MP4, so once the MP4 is written it deletes the `.mkv` unless
`keep_raw_video_file` keeps it, the one name `recording_encode`, `recording_start` and the Start Recording dialog
share; an encode that fails or is cancelled leaves it. It deletes only a recording this session made
(`RecordingFiles::is_finished()`): any other `.mkv` is the person's own file, which stays, and the reply's
`raw_video_file_deleted` and `note` say so.

## Preview data

`plate_slice_stats` reads what the Preview shows for a plate without selecting the plate. Its tables
come from the plate's `gcode_statistics_json()`, the serializer behind the slice reply's statistics
and the exported `statistics.json`, which sums the moves as the legend does: `summary` is the slice
reply's `plate_summary()` with the prepare and travel times and the travel distance; `features` has
each feature type's seconds, share of the normal-mode print time and grams, keyed by the English
`;FEATURE:` names (`ExtrusionEntity::role_to_string()`), and a `Travel` row (the shares leave out
retractions, wipes, Z moves, tool changes and the prepare time, as the legend's do); `layers`,
paged, numbers layers as the slider's value (G-code layer index + 1, which `view_set {layer}` and
`app_state.preview` use) and carries each one's Z, so tools that number layers by the slider's label
can be joined on Z. `objects` lists the G-code's object instances, which it names by object and
instance number; an entry gets the `object_list` index only when one model object has that name.
The wx-free shaping (`preview_features()`, `preview_layers()`, `preview_objects()`) is unit tested.

`view` is the legend's state, read from the libvgcode viewer it draws from
(`GCodeViewer::get_viewer()`), so only for the plate the Preview has loaded (its
`last_result_id()` is the plate's result id; the Preview loads a plate only while shown, so after
another plate is selected it keeps the last one's): view type, time mode,
the layer slider's range with its Z, which feature types and options are visible, and each view
type's colour range. The viewer recomputes ranges at its next frame after a legend change, so the
closure draws the Preview first; the ranges span the whole print but count travels, wipes and Custom
extrusions only while those rows are shown. For another plate `view` is null and a `note` says how to
show it. `app_state.preview` is the same state without the legend, or null when the Preview holds no
toolpaths of the current plate.

The tool refuses while anything slices or exports (`Plater::is_slicing_any()`), since a worker may be
rewriting a result whatever its valid flag says, and for a plate without an up-to-date result. A
G-code file opened for viewing fills the current plate's result without marking the plate sliced;
that plate is read while the Preview shows it (`Plater::only_gcode_mode()`). Of a project opened from
a sliced 3MF, a plate the Preview has not shown keeps only the file's totals, without moves, so it is
refused with "slice again for layer data". The serializer's pass over the moves runs on the UI
thread, about 15 ms on a plate of 4 million moves.

`view_set` changes the Preview through the functions the legend's own controls call
(`GCodeViewer::pick_view_type()` for the combo, `toggle_extrusion_role_visibility()` and
`toggle_option_visibility()` for its rows, `set_time_mode()` for the mode button), so a call leaves
the Preview as the same clicks would. Picking a view type therefore shows every feature type again
when it is Line Type, and is written to the app config as `preview_last_view_type`, which the "Last
used" preview default restores. Before a slice there is no legend to pick from, and `view_type` only
sets the type the next toolpaths show. `legend` names rows as `plate_slice_stats` lists them, in any case
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
`PrintHost::get_print_host()`) goes through the GUI's own dialog. `print_start` and `printer_send`, whose Speed Dial
action is `send_gcode` ("Send G-code to printer host", the dialog's title), run `Ops::open_print_dialog()`, which takes
the route of the Print button's action of that name from `MainFrame::available_print_actions()`
(`Ops::print_route()`) and then runs the button's own call: the plater's handler of `EVT_GLTOOLBAR_PRINT_PLATE` or
`EVT_GLTOOLBAR_PRINT_ALL`, processed in place, for Print plate and Print all; `Plater::send_to_printer()` for the
storage dialog of a printer on the Bambu network or with printer agents; `Plater::send_gcode_legacy()` for any other
send. For a printer with a print host it opens the Send dialog, whose file name an agent sets with
`widget_set_value` and whose Upload or Upload and Print button it presses with `dialog_answer`. The plate is then
sliced if needed, its G-code exported in the background process, and the upload queued. Before an action the
button checks, `print_start` applies the slicing result (`Plater::apply_background_progress()`) and asks the
button's rule for it, `MainFrame::print_action_enabled()`, the rule `get_enable_print_status()` applies to the
button's selected action; a plate not ready is refused `not_sliced`. `plates` names a plate (`Ops::select_plate()`)
or every plate where the button offers that. Host settings are printer settings (`host_type`, `print_host`,
`printhost_apikey`, and `bbl_use_printhost` on a Bambu printer), written with `settings_set {type: "printer"}`; a
route through a print host with an empty `print_host` is refused `no_print_host`, whether or not the project has
objects. The calls are journaled like any call, so a replay opens the dialog again, and the replay summary flags
them as sending to a printer.

`printer_test_host` makes the call the connection dialog's Test button makes, `PrintHost::test()` on a host built
from the printer preset's settings as they stand (`Ops::test_print_host()`), on the UI thread, which does not
redraw while the host is asked. It replies `ok` and the button's message in the app's language
(`get_test_ok_msg()`, `get_test_failed_msg()`). The button goes on to a login dialog for a cloud host whose test
fails; the command stops at the failed test.

`app_state.uploads` lists the session's rows of the upload queue (`PrintHostQueueDialog`), as the
queue window shows them: `id` (its ID column, from 1), `state` (`queued`, `uploading`, `error`,
`cancelling`, `cancelled` or `completed`), `progress` (percent), `host`, `filename`, and `error`, the
queue's message, when there is one. Uploads a person started are listed too, so an agent can wait
for any of them by polling. A failed upload also opens the queue's error dialog
(`PrintHostQueueDialog::on_error()`), which blocks changes until it is closed with
`dialog_answer`.

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
{ "orca_replay_version": "0.2.0", "name": "repro #12345",
  "vars": {"model": "${replay_dir}/part.stl"},
  "steps": [
    {"tool": "project_import", "args": {"paths": ["${vars.model}"]}},
    {"tool": "plate_slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}},
    {"tool": "window_open", "args": {"window": "preferences"}, "await": false, "save_as": "prefs"},
    {"tool": "dialog_answer", "args": {"dialog": {"title": "Preferences"}, "button": "close"}},
    {"tool": "app_wait", "args": {"for": "job", "job": "${jobs.prefs}"}},
    {"tool": "window_screenshot", "args": {"target": "canvas", "save_path": "${out_dir}/preview.png", "overwrite": true}}
  ]}
```

`orca_replay_version` is the semver of the replay format (`REPLAY_VERSION` in `McpReplay.hpp`), which
an exported replay is stamped with. A minor version adds tools or fields, so a build reads replays of
its own major version up to its own minor version; a major version renames or removes them. While the
major version is 0 any change may break a replay, so a build reads only its own minor version. A
replay a build can't read is refused before any step runs, saying whether it needs a newer
OrcaSlicer (`version_error()`). Format `0.2.0` names the commands as the Tool catalog names them, so a
replay of an earlier format, written with other names, is refused with that message rather than with an
unknown tool in its first step.

A replay is JSON with comments allowed. `replay_run` runs a file or an inline replay as a job whose
steps call tools as its children (`ToolContext::call_child()`), so steps are not journaled and a
replay's `app_wait {for: "idle"}` doesn't wait for the replay. An inline replay is the replay object or
its JSON text, comments included, so a caller holding a replay file's text passes it as read; the
journal records the text as the object it parses to. Every string is substituted recursively
with `${replay_dir}`, `${out_dir}`, `${env:NAME}`, `${vars.NAME}` and `${jobs.NAME}`; a string that
is just one placeholder becomes the referenced JSON value. A replay's own `replay_run` step
gives its nested replay as an object: the outer run splices placeholders into a text replay as plain
text, so a value holding a quote or a backslash, such as a Windows path, breaks its JSON. A step that returns a job is awaited,
up to `timeout_s` (default 120), before its `expect` is checked; a wait that times out with a dialog
open names the topmost one, since a step can't answer a dialog it opened. `"await": false` returns the job
at once and stores it under `save_as`, so later steps can answer a dialog it opened and then
`app_wait` for it; a call that finishes within its reply budget, which depends on how busy the machine is,
leaves no job, and an `app_wait` on it gives the step's own result at once. `expect` maps JSON pointers into the result to matchers (`eq ne gt ge lt le contains
regex exists empty`). A failed step or expectation stops the run unless the step is `optional`.
Auto-answer rules registered by a replay end with it. `vars` passed to `replay_run` override the
replay's. A step naming an unknown tool makes the replay invalid before anything runs; so does any
field of the wrong type, and broken JSON is reported with its line and column. The result, also
written to `<out_dir>/result.json`, is `{passed, name, failed_step, steps: [{index, tool, args, ok,
duration_ms, result | error, expect_failure, cancelled, reason}], out_dir, result_file}`. A failed
step's `reason` says why in parts a host can word: `cancelled`, `timeout {seconds}`, `expect {pointer,
op, expected, actual, exists}`, `expect_invalid`, `placeholder {text}`, `missing_job {name,
saved_by}` (the step that saves that job, when one does) or `tool` (the tool's own error). A replay
that names no `out_dir` writes to a new folder, `replay-<timestamp>` (`replay_output_dir()`): in the
`--outputdir` folder of a `--run` launch, else in `orca_action_replay` in the app's temporary folder
(`temporary_dir()`), so a run leaves nothing beside the replay file or in the data folder. A name already
taken, as by the replay that runs this one as a step in the same second, gets `-2`, `-3`... An inline
replay's `${replay_dir}` is its `out_dir`.

The engine (`McpReplay.cpp`, wx-free) and the job around it (`run_replay_job()` in
`McpToolsReplay.cpp`) are shared by `replay_run`, `--run` and Action Replay. A run
stops before its next step, and stops waiting on a step's job, once the runtime stops or a stop was
asked for any job in its lineage (`JobRegistry::request_stop()`), so a nested `replay_run` stops with
the run that holds it. Stop requests last the session, and `JobRegistry` records each job's ancestor
chain when it is created, so a descendant still reaches its root after finished ancestors are
forgotten. A step during which the stop came is failed as `cancelled` whatever it returned (a
cancelled slice still reports its plates) and ends the run, `optional` or not. The engine reports
each step as it starts and ends, and can run a single step with the jobs earlier runs saved.

`orca-slicer --run FILE [--outputdir DIR]` runs a replay (`.json`) or a Python script (`.py`) once the GUI
is ready. `--run` itself asks only for the runtime: the endpoint opens with it when `--mcp` or `--mcp-port` is
also given or the saved `mcp_enabled` preference is on. `--outputdir` names the
session's output folder: `start_launch_run()` hands it to `set_run_output_dir()`, so `output_dir()` returns
it and files a call saves without a path go there, and a replay writes `result.json` and its screenshots
there. Without it a replay writes to a new `replay-<timestamp>` folder in the temporary folder, as
`replay_run` does, and `output_dir()` is `<datadir>/automation/out`. Then the run closes the app without prompting and exits 0 if the replay passed
or the script finished, 1 if a step failed or the script did not finish, and 2 if the file is invalid,
can't be read or is neither a `.json` nor a `.py` file. A dialog still open then is ended as its Cancel or
close box ends it, a message box by its Cancel, No or OK, and the main window closes only once no
`ShowModal()` runs (`close_after_modals()`, which waits for the next idle event of the main loop): closed
from inside a dialog's nested loop, the window would be destroyed with that dialog's `ShowModal()` still
on the stack. A native file picker can't be ended, so the close waits for it. The run's session is the
script's, not a person's (`GUI_App::launched_for_run()`): the startup question about restoring an unsaved
project is not asked, and neither it nor the projects the run opens move `last_backup_path`, so a
person's backup is still offered at their next launch. A `--run` launch keeps a window of its own:
`GUI_Run()` calls `instance_check()` with `never_send`, so the launch is never handed to a running
instance, whatever the `single_instance` preference says, and its exit code is its own. The call still
sets the instance hash, which `post_init()` reads. Where no GUI starts, beside a slicing action or
`--downward-check`, `CLI::run()` refuses `--run` with `CLI_INVALID_PARAMS` and a line on stderr that names
it, rather than leave the file unrun. `OrcaSlicer.cpp` resolves both paths against the original working
directory, since the GUI changes it. wx ignores `OnExit()`'s return value, so `GUI_Run()` returns
`launch_exit_code()` (`LaunchRun.hpp`) after `wxEntry()`.

### Action Replay

File › Action Replay…, `window_open {window: "action_replay"}` and its Speed Dial action (whose id is
`open_action_replay`) open `ActionReplayDialog`, where a person opens or writes a
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
`${…}` placeholders; the tool part comes from the command catalog's listed tools (`editor_tool_catalog()`),
which the dialog sends once the runtime is ready, so the suggestions match the build. Save writes the text
back to the file it was opened from, or
asks where to put a new replay file and offers the replay's name as the file name. A replay saved
somewhere new is that file's from then on: `${replay_dir}` follows the file, as for an opened one. The
window's runs of a replay write into one `replay_output_dir()` folder, named when first needed, so
replaying it again replaces its files; opening another replay or saving this one somewhere new names
another. Closing the window, or opening another replay, with unsaved edits asks first. The page
reports every edit to the host, which parses, runs and saves that one copy of the text, so the host can
ask and save even when the window's close box starts the close. Opening the window starts the runtime
without the endpoint, as `--run` does, so a person replays files with the server off.

**The editor.** Action Replay and the Run Script panel edit in Ace (`ace-builds` 1.45.2, BSD-3-Clause),
vendored unmodified in `resources/web/include/ace/` (its `src-min-noconflict` core, the JSON and Python
modes, the language tools and the search box) and set up by `resources/web/dialog/js/code_editor.js`.
The pages are `file://` URLs, so they load it with script tags, and nothing is loaded on demand: no
workers, no snippets, the default commands that would load a file the repository doesn't ship (the
settings menu, error navigation, go to line, Ace's own command palette, the mode select) removed, the
completers set to the page's own as an array, and the theme applied as an object built from the host's
theme tokens, never by name. Ace draws only the rows on screen, and brings indentation, bracket matching,
folding, search and replace, multiple cursors and its own undo. Its own text (the search box, the
suggestion list, the read-only note, folding and its ARIA labels) is translated: the host puts
`WebViewHostDialog::code_editor_strings()` under `code_editor` in the page's strings, keyed by Ace's
message keys, and `code_editor.js` hands them to `ace.config.setMessages()`. Each page reports the
chords its editor binds (`editor_keys`), which the main frame's menu rule reads (Keys, under Run Script
panel). Action Replay's language is a JSON mode of its own (`orca_replay`, `replay_editor.js`).

`start_replay_run()` (`McpPersonRun.cpp`) runs the replay as a job of `replay_run`'s kind but outside the
session journal, since a person's run is not an agent's call, with the person mark. The runtime follows the
job (`follow()`) past a `running` or `blocked_by_dialog` reply to its result, which it posts once. Step reports
and the result reach the dialog through the runtime's dispatcher as
`PersonRun` events, which the dialog drops once it is gone. `PersonRun::stop()` latches, so a stop
asked before the job exists still counts, requests the stop for the job (`JobRegistry::request_stop()`)
and cancels its operation as `job_cancel` does. Every call the run and its nested replays make
then sees `ToolContext::cancelled()`, so a step that waits, such as `app_wait`, returns at once. `suspend()` and `stop()`
stop every run first (`stop_followed()`), while the main window they run in is still the current one.

Before a run the dialog describes each step with `describe_step()` (`McpReplaySummary.cpp`, wx-free):
what it acts on, and flags for effects beyond the open project (writes a file outside the output
folder, changes presets, replaces the project, sends to a printer). It reads the
arguments after substitution, and nested replays as their run sees them: an inline one with the outer
replay's placeholders already applied, as `replay_run` substitutes the whole step first, and a file
with its own vars and output folder; a step whose effect can't be
read (a plugin action, a nested replay too deep or unreadable) gets every flag. Flags only inform. A
replacing step either checks unsaved changes itself (`project_new` and `calibration_generate_test` called with
their arguments, `project_open` of a project) or goes through the app's own save question
(`close_with_confirm()`): a call of an action's arguments, which asks as the action does (`project_new {}`,
`project_open {}`, `calibration_generate_test {test}`), and `project_open` of a G-code file. When the project
has unsaved changes and the steps to run hold a step of the first kind, or one of the second kind
with a step that can answer a dialog, the dialog asks whether to save first. Yes saves; No lets the
run's replacing tools treat the run as `discard_changes: true`: the run's handler marks its job as it starts
(`JobRegistry::allow_discard()`), the mark counts while the job is unfinished, and a tool reads it from its
call's lineage when it checks (`discard_allowed_in()`), so changes are dropped only when a step really
replaces the project. The runner never answers the app's own question.

The window is one the GUI tools reach, as the app's other windows: `window_list` lists it with a ref,
`widget_list`, locators and `webview_run_javascript` read its page, and `keyboard_press Escape` by its ref closes it, so
an agent or a test can close the window it opened. A replay run from it sees it too: a key sent without a target goes to
the focus, which is in this window once the person has pressed Run, so a replay meant for the main window gives its keys
a target. Every prompt and file dialog it opens is named `kHiddenFromAutomation`, so no tool and no auto-answer rule a
replay registers answers the person's own question. Its own wording is
translated, from the structured step descriptions and failure reasons; a tool's own error, which
agents read in English, is shown as a detail under a translated lead. Tool titles are marked with
`L()` so the dialog can show them translated while `tools/list` keeps the English.

The session journal records client calls to journaled tools in order: every mutate tool, plus
`app_wait` and `window_screenshot`, which a replay needs. It keeps at most 5000 calls and 64 MiB of their
arguments (serialized), so a long CAD run's large `sketch_draw` calls can't grow it without bound:
after each change it drops the oldest calls whose outcome is known until it is within both. A call
still waiting for its reply or its job keeps its place, so calls still waiting may hold it over for a
while; once they finish they are dropped like any other call, oldest first, and only while the
journal is still over a bound. GUI tools record locators instead of refs through
`ToolContext::set_journal_args()`. `replay_export` returns it, and with `path` writes it, through `Journal::to_replay()`; `clear`
starts a new recording. `Journal::to_replay()`
drops failed calls and an input call that only listed the menu it opened when the next call repeats
it with a `popup_item` (otherwise it stays: its right click changed the selection), turns a call that
returned a job into a step with `"await": false` and
`"save_as": "jobN"`, points the `app_wait {for: "job"}` that followed at `${jobs.jobN}` (dropping waits
on jobs it never saw start) and rewrites screenshot paths to `${out_dir}/<file name>`. It moves a
recording's path there too (`recording_start` records the one it used), and an encode's `save_path`, and
an encode's source when an earlier `recording_start` recorded it; another source stays where it is. A
`action_run` of a command's action, named by its key or its full id, is rewritten as a call of that command,
inside the action's `{id, args}`, so a recording started from the Start Recording action moves the same way.

A call that runs as a job is judged by its job's final result, not by its reply. `McpCore` tells the
journal the job's id when it starts it (`Journal::job_started()`) and its result when the handler
returns (`Journal::job_ended()`), before `JobRegistry::finish()`, so an export right after the
`app_wait` that saw the result already knows it. A job that fails after its reply came back as
`running` is dropped like a call that failed at once, so a replay never repeats a call that did
nothing (a slow CAD rebuild that then fails). A call whose outcome is still open, because its job
is still running or it has not replied yet, is left out, and `replay_export`'s reply lists it in
`left_out_running: [{tool, job}]` (`job` is absent for a call that is not one); a later export
includes it once its job has succeeded. A call whose reply was `blocked_by_dialog` stays in the
replay while its job runs and whatever its job's result: the steps after it answer its dialog, and
a replay without it would fail at the first of them. A call whose job was still running when a later
call acted with a modal open (`dialog_answer`, or a recover tool's input while
`UiInvoker::open_dialogs()` lists one) stays the same way, whether it had replied yet or not, since that
dialog may be one its job opened; the journal can't tell whose dialog it was, so it keeps every such
job, as a step with `"await": false`. Its replayed reply can come before the dialog opens, so each
call that acted with a modal open is preceded in the replay by `app_wait {"for": "dialog"}`, expected to
be satisfied. Only the call's own job counts as its job: a
`app_wait` that timed out names the job it waits on in its reply, and stays a plain step.

## Python scripts

A Python script is a `.py` file or a code string that runs on the plugin interpreter as an embedded macro: the
main program of a run inside OrcaSlicer, with the whole plugin API (`orca.host`), without being packaged as a
plugin. A person runs one in the Run Script panel or with `--run file.py`; an agent sends one with
`script_run {code, args, timeout_s, filename}`; a plugin's code may call `script_run` too. A script that runs
under `python file.py` runs the same way here, and code written for a plugin's `execute()` (windows, progress,
presets, storage) runs from a script unchanged. A script is not a plugin: it gets no permission prompt and no
grants, is never in the Plugins list, and ends when its work ends, leaving nothing loaded. It always runs; one
setting decides whether the plugin permission checks hold it (The setting, below).

**Entry contract.** `run_python_code()` (`plugin/PythonScriptRunner.cpp`) runs the code on the calling thread, the
MCP job that runs `script_run`, holding an interpreter lease and the GIL, as a session with one file work (Run
Script panel, below). The code is text, compiled as Python
compiles any code string, so a coding line in it changes nothing; a file's bytes are decoded first as Python reads
a source file, by its BOM or its PEP 263 declaration, else as UTF-8 (`decode_python_source()`). It runs as the main
program, in a `types.ModuleType("__main__")` of its own, with `args` bound and `orca` importable. A run is a *file
run* when `filename` names an existing file (a saved script's in the panel and `--run`'s do): `__file__` and
`sys.argv` (`[path]`) name it, its folder serves its imports, and its PEP 723 dependencies are installed first.
Any other name (the default `<script_run>`, a bare traceback name) is code sent as text, which gets none of that;
`filename` still names it in tracebacks. The value left in `result` is read once when the body returns; in code
sent as text, one that is not JSON (a dict with string keys, a list, a string, a finite number, a boolean or None,
nested) makes the run `raised`, while in a file run, where a standalone script may well have a variable of that
name, it is no result, and the output ends with a line saying it was not JSON. `SystemExit` with code 0 or None
finishes the run. Output is captured up to 256 KiB by a stream installed once over `sys.stdout` and `sys.stderr`
that routes writes by the run bound to the writing thread: the run's own thread, the threads it starts, its pool
tasks and its window callbacks; other threads print through. The outcome is `finished`, `raised` (with the
traceback), `cancelled` or `timed_out`, reported in a successful result, as `replay_run` reports a failed step.

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

**Cancellation.** A watcher thread interrupts the run with `PyThreadState_SetAsyncExc` when `job_cancel {job}` stops
the job (`script_run` is `cancellable`) or the time budget (`timeout_s`, default 300 s; the panel's session has
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

**Identity.** A person's script runs under the audit identity `(script)`, an agent's under `(mcp)`
(`kScriptIdentityPerson`, `kScriptIdentityAgent`); `JobRegistry::person_in()` tells them apart by the run's
lineage, since the jobs of Action Replay, the panel, `--run` and a person's pick of a long command carry the
person mark while they are unfinished (Threading). So a `script_run` that the panel's session calls through
`orca.host.tools` runs as the person's too. The plugin audit
([plugin-permissions.md](plugin-permissions.md)) judges a script under its identity as it judges a plugin, with
one difference: a script has no install state to hold a grant, so it is never asked, and what the implicit
rules don't allow is denied with a `PermissionError` that names the setting. The threads, pool tasks and window
callbacks of a run carry its identity. A run owns what its calls leave behind, as a replay does
(`begin_run_scope()`): auto-answer rules, a recording and a held press end with it. No plugin has one of
the identities as its key, so a plugin is never judged as a script: a local plugin's key keeps only the letters,
digits, `_`, `-` and `.` of its file name (`make_local_plugin_key()`), and a cloud plugin's is its UUID. A
plugin's `script_run` runs under the plugin's own identity and grants instead, with the same
main-program environment, but its windows, callbacks and storage stay the plugin's.

**The setting.** One switch, `python_scripts_unrestricted`, off by default, decides how a person's scripts and an
agent's `script_run` are judged (a plugin's `script_run` is the plugin's and ignores it), and the command line's
`--unrestricted-scripts` turns it on for one run without writing it. Off, a script is
restricted: its Python code reads and writes OrcaSlicer's data folder, reads the resources folder, the standard
library and, in a file run, the folder of its file (`PythonRunScope::allow_folder_reads()`, a scoped root on
each of the run's threads, so its imports work), and is denied every other audited access: files elsewhere,
programs, threads and the network. On, every audited access is allowed to it
(`PluginAuditManager::set_scripts_unrestricted()`, which `apply_python_scripts_setting()` sets at startup and
on a change to the value in force, and the audit reads on whichever thread raises the event). The categorical denies every plugin has
(`is_denied_path()`) hold either way. The audit is a Python audit hook, not a sandbox: the tools a script calls
through `orca.host.tools` and the rest of `orca.host` run OrcaSlicer's own code and are not judged, so a
restricted script still does what an agent's tool calls do. The setting is changed only in `PythonScriptsDialog`,
opened from Preferences › AI features › Python scripts, which is named `kHiddenFromAutomation`: the UI tools never
see it and `ModalTracker` leaves it off the stack, so no dialog tool or auto-answer rule reaches it. No tool and no
command line option writes the key. Two functions keep the saved setting apart from the value in force:
`python_scripts_setting()` is the key, which the dialog and the Preferences row show and the dialog writes, and
`python_scripts_unrestricted()` is the key or the run's flag (`set_python_scripts_for_run()`, called once at
startup), which the audit gets and `app_state.python.scripts_unrestricted` reads, so an agent learns what its
script may do before it plans a run rather than from a refusal. While the flag is on, the row adds "(on for this
run by the command line)" and the dialog says the flag is on under its checkbox.

**The host surface and the bridge.** A script reaches the whole `orca.host`, as a plugin does: reads, `orca.host.ui`
(message boxes, progress dialogs, HTML windows, dock panels, notifications), the staged preset update, bundle
reload and the 3MF copy; nothing is plugin-only. `orca.host.plugin.storage()` is a script's own folder, the folder
of its file, which a restricted script reads but doesn't write; code sent as text has none. `orca.host.tools.call(name, args)` and `run_replay(replay, vars)`, whose
replay is a dict or its JSON text and which calls `replay_run` (the Python API keeps its own function names;
`plugin/host/PluginHostTools.cpp`), reach the commands by their names from any thread of the run
while it lasts, through the run's bridge: its calls are `ToolContext::call_child()` of the `script_run` job, so
each has its tool's guards, admission and undo, and the GIL is released while it waits. At the end the bridge
closes, and the run waits for calls already inside it before its `ToolContext` goes. An error result raises
`orca.host.tools.ToolError` with its data as `details`. Code with no audit identity at all is refused.
`orca.host.tools.list()` and `describe(name)` only read the command catalog, so they need no run and work
on any thread, for any code: `list()` gives every command of `NativeCommands::instance()` and then the
operations its sources hold, each `{name, title, group, description, read_only, listed}`, and
`describe(name)` one command's entry of the command reference (`command_reference()`, see Making a feature
reachable), or raises `ToolError` with `did_you_mean` for a name the catalog does not resolve.

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
its title. A plugin's `script_run` leaves all of this to the plugin.

**Idle.** A `script_run` job doesn't count as busy for `app_wait {for: "idle"}` (`JobRegistry::set_idle()`); the
calls it makes, and what they start, do. A macro waiting for the person in its window would otherwise hold every
agent's idle wait, and a `tools/mcp` driver run from the panel against the same app would wait on its own run.

**Journal and replay.** `script_run` is journaled as one step holding its code, kept when the code failed
partway; its tool calls are not journaled. In a replay a `script_run` step's `code` is never
substituted, and a step whose outcome is not `finished` fails.

**The reference.** `script_search_api` serves the reference `python_api_reference()` builds from the running
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

File › Run Script (the Global shortcut Ctrl+Shift+T, ⌘⇧T on macOS) and the command `script_toggle_panel`, whose
Speed Dial action has the id `toggle_run_script_panel`, show or hide the Run Script panel (`RunScriptPanel`, page
`resources/web/dialog/RunScript/`),
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

**The session job.** `start_python_session()` (`McpToolsPython.cpp`) hosts the panel's session as an unjournaled
person's `script_run` job (`run_python_session_job()`), which the runtime follows (`follow()`) on a thread that
`stop()` joins and `stop_followed()` stops. The job is idle for `app_wait {for: "idle"}` as any `script_run` job is, so agents' and
plugins' runs overlap with it, sharing no state, tracebacks included. It owns what its calls leave behind for the
whole session, as a run does (`begin_run_scope()`): auto-answer rules, recordings, held presses. Between works its
rule layer is paused (`ModalTracker::set_paused()`; the rules listing shows it as paused), so a rule a cell
registered never answers the person's own dialogs between runs and answers again in the next one (a rule a thread of
the session registers between works starts paused, and leaves the dialog open then to the person too), and its held
presses are released as each work ends (`release_run_presses()`). Its `orca.host.tools` calls are child calls of a
`LineageNode`, a running idle job under the session's job that only parents calls: a stop requests the stop for the
node and cancels its operations (`cancel_operations_of()`), so every call in flight, inline ones such as `app_wait`
included, returns at once while the session goes on, and the next work after a stop starts once that cancelling is done,
under a renewed node.

**Stop.** Stop interrupts what runs now: the work, and the session's threads and tasks; while the session is idle a
stop reaches its threads and tasks for five seconds (`kStopWindow`), and the session goes on. A stop moves the run
scope's stop generation on, so a pool task queued before it doesn't run (its future fails with "The task was stopped
before it ran") while one submitted later does, and ends the session's message boxes and modal windows on the UI
thread (`PluginHostUi::end_run_modals()`), so code waiting in them can be interrupted, leaving its modeless windows,
dock panels and callbacks open. A cancel or time-out of the job reaches everything until the end, and ends the
session. A Run all still waiting for the old session to end is dropped by Stop and reported stopped. Restart session, Close script, New, Open, a recent or dropped script, Run all (which runs in a session of
its own, as `python file.py` does), quitting and a main-window rebuild end the session
and stop it, so it ends as a stopped run does: its threads get five seconds, its `atexit` functions run once, and
what the teardown runs can be interrupted, as a stop asked once the session is ending reaches its teardown even when
nothing else was live; then its windows close and its folder's helpers are evicted.

**Lifetime.** `RunScriptPanel` is owned by `GUI_App`, so the script's text, file, unsaved state, session and recent
scripts (AppConfig `script_run` › `recent`, newest first, at most 10) outlive the pane (`RunScriptPane`), which a
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
full-line markers behind the text for the cells on screen; the choice is AppConfig `script_run` › `cell_style` and
`highlight_cell`. Ace has no cells, so the + in the margin a row below the last line, which adds a `# %% ` cell at
the end, is the page's own element, placed after each render. The divider between the editor and the console sets the
editor's share of the width, so a resized pane keeps its proportions; a drag stops at 220 px of editor and 200 px of
console, and its arrow keys, Home and End and double-click (back to 60%) work as a window splitter's. The arrows on its
grip hide a side, as does a drag that ends 48 px past a side's smallest width; a hidden side leaves a strip at its edge
that brings it back at its width: the editor's shows each cell's bar, the console's a dot for a run going, output not
seen, or a failed run. The console's side hides only while it shows the console, never the start page. The share and
the hidden side are AppConfig `script_run` › `split` and `hidden_side`, saved when a drag
ends rather than as it moves.

**Out of the tools' reach.** As Action Replay's page does, the page navigates nowhere but its own loads: a dropped
`.py` opens in the panel, and any other navigation is vetoed. The pane is named `kHiddenFromAutomation`, as are the frame it floats in
(`AuiMgr::CreateFloatingFrame()`) and every prompt and file dialog it opens. `skipped_by_walks()` keeps the walks of
`widget_list`, locators and `webview_run_javascript`'s search for a web view out of it, and a key sent without a target never
goes to the focus inside it, so a script never acts on the panel that runs it. `window_list` lists the floating
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

A plugin's code calls the tools through `orca.host.tools`, with no permission of its own: any loaded plugin
can do what an agent can, work in the GUI, answer dialogs, replace the project, write presets and files. The
tools run OrcaSlicer's own code, which the plugin audit doesn't judge, so a plugin's grants bound its Python
code and not what it asks the tools to do. A plugin's `script_run` is the plugin's own code: its identity and
grants, its windows and storage, though it runs as a main program as any script does.

A plugin's call waits for the UI thread, so it comes only from a thread nothing there waits for: a Script
worker, a Python thread one starts (the thread shim carries the mark) or MCP's own thread running a plugin
operation (`ScopedToolCallerThread`). A call from the UI thread, a slicing hook or another plugin callback
is refused before anything waits. `install_plugin_tool_caller()` routes a plugin's calls. The UI
thread, which owns the runtime and runs the plugin's unload, admits each one: the plugin must still be
loaded and its scope not ended, and the runtime is started without the endpoint when MCP is off; the
call then holds a lease the runtime's drain waits for. A caller that stops waiting because the app closes
marks its admission abandoned, so no lease is taken that nobody releases. The call runs as
`McpCore::call_plugin()`: a top-level job owned by the plugin
(`JobRegistry::owner()`), never journaled, whose children inherit the plugin
(`ToolContext::caller_plugin()`). The session tools are scoped to the plugin: `replay_export` is refused,
and `job_cancel {job}` and `app_wait {for: "job"}` act only on its own jobs (`require_own_job()`). What its calls
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
`orca.register_operation(name, description, schema, handler, changes)` while it loads, without a prompt.
Its operations are staged and offered once the plugin's load has committed
(`PluginOperations::activate()`, in one step under the lock an unload's withdrawal takes); any exit of a
load that did not commit, an exception included, drops them, and they are withdrawn when the plugin
unloads. An operation is reached as
`plugin.<plugin key>.<name>` through `tool_call`, `orca.host.tools.call` and replay steps, and never
appears in `tools/list`: they are a source of the command catalog (`CommandSource`, added by
`register_plugin_operations()`), whose `CommandCatalog::lookup()` resolves those names after the catalog's own
commands, and whose `source_commands()` `tool_search` lists.
It runs as a job, under the mutate guard and journaled when it changes something, under the inspect guard
otherwise; its handler runs under the plugin's identity and grants, and each call is logged with the
plugin and the caller. A call is refused once the plugin has unloaded, by the plugin manager's state, and
by `PluginOperations::invoke()` when the operation found is no longer the one offered (withdrawn, or
offered again by a new load). The call's job is the caller's, so an operation already running when its
plugin unloads runs to its end; its own tool calls are refused from then on.

**`tool_search` and `tool_call`.** `tool_search` reads one inventory (`search_commands()`): every
command of the catalog and the operations its source holds. `{tool}` gives any command's description,
schema, group, `read_only` and `listed`, and its examples when it has some (the Design commands, `undo` and
`redo`, and the commands that close a gap the menu items left, such as `object_fix`, `printer_install` and
`plugin_enable`), and a call with neither `tool` nor `query` lists every name. Its `group` narrows the names and
the command matches to one group (see Tool catalog); the other catalogs' matches beside them are not
narrowed.
Its search by words is the one search across catalogs: the best five commands that match (more with
`limit`), each `{name, group, summary, listed}`, the summary the first sentence of its description. A
listed command's schema is in `tools/list`; one that is not listed, a menu item with no arguments, a Design
command or a
plugin's operation, also carries its `input_schema`, and its description where that says more than the
summary, which nothing else shows (an operation's first
sentence only says a plugin offers it). A command is matched on its key, its title and its description,
and among equal matches a listed one comes first. The reply's `use` says that `tool_call {tool, args}`
runs a command that is not listed. Beside them come the best five of each other catalog with its total
and how to use it: the Speed Dial's actions that are no command, the settings being edited
(`search_settings()`, as `settings_get` searches without a type) and the Python reference. Every catalog is ranked
the same way (`query_words()`, `match_score()`): a query word matches where a word of the name or text
starts with it, CamelCase parts included, and entries matching more words, then more of them in the name,
come first. Scores of different catalogs don't compare, so each keeps its own list. Actions and settings
are read on the UI thread: when it can't run the call (a native dialog holds
it, or it is busy) the search leaves those two out, and before the main window exists (the server starts
ahead of it) it leaves out the settings. The commands, operations and Python reference answer whatever the
UI thread is doing. `tool_call {tool, args}` runs any command of the catalog, listed or not, and is dispatched
before it runs (`resolve_call()`, in the protocol, in child calls and in the plugin caller): the target's arguments
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
by an `McpProtocol` of the bridge's own over the process's command catalog (`NativeCommands::instance()`,
built there on first use, since the bridge never starts the GUI that builds it), given the headers the
server would get: the handshake, `ping`, `tools/list`, `server/discover`, notifications and unknown
methods get exactly the reply this build's server gives. That holds because these methods read
only the catalog and the server's details, and the server keeps no session. The catalog needs no
runtime: registering a command only describes it (a schema built from constants and compile-time
options), and its function runs only when the command is called, reaching the runtime through its
call's context, so building it needs neither wx nor a window. A command's registration must stay that
way, never reading app state; a unit test builds the catalog without the app.

A `tools/call` with nothing answering starts `orca-slicer --mcp` detached, with the bridge's
`--datadir` and `--mcp-port`, and waits up to 60 s for its server, then up to 120 s more for
`app_wait {for: "ready"}` before it forwards the call: the server answers before the main window is
built, and a call that needs the UI thread is refused until then, which on a busy machine outlasts the
call's wait for the UI queue. An app still not ready by then gets the call anyway, which says what
holds it. If that process exits within seconds, a running
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
within its 45 s budget, `app_wait` within 50 s), so a server that never replies can't hang the agent.
A line that is valid JSON but not one message object, such as a batch, gets an error reply.

## Configuration

The CLI options are in `CLIMiscConfigDef`, so the GUI still starts: `--mcp`, `--mcp-port N`, `--mcp-stdio`,
`--run FILE` (with `--outputdir DIR`, see Replays), `--unrestricted-scripts` and `--cad`. `--mcp-port` opens the
endpoint as `--mcp` does, whatever its value, and 0 takes the preference's port: every option has a value after
`CLI::setup()`, so `OrcaSlicer.cpp` reads whether it was given from `m_given_option_keys`. `--mcp`,
`--unrestricted-scripts` and `--cad` each turn their setting on for the run, and the flag never writes the
configuration; without the flag the setting decides. `--mcp` opens the endpoint at startup, and Preferences'
server switch still closes and opens it during the run, saving `mcp_enabled`, as it does without the flag. With
`--unrestricted-scripts` or `--cad` the setting is on until OrcaSlicer closes, whatever Preferences saves
meanwhile. `GUI_InitParams` holds these two flags.
Each of the two settings has two readers: the saved setting (`python_scripts_setting()`,
`GUI_App::cad_feature_setting()`), which Preferences shows with "(on for this run by the command line)" beside
it while the flag is on, and the value in force (`python_scripts_unrestricted()`,
`GUI_App::is_enable_cad_feature()`), which the rest of the app reads, `app_state` included. The AppConfig
`app` section holds `mcp_enabled` (false) and `mcp_port` (13619). Preferences › General › AI features
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

Unit tests are Catch2 cases in two suites. The automation core is the library
`libslic3r_automation`: sixteen units of `src/slic3r/GUI` (the command catalog's type, the dispatch core, the
hand-off to the UI thread, the job registry, the followed threads, the journal, the paths, argument checking,
the protocol and HTTP transport, what the server says of itself, the replay model, Action Replay's summary of a
replay, the setup a client copies, the stdio bridge and the token), which `libslic3r_gui` links.
Its tests are in `tests/automation/`, run with `automation_tests`
(the `test_mcp_*` cases are tagged `[Mcp]`, `test_command_catalog`'s `[CommandCatalog]`); that suite links the
library and libslic3r and nothing of `libslic3r_gui`, so it builds without wx and the embedded Python. What
needs the GUI library (the runtime, the endpoint, the tool families, the process catalog `NativeCommands`
builds) is tested in `tests/slic3rutils/`: the `test_mcp_*.cpp` cases are tagged `[Mcp]`, and the other files
named below carry tags of their own (`test_command_catalog` `[CommandCatalog]`, `test_operations`
`[Operations]`, `test_imgui_wrapper` `[ImGuiWrapper]`), so
`slic3rutils_tests "[Mcp],[CommandCatalog],[Operations],[ImGuiWrapper]"` runs them all. The build keeps wx out
of the core: `src/CMakeLists.txt` adds the library
before it finds wxWidgets. On Linux, wx's include paths and definitions apply at directory scope only from the
point after which the library is added. On Windows and macOS, wx's setup header and definitions come only with
the wx targets, which the library does not link. So a wx include in a core unit fails to compile, and a call
from a core unit into the GUI library fails to link `automation_tests`.

In `tests/automation/`, `test_mcp_protocol` covers both eras, argument validation, the names and values read
in place of a tool's own, and the error for an unknown tool, which names the closest;
`test_mcp_http` the endpoint rules and a live server (deadlines, eviction, stop); `test_mcp_paths` the output
folder, the run's or `<datadir>/automation/out`; `test_mcp_token`
who owns the discovery file and when a port counts as held by another window; `test_mcp_core`
`UiInvoker`, `McpCore` and `JobRegistry` on a hand-stepped dispatcher (guards, abandoned calls,
jobs, blocked_by_dialog, admission, slots, idle, journal and the id arguments it leaves out, a submitted
call, a job's person and discard marks) and the followed threads (`FollowedThreads`: a returned work's thread
joined by the next start, a stop that reaches only the works still running, no start after the join);
`test_command_catalog` (tagged `[CommandCatalog]`) the catalog's own rules (keys, sealing, order,
sources, a Design command's title context, the schema of a two-form command) and a command's reference entry;
`test_mcp_replay` parsing, substitution, matchers and await; `test_mcp_replay_summary` how Action Replay
describes and flags a replay's steps; `test_mcp_setup` the HTTP and stdio setups a client copies.

In `tests/slic3rutils/`, the files of the same name hold the cases that need the GUI library:
`test_mcp_core` a context with no runtime and an action's journal update; `test_mcp_protocol` the order of the
process catalog's `tools/list` and the tool families the stdio bridge lists; `test_command_catalog` the
process catalog itself: unique keys, the commands under their places' headings in the tool list's order,
which are listed, the Speed Dial's 129 actions held as data in `tests/data/speed_dial_actions.json` (tagged
`[SpeedDial]`: each found with its id, title, section, icon and input, every menu item's id an action's key and
no command, every action's arguments fitting its command's schema), the person's forms of the commands with
actions and what a pick of an action runs in place, the names of `retired_names.hpp`, which no command has, the
access labels (`preset_delete` and `plugin_uninstall` alone destructive), a tool list built without a runtime,
each command's `id_args` against its schema, the history `undo` and `redo` take, and the command reference
(tagged `[Reference]`): the checked-in file against the catalog, and that its entries hold what `tools/list`
tells a client and both forms of a two-form command. The others there: `test_operations` (tagged
`[Operations]`) the slice and export outcomes, an operation's `Outcome` and `Refusal`, the checks each
operation makes over a `Model` or a table built in the test (what an operation refuses and works out is a
function over data; what it then does through the Plater is checked on the app), the operation names
commands give and its one exception list; `test_mcp_ui` labels,
snapshot text, ref liveness, synthetic key state, a key's way through the hooks of its target's
parents (with a display), how ImGui items are labelled and named, value
matching and the model-target projection and surface check (`CanvasTarget`); `test_mcp_results`
what covers a canvas click (`imgui_window_at()`), the surface meshes of a modifier target, fresh GL
toolbar state, the auto-answer press and a picker's path answer (with a display), the slice reply's plate summary,
`plate_slice_stats`'s feature, layer and object tables, the legend rows' names, the slider's layer for
a height, menu paths and listings, the UI activity hook, the journal's dropped listing clicks and
calls whose job failed or still runs (with a replay of the export), the
menu capture on real wx menus, row paths and their snapshot text, (with a display) the rows of
real trees, lists, checklists and tables, clicks and checkbox input on them, and popups' owning
windows, plate icon names and bed picking ids, an upload's percentage while its bytes go out, and
`printer_send`'s catalog entry;
`test_imgui_wrapper` the
ImGui recorder (named items, kinds, values, text, clipping) on frames drawn without OpenGL;
`test_mcp_web` a page's lines and their window pixels, web locators, chords as DOM keys per
platform, the page deadline, the request and install scripts, auto-answer matching and standard
button names, the journal's index across pages and frames, and (with a display) the web flag of a
dialog's description, a web dialog rule's wx buttons and web view handles;
`test_mcp_recording` the Matroska file (read back closed and unclosed), the sink's order, memory
budget, non-blocking push, finish, abort and failed frames, the video transform, overlays and frame
repeats, captions and what they hide, the ffmpeg command and listings, the files recordings and
encodes reserve, and the journal's recording paths; `test_mcp_tools` setting serialization, how a query ranks
the Speed Dial's actions, what `action_run` takes and journals for an action, a call with an action's arguments
against one with more, `tool_search`' one inventory and its groups, the tool names of `retired_names.hpp` that are no
command, and which arguments the journal leaves out; `test_mcp_config` the keys and effective values of the
settings commands' targets, the names and places `preset_save` accepts, enum labels and descriptions, the
settings search, per-variant entries and their writes, preset comparison, `changed`, which unsaved changes a
call discarded, which presets `preset_delete` refuses or deletes with a custom printer, the project's slot
colours, flushing multipliers and matrix, a plate's filament orders, layer ranges and filament map and
`plate_list`' layer numbers and layer G-codes, `preferences_get`'s list and `preferences_set`'s checks, the
scripts setting's saved value and value in force, and `calibration_generate_test`'s arguments,
defaults, the dialogs' shared rules, VFA's auto-adjust and the parameters the journal records. End to end, `tools/mcp/orca_mcp.py` launches
the app on Linux on the XWayland of a private headless compositor (KWin, else mutter, which is not
tested yet), which gives it the GPU's OpenGL and a window manager, or, on a machine without a desktop,
on a private Xvfb display (with `-noreset`, and the pointer parked in a corner so no tooltip lands in
a screenshot); on macOS and Windows it opens the app on the desktop. It runs
`tools/mcp/smoke_test.json`, which `--run` also runs headless. `tools/mcp/test_gizmos.py`
checks every gizmo through the GUI tools, with auto-answers that run a button's handler and canvas targets on
negative parts and from below the bed, and the gizmo commands' openings, refusals and acting calls, read back
through `object_list`, the undo log and the saved project, `tools/mcp/test_filaments.py` a filament preset assigned
to a slot by every route, the Auto-flush preference, object and part filaments, a slot merged into another, the
project's settings and mixed filaments,
`tools/mcp/test_presets.py` selecting presets by the names their selectors show, hidden ones included, saving,
reading and comparing presets, explaining settings, per-variant entries, `changed`, reverting keys, answering the
unsaved-changes prompts of a preset switch and a new project, deleting presets, exporting and importing presets
with a path, reading a preference changed in the Preferences dialog, and every calibration test, its preset
answers and its towers in the G-code, `tools/mcp/test_objects.py` plates added, duplicated, named, locked, selected
and deleted, what a lock keeps Arrange and Auto-Orient from doing, a plate's settings, the filament slot commands
and the plate icons and menu, `tools/mcp/test_slice_preview.py` what a slice slices and reports on one and two
colours, `plate_slice_stats`, `app_state.preview` and `view_set`'s preview arguments, the legend combo's semantics
of `view_set {view_type}`, a canvas screenshot straight after a layer or view type change, and the preview refusals
of `view_set`, `tools/mcp/test_gui_tools.py` context and button menus
listed and run without one ever showing, the Edit menu's Undo and Redo picked by their paths, object list rows,
checklist rows, the object settings table, the print button's popup, and the preset bundle dialog, the colour
picker (hex field and spectrum) and the setup wizard's first steps and navigation through the GUI tools alone,
and JavaScript in modal dialogs' web views, the ones `webview_run_javascript` is refused in and a page that closes
its dialog, `tools/mcp/test_recording.py` a recording with clicks, a dialog and a slice, stopped and encoded, and
one a replay ends,
`tools/mcp/test_web_ui.py` every page control kind, action, refusal, locator, timeout and close of
the MCP web fixture plugin's dialog, window, dock panel and configuration UI (side-loaded with
`--install DATADIR`), `tools/mcp/test_set_window.py` `window_list` (the main window first, the Plugins window
and Action Replay by their refs, Preferences as a modal dialog) and `window_set` on the main window (sized twice, moved, placed over its display's work area, maximized and,
on X11, minimized and restored from both, with a window screenshot of the size on Wayland, and the checks
a Wayland session can't run reported SKIP), on the Plugins window by its ref and on Preferences by its
`{label}` locator (on X11 with the main window kept under it in the X server's stacking order),
`tools/mcp/test_speed_dial_open.py` the 3D view's keyboard focus after
a switch to Prepare, Preview or the Design tab, Space opening the Speed Dial there, and the dial without
KWin's title bar, `tools/mcp/test_set_view_camera.py`
`view_set`'s `look_at` and `zoom` after a queued fit (and its note while no 3D view draws), clamping,
an orbit that keeps `look_at`, the camera Preview shares and the Design tab's own, with `camera_state`
and `app_state.camera`, and a fit after the window was sized while no 3D view showed,
`tools/mcp/test_export_running_job.py` an export during and after a job that outlived its reply,
`tools/mcp/test_send_gcode.py` `printer_send`, `print_start`, `printer_test_host` and `app_state.uploads` against
a stub print host that answers 200 or 500, `tools/mcp/test_printers.py` `printer_status` and the job
commands' refusals with no printer connected, and the extruders of a two-extruder printer it installs,
`tools/mcp/test_runtime_lifecycle.py` the app's exit with a job, a recording or a call no stop interrupts still
running and with the endpoint never opened, the server switched off and on during a call, and the GUI
instruments going off with the server,
`tools/mcp/test_stdio_bridge.py` the stdio bridge, and
`tools/mcp/test_runtime_calls.py` answering a dialog while stalled clients hold every
connection, a slice cancelled before it starts, the Run Script panel out of the GUI tools' reach, refs that outlive
their items' labels, and the UI job worker's admission. See `tools/mcp/README.md`.

The commands that do a menu item's job by index, and the gaps they close, have their checks on the app.
`tools/mcp/test_objects.py` calls the commands made from menu items by index and on the selection, the plates'
among them, and reads each back, printable, Fix Model, Auto Drop,
clone, reload, replace and `object_order {order}` among them; `test_files.py` checks the project's files (exports
of some objects, a ZIP's entries), `test_settings.py` `settings_list` and the targets of
the settings commands, `test_presets.py` `preset_list`'s filters, a plate's filament map and a nozzle variant,
`test_slice_preview.py` `view_set`'s switches, the sidebar and the legend, and `test_call_forms.py` a call with an
action's arguments and the forms agents write in place of the schema's. Others check the refusals and what
needs a launch of its own: `tools/mcp/test_menu_items_refusals.py` and `test_additions_refusals.py` each refusal's
reason,
`test_person_forms.py` the person's forms and a pick that runs in place, `test_preferences.py` the
preferences against the dialog, `test_install_printers.py` and `test_install_filaments.py` the installs, also
after a restart, `test_plugin_commands.py` the plugin commands against the Plugins window,
`test_project_open_settings.py` a project opened
with its settings whatever the load-behaviour preference says, and `test_sidebar_floating.py` a floating
sidebar.

What the tests may reach. The sweep runs on scratch datadirs, which are not cut off from the account they may be
signed in to or from the network, so no sweep test opens a dialog that messages a printer: the print calls run on a
printer made a print host, `test_send_gcode.py` stops before any of them where printer agents are on, and
`test_printers.py` calls a job command only after `printer_status` has said no printer is connected. No test
sends a print job or reaches a physical printer: the Print and storage dialogs of a Bambu printer, a real printer's
status and the job commands' refusals on it are left to a person with such a printer. The job commands, and what
they publish, are unit cases against a printer agent double
(`test_device_manager_integration`, `[DeviceManager][Printer]`); the routes, the host test's refusals and the gizmo
commands' rules are `test_operations` (`[Printer]`, `[Gizmos]`) and `test_printhost` cases.

`test_mcp_scene` covers the scene and layer tools without the app: `object_transform`'s `size`, the arguments
of the commands that replace a dialog, notifications updated before the first ImGui frame, `object_arrange`'s option rules,
worker-job admission on a stand-in worker (a job past the reply budget, a slow cancel and a timeout,
three ways to start one, recover input and a person's job), `object_list`' descriptions of a `Model`
built in the test (its rows by plate, the plate filter and paging, placement and boxes on the bed,
mirrors, filament fallback, precise seam subtypes, height ranges, the summary of a variable layer
height profile), `part_add`'s type names and shape
sizes, the object structure commands' arguments, the height ranges `object_set_height_ranges` accepts (order,
overlaps, a range that keeps its settings) and the keys and layer heights a range's settings take, and
`plate_set_custom_gcode`'s edits of a layer slider built in the test (by layer
and height, in-place edits, refusals that leave the slider as it was, the type names). On the app,
`tools/mcp/test_objects.py` checks object placement and structure (transforms on
instances, plates and parts, what `object_list` reports of them, the selection `object_select` and `app_state`
report, shapes at a size, arrange and Fill bed, `part_add`, the object structure commands);
`tools/mcp/test_slice_preview.py` height range modifiers and variable layer height profiles (set, read
back, undone, sliced, refused, kept by a 3MF) and layer G-codes (placed by layer and height, edited in
place, refused, sliced, kept by a 3MF); and `tools/mcp/test_runtime_calls.py` admission while an
arrange started three ways runs.
