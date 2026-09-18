# Printer models and variants

Both live in `resources/profiles/<Vendor>/machine/*.json`; models go in `machine_model_list`, variants
and shared bases in `machine_list`. Every one of them is registered. Some vendors (Elegoo, Eryone,
InfiMech, FlyingBear) nest a further subfolder under `machine/`, so recurse rather than globbing
`machine/*.json`.

## A `machine_model` is not a config preset

It is parsed by a hand-written key switch, and only these keys are stored (`version` and `url` are
matched and discarded):

`name`, `model_id`, `nozzle_diameter`, `machine_tech`, `family`, `bed_model`, `bed_texture`,
`hotend_model`, `default_materials`, `not_support_bed_type`, `image_bed_type`,
`bottom_texture_end_name`, `bottom_texture_rect`, `bottom_texture_rect_longer`, `middle_texture_rect`,
`use_double_extruder_default_texture`.

**Everything else is silently dropped.** Only `name` and `nozzle_diameter` are required. Dead keys ship
on real models today — `url`, `default_bed_type`, even a `desciption` typo — so a neighbour carrying a
key is no evidence it does anything. Printer config options belong on the `machine` preset, never here.

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
| identity | **the `name` of the `machine_model_list` entry**, which is what a variant's `printer_model` must equal. `check_name_consistency` forces it to equal the file's `name`, so they coincide. |
| `model_id` | a *separate* cloud/device printer type. Optional, and not required to be unique. Not the model's identity. Changing it changes device matching. |
| `machine_tech` | only `starts_with("SL")` means SLA; everything else is FFF. Write `FFF`; a few models write `FGF`, which is a label with no effect. |
| `nozzle_diameter` | `;`-separated string, one token per available size. Order is free (Qidi writes `0.4;0.2;0.6;0.8` to put the default first). This list is the authoritative set of legal `printer_variant` values. |
| `default_materials` | `;`-separated filament **preset names**. Used to preselect in the wizard *and* by `PresetBundle::load_installed_filaments` to auto-install a printer's filaments on first run, so a dangling entry costs a real user a filament. Not `,`; case-sensitive (`@System`). `check` fails on a dangling name here or in `default_filament_profile`. |
| `family` | a wizard grouping label only; give every model one. |

### Assets

`bed_model`, `bed_texture` and `hotend_model` are paths relative to the **vendor folder** (by id).
Majority convention: `<Model>_buildplate_model.stl` and `<Model>_buildplate_texture.svg`. An empty string
is the legal "none", and is the norm for `hotend_model`.

**Nothing checks that the file exists.** A missing `hotend_model` falls back to
`resources/profiles/hotend.stl`; a missing `bed_model`/`bed_texture` just renders nothing. Broken
references already ship. Verify by hand.

Every model also has a `<Model>_cover.png` in the vendor folder — treat it as required, not optional.
240×240 is the cap `scripts/optimize_cover_images.py` enforces and the size most covers already use.
A missing cover degrades to a placeholder in both the wizard and the sidebar.

## The `machine` variant

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
`default_print_profile`. The four keys without which the preset will not load at all are `name`,
`instantiation`, `printer_model` and `printer_variant`; `default_filament_profile` is an array
(`["Generic PLA @System"]`) and the model's `default_materials` a `;`-separated string. Unlike a
`machine_model`, a `machine` **is** config-loaded, so a key belonging to another preset type is a
reported error (a misspelled key is still silent).

### `printer_variant` — three hard rules

1. Non-empty, and an exact member of the model's `;`-separated `nozzle_diameter` list.
2. `printer_model` non-empty and naming a model of this vendor.
3. In validation mode, for instantiated presets only: split `printer_variant` on `+`, each token must
   start with a number (a trailing non-numeric suffix such as `HF` is ignored), and the resulting **set**
   must equal `set(nozzle_diameter)`.

Rules 1 and 2 are loader-enforced — failing either drops the preset *and* the whole bundle. Rule 3 only
raises a validation error: the preset still loads, but the validator exits non-zero.

`nozzle_diameter` lists one entry **per physical nozzle**; `printer_variant` lists the **distinct**
diameters joined with `+`. Snapmaker U1 is the worked case: `["0.4","0.4","0.6","0.6"]` against
`"0.4+0.6"` — it passes because the comparison is on sets.

The conventional values are `0.2`, `0.25`, `0.4`, `0.5`, `0.6`, `0.8` and `1.0`. Suffixed forms
(`0.4HF`, `0.6HF`, `0.8HF`, `0.4HS`) are Flashforge-only and the `+` form is rare. A variant is **not**
required to be unique within a model — Volumic ships `EXO42 IDRE`, `… COPY MODE` and `… MIRROR MODE` all
at `0.4` under the one model `EXO42 IDRE`.

The converse is **unchecked**: a nozzle size in the model's list with no matching variant is offered in
the wizard and resolves to nothing. `Wanhao France`'s `D12 500 PRO M2 DIRECT` ships that bug today.

### Other fields worth knowing

