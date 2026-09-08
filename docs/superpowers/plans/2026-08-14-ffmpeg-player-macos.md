# macOS FFmpeg Media Player Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make macOS use the same FFmpeg-based media player (`wxMediaCtrl3` + `AVVideoDecoder`) as Windows/Linux, linking the static FFmpeg libraries from the deps build, and remove the old `wxMediaCtrl2.mm` BambuPlayer-based player.

**Architecture:** The new player is platform-neutral C++ already used on Linux/Windows. Enabling it on macOS is pure build wiring: compile `wxMediaCtrl3.cpp` + `AVVideoDecoder.cpp` on macOS, drop the `__WXMAC__` alias that redirects `wxMediaCtrl3` to the old `wxMediaCtrl2`, and link static FFmpeg (`libavcodec.a`/`libswscale.a`/`libavutil.a`) from the deps install. The Bambu stream API is dlsym'd at runtime from the network plugin (`libBambuSource.dylib`), which already exports it — no plugin changes needed. Rendering reuses the existing `wxImage` → `DrawBitmap` paint path (same as Linux).

**Tech Stack:** C++17, wxWidgets, CMake, FFmpeg 7.0.3 (libavcodec/libswscale/libavutil), macOS (Xcode generator), `deps/` ExternalProject build system.

## Global Constraints

- Branch: `dev/ffmpeg-player-macos`. Commit after every task.
- **Linux and Windows builds must not change** — the FFmpeg deps flag change is guarded by `APPLE`; Linux keeps `--enable-shared`, Windows keeps its prebuilt DLL zips.
- Static FFmpeg only on macOS: deps produce `libavcodec.a`/`libswscale.a`/`libavutil.a`; the app links those explicitly — the app binary must have **no** `libav*` dylib references (`otool -L` check).
- Follow existing code style: PascalCase classes, snake_case functions, C++17.
- No changes to `StatusPanel.cpp`, `MediaPlayCtrl.*`, or the BambuTunnel interface — the app already creates `wxMediaCtrl3` and uses only its public interface.
- The player cannot be unit-tested (hardware/plugin-dependent GUI code); verification is build-level, link-level, and manual runtime on a Mac.
- `localization/i18n/list.txt` references only `wxMediaCtrl2.cpp` (Win/Linux, stays) — no translation-list changes needed.
- Build dirs on the dev machine: main app = `build_arm64/` (Xcode generator, multi-config), deps = `deps/build/arm64/` (Unix Makefiles). App target name: `OrcaSlicer`. Substitute your own configured build dirs where noted.

---

### Task 1: Enable wxMediaCtrl3 on macOS and link static FFmpeg

**Files:**
- Modify: `src/slic3r/GUI/wxMediaCtrl3.h` (lines 18–22: the `#ifdef __WXMAC__` alias branch)
- Modify: `src/slic3r/GUI/wxMediaCtrl3.cpp:13` (uncomment the event define)
- Modify: `src/slic3r/GUI/wxMediaCtrl2.cpp:101` (remove the event define)
- Modify: `src/slic3r/CMakeLists.txt` (APPLE source list ~lines 779–792; FFmpeg link block ~lines 905–910)

**Interfaces:**
- Consumes: nothing new (all classes already exist).
- Produces: `wxMediaCtrl3` class compiled on macOS with the same interface as Linux/Windows — `Load(wxURI)`, `Play()`, `Stop()`, `SetIdleImage(wxString)`, `GetState()`, `GetLastError()`, `GetVideoSize()`, event `EVT_MEDIA_CTRL_STAT` defined once in the lib (from `wxMediaCtrl3.cpp`).

- [ ] **Step 1: Remove the macOS alias in wxMediaCtrl3.h**

Current (lines 16–23 of `src/slic3r/GUI/wxMediaCtrl3.h`):

```cpp
void wxMediaCtrl_OnSize(wxWindow * ctrl, wxSize const & videoSize, int width, int height);

#ifdef __WXMAC__

#include "wxMediaCtrl2.h"
#define wxMediaCtrl3 wxMediaCtrl2

#else

#define BAMBU_DYNAMIC
```

New:

```cpp
void wxMediaCtrl_OnSize(wxWindow * ctrl, wxSize const & videoSize, int width, int height);

#define BAMBU_DYNAMIC
```

Also remove the matching `#endif` that closed the `#else` branch (the one before the final `#endif /* wxMediaCtrl3_h */`), so the file's `#ifndef`/`#endif` guard pair stays balanced.

