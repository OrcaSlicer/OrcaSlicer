# Colours and dark mode

Covers `wxColour`, system colours and the appearance API, `wxEVT_SYS_COLOUR_CHANGED`, wxMSW's own dark
mode, colour inheritance and the places native controls ignore colours, `StateColor`, Orca's dark-mode
machinery (the dark-mode state, the `Update*DarkUI` walk, NppDarkMode, the runtime switch per platform) and
dark-mode icons. Read it before setting any colour, adding a dialog/panel, or debugging "wrong colour in
dark (or light) mode".

Contents: [Rules](#rules) · [wxColour](#wxcolour) · [System colours and appearance](#system-colours-and-appearance) ·
[wxEVT_SYS_COLOUR_CHANGED](#wxevt_sys_colour_changed) · [wxMSW dark mode](#wxmsw-dark-mode-mswenabledarkmode-setappearance-wxdarkmodesettings) ·
[Window colours and native-control limits](#window-colours-inheritance-and-native-control-limits) ·
[StateColor](#statecolor) · [Orca dark-mode state](#orca-dark-mode-state) ·
[The Update*DarkUI walk](#the-updatedarkui-walk) · [NppDarkMode](#nppdarkmode-windows) ·
[Runtime theme switch](#runtime-theme-switch-and-re-applying-colours) · [Dark-mode icons](#dark-mode-icons)

## Rules

1. Ask Orca, not wx, whether the UI is dark: `wxGetApp().dark_mode()`. Never `wxSystemSettings::GetAppearance().IsDark()`
   or `SelectLightDark()` in GUI code — on Windows they report the system app mode — and do not read the
   `dark_color_mode` key directly (macOS `dark_mode()` ignores it). → [Orca state](#orca-dark-mode-state)
2. Theme with palette colours, not `wxSYS_COLOUR_*`: on Windows wx serves its dark palette whenever the
   system app mode is dark, and macOS system colours are dynamic so the dark map cannot key them. → [System colours](#system-colours-and-appearance)
3. Write colours as `wxColour("#RRGGBB")` or `wxColour(r, g, b)`. A packed integer `wxColour(0xRRGGBB)` is
   read as `0x00BBGGRR`; `StateColor` integers are the opposite (`0xRRGGBB`). → [wxColour](#wxcolour)
4. Give every `wxPanel`/`wxScrolledWindow` you create an explicit palette background (`*wxWHITE`, `#F8F8F8`, …),
   and set it **before** creating Orca widgets inside it. → [Window colours](#window-colours-inheritance-and-native-control-limits), [Update*DarkUI](#the-updatedarkui-walk)
5. Background colour is never inherited; foreground only at creation, only from the immediate parent, only for
   `wxControl`-like classes. Set colours on the window that shows them. → [Window colours](#window-colours-inheritance-and-native-control-limits)
6. In a `StateColor`, list specific states first and `Normal` last; negate with `Not*`/`Disabled`, never `~X`. → [StateColor](#statecolor)
7. A literal light colour given to `SetForegroundColour`/`SetBackgroundColour`/`wxPen`/`wxBrush` goes through
   `StateColor::darkModeColorFor()` when it is a `gDarkColors` key, else branch on `dark_mode()`. → [Re-applying colours](#runtime-theme-switch-and-re-applying-colours)
8. End every dialog constructor (after all children exist, before `ShowModal()`) with `wxGetApp().UpdateDlgDarkUI(this)`;
   frames use `UpdateFrameDarkUI`; a subtree built after the app-wide pass ends with `UpdateDarkUIWin(this)`.
   `UpdateDarkUI(win)` themes one window only. → [Update*DarkUI](#the-updatedarkui-walk)
9. Apply deliberate non-palette colours **after** the walk, and again on theme change; the dark walk rewrites every
   visited window's foreground. → [Update*DarkUI](#the-updatedarkui-walk)
10. Re-apply every construction-time colour and every name-selected icon in `on_sys_color_changed()` (DPIDialog/DPIFrame)
    or a `sys_color_changed()` chained from the owner, ending with `Refresh()`. A long-lived (cached, lazily built,
    hidden) window must be reached from `MainFrame::on_sys_color_changed`. → [Runtime switch](#runtime-theme-switch-and-re-applying-colours)
11. A `wxEVT_SYS_COLOUR_CHANGED` handler on a TLW or container calls `Skip()` and is idempotent. Do not rely on the
    event reaching nested children in Orca. → [wxEVT_SYS_COLOUR_CHANGED](#wxevt_sys_colour_changed)
12. Do not call `wxApp::SetAppearance()` or change the `MSWEnableDarkMode(DarkMode_Auto)` → `NppDarkMode::InitDarkMode()`
    order in `GUI_App::on_init_inner`. → [wxMSW dark mode](#wxmsw-dark-mode-mswenabledarkmode-setappearance-wxdarkmodesettings)
13. An explicit `dark_color_mode` ("0" or "1") wins in both directions; `check_dark_mode()` is only the fallback for an
    unset key. → [Orca state](#orca-dark-mode-state)
14. A new theme-switch path first sets the `dark_color_mode` key (Windows: `app_config->set` + `save()`; macOS/Linux:
    `update_dark_config()`), then refreshes the state computed from `dark_mode()`: `m_is_dark_mode` (`Update_dark_mode_flag()`,
    which `update_dark_config()` already calls), StateColor's `gDarkMode` and the label colours (`init_label_colours()`)
    and, on Windows, NppDarkMode's `g_darkModeEnabled` (`force_colors_update()`). `dark_mode()` itself is live; wx's own
    MSW mode follows the system and is not Orca's to set. → [Orca state](#orca-dark-mode-state)
15. Author single-tone SVGs in the substitution palette (uppercase hex, `#262E30` for near-black line art); anything
    else needs a `*_dark` asset chosen by name in code that re-runs on theme change. → [Icons](#dark-mode-icons)
16. Never use a native `wxButton`'s `SetBackgroundColour` for styling (MSW turns it owner-drawn, macOS ignores it):
    use Orca `Button` + `SetStyle()`. → [Window colours](#window-colours-inheritance-and-native-control-limits)
17. Re-apply text-control colours after every `Enable()` (macOS resets them). → [Window colours](#window-colours-inheritance-and-native-control-limits)
18. Windows menu bitmaps follow wx's menu state (`check_dark_mode()`), not `dark_mode()`. → [Icons](#dark-mode-icons)

## wxColour

**Contract.** Constructors: `()`, `(r, g, b, a = wxALPHA_OPAQUE)`, `(unsigned long|long|int|unsigned int)` ("A
packed RGB value", `interface/wx/colour.h:98-101`), `(const wxString&|const char*|const wchar_t*)`;
`wxColour(bool) = delete` (`include/wx/colour.h:225-240`; `docs/changes.txt:246-248`: code "unintentionally
and mistakenly using wxColour ctor from bool … doesn't compile any longer").

| Fact | Cite |
|---|---|
| Packed integers are `0x00BBGGRR` (`0xAABBGGRR` for `SetRGBA`): "Notice the right-to-left order of components!" `wxColour(0x009688)` is R=0x88 G=0x96 B=0x00, not Orca teal. Only symmetric greys (`0xEEEEEE`) read the same both ways. wx writes its own MSW dark palette this way (`wxColour(0x9e5315)` is a blue). | `interface/wx/colour.h:179-183`; `include/wx/colour.h:77-83`; `src/msw/darkmode.cpp` `wxDarkModeSettings::GetColour` |
| `Set(const wxString&)` accepts colour-database names, CSS `rgb(r,g,b)`/`rgba(r,g,b,a)` (case-insensitive) and `#` + 6 hex digits; returns `false` on failure. **[source]** the parser also takes `#rgb`, `#rgba`, `#rrggbbaa`, but the documented form (and XRC, "but not "#rgb"") is `#RRGGBB` — write that. | `interface/wx/colour.h:288-301`; `src/common/colourcmn.cpp` `FromString`; `docs/doxygen/overviews/xrc_format.h:229` |
| The string ctor is `{ Set(colourName); }`: a typo silently yields `IsOk() == false`. An invalid colour passed to `SetBackgroundColour`/`SetForegroundColour` means "reset to the default colour". | `include/wx/colour.h:235`; `interface/wx/colour.h:239-243`; `interface/wx/window.h:2438-2439` |
| 3.3 changed `wxColourDatabase` to CSS values ("GREEN" is `#008000` in the CSS scheme, `#00ff00` traditionally; wxGTK already used CSS); `UseScheme()` reverts. **[source]** stock objects did not change: `*wxGREEN` is still (0,255,0), so `*wxGREEN != wxColour("green")`. | `docs/changes.txt:31-33`; `interface/wx/gdicmn.h:999-1013`; `src/common/gdicmn.cpp:516`, `:805-807` |
| `wxTransparentColour` = `wxColour(0,0,0,wxALPHA_TRANSPARENT)`: valid, black, alpha 0. `IsTransparent()/IsOpaque()/IsTranslucent()` are new in 3.3.1. `Alpha()` returns `wxALPHA_OPAQUE` "on platforms where alpha is not yet supported". | `include/wx/colour.h:31`; `interface/wx/colour.h:110-114`, `:260-282` |
| `GetLuminance()` = 0.299R + 0.587G + 0.114B on 0..1. | `interface/wx/colour.h:212-222` |
| `ChangeLightness(ialpha)`: 0 = black, 100 = unchanged, 200 = white; returns a copy. **[source]** values are clamped to 0..200 and the result is built as `wxColour(r,g,b)` — alpha is dropped. | `interface/wx/colour.h:365-377`; `src/common/colourcmn.cpp` `wxColourBase::ChangeLightness` |
| `MakeDisabled(brightness = 255)` "modifies the object in place and returns the object itself". **[source]** each channel becomes `brightness + 0.4·(c − brightness)` — it keeps 40% of the colour and moves 60% toward `brightness`, so on a dark background the default makes a "disabled" colour light. | `interface/wx/colour.h:336-342`; `src/common/colourcmn.cpp` `MakeDisabled`, `AlphaBlend` |

Alpha in window colours: **[source]** MSW brushes are `CreateSolidBrush(COLORREF)` (alpha dropped) and pen/brush
transparency is style-based, so a solid brush of a transparent colour paints **black** through GDI. Use
`*wxTRANSPARENT_BRUSH` or `wxGCDC`/`wxGraphicsContext` (`src/msw/brush.cpp:187`; `include/wx/brush.h:60-68`).
Background styles and transparent windows: see `references/painting-custom-widgets.md`.

**OrcaSlicer.** Build colours from hex strings or RGB triples, never from names. Text over user/filament
swatches is chosen by luminance (`clr.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK`, `wxExtensions.cpp` swatch
helpers, `PresetComboBoxes.cpp`). A packed `wxColour(0x…)` literal is harmless only for a symmetric grey
(`wxColour(0xEEEEEE)`); integer literals in `StateColor` contexts are RGB.

**Pitfalls**

- **Rule:** Never write a packed integer `wxColour`; keep `StateColor` integers as they are.
  **Why:** `wxColour(unsigned long)` is BGR; `StateColor(unsigned long)`/`append(unsigned long, …)` byte-swap the value
  so it is RGB (`Widgets/StateColor.cpp` `StateColor::append`). "Fixing" one to look like the other inverts R and B.
  ```cpp
  // Wrong: R=0x88 G=0x96 B=0x00 (olive), not #009688
  label->SetForegroundColour(wxColour(0x009688));
  // Right:
  label->SetForegroundColour(wxColour("#009688"));
  // Also right (StateColor integers are 0xRRGGBB, alpha byte 0 = opaque):
  box->SetBorderColor(StateColor(std::make_pair(0x009688, (int) StateColor::Hovered),
                                 std::make_pair(0xDBDBDB, (int) StateColor::Normal)));
  ```
  Cite: `interface/wx/colour.h:179-183`; `Widgets/StateColor.cpp` `StateColor::append(unsigned long, int)`.
- **Rule:** Validate colours from external data with `Set()`.
  **Why:** an invalid colour reaching a setter silently resets the window to its default colour.
  ```cpp
  // Wrong: wxColour c(user_str); win->SetBackgroundColour(c);
  // Right:
  wxColour c; if (!c.Set(user_str)) c = fallback; win->SetBackgroundColour(c);
  ```
  Cite: `include/wx/colour.h:235`; `interface/wx/window.h:2438-2439`.
- **Rule:** `ChangeLightness(80)` darkens by 20%; there are no negative arguments. On dark backgrounds pass a dark
  `brightness` to `MakeDisabled` or use a palette disabled colour (`#6B6B6B`/`#ACACAC`, mapped in dark).
  Cite: `interface/wx/colour.h:365-377`, `:336-342`.

## System colours and appearance

**Contract.** `wxSystemSettings::GetColour(index)`: the values "map 1:1 the native values supported by the Windows'
GetSysColor function. Note that other ports … usually map the same colour to various wxSYS_COLOUR_* values"; "The
returned colour is always valid" (`interface/wx/settings.h:44-48`, `:398-407`). New in 3.3.2:
`wxSYS_COLOUR_GRIDLINES`, `wxSYS_COLOUR_LISTBOXHIGHLIGHT` (`interface/wx/settings.h:120-141`). `wxSYS_COLOUR_FRAMEBK` = BTNFACE.

`wxSystemSettings::GetAppearance()` returns `wxSystemAppearance` (`interface/wx/settings.h:288-371`):

| Method | Contract |
|---|---|
| `IsDark()` | "checks the appearance of the current application and not the other applications on the system, so under MSW … will return false even if dark mode is used system-wide unless the application opted in using dark mode using wxApp::MSWEnableDarkMode()" (`:332-346`). An incompatible 3.3 change (`docs/changes.txt:71-73`). |
| `AreAppsDark()` (3.3.0) | system-wide app dark mode "even if it's not enabled for this particular application"; same as `IsDark()` off MSW (`:308-321`). |
| `IsSystemDark()` (3.3.0) | the "Windows mode", which can differ from the "app mode" (`:348-358`). |
| `IsUsingDarkBackground()` | luminance fallback, "generally not very useful to call directly" (`:360-370`). |
| `GetName()` | "only implemented for macOS", e.g. "NSAppearanceNameAqua"; empty elsewhere (`:323-330`). |
| `wxSystemSettings::SelectLightDark(light, dark)` (3.3.0) | "just a convenient helper using wxSystemAppearance::IsDark()" (`:460-473`); literally `GetAppearance().IsDark() ? dark : light` (`include/wx/settings.h:234-237`). |

**Platforms** (all **[source]**):

| Port | `IsDark()` | `GetColour()` |
|---|---|---|
| MSW | `wxMSWDarkMode::IsActive()` ‖ luminance fallback (`src/msw/settings.cpp:431-442`). Under `DarkMode_Auto`, `IsActive()` is uxtheme's `ShouldAppsUseDarkMode()` (`src/msw/darkmode.cpp` `ShouldUseDarkMode`) — the **system app mode**, not the app's own choice. `AreAppsDark()/IsSystemDark()` read `AppsUseLightTheme`/`SystemUsesLightTheme` from the registry. | `GetSysColor`, except GRIDLINES→BTNFACE, LISTBOXTEXT→WINDOWTEXT, LISTBOXHIGHLIGHT→HIGHLIGHT, LISTBOX→WINDOW, MENUBAR→MENU unless flat menus (`src/msw/settings.cpp:99-148`). LISTBOXHIGHLIGHTTEXT maps only to a raw index `GetSysColor` does not define — use HIGHLIGHTTEXT. **When wx dark mode is active every index is answered by `wxDarkModeSettings::GetColour()` first**; indices it leaves invalid (GRAYTEXT, 3DLIGHT, borders, …) fall back to the light `GetSysColor` value. |
| macOS | `[NSApp effectiveAppearance]` best match is DarkAqua (`src/osx/cocoa/settings.mm` `IsDark`). | WINDOW/LISTBOX = `controlBackgroundColor`, BTNFACE = `windowBackgroundColor` (≥10.14), caption/border/MENU/MENUBAR = `windowFrameColor`, BTNTEXT/WINDOWTEXT/MENUTEXT/CAPTIONTEXT/INACTIVECAPTIONTEXT/INFOTEXT/LISTBOXTEXT = `controlTextColor` (GRAYTEXT = `disabledControlTextColor`, HIGHLIGHTTEXT = `selectedTextColor`), GRIDLINES = `gridColor`, INFOBK/APPWORKSPACE = `windowBackgroundColor` (commented "bogus"), HOTLIGHT = `linkColor` (`src/osx/cocoa/settings.mm` `GetColour`). The result wraps a **dynamic NSColor**: components resolve at call time under the effective appearance (`src/osx/cocoa/colour.mm`), and as a view background it adapts by itself. |
| GTK3 (the default Linux build) | `IsUsingDarkBackground()`: luminance(WINDOWTEXT) − luminance(WINDOW) > 0.2 (`src/common/settcmn.cpp:96-112`); `AreAppsDark()/IsSystemDark()` = `IsDark()` (`:71-84`). wxGTK3 follows the freedesktop portal `org.freedesktop.appearance` `color-scheme` (GNOME's dark style) by setting `gtk-application-prefer-dark-theme` and stripping a `-dark`/`-Dark` theme-name suffix; no portal when `GTK_THEME` is set (`src/gtk/settings.cpp:251-334`, `:369-400`, `:1426-1450`). | From synthetic `GtkStyleContext`s (button, textview, treeview, headerbar, tooltip, menu), **cached** in `gs_systemColorCache` until "notify::gtk-theme-name" or a colour-scheme change (`src/gtk/settings.cpp:728-880`). |
| GTK2 (opt-out build, `-DDEP_WX_GTK3=OFF`) | as GTK3 (luminance of the GTK2 theme); no portal/colour-scheme support (`#ifdef __WXGTK3__`). | `GtkStyle` of helper widgets (`src/gtk/settings.cpp:913ff`). |
| Wayland | no Wayland-specific branch in colour/appearance code. | — |

MSW dark palette (`wxDarkModeSettings::GetColour`, `src/msw/darkmode.cpp:300-370`; "not documented and are subject
to change", `interface/wx/msw/darkmode.h:74-84`): WINDOW/LISTBOX/INFOBK/APPWORKSPACE/ACTIVECAPTION `0x202020`;
the *TEXT indices `0xe0e0e0` except INACTIVECAPTIONTEXT `0xaaaaaa` (GRAYTEXT is left invalid); BTNFACE/GRIDLINES `0x333333`; MENU/INACTIVECAPTION `0x2b2b2b`; MENUBAR/LISTBOXHIGHLIGHT
`0x626262`; HIGHLIGHT/MENUHILIGHT `0x9e5315` (a blue — packed BGR); HOTLIGHT `0xe48435`.

**OrcaSlicer.** Orca does not theme with system colours or with `wxVisualAttributes`/`GetClassDefaultAttributes()`
(the stock advice for matching native controls): it uses a fixed palette with a hand-written dark twin map
(`StateColor`), because the exact-match map needs deterministic RGB that dynamic macOS colours and the MSW dark
palette do not give, and because Windows needs an app-level live toggle wx cannot do (see
[wxMSW dark mode](#wxmsw-dark-mode-mswenabledarkmode-setappearance-wxdarkmodesettings)). `GUI_App::init_label_colours`
still reads `wxSYS_COLOUR_WINDOWTEXT`/`wxSYS_COLOUR_WINDOW` for two light-mode values — on Windows with a dark system
and Orca light those come from wx's dark palette. macOS: Orca's wx is patched to read NSColor components in sRGB
instead of `NSCalibratedRGBColorSpace` (`deps/wxWidgets/0001-macos-use-srgb-colour-components.patch`, commit
a7775296b0 "Fix macOS custom color accuracy"), so RGB read back from native colours matches the expected hex.

**Pitfalls**

- **Rule:** Never branch on wx's appearance in Orca GUI code.
  **Why:** Orca calls `MSWEnableDarkMode(DarkMode_Auto)`, so on Windows `IsDark()`/`SelectLightDark()` follow the
  system app mode — Windows dark + Orca light reports dark.
  ```cpp
  // Wrong:
  auto c = wxSystemSettings::SelectLightDark(wxColour("#FFFFFF"), wxColour("#2D2D31"));
  // Right:
  auto c = StateColor::darkModeColorFor(wxColour("#FFFFFF"));            // #FFFFFF is a gDarkColors key
  auto d = wxGetApp().dark_mode() ? wxColour("#EFEFF0") : wxColour("#333333"); // unmapped colour
  ```
  Cite: `src/msw/settings.cpp:431-442`; `src/msw/darkmode.cpp` `ShouldUseDarkMode`; 7d7f26ed69.
- **Rule:** Theme with palette colours, not `wxSystemSettings::GetColour(wxSYS_COLOUR_*)`.
  **Why:** MSW returns wx's dark palette whenever the system app mode is dark, regardless of Orca's setting
  (`src/msw/settings.cpp:99-107`); on macOS the value is a dynamic colour no `gDarkColors` key matches; on GTK it
  is whatever the theme says. Do not cache macOS system-colour RGB across a theme change.
- **Rule:** Raw `wxButton` code that branches on `__WXMAC__` to `wxSYS_COLOUR_BTNFACE`/`BTNTEXT` only keeps the text
  in the system colour: NSButton ignores the background colour anyway (see
  [native limits](#window-colours-inheritance-and-native-control-limits)). New code uses Orca `Button`.

## wxEVT_SYS_COLOUR_CHANGED

**Contract.** "generated when the user changes the colour settings or when the system theme changes (e.g. automatic
dark mode switching on macOS)". "The default event handler for this event propagates the event to child windows,
since the system events are only sent to top-level windows. If intercepting this event for a top-level window,
remember to either call wxEvent::Skip() on the event, call the base class handler, or pass the event on to the
window's children explicitly" (`interface/wx/event.h:1942-1966`). **[source]** the default handler
(`wxWindowBase::OnSysColourChanged`, `src/common/wincmn.cpp:3011-3027`) sends a fresh event to every
**non-top-level** child and calls `Refresh()`; a child whose own handler does not `Skip()` stops it for its subtree.

**Who sends it** (all **[source]**):

| Port | Trigger |
|---|---|
| MSW | `WM_SYSCOLORCHANGE`, and `WM_SETTINGCHANGE` with "ImmersiveColorSet" (light/dark/accent switch) → `HandleSysColorChange()` on each TLW (`src/msw/window.cpp:3542`, `:5092`, `:5197-5200`). The default MSW handler re-sends a real `WM_SYSCOLORCHANGE` to native children (`wxWindowMSW::OnSysColourChanged`, `:5269-5293`); `wxFrame` also resets its background to `wxSYS_COLOUR_APPWORKSPACE` if `!UseBgCol()` and re-themes the menubar (`src/msw/frame.cpp:476-503`). |
| macOS | per-NSWindow KVO on `effectiveAppearance`, **and** `windowDidChangeBackingProperties` when the window's colour space changes (dragging to a display with another profile) — no theme change involved (`src/osx/cocoa/nonownedwnd.mm:684-740`). Popups are `wxNonOwnedWindow`s and get it too. No defined order across windows. |
| GTK3 | every TLW connects "notify::gtk-theme-name" with `g_signal_connect_after` so the colour cache is cleared before user handlers run (`src/gtk/toplevel.cpp:555-563`, `:951-955`); the portal colour-scheme handler `DoUpdateColorScheme` also loops over `wxTopLevelWindows` (`src/gtk/settings.cpp:251-334`). A dark switch that also renames the theme delivers the event **twice** per TLW. |
| GTK2 (opt-out build) | "notify::gtk-theme-name" only. |

`wxDialogBase::OnSysColourChanged` exists (`src/common/dlgcmn.cpp:561`) but no event table references it.

**Usage.**
```cpp
Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
    e.Skip();      // keep propagation to children (and the wxFrame/wxWindowMSW base work)
    recolor();     // idempotent and cheap: may fire twice (GTK3) or without any theme change (macOS)
});
```

**OrcaSlicer.** `DPIAware<P>` (`GUI_Utils.hpp`) binds the event on every platform:
- macOS/Linux: `update_dark_config()` (writes `dark_color_mode` from `GetAppearance().IsDark()` and calls
  `Update_dark_mode_flag()`), then the virtual `on_sys_color_changed()`, then `Skip()`.
- Windows: the body is empty and deliberately does not `Skip()` ("Not calling Skip() is what stops the event
  propagating on Windows"). That consumes the event at every DPIAware TLW, so wx's
  `wxWindowMSW::OnSysColourChanged` and every child handler (Plater's included) never run there. On Windows the
  theme switch comes only from Preferences ([runtime switch](#runtime-theme-switch-and-re-applying-colours)).

Elsewhere containers that bind the event without `Skip()` stop it for their subtree (`ButtonsListCtrl` in
`Notebook.cpp` binds an empty handler; `Plater::priv::on_apple_change_color_mode` updates the GL canvases and does
not skip). Long-lived UI is therefore refreshed through the `MainFrame::on_sys_color_changed` fan-out, not by
child handlers.

**Pitfalls**

- **Rule:** A TLW or container handler calls `Skip()`.
  **Why:** without it wx's default handler never propagates to the children (`interface/wx/event.h:1951-1956`).
  Cite: `src/common/wincmn.cpp:3011-3027`.
- **Rule:** Make the handler idempotent and cheap; never assume one event per theme change.
  **Why:** macOS fires on display colour-space changes; GTK3 can fire twice per TLW.
- **Rule:** Never put Orca re-theming in a child's SYS_COLOUR handler; implement `on_sys_color_changed()` /
  `sys_color_changed()` and make sure the owner calls it.
  **Why:** on Windows the event never reaches children; elsewhere any non-skipping ancestor swallows it.

## wxMSW dark mode: MSWEnableDarkMode, SetAppearance, wxDarkModeSettings

**Contract.** `bool wxApp::MSWEnableDarkMode(int flags = 0, wxDarkModeSettings* settings = nullptr)` (3.3.0,
`@onlyfor{wxmsw}`, `interface/wx/app.h:1418-1465`):
- "experimental"; uses "undocumented, and unsupported by Microsoft, functions"; works on Windows 10 later than
  v1809 (including LTSC 2019) and all Windows 11; testing before 20H1 (v2004) "has been limited".
- Flags: default follows the system ("dark mode is only used if it is the default mode for the applications on
  the current system"); `DarkMode_Always` forces dark. **[source]** `DarkMode_Auto = 0` exists in
  `include/wx/msw/app.h:48` although only `DarkMode_Always` is documented.
- Returns `true` if enabled, `false` "most likely because the system doesn't support dark mode".
- Alternatives: the `msw.dark-mode` system option (1 = `MSWEnableDarkMode()`, 2 = `DarkMode_Always`, settable by
  environment variable from outside the app, `interface/wx/sysopt.h:85-89`), or `SetAppearance(System|Dark)`.
- Known limitations (`interface/wx/app.h:1434-1448`): anything `TaskDialog()`-based has no dark mode —
  `wxMessageBox()`, `wxMessageDialog`, `wxRichMessageDialog`, `wxProgressDialog`, simple `wxAboutBox()` (wx suggests
  `wxGenericMessageDialog`/`wxGenericProgressDialog`); common-dialog wrappers `wxColourDialog`, `wxFindReplaceDialog`,
  `wxFontDialog`, `wxPageSetupDialog`, `wxPrintDialog`; `wxTimePickerCtrl`, `wxDatePickerCtrl`, `wxCalendarCtrl`
  stay light; toolbar items with `wxToolBar::SetDropdownMenu()` draw the drop-down "almost invisible".

`AppearanceResult wxApp::SetAppearance(Appearance::System|Light|Dark)` (3.3.0, `interface/wx/app.h:1152-1190`):
GTK/macOS follow the system by default and the call is immediate and "affects all the existing windows as well
as any windows created after this call"; "Under MSW, the default appearance is always light" and an app that
wants to follow the system must call it with `Appearance::System`; "the appearance can be only set before any
windows are created and calling this function too late will return AppearanceResult::CannotChange" (only wxMSW
returns it);
`Failure` e.g. "because `GTK_THEME` is defined".

`wxDarkModeSettings` (`interface/wx/msw/darkmode.h:30-111`), passed to `MSWEnableDarkMode()`: `GetColour(wxSystemColour)`
(defaults "not documented and are subject to change"; the doc example names `0x202020` as the default background);
`GetMenuColour(wxMenuColour)` — menu-bar colours match no `wxSystemColour`, affect top-level menus only (items use
`wxOwnerDrawn::SetTextColour()`), "must be valid"; `GetBorderPen()` — invalid pen = system `wxStaticBox` border,
which "doesn't look very well in dark mode"; the base returns grey.

**[source] facts the docs do not state:**
- `SetAppearance` on MSW returns `CannotChange` when any TLW exists **or `MSWEnableDarkMode` was already called**
  (`gs_appMode != AppMode_Default`); `SetAppearance(Light)` returns `Ok` without doing anything
  (`src/msw/darkmode.cpp` `wxApp::SetAppearance`). macOS `System` sets `NSApp.appearance` to
  `[NSAppearance currentAppearance]` rather than nil, pinning the current look (`src/osx/cocoa/utils.mm:485-513`);
  GTK3 maps to the portal colour-scheme machinery; GTK2 always returns `Failure` (`src/gtk/app.cpp:355-381`).
- `MSWEnableDarkMode` may be called any time, but dark title bars are applied only to TLWs **created** afterwards
  (`EnableForTLW` at the end of TLW creation, `src/msw/toplevel.cpp:513`) and controls are dark-enabled at creation
  (`MSWCreateControl` → `AllowForWindow` and, for some controls, `SetForegroundColour(LISTBOXTEXT)`,
  `src/msw/control.cpp:133-140`). wx cannot flip existing windows — the reason `SetAppearance` refuses late calls.
- wx takes ownership of the settings pointer (`wxDarkModeModule::SetSettings`) — allocate it with `new`.
- `msw.dark-mode` is read in `wxApp::Initialize` (`src/msw/app.cpp:492-494`): a user's `WX_MSW_DARK_MODE`
  environment variable enables wx dark mode before Orca's own call.
- wx's owner-drawn menu path keys on `wxMSWDarkMode::IsActive()` (`src/msw/menuitem.cpp` `wxMenuItem::OnDrawItem`,
  `GetColourToUse`; `src/msw/menu.cpp`; menu-bar UAH drawing in `src/msw/darkmode.cpp`).

3.3.x dark-mode fix log: 3.3.2 wxMSW (`docs/changes.txt:297-308`: checkbox accessibility in dark mode, rendering of
several controls, toolbar, menus; also "Revert use of WS_EX_COMPOSITED"), 3.3.1 (`:358-372`: wxStaticBitmap-in-notebook
crash, disabled wxButton bitmaps and wxStaticText, notebook high-contrast background, wxDataViewCtrl light-mode
border regression, selected toolbar buttons, wxComboCtrl, wxTE_RICH wxTextCtrl), 3.3.0 (`:385` "Add experimental dark
mode support to wxMSW"); XRC dark colour variants (`:494`).

**OrcaSlicer.** `GUI_App::on_init_inner` (`#ifdef __WINDOWS__`) calls `MSWEnableDarkMode(DarkMode_Auto)` and then
`NppDarkMode::InitDarkMode(init_dark_color_mode, init_sys_menu_enabled)`. wx's call exists only so that wx-drawn
menus get dark borders; NppDarkMode does the theming (title bars, explorer theme, scrollbars, list headers) because
it can switch live and wx cannot. The code comment "Orca: todo switch to native dark mode support in wxWidgets and
remove NppDarkMode" records the intent; the blocker is that a live Preferences toggle would become restart-only
(`SetAppearance` returns `CannotChange` after startup, existing windows cannot be restyled) and TaskDialog/common
dialogs/pickers stay light anyway — which is also why Orca has its own `MsgDialog` family and `ProgressDialog`.
**[source]** consequences of `DarkMode_Auto`: wx's internal "dark" stays `AllowDark` and follows the **system app
mode**; NppDarkMode's later `SetPreferredAppMode` call does not touch wx's `gs_appMode`. So with Windows light +
Orca dark, wx's dark machinery (menus, `GetColour()`, `IsDark()`) stays off; with Windows dark + Orca light it is
on: `GetColour()` returns the dark palette, new native controls are dark-enabled at creation, `IsDark()` is true.

**Pitfalls**

- **Rule:** Call `MSWEnableDarkMode(DarkMode_Auto)` before `NppDarkMode::InitDarkMode(...)`.
  **Why:** wx 3.3 draws some native chrome (context/file menus) itself; without wx dark mode those menus keep a
  light white border in dark mode. **[source]** both calls end in the same undocumented uxtheme ordinal 135
  (`SetPreferredAppMode`; wx `src/msw/darkmode.cpp` `InitDarkMode`, Orca `dark_mode/dark_mode.hpp`
  `AllowDarkModeForApp`) and the last call wins at OS level: NppDarkMode then sets **ForceDark or ForceLight** per
  Orca's setting, overriding wx's AllowDark in both directions. In the other order wx's AllowDark would replace
  Orca's forced mode. wx's own state still follows the system apps-dark setting; the two coincide only when the
  system theme matches what Orca forces.
  ```cpp
  // Wrong: only NppDarkMode knows about dark mode; wx-drawn menus stay light-bordered
  NppDarkMode::InitDarkMode(init_dark, sys_menu);
  // Right:
  MSWEnableDarkMode(DarkMode_Auto);               // enable wx 3.3's dark machinery
  NppDarkMode::InitDarkMode(init_dark, sys_menu); // then ForceDark/ForceLight overrides AllowDark
  ```
  Cite: bf397a0632 (`GUI_App.cpp`, `GUI_App::on_init_inner`).
- **Rule:** Do not call `wxTheApp->SetAppearance(...)` in Orca.
  **Why:** MSW returns `CannotChange` (Orca already called `MSWEnableDarkMode`), GTK2 fails, and macOS pins
  `NSApp.appearance` while `GUI_App::dark_mode()` keeps reading the system `AppleInterfaceStyle` — the two would
  disagree.
- **Rule:** Do not show wx's TaskDialog-based or common dialogs where dark mode matters; use `MessageDialog` and
  friends (`MsgDialog.hpp`, see `references/windows-dialogs.md`).

## Window colours, inheritance and native-control limits

**Contract** (`interface/wx/window.h`):

| Call | Effect |
|---|---|
| `SetBackgroundColour(c)` | "may not affect the entire control and could be not supported at all depending on the control and platform"; does not refresh ("you may wish to call wxWindow::ClearBackground or wxWindow::Refresh"); "will disable attempts to use themes for this window"; returns `false` if the colour was already set (`:2427-2461`). |
| `SetOwnBackgroundColour(c)` | same, "but prevents it from being inherited by the children" (`:2588-2593`). |
| `SetForegroundColour(c)` | "not all native controls support changing their foreground colour so this method may change their colour only partially or even not at all" (`:2561-2585`). |
| `SetOwnForegroundColour(c)` | non-inheritable foreground (`:2622-2627`). |
| `UseBgCol()`/`UseBackgroundColour()`, `UseForegroundColour()` | whether a colour was set for this window (`:2600-2632`). |
| `InheritsBackgroundColour()`/`InheritsForegroundColour()` | the inheritable flag (`:2595-2639`). |
| `ShouldInheritColours()` | "base class version returns false, but … overridden in wxControl where it returns true" (`:2645-2653`). |
| `InheritAttributes()` | called during creation; a child takes an attribute only if the parent set it explicitly (not via `SetOwn*`) and the child did not (`:4052-4075`). |
| `GetClassDefaultAttributes(variant)` → `wxVisualAttributes{font, colFg, colBg}` | "colBg may be wxNullColour if the controls background colour is not solid"; "All of them may be invalid if it was not possible to determine the default control appearance" (`:135-150`, `:4245-4273`). |

**[source] facts the docs do not spell out** (`src/common/wincmn.cpp`):
- **Background is never inherited.** The background branch of `InheritAttributes()` is `#if 0` ("inheriting (solid)
  background colour is wrong as it totally breaks any kind of themed backgrounds", `:1524-1553`). Only font and
  foreground are copied, only at creation time (`InheritAttributes()` runs from the ports' creation code), only
  from the **immediate** parent.
- `ShouldInheritColours()` is false for `wxWindow`/`wxPanel` (`include/wx/window.h:1682`), `wxAnyButton`
  (`include/wx/anybutton.h:105`), `wxTextCtrl` (`include/wx/textctrl.h:872`), `wxControlWithItems` (`wxChoice`,
  `wxListBox`, …; `include/wx/ctrlsub.h:447`), `wxTreeCtrl` (`include/wx/treectrl.h:399`); true for other
  `wxControl`s (`wxStaticText`, `wxCheckBox`, …; `include/wx/control.h:98`).
  So `panel->SetForegroundColour(x)` reaches only static-text-like direct children created afterwards.
- `GetBackgroundColour()`/`GetForegroundColour()` **never return an invalid colour**: unset, they return
  `GetDefaultAttributes()`, falling back to `GetClassDefaultAttributes()` = `wxSYS_COLOUR_BTNFACE` /
  `wxSYS_COLOUR_WINDOWTEXT` (`:1556-1605`). Test `UseBgCol()` to know whether a colour was really set.
- `SetBackgroundColour`/`SetForegroundColour` call `SetThemeEnabled(!hasBg && !fg.IsOk())` /
  `SetThemeEnabled(!hasFg && !bg.IsOk())`: once either colour is set, theming is off (`:1640-1661`).

**How children still look like their parent** (visual inheritance ≠ `GetBackgroundColour()`; **[source]**):
- MSW: a child without its own background paints the nearest ancestor's **explicit** brush while
  `HasTransparentBackground()` holds (`src/msw/window.cpp` `wxWindowMSW::MSWGetBgBrush`). Containers (`wxPanel`)
  report transparent if an ancestor has an inheritable background (`SetBackgroundColour`, not
  `SetOwnBackgroundColour`; `src/common/containr.cpp:162-175`); `wxStaticText`, `wxCheckBox`, `wxStaticBox`,
  `wxStaticBitmap`, `wxHyperlinkCtrl`, book controls, MSW `wxRadioButton`/`wxSlider` always do.
- macOS: a bare window with the default `wxBG_STYLE_ERASE` is cleared with its **own** `GetBackgroundColour()`
  (`wxWindowMac::MacDoRedraw`, `src/osx/window_osx.cpp:1950-1983`; the `wxWindowDC` background is
  `GetBackgroundColour()`, `src/osx/carbon/dcclient.cpp:89`) — the class default `windowBackgroundColor`, a dynamic
  system grey.
- GTK3: a bare `wxPanel` is theme-enabled (`src/common/panelcmn.cpp:100`) and renders its own GTK style-context
  background (`gtk_render_background` in `wxWindowGTK::GTKSendPaintEvents`, `src/gtk/window.cpp`), which depends on
  the theme.

This is why "dark-theme bugs that only show on macOS" exist: MSW hides an unset panel behind the ancestor's brush,
macOS paints the system grey.

**Native controls that ignore colours** (**[source]**):

| Port | Behaviour |
|---|---|
| MSW | `wxButton`/`wxToggleButton` `SetBackgroundColour`/`SetForegroundColour` switch the native button to `BS_OWNERDRAW` (`src/msw/anybutton.cpp` `wxAnyButton::MakeOwnerDrawn`, `:1324-1390`) — the colour works but native theming is gone. `wxCheckBox`/`wxRadioButton` foreground makes them owner-drawn when themes are active (`src/msw/control.cpp` `MSWMakeOwnerDrawnIfNecessary`); 3.3.2 fixed checkbox accessibility in that mode (`docs/changes.txt:299-300`). Static-text-like children paint the ancestor brush only if that ancestor's background is inheritable. |
| macOS | `SetBackgroundColour` reaches the NSView only if it `respondsToSelector:setBackgroundColor:` and the style is not `wxBG_STYLE_TRANSPARENT` (`src/osx/cocoa/window.mm:3514-3531`): **NSButton (`wxButton`) ignores the background colour**. Foreground uses `setTextColor:` when the view has it (`:3885-3887`); stock NSButton has none. `wxStaticText` is an NSTextField with `setDrawsBackground:NO` — its background is never painted (`src/osx/cocoa/stattext.mm:154`). `wxTextCtrl`'s `setEnabled:` resets the text colour: multi-line (`wxNSTextView`) always, to `controlTextColor`/`disabledControlTextColor`; single-line (`wxNSTextField`) when it does not draw its background, to `controlTextColor`/`secondarySelectedControlColor` (`src/osx/cocoa/textctrl.mm`). |
| GTK3 | `SetBackgroundColour/SetForegroundColour/SetFont` install a per-widget CSS provider `*{color:..;background:..;font:..}` at `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION` (`src/gtk/window.cpp` `wxWindowGTK::GTKApplyStyle`); `wxButton`/`wxCheckBox` apply it to their inner label too (`src/gtk/button.cpp:325-335`, `src/gtk/checkbox.cpp:233-237`). User CSS (priority USER) can still override. |
| GTK2 | colours applied with `gtk_widget_modify_style` — pixmap-engine themes may ignore them. |

**OrcaSlicer — `Utils/MacDarkMode.mm`** installs process-wide Objective-C categories/swizzles that affect *every*
window, so stock-wx advice about NSTextField/NSButton colours differs in Orca:
- every `NSTextField` is created with `drawsBackground = false` (category `NSTextField (drawsBackground)`), so with
  the `textctrl.mm` rule above a custom text colour is lost on every `Enable()` and the background is not painted;
- `NSButton (NSButton_Extended)` adds `textColor`/`setTextColor:` (via the attributed title), which is what makes
  `wxButton`/`wxCheckBox` foreground colours work on macOS; `setBezelStyle:` forces bordered (except shadowless
  square); focus rings are off for NSButton and NSTextField; `NSTableHeaderCell`'s font is forced to `Label::sysFont(13)`;
- the main-frame title text colour is forced (`set_title_colour_after_set_title`; `NSTextField (textColor)` swizzle
  for that one field), and `set_miniaturizable` makes the titlebar transparent over a dark NSWindow background.

Orca's widgets work with these limits: `Label` sets keyed colours (`SetForegroundColour("#262E30")`, background =
`StaticBox::GetParentBackgroundColor(parent)`) so the walk can map them — commit d408db2fde replaced `wxStaticText`
with `Label` to fix Linux dark mode this way; `TextInput::Enable` re-applies the inner control's background and
foreground from its `StateColor`s after enabling; `StaticBox::Create` copies the parent background into its own wx
background (a `StaticBox` parent's `background_color.defaultColor()`, the midpoint of a gradient, else
`parent->GetBackgroundColour()`), used for the corners outside the rounded rectangle.

**Pitfalls**

- **Rule:** Give every `wxPanel`/`wxScrolledWindow` you create an explicit palette background (typically
  `SetBackgroundColour(*wxWHITE)`), even when white "already looks right".
  **Why:** the walk re-keys a background only if it is a `gDarkColors` key. A bare panel paints its class default
  (`wxSYS_COLOUR_BTNFACE`; on macOS the appearance-dependent `windowBackgroundColor`), which is never a key, so it
  shows as a system-grey patch against Orca's palette in either theme. MSW hides this by painting the ancestor's
  explicit brush, so it surfaces on macOS. An explicit palette colour is the key that lets the walk re-map it in
  both directions.
  ```cpp
  wxPanel* p = new wxPanel(parent);   // Wrong: unkeyed; macOS shows system grey, not #2D2D31 / #FFFFFF
  wxPanel* p = new wxPanel(parent);
  p->SetBackgroundColour(*wxWHITE);   // Right: #FFFFFF is a key → #2D2D31 in dark
  ```
  Cite: f7f0c82abb / 90ecdb8f03 (`HMSPanel.cpp`, `StatusPanel.cpp`, `UpgradePanel.cpp`,
  `DeviceTab/wgtDeviceNozzleRack*.cpp`, `SelectMachine.cpp`); `src/common/wincmn.cpp:1524-1553`.
- **Rule:** Set the parent's background before creating Orca widgets in it.
  **Why:** `StaticBox`, `Label`, `CheckBox`, `SwitchButton`, `RadioGroup` snapshot the parent background at construction; a later
  change leaves wrong-coloured corners/label backgrounds.
- **Rule:** Set colours on the control itself, not on an ancestor.
  **Why:** background never inherits; foreground inherits only at creation, from the immediate parent, and not for
  `wxPanel`, `wxTextCtrl`, buttons or item controls.
  ```cpp
  // Wrong: dlg->SetForegroundColour(c);   // after children exist, or on a wxPanel above them
  // Right: label->SetForegroundColour(c);  // per control
  ```
- **Rule:** Re-apply text-control colours after every `Enable()`.
  **Why:** macOS NSTextField/NSTextView reset their text colour in `setEnabled:`, and Orca's swizzle makes every
  NSTextField non-background-drawing. `TextInput::Enable` is the model.
- **Rule:** Never style a native `wxButton` with `SetBackgroundColour`; use `Button` + `SetStyle(...)`.
  **Why:** MSW turns the native button owner-drawn (theming gone); macOS ignores the background colour.

## StateColor

`StateColor` (`Widgets/StateColor.hpp/.cpp`) is an ordered list of `(wxColour, int mask)` pairs plus the global
light→dark map `gDarkColors`. Orca widgets (`StaticBox` and subclasses) take `StateColor`s and resolve them at paint
time.

**Contract** (from the code):
- State bits: `Normal = 0`, `Enabled = 1`, `Checked = 2`, `Focused = 4`, `Hovered = 8`, `Pressed = 16`; the negations
  `Disabled`, `NotChecked`, `NotFocused`, `NotHovered`, `NotPressed` are the same bits `<< 16` and mean "bit must be
  off". Masks OR together (`StateColor::Checked | StateColor::Enabled`). The `(int)` cast on the enum is required
  for `std::pair` deduction.
- `colorForStates(states)` returns the **first** entry whose on-bits are all set and off-bits all clear;
  `takeFocusedAsHovered_` (default true) lets a `Hovered` entry also match `Focused` (`setTakeFocusedAsHovered(false)`
  to style focus separately). No match → `wxColour(0, 0, 0, 0)` (transparent black — black through GDI).
- `colorForStates()` passes the result through the dark map when StateColor's `gDarkMode` is set, **at paint time**:
  owner-drawn widgets switch theme on `Refresh()` with no extra code. `colorForStatesNoDark()` skips mapping;
  `defaultColor()` = `colorForStates(0)`.
- Single-colour ctors `StateColor(wxColour)`, `(wxString)`, `(unsigned long)` store a `Normal` entry. Integer forms
  (`StateColor(unsigned long)`, `append(unsigned long, int)`, `std::make_pair(0x009688, …)`) are **`0xRRGGBB`**; an
  alpha byte of 0 is treated as opaque — the opposite of `wxColour(unsigned long)`.
- `gDarkColors` is a `std::map` ordered by `wxColour::GetRGBA()`: an **exact RGBA** match. Non-opaque or near-miss
  colours never map. Examples: `#FFFFFF→#2D2D31`, `#009688→#00675b`, `#262E30→#EFEFF0`, `#DFDFDF→#3E3E45`,
  `#D4D4D4→#4D4D54`, `#DBDBDB→#4A4A51`, `#000000→#FFFFFE`, `#F8F8F8→#36363C`, `#F1F1F1→#36363B`, `#EEEEEE→#4C4C55`,
  `#6B6B6B→#818183`, `#ACACAC→#65656A`, `#363636→#B2B3B5`, `#F0F0F1→#333337`. Near-white variants (`#FFFFFE` as the
  twin of `#000000`, `#FEFFFF`, `#FFFEFE`) exist so the reverse map stays usable. `#F0F0F0`, `#333333`, `#5C5C5C`
  are not keys.
- Static helpers: `SetDarkMode`, `darkModeColorFor` (returns its input unless `gDarkMode` is set), `lightModeColorFor`
  (the reverse map, built once with `emplace` so the smallest `GetRGBA()` key wins; not gated by `gDarkMode` — it
  maps in either mode), `GetDarkMap()`, LAB math
  `GetLAB`, `GetLightness`, `SetLightness`, `LightenDarkenColor`, `GetColorDifference`/`LAB_Delta_E`.

**Reverse-map and chaining hazards** (exact consequences of the table):
- duplicate dark targets collapse: `#E8E8E8 → #3E3E45 → #DFDFDF`; `#EDFAF2 → #283232 → #E5F0EE`;
- legitimate light colours that equal some dark twin are rewritten by the light walk: `#909090 → #6B6A6A`,
  `#D9D9D9 → #FFFEFE`, `#808080 → #2B3436`, `#FFFFFE → #000000`;
- the dark map chains when a value is also a key: a second dark pass takes `#FFFEFE → #D9D9D9 → #27272A`.

**Usage.**
```cpp
StateColor bg(std::pair{wxColour("#DFDFDF"), (int) StateColor::Disabled},
              std::pair{wxColour("#D4D4D4"), (int) StateColor::Pressed},
              std::pair{wxColour("#D4D4D4"), (int) StateColor::Hovered},
              std::pair{wxColour("#FFFFFF"), (int) StateColor::Normal});   // Normal LAST
box->SetBackgroundColor(bg);   // StaticBox API ("Color"), not wxWindow::SetBackgroundColour
// in doRender(wxDC& dc): dc.SetBrush(background_color.colorForStates(state_handler.states()));
```
How a widget attaches and repaints its `StateColor`s (`StateHandler`, `doRender`) is in
`references/painting-custom-widgets.md`; per-widget colour setters are in `references/orca-widgets.md`.

**OrcaSlicer.** Pick existing keys for new UI. `HyperLink` deliberately uses `#009687` (not a key) so its teal is
*not* mapped. `Button::SetStyle` detects dark mode with `darkModeColorFor("#FFFFFF") != "#FFFFFF"` for the focus
border, and `Button::Rescale()` re-runs `SetStyle` (when a style was set), so calling `Rescale()` on a theme change
refreshes it. A `Button` never given a style keeps its constructor colours, whose `*wxLIGHT_GREY` hover/disabled
entries are not keys — always call `SetStyle()`.
`DialogButtons` sets its own background with `darkModeColorFor(wxColour("#FFFFFF"))`. Widgets that take a plain
`wxColour` (e.g. `ProgressBar`) paint it unmapped — map it yourself. `StaticLine` maps line/text colours at paint time.

**Pitfalls**

- **Rule:** Most specific entries first, `Normal` last.
  **Why:** `Normal` (mask 0) matches every state, so entries after it are dead. Check the order of any table you copy;
  existing tables are not guaranteed correct (an `Enabled` entry after `Normal` never matches).
  ```cpp
  // Wrong: hover never shows
  StateColor c(std::pair{wxColour("#FFFFFF"), (int) StateColor::Normal},
               std::pair{wxColour("#D4D4D4"), (int) StateColor::Hovered});
  // Right:
  StateColor c(std::pair{wxColour("#D4D4D4"), (int) StateColor::Hovered},
               std::pair{wxColour("#FFFFFF"), (int) StateColor::Normal});
  ```
  Cite: `Widgets/StateColor.cpp` `StateColor::colorForStates`.
- **Rule:** Negate with the `Not*` enumerators, never `~`.
  **Why:** `(int) Hovered | NotFocused` = `0x40008` means "hovered and not focused"; `(int) Hovered | ~Focused` =
  `0xFFFFFFFB` sets every on-bit and never matches.
- **Rule:** Do not pass data colours (filament/extruder/user colours) through `darkModeColorFor` or leave them as
  window backgrounds the walk visits; paint them in a paint handler or re-set them after the walk.
  **Why:** a white filament `#FFFFFF` maps to `#2D2D31`.
- **Rule:** Never use a dark-twin value as a light-mode colour (`#FFFFFE`, `#D9D9D9`, `#909090`, `#808080`, …).
  **Why:** the light walk rewrites it through the reverse map.

## Orca dark-mode state

| State | Set by | Read by |
|---|---|---|
| app_config `dark_color_mode` ("1"/"0") | Windows: the Preferences checkbox (`PreferencesDialog::create_item_darkmode`, `#ifdef _WIN32`); `AppConfig::set_defaults` sets `"0"` when empty (`#ifdef _WIN32`). macOS/Linux: overwritten from `GetAppearance().IsDark()` at startup (`GUI_App::on_init_inner`, `#ifndef __WINDOWS__`) and by `update_dark_config()` on every SYS_COLOUR event. | `GUI_App::dark_mode()` (non-macOS); `Plater::priv::on_change_color_mode` and other GL/web code that reads the key directly — new code calls `dark_mode()` instead |
| `GUI_App::m_is_dark_mode` | `GUI_App::Update_dark_mode_flag()` (= `dark_mode()`) | `GUI_App::UpdateDarkUI` (the walk) only |
| `StateColor` file-static `gDarkMode` | `StateColor::SetDarkMode`, called only from `GUI_App::init_label_colours()` | `darkModeColorFor`, `colorForStates` |
| NppDarkMode `g_darkModeEnabled` (Windows) | `NppDarkMode::InitDarkMode` / `SetDarkMode` | title bars, explorer theme, scrollbars, DVC header |
| wx's own MSW mode | `MSWEnableDarkMode(DarkMode_Auto)` → follows the **system app mode** | wx internals, `IsDark()`, `GetColour()` |

`GUI_App::dark_mode()` (static, recomputed every call) is the only query GUI code uses:
- macOS: `wxPlatformInfo::Get().CheckOSVersion(10, 14) && mac_dark_mode()` (10.12/10.13 gave false positives);
  `mac_dark_mode()` (`Utils/MacDarkMode.mm`) reads the `AppleInterfaceStyle` user default == "Dark" — the **system**
  preference. The config key is ignored. wx's `IsDark()` (which feeds `update_dark_config()`) reads
  `[NSApp effectiveAppearance]`; the two normally agree.
- elsewhere: `"1"` → true, `"0"` → false, otherwise `check_dark_mode()` (`GUI_Utils.cpp`:
  `wxSystemSettings::GetAppearance().IsDark()`). On Windows the default makes the fallback effectively unreachable;
  on Linux the key is only empty before `on_init_inner` writes it, so "dark" means "the GTK theme's window colours
  are dark".

`check_dark_mode()` keeps `IsDark()` deliberately; switching it to `AreAppsDark()`/`IsSystemDark()` is not a required
3.3 migration: off MSW the three are identical (`src/common/settcmn.cpp:71-84`), and on MSW it is reached only for an
unset key and for Windows menu bitmaps, where matching wx's own menu state is the point.

There is **no "follow system" setting and no in-app override on macOS/Linux**: the app mirrors the OS and re-syncs
the config on each SYS_COLOUR event. `SUPPORT_DARK_MODE` is defined unconditionally in `libslic3r/AppConfig.hpp`, and
`_MSW_DARK_MODE` is defined to 1 on every platform in `GUI_App.hpp` — neither is a platform gate. Windows-only code
sits under `__WINDOWS__`/`_WIN32` (NppDarkMode sources are added only `if (WIN32)` in `src/slic3r/CMakeLists.txt`).
The other theme-dependent `GUI_App` colours (`m_color_label_modified`, `m_color_label_sys` `#363636`/`#B2B3B5`,
`m_color_label_default`, `m_color_highlight_default` `#F1F1F1`/`#36363B`, `m_color_window_default`, button
label/background) are computed by `init_label_colours()`.

**Pitfalls**

- **Rule:** An explicit `dark_color_mode` wins in both directions; `check_dark_mode()` is a last-resort fallback for
  an unset key.
  **Why:** on Windows, once `MSWEnableDarkMode(DarkMode_Auto)` is active, `IsDark()` reports the system app mode, so
  an explicit "0" falling through to it made dark → light switching silently fail.
  ```cpp
  // Wrong: explicit "0" falls through to the contaminated system query
  return app_config->get("dark_color_mode") == "1" ? true : check_dark_mode();
  // Right: explicit choice wins in both directions
  const auto& val = app_config->get("dark_color_mode");
  if (val == "1") return true;
  if (val == "0") return false;
  return check_dark_mode();   // unset key only
  ```
  Cite: 7d7f26ed69 (`GUI_App.cpp`, `GUI_App::dark_mode`, the non-Apple branch).
- **Rule:** Any new code path that changes the theme first sets `dark_color_mode` (Windows: `app_config->set` +
  `save()`; macOS/Linux: `update_dark_config()`), then refreshes the cached state: `wxGetApp().Update_dark_mode_flag()`
  (`m_is_dark_mode`, read by the walk), `wxGetApp().init_label_colours()` (StateColor's `gDarkMode` and the label
  colours) and, on Windows, `wxGetApp().force_colors_update()` (NppDarkMode's `g_darkModeEnabled` via
  `NppDarkMode::SetDarkMode(dark_mode())`, the main frame's title bar, and the walk flag `update_ui_from_settings()`
  consumes). `dark_mode()` itself is live; wx's own MSW mode follows the system and is not set by Orca.
  **Why:** they are updated at different moments; `update_dark_config()` does not touch StateColor's flag — for the
  main frame that happens in `MainFrame::on_sys_color_changed` — and after startup only `force_colors_update()` calls
  `NppDarkMode::SetDarkMode`.

## The Update*DarkUI walk

`GUI_App` helpers (`GUI_App.hpp/.cpp`):

| Helper | Does |
|---|---|
| `UpdateDarkUI(win, highlited = false, just_font = false)` | One window. `just_font` is unused. Skips `wxBU_AUTODRAW` buttons (`wxButton` or Orca `Button`). Windows only: a `wxButton` with id `wxID_OK`/`wxID_CANCEL` gets `wxNO_BORDER`, palette background/foreground (→ owner-drawn) and four hover/focus handlers **bound again on every call**. Then, using `m_is_dark_mode` (not `dark_mode()`): **dark** — `bg = darkModeColorFor(GetBackgroundColour())`, set only if it changed (exact key); `fg = darkModeColorFor(GetForegroundColour())`, then if ΔE(bg, fg) < 10 → LAB L = 90, if L(fg) < 45 → L = 70, and **fg is always set**; **light** — `lightModeColorFor` on background and foreground, each set only if changed. |
| `update_dark_children_ui(win)` (file-static) | recursive over `GetChildren()` at call time: a `ScalableButton` gets `ScalableButton::UpdateDarkUI()` (= `msw_rescale()`: `UpdateDarkUI(this, m_has_border)` plus re-rasterized icons), anything else `UpdateDarkUI(child)`. `GetChildren()` includes owned TLWs, so dialogs parented to the window are walked too. |
| `UpdateDarkUIWin(win)` | the walk. |
| `UpdateDlgDarkUI(dlg)` / `UpdateFrameDarkUI(frame)` | Windows: `NppDarkMode::SetDarkExplorerTheme` + `SetDarkTitleBar` on the HWND (both follow Orca's mode in both directions); then the walk. |
| `UpdateDVCDarkUI(dvc, highlited)` | **Windows-only body** (no-op on macOS/Linux): `UpdateDarkUI`, dark list header via `NppDarkMode::SetDarkListViewHeader`, header attr text colour `NppDarkMode::GetTextColor()`, `SetAlternateRowColour(m_color_highlight_default)` for `wxDV_ROW_LINES`, forces `wxBORDER_SIMPLE`. |
| `UpdateAllStaticTextDarkUI(parent)` | **Windows-only body**: `UpdateDarkUI(parent)` and `m_color_label_default` on direct `wxStaticText` children. |

Consequences: an unmapped background stays as it is; an unmapped dark foreground becomes grey (L≈70) instead of
dark-on-dark; every visited window ends with an explicit foreground (on MSW that makes raw buttons/checkboxes
owner-drawn); dark → light is lossy (lifted greys are not restored; shared twins collapse, see
[StateColor](#statecolor)); the walk only calls `Set{Background,Foreground}Colour`, so `wxPaintDC` painting is
unreachable by it. The source comment on `UpdateDarkUIWin` ("Don't use this function for Dialog contains
ScalableButtons") is stale: all three entry points run the same `update_dark_children_ui`, which already handles
`ScalableButton`; the only difference is the Windows HWND theming. Use `UpdateDlgDarkUI` for dialogs because of
that theming.

App-wide passes: `MainFrame`'s constructor ends with `UpdateDarkUIWin(this)` (on all platforms); a theme switch runs
`update_dark_children_ui(mainframe)` from `GUI_App::update_ui_from_settings`; the lazily built main-window tabs re-run
`UpdateDarkUIWin(this)` after insertion. Everything created later themes itself.

Raw wx controls do not follow Orca's mode by themselves (on Windows wx themes native controls by the *system* app
mode, see [wxMSW dark mode](#wxmsw-dark-mode-mswenabledarkmode-setappearance-wxdarkmodesettings)). When one cannot
be replaced by an Orca widget, make it dark-safe through the walk (`UpdateDarkUI(ctrl)` for a single control created
after the pass, `UpdateDlgDarkUI(dlg)` for its dialog) or with explicit `darkModeColorFor()` colours.

**Pitfalls**

- **Rule:** Theme a dialog with `UpdateDlgDarkUI`, not `UpdateDarkUI`.
  **Why:** `UpdateDarkUI` touches one window; children keep light defaults and the Windows title bar stays light.
  ```cpp
  // Wrong:
  wxGetApp().UpdateDarkUI(this);
  // Right:
  wxGetApp().UpdateDlgDarkUI(this);
  ```
  Cite: 465f634988 (`AMSMaterialsSetting.cpp` `AMSMaterialsSetting::Show`).
- **Rule:** After building a runtime-created subtree (widgets constructed after the initial pass), end the
  constructor with one `wxGetApp().UpdateDarkUIWin(this)`; for an ad-hoc `wxDialog`, call
  `wxGetApp().UpdateDlgDarkUI(&dlg)` after all children exist and before `ShowModal()` (the order relative to `Fit()`
  does not matter).
  **Why:** the app-wide pass themes only windows that existed when it ran; HMS notify items, device
  firmware/nozzle panels, one-off confirmation dialogs appear with light defaults in dark mode on every platform.
  One subtree walk also themes child labels and beats sprinkling per-widget `darkModeColorFor` calls.
  ```cpp
  // Wrong: dialog built and shown with hard-coded light colours only
  dlg.SetSizer(main_sizer); dlg.Fit(); dlg.ShowModal();
  // Right:
  dlg.SetSizer(main_sizer); dlg.Fit();
  wxGetApp().UpdateDlgDarkUI(&dlg);
  dlg.ShowModal();
  ```
  Cite: f7f0c82abb (`HMSPanel.cpp`, `DeviceTab/uiDeviceUpdateVersion.cpp`, `DeviceTab/wgtDeviceNozzleSelect.cpp`,
  `SelectMachine.cpp` `SelectMachineDialog::show_timelapse_storage_dialog`).
- **Rule:** Apply deliberate non-palette colours after the walk (and again on theme change).
  **Why:** in dark mode the walk rewrites every visited window's foreground — white text on an accent chip becomes
  mid-grey.
  ```cpp
  wxGetApp().UpdateDlgDarkUI(this);
  m_badge->SetForegroundColour(*wxWHITE);   // after the walk
  ```
- **Rule:** Run the dialog walk once per theme state; do not call it from paint or size handlers.
  **Why:** the map chains (`#FFFEFE → #D9D9D9 → #27272A`) and the Windows OK/Cancel branch stacks handlers per call.

## NppDarkMode (Windows)

Vendored Notepad++ dark-mode code in `src/slic3r/GUI/dark_mode.cpp/.hpp` and `src/slic3r/GUI/dark_mode/*.hpp`,
compiled only on Windows. Namespace `NppDarkMode`:

| Function | Does |
|---|---|
| `InitDarkMode(bool dark, bool sys_menu)` | loads the uxtheme entry points, records the system-menu setting, `SetDarkMode(dark)`. |
| `SetDarkMode(bool)` | sets `g_darkModeEnabled`; `AllowDarkModeForApp(dark)` → `SetPreferredAppMode(ForceDark|ForceLight)` (Windows 1903+) or `AllowDarkModeForApp` (1809); flushes menu themes when the system-menu setting is on; scrollbar fix. |
| `SetDarkTitleBar(HWND)` | allows dark for the window per `IsEnabled()`, refreshes the title-bar colour, applies the explorer theme. |
| `SetDarkExplorerTheme(HWND)` | `SetWindowTheme(hwnd, IsEnabled() ? L"DarkMode_Explorer" : nullptr, nullptr)`. |
| `SetDarkListViewHeader(HWND)` | dark `ItemsView` theme on a list header. |
| `GetTextColor()` | `0xF0F0F0` when enabled, else `wxSYS_COLOUR_WINDOWTEXT` — which is wx's dark-palette `0xe0e0e0` when the Windows app mode is dark and Orca is light **[source]**. |

Unlike wx's MSW dark mode it switches live: every function follows `g_darkModeEnabled` in both directions, so
re-running them on existing HWNDs re-themes them. `GUI_App::force_colors_update()` re-arms it
(`NppDarkMode::SetDarkMode(dark_mode())`, `SetDarkTitleBar(mainframe)`); `UpdateDlgDarkUI`/`UpdateFrameDarkUI`
apply it per TLW. Its `#if wxVERSION_NUMBER < 3300` block that themed the tooltip window is dead:
`wxToolTip::GetToolTipCtrl()` is private in 3.3 (`include/wx/msw/tooltip.h:92`), and wx dark-enables the
tooltip window itself through `wxMSWDarkMode::AllowForWindow`, so tooltips follow wx's mode (the system app
mode), not Orca's **[source]** (`src/msw/tooltip.cpp:321`). `update_dark_ui(wxWindow*)` (`GUI_Utils.cpp`, `_WIN32`), which `DPIAware`'s constructor and
`force_color_changed()` call, has an empty body.

## Runtime theme switch and re-applying colours

```text
Startup (GUI_App::on_init_inner): init_label_colours() [StateColor::SetDarkMode] → Update_dark_mode_flag()
  → (non-Windows) dark_color_mode := GetAppearance().IsDark()
  → (Windows) MSWEnableDarkMode(DarkMode_Auto); NppDarkMode::InitDarkMode(dark_mode(), sys_menu)
  MainFrame ctor ends with UpdateDarkUIWin(this)

Windows — Preferences "Enable dark Mode" (PreferencesDialog::create_item_darkmode, the only toggle):
  1 set + save dark_color_mode   2 Update_dark_mode_flag()
  3 force_colors_update(): NppDarkMode::SetDarkMode(dark_mode()), SetDarkTitleBar(mainframe), m_force_colors_update
  4 update_ui_from_settings():
       mainframe->force_color_changed() [_WIN32: update_dark_ui (empty) + MainFrame::on_sys_color_changed()]
       update_scrolls(mainframe), update_scrolls(&m_settings_dialog)
       update_dark_children_ui(mainframe)            (all platforms when m_force_colors_update)
  5 PreferencesDialog::set_dark_mode() → UpdateDlgDarkUI(this)
  6 wxPostEvent(plater, EVT_GLCANVAS_COLOR_MODE_CHANGED) → Plater::priv::on_change_color_mode (GL canvases, sidebar)

macOS / Linux — system switch: wxEVT_SYS_COLOUR_CHANGED at every DPIAware TLW (order undefined)
  → update_dark_config() [config + m_is_dark_mode] → on_sys_color_changed() → Skip()
  MainFrame::on_sys_color_changed(): init_label_colours() → force_colors_update() → update_ui_from_settings()
       [update_dark_children_ui(mainframe)] → fan-out below
  Plater::priv::on_apple_change_color_mode → GL canvases
```

At startup `init_label_colours()` and `Update_dark_mode_flag()` run before the key is rewritten from the system, and
the later re-check only fires when `dark_mode()` changes after that rewrite. On Linux (where `dark_mode()` reads the
key) a start after the GTK theme changed while Orca was closed therefore leaves StateColor's flag and
`m_is_dark_mode` on the previous session's value — including for the `MainFrame` constructor's walk — until the next
`MainFrame::on_sys_color_changed`. **[source; consequence inferred from the call order in
`GUI_App::on_init_inner`, not observed]**

`MainFrame::on_sys_color_changed` is the **registry for long-lived UI**: `DiffPresetDialog::if_built()`,
`m_tabpanel->Rescale()`, `m_param_panel->msw_rescale()`, `plater()->sys_color_changed()` (→ `Sidebar::sys_color_changed`
→ …), `MonitorPanel::when_built`, `CalibrationPanel::when_built`, every `Tab::sys_color_changed()` (tabs, model tabs,
plate tab), `MenuFactory::sys_color_changed(m_menubar)` (its body is compiled out with `#if 0`, so menu-bar item
icons are not re-rasterized; the cached context menus are, via `Plater::sys_color_changed` →
`MenuFactory::sys_color_changed()`), `WebView::RecreateAll()`, then `Refresh()`. A cached,
hidden or lazily built window must be added here or chained from something here: the walk reaches dialogs parented
to the main frame (they are in `GetChildren()`), but only their colours (and `ScalableButton` icons) — not their
title bar, `ScalableBitmap`-based images or `on_sys_color_changed()`. Commit bab3c72e4f fixed the compare dialog keeping old row colours by calling
`diff_dialog.on_sys_color_changed()` from this fan-out (the dialog is not destroyed on close).

Who gets what on a switch:

| Window | Windows (Preferences) | macOS/Linux (system) |
|---|---|---|
| MainFrame subtree | walk + fan-out | walk + fan-out |
| Open dialog parented to the main frame | walk only (Preferences re-themes itself) | its own `on_sys_color_changed()` via the OS event + the walk |
| Hidden cached dialog parented to the main frame | walk only, unless chained in the fan-out | walk + its own handler (the OS event reaches every TLW) |
| Modal dialogs (`dialogStack`, via `DPIAware::ShowModal`) | cannot be open across a Preferences change | live |

**What a dialog must do to survive a toggle:**
1. Derive from `DPIDialog` (`DPIAware<wxDialog>`, `GUI_Utils.hpp`; `on_sys_color_changed()` is a protected virtual
   no-op by default) and use palette colours / Orca widgets.
2. End the constructor with `wxGetApp().UpdateDlgDarkUI(this)` — it also sets the Windows dark title bar and
   explorer theme the children walk alone does not.
3. Override `on_sys_color_changed()` to re-create `ScalableBitmap`s (`msw_rescale()` + re-`SetBitmap`), re-pick
   `*_dark` icon names, re-apply construction-time and owner-drawn colours, call widget `Rescale()` where styles
   depend on the theme, then `Refresh()`. It runs on macOS/Linux from the OS event; on Windows only if the main-frame
   fan-out calls it.
4. If it outlives a show (cached singleton, lazily built), register it in `MainFrame::on_sys_color_changed`.

**Pitfalls**

- **Rule:** Never hand a literal light-theme colour straight to `SetForegroundColour`/`SetBackgroundColour`/`wxPen`/`wxBrush`;
  wrap it in `StateColor::darkModeColorFor(...)` when it is a `gDarkColors` key, or branch on
  `wxGetApp().dark_mode()` with an explicit dark counterpart when it is not.
  **Why:** `darkModeColorFor` is an exact RGBA lookup; an unmapped colour (`#F0F0F0`, `#333333`, `#5C5C5C`) passes
  through unchanged and renders as a light patch, or — set directly with no walk after it — dark-on-dark text. This
  also applies to owner-drawn `wxPaintDC` painting (popup borders/fills), which the walk cannot reach.
  ```cpp
  label->SetForegroundColour(wxColour("#009688"));                     // Wrong: stays light-theme teal
  label->SetForegroundColour(StateColor::darkModeColorFor("#009688"));  // Right: → #00675b in dark
  // unmapped colour: branch explicitly
  msg->SetForegroundColour(wxGetApp().dark_mode() ? wxColour("#EFEFF0") : wxColour(0x33, 0x33, 0x33));
  ```
  Cite: f7f0c82abb (`SelectMachine.cpp` `SelectMachineDialog::Enable_Auto_Refill`, `show_timelapse_folder_popup`;
  `Plater.cpp` `HoverLabel`).
- **Rule:** Any colour chosen at construction time (chip backgrounds, per-state label colours, `dark_mode()`-dependent
  picks) is re-applied on a live switch: override `on_sys_color_changed()` (DPIDialog/DPIFrame) or add a
  `sys_color_changed()` method that the owner's `sys_color_changed()` chains to, ending with `Refresh()`.
  **Why:** `dark_mode()` is live but a value computed once in a constructor is not; the walk fixes mapped colours only,
  not unmapped colours or platform-conditional picks.
  ```cpp
  // Wrong: colours set once in the ctor, never again
  HoverLabel(...) { SetBackgroundColour(extruder_group_chip_bg()); ... }
  // Right: also re-apply on theme switch, chained from the parent
  void HoverLabel::sys_color_changed()   { SetBackgroundColour(extruder_group_chip_bg()); /* label fg */ Refresh(); }
  void ExtruderGroup::sys_color_changed() { if (hover_label) hover_label->sys_color_changed(); ...; Refresh(); }
  void Sidebar::sys_color_changed()       { ...; for (auto* ext : extruders) ext->sys_color_changed(); }
  ```
  Cite: f7f0c82abb (`Plater.cpp`: `HoverLabel::sys_color_changed`, `ExtruderGroup::sys_color_changed`,
  `Sidebar::sys_color_changed`).
- **Rule:** In a dialog's `on_sys_color_changed()` that derives colours through `darkModeColorFor`/`StateColor`,
  call `wxGetApp().init_label_colours()` first.
  **Why:** `darkModeColorFor` uses StateColor's flag, which only `init_label_colours()` refreshes (startup and
  `MainFrame::on_sys_color_changed`). On macOS each NSWindow delivers the event from its own KVO with no defined
  order, so a dialog handler can run before the main frame's and see the old flag. **[source; the ordering risk is
  inferred, not observed]**

## Dark-mode icons

Icons are SVGs in `resources/images/` (a PNG of the same name is only a fallback, with no dark substitution), named
without extension, rasterized by Orca itself (wx has no SVG
support in Orca's build — see `references/dpi-bitmaps-fonts.md` for sizing, `ScalableBitmap` and `BitmapCache`
mechanics). Dark mode reaches icons in two regimes:

1. **Palette substitution.** `create_scaled_bitmap()` (`wxExtensions.cpp`) passes `wxGetApp().dark_mode()` to
   `BitmapCache::load_svg`, which text-replaces palette colours in the SVG before nanosvg parses it. An SVG drawn
   purely in substitution colours is dark-correct automatically.
2. **`*_dark` asset variants**, chosen by name in code, for everything else.

**Substitution table** (`BitmapCache::load_svg`; replacement by `BitmapCache::nsvgParseFromFileWithReplace`):

| Mode | Replacements |
|---|---|
| light | `"#00FF00"` → `"#52c7b8"`; unquoted `#949494` → `#7C8282` (icon line colour) |
| dark | `"#262E30"` → `"#EFEFF0"` and unquoted `#262E30` → `#EFEFF0`; `"#323A3D"` → `"#B3B3B5"`; `"#808080"` → `"#818183"`; `"#CECECE"` → `"#54545B"`; `"#6B6B6B"` → `"#818182"`; `"#909090"` → `"#FFFFFF"`; `"#00FF00"` → `"#FF0000"`; `"#009688"` → `"#00675b"`; `"#F1F1F1"` → `"#36363B"`; unquoted `#DBDBDB` → `#4A4A51` (border), `#F0F0F1` → `#333337` (disabled background) |
| dark, name contains `toggle_on` | additionally unquoted `#009688` → `#00675b` |
| `new_color` argument | replaces the `"#009688"` slot in both modes (in dark mode it overrides the `#00675b` mapping too) |
| name contains `printer_thumbnail` | no replacement at all |
| both modes | the key `"#0x00AE42"` is malformed (contains `0x`) and matches nothing real — dead |

Mechanics: literal, **case-sensitive** `boost::replace_all` over the raw file text, applied in `std::map` key order,
so all quoted keys (`"` = 0x22) run before unquoted ones (`#` = 0x23). Quoted keys match only a full attribute value
written `="#RRGGBB"` (`fill="#262E30"`, `stroke="…"`); they never match CSS `style="fill:#…"` or single quotes. Only the
unquoted keys (`#262E30`, `#DBDBDB`, `#F0F0F1` in dark; `#949494` in light) reach style attributes. The cache key
contains size, scale, `-dm`, `-gs` and the `new_color` string, so light and dark rasterizations are cached
separately. This SVG map is separate from, and not identical to, `gDarkColors`.

`create_scaled_bitmap` re-queries the mode on every call, with two exceptions: on Windows `menu_bitmap = true`
(`create_menu_bitmap`) uses `check_dark_mode()`; `bitmap2 = true` routes to `create_scaled_bitmap2` →
`BitmapCache::load_svg2`, which applies **no** palette substitution (only `#D9D9D9`/`fill-opacity` from
`array_new_color`). Re-rasterizing is therefore the theme hook: `ScalableBitmap::msw_rescale()` and
`ScalableButton::msw_rescale()` re-run `create_scaled_bitmap`, so the DPI path doubles as the theme path
(`Tab::sys_color_changed` calls `msw_rescale()` on every cached button/bitmap and rebuilds its `wxImageList`). The
walk reaches `ScalableButton`s (`ScalableButton::UpdateDarkUI` = `msw_rescale()`), but not `ScalableBitmap`
members — they are not windows; their owner calls `msw_rescale()` and re-`SetBitmap`s. `ScalableBitmap::msw_rescale()`
re-creates from name, size, grayscale and resize only: `new_color` and `bitmap2` are not re-applied.

**When a `*_dark` variant + explicit re-pick is required:** whenever the icon's colours are not in the substitution
table (multi-colour artwork, brand colours, off-palette greys like `#1F1F1F`), or the dark rendition is not a 1:1
colour mapping of the light one. The code chooses the `_light`/`_dark` name from `wxGetApp().dark_mode()` **in code
that re-runs on theme change**:
```cpp
// in the ctor AND in on_sys_color_changed():
m_icon = ScalableBitmap(this, wxGetApp().dark_mode() ? "icon_dark" : "icon", 20);
m_static_bmp->SetBitmap(m_icon.bmp());
```
The substitution still runs on whichever file is loaded, so variant files must use
off-palette colours or the `_dark` asset is recoloured a second time (a `#262E30` in a `_dark` file still becomes
`#EFEFF0`). Re-rasterizing a stored name (`ScalableBitmap::msw_rescale`) does **not** switch variants. Reference
pattern: `AmsHumidityLevelList` (`AmsMappingPopup.cpp`) preloads both variants as `ScalableBitmap`s and picks
`hum_level_img_dark`/`hum_level_img_light` by `dark_mode()` inside its render path, so a `Refresh()` re-picks.
Alternatively re-run the name selection inside `on_sys_color_changed()`/`msw_rescale()`.

**Menu bitmaps.** Menu items built with `append_menu_item(..., icon_name, ...)` use `create_menu_bitmap` (16 px,
no window, `menu_bitmap = true`), and the icon name is remembered per item id (not on GTK). On Windows the SVG substitution
then uses `check_dark_mode()` as its dark flag — wx's own answer,
which is what wx uses to draw menus (its owner-drawn menu path keys on `wxMSWDarkMode::IsActive()`), so the icon
matches the menu background even when Orca's mode differs from the Windows app mode. On a theme change Windows menu
icons are re-rasterized by `msw_rescale_menu` (a no-op elsewhere) from the stored icon names;
`MenuFactory::sys_color_changed()` does this for the cached context menus (the menu-bar overload is compiled out). Menus themselves are covered in
`references/popups-menus.md`.

**Pitfalls**

- **Rule:** Author single-tone SVG icons in the colours `load_svg` substitutes — for near-black line art use
  `#262E30` (uppercase), not an arbitrary near-black like `#1F1F1F` or `#333333`.
  **Why:** recolouring is a literal, case-sensitive string replacement on a fixed palette; an off-palette fill (or
  lowercase `#262e30`) passes through untouched and the icon disappears against the dark background. The fix is a
  colour change in the asset; no code change.
  ```xml
  <path d="..." fill="#1F1F1F"/>   <!-- Wrong: not substituted, invisible in dark mode -->
  <path d="..." fill="#262E30"/>   <!-- Right: → #EFEFF0 in dark mode -->
  ```
  Cite: f658aad7ca (`resources/images/ams_drying.svg`); `BitmapCache::load_svg`, `BitmapCache::nsvgParseFromFileWithReplace`.
- **Rule:** When an icon exists as `*_light`/`*_dark` variants, select the variant matching the mode — `_dark` in
  dark mode — and make the selection run inside code that re-executes on theme change, not once.
  **Why:** the classic copy-paste bug returned the `_light` name in both branches, so dark mode showed
  near-invisible light-theme humidity glyphs; nothing else corrects it because the variant files are authored
  off-palette. A selection made only when the data changes keeps the old variant after a live switch until the data
  changes again.
  ```cpp
  // Wrong: light asset in dark mode
  if (wxGetApp().dark_mode()) return "hum_level" + std::to_string(hum_level) + "_no_num_light";
  else                        return "hum_level" + std::to_string(hum_level) + "_no_num_light";
  // Right:
  if (wxGetApp().dark_mode()) return "hum_level" + std::to_string(hum_level) + "_no_num_dark";
  else                        return "hum_level" + std::to_string(hum_level) + "_no_num_light";
  ```
  Cite: 668654da5f (`AMSDryControl.cpp` `get_humidity_level_img_path`); live-switch shape: `AmsHumidityLevelList`
  (`AmsMappingPopup.cpp` `AmsHumidityLevelList::doRender`).
- **Rule:** Do not "fix" a Windows menu icon by passing `wxGetApp().dark_mode()`.
  **Why:** `create_menu_bitmap` deliberately follows `check_dark_mode()` (wx's menu state = the Windows app mode),
  not Orca's setting; the icon must match the background wx draws.
  Cite: `wxExtensions.cpp` `create_scaled_bitmap`, `create_menu_bitmap`.
- **Rule:** Pass the real window to `create_scaled_bitmap`/`ScalableBitmap` and re-create bitmaps in
  `on_sys_color_changed()` as well as `on_dpi_changed()`; a missing icon name throws `Slic3r::RuntimeError`.
  Cite: `wxExtensions.cpp` `create_scaled_bitmap`.
