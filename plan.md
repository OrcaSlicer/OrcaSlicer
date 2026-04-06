# Native Wayland Support for OrcaSlicer — Implementation Plan

## Context

OrcaSlicer currently forces all Linux users onto XWayland by setting `GDK_BACKEND=x11` at startup. With the migration to wxWidgets 3.3.2, which introduces runtime GLX/EGL selection (`wxGLCanvas::PreferGLX()`), native Wayland support is now technically feasible without breaking X11 compatibility. The main blockers are: (1) GLEW has no EGL support and is unmaintained, (2) wxWidgets is built with `wxUSE_GLCANVAS_EGL=OFF`, and (3) several X11-specific code paths need conditional handling.

This plan is ordered from lowest-risk/highest-impact to highest-risk, with each phase independently testable.

---

## Phase 1: Runtime Display Backend Detection Infrastructure

**Goal:** Create a utility to detect X11 vs Wayland at runtime. Foundation for all subsequent phases.

**Risk:** Very low — additive only, no existing code modified.

### Files to create:
- `src/slic3r/GUI/LinuxDisplayBackend.hpp` — Header with `is_running_on_wayland()` / `is_running_on_x11()` predicates
- `src/slic3r/GUI/LinuxDisplayBackend.cpp` — Implementation using `GDK_IS_X11_DISPLAY()` / `GDK_IS_WAYLAND_DISPLAY()` from `<gdk/gdkx.h>` / `<gdk/gdkwayland.h>`, guarded by `wxHAVE_GDK_X11` / `wxHAVE_GDK_WAYLAND` (already detected in `cmake/modules/FindGTK3.cmake:48-49`)

### Files to modify:
- `src/slic3r/CMakeLists.txt` — Add new sources, pass `wxHAVE_GDK_WAYLAND`/`wxHAVE_GDK_X11` as compile definitions
- `cmake/modules/FindGTK3.cmake` — Verify the existing `check_symbol_exists` calls propagate correctly

### Design:
```cpp
namespace Slic3r::GUI {
enum class LinuxDisplayBackend { X11, Wayland, Unknown };
LinuxDisplayBackend get_linux_display_backend(); // Call after gtk_init
bool is_running_on_wayland();
bool is_running_on_x11();
}
```

### Testing:
- Build and run on X11 → logs "X11 backend detected"
- Build and run on Wayland → logs "Wayland backend detected" (still falls back to X11 due to `GDK_BACKEND=x11` until Phase 2)

---

## Phase 2: Conditional X11 Initialization

**Goal:** Remove the forced `GDK_BACKEND=x11`, make `XInitThreads()` and NVIDIA env vars conditional, restore display validation.

**Risk:** Medium-low. Includes a compile-time safety fallback for builds without EGL.

### File to modify: `src/OrcaSlicer.cpp`

### Changes (lines 1185–1210):

1. **Remove `GDK_BACKEND=x11` force** (line 1189) — THE core blocker for native Wayland

2. **Add compile-time safety fallback** — If `wxUSE_GLCANVAS_EGL` is OFF (before Phase 4), re-apply `GDK_BACKEND=x11` with a warning log so non-EGL builds don't crash:
   ```cpp
   #if !wxUSE_GLCANVAS_EGL
   if (wayland_env && *wayland_env) {
       BOOST_LOG_TRIVIAL(warning) << "Wayland detected but EGL not compiled in. Forcing X11.";
       ::setenv("GDK_BACKEND", "x11", true);
   }
   #endif
   ```

3. **Conditional `XInitThreads()`** (line 1209) — Only call when X11 is involved:
   ```cpp
   const char* display_env = ::getenv("DISPLAY");
   if (display_env && *display_env) {
       XInitThreads();
   }
   ```

4. **Guard `#include <X11/Xlib.h>`** (lines 90-93) — Behind `wxHAVE_GDK_X11`

5. **NVIDIA `__GLX_VENDOR_LIBRARY_NAME`** (line 1204) — Only set on X11 sessions (GLX-specific)

6. **`WEBKIT_DISABLE_COMPOSITING_MODE`** (line 1194) — Only set for XWayland (both `DISPLAY` and `WAYLAND_DISPLAY` present), not for native Wayland

7. **Uncomment display validation** (lines 1283-1295) — Check for EITHER `DISPLAY` or `WAYLAND_DISPLAY`

### Testing:
- On X11: No behavior change (safety fallback does not trigger)
- On Wayland without EGL: Falls back to X11 with warning log
- On Wayland with EGL (after Phase 4): Native Wayland launches

---

## Phase 3: GLEW → GLAD Migration

**Goal:** Replace the unmaintained GLEW (no EGL support) with GLAD (supports both GLX and EGL). This is the largest code change but is entirely testable on X11.

