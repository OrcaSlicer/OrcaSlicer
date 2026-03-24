# wxWidgets Upgrade Plan: 3.1.5 → 3.3.2

## Context

OrcaSlicer currently uses wxWidgets 3.1.5 via a custom fork (`https://github.com/SoftFever/Orca-deps-wxWidgets`). The goal is to upgrade to 3.3.2 from the same fork's `v3.3.2` branch. The codebase has several version-gated workarounds and compatibility shims that can be removed, and the 3.2/3.3 releases introduce breaking API changes that must be addressed.

## Branch / Fork Policy

- Use the floating `v3.3.2` **branch** from the SoftFever fork (no pinned tag for now). Set `GIT_TAG v3.3.2` in CMake so ExternalProject tracks the branch head.
- If a wx-side fix is needed, patch the SoftFever `v3.3.2` branch directly rather than adding Orca-side vendored diffs.
- Do **not** proactively port old `master` patches (dark theme, DataView, WebView ARM64, libwebkit2gtk, GLX/XWayland, GTK). Only reintroduce if a regression is reproduced against `v3.3.2`.

---

## Phase 1: Build System Changes

### 1.1 Update `deps/wxWidgets/wxWidgets.cmake`
- Add `GIT_TAG v3.3.2` (branch name) to the `orcaslicer_add_cmake_project` call — this tracks the floating branch head, not a fixed tag
- Remove `-DwxUSE_UNICODE=ON` (unicode-only in 3.3, option removed)
- Review `-DwxUSE_GLCANVAS_EGL=OFF` — still valid but verify
- Keep `-DwxUSE_WEBVIEW_IE=OFF` — the option still exists in 3.3.2 and IE backend is still present; removing it would re-enable IE on Windows
- All other cmake args should remain valid

### 1.2 Update `src/CMakeLists.txt` (lines 31-40)
- **Keep `adv` in `find_package` COMPONENTS lists** — wxWidgets 3.3.2 still ships `wxadv` as a backward-compatible empty shell (all classes moved to `core`, but the component remains findable). Removing it is safe but unnecessary churn; keeping it avoids breaking the build for no benefit.
- Change minimum version from `3.1` / `3.0` to `3.3`
- Remove the `SLIC3R_WX_STABLE` conditional path (3.0 no longer supported)

### 1.3 Update Flatpak manifest
- `scripts/flatpak/com.orcaslicer.OrcaSlicer.yml` — update wxWidgets source URL/tag and sha256

---

## Phase 2: Remove Outdated Workarounds

These are mechanical changes — the workarounds are already dead code on 3.1.5 or explicitly marked with FIXME comments for removal on upgrade.

### 2.1 Remove DPI fallback code (pre-3.1.3 workaround)
- **`src/slic3r/GUI/GUI_Utils.hpp:84-100`** — Remove `DpiChangedEvent` struct and `EVT_DPI_CHANGED_SLICER` declaration (guarded by `#if !wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)`)
- **`src/slic3r/GUI/GUI_Utils.cpp`** — Remove corresponding `EVT_DPI_CHANGED_SLICER` definition
- **`src/slic3r/GUI/GUI_App.cpp`** — Remove `register_win32_dpi_event()` function and its call (guarded by same version check)

### 2.2 Remove const_cast workarounds for wxExecute
- **`src/slic3r/GUI/GUI.cpp`** (2 instances) — FIXME says "not needed in wxWidgets 3.1"
- **`src/slic3r/GUI/NotificationManager.cpp`**
- **`src/slic3r/GUI/Downloader.cpp`**

### 2.3 Remove OSX 10.9.5 crash workaround
- **`src/slic3r/GUI/OpenGLManager.cpp:22-26, 240-256, 416-419`** — wxWidgets 3.3 requires macOS 10.11+, so 10.9.5 is unsupported. Remove the version check and always clean up wxGLContext normally.

