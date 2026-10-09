# Threads, timers and the application lifecycle

The GUI-thread rule and every way OrcaSlicer moves work between threads (`CallAfter`,
`wxQueueEvent`, `wxThreadEvent`, the Jobs framework, `BackgroundSlicingProcess`, network-agent
callbacks), worker-thread exception safety, `wxTimer`, when queued and idle work actually runs,
yields and nested event loops, progress dialogs, startup, shutdown and exception policy, the splash
screen, and the access rules for `wxGetApp()` and `app_config`. Read it whenever code runs off the
main thread, defers work, starts a timer, yields, shows progress, or runs during startup or shutdown.

wx cites are relative to the pinned wx 3.3.2 tree (located as in `SKILL.md` §Ground truth).
Orca builds wx with `wxBUILD_DEBUG_LEVEL=0` and `libslic3r_gui` with `wxDEBUG_LEVEL=0`, so `wxASSERT`
is compiled out and `wxCHECK*` returns silently: a timer started off the main thread never fires on macOS,
`Exit()` on a loop that is not the active one is ignored, `Start(0)` on macOS fails — all without
a diagnostic. **[source]** marks behaviour derived from the implementation rather than the docs.
The generic `CallAfter` mechanics and the liveness rule for deferred lambdas on the main thread
live in `references/events.md` §CallAfter; this file owns the cross-thread side.

