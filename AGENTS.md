# Repository Operator Guide (AGENTS.md)

This file is the canonical operator/developer guide for working in this repository.

## Project Structure & Module Organization

- `src/libslic3r/`: core slicing engine and geometry algorithms.
- `src/slic3r/`: desktop application logic and wxWidgets GUI.
- `src/libvgcode/`: G-code visualization and viewer support.
- `src/portability/`: portability contracts and adapter implementations (desktop + iOS scaffold).
- `src/mobile/ios/`: iOS shell/app entry points and simulator-oriented app target files.
- `resources/`: bundled assets, presets, localization resources, and web assets.
- `localization/`: translation source files and update assets.
- `tests/`: Catch2-based test suites and test fixtures.
- `cmake/`: project CMake modules and configuration helpers.
- `scripts/` and `tools/`: automation and developer tooling.
- `doc/`: first-party documentation and planning notes.
- `deps/`, `deps_src/`: vendored third-party snapshots (avoid direct edits unless intentionally updating vendored code).

## Build, Test, and Development Commands

### Core CMake flow

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OrcaSlicer --config Release --parallel
```

### Enable and run tests

`BUILD_TESTS` defaults to `OFF` in root `CMakeLists.txt`, so enable it explicitly when needed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --target tests --config Release --parallel
ctest --test-dir build --output-on-failure
```

### Linux helper script flow

```bash
./build_linux.sh -u      # install system deps
./build_linux.sh -dsi    # build bundled deps + app + AppImage
```

Use `./build_linux.sh -h` for the current flag reference.

### iOS portability smoke flow

```bash
cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DORCASLICER_BUILD_IOS_PORTABILITY=ON
cmake --build build-ios --target orcaslicer_ios_smoke --config Release
```

## Coding Style & Naming Conventions

- C++ standard: C++17 (with selective modern usage as already present in the codebase).
- Formatting: `.clang-format` (4 spaces, 140 column limit, brace rules).
- Naming style used in project code:
  - Classes/types: `CamelCase`
  - Functions/local variables: `snake_case`
  - Constants/macros: `SCREAMING_CASE`
- Keep portability layer (`src/portability/**`) free of desktop UI includes (`<wx/...>`, desktop OpenGL headers).

## Testing Guidelines

- Test framework: Catch2 under `tests/catch2/`.
- Main test areas:
  - `tests/libslic3r/`
  - `tests/fff_print/`
  - `tests/sla_print/`
  - `tests/libnest2d/`
  - `tests/slic3rutils/`
- Add deterministic fixtures under `tests/data/` for algorithm changes.
- Document manual validation for behavior that cannot be covered by automated tests.

## CI / Workflows Snapshot

Current workflow files are under `.github/workflows/`, including:

- Build and dependency workflows (`build_all.yml`, `build_orca.yml`, `build_deps.yml`, `build_check_cache.yml`)
- Validation workflows (`check_locale.yml`, `check_profiles.yml`, `check_profiles_comment.yml`, `shellcheck.yml`)
- iOS verification workflows (`ios_simulator_smoke.yml`, `ios_ui_screenshot.yml`)
- Automation/support workflows (`assign.yml`, `auto-close-duplicates.yml`, `backfill-duplicate-comments.yml`, `dedupe-issues.yml`, `doxygen-docs.yml`, `update-translation.yml`)

## Platform Status (high level)

- Desktop (Windows/macOS/Linux): primary production platform.
- iOS: scaffolded portability + shell/screenshot/smoke infrastructure exists; not feature-parity with desktop.
- Android: planned after iOS milestones; no equivalent production shell yet.

## Contributor Caveats

- Do not modify files in `deps/` or `deps_src/` casually; these are vendored snapshots.
- Keep docs aligned with implementation reality, especially for mobile portability status.
- Prefer concrete statements over roadmap speculation unless explicitly labeled as planned.

## Markdown File Inventory

