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
matches. `validate_arguments()` checks arguments against the tool's schema first. The server
`instructions` (under the 4096 characters clients keep) give the workflow `get_state`, act, `wait`,
`screenshot`, and say to prefer typed tools over clicks and never retry a call that returned a job.

## Security, token and discovery

The bearer token is 32 random bytes as 64 hex characters in `<datadir>/mcp/token`: directory 0700
and file created 0600 with `O_CREAT|O_EXCL` on POSIX, the per-user ACL of `%APPDATA%` on Windows.
It stays out of AppConfig, whose writes and backups use default permissions. `ORCA_MCP_TOKEN`
(preferred, as it stays out of the process list) or `--mcp-token` overrides it for one run;
`get_state.mcp.token_source` says which is in force, and "New token" in Preferences says it takes
effect on the next start while an override holds. While
serving, `<datadir>/mcp/server.json` holds `{url, port, pid, version, token}` with the token in
use, so the stdio bridge and the harness connect without the launch environment. It is written
owner-only via an exclusive temporary file and a rename (`write_private_file()`), and removed on
stop.

## Lifecycle

`GUI_App::on_init_inner()` starts the service once AppConfig is loaded. That is before the first
startup dialog, such as the TLS prompt or the setup wizard, so an agent can answer those dialogs.
`--mcp` or the `mcp_enabled` preference opens the endpoint, while `--mcp-script` alone only builds
the catalog. `get_state.ready` stays false until `post_init()` calls `mark_ready()`, and tools that
need the main window fail with "still starting". `mark_ready()` also starts a command-line script.

The service stops, idempotently, at the start of `MainFrame::shutdown()` when the app closes
(before panels are destroyed) and on `wxEVT_END_SESSION` (after which macOS calls `exit()`).
`McpService::stop()` wakes every waiting call with "shutting down" (`UiInvoker::shutdown()`,
`JobRegistry::wake_all()`), refuses new calls, closes the endpoint and removes the discovery file,
drops every auto-answer rule, clears `SyntheticInput` and turns ImGui recording off. It frees
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

Preferences turns the server on with `GUI_App::start_mcp_service()` and off with
`McpService::stop_serving()`, which closes only the endpoint and removes the discovery file. The
catalog, running jobs and a launch script carry on, because the call that opened Preferences may
itself be an MCP call still on the stack. A new port or token reopens the endpoint through
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
| `Inspect` | get_state, wait, get_log, list tools, get_settings, screenshot, ui_snapshot | yes |
| `Recover` | dialog_respond, dialog_auto_answer, GUI input aimed at the topmost modal | yes |
| `Mutate` | everything else | no |

The closure checks the guards on the UI thread, since the app can change between posting and
running. A closing app or a stopped service fails the call with "shutting down". A frame being
recreated fails it with "retry". A mutate call is refused while a modal dialog is open, and the
refusal lists the dialogs. It is also refused when it would run inside another UI call's nested
event loop: a modal, `ProgressDialog::Update` or `wxYield`. The GUI input tools (`ui_click`,
`ui_input`, `ui_key`) and `dialog_respond` run as recover calls. Once a target resolves outside the
topmost modal, they apply the mutate checks themselves (`UiInvoker::require_mutate()`), along with
operation admission. `cancel` is a recover call too, so neither a dialog the operation opened nor
another call's nested loop keeps it out.

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
request thread posts a probe closure, which runs inside that loop and returns the `ModalTracker`
snapshot as JSON; no `wxDialog*` crosses threads. With a modal open, the reply is
`{status: "blocked_by_dialog", job, dialogs}`. A probe unanswered within 1 s changes nothing.

**Blocking slots.** A request that may block (an inline call, or a job call within its budget)
first takes one of 32 slots. Without one, a job call returns its `running` envelope at once and an
inline call returns `{status: "pending"}`, so at most 32 request threads ever block, well under the
64-connection cap.

### Operations

Slicing and G-code export finish in the background process. Their handlers start the work in one
short UI closure, which first calls `ToolContext::begin_operation()`; that fails if another
operation runs. `poll_ui()` then checks on it through `observe_ui()` every 200 ms, so a dialog that
opens meanwhile doesn't stop the watch, until the work ends or the deadline passes. The deadline is
`slice.timeout_s` (default 30 minutes) for a slice and 10 minutes for an export.