### 2.4 Simplify version-check guards
Remove `#if wxCHECK_VERSION(...)` guards, keeping only the "true" branch:
- **`src/slic3r/GUI/I18N.hpp:67`** — `wxCHECK_VERSION(3, 1, 1)`
- **`src/slic3r/GUI/ExtraRenderers.hpp:8`** — `wxCHECK_VERSION(3, 1, 1)` for `SUPPORTS_MARKUP`
- **`src/slic3r/GUI/ConfigWizard.cpp:906`** — `wxCHECK_VERSION(3, 1, 1)`
- **`src/slic3r/GUI/SendSystemInfoDialog.cpp:484`** — `wxCHECK_VERSION(3, 1, 2)`
- **`src/slic3r/GUI/GUI_Utils.cpp:250`** — `wxCHECK_VERSION(3, 1, 3)`

### 2.5 Clean up wxinit.h
- **`src/slic3r/GUI/wxinit.h:17-23`** — The `#ifndef wxEVT_BUTTON` / `#ifndef wxEVT_HTML_LINK_CLICKED` guards are unnecessary (these macros have existed since 3.0). Can keep for safety since they're harmless, or remove.

---

## Phase 3: Fix Breaking API Changes

These require actual code modifications to compile against wxWidgets 3.3.

### 3.1 Sizer flag conflicts — `wxEXPAND | wxALIGN_*` (~115 occurrences, ~23 files)
wxWidgets 3.2+ asserts on invalid flag combos. `wxEXPAND` fills entire space, making alignment meaningless.
- Search for all `wxEXPAND` combined with `wxALIGN_CENTER`, `wxALIGN_RIGHT`, `wxALIGN_CENTRE`, `wxALIGN_CENTER_VERTICAL`, `wxALIGN_CENTER_HORIZONTAL`, `wxALIGN_BOTTOM`, `wxALIGN_TOP`
- Remove the conflicting alignment flag in each case
- Alternatively, call `wxSizerFlags::DisableConsistencyChecks()` as a temporary measure

### 3.2 `wxTRANSPARENT_WINDOW` removal
- **`src/slic3r/GUI/MainFrame.cpp:121`** — Used in `ResizeEdgePanel` constructor. Remove the flag; the `wxBG_STYLE_TRANSPARENT` on line 125 already handles transparency.

### 3.3 wxGLCanvas multi-sampling (3.3 change)
- Multi-sampling is no longer the default. **`src/slic3r/GUI/OpenGLManager.cpp`** and **GLCanvas3D** — verify that explicit `wxGLAttributes` are used to request multi-sampling where needed.

### 3.4 Global operator scope changes (3.3 change)
- Operators on wx types moved from global to class scope. Code relying on implicit conversions will fail.
- Fix on a case-by-case basis during compilation — likely a small number of actual failures

### 3.5 wxUSE_STD_CONTAINERS default ON (3.3 change)
- wxList/wxArray now behave like std containers. `wxArrayString`, `wxArrayInt` used throughout.
- Test thoroughly. If problems arise, set `-DwxUSE_STD_CONTAINERS=0` in cmake args as fallback.

### 3.6 Missing header includes
- `wx/cursor.h` no longer transitively includes `wx/utils.h` — add explicit `#include <wx/utils.h>` where needed

### 3.7 wxWindow::Raise() no longer implies Show() (3.3 change)
- `Raise()` no longer shows a hidden window. Callers that relied on `Raise()` to both show and raise must add explicit `Show()`.
- 9 files use `->Raise()`: `WebViewDialog.cpp`, `StatusPanel.cpp`, `ReleaseNote.cpp`, `PrinterWebView.cpp`, `Plater.cpp`, `ObjColorDialog.cpp`, `MainFrame.cpp`, `ImageDPIFrame.cpp`, `BaseTransparentDPIFrame.cpp`
- Audit each call: if the window may be hidden when `Raise()` is called, add `Show()` before it.

### 3.8 wxToolTip::GetToolTipCtrl() accessibility (Windows dark tooltips)
- **`src/slic3r/GUI/GUI_App.cpp:4316`** — Uses `wxToolTip::GetToolTipCtrl()` for dark tooltip styling on Windows.
- If this API is no longer accessible in 3.3, either patch the fork branch to keep it or refactor Orca's dark tooltip code.

