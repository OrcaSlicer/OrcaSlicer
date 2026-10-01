# Mouse, keyboard and focus

How input reaches wx windows in the wxWidgets 3.3.2 build Orca ships, and the Orca conventions on
top: mouse capture, mouse and key events, accelerators and Orca's shortcut registry, focus,
tooltips and cursors. Read it when a widget captures the mouse or tracks hover, when adding or
changing a keyboard shortcut, when touching focus, tooltip or cursor code, and when debugging an
"alive but unclickable" UI, lost or phantom clicks, or shortcuts that fire while typing.

wx asserts are compiled out in Orca (`wxDEBUG_LEVEL=0`), so every misuse below that wx documents as
an assert fails silently. "GTK" means wxGTK3, Orca's Linux default (X11 and Wayland); GTK2 is only
an opt-out build (`-DDEP_WX_GTK3=OFF`), noted where it differs. Paths starting `interface/`,
`include/`, `src/`, `docs/` are in the wx tree
(`find deps -maxdepth 5 -type d -path '*dep_wxWidgets-prefix/src/dep_wxWidgets'`); Orca paths are
relative to `src/slic3r/GUI/`.

Contents: [Rules](#rules) · [Mouse capture](#mouse-capture) · [Mouse events](#mouse-events) ·
[Global pointer position and Wayland](#global-pointer-position-and-wayland) ·
[Hover handlers and enter/leave feedback loops](#hover-handlers-and-enterleave-feedback-loops) ·
[Keyboard events](#keyboard-events) · [Accelerators and menu shortcuts](#accelerators-and-menu-shortcuts) ·
[Orca's shortcut registry](#orcas-shortcut-registry) · [Focus](#focus) · [Tooltips](#tooltips) ·
[Cursors](#cursors)

## Rules

1. Capture only with `if (!HasCapture()) CaptureMouse();` and release only with
   `if (HasCapture()) ReleaseMouse();`, at every site. → [Capture stack](#the-capture-stack)
2. On button-up, release whenever `HasCapture()` is true, never gated on a gesture flag that other
   code can clear; decide whether to *commit* separately. → [Cancel-on-lost pattern](#the-cancel-on-lost-pattern)
3. Every window that captures handles `wxEVT_MOUSE_CAPTURE_LOST` by *cancelling*: reset state and
   `Refresh()`. No commit, no `Skip()`, no `CaptureMouse()`, no unguarded `ReleaseMouse()`. Never route it
   through the mouse-up/commit path. → [Cancel-on-lost pattern](#the-cancel-on-lost-pattern)
4. Release capture before `Hide()`, `Destroy()`/`delete` and in the destructor. →
   [While captured](#modal-dialogs-popups-and-destruction-while-captured)
5. Never show a modal dialog, popup or message box from a handler that runs while the mouse is
   captured: release your own capture, then `CallAfter` the dialog. →
   [While captured](#modal-dialogs-popups-and-destruction-while-captured)
6. Write capture code as if `wxEVT_MOUSE_CAPTURE_LOST` did not exist on macOS (it is never sent there);
   a leaked capture freezes every click in the app. → [macOS](#macos-rerouting-and-the-frozen-ui-diagnosis)
7. Mouse handlers take `wxMouseEvent&` (never by value) and `Skip()` `wxEVT_LEFT_DOWN` so focus still
   moves. → [Button state and clicks](#button-state-and-clicks)
8. A handler that counts presses binds `wxEVT_LEFT_DCLICK` too: the second press of a double click
   is a DCLICK, not a DOWN, on every port. → [Button state and clicks](#button-state-and-clicks)
9. Wheel: divide `GetWheelRotation()` by `GetWheelDelta()`, filter `GetWheelAxis()`, accumulate for
   discrete steps. → [Wheel](#wheel)
10. On macOS, read button state on non-button events (motion, enter/leave, up, wheel) from
    `wxGetMouseState()`, not the event. → [Button state and clicks](#button-state-and-clicks)
11. No global screen coordinates in logic that must work on Linux: no `wxGetMousePosition()`,
    `wxFindWindowAtPoint()` or cross-window screen-rect tests. Use the event's client position,
    `GetClientRect()` and focus tracking; guard unavoidable uses with `is_running_on_wayland()`. →
    [Global pointer](#global-pointer-position-and-wayland)
12. Never use `wxGetKeyState()` for non-modifier keys (always false on Wayland); track
    `wxEVT_KEY_DOWN`/`wxEVT_KEY_UP`. → [Global pointer](#global-pointer-position-and-wayland)
13. Enter/leave handlers only record hover state, per child window. Apply `Show()`/`Hide()` +
    `Layout()` from `wxEVT_IDLE` or `CallAfter`, only when the state differs. No `wxFindWindowAtPoint()`
    there, and no `IsShownOnScreen()` as a guard. → [Hover](#hover-handlers-and-enterleave-feedback-loops)
14. To see a child's keys, bind `wxEVT_CHAR_HOOK` on the parent and `Skip()` everything not
    handled; `wxEVT_KEY_DOWN`/`wxEVT_CHAR` do not propagate. → [wxEVT_CHAR_HOOK](#wxevt_char_hook)
15. A top-level `wxEVT_CHAR_HOOK` must not consume printable keys, Space or text-editing chords
    (Ctrl+A/C/V/X/Z, Delete, Backspace, Home/End, arrows) while a text entry has focus. →
    [wxEVT_CHAR_HOOK](#wxevt_char_hook)
16. Test modifiers with `GetModifiers() == wxMOD_…`, not `ControlDown()`; on macOS `wxMOD_CONTROL` is
    Cmd and `wxMOD_RAW_CONTROL` is the Control key. → [Modifiers](#modifiers-and-the-cmd-mapping)
17. Match letters, digits and special keys on `wxEVT_KEY_DOWN` and punctuation on `wxEVT_CHAR`;
    build chords with `KeyChord::from_event`. → [Key codes](#key-codes)
18. New user-facing shortcuts go through the registry (`Shortcut` enum + `shortcut_table` row + a case in
    the context's dispatcher). No raw `wxAcceleratorTable`, hard-coded key test or literal key name in a
    label. → [Registry](#orcas-shortcut-registry)
19. Global chords need Ctrl/Alt or a key that types nothing. On macOS a live menu accelerator is
    consumed by the menu bar before any wx key event, so the menu handler and the dispatcher case
    must do the same thing. → [Accelerators](#accelerators-and-menu-shortcuts)
20. `SetAcceleratorTable()` replaces the window's table; build one table per window. It fires only
    while focus is inside that window and stops at the top-level window. → [Accelerators](#accelerators-and-menu-shortcuts)
21. `SetFocus()` only on a user action or when the top-level window `IsActive()`; never from hover or
    timers unconditionally, never inside `wxEVT_KILL_FOCUS` (defer with `CallAfter`). Focus handlers
    `Skip()`. → [Focus](#focus)
22. Clear a tooltip with `UnsetToolTip()`, not `SetToolTip("")`. A composite that forwards tooltips
    overrides `DoSetToolTip` as well as `DoSetToolTipText`. → [Tooltips](#tooltips)
23. Tooltips on disabled controls only through the MSW-gated parent-motion hack
    (`Button::EnableTooltipEvenDisabled`); never install parent-motion forwarding on macOS. →
    [Tooltips](#tooltips-on-disabled-controls)
24. Set a window cursor once with `SetCursor()`, reset with `wxNullCursor`, never toggle it on
    enter/leave. Busy cursors via `wxBusyCursor` RAII, on the main thread. → [Cursors](#cursors)

## Mouse capture

### Contract

`CaptureMouse()` "Directs all mouse input to this window". wx "maintains the stack of windows having
captured the mouse … you must release the mouse as many times as you capture it, unless the window
receives the wxMouseCaptureLostEvent event. Any application which captures the mouse in the
beginning of some operation must handle wxMouseCaptureLostEvent and cancel this operation when it
receives the event. The event handler must not recapture mouse." (`interface/wx/window.h:3805-3819`)

`wxMouseCaptureLostEvent` goes to **all windows on the capture stack** when capture is lost to an
"external" event (a dialog box shown, another application capturing the mouse). It is "not sent if
the capture changes because of a call to CaptureMouse or ReleaseMouse"
(`interface/wx/event.h:3496-3514`). The doc's "currently emitted under Windows only" /
`@onlyfor{wxmsw}` is stale: wxGTK sends it too, wxOSX never does (table below).
`wxMouseCaptureChangedEvent` is MSW-only and is sent to a window that loses capture "even if
wxWindow::ReleaseMouse was called by the application code"; the doc's purpose: "allows an application
to cater for unexpected capture releases" (`interface/wx/event.h:4659-4680`; only `src/msw/window.cpp`
`wxWindowMSW::HandleCaptureChanged` builds it).

### The capture stack

[source] `src/common/wincmn.cpp:3322-3456` (namespace `wxMouseCapture`; the stack is a
`wxVector`, i.e. `std::vector`):

| Call | Behaviour |
|---|---|
| `CaptureMouse()` (:3352) | Asserts (compiled out) if `this` is already anywhere on the stack. Natively releases the current holder, `DoCaptureMouse()`, pushes `this`. A second call on the same window pushes it **twice**. |
| `ReleaseMouse()` (:3371) | Calls `DoReleaseMouse()` **first**, dropping the native capture whoever owns it. Then `wxCHECK_RET(stack.back() == this)` returns silently if this window is not on top. Pops, and if the stack is non-empty **re-captures the new top** (:3412-3416). |
| `HasCapture()` | `AsWindow() == GetCapture()` (`include/wx/window.h:1125`): true only for the top of the stack. |
| `NotifyCaptureLost()` (:3438) | Does nothing while a wx `Capture/ReleaseMouse` is in progress. Otherwise sends the lost event to each stacked window, top first, popping each. The stack ends empty, so no `ReleaseMouse()` is owed afterwards. A handler that leaves the event unprocessed (`Skip()`) hits a compiled-out `wxFAIL` (:3423-3436). |
| `~wxWindowBase` (:452) | Asserts (compiled out) if the window is still on the stack, and does **not** remove it: the stack keeps a dangling pointer. |

Consequences:
- Double capture followed by one release leaves the **same window captured** (the restore step
  re-captures it). That is a leaked capture.
- An unguarded `ReleaseMouse()` from a window that is not on top frees the real owner's native
  capture and leaves the stack out of step; a later release can re-capture a stale window.
- An unguarded `ReleaseMouse()` inside the lost handler pops the stack while `NotifyCaptureLost`
  is iterating it. The loop then pops an entry it never notified, or calls `pop_back()` on an
  empty vector (undefined behaviour). A `HasCapture()`-guarded release is a harmless no-op there
  (next table).
- The documented way to break capture before a modal, `dialog.CaptureMouse(); dialog.ReleaseMouse();`
  (`docs/doxygen/overviews/eventhandling.h:878-901`), is undone by the restore step when the
  current holder captured through wx: the release re-captures the previous holder. It only breaks
  a capture taken natively, outside wx's stack.

### Per-port delivery

| | MSW | GTK3 / GTK2 | macOS |
|---|---|---|---|
| Native capture | `SetCapture` | `gdk_seat_grab` (GTK ≥ 3.20) or `gdk_pointer_grab`. On an unrealized window `DoCaptureMouse` fails a silent `wxCHECK_RET`, but `wincmn` still pushes it (`src/gtk/window.cpp:6732-6765`) | None. `DoCaptureMouse` sets `wxApp::s_captureWindow` and wx reroutes events (`src/osx/window_osx.cpp:596-612`) |
| Capture-lost sent | `WM_CAPTURECHANGED` → `HandleCaptureChanged` → `NotifyCaptureLost`, then `wxEVT_MOUSE_CAPTURE_CHANGED` (`src/msw/window.cpp:5176-5192`) | GTK `grab-broken-event` (`src/gtk/window.cpp:2636-2646`). `wxDialog::ShowModal` and `wxMessageDialog::ShowModal` release the grab and notify (`src/gtk/dialog.cpp:137`, `src/gtk/msgdlg.cpp:285` → `GTKReleaseMouseAndNotify`) | **Never.** `src/osx` has no `NotifyCaptureLost` call |
| `HasCapture()` inside the lost handler | false: `GetCapture()` returns null while `gs_insideCaptureChanged` (`src/msw/window.cpp:757-768`) | false: `g_captureWindow` is cleared before notifying (`src/gtk/window.cpp:6794-6815`) | n/a |
| Modal dialog shown while captured | Lost event only if something takes the native capture (`WM_CAPTURECHANGED`) | Capture released, lost event sent | Capture kept; the dialog cannot be clicked (`src/osx/dialog_osx.cpp` `ShowModal` has no capture code) |
| Captor destroyed | wx stack dangles | `g_captureWindow` cleared (`src/gtk/window.cpp:3088-3089`); wx stack dangles | `s_captureWindow` dangles; the next mouse event dereferences it |
| `wxEVT_CHAR_HOOK` while captured | Not generated (any native `::GetCapture()`) | Not generated | Generated |
| Enter/leave while captured | Synthesized for the captor only (`src/msw/window.cpp:6019-6074`) | Synthesized for the captor only; grab crossings ignored (`src/gtk/window.cpp:2073-2117, 2384-2456`) | Other views' enter/exit events are rerouted to the captor |

### The cancel-on-lost pattern

```cpp
// EVT_MOUSE_CAPTURE_LOST(MyWidget::mouseCaptureLost) in the event table, or Bind(...)
void MyWidget::mouseDown(wxMouseEvent& e)
{
    e.Skip();                               // let focus move
    m_pressed = true;
    if (!HasCapture()) CaptureMouse();      // a second button mid-press must not push twice
    Refresh();
}
void MyWidget::mouseReleased(wxMouseEvent& e)
{
    e.Skip();
    if (HasCapture()) ReleaseMouse();       // always, not only when m_pressed
    if (!m_pressed) return;
    m_pressed = false;
    Refresh();
    if (GetClientRect().Contains(e.GetPosition()))
        sendButtonEvent();                  // commit only on a real button-up inside
}
void MyWidget::mouseCaptureLost(wxMouseCaptureLostEvent&)
{
    m_pressed = false;                      // cancel: no commit, no Skip(),
    Refresh();                              // no CaptureMouse(), no unguarded ReleaseMouse()
}
MyWidget::~MyWidget() { if (HasCapture()) ReleaseMouse(); }
```

Copy the `HasCapture()` guards from `Widgets/Button.cpp` `Button::mouseDown`/`Button::mouseReleased`,
but not the rest of that class's shape: `Button::mouseReleased` releases only inside its
`pressedDown` branch, and `Button::mouseCaptureLost` routes through `mouseReleased` (the commit
path, see Pitfalls). Take the cancel shape from `Widgets/SwitchButton.cpp`
`ModeSwitchButton::mouseCaptureLost`, minus its `Skip()`.
`Widgets/SpinInput.cpp` `SpinInput::createButton` shows the guards on `Bind` lambdas, with
`wxEVT_LEFT_DCLICK` bound next to `wxEVT_LEFT_DOWN`.

macOS never sends the lost event, so a widget that must survive an interrupted gesture there needs
stand-ins. Cancel and release when the top-level window reports `wxEVT_ACTIVATE` with
`GetActive() == false`, or when a `wxEVT_MOTION` arrives during the gesture while
`!wxGetMouseState().LeftIsDown()`.

### Modal dialogs, popups and destruction while captured

- "you shouldn't show a modal dialog from a mouse click event handler as this would break the mouse
  capture state" (`interface/wx/event.h:490-499`). The overview adds that a modal shown while
  captured "won't receive any mouse input and appear unresponsive"
  (`docs/doxygen/overviews/eventhandling.h:878-901`). Release your own capture, then
  `CallAfter([…]{ dlg.ShowModal(); })`. The same applies to events a capturing control emits mid-drag
  (sash moves, list selection), and to `wxPopupTransientWindow::Popup()`, which takes capture itself on
  macOS (see `references/popups-menus.md`).
- `Destroy()` or `delete` of a capturing window leaves a dangling stack entry (`~wxWindowBase`
  never removes it), and on macOS a dangling `s_captureWindow`. `Hide()` leaves the hidden window
  holding capture; on macOS every click then goes to it. Release first.
- Moving a top-level window from a custom title bar: prefer the window manager's drag over capture.
  `BBLTopbar::OnMouseLeftDown` (`BBLTopbar.cpp`) posts `WM_NCLBUTTONDOWN`/`HTCAPTION` on MSW after a
  `CaptureMouse(); ReleaseMouse();` pair and calls `gtk_window_begin_move_drag` on GTK. Its fallback
  branch (capture, then `Move()` the frame from `OnMouseMotion`) never runs, because `MainFrame`
  creates `BBLTopbar` only off macOS (`#ifndef __APPLE__` in the `MainFrame` ctor).

### macOS rerouting and the frozen-UI diagnosis

[source] `wxNSWindow`/`wxNSPanel` `sendEvent:` calls `WX_filterSendEvent:` first
(`src/osx/cocoa/nonownedwnd.mm:141-165`, called at :188 and :293). While `wxWindow::GetCapture()` is
non-null, every NSEvent of type `NSLeftMouseDown` … `NSMouseExited` goes straight to the capture
window's `wxWidgetCocoaImpl::DoHandleMouseEvent`, and `[super sendEvent:]` never runs. That covers
left/right down/up, moved, left/right dragged, entered and exited (types 1–9), on any wx window. AppKit
does no hit-testing at all, not even for the title-bar buttons. Scroll-wheel and other-button
(middle) events are not rerouted. Key events (type ≥ 10) never are.

Symptom of a leaked capture: the app repaints, logs and runs timers, but no click works anywhere,
including the window's own traffic-light buttons and any modal dialog. The keyboard still works:
Cmd+S and Cmd+Q still save and quit, so nothing needs to be force-killed. The capture is permanent,
because nothing on macOS unwinds the stack.

Diagnosis: run `sample <pid> 3` while moving the mouse over the window.
`WX_filterSendEvent:` → `wxWidgetCocoaImpl::DoHandleMouseEvent` on the main thread is the proof,
because that path is only reachable while a capture is held. Otherwise the main thread idles in
`mach_msg` with all threads clean, so do not hunt for a deadlock. Then audit every `CaptureMouse()`
site:
- the capture is guarded by `HasCapture()`;
- it is released whenever held, never behind a drag flag something else can clear;
- the window has a lost handler, plus macOS stand-ins where gestures can be interrupted;
- the window releases before hide/destroy and in its destructor.

Typical causes: an unguarded capture pushed twice (a second button, or a re-entered DOWN path);
a dialog opened mid-press; a stack-allocated dialog or popup destroyed while a child holds capture.

### OrcaSlicer

- `Button`, `DropDown` and `StepCtrl` in `Widgets/` capture in their own mouse handlers and declare
  `EVT_MOUSE_CAPTURE_LOST` in their event tables, but their lost handlers share the
  route-through-mouse-up shape the Pitfalls below call wrong; follow the cancel pattern above, not
  them.
- `GLCanvas3D` guards every capture with `has_mouse_capture()` and releases in
  `GLCanvas3D::mouse_up_cleanup()` (`if (m_canvas->HasCapture())`). `GLCanvas3D` is not a
  `wxEvtHandler`: it binds on `m_canvas` in `bind_event_handlers()` and must unbind in
  `unbind_event_handlers()`.

### Pitfalls

- **Rule:** Guard both calls.
  **Why:** An unguarded `ReleaseMouse()` drops whichever window holds the native capture, then
  silently skips the stack bookkeeping. An unguarded `CaptureMouse()` in a second DOWN branch pushes
  the window twice, and the single release restores it: a leak, which on macOS is the frozen UI.
  ```cpp
  // Wrong:
  void up(wxMouseEvent&)   { ReleaseMouse(); }
  void down(wxMouseEvent&) { CaptureMouse(); }          // in each of LEFT/RIGHT/MIDDLE_DOWN
  // Right:
  void up(wxMouseEvent& e)   { e.Skip(); if (HasCapture()) ReleaseMouse(); }
  void down(wxMouseEvent& e) { e.Skip(); if (!HasCapture()) CaptureMouse(); }
  ```
  Cite: `src/common/wincmn.cpp:3352-3421`.
- **Rule:** Cancel on capture-lost; never commit.
  **Why:** A default `wxMouseEvent` is at (0,0) (`src/common/event.cpp:576-581`), which is inside
  `wxRect({0,0}, GetSize())`. Routing the lost event through mouse-up therefore fires `wxEVT_BUTTON`
  when capture is lost mid-press (any `wxDialog::ShowModal` on GTK, an external capture change on
  MSW), and in a list or dropdown it commits the hovered item.
  ```cpp
  // Wrong: the lost event runs the commit path
  void W::mouseCaptureLost(wxMouseCaptureLostEvent&) { wxMouseEvent e; mouseReleased(e); }
  // Right:
  void W::mouseCaptureLost(wxMouseCaptureLostEvent&) { pressedDown = false; Refresh(); }
  ```
  Cite: `interface/wx/window.h:3814-3817`.
- **Rule:** Never `Skip()` in a lost handler.
  **Why:** The event then counts as unprocessed, which is the case wx asserts on in debug builds
  (`src/common/wincmn.cpp:3423-3436`).
- **Rule:** Never assume the lost handler will run.
  **Why:** macOS never sends it. The `HasCapture()` guards and the up handler carry the whole release
  logic there.

## Mouse events

### Coordinates

The position is in client coordinates "of the window which generated the event". Convert with
`ClientToScreen()` and then the other window's `ScreenToClient()` (`interface/wx/event.h:2782-2786`). While a window holds
capture, positions are relative to the capturing window and can be negative or outside its client
rectangle. On macOS they are converted from the event's NSWindow (`src/osx/cocoa/window.mm`
`wxSetupCoordinates`). `GetLogicalPosition(dc)` applies the DC's device origin, e.g. scrolling
(`interface/wx/event.h:3003`). A `wxGLCanvas` works in physical pixels, so `GLCanvas3D::on_mouse`
multiplies by the retina scale under `ENABLE_RETINA_GL`; see `references/webview-gl-aui-media.md`.

### Enter and leave

"the mouse is considered to be inside the window if it is over the window and not inside one of its
children … the parent window receives wxEVT_LEAVE_WINDOW event not only when the mouse leaves the
window entirely but also when it enters one of its children" (`interface/wx/event.h:2776-2780`).
Per port [source]:
- **MSW:** ENTER is synthesized on the first `WM_MOUSEMOVE`, so a click can arrive with no prior
  ENTER; LEAVE comes from `TrackMouseEvent` (`src/msw/window.cpp:6019-6074`). Orca's
  `GLCanvas3D::on_mouse` handles this as "Workaround for SPE-832" (`on_enter_workaround`): on MSW, a
  non-enter event while the cached position is invalid is treated as the enter.
- **GTK:** crossing events with a grab/ungrab mode are ignored; outside capture wx re-derives the
  window under the pointer on each motion (`src/gtk/window.cpp:2073-2117, 2384-2456`; fixes #24339
  and #24931–#24933 are in this release, `docs/changes.txt:537, 547`).
- **macOS:** each view's `NSTrackingArea` uses `NSTrackingInVisibleRect`
  (`src/osx/cocoa/window.mm:3924`) and covers its children, so do not rely on the parent getting
  LEAVE when the pointer moves onto a child. `NSMouseMoved` is delivered only to the deepest view
  under the pointer (`src/osx/cocoa/window.mm:1508-1517`).

A composite's hover state must therefore track its children. Orca's `StateHandler`
(`Widgets/StateHandler.cpp`, `StateHandler::attach_child`) keeps per-child state for this; see
`references/painting-custom-widgets.md`. Inside a `wxPopupTransientWindow` on macOS, Orca's
`PopupWindow` (created with `wxPU_CONTAINS_CONTROLS`) synthesizes ENTER/LEAVE itself; re-check geometry there
(`references/popups-menus.md` §6).

### Button state and clicks

- `LeftDown()` means "this event is the press"; `LeftIsDown()` means "the button is held now"; "if
  wxMouseEvent::LeftDown returns true, wxMouseEvent::LeftIsDown will also"
  (`interface/wx/event.h:2788-2799`). `Dragging()` is MOTION with any button down
  (`include/wx/event.h:1850-1853`).
- **macOS:** button flags are filled only for Down/Dragged NSEvents (`mouseChord`,
  `src/osx/cocoa/window.mm:645-700`). ENTER/LEAVE, plain motion, UP and wheel events report every
  button as up. Use `wxGetMouseState()`, which reads `[NSEvent pressedMouseButtons]`
  (`src/osx/cocoa/utils.mm` `wxGetMouseState`). `GLCanvas3D::on_mouse` back-fills the event from
  `wxGetMouseState()` only when the event carries no button, "to preserve wx's synthetic right button
  for Ctrl+left".
- **macOS Ctrl+click is a right click:** button 0 with `NSControlKeyMask` becomes the right button for
  the whole down/drag/up sequence (`src/osx/cocoa/window.mm:664-686`; documented as an emulation hint at
  `interface/wx/event.h:2772-2774`). `RawControlDown()` stays true on that `wxEVT_RIGHT_DOWN`, so a
  `GetModifiers() == wxMOD_NONE` test fails on it.
- `wxEVT_LEFT_DOWN` handlers "should normally call event.Skip() … otherwise the window under mouse
  wouldn't get the focus" (`interface/wx/event.h:2803-2805`). `Skip()` on a by-value copy is lost,
  because dispatch checks the original event (`src/common/event.cpp:1459-1477`; see
  `references/events.md`).
- **Double click:** on every port the second press arrives as `wxEVT_LEFT_DCLICK` **instead of**
  `wxEVT_LEFT_DOWN`, giving DOWN, UP, DCLICK, UP. MSW maps `WM_LBUTTONDBLCLK` (window classes use
  `CS_DBLCLKS`, `src/msw/app.cpp:584`). GTK drops the surplus press before `GDK_2BUTTON_PRESS`
  (`src/gtk/window.cpp:1798-1817`), and GTK2 also suppresses triple clicks (`:1818-1829`). On wxOSX the
  third press of a triple click is a DOWN again, as on MSW (`src/osx/cocoa/window.mm:4105-4140`;
  #25886, `docs/changes.txt:316`). `GetClickCount()` is "implemented only in wxMac and returns -1
  for the other platforms" (`interface/wx/event.h:2965-2974`).
- **Context menu:** "under MSW the context menu event is generated after EVT_RIGHT_UP … but under
  GTK … after EVT_RIGHT_DOWN", so a window handling `wxEVT_CONTEXT_MENU` must not handle (or must
  `Skip()`) both right-button DOWN and UP (`interface/wx/event.h:3306-3312`).

### Wheel

`GetWheelDelta()` is "normally 120", and "you shouldn't assume that one event is equal to 1 line"
(`interface/wx/event.h:3020-3055`). [source]:

| Port | `GetWheelDelta()` | `GetWheelRotation()` |
|---|---|---|
| MSW | `WHEEL_DELTA` | raw (`src/msw/window.cpp:6117-6118`) |
| GTK3 | 120 | `120 × delta` from `GDK_SCROLL_SMOOTH`, fractional streams (`src/gtk/window.cpp:2177, 2207-2241`) |
| GTK2 | 120 | ±120 per notch |
| macOS | **10** | precise scrolling deltas from trackpads; non-precise wheels ×10 (`src/osx/cocoa/window.mm:800-850`) |

A diagonal trackpad scroll on macOS sends two events, vertical first, then one with
`GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL`. GTK3 smooth scrolling splits it too, horizontal first
(`src/gtk/window.cpp:2207-2241`).

### Pitfalls

- **Rule:** Take mouse events by reference.
  **Why:** `Skip()` on a copy does not reach the dispatcher, so the event counts as handled; focus
  and default processing stop.
  ```cpp
  // Wrong:
  w->Bind(wxEVT_LEFT_DOWN, [](wxMouseEvent e) { /*...*/ e.Skip(); });
  // Right:
  w->Bind(wxEVT_LEFT_DOWN, [](wxMouseEvent& e) { /*...*/ e.Skip(); });
  ```
- **Rule:** Bind DCLICK wherever presses are counted (spin arrows, steppers, toggles).
  **Why:** every second press of a fast pair is a DCLICK; a DOWN-only handler loses it.
  Cite: `Widgets/SpinInput.cpp` `SpinInput::createButton`.
- **Rule:** Don't use `evt.LeftIsDown()` in LEAVE, plain MOTION or UP handlers on macOS.
  **Why:** the flags are only filled for Down/Dragged NSEvents.
  ```cpp
  // Wrong:  if (evt.Leaving() && evt.LeftIsDown()) keep_drag();
  // Right:  if (evt.Leaving() && wxGetMouseState().LeftIsDown()) keep_drag();
  ```
- **Rule:** Normalise wheel steps.
  **Why:** the macOS delta is 10, and GTK3/trackpads send fractional streams. Dividing by a
  hard-coded 120 makes a macOS wheel notch (rotation 10) 12× too slow; a fixed step per event races
  or jitters on trackpad streams.
  ```cpp
  // Wrong:
  zoom += evt.GetWheelRotation() / 120.0;   // or: rot > 0 ? step : -step per event
  // Right:
  if (evt.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) return evt.Skip();
  m_acc += double(evt.GetWheelRotation()) / evt.GetWheelDelta();
  while (std::abs(m_acc) >= 1.0) { step(m_acc > 0 ? 1 : -1); m_acc -= (m_acc > 0 ? 1 : -1); }
  ```

## Global pointer position and Wayland

| API | Contract | GTK on Wayland [source] |
|---|---|---|
| `wxGetMousePosition()` | screen coordinates (`interface/wx/utils.h:360-365`) | `gdk_device_get_position` with no Wayland handling (`src/gtk/window.cpp:7026-7042`). Wayland exposes no global pointer position to clients [external]; Orca's comments record (0,0) |
| `wxGetMouseState()` | position, buttons and modifiers (`interface/wx/utils.h:367-375`) | position unreliable, same path (`src/gtk/window.cpp:2827-2867`) |
| `ClientToScreen()`, `ScreenToClient()`, `GetScreenRect()` | — | add the top-level window's `gdk_window_get_origin()` (`src/gtk/window.cpp:4630-4698`). Without a global origin the result is only meaningful relative to the same top-level window: fine for deltas and hit tests inside one window, wrong across windows or for absolute placement |
| `wxFindWindowAtPoint(pt)` | deepest window at a screen point; disabled children count, hidden ones are skipped (`interface/wx/utils.h:385-394`) | built on screen rectangles, so unreliable. On GTK it is `wxGenericFindWindowAtPoint`, a walk over every top-level window and child (`src/gtk/utilsgtk.cpp:98-101`, `src/common/utilscmn.cpp:1294-1345`) |
| `wxGetKeyState(key)` | "In wxGTK, this function can be only used with modifier keys … when not using X11 backend" (`interface/wx/utils.h:352-358`) | Ctrl/Alt/Shift and Caps/Num/Scroll Lock only; any other key returns false (`src/unix/utilsx11.cpp:2596-2662`) |
| `WarpPointer()` | Apple's HIG forbids it; on Wayland it works only with a compositor implementing the pointer-warp protocol, and mutter also needs a pressed button (`interface/wx/window.h:3895-3914`; `docs/changes.txt:294`) | — |
| `wxUIActionSimulator` | "doesn't work when using Wayland" (`interface/wx/uiaction.h:20`) | — |
| `PopupMenu(x, y)` | — | GTK ≥ 3.22 positions it relative to the window (`gtk_menu_popup_at_rect`, `src/gtk/window.cpp:6520-6565`), so it is safe |

On wxOSX `WarpPointer` synthesizes a `wxEVT_MOTION` to the window (`src/osx/window_osx.cpp:1370-1398`),
so warping from a motion handler recurses. On GTK2, pointer queries and warps go straight to X11.

**OrcaSlicer.** Detect the backend with `Slic3r::GUI::is_running_on_wayland()` / `is_running_on_x11()`
(`LinuxDisplayBackend.hpp`), never with environment variables (`references/platforms.md`). Models:
- `SearchDialog` (`Search.cpp`) dismisses by focus on Wayland (`focus_left_popup`) instead of
  comparing `wxGetMousePosition()` with its screen rectangle.
- `BBLTopbar::FindToolByCurrentPosition` (`BBLTopbar.cpp`) uses the last event position and returns
  null on Wayland rather than query the global pointer.
- `BBLTopbar::OnMouseMotion` and `Button::OnParentMotion` use `ClientToScreen(event.GetPosition())`
  within one window ("wxGetMousePosition() … returns (0,0) on Wayland").

### Pitfalls

- **Rule:** On Linux, never rely on global screen coordinates.
  **Why:** wx delegates straight to GDK and does not compensate on Wayland, so hit tests against
  `wxGetMousePosition()` silently fail or misfire.
  ```cpp
  // Wrong:
  if (!GetScreenRect().Contains(wxGetMousePosition())) Dismiss();
  if (wxFindWindowAtPoint(wxGetMousePosition()) != this) /* ... */;
  // Right: event-relative, same window (convert through the event's GetEventObject() window)
  if (!GetClientRect().Contains(evt.GetPosition())) Dismiss();
  // or, for popups and dialogs, focus tracking (focus_left_popup is file-local to Search.cpp):
  if (focus_left_popup(this, wxWindow::FindFocus(), related)) Dismiss();
  ```
- **Rule:** Track held keys with KEY_DOWN/KEY_UP, not `wxGetKeyState(letter)`.
  **Why:** it is always false for letters on Wayland, so held-key and repeat detection built on it
  (including pruning a held-key record with it) breaks there. Clear such a record from KEY_UP.

## Hover handlers and enter/leave feedback loops

**Rule.** In `wxEVT_ENTER_WINDOW`/`wxEVT_LEAVE_WINDOW` handlers, only record hover state, per child
window, because enter and leave fire per window. Apply `Show()`/`Hide()` + `Layout()` from
`wxEVT_IDLE` or `CallAfter`, and only when the desired state differs from the current one.
`Skip()` the events.

**Why.** A layout change inside the handler moves windows under the pointer, which re-fires
enter/leave. On Wayland compositors that keep hidden-workspace surfaces mapped (Hyprland), GTK
delivers a stream of synthetic leave events, and a handler that re-layouts pegs a CPU core
(69e16cd7ef). The fix that moved show/hide out of the handlers removed the freeze caused by the
dynamically hidden printer edit button (e87625e023). Two wx facts block the obvious guards:
- `wxFindWindowAtPoint()` is a full window-tree walk on GTK and unreliable on Wayland (table above).
- `IsShownOnScreen()` is not a visibility test on any platform. It only checks `IsShown()` up the
  parent chain (`src/common/wincmn.cpp:1205-1212`, `interface/wx/window.h:3101-3102`), plus "surface
  exists" for `wxGLCanvas` on Unix. It never reflects minimised, occluded or other-workspace state,
  which is why `Plater::priv::set_current_panel` says "wxWidgets IsShownOnScreen() is buggy and
  cannot be used reliably".

**OrcaSlicer.** The Sidebar printer, nozzle and bed panels (`Plater.cpp`, `Sidebar` ctor) follow
this design:
- each child's ENTER/LEAVE lambda inserts or erases the window in a per-group
  `std::shared_ptr<std::unordered_set<wxWindow*>>` (e.g. `printer_preset_hovered`) and sets the border
  colour;
- a `wxEVT_IDLE` handler compares `!hovered->empty()` with `btn_edit_printer->IsShown()` and calls
  `Show()`/`Hide()` + `Layout()` only on a difference (keep such a handler a cheap comparison: it runs on every
  idle pass, hidden panels included, `references/events.md` §12);
- clicking the edit button clears the set inside `CallAfter`, because opening the preset tab sends
  no LEAVE (ff4147ede3), and because hiding a button from inside its own event handler crashed on
  wxGTK.

```cpp
// Wrong: walks the tree and re-layouts inside the event (feedback loop)
w->Bind(wxEVT_LEAVE_WINDOW, [=](wxMouseEvent& e) {
    if (!hit(wxFindWindowAtPoint(wxGetMousePosition()))) { btn->Hide(); panel->Layout(); }
    e.Skip(); });
// Right: record, then apply once from idle
w->Bind(wxEVT_ENTER_WINDOW, [=](wxMouseEvent& e) { hovered->insert(w); e.Skip(); });
w->Bind(wxEVT_LEAVE_WINDOW, [=](wxMouseEvent& e) { hovered->erase(w);  e.Skip(); });
Bind(wxEVT_IDLE, [=](wxIdleEvent& e) {
    if (btn->IsShown() != !hovered->empty()) { btn->Show(!hovered->empty()); panel->Layout(); }
    e.Skip(); });
```
Cite: 69e16cd7ef, e87625e023, ff4147ede3 (`Plater.cpp`).

## Keyboard events

### Event order per port

[source] One key press, in order:

| Port | Sequence |
|---|---|
| MSW | thread keyboard hook `wxKeyboardHook` → `wxEVT_CHAR_HOOK` to the focus window (or the active window) — skipped while any native `::GetCapture()` exists or a non-wx modal (IME) is open; with IME open a handled hook does not stop the key reaching the IME (`src/msw/window.cpp:7320-7395`) → `wxGUIEventLoop::PreProcessMessage`: accelerator walk from the focus up to the first `IsTopNavigationDomain(Navigation_Accel)` window, with the text-entry exemption (`src/msw/evtloop.cpp:62-119`) → `IsDialogMessage`, which is never given `VK_ESCAPE` (`src/msw/window.cpp:2769-2778`) → `wxEVT_KEY_DOWN` → `wxEVT_CHAR` |
| GTK | `wxEVT_CHAR_HOOK`, skipped while `g_captureWindow` is set → accelerator walk to the top-level window, no text-entry exemption → `wxEVT_KEY_DOWN` → input method → `wxEVT_CHAR` (`src/gtk/window.cpp:1266-1420`) |
| macOS | the main menu's `performKeyEquivalent:`: a menu-bar key equivalent consumes the key before wx sees it (`src/osx/cocoa/window.mm:1611-1644`) → `wxEVT_CHAR_HOOK`, also during capture → accelerator walk, only if no `wxEVT_CHAR_HOOK` handler processed the event (`src/osx/window_osx.cpp:2547-2590`) → Tab navigation by the first `wxTAB_TRAVERSAL` ancestor unless the window has `wxWANTS_CHARS` (`src/osx/cocoa/window.mm:4009-4046`) → `interpretKeyEvents` (IME) → `wxEVT_KEY_DOWN` → `wxEVT_CHAR` (`:4048-4101`). A disabled window gets no key events (`:1616-1617`) |

### wxEVT_CHAR_HOOK

Contract (`interface/wx/event.h:1487-1508`): "Unlike all the other key events, this event is
propagated upwards the window hierarchy … generated before any other key events". If a handler
processes it without `Skip()`, "neither wxEVT_KEY_DOWN nor wxEVT_CHAR events will be generated
(although wxEVT_KEY_UP still will be)". `DoAllowNextEvent()` handles it and still lets normal events
through (`:1683-1702`). It "is not generated when the mouse is captured". [source] Exceptions and
details:
- **macOS** has no capture check, so the hook fires during drags.
- **wxGTK** reads the allow flag from the original event rather than the hook event, so
  `DoAllowNextEvent()` has no effect there (`src/gtk/window.cpp:1266-1282`).
- Propagation stops only at windows with `wxWS_EX_BLOCK_EVENTS` (`src/common/wincmn.cpp:3498-3522`).
  `wxDialog` sets it (`src/common/dlgcmn.cpp:127`), and so does `wxPopupWindow` on MSW and macOS
  (`wxPopupWindowBase::Create`, `src/common/popupcmn.cpp:135`). wxGTK's `wxPopupWindow::Create`
  never calls the base, so a GTK popup does not block (`src/gtk/popupwin.cpp`). Frames do not
  either: keys typed in a modeless frame (or a GTK popup) parented to `MainFrame` reach
  `MainFrame`'s hook.

`wxEVT_KEY_DOWN`/`wxEVT_CHAR` are not command events and do not propagate
(`docs/doxygen/overviews/eventhandling.h:536-544`). If `wxEVT_KEY_DOWN` is handled without `Skip()`, "the
corresponding char event … will not happen … Not doing may also prevent accelerators defined using
this key from working" (`interface/wx/event.h:1456-1463`).

**OrcaSlicer — dialog keys.** `DPIAware<wxDialog>` (`GUI_Utils.hpp`) binds `wxEVT_CHAR_HOOK`: Esc
calls `Close()`, everything else is skipped. A dialog that needs Esc or Enter itself binds its own
hook. Later `Bind`s run first (`docs/doxygen/overviews/eventhandling.h:474-482`), so the dialog's
hook runs before `DPIAware`'s. Orca's `::Button` is not a `wxButton`, so wx's default-button and
Esc emulation (`EmulateButtonClickIfPresent`) never finds it. Dialogs synthesize the click instead,
as the `CloneDialog` ctor does for Enter in its count field:
```cpp
Bind(wxEVT_CHAR_HOOK, [this, ok_btn](wxKeyEvent& e) {
    const int key = e.GetKeyCode();
    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && m_count_spin->GetTextCtrl()->HasFocus()) {
        wxCommandEvent evt(wxEVT_BUTTON, ok_btn->GetId());
        ok_btn->GetEventHandler()->ProcessEvent(evt);
    } else
        e.Skip();
});
```
The full Esc/close path is in `references/windows-dialogs.md`.

### Key codes

- Use `GetUnicodeKey()` for printable characters and `GetKeyCode()` for `WXK_*` specials
  (`interface/wx/event.h:1329-1346`).
- `wxEVT_KEY_DOWN`/`UP`: ASCII letters are their upper-case code; other Latin-1 characters (`ù`,
  `ö`, `²`) are the character itself, not upper-cased; keys producing non-Latin printable characters
  report "the ASCII code of the character the same key would produce in the standard US keyboard
  layout"; specials are their `WXK_*` (`:1371-1394`). A Cyrillic `ц` gives `'W'`, so Ctrl+letter
  shortcuts work across layouts, but an AZERTY key reports its own label (`$` where US has `]`), so
  `Ctrl-;`-style punctuation accelerators may be untypeable on some layouts (`:1396-1407`). wxGTK
  got the non-Latin mapping in 3.3.0 (#23379, `docs/changes.txt:540`).
- `wxEVT_CHAR` reflects Shift and the layout. Ctrl+letter gives 1..26 (`WXK_CONTROL_A` …,
  `:1409-1421`). Exception: on macOS Cmd+letter's CHAR is the letter itself (`'a'`/`'A'`); only the
  physical Control key yields 1..26 (`src/osx/cocoa/window.mm:304-307`).
- Documented inconsistencies: Ctrl-Backspace, Ctrl-Enter, and on GTK no CHAR for Ctrl + a letter
  mapped to a non-Latin one (`:1423-1433`). Modifier keys generate no CHAR (`:1435`).
- `IsAutoRepeat()` is `@onlyfor{wxosx,wxmsw,wxQt}`, not GTK (`:1595-1601`).

`KeyChord::from_event` (`KeyChord.cpp`) normalises all of this: it up-cases letters, folds numpad
keys onto the main keyboard, maps CHAR control codes 1..26 back to letters, and drops Shift from
CHAR punctuation, because the character already reflects it. Use it rather than testing raw codes.

### Modifiers and the Cmd mapping

| Query | MSW / GTK | macOS |
|---|---|---|
| `ControlDown()`, `wxMOD_CONTROL`, `wxACCEL_CTRL`, `"Ctrl+"` in accelerator strings | Ctrl | **Cmd** |
| `RawControlDown()`, `wxMOD_RAW_CONTROL`, `WXK_RAW_CONTROL`, `wxACCEL_RAW_CTRL`, `"RawCtrl+"` | Ctrl (same value as the above) | the physical Control key (a distinct bit) |
| `CmdDown()`, `wxMOD_CMD`, `wxACCEL_CMD` | deprecated aliases of `ControlDown()`/`wxMOD_CONTROL`/`wxACCEL_CTRL` | same |

Sources: `interface/wx/kbdstate.h:38-130`, `interface/wx/accel.h:15-30`, `include/wx/defs.h:2372-2388`,
`include/wx/accel.h:29-41`.
`interface/wx/kbdstate.h:45` claims `wxMOD_CMD` is `wxMOD_META` on Mac; the code defines `wxMOD_CMD = wxMOD_CONTROL`
everywhere [source]. Prefer `GetModifiers() == wxMOD_CONTROL`: `ControlDown()` alone is also true for
Ctrl+Shift, and for AltGr, which reports as Ctrl+Alt (`interface/wx/kbdstate.h:38-70`).

### wxWANTS_CHARS and navigation

- `wxWANTS_CHARS`: the window "get[s] all char/key events for all keys — even for keys like TAB or
  ENTER"; call `Navigate()` yourself for Tab (`interface/wx/window.h:225-232`). On macOS wx's Tab
  navigation is skipped for such windows, and otherwise Tab is consumed by the first
  `wxTAB_TRAVERSAL` ancestor before `wxEVT_KEY_DOWN` (`src/osx/cocoa/window.mm:4009-4046`). The GL
  canvas is created with it (`OpenGLManager::create_wxglcanvas`), so Tab and arrows arrive.
  `GLCanvas3D::on_key` does not `Skip()` Tab and arrows and skips everything else "to have EVT_CHAR
  generated".
- `wxTAB_TRAVERSAL` "should almost never be used in the application code"
  (`interface/wx/window.h:221-224`). A composite with focusable children derives from
  `wxNavigationEnabled<Base>` (`interface/wx/containr.h:10-46`).
- `Navigate(flags)` "is equivalent to calling NavigateIn() method on the parent"
  (`interface/wx/window.h:2968-2988`). `HandleAsNavigationKey(evt)` (`:2715-2724`) and
  `MoveAfterInTabOrder`/`MoveBeforeInTabOrder` (`:2949-2966`) complete the set.
- `DisableFocusFromKeyboard()` removes a window from the Tab chain but keeps click focus
  (`interface/wx/window.h:497-506`). `SpinInput`'s arrow buttons use it.

**OrcaSlicer — `::Button`.** `Button::keyDownUp` turns Space/Return into LEFT_DOWN/UP and passes Tab
and arrows to `HandleAsNavigationKey`. The synthesized LEFT_DOWN runs `Button::mouseDown`, so a held
Space/Return holds the mouse capture until its KEY_UP reaches the button. On MSW
`Button::MSWWindowProc` answers `WM_GETDLGCODE` with `DLGC_WANTMESSAGE`.

### Pitfalls

- **Rule:** Catch children's keys with `wxEVT_CHAR_HOOK` on the parent.
  ```cpp
  // Wrong: never sees keys typed in child controls
  panel->Bind(wxEVT_KEY_DOWN, &P::on_key, this);
  // Right:
  panel->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) { if (!handle(e)) e.Skip(); });
  ```
- **Rule:** A top-level hook lets text-editing keys through to a focused text entry.
  **Why:** the hook runs before the text control and before MSW's text-entry accelerator
  exemption. Eating bare printable keys, Space, Ctrl+C/V/X/A/Z, Delete, Home/End or arrows breaks
  them in every field of the window.
  ```cpp
  // Wrong:
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) { if (e.GetKeyCode() == WXK_DELETE) delete_selection(); else e.Skip(); });
  // Right:
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_DELETE && !dynamic_cast<wxTextEntryBase*>(wxWindow::FindFocus())) delete_selection();
      else e.Skip(); });
  ```
  Cite: `MainFrame.cpp` `focus_keeps_space` (text entries, `wxWebView`, `::Button`, `StaticBox`
  composites and `wxControl`s keep Space).
- **Rule:** Don't expect hook-based shortcuts during a drag on MSW/GTK, and guard drag state on
  macOS, where they do fire.
- **Rule:** Ctrl+letter in `wxEVT_CHAR` is a control code.
  ```cpp
  // Wrong:  if (e.GetEventType() == wxEVT_CHAR && e.ControlDown() && e.GetKeyCode() == 'a')
  // Right:  if (KeyChord::from_event(e) == KeyChord{'A', wxMOD_CONTROL})   // or match on KEY_DOWN
  ```
- **Rule:** Modifier tests are exact.
  ```cpp
  // Wrong:  if (e.ControlDown() && e.GetKeyCode() == 'C')      // also AltGr+C, Ctrl+Shift+C
  // Right:  if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'C')
  ```

## Accelerators and menu shortcuts

### Contract

- "An accelerator takes precedence over normal processing" (`interface/wx/accel.h:178`), but it
  runs after `wxEVT_CHAR_HOOK` on every port (table above).
- [source] On GTK and macOS a hit is sent as `wxEVT_MENU` with the entry's id and, if unprocessed,
  retried as `wxEVT_BUTTON`; macOS treats the key as consumed either way
  (`src/gtk/window.cpp:1340-1366`, `src/osx/window_osx.cpp:2563-2590`). On MSW the `WM_COMMAND` goes to
  a child window with that id if one exists (a `wxButton` clicks), otherwise it becomes `wxEVT_MENU`
  (`src/msw/window.cpp` `wxWindowMSW::HandleCommand`).
- Matching up-cases a–z (`src/generic/accel.cpp:86-88`, `src/osx/accel.cpp:59`) and needs an
  **exact** modifier match (`src/generic/accel.cpp:155-180`; `src/osx/accel.cpp:69-84`, which also
  matches RawCtrl). Ctrl+Shift+Z needs its own entry.
- `SetAcceleratorTable()` **replaces** the window's single table; nothing merges. The walk runs from
  the focused window up to the top-level window, so a table works only while focus is inside its
  window, and a dialog never sees its parent frame's table [source].
- Menu strings take `"Label\tCtrl+X"`: modifiers `CTRL`/`RAWCTRL`/`ALT`/`SHIFT` joined by `+` or `-`,
  plus the special key names listed in `interface/wx/menuitem.h:469-555`.
  `wxAcceleratorEntry::FromString` accepts the bare accelerator or the legacy `"Label\tAccel"`.
  `ToRawString()` is untranslated, for config files (`interface/wx/accel.h:124-146`).
  `AddExtraAccel` is `@onlyfor{wxmsw,wxgtk}` (`interface/wx/menuitem.h:643-647`).

### Platforms

- **Menu accelerators exist only for menus attached to a frame's `wxMenuBar`** [source]. MSW merges
  them in `wxMenuBar::RebuildAccelTable` (`src/msw/menu.cpp:1264-1292`); GTK adds the menu's accel
  group to the top-level window in `AttachToFrame` (`src/gtk/menu.cpp:239-247`). A menu shown with
  `PopupMenu` treats `"\tCtrl+X"` as display text.
- **MSW:** accelerators are not translated while a `wxTextCtrl`/`wxComboBox`/`wxSpinCtrl` has focus and
  the key is a text-editing key: Ctrl+A/C/V/X/Ins/Del/Home/End/Left/Right, Shift+those navigation
  keys, bare Del/Home/End, Alt+Backspace (`src/msw/textentry.cpp:1073-1150`). Multi-line controls also
  keep Enter (`src/msw/textctrl.cpp:2110-2135`).
- **GTK:** the accelerator walk has no such exemption (`src/gtk/window.cpp:1340-1366`). As menu
  accelerators, Shift with non-alphabetic keys does not work, bare arrow keys do not work, and the
  listed keys (Tab, the modifiers, locks …) are unsupported (`interface/wx/menuitem.h:562-575`).
- **macOS:** menu-bar items become `NSMenuItem` key equivalents
  (`src/osx/cocoa/menuitem.mm` `wxMacCocoaMenuItemSetAccelerator`). They run before any wx key event,
  including while a text field has focus. A menu-bar accelerator on a printable key without a
  modifier, or on a text-editing chord such as Cmd+C, takes that key from typing.

### OrcaSlicer menus

The native `wxMenuBar` exists only on macOS (`MainFrame::init_menubar_as_editor`, Preferences under
`OSXGetAppleMenu()`). On Windows and Linux the same `wxMenu`s hang off `BBLTopbar`
(`GetTopMenu()`, `SetFileMenu`, `AddDropDownSubMenu`), so their labels are display-only and the keys
are dispatched by the registry (`MainFrame`'s hook, the canvases). Add shortcut-bearing items with
`MainFrame::append_shortcut_item(menu, Shortcut::X, accelerator, label, …)`:
- `accelerator == true` and the binding is menu-safe (`ShortcutRegistry::accelerator()` is non-empty,
  i.e. `KeyChord::is_menu_accelerator()`): the label is `label + "\t" + accel`, a live key equivalent
  on macOS.
- Otherwise the binding is appended as text. The separator is `" - "` on Apple, so the macOS menu bar
  does not take it as a key equivalent, and `"\t"` elsewhere (`MainFrame::shortcut_label`). The source
  comment cites #8152: the macOS menu bar "handles the key accelerators automatically and breaks key
  handling in normal typing". The macOS Edit menu shows clipboard and undo this way, so that Cmd+C in
  a text field copies text rather than objects.
- `MainFrame::update_shortcut_labels()` rewrites every tracked item after a rebinding
  (`GUI_App::on_shortcuts_changed`).

On Apple `MainFrame`'s hook keeps Cmd+H (consumed; the app menu hides), Cmd+M (`Iconize()`), Cmd+Q
(posts `wxEVT_CLOSE_WINDOW`) and Cmd+Ctrl+F (`EnableFullScreenView(true)` + `ShowFullScreen`
toggle). Preferences (Cmd+, / Ctrl+P) is a Global registry shortcut.

### Pitfalls

- **Rule:** One table per window, built once from all entries.
  ```cpp
  // Wrong: the second call discards the first table
  SetAcceleratorTable(wxAcceleratorTable(1, &copy));  SetAcceleratorTable(wxAcceleratorTable(1, &paste));
  // Right:
  wxAcceleratorEntry e[] = { copy, paste };  SetAcceleratorTable(wxAcceleratorTable(2, e));
  ```
- **Rule:** Don't expect `MainFrame`'s keys inside a dialog: each top-level window needs its own
  handling (dialogs block `wxEVT_CHAR_HOOK` propagation and the accelerator walk stops at them).

## Orca's shortcut registry

The registry is the one table every key dispatcher, menu label, tooltip and the shortcuts dialog
reads; the design is in `docs/HLSD/keyboard-shortcuts.md`.

**Data model.**
- `KeyChord` (`KeyChord.hpp`) is `{key, modifiers}`: the key as `wxEVT_KEY_DOWN` reports it, plus
  `wxMOD_*` limited to CONTROL|SHIFT|ALT|RAW_CONTROL. Its canonical text (`to_string()`, e.g.
  `Ctrl+Shift+S`) is platform-neutral and doubles as the wx accelerator string and the config format.
  `display()` uses translated modifier names and the macOS glyphs. Predicates:
  - `needs_char_event()`: a printable non-alphanumeric key with at most Shift, resolvable only from
    the CHAR that follows.
  - `is_punctuation()`: such a key with no modifier.
  - `is_menu_accelerator()`: Ctrl, Alt or RawCtrl held, or a non-printable key other than Space.
  - `is_system_shortcut()`: Alt+F4 and Alt+Space on Windows.
  - `to_accelerator_entry()` maps RawCtrl to `wxACCEL_RAW_CTRL` on Apple.
- `Shortcut` (enum) and `shortcut_table` (`Shortcuts.cpp`): each row, written with
  `SHORTCUT`/`REPEATING`/`STEPPING`, holds a config key, description, context mask, default chord, and
  the `repeatable`/`modifier_variants` flags. `static_assert`s keep the table in enum order and
  `section_table` ascending.
- `ShortcutRegistry` (`wxGetApp().shortcuts()`) overlays user overrides from the AppConfig section
  `shortcuts`; only overrides are stored, and `none` records an unbound shortcut.
  `ShortcutRegistry::load()` drops a Global override that fails `is_menu_accelerator()` ("A Global
  chord is seen before any text field").
- Lookups: `lookup(context, chord)` matches exactly. `match()` falls back to `modifier_variants`
  shortcuts with Shift/Ctrl added, which are reserved steps (`step_owner()`). `conflicts()` treats
  Global as sharing every context.

**Contexts and dispatch.**

| Context | Dispatcher | Event |
|---|---|---|
| `Global` | `MainFrame::handle_global_shortcut`, from `MainFrame`'s `wxEVT_CHAR_HOOK`, before the focused child gets KEY_DOWN/CHAR (a child's own CHAR_HOOK handler still runs first); `Skip()` otherwise | CHAR_HOOK |
| `Plater`, `Preview` | `GLCanvas3D::handle_shortcut` (→ `ShortcutRegistry::match`) from `GLCanvas3D::on_key`; punctuation retried from `GLCanvas3D::on_char` | KEY_DOWN, CHAR |
| `ObjectList` | `ObjectList::dispatch_shortcut`, from `ObjectList::key_event` on CHAR (non-macOS); on macOS from a `wxAcceleratorTable` built by `ObjectList::update_shortcut_accelerators()` (ids from `wxWindow::NewControlId`), because the native data view delivers no keys there | CHAR / accelerator |
| `Painting` | a painting gizmo's `on_tool_shortcut` (`GLGizmosManager`) | KEY_DOWN |

- A Global chord needs Ctrl/Alt or a non-typing key, because it is seen before any text field. Space
  counts as typing: the speed dial's bare-Space default is skipped when `focus_keeps_space(FindFocus())`.
- Shortcuts in other contexts may share a chord when their contexts do not overlap.
- The macOS ObjectList table is swapped to `wxNullAcceleratorTable` while a name is being edited and
  restored in `ObjectList::OnEditingDone`.
- wxGTK reports no auto-repeat, so the canvases keep one shared record of keys seen going down
  (`key_repeats`/`key_released`) and swallow repeats of non-`repeatable` shortcuts.

**Adding a shortcut.**
1. Add the `Shortcut` value and its row in `shortcut_table`, in dialog order (a new section also needs a
   `section_table` entry). Pick a default that does not collide inside its contexts; the `[Shortcuts]`
   tests check every default.
2. Handle it in the context's dispatcher: `MainFrame::handle_global_shortcut`,
   `GLCanvas3D::handle_shortcut`, `ObjectList::dispatch_shortcut`, or a gizmo's `on_tool_shortcut`. A
   gizmo opened by a key sets `m_shortcut` in its `on_init()` (e.g. `GLGizmoMove3D::on_init`).
3. Where the UI shows the key, ask the registry: `display()` for tooltips, `accelerator()` (via
   `append_shortcut_item`) for menus. Never write a literal key name.

`KBShortcutsDialog::fill_pages` lists registry entries automatically. Only non-rebindable keys and
mouse actions are added there by hand (`fixed(...)`, `mouse(...)`). Edits go through
`GUI_App::on_shortcuts_changed()`, which saves and pushes to menus, canvas tooltips and the macOS
ObjectList table.

**Model for capturing raw chords:** `ShortcutCaptureDialog` (`KBShortcutsDialog.cpp`).
- A `wxWANTS_CHARS` `StaticBox` keeps focus, so the buttons never get the keys; focus is set by
  `capture->CallAfter(... SetFocus())` after construction.
- `wxEVT_CHAR_HOOK` on the dialog handles Esc/Enter, records KEY_DOWN chords and `Skip()`s
  `needs_char_event()` chords.
- A `wxEVT_CHAR` handler on the box records punctuation.
- Its comment notes that the hook runs before the window procedure, so Windows does not open its
  window menu on Alt+Space.

```cpp
// Wrong: ad-hoc key test plus a hand-written label
if (evt.GetKeyCode() == 'E' && evt.ControlDown()) export_gcode();
append_menu_item(menu, wxID_ANY, _L("Export") + "\tCtrl+E", ...);
// Right: registry row + dispatcher case + registry-derived label
case Shortcut::ExportSlicedFile: if (can_export_gcode()) wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE)); return false;
append_shortcut_item(export_menu, Shortcut::ExportSlicedFile, true, _L("Export plate sliced file") + dots, ...);
```

## Focus

### Contract

- `SetFocus()` "sets the window to receive keyboard input" (`interface/wx/window.h:567-572`).
  `FindFocus()` is static (`:4274-4283`). `HasFocus()` also covers a composite's main child
  (`:529-536`).
- `AcceptsFocus()` returning false means the control "doesn't accept input at all" (`:472-480`).
  `AcceptsFocusFromKeyboard()` controls Tab-chain membership (`:482-488`). `CanAcceptFocus()` is
  `AcceptsFocusRecursively() && IsShown() && IsEnabled()` (`include/wx/window.h:751-766`).
- `SetCanFocus()` "is only implemented by ports which have support for native TAB traversal … A call
  to this does not disable or change the effect of programmatically calling SetFocus()"
  (`interface/wx/window.h:538-548`).
- Focus handlers "should almost invariably call wxEvent::Skip() … wxEVT_KILL_FOCUS handler must not
  call wxWindow::SetFocus()" (`interface/wx/event.h:3405-3416`). The event's `GetWindow()` "may be
  NULL" (`:3438-3445`). `wxEVT_CHILD_FOCUS` derives from `wxCommandEvent` and propagates, and its
  window is the *direct* child (`:3453-3491`).
- `wxPanel::SetFocus` focuses the first child if "the control has at least one child";
  `SetFocusIgnoringChildren()` focuses the panel itself (`interface/wx/panel.h:125-142`).
- `wxGetActiveWindow()` "always returns NULL in the other ports", i.e. on everything but MSW and GTK
  (`interface/wx/window.h:4545-4551`).

### Platforms

[source]
- **GTK:** `SetFocus()` calls `gtk_window_present()` on a visible, inactive top-level window, i.e. it
  **raises and activates** the window (`src/gtk/window.cpp:5137-5162`). On a not-yet-shown widget the
  focus becomes "pending", and `FindFocus()` returns the pending window at once (`:5141-5155,
  2777-2791`). While a popup menu is open, `FindFocus()` returns the invoking window.
- **MSW:** disabling the focused control first `Navigate()`s forward (`src/msw/window.cpp`
  `MSWEnableHWND`). Any `WM_SETFOCUS`, `WM_KILLFOCUS` or button-down in a window outside
  `wxCurrentPopupWindow` dismisses a transient popup that lacks `wxPU_CONTAINS_CONTROLS`
  (`src/msw/window.cpp:3018-3033` → `MSWDismissUnfocusedPopup`, `src/msw/popupwin.cpp:218-229`).
- **Modeless windows:** focus requested right after `Show()` can be dropped while activation is still
  in flight (Orca comment in `SpeedDialWebDialog`'s ctor). Set it from `CallAfter` or on
  `wxEVT_ACTIVATE` (`SpeedDialWebDialog`'s activate handler, see `references/popups-menus.md`); the
  modal `ShortcutCaptureDialog` likewise defers its first `SetFocus()` with `CallAfter`.

### OrcaSlicer

- `::Button` overrides `SetCanFocus` to store `canFocus`, which drives `Button::AcceptsFocus()` and
  whether `Button::mouseDown` calls `SetFocus()`. In Orca `SetCanFocus(false)` therefore means "never
  take focus", unlike the stock GTK-only hint.
- `GLCanvas3D::on_mouse` focuses the canvas on any button-down. On ENTER it focuses the canvas only if
  the top-level window `IsActive()` and focus is not in a `wxTextCtrl`, and on MSW only while
  `wxCurrentPopupWindow` is null. Stealing focus would trigger `MSWDismissUnfocusedPopup` and close
  the search dropdown.

### Pitfalls

- **Rule:** No unconditional `SetFocus()` on hover, ENTER or timers.
  **Why:** on GTK it raises and activates the window over whatever the user is doing; on MSW it
  dismisses open transient popups; anywhere it steals the caret from a text field.
  ```cpp
  // Wrong:
  canvas->Bind(wxEVT_ENTER_WINDOW, [=](wxMouseEvent& e) { canvas->SetFocus(); e.Skip(); });
  // Right (wxCurrentPopupWindow is in no wx header: declare
  //        extern wxPopupWindow* wxCurrentPopupWindow; as GLCanvas3D.cpp does):
  canvas->Bind(wxEVT_ENTER_WINDOW, [=](wxMouseEvent& e) {
      auto* tlw = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(canvas));
      if (tlw && tlw->IsActive() && !dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus())
  #ifdef __WXMSW__
          && !wxCurrentPopupWindow
  #endif
      ) canvas->SetFocus();
      e.Skip(); });
  ```
  Cite: `GLCanvas3D::on_mouse`.
- **Rule:** Defer focus changes out of `wxEVT_KILL_FOCUS`, and always `Skip()` focus events.
  ```cpp
  // Wrong:  ctrl->Bind(wxEVT_KILL_FOCUS, [=](wxFocusEvent&) { other->SetFocus(); });
  // Right:  ctrl->Bind(wxEVT_KILL_FOCUS, [=](wxFocusEvent& e) { e.Skip(); other->CallAfter([other] { other->SetFocus(); }); });
  ```

## Tooltips

### Contract

- `SetToolTip(const wxString&)`, `SetToolTip(wxToolTip*)` and `UnsetToolTip()`
  (`interface/wx/window.h:3238-3273`). Setting an **empty string does not remove** the tooltip; the
  code says "use SetToolTip(nullptr)" (`src/common/wincmn.cpp:2245-2259`) [source].
- The string overload reaches the virtual `DoSetToolTipText`; the pointer overload and `UnsetToolTip`
  reach `DoSetToolTip` (`include/wx/window.h:1484-1489`).
- Statics (`interface/wx/tooltip.h:26-85`): `Enable` "may not be supported on all platforms";
  `SetAutoPop`/`SetReshow` "May not be supported (eg. wxCocoa, GTK)"; `SetMaxWidth` is wxMSW-only.
- `wxTipWindow::New()` (3.3.2) returns a `wxTipWindow::Ref` that becomes null when the tip closes
  itself; never keep a raw pointer (`interface/wx/tipwin.h:20-110`).
- `wxRichToolTip` is not a window: each `ShowFor()` creates a new one, and the native MSW version
  only applies to text controls (`interface/wx/richtooltip.h:70-192`).

| Static [source] | MSW | GTK | macOS |
|---|---|---|---|
| `Enable` | works | sets `GtkSettings` `gtk-enable-tooltips` | no-op |
| `SetDelay` | works | sets `gtk-tooltip-timeout` (`src/gtk/tooltip.cpp:74-128`) | writes `NSInitialToolTipDelay` into `[NSUserDefaults standardUserDefaults]`, the app's persistent defaults (`src/osx/cocoa/tooltip.mm:66-72`) |
| `SetAutoPop`, `SetReshow` | work; Orca's `MainFrame` ctor uses `SetAutoPop(32767)` because larger values fail | no-op | no-op |

### Tooltips on disabled controls

MSW tooltips subclass the tool's HWND (`TTF_SUBCLASS`, `src/msw/tooltip.cpp:131-135`), and a
disabled HWND receives no mouse messages [external], so a disabled control shows no tooltip.
`Button::EnableTooltipEvenDisabled()` (`Widgets/Button.cpp`) works around this:
- it binds the **parent's** `wxEVT_MOTION`/`wxEVT_LEAVE_WINDOW` (`Button::OnParentMotion`/
  `Button::OnParentLeave`);
- it pops a `wxTipWindow::Ref` when the pointer is over the disabled button;
- it positions the tip from `ClientToScreen(wxPoint(0, 0))` rather than `wxGetMousePosition()`, which
  returns (0,0) on Wayland;
- it is compiled only under `#if defined(_MSC_VER) || defined(_WIN32)`.

- **Rule:** Install parent-window mouse-tracking handlers that simulate tooltips on disabled controls
  only on Windows; gate them with `#if defined(_WIN32)`.
  **Why:** the hack froze the UI on macOS. The commit records only the symptom. Likely mechanism
  [source]: `wxTipWindow` is a `wxPopupTransientWindow`. On wxOSX its `Show(true)` captures the mouse
  on its child without a guard, and `OnIdle` keeps capture while the pointer is outside the popup
  (`src/common/popupcmn.cpp:335-430, 439-471`). A tip popped from parent MOTION, beside the pointer,
  therefore takes every click (see the macOS capture section), and a re-`Popup()` while shown
  pushes the capture twice.
  ```cpp
  // Wrong: unconditional
  parent->Bind(wxEVT_MOTION, &Button::OnParentMotion, this);
  // Right:
  #if defined(_MSC_VER) || defined(_WIN32)
  parent->Bind(wxEVT_MOTION, &Button::OnParentMotion, this);
  parent->Bind(wxEVT_LEAVE_WINDOW, &Button::OnParentLeave, this);
  #endif
  ```
  Cite: d0cc4b35ee (`Widgets/Button.cpp` `Button::EnableTooltipEvenDisabled`).

### OrcaSlicer

- `TextInput`, `SpinInput` and `TempInput` override `DoSetToolTipText` to forward the text to the
  inner `wxTextCtrl`. Only the string overload forwards; `UnsetToolTip()` and
  `SetToolTip(wxToolTip*)` do not reach the inner control.
- `Sidebar::priv::show_rich_tip` (`Plater.cpp`, `_WIN32` only) uses `wxRichToolTip` and recolours
  the shown popup's first child for dark mode.
- Option tooltips for settings are assembled by the `Field` machinery (`references/orca-settings-ui.md`).

### Pitfalls

- **Rule:** `UnsetToolTip()` to clear.
  ```cpp
  // Wrong:  btn->SetToolTip("");      // keeps an empty tooltip object
  // Right:  btn->UnsetToolTip();
  ```
- **Rule:** A composite forwarding tooltips overrides both virtuals, so `UnsetToolTip()` and the
  `wxToolTip*` overload reach the inner children.
- **Rule:** Don't call `wxToolTip::SetDelay` on macOS casually: it persists in the user's defaults for
  the app.

## Cursors

### Contract

- `SetCursor(c)` "also sets it for the children of the window implicitly"; `wxNullCursor` resets it to
  the default (`interface/wx/window.h:3866-3882`). The system shows the window cursor whenever the
  pointer is over the window, so set it **once**.
- For high DPI, use `SetCursorBundle()` (3.3.0) (`interface/wx/window.h:3884-3892`). A default `wxCursorBundle()` is
  empty and means "no custom cursor", not a blank cursor (`interface/wx/cursor.h:305-317`).
- `wxSetCursor(bundle)` "Globally sets the cursor … overrides any cursor set for the individual
  windows … until this function is called again with an empty cursor bundle"
  (`interface/wx/gdicmn.h:1371-1389`). `wxSetCursor(wxNullCursor)` is that reset, through the implicit
  `wxCursorBundle(const wxCursor&)` (`src/common/curbndl.cpp:174`) [source].
- `wxBusyCursor` is RAII around the nested `wxBeginBusyCursor`/`wxEndBusyCursor` counter, with
  `wxIsBusy()` (`interface/wx/busycursor.h`). These are main-thread GUI calls.
- `wxBitmap(wxCursor)` is invalid on GTK/Wayland (`interface/wx/bitmap.h:393-395`).

### OrcaSlicer

- Dialog work uses `wxBusyCursor` RAII (e.g. `PhysicalPrinterDialog.cpp`).
- Jobs wrap their `process()` in `BusyCursored<Job>` (`Jobs/BusyCursorJob.hpp`). Its
  `CursorSetterRAII` marshals `wxBeginBusyCursor`/`wxEndBusyCursor` to the main thread through
  `ctl.call_on_main_thread`.
- Web dialogs bracket blocking work with `wxSetCursor(wxCURSOR_ARROWWAIT)` …
  `wxSetCursor(wxNullCursor)` (`WebViewDialog.cpp`).

### Pitfalls

- **Rule:** Set the hover cursor once and reset with `wxNullCursor`.
  **Why:** the system already switches cursors on enter/leave. Toggling by hand fights it, and an
  explicit `wxCURSOR_ARROW` overrides the cursor inherited from the parent.
  ```cpp
  // Wrong:
  w->Bind(wxEVT_ENTER_WINDOW, [w](wxMouseEvent& e) { w->SetCursor(wxCURSOR_HAND);  e.Skip(); });
  w->Bind(wxEVT_LEAVE_WINDOW, [w](wxMouseEvent& e) { w->SetCursor(wxCURSOR_ARROW); e.Skip(); });
  // Right:
  w->SetCursor(wxCURSOR_HAND);          // once; w->SetCursor(wxNullCursor) to drop it
  ```
- **Rule:** Busy cursors from worker threads go through the main thread
  (`ctl.call_on_main_thread`, `CallAfter`); never call `wxBeginBusyCursor` from a worker.
