# wx 3.1.5 → 3.3.2: changes Orca code can trip on

OrcaSlicer moved from wxWidgets **3.1.5** to **3.3.2** (8248b06337, #12941). This file digests the
changes between those versions that matter to GUI code — the 3.1.6–3.2.0 incompatible changes, the
3.3 incompatible changes, and the notable 3.3.0/3.3.1/3.3.2 changes — each with what it means for
Orca, plus the migration already done in Orca as rules for new code. Read it when code written
from older wx knowledge behaves oddly, when a wx doc says "since 3.3", or when reviewing code near
the workarounds in §8. Where this file and prior wx knowledge disagree, this file wins.

Contents: [Rules](#rules) · [1 Reading the change logs](#1-reading-the-change-logs) ·
[2 3.1.6 → 3.2.0](#2-changes-from-316-to-320) · [3 3.3 behaviour changes](#3-33-behaviour-changes-that-compile) ·
[4 3.3 build changes](#4-33-changes-that-break-the-build) · [5 3.3.0](#5-330-notable-changes) ·
[6 3.3.1](#6-331-notable-fixes) · [7 3.3.2](#7-332-notable-changes) ·
[8 Migration done in Orca](#8-migration-already-done-in-orca)

All `docs/`, `interface/`, `include/`, `src/`, `build/` cites are relative to the pinned wx tree
(located as in `SKILL.md` §Ground truth), except paths
explicitly called Orca's (`deps/…`, Orca's `src/CMakeLists.txt`) and bare Orca file + symbol cites.

## Rules

1. Read every "now asserts" in the change logs as "now silently returns or does nothing" in Orca:
   wx asserts are compiled out. Validate arguments yourself. → [§1](#1-reading-the-change-logs)
2. Target 3.3.2 only. Do not add `wxCHECK_VERSION` / `wxVERSION_NUMBER` /
   `wxVERSION_EQUAL_OR_GREATER_THAN` branches for older wx. → [§8](#8-migration-already-done-in-orca)
3. Mark every override of a wx virtual `override`. 3.2/3.3 changed parameter types (`wxBitmapBundle`,
   `wxReadOnlyDC`, `wxWindowBase*`, `wxVersionContext`); a stale signature must fail to compile, not
   become a silent overload. → [§4](#4-33-changes-that-break-the-build)
4. Call `Show()` before `Raise()` on a window that may be hidden, guarded by `!IsShown()` (a redundant
   `Show()` is not inert on GTK3). → [§3](#3-33-behaviour-changes-that-compile)
5. Never call `SetLabel`/`SetLabelText` on a `wxTextCtrl`; use `ChangeValue` (quiet) or `SetValue`.
   → [§3](#3-33-behaviour-changes-that-compile)
6. Do not use `wxTRANSPARENT_WINDOW` (it is `0`). Give the panel an explicit background colour, or set
   `wxBG_STYLE_TRANSPARENT` before `Create()`. → [§3](#3-33-behaviour-changes-that-compile)
7. Decide Orca's theme with `GUI_App::dark_mode()`, never `wxSystemAppearance::IsDark()` or
   `SelectLightDark()` (on MSW they report the OS app mode); ask about the OS with `AreAppsDark()` /
   `IsSystemDark()`. → [§3](#3-33-behaviour-changes-that-compile)
8. Request multisampling explicitly (`.SampleBuffers(1).Samplers(4)` or `WX_GL_SAMPLE_BUFFERS` +
   `WX_GL_SAMPLES`); `wxGLAttributes::Defaults()` no longer includes it. → [§3](#3-33-behaviour-changes-that-compile)
9. Size a `wxImageList` in physical pixels (the bitmaps' `GetSize()`), or use the `wxBitmapBundle`
   image APIs. → [§3](#3-33-behaviour-changes-that-compile)
10. Return translated strings as `wxString` by value. → [§3](#3-33-behaviour-changes-that-compile)
11. Escape `&` in user text used as a control label or book page title. → [§3](#3-33-behaviour-changes-that-compile)
12. `wxDynamicCast` only on a pointer whose static type derives from `wxObject`; use `dynamic_cast`
    for mixin interfaces (`wxComboPopup`, `wxItemContainer`, `wxTextEntry`). → [§4](#4-33-changes-that-break-the-build)
13. Build a `wxArrayString` with `Add()` or an initializer list, walk wx lists with
    `compatibility_iterator` or range-for, and make a `wxString` the first operand of a mixed
    `+` chain. → [§4](#4-33-changes-that-break-the-build)
14. MSW windows are not double-buffered by default in 3.3.2; a custom-painted control buffers itself.
    → [§5](#5-330-notable-changes)
15. `wxEVT_DPI_CHANGED` now fires on GTK3 too; such handlers must be correct on GTK, and every handler
    you bind calls `Skip()` (`DPIAware`'s own is the one exception). → [§5](#5-330-notable-changes)
16. wx MSW dark mode cannot be switched at runtime; do not replace Orca's NppDarkMode with it without
    keeping live theme switching. → [§5](#5-330-notable-changes)
17. After `wxStaticText::SetLabel`, re-wrap with `Wrap(-1); Wrap(w);` (or use `Label`); `wxST_WRAP`
    is opt-in and works only inside a sizer. → [§7](#7-332-notable-changes)
18. On Linux/X11 call `wxGLCanvas::PreferGLX()` before any GL use; on Unix set a swap interval explicitly
    if frame pacing matters (wx forces 0). → [§7](#7-332-notable-changes)
19. Compare `wxGrid::GetSelectedBlocks()` `begin()` with `end()` before dereferencing.
    → [§8](#8-migration-already-done-in-orca)
20. Keep the post-upgrade workarounds (MainFrame `WS_CAPTION` masking, macOS deep-link handler,
    `MSWEnableDarkMode` ordering, `SidePopup` anchoring, MSW `GLCanvas3D::on_paint` render) when
    touching that code. → [§8](#8-migration-already-done-in-orca)

## 1 Reading the change logs

**Sources.** `docs/changes.txt` lists changes since 3.2: incompatible behaviour changes :11-146,
build-breaking changes :149-252, 3.3.2 :255-330, 3.3.1 :333-377, 3.3.0 :380-622.
`docs/changes_32.txt` covers 3.x → 3.2.0: the cumulative "INCOMPATIBLE CHANGES SINCE 3.0.x" list
:9-233, 3.2.0 :235-276, 3.1.7 :279-340, 3.1.6 :343-444. wxQt, wxiOS, wxUniv, wxMotif and wxGTK1
items are left out here.

**Coverage gaps.**
- The "since 3.0.x" list in `changes_32.txt` is cumulative over all of 3.1.x. Most of it was already
  in force in 3.1.5; §2.1 lists only what was added after 3.1.5, §2.3 the older items worth knowing.
- `changes_32.txt` in this tree stops at 3.2.0. The 3.2.1–3.2.10 maintenance fixes are not listed
  anywhere in the tree.
- 3.3.0's list is relative to 3.2.8 (`changes.txt:383`). 3.3.2's list is relative to 3.2.10, and the
  full set of changes since 3.3.1 *also includes* the 3.2.9 and 3.2.10 fixes, which are **not**
  listed in this file (`changes.txt:258-259`).

**"Asserts" mean silence in Orca.** wx is built with `-DwxBUILD_DEBUG_LEVEL=0`
(`deps/wxWidgets/wxWidgets.cmake` → `build/cmake/init.cmake:245-246`), and `libslic3r_gui` gets
`-DwxDEBUG_LEVEL=0` when `SLIC3R_STATIC` (Orca's `src/slic3r/CMakeLists.txt`). At level 0 `wxASSERT`,
`wxASSERT_MSG`, `wxFAIL` and `wxFAIL_MSG` expand to nothing (`include/wx/debug.h:314-324`), while
`wxCHECK*` still test the condition and return early, but silently (`include/wx/debug.h:342-368`).
Every "now asserts" item below is, in Orca, either a silent early return (`wxCHECK`) or carrying on
with bad state (`wxASSERT`). Nothing ever shows a dialog.

**Build configuration that decides which changes bite** (generated `wx/setup.h` in the wx build
directory, `dep_wxWidgets-build/lib/wx/include/<toolkit>-unicode-static-3.3/wx/setup.h`, plus
`deps/wxWidgets/wxWidgets.cmake`):

| Setting | Orca value | Consequence |
|---|---|---|
| Linux toolkit | GTK3 (`option(DEP_WX_GTK3 … ON)` in Orca's `deps/CMakeLists.txt`, `SLIC3R_GTK` "3"; Flatpak gtk3) | GTK3 items apply; GTK2 is only the `-DDEP_WX_GTK3=OFF` opt-out |
| `WXWIN_COMPATIBILITY_3_0` / `_3_2` | `0` / `1` | 3.0-deprecated API is gone, 3.2-deprecated still compiles |
| `wxUSE_STD_CONTAINERS` | `1` | wx containers are std-like; `wxList::Node` and `wxArrayString(n, s)` are gone (§4) |
| `wxUSE_STD_STRING_CONV_IN_WXSTRING` / `wxUSE_UNSAFE_WXSTRING_CONV` | `0` / `1` | no implicit `wxString` → `std::string`; use `into_u8` / `ToStdString` |
| `wxUSE_NANOSVG` / `wxUSE_LUNASVG` | `0` / `0` | `wxHAS_SVG` undefined: no `wxBitmapBundle::FromSVG*`; Orca rasterises SVG itself |
| `wxUSE_STC` | `OFF` | Scintilla items do not apply |
| `wxUSE_LIBWEBP` | `builtin` (Flatpak: `sys`) | WebP decodes through `wxImage` (§5) |
| `wxUSE_WEBVIEW_EDGE` | MSVC only, IE off | Edge items apply on Windows only; Chromium backend not built |
| `wxUSE_GLCANVAS_EGL` | `ON` | effective only on GTK3 with EGL found |

## 2 Changes from 3.1.6 to 3.2.0

### 2.1 Incompatible changes new since 3.1.5

These entries of `changes_32.txt` "INCOMPATIBLE CHANGES SINCE 3.0.x" were not in 3.1.5's list, plus
the 3.1.7-specific section.

| Change | Cite (`changes_32.txt`) | Orca relevance |
|---|---|---|
| `wxRegEx` uses PCRE; `wxRE_ADVANCED` syntax is now PCRE syntax, `wxRE_BASIC` is deprecated, POSIX classes `[:XXXX:]` fail to compile | :15-17; `interface/wx/regex.h:95, 148-201` | Orca uses `wxRegEx` for simple patterns (e.g. `TroubleshootDialog`); write new patterns in PCRE syntax |
| `wxSpinCtrlDouble::SetValue(wxString)` with invalid text resets to `GetMin()` | :126-127 | raw spin controls only; Orca's `SpinInput` is custom |
| `wxSpinCtrl::SetValue(wxString)` sends no events on MSW (as documented) | :129-130 | do not rely on setter events (`controls-dataview.md`) |
| `wxButton::GetBitmap{Current,Disabled,Focus,Pressed}()` return a valid bitmap on MSW only if set | :132-134 | check `IsOk()` |
| `wxFileName::GetVolume()` returns `\\share` for UNC paths and `\\?\Volume{GUID}` for GUID paths | :136-140 | path code comparing volumes |
| `wxBitmapComboBoxBase::SetItemBitmap()` takes `wxBitmapBundle` | :152-153 | the bundle change also retyped `OnAddBitmap` (5f365b5c6b, §8) |
| MSW also links `oleacc` (3.1.5 already needed `shlwapi`, `uxtheme`, `version`) | :158-163 | automatic with MSVC |
| Xcode projects drop i386, add arm64 | :205-207 | none (CMake build) |
| `wxImage` ctor from XPM data is `explicit` | :225-226 | write `wxImage(xpm)` |
| `wxWindow::DoGetBorderSize()` removed | :228-229 | use `GetWindowBorderSize()` |
| MSVC 7 unsupported | :231-232 | none |
| 3.1.7: `wxImageFileProperty` members and internal `wxPropertyGridPageState` functions removed | :282-290 | no propgrid in Orca's own code (only the `wxInspector` dev dependency links it, on MSW/macOS) |

### 2.2 New APIs and behaviour in 3.1.6–3.2.0

| Change | Cite | Orca relevance |
|---|---|---|
| `wxBitmapBundle` added and used "throughout the entire API" (3.1.6) | `changes_32.txt:366` | setters take bundles (`wxBitmap` converts implicitly); **overrides** of virtuals that took `wxBitmap` break (5f365b5c6b). Bundles and sizing: `dpi-bitmaps-fonts.md` |
| Bitmap logical/DIP API: `CreateWithDIPSize` (3.1.6), `CreateWithLogicalSize` (3.3.0), `GetLogicalSize`; `CreateScaled` and `GetScaledSize/Width/Height` are "older synonyms … use the new function in the new code" | `interface/wx/bitmap.h:514, 551, 563-579, 706, 770-787` | new code uses the new names; Orca's older call sites (`BitmapCache`, `ScalableBitmap`) still compile |
| `wxDC::GetContentScaleFactor()` returns the effective DPI factor for window DCs (e.g. 1.5 on MSW), unlike `wxWindow::GetContentScaleFactor()` (always 1 on MSW), "since wxWidgets 3.1.6" | `interface/wx/dc.h:153-168` | do not divide bitmap sizes by the DC scale (`painting-custom-widgets.md`) |
| `wxDPIChangedEvent::Scale()` (3.1.6) | `changes_32.txt:372` | convenience for rescaling stored sizes |
| MSW: TLW resizing on DPI change improved and overridable — a handler that sizes the TLW itself does not `Skip()` (3.2.0) | `changes_32.txt:269`; `interface/wx/event.h` (`wxDPIChangedEvent`) | Orca's `DPIAware` handler does not `Skip()` (`dpi-bitmaps-fonts.md`) |
| `wxUILocale` (3.1.6); `wxLocale::IsAvailable` is now implemented through it | `changes_32.txt:348`; `src/common/intl.cpp:740-781` | see `wxUILocale::IsSupported` in §3 |
| `wxWebView::RunScriptAsync()` (3.1.6) | `changes_32.txt:382` | `webview-gl-aui-media.md` |
| wxOSX: "Allow user input in `wxPopupTransientWindow`" (3.1.7) — the popup's child holds mouse capture while the cursor is outside it and releases it inside, toggled from `OnIdle` [source] | `changes_32.txt:334`; `src/common/popupcmn.cpp:438-471` | how wxOSX transient popups track the mouse; the macOS gap-dismissal worked around in `SidePopup::Popup` is not traced to a specific wx change (§8; `popups-menus.md`) |
| wxGTK: Wayland fixes (3.1.6), no GDK errors from `PopupMenu()` on Wayland (3.1.7), `wxCURSOR_SIZING` on Wayland (3.2.0) | `changes_32.txt:397, 317, 261` | `platforms.md` |
| MSW: all native modal dialogs are app-modal (3.1.6) | `changes_32.txt:409` | `windows-dialogs.md` |
| Also new: `wxKeyEvent::IsAutoRepeat()`, `wxSpinCtrl::GetTextValue()/SetIncrement()`, `wxTopLevelWindow::SetContentProtection()`, `wxEVT_SPLITTER_SASH_POS_RESIZE`, OSX full-screen view options, OSX `wxEVT_CHAR` from `wxDataViewCtrl` | `changes_32.txt:364-436` | available API |

### 2.3 Older 3.x changes that still trip prior knowledge

These were already in force in 3.1.5, so the upgrade did not change them, but code written from
3.0-era knowledge gets them wrong (`changes_32.txt` line):

| Contract | Line |
|---|---|
| MSW `wxYield()` generates `wxEVT_IDLE` (idle handlers can run inside a yield) | :25-27 |
| A 0-width or 0-height `wxBitmap` fails on every port | :29-30 |
| Invalid sizer flags assert (silent in Orca; `sizers-layout.md`) | :32-37 |
| `Validate()`/`TransferData*Window()` recurse into children by default (`wxWS_EX_VALIDATE_RECURSIVELY`) | :39-41 |
| MSW: call `Skip()` in `wxEVT_KEY_DOWN`/`wxEVT_CHAR` handlers, or system keys such as Alt+Space and Alt+F4 stop working | :54-57 |
| MSW dotted/dashed pens are high quality and slow; `wxPenInfo::LowQuality()` | :59-62 |
| `wxEVT_AUINOTEBOOK_PAGE_CHANGED` comes after the change | :68-69 |
| Generic `wxDataViewCtrl` stretches its last column | :76-77 |
| GTK `wxNotebook::AddPage()` sends no event for the first page; GTK `wxTextCtrl` sends no `wxEVT_TEXT` at creation | :79-83 |
| `wxDC::GetTextExtent("")` height is 0 on every port | :85-86 |
| `wxTE_PROCESS_ENTER` is required for `wxEVT_TEXT_ENTER`, even multi-line | :96-99 |
| `wxGLCanvas` uses physical pixels on GTK3/macOS: multiply `GetSize()` by `GetContentScaleFactor()` (Orca: `RetinaHelper::get_scale_factor`, GTK3 variant in `GLCanvas3D.cpp`) | :101-104 |
| `wxSizer::RecalcSizes()` is not to be called; call `Layout()` | :116-117 |
| `wxFileDialog::GetPath()/GetFilename()` assert and return empty with `wxFD_MULTIPLE` | :119-120 |
| `wxChoice::GetString()` asserts on an invalid index | :124 |
| Application code cannot construct `wxPaintEvent` | :165-167 |

## 3 3.3 behaviour changes that compile

`changes.txt` "Changes in behaviour not resulting in compilation errors" (:11-146). Each row: the
change, its line, and what it means for Orca.

| Change | Line | Orca relevance |
|---|---|---|
| wxMSW needs Windows 7+ | :14-15 | drop XP/Vista assumptions (they need wx 3.2) |
| Fatal-error exit code is 255 everywhere (was 127 with MSVC); `wxApp::SetErrorExitCode()` (virtual, `interface/wx/app.h:886`), static `SetFatalErrorExitCode()` (:922) | :17-19 | update anything (CI, crash reporting, wrapper scripts) that matches the old code |
| `wxGLCanvas` no longer multisamples by default | :21-23 | pitfall below |
| `wxFileConfig` on Unix defaults to XDG `~/.config/appname.conf` (old file still used; `wxCONFIG_USE_XDG`/`wxCONFIG_USE_HOME`, `MigrateLocalFile()`) | :25-29 | none: Orca uses `AppConfig`, no `wxConfig` (`strings-i18n-files.md`) |
| `wxColourDatabase` uses CSS values; `UseScheme()` restores the old ones (`interface/wx/gdicmn.h:845, 973-996`) | :31-33 | none: stock colours (`*wxGREEN`, …) are fixed RGB (`src/common/gdicmn.cpp:789-830`) and Orca builds colours from hex/RGB. Do not build colours from names |
| `wxAuiNotebook` default art is the new flat art; `wxAuiNativeTabArt` (or `"native"` in XRC) keeps the old look | :35-37 | none: no `wxAuiNotebook` in Orca |
| `wxTHREAD_WAIT_DEFAULT` is `wxTHREAD_WAIT_BLOCK`: `wxThread::Delete()/Wait()` no longer pump events | :39-41 | Orca uses std/boost threads (`wxThread` only for `IsMain()`); a new `wxThread` whose exit needs the main loop would deadlock |
| `wxDocument::OnCloseDocument()` runs once, after the views are destroyed (it used to run twice when the document was closed from the menu); an override must not rely on any view existing | :43-46 | none: no doc/view |
| `wxGrid::FreezeTo()` asserts on out-of-range counts, and freezes even when the grid is too small | :48-51 | silent `false` in Orca; it also refuses reordered or drag-movable rows/columns [source] (`src/generic/grid.cpp:5742-5750`). Clamp arguments (`controls-dataview.md`) |
| Invalid `wxImageList` calls assert | :53-56 | silent in Orca; `wxCHECK` returns (generic `Add` → -1). Create the list with a valid size before use |
| `wxTRANSPARENT_WINDOW` does nothing (`#define wxTRANSPARENT_WINDOW 0`, `include/wx/defs.h:1449`); MSW code that needs it can set `WS_EX_TRANSPARENT` | :58-60 | pitfall below; 026b105dcb |
| MSW `wxTextDataObject::SetData()` size includes the 2-byte NUL (consistent with `GetDataSize()`); an old-style size chops the last character | :62-66 | none: Orca uses the `wxTextDataObject(text)` ctor; use `SetText()`, never `SetData()` |
| `wxListCtrl::EditLabel()` asserts without `wxLC_EDIT_LABELS` | :68-69 | silent no-op in Orca; add the style where editing is intended |
| MSW `wxSystemAppearance::IsDark()` reports the app's own mode; `AreAppsDark()`/`IsSystemDark()` report the OS | :71-73 | pitfall below |
| Unix `wxUILocale::IsSupported()` no longer falls back to another region of the same language; pass just `"fr"` to accept any `fr_XX` | :75-79 | `wxLocale::IsAvailable` builds the region-qualified tag (`GetCanonicalWithRegion()`) and calls `IsSupported()` [source] (`src/common/intl.cpp:740-781`); `GUI_App::load_language` relies on it, so on Linux a language whose canonical locale (e.g. `fr_FR`) is not installed reports unavailable. Keep the fallbacks in `load_language` |
| Deprecated `wxPGCellRenderer::DrawCaptionSelectionRect()` overload not called; override the overload taking `wxWindow*`, or enable 3.0 compatibility | :81-83 | none: no propgrid in Orca's own code |
| `wxImageList` size is in physical pixels | :85-88 | pitfall below |
| Mac `wxWebRequest` no longer uses persistent storage. The entry names `wxWebRequest::EnablePersistentStorage()`; the real API is `wxWebSession::EnablePersistentStorage(bool)` (`interface/wx/webrequest.h:1612-1629`; also `wxWebSessionSync`, :1835), macOS-only, before the first request | :90-92 | none: Orca uses `wxWebRequest` only for image downloads (`wxWebSession::GetDefault().CreateRequest` in `StatusPanel`, `SliceInfoPanel`, `ReleaseNote`, `DeviceErrorDialog`); network and login agents use libcurl (`Slic3r::Http`) |
| MSW `wxBitmap::Create(size, dc)` no longer multiplies by the DC's content scale; the size is physical | :94-96 | remove compensating scaling; use `CreateWithLogicalSize` for logical sizes |
| `wxIMAGE_QUALITY_NEAREST` has a new value and is no longer `wxIMAGE_QUALITY_NORMAL`; `NORMAL` (the `Scale`/`Rescale` default) is bilinear + box average (`interface/wx/image.h:31-67`) | :98-99 | never store or compare the enum numerically; default-quality thumbnails and icons look smoother than under 3.1.5; pass `wxIMAGE_QUALITY_NEAREST` for pixel-exact scaling |
| `wxTextCtrl::{Save,Load}File()` treat `.rtf` as RTF | :101-104 | pass `wxTEXT_TYPE_PLAIN` to keep plain text |
| `wxClientDC`/`wxPaintDC` offset their origin by a `wxFrame` toolbar on every port | :106-109 | none: Orca frames have no native toolbar (`BBLTopbar` is a child `wxAuiToolBar`) |
| `wxTextCtrl::SetLabel()` does nothing and asserts on every port (MSW used to act as `SetValue`) | :111-113 | pitfall below |
| `wxAuiGenericTabArt` subclasses (also via `wxAuiMSWTabArt`) override `DrawPageTab()`/`GetPageTabSize()` instead of `DrawTab()`/`GetTabSize()`; direct `wxAuiTabArt` subclasses still work | :115-120 | none: no Orca tab art |
| `wxAuiNotebook` page index is logical (reorder-independent); `GetPagePosition()` gives the screen position | :122-126 | none; index math under `wxAUI_NB_TAB_MOVE` is what breaks |
| `wxListbook`/`wxChoicebook` interpret mnemonics in page titles "just as the other wx*book classes already did" | :128-130 | Orca uses neither (`BedShapeDialog` uses `wxSimplebook` + a combo); every book interprets `&` (`interface/wx/bookctrl.h:141-147`) → rule 11 |
| `wxAUI_MGR_HINT_FADE` is not in the default `wxAuiManager` style | :132-133 | Orca's `AuiMgr` keeps the default flags (minus `wxAUI_MGR_ALLOW_FLOATING` on Wayland, `AuiMgr::init`), so the docking hint no longer fades; add the flag if wanted |
| `wxPrintDialogData::SetAllPages(false)`/`SetSelection(false)` changed meaning | :135-137 | none |
| `wxGetTranslation()` returns `wxString` by value | :139-142 | pitfall below |
| `wxWindow::Raise()` no longer shows a hidden window on any port | :144-146 | pitfall below; ba867cc534 |

**Pitfalls**

- **Rule:** Request multisampling explicitly; never rely on `wxGLAttributes::Defaults()` or a null
  attribute list for MSAA.
  **Why:** 3.1.5's `Defaults()` was RGBA, double buffer, depth 16 plus `SampleBuffers(1).Samplers(4)`
  on every port; 3.3's is `RGBA().Depth(16).DoubleBuffer()` (`include/wx/glcanvas.h:175-178`), also used for a null
  attribute list (`src/common/glcmn.cpp:159-165`). The change-log line has the arguments swapped:
  `SampleBuffers(n)` is "number of sample buffers, usually 1" and `Samplers(n)` the samples per pixel
  (`interface/wx/glcanvas.h:233-246`). The plater canvas is unaffected:
  `OpenGLManager::create_wxglcanvas` passes `WX_GL_SAMPLE_BUFFERS`/`WX_GL_SAMPLES` explicitly (from
  the anti-aliasing setting). A canvas built with `.Defaults()` — the `SkipPartCanvas` that
  `PartSkipDialog` creates — renders without MSAA.
  ```cpp
  // Wrong: attrs.PlatformDefaults().Defaults().Stencil(8).EndList();                 // no MSAA in 3.3
  // Right: attrs.PlatformDefaults().Defaults().SampleBuffers(1).Samplers(4).Stencil(8).EndList();
  ```
  Cite: `docs/changes.txt:21-23`; `OpenGLManager::create_wxglcanvas`.

- **Rule:** Use `GUI_App::dark_mode()` for "is Orca dark", and `AreAppsDark()`/`IsSystemDark()` for
  "is the OS dark"; never `GetAppearance().IsDark()` in GUI code (it breaks on MSW, see below;
  `colours-dark-mode.md` rule 1 forbids it on every port).
  **Why:** `IsDark()` returns true whenever wx's own dark mode is active, else it falls back to
  `IsUsingDarkBackground()` (`src/msw/settings.cpp:431-441`). Orca calls
  `MSWEnableDarkMode(DarkMode_Auto)` (`DarkMode_Auto = 0`, `include/wx/msw/app.h:48`), which puts wx
  in `AppMode_AllowDark` (`src/msw/darkmode.cpp:235-252`); from then on `IsDark()` follows the OS
  apps setting (`ShouldUseDarkMode()`, :205-227), whatever theme the user picked in Orca.
  `AreAppsDark()` reads `AppsUseLightTheme`, `IsSystemDark()` reads `SystemUsesLightTheme`
  (`src/msw/settings.cpp:444-452`; `interface/wx/settings.h:307-358`). On other ports the three agree.
  `GUI_App::dark_mode()` honours the `dark_color_mode` app-config value first and only then calls
  `check_dark_mode()`, which still uses `IsDark()`.
  ```cpp
  // Wrong: bool dark = wxSystemSettings::GetAppearance().IsDark();   // OS apps setting on MSW
  // Right: bool dark = wxGetApp().dark_mode();
  ```
  Cite: 8248b06337; `GUI_App::dark_mode`, `check_dark_mode` (`GUI_Utils.cpp`); `colours-dark-mode.md`.

- **Rule:** `Show()` before `Raise()`, and `Show()` only if the window is hidden.
  **Why:** "If the window is currently hidden, this function does *not* show it automatically"
  (`interface/wx/window.h:3015-3033`). MSW already behaved so; GTK and macOS used to show it, so a
  bring-to-front path that only raises leaves a hidden frame hidden on those ports. On GTK3 a
  redundant `Show(true)` still runs `GTKSendSizeEventIfNeeded()` [source]
  (`src/gtk/toplevel.cpp:1259-1269`), which froze Orca once (dd8cb89f6d), so guard it.
  ```cpp
  // Wrong: wxGetApp().mainframe->Raise();
  // Right: auto* mf = wxGetApp().mainframe; if (!mf->IsShown()) mf->Show(); mf->Raise();
  ```
  Cite: ba867cc534 (the other-instance handlers in `Plater::priv::priv`,
  `Plater::priv::bring_instance_forward`); `windows-dialogs.md` §4.

- **Rule:** Do not use `wxTRANSPARENT_WINDOW`; give the panel the background it must blend with, or
  set `wxBG_STYLE_TRANSPARENT` **before** `Create()`.
  **Why:** the flag is `0`. Under 3.1.5 it set `WS_EX_TRANSPARENT` on MSW; now the panel paints its
  own background. A `SetBackgroundStyle(wxBG_STYLE_TRANSPARENT)` call after the window exists is a
  `wxCHECK_MSG` that returns `false` silently (`src/common/wincmn.cpp:1616-1625`), so it is no
  replacement for the flag — 026b105dcb assumed it was.
  ```cpp
  // Wrong: new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTRANSPARENT_WINDOW);
  // Right: auto p = new wxPanel(this, wxID_ANY);
  //        p->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#3B4446")));
  ```
  Cite: 026b105dcb, 8248b06337 (`MainFrame::create_side_tools`, `MainFrame::update_side_button_style`
  re-apply the colour on theme change); `painting-custom-widgets.md`.

- **Rule:** Never `SetLabel`/`SetLabelText`/`GetLabel` on a `wxTextCtrl` to show or read its value.
  **Why:** `wxTextCtrlBase::SetLabel` is only `wxFAIL_MSG("Use SetValue() or ChangeValue()
  instead.")` (`src/common/textcmn.cpp:933-936`); `SetLabelText` calls the virtual `SetLabel`
  (`include/wx/control.h:64-67`), and `GetLabel()` returns `m_labelOrig`, not the text
  (`include/wx/control.h:61`). Orca compiles the assert out, so the call is a complete no-op: the
  field never updates. Under 3.1.5 MSW it acted as `SetValue`, so code written then looked fine on
  Windows.
  ```cpp
  // Wrong: m_input_ip->GetTextCtrl()->SetLabelText(m_obj->get_dev_ip());
  // Right: m_input_ip->GetTextCtrl()->ChangeValue(m_obj->get_dev_ip());   // no wxEVT_TEXT
  ```
  Cite: `docs/changes.txt:111-113`; `controls-dataview.md`.

- **Rule:** Size a `wxImageList` from the bitmaps' physical size, or use
  `SetImages(std::vector<wxBitmapBundle>)`.
  **Why:** "the size is specified in physical pixels and must correspond to the size of bitmaps, in
  pixels" (`interface/wx/imaglist.h:62-63`). An Orca icon from `create_scaled_bitmap(name, win, 16)`
  is larger than 16 px on HiDPI MSW, where `Add()` hands it to the native `ImageList_Add`
  (`src/msw/imaglist.cpp:299-313`), which splits a wider bitmap into list-width images. The generic
  list (GTK, macOS) keeps a bitmap whose scale factor is not 1 intact, but returns -1 for a narrower
  1×-scale bitmap and chops a wider one into several list-width images [source]
  (`src/generic/imaglist.cpp:126-160`).
  ```cpp
  // Wrong: m_images = new wxImageList(16, 16); m_images->Add(create_scaled_bitmap("icon", this, 16));
  // Right: wxBitmap bmp = create_scaled_bitmap("icon", this, 16);
  //        m_images = new wxImageList(bmp.GetWidth(), bmp.GetHeight(), false);
  ```
  Cite: `docs/changes.txt:85-88`; `Tab` builds its list from `bmp().GetWidth()/GetHeight()`;
  `dpi-bitmaps-fonts.md`.

- **Rule:** A function that returns a translation returns `wxString` by value.
  **Why:** `wxGetTranslation()` returns by value now (`include/wx/translation.h:278-321`); returning
  it as `const wxString&` dangles. Orca's `I18N::translate` overloads and `_L` already return by
  value.
  ```cpp
  // Wrong: const wxString& title() { return _L("Printer"); }
  // Right: wxString title() { return _L("Printer"); }
  ```
  Cite: `docs/changes.txt:139-142`; `I18N.hpp`.

- **Rule:** Escape `&` in user data (preset, filament, printer, file names) shown as a control label or
  book page title.
  **Why:** labels and page titles interpret `&` as a mnemonic; an unescaped `&` disappears or
  underlines the next character. 3.3 extended this to `wxListbook`/`wxChoicebook`.
  ```cpp
  // Wrong: book->AddPage(page, preset_name);
  // Right: book->AddPage(page, wxControl::EscapeMnemonics(preset_name));   // or label->SetLabelText(name)
  ```
  Cite: `docs/changes.txt:128-130`; `interface/wx/bookctrl.h:141-147`; `controls-dataview.md`.

## 4 3.3 changes that break the build

`changes.txt` "Changes in behaviour which may result in build errors" (:149-252).

| Change | Line | Orca relevance |
|---|---|---|
| 3.0-deprecated symbols disabled by default (`WXWIN_COMPATIBILITY_3_0=1` at wx build time re-enables them), 2.8-deprecated removed | :152-154 | Orca builds `WXWIN_COMPATIBILITY_3_0 0`, `_3_2 1`: port off 3.0-deprecated API |
| `wxUSE_UNICODE=0` unsupported | :156 | none |
| `wxUSE_STD_CONTAINERS=1` by default ("Container Classes" overview); building wx with 0 keeps the old containers | :158-161 | Orca builds with 1 → pitfall below |
| `wxUSE_STL` gone; implicit `wxString` → `std::[w]string` only with `wxUSE_STD_STRING_CONV_IN_WXSTRING=1` at wx build time | :163-166 | Orca builds 0: convert explicitly (`into_u8`, `ToStdString`, `ToUTF8()`) |
| MSW links `gdiplus.lib`, `msimg32.lib` | :168-172 | automatic with MSVC/wx-config; only static non-MSVC builds add them |
| wxMotif, wxGTK1 removed | :174-175 | none |
| Private containers (e.g. `wxSimpleDataObjectList`) removed; object arrays (`wxImageArray`) compare values in `Index()` | :177-183 | use `std::vector`/`std::list` |
| Operators on wx types are hidden (not global) | :185-189 | pitfall below |
| `wxString` from `std::string_view` makes `wxstr = {"Hello", 2}` ambiguous | :191-194 | write `wxString{"Hello", 2}` |
| Generic `wxSearchCtrl` lost multi-line-only methods | :196-197 | none |
| Wide-filename `wxOnAssert()` overload removed | :199-200 | none |
| 64-bit DLLs carry an `x64` suffix in all build systems | :202-204 | packaging scripts matching DLL names; Orca links wx statically (Flatpak builds it shared in its own manifest) |
| CMake config installs to `lib/cmake/wxWidgets-3.3`; plain `find_package(wxWidgets)` is unaffected, hard-coded paths break | :206-211 | Orca: `find_package(wxWidgets 3.3 CONFIG …)` on Windows/macOS, `wx-config --toolkit=gtk${SLIC3R_GTK}` on Linux (Orca's `src/CMakeLists.txt`) |
| Memory-tracing options removed | :213-216 | use ASan |
| `wxTEST_DIALOG()` needs a trailing `;` | :218-219 | none |
| `wxWindow::GetDefaultBorderForControl()` not virtual | :221-223 | do not override; use `wxBORDER_THEME` |
| GTK `wxDirButton::Create()` lost `wildcard` | :225-226 | none |
| Several virtuals take `wxReadOnlyDC` | :228-231 | pitfall below |
| `wxSizer::Detach()` takes `wxWindowBase*` | :233-236 | only custom sizer subclasses |
| `wx/cursor.h` no longer includes `wx/utils.h` | :238-240 | include `<wx/utils.h>` explicitly (8248b06337 added it to `GLCanvas3D.cpp`) |
| `wxStyledTextCtrl::AddSelection()` returns void | :242-244 | none: `wxUSE_STC=OFF` |
| `wxColour` from `bool` no longer compiles | :246-248 | use the RGB or string ctor explicitly |
| `wxGLCanvas::CreateSurface()` removed from EGL builds | :250-252 | none: wx chooses EGL or GLX itself (§7) |

**Pitfalls**

- **Rule:** Override wx measuring virtuals with `wxReadOnlyDC&` and mark them `override`.
  **Why:** these virtuals now take `wxReadOnlyDC&` (the non-drawing base of `wxDC`, since 3.3.0,
  `interface/wx/dc.h:115-127`): `wxRendererNative::GetCollapseButtonSize`
  (`interface/wx/renderer.h:477`), the AUI tab and toolbar art size getters (`GetTabSize`,
  `GetLabelSize`, `GetToolSize`, …; `interface/wx/aui/auibook.h:1186+`, `interface/wx/aui/auibar.h:648-664, 887-903`),
  the `wxGridCellRenderer::GetPreferred{Size,Height,Width}`/`GetMaxSize` virtuals that 3.3 added
  beside the old `wxDC&` `GetBestSize` family (`include/wx/generic/grid.h:203-251`), the new `wxScrolled::PrepareReadOnlyDC`
  (`interface/wx/scrolwin.h:519`), and richtext/ribbon art. Nothing near `DoGetBestSize`, which takes
  no DC. Without `override` an old `wxDC&` signature silently becomes an overload that is never
  called; with `override` it fails to compile, which is what you want. Orca's `Widgets/` override
  none of them; `BBLTopbarArt` overrides only `DrawBackground`/`DrawButton` (still `wxDC&`); the
  `ObjectTable` grid renderers override the compatibility `GetBestSize(…, wxDC&, …)`, which "are the
  ones actually called by wxGrid" (`include/wx/generic/grid.h:253-271`). Callers are unaffected; a
  helper taking `wxDC&` cannot accept a `wxInfoDC`.
  ```cpp
  // Wrong: wxSize GetToolSize(wxDC& dc, wxWindow* w, const wxAuiToolBarItem& it);       // never called
  // Right: wxSize GetToolSize(wxReadOnlyDC& dc, wxWindow* w, const wxAuiToolBarItem& it) override;
  ```
  Cite: `docs/changes.txt:228-231`; `painting-custom-widgets.md`.

- **Rule:** With `wxUSE_STD_CONTAINERS=1`, build a `wxArrayString` with `Add()` or an initializer
  list, and walk `WX_DECLARE_LIST` lists with `compatibility_iterator` or range-for.
  **Why:** the std-container `wxArrayString` has no `(count, value)` ctor
  (`include/wx/arrstr.h:64-84`), and `List::Node` no longer exists. Convert Orca's UTF-8
  `std::string` explicitly: the implicit `wxString(const std::string&)` uses the current locale
  (`include/wx/string.h:1323-1326`).
  ```cpp
  // Wrong: load_files(wxArrayString(1, wxString::FromUTF8(target_path.string())));
  //        AmsRadioSelectorList::Node* node = m_radio_group.GetFirst();
  // Right: wxArrayString arr; arr.Add(wxString::FromUTF8(target_path.string())); load_files(arr);
  //        for (AmsRadioSelector* rs : m_radio_group) { ... }   // or ::compatibility_iterator
  ```
  Cite: 1765d296a8 (`Plater::import_model_id`, `SendMultiMachinePage::request_params`); `strings-i18n-files.md`.

- **Rule:** In a concatenation, make a `wxString` the first operand when the others are `char`,
  `wchar_t`, `std::string` or `std::wstring`.
  **Why:** wx operators are hidden friends now, found only by argument-dependent lookup on a wx type;
  "preventing them from implicitly being used with types convertible to wx types"
  (`changes.txt:185-189`). `char + std::wstring + …` compiled under 3.1.5 through the global
  `operator+(char, const wxString&)`, converting the `std::wstring` implicitly; under 3.3 that
  operator is a hidden friend (`include/wx/string.h:2150`) and no operand is a `wxString`, so it
  does not.
  ```cpp
  // Wrong: return marker_by_type(opt.type, printer_technology) + opt.category_local + sep + opt.label_local;
  // Right: return wxString(marker_by_type(opt.type, printer_technology)) + opt.category_local + sep + opt.label_local;
  ```
  (`marker_by_type` returns `char`, the `*_local` labels are `std::wstring`.) Making only `sep` a
  `wxString` (1765d296a8) did not help, because the leading `char + std::wstring` is evaluated first.
  Cite: 1765d296a8, 2b3328c2b2 (`OptionsSearcher::search` `get_tooltip`, `Search.cpp`).

- **Rule:** Use `wxDynamicCast` only on a pointer whose static type derives from `wxObject`; for
  mixin interfaces use `dynamic_cast`.
  **Why:** 3.3's macro casts its argument straight to `const wxObject*` [source]
  (`include/wx/object.h:118-121`); 3.1.5 first `static_cast` it to the target class, so a
  `wxComboPopup*` → `wxCheckListBoxComboPopup` cast used to compile. `wxComboPopup` has no `wxObject`
  base (`include/wx/combo.h:748`), so the 3.3 cast does not compile. The same holds for
  `wxItemContainer`, `wxTextEntry` and other non-`wxObject` mixins.
  ```cpp
  // Wrong: auto* p = wxDynamicCast(combo->GetPopupControl(), wxCheckListBoxComboPopup);
  // Right: auto* p = dynamic_cast<wxCheckListBoxComboPopup*>(combo->GetPopupControl());
  ```
  Cite: 7ea69199fd (`combochecklist_get_flags`, `combochecklist_set_flags`, `GUI.cpp`).

## 5 3.3.0 notable changes

`changes.txt:380-592` (relative to 3.2.8).

**Major changes** (:385-394)

| Change | Orca relevance |
|---|---|
| Experimental MSW dark mode (#23028), with `wxApp::SetAppearance()` (#24461) | pitfall below; Orca kept NppDarkMode and only calls `MSWEnableDarkMode(DarkMode_Auto)` |
| Chromium `wxWebView` backend (#706) and `wxEVT_WEBVIEW_CREATED` | Chromium is not built; `wxEVT_WEBVIEW_CREATED` is the documented ready signal for Edge (`webview-gl-aui-media.md`) |
| WebP images (#25205) | Orca builds `wxUSE_LIBWEBP=builtin` and calls `wxInitAllImageHandlers()` (`GUI_App::on_init_inner`), so `wxImage` decodes WebP (e.g. downloaded thumbnails) |
| Pinned and multi-row AUI tabs (#25187, #25076) | none: no `wxAuiNotebook` |
| Synchronous `wxWebRequest` (#24760) | available (`wxWebSessionSync`); "must not be used from the main thread of GUI applications" (`interface/wx/webrequest.h:614-615`) |
| Raw touch events (#17077); wxGrid accessibility (#24368) | available |
| Unix power events and blockers (#22396, #23717) | could keep a Linux system awake during long work; Linux covers only `wxPOWER_RESOURCE_SYSTEM` and needs systemd ≥ 183 (`interface/wx/power.h:163-171`); not used by Orca |
| Native GTK file dialogs when possible (#24486, #25104) | the portal dialog is used only with GTK ≥ 3.20 at runtime, without `wxFD_PREVIEW` and without an extra control [source] (`src/gtk/filedlg.cpp:265-273, 438-443`); Orca's `CheckboxFileDialog` (`SetExtraControlCreator`, `GUI_Utils.hpp`) therefore gets the non-native GTK dialog |
| Native `wxTextCtrl` contents / RTF (#24626, #24912) | available (`GetRTFValue`, `SearchText`) |

**All** (:398-442)

| Change | Orca relevance |
|---|---|
| `wxWebRequest`: base URL (#24769), proxy (#24762), repeated headers (#24878); `wxWebSession::EnablePersistentStorage()` (#23743) | see §3 for storage |
| `wxString`: move operations (#23215), `std::string_view` ctor (#23711), faster and more robust `To/FromCDouble()` (#23287), `errno` preserved (#23113), `wc_string()` (#23463), `wxWARN_UNUSED` on the class (#24833, unused-variable warnings for `wxString` locals) | settings fields do **not** parse through `ToCDouble`: `Field` normalises the decimal separator and calls `wxString::ToDouble`, and `double_to_string` uses `wxNumberFormatter::ToString`; `ToCDouble`/`FromCDouble` appear only in a few helpers (e.g. `PreferencesDialog::create_camera_orbit_mult_input`). The `wxNumberFormatter` changes (3.3.1, 3.3.2) matter more for `Field` |
| Lambdas with `Bind()` without RTTI (#14850); move-only `wxMessageQueue` (#25026); `wxLogXXX(string)` safe with a single string (#25414) | available |
| Environment variables use UTF-8 (#25101) | check round-tripping of non-ASCII paths through `wxGetEnv`/`wxSetEnv` |
| Improved locale matching (#24855); thread-safe `wxPlatformInfo::Get()` (#25459); `wxXmlParseError` from `wxXmlDocument::Load()` (#24215); customisable error exit code (#24770) | available |

**All (GUI)** (:444-500)

| Change | Orca relevance |
|---|---|
| High-DPI wave: `wxCursorBundle` (#25374), animations (#23817), generic `wxListCtrl` (#22916), print preview (#24666), AUI dock art after DPI change (#23420), `wxBusyInfo` bitmaps (#23813) | AUI dock-art size metrics are DIP-like now: `wxAuiManager` reads them through `GetMetricForWindow()` (since 3.3.0), which scales them by the window DPI. The docs exempt `wxAUI_DOCKART_SASH_SIZE` and `wxAUI_DOCKART_PANE_BORDER_SIZE` (`interface/wx/aui/dockart.h:274-294`), but the default implementation exempts only `wxAUI_DOCKART_PANE_BORDER_SIZE` (and the non-pixel gradient type) and does scale the sash size [source] (`src/aui/dockart.cpp:199-226`). Pass DIP values to `SetMetric` (Plater's `SetMetric(wxAUI_DOCKART_CAPTION_SIZE, 18)`), never `FromDIP(…)` |
| Dark-mode colours in XRC (#23571); CSS colour names (#23518) | none: no XRC, no colour names |
| `wxTextCtrl::SearchText()` (#24756) and RTF (#24626); locale-aware date/time pickers (#23965, display format changes); `wxSearchCtrl` derives from `wxTextEntry` on all ports (#23686) | available |
| Scintilla 5.0 / Lexilla 5.3 (#23117, #24369); nanosvg crash fixes (#24213) | none: `wxUSE_STC=OFF`, `wxUSE_NANOSVG=OFF` |
| Better `wxImage` resizing (#25252) | default-quality scaling output differs from 3.1.5 (§3) |
| `wxAuiManager::{Save,Load}Layout()` (#24235); `wxAuiNotebook` layout save/restore (#24950) | Orca persists docking with `SavePerspective`/`LoadPerspective` (Plater, `AuiPaneLayout`) |
| Non-live resize restored in wxAUI and `wxSplitterWindow` (#24193) | `wxAUI_MGR_LIVE_RESIZE` is in `wxAUI_MGR_DEFAULT` since 3.3.0 (`interface/wx/aui/framemanager.h:59-66`). The style table's "always enabled in wxGTK3 and wxOSX ports as non-live resizing is not implemented in them" (:199-206) is as stale as the `AlwaysUsesLiveResize()` note: that function "always returns false" as of 3.3.0 (:345; `src/aui/framemanager.cpp:711-714`), and the flag decides on every port [source] (`HasLiveResize`, :716-719). See `webview-gl-aui-media.md` §AUI docking |
| `wxInfoBar::ShowCheckBox()` (#25394); printing multiple page ranges (#25030); `wxGrid::CopySelection()` (#24124); new default flat AUI tab art (#25316); `wxApp::SetAppearance()` (#24461) | available |

**All (WebView)** (:510-525) — `wxWebViewConfiguration` + `GetNativeConfiguration()`, `SetProxy()`,
`ShowDevTools()`, `EnablePersistentStorage()`, `EnableBrowserAcceleratorKeys()`, clearing browsing
data, advanced requests, child-window handling, `IsTargetMainFrame()`, Edge user agent settable after
creation, and **Edge events queued** (#22744, #19075): Edge handlers run later than on the other
backends. Edge posts its events with `AddPendingEvent` (script messages: `src/msw/webview_edge.cpp:811`)
except the vetoable ones, `wxEVT_WEBVIEW_NAVIGATING` (:599) and `wxEVT_WEBVIEW_NEWWINDOW` with its
`NEWWINDOW_FEATURES` follow-up (:723, :737), which stay synchronous; WebKit and
WebKit2GTK deliver script messages synchronously (`src/osx/webview_webkit.mm:1360`,
`src/gtk/webview_webkit2.cpp:408`) [source]. `webview-gl-aui-media.md` owns the details.

**wxGTK** (:527-547)

| Change | Orca relevance |
|---|---|
| `wxDPIChangedEvent` generated (#19290, #24040), GTK ≥ 3.10 (`interface/wx/event.h:3592-3593`) | pitfall below |
| `wxGLCanvas` scale fixed with EGL/Wayland in high DPI (#23733) | Orca has no compensating hack: GTK3 `RetinaHelper::get_scale_factor` returns `GetContentScaleFactor()` |
| `wxKeyEvent::GetKeyCode()` fixed for non-US layouts (#23379) | shortcuts match through `KeyChord::from_event` (`mouse-keyboard-focus.md`) |
| Missing enter/leave events fixed (#24339); mouse event generation fixes (#24931-#24933) | hover logic (`StateHandler`) gets enter/leave reliably |
| Total window size with GNOME on X11 (#25348) | TLW geometry |
| `libwebkit2gtk-4.1` support (#23633) | wx's CMake build prefers 4.1 and falls back to 4.0 (`build/cmake/init.cmake:571-577`) |
| `libsecret` not required at runtime (#25355) | `wxSecretStore` loads it on demand: always check `IsOk()` (`interface/wx/secretstore.h:197-201`), as `OrcaCloudServiceAgent` does |
| Multi-line `wxTextCtrl` max length (#24751); `wxRB_SINGLE` (#23652); `GTKSetPangoMarkup()` (#24912) | `controls-dataview.md` |
| `wxDC::DrawRoundedRectangle()` radius limited to half the smaller side (#24327) | the clamp is in the GTK2 GDK DC (`src/gtk/dcclient.cpp:874-875`, built only in the GTK2 opt-out, `GTK2_LOWLEVEL_SRC` in `build/files`) and also in the common `wxGraphicsPathData::AddRoundedRectangle` (`src/common/graphcmn.cpp:439-440`, absent in 3.1.5) that `wxGraphicsContext::DrawRoundedRectangle` uses, so every `wxGCDC` clamps: GTK3 (Cairo) and macOS window DCs, and Orca's memory-DC + `wxGCDC` paint paths on all ports [source]. A radius larger than half the smaller side is now clamped instead of drawing overlapping arcs |
| Read-only `wxBitmapComboBox` height fixed (#25468) | `Slic3r::GUI::BitmapComboBox` on GTK |

**wxMSW** (:549-583)

| Change | Orca relevance |
|---|---|
| "Enable double buffering for all windows" (#22851) | **reverted in 3.3.2** (#25808) → pitfall below |
| `wxOverlay` reimplemented with layered windows (#23261) | none: no `wxOverlay` |
| `wxBG_STYLE_TRANSPARENT` implemented (#23412) | as `WS_EX_TRANSPARENT` on non-TLW children [source] (`src/msw/window.cpp:1581-1582`); set before `Create()` (`painting-custom-widgets.md`) |
| `wxCAPTION` turned on when min/max/close boxes are set (#23575) | `WS_CAPTION` is added to the style (`src/msw/toplevel.cpp:132-135`); origin of the MainFrame workaround (§8) |
| `wxButton` default size larger in high DPI (#25297) | raw `wxButton` layouts may grow; Orca dialogs use `Button`/`DialogButtons` |
| Markup in `wxStaticText` (#25000); `wxHyperlinkCtrl` colour changeable (#23549) | `SetLabelMarkup` now works on MSW (single line) |
| Extended-length paths (#25033); `wxFileDialog` no unwanted extension (#24949); UTF-8 build fixes (#23313); non-BMP strings (#25128) | available |
| `wxDisplay` invalidated on display change (#25396); TLW with one child resized on DPI change (#22983); Aero-snapped geometry saved | multi-monitor geometry |
| Modern default `wxTreeCtrl` look (#23844); RTL fixes for `wxOverlay`/`wxScrolled` (#25413) and GDI+ (#25431); `wxBitmap::UseAlpha()` returns `bool` (#23919) | available |

**wxOSX** (:585-592) — `wxEventLoop::OnExit()` is always called (#25409); TLW cursor setting fixed
(#25131); Cmd-C no longer activates a "Close" button (#25346); `wxCursor` loadable from resources
(#24374); `wxUIActionSimulator` works (#23692), usable for GUI tests.

**Pitfalls**

- **Rule:** Do not replace Orca's MSW dark mode with wx's (`SetAppearance`/`MSWEnableDarkMode`)
  unless live theme switching is kept.
  **Why:** wx's MSW dark mode cannot change once any top-level window exists or a mode was chosen:
  `SetAppearance` returns `CannotChange` (`src/msw/darkmode.cpp:263-270`;
  `interface/wx/app.h:1166-1173`). TaskDialog-based dialogs (`wxMessageDialog`, `wxProgressDialog`,
  simple `wxAboutBox`), the common dialogs (colour, find/replace, font, page setup, print) and the
  date/time/calendar controls stay light (`interface/wx/app.h:1434-1448`). Orca switches themes at
  runtime, which is why 8248b06337 kept NppDarkMode and only informs wx with
  `MSWEnableDarkMode(DarkMode_Auto)` (§8).
  Cite: 8248b06337 (PR text); `colours-dark-mode.md`.

- **Rule:** On MSW, a custom-painted control buffers its own drawing (`wxAutoBufferedPaintDC`/
  `wxBufferedPaintDC` with `wxBG_STYLE_PAINT`, or the Orca memory-DC + `wxGCDC` path), or calls
  `SetDoubleBuffered(true)` after creation.
  **Why:** 3.3.0's global `WS_EX_COMPOSITED` was reverted in 3.3.2 (`changes.txt:308`). In 3.3.2
  nothing sets it except an explicit `SetDoubleBuffered(true)` [source] (`src/msw/window.cpp:4704-4721`;
  the only other use is `MSWDisableComposited`, :1643-1652), so MSW windows are not double-buffered by
  default, exactly as in 3.1.5. `wxAutoBufferedPaintDC` is a `wxBufferedPaintDC` on MSW at compile
  time (`include/wx/dcbuffer.h:18-23, 215-221`). Nothing tuned against 3.3.0/3.3.1 applies.
  Cite: `docs/changes.txt:308, 557`; `painting-custom-widgets.md`.

- **Rule:** A `wxEVT_DPI_CHANGED` handler must be correct on GTK3, and every handler you bind — on a
  child, a control or a `DPIDialog`/`DPIFrame` — calls `Skip()`.
  **Why:** wxGTK3 now emits the event from `wxTopLevelWindowGTK::GTKConfigureEvent` when the integer
  content scale changes, with DPI = 96 × scale [source] (`src/gtk/toplevel.cpp:336-351`;
  `src/common/wincmn.cpp:2828-2830`). The event reaches each top-level window and its children
  recursively, and the docs say handlers "should almost always call `event.Skip()`"
  (`interface/wx/event.h:3564-3582`). Orca's `DPIAware` binds it on every non-macOS port and does
  not `Skip()`; it sets `m_scale_factor` from that DPI although GTK3 pixels are already DIPs (the
  ctor starts at 1 because `get_dpi_for_window` returns 96 on Linux). Double scaling when a window moves between monitors of
  different scale is a risk that has not been verified at runtime; test DPI changes on GTK when
  touching `DPIAware::rescale` paths.
  Cite: `docs/changes.txt:541`; `dpi-bitmaps-fonts.md` §wxEVT_DPI_CHANGED.

## 6 3.3.1 notable fixes

`changes.txt:333-377`.

| Port | Change | Orca relevance |
|---|---|---|
| All | Persistence for `wxCheckBox` (#25515) and `wxRadioButton` groups (#25530); `wxAuiPaneInfo::FloatingClientSize()` (#25483); PNG "Description" chunk (#25556) | available |
| All | Settable app id (#25548): `wxAppConsole::SetClassName()` — the Windows AppUserModelID and the Wayland `app_id` (wxGTK ≥ 3.24.22), unused elsewhere; call it before any TLW, typically in the app ctor; on Windows it also changes shell behaviour (shift-middle-click new instance, shell MRU) (`interface/wx/app.h:760-812`) | Orca calls only `SetAppName`; setting a class name would change Windows taskbar grouping and jump lists |
| All | `wxDataViewCtrl::Collapse()` safe from event handlers (#25631); no `wxEVT_GRID_SELECT_CELL` at `wxGrid` creation (#25498); empty `wxGridSizer` no longer asserts (#25641); `wxPropertyGrid` compatibility (#25627); `wxNumberFormatter` (#25614, #25635); reproducible static Unix builds (#25502) | `ObjectList`/`ObjectGrid` event handlers; `Field` number formatting |
| wxGTK | Crash sorting a `wxDataViewCtrl` with a single leaf (#25625); `wxListCtrl` contents lost after `AppendColumn()` (#25519) | native GTK `wxDataViewCtrl` |
| wxMSW | Dark-mode fixes: disabled `wxButton` bitmaps (#25575), disabled `wxStaticText` (#25574), `wxComboCtrl` (#23766), `wxTE_RICH` `wxTextCtrl` (#25602), selected toolbar buttons (#25616), `wxStaticBitmap` in `wxNotebook` crash (#25499), notebook background in high-contrast (#25542) | apply only where wx's own dark mode is active (`wxMSWDarkMode::IsActive()`; with Orca's `DarkMode_Auto`, whenever `ShouldAppsUseDarkMode()` reports dark, `src/msw/darkmode.cpp:205-227, 414-417`) |
| wxMSW | `wxDataViewCtrl` border in light mode (#25532); `wxTreeCtrl::EnsureVisible()` while frozen (#18435); preferred-languages buffer overrun (#25612); `wxAcceleratorTable` with 0 entries (#25517); date/time pickers on non-English Windows (#25511); per-window menu MDI crash (#25522) | available |
| wxOSX | Border look of `wxDataViewCtrl`, `wxListBox`, `wxTextCtrl` (#25570); startup crash with Farsi system language (#25561) | native macOS controls |

## 7 3.3.2 notable changes

`changes.txt:255-318`, relative to 3.2.10 (see §1 for the unlisted 3.2.9/3.2.10 fixes).

| Port | Change | Orca relevance |
|---|---|---|
| All | `wxWebRequestDebugLogger` (#26086); configurable `wxWebRequest` timeouts (#25673); number/currency formatting (#25765); 3rd-party libraries updated (#26010); `wxSOCKET_NOWAIT_READ\|wxSOCKET_WAITALL_WRITE` (#17114) | image downloads; `Field` formatting |
| All | `wxDC::DrawLabel()` bitmap position fixed in high DPI (#25888) | available |
| GUI | `wxGLContext::ClearCurrent()` (#25958) and `wxGLContext::GetProcAddress()` (#9215) — static members of `wxGLContext`, not `wxGLCanvas` as the change log says; `GetProcAddress` "is currently not implemented under macOS and always returns NULL" (`interface/wx/glcanvas.h:550-599`) | Orca loads GL with GLAD (through `eglGetProcAddress` on Wayland, `OpenGLManager::init_gl`) |
| GUI | `wxGLCanvas::SetSwapInterval()` (#25449) returning `SwapInterval::{NotSet, Set, NonAdaptive}`, `GetSwapInterval()`, `DefaultSwapInterval` (`interface/wx/glcanvas.h:866-893, 1036-1154`) | pitfall below |
| GUI | Automatic `wxStaticText` wrapping (#25753) | pitfall below |
| GUI | `wxDisplay::GetRawPPI()` (#26082); configurable `wxScrolled<>` autoscroll (#25978, `EnableAutoScrollInside`/`DisableAutoScrollOutside`); `wxScrolled::GetViewStartPixels()`; `wxWindow::GetMinSizeFromKnownDirection()` | `sizers-layout.md` |
| GUI | `wxPersistentDVC` restores column positions (#26222); safer `wxTipWindow` close detection (#26070); wxDC-derived objects movable (#25726) | available |
| GUI | AUI: pane minimising (#23986), crash on hover after closing a notebook tab (#25959), notebook splitting (#26081) | Plater docking |
| GUI | LunaSVG option (#25902); `wxSVGFileDC` improvements (#25723); generic `wxCalendarCtrl` DPI-aware (#25713); `wxTextEntryDialog::SetHint()` (#26176); `wxStyledTextCtrlMiniMap` (#25887) | LunaSVG and STC are off in Orca's build |
| GUI | Many RTL layout fixes in wxMSW and wxGTK (#25426) | RTL languages |
| wxGTK | GLX and EGL in the same program (#26023): wx defaults to EGL even on X11 unless `PreferGLX()` or `wx_opengl_egl=0`; Wayland is always EGL [source] (`src/unix/glcanvas.cpp:218-245`; `interface/wx/glcanvas.h:1082-1102`) | pitfall below |
| wxGTK | `WarpPointer()` on Wayland compositors with the pointer-warp protocol (#23778); mutter moves the pointer only while a button is pressed (`interface/wx/window.h:3895-3914`) | Orca does not call `WarpPointer` |
| wxGTK | Gesture handling fixes (#26241); `wxDIRP_DIR_MUST_EXIST` | `GLCanvas3D::bind_event_handlers` binds `wxEVT_GESTURE_PAN/ZOOM/ROTATE` |
| wxMSW | `WS_EX_COMPOSITED` use from earlier 3.3 reverted (#25808) | §5 pitfall |
| wxMSW | Dark-mode rendering of several controls (#25835), toolbar (#25892), menus (#26182); accessibility: `wxCheckBox` in dark mode (#26184), full `wxCheckListBox` (#25948) and `wxStyledTextCtrl` (#25956), basic `wxRichTextCtrl` (#26202) | wx dark mode only |
| wxMSW | `wxNO_WIN32_W` (#25965); MSVS 2026 project files (#26131); `wxString` debug visualiser (#25684) | none |
| wxOSX | Visual fixes for macOS 26 Tahoe (#25766, #25743, #25767); dark-mode grid lines (#25783); nested markup attributes (#25864); suspend/resume events (#25778); threaded `wxGA_SMOOTH` animation (#25906); more joystick axes (#26216) | native look on Tahoe |
| wxOSX | Click events consistent with wxMSW (#25886) | pitfall below |

**Pitfalls**

- **Rule:** After `SetLabel` on a `wxStaticText`, re-wrap with `Wrap(-1); Wrap(w);`, or use Orca's
  `Label` (`LB_AUTO_WRAP`, `Label::Wrap`); use `wxST_WRAP` only inside a sizer that constrains the
  width.
  **Why:** "Automatic wrapping" is the opt-in `wxST_WRAP` style, which "only works when the control is
  used inside a sizer" (`interface/wx/stattext.h:46-49`); labels without it lay out as before, and
  Orca uses no `wxST_WRAP`. What does change existing code is the rewritten `Wrap()`: it returns
  immediately when `width == m_currentWrap` (`src/common/stattextcmn.cpp:259-263`), and `SetLabel`
  clears the saved unwrapped text but not `m_currentWrap` (`UpdateLabelOrig`, :354-365) [source].
  So `SetLabel(new); Wrap(sameWidth);` — correct under 3.1.5, whose `Wrap()` always re-wrapped —
  leaves the new label unwrapped. `wxST_WRAP` wraps at the width the sizer offers via
  `GetMinSizeFromKnownDirection` (:285-316), but the first `CalcMin` still uses the unwrapped best
  size, so a fitted dialog grows to the full line [source]; constrain the width another way (a fixed
  or max width on the container). `Label::Wrap` re-wraps from its stored text and has no cache.
  ```cpp
  // Wrong: m_static_valid->SetLabel(info_line); m_static_valid->Wrap(FromDIP(300));
  // Right: m_static_valid->SetLabel(info_line); m_static_valid->Wrap(-1); m_static_valid->Wrap(FromDIP(300));
  ```
  Cite: `docs/changes.txt:280`; `sizers-layout.md` §wxStaticText wrapping.

- **Rule:** On Linux/X11, call `wxGLCanvas::PreferGLX()` before any GL use, attribute objects
  included; set a swap interval explicitly if the canvas needs VSync.
  **Why:** `PreferGLX()` called late "will trigger an assert failure and have no other effect" (silent
  in Orca) and has no effect on Wayland (`interface/wx/glcanvas.h:1082-1102`). Orca calls it when
  `is_running_on_x11()` early in `GUI_App::on_init_inner`. On Unix wx sets the swap interval to **0**
  (VSync off) at the first `SwapBuffers` unless `SetSwapInterval` was called, so that
  `eglSwapBuffers`/`glXSwapBuffers` never block on an occluded window [source]
  (`include/wx/unix/private/glcanvas.h:96`; `src/unix/glegl.cpp:897-915`; `src/unix/glx11.cpp:940-950`).
  `SetSwapInterval(DefaultSwapInterval)` keeps the driver's default. macOS and MSW leave the default
  unless asked. `OpenGLManager` sets no swap interval, so on Linux the 3D view's buffer swaps are
  not synchronised to VSync.
  ```cpp
  // Right (if frame pacing is wanted): canvas->SetSwapInterval(1);   // before the first SwapBuffers
  ```
  Cite: `docs/changes.txt:275-276, 292`; `webview-gl-aui-media.md` §EGL vs GLX.

- **Rule:** Do not count clicks from `wxEVT_LEFT_DCLICK` alone on macOS; handle `DOWN` and `DCLICK`
  as on MSW.
  **Why:** `wxWidgetCocoaImpl::DoHandleMouseEvent` turns every second `DCLICK` back into a `DOWN`
  (left and right buttons), so a triple click gives `DOWN, DCLICK, DOWN` as on MSW; previously every
  click with `clickCount > 1` was a `DCLICK` [source] (`src/osx/cocoa/window.mm:4103-4150`). Per-platform click-count ifdefs
  written for 3.1.5 need rechecking.
  Cite: `docs/changes.txt:316`; `mouse-keyboard-focus.md`.

## 8 Migration already done in Orca

The upgrade landed as 8248b06337 ("Updated wxWidgets to 3.3.2", #12941; build system 2d7e26292b).
Its PR kept Orca's own MSW dark mode to avoid broader changes and because wx's needs an app restart to
switch. Each row is a rule for new code; the owning file has the detail.

| Commit | Rule for new code | Where | Owner |
|---|---|---|---|
| 8248b06337 | No version-conditional code for wx < 3.3: the upgrade deleted the pre-3.1.3 DPI-event shim (`DpiChangedEvent`, `EVT_DPI_CHANGED_SLICER`), the luma fallback in `check_dark_mode`, the `wxCHECK_VERSION` guards in `I18N.hpp`/`GUI_App.hpp`, and the macOS 10.9.5 `wxGLContext` hack | `GUI_Utils.hpp` `DPIAware`, `OpenGLManager` | this file |
| 8248b06337 | `MSWEnableDarkMode(DarkMode_Auto)` runs before `NppDarkMode::InitDarkMode()`, so NppDarkMode's `SetPreferredAppMode(ForceDark` or `ForceLight`, per Orca's setting) overrides wx's `AllowDark` at OS level in both directions; `GUI_App::dark_mode()` honours `dark_color_mode` before `check_dark_mode()` | `GUI_App::on_init_inner`, `GUI_App::dark_mode` | `colours-dark-mode.md` |
| 8248b06337 | `wxToolTip::GetToolTipCtrl()` is private in 3.3 (`include/wx/msw/tooltip.h:92`); wx applies dark mode to the tooltip window itself through `wxMSWDarkMode::AllowForWindow`, which follows wx's mode [source] (`src/msw/tooltip.cpp:321`; `src/msw/darkmode.cpp:454-457`) | `GUI_App::force_colors_update` (the `#if wxVERSION_NUMBER < 3300` block is dead under 3.3) | `colours-dark-mode.md` |
| 8248b06337 | Backend webview factories override `GetVersionInfo(wxVersionContext)` without a default argument (`include/wx/msw/webview_edge.h:150`); pass `wxVersionContext::RunTime` when calling through the concrete factory | `WebView::CheckWebViewRuntime` | `webview-gl-aui-media.md` |
| 8248b06337 | The GL canvas gets `wxBG_STYLE_PAINT`; on MSW `GLCanvas3D::on_paint` renders immediately because idle events are not dispatched inside the modal resize loop (c06a0223a7) | `OpenGLManager::create_wxglcanvas`, `GLCanvas3D::on_paint` | `webview-gl-aui-media.md` |
| 8248b06337 | On MSW, do not `SetFocus()` the GL canvas while `wxCurrentPopupWindow` is set: the focus change makes wx call `MSWDismissUnfocusedPopup` and closes the search dropdown. `wxCurrentPopupWindow` is a wx-internal global (`src/msw/popupwin.cpp`) that Orca declares `extern` itself, usable only because wx is linked statically | `GLCanvas3D::on_mouse` (`evt.Entering()` branch) | `popups-menus.md` |
| 6148ba16b3 (in 8248b06337), 988b500f33 | On GTK a `wxBitmapToggleButton`/`wxButton`-based widget sized to exactly its bitmap leaves no room for the theme's CSS padding (GTK "negative content width" criticals). Either strip the native button CSS with `Slic3r::GUI::RemoveButtonBorder` and size to the bitmap (`CheckBox`), or size to `GetBestSize()` grown to the bitmap (`IncTo`; `RadioBox` and `SwitchButton` do both) | `RemoveButtonBorder` (`GUI_Utils.cpp`), `CheckBox::Rescale`, `RadioBox::Rescale`, `SwitchButton::Rescale` | `platforms.md` §GTK native chrome |
| 8248b06337 | The macOS-only vertical text nudges in `Button::render` and `SideButton::dorender` were removed; do not re-add per-OS baseline offsets | `Button::render`, `SideButton::dorender` | `painting-custom-widgets.md` |
| 8248b06337 | `wxEXPAND` combined with `wxALIGN_*` in a box sizer was cleaned up; the combination is ignored | `Sidebar::priv::layout_printer`, `AMSControl::createAmsPanel` | `sizers-layout.md` |
| 8248b06337 | `GUI_App::on_init_inner` filters known-harmless GTK criticals (allocation on hidden widgets, events on unrealised widgets, style-context calls from `SetBackgroundColour` before realisation); check that filter before chasing such a message | `GUI_App::on_init_inner` | `platforms.md` |
| 5f365b5c6b | `wxBitmapComboBox` overrides take `wxBitmapBundle` (`OnAddBitmap(const wxBitmapBundle&)`, `include/wx/bmpcbox.h:89`); `m_bitmaps` became `m_bitmapbundles`; get a bitmap with `GetBitmap(GetDefaultSize())` | `BitmapComboBox::OnAddBitmap`, `BitmapComboBox::OnDrawItem` (both macOS-only overrides) | `dpi-bitmaps-fonts.md` |
| ed88cbe3f5 → d62aa42e61 | 3.3's `wxWebViewWebKit` has neither the default ctor nor the creating `(parent, id, url, …)` ctor of 3.1.5; its only ctor is `explicit wxWebViewWebKit(const wxWebViewConfiguration&, WX_NSObject request = nullptr)` (`include/wx/osx/webview_webkit.h:36`). ed88cbe3f5 switched macOS to `wxWebView::New()`, which bypassed the subclass destructor that calls `RemoveScriptMessageHandler("wx")`; d62aa42e61 restored `WebViewWebKit` via `wxWebView::NewConfiguration(wxWebViewBackendWebKit)`. Only Linux uses `wxWebView::New()` | `WebView::CreateWebView`, `WebViewWebKit` | `webview-gl-aui-media.md` |
| 1765d296a8, 2b3328c2b2 | `wxArrayString` via `Add()`; `wxString` first in mixed concatenation; `compatibility_iterator` for wx lists (§4) | `Plater::import_model_id`, `OptionsSearcher::search`, `SendMultiMachinePage` | this file |
| 7ea69199fd | `dynamic_cast`, not `wxDynamicCast`, on `wxComboPopup` (§4) | `combochecklist_get_flags`/`_set_flags` (`GUI.cpp`) | this file |
| ba867cc534 | `Show()` (only if `!IsShown()`) before `Raise()` (§3) | `Plater::priv::priv` handlers, `Plater::priv::bring_instance_forward` | `windows-dialogs.md` |
| 026b105dcb | No `wxTRANSPARENT_WINDOW` (§3); it is a no-op `0`, so this was cleanup, not a build fix | `MainFrame` (`ResizeEdgePanel`, side-tool panels) | this file |
| eefdabcd98, f70d30bf79 (#13074) | The auto-added `WS_CAPTION` (3.3.0, #23575; eefdabcd98's message says 3.3.2) made `DefWindowProc` subtract a caption from the maximised client area, and on Windows 10 left the native frame visible behind the custom title bar ("double window"). The `MainFrame` ctor strips `WS_CAPTION` from `GWL_STYLE` right after creation (`SetWindowPos(… SWP_FRAMECHANGED)`); `WM_NCCALCSIZE` computes border thickness with `GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION` and strips the border overshoot itself when maximised; f70d30bf79 restored the `wxEVT_MAXIMIZE` handler that clamps the maximised frame to the display client area | `MainFrame::MainFrame`, `MainFrame::MSWWindowProc` (`WM_NCCALCSIZE`), `AdjustWorkingAreaForAutoHide` | `platforms.md` §MSW title bar |
| 46e47cec0a | `wxGrid::GetSelectedBlocks()` (unordered, possibly overlapping, `interface/wx/grid.h:5091-5109`) is an empty range after the user deselects; compare `begin()` with `end()` and fall back to the activating cell | `GridCellSupportEditor::DoActivate` (`GUI_ObjectTable.cpp`) | `controls-dataview.md` |
| 9a053f15eb (#12936) | Since the upgrade, on macOS the Slice/Print split-button's transient popup was dismissed the moment the cursor entered the gap between button and menu; on macOS anchor transient popups flush with (slightly overlapping, 2 px) their button | `SidePopup::Popup` (`__APPLE__` branch) | `popups-menus.md` |
| 1f2ed70288 (#13119) | Orca registers its own `kAEGetURL` handler so `orcaslicer://` links reach `MacOpenURL` | `register_mac_deep_link_handler` (`DeepLinkHandlerMac.mm`), called from `GUI_App::on_init_inner` | this file |

These post-upgrade regressions were fixed: 46e47cec0a (ObjectTable crash on cell deselect),
1f2ed70288 (macOS deep links), d62aa42e61 (WebView script-handler cleanup), eefdabcd98 (maximised
window not filling the desktop), f70d30bf79 (Windows 10 double window frame), c06a0223a7 (blank 3D
canvas during MSW resize). Watch for them to
recur when touching the same code.

**Pitfalls**

- **Rule:** Keep Orca's own `kAEGetURL` registration in `GUI_App::on_init_inner`; do not rely on
  wx's handler for `orcaslicer://` links.
  **Why:** wx installs its handler in `applicationWillFinishLaunching:`
  (`src/osx/cocoa/utils.mm:46-57`); it forwards to `MacOpenURL` only once `OSXInitWasCalled()` is
  true and otherwise stores the URL with `OSXStoreOpenURL` (:191-200) [source]. After the upgrade
  the wx handler stopped delivering deep links on macOS (#13119, reported on macOS 26): links from
  Printables/Thingiverse opened a blank project. `NSAppleEventManager` keeps the last registration, so Orca's later registration wins
  and routes straight to `MacOpenURL` → `start_download`. Removing it, or registering before wx does,
  brings the regression back.
  ```cpp
  // Right (GUI_App::on_init_inner, __APPLE__): register_mac_deep_link_handler();
  ```
  Cite: 1f2ed70288; `DeepLinkHandlerMac.mm`.

- **Rule:** Do not reintroduce `wxTRANSPARENT_WINDOW` or a post-creation
  `SetBackgroundStyle(wxBG_STYLE_TRANSPARENT)` to make a panel blend in; set its background colour,
  dark-mode mapped, and re-apply it on theme change.
  **Why:** see §3. The side-tool panels lost MSW transparency with the upgrade, and 8248b06337
  (b9952b39ad) fixed them by giving them `StateColor::darkModeColorFor(wxColour("#3B4446"))`,
  re-applied in `MainFrame::update_side_button_style`.
  Cite: 026b105dcb, 8248b06337 (b9952b39ad).
