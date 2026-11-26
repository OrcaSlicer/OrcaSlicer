# MCP Server

The MCP server lets an AI agent drive a running OrcaSlicer through the Model Context Protocol. A developer's agent
reproduces a GitHub issue: it loads the files, picks presets, changes settings, clicks through the GUI, slices, takes
screenshots before and after a fix and saves the steps as a replay. A user drives OrcaSlicer from Claude Code, Codex,
Cursor or Claude Desktop. The server is off by default.

This document is the server's architecture and the principles its commands are built on. The commands themselves,
how they are named and grouped, what each does and takes, which calls open a dialog and the behaviour a caller relies
on, are in [mcp-tools.md](mcp-tools.md), and each command's description and schema, which `tool_describe` gives, is
its manual. The Design tab, whose commands run on the same server, is in [design-tab.md](design-tab.md).

## Terms

- **Command**: one function the app can be asked to run, with a key (`object_mirror`), a JSON schema and a
  description. MCP lists it as a tool.
- **Action**: an entry of the Speed Dial, the app's command palette. A command's actions run it with fixed
  arguments, and most of them are its menu items.
- **Person's form**: a call whose arguments are exactly one of the command's actions'. It does what a person's pick
  of that action does, which may ask in a dialog; any other call runs from its arguments and opens nothing.
- **Front end**: anything that runs commands: an MCP client over HTTP or the stdio bridge, Action Replay, the Run
  Script panel, a `--run` file, a plugin, and a person's pick in the Speed Dial or a menu.
- **Job**: a call that runs on a thread of its own and can outlive its reply; `app_wait {for: "job"}` returns its
  result.
- **Guard**: a command's class, `Inspect`, `Recover` or `Mutate`, which decides when it may run.
- **Operation**: a function of the operations layer (`GUI/Operations/`), which holds what a command, a dialog and
  the plugin API share. A call's *background operation* is the slice, export or worker job it started and holds.
- **Journal**: the session's record of the calls that changed something, which `replay_export` turns into a
  **replay**, a JSON file of calls that `replay_run`, `--run` and Action Replay run again.
- **GUI instruments**: what driving the GUI needs beyond running commands: the UI inspector, the menu capture, ImGui
  item recording and, on Wayland, window-state hooks.

## Architecture

```
 MCP client ──HTTP──▶ McpEndpoint ◀──HTTP── orca-slicer --mcp-stdio ◀── stdio client
                      (HTTP server, protocol, token, discovery file)
                           │
                           ▼
 Action Replay ──▶  AutomationRuntime ── McpCore: arguments, guards, admission,
 Run Script    ──▶         │                      jobs (JobRegistry), Journal
 --run, plugins ─▶         │
                           ▼
 a person's pick ──▶ CommandCatalog (NativeCommands): Mcp/Tools/<Place>Tools.cpp
                           │
                 UiInvoker: every wx call on the UI thread
                           │
        ┌──────────────────┼───────────────────────┬────────────────────────┐
        ▼                  ▼                       ▼                        ▼
 Operations layer    GUI automation:           Capture:               Replay engine,
 (GUI/Operations/)   UiInspector, ModalTracker, ScreenCapture,         Python runner
        │            SyntheticInput, page agent McpRecorder
        ▼
 Plater, Tab, Sidebar, GCodeViewer and the other owners
```

| Component | Its job | Code |
|---|---|---|
| `McpEndpoint` | The MCP server and nothing else: HTTP, the protocol, the request gate, the token, the discovery file, taking over a port another window gives up | `Mcp/McpEndpoint.cpp`, `McpHttpServer`, `McpProtocol` |
| `AutomationRuntime` | Runs commands for every front end; owns the dispatch core, the jobs, the journal, the UI invoker, the `ModalTracker`, the recorder and the scopes a run, a plugin or a client leaves state in | `Mcp/AutomationRuntime.cpp` |
| `McpCore` | Dispatches a call: checks its arguments, applies its guard and operation admission, runs it inline or as a job, journals it | `Mcp/McpCore.cpp` |
| `CommandCatalog` | Every command, built once at startup and sealed, so any thread reads it | `CommandCatalog.hpp`, `NativeCommands.cpp`, `Mcp/Tools/` |
| `ActionRegistry` | The Speed Dial's actions, a command's among them | `ActionRegistry.cpp` |
| Operations layer | What more than one interface does, in one place: checks, refusals, undo, the owner calls | `GUI/Operations/` |
| GUI automation | Sees the GUI as a person does and acts on it: the inspector and its refs, input, dialogs, web pages | `Mcp/UiInspector.cpp`, `Mcp/ModalTracker.cpp`, `Mcp/WebPages.cpp` |
| Capture | Screenshots and recordings | `Mcp/ScreenCapture.cpp`, `Mcp/McpRecorder.cpp` |
| Replay engine | Runs replays, and describes one for Action Replay | `Mcp/McpReplay.cpp`, `Mcp/McpReplaySummary.cpp` |
| Python runner | Runs a script as the main program of a run, with the plugin API | `plugin/PythonScriptRunner.cpp`, `plugin/PythonSession.hpp` |
| stdio bridge | Serves stdio clients and starts the app on demand | `Mcp/McpStdioBridge.cpp` |

