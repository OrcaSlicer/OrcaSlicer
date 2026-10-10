# Printer models and variants

Both live in `resources/profiles/<Vendor>/machine/`; models go in `machine_model_list`, variants and
shared bases in `machine_list`. Every one of them is registered. A bundle may nest further subfolders
under `machine/`, so recurse rather than globbing `machine/*.json`.

## `machine_model`: a record, not a config preset

The loader reads a fixed set of keys from a `machine_model` and stores only these (`version` and `url`
are recognised and discarded):

`name`, `model_id`, `nozzle_diameter`, `machine_tech`, `family`, `bed_model`, `bed_texture`,
`hotend_model`, `default_materials`, `not_support_bed_type`, `image_bed_type`,
`bottom_texture_end_name`, `bottom_texture_rect`, `bottom_texture_rect_longer`, `middle_texture_rect`,
`use_double_extruder_default_texture`.

**Everything else is silently dropped**, a printer config key such as `default_bed_type` or a
misspelling included, so a neighbour carrying a key is no evidence it does anything. Printer config options belong on the `machine` preset, never here. The loader drops a model
silently if its index entry has no name or its `nozzle_diameter` yields no sizes; `check` also requires
the file's own `name`.

```json
{
    "type": "machine_model",
    "name": "Phrozen Arco",
    "machine_tech": "FFF",
    "family": "Phrozen",
    "model_id": "Phrozen Arco",
    "nozzle_diameter": "0.4",
    "bed_model": "Phrozen Arco_buildplate_model.stl",
    "bed_texture": "Phrozen Arco_buildplate_texture.svg",
    "hotend_model": "",
    "default_materials": "Generic PLA @Phrozen Arco 0.4 nozzle"
}
```

| Field | Notes |
| --- | --- |
| identity | **the `name` of the `machine_model_list` entry**, which is what a variant's `printer_model` must equal. `check` forces it to equal the file's `name`, so they coincide. |
| `model_id` | a *separate* cloud/device printer type. Optional, and not required to be unique. Not the model's identity; changing it changes device matching. |
| `machine_tech` | only a value starting with `SL` means SLA; everything else is FFF. Write `FFF`; `FGF` behaves as FFF. |
| `nozzle_diameter` | `;`-separated string, one token per available size. Order is free (`0.4;0.2;0.6;0.8` puts the default first). This list is the authoritative set of legal `printer_variant` values. |
| `default_materials` | `;`-separated filament **preset names**, not `,`; case-sensitive (`@System`); order is ignored. Used to preselect filaments in the setup wizard *and* to install a printer's filaments on first run, so a dangling entry costs a real user a filament. Every name must exist (`check` fails on a name matching no filament file, here or in `default_filament_profile`), and every variant of the model needs at least one entry compatible with it (`validate_system`). |
| `family` | a wizard grouping label only; give every model one. |

### Assets

`bed_model`, `bed_texture` and `hotend_model` are paths relative to the **vendor folder** (named by the
vendor id). Convention: `<Model>_buildplate_model.stl` and `<Model>_buildplate_texture.svg`. An
empty string is the legal "none", and is the norm for `hotend_model`.

**Nothing checks that the files exist.** A missing `hotend_model` falls back to
`resources/profiles/hotend.stl`; a missing `bed_model` makes the bed render as a generic custom bed, and a missing
`bed_texture` renders no texture.
Verify by hand, in exact case.

Every model also has a `<Model>_cover.png` in the vendor folder; treat it as required, not optional.
240×240 is the cap `scripts/optimize_cover_images.py` enforces. A missing cover degrades to a placeholder in both the wizard and the sidebar.

## `machine`: the variant

```json
{
    "type": "machine",
    "name": "Phrozen Arco 0.4 nozzle",
    "inherits": "fdm_machine_common",
    "from": "system",
    "setting_id": "lvaYKTUZr5C9jSwk",
    "instantiation": "true",
    "printer_model": "Phrozen Arco",
    "printer_variant": "0.4",
    "nozzle_diameter": ["0.4"],
    "default_print_profile": "0.20mm Standard @Phrozen Arco 0.4 nozzle",
    "default_filament_profile": ["Generic PLA @Phrozen Arco 0.4 nozzle"],
    "printable_area": ["0x0", "300x0", "300x300", "0x300"],
    "printable_height": "300"
}
```

Minimum viable key set: `type`, `name`, `from`, `instantiation`, `setting_id`, `inherits`,
`printer_model`, `printer_variant`, `nozzle_diameter`, `printable_area`, `printable_height`,
`default_print_profile`. `default_filament_profile` is optional; when written it
is an array (`["Generic PLA @System"]`), while the model's `default_materials` is a `;`-separated
string. Unlike a `machine_model`, a `machine` **is** a config preset, so a key belonging to another
preset type is a reported error and is removed; a misspelled key is still dropped silently.

