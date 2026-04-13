# Linux EGL Build and Runtime Guide

## Overview

OrcaSlicer's Linux build uses EGL instead of GLX for OpenGL context creation. This enables
native Wayland support alongside X11 with a single binary -- the application detects the
display server at runtime and creates the appropriate OpenGL context automatically.

EGL is the platform-agnostic API for managing rendering contexts and surfaces. By using EGL,
OrcaSlicer can render its 3D viewport on both X11 (via EGL-on-X11) and Wayland without
requiring XWayland.

## Build Flags

Two CMake flags enable EGL in the dependency build. Both must be set together; a compile-time
guard in `src/slic3r/GUI/OpenGLManager.cpp` enforces consistency.

### wxWidgets: `wxUSE_GLCANVAS_EGL=ON`

Location: `deps/wxWidgets/wxWidgets.cmake`

Tells wxWidgets to build its `wxGLCanvas` widget using EGL instead of GLX on Linux. This is
the primary flag that switches the GL context backend.

```cmake
-DwxUSE_GLCANVAS_EGL=ON
```

### GLEW: `GLEW_USE_EGL=ON`

Location: `deps/GLEW/GLEW.cmake`

Builds GLEW with EGL support so that OpenGL extension loading works with EGL contexts.

```cmake
-DGLEW_USE_EGL=ON
```

**Important:** If one flag is enabled without the other, the build will produce a binary that
cannot initialize OpenGL correctly. Always set both flags to the same value.

## Build Dependencies

The following system packages are required for an EGL-enabled build, in addition to the
standard OrcaSlicer build dependencies.

### Debian / Ubuntu

```bash
sudo apt install libegl1-mesa-dev libwayland-dev
```

### Fedora

```bash
sudo dnf install mesa-libEGL-devel wayland-devel
```

### Arch Linux

```bash
sudo pacman -S mesa wayland
```

(Arch's `mesa` package includes EGL development files.)

**Note:** The existing `build_linux.sh -u` script installs system dependencies but may not
include the EGL/Wayland development packages listed above. Verify these are installed before
building.

## Runtime Requirements

### Mesa / AMD / Intel GPUs

No additional runtime packages are needed. EGL support is built into the Mesa driver stack.

### NVIDIA GPUs (Wayland)

NVIDIA users running Wayland need the `egl-wayland` library, which provides the
`EGL_WL_bind_wayland_display` extension required for EGL to create surfaces on Wayland
compositors.

| Distribution    | Package                      | Install Command                          |
|-----------------|------------------------------|------------------------------------------|
| Arch Linux      | `egl-wayland`                | `sudo pacman -S egl-wayland`             |
| Fedora          | `egl-wayland`                | `sudo dnf install egl-wayland`           |
| Debian / Ubuntu | `libnvidia-egl-wayland1`     | `sudo apt install libnvidia-egl-wayland1`|

**Minimum NVIDIA driver version:** 535 or later is recommended. The `nvidia-open` kernel
module is compatible with EGL and has been tested with OrcaSlicer. Earlier driver versions
may work but are untested.

NVIDIA users on X11 do not need the `egl-wayland` package -- the NVIDIA driver provides
EGL-on-X11 support directly.

## Verifying EGL

### Check EGL availability

```bash
eglinfo | head -20
```

This should list an EGL platform, vendor, and version. If the command is not found, install
`mesa-utils` (Debian/Ubuntu/Arch) or `mesa-demos` (Fedora).

### NVIDIA PRIME offload verification

If using NVIDIA PRIME render offload (e.g., laptop with integrated + discrete GPU):

```bash
__NV_PRIME_RENDER_OFFLOAD=1 eglinfo | head -20
```

This should show the NVIDIA EGL platform rather than the integrated GPU's.

### Verify OrcaSlicer is using EGL

Launch OrcaSlicer and check stderr output. On a Wayland session the log should show EGL
context creation messages from wxWidgets rather than GLX. No environment variable overrides
(such as `MESA_LOADER_DRIVER_OVERRIDE` or `GALLIUM_DRIVER`) should be needed.
