# Resource Packs — High Level Design

## Purpose and scope

A resource pack is a plugin that ships read-only files for the slicer to use: bed models,
bed textures and surface (displacement) textures. Printer presets refer to pack files by a
portable reference instead of an absolute path, so a shared preset keeps its custom bed on
another machine.

Packs hold files only. They do not run code at use time and never modify a preset.

## Capability types

| Type id | Python base class | Directory method | Serves |
|---|---|---|---|
| `resource-pack` | `orca.resource_pack.ResourcePackPluginCapabilityBase` | `get_resource_dir()` and `get_resource_kinds()` | every kind |
| `resource-texture` | `orca.resource_texture.ResourceTexturePluginCapabilityBase` | `get_texture_dir()` | `displacement_texture` only |

`resource-texture` is the original texture pack type. It stays a separate capability so that
existing texture plugins, their `resource-texture` manifest entries, and the Orca Cloud
`types=resource-textures` listing keep working unchanged. Its Python contract, its 64-file
cap and its extension list (`.png .jpg .jpeg .bmp`) are fixed. The host serves it through
the same registry as `resource-pack`, as `displacement_texture` files.

## File kinds

A file declares the kinds it serves. The declaration is Python only (`get_resource_kinds()`
returns file name to list of kinds); there is no index file. A consumer lists only the files
declared for its kind.

| Kind | Extensions | Cap per pack | Consumer |
|---|---|---|---|
| `bed_model` | `.stl` | 16 | Bed Shape dialog, bed model |
| `bed_texture` | `.png .svg` | 16 | Bed Shape dialog, bed texture |
| `displacement_texture` | `.png .jpg .jpeg .bmp` | 64 | texture displacement picker |

Kinds and limits live in `src/slic3r/plugin/ResourcePackKinds.hpp`. One file may declare
several kinds, so the same PNG can be both a bed texture and a displacement texture.

## Loading and validation

The loader calls the Python methods once per capability, under the GIL, and caches the
results. `collect_resource_pack_files` then checks:

- the directory is set, exists, and is inside the plugin folder;
- files are regular files directly in the directory (no recursion), sorted, and each file
  stays inside the plugin folder after symlink resolution;
- a file not named in the kinds map is ignored;
- an unknown kind, or an extension not allowed for a kind, skips that kind for that file;
- a name in the map with no file on disk is logged and skipped;
- each kind's cap applies after sorting, so every file system keeps the same files.

A Python error, a non-dict return or a rejected directory leaves the capability loaded with
no files. It never fails the plugin load.

## Registry

`PluginResourcePacks` tracks every loaded and enabled `resource-pack` and `resource-texture`
capability. Each entry stores strings only (never a Python object, which would outlive the
interpreter): ids, paths, and `kind -> files`. The file lists are a snapshot taken when the
capability loads. Consumers do not rescan the folder; a change shows after the pack reloads.

Entries are keyed by capability id. A capability name is unique only within one type, so
the texture picker groups sections by `pack_key` (`pack:<name>` or `texture:<name>`), not
by name.

## Printer asset references

`bed_custom_model` and `bed_custom_texture` store `orcacloud-asset/<plugin id>/<file>`
(`libslic3r/PrinterAssetRef.hpp`). `<plugin id>` is the Orca Cloud plugin uuid, or the plugin
key for a pack installed by hand. The reference has no kind: the field decides it, and
`resolve(ref, kind)` returns the installed file only if the pack declares that file for that
kind. Only `resource-pack` entries resolve references; a `resource-texture` pack is never a
bed asset pack.

A reference contains no colon or backslash and rejects Windows device names, so it is the
same on every OS. A slicer without the pack resolves nothing and draws the vendor bed, the
same as a foreign absolute path.

## Printer manifest

The hidden option `printer_asset_plugins` names the packs the bed files come from. It is
plugin-backed with type `resource-pack`, so the printer preset manifest (`printer_plugins`)
lists them and the missing-plugin notice offers the install. A before-save hook re-derives
the option on every printer preset save. A pack not installed here keeps the name already in
the saved preset's manifest. The notice never blocks slicing.

## Constraints

- Windows `MAX_PATH`: installed files sit under a long `orca_plugins` path, so Orca Cloud caps
  asset file names at 64 characters. The slicer logs a file it cannot read.
- Pack files are read-only. Removing a user's own textures never touches pack files.
- Orca Cloud validates uploaded content (STL structure, PNG size, SVG active content). The
  slicer re-checks only the directory layout and extensions.
