# CLAUDE.md

Guidance for automation agents and contributors using Claude-oriented workflows in this repository.

## Repository Overview

OrcaSlicer is a C++17 project built with CMake.

Key first-party areas:

- `src/libslic3r/`: slicing engine and geometry processing.
- `src/slic3r/`: desktop app and wxWidgets GUI.
- `src/libvgcode/`: G-code visualization/viewer support.
- `src/portability/`: platform/render portability contracts and adapters.
- `src/mobile/ios/`: iOS shell/app target files.
- `tests/`: Catch2 suites.

## Build Commands

### Standard desktop configure + build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OrcaSlicer --config Release --parallel
```

### Build tests

`BUILD_TESTS` defaults to `OFF` in root `CMakeLists.txt`; enable it explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --target tests --config Release --parallel
ctest --test-dir build --output-on-failure
```

### Linux helper flow

```bash
./build_linux.sh -u
./build_linux.sh -dsi
```

Use `./build_linux.sh -h` for current script flags.

### iOS portability smoke flow

```bash
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DORCASLICER_BUILD_IOS_PORTABILITY=ON
cmake --build build-ios --target orcaslicer_ios_smoke --config Release
```

## Architecture Notes

- Core slicing logic should remain in shared engine modules (`libslic3r`) without desktop GUI dependencies.
- `src/portability/**` is reserved for platform-agnostic contracts and adapter implementations.
- Avoid desktop UI includes in portability code (`<wx/...>`, desktop OpenGL headers).

## Testing Notes

- Framework: Catch2 (in-repo under `tests/catch2`).
- Main suites:
  - `tests/libslic3r`
  - `tests/fff_print`
  - `tests/sla_print`
  - `tests/libnest2d`
  - `tests/slic3rutils`

For detailed testing conventions and Catch pitfalls, see `tests/CLAUDE.md`.

## Mobile Portability Docs of Record

- `doc/mobile-porting/README.md`
- `doc/mobile-porting/reference-map.md`
- `doc/mobile-porting/ios-android-porting-plan.md`
- `doc/mobile-porting/file-edit-plan.md`
- `doc/mobile-porting/implementation-status.md`

Use these as the source of truth for mobile/portability status and planned sequencing.
