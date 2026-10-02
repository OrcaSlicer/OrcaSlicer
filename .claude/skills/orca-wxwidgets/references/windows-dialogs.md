# Windows, dialogs and window lifetime

How windows are created, parented, closed and destroyed; how to tell whether a window is still
alive; how top-level windows show, raise and persist; how modal and modeless dialogs work on each
port; what Orca's `DPIDialog`/`DPIFrame` add; the Orca dialog recipe; and the `MsgDialog` family.
Read it before writing or reviewing any dialog, frame, close handler, `Destroy()`/`delete`, or code
that keeps a pointer to a window across an event, a `CallAfter` or a modal loop.

wx cites are relative to the pinned wx 3.3.2 tree (`find deps -maxdepth 5 -type d -path
'*dep_wxWidgets-prefix/src/dep_wxWidgets'`). wx is built with `wxBUILD_DEBUG_LEVEL=0` and
`libslic3r_gui` with `wxDEBUG_LEVEL=0`: every wx assert quoted below is compiled out, so misuse
fails silently (dropped call, stuck loop, freed memory), never with an assert dialog. "GTK" below
means wxGTK as Orca builds it on Linux: GTK3 by default (X11 or Wayland); GTK2 is only an opt-out.

Contents: [Rules](#rules) · [1 Creating and parenting](#1-creating-and-parenting-windows) ·
[2 Destroy, delete, Close](#2-destroying-windows-destroy-delete-close-and-the-close-event) ·
[3 Liveness](#3-liveness-is-this-window-still-alive) ·
[4 Top-level windows](#4-top-level-windows-show-raise-enable-state-geometry) ·
[5 Window styles](#5-window-styles) · [6 Dialogs and modality](#6-dialogs-and-modality) ·
[7 DPIDialog and DPIFrame](#7-dpidialog-and-dpiframe) ·
[8 The Orca dialog recipe](#8-the-orca-dialog-recipe) ·
[9 MsgDialog family](#9-message-boxes-the-msgdialog-family) ·
[10 Overlay frames](#10-overlay-frames-basetransparentdpiframe)

## Rules

1. Free heap-allocated windows with `Destroy()`, never `delete`; a modal dialog on the stack is freed by its scope and must never be `Destroy()`ed. §2
2. Never `Destroy()` a child window (or an ancestor of it) from inside that child's own event handler: child `Destroy()` is a synchronous `delete this`. Defer with `wxTheApp->ScheduleForDestruction(w)` or a guarded `CallAfter`. §2
3. A close handler either vetoes or destroys/ends the window; prompts and vetoes only when `CanVeto()`; when `!CanVeto()` it must not veto. §2
4. Do irreversible teardown in a close handler only once every veto is decided — the default top-level handler still vetoes a vetoable close while any modal dialog is open. §2
5. Give every dialog and frame a live top-level parent (`parent ? parent : wxGetApp().mainframe`); a parentless window that only hides on close keeps the process alive. §1, §2
6. Never construct a window to reach computation; make the logic `static` or free. §1
7. To ask "is this top-level window still alive", check `wxTheApp->IsScheduledForDestruction(w)` as well as `IsBeingDeleted()`; `wxWeakRef` nulls only at the very end of destruction; cross-thread code uses a `std::shared_ptr<std::atomic<bool>>` alive flag. Re-check after every nested event loop. §3
8. A `wxEVT_DESTROY` handler on a parent fires for every descendant: compare `GetEventObject()` and `Skip()`. For a top-level window it runs after the derived destructor — do derived cleanup in the destructor. §3
9. Accessors reachable from teardown-time events return `nullptr` instead of dereferencing a pimpl or child; the real fix is still to stop the events first. §3
10. Bring a top-level window forward with `if (!w->IsShown()) w->Show(); w->Raise();` — `Raise()` never shows, and a redundant `Show()` is not inert on GTK3. §4
11. On macOS, after a native modal (file/dir dialog, native message box) or a generic progress dialog opened from a secondary top-level window, re-raise that window with a deferred, liveness-guarded `Raise()`. §4
12. Remove style bits with `& ~flag`, never `!flag`. §5
13. Always pass the style to a `DPIDialog`: the `DPIAware` default is `wxDEFAULT_FRAME_STYLE` (resizable, min/max boxes). §5, §7
14. A `wxFRAME_FLOAT_ON_PARENT` frame takes a non-null top-level parent (parent to `mainframe`, not to a panel). §5
15. End a modal dialog with `EndModal(wxID_*)` (or `EndDialog` from inside the class); never with `Hide()`, `Show(false)` or `Destroy()` while its loop runs. A heap modal is `Destroy()`ed after `EndModal()` — normally by the caller once `ShowModal()` returns. §6
16. Return codes are `wxID_*` ids, never the `wxOK/wxCANCEL/wxYES/wxNO/wxCLOSE` style bits (the `MsgDialog` "Go to" button's `wxFORWARD` is the one deliberate exception). §6, §9
17. Nested modal dialogs end innermost first; `DPIDialog::EndModal` refuses (logs, leaves the dialog open) on a dialog that is not the innermost `DPIDialog`. §6, §7
18. Never call `EndModal()` on a modeless dialog; use `Hide()`, `Close()`, `Destroy()`, or `EndDialog(rc)` inside the class. §6
19. Never show a modal dialog directly from a mouse-button or motion handler; defer with `CallAfter`. §6
20. Run cancel cleanup on every close path: ESC and the title-bar close box go through `Close()` and `wxID_CANCEL`, never through an Orca `Button`'s handler. §6, §8
21. Dialogs derive `DPIDialog`, implement `on_dpi_changed`, call `CenterOnParent()` after fitting, and call `wxGetApp().UpdateDlgDarkUI(this)` last; an override of `ShowModal()` calls `DPIDialog::ShowModal()`. §7, §8
22. The bottom row is `DialogButtons` with untranslated labels; bind every button whose default wx handling is not what you want — Yes/No/Confirm/custom buttons never close the dialog by themselves. §8
23. Messages to the user go through the `MsgDialog` family (`MessageDialog`, `RichMessageDialog`, `WarningDialog`, `ErrorDialog`, `InfoDialog`, `show_error`/`show_info`), never `wxMessageBox`/`wxMessageDialog` once the GUI exists; treat `wxID_CANCEL` (ESC/close box) as "no". §9
24. `show_error` is asynchronous and captures its parent pointer; pass a long-lived parent or `nullptr`. §9

---

## 1 Creating and parenting windows

**Contract.**
- Child windows are created shown; top-level windows (frames, dialogs) are created hidden "to allow
  you to create their contents without flicker" (`interface/wx/window.h:3140-3158`).
- "Child windows are deleted from within the parent destructor. This includes any children that are
  themselves frames or dialogs, so you may wish to close these child frame or dialog windows explicitly
  from within the parent close handler" (`docs/doxygen/overviews/windowdeletion.h:96-100`).
- `wxDialog` with a NULL parent "will be owned by the application's top window, if any. Use
  `wxDIALOG_NO_PARENT` style to really make dialog not owned by any window" (`interface/wx/dialog.h:166-172`).
  The style doc adds: orphan dialogs are "not recommended for modal dialogs" (`interface/wx/dialog.h:122-126`).
- **[source]** Only the *native* owner is substituted; `GetParent()` stays NULL. Owner resolution
  (`wxDialogBase::DoGetParentForDialog`, `src/common/dlgcmn.cpp:180-204`): the given parent's
  top-level parent → the active window's top-level parent → `wxApp::GetMainTopWindow()`, each
  rejected if it is pending delete, being deleted, has `wxWS_EX_TRANSIENT`, or — for modal use — is
  not `IsShownOnScreen()` (`CheckIfCanBeUsedAsParent`, `src/common/dlgcmn.cpp:130-178`).

**Two-step creation.** "the underlying window must be created exactly once… if you use the default
constructor… you *must* call Create() before using the window and if you use the non-default
constructor, you can *not* call Create()" (`interface/wx/window.h:411-417`). Methods may be called
between the C++ constructor and `Create()` — `Hide()` to build invisibly, `Enable(false)`,
`SetExtraStyle()` for create-time extra styles (`interface/wx/window.h:419-428, 3120-3126`; `interface/wx/dialog.h:127-133`).
```cpp
auto* panel = new wxPanel();          // C++ object only
panel->Hide();                        // legal before Create
panel->Create(parent, wxID_ANY);      // native window, still hidden
/* build children */  panel->Show();
```
**[source]** `Destroy()` on a never-created window skips `wxEVT_DESTROY` (`src/common/wincmn.cpp:559-573`),
and a never-created top-level window is deleted immediately (`src/common/toplvcmn.cpp:102-112`; not on
macOS, whose `Destroy()` always defers, §2 table).
Orca's widgets (`StaticBox`, `Button`, `TextInput`, `SpinInput`) follow the layering:
default constructor + `Create(...)`, the convenience constructor delegates to both, and each
`Create` calls its base `Create` first, then attaches `state_handler`. `ComboBox` has only the
convenience constructor, which default-constructs its `TextInput` base and calls `TextInput::Create`.
Subclasses keep that layering.

**Reparent.** `Reparent(newParent)` moves the window between children lists (and between
`wxTopLevelWindows` and a parent); "you need to explicitly call wxNotebook::RemovePage() before
reparenting a notebook page" (`interface/wx/window.h:730-742`). **[source]** It does not touch
sizers (`src/common/wincmn.cpp:1339-1377`); GTK hides the widget and re-shows it on idle if the new parent is
visible (`src/gtk/window.cpp:5195-5230`); MSW just calls `::SetParent`.

**Platforms.**
- MSW fixes a dialog's native owner at `Create` time (`wxTopLevelWindowMSW::CreateDialog` →
  `GetParentForModalDialog`, `src/msw/toplevel.cpp:337-352`). A dialog created while its parent is
  not on screen (e.g. inside the parent frame's constructor) is owned by the active window, the main
  top window, or nothing — wrong z-order and taskbar grouping.
- GTK sets `transient_for` for modal dialogs in `ShowModal()` (`src/gtk/dialog.cpp:139-144`), and for
  dialogs/float-on-parent/stay-on-top frames with a top-level parent in `Create`
  (`src/gtk/toplevel.cpp:774-782`).
- Wayland: the compositor places top-level windows (see §4); dialog placement comes from
  `transient_for`, so a real parent is the only positioning control there is.

**OrcaSlicer.** Dialogs take `parent ? parent : wxGetApp().mainframe` — never orphan a dialog (the
`MsgDialog` constructor does this substitution itself). Passing a child panel as parent is legal:
the dialog dies with that panel (§2) and `CenterOnParent()` centres on the panel's screen rect
(`wxTopLevelWindowBase::DoCentre`, `src/common/toplvcmn.cpp:239-279`); with no wx parent it centres on the display.

**Pitfalls.**
- **Rule:** Never construct a window — especially one hosting a `wxWebView` — just to call one of
  its computation methods; make the computation `static` or a free function.
  **Why:** window construction has heavy native side effects (native handles, webview processes,
  event bindings). `is_flush_config_modified()` built a full `WipingDialog` (with a WebView) only to
  call `CalcFlushingVolumes()`; it runs on the UI-rebuild path (`Sidebar::msw_rescale`,
  `Sidebar::sys_color_changed`), and on macOS the language switch froze the app.
  ```cpp
  // Wrong
  WipingDialog dlg(wxGetApp().mainframe, extra_flush_volumes);
  auto m = dlg.CalcFlushingVolumes(i);
  // Right
  static VolumeMatrix CalcFlushingVolumes(int extruder_id);   // no window needed
  auto m = WipingDialog::CalcFlushingVolumes(i);
  ```
  Cite: 80f4a7a40f (`WipingDialog::CalcFlushingVolumes`/`CalcFlushingVolume` in `src/slic3r/GUI/WipeTowerDialog.hpp`).
- **Rule:** Create a dialog lazily, after its parent is on screen, when MSW ownership matters.
  **Why:** the native owner is chosen at `Create` and a hidden parent is rejected (above).
- **Rule:** `Reparent()` does not move sizer membership.
  ```cpp
  // Wrong: w->Reparent(p);
  // Right:
  old_sizer->Detach(w); w->Reparent(p); new_sizer->Add(w, 0, wxEXPAND); p->Layout();
  ```
- **Rule:** Call `Create()` exactly once; never on an object built with the non-default constructor.

---

## 2 Destroying windows: Destroy, delete, Close and the close event

**Contract.**
- `Destroy()`: "Use this function instead of the delete operator, since different window classes can
  be destroyed differently. Frames and dialogs are not destroyed immediately when this function is
  called -- they are added to a list of windows to be deleted on idle time, when all the window's
  events have been processed. This prevents problems with events being sent to non-existent windows."
  Returns true "if the window has either been successfully deleted, or it has been added to the list of
  windows pending real deletion" (`interface/wx/window.h:3607-3618`).
- Destructor: "Deletes all sub-windows, then deletes itself. Instead of using the delete operator
  explicitly, you should normally use Destroy() so that wxWidgets can delete a window only when it is
  safe to do so, in idle time" (`interface/wx/window.h:389-394`). `DestroyChildren()` is called automatically by the
  destructor (`interface/wx/window.h:626`).
- "Windows with parents, such as controls, don't have delayed destruction… For consistency, continue
  to use the wxWindow::Destroy function instead of the delete operator when deleting these kinds of
  windows explicitly" (`docs/doxygen/overviews/windowdeletion.h:103-109`).
- `Close(force)` "simply generates a wxCloseEvent whose handler usually tries to close the window. It
  doesn't close the window itself"; `force=true` means the handler cannot veto; returns "true if the
  event was handled and not vetoed" — a handler that hides instead of destroying still makes it return
  true. "Calling Close does not guarantee that the window will be destroyed… To guarantee that the
  window will be destroyed, call wxWindow::Destroy instead" (`interface/wx/window.h:3574-3605`).
- Close handler: "If [CanVeto()] is false, you *must* destroy the window using wxWindow::Destroy… If you
  don't destroy the window, you should call wxCloseEvent::Veto" (`interface/wx/event.h:4708-4717`).
  "The wxCloseEvent handler should only call wxWindow::Destroy to delete the window, and not use the
  delete operator" (`docs/doxygen/overviews/windowdeletion.h:38-42`).
- Defaults (`docs/doxygen/overviews/windowdeletion.h:63-73`): wxDialog's close handler simulates `wxID_CANCEL`; the cancel
  handler hides a modeless dialog or `EndModal(wxID_CANCEL)`s a modal one — "the dialog *is not*
  destroyed (it might have been created on the stack)". wxFrame's default close handler calls `Destroy()`.
  **[source]** The top-level default (`wxTopLevelWindowBase::OnCloseWindow`, `src/common/toplvcmn.cpp:535-546`)
  **vetoes** a vetoable close while `wxModalDialogHook::GetOpenCount() > 0` (any app-modal wx or native
  dialog open), otherwise calls `Destroy()`.
- Deleting from inside a handler: "it may be unsafe for an event handler to delete the object which
  generated the event because more events may be still pending for the same object. In this case the
  handler may call ScheduleForDestruction() instead" (`interface/wx/app.h:191-212`); without an event
  loop it deletes immediately.

**When does `Destroy()` actually free the window?** **[source]**

| Window | `Destroy()` does | Cite |
|---|---|---|
| Child (control, panel) | sends `wxEVT_DESTROY`, then `delete this` **synchronously**; the window detaches from its containing sizer in `~wxWindowBase` | `src/common/wincmn.cpp:559-573` |
| Top-level, normal case | appends to `wxPendingDelete`, hides unless it is the last visible top-level window; deleted by `DeletePendingObjects()` at the next idle — which also runs inside full yields and modal loops (`wxYield`, `ShowModal`), not inside a masked `YieldFor` (progress-dialog `Update`) | `src/common/toplvcmn.cpp:102-142`; `src/common/appbase.cpp:643-662` |
| Top-level, parent already being deleted, or never created (not macOS) | deleted immediately | `src/common/toplvcmn.cpp:102-112` |
| Top-level, macOS | always deferred and always `Hide()`n | `src/osx/toplevel_osx.cpp:96-105` |
| Top-level, MSW | base behaviour plus `wxWakeUpIdle()` so iconized windows still get deleted | `src/msw/toplevel.cpp:771-784` |
| Any child of a dying parent, top-level children included | `DestroyChildren` calls the non-virtual `wxWindowBase::Destroy()`: deleted **immediately**, not queued | `src/common/wincmn.cpp:586-607` |
| `wxPopupTransientWindow` | deferred to idle (not hidden); a second `Destroy()` fails a `wxCHECK` — see `references/popups-menus.md` | `src/common/popupcmn.cpp` `wxPopupTransientWindowBase::Destroy` |

**Close-handler shapes** (pick one per window, never mix; `confirm_discard`/`cleanup` stand for your code):
```cpp
// destroy-on-close (a frame, or a modeless dialog the user owns)
Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
    if (e.CanVeto() && !confirm_discard()) { e.Veto(); return; }
    cleanup();             // members still alive here
    e.Skip();              // frame: default Destroy() (vetoed while a modal is open, Rule 4)
    // modeless dialog: call Destroy() instead -- its default only EndDialog(wxID_CANCEL)s = hides it
});
// hide-on-close (a reusable window someone else owns) — TextureProjectorFrame's shape
Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
    if (e.CanVeto()) { e.Veto(); Hide(); } else e.Skip();   // a forced close must still destroy
});
// usable both modal and modeless — WebDialog::on_close_window's shape
void on_close(wxCloseEvent&) { if (IsModal()) EndModal(wxID_CANCEL); else Destroy(); }
```
A dialog's `e.Skip()` reaches `wxDialogBase::OnCloseWindow`, which **ends** the dialog
(`EndDialog(wxID_CANCEL)` = `EndModal` or `Hide`) but never destroys it: a modeless dialog closed
with the close box is only hidden (`src/common/dlgcmn.cpp:525-559`).

**Application exit.** The app exits when the last top-level window is destroyed
(`docs/doxygen/overviews/windowdeletion.h:89-93`; `SetExitOnFrameDelete(false)` disables it, `interface/wx/app.h:1195-1207`). "By default,
the application stays alive as long as there are any open top level windows" — hidden ones count;
override `ShouldPreventAppExit()` to return false for unimportant windows (`interface/wx/toplevel.h:650-657`).
**[source]** `IsLastBeforeExit` runs from `~wxTopLevelWindowBase` (`src/common/toplvcmn.cpp:93-97, 144-189`);
`GetTopWindow()` skips windows pending delete (`src/common/appcmn.cpp:185-208`).

**OrcaSlicer — shapes to follow.**
- *Modeless singleton owned by `GUI_App`* (`GUI_App::open_terminal_dialog`, `GUI_App::open_speed_dial`):
  ```cpp
  if (!m_dlg) {
      m_dlg = new TerminalDialog(mainframe, wxID_ANY, _L("Plugin Terminal"));
      m_dlg->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent& e) {
          if (e.GetEventObject() == m_dlg) m_dlg = nullptr;   // the event also arrives from children
          e.Skip();
      });
  }
  if (!m_dlg->IsShown()) m_dlg->Show();
  m_dlg->Raise();
  ```
  These dialogs bind no close handler, so the close box only hides them (the wx default for a modeless
  dialog, below) and the reopen path re-shows the same window; they die with their parent `mainframe`
  (or an explicit `Destroy()`, as `GUI_App::recreate_GUI` does for the speed dial), and the
  `wxEVT_DESTROY` reset clears the pointer then. A reset that does not compare `GetEventObject()` clears
  the pointer when any child of the dialog is destroyed. `open_terminal_dialog` also wraps all of this in
  `CallAfter` because it is reached from a webview script message (§6).
- *Dual-mode dialog* (`WebDialog`): close handler `IsModal() ? EndModal(wxID_CANCEL) : Destroy()`;
  forced teardown (`WebDialog::destroy_silently`) calls `EndModal` first, then `Destroy()`, because
  destroying a modal "can leave ShowModal() running". Registry cleanup runs in `~WebDialog`, explicitly
  not in a `wxEVT_DESTROY` handler (comment there: the event comes from the base `~wxDialog`, after the
  members died).
- *Hide-on-close reusable frame* (`TextureProjectorFrame`): veto + `Hide()` when `CanVeto()`, else
  `Skip()`. The owner (the texture gizmo) holds the pointer; it is parented to `mainframe` because
  `wxFRAME_FLOAT_ON_PARENT` needs a real top-level parent.
- *Pseudo-modal modeless dialog* (`ParamsDialog`): see §6 "Modality helpers".
- *By-value top-level member*: `MainFrame::m_settings_dialog` is a `SettingsDialog` (a `DPIDialog`)
  stored by value with a NULL parent; its close handler only `Hide()`s. Such a window must never be
  `Destroy()`ed or `delete`d — it dies in member destruction, before the frame's base destructors.
  Prefer a heap child for new code.
- *Main-frame replacement* (`GUI_App::recreate_GUI`): `mainframe->shutdown()`, create the new
  `MainFrame`, `SetTopWindow(new_frame)`, then `old->Destroy()` — new frame first, so wx never sees
  "last top-level window gone" and exits.
- *Main-frame close* (`MainFrame` constructor's `wxEVT_CLOSE_WINDOW` lambda): every prompt and veto is
  gated by `event.CanVeto()` (gizmo editing, `Plater::close_with_confirm`, print-host queue); then
  `set_closing(true)`, `m_plater->reset()`, `shutdown()`, `event.Skip()` → default `Destroy()`. Cmd+Q on
  macOS posts a vetoable close (`wxPostEvent(this, wxCloseEvent(wxEVT_CLOSE_WINDOW))`); updater paths use
  `Close(true)` to force. Its teardown runs before `Skip()`, so a vetoable close that arrives while a
  modal is open (the `wxEVT_QUERY_END_SESSION` handler in `GUI_App::on_init_inner` sends exactly that) is
  vetoed by the default handler after teardown — the hazard of Rule 4; don't copy that ordering into new
  handlers. Sequence detail: `references/orca-architecture.md`.
- *Lifetime-tied cleanup*: `GUI_App::recreate_GUI` attaches a `wxClientData` subclass with
  `SetClientObject`; its destructor runs in `~wxEvtHandler`, after all children are gone.
- *Lazily built window* (`Lazy<T>` / `LazyInstance<T>` in `Lazy.hpp`, e.g. `MainFrame::m_diff_dialog`, a
  `Lazy<DiffPresetDialog>`): the holder builds the window on demand or at idle, but the window's parent
  owns it; code that only needs an already-built instance asks `T::if_built()` instead of forcing a
  build. Design: `docs/HLSD/deferred-page-construction.md`, `references/orca-architecture.md`.

**Pitfalls.**
- **Rule:** Destroy heap-allocated top-level windows with `Destroy()`, never `delete`; stack-allocated
  modal dialogs are destroyed by scope.
  **Why:** `delete` skips the pending-delete queue, and for a top-level window `wxEVT_DESTROY` is then
  sent only from the base destructor. `Destroy()` *delays* deletion; it does not stop events: queued
  events and `CallAfter`s of a window sitting in `wxPendingDelete` still run (§3), so deferred code
  still needs liveness checks.
  ```cpp
  // Wrong: auto* dlg = new MyDialog(this); dlg->ShowModal(); delete dlg;
  // Right: auto* dlg = new MyDialog(this); dlg->ShowModal(); dlg->Destroy();
  // Right: MyDialog dlg(this); dlg.ShowModal();
  ```
  Cite: 0a0d59b76b (`detail::run_off_thread_with_progress` in `src/slic3r/GUI/PluginsDialog.hpp`: the
  worker body in `try/catch`, then one main-thread `CallAfter` that does `timer->Stop(); delete timer;`
  and `progress->Destroy()` only while the host's alive flag is set, or when the caller passed no flag —
  the progress dialog is a child of the host and has already died with it otherwise, §2 table). Older code that `delete`s a dialog
  after `ShowModal()` is legacy; don't extend it.
- **Rule:** Never `Destroy()` a control (or its ancestor panel) from inside that control's own handler.
  ```cpp
  // Wrong: m_btn->Bind(wxEVT_BUTTON, [this](auto&) { m_panel->Destroy(); });  // m_btn is inside m_panel
  // Right:
  m_btn->Bind(wxEVT_BUTTON, [this](auto&) { wxTheApp->ScheduleForDestruction(m_panel); m_panel = nullptr; });
  ```
  **Why:** child `Destroy()` deletes synchronously; the dispatcher then returns into freed memory.
- **Rule:** A close handler that neither destroys nor vetoes, or that vetoes without checking
  `CanVeto()`, is wrong.
  **Why:** `Close(true)`, session end and the default top-level handler rely on a non-vetoable close
  destroying the window; a hide-only handler leaks the window, and a parentless leaked window keeps the
  process alive after the main frame is gone. Parent it to `mainframe`, `Destroy()` it, or override
  `ShouldPreventAppExit()`.
- **Rule:** Do irreversible teardown only when the close is certain.
  **Why:** a handler that tears down and then `Skip()`s can still be vetoed by
  `wxTopLevelWindowBase::OnCloseWindow` when the close is vetoable and any modal dialog is open (for
  example a vetoable close sent while a dialog is up, as the `wxEVT_QUERY_END_SESSION` path does): the
  window survives, half torn down.
  ```cpp
  // Wrong
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { shutdown(); e.Skip(); });
  // Right
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
      if (e.CanVeto() && wxModalDialogHook::GetOpenCount() > 0) { e.Veto(); return; }
      shutdown(); e.Skip();
  });
  ```
- **Rule:** A `wxTimer` must not outlive the handler that receives its events — make it a member
  (`wxTimer m_timer{this}`) or delete it before the owner dies. Detail in `references/threads-timers-app.md` §wxTimer.

---

## 3 Liveness: is this window still alive?

**`IsBeingDeleted()`.** Doc: true "if this window, or one of its parent windows, is scheduled for
destruction and can be useful to avoid manipulating it as it's usually useless to do something with a
window which is at the point of disappearing anyhow" (`interface/wx/window.h:3620-3633`). **[source]** Wrong for top-level windows: the flag is
set only by `SendDestroyEvent()`, i.e. when deletion actually starts (`src/common/wincmn.cpp:541-557`);
`wxTopLevelWindowBase::Destroy()` only queues and hides, so **after `tlw->Destroy()` returns,
`tlw->IsBeingDeleted()` is false until idle**. The parent walk also stops at a top-level window
(`m_isBeingDeleted || (!IsTopLevel() && m_parent->IsBeingDeleted())`, `src/common/wincmn.cpp:535-539`): a dialog
does not report its parent frame's deletion.

**`wxApp::IsScheduledForDestruction(obj)` / `ScheduleForDestruction(obj)`** (`interface/wx/app.h:191-221`):
the first answers "has `Destroy()` (or `ScheduleForDestruction`) already been called"; both share
`wxPendingDelete` (`src/common/appbase.cpp:624-641`). `ScheduleForDestruction` defers deletion of *any*
`wxObject`, children included; **[source]** it deletes with plain `delete` at idle (`src/common/appbase.cpp:643-662`).

| After… | `IsBeingDeleted()` | `IsScheduledForDestruction()` | `wxWeakRef` |
|---|---|---|---|
| `child->Destroy()` returned | object gone | object gone | null |
| `tlw->Destroy()` returned, before idle | **false** | true | **non-null** |
| inside `wxEVT_DESTROY` handlers and the wx base destructors | true | false (already removed) | **non-null** until `~wxTrackable` |
| inside a top-level window's own derived destructor (deleted at idle or by `delete`) | **false** — `SendDestroyEvent()` runs later, in `~wxFrameBase` or the port's top-level/dialog destructor (`src/common/framecmn.cpp:200`, `src/msw/toplevel.cpp:522`, `src/gtk/toplevel.cpp:975`, `src/osx/dialog_osx.cpp:90`) | false | non-null |

**`wxWeakRef<T>`** auto-resets "when the object pointed is destroyed"; works for `wxEvtHandler`/`wxWindow`
(`interface/wx/weakref.h:40-99`). **[source]** The reset happens in `~wxTrackable`, the last base
destructor (`include/wx/tracker.h`) — non-null throughout the destroy sequence and while a top-level window
sits in the pending list. The tracker list is unlocked: main thread only. Orca uses it for the splash
(`wxWeakRef<SplashScreen> scrn` in `GUI_App::on_init_inner`), `g_delay_webviews` (`Widgets/WebView.cpp`),
`DockPanel`, `GuideFrame`.

**`wxWindowPtr<T>`** is a shared pointer that calls `Destroy()` at refcount 0
(`interface/wx/windowptr.h:10-26`). It does not track: if the window dies another way (parent deletion,
default frame close) the last release calls `Destroy()` on freed memory. Use it only for parentless,
self-owned top-level windows — the documented `ShowWindowModalThenDo` idiom (§6).

**`wxEVT_DESTROY`** (`wxWindowDestroyEvent`, `interface/wx/event.h:4525-4553`): for top-level windows it
is sent "by wxFrame or wxDialog destructor, i.e. after the destructor of the derived class was executed";
for children "just before deleting the window from wxWindow::Destroy()… or from the window destructor if
operator delete was used directly". **[source]** It derives from `wxCommandEvent` and **propagates to the
parent** unless the parent is being deleted (`wxWindowBase::TryAfter`, `src/common/wincmn.cpp:3499-3522`), so a
parent's handler fires for every descendant destroyed before it. Derived-class code that must run at
destruction goes in the derived destructor (or call `SendDestroyEvent()` there).

**Pending events and nested loops.** **[source]** `ProcessPendingEvents` runs queued events and
`CallAfter`s of a top-level window that is already in `wxPendingDelete` (`src/common/appbase.cpp:561-601`); only the
actual deletion discards them (`~wxEvtHandler` → `DeletePendingEvents`, `src/common/event.cpp`). Idle
events are skipped for pending-delete windows (`src/common/appcmn.cpp:405-428`). "Deferred" does not mean "after the
current handler returns": `ShowModal()`, `wxYield()` and `wxSafeYield()` run pending events *and*
`DeletePendingObjects()` (`src/common/evtloopcmn.cpp:172-192`), so a raw pointer to a `Destroy()`ed
window dies across any of them, and a lambda queued before a modal opens can run while the caller is
still inside `ShowModal()`. A masked `YieldFor` — `wxProgressDialog::Update`/`Pulse` — runs no idle pass
and only the queued events its categories allow (none on GTK): `references/threads-timers-app.md`
§Event categories and yields. `CallAfter` liveness mechanics (self-queued calls are dropped
with their handler; `wxGetApp().CallAfter` calls are not): `references/events.md` §CallAfter. Yields:
`references/threads-timers-app.md`.

**OrcaSlicer.**
- Alive flag for anything that may run after the window died, including worker threads:
  `std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);`, set false in the
  destructor (`PluginsDialog::~PluginsDialog`), captured by value, checked inside the lambda. Unlike
  `wxWeakRef` it flips at the *start* of the derived destructor and is thread-safe.
- App shutdown gate: `wxGetApp().is_closing()`; background-to-UI callbacks check it before posting and
  again inside the `CallAfter` lambda (`references/threads-timers-app.md`).

**Pitfalls.**
- **Rule:** For a top-level window that something may have `Destroy()`ed, test both flags.
  ```cpp
  // Wrong: if (!tlw->IsBeingDeleted()) use(tlw);
  // Right: if (!wxTheApp->IsScheduledForDestruction(tlw) && !tlw->IsBeingDeleted()) use(tlw);
  ```
- **Rule:** Don't trust `wxWeakRef` inside teardown code; combine it with `IsBeingDeleted()` /
  `IsScheduledForDestruction()` or an alive flag.
- **Rule:** In a `wxEVT_DESTROY` handler bound on a parent, compare the event object and `Skip()`.
  ```cpp
  // Wrong: parent->Bind(wxEVT_DESTROY, [this](auto&) { m_child = nullptr; });
  // Right:
  parent->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent& e) {
      if (e.GetEventObject() == m_child) m_child = nullptr;
      e.Skip();
  });
  ```
- **Rule:** After `ShowModal()` returns, re-validate anything the modal loop could have destroyed (a
  popup, a panel rebuilt on a language or preset change) — hold a `wxWeakRef` or alive flag and re-check.
- **Rule:** Make accessors that are reachable from teardown-time events null-safe: guard the pimpl or
  child pointer and return `nullptr` instead of dereferencing.
  **Why:** on macOS, close and shutdown still deliver events (render, idle, focus) that call back into
  widget accessors after internals are gone; `Plater::get_view3D_canvas3D()` crashed on app close until it
  checked its pimpl. `Plater::~Plater() = default` destroys `std::unique_ptr<priv> p` *before* the base
  `wxWindow` destructor runs `DestroyChildren()` (`src/osx/window_osx.cpp:248`, `src/gtk/window.cpp:3105`,
  `src/msw/window.cpp:421`; `src/common/wincmn.cpp:586-609`), so events raised while children die see a dead pimpl.
  The null check works only because libc++'s `~unique_ptr` resets before deleting; the standard does not
  require that, and touching a member after its destructor ran is UB. Treat the guard as a last line of
  defence; the real fix is to stop the events (unbind, `is_closing()`) before teardown.
  ```cpp
  return p->view3D->get_canvas3d();                  // Wrong: crash on close
  return p ? p->view3D->get_canvas3d() : nullptr;    // Right
  ```
  Cite: a162e3f031 (`Plater::get_view3D_canvas3D` in `src/slic3r/GUI/Plater.cpp`).

---

## 4 Top-level windows: show, raise, enable, state, geometry

**Show / Hide.** `Show()` returns false when nothing changed (`interface/wx/window.h:3140-3158`).
`IsShownOnScreen()` = shown and every parent up to the top-level window shown (`interface/wx/window.h:3100-3106`).
**[source]** GTK3: `wxTopLevelWindowGTK::Show` calls `GTKSendSizeEventIfNeeded()` even when nothing
changes, so a redundant `Show(true)` can flush a pending size event into layout handlers
(`src/gtk/toplevel.cpp:1258-1268`). X11 (GTK2/GTK3 without client-side decorations): the first `Show()`
may be deferred until `_NET_FRAME_EXTENTS` arrives — `IsShown()` is already true but the window is not
mapped (`src/gtk/toplevel.cpp:1141-1243`).

**Raise.** "only requests the window manager to raise this window… If the window is currently hidden,
this function does *not* show it", top-level windows only (`interface/wx/window.h:3013-3033`); true on all ports
since 3.3 (`docs/changes.txt:144-146`). **[source]** MSW = `::SetForegroundWindow`, subject to the
foreground lock — Windows may only flash the taskbar button (`src/msw/toplevel.cpp:650-655`); GTK =
`gtk_window_present` only if shown (`src/gtk/toplevel.cpp:1301-1310`; during a deferred X11 first show it
already counts as shown); macOS = `makeKeyAndOrderFront` only if shown (`src/osx/nonownedwnd_osx.cpp:289-295`, `src/osx/cocoa/nonownedwnd.mm:897-899`),
which also makes a `wxPopupWindow` the key window — never `Raise()` a popup (`references/popups-menus.md` §5, §10).

**Enable.** `Enable(false)` on a parent disables children logically: `IsEnabled()` reflects ancestors,
`IsThisEnabled()` the window's own flag (`interface/wx/window.h:3060-3070, 3116-3138`). **[source]** On MSW/macOS wx
propagates through `NotifyWindowOnEnableChange` → `DoEnable` on non-top-level children — not through the
virtual `Enable()` — and skips children entirely when a top-level window is disabled, so a modal dialog
does not grey the frame (`src/common/wincmn.cpp:1150-1191`); GTK relies on native sensitivity. Orca widgets update
their painted state only from their own `Enable()` override, so disabling an ancestor leaves them
looking enabled: `references/orca-widgets.md`.

**State and geometry** (`interface/wx/toplevel.h`):

| API | Contract / platform note |
|---|---|
| `Iconize()`, `Maximize()` | on wxGTK "the change… is not immediate" (`:260-274, 335-346`); **[source]** MSW on a hidden window only records the state for the next show (`src/msw/toplevel.cpp:661-735`) |
| `Restore()` | on wxGTK does not unmaximize — call `Maximize(false)` (`:394-404`) |
| `ShowFullScreen(show, style)` | also shows a hidden window (`:730-750`) |
| `EnableFullScreenView()` | wxOSX only; then `ShowFullScreen` uses the native Spaces API and only `wxFULLSCREEN_NOTOOLBAR|NOMENUBAR` apply (`:697-728`); `wxEVT_FULLSCREEN` is macOS-only, only with it, and not generated by `ShowFullScreen()` (`interface/wx/event.h:2391-2412`) |
| `RequestUserAttention()` | documented for Win32 (taskbar flash) and wxGTK (`:375-392`); **[source]** macOS bounces the dock icon (`src/osx/cocoa/nonownedwnd.mm:1306`) |
| `SetIcon/SetIcons` | MSW needs a 16×16 or 32×32 icon; no effect on Wayland — ship a `.desktop` file (`:506-546`); **[source]** no macOS override: stored, never shown |
| `SetSizeHints/SetMinSize/SetMaxSize` | on a top-level window they also constrain programmatic `SetSize()` (`:576-603`) |
| `SetTransparent()` | on wxGTK call it before the first show (`:632-648`) |
| `EnableMaximizeButton/EnableMinimizeButton` | MSW and macOS only (`:172-203`) |
| `EnableCloseButton` | all ports, but its result is unreliable on X11, GTK included (`:160-170`) |
| `wxEVT_MOVE_START/END` | wxMSW only (`:80-87`) |
| `wxEVT_SHOW` | not sent for iconize/restore on MSW (`interface/wx/event.h:4905-4914`) |
| `SaveGeometry/RestoreToGeometry`, `wxPersistentTLW` | serializer-based persistence (`:406-492`; `interface/wx/persist/toplevel.h`) — not used by Orca |

**Wayland.** `SetIcon(s)` do nothing (`interface/wx/toplevel.h:518-521, 539-542`); the app id comes from
`SetClassName` with GTK ≥ 3.24.22 (`interface/wx/app.h:765-771`). `SetPosition()`/`Move()` on a
top-level window is a no-op (the compositor places windows), so `CentreOnParent` and saved positions are
ignored (observed; recorded in `GUI_App::window_pos_restore`, not documented by wx). Detection: `Slic3r::GUI::is_running_on_wayland()`; see `references/platforms.md`.

**OrcaSlicer geometry persistence.** Orca uses `AppConfig`, not `wxPersistentTLW`:
`GUI_App::window_pos_save` / `window_pos_restore` / `window_pos_sanitize` / `window_pos_center` (key
`window_<name>`, a `WindowMetrics` = screen rect + maximized; restore skips `SetPosition` on Wayland), and
`on_window_geometry(tlw, callback)` (`GUI_Utils.cpp`) to run the callback when geometry is real — MSW
immediately (no `wxEVT_SHOW` for windows created maximized), Linux on `wxEVT_SHOW` + `CallAfter`, macOS on
`wxEVT_SHOW`. `GUI_App::persist_window_geometry(window, default_maximized)` saves on
`wxEVT_CLOSE_WINDOW` (then `Skip()`) but always uses the key `window_mainframe`, whatever the window's
name — for any other window call `window_pos_save/restore` with its own name.

**Pitfalls.**
- **Rule:** When restoring a top-level window (the main frame after hiding a popup or overlay frame, or a
  singleton dialog being re-opened), call `Show()` only if `!IsShown()`, and call `Raise()` unconditionally.
  **Why:** `Raise()` never shows a hidden window (3.3 on all ports); on GTK3 a redundant `Show(true)` runs
  `GTKSendSizeEventIfNeeded()`, flushing a pending size event into layout handlers — in Orca this froze the
  app permanently after hiding the filament-sync popup.
  ```cpp
  // Wrong
  mainframe->Show(); mainframe->Raise();
  // Right
  if (!mainframe->IsShown()) mainframe->Show();
  mainframe->Raise();
  ```
  Cite: dd8cb89f6d (`BaseTransparentDPIFrame::on_hide`); the same guard in `GUI_App::open_terminal_dialog`.
- **Rule:** On macOS, after any native modal (`wxFileDialog`/`wxDirDialog`, `wxMessageBox`/`wxMessageDialog`)
  or a generic `wxProgressDialog` opened from a secondary top-level dialog, re-raise that dialog with a
  deferred, liveness-guarded `Raise()` (`CallAfter` + alive flag + `IsShown()`).
  **Why:** when the native panel closes, macOS re-activates the app's main window instead of the dialog
  that opened it, burying the dialog behind the main frame (observed; not documented by wx). wx compensates
  only for its own `wxDialog` modals — `EndModal` raises the parent (`src/osx/dialog_osx.cpp:191-202`) —
  while `wxFileDialog::ShowModal` runs `[panel runModal]` with no such step (`src/osx/cocoa/filedlg.mm:597-620`),
  and on macOS `wxProgressDialog` is the generic dialog (only MSW has a native one,
  `include/wx/progdlg.h:30-37`), normally shown modeless behind a disabler and destroyed, never ended
  through `EndModal`. The `Raise()` is deferred to run after the modal has fully torn down, and guarded
  because the dialog may be destroyed while queued.
  ```cpp
  void PluginsDialog::restore_z_order()
  {
      wxGetApp().CallAfter([this, alive = m_alive]() {
          if (alive->load(std::memory_order_acquire) && IsShown())
              Raise();
      });
  }
  ```
  Cite: 0a0d59b76b (`PluginsDialog::restore_z_order`; also passed as the `restore` callback of
  `detail::run_off_thread_with_progress`).
- **Rule:** Never position windows by absolute coordinates on Wayland, and never expect `SetIcon` to show on
  macOS or Wayland.

---

## 5 Window styles

**Contract.**
- `wxDEFAULT_FRAME_STYLE` = `wxSYSTEM_MENU | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX |
  wxCLOSE_BOX | wxCAPTION | wxCLIP_CHILDREN` (`interface/wx/toplevel.h:55-61`); `wxDEFAULT_DIALOG_STYLE`
  = `wxCAPTION | wxSYSTEM_MENU | wxCLOSE_BOX`, `wxSYSTEM_MENU` unused under Unix (`interface/wx/dialog.h:20, 99-101`).
  Non-resizable frame: `wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX)`.
- `wxMINIMIZE_BOX`, `wxMAXIMIZE_BOX`, `wxCLOSE_BOX` implicitly enable `wxCAPTION` "on most systems"
  (`interface/wx/dialog.h:94-114`, `interface/wx/frame.h:61-80`). `wxMAXIMIZE_BOX` is ignored on wxGTK without
  `wxRESIZE_BORDER` (`interface/wx/frame.h:75-78`). The `wxMAXIMIZE` style works on Windows and GTK only; `wxICONIZE`/
  `wxMINIMIZE` on Windows only (`interface/wx/frame.h:59-74`).
- `wxSTAY_ON_TOP`: above all other windows. `wxFRAME_FLOAT_ON_PARENT`: above its parent only, "must have a
  non-null parent". `wxFRAME_NO_TASKBAR`: no taskbar button on Windows/GTK (GTK only with
  `_NET_WM_STATE_SKIP_TASKBAR` support). `wxFRAME_TOOL_WINDOW`: small title bar, no taskbar button
  (`interface/wx/frame.h:82-106`).
- Borders: `wxBORDER_NONE/SIMPLE/SUNKEN/RAISED/STATIC(MSW)/THEME`; `wxTRANSPARENT_WINDOW` is obsolete and
  does nothing (`interface/wx/window.h:191-220`).
- Extra styles (`SetExtraStyle`, some must precede two-step `Create`): `wxWS_EX_BLOCK_EVENTS`,
  `wxWS_EX_TRANSIENT` ("Don't use this window as an implicit parent… risk of creating a dialog/frame with
  this window as a parent, which would lead to a crash"), `wxWS_EX_PROCESS_IDLE`,
  `wxWS_EX_PROCESS_UI_UPDATES` (`interface/wx/window.h:262-290`). Dialogs set `wxWS_EX_BLOCK_EVENTS` by default
  (`src/common/dlgcmn.cpp:124-127`): command events from inside a dialog never reach its parent frame;
  frames do not block.

**Platforms. [source]**

| Style | MSW | macOS | GTK |
|---|---|---|---|
| MIN/MAX/CLOSE_BOX | force `WS_CAPTION` (`src/msw/toplevel.cpp:132-137`) — a custom title bar must strip it | — | — |
| frame with a parent, no `FLOAT_ON_PARENT` | unowned; gets its own taskbar button (`WS_EX_APPWINDOW`) unless `wxFRAME_NO_TASKBAR` (`src/msw/toplevel.cpp:193-244`) | — | — |
| `wxFRAME_FLOAT_ON_PARENT` | native owner = `GetHwndOf(parent)` as given (`MSWGetParent`, `src/msw/toplevel.cpp:212-240`) — pass a top-level window | `NSFloatingWindowLevel`, a *global* level: floats above other apps' windows too (`src/osx/cocoa/nonownedwnd.mm:835-876`) | `transient_for` the parent's top-level window, set only in `Create` (`src/gtk/toplevel.cpp:774-782`) |
| `wxFRAME_TOOL_WINDOW` | small caption, no taskbar | `NSFloatingWindowLevel` | no taskbar |
| `wxSTAY_ON_TOP` | `WS_EX_TOPMOST` | `NSModalPanelWindowLevel` | keep-above + `transient_for` in `Create` (`src/gtk/toplevel.cpp:774-791`); runtime change honoured |
| runtime `SetWindowStyleFlag` | — | re-levels the window (`src/osx/cocoa/nonownedwnd.mm:1038-1056`) | updates only `STAY_ON_TOP` and `NO_TASKBAR` (`src/gtk/toplevel.cpp:1943-1968`) |

macOS dialogs are not attached as Cocoa child windows (no `addChildWindow`, `src/osx/cocoa/nonownedwnd.mm:947-991`),
so they do not move with their parent; non-tool windows get `setHidesOnDeactivate:NO`.

**OrcaSlicer.**
- `DPIAware`'s constructor defaults `style = wxDEFAULT_FRAME_STYLE` and `name = wxFrameNameStr` for
  dialogs too: `DPIDialog(parent, id, title)` without a style is resizable with min/max boxes. Pass
  `wxCAPTION | wxCLOSE_BOX` or `wxDEFAULT_DIALOG_STYLE` (§7).
- `MainFrame` uses `BORDERLESS_FRAME_STYLE` (`MainFrame.cpp`: min/max/close boxes, plus `wxRESIZE_BORDER`
  off Apple, no `wxCAPTION`) and paints its own title bar: MSW strips the `WS_CAPTION` that wx adds,
  macOS calls `set_miniaturizable`, GTK adds `ResizeEdgePanel`s. Custom titlebar rules:
  `references/platforms.md`.
- `TextureProjectorFrame` is the model for a tool frame floating above the main window:
  `wxCAPTION | wxRESIZE_BORDER | wxCLOSE_BOX | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT`, parented to
  `mainframe`, `SetTransparent` before the first show.

**Pitfalls.**
- **Rule:** Remove style bits with `& ~flag`; `!flag` is 0.
  ```cpp
  // Wrong: !wxCAPTION | !wxCLOSE_BOX | wxBORDER_NONE      // == wxBORDER_NONE by accident
  // Right: wxBORDER_NONE                                   // or: wxDEFAULT_FRAME_STYLE & ~wxCAPTION
  ```
- **Rule:** Parent a `wxFRAME_FLOAT_ON_PARENT` frame to a top-level window, never to a panel or NULL.
  **Why:** with NULL wx asserts (silently in Orca) and ignores the flag (`wxTopLevelWindowMSW::MSWGetParent`).
  With a panel, the ports disagree on the owner — MSW passes the panel's own HWND (`GetHwndOf(parent)`),
  GTK resolves `wxGetTopLevelParent(parent)` (`src/gtk/toplevel.cpp:774-782`) — and the frame is deleted
  with the panel (§2). A top-level parent gives every port the same owner and lifetime.
- **Rule:** Don't use `wxSTAY_ON_TOP` or `wxFRAME_FLOAT_ON_PARENT` to keep a tool window "above the app" on
  macOS without accepting that it also floats above other applications.

---

## 6 Dialogs and modality

**Contract** (`interface/wx/dialog.h`).
- Stack allocation is the sanctioned form for a modal dialog: "the modal dialog is one of the very few
  examples of wxWindow-derived objects which may be created on the stack… no need to call Destroy()";
  heap form `ShowModal()` then `dlg->Destroy()` (`interface/wx/dialog.h:61-88`).
- `ShowModal()`: "Program flow does not return until the dialog has been dismissed with EndModal()…
  ShowModal() can't be called twice without intervening EndModal() calls… creates a temporary event loop…
  also results in a call to wxApp::ProcessPendingEvents()" (`interface/wx/dialog.h:597-618`). Timers, `CallAfter`s,
  idle-time deletion, socket and worker events all run inside it.
- `EndModal(retCode)` sets the value `ShowModal()` returns (`interface/wx/dialog.h:340-349`). `Show(false)`: "The preferred
  way of dismissing a modal dialog is to use EndModal()" (`interface/wx/dialog.h:586-595`).
- `ShowWindowModal()` is "only fully implemented in wxOSX… under the other platforms it behaves like
  ShowModal()" (`interface/wx/dialog.h:620-638`). `ShowWindowModalThenDo(functor)`: the dialog must outlive the functor —
  hold it in a `wxWindowPtr` captured by value (`interface/wx/dialog.h:640-678`).
- "you shouldn't show a modal dialog from a mouse click event handler as this would break the mouse capture
  state" — defer with `CallAfter` (`interface/wx/event.h:490-497`). **[source]** GTK's `ShowModal` releases
  any mouse capture first (`GTKReleaseMouseAndNotify`, `src/gtk/dialog.cpp:137`). Capture rules:
  `references/mouse-keyboard-focus.md`.

**[source] facts.**
- `wxDialogBase::EndDialog(rc)` (protected, `src/common/dlgcmn.cpp:361-367`) = `IsModal() ? EndModal(rc) : Hide()`:
  ends either kind from inside the class.
- Modal loops nest and unwind LIFO on every port: MSW `wxEventLoopManual::DoStop` only wakes the loop
  (`src/common/evtloopcmn.cpp:388-401`), GTK re-enters `gtk_main()` until its own `m_shouldExit`
  (`src/gtk/evtloop.cpp:58-90`), macOS keeps a LIFO `s_modalStack` (`src/osx/dialog_osx.cpp:47-60`) and
  stops via `[NSApp abortModal]`, which hits the innermost session (`src/osx/cocoa/evtloop.mm:453-456`).
  Every port's `EndModal` stops its loop through `wxEventLoopBase::Exit()`, whose
  `wxCHECK_RET(IsRunning())` returns silently unless that loop is the active (innermost) one
  (`src/common/evtloopcmn.cpp:91-96`; MSW via `Hide()` → `wxDialogModalData::ExitLoop`,
  `src/msw/dialog.cpp:64-67, 199-207, 261-268`; macOS `src/osx/dialog_osx.cpp:191-194`; GTK's `EndModal` tests
  `IsRunning()` itself, `src/gtk/dialog.cpp:199-202`). Ending a lower dialog first therefore hides it
  but never tells its loop to exit: its `ShowModal()` does not return even after every dialog above it
  has ended. On MSW its `wxWindowDisabler` (owned by the generic `wxModalEventLoop` that MSW's
  `ShowModal` runs, deleted only in its `OnExit()`, `include/wx/evtloop.h:377-396`) stays alive too,
  so the other top-level windows remain disabled. GTK uses no disabler: modality is each dialog's own
  `gtk_window_set_modal` grab (`src/gtk/dialog.cpp:158`). The same silent `Exit()` no-op applies to
  nested `wxEventLoop`s: `references/threads-timers-app.md` §Nested event loops.
- Return codes are ids: `wxID_OK = 5100`, `wxID_CANCEL`, `wxID_APPLY`, `wxID_YES`, `wxID_NO`, …
  (`include/wx/defs.h:1847`). `wxYES 0x2`, `wxOK 0x4`, `wxNO 0x8`, `wxCANCEL 0x10`, `wxAPPLY 0x20`,
  `wxCLOSE 0x40` are style bits (`include/wx/defs.h:1665-1672`) — `EndModal(wxCANCEL)` makes `ShowModal() == wxID_CANCEL` false.
- Whichever `EndModal` runs last before `ShowModal()` returns sets the result: every port's `EndModal`
  calls `SetReturnCode` unconditionally and `ShowModal()` returns `GetReturnCode()`.

**Per-port modal behaviour. [source]**

| | MSW (`src/msw/dialog.cpp:195-268`) | macOS (`src/osx/dialog_osx.cpp:106-202`) | GTK (`src/gtk/dialog.cpp:60-205`) |
|---|---|---|---|
| `Hide()`/`Show(false)` on a modal dialog | exits the loop; `ShowModal()` returns the current code (0 if none set) | **does not exit**: clears the modality, orders the window out; `ShowModal()` stays blocked behind an invisible dialog, and wx's `EndDialog` path then takes the `Hide()` branch | calls the virtual `EndModal(wxID_CANCEL)`, overwriting any code |
| `EndModal()` on a modeless dialog | sets the code and hides (assert compiled out) | sets the code, hides, raises the parent | sets the code, then `wxFAIL` + return: the dialog stays visible |
| `IsModal()` between `EndModal()` and the return of `ShowModal()` | true (`m_modalData`, `include/wx/msw/dialog.h:48`) | false | false |
| `EndModal()` raises the parent | no | yes ("Prevent app frame from taking z-order precedence") | no |
| native owner / transient | fixed at `Create` (§1) | none (not a child window) | `transient_for` set in `ShowModal()` |
| how other windows are blocked | `wxWindowDisabler` in a generic `wxModalEventLoop` (`src/msw/dialog.cpp:70`) | Cocoa modal session (`src/osx/cocoa/evtloop.mm:424-456`) | `gtk_window_set_modal` grab; mouse capture released first |

**Buttons, ESC and the close box. [source]**
- `SetAffirmativeId(id)` (default `wxID_OK`): that button runs `Validate()` + `TransferDataFromWindow()` and
  closes with the id (`interface/wx/dialog.h:480-494`). `SetEscapeId(id)`: default `wxID_ANY` = the `wxID_CANCEL`
  button if present, else the affirmative one; `wxID_NONE` = ignore ESC; native dialogs cannot be
  customized (`interface/wx/dialog.h:496-515`). `CreateStdDialogButtonSizer` makes `wxButton`s and sets the affirmative
  id (`interface/wx/dialog.h:291-304`) — Orca uses `DialogButtons` instead (§8).
- Routing is by **id**, not type: `wxDialogBase::OnButton` (static table, `src/common/dlgcmn.cpp:455-480`) handles any
  `wxEVT_BUTTON` that propagates to the dialog — affirmative id → `AcceptAndClose()` (`Validate()` +
  `TransferDataFromWindow()` then `EndDialog(id)`), `wxID_APPLY` → validate + transfer, no close, escape id
  or `wxID_CANCEL` → `EndDialog(wxID_CANCEL)`, anything else skipped. An Orca `Button` with `wxID_OK` or
  `wxID_CANCEL` and no handler (or a handler that `Skip()`s) closes the dialog by itself; a handler bound
  on the button that does not `Skip()` suppresses it.
- ESC → `wxDialogBase::OnCharHook` → `SendCloseButtonClickEvent()`: tries the escape id (`wxID_CANCEL`), then
  the affirmative id, through `EmulateButtonClickIfPresent`, which does
  `wxDynamicCast(FindWindow(id), wxButton)` and requires the button enabled and shown
  (`src/common/dlgcmn.cpp:387-453`). Orca's `Button` derives from `StaticBox : wxWindow`, so emulation finds nothing:
  in a plain `wxDialog` with Orca buttons ESC does nothing; in a `DPIDialog`, `DPIAware`'s own char hook
  turns ESC into `Close()` first (§7).
- Close box (and `Close()`) → `wxDialogBase::OnCloseWindow` (`src/common/dlgcmn.cpp:525-559`): if shown, try
  `SendCloseButtonClickEvent()`; when that finds no `wxButton`, `EndDialog(wxID_CANCEL)`. The Cancel
  button's own handler never runs on this path.

**Modeless dialogs.** `Show()`; the close box only hides them (above). Long-lived modeless windows (monitor
pages, progress dialogs, plugin dialogs) either destroy themselves in a close handler or are deliberately
hide-on-close and reused (§2 shapes).

**Modality helpers.**
- `wxWindowDisabler(winToSkip, winToSkip2)` disables all *shown and enabled* top-level windows except the
  skipped ones and re-enables them in its destructor (`interface/wx/utils.h:58-111`). MSW: a skipped window
  that appears in the taskbar lets the user close the whole app from the taskbar (`interface/wx/utils.h:93-99`).
  **[source]** The destructor re-enables every top-level window not recorded as skipped — including ones
  created after the disabler (`src/common/utilscmn.cpp:1527-1546`); on macOS the constructor begins a Cocoa
  modal session and **shows** `winToSkip` if it is not on screen (`src/osx/cocoa/evtloop.mm:458-514, 573-584`).
  Keep disablers scoped and strictly nested.
- `wxModalDialogHook::GetOpenCount()` (since 3.3.0, `interface/wx/modalhook.h:118-126`) counts every open
  modal — generic and native message/file/colour/font/print dialogs all use `WX_HOOK_MODAL_DIALOG`. Use it
  for "is any modal open" in new code.
- `wxFrame::SetWindowModality(wxWindowMode)` (new in 3.3.2): call before showing; `AppModal`/`WindowModal`
  (`interface/wx/frame.h:318-329`). **[source]** Applies immediately in the call, ends at the first hide, adds
  `wxFRAME_NO_TASKBAR` and removes `wxMINIMIZE_BOX` (`src/common/framecmn.cpp:159-195, 304-339`); it does not
  block — the caller keeps running.
- Orca's pseudo-modal `ParamsDialog` (filament/printer settings): shown modeless with `Popup()` →
  `Show()`, creates a heap `wxWindowDisabler(this)` in its `wxEVT_SHOW` handler and deletes it on hide; its
  close handler validates (vetoes when validation fails and `CanVeto()`), hides, and never destroys, so the
  hosted tabs stay reusable; `ParamsDialog::Popup()` calls `Reparent(mainframe)` on Windows before showing.

**Pitfalls.**
- **Rule:** Use `wxID_*` codes with `EndModal`.
  ```cpp
  // Wrong: EndModal(wxCANCEL);  EndModal(wxCLOSE);  EndModal(wxOK);
  // Right: EndModal(wxID_CANCEL); EndModal(wxID_CLOSE); EndModal(wxID_OK);
  ```
- **Rule:** Dismiss a modal dialog with `EndModal(rc)`, not `Hide()`/`Show(false)`.
  **Why:** macOS keeps `ShowModal()` blocked behind an invisible dialog; GTK forces `wxID_CANCEL`; MSW
  returns whatever code was last set.
- **Rule:** `EndModal()` only on a modal dialog; a modeless one is hidden/closed/destroyed (or `EndDialog`).
  **Why:** GTK ignores `EndModal` on a modeless dialog — it stays on screen there and only there.
- **Rule:** Never `Destroy()` a modal dialog whose loop is still running; `EndModal(rc)` first, then
  `Destroy()` (heap) — from the caller once `ShowModal()` has returned, or straight after `EndModal()`
  as forced teardown does, relying on a top-level `Destroy()` only queuing the deletion. Never `Destroy()`
  a stack dialog.
  Cite: `WebDialog::destroy_silently` (`EndModal(wxID_CANCEL)` then `Destroy()`).
- **Rule:** Close nested modal dialogs innermost-first.
  **Why:** ending a lower one hides it but leaves its `ShowModal()` stranded (and, on MSW, its disabler
  alive) on every port — see the LIFO bullet above.
- **Rule:** Show a modal from a mouse handler only through `CallAfter`.
  ```cpp
  // Wrong: m_btn->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { MyDialog dlg(this); dlg.ShowModal(); });
  // Right:
  m_btn->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      e.Skip();
      CallAfter([this] { MyDialog dlg(this); dlg.ShowModal(); });
  });
  ```
  The same applies to `EndModal` bound to `wxEVT_LEFT_DOWN`; bind `wxEVT_BUTTON` on an Orca `Button` instead.
- **Rule:** No window work (create, show, raise, destroy, modal dialogs) on the stack of a
  `wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED` handler — WebKitGTK and WKWebView deliver it synchronously. Detail
  and the `WebViewHostDialog` contract: `references/webview-gl-aui-media.md`.
- **Rule:** Scope a `wxWindowDisabler` on the stack or tie it strictly to show/hide; never let heap
  disablers outlive their window or end out of order (on macOS each one is a modal session).

---

## 7 DPIDialog and DPIFrame

`template<class P> class DPIAware : public P, public wxInspector::wxInspectable` (`GUI_Utils.hpp`);
`typedef DPIAware<wxFrame> DPIFrame;` `class DPIDialog : public DPIAware<wxDialog>`. New Orca dialogs and
frames derive one of them. Public API: `scale_factor()`, `prev_scale_factor()`, `em_unit()`,
`normal_font()`, `enable_force_rescale()`; on Windows `force_color_changed()`. Subclasses implement the pure
virtual `on_dpi_changed(const wxRect&)` and may override `on_sys_color_changed()`.

**What the constructor does** (one-step construction only; signature
`(parent, id, title, pos = wxDefaultPosition, size = wxDefaultSize, style = wxDEFAULT_FRAME_STYLE,
name = wxFrameNameStr)`):

| Step | Detail | Owner of the topic |
|---|---|---|
| scale factor, normal font | from `get_dpi_for_window(this)`; `SetFont(m_normal_font)` **except on macOS** (`#ifndef __WXOSX__`, avoids name cutting in `ObjectList`) | `references/dpi-bitmaps-fonts.md` |
| `CenterOnParent()` | runs **before any content exists** — re-centre after fitting | §8 |
| `SetupInspectorAccelerator(this)` | wxInspector toggle (Ctrl+Shift+I) in builds without `WXINSPECTOR_DISABLE`; a later `SetAcceleratorTable()` on the window replaces it | `references/orca-widgets.md` |
| MSW `update_dark_ui(this)` | no-op (its body only reads the dark flag) | `references/colours-dark-mode.md` |
| `update_em_unit()` | non-GTK `max(10, 10·scale)`; GTK `max(10, GetTextExtent("m").x - 1)` | `references/dpi-bitmaps-fonts.md` |
| bind `wxEVT_DPI_CHANGED` (not macOS) | rescales (`Freeze` → font/em → `on_dpi_changed` → `Layout` → `Thaw`) when the scale changed and no monitor drag is in progress, and does **not** `Skip()` — so wx's default top-level handler (scale the window size by the DPI ratio) never runs (`interface/wx/event.h:3572-3583`) | `references/dpi-bitmaps-fonts.md` |
| bind `wxEVT_MOVE_START/END` (MSW-only events) | defer rescale while the window is dragged between monitors | `references/dpi-bitmaps-fonts.md` |
| bind `wxEVT_SYS_COLOUR_CHANGED` | non-Windows: `update_dark_config()` + `on_sys_color_changed()` + `Skip()`; Windows: swallowed | `references/colours-dark-mode.md` |
| dialogs only: bind `wxEVT_CHAR_HOOK` | `WXK_ESCAPE` → `this->Close()`, never skipped; other keys skipped | below |

**ESC in a `DPIDialog`.** ESC = the close box: your `wxEVT_CLOSE_WINDOW` handler if any, else
`wxDialogBase::OnCloseWindow` → `EndDialog(wxID_CANCEL)` (modal: `ShowModal()` returns `wxID_CANCEL`;
modeless: hidden). The dynamic `DPIAware` hook runs before wx's static `wxDialogBase::OnCharHook`, so
`SetEscapeId()` has no effect on ESC. A `MessageDialog` with only Yes/No buttons therefore returns
`wxID_CANCEL` on ESC or the close box — callers must treat anything but `wxID_YES` as "no".

**`dialogStack` and the `EndModal` guard.** `DPIAware::ShowModal()` (same signature as the virtual
`wxDialog::ShowModal`, so it overrides it) pushes `this` on the global `std::deque<wxDialog*> dialogStack`
(`GUI_Utils.cpp`) and pops it after the loop. `DPIDialog::EndModal(retCode)` **refuses** — logs
"DPIAware::EndModal Error…", returns, the dialog stays open and modal — when the stack is non-empty and
`this` is not `dialogStack.front()`. It is a guard for the LIFO rule of §6, not a fix for a wx bug: wx
cannot end a lower loop first on any port (the dialog would be hidden with its `ShowModal()` stranded),
and the guard turns that into a no-op. It only knows
`DPIDialog`s that went through `DPIAware::ShowModal()`; native and plain-wx modals are not on the stack.
Consumers: `GUI_App::ShowDownNetPluginDlg` (searches the stack to avoid a second instance) and the
`wxEVT_QUERY_END_SESSION` handler in `GUI_App::on_init_inner` (sends a vetoable close to `mainframe`, then
`EndModal(wxID_ABORT)` on every stacked dialog — with the guard only the innermost actually ends). For "is
any modal open" in new code prefer `wxModalDialogHook::GetOpenCount()`.

There is no `Destroy()` override: heap `DPIDialog`s follow the stock `ShowModal(); Destroy();` or the
stack form.

**Pitfalls.**
- **Rule:** Pass the style explicitly.
  ```cpp
  // Wrong: MyDialog(wxWindow* p) : DPIDialog(p, wxID_ANY, _L("Title")) {}   // resizable, min/max boxes
  // Right: MyDialog(wxWindow* p) : DPIDialog(p, wxID_ANY, _L("Title"), wxDefaultPosition,
  //                                          wxDefaultSize, wxCAPTION | wxCLOSE_BOX) {}
  ```
- **Rule:** Call `CenterOnParent()` again after `SetSizerAndFit()` (`MsgDialog::finalize` does).
  **Why:** the `DPIAware` constructor centred the empty, default-sized window.
- **Rule:** An override of `ShowModal()` must call `DPIDialog::ShowModal()` (as
  `RichMessageDialog::ShowModal` → `MsgDialog::ShowModal()`, `UnsavedChangesDialog`, `TextureImportDialog`
  do), never `wxDialog::ShowModal()`.
  **Why:** a dialog that bypasses the push is not on `dialogStack`; when it is opened above another
  `DPIDialog`, its own `EndModal` is refused and it cannot close.
- **Rule:** To keep ESC from closing a `DPIDialog`, veto in the close handler or bind your own
  `wxEVT_CHAR_HOOK` that swallows `WXK_ESCAPE`; `SetEscapeId(wxID_NONE)` does nothing here.
  ```cpp
  // Right: bound in the subclass constructor, i.e. after DPIAware's hook, so it runs first
  Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& e) { if (e.GetKeyCode() != WXK_ESCAPE) e.Skip(); });
  ```
- **Rule:** A child that binds the dialog's `wxEVT_DPI_CHANGED` must be bound after `DPIAware` (any child
  created in the subclass constructor is) and must `Skip()`.
  **Why:** dynamic handlers run most-recently-bound first (`interface/wx/event.h:592-593`) and `DPIAware`'s
  handler does not skip; `DialogButtons` binds its parent's event in its constructor and calls `Skip()`,
  so it runs and then `DPIAware` rescales. It unbinds in its destructor — the model for any widget that
  binds an event on another window.

---

## 8 The Orca dialog recipe

Exemplars: `src/slic3r/GUI/CloneDialog.cpp` (minimal; `DialogButtons` with a left-aligned extra button,
an Enter-key `wxEVT_CHAR_HOOK` that synthesizes the OK `wxEVT_BUTTON`; its OK handler's `wxYield()` loop
inside a frozen plater is not a pattern to copy), `PurgeModeDialog.cpp` (custom-painted clickable card
panels; OK/Cancel `Button`s that close purely by id through `wxDialogBase::OnButton`;
`on_dpi_changed` with min size + `Fit()`/`Refresh()`; its `msw_buttons_rescale` call also resizes those
Orca `Button`s, so leave it out when copying), `FilamentPickerDialog.cpp`
(larger; helper `Create*()` methods returning sizers; positions itself next to the sidebar).

```cpp
class MyDialog : public DPIDialog
{
public:
    explicit MyDialog(wxWindow* parent)
        : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,
                    _L("My Dialog"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX)              // or wxDEFAULT_DIALOG_STYLE; never omit
    {
        SetBackgroundColour(*wxWHITE);                    // light design colour, dark-mapped at the end
        SetFont(Label::Body_14);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        // ... children: Orca widgets, sizes via FromDIP(n), labels via _L() ...
        sizer->Add(content_sizer, 1, wxEXPAND | wxALL, FromDIP(10));

        auto* dlg_btns = new DialogButtons(this, {"OK", "Cancel"});   // NOT pre-translated
        dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { /* apply */ EndModal(wxID_OK); });
        dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { /* cancel work */ EndModal(wxID_CANCEL); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { /* same cancel work */ e.Skip(); });  // ESC, close box
        sizer->Add(dlg_btns, 0, wxEXPAND);

        SetSizerAndFit(sizer);
        CenterOnParent();                                 // DPIAware centred the empty window
        wxGetApp().UpdateDlgDarkUI(this);                 // always last, after all children exist
    }

protected:
    void on_dpi_changed(const wxRect&) override;          // rescale bitmaps/widgets, min sizes, GetSizer()->SetSizeHints(this), Refresh()
};

// caller
MyDialog dlg(this);
if (dlg.ShowModal() == wxID_OK) { /* read results */ }
```

**Conventions and why.**
- `DPIDialog` supplies `scale_factor()`, `em_unit()`, DPI-change handling and ESC handling, and requires
  `on_dpi_changed(const wxRect&)`. Typical body: `Rescale()` on Orca widgets, `msw_rescale()` (+
  `SetBitmap()` on the displaying control) on `ScalableBitmap`s, min sizes reset in em units or `FromDIP`,
  then `GetSizer()->SetSizeHints(this)` (or a dialog `SetMinSize` + `Fit()`) and `Refresh()`.
  `msw_buttons_rescale(this, em_unit(), ids)` (min height 2.5 em on the windows with those ids) is for raw
  `wxButton`s only: it also overrides the style height of Orca `Button`s carrying those ids, including the
  `DialogButtons` OK/Cancel. An empty override is acceptable only for trivially simple dialogs
  (`CloneDialog`; on MSW it then keeps its old pixel size). Detail: `references/dpi-bitmaps-fonts.md`
  §DPIAware rescale path, `references/sizers-layout.md` §Layout on DPI change.
- Parent fallback `parent ? parent : wxGetApp().mainframe` — never orphan a dialog (§1).
- Title and labels through `_L()` (`references/strings-i18n-files.md`).
- `SetBackgroundColour(*wxWHITE)` and `SetFont(Label::Body_14)` at the top. Setting a font on a dialog is
  fine on every platform; the macOS "no `SetFont`" concern is `DPIAware`'s per-DPI default font
  (ObjectList name cutting), which `DPIAware` already skips there.
- Light design colours everywhere; `wxGetApp().UpdateDlgDarkUI(this)` as the last line maps them for dark
  mode and themes the native parts on Windows; child panels built later use `UpdateDarkUIWin`. Hand-picked
  colours go through `StateColor::darkModeColorFor(wxColour("#..."))`. Detail: `references/colours-dark-mode.md`.
- `SetSizerAndFit(sizer)` on the dialog (AGENTS.md rule); where `SetSizer` must come first, follow with
  `sizer->SetSizeHints(this)` (`MsgDialog::finalize` does `GetSizer()->SetSizeHints(this)`). Child panels use
  plain `SetSizer`. Detail: `references/sizers-layout.md`.
- `CenterOnParent()` after sizing. Dialogs may set the app icon
  (`SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO))` with
  `resources_dir()/images/OrcaSlicerTitle.ico`, as `PurgeModeDialog` does — no effect on macOS or
  Wayland, §4) and
  clamp their size with `SetMinSize/SetMaxSize(FromDIP(...))`.
- Modality: construct on the caller's stack, `ShowModal()`, read the `wxID_*` result. Heap form:
  `auto* dlg = new MyDialog(this); dlg->ShowModal(); dlg->Destroy();`. `Show()` is for modeless,
  long-lived windows (monitor pages, progress dialogs, plugin dialogs), with a close shape from §2.

**`DialogButtons` and how its buttons close the dialog.** `Slic3r::GUI::DialogButtons(parent,
non_translated_labels, primary_btn_translated_label = "", left_aligned_buttons_count = 0)` is a `wxPanel`
of Orca `Button`s. It calls `_L()` on each label itself and assigns a stock id by matching the lower-cased
label (catalogue: `references/orca-widgets.md`).

| Label | Id | Closes the dialog with no handler bound? |
|---|---|---|
| OK | `wxID_OK` | yes — `AcceptAndClose()` → `EndDialog(wxID_OK)` (affirmative id) |
| Cancel | `wxID_CANCEL` | yes — `EndDialog(wxID_CANCEL)` |
| Apply, Confirm | `wxID_APPLY` (both) | no — `Validate()` + `TransferDataFromWindow()` only |
| Yes, No, Save, Delete, … | `wxID_YES`, `wxID_NO`, `wxID_SAVE`, `wxID_DELETE`, … | no |
| anything unknown | auto id | no — fetch with `GetButtonFromLabel(_L("…"))` or `GetButtonFromIndex(i)` |

Getters (`GetOK`, `GetCANCEL`, …), the full label → id map, and how the primary (Confirm-styled) and alert
buttons are chosen — in numeric id order, so `{"Save", "OK"}` makes Save primary — are in
`references/orca-widgets.md §DialogButtons`.

**Pitfalls.**
- **Rule:** Run cancel cleanup on every close path — the Cancel handler *and* `wxEVT_CLOSE_WINDOW` (or after
  `ShowModal()` returns, where every path ends).
  **Why:** ESC (via `DPIAware` → `Close()`) and the close box go through `wxDialogBase::OnCloseWindow` →
  `EndDialog(wxID_CANCEL)`; wx's button emulation needs a real `wxButton`, so an Orca Cancel button's
  handler never runs on those paths (§6).
- **Rule:** Bind every button whose default handling is not what you want; Yes/No/Apply/Confirm/custom
  buttons never close by themselves, and an OK handler that does real work ends the dialog itself
  (`EndModal(wxID_OK)`) or `Skip()`s to the default `AcceptAndClose()`.
- **Rule:** Pass untranslated labels to `DialogButtons`.
  ```cpp
  // Wrong: new DialogButtons(this, {_L("OK"), _L("Cancel")});   // double translation; no stock ids in non-English UIs
  // Right: new DialogButtons(this, {"OK", "Cancel"});
  ```
- **Rule:** `UpdateDlgDarkUI(this)` runs once, after every child exists; children added later are themed with
  `UpdateDarkUIWin(child)`.
- **Rule:** Don't create dialogs in a constructor of their parent or before the main frame is shown when MSW
  ownership matters (§1); don't keep `CloneDialog`'s `wxYield()` loop pattern — long work goes to a job
  (`references/threads-timers-app.md`).

---

## 9 Message boxes: the MsgDialog family

**Rule.** Never `wxMessageBox`/`wxMessageDialog`/`wxRichMessageDialog` once the GUI exists; use the themed
replacements in `src/slic3r/GUI/MsgDialog.hpp` (`Slic3r::GUI`), rooted in `MsgDialog : DPIDialog` (logo on the
left, content on the right, `Button` row underneath, dark-mode and DPI aware). Why: on MSW the TaskDialog-based
native boxes (`wxMessageBox`, `wxMessageDialog`, `wxRichMessageDialog`, `wxProgressDialog`) ignore dark mode
(`interface/wx/app.h:1436-1446`), and native boxes cannot match Orca's look. `wxMessageBox` remains only for
failures before the GUI exists (e.g. `GUI_App::load_language`). Native message-box style limits:
`references/strings-i18n-files.md`. Return-value trap when reading old code: `wxMessageBox()` returns
`wxYES/wxNO/wxCANCEL/wxOK/wxHELP`, while `ShowModal()` returns `wxID_YES/…` (`interface/wx/msgdlg.h:269-276` vs `309-311`).

| Class | Constructor | Notes |
|---|---|---|
| `MessageDialog` | `(parent, message, caption = "", style = wxOK, forward_str = "", link_text = "", link_callback = nullptr)` | default choice; first four parameters match `wxMessageDialog` (style `wxOK`, `wxCANCEL`, `wxYES_NO`, `wxICON_*`); empty caption → "<app> info" |
| `RichMessageDialog` | `(parent, message, caption = "", style = wxOK)` | adds `ShowCheckBox(text, checked)` / `IsCheckBoxChecked()` (a "Don't show again" check box added in its `ShowModal()`); its `SetYesNoLabels`/`SetYesNoCancelLabels`/`SetOKLabel`/`SetOKCancelLabels`/`SetHelpLabel` only store strings and never relabel a button |
| `WarningDialog` | `(parent, message, caption = "", style = wxOK)` | empty caption → "<app> warning" |
| `ErrorDialog` | `(parent, msg, has_code_excerpts)` | caption "<app> error"; `has_code_excerpts` renders source/caret line pairs monospaced (placeholder-parser errors) |
| `InfoDialog` | `(parent, title, msg, is_marked = false, style = wxOK | wxICON_INFORMATION)` | caption is always "<app> information"; `title` is passed as the base's headline, which is not displayed |
| `DeleteConfirmDialog` | `(parent, title, msg)` | a plain `DPIDialog`, not a `MsgDialog`: Delete → `wxID_OK`, Cancel → `wxID_CANCEL` |

`DownloadDialog` and `FilamentWarningDialog` are single-purpose `MsgDialog` subclasses; read their
constructors before reusing them.

**Behaviour** (`MsgDialog.cpp`):
- Window style is always `wxDEFAULT_DIALOG_STYLE`; the `style` argument only selects buttons and icon. A NULL
  parent becomes `wxGetApp().mainframe`.
- `MsgDialog::apply_style`: `wxOK` → OK, `wxYES` → Yes, `wxNO` → No, `wxCANCEL` → Cancel; Orca's use of
  `wxFORWARD` adds a "Go to <forward_str>" button and turns OK into "Later" (`wxID_CANCEL`). Every button's
  handler is `EndModal(btn_id)`, so compare with `wxID_OK/wxID_YES/wxID_NO/wxID_CANCEL` — and with
  `wxFORWARD` (the style bit `0x2000`, used as the button id) for "Go to". OK/Yes/Go-to are Confirm-styled and
  focused; `wxNO_DEFAULT`, `wxCANCEL_DEFAULT`, `wxHELP` and `wxSTAY_ON_TOP` are ignored.
- Icon from the style: `wxAPPLY` → "completed", `wxICON_WARNING` → "exclamation", `wxICON_INFORMATION` →
  "info", `wxICON_QUESTION` → "question", otherwise the app logo; `wxICON_ERROR` greys it.
- ESC and the close box return `wxID_CANCEL` whatever the buttons (§7).
- Content (`add_msg_content`): plain text → a wrapped `Label` in a scrolled window, so `&` is a mnemonic —
  escape user text (`references/strings-i18n-files.md`); with `link_text`/`link_callback`, `is_marked`,
  code excerpts, or a message containing `<tr>` → a `wxHtmlWindow` with the text `xml_escape`d (`is_marked`
  keeps `<`/`>` so markup works) and `\n` → `<br>`.
- Base helpers: `SetButtonLabel(wxID_*, label, set_focus = false)` relabels a created button;
  `AddButton(id, label, set_focus = false)` appends a choice button that also ends with `EndModal(id)`;
  `show_dsa_button(title = {})` adds a "Don't show again" `CheckBox` that posts `EVT_CHECKBOX_CHANGE`
  (int = checked) to the dialog; `get_checkbox_state()`.
- `finalize()` (called by each subclass constructor): `SetSizeHints`, `Layout`, `Fit`, `CenterOnParent`,
  `UpdateDlgDarkUI` — the recipe of §8 in one call; a custom `MsgDialog` subclass ends its constructor with it.

**Free helpers** (`GUI.hpp`): `show_error(parent, msg, has_code_excerpts = false)` is **asynchronous** — it
shows an `ErrorDialog` from `wxGetApp().CallAfter`, capturing the raw `parent`; `show_info(parent, msg,
title)` and `warning_catcher(parent, msg)` are synchronous `MessageDialog`s. The `const char*`/`std::string`
overloads decode UTF-8.

**Pitfalls.**
- **Rule:** Relabel buttons with `SetButtonLabel`, not the `RichMessageDialog::Set*Labels` methods.
  ```cpp
  // Wrong: dlg.SetYesNoLabels(_L("Discard"), _L("Keep"));        // stored, never shown
  // Right: dlg.SetButtonLabel(wxID_YES, _L("Discard")); dlg.SetButtonLabel(wxID_NO, _L("Keep"));
  ```
- **Rule:** Test for the positive answer; everything else (No, Cancel, ESC, close box) is "no".
  ```cpp
  // Wrong: if (dlg.ShowModal() != wxID_NO) discard();
  // Right: if (dlg.ShowModal() == wxID_YES) discard();
  ```
- **Rule:** Never compare a `MsgDialog` result with `wxYES`/`wxOK` (style bits), except `wxFORWARD` for "Go to".
- **Rule:** Give `show_error` a parent that outlives the deferred call (the main frame, or `nullptr`, which
  `MsgDialog` maps to it), not a dialog that may close first; don't rely on the error being visible when
  `show_error` returns.

---

## 10 Overlay frames: BaseTransparentDPIFrame

`BaseTransparentDPIFrame : DPIFrame` (`BaseTransparentDPIFrame.hpp/.cpp`) is the base for small
semi-transparent prompt frames (text, OK/Cancel `Button`s, optional timed fade-out via
`DisappearanceMode::TimedDisappearance`). Its lifetime design:
- Parent is always `wxGetApp().mainframe`; borderless.
- `wxEVT_CLOSE_WINDOW` → `on_hide()` on every close, forced or not (it neither vetoes nor destroys): stop
  the refresh timer, `Hide()`, then restore the main frame with
  `if (!mainframe->IsShown()) mainframe->Show(); mainframe->Raise();` (§4, dd8cb89f6d). The frame is reused;
  it is destroyed only through `on_close()` → `Destroy()`, or with the main frame.
- `Show(bool)` override starts/stops the refresh timer; `on_full_screen` adds `wxSTAY_ON_TOP` on macOS so the
  overlay stays above a full-screen main window.

When writing a similar overlay: keep the restore rule; give a close handler that respects `CanVeto()` if the
frame can outlive its owner; own the timer as a member (`wxTimer m_timer{this}`) rather than a heap
`new wxTimer()` that is never deleted; write the style as `wxBORDER_NONE` (not `!wxCAPTION | …`, §5).
