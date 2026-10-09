# Platforms, the wx build, and platform-specific code

Read this when code has to differ per platform, when a bug shows up on only one OS, toolkit or display
server, or when you need to know how Orca's wxWidgets is built. It covers the wx fork and its build
options, platform macros, per-platform summaries that point into the topic files, and where
platform-specific code lives. It also owns runtime X11/Wayland detection, the Wayland gap list, custom
title bars and window decoration, GTK native-chrome removal, and the cross-platform test checklist.

Contents: [Rules](#rules) · [The wx build Orca uses](#the-wx-build-orca-uses) ·
[Platform macros and native handles](#platform-macros-and-native-handles) ·
[Per-platform summaries](#per-platform-summaries) · [The ifdef landscape](#the-ifdef-landscape) ·
[Runtime X11/Wayland detection](#runtime-x11wayland-detection) · [Wayland gaps](#wayland-gaps) ·
[Window decoration and custom title bars](#window-decoration-and-custom-title-bars) ·
[GTK native chrome and GTK size calls](#gtk-native-chrome-and-gtk-size-calls) ·
[wx 3.3 migration notes](#wx-33-migration-notes) ·
[Cross-platform testing checklist](#cross-platform-testing-checklist)

## Rules

1. Every GUI change must work on Windows (wxMSW), macOS (wxOSX/Cocoa) and Linux wxGTK3, under both
   X11 and Wayland. GTK-guarded code must still compile against GTK2, but GTK2 is an opt-out build and
   not something Orca ships. → [The wx build](#the-wx-build-orca-uses), [Testing](#cross-platform-testing-checklist)
2. Use the wx toolkit macros (`__WXMSW__`, `__WXOSX__`, `__WXGTK__`, `__WXGTK3__`) when the behaviour
   comes from wx. Use `_WIN32` / `__APPLE__` / `__linux__` only for OS APIs and in code that is not
   built against wx (`src/libslic3r`). → [Platform macros](#platform-macros-and-native-handles)
3. Guard a declaration in the header exactly as its definition is guarded in the `.cpp`, and call a
   guarded helper only under the same guard. → [Platform macros](#platform-macros-and-native-handles)
4. Decide X11 vs Wayland at runtime with `is_running_on_wayland()` / `is_running_on_x11()`, and only
   after GTK is initialised. Read `WAYLAND_DISPLAY` / `DISPLAY` / `GDK_BACKEND` only before GTK starts
   (`CLI::run`). → [Runtime detection](#runtime-x11wayland-detection)
5. On Wayland, none of these work: global pointer coordinates, positioning top-level windows,
   drawing through `wxClientDC`, `Update()`, `SetIcon`, `WarpPointer` (outside narrow conditions),
   AUI floating panes, `wxUIActionSimulator`. → [Wayland gaps](#wayland-gaps)
6. Orca's wx has no SVG support and no asserts. Never call `wxBitmapBundle::FromSVG*`. Wherever the
   wx docs say a call "asserts", expect a silent failure in Orca and check the precondition yourself.
   → [The wx build](#the-wx-build-orca-uses)
7. Change wx build options or patches only in `deps/wxWidgets/wxWidgets.cmake`, and make the same
   change in the wxWidgets module of the Flatpak manifest. → [The wx build](#the-wx-build-orca-uses)
8. On MSW, `MainFrame` draws its own non-client area. Mask `WS_CAPTION` out of every non-client
   computation, and handle `WM_NCCALCSIZE` for the maximised case yourself.
   → [MSW title bar](#msw-the-mainframe-custom-title-bar)
9. On Linux, move and resize the borderless main frame through the window manager
   (`gtk_window_begin_move_drag` / `gtk_window_begin_resize_drag`). Never call `Move()` or `SetSize()`
   from mouse coordinates. → [GTK frame](#linux-gtk-the-borderless-mainframe)
10. To show an undecorated top-level window on Wayland, install an empty client-side titlebar and then
    call `gtk_window_set_decorated(false)`, both in the constructor, before control returns to the
    event loop.
    → [Undecorated windows on Wayland](#wayland-undecorated-top-level-windows-splash)
11. Remove GTK theme borders from custom-drawn controls with `RemoveButtonBorder` / `RemoveInputBorder`
    (`__WXGTK__` only). Call raw GTK size functions only with strictly positive sizes.
    → [GTK native chrome](#gtk-native-chrome-and-gtk-size-calls)
12. Put platform glue where it already lives: Cocoa code in `.mm` files listed in the `APPLE` block of
    `src/slic3r/CMakeLists.txt`, Win32 messages in `MSWWindowProc` overrides, and GDK/GTK calls behind
    `__WXGTK__` with the GTK header included under the same guard. → [Ifdef landscape](#the-ifdef-landscape)
13. Before fixing something "for platform X", find the wx mechanism that differs there (the
    per-platform summaries point to it) and check whether the same bug class exists on the other
    platforms. → [Per-platform summaries](#per-platform-summaries)

## The wx build Orca uses

### Source, pin and local patch

- `deps/wxWidgets/wxWidgets.cmake` builds `https://github.com/SoftFever/Orca-deps-wxWidgets` at tag
  **`v3.3.2`** (`GIT_SHALLOW ON`, submodules `3rdparty/catch`, `3rdparty/pcre` and `3rdparty/libwebp`
  only). The fork carries Orca's build fixes, the clang-cl fix among them; do not duplicate
  a fork fix as a local patch under `deps/wxWidgets/` (cc390f11ee removed the local
  `0001-Clang-CL-fix.patch` once the fork carried the fix). The fork's clang-cl fix is the MSVC lib-dir
  selection in the installed `wxWidgetsConfig.cmake`: it looks for `<prefix>_<arch>_lib` (or `_dll`)
  under the consuming compiler's prefix first and then the sibling one (`clang` ↔ `vc`), because cl
  and clang-cl share an ABI and either can consume either build
  (`build/cmake/wxWidgetsConfig.cmake.in:53-73`).
- **The one local patch** is `deps/wxWidgets/0001-macos-use-srgb-colour-components.patch`, applied
  only `if (APPLE)`. The `PATCH_COMMAND` first runs `git checkout -f -- src/osx/cocoa/colour.mm` and
  then `git apply`, so the step can run again safely (a7775296b0). The patch makes the wxOSX
  `wxColour` component getters (`wxNSColorRefData::Red/Green/Blue/Alpha` and `IsSolid`) convert the
  `NSColor` with `[NSColorSpace sRGBColorSpace]` instead of `NSCalibratedRGBColorSpace`, so colours
  read back on macOS match their sRGB values (custom-colour accuracy). Colour usage:
  `references/colours-dark-mode.md`.
- The checked-out source is the tree that every wx citation in this skill refers to:
  `deps/build/<arch>/dep_wxWidgets-prefix/src/dep_wxWidgets` on macOS and
  `deps/<tree>/dep_wxWidgets-prefix/src/dep_wxWidgets` on Linux and Windows (`deps/build` for a
  release build; `build_win.bat` names the others). Locate it with the bash or PowerShell lookup in
  `SKILL.md` §Ground truth. On macOS its
  `src/osx/cocoa/colour.mm` already has the patch applied.
- **Flatpak builds wx separately.** `deps/CMakeLists.txt` leaves `dep_wxWidgets` out of the deps
  target when `FLATPAK` is set. Instead, `scripts/flatpak/com.orcaslicer.OrcaSlicer.yml` has its own
  `wxWidgets` module whose config-opts "mirror deps/wxWidgets/wxWidgets.cmake with FLATPAK=ON,
  DEP_WX_GTK3=ON": `-DwxBUILD_TOOLKIT=gtk3`, a shared build (`wxBUILD_SHARED=ON`,
  `BUILD_SHARED_LIBS=ON`, `d` debug postfix), and `wxUSE_LIBWEBP=sys`, because the builtin webp
  libraries are installed only by static builds. It links with lld and pins the fork's tag
  `orca-3.3.2` at a fixed commit. Option and version changes must be made in both files.

### Toolkit per platform

| Platform | wx port | How it is selected |
|---|---|---|
| Windows | wxMSW | default port; the Edge WebView backend is built only for MSVC-family compilers |
| macOS | wxOSX/Cocoa | default port; wx's exported targets add `__WXOSX_COCOA__;__WXMAC__;__WXOSX__` |
| Linux | **wxGTK3** | `deps/CMakeLists.txt` declares `option(DEP_WX_GTK3 "Build wxWidgets against GTK3" ON)` (default ON since 026499c5b7, #10294), which gives `-DwxBUILD_TOOLKIT=gtk3`. The root `CMakeLists.txt` sets `SLIC3R_GTK "3"`, so `src/CMakeLists.txt` finds wx through `wx-config --toolkit=gtk${SLIC3R_GTK}` and `src/slic3r/CMakeLists.txt` links `GTK${SLIC3R_GTK}`. Flatpak uses gtk3 too. |

**GTK2 is an opt-out, not a default.** `wxWidgets.cmake` starts from `_gtk_ver 2` and switches to 3
when `DEP_WX_GTK3` is on, so you get GTK2 only by passing `-DDEP_WX_GTK3=OFF` (and `SLIC3R_GTK=2`).
A GTK2 build loses the following (wx `build/cmake/init.cmake`, `include/wx/features.h`):
- EGL: `wxUSE_GLCANVAS_EGL` is forced OFF unless GTK3 and EGL are both found (init.cmake:559-561), and
  `wxHAS_EGL` is set only on GTK3 (:531-540). No EGL means no native Wayland GL.
- WebKit2: GTK2 gets WebKit1 (init.cmake:568-569); GTK3 uses webkit2gtk-4.1 and falls back to 4.0
  (:571-577).
- DIP pixels: `wxHAS_DPI_INDEPENDENT_PIXELS` is defined only for `__WXGTK3__ || __WXMAC__ || __WXQT__`
  (`include/wx/features.h:115-120`), so GTK2 uses physical pixels and gets no `wxEVT_DPI_CHANGED`.
- Backend detection: Orca's `wxHAVE_GDK_*` macros are not defined, so `get_linux_display_backend()`
  always returns `Unknown` ([Runtime detection](#runtime-x11wayland-detection)).
- Border removal: `RemoveButtonBorder` / `RemoveInputBorder` fall back to a global `gtk_rc` style.

`src/slic3r/CMakeLists.txt` also requires `webkit2gtk-4.1`, a GTK3 library, so a GTK2 GUI would load
GTK2 and GTK3 into one process. Treat GTK2 as a compile-compatibility target only.

wx is linked statically (`wxBUILD_SHARED=OFF`) everywhere except Flatpak. On Windows and macOS,
`src/CMakeLists.txt` uses `find_package(wxWidgets 3.3 CONFIG … propgrid)` (`propgrid` is needed by
wxInspector). On Linux it uses the `wx-config` module mode.

### Build options

The options `deps/wxWidgets/wxWidgets.cmake` passes, with the consequence each one has for GUI code:

| Option | Value | Consequence for GUI code |
|---|---|---|
| `wxBUILD_DEBUG_LEVEL` | `0` | wx asserts are compiled out; see [Debug level 0](#debug-level-0-no-wx-asserts) |
| `wxBUILD_SHARED` | `OFF` (Flatpak: `ON`) | static; private wx globals such as `wxCurrentPopupWindow` can be reached with `extern` |
| `wxBUILD_PRECOMP` / `wxBUILD_SAMPLES` | `ON` / `OFF` | build speed only |
| `wxUSE_NANOSVG` | `OFF` | no SVG in wx; see [No SVG](#no-svg-in-orcas-wx). It was disabled to avoid duplicate symbols with Orca's own NanoSVG, which carries an `nsvgRasterizeXY` extension (7658cf9076) |
| `wxUSE_GLCANVAS_EGL` | `ON` | takes effect only on GTK3 with EGL found; with both EGL and GLX built, wx picks EGL even on X11 unless `PreferGLX()` is called → `references/webview-gl-aui-media.md` §EGL vs GLX |
| `wxUSE_OPENGL` | `ON` | `wxGLCanvas` |
| `wxUSE_WEBVIEW` / `wxUSE_WEBVIEW_EDGE` / `wxUSE_WEBVIEW_IE` | `ON` / `ON` only `if (MSVC)` / `OFF` | Edge (WebView2) on Windows, WKWebView on macOS, WebKit2GTK on Linux. `wxUSE_WEBVIEW_CHROMIUM` keeps its default `OFF` (`build/cmake/options.cmake:303`), so Chromium-backend notes never apply. `wxUSE_WEBVIEW_EDGE_STATIC` keeps its default `OFF` (`build/cmake/options.cmake:514`), so the root `CMakeLists.txt` ships `WebView2Loader.dll` from `deps/WebView2/lib/win-<arch>` next to the executable → `references/webview-gl-aui-media.md` §wxWebView backends |
| `wxUSE_WEBREQUEST` | `ON` | `wxWebSession`/`wxWebRequest` are available (used only for a few image downloads) |
| `wxUSE_MEDIACTRL` | `ON` | kept for `wxMediaState`; the camera view is Orca's `wxMediaCtrl3`, not a `wxMediaCtrl` |
| `wxUSE_PRIVATE_FONTS` | `ON` | `wxFont::AddPrivateFont` for the bundled fonts → `references/dpi-bitmaps-fonts.md` |
| `wxUSE_AUI` | `ON` | Plater docking and `BBLTopbar` (a `wxAuiToolBar`) |
| `wxUSE_STC` | `OFF` | no `wxStyledTextCtrl` |
| `wxUSE_DETECT_SM` | `OFF` | no X11 session-manager detection |
| `wxUSE_REGEX` | `builtin` | — |
| `wxUSE_LIBPNG` / `ZLIB` / `LIBJPEG` / `EXPAT` | `sys` (from the deps tree) | — |
| `wxUSE_LIBTIFF` | `OFF` | no TIFF image handler |
| `wxUSE_LIBWEBP` | `builtin` (Flatpak: `sys`) | WebP image handler is available |
| `wxUSE_LIBSDL` / `wxUSE_XTEST` | `OFF` | no SDL audio backend; `wxUIActionSimulator` on X11 uses its non-XTest path (`src/unix/uiactionx11.cpp`) |

Options left at wx defaults that matter: LunaSVG is off (`wxUSE_LUNASVG 0` in the installed
`setup.h`), `wxUSE_STD_CONTAINERS 1`, `WXWIN_COMPATIBILITY_3_0 0`, and `WXWIN_COMPATIBILITY_3_2 1`.
On macOS `wxUSE_NATIVE_DATAVIEWCTRL` is 1 (`references/controls-dataview.md`).

### Debug level 0: no wx asserts

wx is built with `-DwxBUILD_DEBUG_LEVEL=0`, which `build/cmake/init.cmake:245-246` turns into
`-DwxDEBUG_LEVEL=0`. `src/slic3r/CMakeLists.txt` also adds `wxDEBUG_LEVEL=0` to `libslic3r_gui` when
`SLIC3R_STATIC`. The Flatpak module and wxInspector use level 0 as well. At level 0, `wxASSERT`,
`wxFAIL` and `wxTrap` "do nothing at all", while "wxCHECK macros always check their conditions,
setting debug level to 0 only makes them silent in case of failure" (`include/wx/debug.h:229-231`,
`:342-382`).

In practice, misuse that a debug wx would report shows up in Orca as a silent no-op, an early return
or a wrong result: for example a second `ReleaseMouse`, a late `PreferGLX()`, a second `Destroy()` on
a transient popup, a window added to a second sizer, `SetCurrent` on a hidden GL canvas, or a
duplicate AUI pane name. Write "wx would assert in a debug build; in Orca it silently …". When the
wx docs state a precondition, check it in your own code.

### No SVG in Orca's wx

`wxHAS_SVG` is defined only when `wxHAS_RAW_BITMAP && (wxUSE_NANOSVG || wxUSE_LUNASVG)`
(`include/wx/features.h:96-97`), and Orca builds with both off. So `wxBitmapBundle::FromSVG`,
`FromSVGFile` and `FromSVGResource` do not exist (`include/wx/bmpbndl.h:81-101`), using them is a
compile error, the Tango art provider returns empty bundles (`src/common/arttango.cpp`, `!wxHAS_SVG`
branch), and wx's AUI tab and dock-art buttons use their non-SVG bitmap fallbacks
(`src/aui/tabart.cpp:92-137`, `src/aui/dockart.cpp:87-128`) [source]. Orca rasterises SVG with its own
NanoSVG in `BitmapCache::load_svg` (through `create_scaled_bitmap` / `ScalableBitmap`) →
`references/dpi-bitmaps-fonts.md`.

### Private headers

The wx 3.3 CMake install does not copy `wx/private`. The `copy_private_headers` step in
`wxWidgets.cmake` runs after install and copies `include/wx/private`, `include/wx/generic/private`
and `include/wx/gtk/private` to `include/wx` (MSVC) or `include/wx-3.3/wx` (elsewhere). The cmake
comment calls this "for accessibility support". The actual consumers are:
- `Widgets/WebView.cpp`: `wx/private/jsscriptwrapper.h` (Windows and macOS only).
- `ExtraRenderers.cpp`: `wx/generic/private/{markuptext,rowheightcache,widthcalc}.h`, under
  `wxHAS_GENERIC_DATAVIEWCTRL`, so MSW only.
- `ExtraRenderers.cpp`: `wx/private/markupparser.h`, under `wxUSE_ACCESSIBILITY`.

Its `wx/gtk/private*` includes are commented out. The per-port headers outside those directories,
`wx/msw/private.h` (`BitmapComboBox.cpp`, `PresetComboBoxes.cpp`, Windows-guarded) and
`wx/osx/private.h` (`Utils/MacDarkMode.mm`), come with the regular wx install. Private headers are
port-specific and unversioned. Include one only under the same macro wx uses for that port or
feature, and re-check it whenever the fork is bumped.

### wxInspector

The deps also build wxInspector (`deps/wxInspector/wxInspector.cmake`, compiled with
`-DwxDEBUG_LEVEL=0`). `DPIAware<P>` derives from `wxInspector::wxInspectable`, and its constructor
calls `SetupInspectorAccelerator(this)`. That calls `SetAcceleratorTable` on the window with
`wxACCEL_CTRL | wxACCEL_SHIFT` + `I` (Cmd+Shift+I on macOS), which toggles an inspection frame showing
the window tree (`src/inspector.cpp` in the wxInspector tree); a later `SetAcceleratorTable` call on a
`DPIAware` window replaces the inspector shortcut. `GUI_App::on_init_inner` registers Orca plugins for it
(`RegisterOrcaInspectorPlugins`, `Utils/wxInspectorPlugins/`). The root `CMakeLists.txt` defines
`WXINSPECTOR_DISABLE` when `BBL_RELEASE_TO_PUBLIC` is set true, or, when that variable is not defined,
for the Release configuration; this turns the whole API into no-op stubs. Use it in
Debug/RelWithDebInfo builds to inspect layouts on each platform.

## Platform macros and native handles

| Macro | Defined when | Use for |
|---|---|---|
| `__WXMSW__` | wxMSW build | wx behaviour on Windows, `MSWWindowProc`, MSW-only wx API |
| `__WXOSX__` (also `__WXMAC__`, `__WXOSX_COCOA__`) | wxOSX build | Cocoa-specific wx behaviour |
| `__WXGTK__` | any wxGTK build | GTK/GDK calls, Linux toolkit behaviour |
| `__WXGTK3__` | GTK ≥ 3.0 | GTK3-only API (CSS providers, DPI events, Wayland) |
| `__WXGTK20__` | GTK ≥ 2.0, **also defined in GTK3 builds** (`build/cmake/setup.cmake:61-72` defines every version macro up to the toolkit version) | `defined(__WXGTK20__) \|\| defined(__WXGTK3__)` in `GUI_App.cpp` is the same as `__WXGTK__` |
| `__WINDOWS__` | defined by wx when `_WIN32`, `__WIN32__` or `__WXMSW__` is defined (`include/wx/platform.h:87-91`) | wx's own Windows checks; Orca's dark-mode code uses it |
| `_WIN32`, `__APPLE__`, `__linux__` | compiler | OS APIs (Win32, Cocoa frameworks, `/proc`), and all of `src/libslic3r` |
| `wxHAS_EGL`, `wxHAS_GLX` | wx `setup.h` (`build/cmake/setup.h.in:1144-1147`) when built with EGL / GLX | GL backend code; test them only after a wx header is included |
| `wxHAVE_GDK_WAYLAND`, `wxHAVE_GDK_X11` | Orca's `cmake/modules/FindGTK3.cmake` (`check_symbol_exists(GDK_WINDOWING_WAYLAND/X11 "gdk/gdk.h" …)`), passed by `src/slic3r/CMakeLists.txt` as PRIVATE definitions of `libslic3r_gui` | only `LinuxDisplayBackend.cpp` needs them; wx headers do not define them |
| `GTK_CHECK_VERSION(a,b,c)` | GTK headers, compile time | branching on GTK API version. `gtk_check_version()` and wx's internal `wx_is_at_least_gtk3(n)` check the runtime version |

The toolkit macros come from wx's compile definitions: `wxTOOLKIT_DEFINITIONS` in
`build/cmake/toolkit.cmake:57-87,157`, exported through the CMake targets and through
`wx-config --cxxflags`. They are therefore defined in every `libslic3r_gui` source, even before the
first wx include; `LinuxDisplayBackend.hpp` relies on this. `src/libslic3r` does not link wx and uses
none of them. Prefer the toolkit macro whenever the difference comes from wx: a later toolkit change
(for example wxGTK on another OS) then keeps the right branch.

**Native handles.** `wxWindow::GetHandle()` returns `WXWidget`:
- wxMSW: the `HWND` (`include/wx/msw/window.h:169`).
- wxOSX: the peer's `NSView*`. Reach the `NSWindow` with `[view window]`, as
  `set_miniaturizable(GetHandle())` does.
- wxGTK: `m_widget` (`include/wx/gtk/window.h:140`). For a top-level window this is the `GtkWindow`.
  `MainFrame`, `BBLTopbar` and `DropDown` also use `m_widget` directly; wxGTK declares it in a
  `public:` implementation block (`include/wx/gtk/window.h:291`), so other classes can read another
  window's `m_widget` (`m_frame->m_widget`).

Native calls on these handles bypass wx's bookkeeping. Keep them minimal, guard them with the toolkit
macro, and prefer an existing helper (`GUI_Utils`, `MacDarkMode.mm`, `GUI_UtilsMac.mm`) over new
inline native code.

- **Rule:** Keep the guard on a declaration identical to the guard on its definition (one toolkit
  guard, version branches with `GTK_CHECK_VERSION` inside the definition), and include GTK headers
  under the same guard as the code that uses them.
  **Why:** a header declaring under `__WXGTK3__` while the `.cpp` defines under `__WXGTK__` (or the
  reverse) breaks the build on the other GTK configuration or leaves an undefined symbol. The
  wrong → right shape is under [GTK native chrome](#gtk-native-chrome-and-gtk-size-calls).
  Cite: 477208a969 (`GUI_Utils.hpp` / `GUI_Utils.cpp`).

## Per-platform summaries

Each bullet names the mechanism and the file that owns it.

### MSW (wxMSW)

- **Pixels and DPI:** logical pixels equal physical pixels and `FromDIP` really scales. Per-monitor
  DPI change events need the PMv2 manifest (`src/dev-utils/platform/msw/OrcaSlicer.manifest.in`
  declares `permonitorv2,permonitor`); under the `permonitor` (V1) fallback on older Windows wx
  generates no DPI events, because it accepts only PMv2 [source: `src/msw/nonownedwnd.cpp`
  `IsPerMonitorDPIAware`]. wx rescales min sizes, fonts and sizer borders before
  `wxEVT_DPI_CHANGED` [source], and `DPIAware` does not `Skip()` it. `wxEVT_MOVE_START/END` exist only on MSW
  (DPIAware uses them to defer rescaling while a window is dragged) → `references/dpi-bitmaps-fonts.md`.
- **Dark mode:** `MSWEnableDarkMode(DarkMode_Auto)` runs before `NppDarkMode::InitDarkMode()`. After
  that, `IsDark()` reports the OS apps setting, so `dark_color_mode` is consulted first. The runtime
  dark-mode toggle is Windows-only. Menu bitmaps choose dark variants through `check_dark_mode()` →
  `references/colours-dark-mode.md`.
- **Popups:** the current popup is the wx-internal global `wxCurrentPopupWindow`. Focus changes and
  clicks outside dismiss popups that lack `wxPU_CONTAINS_CONTROLS` (`MSWDismissUnfocusedPopup`);
  popups with it are dismissed on deactivation, deferred through `CallAfter`. No key dismisses a
  popup, and `ProcessLeftDown` is never called. `PopupWindow::BindUnfocusEvent` is MSW-only →
  `references/popups-menus.md` §5.
- **Painting:** in 3.3.2 windows are not double-buffered by default (the 3.3.0 global
  `WS_EX_COMPOSITED` was reverted, `docs/changes.txt:308`). Custom widgets buffer by hand →
  `references/painting-custom-widgets.md`.
- **Building large panels:** every control is a native child window, and outside a sizer pass a move
  or resize is immediate, repainting when the window is shown [source: `src/msw/window.cpp:2036`
  `DoMoveSibling` → `MSWMoveWindowToAnyPosition(..., IsShown())`]; `wxStaticText::SetLabel`/`SetFont`
  resize the control that way (`src/common/stattextcmn.cpp:334` `AutoResizeIfNecessary`). Created
  inside a shown parent, each control re-clips and erases its shown, overlapping siblings, so the
  cost grows with the number already built and hundreds of controls take seconds. Build a large
  panel while its parent is hidden and show it once complete — `LazyPage::Show` builds its panel
  before showing the page for this reason → `references/orca-architecture.md` §Deferred construction.
- **Modal loops:** idle events do not run inside the Windows sizing/moving modal loop, so the 3D
  canvas renders from `on_paint` on MSW (c06a0223a7) → `references/webview-gl-aui-media.md` §GLCanvas3D rendering.
- **Mouse capture:** `wxEVT_MOUSE_CAPTURE_LOST` and `wxEVT_MOUSE_CAPTURE_CHANGED` are delivered →
  `references/mouse-keyboard-focus.md`.
- **Controls:** `wxDataViewCtrl` is the generic implementation (`references/controls-dataview.md`).
  TaskDialog-based dialogs and common dialogs stay light in wx dark mode
  (`interface/wx/app.h:1434-1448`); Orca's `MsgDialog` family is owner-drawn →
  `references/windows-dialogs.md`.
- **WebView:** Edge (WebView2). It needs the runtime (checked by `GUI_App::init_webview_runtime`),
  creates asynchronously, serves custom schemes as `https://<scheme>.wxsite`, and allows one script
  handler → `references/webview-gl-aui-media.md`.
- **Window frame:** custom title bar and non-client handling → [MSW title bar](#msw-the-mainframe-custom-title-bar).

### macOS (wxOSX/Cocoa)

- **Pixels and DPI:** logical pixel = DIP = point. The standard PPI is 72, so `GetDPI()` and
  `GetNewDPI()` are 72-based. `wxEVT_DPI_CHANGED` is generated on backing-scale changes even though
  the docs don't say so [source: `src/osx/cocoa/nonownedwnd.mm` `windowDidChangeBackingProperties`],
  but `DPIAware` binds it only off macOS. `DPIAware` and
  `MainFrame::init_tabpanel` skip `SetFont` on macOS ("name cutting in ObjectList") →
  `references/dpi-bitmaps-fonts.md`.
- **Menus and keys:** there is a native `wxMenuBar`, and Preferences goes into `OSXGetAppleMenu()`.
  Menu key equivalents run before `wxEVT_CHAR_HOOK`, display-only shortcut text uses `" - "`,
  `wxMOD_CONTROL` means Cmd, Ctrl+click arrives as a right click [source], and Cmd+letter char events
  carry the plain letter [source] → `references/mouse-keyboard-focus.md`, `references/popups-menus.md`.
- **Mouse capture:** capture is a wx-level redirect of every left/right button, motion and
  enter/exit event (not wheel or middle-button events), and capture-lost is never sent [source]. A
  leaked capture looks like a frozen UI whose keyboard still works →
  `references/mouse-keyboard-focus.md`.
- **Popups:** a transient popup toggles mouse capture on idle (since 3.1.7) and dismisses on an
  outside click [source]. Since the 3.3 upgrade a hover-opened popup anchored with a gap below its
  opener was dismissed as the cursor crossed the gap (#12936); the wx mechanism was not established,
  and the fix is to anchor flush → `references/popups-menus.md` §3.
- **Controls:** the native `wxDataViewCtrl` (NSOutlineView) never calls `CreateEditorCtrl`, and the
  current item is always selected (`references/controls-dataview.md`). `wxClientDC` cannot draw
  (`references/painting-custom-widgets.md`).
- **Colours:** Orca's sRGB `wxColour` patch. Dark mode follows the system only (`mac_dark_mode()`) →
  `references/colours-dark-mode.md`.
- **WebView:** WKWebView. Handlers must be registered before `Create`, and adding the same script
  handler twice raises an uncatchable NSException → `references/webview-gl-aui-media.md`.
- **Window frame:** a native titled window with a transparent titlebar →
  [macOS frame](#macos-a-native-titled-window).

### GTK3 (X11 and Wayland)

- **Pixels and DPI:** logical pixel = DIP, and the scale is an integer ("fractional scales are rounded
  to the closest integer"). `wxEVT_DPI_CHANGED` needs GTK ≥ 3.10 and wx ≥ 3.3.0, so `DPIAware`'s
  rescale path runs on Linux. `get_dpi_for_window()` is a fixed-96 stub on Linux (and macOS), so
  `em_unit` is measured from the font (`DPIAware::update_em_unit`) →
  `references/dpi-bitmaps-fonts.md`.
- **Dark mode:** follows the system appearance. `GUI_App::on_init_inner` (non-Windows) and
  `update_dark_config` (called from `DPIAware`'s `wxEVT_SYS_COLOUR_CHANGED` handler off Windows)
  overwrite `dark_color_mode` from `wxSystemSettings::GetAppearance().IsDark()` →
  `references/colours-dark-mode.md`.
- **Sizing:** sizer-fitting calls made on a top-level window that is not yet shown are replayed at
  `Show()` (`wxWindow::Fit()` is not), and dialogs collapse without size hints →
  `references/sizers-layout.md`.
- **Native chrome:** GTK theme borders and padding show through custom-drawn controls →
  [GTK native chrome](#gtk-native-chrome-and-gtk-size-calls).
- **Popups:** `Show()` grabs the pointer (`gdk_seat_grab`) [source]. `PopupWindow` dismisses when the
  top-level window is deactivated, and popups are created with `GDK_WINDOW_TYPE_HINT_COMBO` [source] →
  `references/popups-menus.md`.
- **Mouse capture:** a grab-broken event or a modal dialog delivers `wxEVT_MOUSE_CAPTURE_LOST`
  [source; the docs mark the event MSW-only] → `references/mouse-keyboard-focus.md`.
- **Controls:** `wxDataViewCtrl` is the native GtkTreeView (`references/controls-dataview.md`). `Field`
  control pools delete windows on GTK instead of recycling them (`references/orca-settings-ui.md`).
- **WebView:** WebKit2GTK delivers script messages synchronously, with an empty handler name and no
  event object, and navigation events synchronously too [source] → `references/webview-gl-aui-media.md`.
- **GL:** EGL or GLX. Orca calls `PreferGLX()` on X11 → `references/webview-gl-aui-media.md` §EGL vs GLX.
- **App init:** `GUI_App::on_init_inner` sets `gtk-menu-images` to TRUE so menu icons show, and
  installs a `g_log_set_handler("Gtk", G_LOG_LEVEL_CRITICAL, …)` filter. The filter drops known
  harmless criticals (allocation on hidden widgets, events on unrealised widgets, style-context calls
  before realisation), so GTK criticals not on that list still reach the log.
- **Window frame:** borderless with WM-driven move and resize → [GTK frame](#linux-gtk-the-borderless-mainframe).

### GTK2 (opt-out build)

Compile-compatibility only. See [Toolkit per platform](#toolkit-per-platform) for what it loses: EGL,
WebKit2, DIP pixels, DPI events and backend detection. GTK-guarded code must still compile here:
use `GTK_CHECK_VERSION` branches as `RemoveButtonBorder` does. No GTK2 runtime behaviour is supported.

### Wayland (GTK3 native backend)

The protocol gives clients no global pointer or window positions, and the compositor places
top-level windows. GL is EGL only, drawn into a subsurface. Popups are `xdg_popup` surfaces and must
form a chain of parents. Window icons come from the `.desktop` file. The full list is under
[Wayland gaps](#wayland-gaps). These Wayland rules are owned by other files:
- hover handlers must short-circuit, because on compositors that keep hidden-workspace surfaces
  mapped (e.g. Hyprland) GTK sends a stream of synthetic leave events and `IsShownOnScreen()` stays
  true there (69e16cd7ef) →
  `references/mouse-keyboard-focus.md`;
- GL blending must keep destination alpha at 1 (d8369e5f75);
- GL post-init must retry until the surface is committed (d2c24fdabb) →
  `references/webview-gl-aui-media.md`.

### XWayland (GTK3 X11 backend inside a Wayland session)

Users opt in with `GDK_BACKEND=x11…`. GTK then talks X11, so `is_running_on_x11()` is true and Orca
uses GLX through `PreferGLX()`. `CLI::run` prepares this path before GTK starts (PRIME variables,
`XInitThreads()`, no WebKit compositing workaround; the source comment says multi-monitor handling
is compromised there) → [Runtime detection](#runtime-x11wayland-detection),
`references/webview-gl-aui-media.md` §WebKitGTK on Linux sessions. Intel's XWayland GL exposes a
smaller `GL_MAX_TEXTURE_SIZE`, so the ImGui font atlas is re-packed to fit (22e121f4e4) →
`references/webview-gl-aui-media.md`.

## The ifdef landscape

Platform-specific GUI code falls into recurring categories. Most of it sits in `MainFrame`,
`GUI_App`, `GLCanvas3D`, `GUI_ObjectList`, `Plater`, `wxExtensions`, `Field` and `Widgets/AMSItem`;
start there when looking for prior art. The sites below are exemplars, cited by symbol.

**Focus, capture and popup dismissal** (the largest category) → `references/popups-menus.md`,
`references/mouse-keyboard-focus.md`
- `StatusPanel::on_switch_speed`: on `__WXOSX__` the speed popup gets a `nullptr` parent (the source
  comment says "MacOS has focus problem"); elsewhere the parent is the control.
  `popUp->BindUnfocusEvent()` runs only under `__WXMSW__` and binds the top parent's
  `wxEVT_ACTIVATE` / `wxEVT_ICONIZE` / `wxEVT_SHOW` to `Dismiss()`.
- `GLCanvas3D::on_mouse`, `evt.Entering()` branch: on MSW the canvas does not `SetFocus()` while
  `wxCurrentPopupWindow` is non-null. Stealing focus would trigger `MSWDismissUnfocusedPopup` and
  close the search dropdown. `wxCurrentPopupWindow` is a wx-internal global (`src/msw/popupwin.cpp`)
  that `GLCanvas3D.cpp` declares `extern` itself, which works only because wx is linked statically.
  `SearchDialog` and `SearchObjectDialog` override the virtual `MSWDismissUnfocusedPopup`.
- `GLCanvas3D::on_mouse`: the MSW "on_enter workaround" (comment "SPE-832") handles a spurious mouse
  event that arrives before `evt.Entering()`; `m_mouse.position` is reset at the end of the function.
- `SearchObjectDialog::Popup`: on `__WXOSX__`, focus moves to `m_object_list` before
  `PopupWindow::Popup`, otherwise the text input becomes unusable.
- `SearchDialog::Dismiss`, `SearchObjectDialog::Dismiss`: on Wayland they dismiss by focus tracking
  (`focus_left_popup(...)`) instead of hit-testing `wxGetMousePosition()`.
- `PopupWindow::Create`: on GTK it binds the top-level `wxEVT_ACTIVATE` to `topWindowActiavate` →
  `DismissAndNotify()`, gated by the virtual `ShouldDismissOnTopWindowDeactivate()`, which `DropDown`
  overrides for Wayland popup chains. On `__WXOSX__` with `wxPU_CONTAINS_CONTROLS`,
  `PopupWindow::OnMouseEvent2` hit-tests children, re-dispatches mouse events and synthesises
  enter/leave.
- `SidePopup::Popup` (`Widgets/SideMenuPopup.cpp`): on `__APPLE__` the menu is anchored flush against
  the button with a slight overlap. Since the wx 3.3 upgrade, the transient popup was dismissed as
  soon as the cursor entered the gap (#12936, 9a053f15eb). The wx mechanism was not established
  (`references/popups-menus.md` §3): [source] `wxPopupTransientWindow::OnIdle` captures the mouse
  whenever the cursor is outside the popup rect and releases it inside
  (`src/common/popupcmn.cpp:438-471`, a 3.1.7 addition), and `wxPopupWindowHandler::OnLeftDown`
  dismisses only on an outside click (`popupcmn.cpp:536+`), so neither explains a dismissal on hover.

**Menu bar and accelerators** → `references/mouse-keyboard-focus.md`, `references/popups-menus.md`
- `MainFrame::MainFrame`: `#ifndef __APPLE__` creates `m_topbar = new BBLTopbar(this)`, a
  `wxAuiToolBar` with `BBLTopbarArt : wxAuiDefaultToolBarArt`. macOS gets a plain `wxPanel` top area
  plus the native `wxMenuBar` that `MainFrame::init_menubar_as_editor` sets with `SetMenuBar`.
  Preferences is added with `append_shortcut_item(..., Shortcut::Preferences, ...)` into
  `OSXGetAppleMenu()` on macOS and into `m_topbar->GetTopMenu()` elsewhere.
- `MainFrame::shortcut_label`: items registered with `accelerator=true` whose binding is menu-safe
  (`ShortcutRegistry::accelerator()` non-empty) get `"\t" + accelerator`. That is a live key
  equivalent only in the macOS `wxMenuBar`; the Windows/Linux menus are popped up from `BBLTopbar`,
  where accelerators are display-only and the registry dispatches the keys. Display-only shortcut
  text uses the static `sep`, which is `" - "` on macOS and `"\t"` elsewhere, because the native menu
  bar would otherwise grab keys that must reach text fields (#8152).
- `KeyChord::from_event` normalises char events: control codes 1–26 with `ControlDown()` become
  `'A'..'Z'`, and lowercase becomes uppercase. Match shortcuts through it, never through raw char
  codes for Ctrl/Cmd+letter. [source] `wxOSXTranslateCocoaKey` produces `WXK_CONTROL_A+n` only when
  the physical Ctrl is held (`src/osx/cocoa/window.mm:305-307`), so a Cmd+letter char event carries
  the plain letter. Control-code translation is documented at `interface/wx/event.h:1408-1421`.
- `MainFrame::MainFrame`, `wxEVT_CHAR_HOOK` lambda under `__APPLE__`: Cmd+H is swallowed, Cmd+M calls
  `Iconize()`, Cmd+Q posts `wxEVT_CLOSE_WINDOW`, and Cmd+Ctrl+F calls `EnableFullScreenView(true)` and
  toggles `ShowFullScreen` (`interface/wx/toplevel.h:700-728`, OSX only). Everything else goes to
  `handle_global_shortcut(KeyChord::from_event(evt))`. Cmd+, is the registry's
  `Shortcut::Preferences`, not part of the hook.
- `ObjectList::update_shortcut_accelerators`: the native macOS data view gets no key events, so on
  macOS a `wxAcceleratorTable` is generated from the shortcut registry.

**Window decoration / custom title bar** → [Window decoration](#window-decoration-and-custom-title-bars)
- `MainFrame::MainFrame`: `set_miniaturizable` (OSX); `m_gdkDecor = 0` and three `ResizeEdgePanel`s
  (GTK); the `WS_CAPTION` strip (MSW). `MainFrame::MSWWindowProc` and `AdjustWorkingAreaForAutoHide`
  (MSW). `BBLTopbar::OnMouseLeftDown`, `BBLTopbar::OnFullScreen`, `BBLTopbar::MSWWindowProc`.
  `SplashScreen::SplashScreen` (Wayland).

**Fonts, sizes, Retina** → `references/dpi-bitmaps-fonts.md`
- The `DPIAware` constructor and `MainFrame::init_tabpanel`: `#ifndef __WXOSX__` around `SetFont`,
  "to avoid name cutting in ObjectList".
- `OG_CustomCtrl::CtrlLine::draw_text`: works around the Big Sur bold-font issue. Focused URL labels
  are drawn underlined instead of bold and underlined.
- `SwitchButton::Rescale`: on `__WXOSX__` the measuring font is scaled by `mac_max_scaling_factor()`.
  That helper reads screen 0's backing factor (`[[NSScreen screens] objectAtIndex:0]`, the menu-bar
  screen) on every loop iteration, so despite its name it returns that screen's factor (at least 1),
  not the maximum over all screens.
- `BitmapComboBox` (comment block under `#ifdef __APPLE__` in `BitmapComboBox.hpp`): the bitmap
  `scale` argument means "the image is already sized for that backing scale". The `scale` parameter
  of `wxBitmap(const wxImage&, int depth, double scale)` is not in the interface docs. It exists on
  every port (`include/wx/osx/bitmap.h:115`, `include/wx/gtk/bitmap.h:77`), but wxMSW ignores it
  (`include/wx/msw/bitmap.h:68`). The portable APIs are `CreateWithDIPSize` and `SetScaleFactor`
  (`interface/wx/bitmap.h:491-520, 966`).

**GTK / Wayland** → this file, `references/webview-gl-aui-media.md`, `references/popups-menus.md`
- `CLI::run` (`src/OrcaSlicer.cpp`): sets backend-related environment variables before GTK starts.
- `GUI_App::on_init_inner`: calls `wxGLCanvas::PreferGLX()` when `is_running_on_x11()`, under
  `#if defined(__WXGTK__) && wxHAS_EGL`.
- `OpenGLManager::detect_multisample`: on Wayland without `wxHAS_EGL` it skips `IsDisplaySupported()`,
  which would go through GLX and crash on a missing X11 display. Multisampling is also off on ChromeOS
  (`PlatformFlavor::LinuxOnChromium`).
- `OpenGLManager::init_gl`: loads GLAD through `eglGetProcAddress` on Wayland.
- `DropDown::messureSize`: positive-size `gtk_window_resize` on the GTK wrapper window, plus an idle
  poll that synthesises `mouseMove` on the main dropdown while a submenu holds the grab (Mutter drops
  motion events outside the grabbing surface). `DropDown::mouseMove` sets the submenu
  `gtk_window_set_transient_for` to the mapped main popup at show time. `DropDown::Popup` gives
  data-view cell editors an explicit top-level transient parent.
  `DropDown::ShouldDismissOnTopWindowDeactivate` handles Wayland chains.
- `CheckBox::CheckBox`, `SwitchButton`, `RadioBox`, `ScalableButton` (`wxExtensions.cpp`),
  `PresetComboBoxes` call `RemoveButtonBorder`; `TextInput` and `SpinInput` call
  `RemoveInputBorder` on their inner `wxTextCtrl`.
- `Plater::priv::priv` together with `sanitize_window_layout_for_wayland`: AUI floating is disabled on
  Wayland.
- `GUI_App::window_pos_restore`: skips `SetPosition` on Wayland.
- `BBLTopbar::OnMouseLeftDown` / `OnMouseMotion` convert event coordinates with `ClientToScreen`
  instead of calling `wxGetMousePosition()`. `BBLTopbar::FindToolByCurrentPosition` returns null on
  Wayland when the last event position is unknown or outside the bar.
- `LinuxDisplayBackend.{hpp,cpp}`: runtime X11/Wayland detection.

**Windows rendering and dark mode** → `references/colours-dark-mode.md`,
`references/painting-custom-widgets.md`
- `_MSW_DARK_MODE` is `#define`d to 1 **on every platform** (`GUI_App.hpp`), so it is not a platform
  gate. The MSW-only calls inside those blocks sit under `__WINDOWS__` / `_WIN32`.
  `dark_mode.cpp/.hpp` and `dark_mode/{dark_mode,IatHook,UAHMenuBar}.hpp` (the vendored Notepad++
  dark mode, `NppDarkMode`) are compiled only under `if (WIN32)` in `src/slic3r/CMakeLists.txt`.
  `SUPPORT_DARK_MODE` (`libslic3r/AppConfig.hpp`) gates `GUI_App::dark_mode`.
- `AMSExtText::render`: on `__WXMSW__` it blits the paint DC into a bitmap and renders through a
  `wxGCDC` over the `wxMemoryDC` (GDI+ anti-aliasing), then `DrawBitmap`s the result. Other ports call
  `doRender(dc)` directly. `StaticBox::render` uses a variant (rounded boxes only) that clears the
  bitmap with the background colour instead of blitting. This is the recurring MSW pattern in custom
  widgets.
- `GLCanvas3D::on_paint`: renders immediately on MSW (c06a0223a7).

**Native data view** → `references/controls-dataview.md`
- `GUI_ObjectList.cpp` has `__WXOSX__` editing and model paths, because the native macOS control never
  calls a custom renderer's `CreateEditorCtrl`.

**macOS Objective-C++ glue** — `.mm` files listed in the `APPLE` block of `src/slic3r/CMakeLists.txt`,
for example:
- `Utils/MacDarkMode.mm`: `mac_dark_mode`, `mac_max_scaling_factor`, `set_miniaturizable`,
  `set_title_colour_after_set_title`, the `WKWebView_*` helpers, `initGestures`.
- `GUI/GUI_UtilsMac.mm`: `dataview_remove_insets`, `staticbox_remove_margin`,
  `set_window_corner_radius`, declared under `__WXOSX__` in `GUI_Utils.hpp`.
- `GUI/DeepLinkHandlerMac.mm`, `GUI/InstanceCheckMac.mm`, `GUI/Mouse3DHandlerMac.mm`,
  `GUI/RemovableDriveManagerMM.mm`, `Utils/RetinaHelperImpl.mm`. (`libslic3r/MacUtils.mm` is registered
  in `src/libslic3r/CMakeLists.txt` instead.)

Trackpad gestures: `GLCanvas3D::bind_event_handlers` calls the portable
`EnableTouchEvents(wxTOUCH_ZOOM_GESTURE | wxTOUCH_ROTATE_GESTURE)` plus
`initGestures(m_canvas->GetHandle(), m_canvas)` for the pan recogniser, and `unbind_event_handlers`
calls `initGestures(..., nullptr)`. Deep links: `GUI_App::on_init_inner` → `register_mac_deep_link_handler()`
re-registers the `kAEGetURL` handler after wx installs its own (1f2ed70288, #13119).

**WebView and media** → `references/webview-gl-aui-media.md`
- `WebView::CreateWebView`: `WebViewEdge` on Windows, `WebViewWebKit` on macOS, `wxWebView::New()` on
  Linux.
- Camera: `wxMediaCtrl3`, a plain `wxWindow` that decodes on a worker thread and paints a `wxBitmap`
  frame on Win32 and a `wxImage` frame elsewhere. No per-platform native player sits
  behind it; new camera sources implement `IMediaController` (97955dbab8 and 7e3724b5f3 removed the
  `wxMediaCtrl2.cpp/.mm` players).

## Runtime X11/Wayland detection

**API** (`src/slic3r/GUI/LinuxDisplayBackend.hpp`, declared only `#if defined(__WXGTK__)`, namespace
`Slic3r::GUI`):

```cpp
enum class LinuxDisplayBackend { X11, Wayland, Unknown };
LinuxDisplayBackend get_linux_display_backend();   // "Must be called after gtk_init() / wxWidgets initialization."
bool is_running_on_wayland();
bool is_running_on_x11();
```

**Mechanism.** `get_linux_display_backend()` tests `gdk_display_get_default()` with
`GDK_IS_WAYLAND_DISPLAY` and then `GDK_IS_X11_DISPLAY`. Each test is compiled only when
`wxHAVE_GDK_WAYLAND` / `wxHAVE_GDK_X11` is defined. The result is cached in a function-local static
on the **first** call. So:
- Called before GTK has a display, it caches `Unknown` for the rest of the process.
- In a GTK2 build neither macro is defined, so it always returns `Unknown`.
- `Unknown` makes both predicates false. Write every branch so that "neither" is safe. For example,
  `GUI_App::on_init_inner` logs "Unknown display backend, defaulting to EGL" and does not call
  `PreferGLX()`.

**Usage.**
```cpp
#ifdef __WXGTK__
#include "LinuxDisplayBackend.hpp"
#endif
...
#if defined(__WXGTK__)
    if (Slic3r::GUI::is_running_on_wayland()) {
        // Wayland-only path
    }
#endif
```

**Before GTK starts**, only the environment exists. `CLI::run` (`src/OrcaSlicer.cpp`) branches on
`GDK_BACKEND` (an `x11` prefix means the X11 opt-in). On the default path it forces `GDK_BACKEND=x11`
when wx lacks EGL and `WAYLAND_DISPLAY` is set. It sets `WEBKIT_DISABLE_COMPOSITING_MODE=1`
(non-replacing) only when both `DISPLAY` and `WAYLAND_DISPLAY` are set, and calls `XInitThreads()`
only when `DISPLAY` is set. With neither variable set, the GUI refuses to start ("Neither DISPLAY nor
WAYLAND_DISPLAY set"). The details and the WebKit rule (c12912e0df) are in
`references/webview-gl-aui-media.md` §WebKitGTK on Linux sessions.

**wx's own detectors**, for reference:
- `wxGetDisplayInfo()` (`include/wx/utils.h:731-750`, public header but not in the interface docs)
  returns `wxDisplayX11` / `wxDisplayWayland` / `wxDisplayNone` and the native display.
- `wxGTKImpl::IsWayland` / `IsX11` (`include/wx/gtk/private/backend.h`, GTK3 only) are private, and
  each caches the answer of its first call.

Orca standardises on `LinuxDisplayBackend`, which keeps GDK headers out of callers.

**Pitfalls**
- **Rule:** Decide the backend with the GDK type check (`is_running_on_wayland()` /
  `is_running_on_x11()`), not with `WAYLAND_DISPLAY` / `GDK_BACKEND` in GUI code. Environment
  variables are appropriate only before GTK initialises.
  **Why:** environment variables describe the session, not the backend GTK actually picked. An
  XWayland run has both `DISPLAY` and `WAYLAND_DISPLAY` set while GTK talks X11, and a native Wayland
  run usually has `DISPLAY` set too (XWayland available). GUI decisions keyed on the environment
  misfire in both cases.
  ```cpp
  // Wrong: GUI code reading the session environment
  if (getenv("WAYLAND_DISPLAY"))
      m_aui_mgr.SetFlags(m_aui_mgr.GetFlags() & ~wxAUI_MGR_ALLOW_FLOATING);
  // Right (shape of Plater::priv::priv)
  #if defined(__WXGTK__)
  if (Slic3r::GUI::is_running_on_wayland())
      m_aui_mgr.SetFlags(m_aui_mgr.GetFlags() & ~wxAUI_MGR_ALLOW_FLOATING);
  #endif
  ```
  Cite: 1b71835337 (`GUI_App.cpp`), `src/slic3r/GUI/LinuxDisplayBackend.cpp`.
- **Rule:** Never call the detectors from static initialisers or before `wxEntry` has initialised GTK.
  **Why:** `gdk_display_get_default()` is null then, and the function-local cache keeps `Unknown`
  forever, which silently disables every Wayland or X11 branch.

## Wayland gaps

wx has no dedicated Wayland document; the documented limits are scattered across the interface
headers. Rows marked [source] or "protocol" come from the implementation or from Wayland itself.

| Area | What happens on Wayland | Cite | Orca handling / owner |
|---|---|---|---|
| Global pointer position | `wxGetMousePosition()` / `wxGetMouseState()` call `gdk_device_get_position` [source], but Wayland gives clients no global pointer position, so the result is not a screen position | `src/gtk/window.cpp` `wxGetMousePosition` | use event coordinates + `ClientToScreen` (`BBLTopbar`); dismiss popups by focus tracking (`SearchDialog::Dismiss`) → `references/mouse-keyboard-focus.md` |
| Top-level position | the compositor places windows, and `SetPosition()` / `Move()` on a TLW have no effect (protocol) | — | `GUI_App::window_pos_restore` restores only size and maximised state; moves go through `gtk_window_begin_move_drag` |
| Window icon | `SetIcon()` / `SetIcons()` "doesn't do anything when using Wayland … create a `.desktop` file" | `interface/wx/toplevel.h:517-521, 538-542` | `src/dev-utils/platform/unix/com.orcaslicer.OrcaSlicer.desktop` (`Icon=OrcaSlicer`, `StartupWMClass=orca-slicer`) |
| App id | `wxAppConsole::SetClassName()` is the xdg `app_id` with GTK ≥ 3.24.22 (and the AUMID on Windows); it must be set before any TLW. wx applies it when a TLW is mapped, and only if it is non-empty [source: `wxTopLevelWindowGTK::GTKHandleMapped`] | `interface/wx/app.h:765-812` | Orca calls only `SetAppName(SLIC3R_APP_KEY)`, so GTK's default applies. On Windows, `SetClassName` would also change shell behaviour (MRU, Shift+middle-click) |
| `wxClientDC` | deprecated in 3.3 ("please use wxInfoDC instead for obtaining information", `interface/wx/dcclient.h:43-46`). Drawing through it "simply doesn't have any effect" on GTK3/Wayland or wxOSX. `CanBeUsedForDrawing()` [source]: false on Wayland only for wxGTK (`src/gtk/dc.cpp`), always false on wxOSX, always true on wxMSW (`include/wx/{osx,msw}/dcclient.h`), although its doc also lists "wxMSW when using double buffering" | `interface/wx/dcclient.h:48-53, 80-88` | draw only in `wxPaintDC` after `Refresh()`/`RefreshRect()` → `references/painting-custom-widgets.md` |
| `wxWindow::Update()` | "doesn't do anything in wxGTK port when using Wayland". [source] wx skips the GDK update calls there because they broke later updates (#25036) | `interface/wx/window.h:2405-2407` | never rely on `Update()` to paint synchronously |
| `WarpPointer()` | works only if the compositor implements the pointer-warp protocol; mutter also needs a mouse button held | `interface/wx/window.h:3902-3907`; `docs/changes.txt:294` | Orca never warps the pointer |
| `wxUIActionSimulator` | "currently doesn't work when using Wayland with wxGTK" | `interface/wx/uiaction.h:20` | not used; Orca also builds `wxUSE_XTEST=OFF` |
| `wxBitmap(const wxCursor&)` | creates an invalid bitmap | `interface/wx/bitmap.h:393-395` | — |
| OpenGL | only EGL; `PreferGLX()` has no effect. Without EGL in the build, wxGTK's `wxGLCanvas` shows a fatal message and refuses to work [source: `src/gtk/glcanvas.cpp` `IsAvailable`]. The EGL surface is a subsurface over the canvas, ready only after map and a frame callback [source: `src/unix/glegl.cpp`] | `interface/wx/glcanvas.h:1094-1095` | `CLI::run` forces X11 when wx lacks EGL; overlays are drawn in GL/ImGui, never as wx children over the canvas → `references/webview-gl-aui-media.md` |
| AUI | the doc note "live resize is always used … for wxOSX and wxGTK3 when using Wayland" is obsolete: "As of wxWidgets 3.3.0 this function always returns false", and `wxAUI_MGR_LIVE_RESIZE` is in the default flags. Floating panes need global positions | `interface/wx/aui/framemanager.h:336-345` | `Plater::priv::priv` clears `wxAUI_MGR_ALLOW_FLOATING`; `sanitize_window_layout_for_wayland` strips floating state from the saved layout → `references/webview-gl-aui-media.md` §wxAuiManager |
| Popups | a GTK popup is an `xdg_popup` only for COMBO/DROPDOWN/POPUP_MENU hints, so wx creates popups with `GDK_WINDOW_TYPE_HINT_COMBO` [source]. A chained popup's parent must be the mapped popup, and mapping it with a grab deactivates the toplevel (Orca's comments in `DropDown::mouseMove`, `DropDown::ShouldDismissOnTopWindowDeactivate`) | `src/gtk/popupwin.cpp:110-114` | `DropDown` transient-for chain, `ShouldDismissOnTopWindowDeactivate` → `references/popups-menus.md` |
| Fractional scale | arrives as an integer GDK scale | `docs/doxygen/overviews/high_dpi.md:348-351` | → `references/dpi-bitmaps-fonts.md` |
| Window decorations | some desktop environments draw a title bar on undecorated windows anyway | — | [Undecorated windows](#wayland-undecorated-top-level-windows-splash) |
| `wxWebViewChromium` | X11 only | `interface/wx/webview_chromium.h:133-146` | not built (`wxUSE_WEBVIEW_CHROMIUM` OFF) |

Wayland history in the change logs (`docs/changes_32.txt`): already in 3.1.5, Orca's previous wx,
were two-finger scrolling (703, 3.1.3), the EGL-based `wxGLCanvas` for Wayland (497) and `wxMediaCtrl`
support (498, both 3.1.5). New with the upgrade: "Many bug fixes for Wayland-specific problem" (397)
and a `wxMediaCtrl` fix (402) in 3.1.6, GDK errors from `PopupMenu()` avoided (317, 3.1.7),
`wxCURSOR_SIZING` fixed (261, 3.2.0). In 3.3, `WarpPointer()` on supported compositors
(`docs/changes.txt:294`) and the EGL/Wayland high-DPI scale fix (`changes.txt:538`).

## Window decoration and custom title bars

Only `MainFrame` replaces the native title bar. Dialogs keep native decorations, chosen through their
style flags (`references/windows-dialogs.md`). `MainFrame` is created with

```cpp
#ifndef __APPLE__
#define BORDERLESS_FRAME_STYLE (wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
#else
#define BORDERLESS_FRAME_STYLE (wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
#endif
```

There is no `wxCAPTION` on any platform; each port then needs its own handling.

### MSW: the MainFrame custom title bar

**Contract** [source + documented]. Since wx **3.3.0**, `wxTopLevelWindowMSW::MSWGetStyle` adds
`WS_CAPTION` whenever any of `wxCAPTION | wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX` is set
(`src/msw/toplevel.cpp:133-135`). The 3.3.0 wxMSW change list says "Turn wxCAPTION on automatically if
required by other styles (#23575)" (`docs/changes.txt:581`). Commit eefdabcd98 attributes this to
3.3.2, but it is a 3.3.0 change. `SetWindowStyleFlag()` recomputes the native style through
`MSWGetStyle` and turns the bits back on (`src/msw/window.cpp` `wxWindowMSW::MSWUpdateStyle`).

**OrcaSlicer design** (`MainFrame.cpp`, all under `__WXMSW__`):
- `MainFrame::MainFrame` strips `WS_CAPTION` with `SetWindowLongPtr` and applies it with
  `SetWindowPos(..., SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)`.
  Without it Windows 10 showed the native frame behind the custom title bar, a "double window"
  (f70d30bf79, #13074).
- `MainFrame::MainFrame` also binds `wxEVT_MAXIMIZE`: the handler clamps the frame to the display's
  client area plus the border overshoot (`AdjustWindowRectEx`), moves it there and `Skip()`s, so a
  maximised frame does not overlap the taskbar (restored by f70d30bf79).
- `MainFrame::MSWWindowProc`:
  - `WM_NCACTIVATE`: sets `lParam = -1` so `DefWindowProc` does not repaint the non-client area, while
    the window still receives activation.
  - `WM_NCCALCSIZE` with `wParam` TRUE: computes the border with
    `AdjustWindowRectEx(&r, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, 0)` and insets
    left, right and bottom by it. When not maximised, the top grows by 1 px so the window can be
    resized from its top edge. When maximised, the top is inset by the full border, because Windows
    extends a maximised window beyond the screen by the border thickness. Then it returns 0.
  - `WM_NCHITTEST`: returns `HTCAPTION` when maximised. Otherwise, over the top bar, points within the
    border thickness give `HTTOP` / `HTTOPLEFT` / `HTTOPRIGHT` / `HTLEFT` / `HTRIGHT`, and the rest of
    the bar gives `HTCAPTION`.
  - `WM_GETMINMAXINFO`: `HandleGetMinMaxInfo` + `AdjustWorkingAreaForAutoHide`, which keeps a
    maximised window off an auto-hide taskbar (#8085) and also masks `WS_CAPTION`.
- `BBLTopbar::MSWWindowProc` returns `HTTRANSPARENT` for `WM_NCHITTEST` over empty bar areas and the
  title, and `CenteredTitle::MSWWindowProc` always returns it. The frame's `HTCAPTION` answer then
  provides native dragging, double-click maximise and Snap. `BBLTopbar::OnMouseLeftDown` does
  `CaptureMouse(); ReleaseMouse();` and posts `WM_NCLBUTTONDOWN` with `HTCAPTION`.

**Pitfall**
- **Rule:** For a Windows frame with a custom title bar, handle `WM_NCCALCSIZE` yourself. Mask
  `WS_CAPTION` out of the style passed to every `AdjustWindowRectEx`
  (`GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION`). In the maximised branch, strip the full border
  overshoot on all four sides instead of returning early.
  **Why:** wx adds `WS_CAPTION` whenever a min/max/close box is requested, and puts it back when the
  style is recomputed. Letting `DefWindowProc` (or an unmasked `AdjustWindowRectEx`) compute the
  non-client area subtracts a caption you draw yourself, so a maximised window leaves a gap above the
  taskbar.
  ```cpp
  // Wrong: caption height included; maximised case left to DefWindowProc
  AdjustWindowRectEx(&b, GetWindowLongPtr(hWnd, GWL_STYLE), FALSE, 0);
  if (wPos.showCmd == SW_SHOWMAXIMIZED) break;
  // Right
  AdjustWindowRectEx(&b, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, 0);
  b.left *= -1; b.top *= -1;
  sz->rgrc[0].top += (wPos.showCmd == SW_SHOWMAXIMIZED) ? b.top : 1;
  sz->rgrc[0].left += b.left; sz->rgrc[0].right -= b.right; sz->rgrc[0].bottom -= b.bottom;
  return 0;
  ```
  Cite: eefdabcd98 (`MainFrame::MSWWindowProc`).

### Linux (GTK): the borderless MainFrame

**wx mechanism** [source, `src/gtk/toplevel.cpp`]:
- At creation, wx turns the style into WM hints (`m_gdkDecor`, `m_gdkFunc`, :880-925).
  `wxBORDER_NONE` / `wxSIMPLE_BORDER` call `gtk_window_set_decorated(false)`.
- On Wayland with GTK ≥ 3.10, a bordered window without `wxCAPTION` gets a `gtk_header_bar_new()`
  titlebar (:900-906); `BORDERLESS_FRAME_STYLE` is such a style.
- On realise, `GTKHandleRealized` calls `gdk_window_set_decorations(window, m_gdkDecor)` (:400-444).
  When a client-side titlebar exists it first sets `m_gdkDecor = 0`, because "Don't set WM
  decorations when GTK is using Client Side Decorations".

**OrcaSlicer design:**
- **No WM decorations.** `MainFrame::MainFrame` sets `m_gdkDecor = 0` (a `public:` implementation
  member of `wxTopLevelWindowGTK`, `include/wx/gtk/toplevel.h:108-109`) before the frame is
  realised, so the window manager draws no title bar, while `m_gdkFunc` keeps move, resize, minimise,
  maximise and close. `BBLTopbar` is the only title bar (d50b4cbf3d, #12600 "No more double title
  bar").
- **Move.** `BBLTopbar::OnMouseLeftDown`, on empty bar areas and the title, calls
  `gtk_window_begin_move_drag(GTK_WINDOW(m_frame->m_widget), 1, x, y, gtk_get_current_event_time())`
  with `x, y` taken from `ClientToScreen(event.GetPosition())`. This is a compositor-driven move, and
  on Wayland it is the only way a client can move its window.
- **Maximise.** `BBLTopbar::OnFullScreen`, which also handles a double-click on the title, uses
  `gtk_window_is_maximized` / `gtk_window_maximize` / `gtk_window_unmaximize` on GTK.
- **Resize.** Three `ResizeEdgePanel`s (bottom, left, right; `BORDER_PX` = 5): transparent `wxPanel`s
  (`wxBG_STYLE_TRANSPARENT`, empty `wxPaintDC` paint) that are `Raise()`d above all siblings, so their
  GDK windows receive pointer events even over WebKit2GTK or GL surfaces. On motion they set a named
  cursor (`gdk_cursor_new_from_name`, `"s-resize"`, `"sw-resize"`, …). On left-down they call
  `gtk_window_begin_resize_drag` with the matching `GdkWindowEdge`, unless the frame is maximised or
  fullscreen. `MainFrame::update_edge_panels` hides them while maximised or fullscreen, lays them out
  along the client edges and raises them again. `MainFrame::shutdown` clears the pointers; wx
  destroys the panels as children. They use event-relative coordinates, not a global event filter
  keyed on `wxGetMousePosition()`, which cannot work on Wayland (6049c6e234, #12705).

**Pitfall**
- **Rule:** Move and resize the borderless frame through `gtk_window_begin_move_drag` /
  `gtk_window_begin_resize_drag` with the current event time. Do not capture the mouse and call
  `Move()` / `SetSize()` from global coordinates.
  **Why:** on Wayland, clients cannot position top-level windows and get no global pointer position,
  so a manual drag does nothing or jumps. On X11, the WM-driven drag also gives edge snapping and
  correct multi-monitor behaviour.
  ```cpp
  // Wrong (GTK): manual drag
  CaptureMouse(); /* on motion: */ m_frame->Move(::wxGetMousePosition() - m_delta);
  // Right (GTK)
  wxPoint p = ClientToScreen(event.GetPosition());
  gtk_window_begin_move_drag(GTK_WINDOW(m_frame->m_widget), 1, p.x, p.y, gtk_get_current_event_time());
  ```
  Cite: `BBLTopbar::OnMouseLeftDown`; `ResizeEdgePanel::OnLeftDown` (`MainFrame.cpp`).

### macOS: a native titled window

- [source] wxOSX gives a window `NSTitledWindowMask` as soon as any of `wxMINIMIZE_BOX`,
  `wxMAXIMIZE_BOX`, `wxCLOSE_BOX`, `wxSYSTEM_MENU` or `wxCAPTION` is set, and adds the
  miniaturizable, resizable and closable masks for the matching boxes
  (`src/osx/cocoa/nonownedwnd.mm:816-831`). `BORDERLESS_FRAME_STYLE` therefore still produces a titled
  window with the standard window buttons. `wxRESIZE_BORDER` is left out on Apple, and the window is
  still resizable because `wxMAXIMIZE_BOX` already adds `NSResizableWindowMask`.
- `set_miniaturizable(GetHandle())` (`Utils/MacDarkMode.mm`, called in `MainFrame::MainFrame` under
  `__WXOSX__`) sets `titlebarAppearsTransparent`, sets a dark window background colour, ORs in
  `NSMiniaturizableWindowMask`, and remembers the title `NSTextField`.
  `set_title_colour_after_set_title` re-colours that field white after the title changes.
- There is no `BBLTopbar` on macOS: the top area is a plain `wxPanel`, and the menus live in the
  native `wxMenuBar`.
- Fullscreen: Cmd+Ctrl+F calls `EnableFullScreenView(true)` and toggles `ShowFullScreen()`.
  `EnableFullScreenView` is OSX-only, and the full-screen button is needed for the animated
  fullscreen space (`interface/wx/toplevel.h:700-728`).

### Wayland: undecorated top-level windows (splash)

- **Rule:** To show a truly undecorated top-level window (the splash screen) on Wayland, don't rely on
  wx style flags or window-type hints. Install an empty client-side titlebar and disable decoration
  on the GTK window in the constructor, before control returns to the event loop. [source] The
  `wxSplashScreen` base constructor has already called `Show(true)` (`src/generic/splash.cpp`), so in
  `SplashScreen` these calls land right after the show request and before any event is processed; in
  a window you show yourself, make them before `Show()`:
  ```cpp
  #if defined(__WXGTK__)
  if (Slic3r::GUI::is_running_on_wayland()) {
      GtkWidget* empty = gtk_fixed_new();
      gtk_widget_set_size_request(empty, 0, 0);
      gtk_window_set_titlebar(GTK_WINDOW(GetHandle()), empty);
      gtk_window_set_decorated(GTK_WINDOW(GetHandle()), false);
  }
  #endif
  ```
  **Why:** some Wayland desktop environments ignore splash-typed window properties (wxGTK's
  `wxSplashScreen` sets `GDK_WINDOW_TYPE_HINT_SPLASHSCREEN`) and draw a title bar anyway. This
  happens even though `wxBORDER_NONE` already makes wx call `gtk_window_set_decorated(false)`. Forcing an empty client-side titlebar removes it on every
  desktop environment.
  Cite: 1b71835337 (`SplashScreen::SplashScreen` in `GUI_App.cpp`; the splash style is
  `wxBORDER_NONE | wxFRAME_NO_TASKBAR`, plus `wxSTAY_ON_TOP` on Apple).

## GTK native chrome and GTK size calls

**Helpers** (`GUI_Utils.hpp`, declared under `__WXGTK__`):
- `RemoveButtonBorder(wxWindow*)` is "for wxButton/wxBitmapToggleButton based controls (SwitchButton,
  CheckBox)".
- `RemoveInputBorder(wxWindow*)` is "for TextCtrl based controls (TextInput, ComboBox, SpinInput..)".

Both return immediately if `GetHandle()` is null, so call them after the control is created; the
widgets do it in their constructors.
- **GTK3** (and the GTK4 branch): a `GtkCssProvider` is added to the widget's own style context at
  `GTK_STYLE_PROVIDER_PRIORITY_USER`, then released with `g_object_unref` (the context keeps its
  reference). The CSS covers `button, button:hover, button:active, button:focus`, or
  `entry, entry text, entry undershoot` for inputs, and zeroes `border`, `outline`, `box-shadow`,
  `padding`, `margin`, `min-height` and `min-width`, with `background: none`.
- **GTK2**: `gtk_rc_parse_string` installs a **global** rc style keyed by widget path or class
  (`"*.GtkBitmapToggleButton"`, class `"GtkEntry"`). The first call changes every matching widget in
  the process, not just the one passed in.

**Callers:** `CheckBox::CheckBox`, `SwitchButton`, `RadioBox`, `ScalableButton` and
`PresetComboBoxes` (`RemoveButtonBorder`); the inner `wxTextCtrl` of `TextInput` and `SpinInput`
(`RemoveInputBorder`). A new owner-drawn control built on a native GTK widget needs the same call.

**Sizing a bitmap button on GTK.** A `wxBitmapToggleButton`/`wxButton`-based widget sized to exactly its
bitmap leaves no room for the theme's CSS padding, and GTK logs "negative content width" criticals.
Either strip the button CSS with `RemoveButtonBorder` and size to the bitmap (`CheckBox::Rescale`), or
size to `GetBestSize()` grown to the bitmap (`IncTo`; `RadioBox::Rescale` and `SwitchButton::Rescale`
do both). Cite: 6148ba16b3, 988b500f33.

**Pitfalls**
- **Rule:** To strip native GTK control chrome (entry and button borders, padding), attach a
  `GtkCssProvider` at `GTK_STYLE_PROVIDER_PRIORITY_USER` to the widget's style context, and include the
  pseudo-class states (`button, button:hover, button:active, button:focus { ... }`), plus the inner
  subnodes for entries (`entry, entry text, entry undershoot`). Guard the code with `#ifdef __WXGTK__`,
  branch GTK2/3/4 with `GTK_CHECK_VERSION`, and keep the `.hpp` declaration guard identical to the
  `.cpp` definition guard.
  **Why:** wx border-style flags don't remove GTK theme borders or padding, which show up as black
  borders on Linux. CSS without `:hover` / `:focus` lets the border come back on interaction. Guards
  that differ between header and implementation (`__WXGTK3__` vs `__WXGTK__`) break the build on other
  GTK versions.
  ```cpp
  // Wrong: wx flags alone, border reappears / theme padding stays
  text_ctrl = new wxTextCtrl(this, wxID_ANY, text, pos, size, style | wxBORDER_NONE);
  // Right
  text_ctrl = new wxTextCtrl(this, wxID_ANY, text, pos, size, style | wxBORDER_NONE);
  #ifdef __WXGTK__
  Slic3r::GUI::RemoveInputBorder(text_ctrl);
  #endif
  ```
  Cite: 477208a969 (`GUI_Utils.cpp` `RemoveButtonBorder` / `RemoveInputBorder`, `GUI_Utils.hpp`).
- **Rule:** Guard direct `gtk_window_resize()` calls (and similar GTK size calls) so they run only with
  strictly positive width and height.
  **Why:** GTK checks the arguments (`gtk_window_resize: assertion 'width > 0'`) when a popup is
  measured before it has content. This spams assertion errors on Linux and aborts when GTK criticals
  are made fatal. `DropDown` resizes the GTK wrapper window itself (source comment: "Gtk has a
  wrapper window for popup widget"), so its size can be zero before the items exist.
  ```cpp
  // Wrong
  gtk_window_resize(GTK_WINDOW(m_widget), szContent.x, szContent.y);
  // Right
  if (szContent.x > 0 && szContent.y > 0)
      gtk_window_resize(GTK_WINDOW(m_widget), szContent.x, szContent.y);
  ```
  Cite: dc12126b78 (`DropDown::messureSize`, `Widgets/DropDown.cpp`).

## wx 3.3 migration notes

Orca moved from **3.1.5** to 3.3.2 in 8248b06337 ("Updated wxWidgets to 3.3.2", #12941; build system
in 2d7e26292b), so the 3.1.6–3.2.0 incompatible changes (`docs/changes_32.txt`) apply as well as
`docs/changes.txt`. The version digest, every migration commit as a rule for new code, and the
post-upgrade regressions to watch are owned by `references/wx-33-changes.md` (§8). The
platform-specific ones are described in this file: the `MainFrame` `WS_CAPTION` handling
([MSW title bar](#msw-the-mainframe-custom-title-bar)), the macOS `kAEGetURL` re-registration and the
`SidePopup` anchoring ([ifdef landscape](#the-ifdef-landscape)), and the GTK criticals filter
([GTK3 summary](#gtk3-x11-and-wayland)).

## Cross-platform testing checklist

Because wx asserts are compiled out, a platform bug usually shows up as wrong pixels, a dropped
event or a frozen interaction, not a dialog. Exercise the change; a successful build proves little.
Say in the PR which platforms you exercised.

**Build**
- [ ] Every toolkit branch compiles: `__WXMSW__`, `__WXOSX__`, `__WXGTK__`. GTK code also compiles with
      the GTK2 branch of any `GTK_CHECK_VERSION`, and header/implementation guards match.
- [ ] New Cocoa code is in a `.mm` file registered in the `APPLE` block, and new sources are added to
      `src/slic3r/CMakeLists.txt`.
- [ ] No `FromSVG*`, no new wx private header without the matching port macro, no reliance on a wx
      assert.

**Windows**
- [ ] 100 %, 125 %, 150 % and 175 % scaling, plus moving the window between monitors with different
      scaling (PMv2 `wxEVT_DPI_CHANGED`, `DPIAware` rescale, `on_dpi_changed`).
- [ ] Light and dark, including switching dark mode at runtime from Preferences.
- [ ] `MainFrame` maximise/restore, maximise with an auto-hide taskbar, Snap, drag from the top bar,
      resize from the top edge.
- [ ] Popups: open, then click elsewhere, Alt+Tab, minimise the main window; the popup must close
      exactly once.
- [ ] Live window resize with the 3D view visible (no blank canvas); custom-painted widgets don't
      flicker.

**macOS**
- [ ] A Retina and a non-Retina display, and moving windows between them.
- [ ] Switching system appearance while the app runs.
- [ ] Menu-bar shortcuts vs text fields (typing in a field must not trigger a menu shortcut), and
      Cmd+H / Cmd+M / Cmd+Q / Cmd+Ctrl+F.
- [ ] Popups and dropdown menus: move the cursor from the anchor into the menu without it
      dismissing; nothing stays captured after a drag (the UI must stay clickable).
- [ ] `ObjectList` editing and keyboard shortcuts (native data view).

**Linux, GTK3 on X11** (`GDK_BACKEND=x11` on a Wayland session, or an X11 session)
- [ ] Scale 1 and 2 (`GDK_SCALE=2`), light and dark GTK themes, large font settings (text-driven
      sizes, `em_unit`).
- [ ] No native borders or padding around custom widgets; dialogs open at their fitted size and don't
      collapse after minimising the main window.
- [ ] The GL views render (GLX path), and WebView pages load.

**Linux, GTK3 on native Wayland**: at least GNOME (mutter) and one wlroots compositor (Sway or
Hyprland); KDE is worth a run for decoration differences.
- [ ] Moving and resizing the main window through the top bar and the edges; maximise/restore; no
      second title bar; the splash has no title bar.
- [ ] Popups, chained dropdown submenus and search dropdowns open in the right place, track hover and
      dismiss correctly.
- [ ] Nothing depends on `wxGetMousePosition()`, `SetPosition()` on a top-level window, or
      `wxClientDC` drawing. No CPU spin with the window on an inactive workspace.
- [ ] GL views render after startup (EGL post-init retry), and overlay icons are not translucent.
- [ ] Dock panes cannot float, and a layout saved on X11 loads.

**Packaging**
- [ ] The Flatpak build (shared wx, GTK3, sandbox) if the change touches wx options, file dialogs,
      WebView or desktop integration.

**Debugging aids**
- [ ] In a Debug or RelWithDebInfo build, Ctrl+Shift+I (Cmd+Shift+I on macOS) on any `DPIAware` window
      opens wxInspector to check the window tree, sizes and styles per platform.
- [ ] On Linux, remember that `GUI_App::on_init_inner` filters some GTK criticals. Check the terminal
      output for the rest.
