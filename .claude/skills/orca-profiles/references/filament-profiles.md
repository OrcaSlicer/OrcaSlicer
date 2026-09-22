# Filament profiles and OrcaFilamentLibrary

`OrcaFilamentLibrary` is the filament-only bundle the loader reads **first**; its config map
becomes the base bundle, so any vendor may inherit a library preset by name. It is the only cross-bundle
parent — vendor-to-vendor inheritance always fails.

## Where a filament goes

| Contribution | Location |
| --- | --- |
| Generic material for all printers | `OrcaFilamentLibrary/filament/Generic <mat> @System.json` |
| A brand's product, all printers | `OrcaFilamentLibrary/filament/<Brand>/` |
| A brand's tune for one printer | `OrcaFilamentLibrary/filament/<Brand>/<PrinterVendor>/` — recommended; `<PrinterVendor>/filament/<Brand>/` also works |
| A printer vendor's tune of a generic or its own product | `<Vendor>/filament/` |

Both locations for the last-but-one row are supported: `OrcaFilamentLibrary/filament/<Brand>/<PrinterVendor>/<Name>.json`
(the shape the wiki shows) and `<PrinterVendor>/filament/<Brand>/`. The library path is the one a
filament vendor should contribute to — `OrcaFilamentLibrary/filament/<Brand>/` is the brand's own
folder, while a printer vendor's folder belongs to that printer vendor. Brand tunes do ship under
printer vendors' folders today (Polymaker and SUNLU among others).

Library layout: `filament/base/fdm_filament_*.json` type roots, root-level `Generic <mat> @System.json`
generics, and one subfolder per brand, which may nest printer-specific tunes one level deeper. Adding
a brand means adding a folder here; the folder name is a directory label only — `filament_vendor` inside the JSON is the real vendor string.

## The three-part shape

```jsonc
// Fiberon PA6-CF @base.json  — the product root, holds identity + material values
{ "type": "filament", "name": "Fiberon PA6-CF @base", "from": "system",
  "instantiation": "false", "inherits": "fdm_filament_pa",
  "filament_id": "OFkOviHk",                 // generated here; variants inherit it
  "filament_vendor": ["Polymaker"], "filament_type": ["PA6-CF"], /* … */ }

// Fiberon PA6-CF @System.json — the selectable shim, 7 keys
{ "type": "filament", "name": "Fiberon PA6-CF @System", "from": "system",
  "instantiation": "true", "inherits": "Fiberon PA6-CF @base",
  "setting_id": "…", "compatible_printers": [] }

// <PrinterVendor>/filament/Polymaker/Fiberon PA6-CF @BBL X1C.json — a printer tune
{ … "inherits": "Fiberon PA6-CF @base", "filament_max_volumetric_speed": ["14"],
  "compatible_printers": ["Bambu Lab X1 Carbon 0.4 nozzle", …] }
```

- `@base` is the convention for a root. A base carries **no** `setting_id`, no `compatible_printers`, no
  `filament_settings_id`. Only the `setting_id` half is enforced, and nothing violates it; the other two
  are unchecked and plenty of bases still carry them. Do not copy that from a neighbouring file.
- Every `@System` must be `"instantiation": "true"`. DREMC ships `@System` presets set to `"false"`,
  which therefore ship but can never be selected; no check catches it.
- A duplicated brand `@base` across bundles is normal and intentional (`Fiberon PA6-CF @base` exists in
  both the library and BBL with the same id, differing only in MVS) — bases never enter the preset
  collection, so there is no duplicate-name error.
- You may inherit from an instantiated preset as well as from a base; it is common.

## Color is a runtime property

`filament_id` identifies a product, not a color; filament sync/AMS reads the color from the spool at
runtime. A product ships one all-printer preset and the color is chosen at runtime — never a sibling
preset that differs only by color. A material family (PLA vs PLA Matte vs PLA Silk) is a new product; a
color is not. A printer tune keeps the product alias and does not multiply per color either.

CI does not catch this — per-color presets pass `check` — so it is a review call.

## The two most common contributions

