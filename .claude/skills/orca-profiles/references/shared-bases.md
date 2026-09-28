# Shared bases

Use this when drafting several presets at once, after `fix-variant`, or when sibling presets repeat the
same values. Each value is written once, on the base of the group it is true for, and a selectable
preset holds its identity and what makes it different. The work has two halves: choosing the groups,
which is judgment, and moving the values, which must leave every preset loading exactly what it loaded
before.

## When

- **Drafting** a printer family, a quality ladder or a product line: place each setting at its
  [level](#levels) before writing any preset, then write the presets as overrides
  ([Rule 7](../SKILL.md#rules)).
- **After `fix-variant`.** It resizes each array in the selectable preset that writes it and never
  moves or deletes a value ([variant arrays](validation.md#variant-arrays)). A family whose presets all
  wrote one value now repeats the widened array in every preset, and a preset that restated what it
  inherits now restates it wider.
- **Converting full copies**: a machine that inherits nothing, or a filament that restates fifty-plus
  keys, is the style to move away from ([filament style](filament-profiles.md#style)).

Out of scope:

- **BBL.** Its profiles are synced from BambuStudio: a restructure is overwritten by the next sync and
  makes every later sync diff unreadable. Fix BBL values in the file that holds them.
- **OrcaFilamentLibrary bases for one vendor's values.** A library base reaches every bundle's
  filaments. Put a vendor's shared values on a base in its own bundle (a product `@base`, or a vendor
  base that inherits the library's), and change a library base only for a value true of every filament
  below it in every bundle.
- **Families the change does not touch.** Restructure the presets you are already changing; a
  bundle-wide pass is a change of its own. Commit a restructure without any value change, so `compare`
  alone verifies it.

## Nothing loads differently

A restructure changes where values are written, never what a selectable preset loads. Presets keep
their `name` and `setting_id`, and user presets and projects refer to a system preset by name and store
their own changes against what it loads, so an unchanged load changes nothing for users. Prove it with
the bundled helper, from the repository root (every subcommand takes `--profiles <dir>` to work on a
copy of the tree):

```bash
python3 .claude/skills/orca-profiles/scripts/shared_settings.py snapshot <before.json>   # before any edit
python3 .claude/skills/orca-profiles/scripts/shared_settings.py compare <before.json>    # after: "0 difference(s)", exit 0
```

`snapshot` records every selectable preset of every bundle as the loader stores it: the parent's stored
config, each `include` at the width its file wrote, the preset's own keys, then every variant array
resized to the preset's own variant list ([composition](vendor-bundle.md#inherits-and-include)). It
models the step `check` does not see: a base is stored after its own resize, so an array wider than a
base's list reaches its children cut. It also leaves out what the loader reads from each file and never
hands down (`name`, `type`, `from`, `instantiation`, `inherits`, `include`, `setting_id`,
`renamed_from`, `description`, `version`, `url`, `is_custom_defined`), so those keys never move to a
base. Nor does it record `print_settings_id`, `printer_settings_id` or `filament_settings_id`: the app
replaces them with the selected presets' names before slicing, so a file's value never counts. Delete
them from the presets you restructure rather than moving them to a base.

It does not know the built-in defaults, so `compare` lists a key written on one side only (no file of
the preset's chain writes it on the other) apart from the differences, and exits 1 for either. A
difference is a value the edit changed: undo it, or make it a separate, deliberate change. A one-sided
key is no change only when its written value is the option's default in `PrintConfig.cpp`: confirm
each, as when a preset that loaded the default by leaving a key out must now write it
([Balance 5](#balance)). A value equal to the default needs writing nowhere when no base above writes
another: delete it from the presets rather than moving it, and confirm the one-sided keys.

## Levels

A base stands for a level of the vendor's catalogue, and a setting lives at the level that determines
it. The test for a shared value: if it had to change for one preset of the group, should it change for
all of them? If yes, it is the group's and goes on the group's base. A value that is only equal today
(two unrelated printers with the same acceleration) stays in each preset: a base built on coincidence
is later edited for one preset and silently changes the others. For a default with exceptions
([Balance 5](#balance)), ask the question of the presets that inherit the default.

The tables give each setting's usual level; the test decides for a given bundle: where each toolhead
(Bowden or Direct Drive) has its own default filament, `default_filament_profile` follows the
toolhead, not the nozzle. Levels run
coarse to fine, and a chain need not visit every level: each preset or base inherits the next coarser
level that has a base.

| Machine level | Base | Settings it determines |
| --- | --- | --- |
| Vendor | `fdm_machine_common`, `fdm_<vendor>_common` | the vendor's defaults for every printer |
| Firmware | `fdm_klipper_common`, `fdm_marlin_common` | `gcode_flavor`, G-code that calls the firmware's macros (layer change, pause, filament change), `host_type`, `print_host`, the thumbnail format |
| Hardware family: models that share a frame, motion system, toolhead or extruder layout | `fdm_<vendor>_<family>_common` (`fdm_qidi_x3_common`, `fdm_machine_eryone_ER20_common`) | the `machine_max_*` limits, `extruder_clearance_*`, the toolhead's retraction where every nozzle shares it, `z_hop` and wipe, fitted hardware such as `auxiliary_fan`, the extruder count and per-extruder vectors, the variant layout ([Printer rule 2](extruder-variants.md#printer-machine)) |
| Model: one `machine_model`, which for an IDEX printer includes its mode | the default-nozzle preset where the bundle hangs its other nozzle presets off it; otherwise `fdm_<vendor>_<model>_common` | `printable_area`, `printable_height`, `bed_exclude_area`, the model's start G-code, a COPY or MIRROR mode's settings |
| Nozzle: the selectable preset | none | `printer_model` and `printer_variant`, which name the preset's model and nozzle and stay in every preset as the tree writes them; `nozzle_diameter`, `min_layer_height`, `max_layer_height`, `default_print_profile`, `default_filament_profile`, retraction the vendor tunes per nozzle |

A family may split once more, into toolhead or revision groups that exist only within it:
`fdm_<vendor>_<family>_common` → `fdm_<vendor>_<family>_mk1_common`. A split that crosses another axis is not a
level: when every controller comes with every toolhead, a toolhead base under each controller base
repeats the same values in each ([Balance 6](#balance)).

| Process level | Base | Settings it determines |
| --- | --- | --- |
| Vendor | `fdm_process_common`, `fdm_process_<vendor>_common` | strategy: seam, wall order, infill and support patterns |
| Printer family or variant layout | `fdm_process_<vendor>_<family>_common` (`fdm_process_arena_common`, BBL's `fdm_process_dual_common`) | speeds, accelerations and jerk of that motion system; the variant layout ([Process rule 4](extruder-variants.md#process)) |
| Layer height × nozzle | `fdm_process_<vendor>_<lh>_nozzle_<n>` (BBL's `fdm_process_single_0.20`) | `layer_height`, line widths, shell layers, speeds scaled to the layer |
| Quality × printer: the selectable preset | none | `compatible_printers`, and what is unique to that combination |

| Filament level | Base | Settings it determines |
| --- | --- | --- |
| Material | `fdm_filament_<material>` in OrcaFilamentLibrary, shared by every bundle | material defaults |
| Product | `<Product> @base` | `filament_id` (minted here, [ids](ids.md)), `filament_vendor`, `filament_type`, density, cost, the product's temperatures and cooling |
| Printer or nozzle tune: the selectable preset | none | `compatible_printers` (always in its own file, [Rule 9](../SKILL.md#rules)), volumetric speed, flow ratio, pressure advance and retraction measured on that printer |

**Equal where they must differ is a copy.** A key that follows a finer level, such as the layer-height
limits and line widths that follow the nozzle diameter or `printable_area` that follows the bed, never
moves above that level. When presets that differ in it carry the same value, the value was copied: leave
it in the presets and report it (the [worked example](#worked-example-an-idex-family) has one).

## Balance

1. **One group, one base; use the existing one first.** A base is the home of the presets below it,
   whatever its name. In a bundle whose presets are all one family, the vendor base is the family base,
   so the family's values go there. In a bundle that hangs the other nozzles off the default-nozzle preset, that preset is the model's home.
   Never create a base whose presets are exactly its parent's. Where two existing bases already serve
   the same presets (a copied `fdm_machine_common` above the vendor's own base), the finer one is the
   home, and merging the pair is a change of its own. Moving a key into an existing home adds no file.
   A base value that no preset below it loads is dead: replace it with the group's value when there is
   one; otherwise leave it and report it, since a future preset would inherit it.
2. **A new base must stand for a level and pay for itself.** Its file costs five metadata keys and an
   index entry, so create it only when it takes `k` keys off `n` selectable presets with
   `(n − 1) × k > 6`, where a custom G-code value counts as one key per G-code line: what it removes
   must outnumber what it adds. A model with two nozzles that share three short keys keeps them in both.
3. **No ad-hoc bases:** never a base for presets that merely agree (the ones with 0.8 mm retraction),
   and never a base with one preset below it.
4. **Keep every selectable preset within four ancestors**, selectable parents included. When a level would push a preset past that, fold it into the level above or leave
   its keys in the presets.
5. **A default with exceptions.** A value that only some presets below a shared base load moves to that
   base, whichever axis it follows, when two conditions hold. More presets load it than any other value
   (on a tie the key stays in the presets), and `w − a > 1`, where `w` presets drop their copy and `a`
   presets that take the key from the base and load another value, the built-in default included, must
   now write theirs. Presets that write another value keep it and are unaffected. The base then holds
   the group's default, and the exceptions stay visible in their own files.
6. **One chain; the other axes stay in the presets.** `inherits` follows one axis. When presets vary
   along several (bed size × controller × toolhead), first fill the existing homes by
   rules 1 and 5. Then give new bases to the axis whose bases pay most: sum rule 2's count over its
   bases, less the keys a re-parented preset must now write because it no longer inherits them from its
   old parent; on a tie, follow the layering the bundle already has. Leave the other axes' keys in the
   presets, and never repeat one axis's bases under each group of another. Outside BBL, whose synced
   presets need theirs, add no `include` template for a second axis: the loader reads `include` only
   since #15869 (2026-09-25), and an app that predates it ignores the key, so the template's settings
   never reach the preset.

Name a new base after its level ([base names](naming.md#bases)). The name must be unique in its bundle
and must not equal a selectable preset's: two such presets load silently and the first in the index wins
([uniqueness](naming.md#uniqueness)).

## Restated values

`candidates` lists every key a file writes that it would load unchanged without writing it. Delete it
when the value is what the presets below the base that supplies it share: more of them load it,
written or inherited, than any other value. A family that restates machine limits, clearances and
G-code every other printer of the bundle loads from `fdm_klipper_common` drops its copies. When most
presets below that base load another value, the match is a coincidence: keep the key, and move it to
the level of the presets that share it. Keys of the nozzle level stay in the preset in
either case.

After a restructure the report still lists keys that are right where they are: the nozzle-level keys
each preset keeps, and arrays a list-less multi-extruder base writes at its presets' width for
`check --strict`.

## Variant arrays on a base

- The loader stores a base with its variant arrays resized to the base's own variant list, one variant
  when it writes none, so an array on a narrower base reaches its presets as its first value, padded.
  Move a variant array to a base only when the base declares the presets' variant list (and, for a
  machine or process, their ids), moving the layout keys with it as
  [Printer rule 2](extruder-variants.md#printer-machine) and [Process rule 4](extruder-variants.md#process)
  ask; or when the presets load its first value anyway: every value is equal, or the presets are
  list-less machines, which the loader cuts to one variant. `compare` catches a cut.
- Write the array on the base at the width of the presets it serves, their `N`
  ([sizing equation](extruder-variants.md#sizing-equation)), so `check --strict` judges the right width
  where it reaches them. When the presets below a base need different widths (single- and
  dual-extruder models on one base), leave the array in the presets, or on bases that each serve one
  width.
- On a list-less multi-extruder family base, declare the extruder count: `nozzle_diameter` and the other
  per-extruder vectors at one entry per extruder. The base then has its presets' width, and
  `fix-variant --strict` writes an array that reaches the presets at another width into the base once,
  instead of into every preset.
- Deleting a restated variant array leaves the preset on the inherited array. Plain `check` accepts
  that; `check --strict` reports it when the inherited width differs, which is why a bundle held to
  `--strict` restates the array at each preset's width.

## Procedure

1. **Snapshot** the tree before any edit, `fix-variant` included. After `fix-variant`, run `compare`: a
   difference is a value its padding changed (it repeats the last value, the loader the first). Set that
   value deliberately, then snapshot again as the baseline for the restructure.
2. **List the candidates:**

   ```bash
   python3 .claude/skills/orca-profiles/scripts/shared_settings.py candidates --vendor "<Vendor>" --type machine
   python3 .claude/skills/orca-profiles/scripts/shared_settings.py candidates --vendor "<Vendor>" --type machine --group-by printer_model
   ```

   It prints the [restated values](#restated-values), then, per base, the keys every selectable preset
   below it loads with one value and how many of those presets write it themselves, and under
   `default with exceptions` the values that pass [Balance 5](#balance), with `w` and `a`. With
   `--group-by <key>` it groups the selectable presets by that key's value instead and names each
   group's nearest common base, where a new base would go: `printer_model` for models, `gcode_flavor`
   for firmware, `extruder_type` or `default_filament_profile` for toolheads, `filament_id` for
   filament products, `layer_height` for processes. The report is evidence, not a plan: it cannot tell
   a shared value from a coincidence or a copy.
3. **Decide each key** by [Levels](#levels), [Balance](#balance) and
   [Restated values](#restated-values): delete the restatements of shared values, move group values up
   to the group's home, and create only the bases that pay.
4. **Edit.** Each new base gets `"type"`, `"name"`, `"from": "system"`, `"instantiation": "false"`, no
   `setting_id`, and `inherits` set to the presets' old parent. Point the presets' `inherits` at it and
   delete the moved keys from them. Bump the version, then run `normalize`, `update-index` (it orders
   parents first) and `generate-id --dry-run`, which must write nothing: bases take no id and presets
   keep theirs.
5. **In a bundle held to `--strict`, run `fix-variant --strict` now**, so it writes into the new bases.
6. **Verify.** `compare` prints `0 difference(s)`, and every one-sided key it lists is a default.
   `check` reports no error it did not report before, and neither does `check --strict` where the bundle
   passes it. Then run the [authoring checks](../SKILL.md#creating-or-modifying-a-profile).
7. **Report** each base added (name, level, presets below it, keys it holds), the keys left in presets
   and why, the copies found, and the `compare` result.

## Worked example: an IDEX family

A Klipper bundle's IDEX family has 36 selectable machines (bed size 300, 400 or 500 × normal, COPY or
MIRROR mode × 0.4, 0.5, 0.6 or 0.8 nozzle) that all inherit `fdm_klipper_common` directly and write 49
keys each. `fix-variant` has widened their retraction arrays to two values and their machine limits
to four.

- **Restated.** All 36 restate 10 values that every other printer of the bundle loads from
  `fdm_klipper_common`: three machine limits, the three clearances, wipe, `retract_before_wipe`, and the
  layer-change and pause G-code. Delete them.
- **Family.** All 36 load one value for 24 more keys: the other machine limits, `extruder_offset`,
  retraction, `z_hop`, `single_extruder_multi_material`, `manual_filament_change`, the remaining G-code
  except the start G-code, and thumbnails. A new family base, `fdm_<vendor>_<family>_idex_common`,
  holds them, plus `nozzle_diameter` `["0.4", "0.4"]` for the extruder count: at least
  `(36 − 1) × 24 = 840`.
- **Model.** Each `printer_model` (a bed size in one mode) has four nozzle presets that share
  `printable_area`, `printable_height` and a three-line `machine_start_gcode`: `(4 − 1) × 5 = 15`, so
  one base per model, nine in all. The family does not hang its other nozzles off a default-nozzle
  preset, so the model level here is a base.
- **A coincidence.** The twelve 500 presets' `printable_height` 500 equals `fdm_klipper_common`'s, but
  every preset of the bundle writes its own height and 300 is the most common. The 500 is the base's
  leftover, not a shared value, so it stays on the model bases.
- **A copy.** In COPY and MIRROR mode, every nozzle of a model carries the 0.4 nozzle's
  `min_layer_height`, `max_layer_height` and `retract_lift_below`: 0.06, 0.3 and 0.2 on the 0.8 nozzle,
  where normal mode has 0.12, 0.5 and 0.3. `candidates --group-by printer_model` lists the first and
  last as shared by the model, and `max_layer_height` as restated from `fdm_klipper_common`, whose value
  is also 0.3. They follow the nozzle, so they stay in the presets and are reported for tuning.
- **Nozzle.** Each preset keeps `printer_model`, `printer_variant`, `nozzle_diameter`,
  `min_layer_height`, `max_layer_height` and `retract_lift_below` beside its metadata.
- **Result.** 36 full presets become 36 short ones on 10 new bases. `compare` reports 0 differences,
  `check --vendor "<Vendor>"` passes as before, and `generate-id --dry-run` writes nothing.
  `check --strict` reports more errors than before, because the deleted variant arrays now reach the
  presets at `fdm_klipper_common`'s one value. `fix-variant --strict` writes those arrays into the
  family base alone, after which `check --strict` passes for the family and `compare` still reports 0
  differences.
