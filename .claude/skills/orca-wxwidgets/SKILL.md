---
name: orca-wxwidgets
description: Use when writing, modifying, reviewing or debugging any OrcaSlicer GUI code under src/slic3r/GUI, or when deciding how a wxWidgets API behaves in OrcaSlicer — dialogs, frames, panels, the sidebar, Preferences, device/monitor pages, custom widgets, popups and menus, sizers and layout, painting, DPI scaling, dark mode, colours, icons, fonts, translated strings, settings fields, keyboard shortcuts, mouse capture and focus, events, CallAfter, threads and timers, WebView, the OpenGL canvas, AUI docking, clipboard, drag and drop, file dialogs — and for platform-specific UI bugs on Windows, macOS or Linux GTK/X11/Wayland such as popups that close at once, a frozen or unclickable UI, collapsed or clipped dialogs, invisible dark-mode icons, or crashes on close, even when the task never mentions wxWidgets.
---

# OrcaSlicer wxWidgets GUI

This skill is how OrcaSlicer's wxWidgets GUI is written, changed, fixed and reviewed. Its core is
**wxWidgets 3.3.2 API usage** — the documented contracts, the per-platform behaviour and the known
limitations of the wx version Orca pins — layered with the OrcaSlicer conventions, wrappers, custom
widget library and stable component designs a contributor must follow. Every reference file opens
with a numbered **Rules** checklist (what a diff is checked against), followed by sections that give
the wx contract, the correct code shape, platform differences, the Orca layer, and pitfalls as
wrong → right pairs with the commit that fixed each one.

## Ground truth: the wx tree in deps/

Orca pins **wxWidgets 3.3.2** from the fork `github.com/SoftFever/Orca-deps-wxWidgets` (tag `v3.3.2`,
`deps/wxWidgets/wxWidgets.cmake`), built static (Flatpak: shared). Facts about this build that change
how you read the wx docs:

- **Linux builds against GTK3** (`DEP_WX_GTK3` defaults ON in `deps/CMakeLists.txt`). Target GTK3 on
  both X11 and Wayland; GTK-guarded code must still compile on GTK2, an opt-out build Orca does not ship.
- **wx asserts never fire.** wx is built with `wxBUILD_DEBUG_LEVEL=0` and `libslic3r_gui` with
  `wxDEBUG_LEVEL=0`. Wherever the docs say a call "asserts", Orca silently ignores it, returns early or
  corrupts state. Check preconditions yourself; do not expect a debug build to catch misuse.
- **No SVG in wx** (`wxUSE_NANOSVG=OFF`): `wxBitmapBundle::FromSVG*` does not exist. Orca rasterises
  its SVG icons itself (`BitmapCache`, `create_scaled_bitmap`, `ScalableBitmap`).

Look things up in the source the app is built from — it beats memory, and 3.3 changed real behaviour:

```bash
WX=$(find deps -maxdepth 5 -type d -path '*dep_wxWidgets-prefix/src/dep_wxWidgets' | head -1)
# macOS: deps/build/<arch>/dep_wxWidgets-prefix/src/dep_wxWidgets   Linux: deps/build/dep_wxWidgets-prefix/...
# If deps are not built: git clone --depth 1 -b v3.3.2 https://github.com/SoftFever/Orca-deps-wxWidgets
grep -n "CaptureMouse" -A 30 $WX/interface/wx/window.h      # documented contract (doxygen source)
grep -rn "@onlyfor\|not implemented" $WX/interface/wx/popupwin.h   # documented platform limits
ls $WX/docs/doxygen/overviews/                               # eventhandling.h, sizer.h, high_dpi.md, windowdeletion.h, ...
grep -n "IsDark" $WX/docs/changes.txt                         # what changed in 3.3 (changes_32.txt for 3.2)
grep -n "NotifyCaptureLost" -r $WX/src/osx $WX/src/gtk $WX/src/msw   # what each port actually does
```

`interface/wx/<class>.h` is the documentation; `src/common` holds shared behaviour and
`src/{msw,osx,gtk,unix,generic}` the per-port implementation. When the docs and the source disagree,
the source is what runs — the references mark such facts **[source]**. Orca-side design docs live in
`docs/HLSD/` (`keyboard-shortcuts.md`, `deferred-page-construction.md`, `design-tab.md`,
`printer-agent.md`).

