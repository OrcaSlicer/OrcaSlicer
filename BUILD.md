# Building OrcaSlicer with ZAA on macOS

A step-by-step guide to building OrcaSlicer from source on macOS. No prior experience with building software required.

## What You'll Need

- A Mac running macOS 11.3 (Big Sur) or later
- About 15 GB of free disk space
- An internet connection
- About 1-2 hours for the first build (mostly waiting)

## Step 1: Open Terminal

Press **Cmd + Space**, type **Terminal**, and press Enter. A window with a command prompt will appear. All commands below are typed into this window.

## Step 2: Install Xcode Command Line Tools

This installs the compiler and basic development tools.

```bash
xcode-select --install
```

A dialog will pop up. Click **Install** and wait for it to finish (a few minutes).

To verify it worked:

```bash
xcode-select -p
```

You should see something like `/Library/Developer/CommandLineTools` or `/Applications/Xcode.app/Contents/Developer`.

## Step 3: Install Homebrew

[Homebrew](https://brew.sh) is a package manager that makes it easy to install developer tools on macOS.

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Follow the prompts. When it finishes, it will tell you to run one or two commands to add Homebrew to your PATH. **Run those commands** — they look something like:

```bash
echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
eval "$(/opt/homebrew/bin/brew shellenv)"
```

To verify:

```bash
brew --version
```

## Step 4: Install CMake

CMake is the build system OrcaSlicer uses.

```bash
brew install cmake
```

To verify:

```bash
cmake --version
```

You need version 3.13 or later (anything recent from Homebrew will work).

## Step 5: Install Git (if needed)

Git usually comes with the Xcode Command Line Tools. Check:

```bash
git --version
```

If it's not found:

```bash
brew install git
```

## Step 6: Clone the Repository

Choose where you want the source code. For example, in your home directory:

```bash
cd ~
git clone https://github.com/mnott/OrcaSlicer.git
cd OrcaSlicer
```

If you want the ZAA feature branch:

```bash
git checkout feature/zaa-contouring
```

## Step 7: Build

The `build.sh` script handles everything. For a first-time build:

```bash
chmod +x build.sh
./build.sh --full
```

This does three things:
1. **Builds dependencies** (Boost, wxWidgets, OpenSSL, etc.) — takes 30-60 minutes the first time
2. **Configures CMake** — sets up the build system
3. **Builds OrcaSlicer** — compiles the application

Go get a coffee. The dependency step only needs to run once.

### After the first build

For subsequent builds (after making code changes), just run:

```bash
./build.sh
```

This does an incremental build — only recompiles what changed. Usually takes 1-5 minutes.

### Other build options

```bash
./build.sh --help       # Show all options
./build.sh --clean      # Clean rebuild (if something is broken)
./build.sh --deps       # Rebuild dependencies only
./build.sh --configure  # Reconfigure CMake only
```

## Step 8: Run OrcaSlicer

After a successful build, the script prints the app location. You can run it with:

```bash
open build/arm64/src/RelWithDebInfo/OrcaSlicer.app
```

Or find `OrcaSlicer.app` in Finder and double-click it.

## Troubleshooting

### "CMake not found"

Make sure Homebrew is in your PATH (see Step 3), then `brew install cmake`.

### "No rule to make target" or CMake generator errors

Your build directory may have been configured with a different generator. Clean and reconfigure:

```bash
./build.sh --clean --full
```

### Build fails during dependencies

Some deps need a lot of memory. Close other applications and try again. If a specific dependency fails, check if you have the latest Xcode Command Line Tools:

```bash
softwareupdate --list
```

### wxWidgets "hardcoded path" errors

wxWidgets bakes the installation path into its config files. If you move the source directory after building deps, you need to rebuild them:

```bash
./build.sh --deps
./build.sh --clean
```

### "Permission denied" on build.sh

```bash
chmod +x build.sh
```

### Apple Silicon (M1/M2/M3/M4) vs Intel

The build script auto-detects your architecture. If you need to specify it explicitly:

```bash
./build.sh --full --arch arm64    # Apple Silicon
./build.sh --full --arch x86_64   # Intel
```

## Project Structure

```
OrcaSlicer/
├── src/
│   ├── libslic3r/        # Core slicing engine
│   │   ├── ContourZ.cpp  # ZAA raycasting algorithm
│   │   └── ...
│   └── slic3r/GUI/       # User interface
├── deps/                  # External dependencies
├── build/                 # Build output (created by build.sh)
│   └── arm64/
│       └── src/RelWithDebInfo/
│           └── OrcaSlicer.app
├── docs/
│   └── ZAA.md            # ZAA feature documentation
├── build.sh              # Build script (this guide uses it)
├── BUILD.md              # This file
└── build_release_macos.sh  # Official release build script
```

## ZAA (Z Anti-Aliasing)

See [docs/ZAA.md](docs/ZAA.md) for details on the Z Anti-Aliasing feature. In short: enable **Z contouring** in Print Settings > Quality to get smoother curved surfaces.