The server runs inside the GUI process, because most of what an agent reproduces is GUI behaviour. `GUI_App` owns
the runtime and the endpoint, and neither exists until something needs it: before then there is no thread, socket,
modal-dialog hook or synthetic input state. The endpoint holds the runtime by reference, and the runtime does not
know the endpoint, so every front end but MCP runs with the server off. The core that uses no wx (the command
catalog's type, the dispatch core, the hand-off to the UI thread, the jobs, the journal, argument checking, the
protocol and HTTP transport, the replay model, the stdio bridge and the token) is the library
`libslic3r_automation`, unit-tested without the GUI.

## Design principles

1. **One command, every route.** A command is one function, `run(args, context)`, with JSON arguments that match its
   schema. `tools/call`, `tool_call`, a replay step, a script's or plugin's call, a Speed Dial pick and a menu item
   all reach it by its key and run it alike, so a feature added once reaches agents, replays, scripts and people.
2. **A command does what the GUI does, through the GUI's code.** What a command shares with a dialog or another
   interface is an operation, which the dialog calls too, so the two cannot drift apart. A command writes a value
   through the control that sets it, and acts on the selection by selecting as a person would, so the app answers
   with its own undo steps, notifications and dialogs.
3. **A call either asks or runs from its arguments.** The person's form asks as the menu item does; any other call
   opens nothing and takes the dialog's answers as arguments, or refuses with the question as its reason. So an agent
   can work without a person, and a person's pick behaves as it always has.
4. **The UI thread is never blocked by a request, nor re-entered.** Handlers run off the UI thread and post every wx
   call to it; a guard keeps a change out of a modal dialog and out of another call's nested event loop. A call that
   outlives its reply budget becomes a job, and the server never retries a change.
5. **A call owns its result.** A slice or export the call started is its background operation, numbered so it is
   never taken for someone else's, and one runs at a time, so no call changes the project under another's.
6. **A replay does the same thing again.** The journal records each call in its schema's form, with indices rather
   than per-process ids, locators rather than refs, and the values a call resolved from saved state; defaults are
   fixed values, never what a person last set.
7. **Agents find by listing, and replies say what they hold.** An agent lists a catalog and reads the manual of what
   it picked, since a word search does not match a person's words to a command. Descriptions state what a reply
   holds, what every tool shares is said once, and a reply's default size fits a client's reply cap.
8. **Nothing leaves the machine, and secrets stay hidden.** The server binds to loopback and wants a token, scripts
   run under the plugin audit, and captions, the activity log, dialog text and URLs mask secrets.
9. **It costs nothing until used.** The runtime and the endpoint start on first use, and the GUI instruments, which
   cost every frame, are on only while something drives the GUI.
10. **Input stays inside the app.** Input goes in as wx events, never through the OS or the window manager, so no
    command needs OrcaSlicer in front and the app never takes the keyboard from the person's other app.

## Transport

The endpoint is `http://127.0.0.1:<port>/mcp`, bound to loopback; the default port is 13619, since 13618 is the
OAuth callback server. It is stateless Streamable HTTP: each POST carries one JSON-RPC message, with no SSE stream,
no `Mcp-Session-Id` and no batching. `McpHttpServer` serves every socket on one I/O thread and runs each request on a
thread of its own, because a handler can block for a whole reply budget. Connections are capped, and at the cap the
one stalled longest on its client is evicted, never one being handled, so a recovery call gets through. Sockets are
not inherited by child processes, which would otherwise keep the port bound after the server closes it.

Every request is checked before any JSON is parsed: path, method, a loopback `Host` against DNS rebinding, a loopback
`Origin` when sent, the bearer token in constant time, the content type and a size cap. No CORS headers are sent.
`McpProtocol` serves the handshake era (2025-11-25, 2025-06-18) and the 2026-07-28 era, whose requests carry their
version in `_meta` and need no handshake.

A tool result is one compact JSON text block plus any images. No tool declares an `outputSchema`: Claude Code shows
its model a tool's description and input schema but never its output schema, and fails a call whose structured
content does not match it, so a schema would teach nothing and could turn a call that ran into an error. Failures
are `isError` results the model can act on (mcp-tools.md, How a failure shows). Before validation the server reads
what agents commonly send in place of the schema's form (`normalize_arguments()`): an argument under a name the tool
does not take as one it does, a value in another JSON form, an enum value under the other argument whose enum takes
it. Each came from the bench, where it was an agent's first guess many times over, and the journal records the
schema's form.