## Golden rules

1. **Interactive controls are Orca widgets** (`Button` + `SetStyle(...)`, `::CheckBox`, `::ComboBox`,
   `::TextInput`, `SpinInput`, `SwitchButton`, `RadioGroup`, `TabCtrl`); the bottom row of every dialog is
   `DialogButtons`. Never `wxButton`, `wxSpinCtrl`, `wxCheckBox`, `wxChoice` or a single-line `wxTextCtrl`
   in new code; multi-line text is a raw `wxTextCtrl`. Inside `Slic3r::GUI` write `::CheckBox` etc. —
   `Field.hpp` has classes with the same names. → `orca-widgets.md`
2. **DPI.** Layout pixel values go through `FromDIP(n)` (or `n * em_unit()`); `wxBitmap` constructor
   sizes, image-list sizes and GL viewports are physical and are not `FromDIP`'d (icon heights passed to
   `create_scaled_bitmap`/`ScalableBitmap`/`Button` are DIP). Top-level windows are `DPIDialog`/
   `DPIFrame`; `on_dpi_changed` re-rasterises bitmaps (and re-sets them on the controls showing them),
   calls each widget's `Rescale()`, re-applies stored sizes and finishes with
   `GetSizer()->SetSizeHints(this)`; it never runs on macOS. → `dpi-bitmaps-fonts.md`, `sizers-layout.md`
3. **Dark mode.** Ask `wxGetApp().dark_mode()`, never `wxSystemSettings::GetAppearance().IsDark()`. Use
   palette colours through `StateColor` (specific states first, `Normal` last) and pass a literal light
   colour through `StateColor::darkModeColorFor()`; give every panel you create an explicit palette
   background before creating widgets in it; end each dialog constructor with
   `wxGetApp().UpdateDlgDarkUI(this)`; re-apply hand-picked colours and name-selected icons in
   `on_sys_color_changed()` (a cached or long-lived window must also be reached from
   `MainFrame::on_sys_color_changed`). → `colours-dark-mode.md`
4. **Strings.** Mark user text with `_L` / `_u8L` / `L` (and the `_CONTEXT` / `_L_PLURAL` forms) — never
   `_()`, which xgettext does not extract. Build messages with `format_wxstr(_L("… %1% …"), arg)`;
   convert with `from_u8()` / `into_u8()` (`from_path()` / `into_path()` for paths), never `ToStdString()`
   or an implicit `std::string` → `wxString`. → `strings-i18n-files.md`
5. **Messages to the user** use the `MsgDialog` family (`MessageDialog`, `RichMessageDialog`,
   `WarningDialog`, `ErrorDialog`, `InfoDialog`, `show_error`/`show_info`), never `wxMessageBox` once the
   GUI exists. ESC and the close box return `wxID_CANCEL`, so test for the positive answer
   (`== wxID_YES`); `show_error` is asynchronous. → `windows-dialogs.md`
6. **Events.** `Bind()` with handlers taking the event **by reference**; no new static event tables.
   `Skip()` every non-command event you do not fully replace (focus, size, key, DPI, colour change, mouse
   on custom widgets), and any event — command events included, e.g. `::CheckBox`'s
   `wxEVT_TOGGLEBUTTON` — bound on an Orca widget, wx control or window whose own class also handles it:
   your later-bound handler runs first. Unbind lambdas bound on other objects. → `events.md`
7. **Threads.** Only the main thread touches wx. Workers marshal with `wxGetApp().CallAfter([by-value
   captures]{ … })` and the lambda re-checks liveness (a `std::shared_ptr<std::atomic<bool>>` alive flag,
   `is_closing()`) before touching anything — `wxWeakRef` is main-thread only, never created, copied or
   tested on a worker; never `wxPostEvent` from a worker. UI-initiated background work is a `Job` on a
   `Worker`. → `threads-timers-app.md`, `events.md`
8. **Lifetime.** Heap windows die by `Destroy()`, not `delete`; modal dialogs end with
   `EndModal(wxID_*)` (an id, never a `wxOK`/`wxCANCEL` style bit); ESC and the close box never run an
   Orca Cancel button's handler, so cancel cleanup goes where every path ends. No window work on the stack
   of a mouse handler or a WebView script-message callback — `CallAfter` it. → `windows-dialogs.md`
