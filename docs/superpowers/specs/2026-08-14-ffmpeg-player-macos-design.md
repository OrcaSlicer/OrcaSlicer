# FFmpeg Media Player for macOS — Design

Date: 2026-08-14
Branch: `dev/ffmpeg-player-macos`

## Problem

The branch's new FFmpeg-based media player (`wxMediaCtrl3` + `AVVideoDecoder`) is used on
Windows and Linux, but macOS still runs the old player: `wxMediaCtrl2.mm`, an ObjC
`BambuPlayer` class dlsym'd from the Bambu network plugin that renders via CALayer.
On macOS, `wxMediaCtrl3` is currently aliased to `wxMediaCtrl2` and FFmpeg is not linked
into the app at all.

Goal: make macOS use the same FFmpeg player as Windows/Linux, linking the **static**
FFmpeg libraries from the deps build instead of dynamic ones.

## Current state (verified)

- New player (Win/Linux): `GUI/wxMediaCtrl3.cpp` + `GUI/AVVideoDecoder.cpp`. Decodes with
  FFmpeg (libavcodec/libswscale/libavutil), renders frames into `wxImage` (non-Windows) /
  `wxBitmap` (Windows) drawn in a `paintEvent`, feeds via the `Bambu_*` C API
  (`BambuTunnel.h`, `BAMBU_DYNAMIC`) dlsym'd from the network plugin through
  `StaticBambuLib::get()` (`GUI/Printer/PrinterFileSystem.cpp`, compiled on all platforms).
- Old player (macOS): `GUI/wxMediaCtrl2.mm` uses the ObjC `BambuPlayer` class found via
  `dlsym(module, "OBJC_CLASS_$_BambuPlayer")` in `libBambuSource.dylib`.
- The macOS network plugin `libBambuSource.dylib` already exports the full Bambu C API
  (verified with `nm`), so the new player needs zero plugin changes.
- FFmpeg linking in `src/slic3r/CMakeLists.txt` is guarded by `if (NOT APPLE)` —
  macOS currently does not link FFmpeg.
- `deps/FFMPEG/FFMPEG.cmake`: non-MSVC branch builds FFmpeg from source with
  `--enable-shared`. The existing arm64 deps build on the dev machine happened to be
  configured with both static and shared enabled, so `libavcodec.a` / `libswscale.a` /
  `libavutil.a` are already present at
  `deps/build/arm64/OrcaSlicer_dep/usr/local/lib/`.
- `EVT_MEDIA_CTRL_STAT` is `wxDEFINE_EVENT`'d in `wxMediaCtrl2.cpp` (Win/Linux) and
  `wxMediaCtrl2.mm` (macOS); the define in `wxMediaCtrl3.cpp` is commented out.
- `wxMediaCtrl2` is never instantiated anywhere on any platform — dead code.
- `StatusPanel` already creates `wxMediaCtrl3`; `MediaPlayCtrl` only uses the
  `wxMediaCtrl3` interface (`Load/Play/Stop/GetState/GetVideoSize/GetLastError/SetIdleImage`),
  so no UI-side changes are needed.

## Approach (approved)

**Reuse the shared player on macOS.** Compile the existing `wxMediaCtrl3.cpp` +
`AVVideoDecoder.cpp` on macOS so all three platforms run one implementation.
Rendering uses the existing `wxImage` → `DrawBitmap` paint path, identical to Linux.
Known trade-off: frames are scaled to the widget's logical (1x) size, so Retina is
slightly soft compared to the old CALayer player. Accepted for now; a Retina-aware
scaling follow-up is possible later.

Rejected alternative: a native CGImage/CALayer renderer for macOS — faster and
Retina-crisp, but adds a second render implementation to maintain.

## Changes

### 1. Enable the FFmpeg player on macOS (source)

- `GUI/wxMediaCtrl3.h`: remove the `#ifdef __WXMAC__` branch (lines 18–22) that aliases
  `wxMediaCtrl3` → `wxMediaCtrl2`. macOS then compiles the real `wxMediaCtrl3` class,
  including the `BAMBU_DYNAMIC` BambuTunnel path used on Linux.
- Event symbol fix: move `wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent)` into
  `wxMediaCtrl3.cpp` (uncomment the existing line) and remove it from
  `wxMediaCtrl2.cpp`. One definition total in the lib; all three platforms resolve it.

### 2. Static FFmpeg linking (deps + app)

- `deps/FFMPEG/FFMPEG.cmake`: in the non-MSVC branch, pass
  `--disable-shared --enable-static` when `APPLE`. Linux keeps `--enable-shared`;
  Windows keeps its prebuilt shared DLL zips. Fresh macOS deps builds install only
  `libavcodec.a` / `libswscale.a` / `libavutil.a` — no dylibs to bundle, no
  rpath/install_name handling. (The existing local arm64 deps build already contains
  the `.a` files, so no deps rebuild is strictly needed to try the change locally,
  but a fresh CI deps build must produce them.)
- `src/slic3r/CMakeLists.txt`:
  - APPLE branch of `SLIC3R_GUI_SOURCES`: add `GUI/wxMediaCtrl3.cpp`,
    `GUI/wxMediaCtrl3.h`, `GUI/AVVideoDecoder.cpp`, `GUI/AVVideoDecoder.hpp`;
    remove `GUI/wxMediaCtrl2.mm` and `GUI/wxMediaCtrl2.h` (the `.h` stays on
    disk for the Win/Linux build of `wxMediaCtrl2.cpp`, but nothing on macOS
    includes it after this change).
  - Add an APPLE mirror of the `NOT APPLE` FFmpeg block: `find_library` for
    `libavcodec.a`, `libswscale.a`, `libavutil.a` under `${CMAKE_PREFIX_PATH}/lib`
    with `NO_DEFAULT_PATH`, link them (order avcodec → swscale → avutil), and add
    `${CMAKE_PREFIX_PATH}/include` as a SYSTEM include directory. Deps are built with
    `--disable-zlib` and no external codecs, so the three static libs link cleanly.

### 3. Remove the old player

- Delete `GUI/wxMediaCtrl2.mm` and `GUI/BambuPlayer/BambuPlayer.h` (header used only
  by the `.mm`; the real `BambuPlayer` lives inside the network plugin).
- Remove the now-dead `__WXMAC__` section of `GUI/wxMediaCtrl2.h`.
- `wxMediaCtrl2.cpp` (Win/Linux) stays in the build as-is (dead but harmless; out of
  scope to remove on this branch).

### 4. Verification

- Build on macOS: `cmake --build build_arm64` (or `build/arm64`).
- Confirm no dynamic FFmpeg dependency: `otool -L` on the app binary shows no `libav*`
  dylib references.
- Runtime: with the network plugin loaded, the Device tab camera preview streams via
  the FFmpeg player (check the device page / `MediaPlayCtrl`).
- macOS `ctest` still passes — static linking means no test-executable `.so` copying
  hacks (unlike the Linux shared-lib setup).

## Out of scope

- Linux (shared libs, AppImage/flatpak bundling) and Windows (prebuilt DLL zips)
  keep their current FFmpeg setup.
- Retina-aware frame scaling / native CGImage rendering (follow-up if visual quality
  is judged insufficient).
- Audio streaming (neither player plays audio in this UI path).
