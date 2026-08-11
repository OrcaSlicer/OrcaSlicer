<div align="center">

<picture>
  <img alt="Extreme Slicer logo" src="resources/images/ExtremeSlicer.svg" width="15%" height="15%">
</picture>

**Extreme Slicer** — an OrcaSlicer/Slic3r-respecting fork with CUDA-first
exact compute, Vulkan fallback, and an auditable CPU reference path.

[![GitHub Repo stars](https://img.shields.io/github/stars/mk0000001/Extreme-Slicer)](https://github.com/mk0000001/Extreme-Slicer/stargazers)

Extreme Slicer is an independent modified version of OrcaSlicer and Slic3r,
focused on CUDA-first compute, Vulkan acceleration, auditable CPU fallback, and
QIDI-compatible printer workflows.

This repository contains modifications to upstream open-source projects. It is
distributed under the GNU Affero General Public License v3; see
[LICENSE.txt](LICENSE.txt) and [the upstream attribution note](docs/UPSTREAM_ATTRIBUTION.md).
This modified Extreme Slicer version is maintained independently from the
upstream project (modifications published August 12, 2026).

# Official links and community

#### Project repository:

<a href="https://github.com/mk0000001/Extreme-Slicer"><img src="https://img.shields.io/badge/Extreme%20Slicer-181717?style=flat&logo=github&logoColor=white" width="200" alt="GitHub Logo"/> </a>

#### Issues and discussions:

Use the [issue tracker](https://github.com/mk0000001/Extreme-Slicer/issues) for
bug reports and the [discussion area](https://github.com/mk0000001/Extreme-Slicer/discussions)
for design and performance topics.

> Extreme Slicer is distributed only from this repository's
> [Releases](https://github.com/mk0000001/Extreme-Slicer/releases) page. Be
> cautious of similarly named third-party downloads.

</div>

# Main features

- **[Advanced Calibration Tools](https://www.orcaslicer.com/wiki/calibration_guide)**  
  Comprehensive suite: temperature towers, flow rate, retraction & more for optimal performance.
- **[Precise Wall](https://www.orcaslicer.com/wiki/quality_settings_precision#precise-wall) and [Seam Control](https://www.orcaslicer.com/wiki/quality_settings_seam)**  
  Adjust outer wall spacing and apply scarf seams to enhance print accuracy.
- **[Sandwich Mode](https://www.orcaslicer.com/wiki/quality_settings_wall_and_surfaces#innerouterinner) and [Polyholes](https://www.orcaslicer.com/wiki/quality_settings_precision#polyholes) Support**  
  Use varied infill [patterns](https://www.orcaslicer.com/wiki/strength_settings_patterns) and accurate hole shapes for improved clarity.
- **[Overhang](https://www.orcaslicer.com/wiki/quality_settings_overhangs) and [Support Optimization](https://www.orcaslicer.com/wiki#support-settings)**  
  Modify geometry for printable overhangs with precise support placement.
- **[Granular Controls and Customization](https://www.orcaslicer.com/wiki#process-settings)**  
  Fine-tune print speed, layer height, pressure, and temperature with precision.
- **Network Printer Support**  
  Seamless integration with Klipper, PrusaLink, and OctoPrint for remote control.
- **[Mouse Ear Brims](https://www.orcaslicer.com/wiki/others_settings_brim) & [Adaptive Bed Mesh](https://www.orcaslicer.com/wiki/printer_basic_information_adaptive_bed_mesh)**  
  Automatic brims and adaptive mesh calibration ensure consistent adhesion.
- **User-Friendly Interface**  
  Intuitive drag-and-drop design with pre-made profiles for popular printers.
- **[Open-Source](https://github.com/mk0000001/Extreme-Slicer) & [Community Driven](https://github.com/mk0000001/Extreme-Slicer/discussions)**
  Regular updates fueled by continuous community contributions.
- **Wide Printer Compatibility**  
  Supports a broad range of printers: Bambu Lab, Prusa, Creality, Voron, and more.
- Additional Extreme Slicer changes can be found in the [release notes](https://github.com/mk0000001/Extreme-Slicer/releases/).

# Wiki and references

The upstream [OrcaSlicer wiki](https://www.orcaslicer.com/wiki) remains a useful
reference for inherited slicer settings. Extreme Slicer-specific compute
configuration is documented in
[docs/EXTREME_SLICER_COMPUTE.md](docs/EXTREME_SLICER_COMPUTE.md).

# Download

## Stable Release

📥 **[Download the Latest Extreme Slicer Release](https://github.com/mk0000001/Extreme-Slicer/releases/latest)**
Visit the Extreme Slicer GitHub Releases page for experimental and stable builds.

## Nightly Builds

🌙 **[Download development builds](https://github.com/mk0000001/Extreme-Slicer/releases)**
Development builds are experimental; please report reproducible issues in the issue tracker.

### Platform-specific builds

Only the packages listed on the Extreme Slicer
[releases page](https://github.com/mk0000001/Extreme-Slicer/releases) are
Extreme Slicer builds. Upstream OrcaSlicer platform and belt-printer packages
are not redistributed or represented as Extreme Slicer releases here.

# How to install

## Windows

Download the **Extreme Slicer Windows installer** from the [releases page](https://github.com/mk0000001/Extreme-Slicer/releases). Choose the package matching your CPU architecture.

- *For convenience there is also a portable build available.*
    <details>
    <summary>Troubleshooting</summary>

  - *If you have troubles to run the build, you might need to install following runtimes:*
  - [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
    - [Details of this runtime](https://aka.ms/webview2)
  - [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe)
    - This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`
    </details>

### Windows Package Manager

Extreme Slicer is not yet distributed through Windows Package Manager. Use the
installer from the [releases page](https://github.com/mk0000001/Extreme-Slicer/releases).

## Mac

Extreme Slicer macOS packages are not currently published. Build from source
using the upstream build system and the compute-backend notes in
[docs/EXTREME_SLICER_COMPUTE.md](docs/EXTREME_SLICER_COMPUTE.md).

## Linux

Extreme Slicer Linux packages are not currently published. Build from source
using the upstream build system and the compute-backend notes in
[docs/EXTREME_SLICER_COMPUTE.md](docs/EXTREME_SLICER_COMPUTE.md).

# How to Compile

Build notes for the Extreme Slicer compute backends are in
[docs/EXTREME_SLICER_COMPUTE.md](docs/EXTREME_SLICER_COMPUTE.md). The project
uses the upstream OrcaSlicer build system, with optional CUDA and Vulkan
components described in that document.

# Klipper Note

If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.

```gcode
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Support and attribution

Extreme Slicer is an independent community fork. It does not currently claim
the upstream OrcaSlicer sponsors, backers, or donation accounts shown in the
original README. Those links have intentionally been removed to avoid implying
an endorsement or a financial relationship with this project.

The project builds on OrcaSlicer, BambuStudio, PrusaSlicer, and Slic3r work.
Their names, licenses, and required notices remain acknowledged in this
repository. See [LICENSE.txt](LICENSE.txt) and the
[upstream attribution note](docs/UPSTREAM_ATTRIBUTION.md).

## Project background and attribution

Open-source slicing has always been built on a tradition of collaboration and attribution. [Slic3r](https://github.com/Slic3r/Slic3r), created by Alessandro Ranellucci and the RepRap community, laid the foundation. [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research built on Slic3r and acknowledged that heritage. [Bambu Studio](https://github.com/bambulab/BambuStudio) in turn forked from PrusaSlicer, and [SuperSlicer](https://github.com/supermerill/SuperSlicer) by @supermerill extended PrusaSlicer with community-driven enhancements. Each project carried the work of its predecessors forward, crediting those who came before.

Extreme Slicer is a separate community fork based on OrcaSlicer, which in turn
draws from BambuStudio, PrusaSlicer, Slic3r, CuraSlicer, and SuperSlicer. The
Extreme Slicer name, logo, CUDA/Vulkan compute work, and QIDI workflow changes
are maintained independently in this repository.

# License

- **Extreme Slicer** is a modified work distributed under the GNU Affero General Public License, version 3, as required by the upstream covered work.
- This repository preserves upstream copyright and attribution notices and identifies the project as a modified version.
- The **GNU Affero General Public License**, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.
- OrcaSlicer includes a **pressure advance calibration pattern test** adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.
- The **Bambu networking plugin** is based on non-free libraries from BambuLab. It is optional to the OrcaSlicer and provides extended functionalities for Bambulab printer users.