9. **Layout.** `SetSizerAndFit(sizer)` on top-level windows (AGENTS.md rule), plain `SetSizer` on child
   panels; proportion is the second `Add` argument; a scrolled window needs `SetScrollRate` and
   `FitInside()` after content changes; re-wrap a label with `Wrap(-1); Wrap(w);` or use `Label`. Never
   commit a size or wrap from a width not laid out yet: a `wxDefaultSize` child is 20×20 on every port
   until the first sizer layout. → `sizers-layout.md`
10. **Mouse capture.** `if (!HasCapture()) CaptureMouse();` / `if (HasCapture()) ReleaseMouse();` at every
    site; `wxEVT_MOUSE_CAPTURE_LOST` *cancels* the gesture (never commits); release before a modal, hide or
    destroy. macOS never sends capture-lost, and a leaked capture there leaves the app alive but
    unclickable (keyboard still works). → `mouse-keyboard-focus.md`
11. **Popups.** Derive from Orca's `PopupWindow` and pass `wxPU_CONTAINS_CONTROLS` when it hosts
    controls; size it before `Position()`; on macOS anchor a hover-driven popup flush to its opener; on
    MSW call `BindUnfocusEvent()` when it must close with the frame; use a frameless
    `wxDialog` for content that needs typing focus or hosts a WebView. → `popups-menus.md`
12. **Painting.** Draw only in the `wxEVT_PAINT` handler, change state then `Refresh()`; never draw
    through `wxClientDC` (no effect on macOS and Wayland). → `painting-custom-widgets.md`
13. **Cross-platform.** Every change works on Windows, macOS and Linux GTK3 under X11 and Wayland. No
    global pointer coordinates (`wxGetMousePosition()`) in logic that must work on Wayland; decide
    X11/Wayland at runtime with `is_running_on_wayland()`. → `platforms.md`
14. **Registration.** New sources go into `SLIC3R_GUI_SOURCES` in `src/slic3r/CMakeLists.txt` (platform-only
    files into the `if (WIN32)` / `if (APPLE)` blocks, CAD UI into `if (SLIC3R_CAD)`, `GUI/DeviceCore` /
    `GUI/DeviceTab` files into their own `CMakeLists.txt`), and a new file with translatable strings into
    `localization/i18n/list.txt`. → `orca-architecture.md`, `strings-i18n-files.md`

## The standard dialog

Exemplars: `src/slic3r/GUI/CloneDialog.cpp` (minimal), `FilamentPickerDialog.cpp` (larger, `Create*()`
helpers), `PurgeModeDialog.cpp` (custom-painted clickable cards). Full conventions, `DialogButtons` id
semantics and the `MsgDialog` API are in `windows-dialogs.md` §8–§9.

```cpp
class MyDialog : public DPIDialog
{
public:
    explicit MyDialog(wxWindow* parent)
        : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,
                    _L("My Dialog"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX)            // always pass a style: DPIAware's default is wxDEFAULT_FRAME_STYLE
    {
        SetBackgroundColour(*wxWHITE);                  // light palette colour, dark-mapped by UpdateDlgDarkUI
        SetFont(Label::Body_14);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        // ... Orca widgets, sizes via FromDIP(n), text via _L() ...
        sizer->Add(content_sizer, 1, wxEXPAND | wxALL, FromDIP(10));

        auto* btns = new DialogButtons(this, {"OK", "Cancel"});   // untranslated: DialogButtons calls _L() and assigns wxID_OK/wxID_CANCEL
        btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { /* apply */ EndModal(wxID_OK); });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);
        // cancel cleanup that must also run on ESC / close box: after ShowModal() returns, or in wxEVT_CLOSE_WINDOW

        SetSizerAndFit(sizer);
        CenterOnParent();                               // after fitting: DPIAware centred the empty window
        wxGetApp().UpdateDlgDarkUI(this);               // last, after every child exists
    }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        // msw_rescale() ScalableBitmaps and re-SetBitmap() them, Rescale() Orca widgets, re-apply FromDIP/em
        // sizes (no msw_buttons_rescale(): it would override the DialogButtons' style height), then:
        GetSizer()->SetSizeHints(this);
        Refresh();
    }
};

MyDialog dlg(this);                                     // modal on the caller's stack
if (dlg.ShowModal() == wxID_OK) { /* read results */ }
```

