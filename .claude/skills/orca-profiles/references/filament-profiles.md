# Filament profiles and OrcaFilamentLibrary

`OrcaFilamentLibrary` is the filament-only bundle the loader reads **first**, so any vendor's filament may
inherit a library preset by name. It is the only cross-bundle parent: vendor-to-vendor inheritance
always fails.

## Where a filament goes

| Contribution | Location |
| --- | --- |
| Generic material for all printers | `OrcaFilamentLibrary/filament/Generic <mat> @System.json` |
| A brand's product, all printers | `OrcaFilamentLibrary/filament/<Brand>/` |
| A brand's tune for one printer vendor | `OrcaFilamentLibrary/filament/<Brand>/<PrinterVendor>/` (recommended); `<PrinterVendor>/filament/<Brand>/` also works |
| A printer vendor's tune of a generic, or its own product | `<PrinterVendor>/filament/` |

Both locations in the third row are supported: `OrcaFilamentLibrary/filament/<Brand>/<PrinterVendor>/<Name>.json`
(the shape the wiki shows) and `<PrinterVendor>/filament/<Brand>/`. The library path is the one a
filament brand should contribute to: `OrcaFilamentLibrary/filament/<Brand>/` is the brand's own folder,
while a printer vendor's folder belongs to that printer vendor.

Library layout: `filament/base/fdm_filament_*.json` material roots, root-level
`Generic <mat> @System.json` generics, and one subfolder per brand, which may nest printer-specific
tunes one level deeper. Adding a brand means adding a folder here; the folder name is a directory label
only, and `filament_vendor` inside the JSON is the real vendor string.

## The three-part shape

```jsonc
// OrcaFilamentLibrary/filament/Polymaker/Fiberon PA6-CF @base.json — the product root: identity + material values
{ "type": "filament", "name": "Fiberon PA6-CF @base", "from": "system",
  "instantiation": "false", "inherits": "fdm_filament_pa",
  "filament_id": "OFkOviHk",                 // minted here by generate-id; every child inherits it
  "filament_vendor": ["Polymaker"], "filament_type": ["PA6-CF"], /* … */ }

// OrcaFilamentLibrary/filament/Polymaker/Fiberon PA6-CF @System.json — the selectable all-printer shim, 7 keys
{ "type": "filament", "name": "Fiberon PA6-CF @System", "from": "system",
  "instantiation": "true", "inherits": "Fiberon PA6-CF @base",
  "setting_id": "…", "compatible_printers": [] }

// BBL/filament/Polymaker/Fiberon PA6-CF @BBL X1C.json — a printer tune (BBL keeps its own copy of the @base)
{ …, "inherits": "Fiberon PA6-CF @base", "filament_max_volumetric_speed": ["14"],
  "compatible_printers": ["Bambu Lab X1 Carbon 0.4 nozzle", …] }
```

- `@base` is the convention for a root; a root is really `instantiation: "false"`. A base carries
  **no** `setting_id`, no `compatible_printers` and no `filament_settings_id`. Only the `setting_id`
  half is enforced; the other two are unchecked, so a neighbouring base that carries them is no model.
- Every `@System` shim must be `"instantiation": "true"`; one set to `"false"` would ship but could
  never be selected, and no check catches it. The shim exists only for products in the library.
- `filament_cost`, `filament_density`, `filament_type` and `filament_vendor` belong on the root and
  should not appear in a printer tune.
- A brand `@base` duplicated across bundles is legal (a vendor bundle may keep its own copy of a
  library product root, with the same id): bases never become selectable presets and the
  duplicate-name error covers only those, so there is none.
- You may inherit from an instantiated preset as well as from a base.

## Colour is a runtime property

`filament_id` identifies a product, not a colour; filament sync and AMS read the colour from the spool
at runtime. A product ships one all-printer preset and the colour is chosen at runtime, never a sibling
preset that differs only by colour. A material family (PLA vs PLA Matte vs PLA Silk) is a new product;
a colour is not. A printer tune keeps the product alias and does not multiply per colour either.

CI does not catch this (per-colour presets pass `check`), so it is a review call.

## The two most common contributions