- [ ] **Step 2: Move the EVT_MEDIA_CTRL_STAT definition into wxMediaCtrl3.cpp**

In `src/slic3r/GUI/wxMediaCtrl3.cpp:13`, uncomment:

```cpp
//wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);
```

becomes:

```cpp
wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);
```

In `src/slic3r/GUI/wxMediaCtrl2.cpp:101`, delete:

```cpp
wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);
```

(One definition total in the lib — `MediaPlayCtrl.cpp:59` binds this event on the media ctrl.)

- [ ] **Step 3: Update the APPLE source list in CMakeLists.txt**

In `src/slic3r/CMakeLists.txt`, the APPLE branch (currently compiles `wxMediaCtrl2.mm`, which becomes dead on macOS):

```cmake
            GUI/wxMediaCtrl2.mm
            GUI/wxMediaCtrl2.h
            GUI/wxMediaCtrl3.h
        )
```

becomes:

```cmake
            GUI/AVVideoDecoder.cpp
            GUI/AVVideoDecoder.hpp
            GUI/wxMediaCtrl3.cpp
            GUI/wxMediaCtrl3.h
        )
```

(The `else ()` branch — Win/Linux — stays exactly as it is.)

- [ ] **Step 4: Link static FFmpeg on macOS**

In `src/slic3r/CMakeLists.txt`, the FFmpeg block (currently `if (NOT APPLE)`):

```cmake
if (NOT APPLE)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
        libavcodec
        libswscale
        libavutil
    )
    target_link_libraries(libslic3r_gui PkgConfig::LIBAV)
endif()
```

becomes:

```cmake
if (APPLE)
    # Static FFmpeg from the deps install: nothing to bundle into the .app,
    # no rpath/install_name handling. Order matters: avcodec -> swscale -> avutil.
    find_library(LIBAVCODEC_LIBRARY NAMES libavcodec.a PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
    find_library(LIBSWSCALE_LIBRARY NAMES libswscale.a PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
    find_library(LIBAVUTIL_LIBRARY NAMES libavutil.a PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
    target_link_libraries(libslic3r_gui ${LIBAVCODEC_LIBRARY} ${LIBSWSCALE_LIBRARY} ${LIBAVUTIL_LIBRARY})
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${CMAKE_PREFIX_PATH}/include)
else ()
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
        libavcodec
        libswscale
        libavutil
    )
    target_link_libraries(libslic3r_gui PkgConfig::LIBAV)
endif()
```

The deps install (`${CMAKE_PREFIX_PATH}/lib`) already contains the three `.a` files from the existing arm64 deps build — no deps rebuild needed for this task.

- [ ] **Step 5: Reconfigure and build the app**

Run (Xcode generator; `cmake` re-runs automatically on build):

```bash
cmake --build build_arm64 --config RelWithDebInfo --target OrcaSlicer
```

Expected: configure succeeds (no `pkg_check_modules` errors on macOS, `find_library` finds all three `.a` files), compile succeeds (`wxMediaCtrl3.cpp` and `AVVideoDecoder.cpp` compile on macOS without changes), link succeeds.

If CMake complains that `wxMediaCtrl3.h` is included but not in the source list or similar IDE-only warnings — ignore; headers in the list are cosmetic.

- [ ] **Step 6: Verify no dynamic FFmpeg dependency**

```bash
otool -L build_arm64/src/RelWithDebInfo/OrcaSlicer.app/Contents/MacOS/OrcaSlicer | grep -i "libav" || echo "OK: no dynamic FFmpeg"
```

Expected: prints `OK: no dynamic FFmpeg` (empty grep output). This is the whole point of static linking — nothing to bundle into the `.app`.

- [ ] **Step 7: Quick sanity — macOS unit tests still pass**

```bash
ctest --test-dir build_arm64/tests/libslic3r --output-on-failure
```

