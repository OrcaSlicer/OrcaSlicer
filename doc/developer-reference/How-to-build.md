# How to Build

This wiki page provides detailed instructions for building OrcaSlicer from source on different operating systems, including Windows, macOS, and Linux.  
It includes tool requirements, setup commands, and build steps for each platform.

Whether you're a contributor or just want a custom build, this guide will help you compile OrcaSlicer successfully.

- [Windows 64-bit](#windows-64-bit)
  - [Windows Tools Required](#windows-tools-required)
  - [Windows Hardware Requirements](#windows-hardware-requirements)
  - [Windows Instructions](#windows-instructions)
- [MacOS 64-bit](#macos-64-bit)
  - [MacOS Tools Required](#macos-tools-required)
  - [MacOS Instructions](#macos-instructions)
  - [Debugging in Xcode](#debugging-in-xcode)
- [Linux](#linux)
  - [Using Docker](#using-docker)
    - [Docker Dependencies](#docker-dependencies)
    - [Docker Instructions](#docker-instructions)
  - [Troubleshooting](#troubleshooting)
  - [Linux Build](#linux-build)
    - [Dependencies](#dependencies)
      - [Common dependencies across distributions](#common-dependencies-across-distributions)
      - [Additional dependencies for specific distributions](#additional-dependencies-for-specific-distributions)
    - [Linux Instructions](#linux-instructions)
    - [Unit Testing](#unit-testing)
- [Portable User Configuration](#portable-user-configuration)
  - [Example folder structure](#example-folder-structure)

## Windows 64-bit

How to building with Visual Studio on Windows 64-bit.

### Windows Tools Required

- [Visual Studio](https://visualstudio.microsoft.com/vs/) 2026, 2022 or Visual Studio 2019
  ```shell
  winget install --id=Microsoft.VisualStudio.Community -e
  ```
- [CMake](https://cmake.org/)
  ```shell
  winget install --id=Kitware.CMake -e
  ```
- [Strawberry Perl](https://strawberryperl.com/)
  ```shell
  winget install --id=StrawberryPerl.StrawberryPerl -e
  ```
- [Git](https://git-scm.com/)
  ```shell
  winget install --id=Git.Git -e
  ```
- [git-lfs](https://git-lfs.com/)
  ```shell
  winget install --id=GitHub.GitLFS -e
  ```

> [!TIP]
> GitHub Desktop (optional): A GUI for Git and Git LFS, which already includes both tools.
> ```shell
> winget install --id=GitHub.GitHubDesktop -e
> ```

> [!IMPORTANT]
> Check your CMake version. Run `cmake --version` in your terminal and verify it returns a **4.x** version.  
> If you see an older version (e.g. 3.29), it's likely due to another copy in your system's PATH (e.g. from Strawberry Perl).  
> You can run where cmake to check the active paths and rearrange your **System Environment Variables** > PATH, ensuring the correct CMake like `C:\Program Files\CMake\bin` appears before others like `C:\Strawberry\c\bin`.

![windows_variables_path](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/develop/windows_variables_path.png?raw=true)
![windows_variables_order](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/develop/windows_variables_order.png?raw=true)

### Windows Hardware Requirements

- Minimum 16 GB RAM
- Minimum 23 GB free disk space
- 64-bit CPU
- 64-bit Windows 10 or later

### Windows Instructions

1. Clone the repository:
   - If using GitHub Desktop clone the repository from the GUI.
   - If using the command line:
     1. Clone the repository:
     ```shell
     git clone https://github.com/OrcaSlicer/OrcaSlicer
     ```
     2. Run lfs to download tools on Windows:
     ```shell
     git lfs pull
     ```
2. Open the appropriate command prompt:
   ```MD
   x64 Native Tools Command Prompt for VS
   ```
   1. Navigate to correct drive (if needed), e.g.:
      ```shell
      N:
      ```
   2. Change directory to the cloned repository, e.g.:
      ```shell
      cd N:\Repos\OrcaSlicer
      ```
   3. Run the build script:
      ```shell
      build_release_vs.bat
      ```

![vs_cmd](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/develop/vs_cmd.png?raw=true)

> [!NOTE]
> The build process will take a long time depending on your system but even with high-end hardware it can take up to 40 minutes.

> [!TIP]
> If you encounter issues, you can try to uninstall ZLIB from your Vcpkg library.

1. If successful, you will find the Visual Studio solution file in:
   ```shell
   build\OrcaSlicer.sln
   ```
2. Open the solution in Visual Studio, set the build configuration to `Release` and run the `Local Windows Debugger`.  
   ![compile_vs_local_debugger](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/develop/compile_vs_local_debugger.png?raw=true)
3. Your resulting executable will be located in:
   ```shell
   \build\src\Release\orca-slicer.exe
   ```

> [!NOTE]
> The first time you build a branch, it will take a long time.  
> Changes to .cpp files are quickly compiled.  
> Changes to .hpp files take longer, depending on what you change.  
> If you switch back and forth between branches, it also takes a long time to rebuild, even if you haven't made any changes.

> [!TIP]
> If the build fails, try deleting the `build/` and `deps/build/` directories to clear any cached build data. Rebuilding after a clean-up is usually sufficient to resolve most issues.

## MacOS 64-bit

How to building with Xcode on MacOS 64-bit.

### MacOS Tools Required

- Xcode
- CMake (version 3.31.x is mandatory)
- Git
- gettext
- libtool
- automake
- autoconf
- texinfo

> [!TIP]
> You can install most of them by running:
> ```shell
> brew install gettext libtool automake autoconf texinfo
> ```

Homebrew currently only offers the latest version of CMake (e.g. **4.X**), which is not compatible. To install the required version **3.31.X**, follow these steps:

1. Download CMake **3.31.10** from: [https://cmake.org/download/](https://cmake.org/download/)
2. Install the application (drag it to `/Applications`).
3. Add the following line to your shell configuration file (`~/.zshrc` or `~/.bash_profile`):

```sh
export PATH="/Applications/CMake.app/Contents/bin:$PATH"
```

4. Restart the terminal and check the version:

```sh
cmake --version
```

5. Make sure it reports a **3.31.x** version.

> [!IMPORTANT]
> If you've recently upgraded Xcode, be sure to open Xcode at least once and install the required macOS build support.

### MacOS Instructions

1. Clone the repository:
   ```shell
   git clone https://github.com/OrcaSlicer/OrcaSlicer
   cd OrcaSlicer
   ```
2. Build the application:
   ```shell
   ./build_release_macos.sh
   ```
3. Open the application:
   ```shell
   open build/arm64/OrcaSlicer/OrcaSlicer.app
   ```

### Debugging in Xcode

To build and debug directly in Xcode:

1. Open the Xcode project:
   ```shell
   open build/arm64/OrcaSlicer.xcodeproj
   ```
2. In the menu bar:
   - **Product > Scheme > OrcaSlicer**
   - **Product > Scheme > Edit Scheme...**
     - Under **Run > Info**, set **Build Configuration** to `RelWithDebInfo`
     - Under **Run > Options**, uncheck **Allow debugging when browsing versions**
   - **Product > Run**

## Linux

Linux distributions are available in two formats: [using Docker](#using-docker) (recommended) or [building directly](#linux-build) on your system.

### Using Docker

How to build and run OrcaSlicer using Docker.

#### Docker Dependencies

- Docker
- Git

#### Docker Instructions

```shell
git clone https://github.com/OrcaSlicer/OrcaSlicer && cd OrcaSlicer && ./scripts/DockerBuild.sh && ./scripts/DockerRun.sh
```

### Troubleshooting

The `scripts/DockerRun.sh` script includes several commented-out options that can help resolve common issues. Here's a breakdown of what they do:

- `xhost +local:docker`: If you encounter an "Authorization required, but no authorization protocol specified" error, run this command in your terminal before executing `scripts/DockerRun.sh`. This grants Docker containers permission to interact with your X display server.
- `-h $HOSTNAME`: Forces the container's hostname to match your workstation's hostname. This can be useful in certain network configurations.
- `-v /tmp/.X11-unix:/tmp/.X11-unix`: Helps resolve problems with the X display by mounting the X11 Unix socket into the container.
- `--net=host`: Uses the host's network stack, which is beneficial for printer Wi-Fi connectivity and D-Bus communication.
- `--ipc host`: Addresses potential permission issues with X installations that prevent communication with shared memory sockets.
- `-u $USER`: Runs the container as your workstation's username, helping to maintain consistent file permissions.
- `-v $HOME:/home/$USER`: Mounts your home directory into the container, allowing you to easily load and save files.
- `-e DISPLAY=$DISPLAY`: Passes your X display number to the container, enabling the graphical interface.
- `--privileged=true`: Grants the container elevated privileges, which may be necessary for libGL and D-Bus functionalities.
- `-ti`: Attaches a TTY to the container, enabling command-line interaction with OrcaSlicer.
- `--rm`: Automatically removes the container once it exits, keeping your system clean.
- `orcaslicer $*`: Passes any additional parameters from the `scripts/DockerRun.sh` script directly to the OrcaSlicer executable within the container.

By uncommenting and using these options as needed, you can often resolve issues related to display authorization, networking, and file permissions.

### Linux Build

How to build OrcaSlicer on Linux.

#### Dependencies

The build system supports multiple Linux distributions including Ubuntu/Debian and Arch Linux. All required dependencies will be installed automatically by the provided shell script where possible, however you may need to manually install some dependencies.

> [!NOTE]
> Fedora and other distributions are not currently supported, but you can try building manually by installing the required dependencies listed below.

##### Common dependencies across distributions

- autoconf / automake
- cmake
- curl / libcurl4-openssl-dev
- dbus-devel / libdbus-1-dev
- eglexternalplatform-dev / eglexternalplatform-devel
- extra-cmake-modules
- file
- gettext
- git
- glew-devel / libglew-dev
- gstreamer-devel / libgstreamerd-3-dev
- gtk3-devel / libgtk-3-dev
- libmspack-dev / libmspack-devel
- libsecret-devel / libsecret-1-dev
- libspnav-dev / libspnav-devel
- libssl-dev / openssl-devel
- libtool
- libudev-dev
- mesa-libGLU-devel
- ninja-build
- texinfo
- webkit2gtk-devel / libwebkit2gtk-4.0-dev or libwebkit2gtk-4.1-dev
- wget

##### Additional dependencies for specific distributions

- **Ubuntu 22.x/23.x**: libfuse-dev, m4
- **Arch Linux**: mesa, wayland-protocols

#### Linux Instructions

1. **Install system dependencies:**
   ```shell
   ./build_linux.sh -u
   ```

2. **Build dependencies:**
   ```shell
   ./build_linux.sh -d
   ```

3. **Build OrcaSlicer with tests:**
   ```shell
   ./build_linux.sh -st
   ```

4. **Build AppImage (optional):**
   ```shell
   ./build_linux.sh -i
   ```

5. **All-in-one build (recommended):**
   ```shell
   ./build_linux.sh -dsti
   ```

**Additional build options:**

- `-b`: Build in debug mode (mostly broken at runtime for a long time; avoid unless you want to be fixing failed assertions)
- `-c`: Force a clean build
- `-C`: Enable ANSI-colored compile output (GNU/Clang only)
- `-e`: Build RelWithDebInfo (release + symbols)
- `-j N`: Limit builds to N cores (useful for low-memory systems)
- `-1`: Limit builds to one core
- `-l`: Use Clang instead of GCC
- `-p`: Disable precompiled headers (boost ccache hit rate)
- `-r`: Skip RAM and disk checks (for low-memory systems)

> [!NOTE]
> The build script automatically detects your Linux distribution and uses the appropriate package manager (apt, pacman) to install dependencies.

> [!TIP]
> For first-time builds, use `./build_linux.sh -u` to install dependencies, then `./build_linux.sh -dsti` to build everything.

> [!WARNING]
> If you encounter memory issues during compilation, use `-j 1` or `-1` to limit parallel compilation and `-r` to skip memory checks.

#### Unit Testing

See [How to Test](How-to-test) for more details.

---

## Portable User Configuration

If you want OrcaSlicer to use a custom user configuration folder (e.g., for a portable installation), you can simply place a folder named `data_dir` next to the OrcaSlicer executable. OrcaSlicer will automatically use this folder as its configuration directory.

This allows for multiple self-contained installations with separate user data.

> [!TIP]
> This feature is especially useful if you want to run OrcaSlicer from a USB stick or keep different profiles isolated.

### Example folder structure

```shell
OrcaSlicer.exe
data_dir/
```

You don’t need to recompile or modify any settings — this works out of the box as long as `data_dir` exists in the same folder as the executable.
