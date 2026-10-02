# Popups, transient windows and menus

How wx 3.3.2 popups and menus behave on each port, and the Orca wrappers built on them: `PopupWindow`,
`DropDown`/`ComboBox`, `SidePopup`, the search popups, frameless dialogs used as popups, `MenuFactory`,
`append_menu_item`, `Plater::PopupMenu` and the macOS menubar versus `BBLTopbar`. Read it before you add
or change anything that opens over other UI and must close by itself, or any context menu or menubar item.

wx cites are relative to the wx tree root (`deps/build/<arch>/dep_wxWidgets-prefix/src/dep_wxWidgets`).
**[source]** marks behaviour derived from the implementation that the wx docs do not state or contradict.
Orca builds wx with `wxBUILD_DEBUG_LEVEL=0`, so every "asserts" below means "fails silently in Orca".
"GTK" means wxGTK3 (X11 and Wayland), the default Linux build; GTK2 is only an opt-out (`-DDEP_WX_GTK3=OFF`).
Mouse capture in general (capture stack, guards, `wxEVT_MOUSE_CAPTURE_LOST`) is owned by
`references/mouse-keyboard-focus.md`. This file covers only what popups do with capture.

Contents: [Rules](#rules) · [1. Choosing the window kind](#1-choosing-the-window-kind) ·
[2. wxPopupTransientWindow contract](#2-wxpopuptransientwindow-contract) · [3. Position()](#3-position) ·
[4. wxPU_CONTAINS_CONTROLS and plain wxPopupWindow](#4-wxpu_contains_controls-and-plain-wxpopupwindow) ·
[5. Dismissal mechanics per platform](#5-dismissal-mechanics-per-platform) ·
[6. Orca PopupWindow](#6-orca-popupwindow) · [7. DropDown and the ComboBox popup](#7-dropdown-and-the-combobox-popup) ·
[8. Other Orca popups](#8-other-orca-popups) · [9. GTK and Wayland parenting and grabs](#9-gtk-and-wayland-parenting-and-grabs) ·
[10. Frameless dialog instead of a popup](#10-frameless-dialog-instead-of-a-popup) ·
[11. wxComboCtrl](#11-wxcomboctrl--wxcombopopup) · [12. Tip windows, info bars, notifications](#12-tip-windows-info-bars-notifications) ·
[13. wxMenu and wxMenuItem](#13-wxmenu-and-wxmenuitem) · [14. PopupMenu and menu event routing](#14-popupmenu-and-menu-event-routing) ·
[15. wxMenuBar on macOS](#15-wxmenubar-on-macos) · [16. Orca menus](#16-orca-menus)

## Rules

1. Transient popups derive from Orca's `PopupWindow`, not raw `wxPopupTransientWindow`. A popup that hosts
   interactive children passes `wxBORDER_NONE | wxPU_CONTAINS_CONTROLS`. (§4, §6)
2. Size the popup (`SetSizerAndFit`/`SetSize`) **before** `Position()`, and pass the *anchor's* height as the
   second argument: `Position(anchor->ClientToScreen({0,0}), {0, anchor->GetSize().y})`. (§3)
3. On macOS, anchor a hover-driven transient popup flush against its opener, with no pixel gap. (§3)
4. Never call `Popup()` on a popup that is already shown: guard with `if (!popup->IsShown())`. (§2)
5. Destroy a popup with one `Destroy()`, typically from `wxEVT_SHOW` when it hides. Never `delete` it or
   call `Destroy()` twice. (§2)
6. Command events from controls inside a popup stop at the popup (`wxWS_EX_BLOCK_EVENTS`) on MSW and macOS,
   but bubble on to the popup's parent on wxGTK. Bind on the popup or the control, or re-emit on the opener. (§2)
7. Veto automatic dismissal by overriding `Dismiss()`. `OnDismiss()` still runs after a veto, so it must
   re-check `IsShown()` and chain state. (§2)
8. Cleanup that must run on every close goes in a `Dismiss()` override: a direct `Dismiss()` never calls
   `OnDismiss()`. (§2)
9. `ProcessLeftDown` is never called on MSW. Keep-open logic needs a per-platform path. (§2, §5)
10. A control that opens a popup on click must debounce reopening right after a dismissal
    (`DropDown::HasDismissLongTime`), because the dismissing click reaches the opener on MSW and macOS. (§5)
11. On MSW, a popup that must close when the frame deactivates or minimizes calls `BindUnfocusEvent()`. On
    macOS, nothing dismisses a popup on app deactivation. (§5, §6)
12. On macOS, re-verify the cursor with `GetClientRect().Contains(ScreenToClient(wxGetMousePosition()))`
    before acting on ENTER/LEAVE or on a hover timer inside a transient popup. (§6)
13. Mouse handlers of a popup return early when `!IsShown()` (a stray `LEFT_DOWN` arrives on macOS after
    `OnDismiss`). (§6)
14. Parent a `PopupWindow` to a control, not to a top-level window. Never pass a null parent outside
    macOS (`PopupWindow::Create` dereferences it on GTK). (§6)
15. On GTK, set a chained popup's `transient_for` to the currently mapped parent popup right before showing
    it, and keep chain peers from dismissing each other on toplevel deactivation. (§7, §9)
16. On GTK, never open a popup from a window that is not mapped yet. Defer with `CallAfter` plus
    `IsShownOnScreen()`. (§9)
17. Don't shrink a dropdown below two rows to fit the screen. (§7)
18. Content that needs typing focus or hosts a `wxWebView` uses a frameless `wxDialog` that hides on
    deactivation, not a transient popup. (§10)
19. Display-only overlays (HUDs, toasts) use a plain `wxPopupWindow`: `Show()` never gives it focus and it
    never auto-dismisses. Never `Raise()` any `wxPopupWindow`: `Raise()` is for top-level windows only, and on
    macOS it makes the popup the key window. (§4, §5, §10)
20. On MSW, don't `SetFocus()` another window on hover while `wxCurrentPopupWindow` is non-null. (§8)
21. Menu items use `wxID_ANY` and read `item->GetId()`. `wxNewId()` is deprecated. (§13)
22. Set a menu item's bitmap before `Append`. Don't expect icons on check or radio items. Never call
    `SetTextColour`/`SetBackgroundColour`/`SetFont` on menu items. (§13)
23. Bind `wxEVT_MENU` on the menu or on the invoking window or one of its ancestors. Bind
    `wxEVT_MENU_OPEN/CLOSE` on the menu or the invoking window, never on a sibling or child of it. (§14)
24. Never tear down menu state in `wxEVT_MENU_CLOSE`: it arrives before `wxEVT_MENU`. (§14)
25. A shared `UPDATE_UI` handler calls `evt.Check()` only when `evt.IsCheckable()`. (§14)
26. Plater, canvas and object-list context menus go through `Plater::PopupMenu`, with coordinates in
    MainFrame client space. Other menus use the hosting window's `PopupMenu`. (§16)
27. Build `MenuFactory` menus once. Pass `parent` to `append_menu_item` only for items that persist. (§16)
28. macOS: bind `wxID_EXIT` on `OSXGetAppleMenu()` to `Close()`, only insert into the Apple menu, and always
    `Skip()` a frame's `wxEVT_ACTIVATE`. (§15)

## 1. Choosing the window kind

| Need | Use | Why |
|---|---|---|
| Combo-like list or small interactive panel that closes on outside click | `PopupWindow(parent, wxBORDER_NONE \| wxPU_CONTAINS_CONTROLS)` | auto-dismissal, plus Orca's per-platform fixes (§6) |
| A choice list styled like Orca | `::ComboBox` (owns a `DropDown`), or `DropDown` standalone | §7 |
| Display-only overlay: HUD, toast, status chip | plain `wxPopupWindow` | never takes focus, never auto-dismisses (§4) |
| Typing, a WebView, or rich focus | frameless `wxDialog` that hides on deactivate | normal activation on every port (§10) |
| Command list | `wxMenu` + `PopupMenu` (`Plater::PopupMenu` on the plater) | native, synchronous, keyboard-navigable (§14) |
| Tooltip-like text | `wxTipWindow::New` | self-destroys, weak `Ref` (§12) |
| Message strip inside a panel | `wxInfoBar` | lives in the sizer (§12) |

## 2. wxPopupTransientWindow contract

**Contract** (`interface/wx/popupwin.h:71-131`).
- The class is "A wxPopupWindow which disappears automatically when the user clicks mouse outside it or if it
  loses focus in any other way" (74-75). It is meant for "custom combobox-like controls".
- `Popup(focus)`: "If @a focus is non-null, it will be kept focused while this window is shown if supported by
  the current platform, otherwise the popup itself will receive focus. In any case, the popup will disappear
  automatically if it loses focus because of a user action" (100-109).
- `Dismiss()` hides the popup programmatically (111-114).
- `ProcessLeftDown(event)`: "Return @true from here to prevent its normal processing by the popup (which
  consists in dismissing it if the mouse is clicked outside it)" (116-122).
- `OnDismiss()` (protected) "is called when the popup is disappeared because of anything else but direct call
  to Dismiss()" (125-129). It is the hook for cleanup after automatic dismissal.

**What the docs don't tell you** [source].
- **All automatic dismissal goes through `DismissAndNotify()` = `Dismiss(); OnDismiss();`**, which is protected
  and undocumented (`include/wx/popupwin.h:114-119`). It has two consequences:
  - A `Dismiss()` override intercepts automatic dismissal as well as programmatic dismissal. That is how Orca
    vetoes it (`DropDown::Dismiss`, `SearchDialog::Dismiss`).
  - `OnDismiss()` runs even when your `Dismiss()` override refused to hide. `OnDismiss` must re-check
    `IsShown()` or whatever state the veto tested (`DropDown::OnDismiss` re-tests the chain state: it returns
    while its sub-dropdown is shown).
- **A direct `Dismiss()` never calls `OnDismiss()`.** State that must reset on every close belongs in a
  `Dismiss()` override that calls the base (`FilamentGroupPopup::Dismiss` stops its timer and clears
  `m_active` there).
- **`ProcessLeftDown` is never called on MSW.** Only the non-MSW `wxPopupWindowHandler::OnLeftDown` calls it
  (`src/common/popupcmn.cpp:536-543`, inside `#ifndef __WXMSW__`). `src/msw/popupwin.cpp` has no click
  handler. An override that returns true does not keep the popup open on Windows (§5).
- **`Popup()` must not run on a shown popup (non-MSW).** `Popup()` pushes `m_handlerPopup` onto `m_child` and
  `m_handlerFocus` onto the focus window every time (`popupcmn.cpp:308-332`). The "still in use" asserts are
  compiled out in Orca. A second push links the handler chain into a cycle, and the next event that no handler
  in the cycle consumes loops forever in `wxEvtHandler::DoTryChain` (`src/common/event.cpp:1591-1640`), so the
  app hangs. On MSW, `Popup()` is just `Show()` plus an optional `SetFocus` (`src/msw/popupwin.cpp:184-193`), so
  a repeat is harmless there.
- **`Destroy()` is deferred.** The popup goes into `wxPendingDelete`, and a second `Destroy()` fails a silent
  `wxCHECK` "Shouldn't destroy the popup twice." (`popupcmn.cpp:209-221`). The comment explains why: "The popup
  window can be deleted at any moment, even while some events are still being processed for it".
- **Command events from popup children stop at the popup on MSW and macOS.** `wxPopupWindowBase::Create` sets
  `wxWS_EX_BLOCK_EVENTS` (`popupcmn.cpp:129-138`), and `wxWindowBase::TryAfter` honours it
  (`src/common/wincmn.cpp:3499-3522`). The MSW and macOS `wxPopupWindow::Create` call the base; wxGTK's never does
  (`src/gtk/popupwin.cpp` `wxPopupWindow::Create`), so a GTK popup does not block. An unconsumed `wxEVT_BUTTON`
  from a button inside the popup never reaches the window that opened it on MSW/macOS, but reaches the popup's
  parent and its ancestors on GTK (`references/events.md` §5).
- **Keyboard (non-MSW).** Any `wxEVT_CHAR` that the popup does not process dismisses it
  (`wxPopupFocusHandler::OnChar`, `popupcmn.cpp:653-671`). On MSW no key dismisses a popup, so Esc must be
  handled explicitly.
- **The focus window passed to `Popup(win)` gets a pushed handler** (`popupcmn.cpp:317-332`). If that window is
  destroyed while the popup is shown, its pushed handler is left dangling (the `~wxWindowBase` assert "any pushed
  event handlers must have been removed", `wincmn.cpp:469-472`, is compiled out), and the next `Dismiss()` calls
  `RemoveEventHandler` on freed memory. On GTK the focus target is forced to the popup itself (320-324). On MSW
  `focus` is honoured only with `wxPU_CONTAINS_CONTROLS` and only for a descendant of the popup
  (`msw/popupwin.cpp:188-192`); focusing any other window would deactivate the popup and dismiss it.
- **Single-child popups.** When the popup has exactly one child, wx gives the outside-click handler (and, on
  macOS, the mouse capture) to that child instead of the popup ("we suppose that it must cover the entire popup
  window", `popupcmn.cpp:286-304`, 421-426).

**Usage.**
```cpp
auto* pop = new PopupWindow(anchor, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS);
/* build children + sizer */
pop->SetSizerAndFit(sizer);                                          // size BEFORE Position
pop->Position(anchor->ClientToScreen({0, 0}), {0, anchor->GetSize().y});
pop->Bind(wxEVT_SHOW, [pop](wxShowEvent& e) { if (!e.IsShown()) pop->Destroy(); e.Skip(); });
pop->Popup();
```
A popup created per open is a child of its parent until it is destroyed. Destroy it on hide (above;
`StatusPanel::on_switch_speed`), or keep one instance and reuse it.

**Pitfalls.**
- **Rule:** Guard every `Popup()` call with `IsShown()`.
  **Why:** On non-MSW ports the second push creates a cycle in the handler chain and the app hangs on the next
  unconsumed event. MSW is unaffected, which hides the bug from Windows testing.
  ```cpp
  // Wrong: re-pops on every motion or click
  void on_motion(wxMouseEvent&) { m_popup->Popup(); }
  // Right
  void on_motion(wxMouseEvent&) { if (!m_popup->IsShown()) m_popup->Popup(); }
  ```
  Cite: `DropDown::mouseMove` (`if (!drop.IsShown()) drop.Popup(&drop)`), `SearchObjectDialog::Popup`
  (`if (m_is_dismissing || this->IsShown()) return;`). `Button::OnParentMotion` may re-pop its `wxTipWindow` on
  every move only because `Button::EnableTooltipEvenDisabled` is MSW-only.
- **Rule:** Veto in `Dismiss()`. In `OnDismiss()`, act only if the popup really closed.
  **Why:** `DismissAndNotify` calls `OnDismiss` unconditionally after `Dismiss`.
  ```cpp
  // Wrong: cleanup runs although Dismiss() refused
  void Dismiss() override { if (m_sub && m_sub->IsShown()) return; PopupWindow::Dismiss(); }
  void OnDismiss() override { m_state = Closed; notify_closed(); }
  // Right
  void OnDismiss() override { if (IsShown() || (m_sub && m_sub->IsShown())) return; m_state = Closed; notify_closed(); }
  ```
  Cite: `DropDown::Dismiss`, `DropDown::OnDismiss`.
- **Rule:** Bind a popup child's command events on the child or the popup, never on the opener.
  ```cpp
  // Wrong: fires on GTK only; on MSW/macOS the event stops at the popup
  opener_panel->Bind(wxEVT_BUTTON, &Panel::on_choice, this, ID_CHOICE);
  // Right
  btn_in_popup->Bind(wxEVT_BUTTON, &Panel::on_choice, this);
  // or re-emit with the opener's id, as DropDown does for its sub-dropdown:
  sub->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) { e.SetEventObject(this); e.SetId(GetId()); GetEventHandler()->ProcessEvent(e); });
  ```
  Cite: `DropDown::messureSize` (sub-dropdown relay), `DropDown::sendDropDownEvent`, `ComboBox` ctor (relay from
  `drop`).
- **Rule:** Destroy a popup with `Destroy()`, once. Dismiss the popup before you destroy an external window
  passed to `Popup(win)`.
  ```cpp
  // Wrong
  delete m_popup;                     // may be inside its own event processing
  m_popup->Destroy(); m_popup->Destroy();
  // Right
  m_popup->Dismiss(); m_popup->Destroy(); m_popup = nullptr;
  ```
  Cite: `popupcmn.cpp:209-221`; `Button::OnParentMotion` (`Dismiss()` + `Destroy()` on its tip).

## 3. Position()

**Contract** (`interface/wx/popupwin.h:54-68`). It moves the popup "such that it is entirely visible". "The
popup is positioned at ptOrigin + size if it opens below and to the right (default), at ptOrigin - sizePopup if
it opens above and to the left". `ptOrigin` "Must be given in screen coordinates!". The doc names the second
parameter `sizePopup`, "The size of the popup window".

**What it really does** [source] (`src/common/popupcmn.cpp:140-203`).
- The second argument is the **anchor offset**, not the popup size. The popup's own size comes from
  `GetSize()` (162), so the popup must be sized first.
- Default placement: `y = ptOrigin.y + size.y`, `x = ptOrigin.x + size.x` (mirrored in RTL layouts, 181-188).
- Vertical flip: if the popup overflows the display's bottom and `ptOrigin.y > popupHeight`, then
  `y = ptOrigin.y - popupHeight`, i.e. it ends at the anchor's top edge (166-176). The test compares an absolute
  coordinate, not a display-relative one.
- Horizontal flip: if it overflows the right edge and `ptOrigin.x > popupWidth`, then
  `x = ptOrigin.x - popupWidth` (191-200).
- If it fits neither way it stays in the default position, partly off-screen: "entirely visible" is not
  guaranteed. It never resizes the popup, so clamping or shrinking is the caller's job.
- The display is `wxDisplay::GetFromPoint(ptOrigin)` and its full `GetGeometry()`, not the client area
  (147-159).
- wxUniversal's combo popup (`wxPopupComboWindow::PositionNearCombo`) passes `wxSize(0, comboHeight)`: "the
  width is 0 to put it exactly below us" (`popupcmn.cpp:513-515`).

**Canonical call.**
```cpp
popup->SetSizerAndFit(sizer);                       // or SetSize(); size is read by Position()
const wxPoint tl = anchor->ClientToScreen(wxPoint(0, 0));
popup->Position(tl, wxSize(0, anchor->GetSize().y)); // below, left-aligned; above if no room
```

**Platforms.** On Wayland the global coordinates are meaningless (§9), so the flip decision is made against
garbage, and the compositor places `xdg_popup`s relative to their parent surface anyway.

**Pitfalls.**
- **Rule:** Pass `{0, anchorHeight}`, not the anchor's full size.
  **Why:** The `x` offset is added too, so the popup starts at the anchor's right edge.
  ```cpp
  // Wrong
  pop->Position(btn->ClientToScreen({0, 0}), btn->GetSize());
  // Right
  pop->Position(btn->ClientToScreen({0, 0}), {0, btn->GetSize().y});
  ```
  `Button::OnParentMotion` passes the full size on purpose, to put its tip at the button's bottom-right corner.
- **Rule:** Fit the popup before `Position()`.
  **Why:** The above/below and left/right decisions use the current, often zero, size.
- **Rule:** On macOS, anchor a hover-driven transient popup flush against its opener, with a slight overlap and
  no gap.
  **Why:** Since the wx 3.3 upgrade, the macOS Print/Export `SidePopup` was dismissed as the cursor crossed the
  gap below the opener, which made "Print → Export" unselectable (#12936). The mechanism was not established:
  stock wx dismisses only on an outside click, kill-focus, an unhandled char or capture-lost (§5), and the idle
  capture juggling that macOS popups use dates from wx 3.1.7 ("Allow user input in wxPopupTransientWindow",
  `docs/changes_32.txt:334`), not 3.3. The fix is geometric: leave no dead zone for the cursor to cross.
  ```cpp
  // Wrong: 6 px above the button, anchor height + 12: a gap below the button
  wxPoint pos = focus->ClientToScreen(wxPoint(0, -6));
  Position(pos, {0, focus->GetSize().y + 12});
  // Right (macOS): anchor at the button edge with a 2 px overlap
  pos.y = focus->ClientToScreen(wxPoint(0, 0)).y;
  Position(pos, {0, focus->GetSize().y - 2});
  ```
  Cite: 9a053f15eb (#12936), `Widgets/SideMenuPopup.cpp` `SidePopup::Popup` (`#ifdef __APPLE__`).

## 4. wxPU_CONTAINS_CONTROLS and plain wxPopupWindow

**Contract.**
- `wxPopupWindow` is "A special kind of top level window used for popup menus, combobox popups and such"
  (`interface/wx/popupwin.h:13-14`). It has no automatic dismissal of any kind: all of that lives in
  `wxPopupTransientWindow` (71-131).
- Its `Create` style "may only contain border flags" (`include/wx/popupwin.h:41-42`).
- `wxPU_CONTAINS_CONTROLS` (`interface/wx/popupwin.h:17-27`): "By default in wxMSW, a popup window will not take
  focus from its parent window. However many standard controls, including common ones such as wxTextCtrl, need
  focus to function correctly and will not work when placed on a default popup. This flag can be used to make
  the popup take focus and let all controls work but at the price of not allowing the parent window to keep
  focus while the popup is shown … This style is currently only implemented in MSW and simply does nothing under
  the other platforms" (since 3.1.3, `docs/changes_32.txt:720`). wx's own `wxComboCtrl` popup passes it
  (`src/common/combocmn.cpp:332`).

**MSW** [source] (`src/msw/popupwin.cpp`). The comment calls the two variants "your poison" (72-84):

| | Without `wxPU_CONTAINS_CONTROLS` | With `wxPU_CONTAINS_CONTROLS` |
|---|---|---|
| Native window | `WS_CHILD` of the desktop window, so it can extend beyond the parent (`MSWGetParent`, 100-112) | `WS_POPUP` owned by the parent (85-89) |
| Extended style | `WS_EX_TOPMOST \| WS_EX_TOOLWINDOW` (91-95) | same |
| Show | `SetWindowPos(HWND_TOP)` + `SetForegroundWindow` (160-176) | `wxPopupWindowBase::Show`: the popup is activated |
| Focus | `SetFocus()` is a no-op (114-123) | the popup takes focus |
| Dismissal | the global hook in `wxWindowMSW::MSWHandleMessage` (§5) | `WM_ACTIVATE`/`WA_INACTIVE` → `CallAfter(&DismissOnDeactivate)`, deferred (231-264) |
| Frame minimized | not hidden by Windows: it is not owned by the frame | hidden with the owner |

Other ports read only the border bits; the flag changes nothing there in stock wx.

**OrcaSlicer.** The convention is `wxBORDER_NONE | wxPU_CONTAINS_CONTROLS` for every interactive popup
(`SidePopup`, `SearchDialog`, `SearchObjectDialog`, `FilamentGroupPopup`, `CameraPopup`, `SelectMachinePopup`,
`AMSCountPopupWindow`; `DropDown::Create` passes `wxPU_CONTAINS_CONTROLS` alone). Orca also reuses the flag as the switch for its macOS mouse
forwarding (§6), so on macOS it is not a no-op for a `PopupWindow`. Use raw `wxPopupTransientWindow` only for
display-only content (`MarkdownTip`). Plain `wxPopupWindow` is right for non-focusable overlays
(`DesignCanvas` HUD and status chip; §10).

**Pitfalls.**
- **Rule:** Pass `wxPU_CONTAINS_CONTROLS` when the popup hosts focusable or clickable children.
  **Why:** On MSW without it the popup cannot take focus, so text fields such as `SpinInput`/`TextInput` don't
  work. Clicking a focusable child also moves focus off a window that is not a descendant of the popup. Its
  `WM_KILLFOCUS` runs the dismissal hook, so the popup closes on the first click. On macOS, without the flag,
  `PopupWindow` installs no mouse forwarding.
  ```cpp
  // Wrong: controls inside are dead / popup dismisses on first click
  AMSCountPopupWindow(ExtruderGroup* extruder, int index) : PopupWindow(extruder, wxBORDER_NONE) {...}
  // Right
  AMSCountPopupWindow(ExtruderGroup* extruder, int index) : PopupWindow(extruder, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS) {...}
  ```
  Cite: 8ace17eaef (`Plater.cpp` `AMSCountPopupWindow`, which hosts two `SpinInput`s).

## 5. Dismissal mechanics per platform

Stock triggers [source]:

| Trigger | MSW, default style | MSW, `wxPU_CONTAINS_CONTROLS` | macOS | GTK3 (X11 and Wayland) |
|---|---|---|---|---|
| Left click outside, in this app | yes, via hook | yes, via deactivation (deferred) | yes (`OnLeftDown`) | yes (`gtk_popup_button_press`) |
| Right or middle click outside | yes, via hook | yes, if it activates another window | no (only `EVT_LEFT_DOWN` is watched) | yes (any button press) |
| Click in another application | only indirectly, when the wx window holding focus gets `WM_KILLFOCUS` | yes, deactivation | no; panel hides with the app | yes, through the pointer grab |
| Focus moves to another window of the app | yes (`WM_SETFOCUS`/`WM_KILLFOCUS` hook) | yes, deactivation | if the focus window gets `wxEVT_KILL_FOCUS` (`wxPopupFocusHandler::OnKillFocus`) | via the synthesized kill-focus |
| App or toplevel deactivated (Alt+Tab, Cmd+Tab) | only indirectly, as above | yes | no `OnDismiss`: Cocoa hides the panel, `IsShown()` stays true | no (Orca adds it, §6) |
| Unhandled key | no | no | yes (`OnChar`) | yes (`OnChar`) |
| Frame minimized | not hidden with the frame (not owned); dismissed only by a focus message, as above | hidden with the owner | — | — |
| Does the dismissing click reach the window under it? | yes | yes, and it runs *before* the deferred dismissal | yes: re-posted with `wxPostEvent` | no: consumed |

A click *inside* the popup is never treated as an outside click: the MSW hook skips descendants of
`wxCurrentPopupWindow` (`src/msw/window.cpp:3018`), and `wxPopupWindowHandler::OnLeftDown` dismisses only on
`wxHT_WINDOW_OUTSIDE` (`popupcmn.cpp:555-606`). On macOS, while the popup holds the capture, an inside click is
delivered to the capture holder `m_child`, not to the child under the cursor; that is what Orca's forwarding
compensates for (§6).

**MSW** [source].
- The hook in `wxWindowMSW::MSWHandleMessage` (`src/msw/window.cpp:3014-3034`): any wx window that is *not* a
  descendant of `wxCurrentPopupWindow` and receives `WM_[NC]{L,R,M}BUTTONDOWN`, `WM_SETFOCUS` or `WM_KILLFOCUS`
  calls `wxCurrentPopupWindow->MSWDismissUnfocusedPopup()`. That function dismisses only popups *without*
  `wxPU_CONTAINS_CONTROLS` (`src/msw/popupwin.cpp:218-229`). It is virtual (`include/wx/msw/popupwin.h:46`),
  so a subclass can replace it.
- The hook runs inside the target's own message processing, so the click still reaches its target.
- `wxCurrentPopupWindow` is a single global: the last popup shown (`src/msw/popupwin.cpp:36`, 125-152).
  Showing a second popup overwrites it. The comment expects the old popup to close "as it will result in
  activation loss" (140-144), which holds only when the old popup is a `wxPU_CONTAINS_CONTROLS` popup. That is
  why a chained popup must veto its own deactivation dismissal while its sub-popup is shown (`DropDown::Dismiss`).
- `IsDescendant` stops at the top-level boundary (`src/common/wincmn.cpp:1273-1289`), and every popup is
  top-level, so a sub-popup does not count as part of its parent popup.
- While its popup is shown, the owner answers `WM_NCACTIVATE` as active and keeps an active title bar
  (`src/msw/window.cpp:3789-3800`).
- With `wxPU_CONTAINS_CONTROLS`, dismissal waits for `CallAfter(&DismissOnDeactivate)` (`popupwin.cpp:231-258`).
  The click that deactivated the popup is fully processed first, including the opener's own click handler.
- Since 3.1.0 minimized windows get no activation events on MSW (`interface/wx/event.h:3238-3241`), so
  minimizing must be watched through `wxEVT_ICONIZE`.
- There is no Esc handling and no `ProcessLeftDown`.

**macOS** [source].
- The popup is an `NSPanel` at `NSPopUpMenuWindowLevel` (`src/osx/cocoa/nonownedwnd.mm:797`, 843). It is shown
  with `ShowWithoutActivating` → `setHidesOnDeactivate:YES` + `orderFront` (`src/osx/carbon/popupwin.cpp:56-75`,
  `nonownedwnd.mm:938-945`). When the app deactivates, Cocoa hides the panel and shows it again on reactivation.
  wx never calls `OnDismiss`, and `IsShown()` stays true. This applies to plain `wxPopupWindow` overlays too.
- `Raise()` activates the popup. It is `makeKeyAndOrderFront` (`src/osx/nonownedwnd_osx.cpp:289-295`,
  `nonownedwnd.mm:897-899`), and `wxNSPanel` answers `canBecomeKeyWindow` with YES (`nonownedwnd.mm:271`), so
  the popup becomes the key window. Keys go to it, and the frame loses key status: `windowDidResignKey` →
  `HandleActivated(0, false)` → `wxEVT_ACTIVATE(false)` on the frame (`nonownedwnd.mm:567-576`,
  `nonownedwnd_osx.cpp:303-310`). Hiding the key popup gives key back to the frame, which then gets
  `wxEVT_ACTIVATE(true)` (observed; AppKit behaviour, not in the wx tree). A popup at `NSPopUpMenuWindowLevel`
  is already above its frame, so `Raise()` buys nothing.
- Capture: `Show(true)` makes `m_child` capture the mouse ("Assume that the mouse is outside the popup to begin
  with", `popupcmn.cpp:421-426`). `OnIdle` releases the capture while the cursor is inside and re-captures it
  outside, but only when the mouse position has changed since the last idle pass. `s_posLast` is a
  function-static shared by all popups (`popupcmn.cpp:438-471`). The machinery dates from 3.1.7
  (`docs/changes_32.txt:334`).
- While captured, every mouse `NSEvent` from left-down to mouse-exited, in every wx window, goes to the capture
  window (`WX_filterSendEvent`, `nonownedwnd.mm:141-163`). So an outside click reaches
  `wxPopupWindowHandler::OnLeftDown` → `HitTest` outside → `DismissAndNotify()` → the `LEFT_DOWN` is re-posted
  to `wxFindWindowAtPoint` ("dismissing a tooltip shouldn't waste a click", `popupcmn.cpp:557-581`). A click on
  the opener can therefore re-open the popup it just closed.
- `wxEVT_MOUSE_CAPTURE_LOST` is never generated by wxOSX, so `wxPopupWindowHandler::OnCaptureLost`
  (`popupcmn.cpp:625-632`) never runs on macOS.
- Calling `CaptureMouse()` on a window that already holds the capture pushes it twice on the capture stack
  (the "Recapturing" assert is compiled out). A popup that already holds the capture through `m_child` must
  use `if (!HasCapture()) CaptureMouse();`. See `references/mouse-keyboard-focus.md` for the capture stack.
- A single child receives the capture instead of the popup (§2), so handlers bound on the popup see none of
  the captured events.

**GTK3** [source]. GTK2 behaves the same except where noted.
- The window is `GTK_WINDOW_POPUP` with type hint `GDK_WINDOW_TYPE_HINT_COMBO`. Its `transient_for` is the
  parent's toplevel, set only at `Create` and only if `gtk_widget_get_toplevel(parent)` is already a `GtkWindow`
  (`src/gtk/popupwin.cpp:90-122`).
- On show: `gtk_grab_add` plus `gdk_seat_grab(ALL_POINTING, owner_events=true)` on GTK ≥3.20, `gdk_pointer_grab`
  on older GTK and GTK2. On hide: `gdk_seat_ungrab` + `gtk_grab_remove` (`popupcmn.cpp:335-403`).
  `gdk_seat_ungrab` releases the *seat's* grab, whichever popup took it, so hiding a sub-popup also drops the
  parent popup's pointer grab.
- An outside press arrives at the grab widget. `gtk_popup_button_press` sends `wxEVT_KILL_FOCUS` to the popup
  and **consumes the click** (`return TRUE`, `src/gtk/popupwin.cpp:28-58`). Presses older than the popup's
  creation time are ignored (33-34).
- `m_focus` is forced to the popup itself (`popupcmn.cpp:320-324`). `wxPopupWindow::SetFocus` focuses the first
  child that accepts focus (`src/gtk/popupwin.cpp:191-208`).
- Stock wx does not dismiss on app or toplevel deactivation: Alt+Tab leaves the popup up.
- A popup opened on hover holds the grab, so it steals the next click from the window that opened it.
  `MainFrame` installs its slice-button hover-open handlers only `#ifndef __linux__` for this reason.

**Wayland** (GTK3 only). The `COMBO` hint is required ("GTK only maps COMBO and {DROPDOWN,POPUP}_MENU to
popups", `src/gtk/popupwin.cpp:110-114`), and an `xdg_popup` needs a mapped parent surface (§9).
`wxGetMousePosition` has no global coordinate space there (`references/mouse-keyboard-focus.md`), so
geometry-based dismissal checks must use focus instead (§8).

**Pitfalls.**
- **Rule:** A toggle opener must ignore clicks that arrive right after the popup was dismissed.
  **Why:** On MSW the dismissing click reaches the opener (default style), or is processed before the deferred
  dismissal (`wxPU_CONTAINS_CONTROLS`). On macOS it is re-posted to the opener. Either way the popup reopens
  immediately.
  ```cpp
  // Wrong
  void on_opener_click(wxMouseEvent&) { m_drop.Popup(); }
  // Right: stamp the time in OnDismiss, refuse to reopen for a short window
  void on_opener_click(wxMouseEvent&) { if (m_drop.HasDismissLongTime()) m_drop.Popup(&m_drop); }
  ```
  Cite: `DropDown::HasDismissLongTime` (≥20 ms after `OnDismiss`, used by `ComboBox::mouseDown`),
  `StatusPanel::on_switch_speed` (`speed_dismiss_time`, 200 ms).
- **Rule:** Don't rely on `ProcessLeftDown` to keep a popup open on Windows.
  **Why:** MSW never calls it.
  ```cpp
  // Wrong: works on macOS/GTK only
  bool ProcessLeftDown(wxMouseEvent& e) override { return keep_open(e); }
  // Right: decide in a Dismiss() override, and on MSW route the hook there too
  void Dismiss() override { if (!should_close()) return; PopupWindow::Dismiss(); }
  #ifdef __WXMSW__
  void MSWDismissUnfocusedPopup() override { Dismiss(); OnDismiss(); }
  #endif
  ```
  Cite: `SearchDialog::Dismiss`, `SearchDialog::MSWDismissUnfocusedPopup`. Overrides such as
  `SidePopup::ProcessLeftDown` only forward to the base and add nothing.

## 6. Orca PopupWindow

`src/slic3r/GUI/Widgets/PopupWindow.{hpp,cpp}`:
```cpp
class PopupWindow : public wxPopupTransientWindow {
public:
    PopupWindow(wxWindow* parent, int style = wxBORDER_NONE);
    bool Create(wxWindow* parent, int flags = wxBORDER_NONE);
#ifdef __WXMSW__
    void BindUnfocusEvent();                                              // opt-in
#endif
protected:
    virtual bool ShouldDismissOnTopWindowDeactivate() { return true; }    // consulted on GTK only
};
```

What it adds per platform:

| Port | Behaviour |
|---|---|
| GTK | `Create` binds `wxEVT_ACTIVATE` on `GetTopParent(parent)`. `topWindowActiavate` calls `event.Skip()`, then `DismissAndNotify()` if the toplevel deactivated, the popup is shown and `ShouldDismissOnTopWindowDeactivate()` returns true. This supplies the app/toplevel deactivation dismissal that stock wxGTK lacks. |
| MSW | Nothing automatic. `BindUnfocusEvent()` binds the toplevel's `wxEVT_ACTIVATE`/`wxEVT_ICONIZE`/`wxEVT_SHOW` to `Dismiss()` (no `OnDismiss`). It is for default-style popups: they are desktop children that Windows does not hide with the frame, and stock wx closes them on deactivation or minimizing only if a focus message reaches a wx window (§4, §5). `topWindowActivate` does **not** `Skip()`, so while the popup lives it swallows the frame's `wxEVT_ACTIVATE`, and with it `wxTopLevelWindowMSW::OnActivate`'s focus save and restore (`src/msw/toplevel.cpp:1326-1360`). |
| macOS | Only with `wxPU_CONTAINS_CONTROLS`: `OnMouseEvent2`, bound on the popup for `LEFT_DOWN/UP/DCLICK`, `MOTION` and `MOUSEWHEEL`, hit-tests the children (static `HitTest`) and re-dispatches the event to the deepest child under the cursor with `ProcessEventLocally`. On `MOTION` it synthesizes `ENTER`/`LEAVE` for the hovered target. This compensates for the capture rerouting in §5. |

Details [source] (`Widgets/PopupWindow.cpp`).
- `GetTopParent(w)` returns the first *strict* ancestor of `w` that is a `wxNonOwnedWindow`, or the root if there
  is none. `Create` starts from `parent`, while `BindUnfocusEvent` and the destructor start from the popup. When
  `parent` is itself a top-level window with a parent (a dialog), `Create` therefore binds the dialog's parent
  (e.g. MainFrame), and the GTK deactivation hook watches the wrong toplevel. A dialog-parented popup also gets no
  GTK dismissal when the dialog alone deactivates. Parent popups to a control inside the toplevel.
- A null parent is dereferenced by `GetTopParent` in `Create` on GTK. `StatusPanel::on_switch_speed` passes
  `nullptr` only under `__WXOSX__` ("MacOS has focus problem"), where nothing is bound.
- The destructor unbinds what it can. The binds use member functions with the popup as handler, so wx removes
  them from the toplevel when the popup is destroyed anyway (the sink tracking in `wxEvtHandler::DoBind` /
  `OnSinkDestroyed`, `src/common/event.cpp:1793-1802`, 2022-2042), but late: after the popup's native window and
  children are gone, so the toplevel can still call into the half-destroyed popup meanwhile
  (`references/events.md` §2). The destructor's unbind closes that window wherever it resolves the same toplevel
  as the bind. A **lambda** capturing `this` and bound on another window is not tracked. Unbind it explicitly in the
  destructor, or bind on the popup itself. See `references/events.md` §2 Bind and Unbind (lifetime by handler kind).
- The macOS forwarder sees captured events only when the popup has zero children or two or more. With exactly
  one child (a single content panel), wx gives the capture to that child (§2), and `OnMouseEvent2`, bound on the
  popup, never runs.
- `OnMouseEvent2` keeps the last hovered window in the raw pointer `hovered` and never resets it. Don't destroy
  children of a *shown* `PopupWindow` on macOS: the next `MOTION` would dereference the dead child. Rebuild the
  content while the popup is hidden.
- Forwarding goes through wx handlers only. A native control (`wxBitmapToggleButton`-based `SwitchButton` or
  `CheckBox`, a native `wxButton`) never sees the `NSEvent`, so its native click action may not fire inside a
  `PopupWindow` on macOS. `FilamentGroupPopup` binds `wxEVT_LEFT_DOWN` on its `SwitchButton` under `__WXOSX__` and
  calls `Command()` itself. Orca's owner-drawn widgets (the `StaticBox`-based `Button`, the `wxWindow`-based
  `SideButton`) react to wx mouse events and work.

**Pitfalls.**
- **Rule:** On macOS, never trust a synthesized ENTER/LEAVE or a hover-timer callback inside a transient popup:
  re-check the real cursor position first.
  **Why:** `OnMouseEvent2` synthesizes ENTER/LEAVE from `MOTION`. After the cursor leaves, `wxPopupTransientWindow::OnIdle`
  re-captures. The next `MOTION` outside reaches `OnMouseEvent2` with no child hit, so a spurious ENTER fires on
  the popup with the cursor already outside. That cancels a dismiss timer, and the popup never closes
  (797ee70b0b). Separately, moving from the opener into the popup delivers the popup's ENTER *before* the opener's
  LEAVE. The opener's LEAVE then restarts the timer, and the popup closes under the cursor (f899d5a35d).
  ```cpp
  // Wrong
  void OnEnterWindow(wxMouseEvent&) { ResetTimer(); }
  void OnTimer(wxTimerEvent&)       { Dismiss(); }
  // Right
  void OnEnterWindow(wxMouseEvent&) {
      if (!GetClientRect().Contains(ScreenToClient(wxGetMousePosition()))) return; // spurious ENTER
      ResetTimer();
  }
  void OnTimer(wxTimerEvent&) {
      if (GetClientRect().Contains(ScreenToClient(wxGetMousePosition()))) return;  // still hovering
      Dismiss();
  }
  ```
  Cite: 797ee70b0b, f899d5a35d (`FilamentGroupPopup::OnEnterWindow`, `OnLeaveWindow` and `OnTimer`, whose guard is
  `#if __APPLE__`). The opener side is `MainFrame`'s `m_slice_btn`/`slice_panel` `wxEVT_LEAVE_WINDOW` →
  `FilamentGroupPopup::tryClose()`. The root cause is `PopupWindow::OnMouseEvent2`.
- **Rule:** In popup mouse handlers, return early when `!IsShown()`.
  **Why:** macOS can deliver a stray `LEFT_DOWN` to a popup after `OnDismiss`. The mechanism is not established.
  A related wx behaviour re-posts the dismissing `LEFT_DOWN` asynchronously to the window under the cursor (§5).
  ```cpp
  void DropDown::mouseDown(wxMouseEvent& e) { if (!IsShown()) return; /* stray LEFT_DOWN on Mac after OnDismiss */ ... }
  ```
  Cite: `DropDown::mouseDown`.
- **Rule:** On MSW, call `BindUnfocusEvent()` for a default-style popup that must close when the frame
  deactivates, minimizes or hides. Any handler you add on another window's `wxEVT_ACTIVATE` must `Skip()`.
  ```cpp
  auto* pop = new PopupWindow(m_switch_speed);
  #ifdef __WXMSW__
  pop->BindUnfocusEvent();
  #endif
  ```
  Cite: `StatusPanel::on_switch_speed`.

## 7. DropDown and the ComboBox popup

`Widgets/DropDown.{hpp,cpp}` is the stable design for list popups.

- **Construction.** `DropDown` is a `PopupWindow` created with `wxPU_CONTAINS_CONTROLS` that paints its rows
  itself. It has no child windows and owns no items: it holds `std::vector<Item>&` **by reference**, so the
  caller (normally `ComboBox`) owns the items and must outlive it. Call `Invalidate()` after mutating the
  items. It is usable standalone.
- **Opening.** `ComboBox::ForceDropdownOpen` and `ComboBox::mouseDown` run `messureSize()` → `autoPosition()` →
  `Popup(&drop)`, then emit `wxEVT_COMBOBOX_DROPDOWN`. `autoPosition` passes `off.x = 0` (left-aligned, §3) but
  starts 6 px above the combo's top-left with `off.y = comboHeight + 12`, so the list sits 6 px below (or, flipped,
  above) the combo. A sub-dropdown is placed beside the hovered row of its main dropdown instead.
- **Closing.** `OnDismiss` stamps `dismissTime` and sends `EVT_DISMISS`, which `ComboBox` maps to
  `wxEVT_COMBOBOX_CLOSEUP`. `HasDismissLongTime()` (not shown and ≥20 ms since dismissal) gates reopening (§5).
  `ComboBox` builds `wxEVT_COMBOBOX_DROPDOWN`/`CLOSEUP` with no id and no event object, so bind them on the combo
  without an id filter.
  `DropDown` declares `friend class ComboBox` so the combo can call the private `messureSize`/`autoPosition`
  (and the protected `DismissAndNotify`).
- **Selection.** `sendDropDownEvent` emits `wxEVT_COMBOBOX` (int = index, string = text) from the popup, and
  `ComboBox` consumes it and re-emits it with its own id. This is the `wxWS_EX_BLOCK_EVENTS` relay of §2 (needed on
  MSW/macOS; on GTK the event would bubble without it, but with the popup's id and object).
- **macOS: capture held for the popup's lifetime.** `DropDown::Create` binds an empty `wxEVT_IDLE` handler under
  `__WXOSX__` ("PopupWindow releases mouse on idle, which may cause … losting mouse move, and dismissing soon on
  first LEFT_DOWN"). Dynamic handlers run before the static event table (`src/common/event.cpp:1650-1656`), and
  this one doesn't `Skip()`, so `wxPopupTransientWindow::OnIdle` (event table, `popupcmn.cpp:108-112`) never
  runs. The capture taken in `Show(true)` is held until `Show(false)`. `DropDown` therefore gets every mouse
  event in its own coordinates, including points outside it, which is why `DropDown::mouseMove` forwards
  out-of-bounds points to `mainDropDown` on `__WXOSX__`. A popup subclass can use the same technique to opt out of
  the idle capture juggling. Such a popup holds the capture for its whole life, so its own press handler must
  use `if (!HasCapture()) CaptureMouse();`, or it pushes itself twice on the capture stack
  (`references/mouse-keyboard-focus.md`). `DropDown::mouseDown` itself calls `CaptureMouse()` unguarded (only its
  release is guarded), so don't copy its press handler.
- **Chained sub-dropdown** (`subDropDown`/`mainDropDown`, for grouped items):
  - The sub is created in `messureSize` with `subDropDown->Create(GetParent())`. Its wx parent is the combobox,
    not the main dropdown. On GTK the wx parent decides ownership (`m_parent->AddChild`,
    `src/gtk/popupwin.cpp:134`) and only the *initial* `transient_for`, which is the frame (116-122). wx pushes
    its handlers onto the popup's own `m_child` (`popupcmn.cpp:312-315`), not onto the parent.
  - On GTK, `gtk_window_set_transient_for(sub, main)` runs in `DropDown::mouseMove`, right before
    `autoPosition()` and `Popup()`, so the sub points at the currently mapped parent popup.
  - `ShouldDismissOnTopWindowDeactivate()` returns false while a chain peer is shown.
  - `Dismiss()` vetoes while the sub is shown. `OnDismiss` returns early in that case.
  - The sub's `OnDismiss` dismisses the main dropdown unless the cursor is over it. On Win32 it calls
    `SetActiveWindow(mainDropDown->GetHandle())` to re-activate the main one.
  - On GTK the sub binds an idle handler. While both are shown and the cursor is over main but not over the
    sub, it builds a `wxMouseEvent(wxEVT_MOTION)` from `wxGetMousePosition()`, calls `mainDropDown->mouseMove(ev)`
    **directly** (nothing is posted), and calls `e.RequestMore()` to keep polling.
- **GTK wrapper sizing.** `messureSize` calls `gtk_window_resize` on `m_widget` after `SetSize`, because wxGTK's
  `DoSetSize` only uses `gtk_widget_set_size_request` ("gtk_window_resize does not work for GTK_WINDOW_POPUP",
  `src/gtk/popupwin.cpp:181-188`).

**Pitfalls.**
- **Rule:** On wxGTK/Wayland, when chaining popups, keep the wx parent as the original combobox, but call
  `gtk_window_set_transient_for()` on the sub's native window, pointing at the *currently mapped* parent popup,
  immediately before positioning and showing it, not at creation. Also make the dismissal hook ignore
  deactivation while a chain peer is shown.
  **Why:** xdg-shell requires a popup's parent to be the topmost mapped popup. A `transient_for` set at creation
  can point at a surface that is no longer the mapped one by show time (broken on wlroots/Hyprland). Mapping a
  chained `xdg_popup` with a grab makes the parent toplevel inactive, and `PopupWindow`'s GTK activate handler
  would read that as "clicked elsewhere" and cascade-dismiss the whole chain. Mutter also drops motion events
  outside the grabbing surface, so the parent needs the idle poll above to keep its hover tracking alive while
  the sub holds the grab. (The compositor facts come from the commits and code comments; they cannot be checked
  in the wx tree.)
  ```cpp
  // Wrong: set once at creation (stale parent on wlroots), always dismiss on deactivate
  subDropDown->Create(GetParent());
  gtk_window_set_transient_for(GTK_WINDOW(subDropDown->GetHandle()), GTK_WINDOW(GetHandle()));
  // Right: re-parent natively just before showing, and gate the deactivate-dismiss
  drop.messureSize();
  #ifdef __WXGTK__
  if (m_widget && drop.m_widget)
      gtk_window_set_transient_for(GTK_WINDOW(drop.m_widget), GTK_WINDOW(m_widget));
  #endif
  drop.autoPosition();
  drop.paintNow();
  if (!drop.IsShown()) drop.Popup(&drop);

  bool DropDown::ShouldDismissOnTopWindowDeactivate()   // declared `override` in the class
  {
      return !((mainDropDown && mainDropDown->IsShown()) || (subDropDown && subDropDown->IsShown()));
  }
  ```
  Cite: c0ae2bda99, b9ff15054f (`DropDown::mouseMove`, `DropDown::messureSize`, `DropDown::ShouldDismissOnTopWindowDeactivate`,
  `PopupWindow::ShouldDismissOnTopWindowDeactivate`).
- **Rule:** When shrinking a dropdown to fit above the display bottom, give up if fewer than two rows would
  remain.
  **Why:** On Wayland the popup can land where almost no vertical space is left. Clamping to that sliver made an
  unusable, near-zero-height submenu instead of letting the normal size stand. `Position()` never shrinks
  (§3), so this guard is the caller's. It is unconditional, not Wayland-only.
  ```cpp
  // Wrong: always clamp
  size.y = drect.GetBottom() - GetPosition().y - 10;
  // Right
  int available_height = drect.GetBottom() - GetPosition().y - 10;
  if (available_height < rowSize.y * 2) return;
  size.y = available_height;
  ```
  Cite: b9ff15054f (`DropDown::autoPosition`).

## 8. Other Orca popups

- **`SidePopup`** (`Widgets/SideMenuPopup.{hpp,cpp}`): the Slice/Print option menus. A
  `PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)` filled with `SideButton`s via `append_button()`.
  `Popup(focus)` lays out the buttons, positions under `focus` (flush on macOS, §3), shifts left to stay on the
  display, and calls `set_side_menu_popup_status(true)`. `OnDismiss` resets the status, so it is reset only on
  automatic dismissal: a menu button that closes the popup with a direct `Dismiss()` skips it (§2, Rule 8).
- **Self-vetoing search popups** (`Search.cpp` `SearchDialog`, `SearchObjectDialog`):
  - `Dismiss()` decides whether to `Die()` (the real `PopupWindow::Dismiss()` plus a posted
    `wxCUSTOMEVT_EXIT_SEARCH`). It dismisses when focus is gone, or when the mouse is outside the popup and its
    related controls.
  - On GTK under Wayland (`is_running_on_wayland()`, `LinuxDisplayBackend.hpp`) the mouse rectangle is useless,
    so it dismisses when focus has left the popup and its related controls (`focus_left_popup`).
  - On MSW, `MSWDismissUnfocusedPopup()` is overridden to `Dismiss(); OnDismiss();`, so the hook goes through
    the veto.
  - On macOS, `SearchObjectDialog::Popup` moves focus off the text input (`m_object_list->SetFocus()`) before
    `Popup()`, because otherwise the input is unusable.
  - `SearchObjectDialog::Popup` refuses while already shown or while `m_is_dismissing`.
- **Speed popup** (`StatusPanel::on_switch_speed`): a default-style `PopupWindow` hosting one `StepCtrl`. It is
  parented to `nullptr` on macOS and to the button elsewhere, calls `BindUnfocusEvent()` on MSW, destroys itself
  on `wxEVT_SHOW(false)`, and uses a 200 ms `speed_dismiss_time` debounce. Because it has a single child, the
  `StepCtrl` holds the macOS capture (§2).
- **GL canvas focus vs. MSW popups**: `GLCanvas3D::on_mouse` grabs focus on `evt.Entering()` so hotkeys work,
  but skips it on MSW while `wxCurrentPopupWindow` is non-null (declared `extern wxPopupWindow*
  wxCurrentPopupWindow;` under `__WXMSW__`). Stealing focus would run the §5 hook and close the open search
  dropdown. Any window that grabs focus on hover needs the same guard.
- **Hover popups and Linux**: `FilamentGroupPopup` opens on hover of the slice button and closes on a 300 ms
  timer. `MainFrame` installs those hover handlers only off Linux, because the GTK grab would steal the slice
  button's click (§5).

## 9. GTK and Wayland parenting and grabs

§5 (GTK3, Wayland) gives the window type, the `COMBO` type hint, the one-time `transient_for` and the grabs.
An `xdg_popup` needs a parent surface to be placed, and wx never retries `transient_for` after `Create`. Every
shown transient popup holds a GTK grab and a seat pointer grab, so a WebView or a native text field inside it
fights the grab and focus model (§10). Two more [source] facts:
- **Size.** Move and resize go through `gtk_window_move` and `gtk_widget_set_size_request`. `DoMoveWindow`
  fails a silent `wxFAIL` (`src/gtk/popupwin.cpp:146-149`).
- **Coordinates.** `wxGetMousePosition` uses `gdk_device_get_position` (`src/gtk/window.cpp:7026-7041`), and
  Wayland has no global coordinate space. Orca code records the results as "(0,0)" or "unreliable"
  (`Button::OnParentMotion`, `SearchDialog::Dismiss` comments). Detect Wayland at runtime with
  `Slic3r::GUI::is_running_on_wayland()`.

**Pitfalls.**
- **Rule:** On wxGTK, never open a popup from a window that is not mapped yet. Defer with `CallAfter` and check
  `IsShownOnScreen()`. When the popup's logical parent is a short-lived embedded window (a data-view cell editor),
  set the popup's `transient_for` explicitly by walking `GetParent()` up to a widget whose
  `gtk_widget_get_toplevel()` is a real `GtkWindow`.
  **Why:** A data-view editor gets `wxEVT_SET_FOCUS` before its native window is mapped. A popup created then has
  no transient parent, because wxGTK sets it only at `Create`, and only if the parent is already inside a
  `GtkWindow`. The popup is mispositioned or orphaned on X11, and Wayland needs a parent surface to place it.
  ```cpp
  // Wrong: open synchronously from the focus event
  c_editor->Bind(wxEVT_SET_FOCUS, [c_editor](wxFocusEvent& e) { c_editor->ForceDropdownOpen(); e.Skip(); });
  // Right (wxGTK): one event-loop turn later, only if actually mapped
  c_editor->CallAfter([c_editor] { if (c_editor->IsShownOnScreen()) c_editor->ForceDropdownOpen(); });
  // and in the popup, before showing (DropDown::Popup, top-level dropdown only):
  for (wxWindow* w = GetParent(); w; w = w->GetParent())
      if (GtkWidget* gw = static_cast<GtkWidget*>(w->GetHandle()))
          if (GtkWidget* top = gtk_widget_get_toplevel(gw); GTK_IS_WINDOW(top)) {
              gtk_window_set_transient_for(GTK_WINDOW(m_widget), GTK_WINDOW(top)); break; }
  ```
  The deferral is safe if the editor dies first: `CallAfter` queues on `c_editor`, and pending events die with
  their handler (see `references/events.md` §CallAfter). Other ports open synchronously.
  Cite: 88b4a63228 (`ExtraRenderers.cpp` `BitmapChoiceRenderer::CreateEditorCtrl`, `#ifdef __WXGTK__`;
  `DropDown::Popup`, applied when `!mainDropDown && m_widget`).
- **Rule:** Don't open a grabbing popup on hover on Linux if the opener must stay clickable.
  Cite: `MainFrame` slice-button hover handlers (`#ifndef __linux__`).

## 10. Frameless dialog instead of a popup

A transient popup cannot guarantee keyboard focus. On MSW it gets focus only with the flag, on macOS it is a
non-activating panel, and on GTK it sits under grabs. A frameless dialog gets normal activation and focus on
every port, and closes itself like a popup by hiding on deactivation.

**OrcaSlicer.** `SpeedDialWebDialog` (`SpeedDialDialog.{hpp,cpp}`) is a `WebViewHostDialog` (→ `DPIDialog`)
created with `wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxFRAME_SHAPED`.
- `wxFRAME_NO_TASKBAR` gives no taskbar entry on MSW/GTK, on GTK only when the WM supports
  `_NET_WM_STATE_SKIP_TASKBAR` (`interface/wx/frame.h:97-103`).
- `wxFRAME_FLOAT_ON_PARENT` requires a non-null parent (`interface/wx/frame.h:104-106`).
- Its `wxEVT_ACTIVATE` handler hides on deactivation and `Skip()`s. On activation it focuses the web view, because
  modeless focus is granted asynchronously and a focus request made right after `Show()` is dropped.
- On Linux `focus_webview` grabs focus on the WebKit native widget (`gtk_widget_grab_focus` on
  `GetNativeBackend()`). `browser()->SetFocus()` focuses only the container.
- Display-only web content can stay in a transient popup (`MarkdownTip`).

**Pitfalls.**
- **Rule:** For popup-like UI with complex content (a `wxWebView`, focus-holding controls), use a frameless
  `wxDialog` that hides on `wxEVT_ACTIVATE`-inactive, instead of a `wxPopupTransientWindow`.
  **Why:** On GTK a `wxPopupWindow` is a `GTK_WINDOW_POPUP` that the window manager does not manage
  (`src/gtk/popupwin.cpp:105`), and `Show` grabs all pointing devices (`popupcmn.cpp:372-402`). A WebView inside
  cannot reliably take keyboard focus, and dismissal diverges per platform, worst on Linux.
  ```cpp
  // Wrong: SpeedDialWebPopup : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS) hosting a wxWebView
  // Right:
  SpeedDialWebDialog(wxWindow* parent)
      : WebViewHostDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                          wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxFRAME_SHAPED)
  {
      Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
          if (e.GetActive() && IsShown())       focus_webview(browser(), m_page_ready);
          else if (!e.GetActive() && IsShown()) Hide();
          e.Skip();
      });
  }
  ```
  Cite: f142716e51, a86202b45b (`SpeedDialPopup.cpp` replaced by `SpeedDialDialog.{hpp,cpp}`; `GUI_App.cpp`).
- **Rule:** When restoring the main frame after hiding a popup or overlay frame, call `Show()` only if
  `!IsShown()`, and `Raise()` unconditionally (top-level `Show`/`Raise` are owned by
  `references/windows-dialogs.md`).
  **Why:** On GTK3 a redundant `Show(true)` is not inert. `wxTopLevelWindowGTK::Show` runs
  `GTKSendSizeEventIfNeeded()` even when nothing changed (`src/gtk/toplevel.cpp:1259-1269`), which synchronously
  flushes a pending size event into layout handlers. In Orca this froze the app permanently after the
  filament-sync popup was hidden.
  ```cpp
  // Wrong
  mainframe->Show(); mainframe->Raise();
  // Right
  if (!mainframe->IsShown()) mainframe->Show();
  mainframe->Raise();
  ```
  Cite: dd8cb89f6d (`BaseTransparentDPIFrame::on_hide`).
- **Rule:** For overlays that must never take keyboard focus (above a GL surface), use a plain
  `wxPopupWindow(top, wxBORDER_NONE)`, not a `wxFrame`.
  **Why:** A frame took the X input focus and swallowed every shortcut until the user clicked the canvas. A popup
  window does not take focus when shown. On macOS these overlays hide while the app is inactive (§5).
  Cite: `CAD/DesignCanvas.cpp` (`m_hud`, `m_status_hud`).
- **Rule:** Never `Raise()` a `wxPopupWindow`. To bring an overlay up, `Show()` it if it is hidden, then
  `Move()` it.
  **Why:** `Raise()` is documented for top-level windows only (`interface/wx/window.h:3028-3029`), and a popup
  derives from `wxNonOwnedWindow`, not `wxTopLevelWindow` (`include/wx/popupwin.h:33`). On macOS it makes the
  popup the key window (§5): the popup takes the keys meant for the window below it, and the frame receives
  `wxEVT_ACTIVATE(false)`. A frame activate handler that hides the overlay on deactivation and re-places it on
  activation then loops: each `Raise()` deactivates the frame, the hide reactivates it, and the re-place raises
  again, recursing until the main thread's stack overflows. A popup's `Show()` is `ShowWithoutActivating`
  and is safe.
  ```cpp
  // Wrong
  if (!overlay->IsShown()) overlay->Show();
  overlay->Move(pos);
  overlay->Raise();
  // Right
  if (!overlay->IsShown()) overlay->Show();
  overlay->Move(pos);
  ```

## 11. wxComboCtrl / wxComboPopup

- "It is important to call SetPopupControl() as soon as possible" (`interface/wx/combo.h:310-317`).
- `wxComboPopup`: prepare members in `Init()`, which runs right after construction with `m_combo` set ("m_combo
  is not valid in constructor"); create the control in `Create(parent)` and return it from `GetControl()`;
  `LazyCreate()` returning true defers `Create()` to the first show (`interface/wx/combo.h:35-142`).
- The popup host is a `wxPopupTransientWindow` subclass created with `wxPU_CONTAINS_CONTROLS`
  (`src/common/combocmn.cpp:181-186`, 324-332).
- `UseAltPopupWindow()` "guarantees ability to focus the popup control, and allows common native controls to
  function normally. This alternative popup window is usually a wxDialog, and as such, when it is shown, its
  parent top-level window will appear as if the focus has been lost from it" (`combo.h:927-934`). It is
  really a `wxFrame` on MSW, a `wxDialog` on GTK and a `wxNonOwnedWindow` on macOS (`combocmn.cpp:125-167`).
- Orca's `create_combochecklist` (`GUI.cpp`, with `wxCheckListBoxComboPopup` from `wxExtensions.hpp`) calls
  `UseAltPopupWindow()`. Its FIXME records the trade-off: without it the check list does not react to clicks, and
  with it the popup cannot be closed by clicking the combo button on Windows 10. New Orca code uses
  `::ComboBox`/`DropDown` instead.

## 12. Tip windows, info bars, notifications

- **`wxTipWindow`** derives from `wxPopupTransientWindow` (`include/wx/tipwin.h:26`), although the doc says
  `wxWindow`. "The window automatically destroys itself when the user clicks on it or it loses the focus"
  (`interface/wx/tipwin.h:13-14`). Since 3.3.2 use `static Ref New(parent, text, maxLength, rectBound)`. The
  returned `wxTipWindow::Ref` is "guaranteed to become invalid when the tip window is closed", and the raw
  constructor is deprecated (`tipwin.h:27-132`; `docs/changes.txt:286`). `Create` positions the tip at the mouse
  and pops it up immediately (`src/generic/tipwin.cpp:171-240`); on GTK it also captures the mouse.
  `SetBoundingRect` closes the tip when the mouse leaves that rectangle (`tipwin.h:160-172`). Orca:
  `Button::OnParentMotion` (MSW-only tooltips for disabled buttons) keeps a `wxTipWindow::Ref`.
- **`wxRichToolTip`** popups are private wx classes. `Sidebar::priv::show_rich_tip` (`_WIN32`) finds the shown
  popup by `dynamic_cast` to `wxCustomBackgroundWindow<wxPopupTransientWindow>`, which mirrors wx's private
  `wxRichToolTipPopup` (`src/generic/richtooltipg.cpp`). Re-check it on every wx update.
- **`wxInfoBar`** "calls its parent wxWindow::Layout() method and assumes that it will change the parent
  layout": add it to the parent's sizer (`interface/wx/infobar.h:38-44`). It is generic on all ports and native
  in wxGTK. Use `ShowMessage(msg, flags)`/`Dismiss()`. 3.3 adds `ShowCheckBox` (`docs/changes.txt:460`).
- **`wxNotificationMessage`** is "not a window", native on MSW, macOS and GTK (`interface/wx/notifmsg.h:12-18`).
  MSW recommends `MSWUseToasts()` before the first notification, macOS needs the alert notification style for
  actions, and GTK ignores the timeout for warnings and errors (`notifmsg.h:23-38`, 179-182). 3.3 reports the
  dismissal reason (`docs/changes.txt:488`). Orca does not use it. Its in-app notifications are the GL-canvas
  `NotificationManager`.

## 13. wxMenu and wxMenuItem

**Ownership** (`interface/wx/menu.h:471-478`). "All menus must be created on the heap because all menus attached
to a menubar or to another menu will be deleted by their parent … The only exception to this rule are the popup
menus (i.e. menus used with wxWindow::PopupMenu()) … But the exception applies only to the menus themselves and
not to any submenus of popup menus which are still destroyed by wxWidgets as usual and so must be
heap-allocated." `PopupMenu` never deletes the menu (`interface/wx/window.h:3337`). `Delete(id)` does not delete a
submenu (it leaks), `Destroy(id)` deletes it, and `Remove()` returns ownership (`menu.h:677-720`).

**Appending** (`menu.h`).
- `Append(id, text, help, kind)`, `AppendCheckItem`, `AppendRadioItem`, `AppendSeparator`,
  `AppendSubMenu(submenu, text, help)`. `Append(id, text, wxMenu*)` "is deprecated, use AppendSubMenu()
  instead" (576-580).
- `Append(wxMenuItem*)` takes ownership ("do not delete it yourself"). The `wxMenuItem*` overloads
  (`Append`/`Insert`/`Prepend`) are how you set a bitmap or font before insertion (600-618).
- `Break()` "only actually inserts a break in wxMSW" (656-661).

**IDs.**
- Use `wxID_ANY`. Menu items then get negative auto ids from `wxWindow::NewControlId()`, held by a
  `wxWindowIDRef` (`src/common/menucmn.cpp:228-232`, `include/wx/menuitem.h:163`,
  `docs/doxygen/overviews/windowids.h`). On MSW (`wxUSE_AUTOID_MANAGEMENT`) the ref releases the id for reuse when
  the item dies; on macOS and GTK the auto ids count down through -2000…-1000000 without reuse and then wrap
  (`references/events.md` §10).
- `wxNewId()` is deprecated: "Ids generated by it can conflict with the Ids defined by the user code, use
  wxID_ANY to assign ids which are guaranteed to not conflict with the user-defined ids for the controls and
  menu items you create instead of using this function" (`interface/wx/utils.h:432-444`). It counts up forever
  from 100, skipping the stock range (`src/common/utilscmn.cpp:653-663`).
- Explicit ids must lie in `[0, SHRT_MAX)` or in the auto range: "ids are limited to 16 bits under MSW"
  (`menucmn.cpp:251-258`; the assert is compiled out in Orca).

**Check and radio items.**
- "wxWidgets automatically toggles the flag value when the item is clicked" (`menu.h:455-458`). The handler
  reads `evt.IsChecked()`, set via `SetInt` in `wxMenuBase::SendEvent` (`menucmn.cpp:670-676`); macOS toggles the
  item before sending (`wxMenu::HandleCommandProcess`, `src/osx/menu_osx.cpp:352-359`).
- A radio group "is formed by a contiguous range of radio items … inserting or removing the items in the menu
  containing the radio items risks to not work correctly" (`menu.h:459-466`).
- GTK silently ignores `Check(false)` on a radio item (`src/gtk/menu.cpp:763-777`).
- `Check(id)`, `Enable(id)` and `SetLabel(id)` on a missing id fail a silent `wxCHECK` and do nothing
  (`menucmn.cpp:816-860`).

**Bitmaps** [source].
- Set the bitmap (`SetBitmap(const wxBitmapBundle&)`) **before** `Append`. On GTK a normal item becomes a
  `GtkImageMenuItem` only if it already has a bitmap at append time. Otherwise it is a plain `GtkMenuItem`, and
  the later `SetupBitmaps()` casts it to `GTK_IMAGE_MENU_ITEM` and fails (`src/gtk/menu.cpp:748-761`, 944-1016).
- GTK check and radio items are `GtkCheckMenuItem`/`GtkRadioMenuItem` and show no bitmap. On MSW a bitmap on a
  checkable item *replaces* the check mark (`src/msw/menuitem.cpp:757-769`).
- GTK honours the global `gtk-menu-images` setting (`interface/wx/menuitem.h:408-412`).
- `SetBitmap(bmp, checked)`, `SetBitmaps` and `SetDisabledBitmap` are `@onlyfor{wxmsw}` (`menuitem.h:415-441`).
- Bundles resolve at the DPI of the menu's window (`menucmn.cpp:344-362`). MSW re-runs `SetupBitmaps()` on every
  `wxEVT_MENU_OPEN` (`src/msw/window.cpp:2494-2501`). GTK does it in `DoPopupMenu`.

**Owner drawn (MSW)** (`interface/wx/menuitem.h:40-50`). `SetTextColour`, `SetBackgroundColour` and `SetFont` on
an item switch it to owner-drawn (`MSWMustUseOwnerDrawn`, `src/msw/menuitem.cpp:1371-1377`). "Owner drawn items
are also incompatible with dark mode support and using them makes the entire menu containing them to use
standard light mode colours even in dark mode." 3.3.2 reworked MSW dark-mode menu rendering
(`docs/changes.txt:305-306`); owner-drawn items still opt out of it.

**Labels and accelerators** (`menuitem.h:460-490`, 555-570).
- The syntax is `"&Label\tCtrl+X"`; `&&` is a literal ampersand. `CTRL` means Cmd on macOS, and `RAWCTRL` gives
  the real Control key there. `RAWCTRL` equals `CTRL` elsewhere.
- GTK: `Shift` with non-alphabetic keys does not work, and bare arrow keys need a modifier.
- [source] Only a menubar's accelerators are live. An accelerator in a *popup* menu label is display-only on
  MSW and GTK (GTK adds the menu's accel group to the window only in `AttachToFrame`,
  `src/gtk/menu.cpp:239-246`).
- On macOS the menubar's key equivalents run before `wxEVT_CHAR_HOOK` (`references/mouse-keyboard-focus.md`).

**Pitfalls.**
- **Rule:** Use `wxID_ANY` and bind by `item->GetId()`.
  **Why:** Auto ids are negative, so they can't collide with explicit ids, and on MSW they are recycled.
  `wxNewId()` is deprecated, grows for the whole session, and approaches the 16-bit MSW limit in menus that are
  rebuilt per show. Because MSW recycles auto ids, bind them on the per-show menu itself (whose bindings die with
  it), never on a long-lived window.
  ```cpp
  // Wrong
  const int id = wxNewId(); menu.Append(id, _L("Delete")); menu.Bind(wxEVT_MENU, cb, id);
  // Right
  wxMenuItem* del = menu.Append(wxID_ANY, _L("Delete"));
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { delete_item(); }, del->GetId());
  ```
  Cite: `Sidebar::update_mixed_filament_list` (the mixed-filament menu button).
- **Rule:** Bitmap first, then `Append`. Use no icons on check or radio items.
  ```cpp
  // Wrong
  wxMenuItem* mi = sub->Append(wxID_ANY, name); mi->SetBitmap(icon);   // GTK: no image item
  // Right
  auto* mi = new wxMenuItem(sub, wxID_ANY, name);
  mi->SetBitmap(icon);
  sub->Append(mi);
  ```
  Cite: `Sidebar::update_mixed_filament_list` (which skips the bitmap `#ifndef __linux__`), `append_menu_item`.
- **Rule:** Don't theme menu items with colours or fonts.
  **Why:** On MSW the item becomes owner-drawn, and the whole menu renders light in dark mode.
- **Rule:** When a menu is rebuilt, remove old items with `Destroy(id)` (it deletes submenus); `Delete(id)`
  leaks a submenu. A stack `wxMenu` is fine for a popup, but its submenus must be heap-allocated, and a submenu
  that was never appended must be deleted by you (`Sidebar::update_mixed_filament_list` does `delete sub_menu`).

## 14. PopupMenu and menu event routing

**Contract** (`interface/wx/window.h:3283-3345`).
- `PopupMenu` "Pops up the given menu at the specified coordinates, relative to this window, and returns control
  when the user has dismissed the menu. If a menu item is selected, the corresponding menu event is generated".
  Without coordinates it uses the mouse position.
- "Just before the menu is popped up, wxMenu::UpdateUI is called … The menu does not get deleted by the window."
- "It is recommended to not explicitly specify coordinates when calling PopupMenu in response to mouse click,
  because some of the ports (namely, wxGTK) can do a better job of positioning the menu in that case."
- `GetPopupMenuSelectionFromUser` returns "The selected menu item id or wxID_NONE". It "temporarily disables UI
  updates for the window, so you need to manually disable … any items which should be disabled in the menu
  before showing it".

**Semantics** [source].
- **Synchronous on every port.** MSW: `TrackPopupMenu`, then `wxYieldForCommandsOnly()`, so the `wxEVT_MENU`
  handler runs before `PopupMenu` returns and a stack `wxMenu` is safe ("the menu may be destroyed as soon as we
  return (it can be a local variable in the caller …)", `src/msw/window.cpp:2379-2408`). GTK: a nested
  `gtk_main_iteration()` loop runs until the menu hides (`src/gtk/window.cpp:6492-6597`). macOS:
  `popUpMenuPositioningItem` (`src/osx/cocoa/menu.mm:333-367`).
- **Return value.** GTK returns false when the menu failed to map. MSW and macOS always return true.
- **"Note that this function switches focus to this window before showing the menu" (window.h:3332) is not
  implemented.** Neither `wxWindowBase::PopupMenu` (`src/common/wincmn.cpp:3048-3060`) nor any port's
  `DoPopupMenu` calls `SetFocus`.
- **Coordinates** are client coordinates of the invoking window.
  - macOS nudges `x+1` when the menu's x equals the cursor's x, so no item is pre-selected
    (`src/osx/window_osx.cpp:879-884`).
  - With a modal dialog open, macOS falls back to `popUpContextMenu` with a synthetic right-click, because
    "action and validation methods are not called from macos for modal dialogs" (`menu.mm:340-363`).
  - On Wayland (GTK ≥3.22) wx uses `gtk_menu_popup_at_rect` anchored to the main GdkWindow and fabricates a
    trigger event when there is none (`src/gtk/window.cpp`, `wxWindowGTK::DoPopupMenu`; "Avoid GDK errors when
    using PopupMenu() with Wayland", `docs/changes_32.txt:317`).
  - Orca passes `wxDefaultPosition` under `__linux__` in `Plater::priv::show_right_click_menu`: "on Linux the
    menu isn't displayed if position is specified".
- **Destroying the invoking window while its menu is up is tolerated**: `~wxWindowBase` disassociates
  `wxCurrentPopupMenu` (`wincmn.cpp:474-478`).

**`wxEVT_MENU` routing** (`wxMenuBase::DoProcessEvent`, `src/common/menucmn.cpp:679-730`; documented in
`menu.h:483-500`). The order is: the menu's own handler, then each parent menu, then the menubar (if any), then
the invoking window (for popups, the window that called `PopupMenu`). From that window the command event bubbles
*up* through its parents as usual. Handlers bound on a sibling or child of the invoking window never fire.
Binding submenu items on a parent menu works on all ports ("Allow binding to events generated by their items in
submenus too", 3.1.2, `docs/changes_32.txt:787`).

**`wxMenuEvent`** (`interface/wx/event.h:4825-4860`). `MENU_OPEN`, `MENU_CLOSE` and `MENU_HIGHLIGHT` go to "The menu
object itself … The wxMenuBar to which this menu is attached … The window associated with the menu, e.g. the one
calling PopupMenu() … The top level parent of that window". "Unlike [command events], wxMenuEvent are only sent
to the window itself and its top level parent but not any intermediate windows".
- `EVT_MENU_OPEN`: "On Windows, this is only sent once for each navigation of the menubar".
- `EVT_MENU_CLOSE` "is currently being sent before the menu selection (wxEVT_MENU) event, if any".
- On MSW, popup-menu open/close events carry a menu only for the current popup menu itself; for its submenus
  `GetMenu()` is null [source] (`wxWindowMSW::MSWFindMenuFromHMENU`, `src/msw/window.cpp:2512-2519`).
- `wxEVT_MENU_HIGHLIGHT` uses `wxID_NONE` for "no item" (`menuitem.h:30-34`).

**`wxEVT_UPDATE_UI` for menus** (`interface/wx/event.h:2466-2488`).
- "These events will work for popup menus as well as menubars. Just before a menu is popped up, wxMenu::UpdateUI
  is called". "On Windows and GTK+, events for menubar items are only sent when the menu is about to be shown, and
  not in idle time."
- [source] `wxMenuBase::UpdateUI` sends each item's event to the **invoking window's** handler, from where it
  propagates up as a command event (`menucmn.cpp:624-668`), stopping at a window with `wxWS_EX_BLOCK_EVENTS`
  (dialogs; popups on MSW and macOS, §2). It does not consult the menu's own handlers, so bind `UPDATE_UI` on
  the invoking window or an ancestor below that boundary.
- [source] On macOS, Cocoa calls `validateMenuItem` for every item on every menu open and on key-equivalent
  matching. wx then sends `wxEVT_UPDATE_UI` through the full menu route (menu → menubar → window) and returns
  `IsEnabled()` (`src/osx/cocoa/menuitem.mm:103-116`, `src/osx/menu_osx.cpp:327-350`). Keep these handlers cheap.
- For a non-checkable item the event is marked `DisallowCheck()`. `evt.Check()` goes to `Set3StateValue`
  (`include/wx/event.h:3029`), whose assert "Shouldn't be called if non-checkable"
  (`src/common/event.cpp:476-483`) is compiled out in Orca. The menu then calls `Check()` on the item, which the
  port refuses silently (`wxCHECK_RET(IsCheckable(), …)` in `src/msw/menuitem.cpp:517-519` and
  `src/osx/menuitem_osx.cpp:97-99`; a compiled-out `wxFAIL_MSG` on GTK, `src/gtk/menu.cpp:784-785`). So in Orca it
  is a no-op, but it is a contract violation that asserts in any debug wx build. Test `evt.IsCheckable()` first
  (`interface/wx/event.h:2566-2579`, since 3.1.5).

**`GetPopupMenuSelectionFromUser`** [source] (`wincmn.cpp:3081-3101`) binds non-skipping `wxEVT_MENU` and
`wxEVT_UPDATE_UI` handlers on the window for the duration. Item handlers bound on the menu itself run *first*
(they are earlier in the route), and if they don't `Skip()`, the function returns `wxID_NONE`. Use one mechanism,
not both.

**Pitfalls.**
- **Rule:** Bind `MENU_OPEN`/`MENU_CLOSE` on the menu or on the window that calls `PopupMenu`.
  **Why:** `wxMenuEvent` skips intermediate windows. A toolbar that pops its menu with
  `GetParent()->PopupMenu(...)`, as `BBLTopbar` does, never receives a `wxEVT_MENU_CLOSE` bound on itself.
  ```cpp
  // Wrong: the invoking window is GetParent(), this handler is never reached
  this->Bind(wxEVT_MENU_CLOSE, &MyToolbar::OnMenuClose, this);
  GetParent()->PopupMenu(&m_menu, pos);
  // Right: bind on the menu (or on the invoking window)
  m_menu.Bind(wxEVT_MENU_CLOSE, &MyToolbar::OnMenuClose, this);
  ```
- **Rule:** Don't tear down menu state in `wxEVT_MENU_CLOSE`.
  **Why:** It is sent before `wxEVT_MENU`, so the selected item's handler would see the torn-down state.
- **Rule:** In a shared `UPDATE_UI` handler, write `if (evt.IsCheckable()) evt.Check(x);`.
- **Rule:** Item handlers bound on the menu must `Skip()` when you also read
  `GetPopupMenuSelectionFromUser`'s return value. Better, use only one of the two.
- **Rule:** Prefer `PopupMenu(menu)` without coordinates in response to a mouse click. Pass explicit coordinates
  only for anchoring to a control, in the invoking window's client coordinates.

## 15. wxMenuBar on macOS

**Special ids** [documented + source].
- `wxID_ABOUT` and `wxID_EXIT` "will be taken out of the normal menus under macOS and will be inserted into the
  system menu" (`interface/wx/menu.h:445-449`). `wxID_PREFERENCES` behaves the same way [source]. The ids come from
  `wxApp::s_macAboutMenuItemId`, `s_macPreferencesMenuItemId` and `s_macExitMenuItemId`, which default to
  `wxID_ABOUT`, `wxID_PREFERENCES` and `wxID_EXIT` (`src/osx/carbon/app.cpp:67-69`).
- The original items are hidden in place, along with an adjacent separator (`src/osx/menu_osx.cpp:268-300`).
- `CreateAppleMenu` builds About, Preferences… (`Ctrl+,`, i.e. Cmd+,), Services, Hide (`Ctrl+H`), Hide Others,
  Show All and Quit (`Ctrl+Q`) (`menu_osx.cpp:438-495`).
- `MacInstallMenuBar` hides the Apple menu's About and Preferences items when the menubar has no item with those
  ids, and copies their labels when it does (`menu_osx.cpp:566-600`).

**`OSXGetAppleMenu()`** (`menu.h:411-421`): "You shouldn't remove any items from it, but it is safe to insert extra
menu items or submenus into it."

**Quit** [source]. An unhandled `wxID_EXIT` falls through to `DoDefault()`, which calls
`wxTheApp->ExitMainLoop()` without sending a close event (`src/osx/cocoa/menuitem.mm:329-332`), so close and save
handlers never run.

**Stock edit ids** [source]. `wxID_CUT/COPY/PASTE/CLEAR/SELECTALL` map to the native selectors `cut:`, `copy:`,
`paste:`, `delete:` and `selectAll:` (`menuitem.mm:34-45`). They travel through the Cocoa responder chain, so the
focused native text field consumes them. `wxID_UNDO`/`wxID_REDO` are *not* mapped (`#if 0`: "we don't have
NSUndoManager support").

**Window and Help menus.** A Window menu (Minimize/Zoom/Bring All to Front) is created automatically unless
`wxMenuBar::SetAutoWindowMenu(false)` is called (`include/wx/osx/menu.h:158`; `src/osx/cocoa/menu.mm:283-310`).
The Help menu is found by its title: `wxApp::s_macHelpMenuTitleName` (default `"&Help"`, `app.cpp:70`), else wx's
own translation `_("&Help")` (`menu.mm:213-231`).

**Frames without a menubar.** `MacSetCommonMenuBar` sets "the menubar displayed when the app is running without
any frames open" (`menu.h:386-397`). On activation, a frame installs its own menubar, or, if it has none,
`wxTheApp->GetTopWindow()`'s, inside `wxFrame::OnActivate` (`src/osx/carbon/frame.cpp:202-228`). That handler is in
the static event table, so a non-skipping dynamic `wxEVT_ACTIVATE` handler on a frame breaks menubar switching on
macOS. On MSW it breaks `wxTopLevelWindowMSW::OnActivate`'s focus restore in the same way.

**Pitfalls.**
- **Rule:** Bind `wxID_EXIT` on the Apple menu and call `Close()`.
  ```cpp
  // Right (MainFrame menubar init)
  if (wxMenu* apple_menu = m_menubar->OSXGetAppleMenu())
      apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
  ```
  Cite: `MainFrame` menubar init ("quit command doesn't emit window close events", wx trac #18328).
- **Rule:** Only insert into `OSXGetAppleMenu()`. Never `Delete`/`Remove` its items.
- **Rule:** Always `event.Skip()` in a frame's `wxEVT_ACTIVATE` handler. Dynamic handlers run before the static
  table (`docs/doxygen/overviews/eventhandling.h:474-482`).
- **Rule:** Don't give app-specific commands stock edit ids on macOS. A focused native text field would take
  `copy:`/`paste:`, and Orca's plater copy and paste must not depend on the responder chain.

## 16. Orca menus

**Platform split** (`MainFrame`).
- macOS gets a native `wxMenuBar` (`MainFrame::init_menubar_as_editor`, `SetMenuBar(m_menubar)`).
- Windows and Linux have no menubar. `BBLTopbar` (a `wxAuiToolBar` serving as the custom title bar) owns
  `m_top_menu`, `m_calib_menu` and the file menu, and shows them with
  `GetParent()->PopupMenu(menu, wxPoint(toolRect.GetLeft(), GetSize().GetHeight() - 2))`. The invoking window is
  therefore the topbar's parent (MainFrame). Accelerators in these labels are display-only (§13). Keys are
  dispatched through Orca's shortcut registry instead (`references/mouse-keyboard-focus.md`). The
  `m_skip_popup_*` flags implement "click the tool again to close", and the `wxEVT_MENU_CLOSE` they depend on
  must be bound per §14.
- `MainFrame::init_menubar_as_editor` builds the File, Edit, View and Help menus once, then attaches them per
  platform: `m_topbar->SetFileMenu`/`AddDropDownSubMenu` under `#ifndef __APPLE__`, `m_menubar->Append` on
  macOS. The Calibration menu and the app-level items (Preferences, About) are built separately in each branch
  (`m_topbar->GetCalibMenu()`/`GetTopMenu()` versus a `calib_menu` and `OSXGetAppleMenu()`). A new item in those
  menus goes into both branches.

**macOS Apple menu** (`MainFrame` menubar init).
- Orca inserts its own About (`wxID_ANY`, position 0) and Preferences (`append_shortcut_item` with
  `Shortcut::Preferences`, position 1) into `OSXGetAppleMenu()`. No `wxID_ABOUT`/`wxID_PREFERENCES` item exists, so
  `MacInstallMenuBar` hides wx's built-in About and Preferences entries.
- The File menu's Quit uses `wxID_EXIT`, so wx moves it to the Apple menu. The Apple-menu `wxID_EXIT` binding
  calls `Close()` (§15).
- Cmd+H/M/Q and Ctrl+Cmd+F are also handled in `MainFrame`'s `wxEVT_CHAR_HOOK`
  (`references/mouse-keyboard-focus.md`).
- The menus use no stock edit ids.

**Shortcut labels.** `MainFrame::shortcut_label(label, shortcut, accelerator)` and
`MainFrame::append_shortcut_item` take the key text from `ShortcutRegistry`.
- With `accelerator == true` the key follows `"\t"` and becomes a real accelerator (`shortcuts.accelerator()`).
- Otherwise the text is display-only and joined with `sep`, which is `" - "` on macOS and `"\t"` elsewhere. On
  macOS a `"\t"` would turn it into a live key equivalent. In Win/Linux topbar popup menus accelerators are
  display-only anyway.
- `append_shortcut_item` records the item in `m_shortcut_menu_items`, and `update_shortcut_labels()` rewrites the
  labels with `SetItemLabel` after the user rebinds a key. Menu text for a registry shortcut always goes through
  these helpers, never through a hard-coded `"\tCtrl+X"`.

**`Plater::PopupMenu(menu, pos)`** (`Plater.cpp`).
- It calls `wxGetApp().mainframe->PopupMenu(menu, pos)`. The invoking window is MainFrame, so **`pos` is in
  MainFrame client coordinates**. Callers convert, for example the Sidebar filament edit button:
  ```cpp
  wxPoint pt{0, edit_btn->GetSize().GetHeight() + FromDIP(2)};
  pt = wxGetApp().mainframe->ScreenToClient(edit_btn->ClientToScreen(pt));
  p->plater->PopupMenu(menu, pt.x, pt.y);
  ```
- It holds a `SuppressBackgroundProcessingUpdate` so tracking the menu doesn't wake reslicing. It sets
  `m_tracking_popup_menu` so slicing errors are buffered in `m_tracking_popup_menu_error_message`, then shows them
  afterwards through `wxTheApp->CallAfter` (after the menu command has been processed).
- Use it for plater, 3D canvas and object-list context menus (`Plater::priv::show_right_click_menu`). Elsewhere
  (a sidebar-local `wxMenu menu; PopupMenu(&menu);`, `BBLTopbar`, dialogs) a raw `PopupMenu` on the hosting window
  is correct.

**`MenuFactory`** (`GUI_Factories.{hpp,cpp}`).
- It builds and caches the right-click menus: `default_menu()`, `object_menu()`, `sla_object_menu()`,
  `part_menu()`, `text_part_menu()`, `svg_part_menu()`, `instance_menu()`, `layer_menu()`,
  `multi_selection_menu()`, `plate_menu()`, `assemble_object_menu()`, `assemble_part_menu()`,
  `assemble_multi_selection_menu()`. `msw_rescale()` and `sys_color_changed()` regenerate the icons (the static
  `sys_color_changed(wxMenuBar*)` handles the macOS menubar).
- It is initialized with MainFrame (`menus.init(main_frame)` in the Plater setup). The `parent` it passes to the
  helpers is therefore the invoking window of `Plater::PopupMenu`. That is what makes the helpers'
  `wxEVT_UPDATE_UI` enable callbacks fire (§14).
- The menus are by-value members (`MenuWithSeparators m_object_menu`, …, `wxMenu m_filament_action_menu`), which
  is legal for popup menus. Their submenus are heap-allocated.
- Items that change per show are rebuilt by first removing the old ones: `FindItem(label)` → `Destroy(id)`
  (`MenuFactory::append_menu_item_change_extruder`).

**Helpers** (`wxExtensions.{hpp,cpp}`).
```cpp
wxMenuItem* append_menu_item(wxMenu* menu, int id, const wxString& string, const wxString& description,
    std::function<void(wxCommandEvent&)> cb, const std::string& icon = "",          // or const wxBitmap& icon
    wxEvtHandler* event_handler = nullptr, std::function<bool()> const cb_condition = []{ return true; },
    wxWindow* parent = nullptr, int insert_pos = wxNOT_FOUND);
wxMenuItem* append_submenu(wxMenu* menu, wxMenu* sub_menu, int id, const wxString& string, const wxString& description,
    const std::string& icon = "", std::function<bool()> const cb_condition = ..., wxWindow* parent = nullptr, int insert_pos = wxNOT_FOUND);
wxMenuItem* append_menu_radio_item(wxMenu*, int id, const wxString&, const wxString&, std::function<void(wxCommandEvent&)> cb, wxEvtHandler*);
wxMenuItem* append_menu_check_item(wxMenu*, int id, const wxString&, const wxString&, std::function<void(wxCommandEvent&)> cb,
    wxEvtHandler*, std::function<bool()> const enable_condition = ..., std::function<bool()> const check_condition = ..., wxWindow* parent = nullptr);
```
- Icons are SVG **names** (rasterized by `create_menu_bitmap`: 16 px, no window, "FIXME: pass window ptr") or a
  ready `wxBitmap`. They are plain `wxBitmap`s, not `wxBitmapBundle`s. The bitmap is set before `Append`/`Insert`,
  so GTK image items work. Off GTK the icon name is recorded per id in `msw_menuitem_bitmaps`, so
  `msw_rescale_menu` (MSW) and `enable_menu_item` (macOS) can re-create it. On Windows a menu icon's dark variant
  follows `check_dark_mode()` (the system appearance), not `wxGetApp().dark_mode()` (`create_scaled_bitmap` with
  `menu_bitmap = true`).
- `wxID_ANY` becomes `wxNewId()` inside the helpers, not an auto id.
- `cb` is bound on the menu. On MSW only, a non-null `event_handler` that is not the menu receives the
  `wxEVT_MENU` binding instead. Other ports ignore `event_handler`. Binding on the menu works on every port, so
  pass `nullptr`.
- A non-null `parent` binds a `wxEVT_UPDATE_UI` handler for that id on `parent` that is never unbound. It calls
  `enable_menu_item` (`evt.Enable(cb_condition())`), which on macOS also re-applies the icon, grayscale when
  disabled, on every validation (§14). `append_menu_check_item` binds `Enable` + `Check` the same way.
  `append_menu_radio_item` takes no conditions.
- Without `parent`, `cb_condition` is ignored: no enable callback is wired.

**Pitfalls.**
- **Rule:** Build `MenuFactory` menus once, and pass `parent` only for persistent items.
  **Why:** Every call with `parent` adds one more `UPDATE_UI` binding on MainFrame, and every helper call (with or
  without `parent`) uses up a `wxNewId()`. A menu rebuilt per show accumulates handlers that run on every menu
  validation (on macOS, on every key equivalent), and ids that grow toward the MSW 16-bit limit. For a per-show
  menu, append plain `wxID_ANY` items (auto ids, recycled on MSW), bind them on that menu by `GetId()`, and set
  the enabled state before showing it.
  ```cpp
  // Wrong: per-show rebuild with parent → one more MainFrame UPDATE_UI handler and one more wxNewId each time
  for (auto& f : filaments) append_menu_item(menu, wxID_ANY, f.name, "", cb, "", nullptr, cond, wxGetApp().mainframe);
  // Right: per-show items with auto ids, bound on the (per-show) menu, enabled directly
  for (auto& f : filaments) {
      wxMenuItem* it = menu->Append(wxID_ANY, f.name);
      menu->Bind(wxEVT_MENU, cb, it->GetId());
      it->Enable(cond());
  }
  ```
- **Rule:** Show plater, canvas and object-list menus with `Plater::PopupMenu` and MainFrame-client coordinates.
  **Why:** `Plater::PopupMenu` suppresses background-processing updates while the menu tracks and defers
  slicing error dialogs until the command has run; a raw `PopupMenu` does neither. Its invoking window is
  MainFrame, so coordinates computed for another window put the menu in the wrong place. The `MenuFactory`
  enable callbacks are bound on MainFrame: they fire for a menu popped from MainFrame or from a descendant whose
  `UPDATE_UI` propagates up to it, never from inside a dialog or (on MSW and macOS) a popup
  (`wxWS_EX_BLOCK_EVENTS`).
  ```cpp
  // Wrong: button-relative coordinates passed to the MainFrame-based wrapper
  wxGetApp().plater()->PopupMenu(menu, wxPoint(0, edit_btn->GetSize().y));
  // Right
  wxPoint pt = wxGetApp().mainframe->ScreenToClient(edit_btn->ClientToScreen({0, edit_btn->GetSize().y}));
  wxGetApp().plater()->PopupMenu(menu, pt);
  ```
- **Rule:** Don't pass `event_handler` to the helpers to route commands. It only works on MSW. Bind on the menu,
  or on the invoking window and its ancestors.
