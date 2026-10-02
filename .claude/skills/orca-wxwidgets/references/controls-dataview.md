# Standard controls, data views and Orca's widget event contracts

The value and event contracts of wx's standard controls (text, choice/combo, check/radio, spin, slider, gauge,
static text, hyperlink, static bitmap, book controls, splitter, collapsible pane, list/tree/grid), the
`wxDataViewCtrl` model/renderer machinery with its native-vs-generic limits, `wxVariant` and validators; then
which events Orca's replacement widgets emit and where to bind them, and the `ObjectList` / `ObjectGrid` designs.
Read it before writing or reviewing code that sets a control's value, reacts to its events, or touches a data view.

Contents: [Rules](#rules) · [Native vs generic](#native-vs-generic-and-silent-asserts) ·
[Programmatic changes](#events-from-programmatic-changes) · [wxTextCtrl](#wxtextctrl--wxtextentry) ·
[wxStaticText](#wxstatictext-labels-mnemonics-markup) · [Item containers](#item-containers) ·
[Check and radio](#check-boxes-and-radio-buttons) · [Spin, slider, gauge](#spin-controls-slider-gauge) ·
[Book controls](#book-controls) · [Splitter, pane, hyperlink, bitmap](#splitter-collapsible-pane-hyperlink-static-bitmap) ·
[List and tree](#wxlistctrl-wxtreectrl-image-lists) · [wxGrid](#wxgrid) · [Validators](#validators) ·
[DVC per port](#wxdataviewctrl-native-vs-generic) · [DVC model](#wxdataviewmodel-contract) ·
[DVC control API](#wxdataviewctrl-control-api) · [Renderers and editing](#custom-renderers-and-in-place-editing) ·
[wxVariant](#wxvariant-with-custom-objects) · [Orca widgets](#orca-replacement-widgets-event-contracts) ·
[ObjectList](#objectlist-objectdataviewmodel-extrarenderers) · [ObjectGrid](#objectgrid-gui_objecttable)

## Rules

1. Model→view refreshes use the quiet setters (`ChangeValue`, `ChangeSelection`). `wxTextEntry::SetValue`,
   `wxBookCtrlBase::SetSelection`, `wxTreeCtrl::SelectItem` and macOS `wxDataViewCtrl::Select` *do* emit events.
   → [Programmatic changes](#events-from-programmatic-changes)
2. Never call `SetLabel`/`SetLabelText`/`GetLabel` on a `wxTextCtrl` to set or read its text; the setters are
   silent no-ops in 3.3 and `GetLabel` returns the label, not the text. → [wxTextCtrl](#wxtextctrl--wxtextentry)
3. To consume Enter, handle `wxEVT_TEXT_ENTER` (needs `wxTE_PROCESS_ENTER`) and do not `Skip()`; skipping lets
   Enter activate the dialog's default button. → [wxTextCtrl](#wxtextctrl--wxtextentry)
4. Call `OSXDisableAllSmartSubstitutions()` (wxOSX-only API, so inside `#ifdef __WXOSX__`) on every text control
   that holds G-code, paths, URLs or code. → [wxTextCtrl](#wxtextctrl--wxtextentry)
5. Do not rely on `SetHint`/`SetMaxLength` on multi-line controls (hints: MSW and GTK2 only; max length: MSW and
   GTK only). → [wxTextCtrl](#wxtextctrl--wxtextentry)
6. Show user data (preset, filament, file names) in labels and page titles with `SetLabelText` /
   `wxControl::EscapeMnemonics`; quote it with `wxMarkupParser::Quote` inside markup.
   → [wxStaticText](#wxstatictext-labels-mnemonics-markup)
7. Typed `wxClientData` belongs to the control (never delete it); `::ComboBox` supports only untyped `void*`
   data, which the caller owns. → [Item containers](#item-containers), [::ComboBox](#combobox)
8. A 3-state checkbox is read with `Get3StateValue()`; every radio group starts with `wxRB_GROUP`.
   → [Check and radio](#check-boxes-and-radio-buttons)
9. Read spin values after the commit event (`wxEVT_SPINCTRL`), not from `wxEVT_TEXT`.
   → [Spin](#spin-controls-slider-gauge)
10. Book pages are created with the book as parent; in `PAGE_CHANGED` use `event.GetSelection()`.
    → [Book controls](#book-controls)
11. Pixel-valued setters take `FromDIP(n)` (`SetMinimumPaneSize`, `SetRowHeight`, column widths), and are
    re-applied on rescale. → [Splitter](#splitter-collapsible-pane-hyperlink-static-bitmap), [DVC API](#wxdataviewctrl-control-api)
12. Never dereference `begin()` of a wx selection range (`wxGrid::GetSelectedBlocks()`) without comparing it to
    `end()`; never assume `wxDataViewCtrl::GetSelection()` is valid. → [wxGrid](#wxgrid)
13. A `wxGridCellChoiceEditor` subclass whose `m_control` is not a `wxComboBox` overrides `Reset()`,
    `GetValue()` and `SetParameters()` too. → [wxGrid](#wxgrid)
14. Validators only filter keystrokes; parse and range-check values on commit. → [Validators](#validators)
15. Never `delete` a `wxDataViewModel`; keep exactly the references you mean to own. → [DVC model](#wxdataviewmodel-contract)
16. Change the model first, then notify (`ItemAdded`/`ItemDeleted`); after deletion use the pointer only as an
    ID. Use `Cleared()` only when everything changed. → [DVC model](#wxdataviewmodel-contract)
17. `GetValue` fills the variant type the column's renderer expects; a mismatch shows nothing.
    → [DVC model](#wxdataviewmodel-contract)
18. With `wxDV_MULTIPLE` use `GetSelections()`/`HasSelection()`; `event.GetItem()` of `SELECTION_CHANGED` may be
    invalid on GTK and macOS. → [DVC API](#wxdataviewctrl-control-api)
19. Bracket programmatic `Select`/`UnselectAll`/model mutations with a suppress flag that the
    `SELECTION_CHANGED` handler checks. → [DVC API](#wxdataviewctrl-control-api)
20. Give a data view with custom renderers an explicit `SetRowHeight` sized for the tallest content; repeat it
    after `SetFont` and in the rescale handler. → [DVC API](#wxdataviewctrl-control-api)
21. Never carry a `wxDataViewItem` across a deferred call without re-validating it against the model.
    → [DVC model](#wxdataviewmodel-contract), [ObjectList](#objectlist-objectdataviewmodel-extrarenderers)
22. Check `!v.IsNull() && v.GetType() == "<Class>"` before every `obj << variant`; `dynamic_cast` the editor in
    `GetValueFromEditorCtrl`. → [wxVariant](#wxvariant-with-custom-objects), [Renderers](#custom-renderers-and-in-place-editing)
23. A compound in-place editor (TextInput, `::ComboBox`) commits by calling the renderer's `FinishEditing()` /
    `CancelEditing()` itself. → [Renderers](#custom-renderers-and-in-place-editing)
24. On macOS, `CreateEditorCtrl` never runs natively: veto `START_EDITING`, `CallAfter` the renderer's own
    `StartEditing`, then `SetCustomRendererPtr/Item`, all under `#ifdef __WXOSX__`.
    → [Renderers](#custom-renderers-and-in-place-editing)
25. Bind `::TextInput`/`::SpinInput` `wxEVT_TEXT_ENTER`/`wxEVT_KILL_FOCUS` on the widget itself (or its
    `GetTextCtrl()`), never on a parent filtered by the widget's id. `::ComboBox` ignores the id passed to its
    constructor, so bind on the combo (or `SetId()` after construction if an id filter is unavoidable).
    → [Orca widgets](#orca-replacement-widgets-event-contracts)
26. `::CheckBox`/`SwitchButton` emit `wxEVT_TOGGLEBUTTON`, and a handler bound on the widget itself must `Skip()`;
    `RadioGroup::SetSelection` always emits; `SpinInput`'s `wxEVT_SPINCTRL` is a plain `wxCommandEvent`.
    → [Orca widgets](#orca-replacement-widgets-event-contracts)
27. Expect Orca's colours on every generic data view and `RenderText` on MSW: `ObjectList` installs a
    process-global `wxRendererNative`. → [ObjectList](#objectlist-objectdataviewmodel-extrarenderers)

## Native vs generic, and silent asserts

Orca builds wx with `wxBUILD_DEBUG_LEVEL=0` and `libslic3r_gui` with `wxDEBUG_LEVEL=0`, so `wxASSERT`/`wxFAIL`
vanish and `wxCHECK_*` returns early without a message (`include/wx/debug.h:229-231, 340-368`). Every "asserts" in
the docs below means, in Orca, that the call silently does nothing (a `wxCHECK`) or carries on with bad state (a
`wxASSERT`). Linux builds target GTK3 (GTK2 is only the `-DDEP_WX_GTK3=OFF` opt-out); GTK2-only behaviour is noted
where it differs.

Which implementation a control uses decides most platform differences:

| Control | MSW | GTK | macOS | Cite |
|---|---|---|---|---|
| `wxDataViewCtrl` | generic | native `GtkTreeView` | native `NSOutlineView` | `include/wx/dataview.h:36-44` |
| `wxBitmapComboBox` | native (owner-drawn CB) | native | generic `wxOwnerDrawnComboBox` | `include/wx/bmpcbox.h:27-28,112-119` |
| `wxTreeCtrl` | native | generic | generic | `include/wx/treectrl.h:27-28` |
| `wxListCtrl` | native | generic | generic | `include/wx/listctrl.h:29-34` |
| `wxHyperlinkCtrl` | native | native | generic | `interface/wx/hyperlink.h:90` |
| `wxGrid` | generic | generic | generic | `src/generic/grid.cpp` |

## Events from programmatic changes

The general wx rule is that setters do not send events. The exceptions are the bugs:

| Call | Event emitted? | Cite |
|---|---|---|
| `wxTextEntry::SetValue(s)` | **yes**, one `wxEVT_TEXT`, even when `s` equals the current text | `interface/wx/textentry.h:539-542`; [source] `src/common/textentrycmn.cpp:236-254`, `src/msw/textctrl.cpp:1133-1145` |
| `wxTextEntry::ChangeValue(s)` | no | `interface/wx/textentry.h:177-178` |
| `wxTextEntry::Clear()` | yes (= `SetValue("")`) | `interface/wx/textentry.h:193-194` |
| `wxTextCtrl::SetLabel/SetLabelText` | nothing at all (3.3) | `docs/changes.txt:111-113`; `src/common/textcmn.cpp:934-937` |
| `SetSelection/SetStringSelection` on wxChoice, wxComboBox, wxListBox, wxRadioBox | no | `interface/wx/ctrlsub.h:102-103,126` |
| `wxComboBox::SetValue` | `wxEVT_TEXT` if editable; none with `wxCB_READONLY` | `interface/wx/combobox.h:256-272` |
| `wxComboBox::Popup()/Dismiss()` | DROPDOWN/CLOSEUP, except on wxOSX | `interface/wx/combobox.h:279-281,292-294` |
| `wxCheckBox::SetValue/Set3StateValue`, `wxRadioButton::SetValue`, `wxCheckListBox::Check` | no | `interface/wx/checkbox.h:170-180`, `interface/wx/radiobut.h:117-118`, `interface/wx/checklst.h:135-137` |
| `wxSpinCtrl[Double]::SetValue/SetRange` | no; `SetRange` may silently clamp the value | `interface/wx/spinctrl.h:186-197,220-231,432-441` |
| `wxBookCtrlBase::SetSelection` | **yes**, PAGE_CHANGING (vetoable) + PAGE_CHANGED | `interface/wx/bookctrl.h:175-183` |
| `wxBookCtrlBase::ChangeSelection` | no | `interface/wx/bookctrl.h:192-199` |
| `AddPage/InsertPage(select=true)` | yes, except for the very first page | `interface/wx/bookctrl.h:256-259` |
| `DeletePage/RemovePage` | yes if the selection shifts, except when deleting the last page | `interface/wx/bookctrl.h:286-295` |
| `wxTreeCtrl::SelectItem` | **yes**, SEL_CHANGING (vetoable) + SEL_CHANGED | `interface/wx/treectrl.h:866-868` |
| `wxDataViewCtrl::Select/SetSelections/UnselectAll` | generic: no; GTK: suppressed; **macOS: `SELECTION_CHANGED`** | [source] `src/generic/datavgen.cpp:6396-6412`; `src/gtk/dataview.cpp:5268-5282`; `src/osx/dataview_osx.cpp:687-738`, `src/osx/cocoa/dataview.mm:1824-1832` |
| `wxDataViewModel::ItemChanged/ValueChanged/ChangeValue` | `wxEVT_DATAVIEW_ITEM_VALUE_CHANGED` | `interface/wx/dataview.h:116-117,331,392` |
| generic `wxDataViewCtrl::Expand/Collapse` | EXPANDING/EXPANDED, COLLAPSING/COLLAPSED; `Collapse` can also send SELECTION_CHANGED when it hides selected rows | [source] `src/generic/datavgen.cpp:4110-4150` |

Orca's widgets add their own rows; see [Orca widgets](#orca-replacement-widgets-event-contracts).

Pitfalls:
- **Rule:** A model→view refresh must not loop back through the view's change handler.
  **Why:** `SetValue` and `SetSelection` on a book fire synchronously inside the refresh; the handler writes the
  value back, marks the preset dirty, or rebuilds the page it is running in. On macOS a programmatic DVC
  `Select` re-enters the selection handler.
  ```cpp
  // Wrong
  text->SetValue(v);  book->SetSelection(i);  dvc->Select(item);
  // Right
  text->ChangeValue(v);  book->ChangeSelection(i);
  m_suppress = true; dvc->Select(item); m_suppress = false;   // handler: if (m_suppress) return;
  ```
  Cite: the table above; `ObjectList` uses `m_prevent_list_events` for the same reason.

## wxTextCtrl / wxTextEntry

Contract:
- `wxTE_PROCESS_ENTER` is required for `wxEVT_TEXT_ENTER` (`interface/wx/textctrl.h:1562-1564`). If no handler
  exists, *or the handler calls `Skip()`*, Enter is processed internally or "used to activate the default button
  of the dialog" (`interface/wx/textctrl.h:1324-1331`). `wxComboBox` has the same flag and semantics
  (`interface/wx/combobox.h:42-48`); for `wxBitmapComboBox` it is documented as Windows-only
  (`interface/wx/bmpcbox.h:29-33`).
- `wxTE_PROCESS_TAB` has no effect on single-line controls under wxGTK (`interface/wx/textctrl.h:1332-1338`).
- `wxEVT_TEXT` "is however not sent during the control creation" (`interface/wx/textctrl.h:1556-1560`). A
  `SetValue` right after the constructor does send it, which is harmless only while nothing is bound.
- `IsModified()` is true only after user edits; `SetValue`/`ChangeValue` reset it (`interface/wx/textentry.h:170-172`,
  `interface/wx/textctrl.h:1880-1886`). Use it on commit to skip writing back untouched values.
- Styles fixed at creation: `wxTE_READONLY`, `wxTE_PASSWORD` and the wrap styles can change later on GTK but not
  on MSW; every other style except alignment is creation-time only (`interface/wx/textctrl.h:1398-1402`). Toggle
  editability with `SetEditable(bool)`, not `SetWindowStyleFlag`.
- Multi-line positions are not string indices on `\r\n` platforms: use `GetRange()`, not
  `GetValue().Mid(GetInsertionPoint())` (`interface/wx/textctrl.h:1405-1418`).
- `SetMaxLength(n)` works on single-line controls everywhere, on multi-line ones only in wxMSW and wxGTK; extra
  input is discarded and `wxEVT_TEXT_MAXLEN` sent (`interface/wx/textentry.h:398-416`). It does not filter
  `SetValue`.
- `SetHint()` is native on MSW, macOS and GTK ≥ 3.2. Without native support (GTK2) wx's fallback requires you to
  `Skip()` focus and `wxEVT_TEXT` events and avoid `WriteText`/`Replace` while the control is empty. Hints are
  ignored with `wxTE_PASSWORD`, and on multi-line controls they work only on MSW and GTK2 — so not on macOS or on
  Orca's GTK3 Linux build (`interface/wx/textentry.h:467-486`).
- `wxTE_RICH` is MSW-only (prefer `wxTE_RICH2`); `wxTE_NOHIDESEL` is MSW-only (`interface/wx/textctrl.h:1347-1363`).

Platforms — macOS: quote and dash smart substitution are "enabled by default" (`interface/wx/textctrl.h:2114-2134`);
`OSXDisableAllSmartSubstitutions()` turns them off (`:2136-2144`). A single-line control also replaces pasted
newlines with spaces unless `OSXEnableNewLineReplacement(false)` (`:2094-2113`). These `OSX*` methods are
`@onlyfor{wxosx}` and declared only in `include/wx/osx/textctrl.h`, so calls must sit under `#ifdef __WXOSX__` or
the MSW and GTK builds fail to compile.

OrcaSlicer: interactive single-line text uses `::TextInput` ([below](#textinput)); raw `wxTextCtrl` remains for
multi-line text. Field's `TextCtrl::BUILD` calls `OSXDisableAllSmartSubstitutions()` under `#ifdef __WXOSX__`; copy
that for any G-code or path editor.

Pitfalls:
- **Rule:** Never use `SetLabel`, `SetLabelText` or `GetLabel` for a text control's content.
  **Why:** `wxTextCtrlBase::SetLabel` is `wxFAIL_MSG("Use SetValue() or ChangeValue() instead.")` and nothing else
  (`src/common/textcmn.cpp:934-937`); `SetLabelText` calls the virtual `SetLabel` (`include/wx/control.h:64-67`);
  `GetLabel()` returns `m_labelOrig`, not the text (`include/wx/control.h:61`). The assert is compiled out, so the
  control silently never updates, on every platform (3.3 made this consistent; MSW used to treat it as `SetValue`).
  ```cpp
  ctrl->GetTextCtrl()->SetLabel(s);        // Wrong: no-op
  ctrl->GetTextCtrl()->ChangeValue(s);     // Right (or SetValue if listeners must react)
  ```
  Cite: `docs/changes.txt:111-113`.
- **Rule:** Handle `wxEVT_TEXT_ENTER` without `Skip()` unless you want the default button activated as well.
  **Why:** a skipped Enter falls through to the dialog's default button and closes it.
  ```cpp
  txt->Bind(wxEVT_TEXT_ENTER, [](wxCommandEvent& e) { commit(); e.Skip(); });  // Wrong in a dialog
  txt->Bind(wxEVT_TEXT_ENTER, [](wxCommandEvent&)   { commit(); });           // Right
  ```
  Cite: `interface/wx/textctrl.h:1324-1331`.
- **Rule:** Disable macOS smart substitutions on code, path and URL editors.
  **Why:** typed `"` becomes `”` and `--` becomes `—`, silently corrupting G-code macros and paths.
  ```cpp
  auto* ed = new wxTextCtrl(this, wxID_ANY, gcode, wxDefaultPosition, sz, wxTE_MULTILINE);  // Wrong alone
  #ifdef __WXOSX__                                                                          // Right: add this
  ed->OSXDisableAllSmartSubstitutions();
  #endif
  ```
  Cite: `interface/wx/textctrl.h:2114-2144`; `include/wx/osx/textctrl.h:154`; Field `TextCtrl::BUILD`.
- **Rule:** Don't put the only explanation of a multi-line field in `SetHint()`, and don't count on
  `SetMaxLength()` there.
  **Why:** multi-line hints are ignored on macOS and GTK3; multi-line max length is ignored on macOS.
  Cite: `interface/wx/textentry.h:414-415, 485-486`.

## wxStaticText: labels, mnemonics, markup

Contract:
- `SetLabel` treats every `&` as a mnemonic marker; a literal ampersand must be `&&`. `SetLabelText()` shows text
  verbatim, and `wxControl::EscapeMnemonics()` escapes it (`interface/wx/control.h:174-195, 379-387`). This applies
  to `wxStaticText`, `wxCheckBox`, `wxRadioButton`, `wxButton` labels and to book page titles: all `wx*book`
  classes interpret mnemonics in page text (`interface/wx/bookctrl.h:141-147`), and since 3.3 wxListbook and
  wxChoicebook do too (`docs/changes.txt:128-130`).
- `SetLabelMarkup()` needs well-formed markup or the label "won't be shown at all" (returns false, keeps the old
  label). A bare `&` is still a mnemonic. Multi-line markup works only on GTK and macOS; the generic version used on
  MSW handles single lines (`interface/wx/control.h:338-347`). Quote untrusted text with
  `wxMarkupParser::Quote()` (`include/wx/private/markupparser.h:137-141`); the header is private, but Orca's wx
  install copies `include/wx/private` (`deps/wxWidgets/wxWidgets.cmake`, `copy_private_headers`).
- Without `wxST_NO_AUTORESIZE`, `SetLabel` resizes the control to its best size but never re-lays-out the parent;
  call the parent's or sizer's `Layout()` after a size-changing label. `SetLabel` is a no-op when the text is
  unchanged (`interface/wx/stattext.h:30-36, 107-117`; `src/common/stattextcmn.cpp:334-352`).
- Ellipsizing (`wxST_ELLIPSIZE_START/MIDDLE/END`): the best size is the full text extent on every port (GTK even
  switches ellipsizing off to measure, `src/gtk/stattext.cpp:239-295`), so a label ellipsizes only when something
  constrains its width — an explicit min/initial width, or `wxST_NO_AUTORESIZE` plus a sizer-assigned size.
- Wrapping (`Wrap(w)` caches its width in 3.3.2, so `SetLabel(new); Wrap(sameWidth)` leaves the new label
  unwrapped — [source] `src/common/stattextcmn.cpp:259-264`; `wxST_WRAP`; Orca's `::Label`): see
  `references/sizers-layout.md` §wxStaticText wrapping.

Pitfalls:
- **Rule:** User-provided names go through `SetLabelText` / `EscapeMnemonics`, and through
  `wxMarkupParser::Quote` inside markup.
  **Why:** an `&` in a preset or file name disappears or underlines the next character; a `<` makes the markup
  invalid and the label shows nothing.
  ```cpp
  new wxStaticText(p, wxID_ANY, preset_name);                    // Wrong
  auto* st = new wxStaticText(p, wxID_ANY, ""); st->SetLabelText(preset_name);   // Right
  book->AddPage(pg, wxControl::EscapeMnemonics(filament_name));  // Right for page titles
  lbl->SetLabelMarkup("<b>" + wxMarkupParser::Quote(name) + "</b>");             // Right for markup
  ```
  Cite: `interface/wx/control.h:174-195, 338-347`. Escaping translated strings: `references/strings-i18n-files.md`.

## Item containers

`wxChoice`, `wxComboBox`, `wxBitmapComboBox`, `wxListBox`, `wxCheckListBox`, and Orca's `::ComboBox` derive from
`wxItemContainer`.

Contract:
- Client data: the control *owns* typed `wxClientData*` and deletes it in `Delete()`, `Clear()` and its destructor;
  untyped `void*` is never touched. All items of one control use one kind, fixed by the first `Append(..., data)`
  or `SetClient*` call (`interface/wx/ctrlsub.h:172-185, 466-480`). Asking for the other kind is a silent `wxCHECK`
  returning `nullptr` (`src/common/ctrlsub.cpp:182-191, 221-230`); mixing kinds fails only a compiled-out
  `wxASSERT` (`:213-214`).
- `SetStringSelection` matches case-insensitively and takes the first hit (`interface/wx/ctrlsub.h:128-131`); MSW
  wxComboBox "doesn't behave correctly" with items differing only in case (`interface/wx/combobox.h:24-28`).
- While the dropdown is open only `GetCurrentSelection()` reflects the highlighted item
  (`interface/wx/choice.h:148-161`, `interface/wx/combobox.h:200-208`). In a `wxEVT_COMBOBOX` handler `GetValue()`
  already returns the new value (`interface/wx/combobox.h:53-56`).
- `wxComboBox::IsEmpty()` is ambiguous and does not compile; use `IsListEmpty()` or `IsTextEmpty()`
  (`interface/wx/combobox.h:219-249`).
- With `wxCB_READONLY`, `SetValue(s)` requires `s` to be in the list (case-insensitive) (`interface/wx/combobox.h:264-267`).
- `wxComboBox` DROPDOWN/CLOSEUP exist on wxMSW, GTK ≥ 2.10 and wxOSX/Cocoa (`interface/wx/combobox.h:64-73`), but
  `Popup()`/`Dismiss()` never send them on wxOSX (`:279-281`).
- `wxChoice::SetColumns` is GTK-only (`interface/wx/choice.h:164-172`). `wxLB_MULTIPLE` equals `wxLB_EXTENDED`
  on GTK2 (`interface/wx/listbox.h:34`). In `wxEVT_CHECKLISTBOX`, `event.IsChecked()` is invalid; use `GetInt()`
  with `wxCheckListBox::IsChecked(i)` (`interface/wx/checklst.h:18-23`).

Platforms — macOS: `wxBitmapComboBox` is a `wxOwnerDrawnComboBox`; its `Select()` uses `ChangeValue` and sends no
event (`src/generic/odcombo.cpp:1041-1060`), and all bitmaps must share one size (`interface/wx/bmpcbox.h:12-13`).

OrcaSlicer: `Slic3r::GUI::BitmapComboBox` (`BitmapComboBox.hpp`) wraps `wxBitmapComboBox` where a native bitmap
combo is still used (e.g. `apply_extruder_selector` in `wxExtensions.cpp`, `PrintHostDialogs`); on macOS it overrides `OnAddBitmap`/`OnDrawItem`
because the generic owner-drawn base would size items from Retina-scaled bitmaps. Settings fields use
`::ComboBox`, which has different client-data rules ([below](#combobox)).

Pitfalls:
- **Rule:** Never delete typed client data yourself; free `void*` data yourself (fetch the pointers before
  `Clear()`/`Delete()`, which only drop them).
  ```cpp
  combo->Append(name, new wxStringClientData(id));      // typed: the control owns and deletes it
  delete combo->GetClientObject(i);                     // Wrong: double free on Delete()/Clear()/destruction
  auto* d = static_cast<wxStringClientData*>(combo->GetClientData(i));     // Wrong kind: silent nullptr
  auto* d = static_cast<wxStringClientData*>(combo->GetClientObject(i));   // Right; never delete d
  ```
  Cite: `interface/wx/ctrlsub.h:172-185`; `src/common/ctrlsub.cpp:221-230`.

## Check boxes and radio buttons

Contract:
- 3-state checkboxes need `wxCHK_3STATE`; users reach the third state only with `wxCHK_ALLOW_3RD_STATE_FOR_USER`
  (`interface/wx/checkbox.h:50-56`). Read them with `Get3StateValue()`. `IsChecked()` asserts on a 3-state box
  (`include/wx/checkbox.h:57-62`), so in Orca it silently returns `GetValue()`; `Set3StateValue(wxCHK_UNDETERMINED)`
  on a 2-state box is likewise only an assert (`interface/wx/checkbox.h:179-185`).
- Radio groups form from consecutive sibling buttons; `wxRB_GROUP` starts a new group and that button is the
  initial selection (`interface/wx/radiobut.h:15-21`). `wxRB_SINGLE` takes a button out of any group, but only on
  MSW and GTK (≥ 3.3.0); elsewhere such a button "can't be turned off" (`:31-39`). `SetValue(false)` on a grouped
  button is invalid — select another. On MSW the focused radio button is always the selected one (`:120-130`).
- `wxRadioBox::SetSelection(n)` needs a valid `n`, never `wxNOT_FOUND` (`interface/wx/radiobox.h:292-295`), and
  sends no event.

OrcaSlicer: interactive check boxes are `::CheckBox`/`SwitchButton`, radio rows are `::RadioGroup`; their event
contracts differ from these ([below](#checkbox-and-switchbutton)).

Pitfalls:
- **Rule:** Put `wxRB_GROUP` on the first button of *every* group.
  **Why:** without it, adjacent groups merge into one and selecting in one clears the other.
  Cite: `interface/wx/radiobut.h:15-21`.

## Spin controls, slider, gauge

Contract:
- Typed spin text "is not validated until the control loses focus"; it is then clamped and `wxEVT_SPINCTRL` is
  sent only if the value differs from the last one sent. Raw typing produces `wxEVT_TEXT`
  (`interface/wx/spinctrl.h:36-45`). `wxSpinCtrlDouble` sends `wxEVT_SPINCTRLDOUBLE` instead and also commits on
  Enter (`:276-279`). Add `wxTE_PROCESS_ENTER` for `wxEVT_TEXT_ENTER` (`:18-22`).
- `wxEVT_SPINCTRL` is declared with the `wxSpinEvent` class (`include/wx/spinctrl.h:22`); `wxSpinEvent::GetValue()`
  and `GetPosition()` both return `GetInt()` (`include/wx/spinbutt.h:104-107`).
- `wxSlider`: handle `wxEVT_SLIDER`. `wxEVT_SCROLL_CHANGED` exists on MSW only (`interface/wx/slider.h:35-38, 103`).
  `wxSL_LEFT/TOP` work on Windows and GTK3 only; `wxSL_BOTH` and `wxSL_SELRANGE` on Windows only; tick marks
  (`wxSL_AUTOTICKS`) need Windows or GTK ≥ 2.16 (`interface/wx/slider.h:32-66`).
- `wxGauge`: `Pulse()` switches to indeterminate mode until the next `SetValue()`; under wxMSW `SetRange` in
  indeterminate mode controls the bounce span (`interface/wx/gauge.h:141-161`).

OrcaSlicer: integer spinners are `::SpinInput` ([below](#spininput)); progress bars are Orca's `ProgressBar`
(`references/orca-widgets.md`). Raw `wxSlider` remains in Field's `SliderCtrl`.

## Book controls

Contract:
- A page must be created with the book as its parent and added once; the book owns and deletes it
  (`interface/wx/bookctrl.h:253-254, 273`). `RemovePage` detaches without deleting, and you then own it (`:324-330`).
- `GetSelection()` inside a `PAGE_CHANGED` handler may return the old or the new page depending on the platform; use
  `event.GetSelection()` (`interface/wx/bookctrl.h:160-166`).
- `wxSimplebook` has no UI; switch with `ChangeSelection()`. `SetSelection()` sends PAGE_CHANGING/CHANGED
  (`interface/wx/simplebook.h:17-31`); `ShowNewPage()` adds and selects (`:130-138`).
- `wxNotebook`: `wxNB_LEFT/RIGHT/BOTTOM` are unsupported on themed MSW (`interface/wx/notebook.h:60-63`); the themed
  MSW page background is disabled with `wxNB_NOPAGETHEME`, an explicit page `SetBackgroundColour`, or app-wide with
  `wxSystemOptions::SetOption("msw.notebook.themed-background", 0)` (`:76-108`).

OrcaSlicer: three tab mechanisms, not interchangeable —
- `Notebook` (`Notebook.hpp`): a custom `wxBookCtrlBase` with a `ButtonsListCtrl` header, used for MainFrame's
  Home/Prepare/Preview/Device tabs. Header clicks arrive internally as `wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED`; the book
  itself emits `wxEVT_BOOKCTRL_PAGE_CHANGING/CHANGED` from `SetSelection` and nothing from `ChangeSelection`,
  matching wx.
- `::TabCtrl` (`Widgets/TabCtrl`): a tab *bar* only; content switching is the caller's ([below](#radiogroup-tabctrl)).
- `wxSimplebook`: headerless page stacks (e.g. `StatusPanel`, `SelectMachine`, `ReleaseNote`).

Pitfalls:
- **Rule:** Sync code uses `ChangeSelection`; a `PAGE_CHANGED` handler reads `event.GetSelection()`.
  ```cpp
  book->SetSelection(i);                     // Wrong in a sync routine whose PAGE_CHANGED handler rebuilds UI
  book->ChangeSelection(i);                  // Right
  int sel = book->GetSelection();            // Wrong inside PAGE_CHANGED
  int sel = event.GetSelection();            // Right
  auto* pg = new MyPage(dialog); book->AddPage(pg, t);   // Wrong: page must be the book's child
  auto* pg = new MyPage(book);   book->AddPage(pg, t);   // Right
  ```
  Cite: `interface/wx/bookctrl.h:160-199, 253-254`.

## Splitter, collapsible pane, hyperlink, static bitmap

- `wxSplitterWindow`: the default minimum pane size is 0, so the user can drag a pane shut. `SetMinimumPaneSize()`
  takes pixels — pass `FromDIP(n)` (`interface/wx/splitter.h:300-317`). `Unsplit()` only hides the removed pane
  (`:452-466`). Sashes resize at idle time; `UpdateSize()` forces it before `Show` (`:468-479`).
- `wxCollapsiblePane`: put children on `GetPane()`, not on the pane control; re-layout on
  `wxEVT_COLLAPSIBLEPANE_CHANGED`; the pane resizes its top-level window unless `wxCP_NO_TLW_RESIZE`
  (`interface/wx/collpane.h:54-96`).
- `wxHyperlinkCtrl`: if the `wxEVT_HYPERLINK` handler `Skip()`s, or there is none, wx calls
  `wxLaunchDefaultBrowser` (`interface/wx/hyperlink.h:55-58`). Orca's `Slic3r::GUI::HyperLink` and `::Label` with
  `LB_HYPERLINK` are the styled alternatives (`references/orca-widgets.md`).
- `wxStaticBitmap`: native versions are meant for small icons and only the generic one supports every
  `SetScaleMode`; use `wxGenericStaticBitmap` for large images. MSW centres a smaller bitmap, other ports draw it at
  the origin (`interface/wx/statbmp.h:11-23, 146-152`). `SetBitmap` takes a `wxBitmapBundle` (`:132`); Orca builds
  bundles from its own SVG rasteriser (`references/dpi-bitmaps-fonts.md`).

## wxListCtrl, wxTreeCtrl, image lists

- Virtual list (`wxLC_REPORT|wxLC_VIRTUAL`): call `SetItemCount()` and override `OnGetItemText` (optionally
  `OnGetItemImage`/`OnGetItemAttr`) (`interface/wx/listctrl.h:128-134`). `EditLabel()` asserts without
  `wxLC_EDIT_LABELS` (`docs/changes.txt:68-69`).
- Images: prefer `SetImages(std::vector<wxBitmapBundle>)`; `wxImageList` is discouraged
  (`interface/wx/withimages.h:21-40`) and is measured in physical pixels in 3.3 (`docs/changes.txt:85-88`); calling
  its methods on an invalid (unsized) list now asserts, i.e. fails silently in Orca (`docs/changes.txt:53-56`).
  `Assign*` transfers ownership, `Set*ImageList` does not (`interface/wx/withimages.h:90-110`). Sizing:
  `references/dpi-bitmaps-fonts.md`.
- `wxTreeCtrl`: `SelectItem` emits events ([table](#events-from-programmatic-changes)).
  `Delete/DeleteChildren/DeleteAllItems` send `wxEVT_TREE_DELETE_ITEM` for every item; `DeleteChildren` does not
  clear `SetItemHasChildren` (`interface/wx/treectrl.h:318-344`). The tree owns `wxTreeItemData`.

## wxGrid

Contract:
- `GetSelectedBlocks()` returns a range of unordered, possibly overlapping blocks (`interface/wx/grid.h:5090-5109`).
  [source] It is empty when nothing is selected, and the grid cursor cell is not part of it
  (`src/generic/grid.cpp:11295-11302` returns an empty `wxGridBlocks()` without a selection object, otherwise the
  selection's blocks).
- `FreezeTo(row, col)` returns false (an assert, silent in Orca) for out-of-range values, merged cells or the native
  header (`interface/wx/grid.h:5747-5772`); [source] also when rows/columns were reordered or drag-moving is enabled
  (`src/generic/grid.cpp:5742-5750`). In 3.3 it freezes even when the grid is too small (`docs/changes.txt:48-51`).
- Editor contract: `EndEdit` must not modify the grid — it stores the value and returns true if it changed;
  `ApplyEdit` writes it after `wxEVT_GRID_CELL_CHANGING` was not vetoed (`interface/wx/grid.h:610-638`). Editors,
  renderers and attrs are ref-counted and the setters take ownership (`:1263-1305, 1739-1770, 2681-2701`).
- A custom table sends `wxGridTableMessage` via `ProcessTableMessage()` whenever rows or columns are added or
  removed (`interface/wx/grid.h:1832-1840`). `AssignTable()` takes ownership and may be called once (`:3088-3108`).
- `wxGridCellChoiceEditor::Combo()` is a C-cast of `m_control` to `wxComboBox*`
  (`include/wx/generic/grideditors.h:394`); `Reset()`, `GetValue()`, `EndEdit()` and `SetParameters()` use it
  (`src/generic/grideditors.cpp:1538-1605`), and Esc in any editor calls `Reset()` (`:87-93`).

Pitfalls:
- **Rule:** Treat wx selection ranges as possibly empty: compare `begin()` with `end()` before dereferencing, and
  fall back to the (row, col) that triggered the event.
  **Why:** the docs never promised a non-empty range; with no selection 3.3 returns an empty range, and the
  unchecked `begin()->GetLeftCol()` in `ObjectGrid` crashed on cell deselect after the 3.3 upgrade. The data-view
  analogue: `wxDataViewCtrl::GetSelection()` is invalid with no selection *or* with more than one.
  ```cpp
  auto left = grid->GetSelectedBlocks().begin()->GetLeftCol();    // Wrong
  auto blocks = grid->GetSelectedBlocks();                        // Right
  auto it = blocks.begin();
  int left = (it == blocks.end()) ? col : it->GetLeftCol();
  ```
  Cite: 46e47cec0a (`GUI_ObjectTable.cpp`, `GridCellSupportEditor::DoActivate`).
- **Rule:** A `wxGridCellChoiceEditor` subclass that puts any other control (e.g. `::ComboBox`) in `m_control` must
  also override `Reset()`, `GetValue()` and `SetParameters()` — or derive from `wxGridCellEditor` instead.
  **Why:** shadowing the non-virtual `Combo()` does not change the base methods, which still C-cast `m_control` to
  `wxComboBox*`; Esc calls `Reset()` on it.
  Cite: `include/wx/generic/grideditors.h:394`, `src/generic/grideditors.cpp:87-93, 1561-1605`.
- **Rule:** Clamp `FreezeTo` arguments to `[0, GetNumberRows()]` / `[0, GetNumberCols()]` and check its result.
  Cite: `src/generic/grid.cpp:5742-5750`.

## Validators

- `SetValidator` stores a `Clone()` (`interface/wx/window.h:3369-3371`). `wxTextValidator` filters `wxEVT_CHAR` and
  pasted text (`src/common/valtext.cpp:60-61, 277-301`) but never `SetValue`. `wxFILTER_DIGITS` rejects `-`, `.` and
  `+`; `wxFILTER_NUMERIC` allows `.`, signs and `e/E` but "is not the same behaviour of wxString::IsNumber()"
  (`interface/wx/valtext.h:43-52`).
- Data transfer is automatic only for dialogs: `ShowModal()/Show()` → `InitDialog()` → `TransferDataToWindow()`;
  the default `wxID_OK` handler runs `Validate() && TransferDataFromWindow()`. Panels must call `InitDialog()`
  themselves (`docs/doxygen/overviews/validator.h:93-131`). A custom OK handler that calls `EndModal` skips
  validation.

OrcaSlicer: validators are keystroke filters only — a `wxTextValidator` with `wxFILTER_DIGITS`, `wxFILTER_NUMERIC` or
`wxFILTER_INCLUDE_CHAR_LIST` set on `TextInput::GetTextCtrl()` (e.g. `Preferences.cpp`, `calib_dlg.cpp`) and inside
`SpinInput`. `TransferDataTo/FromWindow` is not used: values are read, parsed and range-checked explicitly on commit.
Keep that pattern.

Pitfalls:
- **Rule:** Pick the filter for the full value range, and still parse and range-check on commit.
  ```cpp
  ctrl->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_DIGITS));   // Wrong if negatives are valid
  ctrl->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));  // Right, plus ToDouble + range check
  ```
  Cite: `interface/wx/valtext.h:43-52`.

## wxDataViewCtrl: native vs generic

All cites `interface/wx/dataview.h`. MSW uses the generic implementation; wxGTK and wxOSX use native controls
(915-917, 1039-1041, 3953-3955). Orca's macOS wx has `wxUSE_NATIVE_DATAVIEWCTRL 1`, so `ObjectList`,
`DiffViewCtrl`, `ParamsViewCtrl` and the other subclasses are `NSOutlineView`s there.

| Feature | Generic (MSW) | GTK | macOS | Cite |
|---|---|---|---|---|
| Multi-column sort (`AllowMultiColumnSort()` reports it) | yes | no | no | 915-926, 1039-1041 |
| `wxDataViewVirtualListModel` truly virtual | yes | yes | no ("not supported by macOS") | 604-614 |
| Item attr `SetBackgroundColour` | since 2.9.4 | since 3.1.1 | since 3.1.4 | 714-720 |
| Item attr `SetStrikethrough` | yes | yes | ignored | 729-733 |
| Current item | may be unselected | may be unselected | always selected; `SetCurrentItem` selects in multi-selection | 1456-1458, 1655-1665 |
| `IsExpanded()` | correct | correct | may be true for leaves (documented bug) | 1597-1603 |
| `CreateEditorCtrl()` | called | called | **never called** | 2626-2629 |
| `wxDataViewRenderer::SetAlignment()` | yes | yes | ignored; aligns as the column header | 2079-2085 |
| `IsEditCancelled()` / veto `EDITING_DONE` | yes | documented as unavailable; [source] text commits (`src/gtk/dataview.cpp:2240-2246`) and custom-editor commits (`src/common/datavcmn.cpp:791-853`) do go through `DoHandleEditingDone`, so veto works | documented as unavailable; [source] native text commit reaches the model before `EDITING_DONE` | 3942-3958 |
| Icon+text markup (`EnableMarkup` on `wxDataViewIconTextRenderer`) | no | yes | no | 2215-2218 |
| `EnableDropTargets` | full | first format only | full | 1377-1385 |
| `SetDragFlags()` | honoured | ignored | ignored | 4012-4018 |
| `GetDropEffect()` | real | `wxDragNone` | `wxDragNone` | 4030-4037 |
| `GetProposedDropIndex()` from `ITEM_DROP_POSSIBLE` | yes | no | yes (all ports from `ITEM_DROP`) | 4052-4060 |
| Generic mouse events (`wxEVT_LEFT_DOWN`…) | yes (bind on `GetMainWindow()`) | "notably it doesn't work in wxGTK" | not all | 994-997 |
| Explorer theme (`wxSystemThemedControl`) | on by default since 3.1.0; `EnableSystemTheme(false)` disables it | — | — | 1000-1003 |
| `GetMainWindow()` ≠ the control | yes | no | no | 1505-1513 |
| `SetAlternateRowColour`, `SetHeaderAttr` | yes | no | no | 1636-1646, 1675-1689 |
| `SetRowHeight` (uniform rows, raise only) | yes | yes | yes (3.1.1+) | 1715-1733 |
| `GetCountPerPage()` | yes | needs ≥ 1 item | yes | 1746-1751 |
| `GetTopItem()` | yes | may be unimplemented | may be unimplemented | 1755-1761 |
| `wxEVT_DATAVIEW_COLUMN_REORDERED` | yes | not sent | yes | 3863-3866 |
| `wxDataViewColumn::SetWidth()` | immediate | applied "only slightly later" (`GetWidth()` returns the old width, 0 initially; widths set before showing apply when visible) | immediate | 2793-2797 |

Other per-port facts: editing starts on a slow double-click or a platform key — "F2 is typical on Windows, Space
and/or Enter is common elsewhere" (2604-2607, 1915-1918); a custom renderer's `StartDrag()` is "Not yet supported"
(2717-2719); `RenderText()` should be used inside `Render()` so text matches native renderers (2708-2714). Calling
`Collapse()` from an event handler was fixed in 3.3.1 (`docs/changes.txt:346`; the generic re-check is described
under [DVC control API](#wxdataviewctrl-control-api)).

OrcaSlicer: `ParamsViewCtrl` (`EditGCodeDialog`) and `DiffViewCtrl` (`UnsavedChangesDialog`) use
`wxDataViewIconTextRenderer::EnableMarkup` only under `#ifdef __linux__`, matching the GTK-only markup row; on MSW
and macOS they use Orca's `BitmapTextRenderer(use_markup = true)`, which handles markup itself. `ObjectList`
force-sets column widths on macOS because the column constructor's width is not applied on 4K/5K screens (see
`ObjectList::create_objects_ctrl`).

## wxDataViewModel contract

**You implement** `IsContainer`, `GetParent`, `GetChildren`, `GetValue` and `SetValue`
(`interface/wx/dataview.h:16-19, 383-385`).

**Ownership.** The model is a `wxRefCounter` and "cannot be deleted directly" (its destructor is protected,
`include/wx/dataview.h:291-294`). `AssociateModel` adds a reference (`interface/wx/dataview.h:1341-1345`;
`src/common/datavcmn.cpp:1248-1263`), and the control drops it in its destructor or when another model (or
`nullptr`) is associated. Either `DecRef()` once after associating, or hold it in `wxObjectDataPtr`
(`interface/wx/dataview.h:68-92`), or keep one deliberate owning reference and `DecRef()` it in your destructor.
Detaching with `AssociateModel(nullptr)` is safe only while you hold your own reference — with the
DecRef-after-associate pattern it destroys the model.

**`wxDataViewItem` is an opaque `void*`.** It must be unique and stable for the item's whole life; `nullptr` means
both "invalid" and "the invisible root" (`interface/wx/dataview.h:788-800`). The ports keep the pointer: GTK stores
it in `GtkTreeIter::user_data` (`src/gtk/dataview.cpp:1843-1845`), the generic control in its tree nodes, Cocoa in
its buffers. Once the node is deleted every copy dangles — copies captured in lambdas and "last selected" members
included — and a later allocation can reuse the address, so a liveness scan by pointer can be fooled.

**Notification ordering (all ports):**
```cpp
// add: insert into the model first, so GetChildren(parent) already returns it
parent_node->Append(node);  model->ItemAdded(wxDataViewItem(parent_node), wxDataViewItem(node));
// delete: remove from the model first, then notify (pointer used only as an ID), then free
parent_node->Remove(node);  model->ItemDeleted(wxDataViewItem(parent_node), wxDataViewItem(node));  delete node;
// IsContainer(parent) must already be correct after the removal
```
Why each port needs it [source]:
- Generic `ItemDeleted` scans its own nodes because the item "was already removed from the model by the time
  ItemDeleted() is called", then asks `IsContainer(parent)` (`src/generic/datavgen.cpp:3260-3310`).
- GTK `ItemAdded` looks the item up in `GetChildren(parent)` and silently returns ("adding non-existent item?") if
  it is not there (`src/gtk/dataview.cpp:3995-4010`); GTK `ItemDeleted` checks `IsContainer(parent)` (`:1885-1895`).
- macOS `ItemAdded/ItemDeleted` re-query the parent's children with `reloadItem:reloadChildren:`; for the root parent
  they call `reloadData`, reloading the whole outline (`src/osx/cocoa/dataview.mm:2284-2300, 2393-2400`).
- Notifications for an item the control never realised (collapsed parent) are ignored on purpose
  (`src/generic/datavgen.cpp:3127-3147`, `src/gtk/dataview.cpp:1830-1838`).

**`Cleared()`** means "everything changed", not "emptied" (`interface/wx/dataview.h:54-56, 141-151`). Every port
discards its tree: generic resets selection and the current row (`src/generic/datavgen.cpp:3404-3425`), GTK does
BeforeReset+AfterReset (`src/gtk/dataview.cpp:1998-2001`), macOS also scrolls to the top
(`src/osx/cocoa/dataview.mm:2384-2391`). Save and restore expansion and selection around it. Re-associating a model
on macOS likewise reloads the outline from scratch ([source] `wxCocoaDataViewControl::AssociateModel` replaces the
data source).

**ItemChanged vs ValueChanged.** Both end in `wxEVT_DATAVIEW_ITEM_VALUE_CHANGED` (`interface/wx/dataview.h:328-336,
388-393`). `ChangeValue()` is `SetValue()` + `ValueChanged()` (`:116-117, 376-381`). On macOS `ValueChanged` calls
`model->GetParent(item)` (`src/osx/dataview_osx.cpp:263-268`), so `GetParent` must work for any live item.

**GetValue/HasValue types.** `GetValue` must fill the type the renderer expects; on a mismatch "nothing will be
shown and a debug error message will be logged" (`interface/wx/dataview.h:258-267`) — concretely
`CheckedGetValue` nulls the value when `!IsCompatibleVariantType()` (`src/common/datavcmn.cpp:854-885`). Container
rows show only column 0 unless `HasContainerColumns()` returns true or `HasValue()` is overridden
(`interface/wx/dataview.h:271-314`).

Pitfalls:
- **Rule:** Mutate the model, then notify; never notify about a node you have already freed or not yet inserted.
  **Why:** GTK silently drops an `ItemAdded` the model does not report; generic and GTK call `IsContainer(parent)`
  during `ItemDeleted`; a freed node read during notification is a use-after-free.
  ```cpp
  delete node; model->ItemDeleted(parent, wxDataViewItem(node));       // Wrong order (and any read is UAF)
  model->ItemAdded(parent, wxDataViewItem(node)); parent_node->Append(node);   // Wrong order
  ```
  Cite: `src/gtk/dataview.cpp:3995-4010`, `src/generic/datavgen.cpp:3260-3310`.
- **Rule:** Use `ItemAdded/ItemDeleted/ItemChanged` for local changes, not `Cleared()`.
  **Why:** `Cleared()` drops selection and expansion on every port and scrolls to the top on macOS.
- **Rule:** Moving a subtree is delete + re-add, and the re-add must announce every descendant again (see
  `ObjectDataViewModel::AddAllChildren`).
  **Why:** the control discarded the subtree with the deleted node; native GTK otherwise shows the moved node
  childless ("just to add a deleted item is not enough on Linux").

## wxDataViewCtrl control API

Contract:
- `GetSelection()` returns an invalid item when nothing *or more than one item* is selected
  (`interface/wx/dataview.h:1532-1540`; `src/common/datavcmn.cpp:1329-1337`). With `wxDV_MULTIPLE` use
  `HasSelection()`/`GetSelections()`. GTK and macOS build `SELECTION_CHANGED` from `dv->GetSelection()`
  (`src/gtk/dataview.cpp:4507-4516`, `src/osx/cocoa/dataview.mm:1824-1832`), so in multi-selection
  `event.GetItem()` can be invalid there; generic passes a concrete row — the clicked one, or the first selected
  row of a Shift range (`src/generic/datavgen.cpp:4822-4827`).
- `UnselectAll()` "only has effect if multiple selections are allowed" (`interface/wx/dataview.h:1709-1713`);
  `SetSelections` silently ignores invalid items (`:1699`).
- [source] `Select()` and `EnsureVisible()` expand ancestors themselves on every port
  (`src/generic/datavgen.cpp:6396-6398, 6503-6505`, `src/gtk/dataview.cpp:5272, 5335`,
  `src/osx/dataview_osx.cpp:584-591, 687-694`). On GTK calling them before `AssociateModel` is a silent
  `wxCHECK_RET` (`src/gtk/dataview.cpp:5270, 5332`).
- [source] Selection events differ: GTK suppresses them for programmatic selection
  (`SelectionEventsSuppressor`, `src/gtk/dataview.cpp:5268-5322`) but deleting a selected row emits
  `SELECTION_CHANGED` through GTK's "changed" signal (`:4507-4516`); macOS emits `SELECTION_CHANGED` for
  `Select`/`SetSelections`/`UnselectAll` (`src/osx/cocoa/dataview.mm:2504-2540, 1824-1832`).
- `SetRowHeight(h)` works on generic, GTK and macOS, only for uniform rows, and only *raises* the height above the
  renderers' minimum (`interface/wx/dataview.h:1715-1733`). [source] On macOS the floor is the font's line height
  and renderer `GetSize()` is not consulted (`src/osx/cocoa/dataview.mm:2615-2628`); per-item row height is
  unsupported (`:2630-2633`); and the control's `SetFont` resets the row height to that default (`:2645-2651`).
- `EditItem(item, col)` "doesn't do anything if the item or this column is not editable"
  (`interface/wx/dataview.h:1361-1369`).
- On MSW bind mouse and motion events on `GetMainWindow()`; generic mouse events do not work on wxGTK
  (`interface/wx/dataview.h:994-997, 1505-1513`). Use a custom renderer's `ActivateCell` instead (`:2593`).
- [source] The generic `Collapse` re-checks whether a `SELECTION_CHANGED` handler already collapsed the node
  (`src/generic/datavgen.cpp:4119-4129`).

Pitfalls:
- **Rule:** With `wxDV_MULTIPLE`, never take `GetSelection()` or `event.GetItem()` as "the" selection.
  ```cpp
  auto it = dvc->GetSelection(); if (it.IsOk()) apply(it);       // Wrong: invalid once two are selected
  wxDataViewItemArray sels; dvc->GetSelections(sels);            // Right
  auto* node = (Node*)event.GetItem().GetID(); if (node) ...     // Right: tolerate a null item on GTK/macOS
  ```
  Cite: `interface/wx/dataview.h:1532-1540`.
- **Rule:** Do not assume a native `wxDataViewCtrl` grows its rows to fit a custom renderer's `GetSize()` — set an
  explicit `SetRowHeight` for the tallest custom content, on all platforms, after any `SetFont` on the control,
  and again in the rescale handler with the new `em`.
  **Why:** on macOS the native row is the font line height, so custom-drawn content (the filament colour badge)
  overflows into adjacent rows; `SetRowHeight` can only raise it, a later `SetFont` resets it, and wx never
  rescales a value passed to a setter, so after a DPI or theme change the badge stops fitting. Setting it on every
  platform keeps spacing consistent (MSW is the generic control, Linux the native GTK one).
  ```cpp
  // create_objects_ctrl():  SetRowHeight(2 * em + FromDIP(2));
  // msw_rescale():          SetRowHeight(2 * em + FromDIP(2));   // repeat with the new em
  ```
  Cite: d5638273c6 (`ObjectList::create_objects_ctrl`, `ObjectList::msw_rescale`). General rescale rule:
  `references/dpi-bitmaps-fonts.md`.
- **Rule:** Before moving a selected item (delete + re-add), `Unselect` it and re-`Select` it afterwards.
  **Why:** the control's selection is not reliably updated by `ItemDeleted`; `ObjectList::update_plate_values_for_items`
  does this ("hotfix for wxDataViewCtrl selection not updated after wxDataViewModel::ItemDeleted()").

## Custom renderers and in-place editing

**Renderer contract.** Implement `Render(rect, dc, state)`, `GetSize()`, `SetValue(variant)` and `GetValue(variant)`;
call `RenderText()` inside `Render` (`interface/wx/dataview.h:2702-2718`). The constructor's `varianttype` is checked
against model values by `IsCompatibleVariantType()` (`:1965-1980, 2063-2076`). Editing needs `HasEditorCtrl()` →
true, `CreateEditorCtrl()` and `GetValueFromEditorCtrl()`.

**Editing flow** (common code: generic, GTK custom renderers, and any explicit `renderer->StartEditing()`):
1. `wxEVT_DATAVIEW_ITEM_START_EDITING` is sent; a veto aborts (`src/common/datavcmn.cpp:715-725`).
2. `CreateEditorCtrl(GetMainWindow(), rect, CheckedGetValue(...))` runs; the value is **null** after a type
   mismatch or when `HasValue(item, col)` is false (`:854-885`). The returned control is stored as `m_editorCtrl`
   (`:733`); returning `nullptr` cancels.
3. wx pushes a `wxDataViewEditorCtrlEvtHandler` onto *the returned control only* and focuses it — on native GTK on
   idle (`:742-751`). That handler commits on Enter, cancels on Esc and commits on kill-focus unless focus moved to
   a child of the editor (`src/common/datavcmn.cpp:1129-1198`). Key and focus events of a compound editor's children
   never reach it.
4. `FinishEditing()` calls `GetValueFromEditorCtrl(m_editorCtrl, value)` (`:799`), then hides the editor and deletes
   it **later** via `wxPendingDelete` (`:765-785`). `Validate()` runs after the editor is gone
   (`interface/wx/dataview.h:2125-2135`). `wxEVT_DATAVIEW_ITEM_EDITING_DONE` follows; if allowed,
   `model->ChangeValue()` raises `VALUE_CHANGED` (`src/common/datavcmn.cpp:783-853`).

**macOS (native).** `CreateEditorCtrl` "will be never called there" (`interface/wx/dataview.h:2626-2629`).
[source] `EditItem` → `StartEditor` → `[NSOutlineView editColumn:row:…]` starts native text editing of the cell
(`src/osx/dataview_osx.cpp:757-760`, `src/osx/cocoa/dataview.mm:2564-2566`); `START_EDITING` comes from
`textShouldBeginEditing:`, and vetoing it returns NO to native editing (`src/osx/cocoa/dataview.mm:1885-1904`).
`wxEVT_DATAVIEW_ITEM_EDITING_STARTED` follows from `textDidBeginEditing:` (`:1936`) and is not vetoable
(`wxDataViewRendererBase::NotifyEditingStarted` never checks `IsAllowed()`, `src/common/datavcmn.cpp:756-763`).
A custom cell's `objectValue` is a `wxCustomRendererObject` (`wxDataViewCustomRenderer::MacRender`,
`src/osx/cocoa/dataview.mm:2926-2929`), so the field shows its description `wxCustomRendererObject: 0x…`
(`src/osx/cocoa/dataview.mm:117-160, 1112-1123`). On commit `wxDataViewRenderer::OSXOnCellChanged` builds a
**"string"** variant and calls `model->ChangeValue()` directly (`src/osx/cocoa/dataview.mm:2766-2797`), before
`EDITING_DONE` is sent (`:1944-1955`) — the model's `SetValue` must type-check, and vetoing `EDITING_DONE` cannot
stop it. `SetCustomRendererPtr`/`SetCustomRendererItem` are undocumented wxOSX-only members
(`include/wx/osx/dataview.h:245-259`); `wxDataViewCtrl::FinishCustomItemEditing` reads them to close a custom
editor when native editing starts (`src/osx/dataview_osx.cpp:779-786`, called from `src/osx/cocoa/dataview.mm:1930`).

Pitfalls:
- **Rule:** In `GetValueFromEditorCtrl()`, `dynamic_cast` the editor to the expected type and return `false` on
  mismatch; in `CreateEditorCtrl()`, check the incoming variant before extracting.
  **Why:** wx only ever passes the control your `CreateEditorCtrl` returned, so the cast is defence in depth against
  app-side bookkeeping confusion — cheap insurance where a wrong `static_cast` reads garbage and crashes on commit.
  It matters most on macOS, where native editing never calls `CreateEditorCtrl` at all and Orca's editor exists only
  because `start_filament_editor` calls `StartEditing` directly.
  ```cpp
  auto* c = static_cast<::ComboBox*>(ctrl);          // Wrong
  auto* c = dynamic_cast<::ComboBox*>(ctrl);         // Right
  if (!c || c->GetSelection() < 0) return false;
  ```
  Cite: c965b2a5b3 (`ExtraRenderers.cpp`, `BitmapChoiceRenderer::GetValueFromEditorCtrl`;
  `BitmapTextRenderer::GetValueFromEditorCtrl` uses `wxDynamicCast`); `src/common/datavcmn.cpp:733, 799`.
- **Rule:** On macOS, do not open a custom-renderer column editor through `EditItem()` or let the native
  start-editing path run: `Veto()` `wxEVT_DATAVIEW_ITEM_START_EDITING`, `CallAfter` a call to the renderer's own
  `StartEditing(item, GetItemRect(item, column))`, then `SetCustomRendererPtr`/`SetCustomRendererItem`. Guard against
  re-entry and an already-open editor (`renderer->GetEditorCtrl()`), re-validate the item inside the lambda, and keep
  it all under `#ifdef __WXOSX__`.
  **Why:** `EditItem()` on Cocoa enters native text editing of the `wxCustomRendererObject` instead of your editor,
  glitching the cell and crashing on commit. The re-entry guard is needed because `StartEditing` itself sends
  `START_EDITING` (`src/common/datavcmn.cpp:716-725`), which the same handler would otherwise veto.
  ```cpp
  void ObjectList::OnStartEditing(wxDataViewEvent& event) {
  #ifdef __WXOSX__
      if (event.GetColumn() == colFilament) {
          if (m_starting_filament_editor) return;          // our own StartEditing re-entering
          event.Veto();
          CallAfter([this, item = event.GetItem()] {
              if (!is_live_model_item(item)) return;
              start_filament_editor(item);                 // StartEditing + SetCustomRendererPtr/Item
          });
          return;
      }
  #endif
  ```
  Cite: c965b2a5b3 (`ObjectList::OnStartEditing`, `ObjectList::start_filament_editor`).
- **Rule:** A compound editor commits by calling the renderer's `FinishEditing()` (or `CancelEditing()`) from its
  own commit event.
  **Why:** wx's Enter/Esc/kill-focus handler sits only on the top editor window; a `::ComboBox` or TextInput editor's
  inner controls never reach it, and kill-focus commit is unreliable on Linux.
  ```cpp
  c_editor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) { e.StopPropagation(); FinishEditing(); });
  ```
  Cite: `BitmapChoiceRenderer::CreateEditorCtrl`; `src/common/datavcmn.cpp:742-751, 1129-1198`.
- **Rule:** Do not touch the editor after `FinishEditing()`/`CancelEditing()`, and do not rely on vetoing
  `EDITING_DONE` outside MSW.
  **Why:** the editor is queued on `wxPendingDelete` and dies at the next idle or yield; on macOS the value is
  already in the model when `EDITING_DONE` arrives.

## wxVariant with custom objects

`wxIMPLEMENT_VARIANT_OBJECT(T)` (and the older `IMPLEMENT_VARIANT_OBJECT`) generates `T& operator<<(T&, const
wxVariant&)`, which only `wxASSERT`s the type and then C-casts `GetData()` (`include/wx/variant.h:525-532`). The
custom type name is the wxObject class name (`GetType()` → `GetClassInfo()->GetClassName()`, `:515-518`); a null
variant's type is `"null"` (`interface/wx/variant.h:420-425`). wx's own `wxBitmap << variant` extraction is generated
the same way.

Pitfalls:
- **Rule:** Check `!v.IsNull() && v.GetType() == "<Class>"` before every `obj << v` extraction.
  **Why:** the assert is compiled out of Orca in every configuration, so a wrong-typed variant is read as garbage and
  a null one dereferences `nullptr`. Wrong types really happen: `CheckedGetValue` passes a null variant to
  `CreateEditorCtrl` after a model/renderer type mismatch, and native macOS editing hands the model's `SetValue` a
  `"string"` variant built from the field text (`src/osx/cocoa/dataview.mm:2766-2797`).
  ```cpp
  DataViewBitmapText data; data << variant;                                    // Wrong: blind extraction
  if (variant.IsNull() || variant.GetType() != wxT("DataViewBitmapText"))      // Right
      return false;
  DataViewBitmapText data; data << variant;
  ```
  Cite: c965b2a5b3 (`ExtraRenderers.hpp`, `ObjectDataViewModelNode::SetValue`).
  Note: `wxObject`'s default copy and assignment share ref-data with correct refcounting
  (`include/wx/object.h:324-338`); `DataViewBitmapText`'s field-copying operators are harmless but not required.

## Orca replacement widgets: event contracts

Orca's interactive controls replace raw wx ones (catalog, constructors and quirks: `references/orca-widgets.md`).
They emit the native event *types* through `GetEventHandler()->ProcessEvent`, so command events propagate to parents
like native ones and stop at dialogs (`wxWS_EX_BLOCK_EVENTS`) and, on MSW and macOS, at popups (not on wxGTK,
`references/events.md` §5) — but ids, event objects, event classes and setter side effects differ. "Ancestor" in the
Bind column means an ancestor bound by event type; binding an ancestor by the widget's id is discouraged
(`references/events.md` §10):

| Widget | Replaces | Emits (user action) | Id / object | Setter side effects | Bind |
|---|---|---|---|---|---|
| `::TextInput` | `wxTextCtrl` | inner `wxEVT_TEXT` (propagates); `wxEVT_TEXT_ENTER`, `wxEVT_KILL_FOCUS` re-sent to the wrapper only | TEXT: inner ctrl's id/object; ENTER/KILL_FOCUS: wrapper's id, inner object | `GetTextCtrl()->SetValue` → `wxEVT_TEXT` | TEXT: wrapper or `GetTextCtrl()`; ENTER/KILL_FOCUS: wrapper or `GetTextCtrl()`, never a parent |
| `::ComboBox` | `wxComboBox`/`wxChoice` | `wxEVT_COMBOBOX` (int = index, string = text); DROPDOWN/CLOSEUP | COMBOBOX: combo's auto-generated id (the ctor `id` is ignored) and object; DROPDOWN/CLOSEUP: id 0, no object | `SetSelection`/`SetValue` → `wxEVT_TEXT` when the text ctrl is shown or `CB_NO_TEXT`; `SelectAndNotify` → `wxEVT_COMBOBOX` | COMBOBOX on the combo (or an ancestor); DROPDOWN/CLOSEUP on the combo without id |
| `::SpinInput` | `wxSpinCtrl` | `wxEVT_SPINCTRL` (a `wxCommandEvent`); `EVT_SPINCTRL_TEXT` per parsable keystroke; inner `wxEVT_TEXT` | SPINCTRL / SPINCTRL_TEXT: spinner id/object | `SetValue(int)` → `EVT_SPINCTRL_TEXT` + `wxEVT_TEXT`, no `wxEVT_SPINCTRL` | SPINCTRL on the spinner or an ancestor, handler takes `wxCommandEvent&` |
| `::CheckBox`, `SwitchButton` | `wxCheckBox` | `wxEVT_TOGGLEBUTTON` | native | `SetValue` silent | on the widget (with `Skip()`) or an ancestor; never `wxEVT_CHECKBOX` |
| `::RadioGroup` | `wxRadioBox` | `wxEVT_COMMAND_RADIOBOX_SELECTED` | group id, no object | **every** `SetSelection` emits | on the group or an ancestor |
| `::TabCtrl` | tab bar | `wxEVT_TAB_SEL_CHANGING` then `wxEVT_TAB_SEL_CHANGED` (plain `wxCommandEvent`) | tab ctrl id/object; CHANGING carries the old index | `SelectItem(i)` emits both when `i` changes | on the ctrl or an ancestor; cannot veto |
| `Button` | `wxButton` | `wxEVT_BUTTON` | button id/object | — | on the button or an ancestor |

### TextInput

`::TextInput` (`Widgets/TextInput.cpp`, `TextInput::Create`; ctor `TextInput(parent, text, label = "", icon = "",
pos, size, style)`) is a `StaticBox` frame around a real `wxTextCtrl` (on MSW the `TextCtrl` subclass in
`Widgets/TextCtrl.h`). Style flags pass through to the inner control (alignment flags are stripped); tooltips forward.
- It always ORs in `wxTE_PROCESS_ENTER`, and its `wxEVT_TEXT_ENTER` handler does not `Skip()`: Enter in a TextInput
  never activates a dialog's default button and never propagates to parents.
- The inner control's `wxEVT_TEXT_ENTER` and `wxEVT_KILL_FOCUS` are re-dispatched with the wrapper's id via
  `ProcessEventLocally`, which runs the wrapper's own handlers but [source] never `TryAfter`, so never the parents
  (`src/common/event.cpp:1582-1589`; the doc at `interface/wx/event.h:626-650` says otherwise).
- `wxEVT_TEXT` is a command event from the inner control and propagates normally, carrying the inner control's id
  and object. Key, char and focus-in events exist only on `GetTextCtrl()`.
- The value API lives on `GetTextCtrl()`. `TextInput::SetLabel()` sets the painted side label, not the text.
- A handler bound later on `GetTextCtrl()` for ENTER or KILL_FOCUS runs before the wrapper's internal one
  (dynamic handlers run most-recently-bound first, `docs/doxygen/overviews/eventhandling.h:475-482`) and must
  `Skip()` so `OnEdit()` and the re-dispatch still run.
- `TextInput` has no id parameter: its `StaticBox` is created with `wxID_ANY`, so the wrapper id seen by
  ENTER/KILL_FOCUS handlers is auto-generated.

### ComboBox

`::ComboBox` (`Widgets/ComboBox.cpp`) is `wxWindowWithItems<TextInput, wxItemContainer>` with an owned `DropDown`
popup; ctor `ComboBox(parent, id, value = "", pos, size, n, choices[], style)`. The `id` argument is ignored:
the constructor calls `TextInput::Create`, which creates the window with `wxID_ANY`, so `GetId()` is auto-generated
and a parent bound with the id you passed never fires. `wxCB_READONLY` hides the text control and paints the value;
`CB_NO_DROP_ICON`/`CB_NO_TEXT` are Orca style flags.
- `SetSelection(n)` sends no `wxEVT_COMBOBOX` (matching wx) and returns early when `n` is already selected. When the
  inner text control is shown (editable) or `CB_NO_TEXT` is set, it — and `SetValue` — go through
  `GetTextCtrl()->SetValue()`, which sends a propagating `wxEVT_TEXT`. `SelectAndNotify(n)` selects and sends
  `wxEVT_COMBOBOX`.
- `SetLabel`/`GetLabel` are the displayed value; `SetTextLabel` writes the wrapper's painted label directly.
- Only untyped `void*` client data is supported: every `Append` overload calls
  `SetClientDataType(wxClientData_Void)`, and the caller owns and frees the data. `DeleteOneItem()` skips the base
  class's client-object reset.
- `Clear()`, `Insert()`, `Set()` and `Delete()` (via `DoClear`/`DoInsertItems`/`DoDeleteOneItem` →
  `DropDown::Invalidate(true)`) reset the selection to -1 without clearing the shown text; call
  `SetSelection`/`SetValue` afterwards.
- Mouse-wheel selection is disabled. `GetDropDown()` exposes the popup (e.g. `SetUseContentWidth(true)`).

### SpinInput

`::SpinInput` (`Widgets/SpinInput.cpp`; ctor `SpinInput(parent, text, label = "", pos, size, style, min = 0,
max = 100, initial = 0, step = 1)`) is integer-only: an inner `TextCtrl` with `wxTextValidator(wxFILTER_DIGITS)` plus
two arrow `Button`s with key-repeat; `SetValue/GetValue/SetRange/SetStep`.
- It commits — clamps, then sends `wxEVT_SPINCTRL` — on Enter, kill-focus and arrow keys only if the value changed,
  but on every arrow-button press and key-repeat tick even when the value is pinned at `min`/`max`
  (`SpinInput::createButton`, `onTimer`); mouse-wheel stepping is disabled (`EVT_MOUSEWHEEL` is commented out of
  the event table, so `mouseWheelMoved` never runs). `EVT_SPINCTRL_TEXT` (int + string) is the live
  per-keystroke event.
- `wxEVT_SPINCTRL` is built as a `wxCommandEvent` with no `SetInt`: bind with a `wxCommandEvent&` handler and read
  `GetValue()` from the spinner; a `wxSpinEvent&` handler would read `GetPosition()` == 0 from a mis-typed object.
- `SetValue(int)` clamps and sends no `wxEVT_SPINCTRL`, but sends `EVT_SPINCTRL_TEXT` and a propagating `wxEVT_TEXT`
  (it uses the inner `SetValue`). `SetRange` does not re-clamp the current value — set the range before the value.
- Typing accepts digits only; `-` cannot be typed, so negative ranges need another control.
- Enter and kill-focus are re-dispatched locally, as in TextInput. Dialogs sometimes `Disable()` a SpinInput to
  force a commit before reading; that depends on the platform delivering `wxEVT_KILL_FOCUS` to the focused inner
  control when it is disabled, which wx does not promise — read the inner text and commit explicitly when it matters.

### CheckBox and SwitchButton

`::CheckBox` (ctor `CheckBox(parent, id = wxID_ANY)`, no label — pair it with a `wxStaticText`/`Label`, as
`CloneDialog` does) and `SwitchButton` are `wxBitmapToggleButton`s. They emit `wxEVT_TOGGLEBUTTON`, never
`wxEVT_CHECKBOX`; `SetValue` sends nothing. The half state is drawn only (`SetHalfChecked`/`IsHalfChecked`); any click
clears it via the widget's own `wxEVT_TOGGLEBUTTON` handler (bound in the constructor), which also swaps the on/off
bitmap. Handlers bound later on the same widget run first, so they must `Skip()`, or the bitmap keeps showing the old
state and the half state is never cleared (`PreferencesDialog::create_item_bambu_cloud` marks this with "let
CheckBox::update() refresh the bitmap"). Inside `Slic3r::GUI` write `::CheckBox` — `Field.hpp` declares a `Slic3r::GUI::CheckBox` field class.

### RadioGroup, TabCtrl

- `::RadioGroup::SetSelection(i)` **always** sends `wxEVT_COMMAND_RADIOBOX_SELECTED` — for programmatic calls and
  for an unchanged index too — the opposite of `wxRadioBox`. Guard the handler or set a flag around sync code.
- `::TabCtrl::SelectItem(i)` sends `wxEVT_TAB_SEL_CHANGING` (cannot be vetoed; `sendTabCtrlEvent` always returns true)
  then `wxEVT_TAB_SEL_CHANGED`; switching content is the caller's job. `TabCtrl::AssignImageList` is Orca's own
  method, not `wxWithImages`. `SelectItem` also sends a synthetic `wxEVT_CHECKBOX` (id 0, object = the tab `Button`)
  to the old and new tab buttons to toggle their `StateHandler` Checked state; the state handler `Skip()`s it, so it
  propagates to the TabCtrl's ancestors — an ancestor bound to `wxEVT_CHECKBOX` without an id filter receives it.

### Field widgets

Settings fields (`Field.cpp`, built by `OptionsGroup::build_field`) wrap these widgets — `TextCtrl` → `::TextInput`
(a raw multi-line `wxTextCtrl` when `opt.multiline`), `CheckBox` → `::CheckBox`, `SpinCtrl` → `::SpinInput`,
`Choice` and `PrinterAgentChoice` → `::ComboBox` (`choice_ctrl`; a `Choice` is editable with `wxTE_PROCESS_ENTER`
for open-enum GUI types without a dynamic list, `wxCB_READONLY` otherwise), `ColourPicker` → `wxColourPickerCtrl`,
`PointCtrl` → two `::TextInput`, `StaticText` → `wxStaticText(wxST_ELLIPSIZE_MIDDLE)`, `SliderCtrl` → `wxSlider` +
`wxTextCtrl`, `PluginConfigField` → `Button`. Programmatic field updates use the
`m_disable_change_event` bracket around `SetValue`, which works because wx delivers `wxEVT_TEXT` synchronously. Field
machinery, pooling and the bind-with-id rule: `references/orca-settings-ui.md`.

Pitfalls:
- **Rule:** Bind TextInput/SpinInput ENTER and KILL_FOCUS on the widget (or its `GetTextCtrl()`), never on a parent;
  bind `::ComboBox` events on the combo, never by the id passed to its constructor.
  ```cpp
  dialog->Bind(wxEVT_TEXT_ENTER, &Dlg::on_enter, this, input->GetId());   // Wrong: never fires
  input->Bind(wxEVT_TEXT_ENTER, &Dlg::on_enter, this);                    // Right
  panel->Bind(wxEVT_TEXT, h, input->GetId());                             // Wrong: TEXT carries the inner id
  input->Bind(wxEVT_TEXT, h);                                             // Right
  auto* c = new ::ComboBox(this, ID_MODE); Bind(wxEVT_COMBOBOX, h, ID_MODE);    // Wrong: ID_MODE is ignored
  c->Bind(wxEVT_COMBOBOX, h);                                             // Right
  ```
  Cite: `TextInput::Create`; `ComboBox::ComboBox`; `src/common/event.cpp:1582-1589`.
- **Rule:** Bind `::CheckBox`/`SwitchButton` with `wxEVT_TOGGLEBUTTON` (and `Skip()` when bound on the widget itself),
  and `SpinInput` with a `wxCommandEvent&` handler.
  ```cpp
  cb->Bind(wxEVT_CHECKBOX, h);                                            // Wrong: never sent
  cb->Bind(wxEVT_TOGGLEBUTTON, [](wxCommandEvent& e) { apply(); });       // Wrong: stale bitmap
  cb->Bind(wxEVT_TOGGLEBUTTON, [](wxCommandEvent& e) { apply(); e.Skip(); });  // Right
  spin->Bind(wxEVT_SPINCTRL, [](wxSpinEvent& e) { use(e.GetPosition()); });     // Wrong
  spin->Bind(wxEVT_SPINCTRL, [spin](wxCommandEvent&) { use(spin->GetValue()); });  // Right
  ```
  Cite: `CheckBox::CheckBox`, `SwitchButton::SwitchButton`; `SpinInput::sendSpinEvent`.
- **Rule:** Treat an editable `::ComboBox`'s `SetSelection`/`SetValue` as emitting `wxEVT_TEXT`, and a
  `RadioGroup::SetSelection` as emitting its selection event.
  **Why:** both fire synchronously inside model→view refreshes and re-enter change handlers.

## ObjectList, ObjectDataViewModel, ExtraRenderers

`ObjectList` (`GUI_ObjectList.cpp/.hpp`) is the sidebar's `wxDataViewCtrl` (`wxDV_MULTIPLE | wxNO_BORDER |
wxDV_NO_HEADER`) over `ObjectDataViewModel` (`ObjectDataViewModel.cpp/.hpp`), a custom `wxDataViewModel` of
`ObjectDataViewModelNode`s typed by the `ItemType` bitmask (`itPlate, itObject, itVolume, itInstanceRoot, itInstance,
itSettings, itLayerRoot, itLayer, itInfo`) with columns `ColumnNumber` (`colName, colHeight, colPrint, colFilament,
colSupportPaint, colColorPaint, colSinking, colEditing`). Per-object, per-part and per-layer overrides appear as an
`itSettings` child; selecting it opens the model-scope tabs (`TabPrintModel` → `TabPrintPlate/Object/Part/Layer`),
which reuse the `OptionsGroup`/`Field` machinery bound to the items' `ModelConfig`s instead of a preset config. Which
options are offered: `references/orca-settings-ui.md`.

**Model design.**
- Ownership: `ObjectList::create_objects_ctrl` does `new ObjectDataViewModel; AssociateModel(m_objects_model);`
  without an immediate `DecRef`, and `ObjectList::~ObjectList` calls `m_objects_model->DecRef()` — ObjectList owns one
  reference for its lifetime. That is what makes the macOS bulk-update pattern safe: `add_objects_to_list` and
  `update_plate_values_for_items` detach with `AssociateModel(nullptr)` and reattach afterwards, so the outline
  reloads once [source] instead of once per notification.
- Each `ObjectDataViewModelNode*` *is* its `wxDataViewItem` ID; children live in the parent's pointer array.
  `ObjectDataViewModel::Delete` removes the node from its parent (or from `m_plates`/`m_objects`), calls
  `ItemDeleted(parent, item)`, then deletes it.
- `HasContainerColumns()` returns true so container rows draw their icon columns; `IsContainer(invalid)` is true for
  the root.
- Re-parenting (`ReparentObject`, `ReorganizeChildren`, `ReorganizeObjects`) is remove → `ItemDeleted` → insert →
  `ItemAdded`; `ReorganizeChildren` and `ReorganizeObjects` then call `AddAllChildren`, which re-announces the subtree
  and expands the moved node. `ReparentObject` (plate change) does not; `ObjectList::update_plate_values_for_items`
  re-expands and re-selects the item itself.
- `GetColumnType` returns `"DataViewBitmapText"` for `colName`/`colFilament`, but wx 3.3 never calls it (deprecated,
  `include/wx/dataview.h:285-289`); what wx checks is the renderer's `varianttype`, which both ExtraRenderers set to
  `"DataViewBitmapText"`. `ObjectDataViewModelNode::SetValue` type-checks `"DataViewBitmapText"` for those columns.

**Renderers** (`ExtraRenderers.cpp/.hpp`; `ENABLE_NONCUSTOM_DATA_VIEW_RENDERING` is 0, so both are
`wxDataViewCustomRenderer`s):
- `BitmapTextRenderer` (name column): editor is a `wxTextCtrl` with `wxTE_PROCESS_ENTER`; editing is gated by
  `set_can_create_editor_ctrl_function`; `GetValueFromEditorCtrl` refuses names with illegal filename characters
  and `ObjectList::OnEditingDone` reports `WasCanceled()` through a deferred warning.
- `BitmapChoiceRenderer` (filament column): editor is a `::ComboBox` (`wxCB_READONLY | CB_NO_DROP_ICON |
  CB_NO_TEXT`) filled from `get_extruder_color_icons()`. It force-opens the popup on focus — on GTK deferred with
  `CallAfter` and an `IsShownOnScreen()` check, because the editor "may receive focus before its native window is
  mapped" (popup parenting: `references/popups-menus.md`) — and calls `FinishEditing()` itself on `wxEVT_COMBOBOX`.

**macOS editing.** The filament editor is opened by the veto + `CallAfter` + `start_filament_editor` pattern
([above](#custom-renderers-and-in-place-editing)); `wxEVT_DATAVIEW_ITEM_ACTIVATED` on `colFilament` calls
`start_filament_editor` on macOS and `EditItem` elsewhere. While starting, `m_filament_editor_item` tells the
renderer's callbacks which item is being edited (selection may differ). The bitmap columns are created
`wxDATAVIEW_CELL_EDITABLE` on macOS only, so a click starts native editing, and `ObjectList::OnEditingStarted`
(non-MSW branch) treats the resulting `EDITING_STARTED` as a per-cell click and runs the column's action
(printable toggle, paint gizmos, sinking, settings reset). Its `event.Veto()` there has no effect — only
`START_EDITING` is vetoable ([above](#custom-renderers-and-in-place-editing)); stopping native editing needs a veto in
`OnStartEditing`, as the filament column does.

**Selection and events.**
- `m_prevent_list_events` brackets programmatic `Select`/`UnselectAll`/model mutation; the `SELECTION_CHANGED`
  handler returns early on macOS when it is set, and `ObjectList::selection_changed` checks it on every port —
  covering GTK's selection events from drag-and-drop and row deletion.
- With Shift held the handler recovers the last-clicked item from `GetSelections()`, because the event item is not
  reliable in multi-selection; a null `event.GetItem()` is tolerated.
- `is_live_model_item` (macOS only) re-validates a deferred item by scanning `GetAllChildren` for its pointer. It
  cannot detect a freed node whose address was reused; prefer re-resolving from object/volume indices when you can.

**Row height and fonts.** `SetRowHeight(2 * em + FromDIP(2))` in `create_objects_ctrl` and in `msw_rescale`
(d5638273c6). `ObjectList::ObjectList` calls `SetFont(Label::sysFont(13))` on every platform, before
`create_objects_ctrl` — necessary on macOS because the control's `SetFont` resets the row height. The macOS-only
"don't `SetFont`" guard lives in `DPIAware`'s constructor and concerns only a top-level window's default font
(`references/dpi-bitmaps-fonts.md`).

**Global renderer on MSW.** `ObjectList::ObjectList` calls `wxRendererNative::Set(new wxRenderer)` — a
`wxDelegateRendererNative` overriding `DrawItemSelectionRect`, `DrawFocusRect`, `DrawTreeItemButton` and
`DrawItemText` with Orca colours (through `StateColor::darkModeColorFor`). `Set` replaces "the global renderer"
(`interface/wx/renderer.h:651-657`): once `ObjectList` exists, every generic `wxDataViewCtrl` on MSW
(`src/generic/datavgen.cpp:2735-2974`) and every `RenderText` call (`src/common/datavcmn.cpp:1102`) draws with them.
`DrawItemText` draws at the rect's top-left without alignment or ellipsis. Expect this when a new MSW data view looks
"wrong". Dark styling of data views (`UpdateDVCDarkUI`): `references/colours-dark-mode.md`.

Pitfalls:
- **Rule:** Every deferred lambda that holds a `wxDataViewItem` re-validates it before use.
  **Why:** the model can be rebuilt between queueing and execution; the item is a raw node pointer.
  ```cpp
  CallAfter([this, item] { start_filament_editor(item); });              // Wrong
  CallAfter([this, item] { if (!is_live_model_item(item)) return;        // Right (or re-resolve by index)
                           start_filament_editor(item); });
  ```
  Cite: c965b2a5b3 (`ObjectList::is_live_model_item`). Liveness rules for deferred calls: `references/events.md`
  §CallAfter.

## ObjectGrid (GUI_ObjectTable)

`ObjectTableDialog` (a `DPIDialog`, opened by `Plater::PopupObjectTable`) hosts `ObjectGrid` (a `wxGrid`) with
`ObjectGridTable` (a `wxGridTableBase` set with `AssignTable`).
- Per-cell editors and renderers are installed with `SetCellEditor`/`SetCellRenderer`, which take ownership.
- `GridCellFilamentsEditor` and `GridCellChoiceEditor` derive from `wxGridCellChoiceEditor` but create a
  `::ComboBox` as `m_control` and shadow `Combo()`; they override `BeginEdit`/`EndEdit`. Any change to them, or a new
  editor of this shape, must account for the base `Reset()`/`GetValue()`/`SetParameters()` cast
  ([wxGrid](#wxgrid)).
- `GridCellSupportEditor::DoActivate`, the copy path in `ObjectGrid::OnKeyDown` and `ObjectGrid::paste_data` handle
  an empty `GetSelectedBlocks()` (46e47cec0a).
