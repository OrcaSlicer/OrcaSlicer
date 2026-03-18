# Mobile Porting Implementation Status

## Status snapshot
The repository now has a portability scaffold under `src/portability/` and an initial iOS module entry-point. Current work is intentionally iOS-first with interfaces shared for Android follow-up.

## Current state vs target state
| Area | Current state | Target state | Migration status |
|---|---|---|---|
| Platform service contract | `src/portability/platform/IPlatformServices.hpp` and `ICredentialStore.hpp` | Keep contracts in `src/portability/platform/` | In progress |
| Desktop platform adapter | `src/portability/platform/DesktopPlatformServices.*` + `DesktopInMemoryCredentialStore.*` | Keep as desktop adapter implementation | In progress |
| iOS platform adapter | `src/portability/platform/ios/IOSPlatformServices.mm` now bridges Foundation + GCD for paths/thread dispatch and uses `IOSKeychainCredentialStore` for credentials | Harden Apple API integration edge-cases and extend lifecycle coverage for app/background transitions | In progress |
| Renderer contract | `src/portability/render/IRenderBackend.hpp` | Keep backend-neutral API in `src/portability/render/` | In progress |
| iOS renderer adapter | `src/portability/render/ios/IOSMetalRenderBackend.*` | Wire to Metal render pipeline and scene commands | Scaffolded |
| Build integration | `src/CMakeLists.txt` + `src/portability/CMakeLists.txt` | Canonical portability graph (`orcaslicer_portability_api`, `orcaslicer_platform_desktop`, `orcaslicer_render_null`, plus iOS targets on iOS toolchains) | Scaffolded |
| iOS renderer adapter | `src/portability/render/ios/IOSMetalRenderBackend.mm` initializes Metal device/queue/layer, resizes drawable, and submits a basic clear render pass | Integrate scene command submission/resources and harden frame/layer lifecycle | In progress |
| Build integration | `src/CMakeLists.txt` + `src/portability/CMakeLists.txt` | iOS targets (`orcaslicer_platform_ios`, `orcaslicer_render_ios_metal`) built only for iOS toolchains | Scaffolded |

## What landed in this phase
- Normalized portability interfaces and namespaces under `Slic3r::portability::*` with renderer APIs and scene integration standardized on `Slic3r::portability::render`.
- Added iOS platform services module implementing `IPlatformServices` with Foundation/GCD bridging and a Keychain-backed `ICredentialStore` (`IOSKeychainCredentialStore`).
- Added iOS Metal renderer module implementing `IRenderBackend` with device/queue/layer setup plus a basic clear-color render pass.
- Added iOS-specific CMake subtargets gated by iOS toolchain detection.

## Temporary/legacy placement note
Some desktop integration still exists in `src/slic3r/Utils` for launch/process and wx-bound application wiring. This is temporary and should be migrated behind `src/portability/**` contracts as we move additional services out of GUI-coupled modules.

## Next iOS-focused steps
1. Integrate scene command submission/upload lifecycle into `IOSMetalRenderBackend` so the Metal backend renders real scene content rather than clear-only frames.
2. Harden iOS platform/renderer lifecycle handling (layer ownership, suspend/resume, and thread handoff edge-cases) across `IOSPlatformServices` and `IOSMetalRenderBackend`.
3. Expand smoke/CI coverage for iOS portability targets (including credential-store and renderer bring-up paths) to catch regressions earlier.

## Engine + module completion plan (target: 100% before iOS UI)

The iOS UI should start only after shared-engine and portability modules are functionally complete and stable. Use the checklist below as the release gate.

### Gate 1: Shared engine portability complete (100%)
- [ ] Ensure `orcaslicer_core`-candidate code paths are free of direct wx/OpenGL/UI includes and runtime dependencies.
- [ ] Verify deterministic slicing output parity against desktop baseline fixtures (`tests/data/**`) for representative FFF/SLA scenarios.
- [ ] Confirm project/profile load-save and gcode generation behavior parity for mobile-targeted workflows.

### Gate 2: Application service modules complete (100%)
- [ ] Move remaining mobile-relevant orchestration from `src/slic3r/Utils/**` into portability-safe service boundaries.
- [ ] Replace wx event-loop assumptions with platform-neutral async/task abstractions consumed through `IPlatformServices`.
- [ ] Finalize error/reporting/progress contracts so native UI consumes stable view-state APIs instead of internal engine types.

### Gate 3: iOS portability adapters complete (100%)
- [ ] Complete `IOSPlatformServices` lifecycle handling: foreground/background transitions, cancellation, and thread handoff correctness.
- [ ] Complete `IOSKeychainCredentialStore` edge-case handling (missing keys, migration/update semantics, failure propagation).
- [ ] Complete `IOSMetalRenderBackend` scene submission path (mesh upload, camera updates, frame synchronization, resource teardown).

### Gate 4: Build + validation readiness complete (100%)
- [ ] Keep iOS targets (`orcaslicer_platform_ios`, `orcaslicer_render_ios_metal`, `orcaslicer_ios_smoke`) green in CI/toolchain smoke builds.
- [ ] Add/expand unit and smoke coverage for portability APIs and adapter bring-up paths.
- [ ] Document known limitations as explicit blockers; require zero P0/P1 portability regressions before UI kickoff.

### UI kickoff criteria
Start iOS UI implementation only when all four gates are checked and the team can demonstrate:
- successful end-to-end flow (load model -> slice -> preview -> export) through portability adapters,
- reproducible iOS smoke build success,
- no unresolved critical coupling to desktop-only modules.

## Deferred until after iOS milestone
- Android adapter implementation (JNI + Vulkan/GLES backend).
- Android build composition and packaging automation.

## iOS smoke target invocation
Canonical portability targets exported from `src/CMakeLists.txt` are:

- `orcaslicer_portability_api`
- `orcaslicer_platform_desktop`
- `orcaslicer_render_null`

`orcaslicer_portability` remains as an interface compatibility meta-target that only forwards to the canonical targets above (no duplicated compilation units).

When configuring with an iOS toolchain (for example `-DCMAKE_SYSTEM_NAME=iOS` or an iPhone OS/simulator sysroot), CMake adds `orcaslicer_ios_smoke`, a minimal smoke target in `src/portability/` that links:

- `libslic3r`
- `orcaslicer_portability_api`
- `orcaslicer_platform_ios`
- `orcaslicer_render_ios_metal`

Expected invocation:

```bash
cmake -S . -B build-ios \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DORCASLICER_BUILD_IOS_PORTABILITY=ON

cmake --build build-ios --target orcaslicer_ios_smoke --config Release
```

Configure-time status output explicitly reports whether iOS portability detection is active and whether the smoke target is enabled.
## Namespace contract
- Renderer-facing portability interfaces use `Slic3r::portability::render` as the canonical namespace (`IRenderBackend`, `ISceneRenderer`, `DesktopOpenGLSceneRenderer`, and GUI integration call sites).
- Compatibility aliases were not retained; callers should migrate directly to the canonical namespace.