**A printer vendor tuning a generic.** Keep the `Generic X` alias so it shadows the library preset on
your printers, inherit `Generic X @System`, declare **no** `filament_id` (inheriting the library's is
correct: the product really is the library's generic), and give it a non-empty `compatible_printers` in
its own file:

```jsonc
// <Vendor>/filament/Generic PETG @Acme One 0.4 nozzle.json
{ "type": "filament", "name": "Generic PETG @Acme One 0.4 nozzle", "from": "system",
  "instantiation": "true", "inherits": "Generic PETG @System",
  "filament_flow_ratio": ["0.95"], "filament_max_volumetric_speed": ["10"],
  "compatible_printers": ["Acme One 0.4 nozzle"] }
```

**A printer vendor's own branded product.** Give it a `@base` root on a material base so `generate-id`
can mint the id, then one instantiated leaf per printer in the same bundle. Inheriting
`Generic X @System` directly gives the product the generic's id, which the tool cannot fix
([ids.md](ids.md#what-generate-id-does-and-does-not-fix)). No `@System` shim: that is only for a product
entering OrcaFilamentLibrary.

```jsonc
// <Vendor>/filament/Acme Aura PETG @base.json — instantiation false, no setting_id
{ "type": "filament", "name": "Acme Aura PETG @base", "from": "system",
  "instantiation": "false", "inherits": "fdm_filament_pet",
  "filament_vendor": ["Acme"], "filament_type": ["PETG"] }   // filament_id minted here

// <Vendor>/filament/Acme Aura PETG @Acme One 0.4 nozzle.json
{ "type": "filament", "name": "Acme Aura PETG @Acme One 0.4 nozzle", "from": "system",
  "instantiation": "true", "inherits": "Acme Aura PETG @base",
  "filament_max_volumetric_speed": ["11"],
  "compatible_printers": ["Acme One 0.4 nozzle"] }
```

Omit `filament_settings_id` from new presets: it is runtime bookkeeping the app rewrites to the preset
name.

To offer either kind by default, add its name to each model's `default_materials`; put it first in the
machine's `default_filament_profile` only if it should be the preselected filament
([machine keys](machine-profiles.md#other-keys)).

## `compatible_printers`

- **Library fallbacks** (`@System`): empty `[]` or absent, so they are offered on all printers except
  where [alias shadowing](#alias-shadowing) supplies a printer-specific tune.
- **Library printer-specific tunes**: non-empty, listing exact printer **variant** names. These
  supersede a same-alias fallback just like a tune in a printer vendor's bundle.
- **Instantiated filaments in every other vendor**: non-empty, listing exact printer **variant** names.
  Enforced twice, but not identically: `validate_system` reads the resolved config, so an inherited list
  satisfies it, while `check` reads the file's **own** key. Write the list in the file itself. This is
  the most common filament CI failure.
- Emptying it to "make it apply everywhere" fails that check *and* collides with the library generic's
  `filament_id` on every printer.
- Copying a base's full printer list onto a nozzle-specific tune produces duplicate combobox entries: a
  real shipped bug twice over.

## Overlapping coverage: one variant, one profile per product

`filament_id` is the **product** key, not the preset key: every preset of one product shares it
(`<filament_vendor>/<filament_type>/<alias>`). So if one printer variant appears in the
`compatible_printers` of two presets of that product, the slicer cannot tell them apart at AMS match
time. `validate_system` reports `Ambiguous AMS filament match: N filament presets share filament_id "X"
and are all compatible with printer "Y"`; `orca_profile_tool.py check` does **not** see it and passes.
Resolve the overlap by **specificity**: keep the variant on the most specific profile and remove it from
every more general one. Deleting a profile is the least preferred fix: moving coverage keeps the tune
that users rely on.

Judge specificity from the profile's `compatible_printers` (how many variants it actually covers) and
use the name only as a secondary, often vague hint; decide by the lists, with a judgement call on the
name. Naming conventions differ by vendor: BBL's is the reference (`@<Vendor> <Model>` for a whole
model, `@<Vendor> <Model> <nozzle> nozzle` for one variant, `@<Vendor>` for a vendor-wide generic), but
others vary (`@<printer model>`, a printer serial, or Creality's `@<Model>-all`). A name never overrides
the list; see [preset naming](naming.md#filament) for the shapes.

Specificity, most to least:

1. **Variant-specialized**: lists a single printer variant (BBL-style
   `… @<Vendor> <Model> <nozzle> nozzle`).
2. **Model-specialized**: lists the variants of one printer model (BBL-style `… @<Vendor> <Model>`). It
   should cover every variant of its model, not only the nozzle it was authored for.
3. **Family / series**: lists variants spanning a printer family or series.
4. **Generic / catch-all**: vendor-wide, covering many unrelated models (often the bare
   `Generic <mat> @<Vendor>`).

Rules:

- A model-specialized profile is extended to **all** variants of its model, and each variant it thereby
  starts covering is removed from the family and generic profiles that also listed it, including
  variants that had no overlap before. Apply it per nozzle, not just 0.4.
- Apply it **per product**: trim only the material that has a specialized profile from the generic; a
  material whose product has no specialized profile keeps the variant in the generic.
- Never strip coverage a variant has nowhere else to get. If a variant has no variant-level specialized
  profile, the next level down keeps it; when the model has specialized profiles, the model-level one
  wins over the family and generic ones.
- Moving coverage is preferred over deleting. If a profile must be deleted, remove the more general one,
  not the specialized profile that carries the tune.
- After moving coverage, repoint the affected machine's `default_filament_profile` and clean the model's
  `default_materials`: they should name the most specific profile that covers the variant, and should
  not keep generic entries that no longer cover the model. This rule applies equally when adding or
  fixing defaults.

Several profiles of one product with **disjoint** `compatible_printers` is the intended end state.
Adding coverage to the specialized profile and removing it from the generic is the preferred direction.

**Detection caveat:** `orca_profile_tool.py check` is blind to this; only the validator behind the full
`./scripts/check_profile.sh` reports it (`validate_system`). Always confirm with that, not the
vendor-scoped loop.

## Alias shadowing

A printer-specific filament in either the library or a vendor bundle supersedes the library fallback on
the printers it lists. The matching key is the **alias**: the preset name up to the **first** `@`,
right-trimmed (no `@` → the whole name). So `QIDI ABS-GF@Q2-Series` aliases to `QIDI ABS-GF`.

A library preset with an empty `compatible_printers` is hidden on every printer that a same-alias preset
lists in a non-empty `compatible_printers`, whether that preset is in the library or in a vendor bundle
(a printer matches by its own name or its parent's).

Two consequences:

- **Only an unrestricted library fallback can be shadowed.** Two printer-specific presets sharing an
  alias do not hide each other; overlapping lists for the same product trip the ambiguous-match error
  above instead.
- This is why adding `Generic PLA @<printer>` to a vendor silently removes the library
  `Generic PLA @System` from that printer. That is intended, and the reason a vendor tuning a generic
  must **keep the `Generic X` alias**.

The literal spelling `Generic <mat> @System` is load-bearing beyond shadowing: when a user preset, an
imported preset or a 3MF project names a parent that no longer resolves and contains `Generic`, the
loader rewrites the name into `Generic <mat> @System` and retries. Only the library ships those names,
so keep them.

## `filament_id`, `filament_vendor`, `filament_type`

`filament_id` is minted from the triple `(filament_vendor, filament_type, alias)`. `filament_vendor` and
`filament_type` are therefore **identity, not decoration**: editing either, or the alias, re-mints the
id. Read `docs/HLSD/filament_id.md` before changing any of them, and see [ids.md](ids.md) for the
tooling.

A filament with no resolvable `filament_id` anywhere in its `inherits` chain is a **hard load error**
that discards the vendor bundle. The id inherits across bundles, so a vendor's `Generic ABS @X`
inheriting `Generic ABS @System` gets the library's id for free; a vendor's own product must resolve its
own.

- `filament_type` **must be a JSON array**: the one vector key `check` rejects as a scalar outright. A
  scalar `"PP"` once hung the filament and printer selection UI.
- It is an **open** enum: an unlisted value is accepted silently and falls back to 190–300 °C defaults
  and adhesion 1.0. Prefer a value from `MaterialType::all()` in
  `src/libslic3r/MaterialType.cpp`, or add a row there.
- Generics use `filament_vendor: ["Generic"]`, which `fdm_filament_common` already defaults to.

## `"nil"`

`"nil"` is legal only in an option defined as nullable (`add_nullable`, or `nullable = true`, in
`src/libslic3r/PrintConfig.cpp`). Anywhere else it fails the file, and with it the **whole bundle**
(`Failed loading configuration file`, after `Deserializing nil into a non-nullable object` or
`Invalid value provided for parameter <key>: nil`). To leave a non-nullable key unset, omit it; do not
write `nil`.

In filament presets a minority of the `filament_*` keys are nullable, plus `long_retractions_when_ec`
and `retraction_distances_when_ec`. About half of them are the extruder overrides (`filament_retraction_length`,
`filament_z_hop`, `filament_wipe`, `filament_retract_*`, `filament_retraction_speed`,
`filament_deretraction_speed`, `filament_retraction_minimum_travel`, `filament_wipe_distance`,
`filament_long_retractions_when_cut`, `filament_retraction_distances_when_cut`, …), where `nil` means
*keep the printer's or extruder's own value*. The rest are ordinary nullable options
(`filament_flow_ratio`, `filament_flush_temp`, `filament_adaptive_volumetric_speed`, …), where it means
*unset*. Check the option's definition before writing `nil` anywhere else.

## Tuning per nozzle and per variant

Between a product's `@X` and `@X 0.N nozzle` tunes the keys that usually differ, most often first, are
`filament_max_volumetric_speed`, `filament_retraction_length`, `slow_down_min_speed`,
`filament_flow_ratio`, `slow_down_layer_time`, `nozzle_temperature` and `pressure_advance` (switched on
by `enable_pressure_advance`).

Use measured values for the material, hotend, extruder and nozzle combination. Neither maximum
volumetric speed nor pressure advance has a universal nozzle-only lookup table. When cloning a 0.4
preset for a 0.2 nozzle, explicitly revisit flow limits; do not infer a pressure-advance value, or a
required direction of change, from diameter alone.

On a printer with extruder variants, a filament tunes these per variant too:
`filament_max_volumetric_speed`, `filament_flow_ratio`, `nozzle_temperature` and the retraction
overrides carry one value per variant of `filament_extruder_variant` (Standard, High Flow, …). The
exact key set is [`filament_options_with_variant`](extruder-variants.md#the-four-key-sets);
`slow_down_min_speed` and `pressure_advance` are not in it. Keep every such array at exactly that width, even where the
setting does not differ per variant, and measure the High Flow variant rather than copying Standard
([extruder-variants.md](extruder-variants.md#filament)).

## Bed temperature is twelve keys, not one

There is no single "bed temperature". The plate type selected for the printer (`Cool Plate`,
`Engineering Plate`, `High Temp Plate`, `Textured PEI Plate`, `Textured Cool Plate`, `Supertack Plate`)
picks one of six keys, each with an `_initial_layer` twin: `cool_plate_temp`, `eng_plate_temp`,
`hot_plate_temp`, `textured_plate_temp`, `textured_cool_plate_temp` and `supertack_plate_temp`.

`textured_cool_plate_temp` is the one most often forgotten. A non-BBL printer with
`support_multi_bed_types` off hides the plate selector and uses the printer preset's `default_bed_type`
(High Temp Plate, `hot_plate_temp`, when unset or invalid), but a loaded project or a CLI config can
still carry another plate. So set every plate the printer plausibly has, as the sibling presets in the
bundle do.

## Style

- Overrides, not full copies: an instantiated filament preset carries around a dozen non-meta keys,
  and a library `@System` shim two or three. A preset that restates fifty-plus keys from its parent is
  the pattern to move away from, not to copy. Commit `6943b6ddc3` is the stated model for converting such presets: flip true
  bases to `instantiation: "false"`, strip their `compatible_printers`, `setting_id` and
  `filament_settings_id`, and add `renamed_from` on the surviving selectable preset. A value every
  printer tune of a product shares goes on the product's `@base`
  ([shared bases](shared-bases.md#levels)).
- Prefer the library's `fdm_filament_*` bases over a vendor-local copy; for a new preset, even in a
  bundle whose older presets use one: a local copy drifts from the library's.
- Canonical key order, written by `orca_profile_tool.py normalize` when it rewrites a file: `type`,
  `name`, `renamed_from`, `inherits`, `from`, `setting_id`, `filament_id`, `instantiation`, then
  everything else in the order you wrote it. Not enforced on its own: a file that leads with
  `compatible_printers` passes `check`.
- **Every vector-typed (`co…s`) key must be a JSON array.** Only a scalar `filament_type` is an outright
  error; `normalize` silently arrayifies five more (`filament_cost`, `filament_density`,
  `temperature_vitrification`, `filament_max_volumetric_speed`, `filament_vendor`), and `check` fails
  when it would. Every other vector key is on you, including `filament_start_gcode`,
  `filament_end_gcode`, `filament_extruder_variant`, `compatible_printers` and the plate temperatures.