**Admission.** While an operation job runs, every other `Mutate` call is refused ("Busy: job j12 (a
slice or export) is running"), whichever client or script sends it. So an agent can't change the
project under its own operation. The check runs when the call arrives and again in its UI closure,
where operations start, so a call admitted just before an operation began still can't change the
project under it. Inline tools, recover tools and `allowed_during_operation` tools (`cancel`) still
run.

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
config change, but not when a plate is selected (`Plater::take_plate_selection_snapshot()`), since
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
(`plate_result_is_own()`). `cancel` also drops a pending automatic reslice, including one whose
timer event or call is already queued. An export is numbered in `export_seq` once scheduled, so an
unchanged number means it never started; otherwise its `ExportRecord` ends `ok`, `failed`,
`cancelled` or `superseded`. A G-code export with a path is refused while slicing runs, and
`run_action export_gcode` needs a path: without one it would ask in a native file dialog that MCP
can't fill in. The
decision logic is in `McpOperations`, which uses neither wx nor `Plater`.

**Idle.** `wait {for: "idle"}` holds when no modal is open, nothing slices (`Plater::is_slicing_any()`:
the background process, Slice All, a scheduled background update or a pending automatic reslice)
and the UI job worker (arrange, orient) is idle. It also needs no job running other than the caller and its ancestors,
so a script waiting for idle doesn't wait on itself. Only the job registry, the `ModalTracker`
snapshot and the journal are shared between threads, each behind a mutex. All app state is read on
the UI thread.

## Tool catalog

| Group | Tool | Purpose |
|---|---|---|
| App | `get_state` | version, platform and display backend, ready, tab, mode, project, presets, plates, objects, slicing, dialogs, notifications, jobs, window |
| | `wait` | `ready`, `idle`, `dialog`, `no_dialog` or a `job`; `timeout_s` up to 50 |
| | `get_log` | end of the current log file, filtered by minimum level |
| Speed Dial | `list_actions` | search the action catalog, paged |
| | `run_action` | run an action by id, with its optional `param` |
| Project | `open_files` | load models, a project or G-code by absolute path, with no dialog |
| | `new_project` | start an empty project (`discard_changes` if it has unsaved changes) |
| | `save_project` | save the project as `.3mf` to a path |
| | `export` | `gcode`, `sliced_3mf`, `3mf` or `stl` to a path; `gcode` is an export operation |
| Presets | `list_presets` | printer, process or filament presets with flags, paged |
| | `select_preset` | select by name through the selector (filament per slot) |
| | `get_settings` | values and metadata of a preset, or of an object's or part's overrides |
| | `set_settings` | set values the way the settings tab does, all or none; object and part overrides |
| Objects | `list_objects` | objects, instances, parts, bounding boxes, overrides, paged |
| | `select` | select objects, instances or parts, all, or none |
| | `transform` | translate, rotate or scale one instance, relative or absolute |
| Slice | `slice` | current, one or all plates, then the Preview tab as after the Slice button; outcome and stats for each plate |
| | `cancel` | stop the running slice or G-code export, or a pending automatic reslice (optionally only a given job); allowed during an operation |
| View | `set_view` | tab, camera, preview layers and color scheme, window size |
| | `screenshot` | window, 3D canvas, topmost dialog or one widget: a downscaled image and a full-size PNG |
| GUI | `ui_snapshot` | widgets, menus, GL toolbar items, gizmos and ImGui items, with refs |
| | `ui_click` | activate a ref or locator |
| | `ui_input` | set a control's text, value, selection or checked state |
| | `ui_key` | send a key chord to a target or the focused window |
| | `ui_mouse` | canvas gestures: move, down, up, click, drag, wheel |
| | `dialog_respond` | press a button of the topmost wx dialog |
| | `dialog_auto_answer` | answer matching modals before they show, and one already open |
| | `webview_eval` | run JavaScript in a web view and return the result |
| Scripts | `run_script` | run a script file or inline script, with results per step |
| | `export_script` | write the session journal as a script that can be replayed |
| CAD | `cad_call` | forward to the Design tab's control surface (CAD builds, Design tab enabled) |

