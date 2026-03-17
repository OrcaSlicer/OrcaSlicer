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
## Process launching service (`src/slic3r/Utils/Process.*`)

- Replaced direct `wxStandardPaths::Get().GetExecutablePath()` access in process launch helpers with `IPlatformServices::executable_path()`.
- Added a desktop implementation in `src/slic3r/Utils/PlatformServices.{hpp,cpp}`:
  - `class IPlatformServices`
  - `class DesktopPlatformServices final : public IPlatformServices`
- Updated launch helper signatures to inject `IPlatformServices&`:
  - `void start_new_slicer(IPlatformServices& platform_services, const wxString *path_to_open = nullptr, bool single_instance = false);`
  - `void start_new_slicer(IPlatformServices& platform_services, const std::vector<wxString>& files, bool single_instance = false);`
  - `void start_new_gcodeviewer(IPlatformServices& platform_services, const wxString *path_to_open = nullptr);`
  - `void start_new_gcodeviewer_open_file(IPlatformServices& platform_services, wxWindow *parent = nullptr);`

## Composition root wiring

- `GUI_App` now owns a desktop implementation:
  - `DesktopPlatformServices m_platform_services;`
- `GUI_App` exposes it through:
  - `IPlatformServices& platform_services();`
  - `const IPlatformServices& platform_services() const;`
- Existing desktop call sites in `GUI_App.cpp` and `MainFrame.cpp` now pass `wxGetApp().platform_services()` (or `platform_services()` inside `GUI_App`) so desktop behavior remains unchanged.