### `printer_model` and `printer_variant`

1. `printer_model` is non-empty and names a model of this bundle exactly.
2. `printer_variant` is non-empty and an exact token of that model's `;`-separated `nozzle_diameter`
   list.
3. For instantiated presets, when validating: split `printer_variant` on `+`; each token must start
   with a number (a trailing non-numeric suffix such as `HF` is ignored), and the resulting **set** must
   equal the set of `nozzle_diameter` values.

Rules 1 and 2 are loader-enforced: failing either discards the whole bundle. Rule 3 only raises a
validation error: the preset still loads, but the validator exits non-zero.

`nozzle_diameter` lists one entry **per extruder**; `printer_variant` lists the **distinct** diameters
joined with `+`: `["0.4","0.4","0.6","0.6"]` against `"0.4+0.6"` passes because the comparison is on
sets.

Write `printer_variant` as a bare diameter matching the model's list, with no unit. The conventional
values are `0.2`, `0.25`, `0.4`, `0.5`, `0.6`, `0.8` and `1.0`; a suffixed form (`0.4HF`, `0.4HS`) or
the `+` form is legal under rule 3. A `printer_variant` is **not** required to be unique within a
model: an IDEX model's normal, `COPY MODE` and `MIRROR MODE` presets can all be `0.4`.

The converse is **unchecked**: a nozzle size in the model's list with no matching variant is offered in
the wizard and resolves to nothing. A variant whose `printer_model` names a sibling model by mistake
leaves its own model's size in exactly that state.

### Other keys

- `default_print_profile` is a **scalar**, matched by exact preset name; not a `;` list. The named
  process must be compatible with this printer through its resolved list or condition.
  `validate_slice` attempts to select it and rejects generic Default fallbacks, but compatibility
  updates can choose another compatible preset, and `check` does not resolve the name. Check the exact
  default reference yourself.
- `default_filament_profile` is an **array**, one name per element. Entry 0 is the filament preselected
  when the printer is chosen; entry *i* is the preferred replacement when filament *i* is incompatible,
  and any listed name outranks an unlisted one. The validator checks every entry. The list of a
  printer's filaments is the model's `default_materials`: a new filament goes into `default_materials`;
  put it first in `default_filament_profile` only if it should become the preselected one.
- `printable_area` is an array of `"XxY"` strings: four points for a rectangle; a delta or other circular bed
  is a polygon with one point per segment.