Expected: passes (add `-C RelWithDebInfo` if the multi-config generator requires it). If no tests were built in this build dir, build target `tests` first (`cmake --build build_arm64 --config RelWithDebInfo --target tests`).

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/CMakeLists.txt src/slic3r/GUI/wxMediaCtrl3.h src/slic3r/GUI/wxMediaCtrl3.cpp src/slic3r/GUI/wxMediaCtrl2.cpp
git commit -m "feat: use FFmpeg media player on macOS with static FFmpeg"
```

---

### Task 2: Static-only FFmpeg in the macOS deps build

**Files:**
- Modify: `deps/FFMPEG/FFMPEG.cmake` (non-MSVC branch, APPLE section and CONFIGURE_COMMAND)

**Interfaces:**
- Consumes: nothing.
- Produces: a deps install on macOS containing only `libavcodec.a`, `libswscale.a`, `libavutil.a` (+ headers) — no `libav*` dylibs, so no bundling/rpath machinery is ever needed on macOS. Linux and Windows output are unchanged.

- [ ] **Step 1: Add the static flag variable**

In `deps/FFMPEG/FFMPEG.cmake`, inside the non-MSVC `else ()` branch, in the existing `if (APPLE)` block:

```cmake
    if (APPLE)
        set(_minos_cmd 
            "CFLAGS=-mmacosx-version-min=${DEP_OSX_TARGET}"
            "LDFLAGS=-mmacosx-version-min=${DEP_OSX_TARGET}"
            )
```

add after the `_minos_cmd` set:

```cmake
        # Static FFmpeg: nothing to bundle into the .app, no rpath handling.
        # Shared flags must come AFTER --enable-shared below so they win.
        set(_link_cmd --enable-static --disable-shared)
```

and add a matching `else ()` after the `if (IS_CROSS_COMPILE) ... endif()` block inside that `if (APPLE)`, so non-Apple Unix keeps shared:

```cmake
    else ()
        set(_link_cmd --enable-shared)
    endif ()
```

(If the existing `if (IS_CROSS_COMPILE)` block is the last thing inside `if (APPLE)`, the new `else ()` closes the `if (APPLE)` itself.)

- [ ] **Step 2: Use the variable in CONFIGURE_COMMAND**

In the `ExternalProject_Add(dep_FFMPEG ...)` configure command:

```cmake
            "--prefix=${DESTDIR}"
            --enable-shared
```

becomes:

```cmake
            "--prefix=${DESTDIR}"
            --enable-shared
            ${_link_cmd}
```

Order matters: `--enable-shared` comes first, then `--enable-static --disable-shared` (APPLE) or `--enable-shared` (Linux) — the last flag wins in FFmpeg configure.

- [ ] **Step 3: Rebuild the FFmpeg dep (slow — several minutes, run in background)**

The changed CONFIGURE_COMMAND invalidates the ExternalProject stamp, so this re-configures and rebuilds FFmpeg:

```bash
cmake --build deps/build/arm64 --target dep_FFMPEG
```

For a fully clean static-only check (removes the previous shared build tree, which can leave stale `.dylib` files behind in the in-source build):

```bash
rm -rf deps/build/arm64/dep_FFMPEG-prefix
cmake --build deps/build/arm64 --target dep_FFMPEG
```

- [ ] **Step 4: Verify the artifacts**

```bash
ls deps/build/arm64/dep_FFMPEG-prefix/src/dep_FFMPEG/libavcodec/*.a
ls deps/build/arm64/dep_FFMPEG-prefix/src/dep_FFMPEG/libavcodec/*.dylib 2>/dev/null || echo "OK: no dylibs"
```

Expected: `libavcodec.a` present, second command prints `OK: no dylibs`. Check `libavutil` and `libswscale` the same way.

- [ ] **Step 5: Verify the app still links against the static libs**

```bash
cmake --build build_arm64 --config RelWithDebInfo --target OrcaSlicer
otool -L build_arm64/src/RelWithDebInfo/OrcaSlicer.app/Contents/MacOS/OrcaSlicer | grep -i "libav" || echo "OK: no dynamic FFmpeg"
```

Expected: build succeeds, `OK: no dynamic FFmpeg`.

- [ ] **Step 6: Commit**

```bash
git add deps/FFMPEG/FFMPEG.cmake
git commit -m "build: build static-only FFmpeg for macOS deps"
```

---

### Task 3: Remove the old macOS player

**Files:**
- Delete: `src/slic3r/GUI/wxMediaCtrl2.mm`
- Delete: `src/slic3r/GUI/BambuPlayer/BambuPlayer.h` (and the empty `BambuPlayer/` dir)
- Modify: `src/slic3r/GUI/wxMediaCtrl2.h` (remove the `#ifdef __WXMAC__` section, lines 22–60)

**Interfaces:**
- Consumes: Task 1 (macOS no longer references `wxMediaCtrl2` — nothing includes `wxMediaCtrl2.h` on macOS anymore; `wxMediaCtrl2` is never instantiated on any platform).
- Produces: a clean tree where the old BambuPlayer-based player is gone from macOS. The `BambuPlayer` ObjC class itself remains inside the network plugin (external prebuilt binary) — only the GUI-side consumer is removed.

- [ ] **Step 1: Delete the old player files**

```bash
git rm src/slic3r/GUI/wxMediaCtrl2.mm
git rm src/slic3r/GUI/BambuPlayer/BambuPlayer.h
rmdir src/slic3r/GUI/BambuPlayer 2>/dev/null || true
```

- [ ] **Step 2: Strip the __WXMAC__ section from wxMediaCtrl2.h**

In `src/slic3r/GUI/wxMediaCtrl2.h`, remove the entire macOS branch of the `#ifdef __WXMAC__` guard — from `#ifdef __WXMAC__` (line 22) through the closing `};` of the mac class (line 60), and the `#else` marker — leaving only the non-mac `class wxMediaCtrl2 : public wxMediaCtrl { ... };` definition followed by the final `#endif /* wxMediaCtrl2_h */`. The resulting file keeps its `#ifndef`/`#endif` include guard pair balanced.

