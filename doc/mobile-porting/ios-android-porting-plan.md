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

## Milestone deliverables
1. Build still passes on desktop with adapters enabled.
2. Headless mobile-safe library builds with no wx/OpenGL includes.
3. iOS demo app can load project + run slice + render preview via adapter.
4. Android prototype reuses same C++ APIs with only platform/render replacements.

## Out of scope for this planning set
- Product-roadmap decisions, UX prioritization, or release dates.
- Any migration that requires modifying vendored third-party code under `deps/` or `deps_src/`.