**Risk:** Medium. Touches ~46 files but is mostly mechanical header swaps. Real logic changes are in ~3 files.

### Sub-phase 3A: Generate GLAD Sources

**Create:** `src/glad/` directory with GLAD2 generated for GL compatibility 4.6 + required extensions:
- `GL_EXT_texture_compression_s3tc`
- `GL_ARB_framebuffer_object`
- `GL_EXT_framebuffer_object`
- `GL_EXT_texture_filter_anisotropic`
- `GL_ARB_compatibility`

Also create `src/glad/CMakeLists.txt` as a static library target.

**Unify with libvgcode:** `src/libvgcode/glad/` currently has its own GLAD (core-only GL + GLES2). To avoid duplicate symbols when linking, modify `src/libvgcode/CMakeLists.txt` to use the new shared GLAD from `src/glad/` instead of its private copy. **Note:** The shared GLAD must be generated as a superset (compatibility 4.6 + extensions) that covers both libvgcode's core-only needs and the main app's compatibility profile needs.

### Sub-phase 3B: Mechanical Header Replacement (~46 files)

Replace `#include <GL/glew.h>` → `#include <glad/gl.h>` across all ~46 files:
- `src/slic3r/GUI/*.cpp` (OpenGLManager, GLCanvas3D, 3DScene, GLModel, GLShader, GLTexture, etc.)
- `src/slic3r/GUI/Gizmos/GLGizmo*.cpp` (17 files)
- `src/slic3r/Utils/EmbossStyleManager.hpp/.cpp`
- `src/slic3r/GUI/Jobs/CreateFontNameImageJob.hpp`

### Sub-phase 3C: Replace GLEW-Specific API Calls

**`src/slic3r/GUI/OpenGLManager.cpp`:**
- Lines 248-253: `glewExperimental = true; glewInit()` → `gladLoadGL()`
- Lines 124,129: `GLEW_EXT_texture_filter_anisotropic` → `GLAD_GL_EXT_texture_filter_anisotropic`; `GLEW_ARB_compatibility` → `GLAD_GL_ARB_compatibility`
- Lines 256-267: `GLEW_EXT_texture_compression_s3tc` → `GLAD_GL_EXT_texture_compression_s3tc`; `GLEW_ARB_framebuffer_object` → `GLAD_GL_ARB_framebuffer_object`; `GLEW_EXT_framebuffer_object` → `GLAD_GL_EXT_framebuffer_object`
- Lines 27-34: **Remove** the `#error` EGL/GLX mismatch guards (no longer applies with GLAD)

**`src/slic3r/GUI/ImGuiWrapper.cpp`** (line 2842):
- `GLEW_EXT_texture_compression_s3tc` → `GLAD_GL_EXT_texture_compression_s3tc`

**`src/slic3r/GUI/IMToolbar.cpp`** (line 90):
- `GLEW_EXT_texture_compression_s3tc` → `GLAD_GL_EXT_texture_compression_s3tc`

**`src/OrcaSlicer.cpp`** (line 6456):
- `glewInit` log message → `gladLoadGL` equivalent

### Sub-phase 3D: Build System Changes

- **Remove:** `deps/GLEW/GLEW.cmake` from the dependency chain (or mark as unused)
- **`src/slic3r/CMakeLists.txt`** (line 739): Remove `GLEW::GLEW` from `target_link_libraries`, add `glad` target
- **`deps/CMakeLists.txt`**: Remove GLEW from dependency list
- **Add:** `src/glad/CMakeLists.txt`

### Testing:
- Build entire project on X11 — all GL rendering (3D viewport, thumbnails, gizmos, texture loading) must work identically
- No Wayland changes active yet; this phase is pure X11 regression testing

---

## Phase 4: Enable EGL and Runtime GLX/EGL Selection

**Goal:** Enable `wxUSE_GLCANVAS_EGL=ON`, use wxWidgets 3.3.2's runtime selection to pick GLX on X11 and EGL on Wayland.

**Risk:** Medium-high. Changes the fundamental OpenGL initialization path. NVIDIA proprietary drivers on Wayland/EGL need specific testing.

### Files to modify:

1. **`deps/wxWidgets/wxWidgets.cmake`** (line 41):
   `-DwxUSE_GLCANVAS_EGL=OFF` → `-DwxUSE_GLCANVAS_EGL=ON`

2. **`scripts/flatpak/com.orcaslicer.OrcaSlicer.yml`** (line 102):
   `-DwxUSE_GLCANVAS_EGL=OFF` → `-DwxUSE_GLCANVAS_EGL=ON`