---

## Phase 4: Build, Fix, Iterate

1. Rebuild deps with the new wxWidgets version: `cmake --build deps/build`
2. Attempt main build: `cmake --build build/arm64 --config RelWithDebInfo`
3. Fix compilation errors iteratively — expect 50-200 errors on first pass
4. Most errors will be from Phase 3 issues; fix in order of H1-H7 priority

---

## Phase 5: Runtime Verification

- **Layout testing** — verify all dialogs/panels render correctly (sizer flag changes)
- **Dark mode** — wxWidgets 3.3 adds MSW dark mode support; test for conflicts with OrcaSlicer's custom dark mode. Verify dark tooltips (`wxToolTip::GetToolTipCtrl()` in `GUI_App.cpp:4316`).
- **OpenGL** — verify 3D canvas rendering on all platforms (multi-sampling change)
- **Media playback** — test wxMediaCtrl2 (custom implementation). On Linux, wxMediaCtrl2 depends on legacy GStreamer backend internals (`wxMediaCtrl2.cpp`); if playback breaks, patch fork to set `wxUSE_GSTREAMER_PLAYER OFF`.
- **Locale/language** — test language switching
- **High DPI** — test on high DPI displays. Verify wxImageList sizing (now physical pixels) in `Tab.cpp:471,1332,1366` and `TabCtrl.hpp`.
- **wxClientDC double buffering** (Windows) — wxMSW now double-buffers by default, changing wxClientDC behavior. Test custom drawing in `wxExtensions.cpp`, `OG_CustomCtrl.cpp`, and `Widgets/*.cpp` for rendering artifacts or flickering.
- **Window activation** — `wxWindow::Raise()` no longer implies `Show()`. Test dialogs and transient frames in 9 files that call `Raise()`.
- **wxBitmapBundle** — 3.2+ bitmap API changes may affect custom bitmap combo wrappers and controls. Not a required migration but test for regressions.

---

## Critical Files to Modify

| File | Change |
|------|--------|
| `deps/wxWidgets/wxWidgets.cmake` | Add GIT_TAG, remove obsolete cmake args |
| `src/CMakeLists.txt` | Bump version to 3.3, remove `SLIC3R_WX_STABLE` path |
| `src/slic3r/GUI/GUI_Utils.hpp` | Remove DpiChangedEvent fallback |
| `src/slic3r/GUI/GUI_Utils.cpp` | Remove DPI event definition, simplify version checks |
| `src/slic3r/GUI/GUI_App.cpp` | Remove register_win32_dpi_event, version guards |
| `src/slic3r/GUI/OpenGLManager.cpp` | Remove OSX 10.9.5 hack, verify GL attributes |
| `src/slic3r/GUI/MainFrame.cpp` | Remove wxTRANSPARENT_WINDOW |
| `src/slic3r/GUI/GUI.cpp` | Remove const_cast workarounds |
| `src/slic3r/GUI/NotificationManager.cpp` | Remove const_cast workaround |
| `src/slic3r/GUI/Downloader.cpp` | Remove const_cast workaround |
| `src/slic3r/GUI/wxinit.h` | Optional cleanup of compat shims |
| `src/slic3r/GUI/I18N.hpp` | Remove version guard |
| `src/slic3r/GUI/ConfigWizard.cpp` | Remove version guard |
| `src/slic3r/GUI/SendSystemInfoDialog.cpp` | Remove version guard |
| ~23 files with sizer flags | Remove conflicting `wxALIGN_*` from `wxEXPAND` calls |
| 9 files with `->Raise()` | Audit for missing `Show()` before `Raise()` |

## Approach

Work in order: Phase 1 → 2 → 3 → 4 → 5. Phases 2 and 3 can be partially interleaved during the compilation fix pass. The strategy is to make minimal, targeted changes — not a broad refactor — to get the build working, then verify at runtime.
