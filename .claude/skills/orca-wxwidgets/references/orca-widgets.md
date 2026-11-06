# Orca widget library

The owner-drawn widgets in `src/slic3r/GUI/Widgets/`: which raw wx control each replaces, constructors, the events
each emits and where to bind them, style APIs, and the per-widget quirks behind real bugs. Read it before adding or
changing a control in Orca UI, or when a widget event "never fires", fires twice, or a widget looks wrong after a
DPI or theme change. Writing a *new* widget (StaticBox/StateHandler/render/Rescale) is in
`references/painting-custom-widgets.md`; `StateColor` semantics and the dark map in
`references/colours-dark-mode.md §StateColor`; the `Label` font table in `references/dpi-bitmaps-fonts.md`; raw wx
controls in `references/controls-dataview.md`.

Contents: [Rules](#rules) · [Why the library exists](#why-the-library-exists) ·
[Namespaces](#namespaces-and-the-checkbox-clash) · [Catalog](#catalog) · [Event semantics](#event-semantics) ·
[Custom vs raw](#custom-vs-raw) · [Shared lifecycle rules](#shared-lifecycle-rules) · [Button](#button) ·
[DialogButtons](#dialogbuttons) · [Label and HyperLink](#label-and-hyperlink) · [CheckBox](#checkbox-and-radiobox) ·
[SwitchButton family](#switchbutton-family) · [RadioGroup](#radiogroup) · [TextInput](#textinput) ·
[ComboBox and DropDown](#combobox-and-dropdown) · [SpinInput](#spininput-and-tempinput) · [Tab systems](#tab-systems) ·
[StaticBox, LabeledStaticBox, StaticLine](#staticbox-labeledstaticbox-staticline) · [ProgressBar](#progressbar) ·
[ScrolledWindow](#scrolledwindow) · [PopupWindow](#popupwindow) ·
[ProgressDialog, WebView, device composites](#progressdialog-webview-and-device-page-composites) ·
[Debugging widgets](#debugging-widgets)

## Rules

1. Interactive controls in new or modified UI are Orca widgets (`Button`, `::CheckBox`, `::ComboBox`, `::TextInput`,
   `SpinInput`, `SwitchButton`, `RadioGroup`, `TabCtrl`); never `wxButton`, `wxSpinCtrl`, `wxCheckBox`, `wxChoice` or
   a single-line `wxTextCtrl` in new code. → §Custom vs raw
2. Every dialog's bottom button row is `DialogButtons`. → §DialogButtons
3. Inside `namespace Slic3r::GUI` write `::CheckBox`, `::TextInput`, `::ComboBox` (also in `dynamic_cast` and
   forward declarations, which go at global scope). → §Namespaces
4. Call `Button::SetStyle(ButtonStyle, ButtonType)` on every `Button` you create; re-apply size/font overrides after
   `Rescale()`. → §Button
5. User handlers bound on a widget run **before** the widget's own handlers: `Skip()` in `wxEVT_TOGGLEBUTTON`
   handlers on `::CheckBox`/`SwitchButton`, in mouse handlers on `Button`, and in ENTER/KILL_FOCUS handlers on
   `GetTextCtrl()`, and take events by reference. → §Event semantics
6. Bind widget events on the widget itself (or by event type on an ancestor), not on an ancestor filtered by the
   widget's id: `ComboBox` ignores the id you pass, `TextInput`/`SpinInput` take none, and several events carry id 0
   or another window's id. → §Event semantics
7. Know which setters emit: `RadioGroup::SetSelection`, `MultiSwitchButton::SetSelection`, `TabCtrl::SelectItem`,
   `Notebook::SetSelection`, an editable `ComboBox::SetSelection` (as `wxEVT_TEXT`) and `SpinInput::SetValue` (as
   `wxEVT_TEXT` + `EVT_SPINCTRL_TEXT`) do. → §Event semantics
8. `::CheckBox` and `SwitchButton` emit `wxEVT_TOGGLEBUTTON`, never `wxEVT_CHECKBOX`. → §CheckBox and RadioBox
9. Set a container's background colour before creating widgets in it. → §Shared lifecycle rules
10. Enable/disable custom widgets individually; disabling an ancestor leaves them painted enabled (`::CheckBox`, a native button, greys with it). → §Shared lifecycle
    rules
11. Call each widget's `Rescale()` from `on_dpi_changed` (types qualified); `RadioGroup`, `Label`, `HyperLink` and
    `LabeledStaticBox` have none and `ProgressBar::Rescale()` does nothing. → §Shared lifecycle rules
12. Read and write `TextInput` text through `GetTextCtrl()` (`ChangeValue` for a silent update); bind
    `wxEVT_TEXT_ENTER`/`wxEVT_KILL_FOCUS` on the TextInput or its `GetTextCtrl()`, never on a parent. → §TextInput
13. Multi-line text is a raw `wxTextCtrl` with `wxTE_MULTILINE`, not `TextInput`. → §TextInput
14. `ComboBox`: `wxCB_READONLY` for choice semantics, `SelectAndNotify(n)` when listeners must react, `void*` client
    data only. → §ComboBox and DropDown
15. `SpinInput` is integer-only and cannot type `-`; set the range before the value; read `GetValue()` after the
    commit; bind `wxEVT_SPINCTRL` with a `wxCommandEvent&` handler. → §SpinInput and TempInput
16. `TabCtrl`'s `wxEVT_TAB_SEL_CHANGING` cannot veto; switching content is your job. → §Tab systems
17. Custom `DialogButtons` labels must exist in the translation catalog (write them as `L("…")`). → §DialogButtons
18. `ProgressBar` colours are painted unmapped and its colour-setter names are swapped; use `SetHeight` for thin bars.
    → §ProgressBar
19. Transient popups derive from `PopupWindow`; on MSW call `BindUnfocusEvent()` when the popup must close with the
    frame. → §PopupWindow
20. Children of a `LabeledStaticBox` are created with the box as parent; its label is fixed at `Create()`.
    → §StaticBox, LabeledStaticBox, StaticLine
21. Links are `HyperLink`, or a `Label` given `LB_HYPERLINK` through `SetWindowStyleFlag` (the constructor ignores it).
    → §Label and HyperLink

## Why the library exists

Native controls cannot carry Orca's flat, rounded, palette-coloured look, and they cannot follow all of Orca's
theming:
- On Windows the theme is an app-level setting (Preferences "Enable dark Mode", Windows-only) layered on
  `MSWEnableDarkMode`, and wx's MSW dark mode does not reach `TaskDialog()`-based dialogs (`wxMessageBox`,
  `wxMessageDialog`, `wxRichMessageDialog`, `wxProgressDialog`), the common dialogs or the date/time pickers
  (`interface/wx/app.h:1436-1445`). Hence the `MsgDialog` family (`references/windows-dialogs.md`) and Orca's own
  `ProgressDialog`.
- On macOS and Linux Orca follows the system appearance; native controls are themed by the toolkit, but any
  light palette colour set on them still needs the `UpdateDarkUI` pass (`references/colours-dark-mode.md`).
- GTK theme borders bleed through native controls wrapped inside an owner-drawn frame, so wrappers strip them with
  `Slic3r::GUI::RemoveInputBorder` (`TextInput`, `SpinInput`, `ComboBox`) or `RemoveButtonBorder` (`::CheckBox`,
  `SwitchButton`) under `__WXGTK__` (`GUI_Utils.cpp`, a CSS provider on GTK3).

Design that every widget shares:
- **Colours are data.** Most widgets derive from `StaticBox` (a `wxWindow` painting a rounded rect, border, optional
  vertical gradient and badge) whose colours are `StateColor`s resolved at paint time from state bits tracked by a
  pushed `StateHandler` and from the current dark flag, so painted colours follow a theme switch on the next
  `Refresh()`. `StaticBox::SetBackgroundColor(StateColor)` is the owner-drawn fill and is **not** wx's
  `SetBackgroundColour`. Details: `references/painting-custom-widgets.md`, `references/colours-dark-mode.md §StateColor`.
- **Events mirror wx.** Widgets emit the native event types (`wxEVT_BUTTON`, `wxEVT_TOGGLEBUTTON`, `wxEVT_COMBOBOX`,
  `wxEVT_SPINCTRL`, `wxEVT_RADIOBOX`) through `GetEventHandler()->ProcessEvent`, so command events propagate to
  parents like native ones (`docs/doxygen/overviews/eventhandling.h:536-552`) and stop at dialogs
  (`wxWS_EX_BLOCK_EVENTS`, `interface/wx/window.h:264-270`) and, on MSW and macOS, at popups ([source]
  `src/common/popupcmn.cpp:135` sets the same flag; wxGTK's `wxPopupWindow::Create` never calls it,
  `src/gtk/popupwin.cpp`). Propagation still works with the `StateHandler` pushed in front because the window's
  `TryAfter` forwards to the parent ([source] `src/common/wincmn.cpp:3499-3522`). The differences from native
  controls (ids, event objects, setter side effects) are in §Event semantics.

## Namespaces and the ::CheckBox clash

Widgets are in the **global namespace**, except these, which are in `Slic3r::GUI`: `DialogButtons`, `HyperLink`,
`ProgressDialog`, `RadioBox`, the `AMSControl`/`AMSItem` family, the `FanControl` family, `FilamentLoad`, the
`MultiNozzleSync` dialogs/tables, `SideTools`/`SideToolsPanel`, `WebViewHostDialog`, and the non-widget helpers of
`WebHosting.hpp` (namespace `Slic3r::GUI::web_hosting`) (check the header).

The `::` prefix matters because `Field.hpp` declares settings-field classes `Slic3r::GUI::CheckBox`, `TextCtrl`,
`SpinCtrl`, `Choice`, `StaticText` (plus `ColourPicker`, `PointCtrl`, `SliderCtrl`). Inside `namespace Slic3r::GUI`,
once `Field.hpp` is reachable (through `OptionsGroup.hpp`, `Tab.hpp`, …), unqualified `CheckBox` names the Field
class, which is not a `wxWindow`. `TextCtrl` also names the MSW `wxTextCtrl` subclass/typedef in
`Widgets/TextCtrl.h`.

- **Rule:** Qualify global widget types inside `Slic3r::GUI`, especially in `dynamic_cast`.
  **Why:** `dynamic_cast<CheckBox*>(child)` compiles (the Field class has `msw_rescale()`), but never matches a
  window, so the walk silently skips every `::CheckBox`. `PreferencesDialog::on_dpi_changed` has this shape — copy
  its child walk, not its unqualified casts.
  ```cpp
  // Wrong (in namespace Slic3r::GUI):
  else if (auto* chk = dynamic_cast<CheckBox*>(child)) chk->msw_rescale();   // Field class: never matches
  // Right:
  else if (auto* chk = dynamic_cast<::CheckBox*>(child)) chk->Rescale();
  ```
- **Rule:** Forward-declare global widgets at global scope, before `namespace Slic3r {`.
  **Why:** `class Button;` inside `namespace Slic3r::GUI` declares a distinct, never-defined `Slic3r::GUI::Button`;
  members of that type cannot hold a `::Button*` and calls on them do not compile.
  ```cpp
  class Button; class ComboBox;            // Right: global, as MultiNozzleSync.hpp does
  namespace Slic3r { namespace GUI {
  class MyPanel : public wxPanel { ::Button* m_ok{nullptr}; ::ComboBox* m_combo{nullptr}; };
  }}
  ```

## Catalog

| Widget (header) | Replaces | Base | Constructor | Must-know |
|---|---|---|---|---|
| `Button` | `wxButton`, `wxBitmapButton`, `ScalableButton` | `StaticBox` | `Button(parent, text, icon = "", style = 0, iconSize = 0, id = wxID_ANY)` | icon = SVG **name** from `resources/images/` (no path/extension), default 20 px; **id is last**; style with `SetStyle(...)`; emits `wxEVT_BUTTON`. §Button |
| `DialogButtons` (`Slic3r::GUI`) | `wxStdDialogButtonSizer`, `CreateStdDialogButtonSizer` | `wxPanel` | `DialogButtons(parent, {non-translated labels}, primary_translated_label = "", left_aligned_count = 0)` | labels → stock ids; styles primary/alert; self-rescales. §DialogButtons |
| `Label` | `wxStaticText` | `wxStaticText` | `Label(parent, text = "", style = 0, size)` (font `Body_14`), `Label(parent, font, text, style, size)` | `LB_AUTO_WRAP`, `LB_PROPAGATE_MOUSE_EVENT`, `LB_HYPERLINK` (via `SetWindowStyleFlag`); hosts the font table. §Label |
| `HyperLink` (`Slic3r::GUI`) | `wxHyperlinkCtrl` | `wxStaticText` | `HyperLink(parent, label = "", url = "", style = 0)` | opens `url` with `wxLaunchDefaultBrowser` on left-down. §Label |
| `::CheckBox` | `wxCheckBox` | `wxBitmapToggleButton` | `CheckBox(parent, id = wxID_ANY)` — **no label** | emits `wxEVT_TOGGLEBUTTON`; `SetHalfChecked`; `Rescale()`. §CheckBox |
| `SwitchButton` | on/off `wxCheckBox`, `wxToggleButton` | `wxBitmapToggleButton` | `SwitchButton(parent = nullptr, id = wxID_ANY)` | `SetLabels(on, off)`; track/thumb/text `StateColor`s; emits `wxEVT_TOGGLEBUTTON`. §SwitchButton family |
| `MultiSwitchButton` | segmented `wxRadioBox` | `StaticBox` | `MultiSwitchButton(parent, id, pos, size, style)` | emits `wxCUSTOMEVT_MULTISWITCH_SELECTION`, also from `SetSelection`. §SwitchButton family |
| `RadioGroup` | `wxRadioBox`, `wxRadioButton` rows | `wxPanel` | `RadioGroup(parent, std::vector<wxString> labels, wxHORIZONTAL/wxVERTICAL, row_col_limit = -1)` | emits `wxEVT_RADIOBOX` from **every** `SetSelection`. §RadioGroup |
| `::TextInput` | single-line `wxTextCtrl` | `wxNavigationEnabled<StaticBox>` | `TextInput(parent, text, label = "", icon = "", pos, size, style)` | value API on `GetTextCtrl()`; `label` is a painted side label. §TextInput |
| `::ComboBox` | `wxComboBox`, `wxChoice`, `wxBitmapComboBox` | `wxWindowWithItems<TextInput, wxItemContainer>` | `ComboBox(parent, id, value = "", pos, size, n = 0, choices = NULL, style = 0)` | `wxCB_READONLY` = choice; `SelectAndNotify`; `void*` client data only. §ComboBox |
| `DropDown` | native combo popup, menu used as a list | `PopupWindow` | `DropDown(parent, std::vector<Item>& items, style = 0)` | holds `items` **by reference**; `Invalidate()` after edits. §ComboBox |
| `SpinInput` | `wxSpinCtrl` | `wxNavigationEnabled<StaticBox>` | `SpinInput(parent, text, label = "", pos, size, style, min = 0, max = 100, initial = 0, step = 1)` | integer, non-negative typing; commits on Enter/kill-focus. §SpinInput |
| `TempInput` | temperature `wxTextCtrl` (device pages) | `wxNavigationEnabled<StaticBox>` | `TempInput(parent, type, text, TempInputType, label, normal_icon, active_icon, pos, size, style)` | warning icon + too-high/too-low states; posts `wxCUSTOMEVT_SET_TEMP_FINISH` to its **parent**. §SpinInput |
| `TabCtrl` | `wxNotebook` tab strip | `StaticBox` | `TabCtrl(parent, id, pos, size, style)` | one `Button` per tab; you switch the content. §Tab systems |
| `Notebook` (`GUI/Notebook.hpp`) | `wxNotebook` | `wxBookCtrlBase` | `Notebook(parent, id, pos, size, side_tools = NULL, style = 0)` | MainFrame's main tabs; pages addressed by name. §Tab systems |
| `LabeledStaticBox` | `wxStaticBox` | `wxStaticBox` | `LabeledStaticBox(parent, label = "", pos, size, style)` | usable as a `wxStaticBoxSizer` box; label fixed at `Create()`. §StaticBox… |
| `StaticBox` | plain bordered `wxPanel` | `wxWindow` | `StaticBox(parent, id, pos, size, style)` | rounded group frame; base of most widgets; `ShowBadge(bool)`. §StaticBox… |
| `StaticLine` | `wxStaticLine` | `wxWindow` | `StaticLine(parent, vertical = false, label = {}, icon = {})` | H/V separator with optional label + icon; `SetLineColour`. Avoid `wxStaticLine`. §StaticBox… |
| `ProgressBar` | `wxGauge` | `wxWindow` | `ProgressBar(parent, id = wxID_ANY, max = 100, pos, size, shown = false)` | plain `wxColour`s, unmapped; `Disable(wxString)`. §ProgressBar |
| `ScrolledWindow` + `MyScrollbar` | `wxScrolledWindow` with slim bars | `wxScrolled<wxWindow>` | `ScrolledWindow(parent, id, pos, size, style, marginWidth = 0, scrollbarWidth = 4, tipLength = 0)` | content on `GetPanel()`; vertical use. §ScrolledWindow |
| `PopupWindow` | `wxPopupTransientWindow` | `wxPopupTransientWindow` | `PopupWindow(parent, style = wxBORDER_NONE)` | per-platform dismissal hooks; MSW part opt-in. §PopupWindow |
| `ProgressDialog` (`Slic3r::GUI`) | `wxProgressDialog` | `wxDialog` | `ProgressDialog(title, message, maximum = 100, parent = NULL, style = wxPD_APP_MODAL \| wxPD_AUTO_HIDE, adaptive = false)` | styled copy of the generic dialog. §ProgressDialog… |
| `WebView` | `wxWebView::New` | static helpers | `WebView::CreateWebView(parent, url)` | see `references/webview-gl-aui-media.md`. §ProgressDialog… |
| `CheckList` | `wxCheckListBox` | `wxWindow` | `CheckList(parent, choices, scroll_style = wxVSCROLL)` | filterable multi-select list (`MultiChoiceDialog`). |

Field classes (`Field.cpp`) build their editors from these widgets (`TextCtrl` → `::TextInput`, `CheckBox` →
`::CheckBox`, `SpinCtrl` → `SpinInput`, `Choice` → `::ComboBox`); the field machinery is in
`references/orca-settings-ui.md`.

## Event semantics

| Widget | Emits on user action | Id / event object | Programmatic setters | Bind |
|---|---|---|---|---|
| `Button` | `wxEVT_BUTTON` (on release inside, Space/Enter) | button id / button | `SetValue(bool)` (Checked look) silent | on the button (Rule 6) |
| `::CheckBox`, `SwitchButton` | `wxEVT_TOGGLEBUTTON` | native id / widget | `SetValue`, `SetHalfChecked` silent (`interface/wx/tglbtn.h:98`) | on the widget; never `wxEVT_CHECKBOX` |
| `::ComboBox` | `wxEVT_COMBOBOX` (int = index, string = text) from popup pick, arrow keys (read-only combo), `SelectAndNotify`; `wxEVT_COMBOBOX_DROPDOWN`/`_CLOSEUP` | COMBOBOX: combo's auto id / combo; DROPDOWN/CLOSEUP: **id 0, no object** | `SetSelection`, `SetValue` silent for COMBOBOX; editable or `CB_NO_TEXT`: they send `wxEVT_TEXT` | on the combo |
| `::TextInput` | inner `wxEVT_TEXT` (propagates); `wxEVT_TEXT_ENTER`, `wxEVT_KILL_FOCUS` re-sent to the wrapper only | TEXT: **inner** id/object; ENTER/KILL_FOCUS: wrapper id, inner object | `GetTextCtrl()->SetValue` sends `wxEVT_TEXT`; `ChangeValue` silent | TEXT: wrapper, inner, or ancestor by type; ENTER/KILL_FOCUS: wrapper or inner |
| `SpinInput` | `wxEVT_SPINCTRL` (a plain `wxCommandEvent`, no int) on commit; `EVT_SPINCTRL_TEXT` (int + string) per parseable keystroke; inner `wxEVT_TEXT` | spinner id / spinner | `SetValue` sends `EVT_SPINCTRL_TEXT` + `wxEVT_TEXT`, no `wxEVT_SPINCTRL` | on the spinner; handler takes `wxCommandEvent&` |
| `RadioGroup` | `wxEVT_RADIOBOX` (int, string) | group id / **no object** | **every** `SetSelection(i)` emits, same index included | on the group |
| `TabCtrl` | `wxEVT_TAB_SEL_CHANGING` (int = old index, not vetoable), then `wxEVT_TAB_SEL_CHANGED` (int = new) | ctrl id / ctrl | `SelectItem(i)` emits both when `i` changes | on the ctrl |
| `Notebook` | tab click posts `wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED` (id = page index), then `wxEVT_NOTEBOOK_PAGE_CHANGING`/`_CHANGED` | — | `SetSelection` emits PAGE_*; `ChangeSelection` silent | on the notebook |
| `MultiSwitchButton` | `wxCUSTOMEVT_MULTISWITCH_SELECTION` (int, string) | id / widget | `SetSelection` emits when the index changes | on the widget |
| `SwitchBoard` | `wxCUSTOMEVT_SWITCH_POS` (int: 1 = left half, 0 = right half), **posted** | id 0 / none | — | on the board |
| `ModeSwitchButton` | none — writes the app mode (`wxGetApp().save_mode`) | — | `SetSelection` silent | — |
| `DropDown` (standalone) | `wxEVT_COMBOBOX` (int, string), `EVT_DISMISS` | COMBOBOX: popup id / popup; EVT_DISMISS: id 0, no object | — | on the DropDown (popups block propagation on MSW/macOS, not on GTK) |
| `TempInput` | `wxCUSTOMEVT_SET_TEMP_FINISH` on commit, **posted to the parent** (int = the ctor's `type`, string = the `TempInputType` number) | id 0 / none | — | on the **parent**; tell inputs apart by the int |

Consequences that recur:
- **Ids.** `ComboBox` calls `TextInput::Create`, which takes no id and creates the window with `wxID_ANY`, so the
  combo's `id` argument is ignored; `TextInput` and
  `SpinInput` have no id parameter. `parent->Bind(wxEVT_COMBOBOX, h, ID_MY_COMBO)` never fires. Bind on the widget,
  or call `SetId()` after construction if an id filter is unavoidable.
- **Handler order.** Dynamic handlers run most-recently-bound first, before static event tables
  (`interface/wx/event.h:592-593`). Widgets bind their internal handlers in the constructor (or use event tables),
  so yours run first: a handler that does not `Skip()` cuts off the widget's own behaviour (bitmap refresh, commit,
  click). A handler taking the event **by value** cannot `Skip()` the real event (`references/events.md §Skip
  discipline`).
- **Setters vs wx.** wx's rule is that setters are silent (`interface/wx/ctrlsub.h:102-103`); `wxTextEntry::SetValue`
  and `wxBookCtrlBase::SetSelection` are the documented exceptions (`references/controls-dataview.md §Events from
  programmatic changes`). The table's emitting setters re-enter change handlers synchronously inside model→view
  refreshes: guard with a flag or use the silent variant.

## Custom vs raw

Rules:
1. Interactive controls in new or modified UI: Orca widgets, always. Bottom button rows: `DialogButtons`. When you
   modify an older dialog built from raw controls, convert the controls you touch; untouched raw controls are not a
   model.
2. Static text: plain `wxStaticText` with a `Label::Body_*` font is fine; use `Label` for auto-wrap and the hyperlink
   look, `HyperLink` for a link that opens a URL. Fonts come from the `Label` statics (`Head_10…Head_48` bold for
   titles, `Body_8…Body_16` for content, `Body_14` the dialog default); never hardcode point sizes, because
   `Label::sysFont` already scales them per platform (`references/dpi-bitmaps-fonts.md`).
3. Containers stay raw: `wxPanel`, `wxBoxSizer`, `wxScrolledWindow`, `wxSimplebook`. `StaticBox`/`LabeledStaticBox`
   only for the rounded-border group look; `ScrolledWindow` only for the slim-scrollbar look.
4. If a raw control is unavoidable, make it dark-safe: `wxGetApp().UpdateDarkUI(ctrl)` / `UpdateDlgDarkUI(dlg)` or
   explicit `StateColor::darkModeColorFor()` (`references/colours-dark-mode.md`).
5. Every custom widget inside a `DPIDialog`/`DPIFrame` gets its `Rescale()` called from `on_dpi_changed`
   (§Shared lifecycle rules).

Mixed is normal: `CloneDialog::CloneDialog` pairs raw `wxStaticText` labels with `SpinInput`, `::CheckBox`,
`ProgressBar` and `DialogButtons` (copy its layout, not its OK handler, which runs a `wxYield()` loop inside a frozen
plater).

No Orca replacement exists — use raw wx (`references/controls-dataview.md`) for: multi-line text (`wxTextCtrl` +
`wxTE_MULTILINE`, as Field does for multi-line options), headerless page stacks (`wxSimplebook`), data views and grids
(`wxDataViewCtrl`, `wxGrid`), `wxSlider`, `wxColourPickerCtrl`, `wxSplitterWindow`, `wxStaticBitmap`, and the
`Slic3r::GUI::BitmapComboBox` wrapper where a `wxBitmapComboBox` is required.

## Shared lifecycle rules

**Background snapshot.** `StaticBox::Create`, `Label`, `SwitchButton` (through `StaticBox::GetParentBackgroundColor`:
a parent `StaticBox`'s default fill — the midpoint for a gradient — else `parent->GetBackgroundColour()`), and
`::CheckBox`, `RadioGroup` (`parent->GetBackgroundColour()`) copy the parent's background colour at construction;
wx itself never inherits a background colour ([source] `src/common/wincmn.cpp:1543-1552`).
- **Rule:** Set the container's background before creating widgets in it.
  ```cpp
  auto* panel = new wxPanel(this);
  auto* cb = new ::CheckBox(panel);          // Wrong: snapshots the panel's default colour
  panel->SetBackgroundColour(*wxWHITE);
  // Right: SetBackgroundColour first, then create children
  ```

**Enable state.** A parent's `Enable()`/`Disable()` never calls a child's virtual `Enable()`: on MSW and macOS it
reaches children through `NotifyWindowOnEnableChange` → `DoEnable()`, on GTK the toolkit propagates sensitivity
natively ([source] `src/common/wincmn.cpp:1147-1201`). Orca widgets update their `Enabled` state bit only from
`EVT_ENABLE_CHANGED`, which their `Enable()` override emits — so after `panel->Disable()` they ignore input
(`IsEnabled()` is false, `interface/wx/window.h:3060-3069`) but the `StateColor`-painted ones (`Button`, `TextInput`,
`ComboBox`, `SpinInput`, `RadioGroup`'s labels, …) still paint enabled colours. [source] `::CheckBox`, a native
`wxBitmapToggleButton`, greys with its parent on every port: MSW `EnableWindow`s it and the owner-drawn button paints
its disabled bitmap (`src/msw/window.cpp:576-593`, `src/msw/anybutton.cpp:846-871`, `:969-972`, `:1502`); GTK3's
button image draws a greyed copy of the current bitmap while `!IsEnabled()` (`src/gtk/image_gtk.cpp:37-42`), though
the disabled bitmap itself needs `IsThisEnabled()` false (`src/gtk/anybutton.cpp:140-146`); macOS sends
`setEnabled:NO` (`src/osx/cocoa/window.mm:3784-3791`) and AppKit dims the image (`NSButtonCell`
`imageDimsWhenDisabled`, default YES).
- **Rule:** Enable/disable each custom widget directly (`RadioGroup::Enable` does this for its own buttons).
  ```cpp
  m_options_panel->Enable(on);                                                // Wrong: widgets keep the enabled look
  for (wxWindow* w : std::initializer_list<wxWindow*>{m_combo, m_spin, m_check})
      w->Enable(on);                                                          // Right: virtual Enable() per widget
  ```

**DPI.** Widgets size themselves from `FromDIP` values and cached bitmaps; after a DPI change the owning
`DPIDialog::on_dpi_changed` must call `Rescale()` on each (`Button`, `::CheckBox`, `::TextInput`, `::ComboBox`
(also rescales its `DropDown`), `SpinInput`, `SwitchButton`, `MultiSwitchButton`, `TabCtrl`, `StaticLine`,
`ModeSwitchButton`). Exceptions: `RadioGroup`, `Label`, `HyperLink` and `LabeledStaticBox` (its scale is fixed in
`Create()`) have no `Rescale()`; `ProgressBar::Rescale()` is empty; `DialogButtons` rescales itself. Pass sizes through
`FromDIP` (`references/dpi-bitmaps-fonts.md`).
```cpp
void MyDialog::on_dpi_changed(const wxRect&) {
    m_ok_btn->Rescale();  m_combo->Rescale();  m_check->Rescale();   // ::CheckBox has Rescale(), not msw_rescale()
    m_combo->SetMinSize(wxSize(FromDIP(160), -1));                    // re-apply explicit sizes
    GetSizer()->SetSizeHints(this); Refresh();                        // resize + new minimum (sizers-layout.md)
}
```

**Theme switch.** `StateColor`s follow the dark flag at paint time. What was captured once does not: the wx
background copied at construction, colours baked into bitmaps (`SwitchButton` labels, `::CheckBox`/`RadioGroup`
SVGs), `ProgressBar` colours, `Label` foreground. The `Update*DarkUI` walk re-maps the wx colours among them that
are `gDarkColors` keys (`Label`'s `#262E30`, a palette background) when it reaches the widget, nothing else.
Re-apply them in `on_sys_color_changed()` (call `Rescale()` where
it re-reads colours, e.g. `SwitchButton`), and keep `UpdateDlgDarkUI(this)` as the last constructor line
(`references/colours-dark-mode.md §Runtime theme switch and re-applying colours`).

**Sizing.** No widget implements `DoGetBestSize`: the text/icon widgets set their min size in `messureSize()` whenever
a label, font, icon, style or DPI changes, and the bitmap toggles (`::CheckBox`, `SwitchButton`) size themselves
to their bitmap in `Rescale()`.
Nothing re-lays out the parent: call `Layout()` on the container after changing a widget's content at runtime
(`references/sizers-layout.md`).

**Mouse capture.** `Button`, `DropDown`, `SpinInput`'s arrows, `ModeSwitchButton`, `MyScrollbar`, `StepCtrl` and the
device-page `SideButton`, `ImageSwitchButton` and `AxisCtrlButton` capture the mouse while pressed; a capture leak
shows on macOS as a UI that is alive but unclickable (`references/mouse-keyboard-focus.md §Mouse capture`).

## Button

`Button` (`Widgets/Button.cpp`) is a `StaticBox` with label, optional icon (`ScalableBitmap` from an SVG name), focus
ring and an optional toggle look. `Create` sets `Label::Body_14` and measures; `messureSize()` sets the min size from
text + icon + padding, capped in width by `SetMaxSize` (the label then becomes the tooltip if none is set); a size
given to `Button::SetMinSize` is stored, floors the width and, when its height is > 0, replaces the measured height.

`SetStyle(ButtonStyle, ButtonType)` is the one call that makes it look like an Orca button — don't hand-roll button
colours in new code:

| `ButtonType` | Padding / min size / radius | Font | Use |
|---|---|---|---|
| `Compact` | padding 8×3, radius 8 | `Body_10` | tight spaces |
| `Window` | size and min 58×24, radius 12 | `Body_12` | buttons in windows, away from parameter boxes |
| `Choice` | min 100×32, padding 12×8, radius 4 | `Body_14` | dialog/window choice buttons (`DialogButtons` uses it) |
| `Parameter` | size and min 120×26, radius 4 | `Body_14` | buttons next to parameter boxes |
| `Icon` | padding 5×5, size and min 26×26, radius 4 | — | icon-only; create with `iconSize = 16` and a 16 px icon |
| `Expanded` | min height 32, padding 12×8, radius 4 | `Body_14` | full-width buttons, e.g. inside a static box |

All values go through `FromDIP`. `ButtonStyle::{Regular, Confirm, Alert, Disabled}` picks the background, border and
text `StateColor`s from the `btn_regular/btn_confirm/btn_alert/btn_disabled` tables in `Button.cpp` (palette colours
where the dark map should apply; the focus-border colour is chosen per theme when `SetStyle` runs). `ButtonStyle::Disabled` is a **look**; it does not call `Enable(false)`.

```cpp
auto* export_btn = new Button(this, _L("Export"));
export_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
export_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_export(); });

auto* reload_btn = new Button(parent, wxEmptyString, "refresh", 0, 16);   // icon-only (PreferencesDialog)
reload_btn->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
```

Behaviour (`Button::mouseDown`, `Button::mouseReleased`, `Button::keyDownUp`):
- Left-down focuses (if focusable) and captures; left-up releases and sends `wxEVT_BUTTON` (id = window id, event
  object = button) when the pointer is inside. Space/Enter synthesise down/up, so a focused Button clicks on key-up;
  on MSW it claims `WM_GETDLGCODE` so Enter reaches it instead of the dialog's default-button logic.
- Tab is turned into navigation; arrow keys are swallowed (`HandleAsNavigationKey` handles only Tab, [source]
  `src/common/wincmn.cpp:3566-3583`), which is why `DialogButtons` adds its own arrow handler.
- `SetValue(bool)`/`GetValue()` drive the `Checked` state bit (used by `TabCtrl` and `MultiSwitchButton`); it is
  visible only with `StateColor`s that have `Checked` entries — the `SetStyle` tables have none, the unstyled default
  does. Clicks do not toggle it.
- `SetCanFocus(false)` keeps it out of focus (`AcceptsFocus()` returns the flag). `EnableTooltipEvenDisabled()` shows
  the tooltip on a disabled button by watching the parent's motion — MSW only (elsewhere a no-op).
- `SetIndicator(bool)` draws a small dot after the label (TabCtrl uses it); `SetVertical`, `SetCenter`,
  `SetPaddingSize`, `SetIconSpacing`, `SetTextColor(StateColor)`, `SetIcon(name | wxBitmap)`.
- `Rescale()` re-rasterises a **named** icon (one set from a `wxBitmap` has no source and stays as is), re-measures and
  re-runs `SetStyle` with the stored style/type.

Pitfalls:
- **Rule:** Call `SetStyle` on every Button you create.
  **Why:** the unstyled defaults are wx stock colours (`*wxLIGHT_GREY` hover, `*wxBLACK` text) chosen for no Orca
  design and partly off the dark map, with generic metrics (padding 10×8, `StaticBox` radius 8, `Body_14`) instead of
  a `ButtonType`'s, and no focus border.
- **Rule:** Re-apply your own min size, size or font after `Rescale()`.
  **Why:** `Rescale()` re-runs `SetStyle`, which resets min size, padding, radius and font for the type.
  ```cpp
  btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
  btn->SetMinSize(wxSize(FromDIP(160), FromDIP(32)));     // lost on the next Rescale()…
  // Right: in on_dpi_changed: btn->Rescale(); btn->SetMinSize(wxSize(FromDIP(160), FromDIP(32)));
  ```
- **Rule:** A `wxEVT_LEFT_DOWN`/`_UP` handler bound on a Button must `Skip()`.
  **Why:** the click logic lives in the Button's static event table, which runs after dynamic handlers.
- **Rule:** Do not rely on Button's capture-lost handling to cancel a press.
  **Why:** `Button::mouseCaptureLost` replays release with a default `wxMouseEvent` at (0,0), which is inside the
  button, so losing capture mid-press **sends `wxEVT_BUTTON`**. The lost event comes on MSW and GTK and never on
  macOS ([source] `src/msw/window.cpp:5186`, `src/gtk/window.cpp:6808-6814`; `interface/wx/event.h:3507` says
  Windows only — `references/mouse-keyboard-focus.md §Per-port delivery`). wx's contract is to cancel the
  operation (`interface/wx/window.h:3815-3817`). `ModeSwitchButton::mouseCaptureLost` is the correct shape (clear
  the pressed flag, no action), minus its `Skip()` (a lost handler must not skip).

## DialogButtons

`Slic3r::GUI::DialogButtons` (`Widgets/DialogButtons.cpp`) is a `wxPanel` that builds the standard bottom row: one
`Button` per label, all `ButtonStyle::Regular` + `ButtonType::Choice`, gaps of `ButtonProps::ChoiceButtonGap()`.

```cpp
auto* dlg_btns = new DialogButtons(this, {"OK", "Cancel"});   // NOT pre-translated
dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); EndModal(wxID_OK); });  // no Skip()
sizer->Add(dlg_btns, 0, wxEXPAND);
```
The full dialog recipe (DPIDialog, `SetSizerAndFit`, `CenterOnParent`, `UpdateDlgDarkUI`) is in
`references/windows-dialogs.md`.

Constructor contract:
- **Labels** are untranslated; the constructor shows `_L(label)` and matches `label` lower-cased against a stock-id
  map: `ok`→`wxID_OK`, `yes`→`wxID_YES`, `apply` and `confirm`→`wxID_APPLY`, `no`→`wxID_NO`, `cancel`→`wxID_CANCEL`,
  `open`→`wxID_PRINT` (the map's first `"open"` entry wins), `add`, `copy`, `new`, `save`, `save as`, `refresh`,
  `retry`, `ignore`, `help`, `clone`/`duplicate`→`wxID_DUPLICATE`, `select all`, `replace`, `replace all`,
  `return`→`wxID_BACKWARD`, `next`→`wxID_FORWARD`, `remove`, `delete`, `abort`, `stop`, `reset`, `clear`,
  `exit`/`quit`→`wxID_EXIT`. Other labels keep an auto id.
- **Primary** (`ButtonStyle::Confirm`): the 2nd argument is a *translated* label (`_L("Create")`) naming it;
  empty → the only button if there is one, else the first present of `{wxID_OK, wxID_YES, wxID_APPLY, wxID_SAVE,
  wxID_PRINT}` in **numeric id order** (a `std::set`: `wxID_SAVE` < `wxID_PRINT` < `wxID_OK` < `wxID_APPLY` <
  `wxID_YES`), so `{"Save", "OK"}` makes Save primary. A label that matches no button means no primary. The primary
  takes focus only when nothing in the app has focus.
- **Alert** (`ButtonStyle::Alert`): only with ≥ 2 buttons, the first present (numeric order) of `wxID_EXIT`,
  `wxID_CLEAR`, `wxID_DELETE`, `wxID_RESET`, `wxID_ABORT`, `wxID_REMOVE`, `wxID_STOP`; `SetAlertButton(translated)`
  picks another.
- **`left_aligned_count`** pins the first N buttons to the left (`SetLeftAlignedButtonsCount` later).
- Getters: `GetOK/GetYES/GetAPPLY/GetCONFIRM (= wxID_APPLY)/GetNO/GetCANCEL/GetRETURN/GetNEXT/GetFIRST/GetLAST`,
  `GetButtonFromID`, `GetButtonFromLabel(translated)`, `GetButtonFromIndex`.

How clicks close the dialog: a click sends `wxEVT_BUTTON` with the stock id; unhandled, it propagates to
`wxDialogBase::OnButton` (`src/common/dlgcmn.cpp:105,455-478`): the affirmative id (`wxID_OK` by default) runs
`AcceptAndClose()` = `Validate()` + `TransferDataFromWindow()` then `EndDialog(wxID_OK)` (:369-375); `wxID_APPLY`
validates and transfers without closing; `wxID_CANCEL` → `EndDialog(wxID_CANCEL)`; any other id is skipped. So
OK/Cancel close by themselves; Yes, No, Confirm/Apply and custom labels do nothing until you bind them. wx's
ESC→button emulation looks for a real `wxButton` (`EmulateButtonClickIfPresent`, :387-404) and never finds an Orca
`Button`; ESC handling is in `references/windows-dialogs.md`.

DPI: the constructor binds the **parent's** `wxEVT_DPI_CHANGED` (unbound in the destructor) and `Skip()`s it; being
bound after `DPIAware`'s handler it runs first, then the dialog's `on_dpi_changed` — the dialog does not rescale
DialogButtons itself.

Pitfalls:
- **Rule:** A handler that calls `EndModal` must not `Skip()`; bind every button whose default handling is not what
  you want.
  **Why:** `Skip()` lets the event reach `wxDialogBase::OnButton`, which validates and transfers again and ends the
  dialog a second time with its own code (`EndDialog(affirmative id)` / `EndDialog(wxID_CANCEL)`). `EndDialog` only
  hides a dialog that no longer reports `IsModal()` (GTK, macOS reset it in the first `EndModal`), but on MSW
  `IsModal()` stays true until `ShowModal` returns, so `EndModal` runs again and replaces the return code you passed
  ([source] `src/common/dlgcmn.cpp` `wxDialogBase::EndDialog`, `include/wx/msw/dialog.h` `IsModal`;
  `references/windows-dialogs.md`). Yes/No/Confirm/custom buttons never close on their own.
- **Rule:** Every custom label must be in the translation catalog: write it as `L("Skip for Now")` in the vector, or
  make sure the same string appears in a `_L()` elsewhere.
  **Why:** the constructor's `_L(label)` translates a variable, which xgettext cannot see, so a label used only there
  never reaches `OrcaSlicer.pot` and always shows in English. `L()` is a no-op marker that xgettext extracts
  (`references/strings-i18n-files.md`).
  ```cpp
  new DialogButtons(this, {"Download and Install", "Skip for Now"});        // Wrong: untranslatable
  new DialogButtons(this, {L("Download and Install"), L("Skip for Now")});  // Right
  ```
- **Rule:** Don't call `UpdateButtons()` (or `SetLeftAlignedButtonsCount`) repeatedly in your code.
  **Why:** each call re-binds `wxEVT_KEY_DOWN` on every button (wx `Bind` does not de-duplicate) and the handler
  `Skip()`s, so arrow-key focus moves repeat once per accumulated binding; DPI changes already add one each.
- **Rule:** Don't expect the row to take the dialog's background.
  **Why:** the panel paints `darkModeColorFor("#FFFFFF")`, re-applied on every `UpdateButtons()` (DPI change); on a
  non-white dialog the row stands out.

## Label and HyperLink

`Label` (`Widgets/Label.cpp`) is a `wxStaticText` with font `Body_14` (or the font passed), foreground `#262E30` and
the parent's background. Style bits (no clash with `wxST_*`):

| Bit | Value | Effect |
|---|---|---|
| `LB_HYPERLINK` | `0x20` | underlined font, `#009688`, hand cursor — **only** when set through `SetWindowStyleFlag` |
| `LB_PROPAGATE_MOUSE_EVENT` | `0x40` | left-down/up are forwarded to the parent's handler with `ProcessEventLocally` (label-relative position, label as event object) and consumed |
| `LB_AUTO_WRAP` | `0x80` | `Label::Wrap(GetSize().x)` on every `wxEVT_SIZE` |

`Label::Wrap(width)` is Orca's own wrapper over the stored text and has no width cache, unlike 3.3.2's
`wxStaticText::Wrap` (`references/controls-dataview.md §wxStaticText`). `Label::split_lines(dc, width, text, out,
max_count)` wraps text for owner-drawn code. The static fonts (`Head_*`, `Body_*`) are built by
`Label::initSysFont()` from `GUI_App::on_init_inner` (`references/dpi-bitmaps-fonts.md`).

`Slic3r::GUI::HyperLink` (`Widgets/HyperLink.cpp`) is a `wxStaticText` with `Head_14` (kept underlined by its
`SetFont`), colour `#009687` — deliberately one off the palette `#009688` so the dark map leaves it alone — hover
`#26A69A`, hand cursor, the URL as tooltip, and `wxLaunchDefaultBrowser(url)` on left-down when the URL is non-empty.
Its `style` argument is unused.

Pitfalls:
- **Rule:** Apply `LB_HYPERLINK` through `SetWindowStyleFlag`, not the constructor; it is a look, not a link.
  **Why:** the constructor only stores the bit, and `SetWindowStyleFlag` returns early when the style is unchanged,
  so a label created with the bit never gets the look by re-applying it. Clicks still need your handler (or use
  `HyperLink`).
  On macOS the hyperlink label is set with `SetLabelMarkup`, so quote user text (`wxMarkupParser::Quote`).
  ```cpp
  new Label(this, _L("Learn more"), LB_HYPERLINK);                  // Wrong: stays plain text
  auto* lbl = new Label(this, _L("Learn more"));                    // Right
  lbl->SetWindowStyleFlag(lbl->GetWindowStyle() | LB_HYPERLINK);
  lbl->Bind(wxEVT_LEFT_DOWN, [url](wxMouseEvent&) { wxLaunchDefaultBrowser(url); });
  ```
- **Rule:** For a `HyperLink` with a custom action, leave the URL empty and bind `wxEVT_LEFT_DOWN`.
  **Why:** with a URL its own handler opens the browser; your later-bound handler runs first and would have to
  `Skip()` to keep it.

## CheckBox and RadioBox

`::CheckBox` (`Widgets/CheckBox.cpp`) is a `wxBitmapToggleButton` (`wxBORDER_NONE`) showing 18 px SVGs
`check_{on,half,off}`, `…_disabled`, `…_focused`. It has **no label**: pair it with a `wxStaticText`/`Label`
(as `CloneDialog::CloneDialog` does). `GetValue()` is a 2-state `bool`.
- Emits `wxEVT_TOGGLEBUTTON` on click (native). `SetValue` emits nothing (`interface/wx/tglbtn.h:98`).
- `SetHalfChecked(true)` is a drawn-only third state (`IsHalfChecked()` exists for the inspector only); the widget's own toggle handler
  clears it on any click. `SetValue` does **not** clear it — call `SetHalfChecked(false)` before showing a definite
  state.
- `Rescale()` (no `msw_rescale()`) re-rasterises all nine bitmaps and resets size/min size.
- Platform paths: macOS emulates the disabled/focused/hover bitmaps (`CheckBox::Enable` override,
  `DoGetBitmap`, `updateBitmap`), but wxOSX hands the `NSButton` `m_bitmaps[State_Current]`/`m_bitmaps[State_Normal]`
  directly and calls `DoGetBitmap` only for an `IsOk()` test ([source] `src/osx/anybutton_osx.cpp:84-94`), so only
  the hover (`_focused`) art shows and a disabled box shows its normal art dimmed by AppKit; MSW sets a focus bitmap;
  GTK strips the theme border.

`Slic3r::GUI::RadioBox` is an older single bitmap radio (`wxBitmapToggleButton`); use `RadioGroup` for new radio
sets.

Pitfalls:
- **Rule:** Bind `wxEVT_TOGGLEBUTTON`, never `wxEVT_CHECKBOX`, and `Skip()` in the handler.
  **Why:** `wxEVT_CHECKBOX` is never sent. The widget's own `wxEVT_TOGGLEBUTTON` handler (bound in the constructor)
  runs **after** yours and is what swaps the bitmap (`CheckBox::update`); without `Skip()` the value changes but the
  box keeps showing the old state.
  ```cpp
  cb->Bind(wxEVT_CHECKBOX, h);                                                   // Wrong: never fires
  cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { apply(); });          // Wrong: bitmap not refreshed
  cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { e.Skip(); apply(); }); // Right
  ```

## SwitchButton family

All in `Widgets/SwitchButton.hpp/.cpp`:
- **`SwitchButton`** — `wxBitmapToggleButton` (`wxBORDER_NONE | wxBU_EXACTFIT`), font `Body_12`. Without labels it
  shows the `toggle_on`/`toggle_off` SVGs; `SetLabels(on, off)` switches to a two-segment pill drawn into bitmaps from
  the track/thumb/text `StateColor`s (`SetTrackColor`, `SetThumbColor`, `SetTextColor`, `SetTextColor2`), narrowed
  to `GetMaxWidth()` by shrinking the font. Emits `wxEVT_TOGGLEBUTTON`; `SetValue` is silent; its own toggle handler
  (refreshes the bitmap) runs after yours — `Skip()`, as for `::CheckBox`. Every colour/label setter and
  `SetBackgroundColour` call `Rescale()`, which re-rasterises the SVGs or, with labels, re-reads the parent background
  and re-bakes the pill bitmaps with the current dark mapping: call it on DPI **and** theme change.
- **`ModeSwitchButton`** — the 3-position Simple/Advanced/Expert control (`StaticBox`, `doRender`), plus `SetDevMode`.
  It sends no event: a click calls `SelectAndNotify`, which writes the app mode through `wxGetApp().save_mode()` and
  is ignored in dev mode or when disabled. `SetSelection` is silent and clamps to 0..2.
- **`MultiSwitchButton`** — a segmented control of `Button`s in an internal `wxScrolledWindow` (scrolls instead of
  clipping when squeezed; `SetFitToOptions`). `AppendOption/SetOptions/DeleteAllOptions`, per-option text/client data,
  `GetButton(i)`. Emits `wxCUSTOMEVT_MULTISWITCH_SELECTION` (int = index, string = text) whenever the selection
  changes — **including programmatic `SetSelection`**.
- **`SwitchBoard`** — a two-label device-page switch (a plain `wxWindow`). It **posts** `wxCUSTOMEVT_SWITCH_POS`
  (int = 1 for a click on the left half, 0 for the right half; no id, no event object). Its `Enable()` only flips a
  private flag and repaints, and its non-virtual `IsEnabled()` hides the base one: the window stays enabled for wx.
  `SetAutoDisableWhenSwitch()` makes a click set that flag off until you re-enable it.

## RadioGroup

`RadioGroup` (`Widgets/RadioGroup.cpp`) is a `wxPanel` of `wxStaticBitmap` radio icons plus `Button` labels in a
`wxFlexGridSizer`; `row_col_limit` (−1 = one row for `wxHORIZONTAL`, one column for `wxVERTICAL`) wraps the items
into a grid — check the row/column arithmetic in `RadioGroup::Create` before relying on a layout. Keyboard:
Right/Down and Left/Up on the focused label move the selection with wrap-around (`SelectNext`/`SelectPrevious`);
only the selected label button is focusable, so Tab enters the group once.
`SetRadioTooltip(i, tip)`, `Enable()` (also enables the label buttons and emits `EVT_ENABLE_CHANGED`). There is no
`Rescale()`.

- **Rule:** Treat every `SetSelection(i)` as an event source.
  **Why:** it always sends `wxEVT_COMMAND_RADIOBOX_SELECTED` (= `wxEVT_RADIOBOX`), for programmatic calls and for the
  current index too — the opposite of `wxRadioBox::SetSelection` (`interface/wx/ctrlsub.h:102-103`). The constructor
  fires one before anyone can bind.
  ```cpp
  m_group->SetSelection(cfg.mode);                                            // Wrong: re-enters on_mode_changed
  { m_syncing = true; m_group->SetSelection(cfg.mode); m_syncing = false; }  // Right: handler returns if m_syncing
  ```
- **Rule:** Don't read `GetEventObject()` in its handler.
  **Why:** the event carries the group's id but no event object (null).

## TextInput

`::TextInput` (`Widgets/TextInput.cpp`, `TextInput::Create`) is a `wxNavigationEnabled<StaticBox>` frame around a
real `wxTextCtrl` (on MSW the `TextCtrl` subclass in `Widgets/TextCtrl.h`, which overrides `DoMSWControlColor`).
- `text` is the initial value; `label` is a **painted side label** (`Body_12`), `icon` a 16 px SVG name
  (`SetIcon`, `SetIcon_1` for a second icon, `SetStaticTips` for a grey hint line under the label).
- `style` goes to the inner control with `wxTE_PROCESS_ENTER | wxBORDER_NONE` added and alignment bits stripped;
  the wrapper keeps the alignment bits and uses them to place label and icons, so typed text is left-aligned unless
  you set `wxTE_RIGHT`/`wxTE_CENTRE` on `GetTextCtrl()` afterwards (alignment can change after creation on MSW, GTK
  and macOS, `interface/wx/textctrl.h:1398-1399`).
- Inner font `Body_14`, inner context menu disabled (empty `wxEVT_RIGHT_DOWN` handler), tooltips forwarded
  (`DoSetToolTipText`), `Enable()` also enables the inner control and recolours it.
- `TextInput::SetLabel()` changes the side label, not the text. The value API is `GetTextCtrl()->GetValue()/
  SetValue()/ChangeValue()`; `SetHint`, validators and `SetMaxLength` also go on `GetTextCtrl()`.

Event routing:
- `wxEVT_TEXT` is a command event from the inner control; it propagates to the wrapper and beyond, carrying the
  **inner** control's id and event object.
- `wxEVT_TEXT_ENTER` and `wxEVT_KILL_FOCUS` are caught on the inner control, `OnEdit()` runs (ComboBox resolves the
  typed text there), then they are re-sent with the wrapper's id via `ProcessEventLocally`, which runs the wrapper's
  own handlers but [source] never `TryAfter`, so never the parents (`src/common/event.cpp:1582-1589`; the
  `interface/wx/event.h:626-651` text says it calls `TryAfter`). The event object stays the inner control.
- The internal ENTER handler does not `Skip()`: Enter in a TextInput never activates a dialog's default button
  (`interface/wx/textctrl.h:1324-1331`).
- Key, char and focus-in events exist only on `GetTextCtrl()`.

Pitfalls:
- **Rule:** Bind ENTER and KILL_FOCUS on the TextInput (or `GetTextCtrl()`), never on a parent; bind TEXT on the
  input without an id filter.
  ```cpp
  dialog->Bind(wxEVT_TEXT_ENTER, &Dlg::on_enter, this, input->GetId());   // Wrong: never reaches the dialog
  input->Bind(wxEVT_TEXT_ENTER, &Dlg::on_enter, this);                    // Right
  panel->Bind(wxEVT_TEXT, h, input->GetId());                             // Wrong: TEXT carries the inner id
  input->Bind(wxEVT_TEXT, h);                                             // Right
  ```
  Cite: `TextInput::Create`; `src/common/event.cpp:1582-1589`.
- **Rule:** A handler bound on `GetTextCtrl()` for ENTER or KILL_FOCUS must `Skip()`.
  **Why:** it runs before the internal handler (most-recently-bound first), which does `OnEdit()` and the re-dispatch.
- **Rule:** Don't `static_cast` the event object to `TextInput*`.
  **Why:** every TextInput event's object is the inner `wxTextCtrl`.
- **Rule:** Use `ChangeValue` for model→view updates.
  **Why:** `wxTextEntry::SetValue` sends `wxEVT_TEXT` (`interface/wx/textentry.h:539-542`), [source] even for
  identical text (`src/common/textentrycmn.cpp:236-254`).
- **Rule:** Use a raw `wxTextCtrl` with `wxTE_MULTILINE` for multi-line text.
  **Why:** `TextInput::DoSetSize` sets only the inner control's width and keeps it vertically centred at the height
  it got at creation (its initial best size), so a multi-line TextInput never grows its text area. Hints on
  multi-line controls are ignored except on MSW and GTK2 (`interface/wx/textentry.h:485-486`), so not on macOS or
  GTK3; on macOS call `OSXDisableAllSmartSubstitutions()`
  on any control holding G-code, paths or URLs (`references/controls-dataview.md §wxTextCtrl`).

## ComboBox and DropDown

`::ComboBox` (`Widgets/ComboBox.cpp`) is `wxWindowWithItems<TextInput, wxItemContainer>` plus an owned `DropDown drop`
over its `std::vector<DropDown::Item> items`, so the `wxItemContainer` API (`Append/Insert/Set/Clear/Delete/
GetCount/GetString/FindString/GetSelection/SetSelection/GetStringSelection`) works, and `GetValue/SetValue` mirror the
wxTextEntry side of `wxComboBox`. `ComboBox::SetLabel/GetLabel` are the displayed value — the opposite of
`TextInput::SetLabel` — and `SetTextLabel/GetTextLabel` reach the painted side label.
- `wxCB_READONLY` hides the text control and paints the value (font `Body_14`, focused background `#E5F0EE`): choice
  semantics. Without it the combo is editable.
- Orca style flags: `CB_NO_DROP_ICON` (no arrow), `CB_NO_TEXT` (icon-only items).
- Per-item data (`DropDown::Item`): text, `icon` (list), `icon_textctrl` (shown in the closed combo),
  `text_static_tips`, `data` (`void*`), `group_key`/`group_label` (a group opens a sub-dropdown), `alias`, `tip`,
  `flag`, `style` = `DD_ITEM_STYLE_SPLIT_ITEM` (separator-style header), `DD_ITEM_STYLE_DISABLED` (not selectable by
  click), `DD_ITEM_STYLE_DIMMED` (grey, selectable). `Append(text, bitmap, group, clientData, item_style)` overloads,
  `SetItems(std::vector<DropDown::Item>)`, `SetItemTooltip/Alias/Bitmap`, `SetFlag`.
- `GetDropDown()` exposes the popup (`SetUseContentWidth(true[, limit])`, `SetAlignIcon`, colours).
  `SetKeepDropArrow(true)` keeps the arrow and shows the item icon as a second icon. `ForceDropdownOpen()` opens it
  programmatically (data-view editors use this).
- Opening: click (debounced by `DropDown::HasDismissLongTime()`, ≥ 20 ms since the last dismissal, so the click that
  dismissed the popup does not reopen it), Enter/Space. Up/Down/Left/Right on a focused read-only combo step the
  selection and send `wxEVT_COMBOBOX` (in an editable combo the keys go to the inner text control). Mouse-wheel selection is disabled (handler commented out of the event table). Scrolling an
  ancestor `wxScrollHelper` hides the popup.

Events: see §Event semantics. Picking an item sends `wxEVT_COMBOBOX` even when it is already selected. Typing into
an editable combo sends `wxEVT_TEXT` per keystroke and **no** `wxEVT_COMBOBOX`: the commit is `wxEVT_TEXT_ENTER`/
`wxEVT_KILL_FOCUS` on the combo, after `OnEdit()` has matched the text to an item (exact, case-sensitive) and re-set
it (one more `wxEVT_TEXT`).

Pitfalls:
- **Rule:** Use `SelectAndNotify(n)` when listeners must react; `SetSelection(n)` is silent for `wxEVT_COMBOBOX` and
  returns early when `n` is already selected.
  **Why:** matches wx (`interface/wx/ctrlsub.h:102-103`), but in an **editable** combo (or one with `CB_NO_TEXT`)
  `SetSelection`/`SetValue` go through `GetTextCtrl()->SetValue()` and send a propagating `wxEVT_TEXT`; guard
  `wxEVT_TEXT` handlers during sync.
- **Rule:** Store only untyped `void*` client data and free it yourself.
  **Why:** every `Append` overload calls `SetClientDataType(wxClientData_Void)`, and declaring them hides every base
  `wxItemContainer::Append` (the `wxClientData*` and `wxArrayString` ones included); `DeleteOneItem()` bypasses the
  base client-object reset. Typed `wxClientData` ownership (`references/controls-dataview.md §Item containers`) does
  not apply.
- **Rule:** After `Clear()`, `Insert()`, `Set()` or `Delete()`, call `SetSelection`/`SetValue`.
  **Why:** they reset the selection to -1 (`DropDown::Invalidate(true)`) but leave the shown text.
- **Rule:** Bind `wxEVT_COMBOBOX_DROPDOWN`/`_CLOSEUP` on the combo without an id filter.
  **Why:** they are created with id 0 and no event object; any ancestor bound without a filter receives them from
  every combo below it.

`DropDown` (`Widgets/DropDown.cpp`) standalone: a `PopupWindow` (`wxPU_CONTAINS_CONTROLS`, `wxBG_STYLE_PAINT`,
`wxBufferedPaintDC`) drawing `items`, which it holds **by reference** — the vector must outlive the popup, and
`Invalidate()` must follow any edit of it. `Popup()` on GTK sets the toplevel as transient parent explicitly (a
data-view editor can get focus before wxGTK infers one); `Dismiss()` refuses while its sub-dropdown is shown;
`OnDismiss()` sends `EVT_DISMISS` and stamps the dismissal time; `ShouldDismissOnTopWindowDeactivate()` keeps chained
dropdowns open on Wayland, where mapping a grabbing child popup deactivates the toplevel; on macOS it binds an empty
`wxEVT_IDLE` handler to stop wx's idle-time capture release/re-capture ([source] `src/common/popupcmn.cpp:108-111,
439-472`). It captures without a `HasCapture()` guard, and its capture-lost handler replays release, which can commit
the hovered item. Popup mechanics: `references/popups-menus.md`.

## SpinInput and TempInput

`SpinInput` (`Widgets/SpinInput.cpp`) is a `wxNavigationEnabled<StaticBox>` with an inner `TextCtrl` validated by
`wxTextValidator(wxFILTER_DIGITS)`, two arrow `Button`s (not keyboard-focusable) and a repeat `wxTimer`.
- `text` (if it parses) overrides `initial`; `label` is a painted side label; `style` goes to the inner control
  (`wxTE_PROCESS_ENTER` added).
- `SetValue(int|wxString)` clamps to `[min, max]` and stores; `GetValue()` returns the **last committed** value, not
  the text being typed; `SetRange(min, max)` stores the bounds without re-clamping; `SetStep`.
- Commits — parse, clamp, `wxEVT_SPINCTRL` — on Enter, on kill-focus and on Up/Down keys (which do not step past a
  bound) only if the value changed, but on **every** arrow-button press and auto-repeat tick (while held), even when
  the value is pinned at `min`/`max` (`SpinInput::createButton`, `SpinInput::onTimer`). Mouse-wheel stepping is
  disabled (handler commented out of the event table).
- `EVT_SPINCTRL_TEXT` (int + string) fires on every keystroke that leaves a parseable integer.
- Enter and kill-focus are re-dispatched to the spinner with `ProcessEventLocally`, as in TextInput. Unlike
  TextInput, `SpinInput::onTextLostFocus` never `Skip()`s the inner control's `wxEVT_KILL_FOCUS` (it skips a local
  copy), although focus handlers should (`interface/wx/event.h:3410-3412`); a `KILL_FOCUS` handler bound on
  `GetTextCtrl()` runs before it and must `Skip()` to keep the commit.

Pitfalls:
- **Rule:** Bind `wxEVT_SPINCTRL` with a `wxCommandEvent&` handler and read `GetValue()` from the spinner.
  **Why:** `wxEVT_SPINCTRL` is declared with `wxSpinEvent` (`include/wx/spinctrl.h:22`) but `SpinInput::sendSpinEvent`
  sends a plain `wxCommandEvent` without `SetInt`; a `wxSpinEvent&` handler reads `GetPosition()` == 0 from an object
  of the wrong type.
  ```cpp
  spin->Bind(wxEVT_SPINCTRL, [](wxSpinEvent& e) { use(e.GetPosition()); });           // Wrong
  spin->Bind(wxEVT_SPINCTRL, [spin](wxCommandEvent&) { use(spin->GetValue()); });     // Right
  ```
- **Rule:** Use `SpinInput` only for non-negative integer ranges.
  **Why:** `wxFILTER_DIGITS` rejects `-` (and `.`, `+`) (`interface/wx/valtext.h:43-52`); a negative minimum can be
  reached only with the arrow buttons or Up/Down keys.
- **Rule:** Set the range before the value.
  **Why:** `SetRange` does not re-clamp an existing value.
- **Rule:** Commit explicitly before reading when the user may still be typing.
  **Why:** `GetValue()` is the last committed value. Dialogs sometimes call `Disable()` to force the kill-focus
  commit (`CloneDialog`), which depends on the platform delivering `wxEVT_KILL_FOCUS` to the disabled focused child
  — not a wx contract. Parse `GetTextCtrl()->GetValue()` and call `SetValue` yourself when it matters.
- **Rule:** Expect `SetValue` to emit `EVT_SPINCTRL_TEXT` and a propagating `wxEVT_TEXT`.
  **Why:** it writes through the inner `wxTextCtrl::SetValue`.

`TempInput` (`Widgets/TempInput.cpp`, device pages) is a separate `wxNavigationEnabled<StaticBox>` with normal/active
icons, target and current temperatures (`SetTagTemp`, `SetCurrTemp`) and a warning state (`Warning(bool,
WARNING_TOO_HIGH | WARNING_TOO_LOW | WARNING_UNKNOWN)`); on commit it **posts** `wxCUSTOMEVT_SET_TEMP_FINISH` to its
**parent** (`TempInput::SetFinish`; int = the constructor's `type`, no id, no event object), so bind it on the parent
and tell inputs apart by the int. It guards re-entry with `m_on_changing`, because a handler that opens a dialog
moves focus and re-triggers the kill-focus commit — copy that guard for any commit-on-kill-focus widget whose
handler can show UI.

## Tab systems

Three distinct mechanisms; don't confuse them:

| | `TabCtrl` (`Widgets/TabCtrl.cpp`) | `Notebook` (`GUI/Notebook.hpp`) | `wxSimplebook` |
|---|---|---|---|
| What | a bare tab **bar**: one `Button` per tab in a `StaticBox` | `wxBookCtrlBase` with a `ButtonsListCtrl` header (one `Button` per tab) | standard stacked-page container, no UI |
| Content | **caller** shows/hides it | owns pages | owns pages |
| Events | `wxEVT_TAB_SEL_CHANGING` (int = old) → `wxEVT_TAB_SEL_CHANGED` (int = new), plain `wxCommandEvent`s | posted `wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED` (id = page) → `wxEVT_NOTEBOOK_PAGE_CHANGING`/`_CHANGED` | PAGE_CHANGING/CHANGED from `SetSelection` only (`interface/wx/simplebook.h:28-31`) |
| Use | settings-style category strips (Preferences) | MainFrame's main tabs (Home, Prepare, Preview, Device, …) | headerless page switching (`SelectMachine.hpp`, `ReleaseNote.cpp`, `StatusPanel`) |

`TabCtrl`: `AppendItem(text[, image, selImage, clientData])`, `AppendItem(text, bitmap)`, `DeleteItem`,
`DeleteAllItems`, `SelectItem(i)`/`Unselect()`, `GetSelection`, `SetItemText/Bitmap/Data`, `SetItemBold(i, bool)`,
`SetItemTextColour(i, StateColor)`, `SetItemIndicator(i, bool)` (dot after the label), `SetFont`, `Rescale()`.
`AssignImageList` is Orca's own method (takes ownership), not `wxWithImages`, and nothing draws from that list:
`AppendItem` ignores its `image`, `selImage` and `clientData` arguments — give icons with `AppendItem(text, bitmap)`/
`SetItemBitmap` and data with `SetItemData`. Call `SetFont` before `SetItemBold`: it derives the bold font that
`SetItemBold` applies. Selecting a tab flips the tab Buttons'
`Checked` state by sending them `wxEVT_CHECKBOX` command events (id 0, object = the tab `Button`); those propagate up
the parent chain, so an ancestor bound to `wxEVT_CHECKBOX` without an id/object filter sees them. `PreferencesDialog::
create` is the canonical usage:

```cpp
m_pref_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
m_pref_tabs->SetFont(Label::Body_14);                       // before SetItemBold
m_pref_tabs->AppendItem(_L("General"));                     // one per page (create_items)
m_pref_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent& e) {
    Freeze();
    const int sel = e.GetSelection();                       // GetInt()
    for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i) {
        m_pref_tabs->SetItemBold(i, int(i) == sel);
        f_sizers[i]->Show(int(i) == sel);                   // the caller switches content
    }
    Layout(); Thaw();
});
StateColor item_color(std::make_pair(wxColour("#6B6B6C"), (int) StateColor::NotChecked),
                      std::make_pair(wxColour("#363636"), (int) StateColor::Normal));
for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i) m_pref_tabs->SetItemTextColour(i, item_color);
m_pref_tabs->SelectItem(0);
```

- **Rule:** Don't try to veto a TabCtrl switch from `wxEVT_TAB_SEL_CHANGING`.
  **Why:** it is a plain `wxCommandEvent` and `TabCtrl::sendTabCtrlEvent` always returns true; `SelectItem` continues.

`Notebook`: pages are inserted and addressed by a stable id (`AddPage(id, page, text, bmp_name)`,
`InsertPage(n, id, page, text, bmp_name, bSelect)`, `FindPageByName(id)`, `SelectPageByName(id)`, `GetPageName(n)`,
`GetSelectedPageName()`); a page inserted unselected is hidden at once; one window may sit under two tabs (Prepare
and Preview share the `Plater`). A header click posts `wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED` to the notebook, whose own handler calls
`SetSelection(page)`; a handler bound later (MainFrame) runs first and must `Skip()` to let the switch happen.
`SetSelection` sends the vetoable PAGE_CHANGING and PAGE_CHANGED (`interface/wx/bookctrl.h:177-178`) and hides all
other pages; `ChangeSelection` is silent (`:194-195`). `wxEVT_BOOKCTRL_PAGE_CHANGED` is the same event type as
`wxEVT_NOTEBOOK_PAGE_CHANGED` in Orca's build (`include/wx/bookctrl.h:433-434`); `Notebook` sends the
`wxEVT_BOOKCTRL_*` names.

`wxSimplebook`: switch with `ChangeSelection()` (silent) or `SetSelection()` (events); pages must be created with
the book as parent; page titles are mnemonic-interpreted (`references/controls-dataview.md §Book controls`).

## StaticBox, LabeledStaticBox, StaticLine

`StaticBox` (`Widgets/StaticBox.cpp`) as a container: a rounded, bordered group frame. API: `SetCornerRadius`,
`SetBorderWidth`, `SetBorderColor(StateColor)`/`SetBorderColorNormal`, `SetBorderStyle(wxPenStyle)`,
`SetBackgroundColor(StateColor)`/`SetBackgroundColorNormal`, `SetBackgroundColor2` (vertical gradient),
`SetTopMargin`, `ShowBadge(bool)` (corner badge bitmap), static `GetParentBackgroundColor(parent)`. `wxBORDER_NONE`
makes the border 0. Its children inherit its fill through `GetParentBackgroundColor`. Authoring widgets on it:
`references/painting-custom-widgets.md`.

`LabeledStaticBox` (`Widgets/LabeledStaticBox.cpp`) is a real `wxStaticBox` subclass, so it works as the box of a
`wxStaticBoxSizer`; it paints a rounded border (`Head_14` label in the border gap, `#DBDBDB` border, white fill)
itself (`wxBG_STYLE_PAINT` except on macOS, where `staticbox_remove_margin` is applied and the
`GetBordersForSizer` override sets the side padding other platforms use). It is not focusable. `SetCornerRadius`, `SetBorderWidth`,
`SetBorderColor(StateColor)`, `SetFont`, `Enable`. There is no `StaticGroup` class.

- **Rule:** Create the controls inside the box as children of the box.
  **Why:** since 2.9.1 wx "strongly recommends" box children over siblings to avoid repaint problems
  (`interface/wx/statbox.h:16-24`); `sizer->GetStaticBox()` is the parent to use.
  ```cpp
  auto* box   = new LabeledStaticBox(this, _L("Network"));
  auto* sizer = new wxStaticBoxSizer(box, wxVERTICAL);
  sizer->Add(new ::CheckBox(box), 0, wxALL, FromDIP(5));      // parent = box
  ```
- **Rule:** To change the label, recreate the box.
  **Why:** the label is captured and measured in `Create()`; there is no `SetLabel` override, so `SetLabel` changes the
  native text the widget never paints.
- **Rule:** Don't expect a disabled look.
  **Why:** its `StateColor`s list `Normal` before `Disabled`, and the first match wins
  (`references/colours-dark-mode.md §StateColor`), so the disabled colours never apply.

`StaticLine` (`Widgets/StaticLine.cpp`): horizontal or vertical separator with optional label and icon;
`SetLineColour(wxColour)`, `SetLabel`, `SetIcon`, `Rescale()`; line and text colours go through `darkModeColorFor` at
paint time. Use it instead of `wxStaticLine`.

## ProgressBar

`ProgressBar` (`Widgets/ProgressBar.cpp`) is a `wxWindow` with a rounded track and fill (MSW draws through a memory
DC + `wxGCDC` for anti-aliasing). `SetValue(step)` (re-enables after `Disable(text)`), `SetProgress(step)` (ignores
negatives), `Reset()`, `ShowNumber(bool)`, `SetRadius`, `SetHeight(h)` (sets min height and radius `h/2`), and
`Disable(wxString text)`, which draws an orange "disabled" bar with `text` and hides `wxWindow::Disable()` (name
hiding: `bar->Disable()` does not compile).

Pitfalls:
- **Rule:** Pass dark-mapped colours and re-set them on theme change.
  **Why:** track, fill and text are plain `wxColour`s painted as given; nothing maps them.
  ```cpp
  bar->SetProgressBackgroundColour(StateColor::darkModeColorFor(wxColour("#009688")));   // sets the FILL
  ```
- **Rule:** Mind the swapped setter names.
  **Why:** `SetProgressForedColour` sets the **track** (`m_progress_background_colour`) and
  `SetProgressBackgroundColour` sets the **fill** (`m_progress_colour`).
- **Rule:** Use `SetHeight(FromDIP(h))` for a bar thinner than 14 px.
  **Why:** `ProgressBar::SetMinSize` returns without doing anything (width included) when the height is below its
  `miniHeight` of 14 — so `SetMinSize(wxSize(w, -1))` is ignored too.
- **Rule:** Use `ShowNumber` only with `max == 100`.
  **Why:** it draws the raw step followed by `%`.
- `Rescale()` is empty: re-set the height in `on_dpi_changed`.

## ScrolledWindow

`ScrolledWindow` (`Widgets/ScrolledWindow.cpp`) is a `wxScrolled<wxWindow>` that hides the native scrollbars and
draws `MyScrollbar`s (`Widgets/Scrollbar.cpp`) of `scrollbarWidth` in a `marginWidth` strip; content goes on
`GetPanel()` (the scroll target). Its internal panels are sized from the constructor `size`, so pass a real size
(`FromDIP`/`em`-based, as `Search.cpp` does). Use raw `wxScrolledWindow` for ordinary scrolling.

- **Rule:** Create it with `wxVSCROLL`.
  **Why:** the mouse-wheel handler forwards to the vertical bar unconditionally and `SetBackgroundColour` touches the
  vertical-bar panels unconditionally, so a `wxHSCROLL`-only instance dereferences null pointers; with neither flag
  `GetPanel()` is null. With both flags only the vertical bar is built.
- `MyScrollbar` paints on a `wxClientDC` and forces `Refresh(); Update();` — do not copy its painting
  (`references/painting-custom-widgets.md`).

## PopupWindow

`PopupWindow` (`Widgets/PopupWindow.cpp`) derives `wxPopupTransientWindow` and adds per-platform dismissal hooks.
Use it for every transient popup, knowing which parts are automatic:

| Port | Added behaviour |
|---|---|
| GTK (X11 and Wayland) | `Create` binds `wxEVT_ACTIVATE` on the first top-level window strictly above the parent (`GetTopParent` starts at the parent's parent, so a popup parented directly to a dialog watches the dialog's own parent); deactivation → `DismissAndNotify()` unless `ShouldDismissOnTopWindowDeactivate()` (virtual) returns false — `DropDown` uses that to keep chained popups open on Wayland |
| MSW | dismissal on toplevel deactivate/iconize/hide is **opt-in**: call `BindUnfocusEvent()` (MSW-only member); the destructor unbinds. Its activate handler does not `Skip()`, so while the popup exists the toplevel's earlier-bound `wxEVT_ACTIVATE` handlers and wx's focus save/restore (`wxTopLevelWindowMSW::OnActivate`, `src/msw/toplevel.cpp:1326`) do not run [source] |
| macOS | with `wxPU_CONTAINS_CONTROLS` it hit-tests and forwards mouse and enter/leave events to child controls (`OnMouseEvent2`); wx documents the flag as MSW focus behaviour only (`interface/wx/popupwin.h:17-26`) |

Command events from controls inside the popup stop at the popup on MSW and macOS ([source]
`src/common/popupcmn.cpp:135`) but bubble on to the popup's parent on GTK, whose `wxPopupWindow::Create` never sets
`wxWS_EX_BLOCK_EVENTS` (`src/gtk/popupwin.cpp`; `references/events.md §5`); bind them on the popup or its children,
as `ComboBox` binds `drop`'s `wxEVT_COMBOBOX`, and don't let an ancestor's unfiltered handler also act on them. When a click on the opener both
dismisses and reopens the popup, gate the reopen on `DropDown::HasDismissLongTime()` or an equivalent timestamp.
Dismissal contracts, per-port mechanics and parenting rules: `references/popups-menus.md`.

## ProgressDialog, WebView and device-page composites

- **`Slic3r::GUI::ProgressDialog`** (`Widgets/ProgressDialog.cpp`): a `wxDialog` copy of wx's generic progress dialog
  with Orca styling and a `Button` for Cancel; wx-compatible API (`Update(value, msg, &skip)`, `Pulse`,
  `WasCancelled`, `WasSkipped`, `Resume`, `SetRange`). `Update` yields to the event loop
  (`YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT)`), so handlers can re-enter; usage rules and the Jobs
  alternative: `references/threads-timers-app.md`.
- **`WebView`** (`Widgets/WebView.cpp`): static helpers `CreateWebView(parent, url)`, `LoadUrl`, `RunScript`,
  `CheckWebViewRuntime`, `RecreateAll()` + `EVT_WEBVIEW_RECREATED`; `WebViewHostDialog` (`Slic3r::GUI`) is the base of
  Orca's web dialogs: `references/webview-gl-aui-media.md`.
- **Device-page composites** (Monitor/Device UI; reuse inside those pages, don't copy their painting as a model):
  `SideButton`, `SideTools`/`SideToolsPanel` (`Slic3r::GUI`), `SidePopup` (`SideMenuPopup.hpp`), `StepCtrl`/
  `StepIndicator` (`EVT_STEP_CHANGING`/`EVT_STEP_CHANGED`), `TempInput`, `ImageSwitchButton`/`FanSwitchButton`,
  `AxisCtrlButton`, `FanControl` family, `AMSControl`/`AMSItem` family/`FilamentLoad`, `MultiNozzleSync` tables
  (`Slic3r::GUI`), `CheckList`, `ErrorMsgStaticText`, `AnimaIcon` (`AnimaController.hpp`), `RoundedRectangle`.

## Debugging widgets

wxInspector is compiled into Debug/RelWithDebInfo builds (the top-level `CMakeLists.txt` defines
`WXINSPECTOR_DISABLE` when `BBL_RELEASE_TO_PUBLIC` is set true or, when that variable is undefined, for the Release
configuration; `references/platforms.md §wxInspector`). Every `DPIAware` window installs its accelerator
(`SetupInspectorAccelerator`, Ctrl+Shift+I, Cmd+Shift+I on macOS); the plugins in `src/slic3r/Utils/wxInspectorPlugins/`
(`CustomWidgetsPlugin`) show Button style/type, CheckBox half state, TextInput, SwitchButton, ProgressBar, Label and
LabeledStaticBox properties. The accessors commented "only meant to be used by inspector" (`Button::GetStyle`,
`CheckBox::IsHalfChecked`, `TextInput::GetCornerRadius`, `LabeledStaticBox::GetBorderColor`, …) exist for it — don't
build features on them. A later `SetAcceleratorTable` on a DPIAware window replaces the inspector's table.
