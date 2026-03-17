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

## Deferred until after iOS milestone
- Android adapter implementation (JNI + Vulkan/GLES backend).
- Android build composition and packaging automation.

## iOS smoke target invocation
When configuring with an iOS toolchain (for example `-DCMAKE_SYSTEM_NAME=iOS` or an iPhone OS/simulator sysroot), CMake now adds `orcaslicer_ios_smoke`, a minimal smoke target in `src/portability/` that links:

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
