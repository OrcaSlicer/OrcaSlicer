# Resource Packs — High Level Design

## Purpose and scope

A resource pack is a plugin that ships read-only files for the slicer to use: bed models, bed
textures and displacement textures. Printer presets refer to pack files by a portable reference
instead of an absolute path, so a shared preset keeps its custom bed on another machine.

Packs hold files only. They do not run code at use time and never modify a preset.

## Capability types

Orca Cloud builds the wheel of a resource pack from the files its author uploads. The author
writes no Python, and the cloud stores no per-file metadata. The slicer therefore knows only two
pack types, and what a file is for follows from the type and the file extension.

| Cloud type | Capability id | Python base class | Directory method | Files |
|---|---|---|---|---|
| `resource-textures` | `resource-texture` | `orca.resource_texture.ResourceTexturePluginCapabilityBase` | `get_texture_dir()` | `.png .jpg .jpeg .bmp`, at most 64 |
| `resource-printer-assets` | `resource-printer-asset` | `orca.resource_printer_asset.ResourcePrinterAssetPluginCapabilityBase` | `get_asset_dir()` | `.stl .png .svg` |

`resource-texture` is the original texture pack type. Existing texture plugins, their manifest
entries and the cloud listing depend on its Python contract, type id, 64-file cap and extension
list, so these are fixed.

## File kinds

A kind names a consumer. The registry indexes every pack file under one kind, and a consumer
lists only the files of its kind.

| Kind | Comes from | Extensions | Cap per pack | Consumer |
|---|---|---|---|---|
| `bed_model` | `resource-printer-asset` | `.stl` | 16 | Bed Shape dialog, bed model |
| `bed_texture` | `resource-printer-asset` | `.png .svg` | 16 | Bed Shape dialog, bed texture |
| `displacement_texture` | `resource-texture` | `.png .jpg .jpeg .bmp` | 64 | texture displacement picker |

Kinds, extensions and caps live in `src/slic3r/plugin/ResourcePackKinds.hpp`. A kind is a
slicer-internal name; it is not stored in a pack, a preset or a project. A new kind needs a new
entry there and a consumer.

Extension is enough while each pack type has one use per extension. A pack that needs two
different uses for the same extension (a hotend model next to a bed model, a printer cover next
to a bed texture) needs per-file kinds declared in the pack, which the cloud would have to store
and emit. The registry already indexes by kind, so that is a new capability type and a loader
change, not a registry change.

## Loading and validation

The loader calls the directory method once per capability, under the GIL, and caches the result.
The listing then checks:

- the directory is set, exists, and is inside the plugin folder;
- files are regular files directly in the directory (no recursion), sorted, and each file
  stays inside the plugin folder after symlink resolution;
- files with other extensions are ignored;
- the cap applies after sorting, so every file system keeps the same files.

A Python error or a rejected directory leaves the capability loaded with no files. It never
fails the plugin load.

## Registry

`PluginResourcePacks` tracks every loaded and enabled `resource-texture` and
`resource-printer-asset` capability. Each entry stores strings only (never a Python object,
which would outlive the interpreter): ids, paths, and `kind -> files`. The file lists are a
snapshot taken when the capability loads. Consumers do not rescan the folder; a change shows
after the pack reloads.

Entries are keyed by capability id. A capability name is unique only within one type, so the
texture picker groups sections by `pack_key` (`pack:<name>` or `texture:<name>`), not by name.

## Printer asset references

`bed_custom_model` and `bed_custom_texture` store `orcacloud-asset/<plugin id>/<file>`
(`libslic3r/PrinterAssetRef.hpp`). `<plugin id>` is the Orca Cloud plugin uuid, or the plugin
key for a pack installed by hand. The reference has no kind: the field decides it, and
`resolve(ref, kind)` returns the installed file only if the pack lists that file under that
kind. Only `resource-printer-asset` entries resolve references; a `resource-texture` pack is
never a bed asset pack.

A reference contains no colon or backslash and rejects Windows device names, so it is the
same on every OS. A slicer without the pack resolves nothing and draws the vendor bed, the
same as a foreign absolute path.

## Printer manifest

The hidden option `printer_asset_plugins` names the packs the bed files come from. It is
plugin-backed with type `resource-printer-asset`, so the printer preset manifest
(`printer_plugins`) lists them and the missing-plugin notice offers the install. A before-save
hook re-derives the option on every printer preset save. A pack not installed here keeps the
name already in the saved preset's manifest. The notice never blocks slicing.

## Constraints

- Windows `MAX_PATH`: installed files sit under a long `orca_plugins` path, so Orca Cloud caps
  asset file names at 64 characters. The slicer logs a file it cannot read.
- Orca Cloud caps a printer asset pack at 8 files; the slicer allows 16 per kind.
- Pack files are read-only. Removing a user's own textures never touches pack files.
- Orca Cloud validates uploaded content (STL structure, PNG size, SVG active content). The
  slicer re-checks only the directory layout and extensions.