- `default_print_profile` is a **scalar**, matched by exact preset name. Not a `;` list. The named
  process must be compatible with this printer through its resolved list or condition.
  `validate_slice` attempts to select it and rejects generic Default fallbacks, but compatibility
  updates can choose another compatible preset. Check the exact default reference yourself.
- `default_filament_profile` is an **array**, one name per element.
- `printable_area` is an array of `"XxY"` strings — four points for a rectangle, one per segment for a
  delta or circular bed.
- `gcode_flavor` is usually set once in the base; `klipper`, `marlin`, `marlin2` and `reprapfirmware`
  cover nearly every shipped printer.
- `printer_settings_id` is junk — most files carrying it disagree with their own name. Do not copy it
  when cloning a bundle.
- `min_layer_height` / `max_layer_height` are **machine** keys (per extruder), never process keys.

## Bases

Nearly every machine-bearing vendor registers a base literally named `fdm_machine_common`, and Klipper
vendors add `fdm_klipper_common` on top of it. Two levels is the usual depth.

**There is no leading-underscore convention for bases.**

## Adding a printer to an existing bundle

1. Choose the names first — model, variant(s), process(es); everything else references them.
2. Add the model (`machine_model_list`) and one `machine` variant per nozzle; the minimum key sets are
   above. Bed assets and `<Model>_cover.png` go directly in `<Vendor>/`.
3. Add at least one process per variant naming it in `compatible_printers`
   ([process-profiles.md](process-profiles.md#adding-a-quality-tier-or-a-nozzles-processes)).
4. Register everything (or run `update-index`), bump the version, run the id tool, validate.

## Adding a nozzle variant

1. Extend the model's `nozzle_diameter` (`"0.4"` → `"0.4;0.6"`).
2. Add the variant preset. Either inherit the shared base (the usual choice) or the 0.4 sibling (Elegoo,
   BBL, Prusa and Qidi do this — smaller diff, but the sibling's edits now reach this file too).
3. Override what actually changes with nozzle: `nozzle_diameter`, `printer_variant`,
   `default_print_profile`, `default_filament_profile`, `min_layer_height`/`max_layer_height`, and
   retraction if the vendor tunes it.
4. Add at least one process for the new nozzle — see [process-profiles.md](process-profiles.md).
5. Register both, bump the version, run the id tool, validate.

## Multi-extruder, IDEX and tool-changers

Per-extruder vectors are **silently resized** to the nozzle count, with no error. Padding repeats the
**first** value, not the last — `["0.4","0.6"]` on a 4-nozzle machine becomes `0.4, 0.6, 0.4, 0.4`.
Longer vectors are truncated.

Note the two sizing families: the plain per-extruder keys (`extruder_offset`, `extruder_colour`,
`extruder_printable_height`, `min_layer_height`, `max_layer_height`, `nozzle_diameter`) are sized to the
extruder count, while `printer_options_with_variant_1` (`retraction_length`, `z_hop`, `wipe`,
`nozzle_type`, the rest of the retraction family) is sized to `printer_extruder_variant` instead.

- Give **one entry per extruder** for ordinary per-extruder vectors such as `extruder_offset`,
  `extruder_colour`, `min_layer_height` and `max_layer_height`; size the variant-dependent family
  to `printer_extruder_variant` instead.
  A single `["0x0"]` `extruder_offset` on a dual or multi-tool machine — which already ships — pads every
  toolhead to the same offset, so the offset never applies.
- Overriding `nozzle_diameter` to a different count without re-stating every per-extruder vector is the
  other half of the trap — `Snapmaker U1 (0.4+0.6 nozzle)` inherits 5-entry vectors against 4 nozzles.

Copy targets: `Custom/machine/fdm_toolchanger_common.json` + `Custom/machine/MyToolChanger 0.4
nozzle.json` (a clean minimal variant on a base that gives every vector five entries), and
`Ratrig/machine/RatRig V-Core 4 IDEX 300 0.4 nozzle.json` for IDEX. The BBL extruder-variant machinery
(`extruder_variant_list`, `printer_extruder_id`, `default_nozzle_volume_type`) is used by a handful of
vendors — do not copy it into a new bundle (`nozzle_volume_type` itself is not a machine-preset key).

## Custom G-code

The keys are `machine_start_gcode`, `machine_end_gcode`, `change_filament_gcode`,
`machine_pause_gcode`, `before_layer_change_gcode` and `layer_change_gcode`. Both a single string with
embedded `\n` and a JSON array of lines are legal and both are in use — do not convert one into the
other. Conditionals are `{if …}` / `{elsif …}` / `{else}` / `{endif}`; `{elsif}` is rare but real (Qidi's
`layer_change_gcode` uses it).

Placeholder errors only surface when the config is actually expanded, which means `validate_slice`:

```bash
./scripts/check_profile.sh --vendor "<Vendor>" validate_slice
# Windows:  scripts\check_profile.bat -Vendor "<Vendor>" validate_slice
```

What the sweep covers is in [validation.md](validation.md#validate_slice); no `CP TOOLCHANGE START` in
the output means `change_filament_gcode` never expanded.
