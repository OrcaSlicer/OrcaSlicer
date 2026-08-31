# ZR Spectrum

A **Windows** fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) for the **WonderMaker ZR Ultra S** (a 4-head toolchanger).

**[Download the Windows app](https://github.com/Ryan-AI-Studios/OrcaSlicer-ZR/releases/latest)** — unzip the folder and run `orca-slicer.exe`.

This is **not** official OrcaSlicer. Do not use the OrcaSlicer.com / SoftFever installers if you want the mix features below.

## What this fork adds (vs official OrcaSlicer)

Same familiar slicer (based on Orca **2.4.2**), plus **mixed-color printing** on the Ultra S:

- **ZR Ultra S ready** — printer profiles for 0.4, 0.6, and 0.8 mm nozzles, four tools
- **Mix filaments in one part** — blend 2 or 3 spools (for example cyan/magenta/yellow/black) so you can print more colors than you have heads
- **Paint extra colors** — paint Mix 5+ on the model; the slicer turns those regions into mixes
- **Match a color** — pick a target color and get a printable mix from the filaments you loaded
- **Fades and smoother walls** — height gradients on a mix, optional dither on outer walls
- **Open a normal 4-color project** — File → Open puts it on Ultra S, keeps your slot colors, and keeps the filament *type* (PETG stays PETG)
- **Repair and cut keep paint** — Fix Model / cut / simplify no longer wipe color painting by default

Windows only for now. This is a **toolchanger mix** workflow, not Bambu AMS.

## Run it (Windows)

1. Open the **[latest Release](https://github.com/Ryan-AI-Studios/OrcaSlicer-ZR/releases/latest)**
2. Download the `.zip` and unzip it
3. Open the `OrcaSlicer` folder and double-click **`orca-slicer.exe`**
4. Choose printer **WonderMaker ZR Ultra S**

If Windows blocks it, or the window will not open, install [WebView2](https://go.microsoft.com/fwlink/p/?LinkId=2124703) and the [Visual C++ redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

## Source code (optional)

The app you want is on branch **`base/v2.4.2`**.

- Do **not** clone **`main`** — that is fork setup, not the slicer you run
- Do **not** check out the git tag **`v2.4.2`** — that is the frozen original Orca pin, without ZR Spectrum

```bat
git clone --branch base/v2.4.2 --single-branch https://github.com/Ryan-AI-Studios/OrcaSlicer-ZR.git
```

To build: Visual Studio with **Desktop development with C++**, **CMake 4.x** on PATH *before* Strawberry Perl, then from an **x64 Native Tools** prompt:

```bat
build_release_vs.bat
```

First build is slow (deps + app). After that, `build_release_vs.bat slicer` is enough. General compile notes: [OrcaSlicer wiki — how to build](https://www.orcaslicer.com/wiki/how_to_build).

## Upstream OrcaSlicer

ZR Spectrum includes OrcaSlicer. Wiki, calibration guides, and the wider Orca community still live at [orcaslicer.com](https://www.orcaslicer.com/) and [OrcaSlicer/OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). Those sites ship **plain Orca**, not this fork.

## License

- **OrcaSlicer** / this fork: GNU Affero General Public License, version 3. If you use any part of this software (including behind a web server), your software must be released under the same license.
- Pressure-advance calibration pattern: adapted from Andrew Ellis' generator (GPL-3), itself adapted from Sineos for Marlin (GPL-3).
- The **Bambu networking plugin** uses non-free Bambu Lab libraries. It is optional.
- Vendors [prusa-fdm-mixer](https://github.com/prusa3d/prusa-fdm-mixer) (MIT). Not official Prusa ColorMix / PrusaSlicer.