The server `instructions` fit the 4096 characters clients keep. Clients that defer tools load only names and the
instructions at first, so the instructions say first how to find what to call, then what many tools share: never
retry a call that returned a job, a call with an action's arguments may ask, the paging shape, 0-based indices, ids.

## Security, token and discovery

The bearer token is 32 random bytes in `<datadir>/mcp/token`, owner-only (created 0600 on POSIX, the per-user ACL on
Windows) and kept out of AppConfig, whose writes use default permissions. `ORCA_MCP_TOKEN` overrides it for one run.
Deleting the file and turning the server off and on issues a new token. While serving, `<datadir>/mcp/server.json`
holds the URL, port, pid and token, so the stdio bridge and the test harness connect without the launch environment;
it is written owner-only and removed on stop only by the process it names, since windows sharing a configuration
folder share the file.

## Lifecycle

The endpoint opens at startup, before the first startup dialog, when `--mcp`, `--mcp-port` or the `mcp_enabled`
preference asks, so an agent can answer the TLS prompt or the setup wizard. The runtime starts on first use by any
front end. `app_state.ready` stays false until the main window, the Prepare canvas's OpenGL and the tab pages are
up, since a 3MF save needs GL for its thumbnail and a GTK web view made too early loses its message handler; after
10 s it stops waiting, since a hidden canvas or a busy person can hold either back for ever.

When the app closes, the endpoint closes and the runtime stops: waiting calls wake with "shutting down", new calls
are refused, and auto-answer rules end. Nothing is freed then, since a call may still be on the UI thread's stack in
a nested loop; both are freed outside any event loop once their threads have left, or left in place after 10 s,
since freeing under a call that never returns would crash. A language change rebuilds the GUI and only suspends the
runtime: refs go stale and UI calls ask for a retry.

Windows that share a configuration folder share the port, and the bind is exclusive: the first window serves, and
the others retry and take over when it closes. Windows meant to serve side by side need their own `--datadir` or
`--mcp-port`. Turning the server off in Preferences closes only the endpoint, since the call that opened Preferences
may itself be an MCP call; closing it ends the client's auto-answer rules, recording and held presses, so a person
using the app afterwards never has a prompt answered for them.

## Threading

Tool handlers never run on the UI thread. Anything that touches wx runs in a closure that `ToolContext::run_ui()`
posts through `UiInvoker` and waits for; a closure not started within 10 s is abandoned, and the call says it is safe
to retry. After a change a tool makes one more round trip (`settle()`), so the events the change queued are handled
before the reply. A person's pick is the one call that runs on the UI thread itself, in place.

**Guards.** The guard is checked in the closure, on the UI thread, since the app can change between posting and
running. mcp-tools.md (When a command may run) lists the commands of each.

| Class | What it holds | Behind a modal or inside another call |
|---|---|---|
| `Inspect` | reads, lists, screenshots, `widget_list`, the recording commands | runs |
| `Recover` | answering dialogs, `job_cancel`, `window_set`, GUI input aimed at the topmost modal | runs |
| `Mutate` | everything else | refused, naming the dialogs |

A change inside another UI call's nested loop (a modal, a progress dialog, `wxYield`) would re-enter code that is in
the middle of a call, which is why a mutate call is refused there. The input tools are recover calls so that they can
answer the dialog that holds the app, and apply the mutate checks themselves once their target is outside it.

**Jobs.** Observation runs on the request thread; every tool that can activate UI runs as a job, since a click can
open another modal. The request waits up to the reply budget, 45 s, under Codex's 60 s tool timeout, then replies
`{status: "running", job}`. A handler that opens a modal blocks in its loop, so a probe posted into that loop reads
the open dialogs, and the reply becomes `blocked_by_dialog` with a note on how to answer the dialog and how the next
such call runs without asking, so an agent learns it from the call that asked.

**Platforms.** On macOS a native picker or message box pauses wx's pending events, and every posted closure with
them, so the server answers from the snapshot the tracker publishes for any thread, and native dialogs are answered
ahead by a `dialog_auto_answer` rule. Running closures some other way inside the Cocoa modal session would re-enter
the app while the dialog is up. On Windows and Linux a native dialog's loop runs the closures, and on Windows the
dispatcher also wakes the UI thread with a message, which menus' and system dialogs' loops need. On GTK a window the
compositor isn't drawing gets no frames, so a call that times out says so instead of giving a misleading reason.

**Followed calls and the person mark.** A run a person starts (Action Replay, the Run Script panel, `--run`) outlives
any reply, so the runtime follows it on a thread of its own and hands its result to the window that started it. Its
jobs carry the person mark, so a script under a person's run runs as the person's (Python scripts, Identity).

### Operations

Slicing and G-code export finish in the background process, and arrange, orient and the gizmos' jobs on the Plater's
job worker. A call that starts such work holds it as its background operation until it ends, so its reply is the
work's outcome and `job_cancel` stops it. The worker does not record who queued a job, so a call that finds it idle
at the start of one of its UI closures and busy at the end has started one and claims it; a job a person starts in
that window is claimed too, a known cost of the rule.

