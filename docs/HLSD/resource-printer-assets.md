# Resource Printer Assets — High Level Design

## Purpose and scope

A resource printer asset pack is a plugin that ships bed models (`.stl`) and bed textures
(`.png`, `.svg`). A printer preset refers to a pack file by a portable reference instead of an
absolute path, so a shared preset keeps its custom bed on another machine.

Packs hold files only. They never run code at use time and never modify a preset.

The type is independent of the resource texture type (`resource-texture`). It has its own
capability, registry, loader branch and tests, so either can be changed or removed without the
other.

## Capability

| Orca Cloud type | Capability id | Python base class | Directory method |
|---|---|---|---|
| `resource-printer-assets` | `resource-printer-asset` | `orca.resource_printer_asset.ResourcePrinterAssetPluginCapabilityBase` | `get_asset_dir()` |

Orca Cloud builds the wheel from the uploaded files, so the author writes no Python. The loader
calls `get_asset_dir()` once per capability under the GIL and caches the result.

## Loading and validation

`collect_printer_asset_files` lists the folder:

- it must be set, exist, and be inside the plugin folder;
- only regular files directly in it (no recursion), with extension `.stl`, `.png` or `.svg`
  (any case), each inside the plugin folder after symlink resolution;
- the list is sorted, then capped at 16, so every file system keeps the same files.

A Python error or a rejected folder leaves the capability loaded with no files. It never fails
the plugin load.

## Registry

`PluginPrinterAssets` tracks every loaded and enabled `resource-printer-asset` capability. An
entry stores strings only (never a Python object, which would outlive the interpreter): ids,
paths and the validated file list. Its lifecycle functions return whether the set of packs
changed, so the bed is redrawn only then.

The Bed Shape dialog lists `.stl` files for the model and `.png`/`.svg` files for the texture,
chosen by extension.

## References

`bed_custom_model` and `bed_custom_texture` store `orcacloud-asset/<plugin id>/<file>`
(`libslic3r/PrinterAssetRef.hpp`). `<plugin id>` is the Orca Cloud plugin uuid, or the plugin
key for a pack installed by hand. `<file>` uses `A–Z a–z 0–9 . _ -`, at most 128 characters, and
is not a Windows device name (`CON`, `NUL`, `COM1`, ...). A reference has no colon or backslash,
so it is the same on every OS.

`resolve` matches the file name against the validated list, so a reference cannot leave the
pack. A slicer without the pack resolves nothing and draws the vendor bed, the same as a foreign
absolute path.

## Printer manifest

The hidden option `printer_asset_plugins` names the packs the bed files come from. It is
plugin-backed with type `resource-printer-asset`, so the printer preset manifest
(`printer_plugins`) lists them and the missing-plugin notice offers the install. A before-save
hook re-derives the option on every printer preset save. A pack that is not installed here keeps
the name already in the saved preset's manifest. The notice never blocks slicing.

A printer preset written before `printer_plugins` existed holds its references under the process
preset's `plugins` key. `Preset::remove_invalid_keys` renames them on load, before the
unknown-key check, so an upgrade keeps the notice for a plugin this machine lacks.

## Constraints

- Windows `MAX_PATH`: installed files sit under a long `orca_plugins` path, so Orca Cloud caps
  asset file names at 64 characters. The slicer logs a file it cannot read.
- Orca Cloud caps a pack at 8 files; the slicer allows 16.
- Pack files are read-only.
- Orca Cloud validates uploaded content (STL structure, PNG size, SVG active content). The
  slicer re-checks only the folder layout and extensions.
- A file's use follows its extension. A pack cannot give one extension two uses.
