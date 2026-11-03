# Sizers and layout

How wx 3.3.2 computes window sizes and lays out children, and how OrcaSlicer builds layouts on top of
that: sizers and flags, best/min size, the fitting functions (`SetSizerAndFit`, `SetSizeHints`, `Fit`,
`Layout`, `FitInside`) per platform, show/hide relayout, Freeze/Thaw, scrolled windows, `wxStaticText`
wrapping, layout on DPI change, and Orca's layout idioms. Read it when building or reviewing any dialog
or panel layout, or when debugging a window that is collapsed, clipped, too large or not re-laid out.

Contents: [Rules](#rules) · [Build facts](#build-facts-that-change-how-layout-bugs-present) ·
[Size model](#the-size-model-best-min-effective-min-initial-virtual) ·
[Fitting functions](#fitting-functions-setsizer-setsizerandfit-setsizehints-fit-layout) ·
[Re-layout](#re-layout-after-content-or-visibility-changes) · [Adding items](#adding-items-proportion-flags-wxsizerflags) ·
[Ownership](#ownership-and-removal) · [Specific sizers](#specific-sizers) ·
[Size events](#wxevt_size-handlers) · [Freeze/Thaw](#freeze--thaw) ·
[Scrolled windows](#scrolled-windows) · [Static text wrapping](#wxstatictext-wrapping-and-ellipsizing) ·
[Layout on DPI change](#layout-on-dpi-change) · [Platform summary](#platform-summary) ·
[Orca idioms](#orcaslicer-layout-idioms-and-spacing-conventions)

## Rules

1. On a top-level window, attach the finished sizer with `SetSizerAndFit(sizer)`. If `SetSizer` must
   come before the content exists, call `GetSizer()->SetSizeHints(this)` once the content is built, and
   again after rebuilding content. Never rely on `Fit()` alone. → [Fitting](#fitting-functions-setsizer-setsizerandfit-setsizehints-fit-layout)
2. Child panels use plain `SetSizer`; never `SetSizerAndFit` or `sizer->SetSizeHints(panel)` on a
   non-top-level window (it pins the panel's min size). → [Fitting](#fitting-functions-setsizer-setsizerandfit-setsizehints-fit-layout)
3. Keep a dialog's content within any `SetMaxSize` (cap a scrolled region), or the min > max hints are
   silently dropped. → [Fitting](#fitting-functions-setsizer-setsizerandfit-setsizehints-fit-layout)
4. Proportion is the second argument: `Add(w, 0, wxEXPAND | wxALL, FromDIP(n))`, never `Add(w, wxEXPAND)`.
   → [Adding items](#adding-items-proportion-flags-wxsizerflags)
5. In a box sizer, alignment and `wxEXPAND` act only across the sizer's direction, and `wxEXPAND`
   overrides alignment; contradictory flags are silently ignored in Orca. → [Adding items](#adding-items-proportion-flags-wxsizerflags)
6. Give `proportion > 0` only to items that must stretch; proportions inflate the sizer's min size.
   → [Adding items](#adding-items-proportion-flags-wxsizerflags)
7. A window managed by a sizer is a child of the sizer's containing window (or of the `wxStaticBox` of a
   `wxStaticBoxSizer`), and sits in exactly one sizer; `Detach` before re-adding.
   → [Adding items](#adding-items-proportion-flags-wxsizerflags), [Ownership](#ownership-and-removal)
8. When rebuilding content, destroy the old windows; deleting, clearing or replacing a sizer leaves them
   alive and visible. → [Ownership](#ownership-and-removal)
9. After changing content or visibility, `Layout()` the nearest ancestor whose allocation must change;
   resize a top-level window with `GetSizer()->SetSizeHints(tlw)`. `Hide()` is always followed by a
   `Layout()`. → [Re-layout](#re-layout-after-content-or-visibility-changes)
10. A `wxEVT_SIZE` handler calls `Skip()` and never `SetSize`s its own window. → [Size events](#wxevt_size-handlers)
11. `Freeze()`/`Thaw()` must balance on every path; use `wxWindowUpdateLocker`. → [Freeze/Thaw](#freeze--thaw)
12. A scrolled window needs a non-zero `SetScrollRate`, `FitInside()` after its content changes, and an
   explicit min size or proportion + `wxEXPAND` in its parent. → [Scrolled windows](#scrolled-windows)
13. To re-wrap a `wxStaticText` after `SetLabel`, call `Wrap(-1); Wrap(w);`, or use `Label` with
   `LB_AUTO_WRAP`; use `Label` for CJK text. → [Wrapping](#wxstatictext-wrapping-and-ellipsizing)
14. Never compute or commit a size from a width that has not been laid out yet. → [Wrapping](#wxstatictext-wrapping-and-ellipsizing)
15. Give wrapping labels a fixed width and `-1` height, never a fixed height. → [Wrapping](#wxstatictext-wrapping-and-ellipsizing)
16. In `on_dpi_changed`, re-apply what wx does not rescale, then resize with
   `GetSizer()->SetSizeHints(this)`; never multiply existing min sizes or borders by a DPI ratio.
   → [Layout on DPI change](#layout-on-dpi-change)
17. A custom widget reports its size through its min size (Orca widgets) or `DoGetBestClientSize()`, and
   invalidates it when content, label or font change. → [Size model](#the-size-model-best-min-effective-min-initial-virtual)
18. Pixel values are `FromDIP(n)` or `n * em_unit()`; borders are explicit `FromDIP(n)`; dialog button
   rows are `DialogButtons`. → [Orca idioms](#orcaslicer-layout-idioms-and-spacing-conventions)

## Build facts that change how layout bugs present

- **Every wx layout assert is silent in Orca.** wx is built with `wxBUILD_DEBUG_LEVEL=0` and
  `libslic3r_gui` with `wxDEBUG_LEVEL=0`, so `wxASSERT`/`wxFAIL` compile to nothing and `wxCHECK_*` return
  early without a message (`include/wx/debug.h:314-324, 342-382`). The checks that would flag layout
  mistakes in a debug wx therefore do nothing, and the review has to catch them by reading the code:
  - flag consistency in box sizers (`wxBoxSizer::DoInsert`, `src/common/sizer.cpp:2295`);
  - "window managed by the sizer must have the containing window as parent" (`wxSizer::DoInsert`,
    `sizer.cpp:904, 947`);
  - mixed parents in one `wxStaticBoxSizer` (`wxStaticBoxSizer::RepositionChildren`, `sizer.cpp:2888`);
  - duplicate or out-of-range `AddGrowableCol/Row` (`sizer.cpp:2235, 2250`);
  - `Thaw()` without `Freeze()` (`src/common/wincmn.cpp:1247`);
  - a window added to a second sizer (`wxWindowBase::SetContainingSizer`, `wincmn.cpp:2421`): the
    `wxCHECK_RET` refuses the bookkeeping, but the item is still inserted;
  - top-level `SetSizeHints` with min > max (`wxWindowBase::DoSetSizeHints`, `wincmn.cpp:1041`): the
    whole call is dropped.
- **Linux is GTK3** (`DEP_WX_GTK3` ON, `SLIC3R_GTK` "3"); GTK2 is an opt-out build. GTK-only facts below
  are GTK3 unless marked.
- **DIP model.** `wxHAS_DPI_INDEPENDENT_PIXELS` is defined for wxGTK3 and wxOSX
  (`include/wx/features.h:115`): logical pixels are DIPs and `FromDIP` is the identity. On MSW `FromDIP`
  scales by the window's DPI (`wincmn.cpp` `wxWindowBase::FromDIP`); GTK2 takes the same conversion path,
  but its display PPI is always 96, so `FromDIP` is the identity there too **[source]**. DIP conversion
  itself: see `references/dpi-bitmaps-fonts.md`.

## The size model: best, min, effective min, initial, virtual

**Contract** (`docs/doxygen/overviews/windowsizing.h:23-100`):
- *Best size* is derived from content. *Min size* is "normally explicitly set by the programmer"; most
  controls also take it from a non-default ctor size. *Initial size* is the ctor size; a partly specified
  size such as `wxSize(150, -1)` is completed from the best size. *Virtual size* is the scrollable extent.
- `GetEffectiveMinSize()` merges the best size into the min size: "This is the value used by sizers to
  determine the appropriate amount of space to allocate for the widget" (`interface/wx/window.h:1393-1402`). It is the min size with unspecified components filled
  from the best size (`wincmn.cpp:868`). Once `SetMinSize(wxSize(w, h))` sets both components, the
  content no longer affects the sizer allocation; pass `-1` for the component that must follow content.
- "The best size respects the minimal and maximal size explicitly set for the window" (`window.h:1342-1350`;
  `wincmn.cpp:879`: raised to min, lowered to max). It is **cached only when the window has no sizer**
  **[source]** (`wincmn.cpp:881`). Containers with sizers recompute every time; leaf controls and custom
  widgets return the cache until `InvalidateBestSize()` (`window.h:1645-1651`; the cache is documented for
  `DoGetBestClientSize`, `window.h:4432-4435`).
- `InvalidateBestSize()` also invalidates the parent chain, stopping at a top-level window **[source]**
  (`wincmn.cpp:636`). It does not lay anything out (see [Re-layout](#re-layout-after-content-or-visibility-changes)).
- The default `DoGetBestSize()` uses the sizer's min size; without a sizer, the bounding box of visible
  children; with no children, the min size or (1,1) (`wincmn.cpp:649`).
- `SetInitialSize(size)` sets the min size to `size`, merges it with the best size and resizes
  (`window.h:1742-1757`, `wincmn.cpp:937`). A ctor size therefore becomes the min size: a fixed height
  pins the minimum height.
- `SetMinSize` "doesn't prevent the program from making the window explicitly smaller … by calling
  SetSize(), it just ensures that it won't become smaller than this size during the automatic layout"
  (`window.h:1808-1815`). Top-level windows are the documented exception: their size hints also stop the
  program's own `SetSize()` (`interface/wx/toplevel.h:576-603`). **[source]** On GTK, top-level windows and
  `wxPopupWindow` clamp `SetSize` to min/max (`src/gtk/toplevel.cpp:1365` `ConstrainSize`,
  `src/gtk/popupwin.cpp:171`).
- On a top-level window, `SetMinSize`/`SetMaxSize` go through `SetSizeHints(min, max)`
  (`src/common/toplvcmn.cpp:197-205`), so a min larger than the current max (or the reverse) is dropped
  silently.
- `wxWindow::SetSizeHints` on a non-top-level window "is discouraged. Please use SetMinSize() and
  SetMaxSize() instead" (`window.h:1885-1891`).
- **Height-for-width (3.3.2).** `GetMinSizeFromKnownDirection(direction, size, availableOtherDir)`
  (`window.h:1404-1444`) lets a control report its min size once the layout fixes one dimension; box and
  flex-grid sizers feed the known width to their items during layout, and `wxSizer::CalcMinSizeFromKnownDirection`
  is the sizer side (`interface/wx/sizer.h:339-377`). `InformFirstDirection` is the deprecated
  compatibility path. `wxST_WRAP` and `wxWrapSizer` rely on this negotiation. `DoGetBestClientHeight()`/
  `DoGetBestClientWidth()` are "not used by wxWidgets yet" (`window.h:4453-4455`): overriding them changes
  no sizer layout.

**Writing a custom control (wx way).** Override `DoGetBestClientSize()` and let `DoGetBestSize()` add the
borders (`windowsizing.h:46-51`, `window.h:4421-4441`); the default returns `wxDefaultSize` and the best
size is then arbitrary. Call `SetInitialSize()` at the end of `Create()`, and `InvalidateBestSize()`
whenever content, label or font change.

**OrcaSlicer.** The `StaticBox`-based widgets (`Button`, `TextInput`, `SpinInput`, `ComboBox`) do not
override `DoGetBestClientSize`. Each has (or inherits) a `messureSize()` that measures its content and calls
`wxWindow::SetMinSize(...)`, and they override `SetMinSize` to merge a caller's request with the content
size: `Button::SetMinSize` stores the request (its height overrides, its width is a floor), and
`TextInput::SetMinSize` fills a `-1` height from the current size. Re-measuring happens inside their
`SetLabel`/`SetFont`/`Rescale` overrides, so callers only re-lay out the parent. A min size is honoured
identically by every sizer and has no cache to invalidate, so this design never needs `InvalidateBestSize()`.
Writing a new Orca widget: see `references/painting-custom-widgets.md`.

For owner-drawn text whose height depends on width, follow `WikiLabel` (`src/slic3r/GUI/Preferences.cpp`):
re-wrap on width change, then `SetMinSize(wxSize(-1, totalH))` + `InvalidateBestSize()`; override
`DoGetBestSize()`; guard `GetCharHeight()` against 0 before the window is realized on GTK (Orca comment).

## Fitting functions: SetSizer, SetSizerAndFit, SetSizeHints, Fit, Layout

**Contract.**
- `SetSizer(s, deleteOld = true)`: "The window will then own the object, and will take care of its
  deletion"; `deleteOld` deletes a previous sizer (pass `false` only if you delete it yourself). It "will
  also call SetAutoLayout() implicitly with true … so that the sizer will be effectively used to layout
  the window children whenever it is resized" (`window.h:3693-3715`).
- `SetSizerAndFit(s)` "calls SetSizer() and then wxSizer::SetSizeHints() which sets the initial window
  size to the size needed to accommodate all sizer elements and sets the minimal size to the same size,
  this preventing the user from resizing this window to be less than this minimal size (if it's a
  top-level window …)" (`window.h:3717-3728`; `wincmn.cpp:2414`).
- `wxSizer::SetSizeHints(win)` is documented as "first calls Fit() and then
  wxTopLevelWindow::SetSizeHints() … It does nothing in normal windows or controls", with the idiom of
  calling the panel's sizer's `SetSizeHints(frame)` to size the frame to fit the panel
  (`interface/wx/sizer.h:937-970`). **[source]** The doc is outdated: it calls
  `WXSetInitialFittingClientSize(wxSIZE_SET_CURRENT | wxSIZE_SET_MIN)` (`sizer.cpp:1284`), which calls
  `SetMinClientSize()` then `SetClientSize()` on **any** window (`wincmn.cpp:974`). On a child panel it
  freezes the panel's min size at its current content.
- `wxSizer::Fit(win)` resizes the window so its client area matches the sizer's min size
  (`sizer.h:454-463`; `sizer.cpp:1243`: `wxSIZE_SET_CURRENT` only).
- `wxWindow::Fit()` "only changes the current window size and doesn't change its minimal size"
  (`window.h:1066-1077`); it is `SetSize(GetBestSize())` (`wincmn.cpp:625`), except that a top-level window
  without a sizer and with exactly one child sets its client size to that child's best size
  (`toplvcmn.cpp:508`).
- `Layout()` "doesn't do anything" without a sizer unless the window is top-level; it "is called
  automatically when the window size changes if it has the associated sizer" (`window.h:3753-3769`). It
  positions children inside the current virtual size (`wincmn.cpp:2469`). A top-level window without a
  sizer and with exactly one child resizes that child to fill the client area (`toplvcmn.cpp:475`).

| Call | Sets current size | Sets min size | Clamped to display (TLW) | GTK3: replayed at `Show()` if called while hidden |
|---|---|---|---|---|
| `win->SetSizer(s)` | no | no | – | – |
| `win->SetSizerAndFit(s)` | yes (client) | yes (client) | yes | yes |
| `s->SetSizeHints(win)` | yes | yes | yes | yes (with the window's own sizer) |
| `s->Fit(win)` | yes | **no** | yes | yes |
| `win->Fit()` | yes | **no** | **no** | **no** |
| `win->Layout()` | no (positions children) | no | – | – |

**[source]** `ComputeFittingClientSize` (`sizer.cpp:1194`) clamps a **top-level** window only to the
display client area; the doc's "maximum window size if previously set" applies to child windows only. If
a dialog's `SetMaxSize` is smaller than its content, `SetSizeHints` computes min > max,
`DoSetSizeHints` drops the hints, and on GTK the following `SetClientSize` is clamped to the max: the
dialog ends up with no enforced minimum.

**GTK3 hidden-window replay [source]** (`src/gtk/toplevel.cpp`). `wxTopLevelWindowGTK::WXSetInitialFittingClientSize`
(`:1726`) applies the fit at once and, if the window is still hidden, stores the flags because the "GTK
style cache hasn't been updated yet"; `Show()` (`:1262-1266`) and `GTKDoAfterShow()` (`:1687`) replay them
through `GTKUpdateClientSizeIfNecessary()` (`:1702`), which re-fits with the window's **own** sizer (a
`panel_sizer->SetSizeHints(frame)` on a sizer-less frame is not replayed). An explicit `SetMinSize()`
cancels the pending minimum (`:1715-1723`); an explicit size change cancels the pending current size but
keeps the pending minimum (`:1384-1396`). `wxWindow::Fit()` never takes this path, so a hint-less GTK3
dialog keeps whatever was measured with the stale style cache.

**GTK size hints [source].** WM min/max geometry hints are set only for `wxRESIZE_BORDER` windows; a
non-resizable dialog is sized through `gtk_widget_set_size_request` in `DoSetSize`
(`toplevel.cpp:1485-1503, 1399-1407`). Either way the min comes from `SetSizeHints`.

**Usage — the three canonical shapes:**
```cpp
// (a) content built before the sizer is attached
SetSizerAndFit(main_sizer);                // size + min, display-clamped, GTK3-safe
CenterOnParent();

// (b) sizer attached early, content added later (MsgDialog::finalize)
SetSizer(main_sizer);  /* ... add content ... */
GetSizer()->SetSizeHints(this); Layout(); CenterOnParent();

// (c) content changed after the dialog exists (PrinterPartsDialog::Show)
/* ... show/hide/add rows ... */
GetSizer()->SetSizeHints(this);            // not Fit(): Fit() leaves the old minimum
```
`SetSizeHints` already sets the current size, so a `Fit()` before it is redundant, and a `Fit()` after it
only re-applies the best size without the display clamp.

**OrcaSlicer.** The project rule (`AGENTS.md`): "Always use `SetSizerAndFit(sizer)` instead of
`SetSizer(sizer)` on top level window. Unless `SetSizer` must be called before the full layout is built,
call `sizer->SetSizeHints(window)` afterwards in this case." Models: `CloneDialog` ctor (shape a);
`MsgDialog` (its ctor calls `SetSizer(main_sizer)`, subclasses add content, `MsgDialog::finalize` runs
`GetSizer()->SetSizeHints(this); Layout(); Fit(); CenterOnParent(); wxGetApp().UpdateDlgDarkUI(this);`);
`NetworkPluginDownloadDialog` ctor (`main_sizer->SetSizeHints(this)` after building the mode-specific UI);
`PrinterPartsDialog::Show` (shows/hides its panels and rows, then `GetSizer()->SetSizeHints(this)` before
`DPIDialog::Show`); the calibration dialogs in `calib_dlg.cpp` (`Layout(); Fit(); v_sizer->SetSizeHints(this);`).

**Pitfalls**
- **Rule:** Top-level dialogs get their minimum from `SetSizerAndFit` or `sizer->SetSizeHints(this)`,
  never from `Fit()` alone.
  **Why:** `Fit()` sets no minimum, is not display-clamped and is not replayed at GTK3 show; a dialog
  whose minimum was never propagated from its children renders collapsed or mis-sized on GTK3 and can be
  shrunk below its content anywhere.
  ```cpp
  // Wrong
  SetSizer(main_sizer); Layout(); main_sizer->Fit(this);
  // Right (layout fully built)
  SetSizerAndFit(main_sizer); Layout();
  // Right (sizer set early, content built later, e.g. MsgDialog::finalize; its trailing Fit() is redundant)
  GetSizer()->SetSizeHints(this); Layout(); Fit();
  ```
  Cite: 5ede9711f5 (`MsgDialog::finalize`, `UnsavedChangesDialog::build`, `PrinterPartsDialog::Show`,
  `CloneDialog` ctor and other dialog ctors; added the `AGENTS.md` rule).
- **Rule:** Lock in the minimum in the constructor, before the dialog can receive iconize, refresh or DPI
  events.
  **Why:** Per f760f4e462's message, on wxGTK iconizing the main window while a hint-less dialog is open
  re-ran `Fit()` with transient zero-sized children and collapsed the dialog (OK button clipped); the
  WM then honoured the small geometry, so the user could not resize it back. The trigger chain is not
  visible in wx source; what is verified is that `Fit()` never updates the minimum and that a minimized
  top-level window reports a (0,0) client size (`window.h:1375-1376`; GTK `toplevel.cpp:1463`).
  ```cpp
  Layout(); Fit();                                  // Wrong: no enforced minimum
  Layout(); Fit(); v_sizer->SetSizeHints(this);     // Right (the Fit() is redundant)
  ```
  Cite: f760f4e462 (`FlowRateCalibrationDialog` ctor, `calib_dlg.cpp`).
- **Rule:** Child panels use `SetSizer`, not `SetSizerAndFit`.
  **Why:** `SetSizeHints` pins the panel's min client size at its current content **[source]**. A fully
  specified min size replaces the best size in `GetEffectiveMinSize()`, so the parent sizer stops tracking
  the panel's content: it no longer shrinks when content is removed or translations get shorter, nor grows
  when content is added.
  ```cpp
  panel->SetSizerAndFit(s);   // Wrong
  panel->SetSizer(s);         // Right
  ```
  Cite: `wincmn.cpp:974` `WXSetInitialFittingClientSize`.
- **Rule:** After content added to a shown dialog, call `GetSizer()->SetSizeHints(this)`, not `Fit()`.
  **Why:** `Fit()` grows the window but keeps the old minimum, so the WM can shrink it below the new
  content; after removing content `Fit()` cannot shrink below the stale minimum either.
  ```cpp
  add_extra_row(); Fit();                       // Wrong: stale minimum
  add_extra_row(); GetSizer()->SetSizeHints(this);   // Right
  ```
  Cite: `PrinterPartsDialog::Show` (re-hints after showing/hiding its rows).
- **Rule:** Keep content within a dialog's `SetMaxSize` by capping a scrolled region.
  **Why:** min > max hints are silently rejected (`wincmn.cpp:1041`) and the dialog has no minimum.
  Cite: `MsgDialog` (`MSG_DLG_MAX_SIZE` caps height only, "ban setting the maximum width value") with
  `add_msg_content` capping the scrolled text.

## Re-layout after content or visibility changes

**Contract.**
- `wxSizer::Layout()` recomputes min sizes and repositions items inside the sizer's **current**
  rectangle (`sizer.h:705-710`, `sizer.cpp:1272`); `wxWindow::Layout()` does the same within the window's
  size. Neither propagates upward. If a change alters a container's min size, `Layout()` the nearest
  ancestor whose allocation must change; if the top-level window itself must grow or shrink, call
  `GetSizer()->SetSizeHints(tlw)`. Manual `Layout()` is needed only after a content change that does not
  come with a size change; a resize lays out automatically.
- "To make a sizer item disappear, use Hide() followed by Layout()" (`sizer.h:569-603`). `wxWindow::Show`
  only flips the flag; the ports do not re-lay out the parent (`wincmn.cpp:1128`). A window item is shown
  exactly when the window `IsShown()` (`sizer.cpp:863`) unless it carries `wxRESERVE_SPACE_EVEN_IF_HIDDEN`
  (next item), so `win->Hide()` and `sizer->Hide(win)` are
  equivalent for layout. `sizer->Show(win, …)` with `recursive = false` returns false and does nothing when
  `win` sits in a nested sizer. Hiding is honoured only by `wxBoxSizer` and `wxFlexGridSizer`
  (`docs/doxygen/overviews/sizer.h:134`).
- `wxRESERVE_SPACE_EVEN_IF_HIDDEN` (`wxSizerFlags::ReserveSpaceEvenIfHidden()`) makes `wxSizerItem::IsShown()`
  return true whatever the window's state (`sizer.cpp:863-865`; doc `interface/wx/sizer.h:94-99, 1343-1345`).
  The hidden item keeps its min size and position, since `wxBoxSizer::CalcMin` and `RepositionChildren` skip
  only `!IsShown()` items (`sizer.cpp:2745, 2428`), and its `wxFlexGridSizer` row or column does not collapse
  (`sizer.cpp:1957`): showing or hiding it moves no neighbour and resizes no parent. `sizer->IsShown(win)`
  then reports true for a hidden window (`sizer.cpp:1562`); ask `win->IsShown()`. On a sizer item the flag
  reserves only the nested sizer's min size, which still skips that sizer's hidden children
  (`wxSizerItem::CalcMin`, `sizer.cpp:680-682`), so a nested sizer whose children are all hidden collapses to
  the item's border (or the nested sizer's `SetMinSize`); put the flag on the window items.
- `wxStaticText::SetLabel` resizes the control itself (unless `wxST_NO_AUTORESIZE`) but never re-lays out
  the parent (`src/common/stattextcmn.cpp` `AutoResizeIfNecessary`).
- `SendSizeEvent()`: "if the frame is using either sizers or constraints … it is enough to call
  wxWindow::Layout() directly and this function should not be used in this case" (`window.h:1668-1686`).
  `PostSizeEvent()` queues it instead (`window.h:1653-1658`), which defers a relayout past the current
  handler and the GTK allocation.

**Usage.**
```cpp
row->Show(enabled);                 // or sizer->Show(row_sizer, enabled)
GetParent()->Layout();              // or the ancestor whose min size changed
// top-level window must change size too:
GetSizer()->SetSizeHints(this);
```

**OrcaSlicer.** Page switching by sizer visibility: the `wxEVT_TAB_SEL_CHANGED` handler in
`PreferencesDialog` runs `Freeze(); f_sizers[i]->Show(i == selection); Layout(); Thaw();`.
`PreferencesDialog::UpdateSidebarLayout` re-lays out the sidebar inside `Freeze()/Thaw()` and then calls
`plater->PostSizeEvent()` so the plater re-lays out after GTK has allocated. Show/hide driven by hover
must not run inside enter/leave handlers (it re-fires them); see `references/mouse-keyboard-focus.md`.
Reserved space: `KBShortcutsDialog::create_page` adds each editable row's reset button with
`wxALIGN_CENTRE_VERTICAL | wxRESERVE_SPACE_EVEN_IF_HIDDEN`, so `KBShortcutsDialog::apply_bindings` toggles
`reset->Show(is_customized)` without the buttons column changing width.

## Adding items: proportion, flags, wxSizerFlags

**Contract.**
- `Add(win, int proportion = 0, int flag = 0, int border = 0, userData = nullptr)`
  (`interface/wx/sizer.h:186`). Proportion is the **second** argument; `Add(w, wxEXPAND)` passes
  `0x2000` as a proportion.
- Proportion acts only along the sizer's direction; `wxEXPAND` and alignment act only across it. Default
  alignment is left/top.
- **Proportion inflates the min size [source].** `wxBoxSizer::CalcMin` (`sizer.cpp:2728`) sizes the main
  direction as `max_i(min_i / prop_i) × Σprop + Σ(fixed items)`. Two `proportion = 1` items with min
  widths 100 and 300 make the sizer at least 600 wide, which also widens a `SetSizerAndFit` dialog. The
  overview's "half the extra space each" (`overviews/sizer.h:114`) is outdated: 3.x distributes the
  total space by proportion with min-size floors (`wxBoxSizer::RepositionChildren`, `sizer.cpp:2402`).
- Box-sizer flag rules, checked only by compiled-out asserts in `wxBoxSizer::DoInsert` (`sizer.cpp:2295`):
  a vertical box ignores `wxALIGN_BOTTOM` and `wxALIGN_CENTRE_VERTICAL` (unless combined with
  `wxALIGN_CENTRE_HORIZONTAL`, i.e. `wxALIGN_CENTRE`); a horizontal box ignores `wxALIGN_RIGHT` and
  `wxALIGN_CENTRE_HORIZONTAL` (unless with `wxALIGN_CENTRE_VERTICAL`); `wxEXPAND` without `wxSHAPED`
  overrides every alignment. In `wxGridSizer`, `wxEXPAND | wxALIGN_CENTRE_VERTICAL` means "expand
  horizontally, centre vertically". `DisableConsistencyChecks()` (`sizer.h:1609`) is irrelevant in Orca.
- Parent rule: windows managed by a sizer must be children of the sizer's containing window, or of a
  `wxStaticBox` inside it (`sizer.cpp` `CheckExpectedParentIs`); otherwise they are positioned in the
  wrong coordinate space, silently.
- `wxSizerFlags` (`include/wx/sizer.h:40-240`): `Align()`/`Centre()` **replace** all alignment bits,
  while `Left/Right/Top/Bottom/CentreHorizontal/CentreVertical` set one axis (`:60-75`).
  `Border(dir, px)` takes raw pixels and the doc prefers the default border "to avoid too small borders
  … with high DPI" (`interface/wx/sizer.h:1502-1519`). The default border is 6 on GTK and 5 on macOS (not
  scaled); on MSW it is `5 × GetDPIScaleFactor()` of **`wxApp::GetMainTopWindow()`**, not of the window
  being laid out (`include/wx/sizer.h:125-141`, `sizer.cpp:172` **[source]**; doc `sizer.h:1652-1661`).
  `FixedMinSize()` (`wxFIXED_MINSIZE`) copies the window's current size into its min size when added
  (`sizer.cpp:395-409`). `Shaped()` keeps the aspect ratio. `ReserveSpaceEvenIfHidden()` keeps a hidden
  item's space (`wxRESERVE_SPACE_EVEN_IF_HIDDEN`, `sizer.h:1641-1650`; see
  [Re-layout](#re-layout-after-content-or-visibility-changes)).
- `AddSpacer(n)`: in `wxSizer` it adds `n × n` (a whole cell in grid sizers); in `wxBoxSizer` only along
  the main direction. `AddStretchSpacer(p)` is `Add(0, 0, p)` (`sizer.h:300-331`).

**OrcaSlicer.** Orca uses int flags with explicit DIP borders,
`Add(w, 0, wxEXPAND | wxALL, FromDIP(10))`, rather than `wxSizerFlags::Border()` defaults, so spacing is
identical on every port instead of 5/6 px (and main-window DPI on MSW).

**Pitfalls**
- **Rule:** Pass the proportion before the flags.
  ```cpp
  sizer->Add(ctrl, wxEXPAND);                        // Wrong: proportion 0x2000, no flags
  sizer->Add(ctrl, 0, wxEXPAND);                     // Right
  sizer->Add(ctrl, wxSizerFlags(1).Expand().Border(wxALL, FromDIP(5)));  // Right
  ```
- **Rule:** Do not combine `wxEXPAND` with an alignment in a box sizer, and align only across the sizer.
  **Why:** EXPAND wins; a main-direction alignment is ignored. The assert that names the conflict is
  compiled out, so the flag silently does nothing.
  ```cpp
  vbox->Add(x, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);   // Wrong: alignment ignored
  vbox->Add(x, 0, wxALIGN_CENTER_VERTICAL);              // Wrong: main-direction alignment
  vbox->Add(x, 0, wxALIGN_CENTER_HORIZONTAL);            // Right
  ```
- **Rule:** A fixed-size neighbour of a stretching item gets proportion 0.
  **Why:** proportions inflate `CalcMin`, widening the fitted dialog.

## Ownership and removal

**Contract.**
- "Sizers, like child windows, are owned by the library and will be deleted by it which implies that
  they must be allocated on the heap. However if you create a sizer and do not add it to another sizer or
  window, the library wouldn't be able to delete such an orphan sizer and in this, and only this, case it
  should be deleted explicitly" (`interface/wx/sizer.h:45-49`). Sizers own child sizers and spacers, not
  child windows; a sizer has exactly one owner: a parent sizer or one `SetSizer`.
- `Detach(win|sizer|index)` never destroys and "does not cause any layout or resizing to take place, call
  Layout() to update the layout 'on screen'" (`sizer.h:416-451`). `Remove(wxSizer*)`/`Remove(index)`
  destroy sizers and spacers; `Remove(wxWindow*)` is deprecated and does **not** destroy the window
  despite its name (`sizer.h:789-835`). `Clear(delete_windows = false)` always deletes child sizers and
  destroys child windows (via `Destroy()`) only with `true` (`sizer.h:378-389`, `sizer.cpp:1169`).
  `Replace(oldwin, newwin)` neither hides nor destroys `oldwin`, which stays on screen at its last
  position; `Replace(oldsizer, newsizer)` deletes the old sizer (`sizer.h:836-882`).
- A destroyed window detaches itself from its containing sizer (`wincmn.cpp:510`). Deleting or replacing
  a sizer, including `SetSizer(new)` with `deleteOld`, only clears its windows' containing-sizer pointers
  (`sizer.cpp:519` `wxSizerItem::Free`): the old controls stay alive and visible as unmanaged ghosts.
- A window belongs to one sizer: "Adding a window already in a sizer, detach it first!"
  (`wincmn.cpp:2421`). In Orca the check is silent and the item is still appended, so the second sizer
  holds a dangling pointer once the window dies.
- `wxStaticBoxSizer` owns its box; its destructor destroys the box with `WXDestroyWithoutChildren`, which
  reparents the box's children to the box's parent instead of destroying them (`sizer.cpp:2822`).

**Pitfalls**
- **Rule:** Destroy old windows when rebuilding a panel.
  ```cpp
  panel->SetSizer(build_rows());                      // Wrong: old rows stay as ghosts
  panel->DestroyChildren(); panel->SetSizer(build_rows()); panel->Layout();   // Right
  // or: old_sizer->Clear(true) before refilling it
  ```
- **Rule:** `Detach` a window before adding it to another sizer.
  **Why:** the silent `wxCHECK` leaves both sizers pointing at it.
- **Rule:** After `Replace(old, neu)`, `old->Destroy()` (or `Hide()`) and `Layout()`.
  **Why:** `Replace` leaves `old` drawn at its last position.

## Specific sizers

**`wxStaticBoxSizer`** — "strongly encouraged to create the windows which are added … as children of
wxStaticBox itself … creating them using the static box parent as parent still works too (but note that
items using different parents can't be used inside the same sizer" (`interface/wx/sizer.h:2036-2045`).
Use `sz->GetStaticBox()` as the parent. **[source]** Box-children are positioned box-relative: (0,0) on
GTK, a fixed 10 px inset on macOS, the static borders on MSW (`wxStaticBoxSizer::RepositionChildren`,
`sizer.cpp:2888`); mixing the two parents mispositions one group. Orca: `LabeledStaticBox`
(`Widgets/LabeledStaticBox.hpp`, a `wxStaticBox`) is the box to use (`new wxStaticBoxSizer(stb, wxVERTICAL)`
in `OptionsGroup` and `calib_dlg.cpp`); those layouts parent all items to the dialog, which is valid as long
as no item in that sizer is parented to the box.

**`wxGridSizer`** — every cell gets the size of the largest item; `cols` alone lets rows grow; with both
`rows` and `cols` given, at most `rows × cols` items are allowed (`sizer.h:1897-1944`). **[source]** Adding
more is an assert in a debug wx; in Orca `wxGridSizer::DoInsert` silently forgets the row count and lets rows
grow (`sizer.cpp:1642-1670`). Hidden items still occupy their cells (`wxGridSizer::RepositionChildren`,
`sizer.cpp:1711`, has no `IsShown` check).

**`wxFlexGridSizer`** — per-row heights and per-column widths; growables via `AddGrowableCol(idx, prop)`;
if all proportions are 0, all growables share equally; re-adding an index requires `RemoveGrowableCol`
first (`sizer.h:1770-1792`). Indices are checked only against fixed ctor counts (`sizer.cpp:2235-2262`).
`SetFlexibleDirection`/`SetNonFlexibleGrowMode` "do not trigger relayout" (`sizer.h:1859-1878`). **[source]**
Hidden items keep their cell; a row or column (gap included) collapses only when all its items are hidden
(`wxFlexGridSizer::FindWidthsAndHeights`, `SumArraySizes`). A growable
column grows the cell, not the control: the item also needs `wxEXPAND`.
```cpp
if (!flex->IsColGrowable(1)) flex->AddGrowableCol(1, 1);   // rebuild-safe
flex->Add(value_ctrl, 0, wxEXPAND);                         // fill the grown cell
```

**`wxGridBagSizer`** — `Add(win, wxGBPosition, wxGBSpan, flag, border)` returns `nullptr` when the cell is
occupied (`interface/wx/gbsizer.h:82-95`) and the window stays unmanaged: check the result.
`SetEmptyCellSize` sets the size of empty rows and columns (`gbsizer.h:195`).

**`wxWrapSizer`** — lays items out along the primary direction and wraps to new lines;
`wxEXTEND_LAST_ON_EACH_LINE | wxREMOVE_LEADING_SPACES` is the default (`interface/wx/wrapsizer.h`).
**[source]** Before it has been given a width its min size is the largest single item
(`wxWrapSizer::CalcMin` → `CalcMaxSingleItemSize`, `src/common/wrapsizer.cpp:182, 243`), so a dialog fitted
before the first layout is sized for one item per line. Put it where it receives a definite width
(`wxEXPAND` in a vertical box under a container with a known width) and re-fit after the first `Layout()`.

**`wxStdDialogButtonSizer`** — `AddButton` accepts only the stock ids (`wxID_OK/YES/SAVE/APPLY/CLOSE/NO/
CANCEL/HELP/CONTEXT_HELP`); other ids go through `SetAffirmativeButton`/`SetNegativeButton`/`SetCancelButton`;
`Realize()` must be called to order and space the buttons; order follows the platform, and on macOS a
`wxID_SAVE` button is relabelled "Save" and `wxID_NO` "Don't Save" (`sizer.h:1030-1151`). Orca uses
`DialogButtons` instead
([Orca idioms](#orcaslicer-layout-idioms-and-spacing-conventions)).

**Books and splitters** (other API: `references/controls-dataview.md`). **[source]** A book control's best
size is the **max over all pages** unless the protected `SetFitToCurrentPage(true)` was called
(`wxBookCtrlBase::DoGetBestSize`, `src/common/bookctrl.cpp:125-148`), so a large hidden page enlarges a fitted
dialog. `wxSplitterWindow::SetMinimumPaneSize` takes pixels: pass `FromDIP(n)`.

## wxEVT_SIZE handlers

**Contract.** "Sizers rely on size events … in a sizer-based layout, do not forget to call Skip on all
size events you catch" (`interface/wx/event.h:5058-5060`). Automatic layout is the static-table handler
`wxWindowBase::InternalOnSize` (`wincmn.cpp:124, 2489`) and `wxTopLevelWindowBase::OnSize`
(`toplvcmn.cpp:40`). `Bind()` handlers run before static tables, so a bound handler that does not
`Skip()` suppresses auto-layout (handler order: `references/events.md`). **[source]** `wxScrolled<>` always
runs its own size handling after user handlers, skipped or not (`src/generic/scrlwing.cpp:203-214`).

**Pitfalls**
- **Rule:** Skip, don't `Layout()` (it runs anyway) and never `SetSize` the same window inside its size
  handler; defer other relayouts with `CallAfter` or `PostSizeEvent`.
  ```cpp
  Bind(wxEVT_SIZE, [this](wxSizeEvent&) { recompute(); });               // Wrong: no auto-layout
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { recompute(); e.Skip(); });   // Right
  ```
  Cite: `Label::OnSize`, `CenteredMultiLinePanel::OnSize` (both skip).

New code binds with `Bind()` and lambdas; no new static event tables (`references/events.md`).

## Freeze / Thaw

**Contract** (`window.h:2203-2232`). `Freeze()` suppresses painting of the window and, recursively, its
non-top-level children; calls nest and must balance; it is "mostly just a hint". **[source]** Children
added while frozen are frozen too, removed ones are thawed (`wxWindowBase::AddChild`/`RemoveChild`).
Freezing does **not** stop layout or size events. RAII: `wxWindowUpdateLocker` (`include/wx/wupdlock.h:19`).

**Platforms [source].** MSW: `Freeze()` on a hidden window only counts; the native redraw lock is skipped
(`wxWindowMSW::DoFreeze`, `src/msw/window.cpp:1659`) and applied by a later `Show()` while still frozen
(`wxWindowMSW::Show`). macOS: `Refresh()` returns early while `IsFrozen()` or not shown
(`src/osx/window_osx.cpp:1340-1346`).

**Pitfalls**
- **Rule:** Balance on every path; prefer `wxWindowUpdateLocker`.
  **Why:** an unmatched `Freeze()` leaves the window and its children unpainted. An unmatched `Thaw()`
  is worse: `m_freezeCount` is `unsigned` (`include/wx/window.h:2063`) and the "Thaw() without matching
  Freeze()" assert is compiled out, so the count wraps to `UINT_MAX`, `IsFrozen()` stays true from then on
  (a later balanced pair only drops it to 0 between its `Freeze()` and `Thaw()`), and on macOS the window
  stops repainting.
  ```cpp
  Freeze(); if (!rebuild()) return; Thaw();          // Wrong: early return leaves it frozen
  wxWindowUpdateLocker lock(this); if (!rebuild()) return;   // Right
  ```

**OrcaSlicer.** `wxWindowUpdateLocker` brackets bulk rebuilds in `Tab.cpp`, `ParamsPanel.cpp`,
`PresetComboBoxes.cpp`, `GUI_ObjectSettings.cpp`; `DPIAware::rescale` wraps `on_dpi_changed` in
`Freeze()`/`Thaw()`.

## Scrolled windows

**Contract** (`interface/wx/scrolwin.h`).
- `wxScrolledWindow` (`wxScrolled<wxPanel>`) hosts child controls; `wxScrolledCanvas`
  (`wxScrolled<wxWindow>`) is for drawn content; `wxScrolled<wxControl>` is not advised (`:24-40`).
- **Scrolling is off until a rate is set**: "scrolling is only enabled in orientations with a non-zero
  increment" (`:57-66`); both rates start at 0. Vertical-only: `SetScrollRate(0, FromDIP(n))`.
- With a sizer, the virtual size follows the sizer; "if you add or remove any elements to the sizer, you
  need to call wxSizer::FitInside() to adjust the virtual size" (`:66-68`). `wxWindow::FitInside()` =
  `SetVirtualSize(GetBestVirtualSize())` (`wincmn.cpp:631`); `wxSizer::FitInside(win)` "will not alter the
  on screen size" (`sizer.h:465-473`). **[source]** A size event re-derives the virtual size
  (`wxScrollHelperBase::HandleOnSize`, `scrlwing.cpp:924`).
- **Best size ignores content in a scrolling direction [source]**: with a sizer, in each direction with a
  non-zero rate the best size is `GetMinSize() + scrollbar thickness` (`wxScrolledT_Helper::FilterBestSize`,
  `scrlwing.cpp:1594-1634`: "If the app needs some minimal size for its scrolled window, it should set it
  and put the window into sizer as expandable"). Without `SetMinSize` and proportion/`wxEXPAND` it
  collapses to about the scrollbar width.
- `EnableScrolling(x, y)` toggles **physical** (blit) scrolling; it does not enable or disable a
  direction (`:338-356`). Direction is chosen by the scroll rate and the `wxHSCROLL`/`wxVSCROLL` style.
- `ShowScrollbars(horz, vert)` (`wxSHOW_SB_NEVER/DEFAULT/ALWAYS`) works only after creation; the
  `wxALWAYS_SHOW_SB` style is the ctor-time equivalent for both directions (`:358-384`).
- Children report physical positions: a child at (10,10) reports (10,-90) after scrolling 100 px
  (`:90-96`). `GetViewStart()` is in scroll units; `GetViewStartPixels()` is new in 3.3.2 (`:405-458`).
- `SetTargetWindow(w)` requires overriding `GetSizeAvailableForScrollTarget()` (`:602-617, 719-731`).
- A focused child is scrolled into view; override `ShouldScrollToChildOnFocus` to opt out (`:705-717`).
  Mouse-drag autoscroll is configurable with `EnableAutoScrollInside`/`DisableAutoScrollOutside` (new in
  3.3.2, `:628-661`).
- Drawing in a scrolled window (`OnDraw`, `DoPrepareDC`, `DoPrepareReadOnlyDC`): see
  `references/painting-custom-widgets.md §DC coordinates and scale under DPI`.

**Usage — "shrink to content, scroll beyond a cap":**
```cpp
auto sw = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
sw->SetScrollRate(0, FromDIP(20));
sw->SetSizer(content);
// after every content change:
int h = std::min(content->GetMinSize().y, cap);
sw->SetMinSize(wxSize(-1, h));
sw->FitInside();
parent->Layout();
```
Orca: `Sidebar::update_filaments_area_height` (`SetMaxSize` from a preferred row count, then
`SetMinSize({-1, min(sizer min, max)})` on `m_panel_filament_content`, with `FitInside()` after rebuilds);
`add_msg_content` in `MsgDialog.cpp` (a `wxScrolledWindow(wxVSCROLL)` with `SetScrollRate(0, FromDIP(20))`, a
`Label(…, LB_AUTO_WRAP)` with min = max width, the window's min = max size set to
`(info_width, min(content, 48 * em))`, then `FitInside()`), which keeps the message within the dialog max.

**Orca `ScrolledWindow`** (`Widgets/ScrolledWindow.hpp`) — a `wxScrolled<wxWindow>` that hides the native
bars (`ShowScrollbars(NEVER, NEVER)`) and draws slim `MyScrollbar`s in a margin strip:
- content goes on `GetPanel()`, the `SetTargetWindow` target (`GetSizeAvailableForScrollTarget` is not
  overridden);
- the ctor builds its inner windows from the passed `size`, so pass a real size;
- the virtual size is set manually in pixels: `SetScrollbars(1, 1, w, h)` or its own `SetVirtualSize`,
  which only **hides** the non-virtual `wxWindow::SetVirtualSize` — `FitInside()` and calls through a
  `wxWindow*` bypass the custom bars;
- its size handler resizes the inner panels and calls `Layout(); AdjustScrollbars();`;
- use it with `wxVSCROLL` only: `SetBackgroundColour` dereferences the vertical-only members.
```cpp
// SearchDialog::SearchDialog (Search.cpp)
auto sw   = new ScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(W, H), wxVSCROLL, 6, 6);
auto list = new wxWindow(sw->GetPanel(), wxID_ANY);
list->SetSizer(s); list->Fit();
sw->SetScrollbars(1, 1, 0, list->GetSize().GetHeight());
```
Ordinary dialogs and the sidebar use plain `wxScrolledWindow`.

**Pitfalls**
- **Rule:** A scrolled area that "doesn't scroll" lacks `SetScrollRate` or a `FitInside()` after the
  content changed; one that collapses lacks a min size or proportion + `wxEXPAND`.
- **Rule:** Forbid horizontal scrolling with `SetScrollRate(0, y)` and/or a `wxVSCROLL`-only style.
  ```cpp
  sw->EnableScrolling(false, true);       // Wrong: only disables blit scrolling
  sw->SetScrollRate(0, FromDIP(20));      // Right
  ```

## wxStaticText wrapping and ellipsizing

**Contract.**
- `Wrap(width)` breaks lines at word boundaries and **modifies the label** (`interface/wx/stattext.h:119-130`,
  `include/wx/stattext.h:37-40`). `width < 0` means no wrapping; the width is not exact because of borders.
  `Wrap` is not virtual.
- **3.3.2 wrap cache [source].** `wxStaticTextBase::Wrap` returns at once when `width == m_currentWrap`
  (`src/common/stattextcmn.cpp:259-263`); `SetLabel` → `UpdateLabelOrig` clears the saved unwrapped
  label but **not** `m_currentWrap` (`:354-365`). So `SetLabel(new); Wrap(sameWidth);` leaves the new
  text unwrapped. `Wrap(-1); Wrap(w);` resets the cache.
- wx breaks only at whitespace and outputs an unbreakable run whole (`stattextcmn.cpp:184-215`): CJK text
  without spaces never wraps with `wxStaticText::Wrap`.
- `wxST_WRAP` (new in 3.3.2): "Wrap label text on multiple lines if necessary, using the available
  horizontal space. This style only works when the control is used inside a sizer"
  (`interface/wx/stattext.h:46-49`; `docs/changes.txt:280`). It is opt-in; labels without it are
  unaffected. It is implemented with `GetMinSizeFromKnownDirection` (`stattextcmn.cpp:285-312`).
  **[source]** The initial `CalcMin` still uses the unwrapped best size, so a fitted dialog grows to the
  full one-line width and nothing wraps; constrain the width another way (a fixed or min width on the
  container, `SetMaxSize`).
- `SetLabel` resizes the control to its best size unless `wxST_NO_AUTORESIZE`, which right- or
  centre-aligned labels whose width the sizer sets need (`stattext.h:30-36`); it never re-lays out the
  parent; it does nothing when the text is unchanged (`stattext.h:107-117`).
- `wxST_ELLIPSIZE_*` ellipsize only when the control is narrower than its text (`stattext.h:37-45`); the
  best size is always the full text, so the sizer allots the full width unless something limits it: a
  width cap (`SetMaxSize(wxSize(w, -1))`), or an explicit min width plus `wxST_NO_AUTORESIZE`. **[source]**
  The generic ellipsizer does nothing while the client width or height is < 2 px
  (`wxStaticTextBase::Ellipsize`).
- Mnemonics, markup and `SetLabelText`: `references/controls-dataview.md`.

**Platforms [source].** GTK: `GtkLabel` always has line wrap on, so a `wxStaticText` given less width
than its text wraps natively, while MSW and macOS clip it; best width adds 1 px to avoid spurious wraps
(`src/gtk/stattext.cpp:126, 239-296`). `SetFont` on a hidden label makes wx measure the text itself
because the GTK style cache is stale (`gtk/stattext.cpp:178-190`). GTK2 only: a centre/right-aligned label
silently gets `ELLIPSIZE_MIDDLE`/`START` (`gtk/stattext.cpp:63-90`). MSW uses native end-ellipsis only for
single-line `wxST_ELLIPSIZE_END` labels; other modes are generic (`src/msw/stattext.cpp:233-256`).

**OrcaSlicer `Label`** (`Widgets/Label.hpp`, `: wxStaticText`):
- keeps the original text in `m_text`; `Label::Wrap(int)` **hides** the non-virtual `wxStaticText::Wrap`,
  wraps `m_text` with Orca's `wxTextWrapper2` (breaks between ideographs above U+4E00 and hard-breaks
  space-less runs) and sets the result through the base `SetLabel` with `m_skip_size_evt` set; it has no
  width cache;
- `LB_AUTO_WRAP` binds `wxEVT_SIZE` (with `Skip()`) and re-wraps at the new width; `Label::SetLabel`
  re-wraps at `GetSize().x` under `LB_AUTO_WRAP` and returns early when the text is unchanged;
- the label needs a real width: a ctor size `wxSize(FromDIP(w), -1)`, min = max width, or `wxEXPAND` in a
  vertical sizer whose container width is fixed;
- `Label::split_lines(dc, width, text, out, max_count)` exposes the same wrapper for owner-drawn text.
Orca's wrapping is preferred over `wxST_WRAP` for CJK text.

**Pitfalls**
- **Rule:** Re-wrapping after `SetLabel` must defeat the wrap cache.
  ```cpp
  st->SetLabel(msg); st->Wrap(w);                        // Wrong when w is unchanged: stays unwrapped
  st->SetLabel(msg); st->Wrap(-1); st->Wrap(w);          // Right
  // or: Label with LB_AUTO_WRAP / label->Wrap(w) through a Label*
  ```
- **Rule:** Call `Wrap` through a `Label*`, never through a `wxStaticText*` that points at a `Label`.
  **Why:** the base `Wrap` sets the wrapped text through the virtual `Label::SetLabel`, which overwrites
  `m_text` with the wrapped copy (and re-wraps it under `LB_AUTO_WRAP`).
- **Rule:** Never compute or commit a size or wrap from a width that has not been laid out; guard with a
  sanity check on the client width.
  **Why:** a plain `wxWindow`/`wxPanel` child created with `wxDefaultSize` is wx's 20×20 placeholder on
  every port until the first sizer layout **[source]** (`include/wx/window.h:1914-1915` `WidthDefault`/
  `HeightDefault`; native controls instead take their best size through `SetInitialSize` in `Create()`),
  and a minimized top-level window reports a (0,0) client size. Wrapping against that width commits a garbage min size, which `SetSizeHints` then locks in.
  ```cpp
  void UpdateMinSize() {
      int cWidth = GetClientSize().GetWidth();
      if (cWidth < 50) return;                 // not laid out yet: don't commit a size
      /* wrap at cWidth, SetMinSize(wxSize(-1, h)), GetParent()->Layout() */
  }
  ```
  Cite: 5ede9711f5 (`CenteredMultiLinePanel::UpdateMinSize` and `::OnPaint` in `TroubleshootDialog.hpp`).
- **Rule:** Give a wrapping label a fixed width and `-1` height, then wrap.
  **Why:** the ctor size becomes the min size (`SetInitialSize`), so a fixed height clips the text when a
  translation is longer.
  ```cpp
  new wxStaticText(this, wxID_ANY, txt, wxDefaultPosition, wxSize(FromDIP(490), FromDIP(40)));  // Wrong
  m_action_line = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxSize(FromDIP(490), -1));                                    // Right
  m_action_line->Wrap(width);
  ```
  Cite: 5ede9711f5 (`UnsavedChangesDialog`, `UNSAVE_CHANGE_DIALOG_ACTION_LINE_SIZE`, `m_action_line`).
- **Rule:** An ellipsized label needs a width limit.
  **Why:** its best size is the full text, so the sizer gives it the full width and nothing ellipsizes.

## Layout on DPI change

**What wx does per platform.**

| Platform | When `DPIAware::on_dpi_changed` runs | What wx already rescaled |
|---|---|---|
| MSW (per-monitor v2 manifest) | `wxEVT_DPI_CHANGED`; also `wxEVT_MOVE_END` when the scale differs | **[source]** before the event, `MSWUpdateOnDPIChange` rescales every window's min/max size, the fonts of non-top-level windows (the TLW keeps its font), every sizer item's border, spacer and nested-sizer min sizes, and invalidates best sizes (`src/msw/window.cpp:5002-5085`; detail in `references/dpi-bitmaps-fonts.md` §wxEVT_DPI_CHANGED) |
| GTK3 | `wxEVT_DPI_CHANGED` when the integer GDK scale changes (GTK ≥ 3.10, `src/gtk/toplevel.cpp:336-350`) | nothing; logical pixels are DIPs, so DIP sizes stay valid |
| macOS | never: wx does send `wxEVT_DPI_CHANGED` on backing-scale changes **[source]** (`src/osx/cocoa/nonownedwnd.mm` `windowDidChangeBackingProperties`), but `DPIAware` binds it only `#ifndef __WXOSX__` | nothing needed; Cocoa scales |

A top-level window's default `wxEVT_DPI_CHANGED` handler resizes it, and a handler that does not `Skip()`
suppresses that (`interface/wx/event.h:3572-3584`). `DPIAware`'s handler does not `Skip()`, so on MSW wx
skips its own resize of the top-level window (`src/msw/nonownedwnd.cpp:284-318` `HandleDPIChange`): on MSW
the dialog must resize itself in `on_dpi_changed`. `DPIAware::rescale` (`src/slic3r/GUI/GUI_Utils.hpp`) runs
`Freeze(); update_em_unit(); on_dpi_changed(rect); Layout(); Thaw();`. The em machinery: see
`references/dpi-bitmaps-fonts.md`.

`em_unit` per platform (`DPIAware::update_em_unit`): MSW `max(10, 10 × dpi/96)`; macOS always 10 (Orca's
`get_dpi_for_window` returns 96 there); GTK `max(10, GetTextExtent("m").x − 1)`, i.e. from the font. So
on GTK an `n * em` size tracks the system font size while `FromDIP(n)` does not; pick one consistently
for related sizes.

**What `on_dpi_changed` re-applies.** Everything wx does not own:
- sizes set with `SetSize` or computed into members (cached metrics, wrap widths);
- `wxDataViewCtrl::SetRowHeight`, column widths and other setter-held values (`references/controls-dataview.md`);
- bitmaps and `ScalableBitmap`s (`references/dpi-bitmaps-fonts.md`);
- each Orca widget's `Rescale()` (and `msw_rescale()` where that is its name);
- min sizes expressed in em, recomputed from the new em (a ratio-free recompute is safe on MSW even
  though wx already rescaled the stored value);
- then the top-level resize.

**Usage — the reconciled body:**
```cpp
void MyDialog::on_dpi_changed(const wxRect&)          // MyDialog : DPIDialog; members illustrative
{
    const int em = em_unit();
    msw_buttons_rescale(this, em, {wxID_OK, wxID_CANCEL});  // raw wxButtons only: min height 2.5 em (omit with DialogButtons, below)
    m_apply_btn->Rescale();                                 // Button / TextInput / ComboBox / SpinInput
    m_logo.msw_rescale();                                   // ScalableBitmap
    m_logo_ctrl->SetBitmap(m_logo.bmp());                   // the control holds its own copy
    m_list->SetMinSize(wxSize(40 * em, -1));                // recompute from em, never multiply
    GetSizer()->SetSizeHints(this);                         // resize + new minimum
    Refresh();
}   // DPIAware::rescale then calls Layout() and Thaw()
```
`SetSizeHints` both resizes and re-derives the minimum, which a bare `Fit()` does not. A `Fit()` is
acceptable only when a correct minimum already exists (`PurgeModeDialog::on_dpi_changed` sets
`SetMinSize(wxSize(70 * em, 32 * em))` and then `Fit(); Refresh();`). **[source]** A `Refresh()`-only
body (`FlowRateCalibrationDialog::on_dpi_changed`) leaves the dialog at its old pixel size on MSW, so
content is clipped until the user resizes it; on GTK3 it is harmless. An empty override (`CloneDialog`)
is accepted only for trivially simple dialogs, with the same MSW caveat. Code placed only in
`on_dpi_changed` never runs on macOS.

Larger dialogs walk their widgets: `PreferencesDialog::on_dpi_changed` `dynamic_cast`s each child and
calls `Rescale()` on every `Button`, `TextInput`, `ComboBox`, `SpinInput` and `WikiLabel`, then re-runs its
tab switch. In such a walk, qualify `::CheckBox`: inside `Slic3r::GUI` an unqualified `CheckBox` names the
`Field` subclass from `Field.hpp`, which is not a `wxWindow`, so the cast never matches (namespaces:
`references/orca-widgets.md`). `DialogButtons` rescales itself: it binds its parent's `wxEVT_DPI_CHANGED`,
rebuilds its row and `Skip()`s. `msw_buttons_rescale` (`wxExtensions.cpp`) calls
`SetMinSize(wxSize(-1, 2.5 * em))` on whatever window has each id; `DialogButtons` gives its Orca
`Button`s stock ids, so on a dialog with a `DialogButtons` row it would override their style height
through `Button::SetMinSize`; leave it out there.

**Pitfalls**
- **Rule:** Every dialog has an enforced minimum before any DPI/refresh path can resize it, and
  `on_dpi_changed` resizes with `GetSizer()->SetSizeHints(this)` rather than an unconditional bare `Fit()`.
  **Why:** a `Fit()` on a dialog without a minimum is the f760f4e462 collapse; a `Fit()` with a stale
  minimum keeps the old floor; and on MSW a body that does not resize leaves content clipped because
  `DPIAware` suppresses wx's own resize.
  ```cpp
  // Wrong
  Layout(); Fit();                                      // ctor: no minimum
  void on_dpi_changed(const wxRect&) override { Refresh(); Fit(); }
  // Right
  Layout(); v_sizer->SetSizeHints(this);                // ctor
  void on_dpi_changed(const wxRect&) override { /* rescale content */ GetSizer()->SetSizeHints(this); Refresh(); }
  ```
  Cite: f760f4e462 (`calib_dlg.cpp`), 5ede9711f5; `src/msw/nonownedwnd.cpp` `HandleDPIChange`.
- **Rule:** Re-apply sizes that wx does not rescale with the same expression as the constructor;
  never scale an existing value by a DPI ratio.
  **Why:** setter-held values (row heights, column widths, `SetSize` sizes) never rescale on any port,
  and stale pixels clip or overflow content (the object-list filament badge no longer fitting its row).
  On MSW, wx already rescaled stored min/max sizes and sizer borders, so `SetMinSize(GetMinSize() * ratio)`
  scales twice. On GTK3 and macOS, DIP values need no rescale at all.
  ```cpp
  // ObjectList::create_objects_ctrl and again in ObjectList::msw_rescale
  SetRowHeight(2 * em + FromDIP(2));           // same expression, new em
  SetMinSize(GetMinSize() * new_scale / old);  // Wrong: double scaling on MSW
  ```
  Cite: d5638273c6 (`ObjectList::create_objects_ctrl`, `ObjectList::msw_rescale`).

## Platform summary

- **MSW.** DPI change rescales min/max sizes, non-top-level fonts, sizer borders, spacer and nested-sizer min sizes before
  `wxEVT_DPI_CHANGED`; Orca must resize the top-level window itself. The default `wxSizerFlags` border
  follows the main window's DPI. `Freeze()` on a hidden window skips the native redraw lock until the window
  is shown. A minimized top-level window reports client size (0,0).
- **macOS.** `FromDIP` is the identity and `em_unit` is 10. Default sizer border 5. `wxStaticBoxSizer`
  box-children sit at a 10 px inset. `Refresh()` is a no-op while frozen or not shown.
- **GTK3.** Sizer-fitting calls on a hidden top-level window are replayed at `Show()`; `wxWindow::Fit()` is
  not. **[source]** `SetFont` before the top-level window is shown queues best-size revalidation at show
  (`GTKSizeRevalidate`, `src/gtk/window.cpp:6651-6725`), so best sizes measured in a ctor can be wrong
  until then; re-measure on show or DPI change, or let sizers do it. Top-level and popup `SetSize` are
  clamped to min/max. WM geometry hints only for `wxRESIZE_BORDER`. `GtkLabel` wraps natively when given
  less width. Default sizer border 6. A minimized top-level window reports client size (0,0)
  (`toplevel.cpp:1463`). On X11 the first `Show()` of a top-level window may be deferred until
  `_NET_FRAME_EXTENTS` arrives (`toplevel.cpp:1140-1240`), so geometry is not final right after `Show()`
  (window showing: `references/windows-dialogs.md`). em follows the font.
- **GTK2** (opt-out build). `FromDIP` is the identity in effect (fixed 96 PPI); centre/right-aligned labels
  are implicitly ellipsized.
- **Wayland.** `wxWindow::Update()` "doesn't do anything in wxGTK port when using Wayland"
  (`window.h:2405-2407`): `Layout(); Update();` does not force a synchronous repaint. No deferred first show;
  with client-side decorations the decoration size counts as 0 for hints (`toplevel.cpp:1519-1523`).

## OrcaSlicer layout idioms and spacing conventions

- **Units.** Every pixel value is `FromDIP(n)` or `n * em_unit()` (`em_unit()` on a `DPIDialog`/`DPIFrame`;
  the free `em_unit(wxWindow*)` in `wxExtensions.cpp` returns the enclosing `DPIDialog`/`DPIFrame`'s em,
  else `wxGetApp().em_unit()`). Never raw ints, never `wxSizerFlags::Border()` defaults.
- **Spacing conventions.** Outer dialog padding `FromDIP(10)`–`FromDIP(20)` with `wxEXPAND | wxALL`;
  inter-widget gaps `FromDIP(4)`–`FromDIP(12)`; `FromDIP(1)` separators; `sizer->AddSpacer(FromDIP(n))`
  for vertical rhythm; `AddStretchSpacer()` to push a group to the far side.
- **Separators.** A 1 px `wxPanel` with a light grey background (`FilamentPickerDialog::CreateSeparatorLine`)
  or the `StaticLine` widget; avoid `wxStaticLine`.
- **Button rows.** `DialogButtons` (`Widgets/DialogButtons.hpp`, `Slic3r::GUI`) is a `wxPanel` with its own
  horizontal `wxBoxSizer`, not a `wxStdDialogButtonSizer`; add it with `main_sizer->Add(dlg_btns, 0, wxEXPAND)`.
  `UpdateButtons()` rebuilds the row with `m_sizer->Clear()` (the buttons are the panel's children and
  survive), adds a leading gap when no button is left-aligned, a stretch spacer before the right-aligned
  group, each button with
  `wxRIGHT`/`wxLEFT | wxTOP | wxBOTTOM | wxALIGN_CENTER_VERTICAL` and a `FromDIP(ButtonProps::ChoiceButtonGap())`
  border, then `Layout(); Fit();`. The order is the same on every platform, deliberately unlike wx's
  per-platform ordering. Older dialogs hand-roll the row with `Button` + `AddStretchSpacer()`;
  `DialogButtons` is the form for new code. Construction and labels: `references/orca-widgets.md §DialogButtons`;
its place in a dialog: `references/windows-dialogs.md §8`.
- **Long static text.** Fixed-width `wxStaticText` + `Wrap(FromDIP(w))` once, or `Label` with
  `LB_AUTO_WRAP` when the text changes or must wrap CJK ([Wrapping](#wxstatictext-wrapping-and-ellipsizing)).
- **Bulk rebuilds.** `wxWindowUpdateLocker` (or a balanced `Freeze()`/`Thaw()`), rebuild, `Layout()` the
  owning ancestor, `SetSizeHints` if the top-level size must change; `PostSizeEvent()` on the plater when
  the sidebar's height changed.
- **Size clamps.** Dialogs may clamp with `SetMinSize/SetMaxSize(FromDIP(…))`; keep content within the
  max ([Fitting](#fitting-functions-setsizer-setsizerandfit-setsizehints-fit-layout)).
- **Exemplars.** `CloneDialog.cpp` (minimal `SetSizerAndFit` dialog), `MsgDialog.cpp` (late content,
  capped scrolled text), `PrintOptionsDialog.cpp` `PrinterPartsDialog::Show` (show/hide rows + re-hint),
  `Preferences.cpp` (sizer-visibility pages, height-for-width `WikiLabel`), `Plater.cpp`
  `Sidebar::update_filaments_area_height` (capped scrolled list).