Status legend:
- **current**: appears aligned with code/repo state.
- **partially stale**: useful but has scope/time sensitivity or minor drift risk.
- **likely stale**: appears old, placeholder, or maintained outside this repo’s first-party docs process.

### Root / repository operations

- **Path:** `AGENTS.md`  
  **Purpose:** Canonical operator/developer guide for repository workflows.  
  **Audience:** Contributors, automation agents, maintainers.  
  **Status:** current.

- **Path:** `README.md`  
  **Purpose:** Public project overview, install/build pointers, links to community/docs.  
  **Audience:** End users and new contributors.  
  **Status:** current.

- **Path:** `SECURITY.md`  
  **Purpose:** Security vulnerability reporting policy and disclosure guidance.  
  **Audience:** Security researchers and maintainers.  
  **Status:** current.

- **Path:** `CLAUDE.md`  
  **Purpose:** AI-assistant contributor guidance for this codebase.  
  **Audience:** Tooling/automation agents and developers using Claude workflows.  
  **Status:** current.

### Internal command/automation docs

- **Path:** `.claude/commands/commit-push-pr.md`  
  **Purpose:** Claude command recipe for commit/push/PR automation.  
  **Audience:** Maintainers using Claude command packs.  
  **Status:** partially stale (tooling-specific and environment-dependent).

- **Path:** `.claude/commands/dedupe.md`  
  **Purpose:** Procedure for duplicate issue triage automation.  
  **Audience:** Oncall/support automation operators.  
  **Status:** partially stale (depends on GitHub workflow conventions).

- **Path:** `.claude/commands/oncall-triage.md`  
  **Purpose:** Procedure for high-priority issue labeling triage.  
  **Audience:** Oncall maintainers and automation operators.  
  **Status:** partially stale (process and thresholds are policy-sensitive).

- **Path:** `.github/pull_request_template.md`  
  **Purpose:** Standard PR template fields used on GitHub.  
  **Audience:** Contributors opening pull requests.  
  **Status:** current.

### First-party docs (`doc/`)

- **Path:** `doc/2026-03-18-in-depth-file-search-and-code-review.md`  
  **Purpose:** Point-in-time engineering review and findings snapshot.  
  **Audience:** Maintainers, reviewers.  
  **Status:** partially stale (dated snapshot by design).

- **Path:** `doc/mobile-porting/README.md`  
  **Purpose:** Index and scope framing for mobile porting documents.  
  **Audience:** Contributors working on portability/mobile efforts.  
  **Status:** current.

- **Path:** `doc/mobile-porting/reference-map.md`  
  **Purpose:** Module map and search anchors for portability work.  
  **Audience:** Portability contributors/maintainers.  
  **Status:** current.

- **Path:** `doc/mobile-porting/ios-android-porting-plan.md`  
  **Purpose:** Staged architecture plan for iOS-first, Android-ready work.  
  **Audience:** Portability architects and implementers.  
  **Status:** current.

- **Path:** `doc/mobile-porting/file-edit-plan.md`  
  **Purpose:** File-level checklist for portability refactors.  
  **Audience:** Contributors implementing portability changes.  
  **Status:** partially stale (planning checklist; may lag landed work).

- **Path:** `doc/mobile-porting/implementation-status.md`  
  **Purpose:** Current status of mobile portability implementation and gaps.  
  **Audience:** Portability contributors/reviewers.  
  **Status:** current.

### Scripts and test-process docs

- **Path:** `scripts/linux.d/README.md`  
  **Purpose:** Explains distro-resolution logic used by `build_linux.sh`.  
  **Audience:** Linux contributors and release engineers.  
  **Status:** current.

- **Path:** `tests/CLAUDE.md`  
  **Purpose:** Detailed Catch2 testing practices and pitfalls for contributors/agents.  
  **Audience:** Test authors and automation agents.  
  **Status:** partially stale (contains some version-sensitive guidance and examples).

### Resource placeholders and embedded web docs