## Orca widgets at a glance

| Orca widget (`src/slic3r/GUI/Widgets/`) | Instead of | Must know |
|---|---|---|
| `Button` | `wxButton` | ctor `(parent, text, icon_name, style, iconSize, id)` — id is last; style with `SetStyle(ButtonStyle::…, ButtonType::…)`; emits `wxEVT_BUTTON`; not a `wxButton` (no dialog default/escape emulation) |
| `::CheckBox` | `wxCheckBox` | no label parameter — pair with a `wxStaticText`/`Label`; emits `wxEVT_TOGGLEBUTTON` (never `wxEVT_CHECKBOX`), and a handler bound on it must `Skip()` or the bitmap is not refreshed |
| `::ComboBox` (+ `DropDown`) | `wxComboBox`/`wxChoice` | `wxCB_READONLY` for choices; `SelectAndNotify(n)` fires the event; ignores the ctor id; `void*` client data only |
| `::TextInput` | single-line `wxTextCtrl` | text via `GetTextCtrl()`; bind `wxEVT_TEXT_ENTER`/`wxEVT_KILL_FOCUS` on the widget, never on a parent |
| `SpinInput` | `wxSpinCtrl` | non-negative integers only (`-` cannot be typed); commits on Enter, kill-focus and arrow steps with `wxEVT_SPINCTRL`, a plain `wxCommandEvent` — bind a `wxCommandEvent&` handler and read `GetValue()` |
| `DialogButtons` | `wxStdDialogButtonSizer` | untranslated labels (custom ones written `L("…")`); OK/Cancel close by id, Yes/No/Apply/Confirm do not |
| `Label` | `wxStaticText` | `LB_AUTO_WRAP`, `LB_HYPERLINK` (only through `SetWindowStyleFlag`); also hosts the font table `Label::Head_*`/`Label::Body_*` |
| `SwitchButton`, `RadioGroup`, `TabCtrl`, `LabeledStaticBox`, `StaticLine`, `ProgressBar`, `PopupWindow` | toggle, `wxRadioBox`, notebook bar, `wxStaticBox`, `wxStaticLine`, `wxGauge`, `wxPopupTransientWindow` | quirks per widget in `orca-widgets.md` |

Disabling a parent does not repaint Orca widgets as disabled — enable/disable them individually (`::CheckBox`, a native
button, is the exception: it greys with its parent).
Containers stay raw (`wxPanel`, `wxBoxSizer`, `wxScrolledWindow`, `wxSimplebook`).

## Where to read next

| You are … / the symptom is … | Read |
|---|---|
| creating or closing a dialog or frame, `Destroy`/`delete`, modal results, liveness of a window pointer, message boxes | `references/windows-dialogs.md` |
| binding or emitting events, `Skip()`, propagation, custom events, `CallAfter`, `UPDATE_UI`, idle | `references/events.md` |
| worker-thread callbacks, background jobs, timers, `wxYield`/nested loops, progress dialogs, startup/shutdown | `references/threads-timers-app.md` |
| a sizer, a dialog that is collapsed, too big or clipped, a scrolled list, label wrapping, relayout after DPI change | `references/sizers-layout.md` |
| a custom-drawn control or a new widget in `Widgets/`, flicker, paint/erase, `wxGCDC`, best size | `references/painting-custom-widgets.md` |
| `FromDIP`/`em_unit`, rescale on DPI change, icons and bitmaps, image lists, fonts | `references/dpi-bitmaps-fonts.md` |
| colours, dark mode, theme toggle, `StateColor`, icons invisible in dark mode | `references/colours-dark-mode.md` |
| drag gestures and mouse capture, a frozen/unclickable UI on macOS, hover, wheel, keyboard shortcuts, focus, tooltips, cursors | `references/mouse-keyboard-focus.md` |
| popups and dropdowns (closing at once, not closing), context menus, the menu bar, `MenuFactory` | `references/popups-menus.md` |
| text/combo/check/radio/spin controls, book controls, `wxGrid`, `wxDataViewCtrl`, the object list | `references/controls-dataview.md` |
| WebView pages and dialogs, JS ↔ C++ messages, the GL canvas, ImGui overlays, docking panes, the top bar, camera view | `references/webview-gl-aui-media.md` |
| translations, string conversion and formatting, file/dir dialogs, clipboard, drag and drop, logging, `AppConfig` | `references/strings-i18n-files.md` |
| where new code goes, `GUI_App`/`MainFrame`/`Plater` structure, lazy pages, Preferences, notifications | `references/orca-architecture.md` |
| which Orca widget to use and its quirks | `references/orca-widgets.md` |
| adding or changing a print/filament/printer setting, a settings field, per-object overrides | `references/orca-settings-ui.md` |
| platform `#ifdef`s, wx build options, Wayland gaps, title bars, a bug on one platform only | `references/platforms.md` |
| old code, a wx call that behaves differently than you remember, wx-version migration | `references/wx-33-changes.md` |