**A printer vendor tuning a generic.** Keep the `Generic X` base name so the alias shadows the library
preset on your printers, inherit `Generic X @System`, declare **no** `filament_id` (inheriting the
library's is correct — the product really is the library's generic), and give it a non-empty
`compatible_printers` in its own body:

```jsonc
// <Vendor>/filament/Generic PETG @Acme One 0.4 nozzle.json
{ "type": "filament", "name": "Generic PETG @Acme One 0.4 nozzle", "from": "system",
  "instantiation": "true", "inherits": "Generic PETG @System",
  "filament_flow_ratio": ["0.95"], "filament_max_volumetric_speed": ["10"],
  "compatible_printers": ["Acme One 0.4 nozzle"] }
```

**A printer vendor's own branded product.** Give it a `@base` root so `generate-id` can mint the id (see
[ids.md](ids.md) — inheriting `Generic X @System` directly makes the id unfixable by the tool), then one
instantiated leaf per printer in the same bundle. No `@System` shim: that is only for a product entering
OrcaFilamentLibrary.

```jsonc
// <Vendor>/filament/Acme Aura PETG @base.json      — instantiation false, no setting_id
{ "type": "filament", "name": "Acme Aura PETG @base", "from": "system",
  "instantiation": "false", "inherits": "fdm_filament_pet",
  "filament_vendor": ["Acme"], "filament_type": ["PETG"] }   // filament_id minted here

// <Vendor>/filament/Acme Aura PETG @Acme One 0.4 nozzle.json
{ "type": "filament", "name": "Acme Aura PETG @Acme One 0.4 nozzle", "from": "system",
  "instantiation": "true", "inherits": "Acme Aura PETG @base",
  "filament_max_volumetric_speed": ["11"],
  "compatible_printers": ["Acme One 0.4 nozzle"] }
```

Omit `filament_settings_id` from new presets — it is runtime bookkeeping the app rewrites to the preset
name.

## `compatible_printers`