3. **`src/slic3r/GUI/OpenGLManager.cpp`** — Before GL canvas creation:
   ```cpp
   #if defined(__WXGTK__) && wxUSE_GLCANVAS_EGL
   if (is_running_on_x11()) {
       wxGLCanvas::PreferGLX(); // Maximum driver compat on X11
   }
   // On Wayland, default EGL is used (no GLX available)
   #endif
   ```

4. **`src/OrcaSlicer.cpp`** — Remove the Phase 2 safety fallback (the `#if !wxUSE_GLCANVAS_EGL` block) since EGL is now always compiled in.

### EGL SwapBuffers Throttling Mitigation:
On Wayland, `eglSwapBuffers` blocks when the canvas is hidden/occluded. Check `m_canvas->IsShownOnScreen()` before `SwapBuffers()` in `GLCanvas3D::render()` to avoid stalls on hidden tabs.

### Testing:
- **Pure X11:** Verify `PreferGLX()` activates, GLX context created, all rendering works
- **Pure Wayland (GNOME):** EGL context created, 3D viewport renders correctly
- **Pure Wayland (KDE Plasma):** Same as GNOME
- **XWayland:** X11 detection works, GLX preferred
- **NVIDIA + Wayland:** EGL context with NVIDIA proprietary driver

---

## Phase 5: GLFW Update for CLI Thumbnail Generation

**Goal:** Update GLFW from 3.3.7 (compile-time Wayland-only via `GLFW_USE_WAYLAND`) to 3.4+ (runtime backend selection).

**Risk:** Low-medium. GLFW 3.4 is backward-compatible.

### Files to modify:

1. **`deps/GLFW/GLFW.cmake`:**
   - Update URL to GLFW 3.4 release
   - Replace `-DGLFW_USE_WAYLAND=ON` with `-DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON`
   - Fix existing typo: non-Linux path sets `GLFW_USE_WAYLAND=FF` (should be `OFF`)

2. **`src/OrcaSlicer.cpp`** (lines 6410-6446): No code changes needed — GLFW 3.4 auto-selects backend. The existing `GLFW_VISIBLE=false` hint (line 6429) already creates an offscreen context.

### Testing:
- CLI slicing with thumbnail generation on both X11 and Wayland sessions

---

## Phase 6: Wayland UI Compatibility Workarounds

**Goal:** Address known Wayland limitations for specific UI patterns. These are incremental and each can be done independently.

**Risk:** Medium. Many small changes, each with potential for subtle behavioral differences.

### 6A: `wxGetMousePosition()` Global Coordinate Issues

On Wayland, `wxGetMousePosition()` returns (0,0) for global screen coordinates. There are ~27 call sites in the GUI code.

**Triage by severity:**

| Call Site | Impact on Wayland | Fix |
|-----------|-------------------|-----|
| `BBLTopbar.cpp:661,678` (window drag) | **Works** — GTK `begin_move_drag` ignores x/y on Wayland | No change needed |
| `BBLTopbar.cpp:592` (double-click) | Low — tool item lookup | Use `mouse.GetPosition()` (event-relative) |
| `BBLTopbar.cpp:691,702,746` (mouse move/up) | Medium — used for drag delta | Refactor to use event-relative positions |
| `MainFrame.cpp:200` (resize drag) | **Works** — GTK `begin_resize_drag` ignores x/y on Wayland (uses `ClientToScreen`, not `wxGetMousePosition`) | No change needed |
| `GLCanvas3D.cpp:4819` | Low — extra frame request | Cache mouse position from last event |
| `Plater.cpp:1784,1846,1916` (`wxFindWindowAtPoint`) | Medium — scroll forwarding | Replace with window hierarchy hit-testing |
| Widget files (CapsuleButton, FilamentMapPanel, DropDown, etc.) | Low — hover detection | Use `ScreenToClient` from event coords |
| `Search.cpp:677,907` | Low — popup positioning | Use parent-relative positioning |
| `Button.cpp:519` | Low — tooltip position | Use widget-relative position |

### 6B: Window Positioning (`SetPosition()`)

On Wayland, `SetPosition()` is a no-op for top-level windows. This affects:
- Notification/toast frames and slide animations (`BaseTransparentDPIFrame.cpp:43,258,260,264` — animated position updates will silently fail)
- Camera popup (`CameraPopup.cpp:242`)
- Dialog positioning (`CreatePresetsDialog.cpp:655,1620`, `FilamentPickerDialog.cpp:123`)
- Window restore position (`GUI_App.cpp:294,7711`)

**Fix:** Make these transient children of their parent window (`gtk_window_set_transient_for`) so the compositor places them nearby. Accept that exact pixel positioning is not possible on Wayland — this is a Wayland design decision. For `BaseTransparentDPIFrame` slide animations, consider replacing with opacity-based fade animations on Wayland.

