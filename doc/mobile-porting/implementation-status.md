# Mobile Porting Implementation Status

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