Contents: [Rules](#rules) · [The GUI-thread rule](#the-gui-thread-rule) ·
[Marshaling to the GUI thread](#marshaling-to-the-gui-thread) ·
[Liveness of deferred work](#liveness-of-deferred-work) ·
[Blocking round-trips](#blocking-round-trips) ·
[Worker threads: lifetime and exceptions](#worker-threads-lifetime-and-exceptions) ·
[The Jobs framework](#the-jobs-framework) · [BackgroundSlicingProcess](#backgroundslicingprocess) ·
[wxTimer](#wxtimer) · [When queued and idle work runs](#when-queued-and-idle-work-runs) ·
[Yields and nested event loops](#yields-and-nested-event-loops) ·
[Progress and busy indicators](#progress-and-busy-indicators) ·
[Application lifecycle](#application-lifecycle) · [Exceptions in the main loop](#exceptions-in-the-main-loop) ·
[Splash screen](#splash-screen) · [wxGetApp() and app_config](#wxgetapp-and-app_config)

## Rules

1. Apart from `CallAfter`/`QueueEvent` and `wxLog`, call wx only on the main thread: windows,
   `wxBitmap`/`wxImage` copies, timers, busy cursors, `wxExecute`, yields, `Bind`/`Unbind`,
   `wxWeakRef`. → [GUI-thread rule](#the-gui-thread-rule)
2. From a worker, marshal with `wxGetApp().CallAfter([by-value captures]{…})`, or
   `wxQueueEvent(handler, new Evt…)` to a handler whose lifetime strictly contains the thread's.
   Never post from a worker to a window that can be destroyed while the thread runs.
   → [Marshaling](#marshaling-to-the-gui-thread)
3. Never use `wxPostEvent`/`AddPendingEvent` from a worker. → [Marshaling](#marshaling-to-the-gui-thread)
4. Re-check liveness inside every deferred lambda (`is_closing()` for app-level state, an alive
   flag for a window); a check on the worker side is only an early-out. → [Liveness](#liveness-of-deferred-work)
5. A worker blocked on a UI round-trip must be cancellable from the UI side, and the UI must never
   wait for that worker except through an API that pumps the round-trip. → [Blocking round-trips](#blocking-round-trips)
6. Catch everything at the top of every `std::thread`/`boost::thread` body; use `error_code`
   filesystem calls on workers. → [Worker threads](#worker-threads-lifetime-and-exceptions)
7. Stop or join workers before `OnExit` returns; after wx cleanup starts, `wxGetApp()` dereferences
   null. → [Worker threads](#worker-threads-lifetime-and-exceptions)
8. UI-initiated background work is a `Job` on `Plater::get_ui_job_worker()` (or a dialog-owned
   `PlaterWorker`): UI only in `finalize`/`call_on_main_thread`, poll `was_canceled()`, never apply
   partial results, handle `eptr`. → [Jobs](#the-jobs-framework)
9. Drop queued jobs with `cancel_all()`/`replace_job()`; `cancel()` only flags the running job.
   → [Jobs](#the-jobs-framework)
10. A `wxTimer` must not outlive the handler it notifies; prefer a member timer. → [wxTimer](#wxtimer)
11. Bind `wxEVT_TIMER` with the timer's id, on the timer itself, or override `Notify()`; never rely
    on an owner-wide bind when the owner has two timers. → [wxTimer](#wxtimer)
12. Never start a timer with 0 ms (macOS rejects it); for "right after this handler" use `CallAfter`.
    → [wxTimer](#wxtimer)
13. Don't throw from `wxTimer::Notify()`. → [wxTimer](#wxtimer)
14. Don't feed long UI-thread work through self-reposting `CallAfter` chains or `RequestMore()`
    loops; slice it with `IdleScheduler` or a timer. → [When queued and idle work runs](#when-queued-and-idle-work-runs)
15. Expect `CallAfter` bodies, timers and (category permitting) worker events to run inside every
    `ShowModal()`, `wxYield()` and progress-dialog `Update()`/`Pulse()` (on GTK a progress-dialog
    yield runs timers but no queued event). → [Yields](#yields-and-nested-event-loops)
16. No `wxYield()` in handlers to "let the UI update" — move the work off the main thread. If a
    yield is unavoidable, create the `wxWindowDisabler` before yielding. → [Yields](#yields-and-nested-event-loops)
17. Exit a nested `wxEventLoop` from deferred code with `ScheduleExit()`; `Exit()` and
    `ExitMainLoop()` are silent no-ops while another loop is stacked on top. → [Nested loops](#nested-event-loops)
18. `wxProgressDialog`: use `wxPD_AUTO_HIDE` when closing it programmatically; on `Update()==false`
    destroy it or `Resume()`; don't start one-shot timers before showing it on MSW. → [Progress](#progress-and-busy-indicators)
19. No exception may escape an event handler, `CallAfter` body, timer handler or `Job::finalize`:
    Orca's `OnExceptionInMainLoop` rethrows and the session ends. → [Exceptions](#exceptions-in-the-main-loop)
20. Never touch windows in `OnExit` or the `GUI_App` destructor; stop timers, workers and callbacks
    in `MainFrame::shutdown`/`GUI_App::shutdown`. → [Shutdown](#shutdown)
21. Code reachable during shutdown or `recreate_GUI` checks `!wxTheApp || wxGetApp().is_closing()`
    and null `plater()`/`mainframe`; `is_closing()` stays false during `recreate_GUI`.
    → [wxGetApp() and app_config](#wxgetapp-and-app_config)
22. `AppConfig` is main-thread only; `save()` throws on a worker. → [wxGetApp() and app_config](#wxgetapp-and-app_config)
23. A splash that stays up across startup work overrides `FilterEvent` and is held in a `wxWeakRef`.
    → [Splash screen](#splash-screen)

## The GUI-thread rule

**Contract.** "GUI calls, such as those to a wxWindow or wxBitmap are explicitly not safe at all in
secondary threads … the recommended way is to simply process the GUI calls in the main thread
through an event that is posted by wxQueueEvent(). This does not imply that calls to these classes
are thread-safe, however, as most wxWidgets classes are not thread-safe, including wxString"
(`interface/wx/thread.h:967-978`). The GUI thread is the one in which wx was initialised, where
`wxIsMainThread()` returns true (`docs/doxygen/overviews/thread.h:52-59`; `interface/wx/thread.h:1806`).
New code should use the C++ concurrency classes rather than the wx ones; the GUI rule still applies
(`docs/doxygen/overviews/thread.h:19-21`).

"GUI" is wider than windows:

| Main-thread only | Why |
|---|---|
| Window create/show/destroy, `SetLabel`, `Refresh`, layout, dialogs | documented rule; on GTK no TLW creation off the main thread (`interface/wx/thread.h:1837`) |
| `wxTimer` start/stop | "A timer can only be used from the main thread" (`interface/wx/timer.h:41`) |
| `wxExecute` | "can only be used from the main thread" (`interface/wx/utils.h:1250`); see `references/strings-i18n-files.md` |
| `wxBeginBusyCursor`/`wxBusyCursor`, `wxYield`/`YieldFor` | GUI calls; `YieldFor` off the main thread returns false and does nothing **[source]** (`src/common/evtloopcmn.cpp:128-136`) |
| Copies of `wxImage`/`wxBitmap`/`wxFont` (lambda capture, `SetPayload`) | `wxRefCounter::IncRef` is a plain `m_count++` **[source]** (`include/wx/object.h:166`) |
| `wxWeakRef` create/copy/test, `Bind`/`Unbind` | `wxTrackable`'s tracker list is an unlocked linked list (`include/wx/tracker.h:40-50`); `DoBind` takes no lock **[source]** |

`wxMutexGuiEnter/Leave` "only works under some ports (wxMSW currently)" (`interface/wx/thread.h:1834-1838`)
and the threading overview deliberately leaves them out as unsafe ("we explicitly don't name wxMutexGUIEnter() and
wxMutexGUILeave() as they're not safe. See also ticket #10366", `docs/doxygen/overviews/thread.h:8-11`).
Never use them.

`wxLog` calls are allowed from workers: messages logged from other threads are buffered until
`wxLog::Flush()` runs on the main thread, and may appear out of order
(`docs/doxygen/overviews/log.h:228-237`). Orca code logs with `BOOST_LOG_TRIVIAL`, which is thread-safe.

**OrcaSlicer.**
- Threads that reach the GUI: network agents and printer agents, `Http::perform()` callbacks (run
  on the request's own `std::thread`), the slicing thread (`BackgroundSlicingProcess`), the Jobs
  worker, the preset-sync and preset-updater threads, ad-hoc `std::thread`s (plugin dialogs, texture
  compression, 3Dconnexion HID), and TBB parallel loops (which must make no wx calls at all).
- Main-thread test: `Slic3r::is_main_thread_active()` (`libslic3r/Thread.hpp`; the id is recorded by
  `save_main_thread_id()` at startup). Code callable from both sides uses "run now if main, else
  `CallAfter`":
  ```cpp
  if (is_main_thread_active()) app_config->save();
  else                         CallAfter([this] { app_config->save(); });   // GUI_App::load_pending_vendors
  ```
- Threads are created with `Slic3r::create_thread(attrs, fn)` (`libslic3r/Thread.hpp`, 16 MB stack,
  because deep CGAL recursion in the emboss path overflowed 4 MB) and named with `set_thread_name`.
- Long work never runs in an event handler. UI-initiated work goes to the Jobs framework; slicing
  and export to `BackgroundSlicingProcess`; other background work to a worker thread that reports
  back through the mechanisms below.

**Pitfalls.**
- **Rule:** Never build or copy a `wxImage`/`wxBitmap` on a worker and hand it to the UI thread.
  **Why:** wx makes no thread-safety promise for them and their ref-count is non-atomic; a copy in a
  lambda capture races the UI-side copy. Ship raw bytes and decode on the UI thread.
  ```cpp
  // Wrong (Http thread)
  wxImage img; img.LoadFile(stream); wxGetApp().CallAfter([img] { show(img); });
  // Right
  wxGetApp().CallAfter([alive, body = std::move(body)] {
      if (!alive->load()) return;
      wxMemoryInputStream s(body.data(), body.size()); wxImage img(s); show(img);
  });
  ```
  Cite: `DeviceErrorDialog::get_fail_snapshot_from_cloud` (its comment states this rule). The camera
  decoder (`wxMediaCtrl3`) deliberately builds frames on its worker; it hands them over only through
  `m_frame`, which both threads touch under `m_mutex`, never through a capture or an event payload —
  `references/webview-gl-aui-media.md` §Camera and media.

## Marshaling to the GUI thread

| Mechanism | From a worker | Target lifetime requirement | Use for |
|---|---|---|---|
| `wxGetApp().CallAfter(fn)` / `wxTheApp->CallAfter(fn)` | yes | app lives until wx cleanup; re-check every captured pointer inside | arbitrary code; the Orca default |
| `wxQueueEvent(h, new Evt(…))` / `h->QueueEvent(…)` | yes | `h` must outlive the posting thread | typed results to a known long-lived handler |
| `wxQueueEvent(h, threadEvt.Clone())` with `wxThreadEvent` | yes | same | results that must not run inside masked yields |
| `Job::Ctl::update_status` / `call_on_main_thread` | worker of a `Worker` | the `Worker` (joined in its dtor) | UI-initiated jobs |
| `BackgroundSlicingProcess::execute_ui_task` | slicing thread | plater (BSP thread joined first) | synchronous UI call with cancellation |
| `window->CallAfter(fn)`, `wxPostEvent`, `AddPendingEvent` | **no** (except `window->CallAfter` when the thread is joined before the window dies) | — | main-thread → main-thread deferral |

### CallAfter from a worker

**Contract.** "It is safe to use CallAfter() from other, non-GUI, threads, but that the method will be
always called in the main, GUI, thread context." The method form must name a method of the object
`CallAfter` is called on; the functor form is meant for lambdas (`interface/wx/event.h:491-564`).
Its documented purpose is deferring work that can't run in the current handler — "you shouldn't show
a modal dialog from a mouse click event handler as this would break the mouse capture state".

**Mechanism [source].**
- `CallAfter(fn)` is `QueueEvent(new wxAsyncMethodCallEventFunctor<T>(this, fn))` on the object it is
  called on (`include/wx/event.h:3852-3855`).
- The functor is **copied** (`m_fn(fn)`, `include/wx/event.h:1554-1569`): a lambda capturing a
  `std::unique_ptr` does not compile. Capture a `std::shared_ptr`.
- It runs through `SafelyProcessEvent`, so `wxApp::FilterEvent` sees it and an exception goes to
  `OnExceptionInMainLoop` ([Exceptions](#exceptions-in-the-main-loop)).
- Its event keeps the default **`wxEVT_CATEGORY_UI`** (`include/wx/event.h:1014`), so on MSW and
  macOS the body runs inside every progress-dialog `Update()`/`Pulse()` and every
  `YieldFor(wxEVT_CATEGORY_UI…)`; on GTK a masked yield runs no queued event at all, so it waits
  until the yield returns ([event categories](#event-categories-and-yields)).
- **Target choice.** A `window->CallAfter` queued from the main thread is dropped if the window dies
  first, because `~wxEvtHandler` calls `DeletePendingEvents()` (`src/common/event.cpp:1234-1238`).
  From a worker the same call is a race: `QueueEvent` appends under `m_pendingEventsLock`, but
  `DeletePendingEvents()` runs without it (`src/common/event.cpp:1319-1366`), and a window freed
  before the call is plain use-after-free. Post to the app, which outlives every window.
- `QueueEvent` with `wxTheApp == nullptr` deletes the event silently (`src/common/event.cpp:1323-1333`),
  but `wxGetApp()` is `*static_cast<GUI_App*>(wxApp::GetInstance())` (`include/wx/app.h:941`): after
  `SetInstance(nullptr)` in wx cleanup it dereferences null. Workers must be stopped before that.

**Usage.**
```cpp
// worker thread (agent / Http / std::thread)
if (!wxTheApp || wxGetApp().is_closing()) return;          // early-out only
wxGetApp().CallAfter([this, dev_id, msg] {                  // copies, not references
    if (wxGetApp().is_closing()) return;                    // the real check, on the UI thread
    handle(dev_id, msg);
});
```
Cite: `ActionRegistry::init` (`on_source`/`on_capability`), the `message_arrive_fn` lambda in
`GUI_App` (network-agent callbacks).

`CallAfter` is also the tool on the main thread for deferring past the current event — e.g. the
config wizard is opened from `GUI_App::post_init` through `CallAfter` because on macOS that is the
only way to pop a modal at startup without breaking combo boxes.

### wxQueueEvent, wxPostEvent, AddPendingEvent

**Contract.**
- `QueueEvent(wxEvent*)` is asynchronous, "takes ownership of the event parameter … the pointer can't
  be used any more after the function returns", "can be used for inter-thread communication from the
  worker threads to the main thread. It is safe in the sense that it uses locking internally", and
  wakes the loop with `wxWakeUpIdle()` (`interface/wx/event.h:411-466`). `wxQueueEvent(dest, ev)`:
  `dest` "can't be NULL", `ev` must be heap-allocated (`interface/wx/event.h:5368-5382`).
- `AddPendingEvent`/`wxPostEvent` copy the event with `Clone()` (a stack event is fine) but "can't be
  used to post events from worker threads for the event objects with wxString fields (i.e. in practice
  most of them)" (`interface/wx/event.h:469-488`, `5355-5366`). The thread overview: use `QueueEvent`
  for inter-thread communication; with only the main thread either is fine. In 3.3 `wxString` is
  always standard-string-backed (`std::wstring` in Orca's wchar build) and never copy-on-write
  (`include/wx/string.h:119-132`), so the old shared-buffer explanation is historical — follow the
  documented rule anyway.
- Every queued or posted event class must implement `Clone()` (`references/events.md` §Custom events).
- Ownership is the event's own data only: `wxCommandEvent::SetClientObject/SetClientData` payloads are
  never owned by the event. **The target must outlive the posting thread** — the race with
  `~wxEvtHandler` above applies to every `QueueEvent`.

**Usage.**
```cpp
auto* e = new wxCommandEvent(EVT_MY_DONE, id); e->SetString(path);
wxQueueEvent(long_lived_handler, e);                         // ownership passes to wx
```

### wxThreadEvent

**Contract.** Category `wxEVT_CATEGORY_THREAD`, so it is "NOT to be processed by
wxEventLoopBase::YieldFor calls (unless the wxEVT_CATEGORY_THREAD is specified - which is never in wx
code)"; `Clone()` unshares the string; for `SetPayload<T>`, "T's copy constructor must be thread-safe,
i.e. create a copy that doesn't share anything with the original" (`interface/wx/event.h:3736-3790`).
The thread overview recommends it precisely to avoid out-of-order processing when
`YieldFor`/`wxProgressDialog` is in use (`docs/doxygen/overviews/thread.h:79-85`).

**Source facts.**
- `wxThreadEvent` derives from `wxEvent`, not `wxCommandEvent` (`include/wx/event.h:1367`): it does
  **not** propagate to parents. Queue it on the object that binds `wxEVT_THREAD`.
- Payloads: PODs, `std::string`, or a `std::shared_ptr` the worker no longer touches. Never
  `wxImage`/`wxBitmap`/`wxFont`.
- "Never processed in yields" is only true for masked `YieldFor`: `wxYield()`/`wxSafeYield()` use
  `wxEVT_CATEGORY_ALL`, and the MSW native `wxProgressDialog` yields `ALL & ~USER_INPUT`, which
  includes THREAD (`src/msw/progdlg.cpp` `wxProgressDialog::DispatchEvents`).

```cpp
wxThreadEvent ev(wxEVT_THREAD, wxID_OK);
ev.SetPayload(std::make_shared<Result>(std::move(r)));
wxQueueEvent(dialog, ev.Clone());            // dialog binds wxEVT_THREAD and outlives the thread
```

### Event categories and yields

A queued event whose category is not in the active yield's mask stays queued and runs after the
yield (`src/common/event.cpp` `wxEvtHandler::ProcessPendingEvents`, `IsEventAllowedInsideYield`)
**[source]**. Categories come from `GetEventCategory()`: `wxEvent` defaults to UI
(`include/wx/event.h:1014`), `wxCommandEvent` returns USER_INPUT (`include/wx/event.h:1656`)
**[source]**, `wxThreadEvent` returns THREAD.

| Queued event | Category | Generic / Orca progress dialog `Update`/`Pulse` (UI\|USER_INPUT) | MSW native `wxProgressDialog` (ALL & ~USER_INPUT) | `wxYield`, `wxSafeYield`, `ShowModal` |
|---|---|---|---|---|
| `CallAfter`; plain `wxEvent` subclasses (Orca `SimpleEvent`, `Event<T>`, `SlicingProcessCompletedEvent`) | UI | runs | runs | runs |
| `wxCommandEvent` family (e.g. `new wxCommandEvent(id)` queued by BSP) | USER_INPUT | runs | held | runs |
| `wxThreadEvent` | THREAD | held | runs | runs |

**GTK exception [source]:** the "runs" cells of the masked-yield column hold only on MSW and macOS.
wxGTK's `wxGUIEventLoop::DoYieldFor` loops `while (Pending()) gtk_main_iteration();`, and
`Pending()` → `wxApp::EventsPending()` removes wx's idle source — the only place GTK runs pending
events — before every iteration (`src/gtk/evtloop.cpp:377-395`, `src/gtk/app.cpp` `wxApp::EventsPending`);
the base `DoYieldFor` processes pending events only for `wxEVT_CATEGORY_ALL`
(`src/common/evtloopcmn.cpp:172-193`). So on GTK a masked `YieldFor` (generic/Orca progress dialog
`Update`/`Pulse`) runs no queued event of any category, despite the doc's "selectively processed by
all ports" (`interface/wx/evtloop.h:260-263`); they run once control is back in the main loop (wx
re-arms its idle source from the next GTK `event`/`size_allocate` signal emission, `wx_add_idle_hooks`).
Full yields (`wxYield`, `wxSafeYield`) and modal loops (`ShowModal`) run them on every port.

Timer events are not queued; see [wxTimer](#wxtimer) for the MSW one-shot loss inside masked yields.

### OrcaSlicer: who marshals how

- **Network and printer agents.** `GUI_App` installs the agent callbacks (`set_on_message_fn`,
  `set_on_local_message_fn`, `set_on_server_connected_fn`, …) with the double `is_closing()` check and
  by-value captures, and hands lower layers a queue-on-main function:
  `m_agent->set_queue_on_main_fn([this](std::function<void()> cb) { CallAfter(cb); })`.
  `MachineObject`/`DeviceManager` state is mutated only inside those main-thread lambdas
  (`obj->parse_json(…)`), so device objects need no locking; device UI is pull-based from a timer
  (`MonitorPanel`). See `docs/HLSD/printer-agent.md` for the agent design.
- **Ad-hoc threads** (preset sync, plugin dialogs): `wxGetApp().CallAfter` plus an alive flag (the
  preset-sync thread: app `CallAfter` with an `is_closing()` re-check inside, and a
  `std::weak_ptr<int>` sync token that ends its loop, `GUI_App::start_sync_user_preset`).
- **Windows' own `CallAfter` from a thread** only when the thread is joined before the window dies:
  the 3Dconnexion thread posts `wxGetApp().plater()->CallAfter(…)`, and `MainFrame::shutdown` stops it
  (`get_mouse3d_controller().shutdown()`) first; `wxMediaCtrl3`'s decoding thread posts its repaint
  `CallAfter` on the control, and `~wxMediaCtrl3` joins it before the window's base destructors run.
- **Backup thread.** `MainFrame` registers `Slic3r::set_backup_callback`, whose action 0 calls
  `wxPostEvent(this, wxCommandEvent(EVT_BACKUP_POST))` from the backup thread — the documented-unsafe
  API (harmless only because the event's string is empty). New code uses
  `wxQueueEvent(handler, new wxCommandEvent(…))`. `MainFrame::shutdown` clears the callback first.
- **Main thread → main thread** decoupling uses `wxPostEvent`, e.g. Preferences posting
  `SimpleEvent(EVT_GLCANVAS_COLOR_MODE_CHANGED)` to the plater after a colour-mode change.
- **Webview script messages** arrive synchronously inside native callbacks on WebKitGTK and
  WKWebView; defer window work out of them — see `references/webview-gl-aui-media.md`.

### Coalescing high-rate updates

A worker reporting progress hundreds of times a second must not queue one `CallAfter` per report: each
is a heap event, and the app drains **every** pending event (including ones queued meanwhile) before
it returns to native input and paint (`src/common/appbase.cpp:561-601`) **[source]**. Keep the latest
values in shared state and keep at most one call queued:

```cpp
struct Progress { std::mutex m; int pct = 0; std::string msg; bool queued = false; };
// worker
void report(const std::shared_ptr<Progress>& st, std::shared_ptr<std::atomic<bool>> alive, int pct, std::string msg) {
    { std::lock_guard<std::mutex> lk(st->m); st->pct = pct; st->msg = std::move(msg);
      if (st->queued) return; st->queued = true; }
    wxGetApp().CallAfter([st, alive] {
        int pct; std::string msg;
        { std::lock_guard<std::mutex> lk(st->m); pct = st->pct; msg = st->msg; st->queued = false; }
        if (!alive->load() || wxGetApp().is_closing()) return;
        /* update the UI with pct, msg */
    });
}
```
The Jobs framework avoids the per-report wx event for you: status messages sit in the worker's own
queue and are drained in one batch per idle/paint (each still applied, in order).

**Pitfalls.**
- **Rule:** From a worker, post to the app (or a handler that outlives the thread), never to a window.
  **Why:** `this->CallAfter` / `wxQueueEvent(this, …)` from a thread races the window's destruction;
  `~wxEvtHandler`'s `DeletePendingEvents()` is unlocked (`src/common/event.cpp:1361-1366`).
  ```cpp
  // Wrong (Http / agent thread)
  CallAfter([this, text] { m_label->SetLabel(text); });
  // Right
  wxGetApp().CallAfter([this, alive = m_alive, text] { if (!alive->load()) return; m_label->SetLabel(text); });
  ```
  Cite: `src/common/event.cpp` `wxEvtHandler::QueueEvent`, `wxEvtHandler::~wxEvtHandler`;
  `PluginsDialog`/`DeviceErrorDialog` alive flags.
- **Rule:** Never `wxPostEvent`/`AddPendingEvent` from a worker; use `wxQueueEvent(h, new Evt)` or
  `wxThreadEvent::Clone()`. Cite: `interface/wx/event.h:469-488`.
- **Rule:** Capture `std::shared_ptr`, not `std::unique_ptr`, in `CallAfter`. **Why:** the functor is
  copied; a move-only lambda does not compile. Cite: `include/wx/event.h:1554-1569`.
- **Rule:** Don't expect a `wxThreadEvent` to be held back by every yield. **Why:** only masked
  `YieldFor` holds it; `wxYield` and the MSW native progress dialog dispatch it, and `CallAfter`
  (UI category) is never held on MSW/macOS (GTK holds every queued event in a masked yield).
  Cite: `src/msw/progdlg.cpp` `DispatchEvents`.

## Liveness of deferred work

Every deferred body runs later, on the main thread, when the objects it names may be gone. The check
must happen **inside** the lambda: a worker-side check followed by posting is a TOCTOU race — the
object can die between the check and execution. Three tools, by scope:

| Scope | Tool | Set | Checked |
|---|---|---|---|
| App shutdown | `GUI_App::is_closing()` (`std::atomic<bool> m_is_closing`) | `set_closing(true)` in the `MainFrame` close handler before `m_plater->reset()`, and in `GUI_App::shutdown()` | before posting (early-out) and first thing inside the lambda |
| One window | `std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true)`; dtor does `m_alive->store(false, std::memory_order_release)` | the derived destructor | capture `alive = m_alive` by value; `alive->load(std::memory_order_acquire)` inside |
| Main-thread-only object | `wxWeakRef<T>` (`interface/wx/weakref.h`) | nulls in `~wxTrackable` | main thread only — never create, copy or test it on a worker |

- Exemplars: `PluginsDialog`, `PluginsConfigDialog`, `SpeedDialWebDialog` (alive flag);
  `DeviceErrorDialog` (alive flag plus a request sequence number so a stale reply can't overwrite a
  newer one). The legacy `std::shared_ptr<int>` token + `std::weak_ptr<int>` capture works only when
  the `lock()`/`expired()` check sits inside the main-thread lambda; legacy code that checks on the
  worker and then calls `this->CallAfter` is the race above — don't copy it.
- `is_closing()` covers app shutdown only. `GUI_App::recreate_GUI` (language change) destroys and
  rebuilds `MainFrame` without setting it (`GUI_App::shutdown` returns early while
  `is_recreating_gui()`), so lambdas holding `MainFrame`-child pointers need their own alive flag or
  must re-fetch through `wxGetApp().plater()`.
- After the main loop ends, still-queued app `CallAfter`s are deleted without running
  ([Shutdown](#shutdown)); their captures are destroyed with `wxTheApp == nullptr`, so capture
  destructors must not call into wx.
- `wxWeakRef` and the alive flag differ in timing: the flag flips at the start of the derived
  destructor; a `wxWeakRef` nulls only in `~wxTrackable`, after the derived destructors and
  `DestroyChildren` (`include/wx/event.h:3705-3706`; `include/wx/tracker.h:76-85`). See
  `references/windows-dialogs.md` for `Destroy()`-scheduled windows, which stay alive (and run their
  own `CallAfter`s) until idle deletion.

## Blocking round-trips

Sometimes a worker needs a result computed on the UI thread. Every such round-trip has two failure
modes: deadlock (the UI waits on the worker while the worker waits on the UI) and a lost wake-up
(the queued call is discarded or never pumped, so the worker waits forever).

| Helper | Shape | Deadlock/cancel protection |
|---|---|---|
| `Job::Ctl::call_on_main_thread(fn)` → `std::future<void>` | message in the worker's out-queue, delivered by `process_events()` or synchronously inside `wait_for_current_job`/`wait_for_idle` | the UI waits only through the `Worker` API, which delivers the call |
| `BackgroundSlicingProcess::execute_ui_task(fn)` | `m_plater->CallAfter` + `UITask` state + condition variable; returns false if cancelled | `stop()`/`stop_internal()` call `cancel_ui_task()` on the UI thread before waiting for the slicing thread |
| `run_on_ui_blocking(fn)` (`src/slic3r/plugin/host/PluginHostUi.cpp`) | runs `fn` inline when `wxIsMainThread()`; else `wxGetApp().CallAfter` with `promise`/`fn` captured **by reference**, then `fut.get()` | by-reference capture is safe only because the caller blocks until the lambda ran |

Rules that follow:
- Run inline when already on the main thread — `CallAfter` + `future.get()` on the main thread
  deadlocks.
- Never copy the by-reference capture into a fire-and-forget `CallAfter`.
- Never wait on a `call_on_main_thread` future while holding a lock the UI needs.
- A round-trip stalls while the main thread cannot pump: the idle-driven Jobs pump inside MSW native
  modal loops, and everything while a native macOS message/file/dir panel is up
  ([When queued and idle work runs](#when-queued-and-idle-work-runs)). Don't open native modals while
  a job waits on the UI, or accept the stall.
- Give the UI side a cancel that releases the worker (BSP's `UITask` pattern): when the UI must stop
  a worker that may be blocked on a round-trip, mark the pending task cancelled, wake the worker, then
  wait.
- A round-trip queued with `CallAfter` that is deleted unrun (target destroyed, or the app's queue
  discarded after the main loop ends) never sets its promise: the worker blocks forever in
  `fut.get()`, and whoever later joins it deadlocks. Stop such workers before the loop ends.

## Worker threads: lifetime and exceptions

**Exceptions.** An exception escaping a `std::thread`/`boost::thread` body calls `std::terminate`
and aborts the whole app. Wrap the entire body; log and convert to a status the UI can show.

- **Rule:** Wrap the entire body of any detached or background thread in `try/catch`, and on
  background threads use the `error_code`-based `boost::filesystem::directory_iterator` (log and
  skip) instead of the throwing one.
  **Why:** On macOS `readdir()` can transiently fail with ENOTSUP (errno 45); boost turns it into a
  `filesystem_error`, which killed OrcaSlicer at startup from the preset-sync thread
  (`GUI_App::scan_orphaned_info_files`, run by the thread started in `GUI_App::start_sync_user_preset`).
  ```cpp
  for (auto& e : fs::directory_iterator(dir)) ...            // Wrong on a background thread

  boost::system::error_code ec;                              // Right
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) ...
  if (ec) BOOST_LOG_TRIVIAL(warning) << "scan failed: " << ec.message();
  ```
  Cite: 05ed2e4dfa (`GUI_App::start_sync_user_preset` thread body, `GUI_App::scan_orphaned_info_files`);
  the same top-level `try/catch` is in `detail::run_off_thread_with_progress` (`PluginsDialog.hpp`).
- The wx-side counterpart: `wxTimer::Notify()` must not throw ([wxTimer](#wxtimer)), and a handler
  exception ends the session ([Exceptions](#exceptions-in-the-main-loop)).

**Lifetime.**
- A joinable thread must be joined before the objects it posts to die; a detached thread must reach
  the app only through `wxTheApp`-guarded calls and alive flags, and must not touch wx after
  `OnExit` (`wxGetApp()` dereferences null once `wxEntryCleanup` has run `SetInstance(nullptr)`,
  `src/common/init.cpp` `wxEntryCleanup`). `if (wxTheApp == nullptr) return;` before
  `wxTheApp->CallAfter` (as `detail::run_off_thread_with_progress` does) narrows but does not close
  that window, so long-running detached threads also need a stop signal checked in their loop.
- Orca joins its long-lived threads on the shutdown path: Jobs workers in `~BoostThreadWorker`,
  the slicing thread in `~BackgroundSlicingProcess`/`join_background_thread`, the preset-sync thread
  in `GUI_App::stop_sync_user_preset` (joined when `is_closing()`, otherwise detached), the
  3Dconnexion thread in `MainFrame::shutdown`.

**The `wxThread` family** (reading only — Orca uses `std::thread`/`boost::thread`):
- `wxThread` defaults to **detached** (`interface/wx/thread.h:1023`), `wxThreadHelper` to
  **joinable** (`interface/wx/thread.h:435`). Detached threads delete themselves: never `delete` them
  and never poll `IsRunning()`; "calling a routine on any running wxThread should be avoided if
  possible" — notify completion with `wxQueueEvent` instead. Joinable threads must be `Wait()`ed and
  deleted. `Delete()` only asks, and works only if `Entry()` polls `TestDestroy()`; `Kill()` is a last
  resort; `~wxThreadHelper` uses `Kill`, so `Wait()`/`Delete()` yourself (`interface/wx/thread.h:436-446`).
- `Entry()` must "not call any GUI function … rather use wxQueueEvent()" (`interface/wx/thread.h`
  sample, around line 369).
- Deriving from `wxThread` and `wxEvtHandler` to send messages *to* a worker "does not work at all";
  use `wxThreadHelper` or `wxMessageQueue<T>` (`docs/doxygen/overviews/thread.h:60-74`); since 3.3
  `wxMessageQueue::Post(T&&)` accepts move-only types (`interface/wx/msgqueue.h:90-97`).
- `wxTHREAD_WAIT_DEFAULT` is now `wxTHREAD_WAIT_BLOCK`; `wxTHREAD_WAIT_YIELD` "is dangerous" and "is
  only implemented in wxMSW and simply ignored under the other platforms" (`interface/wx/thread.h:648-675`),
  so the 3.3 default change (`docs/changes.txt:39-41`) only affects MSW code calling
  `Wait()`/`Delete()` from the GUI thread.
- `wxCRITICAL_SECTION(name)` declares a function-static critical section, which is initialised
  safely only if the function is first called from the main thread before other threads call it;
  otherwise make it a global (`interface/wx/thread.h:1762-1779`). Prefer `std::mutex`.
- `wxSecretStore` documents no thread-affinity notes in 3.3.2 (`interface/wx/secretstore.h`).

## The Jobs framework

`src/slic3r/GUI/Jobs/`: the standard way to run UI-initiated work (arrange, orient, fill bed, emboss,
send/print, texture bakes) off the main thread with progress and cancellation.

**Contract** (`Job.hpp`, `Worker.hpp`).
- `Job::process(Ctl&)` runs on the worker thread. `Job::finalize(bool canceled, std::exception_ptr&)`
  runs on the UI thread after `process` returned; an exception from `process` arrives in `eptr`.
  "Assign nullptr to this second argument before function return to prevent further action. Leaving
  it with a non-null value will result in rethrowing by the worker."
- `Job::Ctl` (call from `process`): `update_status(int, std::string)`, `was_canceled()`,
  `call_on_main_thread(fn) → std::future<void>`, `clear_percent()`, `show_error_info(…)`.
- `Worker`: "It is assumed that every method of this class is called from the same main thread."
  `push`, `is_idle`, `cancel` (ask the current job to stop), `cancel_all` (drop queued jobs and cancel
  the current one), `process_events` ("needs to be called continuously … e.g. in a wxIdle handler"),
  `wait_for_current_job(ms)`, `wait_for_idle(ms)`.
- Helpers: `queue_job(w, process_fn[, finish_fn])`, `queue_job(w, std::shared_ptr<Job>)`,
  `replace_job(w, …)` = `cancel_all()` + queue (non-blocking; the new job starts once the current
  one returns), `stop_current_job(w, ms)` = `cancel` + `wait_for_current_job`,
  `stop_queue(w, ms)` = `cancel_all` + `wait_for_idle`.

**`BoostThreadWorker`** (`BoostThreadWorker.cpp`) — one `boost::thread` (via `create_thread`) with an
input job queue and an output message queue (`Status`, `Finalize`, `MainThreadCall`):
- `run()` sets `m_canceled = false` at the start of **each** job, catches every exception from
  `process` into `eptr`, records `canceled`, and pushes a `Finalize` message.
- `process_events()` drains the output queue on the calling (UI) thread.
- `wait_for_current_job`/`wait_for_idle` **deliver messages synchronously inside the waiting call**,
  so `finalize` and `call_on_main_thread` bodies can run inside e.g. `Plater::reslice`'s
  `stop_queue(get_ui_job_worker(), 10000)`.
- Delivering `Finalize` calls `job->finalize(canceled, eptr)` and then `std::rethrow_exception(eptr)`
  if it is still set — on the UI thread, where it reaches `OnExceptionInMainLoop`.
- The destructor runs `cancel_all()`, `wait_for_idle(10 s)` (delivering pending messages), pushes a
  null job and joins for up to 10 s, logging if the join fails.

**`PlaterWorker<W>`** (`PlaterWorker.hpp`) wraps any worker for a `wxWindow*`:
- pumps `process_events()` from `wxEVT_IDLE` **and** `wxEVT_PAINT` handlers bound on that window
  through `EventGuard` (paint can arrive where idle doesn't run, e.g. inside an MSW modal resize loop);
- wraps `Ctl` so every `update_status`/`call_on_main_thread`/`clear_percent`/`show_error_info` is
  followed by `wxWakeUpIdle()`;
- sets the busy cursor for the job's duration (`CursorSetterRAII`, which marshals
  `wxBeginBusyCursor`/`wxEndBusyCursor` through `call_on_main_thread`);
- logs process/finalize timing, and turns a `std::exception` left in `eptr` into
  `show_error(window, …)` before clearing it; non-`std` exceptions still propagate.

The plater's single worker is `Plater::get_ui_job_worker()` (a
`PlaterWorker<BoostThreadWorker>` named "ui_worker", reporting through
`NotificationProgressIndicator`, whose cancel button calls `worker.cancel()`). Dialogs with their own
progress own one: `m_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, m_status_bar, "send_worker")`
(`SelectMachineDialog`, `SendToPrinterDialog`; `DownloadProgressDialog`, `BindMachineDialog` and
`OAuthDialog` the same with their own worker names, `OAuthDialog` with a null progress indicator).

**Usage** — the shape of `ArrangeJob`/`OrientJob`/`FillBedJob`:
```cpp
void MyJob::process(Ctl& ctl) {
    ctl.call_on_main_thread([this] { prepare(); }).wait();   // snapshot the model on the UI thread
    for (...) {
        if (ctl.was_canceled()) return;
        /* work on the snapshot only */
        ctl.update_status(pct, _u8L("Working"));
    }
}
void MyJob::finalize(bool canceled, std::exception_ptr& eptr) {
    try { if (eptr) std::rethrow_exception(eptr); }
    catch (libnest2d::GeometryException&) { show_error(m_plater, _L("…")); eptr = nullptr; } // handled
    catch (...) { eptr = std::current_exception(); }                     // left for PlaterWorker
    if (canceled || eptr) return;              // never apply partial results
    /* apply the result to the model */        // UI thread: safe to touch Plater/canvas
}
replace_job(plater->get_ui_job_worker(), std::make_unique<MyJob>());
```
State written in `process` and read in `finalize` needs no lock: `finalize` is delivered only after
`process` returned.

**Pitfalls.**
- **Rule:** Drop queued work with `cancel_all()`/`replace_job()`, not `cancel()`. **Why:** `cancel()`
  only sets the flag; `run()` resets it when the next job starts, so queued jobs still run and a
  `cancel()` with no job running is a no-op. Cite: `BoostThreadWorker::run`, `Worker.hpp`.
- **Rule:** Clear `eptr` for every exception `finalize` handles. **Why:** a non-null `eptr` is
  rethrown on the UI thread after `finalize`; `PlaterWorker` converts only `std::exception`s, anything
  else reaches `OnExceptionInMainLoop` and ends the session. Cite: `BoostThreadWorker::WorkerMessage::deliver`.
- **Rule:** Touch UI only in `finalize` or `call_on_main_thread`; `process` works on a snapshot.
  Cite: `Job.hpp` comments; `ArrangeJob::process`.
- **Rule:** Stop a dialog-owned worker (`stop_queue(*m_worker, ms)` or `m_worker.reset()`) at the
  start of the owner's close path or destructor, before other members die. **Why:** the worker's
  destructor delivers pending `Finalize`/`MainThreadCall` messages synchronously, so job callbacks
  capturing the dialog run inside its destruction. Cite: `BoostThreadWorker::~BoostThreadWorker`.
- **Rule:** Expect `finalize` to run inside `wait_for_*`/`stop_queue` calls, not only from idle.
  **Why:** synchronous delivery; code after `stop_queue` sees the model already modified by the
  finalized job. Cite: `BoostThreadWorker::wait_for_idle`.

## BackgroundSlicingProcess

The slicing/export thread (`BackgroundSlicingProcess.cpp/.hpp`) talks to the plater three ways:
- **Completion and status events**: `SlicingStatusEvent` and `SlicingProcessCompletedEvent` are
  `wxEvent` subclasses (the latter carries the exception as `std::exception_ptr`, which `Clone()`
  copies safely), queued with `wxQueueEvent(wxGetApp().mainframe->m_plater, evt.Clone())`;
  export-began/finished events are `new wxCommandEvent(id)` (+ `SetString(path)`) passed to
  `wxQueueEvent`. Posting to the plater from a worker is safe here only because the thread is joined
  in `~BackgroundSlicingProcess`/`join_background_thread` before the plater dies.
- **Synchronous UI calls** (thumbnail rendering): `execute_ui_task(fn)` — see
  [Blocking round-trips](#blocking-round-trips). It plans a `UITask` only if cancellation is not in
  progress (`m_mutex.try_lock()` and `!m_print->canceled()`), posts `m_plater->CallAfter`, and waits
  for `Finished` or `Canceled`; the queued lambda runs `fn` only while the task is still `Planned`.
  Only one UI task can be planned at a time because the slicing thread blocks on it.
- **Stopping**: `stop()` and `stop_internal()` run on the UI thread, call `cancel_ui_task(m_ui_task)`
  first, then cancel the print and wait for `STATE_CANCELED`. `stop_internal()` (called by
  `Print::apply()` through the cancel callback) "shall not trigger any UI update through the
  wxWidgets event".

## wxTimer

**Contract** (`interface/wx/timer.h`).
- Three ways to receive notifications (`interface/wx/timer.h:24-39`): override `Notify()`; or
  `SetOwner(handler, id)` / the `(owner, id)` ctor and `Bind(wxEVT_TIMER)` on the owner; or
  default-construct and bind on the timer itself ("the timer object will be its own owner object,
  since it is derived from wxEvtHandler").
- "In any case, you must start the timer with wxTimer::Start() after constructing it before it
  actually starts sending notifications. It can be stopped later with wxTimer::Stop()." "A timer can
  only be used from the main thread." (`interface/wx/timer.h:37-41`)
- Destructor: "Stops the timer if it is running" (`interface/wx/timer.h:65`) — the documented safety net. Events
  go to the owner, so the timer must be stopped or destroyed before its owner handler dies; the
  member-timer-with-`this`-owner layout in the class docs is the sanctioned one.
- `Start(ms = -1, oneShot = false)` returns false "if the timer could not be started (in MS Windows
  timers are a limited resource)"; `-1` reuses the previous interval; a running timer "will be
  stopped by this method before restarting it" (`interface/wx/timer.h:119-135`).
- `StartOnce(ms)` wraps `Start(ms, wxTIMER_ONE_SHOT)` and is the preferred one-shot ("Alternatively,
  and preferably, call StartOnce()"); one-shot timers stop after their single notification
  (`interface/wx/timer.h:129-144`).
- `Notify()`: "throwing exceptions from this method is currently not supported, use event-based
  timer handling" if the handler can throw (`interface/wx/timer.h:104-106`).
- `wxTimerRunner`: RAII, "Starts the timer in its ctor, stops in the dtor" (`interface/wx/timer.h:153-165`).

**Source facts.**
- **Default id is not `wxID_ANY`.** `SetOwner(owner, wxID_ANY)` assigns a fresh `wxNewId()`
  (`src/common/timerimpl.cpp` `wxTimerImpl::SetOwner`). An id-less `Bind(wxEVT_TIMER, h)` on the owner
  catches **every** timer that owner owns; calling `SetOwner(this)` again gives the timer a **new**
  id, so a `Bind(…, timer.GetId())` made earlier stops matching.
- **Delivery is synchronous through a raw owner pointer**: `wxTimerImpl::SendEvent` calls
  `m_owner->SafelyProcessEvent` (`src/common/timerimpl.cpp`; `include/wx/private/timer.h:62`), and
  nothing clears the pointer when the owner dies. A member `wxTimer m_timer{this}` is safe because
  members are destroyed before the owner's `wxEvtHandler` base.
- **macOS rejects 0 ms**: `wxCHECK_MSG(m_milli > 0, false, …)` (`src/osx/core/timer.cpp:74`), so
  `StartOnce(0)` returns false and never fires — and so does a first `Start()`/`StartOnce()` with no
  interval, since the stored interval starts at 0 (`src/common/timerimpl.cpp` `wxTimerImpl` ctor).
- **Needs the app**: `wxTimer::Init` gets its implementation from `wxApp` traits and returns silently
  without them (`src/common/timercmn.cpp:55-62`); a global or static timer constructed before the
  `wxApp` object has no implementation and every call on it is a silent no-op.
- **Started off the main thread it is broken, silently**: the main-thread assert in
  `wxTimerImpl::Start` is compiled out (its comment: "under MSW timers only work when they're
  started from the main thread", `src/common/timerimpl.cpp:53-58`); on macOS the `CFRunLoopTimer` is
  attached to `CFRunLoopGetCurrent()` of the calling thread and never fires
  (`src/osx/core/timer.cpp:94-96`); on GTK `g_timeout_add` registers it with the default GLib
  context (`src/gtk/timer.cpp:59`), so it fires on the main thread while the worker races the
  timer's state.

**Platforms.**

| | Implementation | Notes |
|---|---|---|
| MSW | `::SetTimer` on a hidden window (`src/msw/timer.cpp`) | `Start()` can fail ("Couldn't create a timer", returns false) — check it; one-shot timer events are lost inside a `YieldFor` without `wxEVT_CATEGORY_TIMER` (`interface/wx/event.h:52-59`, `interface/wx/evtloop.h:265-268`), which includes the time a `wxProgressDialog` is shown (`interface/wx/progdlg.h:34-39`). **[source]** The masked yield removes an unwanted `WM_TIMER` from the queue and never re-posts it (`src/msw/evtloop.cpp` `wxGUIEventLoop::DoYieldFor`); the `::SetTimer` stays armed until the first delivered tick (`src/msw/timer.cpp` `wxProcessTimer`), so in practice the one-shot is late by whole intervals, or lost while yields keep discarding its ticks. This bites the generic dialog and Orca's `ProgressDialog` (`UI\|USER_INPUT`), while the native MSW `wxProgressDialog` yields `TIMER` too (`DispatchEvents`) |
| macOS | `CFRunLoopTimer` in `kCFRunLoopCommonModes` (`src/osx/core/timer.cpp:96`) | fires during live resize, menu tracking and native modals **[source]**; interval must be > 0 |
| GTK | `g_timeout_add` at default priority; the callback takes `wxGDKThreadsLock` and calls `WakeUpIdle()` after `Notify()` (`src/gtk/timer.cpp:26-59`) | a timer that is always due runs ahead of repaints and of posted events (pending events run from wx's `G_PRIORITY_LOW` idle source, see [When queued and idle work runs](#when-queued-and-idle-work-runs)); a masked `YieldFor` filters only GDK events, so the timer fires inside it whatever the mask **[source]** (`src/gtk/evtloop.cpp` `wxgtk_main_do_event`) |

**OrcaSlicer.**
- Conventions: a member `wxTimer` owned by the window it serves, or a self-owned timer bound on
  itself (`m_timer.Bind(wxEVT_TIMER, …)`, `IdleScheduler`; `m_timer_set_color`/`m_fps_overlay_timer`
  in `GLCanvas3D`). Start on page/window show, **stop on hide**, stop before destruction.
- `MonitorPanel` keeps a heap `wxTimer` with `SetOwner(this)`, started in `MonitorPanel::init_timer`
  and `MonitorPanel::Show(true)`, stopped in `Show(false)`, stopped and deleted in `~MonitorPanel`.
  Its `Bind(wxEVT_TIMER, &MonitorPanel::on_timer, this)` has no id, which is why re-`SetOwner` in
  `Show` keeps working — and why a second timer on that panel would also land in `on_timer`.
- Debouncing uses `StartOnce` on dedicated members: `GLCanvas3D::m_render_timer`,
  `m_timer_set_color`, `m_fps_overlay_timer`.
- `GLCanvas3D::RenderTimer`/`ToolbarHighlighterTimer`/`GizmoHighlighterTimer` override `Notify()` to
  `wxPostEvent` a distinct event type (`EVT_GLCANVAS_RENDER_TIMER`, …) to the owner, which sidesteps
  owner/id collisions entirely.
- `IdleScheduler` is the sanctioned timer-driven slicer of background UI work
  ([When queued and idle work runs](#when-queued-and-idle-work-runs)).

**Pitfalls.**
- **Rule:** A `wxTimer` must not outlive the handler that receives its events: make it a member of
  (or owned by) the window it notifies, or stop and delete it before that handler dies. Touch it only
  on the main thread. Destroy heap-allocated modeless frames and dialogs with `Destroy()`, never
  `delete`; stack-allocated modal dialogs are fine.
  **Why:** the next tick calls `m_owner->SafelyProcessEvent` on freed memory. The destructor already
  stops the timer, so `Stop(); delete timer;` is belt-and-braces; ordering is what matters. A raw
  `delete` of a TLW bypasses the `wxPendingDelete` queue that lets in-flight events drain, and a
  child dialog is deleted immediately inside its parent's destructor, so a pointer to a child dialog
  dies with the parent (`references/windows-dialogs.md`).
  ```cpp
  // Shape of detail::run_off_thread_with_progress: worker done → back on the UI thread
  wxTheApp->CallAfter([alive, progress, timer, on_finish, restore] {
      timer->Stop(); delete timer;                 // timer first, on the main thread
      if (!alive || alive->load(std::memory_order_acquire)) {
          progress->Destroy();                     // child of the host: only while the host lives
          if (restore) restore();                  // e.g. re-raise the host on macOS
          on_finish();
      }
  });
  ```
  Cite: `detail::run_off_thread_with_progress` (`PluginsDialog.hpp`; `alive` may be null
  for host-less callers such as `install_local_plugin_package`); 0a0d59b76b added the macOS
  re-raise step to this completion sequence.
- **Rule:** With two timers on one owner, bind with `timer.GetId()`, bind on each timer, or check
  `evt.GetTimer()`. **Why:** default ids are fresh `wxNewId()`s and an id-less bind catches all.
- **Rule:** Never `StartOnce(0)` "to run on the next iteration" — use `CallAfter`. **Why:** macOS
  rejects 0 ms silently. `IdleScheduler` uses a 5 ms "next slice" for this reason.
- **Rule:** Don't create timers as globals/statics or start them from workers. **Why:** no
  implementation before the app exists; a worker-started timer never fires on macOS and races the
  main thread on GTK; no diagnostic in Orca.

## When queued and idle work runs

**Order.** Pending (queued) events run before idle (`src/common/evtloopcmn.cpp`
`wxEventLoopManual::DoRunLoop`; macOS `src/osx/core/evtloop_cf.cpp` `CommonModeObserverCallBack`).
`wxAppConsoleBase::ProcessPendingEvents` loops **until no handler has pending events**, including
events queued while it runs (`src/common/appbase.cpp:561-601`) **[source]**: a `CallAfter` that
re-posts itself never lets native input or paint in. Order is FIFO only per target handler
(`references/events.md` §Posting).

**Idle** (contract details in `references/events.md` §Idle events): sent once when the loop becomes
idle, not continuously; a stream needs `RequestMore()` or periodic `wxWakeUpIdle()`, "but note that
both of these approaches (and especially the first one) increase the system load"
(`interface/wx/event.h:4382-4395`). Call `wxWakeUpIdle()` from a worker after changing state an idle
handler consumes (`GLTexture::Compressor::compress`, `PlaterWorker`'s `WakeUpCtl`). The docs say
children of hidden windows get no idle events (`interface/wx/event.h:4396-4401`), but
`wxWindowBase::SendIdleEvents` recurses into every child without a visibility check
(`src/common/wincmn.cpp:2783-2808`) **[source]**; TLWs pending deletion are skipped
(`src/common/appcmn.cpp:408-429`). Idle processing also flushes `wxLog` and deletes `Destroy()`ed TLWs
and `ScheduleForDestruction` objects (`src/common/appbase.cpp` `ProcessIdle`, `DeletePendingObjects`).

**Platforms** **[source]** (per-port table with cites: `references/events.md` §7 Posting and queueing):
MSW runs pending events — not idle — inside native modal loops (menus, move/resize, common dialogs)
through a `WH_GETMESSAGE` hook; macOS runs them from run-loop observers and **pauses** pending events
and idle while a native `wxMessageDialog`/`wxFileDialog`/`wxDirDialog` is modal; GTK (GTK3 by default,
X11 and Wayland) runs both from one `G_PRIORITY_LOW` idle source that timers, redraws and input floods
outrank, which a masked `YieldFor` removes ([event categories](#event-categories-and-yields)) and which
`RequestMore()` keeps installed — a busy loop.

Orca's `MessageDialog` family is generic (`DPIDialog`) and doesn't pause anything; native
`wxMessageBox`/`wxFileDialog` do.

**Visible progress during blocking main-thread work** needs a run-loop turn: `wxWindowGTK::Update()`
returns immediately on Wayland (`src/gtk/window.cpp` `wxWindowGTK::Update`, #25036) and
`wxNonOwnedWindow::Update` on macOS is throttled to about 30 Hz (`src/osx/nonownedwnd_osx.cpp:484-489`),
so `Refresh(); Update();` mid-computation paints nothing or late. Move the work off the main thread
(`references/painting-custom-widgets.md` §Invalidation).

**Idle loops — OrcaSlicer.**
- **`IdleScheduler`** (`IdleScheduler.hpp/.cpp`, `MainFrame::m_idle`, design in
  `docs/HLSD/deferred-page-construction.md`) runs deferred page prebuilds: a self-owned `wxTimer`
  ticks every 250 ms; a slice starts only after 500 ms without input (`GUI_App::input_idle_ms()`,
  stamped by `GUI_App::FilterEvent` for non-command user-input events and main-window resizes) and,
  on Windows, while no input other than mouse moves is queued; slices are 40 ms; the next slice is
  `StartOnce(5)` (a timer message, so paint, timers and input queued meanwhile run first; GTK runs a
  due timer ahead of repaints and posted events; macOS rejects 0 ms); it skips while
  `wxEventLoopBase::GetActive()->IsYielding()` and guards re-entry with `m_in_slice`;
  `MainFrame::shutdown` calls `m_idle.stop()`. Use it — not `wxEVT_IDLE` + `RequestMore()` or
  self-reposting `CallAfter`s — for background UI construction.
- `GLCanvas3D::on_idle` calls `RequestMore()` only while an extra frame is pending (FPS cap, ImGui
  settling), which bounds the busy loop.
- The app-level idle handler (bound at the end of `GUI_App::on_init_inner`) runs `post_init()` once
  and then saves `app_config` whenever it is dirty.
- `GUI_App::FilterEvent` runs for every event in the process: keep it as cheap as it is (a timestamp,
  then `Event_Skip`) (`interface/wx/eventfilter.h:18-20`).

## Yields and nested event loops

### Yields

**Contract.**
- `wxApp::Yield` uses the active loop; with none running, "it creates a temporary event loop … useful
  to process pending messages during the program startup". "Use extreme caution … it can result in
  unexpected reentrances." (`interface/wx/app.h:226-238`). `wxYield()` does nothing without an app
  (`src/common/utilscmn.cpp` `wxYield`).
- `SafeYield(win, onlyIfNeeded)` disables all windows except `win`, yields, re-enables
  (`interface/wx/app.h:1085-1094`). **[source]** Unlike `Yield`, it returns false and does nothing
  when no loop is active (`src/common/appcmn.cpp:384-391`) — useless in `OnInit`.
- `YieldFor(mask)` processes only the given categories (UI=1, USER_INPUT=2, SOCKET=4, TIMER=8,
  THREAD=16; `interface/wx/event.h:25-76`). "only wxMSW and wxGTK do support selective yield of native
  events … wxWidgets events posted using … QueueEvent are instead selectively processed by all ports";
  "Under wxMSW, if eventsToProcess doesn't include wxEVT_CATEGORY_TIMER, events from one-off timers may
  be lost" (`interface/wx/evtloop.h:250-272`). **[source]** wxGTK processes no posted event at all
  inside a masked yield ([event categories](#event-categories-and-yields)).
- The overview: if `YieldFor` is used "directly or indirectly (e.g. through wxProgressDialog)", expect
  re-entrancy and out-of-order processing (`docs/doxygen/overviews/thread.h:79-85`).

**Source facts** (`src/common/evtloopcmn.cpp:128-192`): `YieldFor` from a non-main thread returns
false immediately; `wxLog` is suspended during the yield; exceptions stored during the yield are
rethrown afterwards (`RethrowStoredException`); `Yield(onlyIfNeeded=true)` returns false if already
yielding; nested yields are allowed. A full yield (`wxEVT_CATEGORY_ALL`) runs pending events **and**
one idle pass, so a `Destroy()`ed TLW can be freed inside the yield, not only at the next main-loop
idle.

**What runs inside a yield or modal:** every queued `CallAfter`, timer callback and worker event whose
category the mask allows (on GTK a masked yield runs no queued event but does run timers),
`Destroy()`ed-TLW deletion on full yields, and user clicks unless input is disabled. A lambda queued before a modal opens can run while the caller is still inside `ShowModal()`.
Hidden yields count too: `wxExecute(…, wxEXEC_SYNC)` "will call wxYield()" while it waits (all
windows disabled unless `wxEXEC_NODISABLE`; `wxEXEC_BLOCK` adds `wxEXEC_NOEVENTS` and yields
nothing) (`interface/wx/utils.h:1203-1212, 1243-1248`; see `references/strings-i18n-files.md`).

### Nested event loops

**Contract.**
- Loops form a stack; only the top one is active. `Exit()` works only on the running loop;
  `ScheduleExit()` exits a loop "as soon as all its nested loops exit" (`interface/wx/evtloop.h:25-36,
  104-130`); it requires the loop to be inside `Run()` (`IsInsideRun()`,
  `src/common/evtloopcmn.cpp:98-107`). `IsRunning()` is `GetActive() == this`
  (`include/wx/evtloop.h:366`), false while a nested loop is active.
- `ShowModal()` "creates a temporary event loop which takes precedence over the application's main
  event loop … This also results in a call to wxApp::ProcessPendingEvents()" (`interface/wx/dialog.h:608-618`).
- `wxApp::ExitMainLoop` is documented as calling `Exit()` on the active loop (`interface/wx/app.h:76-82`);
  **[source]** it exits `m_mainLoop` only if that is the running loop (`src/common/appbase.cpp:396-404`),
  so with a modal open it does nothing. Use `GetMainLoop()->ScheduleExit()` or close the top window.
- `wxEventLoopActivator` swaps the active loop RAII-style (`interface/wx/evtloop.h:298-330`).
  `wxEventLoopBase::GetActive()` "may return NULL if there is no active event loop (e.g. during
  application startup or shutdown)" (`interface/wx/evtloop.h:47-52`): guard it before calling
  `GetActive()->YieldFor(…)`.

**OrcaSlicer.**
- `detail::run_wait_with_progress` (`PluginsDialog.hpp`) blocks in a local `wxEventLoop loop; loop.Run();`
  that the worker's completion `CallAfter` exits, with a `finished` flag for a completion that lands
  before `Run()`. A completion can arrive while another modal is stacked on that loop, so the exit
  call must be `ScheduleExit()`: `Exit()` then fails its `IsRunning()` check and returns silently
  (`src/common/evtloopcmn.cpp:91-96`), and an `if (loop.IsRunning()) loop.Exit();` guard skips it
  just as silently — either way the local loop keeps running after the stacked modal closes.
- `GUI_App::drain_pending_events(ms)` (`ProcessPendingEvents` + up to 20 `wxYield`s + 50 ms sleeps)
  belongs to `GUI_App::hot_reload_network_plugin`, not to shutdown: clear agent callbacks → drain
  200 ms → stop discovery/disconnect → `wait_for_network_idle(500)` → drain 100 ms → delete the agent →
  drain → unload the module, so callbacks the old agent already queued run before the agent and its
  module go away. It runs under `wxBusyCursor` + `wxBusyInfo` + `wxWindowDisabler`;
  reentrant by design.

**Pitfalls.**
- **Rule:** Don't `wxYield()` in a handler to let the UI repaint. **Why:** it runs every pending
  `CallAfter`, timer and click re-entrantly (and may free `Destroy()`ed windows); paint may still not
  happen on Wayland. Use a worker or a Job. Never yield from inside a `CallAfter` body that can be
  re-entered.
- **Rule:** Exit a nested loop from deferred code with `ScheduleExit()`.
  ```cpp
  if (loop.IsRunning()) loop.Exit();   // Wrong: silently skipped while a modal is stacked on top
  loop.ScheduleExit();                 // Right: exits once the nested loops have exited
  ```
  Cite: `interface/wx/evtloop.h:104-130`.
- **Rule:** Don't rely on `wxSafeYield` in `OnInit`. **Why:** no active loop → no-op; `wxYield` /
  `wxApp::Yield` create a temporary loop.

## Progress and busy indicators

### wxProgressDialog

**Contract** (`interface/wx/progdlg.h`).
- It internally calls `YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT)`, which can cause
  re-entrancy and out-of-order processing; use `wxThreadEvent` for thread messages; one-shot timers
  firing while it is shown are discarded on wxMSW, so don't start them before showing it
  (`interface/wx/progdlg.h:29-39`).
- Create it on the stack like a modal dialog; created from `OnInit`, it must be destroyed before the
  main loop starts (`interface/wx/progdlg.h:41-59`).
- `Update(value, msg, &skip)` "Returns true unless the Cancel button has been pressed"; on false
  either destroy the dialog or `Resume()`. At `value == maximum`: with `wxPD_AUTO_HIDE` it hides and
  returns; **without it the dialog becomes modal and `Update` doesn't return until the user dismisses
  it** (`interface/wx/progdlg.h:208-229`).
- `Pulse()` is indeterminate mode (`interface/wx/progdlg.h:166`); `WasCancelled()` lets other code poll (`interface/wx/progdlg.h:195`).
- `wxPD_APP_MODAL` disables all windows (`interface/wx/progdlg.h:62-66`); a non-null parent is always disabled; a
  null parent without `wxPD_APP_MODAL` needs your own re-entrancy guard (`interface/wx/progdlg.h:112-117`).

**Platforms.** On MSW `wxProgressDialog` is native: a TaskDialog run by a
`wxProgressDialogTaskRunner` **wxThread**, yielding `ALL & ~USER_INPUT` (TIMER and THREAD included)
(`src/msw/progdlg.cpp` `DispatchEvents`) **[source]**; anything based on `TaskDialog()` has no dark
mode (`interface/wx/app.h:1436-1440`). On macOS and GTK it is `wxGenericProgressDialog`, which creates
a temporary event loop if none is active (`src/generic/progdlgg.cpp:360-366`). On MSW (native
dialog or Orca's generic one) and macOS, `CallAfter` bodies run inside `Update()`/`Pulse()`; on GTK
the masked yield runs no queued event, so they wait until control returns to an event loop
**[source]** ([event categories](#event-categories-and-yields)).

**OrcaSlicer.**
- `Slic3r::GUI::ProgressDialog` (`Widgets/ProgressDialog.hpp`) is a themed fork of
  `wxGenericProgressDialog`: same `YieldFor(UI | USER_INPUT)` semantics, temporary event loop when
  none is active, generic on every platform (so no TaskDialog dark-mode gap).
- `detail::run_off_thread_with_progress` (`PluginsDialog.hpp`, wrapped by
  `PluginsDialog::run_with_dialog`/`run_with_dialog_wait`): a heap wx `wxProgressDialog` (the native
  TaskDialog on MSW, the generic dialog on macOS and GTK) pulsed by a
  self-owned 100 ms `wxTimer`, the work on a detached `std::thread` with a top-level `try/catch`, then
  the completion `CallAfter` shown in [wxTimer](#wxtimer) pitfalls.
- Job progress goes through `NotificationProgressIndicator` (plater) or the dialog's status bar
  (dialog-owned `PlaterWorker`), driven from `deliver` on the UI thread — no yields.

### Busy indicators

- `wxBusyCursor` is RAII around `wxBeginBusyCursor`/`wxEndBusyCursor`, which nest by counter
  (`interface/wx/busycursor.h`) — main thread only.
- `wxBusyInfo` shows a stay-on-top window for its scope; "Normally the main thread should always return
  to the main loop … hence this class shouldn't be needed". To refresh it, call `wxTheApp->Yield()` with
  a `wxWindowDisabler` created **first** (`interface/wx/busyinfo.h:13-15, 59-78`).
- `wxWindowDisabler(winToSkip)` disables shown TLWs; on MSW a skipped window that appears in the
  taskbar lets the user close the app from the taskbar menu — give it `wxFRAME_NO_TASKBAR` or
  `wxFRAME_TOOL_WINDOW` (`interface/wx/utils.h:59-110`).

**Pitfalls.**
- **Rule:** Pass `wxPD_AUTO_HIDE` if the dialog is closed programmatically. **Why:** `Update(max)`
  without it blocks modally until the user dismisses it. Cite: `src/generic/progdlgg.cpp`
  `wxGenericProgressDialog::Update` (`ShowModal()` at maximum).
- **Rule:** Don't `Refresh(); Update();` to show progress mid-computation. **Why:** no paint on
  Wayland, throttled on macOS. Move the work off the main thread.

## Application lifecycle

### Startup

**Contract.**
- `OnInit`: return false to exit; `OnExit` is then **not** called; call the base `wxApp::OnInit()` if
  wx command-line parsing is wanted (`interface/wx/app.h:387-418`).
- Windows shown before the main loop don't end the app when closed (`docs/doxygen/overviews/app.h:79-82`).
- `OnRun` runs the main loop until `ExitMainLoop` or the last TLW closes; `SetExitOnFrameDelete(false)`
  keeps the app alive (`interface/wx/app.h:433-452, 1198-1208`).
- macOS **[source]**: `wxApp::CallOnInit` first runs `[NSApp run]` on a dummy event, so open-file
  requests can arrive before `OnInit`; wx stores them with `OSXStoreOpenFiles` and calls
  `MacOpenFiles` only after `OnInit` returned true (`src/osx/cocoa/utils.mm:135-150, 420-450`).
  Orca overrides `GUI_App::OSXStoreOpenFiles` (it forwards to the base).

**OrcaSlicer.**
- `GUI_Run` (`GUI_Init.cpp`) does `new GUI_App()`, runs the single-instance check, `SetInstance`, and
  calls `wxEntry` with only `argv[0]` (real arguments travel in `GUI_App::init_params`); it catches
  `Slic3r::Exception`/`std::exception` thrown outside wx's own catch (e.g. from the `GUI_App`
  constructor) and shows a fatal message box.
- `DECLARE_APP(GUI_App)`/`IMPLEMENT_APP(GUI_App)` sit inside `namespace Slic3r::GUI` (the wx overview
  wants global scope, `docs/doxygen/overviews/app.h:65-67`; harmless here because `GUI_Run` drives
  `wxEntry` itself), so the accessor is `Slic3r::GUI::wxGetApp()` — qualify it as `GUI::wxGetApp()`
  outside that namespace.
- `GUI_App::OnInit` wraps `on_init_inner()` in `catch (const std::exception&)` →
  `generic_exception_handle()`, return false. That handler terminates or rethrows, so the
  `return false` is never reached: a startup `std::exception` leaves `OnInit` and ends in wx's
  `wxSafeCall` → `OnUnhandledException` (fatal exit code), not in `GUI_Run`'s catch **[source]**
  (`src/common/init.cpp:550-568`). `on_init_inner` runs before the main loop exists: it shows the
  splash, builds `MainFrame`, shows it, destroys the splash, and binds the app idle handler last (full
  order: `references/orca-architecture.md`).
- `post_init()` runs from the first app idle, not from `OnInit`: deferred startup work that needs a
  running loop (opening command-line files, the config wizard via `CallAfter`, page prebuilds through
  `MainFrame::prebuild_pages_when_idle()`) belongs there.
- `wxEVT_QUERY_END_SESSION` (bound on the app in `on_init_inner`): sends a vetoable close to the main
  frame and `EndModal(wxID_ABORT)` to every dialog on `dialogStack`. wx raises it on MSW and, on macOS,
  from `applicationShouldTerminate:` (`src/osx/carbon/app.cpp` `wxApp::OSXOnShouldTerminate`)
  **[source]**; not on GTK.

### Shutdown

**wx order** **[source]** (`wxEntry` → `CallOnExit` → `wxEntryCleanup`):
1. The main loop returns.
2. `CallOnExit`: `DoDelayedCleanup()` deletes pending objects **and `delete`s every remaining TLW**
   (`wxAppBase::DeleteAllTLWs`, `src/common/appcmn.cpp:129-146`).
3. `OnExit()` runs — "called after destroying all application windows and controls, but before
   wxWidgets cleanup … call [base] at the end" (`interface/wx/app.h:358-371`).
4. `DoDelayedCleanup()` again; `wxConfig` is no longer created on demand (`src/common/appbase.cpp:303-323`).
5. `wxEntryCleanup`: `app->CleanUp()`, `wxApp::SetInstance(nullptr)` — **`wxTheApp` is null from
   here** — then `delete app`, whose `~wxEvtHandler` **deletes still-queued `CallAfter` events
   without running them**, then module cleanup (`src/common/init.cpp` `wxEntryCleanup`).

Consequences: in `OnExit` all windows are gone; pending app `CallAfter`s never run after the loop
exits; in `~GUI_App` `wxTheApp` is null. Don't let wx objects die in the app destructor — delete them
in `OnExit` (`docs/doxygen/overviews/app.h:84-126`).

**OrcaSlicer close sequence.**
1. `MainFrame`'s `wxEVT_CLOSE_WINDOW` handler may veto (gizmo in editing mode,
   `Plater::close_with_confirm` project/preset prompts, `check_print_host_queue`).
2. Otherwise: `MarkdownTip::ExitTip()`, `wxGetApp().set_closing(true)`, `m_plater->reset()`,
   `MainFrame::shutdown()`, `event.Skip()` (wx then `Destroy()`s the frame — or, for a vetoable close
   while a modal dialog is open, vetoes it after this teardown: `references/windows-dialogs.md` §2).
3. `MainFrame::shutdown()`: `m_idle.stop()`; plugin pages and dock panes removed;
   `Slic3r::set_backup_callback(nullptr)`; `get_ui_job_worker().cancel_all()`;
   `unbind_canvas_event_handlers()` (on macOS Cmd+Q delivers a mouse event afterwards);
   `reset_canvas_volumes()`; `Show(false)` (no paint events into dying windows); 3Dconnexion shutdown;
   other-instance handler shutdown; save `app_config` if dirty; clear tab lists; `GUI_App::shutdown()`.
4. `GUI_App::shutdown()`: removable-drive manager shutdown; destroy the login dialog; then — unless
   recreating the GUI — stop the http server, `set_closing(true)`, mark the plugin manager shutting
   down, clear the printer-agent cache.
5. `GUI_App::OnExit` (windows already deleted by wx): stop the http server and preset sync, delete
   `DeviceManager`/`UserManager`, clear the agent cache, delete the agent, then `wxApp::OnExit()`.

There is **no** `CallAfter` drain at shutdown: queued app `CallAfter`s are discarded with the app, and
`is_closing()` makes any that still run inert. Anything a callback might touch must be stopped in
steps 3–4, not in `OnExit`.

**Pitfalls.**
- **Rule:** Never touch windows in `OnExit` or `~GUI_App`. **Why:** wx deleted every TLW before
  `OnExit`; `wxTheApp` is null in the destructor.
- **Rule:** A new worker, timer or callback registered on the app or main frame gets a stop in
  `MainFrame::shutdown`/`GUI_App::shutdown`. **Why:** the frame is hidden and the loop is about to end;
  anything still firing reaches half-destroyed windows (the canvas-unbind and Mouse3D steps exist for
  this).

## Exceptions in the main loop

**Contract.** `OnExceptionInMainLoop()` returns true to continue, false to exit; the default exits,
except on MSW where it shows a dialog; if it rethrows and the exception can't be stored, the program
terminates after `OnUnhandledException()` (`interface/wx/app.h:466-512`), which "should not throw"
(`interface/wx/app.h:515-533`). With C++11 `StoreCurrentException` uses `std::current_exception`
(`interface/wx/app.h:555-645`). `wxHandleFatalExceptions()` enables `OnFatalException` for crashes
(MSVC SEH on MSW; `interface/wx/app.h:1636-1653`). `wxExit()` is for emergencies only
(`interface/wx/app.h:1758-1767`).

**Mechanism [source]** (`src/common/event.cpp:1688-1749` `wxEvtHandler::WXConsumeException`): when
`OnExceptionInMainLoop` throws, the active loop `Exit()`s (unless yielding), the exception is stored
and rethrown by `RethrowStoredException` once control is back in wx code (end of a loop's `Run`, end of
`YieldFor`) — it unwinds out of `ShowModal()` and the loop. Exceptions must never cross native
callbacks ("can't propagate through the C GTK+ code").

**OrcaSlicer policy.** `GUI_App::OnExceptionInMainLoop` → `generic_exception_handle()`:
`std::bad_alloc` and `boost::io::bad_format_string` show a `wxMessageBox` and `std::terminate()`; any
other `std::exception` is logged with `wxLogError` and **rethrown** (non-`std` exceptions escape it
unlogged), so per
`WXConsumeException` the active loop exits and the exception is rethrown out of the main loop. `wxEntry`
runs `OnRun` inside `wxSafeCall` (`src/common/init.cpp:550-568`, `include/wx/private/safecall.h`):
`CallOnExit` runs during unwinding, the default `OnUnhandledException` prints "Unhandled …;
terminating the application", and `wxEntry` returns the fatal exit code **[source]** (255 on every
platform since 3.3, including MSVC builds, `docs/changes.txt:17-19`). An uncaught
exception in any handler, `CallAfter` body, timer handler or job `finalize` therefore ends the
session. Catch inside handlers; workers catch everything themselves.

**Debugging.** Set the system option `catch-unhandled-exceptions` to 0 (environment variable
`wx_catch_unhandled_exceptions=0`) to let the exception abort at the throw site with a usable backtrace
instead of being caught by `wxSafeCall` — which also wraps every `SafelyProcessEvent` and event-loop
run, so the option covers handler exceptions too (`include/wx/private/safecall.h`). Set from code, it
must be set "very early during program startup, within the constructor of the wxApp derivative"
(`interface/wx/sysopt.h:16-23, 40-49`; 3.3.0+).

## Splash screen

**Contract.** "Show it in application initialisation, and then either explicitly destroy it or let it
time-out" (`interface/wx/splash.h:21-22`).

**Source facts** (`src/generic/splash.cpp`), none of which the docs mention:
- `wxSplashScreen::Init` registers the splash as a **global `wxEventFilter`**
  (`wxEvtHandler::AddFilter(this)`); `FilterEvent` calls `Close(true)` on any `wxEVT_KEY_DOWN`,
  `LEFT_DOWN`, `RIGHT_DOWN` or `MIDDLE_DOWN`; `OnCloseWindow` calls `Destroy()`.
- `wxSPLASH_TIMEOUT` closes it from a timer the same way. Both paths destroy it while your code may
  still hold the pointer.
- TLW `Destroy()` is deferred to idle, and a full yield runs idle, so the splash is freed inside the
  next `wxYield()`; a `wxWeakRef` to it nulls only when that pending delete runs.
- It adds `wxFRAME_TOOL_WINDOW | wxFRAME_NO_TASKBAR`, sets `wxWS_EX_TRANSIENT` (never chosen as a
  parent) and, on GTK, `GDK_WINDOW_TYPE_HINT_SPLASHSCREEN`.

**OrcaSlicer.** `SplashScreen : wxSplashScreen` (`GUI_App.cpp`):
- no `wxSPLASH_TIMEOUT` — a timeout closed it long before init finished, leaving a blank screen;
- `FilterEvent` overridden to return `wxEventFilter::Event_Skip`, disabling close-on-click/key;
- held in `wxWeakRef<SplashScreen> scrn` in `GUI_App::on_init_inner`, updated with
  `SetText(text, progress)` (+ `wxYield()` at the long phases; `OnInit` runs before the main loop, so
  `wxYield` uses a temporary loop), and removed with `scrn->Destroy(); scrn = nullptr;` after the main
  frame is shown;
- `SetText` copies the text into `m_text_action`, then `Refresh()`/`Update()`, plus `wxYield()` under
  `__WXOSX__` (without it the splash doesn't update on macOS);
- paints itself (`OnPaint` on `m_window`) with `StateColor::darkModeColorFor` colours; on Wayland it
  installs an empty CSD titlebar because some compositors ignore the splash type hint;
  `wxSTAY_ON_TOP` on macOS.

**Pitfalls.**
- **Rule:** If a `wxSplashScreen` must stay up across a long startup phase, override `FilterEvent()`
  to a no-op, hold it in a `wxWeakRef`, and destroy it only explicitly.
  **Why:** the global filter `Close()`s — and thereby `Destroy()`s — the splash on any key or mouse
  press; a stray click during startup freed it inside one of `on_init_inner`'s own `wxYield()`s while
  the code still held the raw pointer — an intermittent use-after-free (the Linux startup crash).
  Cite: 4088a36095 (`SplashScreen::FilterEvent`, `wxWeakRef<SplashScreen> scrn` in
  `GUI_App::on_init_inner`).
- **Rule:** Passing a temporary such as `_L("…") + dots` straight into a `const wxString&` parameter is
  safe even if the callee yields or repaints. **Why:** the temporary lives until the end of the
  full-expression, `SplashScreen::SetText` copies it before yielding, and 3.3's `wxString` is a
  non-COW `std::wstring` (`include/wx/string.h:119-132`). 6b55e324c9 bound one such temporary to a
  named local, but that did not address the crash; 4088a36095 did.
- **Rule:** Never keep a pointer into a temporary conversion buffer beyond its statement:
  `const char* p = s.ToUTF8().data();` (or `mb_str()`, or `c_str()` stored as `const char*`) dangles —
  `ToUTF8()`/`utf8_str()` return a temporary `wxScopedCharBuffer` (`interface/wx/string.h:755, 886`).
  Copy immediately: `std::string u = s.ToUTF8().data();`, or use `s.utf8_string()`
  (`interface/wx/string.h:765`) / Orca's `into_u8(s)` (`GUI.hpp`). See `references/strings-i18n-files.md`.

## wxGetApp() and app_config

- `wxGetApp()` returns `GUI_App&` (`DECLARE_APP`); qualify as `GUI::wxGetApp()` outside
  `Slic3r::GUI`. Singletons hang off it: `app_config`, `preset_bundle`, `plater()`, `mainframe`,
  `getDeviceManager()`, `getAgent()`.
- **Lifetime.** `app_config` is created in the `GUI_App` constructor and is non-null for the whole GUI
  lifetime. `plater()` and `mainframe` are null before `on_init_inner` creates the frame, are replaced
  during `recreate_GUI`, and must not be used after `MainFrame::shutdown`. `sidebar()`, `obj_list()`
  and `model()` dereference the plater unchecked — never call them from code reachable before the
  plater exists or during shutdown.
- **Shutdown and threads.** Code that can run during shutdown, or on a worker, checks
  `!wxTheApp || wxGetApp().is_closing()` before touching the app (`ActionRegistry::init`); deferred
  bodies check `is_closing()` again inside. `wxGetApp()` itself dereferences null once wx cleanup has
  started.
- **`AppConfig` is main-thread only.** It is a plain `std::map` store with no locking, and
  `AppConfig::save()` throws `CriticalException` when `!is_main_thread_active()`. Workers read the
  values they need before starting, and marshal `set`/`save` through `CallAfter` (the
  `GUI_App::load_pending_vendors` shape).
- **Saving.** `set(...)` marks the config dirty; the app idle handler saves whenever `dirty()` (after
  `post_init`), and `MainFrame::shutdown` saves if dirty. Call `app_config->save()` explicitly only
  when the value must hit disk immediately (Preferences rows do).
- **Reading.** Values are strings: `get("key") == "true"`; `get_bool(key)` accepts `"true"` or `"1"`.
  `get_bool(section, key)` tests `"true"` in `section` but reads the `"1"` form from the `"app"`
  section. `AppConfig` vs `wxConfig`: `references/strings-i18n-files.md`.