### 6C: wxAUI Docking Limitations

wxAUI floating pane creation uses global coordinates. Tab undocking may place panes at unexpected positions.

**Fix:** For now, document as a known limitation. wxWidgets 3.3.x continues to improve wxAUI Wayland support. Revisit when upstream fixes land.

### Testing:
- Manual UI testing on Wayland: title bar drag, edge resize, double-click maximize, menus, popups, tooltips, notification toasts, scroll forwarding between panels

---

## Phase 7: Flatpak and Distribution Integration

**Goal:** Ensure Flatpak builds work with native Wayland.

**Risk:** Low.

### Files to modify:

1. **`scripts/flatpak/com.orcaslicer.OrcaSlicer.yml`:**
   - Ensure `--socket=wayland` permission alongside `--socket=x11`
   - `wxUSE_GLCANVAS_EGL=ON` (done in Phase 4)
   - Verify `wayland-protocols` and EGL dev packages are available

2. **Desktop entry and MIME types** — Already Wayland-compatible, no changes needed

### Testing:
- Build Flatpak, run on GNOME Wayland and KDE Plasma Wayland

---

## Phase Dependency Graph

```
Phase 1 (Detection)
   ↓
Phase 2 (Conditional X11 Init) ← Can ship alone with safety fallback
   ↓
Phase 3 (GLEW → GLAD) ← Biggest change, fully testable on X11
   ↓
Phase 4 (EGL Enable) ← Unlocks native Wayland GL rendering
   ↓         ↓
Phase 5    Phase 6 (Parallel: GLFW update + UI workarounds)
   ↓         ↓
      Phase 7 (Flatpak)
```

**Phases 1+2** can be merged into one PR — low-risk, preserves X11 behavior.
**Phase 3** is a standalone PR — large but mechanical, no Wayland changes.
**Phase 4** is the PR that actually enables Wayland — depends on Phase 3.
**Phases 5-7** can be individual PRs in parallel after Phase 4.

---

## Verification Plan

### Build Verification
```bash
cmake --build build --config RelWithDebInfo --target all
```

### Functional Testing Matrix

| Scenario | Phase 1-2 | Phase 3 | Phase 4+ |
|----------|-----------|---------|----------|
| X11 session (Intel/AMD) | No change | No change | GLX via PreferGLX |
| X11 session (NVIDIA) | No change | No change | GLX via PreferGLX |
| Wayland (Intel/AMD, no EGL) | Falls back to X11 | Falls back to X11 | EGL native |
| Wayland (NVIDIA, no EGL) | Falls back to X11 | Falls back to X11 | EGL native |
| XWayland | XInitThreads called | No change | GLX via PreferGLX |
| CLI thumbnail (X11) | No change | GLAD loads | GLAD loads |
| CLI thumbnail (Wayland) | No change | GLAD loads | GLFW 3.4 auto-selects |
| Flatpak (Wayland) | Falls back to X11 | Falls back to X11 | EGL native |

### Key Regression Checks
- 3D viewport rendering (rotations, zoom, pan)
- Gizmo rendering (all gizmos)
- Thumbnail generation (GUI and CLI paths)
- Window resize from edges (Linux custom ResizeEdgePanel)
- Title bar drag
- Maximize/restore
- WebView content (Setup Wizard, web panels)
- Modal dialogs
- DPI scaling / HiDPI
- Multi-monitor behavior

---

## Critical Files Summary

| File | Phases | Purpose |
|------|--------|---------|
| `src/OrcaSlicer.cpp` | 2, 3, 5 | X11 init, env vars, CLI GL path |
| `src/slic3r/GUI/OpenGLManager.cpp` | 3, 4 | GL loader init, EGL/GLX selection |
| `deps/wxWidgets/wxWidgets.cmake` | 4 | wxUSE_GLCANVAS_EGL toggle |
| `deps/GLEW/GLEW.cmake` | 3 | Remove GLEW dep |
| `deps/GLFW/GLFW.cmake` | 5 | Update to 3.4 |
| `src/slic3r/CMakeLists.txt` | 1, 3 | Link targets, new sources |
| `src/slic3r/GUI/BBLTopbar.cpp` | 6 | wxGetMousePosition workarounds |
| `src/slic3r/GUI/MainFrame.cpp` | 6 | ResizeEdgePanel coords |
| `src/slic3r/GUI/GLCanvas3D.cpp` | 3, 4 | Header swap, SwapBuffers guard |
| `scripts/flatpak/com.orcaslicer.OrcaSlicer.yml` | 4, 7 | Flatpak EGL + Wayland socket |
| `cmake/modules/FindGTK3.cmake` | 1 | GDK Wayland/X11 symbol detection |