Each tool family registers itself from its own `McpTools*.cpp` through `register_*_tools()`.
List tools take `limit` and `offset` and report `total` and `next_offset`. `McpPaths.hpp` requires
absolute paths, since a relative one would resolve against `<datadir>/log`, the GUI's working
directory; an output's directory must exist, and an existing file is replaced only with
`overwrite: true`. Paths arrive and leave as UTF-8 text, and so does Orca's `data_dir()`;
`path_from_utf8()` and `path_to_utf8()` convert, through wide strings on Windows, where boost's
narrow conversion would use the ANSI code page.

## Speed Dial catalog

`list_actions` and `run_action` serve the Speed Dial's `ActionRegistry`: built-in commands, plates,
recent projects, plugin scripts and pages, and, on request, settings. Built-in commands are
`NativeCommand`s in `NativeCommands.cpp`, with three fields for automation that the palette never
shows: `description` (English, for agents), `param` (a `CommandParam {type, help}`, typed `None`,
`Percent`, `Tab`, `Path`, `Index` or `Text`) and `completion` (`Export` when the effect finishes in
the background after `run()` returns). `describe_commands()` fills them from one table, with a
key-prefix rule for calibration wizards, camera views and primitives, and `CommandAction` copies
them into the `AppAction`. The palette keeps `input` for its picker and never passes a path.

`ActionRegistry::describe(Query)` searches id, title, group, source and description, including the
English aliases. It can filter by group, lists settings only when asked, sorts by group and title,
and returns one page. Each entry has `id`, the English `title` and `group`, `kind`, `description`,
`param {type, help}`, and `completes: "export"` for exports. Built-in ids look like
`orca_command:<key>:orca`. `run_action` calls `ActionRegistry::run(id, param, record_stats=false)`,
so agent runs never reorder the palette. An unknown id returns up to three close ids. An action
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
| `import_configs` | `.ini` or `.json` | `MainFrame::load_config_file(path)` |
| `save_project_as` | output `.3mf` | `Plater::save_project(true, path)` |
| `export_gcode` | output `.gcode` | `Plater::export_gcode_to(path)`; completion `Export` |
| `export_sliced_file`, `export_all_sliced_file` | output `.gcode.3mf` | `Plater::export_gcode_3mf(all, path)` |
| `export_stl` | output `.stl` | `Plater::export_stl(..., path)` |
| `export_3mf` | output `.3mf` | `Plater::export_core_3mf(path)` |
| `export_stl_multi` | existing directory | `Plater::export_stl(..., multiple, dir)` |

Commands that still open a dialog, such as `export_config` or `plate_rename`, return
`blocked_by_dialog`, and the agent answers the dialog.

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
export do. Describe the tool in English, including what the reply holds and when it is a job.

## GUI automation

`ui_snapshot` (`UiInspector`) returns a compact text tree of the shown top-level windows, the
menus, the GL toolbar items and gizmos, and each canvas's last-frame ImGui items, capped by
`max_nodes` and filtered by `query`. The tree is a second text block of the result. Refs (`w`, `m`,
`t`, `z`, `g` plus a number) are never reused and carry the UI generation and a liveness check, so a
ref to a destroyed or rebuilt widget is reported stale. A field is named by the setting it edits:
its option key is read from the "parameter name" line of its tooltip, and its label comes from the
option's definition. Failing that, the label to its left on the same row names it, then its
tooltip.

Input tools take a `target`: a ref, or a locator, which is stable across runs and is what the
journal stores. A locator is one of:

- `{option: "layer_height"}` for a setting field;
- `{label, class, name, within, index}` for other widgets;
- `{tab: "General", within}` for a notebook page;
- `{menu: "File>Export>Export G-code"}`, `{gltoolbar: "arrange"}` or `{gizmo: "Move"}`;
- `{imgui: "label", window, canvas, index}` for an ImGui item.

The object list's rows are items of a `wxDataViewCtrl`, not windows, so no locator names one.
`select` selects objects, instances and parts, after which `run_action` runs the object operations
the catalog has (delete, split, mirror, center, drop, instances, arrange, orient) and `set_settings`
with the object or volume scope edits their settings. Adding a part or modifier and changing a
part's type have no tool yet.