**Admission.** While a background operation runs, every other `Mutate` call is refused, whichever client, replay or
script sends it, and so are changes while the worker runs a job no call holds; reads are not. The check runs as the
call arrives and again in each UI closure, so a call admitted just before an operation began still can't change the
project under it.

**Identity.** An operation never takes another's completion for its own. `Plater` numbers slice requests and runs and
keeps a project revision, so a slice knows its own runs, and is `superseded` when someone else slices or edits the
project during them. Each plate's outcome comes from its last own run, and statistics come only while the project is
as the slice left it. The rules are in `Operations/RunStatus`, free of wx and `Plater`, so they are unit-tested.

**Idle.** `app_wait {for: "idle"}` holds when no modal is open, nothing slices, the job worker and every gizmo's
background work are idle, and no job runs but the caller and its ancestors, so a replay waiting for idle doesn't wait
on itself.

## The operations layer

The Speed Dial, the MCP commands and the plugin API (`orca.host`) ask the app to do things by name, and what one does
another often does too. The checks, the refusal and its wording, the undo snapshot and the order of the owner calls of
such a function are in one place, `src/slic3r/GUI/Operations/` (namespace `Slic3r::GUI::Ops`), and each interface
converts its arguments and results around it. Where a dialog does the same work (Clone, Preferences, the Plugins
window, the Printer Selection and System Filaments dialogs, AMS sync, Fix Model), the dialog calls the operation too.

**What is an operation.** An effect, or a rule about one owner's state that its own widgets also apply, is a public
method of the owner (`Plater`, `Sidebar`, `Tab`, `GCodeViewer`). What an automation caller invokes by name is an
operation when more than one interface reaches it, and always for a new command. A caller calls an operation to
change something and an owner to read. A function with one entry point is left as it is until a second appears.

**Shape.** An operation is a free function, `Outcome<Result> op(const Args&)`, over plain structs (no JSON, no
`wxString`, no GUI pointer). An `Outcome` holds the result or a `Refusal`: a `kind` (`Precondition`, `Busy` or
`Failed`), a stable `code`, one English message marked for translation, and `names` that help the caller. The Speed
Dial shows the message translated; a tool and a Python exception carry it in English with its code, so a refusal
reads the same through every route.

**Rules.** An operation runs on the UI thread and returns at once: background work returns a ticket and a status
function, and the caller owns the wait, the timeout and the cancel. It takes its function's undo snapshot once, after
its checks. It adds no guard and asks nothing: an answer a caller may give is a field of `Args`. An owner method whose
menu item asks in a dialog takes the answer where a caller can give one (a path, a size, `keep_z`), which skips that
dialog. For its other questions and messages it takes `Prompts`: `Ask` does as the menu item does, and `Skip` runs
without them, each method saying what it answers in their place. An `Args` that passes the choice on holds a `Prompts`
too. An operation includes owner headers, libslic3r and wx, and nothing of `GUI/Mcp`, `plugin/`, JSON or pybind11; the
dispatch core includes nothing of it. A command does what a person's action does by running the code that action runs,
not by writing the model or the config beside it: a dialog's OK is split into a function both call, and a command that
acts on the object list's selection selects the row first.

