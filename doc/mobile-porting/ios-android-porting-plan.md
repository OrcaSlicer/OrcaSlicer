# OrcaSlicer Mobile Port Plan (iOS-first, Android-ready)

Last updated: 2026-03-18

## Goal
Port OrcaSlicer so the **slicing engine remains shared** and only platform/runtime layers swap per target (desktop, iOS, Android).

## Guiding architecture
Use a layered split:

1. **Core Engine (shared C++)**
   - `src/libslic3r/**`
   - pure slicing, geometry, presets model, project IO, gcode generation.
   - no direct wxWidgets/OpenGL/UI symbols.

2. **Application Services (shared C++)**
   - non-UI orchestration for loading models, configuring print jobs, running background slicing, progress events, upload abstractions.
   - currently mixed under `src/slic3r/Utils/**` and parts of `src/slic3r/GUI/**`.

3. **Platform Adapter Layer (replaceable)**
   - filesystem paths, secure storage, network reachability, threading/task dispatch, logging, crash reporting, file pickers/share sheets.
   - one adapter per platform: desktop(wx), iOS, Android.

4. **Renderer Layer (replaceable)**
   - scene graph inputs from shared model + camera state.
   - backend implementations:
     - desktop OpenGL (existing)
     - iOS Metal (or GLES as short-term bridge)
     - Android Vulkan/GLES (later)

5. **Native UI Shell (platform-native)**
   - iOS: SwiftUI/UIKit
   - Android: Jetpack Compose/View
   - desktop: wxWidgets (existing)

## Canonical module layout
- Portability interfaces and adapters live in `src/portability/**`.
- iOS-first modules currently scaffolded:
  - `src/portability/platform/ios/IOSPlatformServices.*`
  - `src/portability/render/ios/IOSMetalRenderBackend.*`
- Any equivalent service classes that still live in `src/slic3r/Utils/**` are transitional desktop wiring and should migrate behind portability interfaces.

## What should stay identical across iOS/Android
- `libslic3r` algorithms and print pipeline
- project/profile semantics and serialization (`Format/*`, presets)
- gcode generation behavior and deterministic outputs
- validation logic and machine/process compatibility checks

## Where the codebase is currently tightly coupled
- `libslic3r_gui` target (`src/slic3r/CMakeLists.txt`) compiles GUI + many `Utils` files together.
- wxWidgets types appear broadly in `src/slic3r/Utils/**`.
- OpenGL-centric rendering lives in `src/slic3r/GUI/{GLCanvas3D,OpenGLManager,GLModel,GLTexture,GLShader,...}`.
- app entrypoint (`src/OrcaSlicer.cpp`) includes both CLI/core and GUI/OpenGL headers.

## Recommended extraction path

### Phase 1: Create mobile-safe facades (no behavior change)
- Introduce interfaces for platform services and rendering hooks.
- Wrap existing wx/OpenGL implementations behind desktop adapters.
- Keep desktop build behavior unchanged.

### Phase 2: Separate target libraries
- New static libraries:
  - `orcaslicer_core` (mostly current `libslic3r`)
  - `orcaslicer_app` (UI-agnostic orchestration)
  - `orcaslicer_render_api` (renderer interfaces)
  - `orcaslicer_platform_api` (platform service interfaces)
  - existing desktop impl libs (`orcaslicer_desktop_platform`, `orcaslicer_desktop_renderer`)

### Phase 3: iOS bootstrap
- Build `orcaslicer_core + orcaslicer_app` for iOS toolchain.
- Implement iOS adapters (storage, networking callbacks, task dispatch, secure keychain, file access).
- Only after portability + app-service gates are complete, add a minimal native iOS shell that does:
  - app launch bring-up
  - one Metal-backed view wired through `IOSMetalRenderBackend`
- Reuse existing `IOSPlatformServices` and `IOSMetalRenderBackend` first; defer broader UI/product flows until after this launch + Metal-view milestone.

### Phase 4: Android bootstrap (same interfaces)
- Reuse shared C++ libs and platform/render contracts.
- Implement Android JNI bridge + renderer backend.

## Renderer modularization recommendation
Define a backend-neutral renderer API first (example concepts):
- scene upload/update
- camera control
- picking/raycast
- tool overlays/gizmos
- gcode path visualization
- texture/font atlas upload

Then keep desktop OpenGL code as the first implementation of this API.

`src/libvgcode` already has a useful precedent: it supports both desktop GL and GLES via compile definitions.

## Risks to plan for
- wxString/wx event types leaking into non-UI modules.
- Threading assumptions tied to wx event loop.
- macOS-specific Objective-C++ helpers (`*.mm`) embedded in shared code paths.
- ImGui usage tightly coupled with OpenGL state.

## Post-shell workflow phase (immediately after first iOS shell milestone)
After the launch + Metal-view shell is proven, execute workflow closure in this strict order:

1. **Import**
   - Open a project/model from native iOS entry points and materialize it in shared C++ state.
2. **Slice job**
   - Trigger background slicing from the iOS shell and surface progress/error completion states.
3. **Preview render**
   - Render sliced output/preview scene through mobile renderer pathways with deterministic outputs.
4. **Export/share**
   - Export generated artifacts and hand off through native share/open-in flows.

## Milestones and acceptance criteria

### Milestone 1 — Portability contracts and desktop adapter safety
- Scope: facades/interfaces introduced; desktop behavior unchanged.
- Acceptance criteria:
  - Desktop builds remain green with adapters enabled.
  - No regressions in existing desktop launch + slice sanity flow.

### Milestone 2 — Mobile-safe shared libraries
- Scope: mobile-safe build separation for core/app contracts.
- Acceptance criteria:
  - Headless/mobile-targeted libraries compile without wx/OpenGL headers in portability boundaries.
  - Portability include guard checks pass in CI/local configure.

### Milestone 3 — iOS shell bring-up (launch + viewport shell only)
- Scope: app launch + Metal-backed viewport shell route.
- Acceptance criteria:
  - iOS simulator smoke build and launch pass.
  - Screenshot CI captures all defined shell scenes successfully.
  - Explicit limitation acknowledged: this milestone does **not** certify import/slice/preview/export workflow completion.

### Milestone 4 — Post-shell workflow closure (import → slice job → preview render → export/share)
- Scope: complete first functional user workflow on iOS using shared engine + adapters.
- Acceptance criteria:
  - Import flow validated from native file entry to loaded project/model state.
  - Slice job validated with progress + completion/error reporting.
  - Preview render validated for sliced content presentation path.
  - Export/share validated end-to-end to at least one supported destination.

### Milestone 5 — Android bootstrap on same contracts
- Scope: Android platform/render replacement using the same shared C++ APIs.
- Acceptance criteria:
  - Android prototype builds and runs minimal import/slice/preview/export workflow using the shared contracts.
  - No platform-specific forks introduced in shared engine behavior.

## Out of scope for this planning set
- Product-roadmap decisions, UX prioritization, or release dates.
- Any migration that requires modifying vendored third-party code under `deps/` or `deps_src/`.
