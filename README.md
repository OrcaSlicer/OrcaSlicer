<div align="center">

<img alt="Unified Slicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">

# Unified Slicer

One slicer for every printer on the bench: Bambu Lab, Elegoo, Creality, Markforged and any Klipper,
OctoPrint or custom machine, plus non-planar printing.

</div>

Unified Slicer is a personal fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) 2.5.0. It
keeps everything OrcaSlicer does and adds the parts that normally need a separate vendor app:
printing to each brand's printers, a Device tab that switches between their clouds, non-planar
slicing from [Unlayered Slicer](https://github.com/Unlayered3D/Unlayered-Slicer), and filament tracking.

> Unified Slicer is not affiliated with OrcaSlicer, Bambu Lab, Elegoo, Creality, Markforged or
> Unlayered. All names and trademarks belong to their owners.

## What it adds to OrcaSlicer

| Area | What you get |
| --- | --- |
| **Elegoo** | Centauri Carbon 2 printing over the local network and through the Elegoo cloud. Live status in the Device tab, plus an ElegooSlicer-style send dialog with filament (Canvas) mapping. |
| **Bambu Lab** | OrcaSlicer's Bambu support, unchanged. Your Bambu account is separate from your other clouds. |
| **Creality** | Creality Print-style send dialog for LAN printers (CFS mapping), and Creality K-series profiles. |
| **Markforged** | Profiles for Mark Two, Onyx One/Pro, X3/X5/X7, FX10 and FX20. Printers and job progress from Eiger in the Device tab. Print sends the plate to Eiger, which slices it and starts or queues it. |
| **Device tab** | One tab for all your printers. A button rail on the side switches between Bambu, Elegoo, Creality and Markforged clouds. |
| **Model browser** | Home-screen tabs to browse and import `.3mf` files from Unlayered Universe, MakerWorld, Elegoo's model site and Thingiverse. |
| **Non-planar printing** | Unlayered's cut-plane multiplanar slicing, curved top layers and hotend collision checks. Off by default, so planar G-code is unchanged. |
| **Filament tracking** | Spool inventory (brand, material, colour, grams left, cost) that subtracts the grams each print uses. |

Each vendor's print dialog follows that vendor's own app, so sending a job feels the same as it
does in ElegooSlicer, Creality Print or Eiger.

### Markforged limits

Markforged printers only run jobs that Eiger, Markforged's closed slicer, has sliced. Unified Slicer
works through Eiger's public API:

- The G-code this slicer makes is only a preview and an estimate. Eiger does the real slicing with
  your Eiger settings preset.
- Pause, stop, temperatures and continuous-fiber placement are not in the API. Use Eiger for those.
- You need API keys from your Eiger organization admin (Settings › API Keys). API access depends
  on your Markforged subscription.

## Branches

- `unified`: the app. All feature branches merge here.
- `main`: tracks upstream OrcaSlicer, so new OrcaSlicer releases can be merged in.

## Building

Unified Slicer builds exactly like OrcaSlicer. For full instructions, see the
[OrcaSlicer build guide](https://www.orcaslicer.com/wiki/how_to_build).

```bash
# Windows (Visual Studio 2022 Build Tools, CMake, Strawberry Perl; about 30 GB free, 1 hour)
build_win.bat

# Linux
./build_linux.sh -dsi

# macOS
./build_release_macos.sh
```

Run the tests with `cd build && ctest -C Release --output-on-failure`.

### Bundled plugins

Plugins in `resources/plugins/` are copied into your data folder (`orca_plugins/`) on first start:

| Plugin | Purpose |
| --- | --- |
| `elegoo_cc2_lan.py` | Centauri Carbon 2 over your network: Device tab status and printing |
| `elegoo_cloud_cc2.py` | Centauri Carbon 2 through the Elegoo cloud |
| `markforged_eiger.py` | Markforged printers through Eiger |
| `filament_tracker.py` | Spool inventory and usage tracking |

## Credits

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) by SoftFever and contributors, which builds on
  [Bambu Studio](https://github.com/bambulab/BambuStudio), [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)
  and [Slic3r](https://github.com/Slic3r/Slic3r).
- [Unlayered Slicer](https://github.com/Unlayered3D/Unlayered-Slicer) for the non-planar slicing engine.
- [ElegooSlicer](https://github.com/ELEGOO-3D/ElegooSlicer) and Elegoo's open-source elegoo-link SDK
  for how Elegoo printers and the Elegoo cloud talk.

## License

- Unified Slicer, like OrcaSlicer, is licensed under the **GNU Affero General Public License,
  version 3**. If you use any part of it, even behind a web server, your software must be released
  under the same license.
- The pressure advance calibration pattern is adapted from Andrew Ellis' generator (GPL-3.0), which
  is itself adapted from Sineos' generator for Marlin (GPL-3.0).
- The **Bambu networking plugin** and Elegoo's cloud messaging library (Agora RTM) are non-free.
  They are not bundled: they are downloaded at runtime, only when you use those printers.
