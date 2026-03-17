# File Edit Plan for iOS-first / Android-ready Port

This is a **planning list** of files likely needing edits to modularize platform and renderer concerns.

## 1) Build system and target split

### `src/CMakeLists.txt`
- Add new modular targets for core/app/platform/render layers.
- Separate GUI/non-GUI executable composition.
- Prepare iOS toolchain-friendly library outputs (static libs/framework-ready).

### `src/slic3r/CMakeLists.txt`
- Split current monolithic `libslic3r_gui` sources into:
  - shared app-service sources (UI-agnostic)
  - desktop wx/OpenGL implementation sources
- Move platform-specific source conditionals into dedicated adapter targets.

### `src/libvgcode/CMakeLists.txt`
- Keep current GL/GLES flexibility; add hooks for renderer API integration.
- Optionally expose as renderer-plugin dependency instead of direct GUI dependency.

## 2) Entrypoint and bootstrap boundaries

### `src/OrcaSlicer.cpp`
- Split into distinct launch paths:
  - headless/CLI runner
  - desktop GUI runner
- Remove direct GUI/OpenGL include dependency from non-GUI path.

### New files (recommended)
- `src/app/AppBootstrap.hpp/.cpp` (shared startup orchestration)
- `src/app/cli/CliMain.cpp`
- `src/app/desktop/DesktopMain.cpp`

## 3) Renderer abstraction extraction

### `src/slic3r/GUI/GLCanvas3D.*`
- Extract interface-level scene commands and input events.
- Move OpenGL-specific operations behind desktop renderer implementation.

### `src/slic3r/GUI/OpenGLManager.*`
- Re-home as desktop backend class implementing a renderer backend contract.

### `src/slic3r/GUI/{GLModel,GLTexture,GLShader,GLShadersManager}.*`
- Keep as desktop OpenGL backend internals.
- Ensure higher layers no longer include these directly.

### `src/slic3r/GUI/3DScene.*` and `src/slic3r/GUI/Camera.*`
- Split scene/camera data/state from immediate renderer calls.
- Put renderer-independent math/state in shared layer.

### `src/libvgcode/include/Viewer.hpp` + `src/libvgcode/src/Viewer*.cpp`
- Adapt API surface so viewer operations can be called from platform-neutral rendering facade.

## 4) Platform service abstraction

### `src/slic3r/Utils/Process.*`
- Replace direct wx process/path usage with platform service interface.

### `src/slic3r/Utils/WxFontUtils.*`
- Isolate font discovery/conversion behind platform font service.

### `src/slic3r/Utils/OrcaCloudServiceAgent.*`
- Remove direct GUI app access; inject auth/secret storage interfaces.

### `src/slic3r/Utils/FileTransferUtils.*`
- Decouple from wx event/UI assumptions; move to app service callbacks.

### `src/slic3r/Utils/PresetUpdater.*`
- Replace wx event dependence with portable observer/callback model.

## 5) macOS-specific files to confine to desktop adapter

### `src/slic3r/Utils/MacDarkMode.mm`
### `src/slic3r/Utils/RetinaHelperImpl.mm`
### `src/slic3r/GUI/GUI_UtilsMac.mm`
### `src/slic3r/GUI/Mouse3DHandlerMac.mm`
### `src/slic3r/GUI/InstanceCheckMac.mm`
### `src/slic3r/GUI/RemovableDriveManagerMM.mm`
### `src/slic3r/GUI/wxMediaCtrl2.mm`
- Keep platform-specific implementation private to desktop/mac target.
- Ensure no shared/mobile target references these directly.

## 6) New abstraction headers to add

- `src/platform/IPlatformPaths.hpp`
- `src/platform/IKeyValueSecureStore.hpp`
- `src/platform/ITaskDispatcher.hpp`
- `src/platform/INetworkReachability.hpp`
- `src/render/IRenderBackend.hpp`
- `src/render/ISceneRenderer.hpp`
- `src/render/IPickingService.hpp`

(Exact names can vary; key requirement is strict interface/implementation split.)

## 7) Initial implementation sequence
1. Introduce interfaces + desktop adapter implementations first.
2. Move compile dependencies so shared targets have no `<wx/...>`/OpenGL includes.
3. Add iOS build target consuming shared libs only.
4. Add Android target after iOS scaffolding, reusing the same interfaces.