- A non-BBL printer shows the plate selector only with `support_multi_bed_types` `"1"`; otherwise it
  uses its `default_bed_type` (a plate name such as `"Textured PEI Plate"`; High Temp Plate when unset).
  Filaments still set every plate
  ([twelve keys](filament-profiles.md#bed-temperature-is-twelve-keys-not-one)).
- `gcode_flavor` is usually set once in the base; the common values are `klipper`, `marlin`, `marlin2`
  and `reprapfirmware`.
- `printer_settings_id` does nothing in a preset file: the app replaces it with the selected preset's
  name before slicing. Omit it, and do not copy it when cloning a bundle.
- `min_layer_height` / `max_layer_height` are **machine** keys (one per extruder), never process keys.

## Bases

The conventional machine root is a base named `fdm_machine_common`, with `fdm_klipper_common` on top
of it for Klipper printers. Which values a hardware
family or model base holds, and when adding one pays off, is in [shared-bases.md](shared-bases.md).

**There is no leading-underscore convention for bases.**

## Adding a printer to an existing bundle

1. Choose the names first: model, variant(s), process(es); everything else references them
   ([naming.md](naming.md)).
2. Add the model (`machine_model_list`) and one `machine` variant per nozzle; the minimum key sets are
   above. Bed assets and `<Model>_cover.png` go directly in `<Vendor>/`.
3. Add at least one process per variant naming it in `compatible_printers`
   ([process-profiles.md](process-profiles.md#adding-a-quality-tier-or-a-nozzles-processes)).
4. Add the variants to the filaments they should offer, and set the model's `default_materials` so every
   variant has a compatible entry.
5. Register everything (`update-index`), bump the version, run the id tool, validate: the
   [authoring workflow](../SKILL.md#creating-or-modifying-a-profile).

## Adding a nozzle variant

1. Extend the model's `nozzle_diameter` (`"0.4"` → `"0.4;0.6"`).
2. Add the variant preset. Either inherit the shared base (the usual choice), or the 0.4 sibling
   (a smaller diff, but the sibling's edits now reach this file too). Follow the bundle.
3. Override what actually changes with the nozzle: `nozzle_diameter`, `printer_variant`,
   `default_print_profile`, `default_filament_profile`, `min_layer_height` / `max_layer_height`, and
   retraction if the vendor tunes it.
4. Add at least one process for the new nozzle (see [process-profiles.md](process-profiles.md)), and
   extend the filaments' `compatible_printers` so at least one `default_materials` entry covers the new
   variant.
5. Register both, bump the version, run the id tool, validate.

## Multi-extruder, IDEX and tool changers

Per-extruder vectors hold one value per extruder (`len(nozzle_diameter)`), and a wrong length raises no
error: a short vector acts as padded with its **first** value, not the last (`extruder_offset`
`["0x0","50x0"]` on a 4-extruder machine reads as `0x0, 50x0, 0x0, 0x0`), and entries beyond the
extruder count are never read.

Note the two sizing families. The plain per-extruder keys (`printer_extruder_options`: `extruder_type`,
`nozzle_diameter`, `default_nozzle_volume_type`, `extruder_printable_height`, `min_layer_height`,
`max_layer_height`, …, given in full with the [key sets](extruder-variants.md#the-four-key-sets), plus
`extruder_offset` and `extruder_colour`) hold one value per extruder. The variant sets
(`retraction_length`, `z_hop`, `wipe`, `nozzle_type`, the `machine_max_*` limits at stride 2;
[the full lists](extruder-variants.md#the-four-key-sets)) are sized to the variant length:
`len(printer_extruder_variant)`, or one variant per extruder when the resolved preset writes no layout,
since `extruder_variant_list` defaults to one `Direct Drive Standard` per extruder
([widths](extruder-variants.md#widths)). `extruders_count` is a printer-tab field, not a preset key; the
loader drops it.

- Give **one entry per extruder** for ordinary per-extruder vectors such as `extruder_offset`,
  `extruder_colour`, `min_layer_height` and `max_layer_height`. Size the variant sets to the variant
  length × stride, one value per variant (per extruder when there is no layout; a (normal, silent)
  pair for the `machine_max_*` limits), even where the values are the same; the loader keeps only the first value of a list-less printer's variant arrays, so declare
  the layout when the extruders differ. A single `["0x0"]` `extruder_offset` on a dual or
  multi-extruder machine pads every extruder to the same offset, so the offset never applies.
- Overriding `nozzle_diameter` to a different count without restating every per-extruder vector is the
  other half of the trap: a 4-extruder preset on a 5-extruder base inherits 5-entry vectors against 4
  extruders.
  The reverse is silent too: a base is stored resized to its own `printer_extruder_variant` (one variant
  when it writes none, whatever its extruder count), so a wider variant array on it reaches the children
  as its first value padded ([composition](extruder-variants.md#padding-truncation-and-composition)).
  `check` does not judge the base on its own; where its extruders need different values, declare the
  layout on the base.

Structure to copy: `Custom/machine/fdm_toolchanger_common.json` + `Custom/machine/MyToolChanger 0.4
nozzle.json` (a minimal variant on a base that gives the per-extruder vectors five entries), and
`Ratrig/machine/RatRig V-Core 4 IDEX 300 0.4 nozzle.json` for IDEX. Take the structure from them and
the widths from the [sizing equation](extruder-variants.md#sizing-equation). Both are list-less, so
every variant array holds one value per extruder and the loader keeps only the first: fine while every
extruder shares the same retraction and limits. Add the extruder-variant layout
(`extruder_variant_list`, `printer_extruder_variant` / `printer_extruder_id`,
`default_nozzle_volume_type`) when the hardware has swappable nozzle volume types or mixed extruder
types, or as soon as one extruder needs its own value in a variant key; how to author it, and the
matching process and filament variants, is in [extruder-variants.md](extruder-variants.md).
(`nozzle_volume_type` itself is not a machine-preset key.)

## Custom G-code

The keys are `machine_start_gcode`, `machine_end_gcode`, `change_filament_gcode`,
`machine_pause_gcode`, `before_layer_change_gcode` and `layer_change_gcode`. Each is one string with
embedded `\n`. Never split G-code into a JSON array of lines: the loader joins array elements with `,`
into a single line (a one-element array is equivalent to the string): a two-element
`machine_start_gcode` becomes one line, `PRINT_START …,SET_PRESSURE_ADVANCE ADVANCE=0.046`.
Conditionals are `{if …}` / `{elsif …}` / `{else}` / `{endif}`.

Placeholder errors only surface when the G-code is actually expanded, which means `validate_slice`:

```bash
./scripts/check_profile.sh --vendor "<Vendor>" validate_slice
# Windows:  scripts\check_profile.bat -Vendor "<Vendor>" validate_slice
```

What the sweep covers is in [validation.md](validation.md#validate_slice); a printer whose output has no
`CP TOOLCHANGE START` fails it, because its `change_filament_gcode` never expanded.
