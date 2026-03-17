# Mobile Porting Implementation Status

## Status snapshot
The repository now has a portability scaffold under `src/portability/` and an initial iOS module entry-point. Current work is intentionally iOS-first with interfaces shared for Android follow-up.

## Current state vs target state
| Area | Current state | Target state | Migration status |
|---|---|---|---|
| Platform service contract | `src/portability/platform/IPlatformServices.hpp` and `ICredentialStore.hpp` | Keep contracts in `src/portability/platform/` | In progress |
| Desktop platform adapter | `src/portability/platform/DesktopPlatformServices.*` + `DesktopInMemoryCredentialStore.*` | Keep as desktop adapter implementation | In progress |
| iOS platform adapter | `src/portability/platform/ios/IOSPlatformServices.*` | Replace placeholder implementations with ObjC++ bridge to Apple APIs | Scaffolded |
| Renderer contract | `src/portability/render/IRenderBackend.hpp` | Keep backend-neutral API in `src/portability/render/` | In progress |
| iOS renderer adapter | `src/portability/render/ios/IOSMetalRenderBackend.*` | Wire to Metal render pipeline and scene commands | Scaffolded |
| Build integration | `src/CMakeLists.txt` + `src/portability/CMakeLists.txt` | iOS targets (`orcaslicer_platform_ios`, `orcaslicer_render_ios_metal`) built only for iOS toolchains | Scaffolded |

## What landed in this phase
- Normalized portability interfaces and namespaces under `Slic3r::portability::*` with renderer APIs and scene integration standardized on `Slic3r::portability::render`.
- Added iOS platform services module implementing `IPlatformServices` with safe placeholders.
- Added iOS Metal renderer module implementing `IRenderBackend` with no-op render lifecycle.
- Added iOS-specific CMake subtargets gated by iOS toolchain detection.

## Temporary/legacy placement note
Some desktop integration still exists in `src/slic3r/Utils` for launch/process and wx-bound application wiring. This is temporary and should be migrated behind `src/portability/**` contracts as we move additional services out of GUI-coupled modules.

## Next iOS-focused steps
1. Replace iOS placeholder filesystem/thread stubs with Objective-C++ bridges (`NSFileManager`, `NSTemporaryDirectory`, GCD main/background queues).
2. Introduce an iOS secure credential store implementation (Keychain-backed) behind `ICredentialStore`.
3. Bridge scene upload/viewport lifecycle from current desktop renderer integration to `IOSMetalRenderBackend`.
4. Add an iOS smoke-test target that links core + portability iOS modules without wx/OpenGL.

## Deferred until after iOS milestone
- Android adapter implementation (JNI + Vulkan/GLES backend).
- Android build composition and packaging automation.

## Namespace contract
- Renderer-facing portability interfaces use `Slic3r::portability::render` as the canonical namespace (`IRenderBackend`, `ISceneRenderer`, `DesktopOpenGLSceneRenderer`, and GUI integration call sites).
- Compatibility aliases were not retained; callers should migrate directly to the canonical namespace.
