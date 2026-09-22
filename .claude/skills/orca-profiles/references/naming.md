# Preset naming

A preset's `name` is the loader's key, not decoration. The index registers it; `inherits`,
`compatible_printers` and the `default_*` keys reference it by the exact string; `renamed_from` depends
on it; and `setting_id` / `filament_id` hash it (see [ids.md](ids.md)). Two presets of one type in a
bundle may not share a name (`check_preset_name_uniqueness`). Treat a name change as an identity change,
not a relabel.

Naming is convention only where the loader does not parse it. What the loader actually acts on:

| Type | Shape | Acted on |
| --- | --- | --- |
| `machine_model` | `<Model>` | the exact string, named by a variant's `printer_model` |
| `machine` | `<Model> <nozzle> nozzle` | the exact string, named by `compatible_printers`; `printer_variant` must equal a nozzle diameter |
| `process` | `<lh>mm <Quality> @<target>` | the exact string when referenced or selected; the `@<target>` half is a label |
| `filament` | `<Product> @<target>` | text before the first `@` is the **alias**, used for shadowing; the rest is a label |

## `machine_model`

`<Model>` — the vendor-prefixed model name (`Bambu Lab X1 Carbon`, `Creality K1`, `Prusa CORE One`). A
variant names it verbatim in `printer_model`; a mismatch makes the variant invalid. `check_name_consistency`
forces the index entry to equal the file's `name`, and the variant's `printer_model` targets this string
([A `machine_model` is not a config preset](machine-profiles.md#a-machine_model-is-not-a-config-preset)).
It is also the `<Model>_cover.png` and bed-asset stem.

## `machine` (variant)

`<Model> <nozzle> nozzle` is near-universal (`Bambu Lab X1 Carbon 0.4 nozzle`). `printer_variant` holds
the bare nozzle (`0.4`) and must be an exact member of the model's `nozzle_diameter` list — the hard
rules are in [The `machine` variant](machine-profiles.md#the-machine-variant). Casing varies
(`nozzle` / `Nozzle`): match the bundle, not this page. A variant that is not nozzle-specific (a special
toolhead, a multi-material build) may drop the suffix — still an exact reference. Bases are named
`fdm_machine_common` / `fdm_<vendor>_common`.

## `process`

`<layer height>mm <quality> @<target>` — [process-profiles.md](process-profiles.md#naming) has the
quality ladder and the `fdm_process_*` base names. The `@<target>` is a human label, not a reference: it
usually does not equal a real variant, and compatibility comes from the resolved `compatible_printers`
list or condition.

## `filament`

`<Product> @<target>`. The product half is what `filament_id` hashes and what survives as the **alias** up
to the first `@`; the target half is a label except for reserved forms:

- `@base` — a non-instantiated product root. `@base` is convention; a base is really identified by
  `instantiation: "false"` and no `setting_id` ([the three-part shape](filament-profiles.md#the-three-part-shape)).
- `@System` — the OrcaFilamentLibrary selectable shim, and the convention for an all-printer product
  (`<Product> @System`, empty `compatible_printers`); not enforced, so a deviation is worth a review
  comment. The literal `Generic <mat> @System` is load-bearing for 3MF/project recovery, beyond the
  alias rule ([alias shadowing](filament-profiles.md#alias-shadowing)).
- `@<Vendor>`, `@<Vendor> <Model>`, `@<Vendor> <Model> <nozzle> nozzle` — printer tunes, BBL's shape.
  Other vendors differ (a bare model, a printer serial, Creality's `@<Model>-all`). Specificity is judged
  from `compatible_printers`, not the name
  ([one variant, one profile](filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product)).
- Color is not part of the product name: `<Product> <Color>` presets are not authored; the color is
  chosen at runtime
  ([color is a runtime property](filament-profiles.md#color-is-a-runtime-property)).

## Not the same as the filename

The loader keys off `name`, and a filename that disagrees usually still loads. Index `name` must equal the
file's `name`, and the filename should match `sub_path`; a mismatch that differs only in case breaks
another platform ([cross-platform paths](validation.md#cross-platform-paths)).
