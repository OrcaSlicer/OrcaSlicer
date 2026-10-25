# Plate look

Users can change how a plate type appears — its label, its icon and its skin on the 3D bed — without changing the plate type itself. `BedType`, profiles and `.3mf` files are untouched.

## Scope

A look belongs to a printer type: vendor, printer model and bed size. Printers that share all three share their looks. A printer with no resolvable model is keyed by its preset name.

Looks are stored in `AppConfig` under the `plate_look` section, one key per printer type, plate type and field (`label`, `icon`, `skin`). An empty field means the built-in one. They are never written to presets or projects, so they travel with the user's installation, not with a shared project.

## Where it applies

| Part | Used in |
| --- | --- |
| Label | sidebar plate list, per-plate settings dialog |
| Icon | sidebar thumbnail and its hover popup, plate button in the 3D view |
| Skin | the whole bed surface in the 3D view, replacing the built-in strips or logo |

Icons and skins must be PNG, because they are also uploaded as GL textures.

## Editing

In the printer settings, under Basic information > Printable space, a Customise button sits next to "Support multi bed types" and shows only while that option is on. It opens one dialog for all plate types of the printer: a plate type picker, the label (clearable on its own), and the icon and skin, each with a preview and the image size to upload. The icon is square. The skin takes the bed's proportions with its longest side at 2048 px, the largest texture the 3D view keeps. Nothing is saved until OK. `Sidebar::refresh_plate_looks()` then re-reads the labels and icons, and it also runs after a printer switch.

## Constraints

A picture is loaded once per path. A missing or invalid file is not retried every frame; it falls back to the built-in look until the path changes.
