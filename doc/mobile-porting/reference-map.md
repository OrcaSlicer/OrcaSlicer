# OrcaSlicer Reference Map (for iOS/Android portability work)

## Top-level orientation
- `src/libslic3r/` — slicing engine, model processing, gcode generation (core candidate).
- `src/slic3r/` — GUI, app orchestration, platform utilities (currently mixed concerns).
- `src/libvgcode/` — G-code visualization library (already has GL/GLES split support).
- `src/portability/` — portability contracts + platform/render adapters (desktop and iOS scaffold).
- `src/dev-utils/platform/` — platform packaging/runtime templates.
- `resources/` — presets/icons/assets.
- `tests/` — unit/integration test coverage.

## Build graph entry points
- `CMakeLists.txt` (root): global options (`SLIC3R_GUI`) and dependency setup.
- `src/CMakeLists.txt`: app target composition (`libslic3r`, `libslic3r_gui`, `OrcaSlicer`).
- `src/libslic3r/CMakeLists.txt`: core engine sources.
- `src/slic3r/CMakeLists.txt`: GUI + many utility/application files + platform-specific source selection.
- `src/libvgcode/CMakeLists.txt`: visualization lib and GL/GLES selection.

## Core engine map (`src/libslic3r`)
- Geometry and mesh primitives: `Geometry/`, `AABB*`, `TriangleMesh*`, `Model*`.
- Slicing algorithms: `Fill/`, `Support/`, `Arachne/`, `PerimeterGenerator*`.
- G-code pipeline: `GCode/`, `GCodeWriter*`, `GCodeProcessor*`, `Print*`.
- File formats: `Format/{3mf,STL,OBJ,AMF,STEP,...}`.
- SLA pipeline: `SLA/`, `SLAPrint*`.

## GUI + rendering map (`src/slic3r/GUI`)
- Application shell: `GUI_App.*`, `MainFrame.*`, `GUI_Init.*`, `Plater.*`.
- 3D rendering core: `GLCanvas3D.*`, `OpenGLManager.*`, `GLModel.*`, `GLTexture.*`, `GLShader.*`, `GLShadersManager.*`.
- Scene/control: `3DScene.*`, `Camera.*`, `Selection.*`, `Raycaster.*`.
- Tooling overlays/gizmos: `Gizmos/*`.
- Web-related UI: `WebViewDialog.*`, `PrinterWebView.*`, `Widgets/WebView.*`.
- Platform-specific GUI files:
  - macOS: `GUI_UtilsMac.mm`, `Mouse3DHandlerMac.mm`, `InstanceCheckMac.mm`, `RemovableDriveManagerMM.mm`, `wxMediaCtrl2.mm`
  - non-Apple fallback: `wxMediaCtrl2.cpp`

## Utility and service map (`src/slic3r/Utils`)
Portable-leaning utilities are mixed with wx/UI-coupled services.

Search clusters:
- Print host integrations: `OctoPrint*`, `Duet*`, `MKS*`, `FlashAir*`, `Repetier*`, `Obico*`, `SimplyPrint*`.
- User/device/network helpers: `NetworkAgent*`, `Bonjour*`, `PresetUpdater*`, `Http*`.
- wx-dependent utility examples:
  - `Process.*`
  - `WxFontUtils.*`
  - `OrcaCloudServiceAgent.*`
  - `FileTransferUtils.*`

## App entrypoint and coupling hot spots
- `src/OrcaSlicer.cpp`
  - mixes CLI behavior with GUI/OpenGL includes.
  - currently hard boundary candidate for splitting into headless + GUI launchers.

## Fast search strings for future edits
- `SLIC3R_GUI`
- `target_link_libraries(libslic3r_gui`
- `orcaslicer_platform_ios`
- `orcaslicer_render_ios_metal`
- `#include <wx/`
- `#include "slic3r/GUI/`
- `OpenGLManager`
- `GLCanvas3D`
- `ImGuiWrapper`
- `wxMediaCtrl2`
- `RetinaHelperImpl`
- `MacDarkMode`

## Portability boundaries to enforce
1. `libslic3r` and future `orcaslicer_app` must compile without wx/OpenGL.
2. Rendering calls must flow through backend-neutral interfaces.
3. Platform services (paths, keychain, task dispatch, share/open handlers) must be abstracted behind adapters.
4. Native UI state should consume shared C++ view-model/state outputs, not raw engine internals.

5. iOS-first scope: add iOS adapters in `src/portability/platform/ios` and `src/portability/render/ios` before Android work starts.
