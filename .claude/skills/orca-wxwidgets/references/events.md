# Events: binding, dispatch, posting and deferred calls

How wx 3.3.2 finds, runs, propagates, queues and drops event handlers, and how OrcaSlicer code
binds, emits and defers. Read it before writing any `Bind`/`Unbind`, `Skip()`, `ProcessEvent`,
`wxPostEvent`/`wxQueueEvent` or `CallAfter`, before defining a custom event, and when debugging a
handler that never runs, runs twice, runs on a dead object, or swallows a widget's own behaviour.

Contents: [Rules](#rules) · [1 Dispatch order](#1-dispatch-order) ·
[2 Bind and Unbind](#2-bind-and-unbind) · [3 Static event tables](#3-static-event-tables) ·
[4 Skip discipline](#4-skip-discipline) · [5 Propagation](#5-propagation) ·
[6 Emitting events synchronously](#6-emitting-events-synchronously) ·
[7 Posting and queueing](#7-posting-and-queueing) · [8 CallAfter](#8-callafter-and-the-liveness-rule) ·
[9 Custom events and payloads](#9-custom-events-and-payload-ownership) · [10 Ids](#10-window-and-event-ids) ·
[11 UPDATE_UI](#11-wxevt_update_ui) · [12 Idle events](#12-idle-events) ·
[13 Event filters](#13-event-filters) · [14 Pushed handlers and blockers](#14-pushed-handlers-wxeventblocker-setevthandlerenabled) ·
[15 Exceptions](#15-exceptions-in-handlers) · [16 How Orca widgets emit events](#16-how-orca-widgets-emit-events)

Build fact that shapes every pitfall below: OrcaSlicer builds wx with `-DwxBUILD_DEBUG_LEVEL=0`
(`deps/wxWidgets/wxWidgets.cmake`) and `libslic3r_gui` with `-DwxDEBUG_LEVEL=0`
(`src/slic3r/CMakeLists.txt`). `wxASSERT`/`wxFAIL` compile to nothing and `wxCHECK_*` return silently
(`include/wx/debug.h:314-324, 356-382`). Every event misuse wx would assert on in a debug build
(pushed handler not popped, `Unbind` of an unknown entry, `RemoveFilter` of an unknown filter,
out-of-range window id, bad `FilterEvent` return) is a silent no-op or a later crash in Orca.

## Rules

1. New code binds dynamically (`Bind` with a lambda or method + handler); never add a static event
   table (existing widget-internal tables stay where they are). Take the event by reference (`auto&`,
   `wxXxxEvent&`). → §2, §3, §4
2. Later-bound handlers run first, and every dynamic handler runs before the static table. A handler
   you bind on an Orca widget, a wx control or a window whose base class already bound the same
   event must `Skip()` or the internal handler never runs. → §4, §16
3. `Skip()` every non-command event you do not fully replace: focus, size, key-down, unhandled
   `wxEVT_CHAR_HOOK` keys, DPI and system-colour changes, TLW activation, mouse events on custom
   widgets. Command events are normally not skipped. → §4
4. A binding on an object other than `this` (parent, TLW, canvas, app) must not outlive the handler:
   method + `wxEvtHandler` sink is removed automatically but late; lambdas and non-`wxEvtHandler`
   sinks are never removed. Unbind in the destructor, or use `EventGuard`. → §2
5. `Unbind` with the same emitter, event type, id range and the same functor object (or the same
   method + handler). A lambda literal never matches. → §2
6. `Bind`/`Unbind` on the main thread only; binding twice registers twice. → §2
7. Only command events, `wxEVT_CHAR_HOOK` and Orca's `SimpleEvent`/`Event<T>` family propagate; they
   stop at dialogs, and at popups on MSW and macOS but not on wxGTK. Bind on the emitting control, or
   on the dialog/popup itself. → §5
8. `wxEVT_DESTROY` bubbles from children: compare `GetEventObject()` with the window. A TLW's destroy
   event arrives after its derived members are destroyed. → §5
9. Emit a window's event with `ProcessWindowEvent()` (or `HandleWindowEvent()` from native
   callbacks), never `win->ProcessEvent()`; forward to another handler with `ProcessEventLocally()`. → §6
10. Construct the event class declared for the type, and set the event object and id. → §6, §9
11. A handler may destroy the emitter: never touch `this` after a synchronous emit that can lead to
    destruction; defer destruction instead. → §6
12. `AddPendingEvent`/`wxPostEvent` only on the main thread; `QueueEvent`/`wxQueueEvent` with a heap
    event from any thread. A posted event is dispatched on the object you posted to, bypassing its
    pushed handlers. → §7
13. Events and `CallAfter`s queued on a handler are deleted with it. A worker must only post to a
    target that outlives the worker (in Orca: `wxGetApp()`), or be stopped first. → §7
14. Order is FIFO only per target; a `CallAfter` that re-queues itself starves the UI. Use `wxTimer`
    for retries and polling. → §7
15. Deferred work runs inside any nested loop (`ShowModal`, `wxYield`) and is held back by some
    native modal loops; design for reentrancy. → §7
16. Every deferred lambda re-checks the liveness of everything it touches, except the handler it was
    queued on. Never capture a bare `this` or `wxDataViewItem` and trust it. → §8
17. `CallAfter` captures are copied: capture by value, `shared_ptr` for move-only state; by-reference
    captures only in a blocking marshal. → §8
18. Window work requested from a mouse handler or a webview script-message callback goes through
    `CallAfter`. → §8
19. `wxDECLARE_EVENT` in the header, `wxDEFINE_EVENT` in exactly one `.cpp`. → §9
20. A custom event class derived from a concrete wx event overrides `Clone()` and copies every
    payload member. Client objects on a `wxCommandEvent` are not owned by the event. → §9
21. Use `wxID_ANY` for controls and bind on the control; do not filter by id on an ancestor. → §10
22. `wxEVT_UPDATE_UI` handlers run every idle pass for every window: keep them trivial. → §11
23. Do not use idle events for periodic work; hidden panels still receive them. → §12
24. `FilterEvent` runs for every event: return `Event_Skip` fast. → §13
25. Pop or remove every pushed handler before its window dies; nest `wxEventBlocker` scopes and push
    nothing else inside one. → §14
26. Catch exceptions inside handlers; one that escapes ends the session. → §15

---

## 1. Dispatch order

**Contract.** `wxEvtHandler::ProcessEvent()` searches in this order (`interface/wx/event.h:567-625`,
`docs/doxygen/overviews/eventhandling.h:451-515`):

| Step | What runs | Notes |
|---|---|---|
| 0 | `wxApp::FilterEvent()` and other `wxEventFilter`s (LIFO) | Anything but `Event_Skip` (-1) stops here. Called once per event, not again as it propagates ([source] `src/common/event.cpp:1537-1552`) |
| 1 | `TryBefore()` | Validators on windows |
| 2 | — | If `SetEvtHandlerEnabled(false)`, skip to step 5 (the overview's wording; the interface doc's "skips to step (7)" is inaccurate — [source] `TryHereOnly` returns `false` and `DoTryChain` still runs, `src/common/event.cpp:1582-1591, 1644-1648`) |
| 3 | Dynamic table (`Bind`) | **Most recently bound first**, before the static table (`docs/doxygen/overviews/eventhandling.h:474-483`) |
| 4 | Static event table | Macro order, derived class before base class |
| 4a | Implicit `CallAfter` entry | Runs a queued `wxAsyncMethodCallEvent` only when its event object is this handler ([source] `src/common/event.cpp:1644-1667` `TryHereOnly`) |
| 5 | Next handlers in the chain | For windows: the pushed-handler stack (§14) |
| 6 | `TryAfter()` | Windows propagate to the parent (§5); finally `wxTheApp->ProcessEvent()` |

`ProcessEvent` returns `true` iff some handler ran and did not call `Skip()` (`interface/wx/event.h:619-622`).
Before each handler call wx resets the flag with `event.Skip(false)` ([source]
`src/common/event.cpp:1443-1475` `ProcessEventIfMatchesId`), so a skip in one handler does not carry over to the
next: every handler that wants processing to continue must call `Skip()` itself.

A handler entry matches when the event type matches and the bound id is `wxID_ANY`, or equals the
event id, or the event id falls in `[id, lastId]` (`src/common/event.cpp:1443-1475`).

---

## 2. Bind and Unbind

**Contract.**
- Forms: `Bind(tag, functor, id = wxID_ANY, lastId = wxID_ANY, userData = nullptr)` and
  `Bind(tag, &Class::method, handlerPtr, id, lastId, userData)` (`interface/wx/event.h:876-957`). The
  method form accepts "an arbitrary method (doesn't need to be from a wxEvtHandler derived class)";
  the handler pointer "must always be specified". `userData`: "wxWidgets will take ownership of
  this pointer" — deleted when the handler is unbound or at program termination.
- Handlers can be bound at any time and removed with `Unbind`
  (`docs/doxygen/overviews/eventhandling.h:237-252`). `Connect()` is the legacy form: "please use
  [Bind] in any new code" (`interface/wx/event.h:705-706`).
- Lifetime (`docs/doxygen/overviews/eventhandling.h:332-335`), for a handler object not derived from `wxEvtHandler`: "the
  lifetime of `myFrameHandler` must be greater than that of `MyFrame` object -- or at least it needs
  to be unbound before being destroyed".
- `Unbind` "can only unbind functions, functors or methods which have been added using the Bind<>()
  method. There is no way to unbind functions bound using the (static) event tables." Its note:
  "functors are compared by their address which, unfortunately, doesn't work correctly if the same
  address is reused for two different functor objects. Because of this, using Unbind() is not
  recommended if there are multiple functors using the same eventType and id and lastId as a wrong
  one could be unbound" (`interface/wx/event.h:958-997`).

**Mechanics** [source]:
- `Bind` takes `const Functor&`, stores a **copy** of the functor and records the **address of the
  object you passed** (`include/wx/event.h:524-570` `wxEventFunctorFunctor`, `:3951-3961`). `Unbind`
  matches on that address plus the functor type. So a lambda is unbindable when the same lvalue
  (a member `std::function`, a named lambda that stays at one address, heap storage) is passed to
  both calls; an inline lambda literal never matches. Method + handler pairs match by value and
  always work. Captures must be copyable.
- `DoUnbind` requires `entry->m_id == id`; only `lastId == wxID_ANY` and `eventType == wxEVT_NULL`
  act as wildcards (`src/common/event.cpp:1806-1824`). A handler bound with `ctrl->GetId()` is not
  removed by `Unbind(evt, fn)` (id defaults to `wxID_ANY`), and vice versa. `Unbind` on a different
  emitter than the one you bound on returns `false` silently.
- `DoBind` always appends (`src/common/event.cpp:1769-1803`): binding the same handler twice runs it twice.
- No locking in `DoBind`/`DoUnbind`: main thread only.

**Lifetime by handler kind** [source]:

| Handler | Removed automatically when the handler object dies? |
|---|---|
| `src->Bind(evt, &C::m, sink)` where `sink` is a `wxEvtHandler` (any window) other than `src` | Yes. `DoBind` registers a `wxEventConnectionRef` on the sink (`src/common/event.cpp:1793-1802`); the sink's `~wxTrackable` calls `OnSinkDestroyed`, which deletes the entries (`include/wx/event.h:4184-4200`, `src/common/event.cpp:2022-2042`) |
| Lambda or functor (`[this]{…}`), free function | No. `GetEvtHandler()` is null for functors |
| Method of a class not derived from `wxEvtHandler` (`GLCanvas3D`, `Plater::priv`) | No |
| Handlers bound on `this` itself | Deleted with `this` (`~wxEvtHandler`, `src/common/event.cpp:1204-1245`) |

The automatic removal runs **late**: `wxEvtHandler` derives from `wxObject, wxTrackable`
(`include/wx/event.h:3705-3706`), so `~wxTrackable` runs after the derived destructor, after member
destruction, and after the port destructor has destroyed the native window and the children
(`src/osx/window_osx.cpp` `~wxWindowMac`, `src/msw/window.cpp` `~wxWindowMSW`). Events the source
emits during that teardown (activation, focus, size, show) still reach the half-destroyed sink.
Unbind explicitly in the sink's destructor whenever the source can fire while the sink dies.

**Usage.**
```cpp
// Method + wxEvtHandler sink on another window: removed on sink death (late) — unbind early anyway
m_parent->Bind(wxEVT_DPI_CHANGED, &DialogButtons::on_dpi_changed, this);
DialogButtons::~DialogButtons() { m_parent->Unbind(wxEVT_DPI_CHANGED, &DialogButtons::on_dpi_changed, this); }

// Lambda on another object: unbindable only through the same stored object
std::function<void(wxShowEvent&)> m_on_show = [this](wxShowEvent& e) { e.Skip(); /* ... */ };
top->Bind(wxEVT_SHOW, m_on_show);
top->Unbind(wxEVT_SHOW, m_on_show);   // same object → matches
```

**OrcaSlicer.**
- `EventGuard` (`src/slic3r/GUI/GUI_Utils.hpp`) is the RAII form: it stores the functor (or method +
  handler) on the heap, so its address is stable and the destructor's `Unbind` matches. Use it when
  the emitter outlives the handler object, or to drop a binding before the owner's base destructor
  runs. The emitter must still be alive when the guard dies.
  ```cpp
  EventGuard on_idle_evt;   // member of PlaterWorker (src/slic3r/GUI/Jobs/PlaterWorker.hpp)
  , on_idle_evt(plater, wxEVT_IDLE, [this](wxIdleEvent&) { process_events(); })
  EventGuard on_progress_evt;   // PrintHostQueueDialog binds on itself, unbinds during member destruction
  , on_progress_evt(this, EVT_PRINTHOST_PROGRESS, &PrintHostQueueDialog::on_progress, this)
  ```
- `GLCanvas3D` is not a `wxEvtHandler`: its method bindings on its `wxGLCanvas` are never removed
  automatically, so `GLCanvas3D::bind_event_handlers`/`unbind_event_handlers` are a mandatory pair,
  and `Plater::priv::set_current_panel` unbinds the canvas of the panel being left before binding
  the active one.
- **Binding on another window (popups and child widgets).** When an object binds on a window other
  than itself — typically the top-level parent — unbind in its destructor.
  `PopupWindow::Create` binds `wxEVT_ACTIVATE` on its top parent (wxGTK), `BindUnfocusEvent()` binds
  `wxEVT_ACTIVATE`/`wxEVT_ICONIZE`/`wxEVT_SHOW` (wxMSW), and `PopupWindow::~PopupWindow` unbinds them
  (`src/slic3r/GUI/Widgets/PopupWindow.cpp`). The static `GetTopParent` there returns the first
  `wxNonOwnedWindow` strictly above its argument (or the root), so for a popup inside another popup
  the handlers sit on the outer popup. Because `Create` passes `parent` and the destructor passes
  `this`, the two calls resolve to different windows when `parent` is itself a TLW or popup that has a
  parent: the wxGTK `Unbind` then misses silently (§2 Mechanics). These method + `this` bindings
  would be removed by wx when the popup dies, but only after the teardown window described above; a
  lambda binding would never be removed. Popup dismissal itself: see `references/popups-menus.md`.

**Pitfalls.**
- **Rule:** A lambda that captures `this` and is bound on a longer-lived object must be unbound
  before `this` dies.
  **Why:** functor bindings are not tracked; the emitter later calls into freed memory.
  ```cpp
  // Wrong: dangles after this panel is destroyed
  GetParent()->Bind(wxEVT_SHOW, [this](wxShowEvent& e) { e.Skip(); refresh(); });
  // Right: method + wxEvtHandler sink, and unbind in the destructor
  GetParent()->Bind(wxEVT_SHOW, &MyPanel::on_parent_show, this);
  MyPanel::~MyPanel() { GetParent()->Unbind(wxEVT_SHOW, &MyPanel::on_parent_show, this); }
  ```
  Cite: `src/common/event.cpp:1793-1802, 2022-2042`; `docs/doxygen/overviews/eventhandling.h:332-335`.
- **Rule:** Unbind with the same object, id and emitter you bound with.
  **Why:** a temporary lambda has a new address; a missing id does not match an id-bound entry. Both
  return `false` and leave the handler bound — silently.
  ```cpp
  // Wrong
  btn->Bind(wxEVT_BUTTON, fn, btn->GetId());   btn->Unbind(wxEVT_BUTTON, fn);
  win->Unbind(wxEVT_SIZE, [this](wxSizeEvent& e) { e.Skip(); });
  // Right
  btn->Unbind(wxEVT_BUTTON, fn, btn->GetId());
  ```
  Cite: `include/wx/event.h:524-570`; `src/common/event.cpp:1806-1824`.
- **Rule:** Move-only captures do not compile in `Bind` or `CallAfter`; use `std::shared_ptr`.
  Cite: `include/wx/event.h:524-570` (functor stored by copy).

---

## 3. Static event tables

**Contract.** `wxDECLARE_EVENT_TABLE()` in the class, `wxBEGIN_EVENT_TABLE(Class, Base)` …
`wxEND_EVENT_TABLE()` in the `.cpp`; entries are searched in macro order, then the base class table
(`interface/wx/event.h:567-625` step 5). They run **after** every dynamic handler for the same event.

**Multiple inheritance.** "it is imperative that the wxEvtHandler(-derived) class is the first class
inherited such that the `this` pointer for the overall object will be identical to the `this`
pointer of the wxEvtHandler portion" (`interface/wx/event.h:376-380`). Put the wx base first in
`class X : public wxPanel, public Other`.

**OrcaSlicer.** New code binds with lambdas (`[this](auto& e)`) or method + `this`. The core widgets
keep their internal handlers in static tables (`DECLARE_EVENT_TABLE()` in `Widgets/StaticBox.hpp`,
`Widgets/Button.hpp`; tables in `Button.cpp`, `TextInput.cpp`, `SpinInput.cpp`, `ComboBox.cpp`,
`DropDown.cpp`, `TabCtrl.cpp`). That is why `Skip()` in user handlers matters (§4): a user `Bind` on
`Button` for `wxEVT_LEFT_DOWN` runs before `Button::mouseDown`. Do not add new static tables; when
changing such a widget, keep its internal handlers where they are.

---

## 4. Skip discipline

**Contract.** `wxEvent::Skip` (`interface/wx/event.h:238-252`): "Without Skip() (or equivalently if
Skip(false) is used), the event will not be processed any more. If Skip(true) is called, the event
processing system continues searching for a further handler function for this event, even though it
has been processed already in the current handler. In general, it is recommended to skip all
non-command events to allow the default handling to take place. The command events are, however,
normally not skipped as usually a single command such as a button click or menu item selection must
only be processed by one handler."

| Event | Rule | Cite |
|---|---|---|
| `wxEVT_SET_FOCUS` / `wxEVT_KILL_FOCUS` | "should almost invariably call wxEvent::Skip()"; a KILL_FOCUS handler "must not call wxWindow::SetFocus()" — defer it (`CallAfter`) | `interface/wx/event.h:3410-3416` |
| `wxEVT_SIZE` | "Sizers … rely on size events to function correctly … call Skip on all size events you catch" | `interface/wx/event.h:5058-5060` |
| `wxEVT_KEY_DOWN` | Not skipping suppresses `wxEVT_CHAR` for that key and "may also prevent accelerators … from working" | `interface/wx/event.h:1454-1461` |
| `wxEVT_CHAR_HOOK` | Propagates upward; handled (not skipped) → no `KEY_DOWN`/`CHAR`. Skip every key you do not handle | `interface/wx/event.h:1485-1508`; keyboard order: `references/mouse-keyboard-focus.md` |
| `wxEVT_PAINT` | The handler "must create a wxPaintDC"; skip only if default painting must also run | `interface/wx/event.h:2274-2285`; `references/painting-custom-widgets.md` |
| `wxEVT_DPI_CHANGED` | "should almost always call event.Skip() … as many controls rely on processing this event"; a TLW handler may deliberately not skip to suppress the default resize | `interface/wx/event.h:3571-3583` |
| `wxEVT_SYS_COLOUR_CHANGED` | The default handler propagates it to children; a TLW handler must Skip, call the base, or forward | `interface/wx/event.h:1950-1955` |
| `wxEVT_ACTIVATE` on a TLW | MSW: `wxTopLevelWindowMSW::OnActivate` (static table) saves and restores the last focused child; macOS: `wxFrame::OnActivate` (static table) installs the frame's menubar. A non-skipping dynamic handler disables both | [source] `src/msw/toplevel.cpp` `wxTopLevelWindowMSW::OnActivate`, `src/osx/carbon/frame.cpp` `wxFrame::OnActivate`; `references/popups-menus.md` §15 |
| Mouse/key events on custom widgets | Dynamic handlers run before the widget's static table: not skipping disables the widget's own press/release/capture logic | [source] `src/common/event.cpp:1644-1667` |
| Command events | Normally not skipped; `Skip()` lets the event continue to the static table, the next handler, then the parent | `interface/wx/event.h:247-251` |

wx's own controls bind some internals dynamically in their constructors — e.g.
`wxBookCtrlBase`, `wxComboCtrlBase` and `wxTreeCtrlBase` bind `wxEVT_DPI_CHANGED`
(`src/common/bookctrl.cpp:60`, `src/common/combocmn.cpp:839`, `src/common/treebase.cpp:172`) — so a
non-skipping handler you bind on
such a control later starves its rescale. DPI propagation order: `references/dpi-bitmaps-fonts.md`.

**OrcaSlicer — deliberate deviations.**
- `DPIAware<P>` (`src/slic3r/GUI/GUI_Utils.hpp`) binds `wxEVT_DPI_CHANGED` on the TLW (not on macOS)
  **without** Skip: Orca rescales itself in `rescale()` and suppresses wx's default TLW resize, which
  the doc allows. Its `wxEVT_SYS_COLOUR_CHANGED` handler Skips on macOS and Linux but not on Windows,
  where the theme is app-forced.
- Because `DPIAware` binds in its own constructor, everything a derived dialog binds on itself later
  runs first. `DialogButtons` binds the parent's `wxEVT_DPI_CHANGED` and calls `Skip()`, so it runs
  before `DPIAware`'s handler and lets it run. Likewise `DPIAware` maps Esc to `Close()` in a
  `wxEVT_CHAR_HOOK` handler on every `DPIDialog`: a derived dialog's own `wxEVT_CHAR_HOOK` handler must
  `Skip()` every key it does not consume, or Esc stops closing the dialog (`CloneDialog`'s Enter→OK
  hook is the model).

**Pitfalls.**
- **Rule:** Take the event parameter by reference.
  **Why:** the functor form compiles with a by-value parameter; the lambda then receives a copy
  (`include/wx/event.h:534-545` calls `m_handler(static_cast<EventArg&>(event))`), and `Skip()` on the
  copy does nothing — the original counts as handled.
  ```cpp
  // Wrong
  ctrl->Bind(wxEVT_KILL_FOCUS, [](wxFocusEvent e) { e.Skip(); commit(); });
  // Right
  ctrl->Bind(wxEVT_KILL_FOCUS, [](wxFocusEvent& e) { e.Skip(); commit(); });
  ```
- **Rule:** A handler you add to an Orca widget, for any event the widget handles internally, calls
  `Skip()`.
  **Why:** your later `Bind` runs first (LIFO, dynamic before static); without Skip the widget's own
  handler never runs.
  ```cpp
  // Wrong: CheckBox's internal toggle handler never runs, the bitmap and half-state go stale
  cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { save(); });
  // Right
  cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { e.Skip(); save(); });
  ```
  The same applies to `GetTextCtrl()->Bind(wxEVT_TEXT_ENTER / wxEVT_KILL_FOCUS / wxEVT_TEXT, …)` on
  `TextInput`/`SpinInput`/`ComboBox`, and to raw mouse/key binds on `Button` and other `StaticBox`
  widgets. Cite: `src/slic3r/GUI/Widgets/CheckBox.cpp` `CheckBox::CheckBox`;
  `src/slic3r/GUI/Preferences.cpp` ("let CheckBox::update() refresh the bitmap").
- **Rule:** Never call `Skip()` (or touch members) after the handler deleted `this` (§6).

---

## 5. Propagation

**Contract** (`docs/doxygen/overviews/eventhandling.h:536-568`):
- "the events of the classes deriving from wxCommandEvent are propagated by default to the parent
  window if they are not processed in this window itself … all event classes not deriving from
  wxCommandEvent … do not propagate upward." Mouse, motion, enter/leave, size, paint and key events
  stay at the window — except `wxEVT_CHAR_HOOK`, which propagates (`interface/wx/event.h:1485-1494`).
- "the event propagation stops when it reaches the parent dialog, if any … The events do propagate
  beyond the frames, however." `SetExtraStyle(wxWS_EX_BLOCK_EVENTS)` blocks at any window, or clears
  the default on a dialog (`interface/wx/window.h:264-270`).
- The mechanism is `m_propagationLevel` (`interface/wx/event.h:263-279`): `wxEVENT_PROPAGATE_NONE` by default,
  `wxEVENT_PROPAGATE_MAX` for command events; any event class may set it in its constructor.
  `StopPropagation()` returns the old level for `ResumePropagation()` (`interface/wx/event.h:255-260`);
  `wxPropagationDisabler` and `wxPropagateOnce` are RAII helpers (`interface/wx/event.h:346-367`).

**Source facts** [source]:
- **Popups block propagation on MSW and macOS, not on wxGTK.** `wxPopupWindowBase::Create` sets
  `wxWS_EX_BLOCK_EVENTS` (`src/common/popupcmn.cpp:129-138`; not in the overview). The MSW and macOS
  `wxPopupWindow::Create` call it (`src/msw/popupwin.cpp`, `src/osx/carbon/popupwin.cpp`, which the
  Cocoa build uses); the wxGTK one never does (`src/gtk/popupwin.cpp` `wxPopupWindow::Create`). So a
  `wxEVT_BUTTON` from a control inside a popup (`wxPopupTransientWindow`, Orca `PopupWindow`,
  `DropDown`) that no handler inside consumes stops at the popup on MSW/macOS but bubbles on to the
  popup's parent and its ancestors on GTK.
- `wxWindowBase::TryAfter` does not propagate to a parent that `IsBeingDeleted()`
  (`src/common/wincmn.cpp:3499-3522`). After a block or the top of the chain, the event still goes
  to `wxTheApp` — except `wxEVT_IDLE` (`src/common/event.cpp:1483-1498` `DoTryApp`).
- `wxWindowDestroyEvent` and `wxWindowCreateEvent` derive from `wxCommandEvent`
  (`interface/wx/event.h:4554, 2255`): a `wxEVT_DESTROY` handler on a dialog also receives every
  child's destroy event (unless the dialog itself is being deleted).
- `wxUpdateUIEvent` is a command event: every unhandled update-UI event walks the parent chain up to
  the first blocking window (a dialog; a popup on MSW/macOS) or the root, and then `wxApp` (§11).

**Destroy-event timing** [source]. `wxWindowBase::Destroy()` of a child sends `wxEVT_DESTROY` before
`delete this` (`src/common/wincmn.cpp:559-573`). For a TLW, and for any `delete`, the event is sent
from a base destructor (`src/common/framecmn.cpp` `~wxFrameBase`, `src/msw/toplevel.cpp`
`~wxTopLevelWindowMSW`, `src/gtk/toplevel.cpp` `~wxTopLevelWindowGTK`, `src/osx/dialog_osx.cpp`
`~wxDialog`, `src/osx/nonownedwnd_osx.cpp` `~wxNonOwnedWindow`, `src/osx/window_osx.cpp` `~wxWindowMac`)
— after the derived class destructor and its members are gone. A `wxEVT_DESTROY` handler may clear
an outside pointer to the window, but must not call into the derived object. Cleanup that needs the derived members belongs in the
derived destructor (`WebDialog::~WebDialog` in `src/slic3r/GUI/WebDialog.cpp` documents this choice).

**OrcaSlicer.** The payload events in `src/slic3r/GUI/Event.hpp` — `SimpleEvent`, `IntEvent`,
`Event<T>`, `ArrayEvent<T, N>` — derive from `wxEvent` but set
`m_propagationLevel = wxEVENT_PROPAGATE_MAX`, so they bubble like command events and, like them,
stop at dialogs and (on MSW/macOS) popups.

**Pitfalls.**
- **Rule:** In a `wxEVT_DESTROY` handler bound on a window with children, check the event object.
  ```cpp
  // Wrong: the first child destroyed clears the pointer
  m_plugins_dlg->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent&) { m_plugins_dlg = nullptr; });
  // Right (GUI_App::open_plugins_dialog)
  m_plugins_dlg->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent& e) {
      if (e.GetEventObject() == m_plugins_dlg) m_plugins_dlg = nullptr;
      e.Skip();
  });
  ```
- **Rule:** Do not expect events from inside a dialog or popup at its opener — and do not rely on
  popup events *not* arriving there either.
  **Why:** `wxWS_EX_BLOCK_EVENTS` (`src/common/dlgcmn.cpp` `wxDialogBase::wxDialogBase`,
  `src/common/popupcmn.cpp`) blocks at every dialog, but at popups only on MSW/macOS; on wxGTK an
  unconsumed command event from inside a popup reaches the opener's ancestors and any unfiltered
  handler there. Bind on the dialog/popup or on the control, consume the event there, or re-emit
  explicitly.
- **Rule:** An ancestor that binds a command event without an id filter receives that event from
  every descendant control of that type.

---

## 6. Emitting events synchronously

| Call | Use for | Note |
|---|---|---|
| `win->ProcessWindowEvent(e)` = `win->GetEventHandler()->ProcessEvent(e)` | Emitting a window's own event | "ProcessEvent() itself can't be called for wxWindow objects as it ignores the event handlers associated with the window; use this function instead" (`interface/wx/window.h:2736-2744`) |
| `win->HandleWindowEvent(e)` = `GetEventHandler()->SafelyProcessEvent(e)` | Same, from code that must not leak exceptions (native callbacks) | `interface/wx/window.h:2726-2734` |
| `win->ProcessWindowEventLocally(e)` | This window and its pushed handlers only, no propagation | `interface/wx/window.h:2746-2757` |
| `handler->ProcessEventLocally(e)` | Forwarding an event to another handler | "should, be called to forward an event to another handler instead of ProcessEvent() which would result in a duplicate call to TryAfter()" (`interface/wx/event.h:627-651`) |
| `handler->SafelyProcessEvent(e)` | Catches exceptions → `wxApp::OnExceptionInMainLoop` | `interface/wx/event.h:653-666` |
| `handler->ProcessEvent(e)` | Non-window handlers | On a window object it bypasses pushed handlers (Orca `StateHandler`) |

**Emitting shape** (`docs/doxygen/overviews/eventhandling.h:660-671`): construct the event with the
type and `GetId()`, `SetEventObject(this)`, fill the payload, `ProcessWindowEvent(event)`.

**Programmatic changes.** wx controls normally send command events only for user actions. The
documented exceptions include `wxNotebook::AddPage/AdvanceSelection/DeletePage/SetSelection`,
`wxTreeCtrl::Delete/DeleteAllItems/EditLabel` and "All wxTextCtrl methods" — use
`wxTextCtrl::ChangeValue` instead of `SetValue`; `Replace`/`WriteText` have no event-free form
(`docs/doxygen/overviews/eventhandling.h:796-815`). `wxBitmapToggleButton::SetValue` "does not cause a EVT_TOGGLEBUTTON event
to be emitted" (`interface/wx/tglbtn.h:169`). Orca widget setters: §16.

**Reentrancy.** A synchronous emit runs every handler before it returns. If a handler destroys the
emitter, the emitter's code after `ProcessEvent` runs on freed memory. Non-TLW `Destroy()` deletes
immediately (`src/common/wincmn.cpp:559-573`).

**OrcaSlicer models.** `Button::sendButtonEvent` (`Widgets/Button.cpp`): `wxCommandEvent` of
`wxEVT_BUTTON` with `GetId()`, `SetEventObject(this)`, `GetEventHandler()->ProcessEvent`.
`MsgDialog::show_dsa_button` (`MsgDialog.cpp`) makes a label click behave like a checkbox click:
`SetValue(!GetValue())`, then emits `wxEVT_TOGGLEBUTTON` with the checkbox's id and object through its
`GetEventHandler()`. `CloneDialog` (`CloneDialog.cpp`) turns Enter in its spin box into an OK click
from its `wxEVT_CHAR_HOOK` handler by emitting `wxEVT_BUTTON` with `ok_btn->GetId()` through
`ok_btn->GetEventHandler()`, and Skips other keys.

**Pitfalls.**
- **Rule:** Never emit a window's event with `win->ProcessEvent()`.
  ```cpp
  // Wrong: skips handlers pushed on the window (StateHandler, wxEventBlocker)
  wxCommandEvent e(wxEVT_BUTTON, GetId()); e.SetEventObject(this); this->ProcessEvent(e);
  // Right
  wxCommandEvent e(wxEVT_BUTTON, GetId()); e.SetEventObject(this); ProcessWindowEvent(e);
  ```
  Cite: `interface/wx/window.h:2736-2744`.
- **Rule:** Construct the event class the type was declared with.
  **Why:** the `Bind` tag ties type and class at compile time, but nothing checks the object you
  construct. `wxCommandEvent e(wxEVT_LEFT_DOWN, id)` compiles; handlers then `static_cast` it to
  `wxMouseEvent&` (undefined behaviour), and it propagates to parents like a command event. To make
  a whole card clickable, call the action directly or emit a semantic event.
  ```cpp
  // Wrong: a "mouse" event that is a wxCommandEvent, emitted past the handler stack
  auto forward = [this](wxMouseEvent&) {
      wxCommandEvent click(wxEVT_LEFT_DOWN, GetId()); click.SetEventObject(this); this->ProcessEvent(click);
  };
  // Right: a semantic command event through the handler stack (or call select() directly)
  auto forward = [this](wxMouseEvent&) {
      wxCommandEvent click(wxEVT_BUTTON, GetId()); click.SetEventObject(this); ProcessWindowEvent(click);
  };
  child->Bind(wxEVT_LEFT_DOWN, forward);
  ```
  Cite: `src/slic3r/GUI/PurgeModeDialog.cpp` `PurgeModeBtnPanel` (re-dispatches child clicks; not a
  model to copy).
- **Rule:** Defer destroying the emitter out of its own handler.
  **Why:** `Button::mouseReleased` is still on the stack when your `wxEVT_BUTTON` handler runs.
  ```cpp
  // Wrong
  btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_panel->Destroy(); });   // m_panel contains btn
  // Right
  btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      CallAfter([w = wxWeakRef<wxWindow>(m_panel)] { if (w) w->Destroy(); });
  });
  ```
  Alternative: `wxTheApp->ScheduleForDestruction(win)` (`interface/wx/app.h:191-212`). Deletion
  rules: `references/windows-dialogs.md`.

---

## 7. Posting and queueing

**Contract.**
- `QueueEvent(wxEvent*)` is asynchronous and "takes ownership of the event parameter, i.e. it will
  delete it itself … the pointer can't be used any more after the function returns". It "can be used
  for inter-thread communication from the worker threads to the main thread. It is safe in the sense
  that it uses locking internally", and wakes the idle loop via `wxWakeUpIdle()`
  (`interface/wx/event.h:409-466`). `wxQueueEvent(dest, evt)` wraps it (`interface/wx/event.h:5368-5382`).
- `AddPendingEvent(const wxEvent&)` copies the event via `Clone()`, so the original may be on the
  stack, but it "can't be used to post events from worker threads for the event objects with
  wxString fields (i.e. in practice most of them)"; "Use QueueEvent() to avoid this"
  (`interface/wx/event.h:468-488`). The overview adds that you "will need to use the latter [QueueEvent] when
  doing inter-thread communication; when you use only the main thread you can also safely use the
  former" (`docs/doxygen/overviews/eventhandling.h:618-620`). `wxPostEvent(dest, evt)` = `dest->AddPendingEvent(evt)`, "not
  thread-safe for event objects having wxString fields, use wxQueueEvent() instead"
  (`interface/wx/event.h:5355-5366`).
- Every posted event class "must implement" `Clone()` (`interface/wx/event.h:125-146`).
- `wxThreadEvent`: `Clone()` unshares the string; its category is `wxEVT_CATEGORY_THREAD`, which
  keeps it out of `YieldFor()` calls that do not ask for that category; `SetPayload<T>` needs a
  copy constructor that is thread-safe, "i.e. create a copy that doesn't share anything with the
  original" (`interface/wx/event.h:3733-3790`). [source] Plain `wxYield()` is `YieldFor(wxEVT_CATEGORY_ALL)`, which
  includes `THREAD`; the protection applies to masked yields such as the generic `wxProgressDialog`'s
  `YieldFor(wxEVT_CATEGORY_UI|wxEVT_CATEGORY_USER_INPUT)` (`src/generic/progdlgg.cpp`).

**Source facts** [source]:
- `AddPendingEvent` is literally `QueueEvent(event.Clone())` (`include/wx/event.h:3780-3789`). wxString
  is always a deep-copy `std::wstring` in 3.3 (`include/wx/string.h:121-132`), so the copy-on-write
  rationale is historical; follow the documented rule anyway.
- **Where** a posted event is dispatched: the queue belongs to the handler you posted to, and
  `ProcessPendingEvents` calls `SafelyProcessEvent` on that object (`src/common/event.cpp:1368-1440`).
  Posting to a `wxWindow*` processes it on the window object itself, **bypassing pushed handlers**
  (`StateHandler`, `wxEventBlocker`). Post to `win->GetEventHandler()` if they must see it.
- **When**: pending events run before idle (`src/common/evtloopcmn.cpp:258-330`
  `wxEventLoopManual::DoRunLoop`, the MSW loop; per-port table below); a nested loop (`ShowModal()`,
  `wxYield()`) processes them too (`src/common/evtloopcmn.cpp:172-192` `DoYieldFor`). An exiting loop
  drains them before it returns on MSW (`DoRunLoop` tail) and in macOS modal loops
  (`src/osx/core/evtloop_cf.cpp` `wxCFEventLoop::OSXDoRun`, run by `wxModalEventLoop::OSXDoRun` in
  `src/osx/cocoa/evtloop.mm`), but not on wxGTK (`src/gtk/evtloop.cpp` `wxGUIEventLoop::DoRun` just
  leaves `gtk_main()`), where they run in the enclosing loop's next idle pass. A callback can run inside any
  `ShowModal()` or `wxYield()` reached from your code. Nested loops: `references/threads-timers-app.md`.
- **Order**: FIFO only per target handler. The app keeps a list of handlers with pending events and
  drains `handler[0]` completely — including events added to it meanwhile — before the next
  (`src/common/appbase.cpp:561-603`, `src/common/event.cpp:1368-1440`). `A->CallAfter(f1); B->CallAfter(f2);
  A->CallAfter(f3);` runs f1, f3, f2. A callback that keeps queueing new work on any handler keeps
  `ProcessPendingEvents` looping and native input and paint starve.
- **Destruction**: `~wxEvtHandler` removes itself from the app's list and deletes its queued events
  (`src/common/event.cpp:1234-1238`) — events posted to a destroyed handler are dropped, not delivered.
  `DeletePendingEvents` does not take `m_pendingEventsLock` (`src/common/event.cpp:1361-1366`): a worker
  posting to an object being destroyed races its destructor, and a worker holding a raw pointer to a
  dead target is undefined behaviour.

**Platforms** [source]:

| Port | Pending events (`CallAfter`, posted) | Idle events (`wxEVT_IDLE`, update-UI, deferred TLW deletes) |
|---|---|---|
| MSW | Keep running inside native modal loops (menu tracking, window move/size, `MessageBox`, common dialogs) through a `WH_GETMESSAGE` hook (`src/msw/window.cpp` `wxIdleWakeUpModule::MsgHookProc` → `wxApp::MSWProcessPendingEventsIfNeeded`, `src/msw/app.cpp:717-730`) | Not processed inside those native loops — do not defer repaint/layout to idle if it must happen during a live resize |
| macOS | Processed by a run-loop observer at `kCFRunLoopBeforeTimers` in common modes (`src/osx/core/evtloop_cf.cpp:86-112`) | At `kCFRunLoopBeforeWaiting`. While a native `wxMessageDialog`, `wxFileDialog` or `wxDirDialog` is modal, `wxCFEventLoopPauseIdleEvents` stops **both** (`src/osx/cocoa/msgdlg.mm:62`, `src/osx/cocoa/filedlg.mm:601`, `src/osx/cocoa/dirdlg.mm:123`, `src/osx/core/evtloop_cf.cpp:362-379`): a `CallAfter` queued before or during such a dialog runs only after it closes |
| GTK3 (default build, X11 and Wayland) / opt-out GTK2 | Pending events and idle run from one GLib idle source at `G_PRIORITY_LOW` (`src/gtk/app.cpp:102-153, 631`, `wxApp::DoIdle`): they starve while higher-priority sources (timers, redraw, input floods) are busy; a masked `YieldFor` removes that source before each iteration, so nothing queued runs inside it (`references/threads-timers-app.md` §Event categories and yields) | same source; `RequestMore()` or remaining pending events keep it installed — a busy loop |

**OrcaSlicer.** `GLCanvas3D::post_event` sets the event object and `wxPostEvent(m_canvas, …)` —
asynchronous, dispatched on the `wxGLCanvas`, where `Plater::priv` binds the canvas events.
`BackgroundSlicingProcess` posts with `wxQueueEvent(wxGetApp().mainframe->m_plater, evt.Clone())` and
`new wxCommandEvent(...)`; `SlicingProcessCompletedEvent` carries the worker's exception as a
`std::exception_ptr`. Worker → GUI patterns, `set_queue_on_main_fn`, Jobs:
`references/threads-timers-app.md`.

**Pitfalls.**
- **Rule:** From a worker, post only to an object that outlives the worker.
  ```cpp
  // Wrong: races ~wxEvtHandler, or uses a dangling pointer
  std::thread([panel] { wxQueueEvent(panel, new wxThreadEvent(EVT_DONE)); }).detach();
  // Right: post to the app and re-validate on the main thread
  std::thread([this, alive = m_alive] {
      wxGetApp().CallAfter([this, alive] { if (alive->load()) on_done(); });
  }).detach();
  ```
  Cite: `src/common/event.cpp:1234-1238, 1361-1366`.
- **Rule:** Use one target for steps that must run in order.
  **Why:** `this->CallAfter(a); wxGetApp().CallAfter(b);` gives no order between `a`'s handler queue
  and the app's.
- **Rule:** Retry and polling go through `wxTimer` (`StartOnce`), not a self-re-queuing `CallAfter`
  or `RequestMore()` (§12). Timers: `references/threads-timers-app.md`.

---

## 8. CallAfter and the liveness rule

**Contract** (`interface/wx/event.h:490-564`). `CallAfter(&T::method, args…)` — 0, 1 or 2 arguments;
"The method being called must be the method of the object on which CallAfter() itself is called" —
and `CallAfter(functor)`. "it is safe to use CallAfter() from other, non-GUI, threads, but … the
method will be always called in the main, GUI, thread context." Its documented purpose: actions that
"can't be performed inside their handlers, e.g. you shouldn't show a modal dialog from a mouse click
event handler as this would break the mouse capture state". The overview adds the alternative of
breaking the capture with `dialog.CaptureMouse(); dialog.ReleaseMouse();` when a dialog really must
be shown from such a handler (`docs/doxygen/overviews/eventhandling.h:878-901`). [source] That pair does not
break a capture taken through wx's capture stack: `ReleaseMouse` re-captures the previous holder
(`src/common/wincmn.cpp:3412-3416`), so release your own capture and `CallAfter` the dialog instead. Capture
rules: `references/mouse-keyboard-focus.md` §Mouse capture.

**Mechanics** [source]. Every overload is `QueueEvent(new wxAsyncMethodCallEvent…(this, …))` on the
object it is called on (`include/wx/event.h:3815-3855`), executed by the implicit entry in
`TryHereOnly` only when the event object is that handler (`src/common/event.cpp:1644-1667`).
Consequences:
- The functor is copied; method-form arguments are stored by value. Captures must be copyable.
- **Target destroyed first → the call is silently dropped**, and its captures are destroyed in
  `DeletePendingEvents` (`src/common/event.cpp:1234-1238`). A `CallAfter` queued from the main thread on a window
  needs no guard for that window itself.
- **`SetEvtHandlerEnabled(false)` drops pending calls**: `TryHereOnly` returns before the implicit
  entry (`src/common/event.cpp:1646-1648`), and the event is consumed.
- Pushed handlers and `wxEventBlocker` do not block `CallAfter` (it is dispatched on the object).
- A TLW after `Destroy()` is not deleted yet: it is put on `wxPendingDelete` and hidden (off macOS, unless
  it is the last visible TLW) (`src/common/toplvcmn.cpp:102-142`; wxOSX `src/osx/toplevel_osx.cpp`); pending events run before
  the idle-time delete, so `CallAfter`s queued on it still execute on the hidden window.
  `IsBeingDeleted()` is documented to cover "scheduled for destruction" (`interface/wx/window.h:3620-3633`)
  but stays `false` until the real delete, because `m_isBeingDeleted` is set only by
  `SendDestroyEvent` (`src/common/wincmn.cpp:535-557`). Use `wxTheApp->IsScheduledForDestruction(w)`
  (`interface/wx/app.h:221`) or an alive flag.
- `wxApp::CallAfter` (Orca: `wxGetApp().CallAfter`) always runs — the app outlives every window — so
  every object captured by pointer must be re-validated inside.

**The liveness rule.** Every `CallAfter` or other deferred lambda that touches a window, a model item
or any object other than the handler it was queued on must re-check liveness **inside** the lambda:
capture a `std::shared_ptr<std::atomic<bool>>` alive flag (set to `false` in the destructor), a
`std::weak_ptr` token or a `wxWeakRef`, or re-validate the target (registry lookup, model-item scan)
before touching it. Never capture a bare `this` or `wxDataViewItem` and trust it.
**Why:** the window or item can die between queueing and execution — dialog closed, plugin unloaded,
model rebuilt — and the deferred body then dereferences freed memory. Three fixes independently
added this guard. Self-queued work is the exception: `window->CallAfter(...)` is discarded unexecuted
when that window is destroyed, so the check is needed only for `wxGetApp().CallAfter(...)` and for
objects other than the window it was queued on. When a lambda touches only one window, queuing it on
that window gives the guard for free.
```cpp
// Wrong
CallAfter([this, item] { start_filament_editor(item); });

// Right — re-validate inside the lambda
CallAfter([this, item] {
    if (!is_live_model_item(item)) return;      // or: if (!alive->load()) return;
    start_filament_editor(item);
});

// Right — app-queued work with an alive flag
wxGetApp().CallAfter([this, alive = m_alive, payload]() {
    if (alive->load(std::memory_order_acquire))
        handle_web_command(payload);
});
```
Cite: 0a0d59b76b (`src/slic3r/GUI/PluginsDialog.cpp/.hpp`, `PluginsDialog::m_alive`,
`PluginsDialog::on_script_message`), c965b2a5b3 (`src/slic3r/GUI/GUI_ObjectList.cpp`,
`ObjectList::is_live_model_item`), b779a7bfed (`src/slic3r/plugin/host/PluginHostUi.cpp`, the
`UiRegistry::is_open` re-check in `ui_create_window`).

**Liveness tools.**

| Tool | Use when | Limits |
|---|---|---|
| Queue on the target itself (`win->CallAfter`) | The lambda touches only `win` | Main thread; still runs for a `Destroy()`ed TLW until the idle delete |
| `std::shared_ptr<std::atomic<bool>>` alive flag (`PluginsDialog::m_alive`) | App-queued work, any thread | Flips at the start of the derived destructor |
| `std::weak_ptr` token (`MachineObject::add_command_error_code_dlg` with `m_token`, `DeviceManager.cpp`) | Non-window objects | — |
| `wxWeakRef<T>` (`interface/wx/weakref.h`) | Main-thread checks of a `wxEvtHandler`/window | Reset in `~wxTrackable`, i.e. after the derived destructor and `DestroyChildren`; tracker list unsynchronised (`include/wx/tracker.h`) — create and test on the main thread only |
| Registry / model re-validation (`UiRegistry::is_open`, `ObjectList::is_live_model_item` in macOS-only code) | Ids or items that may be rebuilt | `is_live_model_item` compares node pointers, so it cannot detect a freed node whose address was reused |
| `wxTheApp->IsScheduledForDestruction(w)` | Detect a `Destroy()`ed TLW still in `wxPendingDelete` | — |
| `GUI_App::is_closing()` | Callbacks during shutdown | Check before posting and again inside the lambda |

Deletion and `wxWeakRef` details: `references/windows-dialogs.md`.

**OrcaSlicer.**
- `run_on_ui_blocking` (`src/slic3r/plugin/host/PluginHostUi.cpp`) captures `fn` and the promise **by
  reference** in `wxGetApp().CallAfter` and runs `fn` inline when `wxIsMainThread()` (a `CallAfter` +
  `future.get()` on the main thread would deadlock). It is safe only because the caller blocks until
  the lambda has run; never copy by-reference captures into a fire-and-forget `CallAfter`.
- Webview script messages arrive synchronously inside the native callback on WebKitGTK and WKWebView;
  defer window work from them with `CallAfter` plus an alive check, and in a `WebViewHostDialog`
  subclass do it in your own `on_script_message`, because the base dispatches synchronously. Details
  and cites (b779a7bfed, f2ccbfc8b5, 0a0d59b76b): `references/webview-gl-aui-media.md`.

**Pitfalls.**
- **Rule:** Do not use `IsBeingDeleted()` to detect a `Destroy()`ed TLW. Use an alive flag or
  `IsScheduledForDestruction`.
- **Rule:** Do not disable a handler with `SetEvtHandlerEnabled(false)` while it still has
  `CallAfter`s it needs (§14).

---

## 9. Custom events and payload ownership

**Declaring.** `wxDECLARE_EVENT(NAME, Class)` in the header, `wxDEFINE_EVENT(NAME, Class)` in exactly
one `.cpp` — "this is a definition so can't be in a header" (`docs/doxygen/overviews/eventhandling.h:634-640`).
[source] `wxDEFINE_EVENT` expands to `const wxEventTypeTag<T> NAME(wxNewEventType())`
(`include/wx/event.h:105-106`). A namespace-scope `const` has internal linkage unless an `extern`
declaration (`wxDECLARE_EVENT`) precedes it, so a header definition alone gives every translation unit
a **different** event type: binds and posts silently never meet, and nothing fails to link.
`wxDECLARE_EXPORTED_EVENT` exists only for DLL export (`include/wx/event.h:110-115`).

**Choosing the class** (`docs/doxygen/overviews/eventhandling.h:600-613`): `wxEvent` (no payload, no propagation) or
`wxCommandEvent` (int, long, string, client data; propagates). For richer payloads derive a class,
add members and implement `Clone() { return new MyEvent(*this); }` (`docs/doxygen/overviews/eventhandling.h:686-741`);
event-table macros need extra boilerplate, `Bind` does not. `wxEvent::Clone()` is pure virtual
(`include/wx/event.h:1009`), so a direct `wxEvent` subclass without `Clone` does not compile — but
a subclass of a concrete event (`wxCommandEvent::Clone` returns `new wxCommandEvent(*this)`,
`include/wx/event.h:1655`) silently inherits a slicing `Clone`.

**Payload ownership.**

| Payload | Ownership | Cite |
|---|---|---|
| `wxCommandEvent::SetString/SetInt/SetExtraLong` | Copied / by value | `interface/wx/event.h` wxCommandEvent |
| `wxCommandEvent::SetClientData(void*)` | Raw pointer, never owned | — |
| `wxCommandEvent::SetClientObject(wxClientData*)` | "not owned by the event … must be owned and deleted by another object (e.g. a control) that has longer life time than the event object" | `interface/wx/event.h:2210-2216` |
| `wxEvtHandler::SetClientObject(wxClientData*)` | Owned by the handler: "Any previous object will be deleted"; deleted in `~wxEvtHandler`; do not mix with `SetClientData` on the same handler | `interface/wx/event.h:1062-1074`; [source] `src/common/event.cpp:1240-1242` |
| `Bind(…, userData)` | Owned by the binding, deleted on unbind or at exit | `interface/wx/event.h:900-905` |
| `wxThreadEvent::SetPayload<T>` | Copied; T's copy must not share state | `interface/wx/event.h:3775-3790` |

[source] For a `wxEVT_TEXT` event whose event object is a text-entry window,
`wxCommandEvent::GetString()` always returns the control's **current** value and ignores the stored
string (`src/common/event.cpp:430-447`). The copy constructor materialises the stored string
(`include/wx/event.h:1622-1632`), but a queued or cloned copy still has the event object, so its
`GetString()` reads the control live at handling time. If the value at emit time matters, capture it
yourself.

**OrcaSlicer.**
- Custom event types are declared in headers (`GLCanvas3D.hpp` and `Plater.hpp` in
  `Slic3r::GUI`; widget events such as `EVT_SPINCTRL_TEXT`, `wxEVT_TAB_SEL_CHANGED` at global scope)
  and defined once in the matching `.cpp`.
- Payload classes in `src/slic3r/GUI/Event.hpp`: `SimpleEvent`, `IntEvent` (`get_data()`),
  `Event<T>` (`.data`), `ArrayEvent<T, N>` (`.data`). All derive from `wxEvent`, propagate (§5), and
  their `Clone()` copies type, data and event object (not the id). `LoadPrinterViewEvent` there shows
  the `wxCommandEvent`-derived shape: a copy constructor that copies the extra member, and
  `Clone() { return new LoadPrinterViewEvent(*this); }`.
  ```cpp
  wxDECLARE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>);   // GLCanvas3D.hpp
  wxDEFINE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>);    // GLCanvas3D.cpp
  post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, +1));    // GLCanvas3D, posts on m_canvas
  view3D_canvas->Bind(EVT_GLCANVAS_INCREASE_INSTANCES, [this](Event<int>& e) { /* e.data */ });
  ```
- Simple dialog-level events use `wxCommandEvent` with `SetInt`/`SetString`/`SetEventObject`
  (`ReleaseNote.cpp` `EVT_SECONDARY_CHECK_*`; `MsgDialog::show_dsa_button` posts
  `EVT_CHECKBOX_CHANGE` with `wxPostEvent(this, event)`).

**Pitfalls.**
- **Rule:** Never put `wxDEFINE_EVENT` in a header.
  ```cpp
  // Wrong (header): one event type per .cpp that includes it
  wxDEFINE_EVENT(EVT_MY_THING, wxCommandEvent);
  // Right
  wxDECLARE_EVENT(EVT_MY_THING, wxCommandEvent);   // header
  wxDEFINE_EVENT(EVT_MY_THING, wxCommandEvent);    // one .cpp
  ```
- **Rule:** A posted custom event class overrides `Clone()` and copies every payload member.
  **Why:** `wxPostEvent`/`wxQueueEvent(evt.Clone())`/`AddPendingEvent` clone the event; an inherited
  `Clone` slices it to the base class, and the handler's cast to the derived type reads garbage.

---

## 10. Window and event ids

**Contract.**
- `wxID_ANY` in a constructor asks wx for an id; automatic ids "are always negative and so will never
  conflict with the user-specified identifiers which must be always positive". Custom ids belong
  above `wxID_HIGHEST` or below `wxID_LOWEST` (`docs/doxygen/overviews/eventhandling.h:863-875`),
  "should not have values 0 or 1", and "they are not needed when using wxEvtHandler::Bind()" if you
  bind on the control (`docs/doxygen/overviews/windowids.h:32-42`). Stock ids span `wxID_LOWEST`
  (5000) to `wxID_HIGHEST` (6000) (`include/wx/defs.h:1786-1787, 1943`).
- `wxNewId()`: "@deprecated Ids generated by it can conflict with the Ids defined by the user code,
  use wxID_ANY …" (`interface/wx/utils.h:433-444`). The header does not mark it deprecated, so there is
  no compiler warning. [source] It starts at 100, skips the stock range and never reuses an id
  (`src/common/utilscmn.cpp:654-663`).
- `wxWindow::NewControlId(count)` reserves auto ids (`interface/wx/window.h:4348-4376`; main thread).

**Platforms** [source]. `wxUSE_AUTOID_MANAGEMENT` defaults ON for Windows builds and OFF elsewhere
(`build/cmake/options.cmake:520-528`). With it (MSW), auto ids are reference-counted in
`-32000..-2000`, handed out upward from -32000, and once that range has been used up the allocator
scans for ids whose window or menu item is gone and **reuses** them — contradicting
`docs/doxygen/overviews/windowids.h:28-30` ("an ID that had never been returned by this function
before"); without it (macOS, GTK) they count down from -2000 to -1000000 and then wrap unchecked
(`src/common/windowid.cpp:187-251`, `include/wx/defs.h:1755-1772`). `wxMenuItem` holds its id as a `wxWindowIDRef`
(`include/wx/menuitem.h`). MSW native ids are 16-bit: `CreateBase` accepts `wxID_ANY`, `0..32766` or
the auto range (`src/common/wincmn.cpp:369-375`; the assert is compiled out in Orca, so an
out-of-range id fails silently).

**OrcaSlicer.**
- `append_menu_item`/`append_submenu` (`src/slic3r/GUI/wxExtensions.cpp`) assign `wxNewId()` for
  `wxID_ANY` and bind handlers filtered by that id: `append_menu_item` binds `wxEVT_MENU` on the menu
  (dies with it; on MSW on a non-null `event_handler` instead), and with a non-null `parent` both helpers bind `wxEVT_UPDATE_UI` on `parent`, never
  unbound. That is safe only because `wxNewId` ids are never reused and the menus are built once; switching them
  to `NewControlId` (recycled on MSW) would let stale handlers fire for new items. Menu event routing:
  `references/popups-menus.md`.
- **Field window pools** (`src/slic3r/GUI/Field.cpp`, `Builder::build`, `free_window`; not on GTK,
  where windows are deleted): option widgets are reused across page rebuilds, and `free_window`
  unbinds every dynamic entry that was bound with an explicit id (on the widget and its inner
  `wxTextCtrl`). Convention: `Field` handlers on pooled widgets pass the control id as the `Bind` id
  (`…, temp->GetId())`); a handler bound without an id survives into the next `Field` that reuses the
  widget and dangles. Widget-internal handlers use `wxID_ANY` and survive by design.
  `GUI_App::recreate_GUI` releases the old pools through a `wxClientData` it hands to the old
  `MainFrame` with `SetClientObject`, so they are freed when that frame's `~wxEvtHandler` runs (§9).

**Pitfalls.**
- **Rule:** Bind on the emitting control, not on an ancestor filtered by id.
  **Why:** with recycled ids (MSW) a stale ancestor binding fires for an unrelated new control; an
  unfiltered ancestor binding catches every descendant.
  ```cpp
  // Wrong
  panel->Bind(wxEVT_BUTTON, &MyPanel::on_ok, this, ok_btn->GetId());
  // Right
  ok_btn->Bind(wxEVT_BUTTON, &MyPanel::on_ok, this);
  ```

---

## 11. wxEVT_UPDATE_UI

**Contract** (`interface/wx/event.h:2444-2523`). Update-UI pseudo-events are sent "in idle time" from
`wxWindow::OnInternalIdle`; handlers call `evt.Enable/Check/Show/SetText`. Popup menus are updated
just before showing (`wxMenu::UpdateUI`). "On Windows and GTK+, events for menubar items are only
sent when the menu is about to be shown, and not in idle time." Default mode
`wxUPDATE_UI_PROCESS_ALL`, interval 0. Throttle with `wxUpdateUIEvent::SetMode(wxUPDATE_UI_PROCESS_SPECIFIED)`
plus `wxWS_EX_PROCESS_UI_UPDATES` on the windows that need it, or `SetUpdateInterval(ms)` with
`UpdateWindowUI()` at critical points (`interface/wx/event.h:2470-2479`; `interface/wx/window.h:286-288`).

**Source facts** [source].
- Each idle pass calls `UpdateWindowUI` for every window whose parent is shown on screen
  (`src/common/wincmn.cpp:2810-2814`, `src/common/event.cpp:499-530` `wxUpdateUIEvent::CanUpdate`).
  The event is a command event: unhandled, it bubbles to the TLW and `wxApp`, and every hop scans
  that handler's whole dynamic table. Many `parent->Bind(wxEVT_UPDATE_UI, …, id)` entries on a frame
  tax every idle pass.
- Menubar update-on-open applies to macOS too: `wxUSE_IDLEMENUUPDATES` is 0 for MSW, GTK and OSX
  (`include/wx/platform.h:535-545`), except wxGTK with a global menu bar, which falls back to idle
  updates (`src/common/framecmn.cpp:66-79`).

**Pitfall.**
- **Rule:** Keep update-UI handlers to cheap state reads; never do layout, I/O or model scans in them.

---

## 12. Idle events

**Contract** (`interface/wx/event.h:4383-4441`). Idle events are sent once when the loop becomes idle;
a continuous stream needs `RequestMore()` or `wxWakeUpIdle()`, and "both of these approaches (and
especially the first one) increase the system load". `wxIdleEvent::SetMode(wxIDLE_PROCESS_SPECIFIED)`
plus `wxWS_EX_PROCESS_IDLE` limits recipients. Documented: "The children of hidden windows do not
receive idle events". The "delayed action" idiom binds an idle handler that unbinds itself;
`CallAfter` is simpler.

**Source facts** [source].
- **Contradicts the doc:** `wxWindowBase::SendIdleEvents` recurses into every child without a
  visibility check (`src/common/wincmn.cpp:2783-2808`); only update-UI skips children of hidden
  windows (`src/common/event.cpp:508-513`). Idle handlers on hidden panels run.
- One `wxIdleEvent` object is reused for all windows in a pass; TLWs pending deletion are skipped,
  and `DeletePendingObjects` runs from the app's idle processing (`src/common/appcmn.cpp:408-429`,
  `src/common/appbase.cpp:442-462`). `wxYield()` runs pending events and one idle pass
  (`src/common/evtloopcmn.cpp:172-192`) — so it can delete `Destroy()`ed TLWs under your feet.

**OrcaSlicer.** `DropDown::Create` binds an empty, non-skipping `wxEVT_IDLE` handler on macOS.
Because dynamic handlers run before the static table, this shadows `wxPopupTransientWindow::OnIdle`
(static table, `src/common/popupcmn.cpp:108-112, 439-471`) and its idle-time capture juggling, so the
capture taken in `Show` is held until the drop-down hides. `FanControlPopupNew` binds the same no-op,
but it is a `wxDialog`, which has no such idle handler to shadow. Popup mechanics:
`references/popups-menus.md`.

**Pitfall.**
- **Rule:** Use `wxTimer` (`StartOnce`) or `CallAfter` instead of idle handlers for periodic or
  delayed work.
  **Why:** idle fires once per wake-up, does not run inside MSW native modal loops (§7), runs for
  hidden panels, and `RequestMore()` busy-loops a core.

---

## 13. Event filters

**Contract.** `wxApp::FilterEvent` and any `wxEventFilter` registered with
`wxEvtHandler::AddFilter` run for every event before anything else, in LIFO order, with wxApp
registered by default (`interface/wx/event.h:1195-1214`). Return `Event_Skip` (-1) to continue,
`Event_Ignore` (0) or `Event_Processed` (1) to stop (`interface/wx/eventfilter.h:85-93`). "having event
filters adds additional overhead to every event … return as quickly as possible"
(`interface/wx/eventfilter.h:18-20`). A standalone filter must be removed with `RemoveFilter` before it is
destroyed (`interface/wx/eventfilter.h:108-114`; misuse is silent in Orca).

**OrcaSlicer.**
- `GUI_App::FilterEvent` only timestamps user input (non-command `wxEVT_CATEGORY_USER_INPUT` events
  and main-frame size events) and always returns `Event_Skip` — keep it that cheap.
- `SplashScreen::FilterEvent` (in `GUI_App.cpp`) returns `wxEventFilter::Event_Skip` to disable
  `wxSplashScreen`'s own filter, which `Close()`s (and so `Destroy()`s) the splash on any key or mouse
  press (`src/generic/splash.cpp:40-45, 105-126`) while `GUI_App::on_init_inner` still holds it
  (as a `wxWeakRef<SplashScreen>`). Cite: 4088a36095.

---

## 14. Pushed handlers, wxEventBlocker, SetEvtHandlerEnabled

**Contract.**
- `PushEventHandler(h)`: `h` must not be part of another chain; events reach the most recently pushed
  handler first and the window last (`interface/wx/window.h:2779-2809`). `PopEventHandler(deleteHandler
  = false)` — an error with nothing pushed (`interface/wx/window.h:2759-2777`); `RemoveEventHandler(h)` removes from
  the middle (`interface/wx/window.h:2811-2827`). `SetNextHandler` on a window is not supported — windows use the
  stack (`interface/wx/window.h:2843-2851`).
- `wxEventBlocker(win, type = wxEVT_ANY)` discards events of the given types directed to `win`; `win`
  "must remain alive until the wxEventBlocker object destruction" (`interface/wx/event.h:287-337`).
- `SetEvtHandlerEnabled(false)`: the handler's dynamic and static tables are skipped, processing
  resumes at the chain step (`docs/doxygen/overviews/eventhandling.h:466-471`).

**Source facts** [source].
- **Pop before destroy.** `~wxWindowBase` asserts "any pushed event handlers must have been removed"
  (`src/common/wincmn.cpp:468-472`) — compiled out in Orca, so a forgotten pushed handler is a
  dangling pointer, not an assert.
- `wxEventBlocker` is a pushed handler whose `ProcessEvent` returns `true` for blocked types
  (`src/common/event.cpp:2075-2103`). Its destructor pops whatever is on top; pushing another handler
  inside its scope corrupts the stack. It only sees events dispatched through `GetEventHandler()` —
  not posted events, `CallAfter`, or a direct `win->ProcessEvent`. Blocked command events count as
  processed and do not propagate.
- `SetEvtHandlerEnabled(false)` does not affect handlers bound on other objects for this window's
  events, validators, the chain, or propagation — and it drops queued `CallAfter`s (§8).
  `wxWindow::Enable(false)` only stops native input; posted and synthetic events still reach the
  handlers (`src/common/event.cpp:1644-1648` checks only the handler flag).

**OrcaSlicer.**
- `StateHandler` (`src/slic3r/GUI/Widgets/StateHandler.cpp`) is a `wxEvtHandler` pushed on every
  `StaticBox`-based widget (member `StaticBox::state_handler`) and on attached children
  (`attach_child`). It binds on itself, tracks enabled/checked/focused/hovered/pressed, always
  Skips, and removes itself with `RemoveEventHandler` in its destructor. Member destruction runs
  before `~wxWindowBase`, so it is gone in time. Call `remove_child(child)` before destroying an
  attached child separately. Emitting through `GetEventHandler()` (§6) is what lets it see
  synthesized events such as `EVT_ENABLE_CHANGED`.
- `PrinterWebView::~PrinterWebView` (`PrinterWebView.cpp`) and `WebViewPanel::~WebViewPanel`
  (`WebViewDialog.cpp`) call
  `SetEvtHandlerEnabled(false)` first, so events raised while the webview and members are torn down
  do not reach handlers that touch freed members.

**Pitfall.**
- **Rule:** Pop pushed handlers in reverse push order, before the window dies.
  ```cpp
  // Wrong: blocker2 pops blocker1's entry (or a foreign handler) — stack corrupted, no assert in Orca
  auto* b1 = new wxEventBlocker(win); wxEventBlocker b2(win); delete b1;
  // Right: nested scopes
  { wxEventBlocker b1(win); { wxEventBlocker b2(win, wxEVT_TEXT); /* … */ } }
  ```

---

## 15. Exceptions in handlers

**Contract.** `wxApp::OnExceptionInMainLoop` returns `true` to continue the loop or `false` to exit;
the default is to exit "in all ports except under Windows where a dialog is shown"; if it rethrows and
the exception cannot be stored, the program terminates (`interface/wx/app.h:465-480`). Since 3.3.0 the
system option `catch-unhandled-exceptions` set to 0 (environment variable
`wx_catch_unhandled_exceptions=0`) stops wx from catching unhandled exceptions, so the default abort
happens and the backtrace or crash dump is more likely to show where the exception came from
(`interface/wx/sysopt.h:17-23, 40-49`).

[source] If `OnExceptionInMainLoop` throws, `wxEvtHandler::WXConsumeException` exits the current loop
and stores the exception or aborts; the wx comment explains that exceptions "can't propagate through
the C GTK+ code and corrupt the stack" (`src/common/event.cpp:1688-1749`).

**OrcaSlicer.** `GUI_App::OnExceptionInMainLoop` calls `generic_exception_handle()`, whose every
path terminates or rethrows, so its `return false` is never reached: `std::bad_alloc` and
`boost::io::bad_format_string` show a message box and terminate; other `std::exception`s are logged,
reported with `wxLogError` and rethrown (non-`std` exceptions escape unlogged), so the main loop exits
and `wxEntry` ends with the fatal exit code. An exception that escapes any handler ends the session.
Unwinding order and exit code: `references/threads-timers-app.md` §Exceptions in the main loop.

**Pitfall.**
- **Rule:** Catch inside any handler (and any `CallAfter` lambda) that can throw — file, network,
  JSON, config parsing — and report the error there; never let an exception cross a native callback.

---

## 16. How Orca widgets emit events

The per-widget table (event types, ids and event objects, which setters emit, where to bind) is
`references/orca-widgets.md` §Event semantics, with per-widget binding pitfalls in
`references/controls-dataview.md` §Orca replacement widgets. The event mechanics behind them:

- **Synchronous, through the handler stack.** Orca widgets emit with `GetEventHandler()->ProcessEvent` (§6), so
  the pushed `StateHandler` (§14) sees the event and command events propagate to ancestors (§5). Exceptions:
  `SwitchBoard` posts `wxCUSTOMEVT_SWITCH_POS` with `wxPostEvent(this, …)` from `SwitchBoard::on_left_down`
  (asynchronous: the handler runs after the click handler returns; no event object, id 0), and `TempInput` posts
  `wxCUSTOMEVT_SET_TEMP_FINISH` to its parent (`TempInput::SetFinish`).
- **Local re-dispatch.** `TextInput` and `SpinInput` catch the inner control's `wxEVT_TEXT_ENTER`/
  `wxEVT_KILL_FOCUS` and re-send them with the wrapper's id through `ProcessEventLocally` (§6): the wrapper's
  handlers run, its parents never see them. The inner `wxEVT_TEXT` propagates normally, with the inner control's id
  and object, so bind it on the wrapper with `wxID_ANY`, not the wrapper's id.
- **Internal handlers run after yours** (§4). `Button` keeps LEFT_DOWN/LEFT_UP/MOUSE_CAPTURE_LOST/KEY_DOWN/KEY_UP/
  PAINT in its static table (`wxEVT_BUTTON` comes from `Button::mouseReleased`; Space/Enter synthesize LEFT_DOWN/UP
  in `Button::keyDownUp`); `::CheckBox` and `SwitchButton` bind their own `wxEVT_TOGGLEBUTTON` handler in the
  constructor (clears the half state, refreshes the bitmap, Skips); `TextInput`'s handlers on the inner control run
  `OnEdit` and the re-dispatch. A user handler bound on the widget (or on `GetTextCtrl()`) for those events must
  `Skip()`. Capture-lost handling: `references/mouse-keyboard-focus.md`.
- **`EVT_ENABLE_CHANGED`** (`StateHandler.hpp`, a `wxCommandEvent` with id 0) comes from the `Enable` overrides of
  `Button`, `SpinInput`, `RadioGroup` and the other `StateHandler` widgets. `StateHandler` Skips it, so it
  propagates to ancestors like any command event.
- **Id-less events still propagate.** `ComboBox`'s `wxEVT_COMBOBOX_DROPDOWN`/`_CLOSEUP` (id 0, no event object),
  `RadioGroup`'s `wxEVT_RADIOBOX` (`wxEVT_COMMAND_RADIOBOX_SELECTED` is an alias, `include/wx/event.h:4910`; no
  event object) and `EVT_ENABLE_CHANGED` reach every ancestor bound by type. Bind on the specific widget, never on
  an ancestor filtered by its id (§10).
- **Popup relay.** `DropDown` emits `wxEVT_COMBOBOX` on itself (`sendDropDownEvent`, its own id and object); as a
  `PopupWindow` it stops an unconsumed event on MSW/macOS only (§5). `ComboBox` binds it on the `DropDown`,
  consumes it and re-emits it with its own id and object.
- **Emitting setters.** `RadioGroup::SetSelection` (every call, same index included), `MultiSwitchButton::SetSelection`,
  `TabCtrl::SelectItem` (CHANGING carries the old index and cannot veto: `sendTabCtrlEvent` always returns `true`),
  `SpinInput::SetValue` (`EVT_SPINCTRL_TEXT` + `wxEVT_TEXT`), `Notebook::SetSelection` (PAGE_CHANGING/CHANGED, as in
  wx) and, in an editable or `CB_NO_TEXT` combo, `ComboBox::SetSelection`/`SetValue` (`wxEVT_TEXT` through
  `ComboBox::SetLabel`) emit synchronously. Guard model→view refreshes (`references/controls-dataview.md` §Events from programmatic changes).
