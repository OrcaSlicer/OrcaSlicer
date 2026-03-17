# Mobile Porting Implementation Status

## Branch bootstrap delivered
This repository now includes a first portability scaffold in `src/portability/` to start separating platform and renderer concerns from slicer core logic.

## Added code scaffolding
- `src/portability/platform/IPlatformServices.hpp`
  - Defines a cross-platform contract for storage paths, task dispatch, and secure key/value access.
- `src/portability/platform/DesktopPlatformServices.hpp/.cpp`
  - Desktop placeholder implementation that can be swapped later by iOS and Android adapters.
- `src/portability/render/IRenderBackend.hpp`
  - Defines the backend-neutral rendering lifecycle contract.
- `src/portability/render/NullRenderBackend.hpp`
  - No-op backend stub for integration points and testing.
- `src/CMakeLists.txt`
  - Adds `orcaslicer_portability` static library target that builds the new scaffold.

## Next code steps
1. Migrate selected `src/slic3r/Utils` services to depend on `IPlatformServices` instead of wx APIs.
2. Introduce desktop OpenGL adapter implementing `IRenderBackend` and bridge from current `GLCanvas3D`.
3. Add iOS adapter target implementing `IPlatformServices` and Metal `IRenderBackend`.
4. Add Android adapter target implementing `IPlatformServices` and Vulkan/GLES `IRenderBackend`.