- **Path:** `resources/tooltip/.md`  
  **Purpose:** Placeholder notice for tooltip documentation (English).  
  **Audience:** Contributors/localizers.  
  **Status:** likely stale (placeholder only).

- **Path:** `resources/tooltip/zh_CN/.md`  
  **Purpose:** Placeholder notice for tooltip documentation (Simplified Chinese).  
  **Audience:** Contributors/localizers.  
  **Status:** likely stale (placeholder only).

- **Path:** `resources/web/include/swiper/README.md`  
  **Purpose:** Vendored Swiper package readme used by embedded web assets.  
  **Audience:** Front-end dependency maintainers.  
  **Status:** partially stale (third-party upstream doc snapshot).

- **Path:** `resources/web/include/swiper/node_modules/dom7/README.md`  
  **Purpose:** Vendored Dom7 dependency documentation.  
  **Audience:** Front-end dependency maintainers.  
  **Status:** likely stale (third-party snapshot).

- **Path:** `resources/web/include/swiper/node_modules/ssr-window/README.md`  
  **Purpose:** Vendored ssr-window dependency documentation.  
  **Audience:** Front-end dependency maintainers.  
  **Status:** likely stale (third-party snapshot).

- **Path:** `resources/web/guide/swiper/README.md`  
  **Purpose:** Vendored Swiper package readme for guide assets.  
  **Audience:** Front-end dependency maintainers.  
  **Status:** partially stale (third-party upstream doc snapshot).

- **Path:** `resources/web/guide/swiper/node_modules/dom7/README.md`  
  **Purpose:** Vendored Dom7 dependency documentation.  
  **Audience:** Front-end dependency maintainers.  
  **Status:** likely stale (third-party snapshot).

- **Path:** `resources/web/guide/swiper/node_modules/ssr-window/README.md`  
  **Purpose:** Vendored ssr-window dependency documentation.  
  **Audience:** Front-end dependency maintainers.  
  **Status:** likely stale (third-party snapshot).

### Vendored dependency markdown (`deps/`, `deps_src/`)

- **Path:** `deps/EXPAT/expat/README.md`  
  **Purpose:** Upstream Expat documentation snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps/GLEW/glew/README.md`  
  **Purpose:** Upstream GLEW documentation snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/README_CMAKE_INTERFACES.md`  
  **Purpose:** Notes about CMake interface targets for vendored dependencies.  
  **Audience:** Build maintainers.  
  **Status:** current.

- **Path:** `deps_src/earcut/CHANGELOG.md`  
  **Purpose:** Upstream earcut changelog snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/earcut/README.md`  
  **Purpose:** Upstream earcut readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/eigen/README.md`  
  **Purpose:** Upstream Eigen readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/fast_float/README.md`  
  **Purpose:** Upstream fast_float readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/hidapi/README.md`  
  **Purpose:** Upstream hidapi readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/imgui/README.md`  
  **Purpose:** Upstream imgui readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/imguizmo/README.md`  
  **Purpose:** Upstream imguizmo documentation snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/libigl/igl/copyleft/README.md`  
  **Purpose:** Upstream libigl/copyleft notes snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/mcut/README.md`  
  **Purpose:** Upstream mcut readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/md4c/LICENSE.md`  
  **Purpose:** Upstream md4c license file.  
  **Audience:** Compliance/legal and dependency maintainers.  
  **Status:** current (license snapshot).

- **Path:** `deps_src/md4c/README.md`  
  **Purpose:** Upstream md4c readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/miniz/ChangeLog.md`  
  **Purpose:** Upstream miniz changelog snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/miniz/readme.md`  
  **Purpose:** Upstream miniz readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).

- **Path:** `deps_src/qoi/README.md`  
  **Purpose:** Upstream qoi readme snapshot.  
  **Audience:** Dependency updaters.  
  **Status:** likely stale (vendored upstream snapshot).