- **Library fallbacks:** empty `[]` or absent, so they are offered on all printers except where
  [alias shadowing](#alias-shadowing) supplies a printer-specific tune.
- **Library printer-specific tunes:** non-empty, listing exact printer **variant** names. These can
  supersede a same-alias fallback just like a tune in a printer vendor's bundle.
- **Instantiated filaments in every other vendor:** non-empty, listing exact printer **variant** names.
  Enforced twice but not identically: the C++ `has_errors` reads the *flattened* config, so an inherited list satisfies it,
  while the Python check reads the file's **own** key. Write the list in the file itself. This is the
  most common filament CI failure.
- Emptying it to "make it apply everywhere" fails that check *and* creates a duplicate-`filament_id`
  collision against the library generic on every printer.
- Copying a base's full printer list onto a nozzle-specific variant produces duplicate combobox entries —
  a real shipped bug twice over.

## Overlapping coverage: one variant, one profile per product

`filament_id` is the **product** key, not the preset key — every variant of one product shares it
(`<filament_vendor>/<filament_type>/<name-before-@>`). So if one printer variant appears in the
`compatible_printers` of two presets of that product, the slicer cannot tell them apart at AMS match time.
The C++ validator reports `Ambiguous AMS filament match: N presets share filament_id "X" … printer "Y"`.
`orca_profile_tool.py check` does **not** see it and passes. Resolve the overlap by **specificity**: keep
the variant on the most specific profile and remove it from every more general one. Deleting a profile is
the least preferred fix — moving coverage keeps the tune that users rely on.

Judge specificity from the profile's `compatible_printers` — how many variants it actually covers — and
use the name only as a secondary, easily-vague hint; decide by the lists, with a best judgement call on
the name. Naming conventions differ by vendor: BBL's is the reference (`@<Vendor> <Model>` for a whole
model, `@<Vendor> <Model> <nozzle> nozzle` for one variant, `@<Vendor>` for a vendor-wide generic), but
others vary (`@<printer model>`, a printer serial, or Creality's `@<Model>-all`). A name never overrides
the list — see [preset naming](naming.md) for the shapes.

Specificity, most to least:

1. **Variant-specialized** — lists a single printer variant (BBL-style
   `... @<Vendor> <Model> <nozzle> nozzle`).
2. **Model-specialized** — lists the variants of one printer model (BBL-style `... @<Vendor> <Model>`).
   It should cover every variant of its model, not only the nozzle it was authored for.
3. **Family / series** — lists variants spanning a printer family or series.
4. **Generic / catch-all** — vendor-wide, covering many unrelated models (often the bare
   `Generic <mat> @<Vendor>`).

Rules:

- A model-specialized profile is extended to **all** variants of its model, and each variant it thereby
  starts covering is removed from the family and generic profiles that also listed it — including variants
  that had no overlap before. Apply it per nozzle, not just 0.4.
- Apply it **per product**: trim only the material that has a specialized profile from the generic; a
  material whose product has no specialized profile keeps the variant in the generic.
- Never strip coverage a variant has nowhere else to get. If a variant has no variant-level specialized
  profile, the next level down keeps it; when the model has specialized profiles, the model-level one wins
  over the family/generic.
- Moving coverage is preferred over deleting. If a profile must be deleted, remove the more general
  one, not the specialized profile that carries the tune.
- After moving coverage, repoint the affected `default_filament_profile` (machine) and clean the model's
  `default_materials`: they should name the most specific profile that covers the variant, and should not
  keep generic entries that no longer cover the model. This rule applies equally when adding or fixing
  defaults.

Multiple profiles of one product with **disjoint** `compatible_printers` is the intended end state.
Adding coverage to the specialized profile and removing it from the generic is the preferred direction.

**Detection caveat:** `orca_profile_tool.py check` is blind to this; only the C++ validator behind the full
`./scripts/check_profile.sh` reports it (`validate_system`). Always confirm with that, not the vendor-scoped
loop.

## Alias shadowing

A printer-specific filament in either the library or a vendor bundle supersedes the library fallback
on the printers it lists. The matching key is the **alias**: the preset name up to the **first** `@`,
right-trimmed (no `@` → the whole name). So
`QIDI ABS-GF@Q2-Series` aliases to `QIDI ABS-GF`.

A library preset with an empty `compatible_printers` collects, into `m_excluded_from`, every printer named
by any same-alias preset that *has* a non-empty list, and is then hidden on those printers.

Two consequences:

- **Only an unrestricted library fallback can be shadowed.** Two printer-specific presets sharing
  an alias do not exclude each other — overlapping lists for the same product trip the
  duplicate-`filament_id` check instead.
- This is why adding `Generic PLA @<printer>` to a vendor silently removes the library `Generic PLA`
  from that printer. Intended — and the reason a vendor tuning a generic must **keep the `Generic X`
  base name**.

The literal spelling `Generic <mat> @System` is load-bearing beyond shadowing: `find_preset2` rewrites an
unresolved name containing "Generic" into that form and retries against the library, which is how 3MF
and project recovery works.

## `filament_id`, `filament_vendor`, `filament_type`

`filament_id` is minted from the triple `(filament_vendor, filament_type, name-before-first-@)`.
`filament_vendor` and `filament_type` are therefore **identity, not decoration** — editing either
re-mints the id. Read `docs/HLSD/filament_id.md` before changing any of them, and see
[ids.md](ids.md) for the tooling.

A filament with no resolvable `filament_id` anywhere in its `inherits` chain is a **hard load error** that
discards the vendor bundle. The id inherits across bundles, so a vendor's `Generic ABS @X` inheriting
`Generic ABS @System` gets the library's id for free; a vendor's own product must resolve its own.

- `filament_type` **must be a JSON array** — the one vector key the Python check enforces. A scalar
  `"PP"` once hung the filament/printer selection UI.
- It is an **open** enum: an unlisted value is accepted silently and falls back to 190–300 °C defaults
  and adhesion 1.0. Off-list values do ship. Prefer a value from `MaterialType::all()` in
  `src/libslic3r/MaterialType.cpp`, or add a row there.
- Generics use `filament_vendor: ["Generic"]`, which `fdm_filament_common` already defaults to.

## `"nil"`

Legal in any key whose `ConfigOptionDef` is `nullable`. In a filament preset that is most of the
`filament_*` family, plus `long_retractions_when_ec` and `retraction_distances_when_ec`. About half are
the extruder overrides (`filament_retraction_length`, `filament_z_hop`, `filament_wipe`,
`filament_retract_*`, `filament_retraction_speed`, `filament_deretraction_speed`,
`filament_retraction_minimum_travel`, `filament_wipe_distance`, `filament_long_retractions_when_cut`,
`filament_retraction_distances_when_cut`, …), where `nil` means *keep the printer/extruder's own value*.
The rest are ordinary nullable options (`filament_flow_ratio`, `filament_flush_temp`,
`filament_adaptive_volumetric_speed`, …) where it means *unset*.

Anywhere else it throws `Deserializing nil into a non-nullable object`. To not set a non-nullable key,
omit it — do not write `nil`.

## What to review per nozzle

Across `@X` / `@X 0.N nozzle` sibling pairs the keys that differ, most often first, are
`filament_max_volumetric_speed`, `filament_retraction_length`, `slow_down_min_speed`,
`filament_flow_ratio`, `slow_down_layer_time`, `nozzle_temperature` and `pressure_advance`.
`filament_cost`, `filament_density`, `filament_type` and `filament_vendor` belong on the `@base` and
should not appear in a printer tune.

Use measured values for the material, hotend, extruder and nozzle combination. Neither maximum
volumetric speed nor pressure advance has a universal nozzle-only lookup table. When cloning a
0.4 preset for a 0.2 nozzle, explicitly revisit flow limits; do not infer a pressure-advance value
or a required direction of change from diameter alone.

## Style

Overrides, not full copies: a typical instantiated filament preset carries around a dozen non-meta keys,
and a library leaf two or three. Presets that restate fifty-plus keys from their parent do still ship —
Phrozen's single filament preset is that style — but they are the pattern to move away from, not to
copy. Commit `6943b6ddc3` is the stated model (flip true bases to `instantiation: "false"`, strip
`compatible_printers`/`setting_id`/`filament_settings_id`, add `renamed_from` on the survivor).

Prefer the library's `fdm_filament_*` bases over a vendor-local copy. Phrozen's local
`fdm_filament_common` has drifted from the library's.

Canonical key order, written by `orca_profile_tool.py normalize` when it rewrites a file: `type`, `name`,
`renamed_from`, `inherits`, `from`, `setting_id`, `filament_id`, `instantiation`, then everything else in
the order you wrote it. Not enforced — a file that leads with `compatible_printers` passes `check`.

**Every vector-typed (`co*s`) key must be a JSON array.** Only `filament_type` is an outright error, but
`normalize` silently arrayifies five more (`filament_cost`, `filament_density`,
`temperature_vitrification`, `filament_max_volumetric_speed`, `filament_vendor`) and `check` fails when
it would. Every other vector key is on you — including `filament_start_gcode`, `filament_end_gcode`,
`filament_extruder_variant`, `compatible_printers` and the plate temperatures.

## Bed temperature is twelve keys, not one

There is no single "bed temperature". Which plate key applies depends on `curr_bed_type`, whose six
selectable values (`btPC`, `btEP`, `btPEI`, `btPTE`, `btPCT`, `btSuperTack`; `btDefault` maps to no key)
`get_bed_temp_key()` turns into `cool_plate_temp`, `eng_plate_temp`, `hot_plate_temp`,
`textured_plate_temp`, `textured_cool_plate_temp` and `supertack_plate_temp` — each with an
`*_initial_layer` twin.

`textured_cool_plate_temp` is the one most often forgotten. A printer with `support_multi_bed_types` off
hides the selector, and the printer preset's
`default_bed_type` decides which plate is selected for it, but `curr_bed_type` can still hold a stale
value carried over from another printer — so set every plate the printer plausibly has, as the sibling
presets in the bundle do.