A label that matches both a row's label and the one field it names resolves to the field.
`within` names a window other than the main one, whose title follows the project name and is left
out of locators. A locator that matches several elements needs `index`; the journal records one
(`UiInspector::locator_for()`) whenever the element it acted on was not the only match. Before
acting, a widget is scrolled into view, as a person would. On GTK this goes through the scrolled
window's adjustment, as the mouse wheel does, because `wxScrollHelper::Scroll()` leaves native
labels unpainted.

Activation prefers a widget's own dispatch: `wxEVT_MENU`, `GLToolbar::force_left_action()` (or
`force_right_action()` for a right click), `GLGizmosManager::open_gizmo()`, a notebook's
`SetSelection()`, a native control's semantic event, or a custom widget's public API. Canvas
gestures and ImGui items get synthesized wx events with a render between steps.
`ImGuiWrapper::set_item_recording()` supplies the ImGui items with their rectangles and checked
state; `ui_input` on an ImGui checkbox clicks it only when its state differs. `SyntheticInput` holds
a gesture's keys, buttons and pointer for code that reads hardware state through `get_key_state()`
and `get_mouse_state()`: the canvas, gizmos, and the object list's Ctrl/Shift multi-select. Held
keys are released however the gesture ends; a `ui_mouse` `down` keeps its button held, and a
`move` drags with it, until an `up`, another gesture or the service's stop. Keys without a target go to the
focused window inside the topmost modal, else that modal, else the focused window, else the 3D
canvas.

`ModalTracker`, a `wxModalDialogHook`, sees every `ShowModal()`, native dialogs included, and
publishes the snapshot `[{title, message, class, native, buttons}]`. `dialog_respond` presses the
topmost wx dialog's button, so the dialog's own handler runs, or ends the dialog with that id when
no such button exists. A native dialog can only be answered ahead with a `dialog_auto_answer` rule:
a regex over title and message, and a button. `Enter()` then returns that button's id without
showing the dialog, and logs it; a file or folder picker can only be cancelled, since any other
answer would return without a path. A new rule also answers a
matching wx dialog that is already open. A prompt posted around the same time, such as the restore
prompt after startup, is handled whether it opens before or after the rule. A script's rules end
with the script, a client's call replaces its earlier rules, and `get_state` lists the rules in
force. `webview_eval` runs JavaScript with `RunScriptAsync`. The result arrives as an event on the
UI thread, in a `ScriptSink` that only the UI thread creates and deletes, along with its weak
reference to the web view. The job thread waits for the result, up to `timeout_s` (default 10),
without the UI; a JSON result comes back parsed.

## Screenshots

`screenshot` captures on the UI thread and encodes off it. It has three targets.

- `canvas`: the visible 3D view, exactly as drawn, toolbars and ImGui panels included, on every
  platform. `GLCanvas3D::render_and_capture()` forces a frame and reads the back buffer just before
  the swap; it fails, saying why, when the canvas is hidden or mid-frame.
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
| Windows | `PrintWindow(PW_CLIENTONLY \| PW_RENDERFULLCONTENT)` | `PrintWindow` fails and the screen is copied instead |
| macOS | `cacheDisplayInRect:toBitmapImageRep:` on the content view | always: web views may be blank |
| Linux X11, Xvfb, XWayland | `XGetImage` of the top-level X window | part of it is off-screen or under another application's window |
| Linux Wayland | `gtk_widget_draw()` of the top-level window's child, which leaves out the client-side decoration's shadow | always: web views may be blank |

