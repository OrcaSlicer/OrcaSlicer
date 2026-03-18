# Mobile Porting Implementation Status

## Status snapshot (repo truth)
The iOS-first portability scaffold is present in-tree and compiles behind iOS toolchain detection. The scaffolded target names are already established and should remain stable:

- `orcaslicer_portability_api`
- `orcaslicer_platform_desktop`
- `orcaslicer_render_null`
- `orcaslicer_portability` (compatibility meta-target)
- `orcaslicer_platform_ios` (iOS only)
- `orcaslicer_render_ios_metal` (iOS only)
- `orcaslicer_ios_smoke` (iOS only)

Current state: contracts + build graph + basic adapters are landed; full mobile runtime behavior and real 3D draw submission are not.

## What is landed

### 1) Portability contracts and canonical namespaces
- `IPlatformServices` and `ICredentialStore` exist under `src/portability/platform/` as platform-neutral contracts.
- `IRenderBackend` and `ISceneRenderer` exist under `src/portability/render/` with canonical namespace `Slic3r::portability::render`.
- `SceneState` is GUI-independent and carries portable camera/model state.

### 2) Desktop adapter baseline
- `DesktopPlatformServices` + `DesktopInMemoryCredentialStore` implement the platform service contracts for desktop builds.
- `DesktopOpenGLSceneRenderer` bridges portable scene-state calls into existing desktop render callbacks.
- `DesktopSceneStateAdapter` converts `Camera` + `GLVolumeCollection` into portability `SceneState` and is wired into `GLCanvas3D` rendering flow.

### 3) iOS scaffold modules (preserve names)
- `IOSPlatformServices` and `IOSKeychainCredentialStore` are implemented and built as `orcaslicer_platform_ios`.
- `IOSMetalRenderBackend` is implemented and built as `orcaslicer_render_ios_metal`.
- iOS targets are enabled only when iOS toolchain detection is active (`ORCASLICER_IOS_TOOLCHAIN_ACTIVE`).

### 4) Smoke integration
- `orcaslicer_ios_smoke` links `libslic3r` + iOS portability targets and performs minimal symbol-level/runtime bring-up checks.
- This is a link/bring-up smoke check, not an end-to-end app workflow.

## What remains stubbed or partial

### Renderer path
- `IOSMetalRenderBackend::render_frame()` currently performs a clear pass and viewport setup only.
- No mesh upload, draw command generation, material pipeline, depth handling, selection/gizmo passes, or parity with desktop object rendering is implemented in the iOS backend yet.
- `submit_scene_state()` currently stores state but does not drive real scene rendering.

### Platform/runtime behavior
- `IOSPlatformServices` currently provides basic path lookup + async dispatch, but app lifecycle semantics (foreground/background transitions, cancellation rules, ownership boundaries across threads) are not fully formalized/tested.
- `IOSKeychainCredentialStore` handles read/write/remove basics, but migration/error-policy hardening for production UX remains to be codified.
- `DesktopPlatformServices::post_to_main_thread()` executes inline (synchronous shortcut), which is acceptable as a desktop stub but not representative of mobile main-thread scheduling semantics.

### Build + validation coverage
- iOS smoke target validates linkage and basic object construction only.
- There is no full CI-proven end-to-end mobile flow yet (`load model -> slice -> preview -> export`) through portability adapters.

## UI start gate (must be true before iOS UI work begins)
Start iOS UI implementation only after all of the following are complete:

1. **Shared engine portability closure**
   - Core slicing/project/export paths required by mobile flows run without desktop UI/event-loop coupling.
   - Deterministic output parity is verified against desktop baselines for representative workloads.

2. **Portability service closure**
   - Remaining mobile-relevant orchestration still in desktop-coupled modules is moved behind portability-facing contracts.
   - Async/progress/error semantics exposed to UI are stable and platform-neutral.

3. **iOS adapter closure**
   - `orcaslicer_render_ios_metal` renders real scene content from portable scene state (not clear-only frames).
   - `orcaslicer_platform_ios` lifecycle and credential-store edge cases are validated for production behavior.

4. **Validation closure**
   - `orcaslicer_platform_ios`, `orcaslicer_render_ios_metal`, and `orcaslicer_ios_smoke` remain green in repeatable iOS toolchain builds.
   - Portability tests/smoke checks cover adapter bring-up + critical failure paths.

## First native iOS shell milestone (post-gate only)
After portability and app-service gates are complete, scope the first native iOS shell milestone to:

1. **App launch only**
   - Bring up a minimal native iOS host (SwiftUI/UIKit shell) that initializes shared services without adding product workflows yet.

2. **Metal-backed view only**
   - Attach `IOSMetalRenderBackend` to a `CAMetalLayer`-backed view and prove frame presentation.
   - Keep rendering scope at bring-up level (no additional UI feature work, slicing UX flows, or Android parity tasks in this milestone).

3. **Reuse existing adapters**
   - Use `IOSPlatformServices` for platform service wiring and `IOSMetalRenderBackend` for render wiring as-is.
   - Any expansion beyond launch + Metal view stays deferred until this minimal shell milestone is validated.

## Deferred until after iOS milestone
- Android platform adapters and renderer backend implementation.
- Android build/packaging automation and mobile UI integration work.

## Canonical iOS smoke invocation

```bash
cmake -S . -B build-ios \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DORCASLICER_BUILD_IOS_PORTABILITY=ON

cmake --build build-ios --target orcaslicer_ios_smoke --config Release
```

Configure-time output reports whether iOS toolchain detection is active and whether iOS portability targets are enabled.

## iOS simulator smoke CI baseline

The `.github/workflows/ios_simulator_smoke.yml` pipeline intentionally uses the same simulator smoke target and configure flags as the local smoke path (`orcaslicer_ios_smoke`, iOS simulator sysroot, `arm64`, deployment target `15.0`, `ORCASLICER_BUILD_IOS_PORTABILITY/SMOKE_TARGET=ON`, and `ORCASLICER_BUILD_IOS_UI_SHELL=OFF`).

Post-success cleanup applied:
- Pin CMake setup action to `lukka/get-cmake@v4.2.3` (instead of floating `@latest`) to keep the workflow stable during GitHub Actions runtime transitions.
- Drop the no-op `--config Release` argument from the Ninja build command; single-config Ninja still uses the existing `-DCMAKE_BUILD_TYPE=Release` configure behavior.


## iOS simulator UI screenshot CI

The repository now includes `.github/workflows/ios_ui_screenshot.yml`, a dedicated follow-on workflow to `ios_simulator_smoke`. It configures and builds the native `OrcaSlicerIOS` app target for the iOS simulator with the Xcode generator (no code signing), uses iOS deployment target `16.0` to match the current SwiftUI shell APIs, boots a simulator device, installs the app once, then relaunches it in deterministic screenshot mode for each current shell scene/menu (`root`, `project`, `tools`, `slice-settings`, `printer`, `view`, and `app-settings`). Each pass captures a portrait screenshot to `artifacts/ios-screenshots/<scene>.png`, records outputs in `artifacts/ios-screenshots/manifest.txt`, and uploads the full directory as a single `ios-ui-screenshot` artifact.

Validation scope: this confirms that the native SwiftUI shell app can be built, installed, launched, and rendered on a simulator in CI across all current scene/menu entry points. It does **not** validate end-to-end slicing workflows, deep UI interaction automation, or renderer feature parity beyond launch-time viewport shell bring-up.