**Keeping it so.** A command names the operations it reaches (`reaching()`), and the command reference lists them.
`test_operations` requires every command outside the Design tab that is not read-only to name one, or to be on its
exception list with a reason (it runs what its arguments name, drives the GUI, changes the session, has one entry
point, runs its menu item's function, or steps a history).

## Tool catalog

The commands are the process's `CommandCatalog` (`NativeCommands::instance()`), built once at startup, before the
runtime starts, and sealed, so any thread reads it. `register_all_commands()` gathers each place's commands from its
file, `Mcp/Tools/<Place>Tools.cpp`, and sorts them by place, the first word of their names, in the order mcp-tools.md
gives. A command is made with a maker that sets its guard and where it runs (`inspect_tool()`, `mutate_tool()`,
`recover_input_tool()`, or `menu_command()` for one made from menu items). `tools/list` shows the commands that run
from arguments; `tool_list` lists the rest (the menu items that take no arguments, the Design tab's commands, the
plugins' operations), and `tool_call` runs any of them.

**Access labels.** Each tool's MCP annotations mark what a call does by its nature, not everything a call could
reach. Codex asks before every call of a destructive tool and Claude Code runs read-only calls in parallel, so
labelling by reach would put a confirmation on calls an agent makes all the time. `destructive` is a command whose
every call deletes what a person made, with no undo; a command that loses work only when an argument says so, or that
runs what its arguments name, is not. mcp-tools.md lists the commands of each label.

**Indices and ids.** Every argument that names an object, part or instance by index has a twin that names it by the id
`object_list` reports. The index is what the app works by and moves when things before it go; the id stays with its
thing, so a stale id is refused where a stale index would quietly name another. A call's ids are read as indices on
the UI thread just before the command runs, so commands read indices only, and the journal records the indices,
since ids belong to one process.

**Descriptions and replies.** A description gives what the tool does, the arguments that change it, `Reply:` with its
fields, then edge cases, and what several tools share is written once (the locator grammar in `widget_list`, the
paging shape in the server instructions), since every session loads the whole list. An argument name means one thing
across the tools (mcp-tools.md, Naming). A name that misses is answered with the closest names, or with why the
catalog left it out (the CAD feature off at startup). Lists page as `{items, offset, total}`, and defaults are sized
so the bare call fits a client's reply cap. Numbers reach a client rounded, since a double's last digits are noise
that costs tokens; a call asks for them whole with `_meta` `"orcaslicer/exact_numbers": true`. Paths are absolute,
since a relative one would resolve against the GUI's working directory, and travel as UTF-8.

### Design tab: the Design commands

The Design tab's commands are made from one table, `kDesignTools` in `CAD/DesignOffer.hpp`, generated from
`scripts/CAD/tool_atlas.json`, which is also the tab's right-click menu and its Speed Dial actions (design-tab.md, The
offer is generated, not hand-written). An entry's description, closed schema, access and the operations it names all
come from the table, and its silent form, in `Mcp/Tools/Design<Heading>Tools.cpp`, is bound to it by id. They join
the catalog only when it is built with the CAD feature on, since the catalog is sealed at startup; otherwise a call by
one of their names is told why (`design_unavailable`). They are not listed, so a session that never opens the tab
pays only for their names, at the end of `tool_list`'s description.

A call that chooses a menu item opens that tool, as the menu does; any other call runs the silent form on the UI
thread under the mutate guard. `design_gate()` refuses by cause first, and the entry's `access` decides what the tab
lets through while a card, a Move or a sketch session is open. A failed rebuild is rolled back, so the design is as it
was and the call is left out of a replay. mcp-tools.md (The Design tab's commands) has how they are named, called,
refused and journaled.

## Speed Dial catalog

`action_list` and `action_run` serve the Speed Dial's `ActionRegistry`: the commands' actions, plates, recent
projects, plugin scripts and pages, and settings on request. A menu item and the command that does its job are one
command, so the items of one job are actions of one command, each with the arguments that pick its variant (Mirror >
Along X Axis is `object_mirror {axis: "x"}`). An action keeps its menu item's id, title, section and icon, because a
person's pins and run counts are stored under the id. The Design tab's actions are live: there while the tab is shown,
in its mode, with the CAD feature on.

**One function, two contexts.** A call's context is the core's; a person's pick has a `PersonContext`, which runs in
place on the UI thread with no job, no journal entry and no endpoint, so every action works with the server off. A
long command a person picks runs as a job on the runtime's core instead, with the same guards and admission as a
call, and shows its result as a notification. The makers place a command's function on the thread it needs:
`ui_command()` on the UI thread, `long_command()` where its call runs, and `with_person_form()` and
`with_menu_item_forms()` for a command whose person's form asks; `silent: true` runs such a command from its
arguments, and the journal keeps it.

**action_run** runs an action as a call with its arguments would, after re-making the live families (plates, recent
projects, the Design tab's actions), so it finds the app as it is. It is journaled as that `action_run`, so a replay
never passes the action arguments it does not take, and an agent's runs never reorder the Speed Dial.

## GUI automation

The GUI commands (mcp-tools.md, Driving the GUI) let an agent see the app as a person does and act where the typed
commands don't reach: a dialog, a gizmo's panel, a web page, a menu item with no command.

- **Seeing.** `UiInspector` turns the shown windows into a text tree: widgets, list rows, menus, the 3D toolbar and
  gizmos, the canvases' ImGui items (from ImGui's test-engine hooks, on only with the instruments) and web pages'
  elements. It reads an element as a person sees it, a field by the setting it edits or its label, so the tree is
  enough to act on without a screenshot. A ref is never reused and goes stale with its element; a locator names an
  element by what a person sees, so it holds across runs, and the journal records locators.
- **Acting.** An element is activated through its owner's own dispatch (`wxEVT_MENU`, a toolbar's click action, a
  control's semantic event), and only custom-drawn widgets and the 3D views get synthesized mouse events, since a
  native control acts on what the system tells it. An element gets the state a person would see before it is listed
  or clicked. `SyntheticInput` holds the keys and buttons of a gesture for the code that reads hardware state, and a
  press is released by whoever made it or when its owner ends. The camera is turned with `view_set`, never a drag,
  since what a drag does is the user's mouse setting.
- **Menus.** A native menu loop holds the UI thread until a person closes the menu, so a menu a call opens is
  captured instead of shown: listed in the reply, or its item dispatched as a pick from a shown menu is. Other menus
  opened while a call runs are closed unseen and noted.
- **Dialogs.** `ModalTracker`, a `wxModalDialogHook`, sees every `ShowModal()`, native dialogs included, keeps the
  modal stack the guards read and publishes a snapshot for other threads. A dialog's message is read as a person
  reads it, with secrets masked. `dialog_answer` presses a wx dialog's own button, so its handler runs, and presses a
  native message box through the OS on Windows and Linux. An auto-answer rule answers a dialog as it opens: a picker
  takes its path as a person's choice would, and a wx dialog's button is pressed once its loop runs, since its
  handler does the dialog's work.
- **Web pages.** Every web view, plugins' included, is read and driven by one page agent
  (`resources/web/mcp/page_agent.js`) that the server sends in with the request when a page lacks it, so no host
  needs a change. It lists a page after Playwright's accessibility snapshot, and sends what a person's input sends,
  since synthesized DOM events get no default action. A page's messages to its host are held while an action runs,
  so a page that closes its dialog can't destroy the web view under the call.
- **Windows.** `window_set` changes only geometry and checks the whole request before anything changes. A window
  manager may apply a request later or change it, so the call waits until the window shows what was asked. Wayland
  has no request to place a window, so positions are refused there.

The windows a person keeps for themself (the Run Script panel, the Agent Activity pane, the Python scripts dialog,
Action Replay's prompts) are named `kHiddenFromAutomation`, so no tool or auto-answer rule acts in them.

## Screenshots and recording

`window_screenshot` captures on the UI thread and encodes off it. The canvas capture forces a frame of a 3D view and
reads it back before the swap, so it is exactly what is drawn on every platform. The window capture takes a window
and composites the 3D views into it, since window capture loses OpenGL content on several platforms, and says
`approximate` where it may differ from the screen.

| Platform | Window capture |
|---|---|
| Windows | Windows Graphics Capture, which reads what DWM composed, covered or not; `PrintWindow` would repaint the window and flash it |
| macOS 14.4 and later | ScreenCaptureKit, the app's own windows only, which needs no Screen Recording permission |
| macOS before 14.4 | the content view drawn into a bitmap, approximate |
| Linux X11 | `XGetImage` of the window |
| Linux Wayland | the window's widgets drawn into a bitmap, approximate |

A recording has to stay light, since it runs while the app works. The UI thread only captures, on a timer that fires
inside modal loops too and backs off when capturing is slow; `RecordingSink`, which uses no wx, writes the frames as
JPEG into a Matroska file, which keeps their real times and plays as it is, and drops a frame rather than wait for
the disk. `recording_encode` runs the system `ffmpeg`, never a program a call names, at constant quality, which suits
a mostly still screen. Captions name the calls that act, with secrets masked. While any recording runs the tab bar
shows a REC pill, so a person always knows the app is being recorded.

## Replays and the journal

A replay is a JSON file of calls, which one engine (`McpReplay.cpp`, wx-free) runs for `replay_run`, `--run` and
Action Replay:

```jsonc
{ "orca_replay_version": "0.2.0", "name": "repro #12345", "vars": {"model": "${replay_dir}/part.stl"},
  "steps": [
    {"tool": "project_import", "args": {"paths": ["${vars.model}"]}},
    {"tool": "plate_slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}},
    {"tool": "window_screenshot", "args": {"target": "canvas", "save_path": "${out_dir}/preview.png", "overwrite": true}}
  ]}
```

`orca_replay_version` is the format's semver: a minor version adds tools or fields, a major one renames or removes
them, and while the major version is 0 a build reads only its own minor version. The step fields, placeholders and
matchers are in `replay_run`'s description. Steps are child calls of the replay's job, so they are not journaled and a
replay's wait for idle doesn't wait for itself.

**The journal** records the client calls that change something, plus the waits, screenshots and recordings a replay
needs, in the schema's form, with locators for refs and indices for ids. A call that runs as a job is judged by its
final result, so a replay never repeats a call that did nothing, and one that asked in a dialog stays, since the
steps after it answer that dialog. It keeps a bounded number of recent calls, so a long session can't grow it without
limit.

**`--run FILE`** runs a replay or a Python script once the GUI is ready, then closes the app and exits 0 when it
passed, 1 when a step failed and 2 when the file is invalid; `--outputdir` is where files saved without a path go.
A `--run` launch is never handed to a running instance, so its exit code is its own, and its session is the run's, so
a person's unsaved-project backup is still offered at their next launch.

**Action Replay** is a window where a person opens, writes and replays a file, whole or a step at a time. A replay is
a file people pass around, so before a run the window describes each step and flags what reaches beyond the project:
writing a file outside the output folder, changing presets, replacing the project, sending to a printer, or running a
program. When a step would replace a project with unsaved changes, it asks whether to save first. The window's
prompts are the person's own, so no tool and no auto-answer rule a replay registers answers them.

## Python scripts

A Python script is a `.py` file or a code string that runs on the plugin interpreter as the main program of a run,
with the whole plugin API and without being packaged as a plugin. A person runs one in the Run Script panel or with
`--run file.py`; an agent sends one with `script_run`, and a plugin's code may too. A script that runs under
`python file.py`, or code written for a plugin's `execute()`, runs here unchanged. A *file run*, whose `filename`
names an existing file, gets `__file__`, `sys.argv`, imports from its folder and its PEP 723 dependencies, installed
into the package folder plugins share; code sent as text gets none of that. The outcome (`finished`, `raised`,
`cancelled`, `timed_out`) is reported in a successful result, so the journal keeps the call and a replay of it fails
again. What a script can rely on:

- **Runs overlap**, an agent's beside a person's, so a run's output, `__main__`, `sys.argv` and identity are keyed by
  the run bound to the calling thread, and carried into the threads and pool tasks it starts.
- **Imports.** A file run imports from its folder without anyone changing `sys.path`, and an edited helper is
  compiled afresh on the next run. `sys.modules` is one cache, so overlapping scripts from different folders share a
  helper of the same name; helpers want distinct names.
- **The end of a run.** Like `python file.py`, a run ends once its code, its non-daemon threads, its window callbacks
  and its pool tasks are done. Then it shuts down the pools its code created and calls its `atexit` functions once.
  Pool workers are shared by every run and belong to none.
- **Stop.** `job_cancel` and `timeout_s` interrupt the run, again every second until it ends, but never inside the
  host's own bookkeeping; five seconds after the first stop the run stops waiting for its work. Code blocked in a C
  call holds the run until it returns.
- **Windows.** What a run opens closes with it. A window callback runs on the UI thread, so it can't call tools,
  which wait for the UI thread, and hands work to the script's own code. While a modal the run opened is up, tool
  calls that change the project are refused, so interactive scripts use modeless windows.
- **Idle.** A `script_run` job doesn't count as busy for `app_wait {for: "idle"}`, though the calls it makes do, so a
  macro waiting for the person doesn't hold every agent's wait.

**Identity.** A person's script runs under the audit identity `(script)` and an agent's under `(mcp)`, told apart by
the person mark. The plugin audit ([plugin-permissions.md](plugin-permissions.md)) judges a script as it judges a
plugin, but a script has no install state to hold a grant, so it is never asked. A plugin's own `script_run` runs
under the plugin's identity and grants.

**The setting.** One switch, `python_scripts_unrestricted`, off by default, decides how scripts are judged. Off, a
script's own code is held to OrcaSlicer's folders and denied programs, threads and the network; on, it may do what
the audit allows any plugin. The audit is a Python audit hook, not a sandbox: the tools a script calls run
OrcaSlicer's own code and are not judged. Only a person changes the switch, in a dialog no tool reaches, and
`app_state.python.scripts_unrestricted` tells an agent what its script may do.

The Run Script panel is a dock pane where a person runs a file, a `# %%` cell or a console line in a live session,
which keeps its globals between pieces of work and is an unjournaled person's job. Its Stop interrupts the work now
running while the session goes on; Restart session, opening or closing a script and quitting end the session as a
stopped run ends.

## Plugins and the tools

A plugin's code calls the commands through `orca.host.tools` only with the tools permission or full access, asked for
at load or when it first calls, as its other permissions are ("Calling OrcaSlicer's tools" in
[plugin-permissions.md](plugin-permissions.md)). The tools run OrcaSlicer's own code, which the plugin audit doesn't
judge, so a plugin's other grants bound its Python code and not what it asks the tools to do, and each call is logged
with the plugin's key. A call runs as a job owned by the plugin, never journaled, so the session tools are scoped to the
plugin, and what its calls leave behind ends when it unloads or loses the permission, from which moment its calls are
refused; a call made before that leaves nothing behind for the client.

A plugin may offer operations, `plugin.<plugin>.<name>`, which `tool_list` lists and `tool_call`, scripts and replay
steps run. They are a source of the command catalog, looked up after its own commands, and run as jobs under the
plugin's identity, guarded and journaled as a change when the plugin declares one.

## Agent Activity pane

View › Agent Activity shows a dock pane that lists, live, every call automation makes in the app, reads included:
an agent's, a replay's, a script's and a plugin's, with the dialogs auto-answer rules answered. A person's own picks
are left out. Each call into the core names its origin, and `ActivityLog` (no wx) lists only while the pane is open.
It masks what captions mask, and fails closed where masking can't be complete. The pane only watches.

## stdio bridge

`orca-slicer --mcp-stdio [--datadir DIR] [--mcp-port N]` serves clients that speak only stdio. It forwards each
message to the instance the discovery file names, and starts OrcaSlicer only for a tool call, so a session
registered user-wide that never calls a tool never opens the app. Until an instance answers, it answers the handshake,
`tools/list` and the rest itself from the same command catalog, with the server's exact replies, which needs a
catalog built without the runtime, so registering a command never reads app state. A tool call with nothing
answering starts `orca-slicer --mcp` and waits for it to be ready. If a running instance without MCP takes that
launch over, each tool call says to turn the server on in Preferences, and a failed start is not tried again, so a
stuck start never makes every call another launch. A request that could not be sent, or was refused because the
token changed, is sent once more; one that lost its reply is never sent again, since it may have run. Every request
has a deadline, so a server that never replies can't hang the agent.

## Making a feature reachable

A change to the GUI needs nothing for automation until someone writes its command: until then the GUI commands (GUI
automation) reach it as a person does. A menu item of the Design tab is an entry of `tool_atlas.json` either way (A
Design command, below).

A feature is a command, named for its place, then its verb (mcp-tools.md, Naming), declared in its place's file:

1. **Build it** with `inspect_tool()` for pure observation or `mutate_tool()` for a change, label it, give it a
   schema, a description in the order above and one to three examples. A new place word goes in `kPlaceWords`, and
   a new heading in `kHeadingOrder`.
2. **Put what it does in an operation** of `GUI/Operations` and name it with `reaching()`, or join
   `test_operations`' exception list with a reason. The handler reads its arguments, resolves its targets, calls
   the operation and shapes the reply; all wx work is in `ctx.run_ui()`, and a change ends with `settle()`.
3. **Make its menu items its actions** (`NativeCommands::action()`), with the arguments that pick the variant, and
   give the command its person's form. The call with arguments must not prompt.

The command then appears in `tools/list` or `tool_list`, `tool_describe`, the Speed Dial and replays. A new or
changed command changes the command reference, `tests/data/command_catalog.json`, where a reader outside the app sees
every command and which the skill's action list is written from (`tools/mcp/gen_skill_reference.py`). The test "the
checked-in command reference matches the catalog" fails until it is regenerated: run
`slic3rutils_tests "[Reference]"` with `ORCA_UPDATE_COMMAND_REFERENCE=1` and check the file in.

#### A Design command

A Design command is an entry of `scripts/CAD/tool_atlas.json` instead:

1. Name it by its place, then its verb (`solid_shell`), and add its entry under its heading: a menu item with its
   argument form (a `tool` block), a menu-only item, an argument-only command, or a folded variant of a command that
   exists.
2. Wire a menu item's handler in `DesignPanel`, which the entry's `action` names.
3. Run `scripts/CAD/gen_offer_table.py`, which writes `DesignOffer.hpp` and refuses a table that breaks its rules.
4. Write its silent form in its heading's `Design<Heading>Tools.cpp` and bind it by id. A change an operation stands
   for goes through the operation, and several Prepare edits share one outer snapshot, so the call is one undo step.
5. Update the counts and names `tests/slic3rutils/test_mcp_cad.cpp` pins, keep `tool_list`'s names index within 4096
   characters, and regenerate the command reference. A new heading also needs its file in `src/slic3r/CMakeLists.txt`
   and `localization/i18n/list.txt`.

## Configuration

- `--mcp` opens the endpoint at startup, and `--mcp-port N` on that port (0 takes the preference's).
- `--mcp-stdio` runs the stdio bridge in place of the app.
- `--run FILE` with `--outputdir DIR` runs a replay or a script (Replays and the journal).
- `--unrestricted-scripts` lets scripts run unrestricted for the run, and `--cad` turns the Design tab on.

A flag turns its setting on for the run and never writes the configuration. AppConfig holds `mcp_enabled` (off) and
`mcp_port` (13619). Preferences › General › AI features turns the server on, shows its port and address, and copies
the setup for an agent (Claude Code, Codex, opencode and others). HTTP is the default, since its URL stays the same
whichever build runs and nothing runs on the agent's side; the cost is that OrcaSlicer must be running with the
server on, and a registration holds the token, so a new token or port means copying the setup again. Stdio suits an
agent that should open OrcaSlicer on demand, and its setup names this executable, so it breaks when the program
moves. `--run` beside a slicing action, where no GUI starts, is refused rather than left unrun.

## Testing

The wx-free core is tested by `automation_tests` (`tests/automation/`), which links nothing of the GUI library, so a
wx include in a core unit fails to compile. What needs the GUI library is tested in `tests/slic3rutils/`
(`slic3rutils_tests "[Mcp],[CommandCatalog],[Operations],[ImGuiWrapper]"`). `[CommandCatalog]` checks the catalog's
rules, `[Reference]` the checked-in command reference, and `[Mcp][CAD]` the Design commands (design-tab.md,
Verification); `tests/AGENTS.md` says where a new case goes.

End to end, the MCP sweep (`tools/mcp/run_sweep.py`) drives the built app through its server, as an agent does:
[tools/mcp/TESTS.md](../../tools/mcp/TESTS.md) holds its rules and the index of its tests, and
[tools/mcp/README.md](../../tools/mcp/README.md) says how to run it. No sweep test sends a print job or reaches a
physical printer.