On every platform, a canvas that can't be captured keeps whatever the window capture drew, and the
result is approximate. A canvas frame that reads back black (`is_blank()`, a stray pixel aside) has
not been drawn yet. This happens on Wayland for a moment after the canvas is first shown, and while
GNOME's overview hides an XWayland window. Such a capture is taken again every 300 ms for up to 3 s,
then returned as approximate, saying so. A 3D view other than Prepare's and Preview's (the Design
tab's) has no frame capture: a window capture says it is left out, and `target: "canvas"` refuses.
Minimized and hidden windows fail. X11 reads only the on-screen part of a window (`XGetImage` fails
for the rest), which leaves the remainder black. X11 keeps no backing store, so a part
covered by another window reads back black. OrcaSlicer's own popups and tooltips stacked above the
window are read and drawn in, as they appear on screen; another application's window leaves the
hole, and the result is approximate. The full-size PNG goes to `save_path` or `<datadir>/mcp/out/screenshot-<timestamp>.png`. The image
block is scaled to `max_dim` on the long edge (default 1280, clamped to 256–2000).
`include_image: false` returns only the path.

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
script's. A step naming an unknown tool makes the script invalid before anything runs. The result,
also written to `<out_dir>/result.json`, is
`{passed, name, failed_step, steps: [{index, tool, args, ok, result | error, expect_failure}],
out_dir, result_file}`. A script file's `out_dir` defaults to `<script name>.out` beside it. An
inline script's defaults to `<datadir>/mcp/out/script-<timestamp>`, which is also its
`${script_dir}`.

`orca-slicer --mcp-script FILE [--mcp-script-out DIR]` runs a script once the GUI is ready. It
doesn't open the endpoint unless `--mcp` is also given. It writes `result.json` and screenshots to
the output directory, which defaults to `<script name>.out` next to the script. Then it closes the
app without prompting and exits 0 if the script passed, 1 if a step failed, and 2 if the script is
invalid. `OrcaSlicer.cpp` resolves both paths against the original working directory, since the
GUI changes it. wx ignores `OnExit()`'s return value, so `GUI_Run()` returns
`McpService::script_exit_code()` after `wxEntry()`.

The session journal records client calls to journaled tools in order: every mutate tool, plus
`wait` and `screenshot`, which a replay needs. It keeps the last 5000 calls, dropping the oldest
call that has replied; a call still waiting for its reply keeps its place. GUI tools record locators instead of refs through
`ToolContext::set_journal_args()`. `export_script` returns it, and with `path` writes it, through `Journal::to_script()`; `clear`
starts a new recording. `Journal::to_script()`
drops failed calls, turns a call that returned a job into a step with `"await": false` and
`"save_as": "jobN"`, points the `wait {for: "job"}` that followed at `${jobs.jobN}` (dropping waits
on jobs it never saw start) and rewrites screenshot paths to `${out_dir}/<file name>`.

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
section holds `mcp_enabled` (false) and `mcp_port` (13619). Preferences › General › Features has
three rows:

- **"MCP server for AI agents"**: checked while the server runs, with its URL or the reason it
  could not start.
- **"MCP server port"**.
- **"Connect an AI agent"**, with two buttons. "Copy setup command" copies
  `claude mcp add --scope user orcaslicer -- "<executable>" --mcp-stdio`, which uses the AppImage's
  own path inside an AppImage. It holds no token, so it keeps working after port and token
  changes. "New token" renews the token and reopens the endpoint.

## Testing

Unit tests are Catch2 cases tagged `[Mcp]` in `tests/slic3rutils/test_mcp_*.cpp`, run with
`slic3rutils_tests "[Mcp]"`. `test_mcp_protocol` covers both eras and argument validation;
`test_mcp_http` the endpoint rules and a live server (deadlines, eviction, stop); `test_mcp_core`
`UiInvoker`, `McpCore` and `JobRegistry` on a hand-stepped dispatcher (guards, abandoned calls,
jobs, blocked_by_dialog, admission, slots, idle, journal); `test_mcp_operations` the slice and
export outcomes; `test_mcp_script` parsing, substitution, matchers and await; `test_mcp_ui` labels,
snapshot text, ref liveness and synthetic key state; `test_mcp_tools` setting serialization. End to end, `tools/mcp/orca_mcp.py` launches
the app on a private Xvfb display (with `-noreset`, and the pointer parked in a corner so no tooltip
lands in a screenshot) and runs `tools/mcp/smoke_test.json`. `tools/mcp/tests/script_basic.json`
checks `--mcp-script` headless, `tools/mcp/test_stdio_bridge.py` the stdio bridge, and
`tools/mcp/test_recovery_under_load.py` answering a dialog while stalled clients hold every
connection. See `tools/mcp/README.md` and the `orca-mcp` skill.