**Reviewing a GUI diff:** for each area the diff touches, check it against that file's `## Rules`
list. **Debugging a UI bug:** find the symptom in the table above; most recurring Orca UI bugs are a
known class with a pitfall entry and a fixing commit.

## Platform gotchas worth memorising

- **macOS:** capture-lost is never sent (a leaked capture freezes all clicks); transient popups hover-
  dismiss across a gap — anchor flush and re-verify the cursor; native modals (file/dir dialogs, native
  message boxes) and generic progress dialogs re-activate the main window, so re-raise a secondary window
  afterwards with a deferred, liveness-guarded `Raise()` — but never `Raise()` a `wxPopupWindow`, which makes
  it the key window; a live menu accelerator consumes the key before
  any wx key event; Control+click arrives as a right-click.
- **Windows:** `IsDark()` and `wxSYS_COLOUR_*` follow the system app mode, not Orca's theme — use
  `dark_mode()`; menu bitmaps follow `check_dark_mode()`; windows are not double-buffered by default in
  3.3.2; `ProcessLeftDown` is never called for popups; a popup that must close with the frame calls
  `BindUnfocusEvent()`.
- **Linux GTK3:** dialogs without size hints collapse (only sizer-fitting calls, not `Fit()`, are
  replayed at the first `Show()`); command events from a popup's children are not stopped at the popup
  (MSW/macOS stop them); chained popups need `transient_for` set to the mapped parent right before
  showing; native borders leak through custom widgets (`RemoveButtonBorder`/`RemoveInputBorder`).
- **Wayland:** no global pointer position, no window positioning, `wxClientDC`, `Update()` and `SetIcon`
  do nothing, no floating AUI panes, GL is EGL only.

## Editing this skill

The skill describes how the pinned wxWidgets behaves and how Orca GUI code must be written, not what
the GUI tree currently contains. State rules, contracts, mechanisms, stable component designs and code
shapes; never usage counts, census lists, lists of today's offenders or dated measurements. Test each
sentence: if a change to Orca code the sentence does not name, with the wx pin unchanged, could make it
false, state the rule behind it or give a generic example instead, and cut it if there is no rule
behind it. Example files named as models to copy, and commit hashes cited as the reason for a rule,
are fine.

Keep the skill in step with what it describes. A change that alters a contract the skill states (an
Orca widget's API or quirk, a shared helper such as `DPIAware` or `UpdateDlgDarkUI`, the Shortcuts
registry) updates the skill in the same change. Moving the wx pin (`GIT_TAG` in
`deps/wxWidgets/wxWidgets.cmake`) means re-verifying every wx citation and **[source]** fact against
the new tree, and adding that version's behaviour changes the way `wx-33-changes.md` records 3.3's.

Cite wx by path relative to the wx tree root with line (`interface/wx/window.h:3805`), which stays
valid because the version is pinned. Cite Orca by file and symbol, never by line, and commits as the
rationale for a rule. Mark behaviour the wx docs don't state, or contradict, as **[source]**. Each
concept lives in one reference file and the others point to it. Verify every new claim in the wx tree
or the Orca code before adding it, and never drop a fact, qualifier or example to save words.
`evals/evals.json` holds the regression tasks (planted-defect review patches in `evals/files/`); rerun
them with and without the skill after substantial edits.