The file stays on disk because Win/Linux compile `wxMediaCtrl2.cpp`, which includes it.

- [ ] **Step 3: Grep for leftover references**

```bash
grep -rn "wxMediaCtrl2.mm\|BambuPlayer/BambuPlayer.h\|BambuPlayer" src/slic3r --include="*.cpp" --include="*.h" --include="*.mm" --include="*.txt"
```

Expected: no hits in `src/slic3r/GUI` (ignore `localization/i18n/list.txt:196`, which lists the Win/Linux `wxMediaCtrl2.cpp` and stays).

- [ ] **Step 4: Rebuild the app**

```bash
cmake --build build_arm64 --config RelWithDebInfo --target OrcaSlicer
```

Expected: configure + compile + link succeed with the deleted files gone.

- [ ] **Step 5: Commit**

```bash
git add -A src/slic3r/GUI
git commit -m "refactor: remove old BambuPlayer-based media player from macOS"
```

---

### Task 4: Runtime verification on hardware

**Files:** none — manual verification.

**Interfaces:** consumes all prior tasks. Final gate: the new player must actually stream on a Mac.

- [ ] **Step 1: Launch the freshly built app**

```bash
open build_arm64/src/RelWithDebInfo/OrcaSlicer.app
```

Expected: app launches normally; no crash in the network/device subsystem.

- [ ] **Step 2: Load the network plugin and open the Device tab**

Log in / ensure the network plugin (`libBambuSource.dylib`) loads, select a printer, open the Device tab (camera monitoring panel).

Expected: the camera preview area shows the idle image initially (no crash — this exercises `wxMediaCtrl3::SetIdleImage` and the `wxImage` load path on macOS for the first time).

- [ ] **Step 3: Start the stream and watch it render**

Click play / wait for `MediaPlayCtrl` to start the stream.

Expected: live video renders in the panel. Check the console/log output (`BOOST_LOG` goes to the terminal if run from it, or check the log file):
- `stat_log ...` lines appear (the `EVT_MEDIA_CTRL_STAT` path is live — proves the Bambu C API dlsym worked from `libBambuSource.dylib`);
- no repeated decode/error messages like `AVVideoDecoder: ...` or `can not find function ...` (proves `StaticBambuLib::get` resolved all Bambu functions);
- Stop/Play toggle works; idle image reappears on stop;
- window resize keeps aspect ratio (exercises `DoSetSize`/`adjust_frame_size`/`paintEvent`).

- [ ] **Step 4: Confirm the old player is really gone**

Expected: nothing in the logs references `BambuPlayer` (the ObjC class is no longer dlsym'd); the video path is entirely `wxMediaCtrl3` + `AVVideoDecoder`.

If a printer is unavailable, at minimum verify Steps 1–2 (launch + idle image) and note in the PR that live-stream verification needs hardware.

- [ ] **Step 5: Final review pass**

```bash
git log --oneline -6
git show --stat HEAD  # and each of the three task commits
```

Expected: the last 4 commits are the design doc + the 3 implementation tasks (each task commit touches only its listed files). Review the diff for scope: no Linux/Windows changes beyond the two `EVT_MEDIA_CTRL_STAT` lines in Task 1, no `StatusPanel`/`MediaPlayCtrl` changes.
