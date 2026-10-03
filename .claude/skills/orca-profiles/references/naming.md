# Preset naming

A preset's `name` is its identity, not decoration. The index registers it; `inherits`,
`compatible_printers`, `printer_model` and the `default_*` keys reference it by the exact,
case-sensitive string; `renamed_from` migrates it; `setting_id` and `filament_id` hash it
([ids.md](ids.md)). Treat a name change as an identity change that needs
[migration](vendor-bundle.md#renamed_from), not a relabel. Which part of each name the loader acts on
is the table in [SKILL.md](../SKILL.md#names); this page holds the conventions.

## `machine_model`

`<Model>`, vendor-prefixed: `Bambu Lab X1 Carbon`, `Creality K1`, `Prusa CORE One`. Each variant's
`printer_model` names it verbatim (a mismatch discards the bundle), and its index entry equals the
file's `name`. It is also the stem of `<Model>_cover.png` and, by convention, of the bed assets
(`<Model>_buildplate_model.stl`).

## `machine` (variant)

`<Model> <nozzle> nozzle` is near-universal (`Bambu Lab X1 Carbon 0.4 nozzle`). Casing varies
(`nozzle` / `Nozzle`): match the bundle, not this page. A variant that is not nozzle-specific (a
special toolhead, a multi-material build, IDEX copy and mirror modes such as
`<Model> COPY MODE (0.4 nozzle)`) may drop or reshape the suffix; it is still an exact reference. `printer_variant`
holds the nozzle token: `0.4`, a suffixed `0.4HF`, or `0.4+0.6` for mixed nozzles
([rules](machine-profiles.md#printer_model-and-printer_variant)).

## `process`

`<layer height>mm <quality> @<target>`. The quality word stays before `@` and the printer target after
it: a printer model in the quality position leaves the tier undescribed. The `@<target>` is a label,
and need not equal any variant name; compatibility comes from `compatible_printers` or the
condition. The quality ladder and per-nozzle labels are in
[process-profiles.md](process-profiles.md#naming).

## `filament`

`<Product> @<target>`. The product half, up to the first `@` and right-trimmed, is the **alias**:
shadowing matches on it and `filament_id` hashes it. The target half is a label, except for the
reserved forms:

- `@base`: a non-instantiated product root. Convention only; a base is really
  `instantiation: "false"` without `setting_id`
  ([the three-part shape](filament-profiles.md#the-three-part-shape)).
- `@System`: the OrcaFilamentLibrary selectable shim, and the convention for an all-printer product
  (`<Product> @System`, empty `compatible_printers`). Not enforced, so a deviation is worth a review
  comment. The literal `Generic <mat> @System` is also load-bearing for project recovery
  ([alias shadowing](filament-profiles.md#alias-shadowing)).
- Printer tunes. BBL's shape is the reference: `@<Vendor>` (vendor-wide), `@<Vendor> <Model>` (one
  model), `@<Vendor> <Model> <nozzle> nozzle` (one variant). Other vendors differ: a bare model
  (`QIDI ABS-GF@Q2-Series`), a printer serial, Creality's `@<Model>-all`. Judge specificity from
  `compatible_printers`, never from the name
  ([one variant, one profile](filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product)).
- No colour in the product name: `<Product> <Colour>` presets are not authored; colour is chosen at
  runtime ([colour](filament-profiles.md#colour-is-a-runtime-property)).

## Bases

| Type | Base names |
| --- | --- |
| `machine` | `fdm_machine_common`, `fdm_<vendor>_common`, `fdm_klipper_common`, or an established machine-family base |
| `process` | `fdm_process_*`: shared roots and per-layer-height or per-nozzle bases such as `fdm_process_single_0.20` or `fdm_process_<vendor>_<lh>_nozzle_<n>` |
| `filament` | `fdm_filament_*` material roots, `<Product> @base` product roots |

There is no leading-underscore convention. Base names repeat across bundles by design:
every bundle may have its own `fdm_process_common`, and a product root such as `Fiberon PA6-CF @base`
can exist in both the library and a vendor. Investigate a newly authored base that kept an unrelated
selectable preset's name from a copy.

## Uniqueness

- Type + name is unique within a bundle, indexed or not; `check` enforces it.
- `machine_model` names are unique across the whole tree; `check` enforces it even with `--vendor`.
  Printer-type lookup matches `printer_model` against every vendor's models and takes the first, so a
  duplicate makes it depend on vendor order.
- At load, two selectable presets with one name discard the bundle, and a duplicate across vendors
  is a validator error. Two bases with one name, or a base and a selectable preset, load silently and
  the first in the index wins; an unindexed twin is therefore one `sub_path` edit away from becoming
  the parent every child resolves to.

## Filenames and paths

The loader keys off `name`, and a filename that disagrees usually still loads, but keep the filename
equal to the `name` and to the index `sub_path`. Match the exact case of every `sub_path` and asset
filename: Linux filesystems distinguish case even when a macOS or Windows checkout does not, and
preset-name references are case-sensitive on every platform. Avoid Windows-invalid characters
(`< > : " | ? *`), reserved device names such as `CON` and `NUL` (with any extension), and trailing
spaces or dots in a path component; a space right before `.json` is not a trailing space.

## Checking names

Check the `name` of every newly added profile, and every intentional rename, against its type and role
(model, selectable preset or base). `check` does not enforce the shapes on this page: inspect the
added or renamed presets in the diff, use neighbouring names as context, and follow the bundle's
established style where the conventions allow variation. Preserve shipped names during ordinary
tuning; renaming a shipped selectable preset needs `renamed_from`.
