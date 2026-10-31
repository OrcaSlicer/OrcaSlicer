# DPI, bitmaps and fonts

How wx 3.3.2 maps DIP, logical and physical pixels on each platform, how DPI changes reach a window,
and how OrcaSlicer sizes layout (`FromDIP`, `em_unit`), rescales (`DPIAware`), rasterizes icons
(`BitmapCache`, `create_scaled_bitmap`, `ScalableBitmap`) and chooses fonts (`Label` table). Read it
for any fixed size, icon, bitmap, image list, font, or `on_dpi_changed` work, and when a bug looks
like "too small / too big / blurry / clipped on another monitor".

Contents: [Rules](#rules) · [Pixel kinds per platform](#pixel-kinds-per-platform) ·
[FromDIP / ToDIP / FromPhys](#fromdip--todip--fromphys--tophys) ·
[Scale factors and GetDPI](#scale-factors-and-getdpi) ·
[Choosing a size unit](#choosing-a-size-unit-fromdip-em_unit-text-metrics) ·
[wxEVT_DPI_CHANGED](#wxevt_dpi_changed) · [DPIAware rescale path](#dpiaware-rescale-path-orca) ·
[wxBitmap](#wxbitmap-physical-size-scale-factor-logical-size) ·
[wxBitmapBundle](#wxbitmapbundle-and-its-limits-in-orcas-build) ·
[Orca icon pipeline](#orca-icon-pipeline) ·
[Image lists, art provider, wxImage](#image-lists-art-provider-wximage) ·
[Window icons](#window-icons) · [Displays](#displays-and-ppi) · [wxFont](#wxfont) ·
[Orca fonts](#orca-fonts-label-table-sysfont-initsysfont)

## Rules

1. Hard-coded layout pixel values (sizes, min sizes, borders, gaps, spacers) go through
   `FromDIP(n)` (or `n * em_unit(this)` in em-based code), never raw ints. §Choosing a size unit
2. Do not `FromDIP` values that are not logical pixels: `wxBitmap` ctor sizes and `GetSize()`,
   `wxImageList` sizes and `wxBitmapBundle::GetBitmap(size)` are physical; bundle default sizes
   and `wxArtProvider::GetBitmapBundle` sizes are DIP. §wxBitmap, §wxBitmapBundle
3. On Linux, widths that must hold text come from text metrics, best sizes or `em_unit`, not
   from a fixed `FromDIP` width. §Choosing a size unit
4. Call `FromDIP` on a created window, or on `parent` in base-ctor arguments; when the window
   may be null use the static `wxWindow::FromDIP(x, win)`, never `win->FromDIP(x)` (fd80ded5a8).
   §FromDIP
5. Pick bitmap resolution with `GetDPIScaleFactor()`; never with `GetContentScaleFactor()`
   (always 1 on MSW) or `GetDPI().x / 96.0` (72-based on macOS). §Scale factors
6. Every top-level window is a `DPIDialog`/`DPIFrame` whose `on_dpi_changed` re-rasterizes named
   bitmaps and re-sets them on controls, calls each Orca widget's `Rescale()`, re-applies every
   size stored from `em_unit`/`FromDIP`, then re-establishes the minimum and fits. An empty
   override is acceptable only for a trivial dialog with no bitmaps and no stored sizes.
   §DPIAware rescale path
7. Every `wxEVT_DPI_CHANGED` handler you bind — on a child, a control, or on the TLW from a
   component — calls `Skip()`; only DPIAware's own TLW handler deliberately does not.
   §wxEVT_DPI_CHANGED
8. In a DPIAware window, never rely on wx's MSW top-level auto-resize; size the window in
   `on_dpi_changed`. §wxEVT_DPI_CHANGED, §DPIAware rescale path
9. `DPIAware::scale_factor()` is the display scale only on MSW (Orca's `get_dpi_for_window` is a
   96 stub elsewhere); use `GetDPIScaleFactor()`/`FromDIP` for anything else. §DPIAware
10. On GTK `em_unit` is measured from the font, with the same formula in the ctor and the
    rescale path (40eab797c6). §DPIAware
11. Icons are SVG resource names (no path, no extension) passed to `create_scaled_bitmap`,
    `ScalableBitmap` or `Button`, with the real window and an explicit size;
    `wxBitmapBundle::FromSVG*` does not exist in Orca's wx. §Orca icon pipeline
12. Layout that depends on a bitmap uses its logical size (`ScalableBitmap::GetBmpSize()`,
    `wxBitmap::GetLogicalSize()`), not `GetSize()`/`GetWidth()`. §wxBitmap
13. Owner-drawn offscreen bitmaps use `CreateWithDIPSize(sz, GetDPIScaleFactor())` or
    `CreateWithLogicalSize(GetClientSize(), GetDPIScaleFactor())`, not `wxBitmap(FromDIP(sz))`.
    §wxBitmap
14. Draw a bundle with `GetBitmapFor(win)`, never `GetBitmap(GetDefaultSize())`; bundle size
    queries take a created, non-null window. §wxBitmapBundle
15. `wxImageList` sizes are physical and must equal the added bitmaps' sizes; prefer
    `SetImages()` with bundles. §Image lists
16. Fonts come from `Label::Head_*`/`Label::Body_*`; never literal point sizes. §Orca fonts
17. After `wxFont::SetFaceName` check `IsOk()` and fall back. §wxFont
18. Private fonts are registered only by `Label::initSysFont()`, early in
    `GUI_App::on_init_inner`; never `AddPrivateFont` on macOS. §Orca fonts
19. On MSW a `wxMemoryDC` sizes text for its bitmap's scale factor: give the bitmap the window's
    `GetDPIScaleFactor()` before selecting it; any `Label` font then renders correctly. §wxFont
20. Never index `wxDisplay` with an unchecked `GetFromWindow()` result. §Displays

## Pixel kinds per platform

Contract (`docs/doxygen/overviews/high_dpi.md:139-149`): "Under MSW, logical pixels are always
the same as physical pixels, but are different from DIPs, while under all the other platforms
with DPI scaling support (currently only GTK 3 and macOS), logical pixels are the same as DIP, but
different from physical pixels." Conversions: DIP↔logical with `FromDIP/ToDIP`, physical↔logical
with `FromPhys/ToPhys`, DIP↔physical by multiplying/dividing by `GetDPIScaleFactor()`.

| | MSW | macOS | GTK3 (Linux default; X11 and Wayland) | GTK2 (opt-out build) |
|---|---|---|---|---|
| logical (all window/DC API) | = physical | = DIP (points) | = DIP | = physical |
| `FromDIP(x)` | x·DPI/96, rounded | identity | identity | identity [source] |
| `GetContentScaleFactor()` | always 1 | backing scale (1 or 2) | integer GDK scale | 1 |
| `GetDPIScaleFactor()` | DPI/96 (1.25, 1.5, 1.75…) | = content scale | = content scale | 1 |
| `GetDPI()` | per window, 96-based | 72 × scale | 96 × scale | 96 |
| bitmap scale factor | stored; drives bundle selection and `wxMemoryDC` text size; never changes drawn size [source] | stored; drawn size = physical / scale | stored; drawn size = physical / scale | not stored |
| `wxEVT_DPI_CHANGED` | PMv2 manifest + Win10 1703 | on backing-scale change [source] | GTK ≥ 3.10, wx ≥ 3.3.0 | never |
| app-level HiDPI | per-monitor v2 manifest | `NSHighResolutionCapable`, `NSPrincipalClass` | automatic; fractional scales rounded to an integer | only global `GDK_SCALE`/`GDK_DPI_SCALE` |

Platforms:
- **MSW.** Orca ships its own manifest, `src/dev-utils/platform/msw/OrcaSlicer.manifest.in`:
  `<dpiAware>true/pm</dpiAware>` and `<dpiAwareness>permonitorv2,permonitor</dpiAwareness>`. That
  is the per-monitor v2 awareness wx needs to send DPI events (`interface/wx/event.h:3585-3590`).
  On Windows versions that only honour the `permonitor` (v1) fallback, wx's
  `IsPerMonitorDPIAware()` accepts only PMv2, so no DPI handling runs there [source]
  `src/msw/nonownedwnd.cpp:IsPerMonitorDPIAware, wxNonOwnedWindow::HandleDPIChange`.
- **macOS.** `src/dev-utils/platform/osx/Info.plist.in` (the template `src/CMakeLists.txt`
  configures) sets `NSPrincipalClass=NSApplication` (the key `docs/doxygen/overviews/high_dpi.md:339-341` requires) and
  `NSHighResolutionCapable=true`. Its standard PPI is 72, not 96 (`include/wx/display.h`
  `wxDisplay::GetStdPPIValue`; documented `interface/wx/display.h:196-211`).
- **GTK3** (the Linux build: `option(DEP_WX_GTK3 … ON)` in `deps/CMakeLists.txt`, Flatpak too).
  "wxGTK only supports integer scaling factors currently and fractional scales are rounded to
  the closest integer" (`docs/doxygen/overviews/high_dpi.md:348-351`). A Wayland compositor's fractional scale therefore
  reaches wx as an integer GDK scale.
- **GTK2** (only with `-DDEP_WX_GTK3=OFF`). `wxHAS_DPI_INDEPENDENT_PIXELS` is defined only for
  `__WXGTK3__ || __WXMAC__ || __WXQT__` (`include/wx/features.h:115-120`), so GTK2 takes the
  "real conversion" branch of `FromDIP`. But the GTK2 `wxDisplayImplGTK` does not override
  `GetScaleFactor()` (only under `GTK_CHECK_VERSION(3,10,0)`, `src/gtk/display.cpp`), and the base
  `GetPPI()` is `GetStdPPI()*GetScaleFactor()` = 96 (`include/wx/private/display.h:99-103`). Net
  effect [source]: `FromDIP` is the identity on GTK2 too, and GTK2 HiDPI exists only through the
  global env vars (`docs/doxygen/overviews/high_dpi.md:353-355`). Code guarded for GTK must still compile there.

Exceptions to "every API takes logical pixels" (`docs/doxygen/overviews/high_dpi.md:169-183`): sizes passed to `wxBitmap`
constructors and returned by `GetWidth/GetHeight/GetSize` are **physical**;
`wxBitmapBundle::GetPreferredBitmapSizeFor()` is physical (`GetPreferredLogicalSizeFor()` is the
logical twin); the bundle **default size** (`FromSVG` argument, `GetDefaultSize()`) is **DIP**.
`wxGLCanvas` drawing is also physical (see `references/webview-gl-aui-media.md`).

## FromDIP / ToDIP / FromPhys / ToPhys

**Contract** (`interface/wx/window.h:1089-1121`): "A DPI-independent pixel is just a pixel at the
standard 96 DPI resolution … this scaling may be already done by the underlying toolkit (GTK+,
Cocoa, ...) automatically. This method performs the conversion only if it is not already done by
the lower level toolkit." It "is only needed when using hard coded pixel values. It is not
necessary if the sizes are already based on the DPI-independent units such as dialog units or if
you are relying on the controls automatic best size determination and using sizers". A component
equal to `-1` is returned unchanged, so `wxSize(FromDIP(490), -1)` keeps "unspecified"
(`interface/wx/window.h:1113-1116`). `ToDIP` is the inverse; the doc's use case is persisting window geometry
DPI-independently (`interface/wx/window.h:1166-1189`).

**Static overloads** `FromDIP(sz|pt|d, const wxWindow* w)` (`interface/wx/window.h:1141-1163`) accept
`w == nullptr`, but are "discouraged as passing NULL will prevent your application from correctly
supporting monitors with different resolutions". [source] With null on MSW the DPI comes from
`wxDisplay().GetPPI()`, the primary display (`src/common/wincmn.cpp` `GetDPIHelper`); on
macOS/GTK the conversion is the identity regardless.

**FromPhys/ToPhys** (`interface/wx/window.h:1233-1321`): physical↔logical; "does nothing under MSW, but divides
the input value by the content scale factor under the other platforms", rounding to the closest
integer ("15 physical pixels are translated to 8"). The static form with a null window uses "the
content scale factor of the main screen if supported" (`interface/wx/window.h:1270-1282`); [source] that is
macOS only, 1 elsewhere (`src/common/wincmn.cpp` `GetContentScaleFactorFor`). Use them for genuinely physical quantities only (bitmap pixel
sizes, GL viewports), never for layout constants.

**Platforms** [source]:
- MSW rounds per call (`wxMulDivInt32`, `include/wx/private/rescale.h`), so
  `FromDIP(a) + FromDIP(b)` can differ from `FromDIP(a + b)` by 1 px at 125 %/175 %. Convert the
  sum when two values must line up.
- MSW `GetDPI()` on a window without an HWND (two-step creation, or `this` inside a base-class
  argument) falls back to the top-level parent's HWND, else to the screen DC — the primary
  monitor (`src/msw/window.cpp` `wxWindowMSW::GetDPI`; the "possibly wrong DPI" log is compiled
  out in Orca).
- The `interface/wx/window.h:1101-1106` example `wxBitmap bmp(FromDIP(32, 32))` contradicts the physical-bitmap
  rule (`docs/doxygen/overviews/high_dpi.md:173-178`) and gives a 1x bitmap on macOS/GTK3; use the `wxBitmap` creation helpers instead
  (§wxBitmap).

**OrcaSlicer.** `FromDIP(n)` is the convention for every fixed size in new code. Shared
macros build on the member form and expand only inside a `wxWindow` member function:
`ICON_SINGLE_SIZE`/`ICON_SIZE` (`GUI_Utils.hpp`, `FromDIP(16)`; their comment says not to change
them and to define new sizes locally) and `MSG_DIALOG_BUTTON_SIZE` (`MsgDialog.hpp`).
`create_scaled_bitmap` uses the static form because its `win` argument may be null.

**Pitfalls**
- **Rule:** Never call a member function through a `wxWindow*` that is allowed to be null; use
  the static null-safe overload.
  **Why:** `win->FromDIP()` on null is UB; clang assumes `this != nullptr` and deletes later
  `win ? … : …` checks, turning the fallback into a call through a null vtable. This crashed
  LLVM/clang-cl builds at startup while MSVC survived by luck. The static overload falls back to
  the primary-display DPI on MSW and is the identity on macOS/GTK.
  ```cpp
  unsigned h = win->FromDIP(px_cnt);              // Wrong: UB when win == nullptr
  unsigned h = wxWindow::FromDIP(px_cnt, win);    // Right: static, null-safe
  ```
  Cite: fd80ded5a8 (`src/slic3r/GUI/wxExtensions.cpp` `create_scaled_bitmap`).
- **Rule:** In base-class constructor arguments convert through the parent, not `this`.
  **Why:** `this` is not yet a constructed window there; on MSW (and GTK2) the member form calls
  the virtual `GetDPI()` on it, which is UB, and even a constructed window without an HWND
  reports the primary monitor's DPI. On macOS/GTK3 the member form is the identity, so the bug
  shows only on Windows.
  ```cpp
  MyPanel(wxWindow* p) : wxPanel(p, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(300), -1)) {}    // Wrong
  MyPanel(wxWindow* p) : wxPanel(p, wxID_ANY, wxDefaultPosition, wxSize(p->FromDIP(300), -1)) {} // Right
  ```
  Cite: [source] `src/msw/window.cpp` `wxWindowMSW::GetDPI`.

## Scale factors and GetDPI

**Contract.**
- `GetDPIScaleFactor()` (`interface/wx/window.h:1605-1626`): "1 for standard DPI screens or 2 for '200%
  scaling' and, unlike for GetContentScaleFactor(), is the same under all platforms. This factor
  should be used to increase the size of icons and similar windows whose best size is not based
  on text metrics … should *not* be used for window sizes expressed in pixels, as they are
  already scaled by this factor by the underlying toolkit under some platforms. Use FromDIP() for
  anything window-related instead." It answers "how many physical pixels per DIP", i.e. which
  raster resolution to produce.
- `GetContentScaleFactor()` (`interface/wx/window.h:1576-1603`): "the factor mapping logical pixels of this
  window to physical pixels"; on platforms without pixel mapping (MSW) it "always returns 1.0".
  Note in the doc: it equalled `GetDPIScaleFactor()` in wx 3.1.0–3.1.3 only. Use it for physical
  buffers (GL, `FromPhys`), not to choose icon sizes.
- `GetDPI()` (`interface/wx/window.h:2285-2295`): per window, can differ between windows on Windows 10;
  `wxSize(0,0)` if unavailable. On macOS it is 72-based: `wxWindowMac::GetDPI()` is
  `MakeDPIFromScaleFactor(GetDPIScaleFactor())` = 72 × scale [source] `src/osx/window_osx.cpp`.

**Platforms** [source]: macOS `GetContentScaleFactor()` is the `NSWindow`'s
`backingScaleFactor`, or the main screen's when the view has no window yet
(`src/osx/cocoa/window.mm` `wxWidgetCocoaImpl::GetContentScaleFactor`). GTK returns
`gtk_widget_get_scale_factor` (an integer; 1 on GTK2), and `GetDPIScaleFactor()` is the same value
(`src/gtk/window.cpp` `wxWindowGTK::GetContentScaleFactor/GetDPIScaleFactor`).

**Pitfalls**
- **Rule:** Derive a scale with `GetDPIScaleFactor()` or `wxDPIChangedEvent::Scale*`, never by
  dividing a DPI by 96.
  **Why:** macOS's standard PPI is 72 (`interface/wx/display.h:203-205`), so `GetDPI().x / 96.0`
  is 1.5 on a 2x Retina screen and 0.75 on a 1x one.
  ```cpp
  double s = GetDPI().x / 96.0;          // Wrong on macOS
  double s = GetDPIScaleFactor();        // Right
  ```
- **Rule:** Choose icon/raster resolution from `GetDPIScaleFactor()`, not
  `GetContentScaleFactor()`.
  **Why:** content scale is always 1 on MSW (`interface/wx/window.h:1587-1592`), so icons never grow there.

## Choosing a size unit: FromDIP, em_unit, text metrics

Two scaling currencies coexist in Orca, plus the text metrics wx recommends:

| Unit | Use for | Value per platform |
|---|---|---|
| `FromDIP(n)` | fixed sizes in new code: icon sizes, borders, gaps, control heights, min sizes not driven by text | MSW n·DPI/96; macOS/GTK identity |
| `em_unit` (`em_unit(this)`, `wxGetApp().em_unit()`, `DPIAware::em_unit()`) | the settings code (`Tab`, `OptionsGroup`, `Field`, `ObjectList` columns) and any size that must follow text on Linux; sizes written as multiples, `wxSize(65 * em, 30 * em)` | MSW `max(10, 10 × scale_factor)`; macOS always 10; GTK width of "m" in the window font − 1 (min 10) |
| text metrics (`GetTextExtent`, best sizes, `ConvertDialogToPixels`) | widths that hold translated text | follow the font everywhere |

The overview prefers text metrics or dialog units over pixel values and calls `FromDIP` "the
simplest change" (`docs/doxygen/overviews/high_dpi.md:71-78`).

**Platforms.** On GTK, text follows the font DPI and the desktop text-scaling factor (Xft DPI,
`GDK_DPI_SCALE`) while `FromDIP` stays the identity at GDK scale 1, so a fixed `FromDIP` width
that fits a label on MSW/macOS can clip it on Linux. That is why `DPIAware` measures `em_unit`
from the font on GTK (§DPIAware rescale path).

**OrcaSlicer.** `em_unit(wxWindow*)` (`wxExtensions.cpp`) walks to the top-level parent
(`find_toplevel_parent`) and returns that `DPIDialog`'s or `DPIFrame`'s own `em_unit()`, else
`wxGetApp().em_unit()`; the per-window value matters when windows sit on monitors with different
DPI. `wxGetApp().em_unit()` is 10 until `GUI_App::update_fonts` copies the main frame's value
(called from `MainFrame::on_dpi_changed` and at main-frame setup).

**Pitfalls**
- **Rule:** Size boxes that contain text from the text, not from a fixed `FromDIP` width.
  ```cpp
  label->SetMinSize(wxSize(FromDIP(120), -1));                       // Wrong: clips on GTK text scaling
  label->SetMinSize(wxSize(label->GetTextExtent(text).x + FromDIP(8), -1)); // Right (or N * em, or -1 + sizer)
  ```
  Cite: `docs/doxygen/overviews/high_dpi.md:71-78`; [source] `DPIAware::update_em_unit` comment (`GUI_Utils.hpp`).

## wxEVT_DPI_CHANGED

**Contract** (`interface/wx/event.h:3560-3593`): sent "to each wxTopLevelWindow affected by the
change, and all its children recursively (post-order traversal)" — on a move to a monitor with a
different DPI or a system DPI change. "You should almost always call event.Skip() … as many
controls rely on processing this event in order to update their appearance". The TLW's default
handler "only sets the new window size, by scaling the current size by the DPI ratio … and also
ensuring that the window is still bigger than its best size"; to prevent it, handle the event on
the TLW, `SetSize()` there and do *not* Skip. Documented generators: wxMSW "if and only if" Windows
10 1703+ with a PerMonitorV2 manifest; wxGTK "when using GTK 3.10 or later and only since
wxWidgets version 3.3.0".

Helpers (`interface/wx/event.h:3610-3660`): `GetOldDPI()`, `GetNewDPI()`, `Scale(wxSize)`, and since 3.3.0
`Scale(wxPoint)`/`Scale(wxRect)`, `ScaleX/ScaleY` — old-DPI→new-DPI via `wxMulDivInt32`. Prefer
them to `GetNewDPI()/96` (72-based on macOS).

**Platforms** [source unless noted]:

| Port | Who generates it | What wx itself rescales |
|---|---|---|
| MSW | `WM_DPICHANGED` → `wxNonOwnedWindow::HandleDPIChange` (`src/msw/nonownedwnd.cpp`), only for PMv2-aware windows | `wxWindowMSW::MSWUpdateOnDPIChange` (`src/msw/window.cpp`) recurses from the TLW through every non-TLW child; for each window it first rescales `m_min/maxWidth/Height`, invalidates best size, re-creates the window font at the new PPI (`MSWUpdateFontOnDPIChange`; the TLW's override `wxTopLevelWindowMSW::MSWUpdateFontOnDPIChange` only re-selects its icons and leaves the TLW font alone), rescales sizer borders, spacer sizes and nested-sizer min sizes (`UpdateSizerOnDPIChange`; window items keep their min size because each window scales its own), then recurses into its children, then sends the event to that window. If the TLW's event was **not processed**, wx `SetSize()`s the TLW to the suggested rect inflated to the sizer's min size |
| macOS | `windowDidChangeBackingProperties` when the backing scale changes (`src/osx/cocoa/nonownedwnd.mm`), DPIs built from the 72-based std PPI (`wxWindowBase::WXNotifyDPIChange`). Undocumented. The same notification sends `wxSysColourChangedEvent` when the colour space changes | nothing (logical = DIP) |
| GTK3 | TLW configure event when `GetContentScaleFactor()` changed (`src/gtk/toplevel.cpp` `wxTopLevelWindowGTK::GTKConfigureEvent`, `__WXGTK3__` only); the initial scale is captured at creation, so there is no event at startup; DPIs are 96 × integer scale | nothing (logical = DIP) |
| GTK2 | never | — |

The default TLW resize exists only on MSW; on macOS/GTK3 logical sizes do not change with DPI.

Handler order: children receive the event before their TLW (post-order, documented; [source]
`src/common/wincmn.cpp` `NotifyAboutDPIChange`; MSW recursion in `MSWUpdateOnDPIChange`).
Dynamic handlers run most recently bound first, before static tables
(`docs/doxygen/overviews/eventhandling.h:475-482`), so a handler you `Bind` on
a control runs before the control's own (`wxBookCtrlBase`, `wxComboCtrlBase`, `wxTreeCtrlBase` bind
one; wxMSW `wxStaticBitmap` uses a static table entry, `src/msw/statbmp.cpp`, and MSW button
bitmaps bind one, `src/msw/anybutton.cpp`).

**What wx rescales vs what Orca must re-apply:**

| Item | MSW (done by wx before the event) | macOS / GTK3 | Orca's `on_dpi_changed` must |
|---|---|---|---|
| window min/max sizes | rescaled by the DPI ratio | unchanged (logical = DIP) | re-set only if it recomputes them anyway |
| window font set by `SetFont` | re-created at the new PPI for non-TLW windows; the TLW keeps its old-PPI font [source] | unchanged (points) | nothing: a `wxFont` stores points and every window or DC `SetFont` re-adjusts it to its target's PPI; `DPIAware::rescale` re-reads it and updates `em_unit` |
| sizer borders, spacers, nested-sizer min sizes | rescaled | unchanged | nothing |
| TLW size | resized only if the TLW event is unprocessed — never for DPIAware windows | unchanged | re-establish min size, `Fit()`/`SetSize()` |
| values given to setters (`SetRowHeight`, column widths, `SetItemMinSize`, custom-widget sizes, cached pixel members) | never | never | re-apply from `FromDIP`/`em_unit` |
| bitmaps on native controls | reselected from the control's bundle; for Orca's single bitmaps that is the old raster, unscaled or integer-upscaled | GTK never; macOS from the bundle | `msw_rescale()` + `SetBitmap()` |
| Orca widgets (cached measures, named icons) | never | never | call `Rescale()` |

**Pitfalls**
- **Rule:** Call `Skip()` in every `wxEVT_DPI_CHANGED` handler you bind, on a child, a control or
  the TLW (DPIAware's own handler is the one exception).
  **Why:** your dynamically bound handler runs first; without `Skip()` the control's own handler
  (book controls, combo controls, MSW static bitmaps and button images) never runs and keeps its
  old-DPI appearance. Bound on a `DPIDialog`/`DPIFrame` (as `m_parent` is here), it also starves
  DPIAware's own handler, so `on_dpi_changed` never runs.
  ```cpp
  m_parent->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) { UpdateButtons(); });             // Wrong
  m_parent->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) { UpdateButtons(); e.Skip(); });   // Right
  ```
  Cite: `interface/wx/event.h:3572-3575`; `src/slic3r/GUI/Widgets/DialogButtons.cpp`
  `DialogButtons::on_dpi_changed`.
- **Rule:** Do not add a second `wxEVT_DPI_CHANGED` handler on a `DPIDialog` expecting wx to
  resize the dialog; put the sizing in `on_dpi_changed()`.
  **Why:** DPIAware's TLW handler never Skips, so the event counts as processed and wxMSW skips
  its "scale size / ensure ≥ best size" resize.
  Cite: [source] `src/msw/nonownedwnd.cpp` `wxNonOwnedWindow::HandleDPIChange`.

## DPIAware rescale path (Orca)

`template<class P> DPIAware : public P` (`src/slic3r/GUI/GUI_Utils.hpp`) wraps `wxDialog`/`wxFrame`;
`DPIFrame` (typedef) and `DPIDialog` (subclass) are the instantiations every Orca top-level window
uses. Its modal, ESC and dark-mode parts are in `references/windows-dialogs.md` and
`references/colours-dark-mode.md`; this section is the DPI part.

**Constructor.**
- `m_scale_factor = get_dpi_for_window(this) / 96` and `m_prev_scale_factor` = the same.
  `get_dpi_for_window` (`GUI_Utils.cpp`) is real only on Windows (`GetDpiForWindow`, falling back
  to `GetDpiForMonitor` or the DC); on Linux and macOS it is a `// TODO` stub returning
  `DPI_DEFAULT` (96), so `m_scale_factor` starts at 1.0 there.
- `m_normal_font = get_default_font_for_dpi(this, dpi)` (MSW: `SystemParametersInfoForDpi`
  message font for that DPI; elsewhere `wxSYS_DEFAULT_GUI_FONT`), applied with `SetFont` except
  on macOS (`#ifndef __WXOSX__`, comment "Don't call SetFont under OSX to avoid name cutting in
  ObjectList"). The window font is set before `em_unit` is measured because the default window
  font is the primary display's.
- `update_em_unit()`:
  ```cpp
  #if !defined(__WXGTK__)
      m_em_unit = std::max<size_t>(10, 10.0f * m_scale_factor);   // MSW: DPI-based; macOS: always 10
  #else
      m_em_unit = std::max<size_t>(10, this->GetTextExtent("m").x - 1);   // GTK: from the font
  #endif
  ```

**Bindings.**
- `wxEVT_DPI_CHANGED`, non-macOS only (`#ifndef __WXOSX__`): stores
  `GetNewDPI().x / 96` in `m_scale_factor` and calls `rescale(wxRect())` if
  `m_can_rescale && (m_force_rescale || is_new_scale_factor())`. It does **not** `Skip()`: the
  dialog sizes itself, so wx's MSW TLW resize is suppressed. On GTK3 this handler fires (wx 3.3)
  and sets a real integer scale from the 96-based DPI; on macOS it is not bound, so
  `on_dpi_changed` never runs there; on GTK2 there is no event.
- `wxEVT_MOVE_START`/`wxEVT_MOVE_END` (wxMSW-only events, `interface/wx/event.h:5006-5014`): START
  clears `m_can_rescale`, so a DPI event during an interactive drag only records the new factor;
  END rescales with the move rect if the factor changed, else re-arms `m_can_rescale`. The DPI
  handler itself is what rescales; MOVE_END only performs a rescale that was deferred.
- `enable_force_rescale()` makes the next DPI event rescale even if the factor is unchanged.

**`rescale(suggested_rect)`**: `Freeze()` → `m_normal_font = GetFont()` (the TLW font, which wxMSW
does not re-create on a DPI change; harmless, because a `wxFont` stores points and is re-adjusted
to the PPI of whatever window or DC it is set on) → `update_em_unit()` → pure virtual `on_dpi_changed(suggested_rect)`
→ `Layout()` → `Thaw()` → `m_prev_scale_factor = m_scale_factor`. `suggested_rect` is empty on the
DPI-event path and the moved window rect on the MOVE_END path; do not rely on it.

**What `on_dpi_changed` does** (the canonical shape):
```cpp
void MyDialog::on_dpi_changed(const wxRect&) {
    m_logo.msw_rescale();                    // ScalableBitmap: new raster at the new DPI
    m_logo_ctrl->SetBitmap(m_logo.bmp());    // the control holds its own copy
    m_ok_btn->Rescale();                     // every Orca widget (Button, TextInput, ComboBox, ...)
    const int em = em_unit();
    msw_buttons_rescale(this, em, {wxID_CLOSE});   // native stock-id wxButtons only
    m_list->SetMinSize(wxSize(-1, 16 * em));       // re-apply stored sizes
    SetMinSize(wxSize(65 * em, 30 * em));          // explicit minimum + Fit(), or
    Fit();                                         // GetSizer()->SetSizeHints(this) in place of both
    Refresh();
}
```
`AboutDialog::on_dpi_changed` is this shape (ScalableBitmap + `SetBitmap`, html fonts re-derived
from `GetFont()`, `msw_buttons_rescale`, min sizes, `Fit()`, `Refresh()`).
`PreferencesDialog::on_dpi_changed` shows the child walk: recurse `GetChildren()` and call
`Rescale()` on each Orca widget found by `dynamic_cast`; inside `namespace Slic3r::GUI` write the
types qualified (`::CheckBox`, whose method is `Rescale()`, not `msw_rescale()`), because
unqualified `CheckBox` names the `Field` class there (`references/orca-widgets.md`). wx 3.3's
`CallForEachChild(functor)` (`interface/wx/window.h:602-624`) does the same recursive walk, the
window itself included; it also descends into owned top-level children ([source]
`include/wx/window.h` `wxWindowBase::CallForEachChild`).

**Cascade.** `MainFrame::on_dpi_changed` is the root for the main window: `update_fonts`, the
tab panel / top bar / buttons `Rescale()`, `plater()->msw_rescale()`, the param panel, lazily
built pages through `when_built`, then a `SetSize(sz + 1)`/`SetSize(sz)` jiggle (with
un-maximize/re-maximize) to force a full redraw. Child panels expose `msw_rescale()`/`Rescale()`
and are called from their owner; they do not get `on_dpi_changed`.

**Self-rescaling components.** `DialogButtons` binds its parent's `wxEVT_DPI_CHANGED` in the ctor,
unbinds in the dtor, restyles and `Skip()`s. Being bound after DPIAware's handler, it runs first.
This is the model for a component that must rescale without its owner's help. Keep one-time
`Bind` calls out of the restyle function such a handler runs: `Bind` does not deduplicate, so a
handler bound there runs once more after every DPI change.

**Helpers.**
- `msw_buttons_rescale(dlg, em, ids)` (`wxExtensions.cpp`; all platforms despite the name) calls
  `SetMinSize(wxSize(-1, 2.5 * em))` on whatever window has each id. Meant for native `wxButton`s;
  an Orca `Button` with that id (every `DialogButtons` OK/Cancel) takes it too and loses its style
  height through `Button::SetMinSize`, so leave it out for Orca buttons
  (`references/sizers-layout.md` §Layout on DPI change).
- `scale_factor()`/`prev_scale_factor()` are meaningful only on MSW (and on GTK3 after a DPI
  event); `em_unit()`, `normal_font()` are the per-window values.

**Pitfalls**
- **Rule:** On GTK derive `em_unit` from the current font, and use the same computation in the
  ctor and the rescale path (one helper).
  **Why:** on GTK `DPIAware` starts with scale 1.0 because Orca's `get_dpi_for_window` is a stub
  there; only the GTK3 `wxEVT_DPI_CHANGED` sets a real (integer) scale. The defect 40eab797c6 fixed: the ctor
  measured the font while `rescale()` used `max(10, 10 × scale)` — e.g. 20 at 2× — so controls laid
  out with a different em after a Linux DPI change than at construction.
  ```cpp
  m_em_unit = std::max<int>(10, 10.0f * m_scale_factor);   // Wrong: in rescale(), on every platform
  update_em_unit();                                        // Right: same platform-branched helper as the ctor
  ```
  Cite: 40eab797c6 (`src/slic3r/GUI/GUI_Utils.hpp` `DPIAware::update_em_unit`).
- **Rule:** In `on_dpi_changed`/`msw_rescale`, re-apply every size computed from `em_unit` or
  `FromDIP` at construction (row heights, column widths, min sizes, cached pixel members).
  **Why:** wx never rescales values passed to setters (`SetRowHeight`, column widths) on any
  port; wxMSW only rescales stored min/max sizes and sizer spacers. Stale values clip or
  overflow after a DPI or theme change (the object-list filament badge stopped fitting its row).
  ```cpp
  // ctor:           SetRowHeight(2 * em + FromDIP(2));
  // msw_rescale():  SetRowHeight(2 * em + FromDIP(2));   // must repeat with the new em
  //                 GetColumn(cn)->SetWidth(m_columns_width[cn] * em);
  ```
  Cite: d5638273c6 (`src/slic3r/GUI/GUI_ObjectList.cpp` `ObjectList::create_objects_ctrl`,
  `ObjectList::msw_rescale`). Layout side: `references/sizers-layout.md`.
- **Rule:** In `on_dpi_changed`, re-establish the minimum and resize: `GetSizer()->SetSizeHints(this)`
  (does both), or `SetMinSize(...)` then `Fit()`/`SetSize()`; never leave a dialog without a
  minimum that re-`Fit()`s on refresh paths.
  **Why:** on MSW nothing else resizes a DPIAware dialog (wx's TLW resize is suppressed), so
  `Refresh()` alone leaves it at its old physical size. A `Fit()` on a dialog without size hints
  can collapse it on GTK when children are transiently zero-sized (after iconizing the main
  window); with hints in place `Fit()` is safe. `wxSizer::SetSizeHints(win)` "first calls Fit()
  and then wxTopLevelWindow::SetSizeHints()" (`interface/wx/sizer.h:937-970`), so a `Fit()` right
  before it is redundant, and one after it re-applies the best size without the display clamp
  (`references/sizers-layout.md` §Fitting functions); plain `Fit()` sets no minimum.
  ```cpp
  Layout(); Fit();                                   // Wrong: ctor, no enforced minimum
  void on_dpi_changed(const wxRect&) override { Refresh(); Fit(); }
  Layout(); Fit(); v_sizer->SetSizeHints(this);      // Right: ctor (the Fit() is redundant)
  void on_dpi_changed(const wxRect&) override { GetSizer()->SetSizeHints(this); Refresh(); }
  ```
  Cite: f760f4e462 (`src/slic3r/GUI/calib_dlg.cpp` `FlowRateCalibrationDialog`; the GTK collapse
  mechanism is from the commit message, not visible in source); mechanism in
  `references/sizers-layout.md`.
- **Rule:** Do not size anything from `scale_factor()`/`prev_scale_factor()` off MSW.
  **Why:** `get_dpi_for_window()` returns a hard-coded 96 on Linux and macOS, so the factor is 1
  at construction there (on GTK3 it can later jump to the event's integer scale; on macOS it
  never changes).
  ```cpp
  int w = int(120 * scale_factor());   // Wrong: 120 px on a Retina Mac / 2x GTK3 at startup
  int w = FromDIP(120);                // Right
  ```

## wxBitmap: physical size, scale factor, logical size

**Contract.**
- Size arguments of `wxBitmap` constructors and `GetWidth/GetHeight/GetSize` are physical
  (`docs/doxygen/overviews/high_dpi.md:173-178`, `interface/wx/bitmap.h:788-800`).
- `CreateWithDIPSize(size, scale)` (`interface/wx/bitmap.h:486-521`): physical size = `size × scale`, rounded;
  afterwards `GetDIPSize() == size`, `GetScaleFactor() == scale`. For fixed (compile-time) sizes.
  `CreateScaled` is its older synonym (`interface/wx/bitmap.h:579`).
- `CreateWithLogicalSize(size, scale)` (since 3.3.0, `interface/wx/bitmap.h:523-558`): for sizes from
  `GetClientSize()` etc. with `scale = GetDPIScaleFactor()`; physical = `size` on MSW,
  `size × scale` where `wxHAS_DPI_INDEPENDENT_PIXELS` is defined.
- `GetLogicalSize()` (`interface/wx/bitmap.h:685-706`): physical / scale factor on DPI-independent ports,
  `GetSize()` elsewhere; "must be used in any computations involving the sizes expressed in
  logical units" (`docs/doxygen/overviews/high_dpi.md:176-178`). `GetScaledSize/Width/Height` are its older synonyms.
  `GetDIPSize()` (`interface/wx/bitmap.h:645-659`) is the same value on all platforms and "should not be used
  as window or device context coordinates".
- `SetScaleFactor(scale)` (`interface/wx/bitmap.h:951-966`) changes no pixels, only the apparent drawn size,
  "in the ports in which logical and physical pixels differ (i.e. wxOSX and wxGTK3, but not
  wxMSW)". The doc of `GetScaleFactor()` says it "always returns 1 under the other platforms"
  (`interface/wx/bitmap.h:744-751`) — **[source] contradicted on MSW**: `wxGDIImage` stores the factor "to use
  the correct sizes in the code which uses it to decide on the bitmap size to use"
  (`src/msw/gdiimage.cpp` `wxGDIImage::SetScaleFactor`, `GetDIPSize`); bundle selection reads
  it (§wxBitmapBundle) and the MSW memory DC sizes text by it (§wxFont). GTK2 has no scale storage (`include/wx/gtk/bitmap.h`, `__WXGTK3__` only).
- `wxBitmap(const wxImage&, int depth, double scale)` exists on all three ports [source], but the
  MSW one ignores `scale` (`double WXUNUSED(scale)`, `include/wx/msw/bitmap.h:68`); call
  `SetScaleFactor()` afterwards on MSW. The `scale` argument does not resize: it declares that
  the image is already sized for that backing scale (Orca's comments in `BitmapCache.cpp`
  `wxImage_to_wxBitmap_with_alpha` and `BitmapComboBox.cpp` say the same). `wxBitmap(img, dc)`
  inherits the DC's scale (`interface/wx/bitmap.h:370-385`).
- `wxBitmap(const wxCursor&)` is invalid on GTK under Wayland (`interface/wx/bitmap.h:388-401`).
- wxMSW `wxBitmap::Create(size, dc)` no longer multiplies by the DC's content scale
  (`docs/changes.txt:94-96`).

**Offscreen drawing.** "The scaling factor of the bitmap determines the scaling factor used by
this device context" (`interface/wx/dcmemory.h:41-58`); `wxMemoryDC(wxDC*)` does **not** inherit
the DC's scaling (`interface/wx/dcmemory.h:80-89`). The cross-platform shape needs no `#ifdef`:
```cpp
wxBitmap bmp;
bmp.CreateWithDIPSize(wxSize(24, 24), GetDPIScaleFactor());   // fixed-size art
{ wxMemoryDC mdc(bmp); mdc.SetFont(GetFont()); /* draw in logical coords: FromDIP() values */ }
dc.DrawBitmap(bmp, pos);                                       // 24 DIP, sharp on Retina/GTK3
// back buffer: bmp.CreateWithLogicalSize(GetClientSize(), GetDPIScaleFactor());
```
[source] On MSW the memory DC does not scale coordinates (logical = physical there, so a
`CreateWithDIPSize` bitmap is drawn with `FromDIP` coordinates), but it does size text by the
bitmap: `wxMemoryDCImpl::DoSelect` records the selected bitmap's scale factor and
`wxMemoryDCImpl::SetFont` adjusts every font to `GetPPI()` = 96 × that factor
(`src/msw/dcmemory.cpp`). The macOS memory DC applies the bitmap scale to its graphics context
(`src/osx/core/dcmemory.cpp`). Back-buffering and DC coordinates are in
`references/painting-custom-widgets.md`.

**OrcaSlicer.** `SwitchButton::Rescale` is the legacy manual HiDPI pattern: on macOS it measures
with `dc.GetFont().Scaled(scale)`, draws into a `scale ×` image and wraps it with
`wxBitmap(img, -1, scale)`, using `mac_max_scaling_factor()`; on MSW it draws into a scale-1
bitmap with `GetFont().Scaled(GetDPIScaleFactor())` (compensating the memory DC's 96-PPI text) and
tags the result with `SetScaleFactor` afterwards. New owner-drawn caches use `CreateWithDIPSize`/`CreateWithLogicalSize` +
`wxMemoryDC` instead.

**Pitfalls**
- **Rule:** Create drawn bitmaps with a DIP size and the window's scale.
  **Why:** bitmap sizes are physical and `FromDIP` is the identity on macOS/GTK3, so the first
  form is a 1x bitmap upscaled (blurry) on Retina and 2x GTK3; on MSW its scale factor stays 1, so
  text drawn into it through a `wxMemoryDC` comes out at 100 % size.
  ```cpp
  wxBitmap bmp(FromDIP(wxSize(32, 32)));                                    // Wrong
  wxBitmap bmp; bmp.CreateWithDIPSize(wxSize(32, 32), GetDPIScaleFactor()); // Right
  ```
  Cite: `docs/doxygen/overviews/high_dpi.md:173-178`, `interface/wx/dcmemory.h:41-56`.
- **Rule:** Lay out from logical bitmap sizes.
  **Why:** physical ≠ logical off MSW; `GetSize()` of a 2x bitmap is twice its drawn size.
  ```cpp
  int w = bmp.GetWidth() + FromDIP(4);          // Wrong: double width on Retina
  int w = bmp.GetLogicalSize().x + FromDIP(4);  // Right (ScalableBitmap::GetBmpWidth() for Orca icons)
  ```
  Cite: `interface/wx/bitmap.h:685-706`.

## wxBitmapBundle and its limits in Orca's build

**Contract.**
- Any API taking `const wxBitmapBundle&` accepts a `wxBitmap` through the implicit converting
  constructor (`interface/wx/bmpbndl.h:110-116`). This is how every Orca bitmap reaches wx
  controls: Orca code does not build bundles itself.
- Selection (`docs/doxygen/overviews/high_dpi.md:245-255`, `interface/wx/bmpbndl.h:52-62`): use the closest existing bitmap without
  scaling; scale only when the mismatch is large. The overview says "equal or greater than 1.5";
  **[source]** the code scales only when the target scale is **greater than** 1.5 × the largest
  available, and then by an integer factor (or rounds the target scale)
  (`src/common/bmpbndl.cpp` `wxBitmapBundleImpl::DoGetPreferredSize`).
- Single-bitmap bundle [source] (`bmpbndl.cpp` `wxBitmapBundleImplSet::Init`,
  `GetNextAvailableScale`): default size = `GetDIPSize()` of the smallest bitmap; its available
  scale = (DIP size / default size) × `GetScaleFactor()`. Consequences: a 16 px bitmap with
  scale 1 is shown unscaled at 150 % and upscaled to 32 px at 175 %/200 %; a 24 px bitmap tagged
  `SetScaleFactor(1.5)` has DIP size 16 and is used as-is at 150 %.
- `GetBitmap(size)` (`interface/wx/bmpbndl.h:415-428`): size "in physical pixels"; dynamically created sizes
  are cached until exit ("avoid calling it for many different sizes"). [source] the result gets
  `SetScaleFactor(size.y / GetDefaultSize().y)` (`bmpbndl.cpp` `wxBitmapBundle::GetBitmap`), so
  `GetBitmap(GetDefaultSize())` always yields a scale-1 bitmap at the DIP size — a downscaled 1x
  bitmap on HiDPI.
- `GetBitmapFor(win)`, `GetPreferredBitmapSizeFor(win)` (physical),
  `GetPreferredLogicalSizeFor(win)` (logical) take a "Non-null and fully created window"
  (`interface/wx/bmpbndl.h:392-441`); null hits a `wxCHECK` and returns `wxDefaultSize` silently in Orca.
- `FromBitmaps(vec)` / `FromBitmaps(b1, b2)` (`interface/wx/bmpbndl.h:158-169`): all bitmaps valid, sizes
  physical, the smallest defines the default size. `FromImpl(new MyImpl)` takes ownership ("must
  not call DecRef()", `interface/wx/bmpbndl.h:200-212`). `FromFiles` also looks in a `2.0x` subdirectory since
  3.3.2 (`interface/wx/bmpbndl.h:231-247`). A custom `wxBitmapBundleImpl` implements `GetDefaultSize()` (DIP),
  `GetPreferredBitmapSizeAtScale()` (physical; may defer to `DoGetPreferredSize()` when
  `GetNextAvailableScale()` is overridden) and non-const `GetBitmap(size)` (`interface/wx/bmpbndl.h:522-560`).
- Auto-update on DPI change happens only on MSW and macOS (`docs/doxygen/overviews/high_dpi.md:205-209`); GTK controls
  keep the bitmap chosen at set time.
- The overview asks for art usable unscaled at least at 100 % and 200 % (or a single SVG), and
  advises against shipping only a high-resolution version to be downscaled on 1x displays
  ("contours become more blurry", `docs/doxygen/overviews/high_dpi.md:189-197`). In Orca the SVG
  route is `BitmapCache`, not a bundle (below).

**Orca's build.** `deps/wxWidgets/wxWidgets.cmake` passes `-DwxUSE_NANOSVG=OFF` (7658cf9076,
duplicate symbols with Orca's own nanosvg) and LunaSVG stays off, so `wxHAS_SVG` — defined only for
`wxHAS_RAW_BITMAP && (wxUSE_NANOSVG || wxUSE_LUNASVG)` (`include/wx/features.h:96-98`) — is
undefined. `wxBitmapBundle::FromSVG`, `FromSVGFile` and `FromSVGResource` do not exist (compile
error; `interface/wx/bmpbndl.h:272-275` says to check `wxHAS_SVG`). Knock-on effects [source]: the Tango art
provider returns empty bundles (`src/common/arttango.cpp`, `!wxHAS_SVG` branch), the std
provider's SVG logo is absent (`src/common/artstd.cpp`), and wxAUI tab/dock buttons fall back to
1-bit XBM art (`src/aui/tabart.cpp`, `src/aui/dockart.cpp`). Orca rasterizes SVG itself
(`BitmapCache::load_svg`) and recolours it for dark mode, which a stock bundle could not do.

**Why Orca keeps single bitmaps + explicit rescale** rather than bundles: no SVG bundles in this
build; GTK does not auto-update bundles anyway; owner-drawn widgets must re-measure on DPI change;
and the icon raster must also change on a theme switch. The MSW `SetScaleFactor` tagging in
`create_scaled_bitmap` makes the implicit single-bitmap bundle report the intended DIP size
(§Orca icon pipeline).

If a stock wx control ever needs auto-updating multi-resolution art, the Orca-compatible shape
is a bundle implementation backed by `BitmapCache` (not existing Orca code; a sketch):
```cpp
struct OrcaSvgBundleImpl : wxBitmapBundleImpl {
    std::string name; wxSize def;                                  // def in DIP
    wxSize GetDefaultSize() const override { return def; }
    wxSize GetPreferredBitmapSizeAtScale(double s) const override { return def * s; }
    wxBitmap GetBitmap(const wxSize& sz) override {                // sz is physical
        static Slic3r::GUI::BitmapCache cache;
        wxBitmap* b = cache.load_svg(name, 0, sz.y, false, wxGetApp().dark_mode());
        return b ? *b : wxBitmap();
    }
};
// wxBitmapBundle::FromImpl(new OrcaSvgBundleImpl{...});   // takes ownership
```
On macOS `BitmapCache` already multiplies by its own `m_scale`; such an impl would need a cache
whose scale is 1.

**Pitfalls**
- **Rule:** Never call `wxBitmapBundle::FromSVG*` in Orca.
  ```cpp
  auto b = wxBitmapBundle::FromSVGFile(path, wxSize(16, 16));   // Wrong: does not compile here
  ScalableBitmap icon(this, "cog", 16);                         // Right (or create_scaled_bitmap("cog", this, 16))
  ```
  Cite: `include/wx/features.h:96-98`, `deps/wxWidgets/wxWidgets.cmake`.
- **Rule:** Draw a bundle at the bitmap the window needs.
  **Why:** the size argument of `GetBitmap` is physical and the result is forced to scale 1.
  ```cpp
  wxBitmap b = bundle.GetBitmap(bundle.GetDefaultSize());   // Wrong: 1x, downscaled on HiDPI
  wxBitmap b = bundle.GetBitmapFor(this);                   // Right; draw at b.GetLogicalSize()
  ```
  Cite: [source] `src/common/bmpbndl.cpp` `wxBitmapBundle::GetBitmap`; `interface/wx/bmpbndl.h:425`.
- **Rule:** Pass a created, non-null window to bundle size queries.
  ```cpp
  bundle.GetPreferredBitmapSizeFor(nullptr);   // Wrong: wxDefaultSize, silently
  bundle.GetPreferredBitmapSizeFor(this);      // Right, after Create()
  ```
  Cite: `interface/wx/bmpbndl.h:399`.

## Orca icon pipeline

Icons are SVG files in `resources/images/`, referenced by **name string without extension**
(`BitmapCache` resolves `Slic3r::var(name + ".svg")`, then `".png"`). The entry points live in
`src/slic3r/GUI/wxExtensions.hpp/.cpp` and `BitmapCache.hpp/.cpp`.

**`create_scaled_bitmap(name, win = nullptr, px_cnt = 16, grayscale, new_color, menu_bitmap,
resize, bitmap2, array_new_color)`**:
```cpp
static BitmapCache cache;                                     // process-wide, never cleared
unsigned h = wxWindow::FromDIP(px_cnt, win) + 0.5f;           // static overload: win may be null
bool dark = menu_bitmap (MSW only) ? check_dark_mode() : wxGetApp().dark_mode();
wxBitmap* b = cache.load_svg(name, 0, h, grayscale, dark, new_color, resize ? em_unit(win) * 0.1f : 0);
if (!b) b = cache.load_png(name, 0, h, grayscale, ...);       // neither found: throws Slic3r::RuntimeError
#ifdef __WXMSW__
b->SetScaleFactor(win ? win->GetDPIScaleFactor() : wxWindow::FromDIP(100, nullptr) / 100.0);
#endif
return *b;
```
`px_cnt` is the icon height in DIP. A missing icon name throws. `bitmap2 = true` routes to
`create_scaled_bitmap2`/`load_svg2` (semi-transparent filament art, no dark recolour).

Raster per platform [source]:

| Platform | Physical height | Scale factor | Drawn (logical) size |
|---|---|---|---|
| MSW | `FromDIP(px, win)` | `win->GetDPIScaleFactor()` (primary-display ratio when `win` is null) | `FromDIP(px)` px; the tag makes the implicit bundle's DIP size `px`, so wx uses it unscaled |
| macOS | `px × BitmapCache::m_scale` (SVG) | `m_scale`, via `wxBitmap(image, -1, m_scale)` | `px` points |
| GTK3 | `px` | 1 | `px`; at GDK scale 2 it is drawn upscaled (no HiDPI raster on GTK3) |
| GTK2 | `px`, round-tripped through PNG to fix broken alpha (`wxImage_to_wxBitmap_with_alpha`) | — | `px` |

Why the MSW tag matters: at 200 % `FromDIP(16)` is a 32 px raster; untagged (scale 1) its
implicit bundle has a 32-DIP default size and wx doubles it again to 64 px, while tagged 2.0 its
DIP size is 16 and it is used as-is. A raster kept from an older DPI is reselected by the same
rule after a DPI change: a 16 px / scale-1 bitmap stays 16 px at 150 % and is upscaled to 32 px
at 200 % — hence the re-`SetBitmap` in `on_dpi_changed` (§wxBitmapBundle selection).

`BitmapCache` [source]:
- `m_scale` (macOS only) is `mac_max_scaling_factor()` read when the cache is constructed — for
  the static cache in `create_scaled_bitmap`, at the first icon load. Despite its name,
  `mac_max_scaling_factor()` (`src/slic3r/Utils/MacDarkMode.mm`) loops over the screens but
  reads `objectAtIndex:0` each time, i.e. it returns the backing factor of the first screen (the
  one with the menu bar). Icons are therefore rasterized once for that screen: 1x on a Retina
  laptop whose primary display is a 1x external monitor, with no re-rasterization when windows
  move.
- `load_svg` keys the cache by name, height, `m_scale`, `-dm` (dark), `-gs` (grayscale) and
  `new_color`; dark-mode recolouring by palette substitution is in
  `references/colours-dark-mode.md`.
- `load_png` never applies the Retina factor (`wxImage_to_wxBitmap_with_alpha(image)` with scale
  1; resized with `wxIMAGE_QUALITY_BILINEAR`) and gets no dark recolour.

**`ScalableBitmap(parent, icon_name = "", px_cnt = 16, grayscale, resize, bitmap2, new_color)`**
holds `{m_parent, m_icon_name, m_px_cnt, m_grayscale, m_resize, m_bmp}`.
- `msw_rescale()` re-runs `create_scaled_bitmap(m_icon_name, m_parent, m_px_cnt, m_grayscale,
  "", false, m_resize)` — it is the DPI path on every platform and also the theme-switch path,
  because it re-reads the dark flag. It does **not** re-apply `new_color` or `bitmap2`.
- `m_parent` is a raw pointer; the parent must outlive the `ScalableBitmap`.
- `GetBmpSize()/GetBmpWidth()/GetBmpHeight()` return the scaled (logical) size on Apple and
  `GetSize()` elsewhere — equivalent to `GetLogicalSize()` given the scales above.
- `bmp()` returns the bitmap; wx controls that were given it keep their own copy.

**`ScalableButton(parent, id, icon_name, label, size, pos, style = wxBU_EXACTFIT | wxNO_BORDER,
use_default_disabled_bitmap, bmp_px_cnt = 16)`** is a native `wxButton` with a scaled bitmap. An
explicit `size` is stored in em/10 units (`size * 10 / em`) and re-applied as `m * em / 10` in
`msw_rescale()`; `UpdateDarkUI()` is `msw_rescale()`; on GTK it calls `RemoveButtonBorder`. New
code uses `Widgets/Button` instead.

**Orca widgets.** `Button(parent, text, icon = "", style = 0, iconSize = 0, id)` keeps its icon
as a `ScalableBitmap` (20 px when `iconSize <= 0`). `Button::Rescale()` re-rasterizes a **named**
icon, re-measures and re-applies the style; an icon set through `SetIcon(const wxBitmap&)` has no
name and cannot be re-rasterized, so prefer `SetIcon(const wxString&)`. Other widgets' `Rescale()`
follow the same idea (`references/orca-widgets.md`, `references/painting-custom-widgets.md`).

**Menu icons.** `create_menu_bitmap(name)` = `create_scaled_bitmap(name, nullptr, 16, false, "",
true)`: created without a window, so at primary-display DPI on MSW, and on MSW the dark variant
follows `check_dark_mode()`. `msw_rescale_menu` exists only on MSW. Menus
are in `references/popups-menus.md`.

**Pitfalls**
- **Rule:** Pass the real window (not `nullptr`) and re-create the bitmap in `on_dpi_changed`.
  **Why:** a null window means primary-display DPI on MSW (wrong raster and wrong scale tag on a
  secondary monitor); `em_unit(nullptr)` falls back to the main frame's em for `resize`.
  ```cpp
  m_icon = ScalableBitmap(nullptr, "cog", 16);   // Wrong
  m_icon = ScalableBitmap(this, "cog", 16);      // Right; m_icon.msw_rescale() in on_dpi_changed
  ```
- **Rule:** After `msw_rescale()`, hand the new bitmap to every control that displays it.
  **Why:** `msw_rescale()` replaces only the `ScalableBitmap`'s own `m_bmp`; a `wxStaticBitmap` or
  native button keeps the old copy (on MSW wx merely rescales that old raster).
  ```cpp
  m_icon.msw_rescale();                                   // Wrong alone
  m_icon.msw_rescale(); m_bmp_ctrl->SetBitmap(m_icon.bmp()); // Right
  ```
  Cite: `src/slic3r/GUI/AboutDialog.cpp` `AboutDialog::on_dpi_changed`.
- **Rule:** Pass a bare resource name and an explicit DIP height.
  **Why:** the third argument is `px_cnt`, not a bitmap type, and the name is resolved as
  `var(name + ".svg"|".png")`. `px_cnt = 0` means "the asset's own height": it dereferences
  `parent`, and on Retina macOS it stores the physical height, which doubles the icon [source]
  `ScalableBitmap::ScalableBitmap`.
  ```cpp
  ScalableBitmap(this, Slic3r::var("logo.png"), wxBITMAP_TYPE_PNG);   // Wrong: path + type as px_cnt
  ScalableBitmap(this, "logo", 16);                                   // Right
  ```
- **Rule:** Author new icons as SVG.
  **Why:** `load_png` is never Retina-scaled and never dark-recoloured.
- **Rule:** Re-apply `new_color`/`bitmap2` art yourself on rescale.
  **Why:** `ScalableBitmap::msw_rescale()` drops both, so a recoloured icon reverts to its default
  colours after a DPI or theme change. Keep the colour and rebuild with the full constructor.

## Image lists, art provider, wxImage

**`wxImageList`** (`interface/wx/imaglist.h:37-39, 60-63`): "Use of this class is not recommended
in the new code as it doesn't support showing DPI-dependent bitmaps. Please use
wxWithImages::SetImages() instead"; "the size is specified in physical pixels and must correspond
to the size of bitmaps … that will be added". 3.3 made the size physical and makes calls on an
invalid list assert (`docs/changes.txt:53-56, 85-88`) — silently in Orca's assert-free build.
When a list is unavoidable, `wxBitmapBundle::CreateImageList(win, bundles)` builds one at the
consensus size (public but undocumented, [source] `include/wx/bmpbndl.h`).
```cpp
auto* il = new wxImageList(FromDIP(16), FromDIP(16)); il->Add(bmp_of_other_size);  // Wrong
auto sz = bmps[0].GetSize(); auto* il = new wxImageList(sz.x, sz.y);                 // Right (or SetImages(bundles))
```

**`wxArtProvider`** (`interface/wx/artprov.h:285-330`): `GetBitmap(id, client, size)` returns
that physical size; "applications using wxWidgets 3.1.6 or later should prefer calling
GetBitmapBundle()". `GetBitmapBundle(id, client, size)` takes the DIP default size — "this
implies that wxWindow::FromDIP() must not be used with it". The provider stack is native → Tango
→ std [source] `src/common/artprov.cpp`; in Orca's build Tango contributes nothing (no SVG), so
non-native ids come from low-resolution XPMs. `GetBitmap(id, client, FromDIP(wxSize(16, 16)))` is a
16-physical-pixel bitmap on Retina/GTK3 (drawn upscaled); hand the bundle to the control instead.
```cpp
wxArtProvider::GetBitmapBundle(wxART_WARNING, wxART_OTHER, FromDIP(wxSize(16, 16)));  // Wrong: double-scaled on MSW
wxArtProvider::GetBitmapBundle(wxART_WARNING, wxART_OTHER, wxSize(16, 16));           // Right; give the bundle to the control
```

**`wxImage`** resizing (`interface/wx/image.h:28-89`): `wxIMAGE_QUALITY_NEAREST` is no longer an
alias of `NORMAL` since 3.3.0 (`docs/changes.txt:98-99`; `wxIMAGE_QUALITY_FAST` is the speed
synonym). `NORMAL` (default) = bilinear down to an integer multiple, then box average; `HIGH` =
box average when shrinking, bicubic when enlarging; `BILINEAR`, `BICUBIC`, `BOX_AVERAGE` explicit.
High-quality scaling "may not work as expected when using a single mask colour for
transparency" — use alpha (`interface/wx/image.h:1016-1019`). `Rescale` mutates and returns `*this`; `Scale`
returns a copy. `wxInitAllImageHandlers()` registers the compiled handlers: in Orca there is no
TIFF (`wxUSE_LIBTIFF=OFF`), WebP is built in, and SVG is not an image handler.
`wxImage::SetDefaultLoadFlags(0)` drops `Load_Verbose` warnings for images created afterwards
(`interface/wx/image.h:1825-1838`). For pixel-exact glyphs choose `NEAREST`; for icons `BILINEAR` (what
`load_png` uses) or `HIGH`; for photos `HIGH`.

## Window icons

`wxTopLevelWindow::SetIcon` (`interface/wx/toplevel.h:505-525`): "In wxMSW, icon must be either
16x16 or 32x32"; under Wayland it "doesn't do anything … create a `.desktop` file".
`SetIcons(wxIconBundle)` (`interface/wx/toplevel.h:527-546`): MSW wants 16 and 32, "preferably both"; also a
no-op on Wayland, where the icon comes from the desktop file matched by app id
(`wxApp::SetClassName`, `interface/wx/app.h:761-768`). [source] wxMSW picks the small and big
icon from the bundle at the window's DPI-aware system-metric sizes with `FALLBACK_NEAREST_LARGER`
and re-picks them on every DPI change (`src/msw/toplevel.cpp` `wxTopLevelWindowMSW::DoSetIcons`,
`MSWUpdateFontOnDPIChange`), so a bundle with 16/20/24/32/48 px entries stays sharp at any
scale; `SetIcon` is a one-icon bundle. `wxIconBundle(file,
type)` loads every icon in the file (`interface/wx/iconbndl.h:53`); `GetIcon(size, flags)` falls
back per `FALLBACK_SYSTEM` (default), `FALLBACK_NEAREST_LARGER` or `FALLBACK_NONE`
(`interface/wx/iconbndl.h:29-42, 141-158`).

Orca: the main frame takes its icon from the executable's resource on MSW and from
`OrcaSlicer_128px.png` elsewhere (`MainFrame.cpp` `main_frame_icon`).
```cpp
SetIcon(wxIcon(path_to_multi_size_ico, wxBITMAP_TYPE_ICO));            // Wrong: one size, scaled for both slots
SetIcons(wxIconBundle(path_to_multi_size_ico, wxBITMAP_TYPE_ICO));     // Right
```

## Displays and PPI

Window placement on displays is in `references/windows-dialogs.md`; the resolution side:
- `wxDisplay(const wxWindow*)` (since 3.1.2) is the display showing the window, "falling back to
  the default display if it is not shown at all or positioned outside of any display"
  (`interface/wx/display.h:35-50`). `GetFromWindow(win)` returns `wxNOT_FOUND` when the window is
  on no display (`interface/wx/display.h:115-126`). [source] on macOS it picks an intersecting display with the
  same backing scale as the window, else `wxNOT_FOUND` (`src/osx/core/display.cpp`
  `wxDisplayFactoryMacOSX::GetFromWindow`).
- `GetPPI()` is the scaled resolution, `wxSize(0,0)` if unknown (`interface/wx/display.h:158-168`);
  `GetRawPPI()` is unscaled, new in 3.3.2 (`interface/wx/display.h:170-181`); `GetScaleFactor()` = PPI / std
  PPI (`interface/wx/display.h:183-194`); `GetStdPPIValue()` is 96, 72 on Apple (`interface/wx/display.h:196-221`).
- `IsConnected()` (3.3.0): objects go stale after a display configuration change; recreate them on
  `wxEVT_DISPLAY_CHANGED`, do not cache `wxDisplay` (`interface/wx/display.h:223-241`).
- [source] `wxDisplay(unsigned n)` only `wxASSERT`s the index and then indexes a vector
  (`src/common/dpycmn.cpp`), so `(unsigned)wxNOT_FOUND` reads out of bounds in Orca's
  assert-free build. `GUI_App::window_pos_sanitize`/`window_pos_center` show the checked pattern.
```cpp
wxDisplay(wxDisplay::GetFromWindow(win)).GetClientArea();   // Wrong: wxNOT_FOUND → out of bounds
wxDisplay(win).GetClientArea();                              // Right (or check != wxNOT_FOUND first)
```

## wxFont

**Contract.**
- Sizes are points (1/72 in): `wxFontInfo(double pointSize)` (fractional since 3.1.2,
  `interface/wx/font.h:323-330`), or pixels via `wxFontInfo(wxSize)` / `SetPixelSize`, which is
  "directly supported only under wxMSW and wxGTK currently; under other platforms a font with the
  closest size … is found using binary search" (`interface/wx/font.h:1124-1138`). Prefer
  `SetFractionalPointSize` to the legacy integer `SetPointSize` (`interface/wx/font.h:1100-1123`).
- `MakeBold/MakeLarger/MakeSmaller/Scale` mutate; `Bold/Larger/Smaller/Scaled` return copies
  (`interface/wx/font.h:853-999`); Larger/Smaller use a factor of 1.2.
- `SetFaceName(face)` (`interface/wx/font.h:1021-1036`): if the face does not exist "the font is invalidated (so
  that IsOk() will return false) and false is returned" ([source] `src/common/fontcmn.cpp`
  `wxFontBase::SetFaceName` → `UnRef()`; the check is `wxFontEnumerator::IsValidFacename`, which
  caches the face list on first use for the session (`src/common/fontenumcmn.cpp`); only the
  Unix `AddPrivateFont` invalidates that cache).
- `wxFont::AddPrivateFont(path)` (`interface/wx/font.h:719-751`):
  - macOS: does nothing but check that the file exists inside `Resources/Fonts` of the bundle;
    the app must ship it there and set `ATSApplicationFontsPath`. [source] it compares the path
    with `GetResourcesDir() + "/Fonts"` and `wxLogError`s otherwise (`src/osx/fontutil.cpp`).
  - MSW: "must be called before any wxGraphicsContext objects have been created";
    [source] `AddFontResourceEx(FR_PRIVATE)`, remembered for GDI+ (`src/msw/font.cpp`).
  - Unix: needs Pango ≥ 1.38, else returns false and logs. [source] creates one fontconfig config
    on the first call, then on **every** call adds the file, re-installs the config into Pango's
    font map (`pango_fc_font_map_set_config`) and invalidates the face-name cache
    (`src/gtk/font.cpp` `wxFontBase::AddPrivateFont`).

**DPI** [source]:
- On MSW a `wxFont` stores points; `SetFractionalPointSize` computes `lfHeight` at the primary
  screen PPI and relies on `WXAdjustToPPI()` later (`src/msw/font.cpp`
  `wxNativeFontInfo::SetFractionalPointSize`).
- `wxWindow::SetFont/GetFont` adjust the window's copy to its own PPI
  (`src/common/wincmn.cpp` `wxWindowBase::SetFont/GetFont` → `WXAdjustFontToOwnPPI`), and
  non-TLW window fonts are re-adjusted on DPI change (`src/msw/window.cpp`
  `MSWUpdateFontOnDPIChange`; `src/msw/toplevel.cpp` overrides it for TLWs to re-select icons
  only).
- `wxMSWDCImpl::SetFont` adjusts to the DC's window PPI when it has a window (`src/msw/dc.cpp`).
  A `wxMemoryDC` has none; `wxMemoryDCImpl` overrides `SetFont`/`GetPPI` instead and adjusts every
  font to 96 × the scale factor of the bitmap selected into it, re-applied at each `SelectObject`
  (`src/msw/dcmemory.cpp` `wxMemoryDCImpl::DoSelect/SetFont/GetPPI`). With a scale-1 bitmap text is
  laid out at 100 % whatever the monitor and whatever font object is passed: `GetFont()` of a
  150 % window is re-adjusted down to 96 PPI too. A `wxGCDC`/`wxGraphicsContext` created from that
  memory DC follows the same rule: the GDI+ context has no window, and its DPI is 96 × the bitmap's
  scale factor (`src/msw/graphics.cpp` `wxGDIPlusRenderer::CreateContext(const wxMemoryDC&)`,
  `wxGDIPlusContext::GetDPI`) — which is what `StaticBox::render`'s MSW anti-aliasing block hands to
  `doRender` (`references/painting-custom-widgets.md`).
- So a global `wxFont` such as `Label::Body_14` is DPI-correct on any monitor when set on a window,
  a window DC, or a memory DC whose bitmap was given the window's `GetDPIScaleFactor()` before it
  was selected (`CreateWithDIPSize`/`CreateWithLogicalSize`, or `SetScaleFactor` then
  `SelectObject`, as `get_extruder_color_icon` in `wxExtensions.cpp` does).
- On macOS and GTK a point is a fixed number of logical pixels (1 on macOS, 4/3 at 96 DPI on
  GTK), so fonts need no DPI handling.

**Pitfalls**
- **Rule:** Check `IsOk()` after `SetFaceName` and fall back.
  ```cpp
  font.SetFaceName("X"); dc.SetFont(font);                              // Wrong: invalid font if X is missing
  if (!font.SetFaceName("X")) font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);  // Right (Label::sysFont pattern)
  ```
  Cite: `interface/wx/font.h:1030-1032`.
- **Rule:** On MSW, give a memory-DC bitmap the window's scale factor before selecting it; the
  font object does not fix memory-DC text size.
  **Why:** the MSW memory DC sizes text for 96 × the selected bitmap's scale factor, so text drawn
  into a scale-1 `FromDIP`-sized bitmap comes out at 100 % size, too small at 125–200 %, even with
  the window's own `GetFont()`.
  ```cpp
  wxBitmap bmp(FromDIP(wxSize(24, 24)));                   // Wrong: scale 1
  wxMemoryDC mdc(bmp); mdc.SetFont(GetFont());             //        text at 96 PPI on a 150 % monitor
  wxBitmap bmp; bmp.CreateWithDIPSize(wxSize(24, 24), GetDPIScaleFactor());   // Right
  wxMemoryDC mdc(bmp); mdc.SetFont(Label::Body_12);        //        text at 144 PPI
  ```
  Cite: [source] `src/msw/dcmemory.cpp` `wxMemoryDCImpl::DoSelect`, `wxMemoryDCImpl::SetFont`;
  `src/slic3r/GUI/wxExtensions.cpp` `get_extruder_color_icon` (`SetScaleFactor` before
  `SelectObject`).

## Orca fonts: Label table, sysFont, initSysFont

**The table** (`src/slic3r/GUI/Widgets/Label.hpp/.cpp`): static fonts `Label::Head_48, 32, 24, 20,
18, 16, 15, 14, 13, 12, 11, 10` (bold) and `Label::Body_16, 15, 14, 13, 12, 11, 10, 9, 8` (regular),
built by `Label::initSysFont()`. Convention: `Head_*` for titles and section headers, `Body_*` for
content; `Body_14` is the dialog workhorse (`SetFont(Label::Body_14)` on dialogs and controls),
`Body_12`/`Body_13` for dense secondary text. `Label` widgets default to `Body_14`.

**`Label::sysFont(size, bold)`**:
```cpp
#ifndef __APPLE__
    size = size * 4 / 5;                           // integer arithmetic
#endif
wxString face = "HarmonyOS Sans SC";
if (wxLocale::GetSystemLanguage() == wxLANGUAGE_KOREAN) face = "NanumGothic";
wxFont font{size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL, false, face};
font.SetFaceName(face);
if (!font.IsOk()) { font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT); if (bold) font.MakeBold(); font.SetPointSize(size); }
```
- The 4/5 factor approximates the 72-vs-96 PPI difference (a point is 1 logical px on macOS,
  4/3 px at 96 DPI elsewhere), so the same `Body_N` looks alike on macOS and MSW/Linux. That is
  why literal point sizes are wrong: 12 pt is 12 logical px on macOS but 16 px at 100 % on MSW, so
  `wxFont(12, …)` looks a third larger there.
- Integer truncation makes some entries identical on MSW/Linux: `Body_16`/`Body_15` and
  `Head_16`/`Head_15` are 12 pt, `Body_11`/`Body_10` and `Head_11`/`Head_10` are 8 pt. Pick the
  next step down when a visible difference matters.
- The Korean face is keyed on the **system** language (`wxLocale::GetSystemLanguage()`), not on
  Orca's UI language.
- The statics are created once and stay DPI-correct on MSW because wx adjusts fonts per window,
  window DC and memory-DC bitmap scale (§wxFont).

**`Label::initSysFont()`** runs near the start of `GUI_App::on_init_inner()` (after the log
target and the macOS deep-link handler), before any window or `wxGraphicsContext` exists, which
satisfies the MSW `AddPrivateFont` rule. On MSW and Linux it registers
`resources/fonts/HarmonyOS_Sans_SC_{Bold,Regular}.ttf` and `NanumGothic-{Regular,Bold}.ttf`
with `wxFont::AddPrivateFont` (`wxUSE_PRIVATE_FONTS=ON` in `deps/wxWidgets/wxWidgets.cmake`). On
Linux it skips all four calls when fontconfig already knows both families (e.g. installed
system-wide in a Flatpak): Orca's comment records that `AddPrivateFont` triggers a Pango crash in
`ensure_faces()` on Pango ≥ 1.48 because `FcConfigAppFontAddFile` invalidates Pango's cached font
map. macOS never calls it: the bundle plist sets `ATSApplicationFontsPath = fonts/`, and wx's
macOS `AddPrivateFont` would reject any path outside `Resources/Fonts`.

**App fonts.** `GUI_App::init_fonts()`/`update_fonts()` derive `normal_font()`, `small_font()`,
`bold_font()`, `link_font()` and `code_font()` from the `Label` statics (`update_fonts` uses
`Body_14`; `init_fonts` overrides small/bold sizes on macOS; the code font is
`wxFONTFAMILY_TELETYPE` at the small size). The splash screen's `scale_font` works around MSW
`SetFractionalPointSize` using the primary PPI by computing `lfHeight` for the splash's own DPI.

**macOS notes.** `DPIAware` and the main frame skip their default-font `SetFont` on macOS ("name
cutting in ObjectList"); that is about the window default font, not a ban — dialogs set
`Label::Body_14` normally. `OG_CustomCtrl` draws a focused URL label underlined but not bold on
macOS (workaround for a Big Sur bold-font rendering issue).

**Pitfalls**
- **Rule:** Use the `Label` table, never hard-coded point sizes or ad-hoc faces.
  ```cpp
  title->SetFont(wxFont(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));  // Wrong
  title->SetFont(Label::Head_12);                                                         // Right
  ```
  Cite: `src/slic3r/GUI/Widgets/Label.cpp` `Label::sysFont`.
- **Rule:** Do not call `wxFont::AddPrivateFont` for Orca's fonts outside `Label::initSysFont`.
  **Why:** macOS rejects paths outside `Resources/Fonts` (fonts load through the plist); MSW
  requires the call before any graphics context, and [source] does not invalidate the
  session-cached face list, so a `SetFaceName` check made before registration keeps rejecting the
  private face; on Linux every call re-installs Pango's fontconfig map, which crashes Pango ≥ 1.48.
  Cite: `interface/wx/font.h:719-751`; `Label::initSysFont`.
