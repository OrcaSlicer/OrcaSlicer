# Extruder variants

Use this when a printer's hotend or extruder can be in more than one hardware configuration that
needs different settings: a Standard and a High Flow nozzle, a TPU High Flow, E3D High Flow or Extra High Flow nozzle, or
a Direct Drive and a Bowden extruder on one machine. Variants let one printer, process and filament preset carry a
separate value per configuration; the user picks the configuration in the sidebar, and slicing uses
the matching values. They are not for nozzle **diameter**: that stays one `machine` preset per
`printer_variant`.

## Variant strings

- A **variant string** is `"<extruder_type> <nozzle volume type>"`: the extruder's `extruder_type`
  value, a space, and its nozzle volume type (the project's `nozzle_volume_type`, seeded by the
  printer preset's `default_nozzle_volume_type`), e.g. `"Direct Drive High Flow"`.
- A **variant** is one entry of a preset's variant list, named by its variant string (plus an extruder
  id for printer and process lists). A variant-aware key holds **one value per variant** (a
  (normal, silent) pair for the `machine_max_*` limits), and each preset declares its variants in
  that list. Slicing picks, for each extruder, the variant whose
  variant string equals the current `extruder_type` + nozzle volume type.
- Matching is an **exact string compare** of the whole string (plus the extruder id for printer and
  process lists) against the preset's own resolved list, in any order. With no match the **first
  variant** is used, silently: variant index 0 for printer and process keys (extruder 1's first
  variant, whichever extruder asks), the filament's own first variant for filament keys. Printer and
  process keys are matched only on a printer with several extruders or whose
  `extruder_variant_list` offers several variant strings (`support_different_extruders`); on any
  other printer their variant index 0 is read whatever it names. Nothing rejects a profile for a
  variant mismatch; mistakes surface only as wrong values in the G-code.
- Variant index and array length follow [Widths](#widths).

The complete enum is `s_keys_map_ExtruderType` and `s_keys_map_NozzleVolumeType` in
`src/libslic3r/PrintConfig.cpp` (`grep -A5 s_keys_map_NozzleVolumeType` there to confirm):

| Part | Values |
| --- | --- |
| Extruder type | `Direct Drive`, `Bowden` |
| Nozzle volume type | `Standard`, `High Flow`, `TPU High Flow`, `E3D High Flow`, `Extra High Flow` (`Hybrid` exists but is runtime-only) |

So the ten legal variant strings are the two extruder types × the five writable nozzle volume types,
and this table is the whole test of legality: a string's presence in a shipped profile is no evidence
for it. `Hybrid` (an extruder with several sub-nozzles) is never a variant: a filament on it reads the
variant of its own nozzle volume type from the project's `filament_volume_map`, printer and process
keys get one variant per nozzle volume type the extruder holds (`extruder_nozzle_stats`), and any
other lookup reads `Standard`. Never write it in a variant string. The same per-filament and
per-type reading applies on any extruder once `extruder_nozzle_stats` lists more nozzle volume
types than there are extruders.

Every other string (a nozzle volume type name from another slicer, a typo, a variant copied from a
shipped profile) is a **dead variant**: nothing selects it, and since lookup is by string it does not
shift the variants beside it; it still counts toward the variant length when arrays are sized.
`orca_profile_tool.py check` reports it as an error in every bundle, BBL included
([variant names](validation.md#variant-names)). Legacy names are errors too: in the four variant
lists, `default_nozzle_volume_type` and `nozzle_volume_type`, the loader still rewrites `Normal` →
`Standard` and `Big Traffic` → `High Flow`, so a ported `Direct Drive Normal` variant would load, but
`check` rejects the spelling and names the enum name to write. `DirectDrive` is rewritten only in
`extruder_type`, so `DirectDrive Standard` in a variant list is dead. Write the enum names;
`normalize` does not convert them.

### A nozzle the enum does not name

The nozzle volume types are fixed by the engine, and a profile cannot add one. A nozzle the table does
not name needs the type added in code first, which is outside profile work; until then any string
for it is a dead variant. Once the engine has the type, its variant string is the enum name after
the extruder type, e.g. `"Direct Drive <name>"`, and `check` accepts it with no tool change, since it
reads the enum from `PrintConfig.cpp`. Existing arrays are unaffected
([slice time](#slice-time-and-existing-users)).

## When to use variants

| Hardware | Do |
| --- | --- |
| One extruder, one nozzle type | Nothing. Without a variant list the printer has one variant ([variant length](#widths)) and no printer-key lookup takes place; a filament with several variants still gets the one for the printer's variant string. |
| Bowden-only printer | Nothing either. A list-less printer matches no string, so every printer-key lookup reads variant index 0 whether that variant is called `"Bowden Standard"` or `"Direct Drive Standard"`; naming it is cosmetic while it is the printer's only variant. A multi-extruder Bowden printer that declares the layout writes `"Bowden Standard"` variants: `"Direct Drive Standard"` entries match none of its extruders, so every extruder reads variant index 0 (and `check` reports them under [Printer rule 3](#printer-machine)). A filament with a `"Bowden Standard"` variant does get that variant there. |
| Nozzle types the user swaps (Standard / High Flow / TPU High Flow / E3D High Flow / Extra High Flow) | The printer lists them as variants; tune the keys that really differ per nozzle volume type. |
| Extruders of different types on one machine | One `extruder_type` per extruder, each extruder listing its own variants. |
| Several extruders that need different values in a variant key (retraction, z-hop, `nozzle_volume`, the `machine_max_*` limits) | Declare the layout even with a single nozzle volume type: `extruder_variant_list` with one `"<type> Standard"` per extruder, and the flattened pair. Without it the arrays still hold one value per extruder ([variant length](#widths)), but the loader keeps only extruder 1's value of a list-less printer's arrays, so every extruder prints with it. |

A preset without variant keys keeps working on a variant printer: its single variant is applied to
every extruder. So adding variants to a printer does not break existing processes or library
filaments; it only makes per-variant tuning possible.

## Printer (`machine`)

```json
"extruder_type":              ["Direct Drive", "Direct Drive"],
"extruder_variant_list":      ["Direct Drive Standard,Direct Drive High Flow",
                               "Direct Drive Standard,Direct Drive High Flow,Direct Drive TPU High Flow"],
"printer_extruder_id":        ["1", "1", "2", "2", "2"],
"printer_extruder_variant":   ["Direct Drive Standard", "Direct Drive High Flow",
                               "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive TPU High Flow"],
"default_nozzle_volume_type": ["Standard", "Standard"]
```

Rules:

1. `extruder_variant_list` has **one entry per extruder**; each entry is the `,`-joined variants that
   extruder supports. It is the per-extruder menu the sidebar offers. It is in no
   [variant set](#the-four-key-sets), so the variant-length resize leaves it alone. Extruder 1's
   first variant, variant index 0 of the flattened pair, is the fallback of every extruder whose
   variant is missing when the arrays are collapsed for slicing
   ([slice time](#slice-time-and-existing-users)); it is not necessarily the configuration the user
   sees, which is `default_nozzle_volume_type` (rule 4).
2. `printer_extruder_variant` is that list **flattened** extruder-major, one entry per variant, and
   `printer_extruder_id` gives each entry its 1-based extruder. These two size and address every
   variant. Write all three keys and keep them in agreement. At load with
   `single_extruder_multi_material` off, and in the app when the printer tab loads a printer with a
   different number of extruders, the pair is rebuilt from `extruder_variant_list` (one
   `Direct Drive Standard` per extruder when the list is absent) and the variant arrays are resized to
   the rebuilt pair, padded with their first value or cut. The `machine_max_*` limits are padded the
   same way, so extruder 2 and up of a list-less printer take extruder 1's normal limit as their
   silent one too. The resize skips `hotend_heating_rate` / `hotend_cooling_rate`: they keep their
   width, and an extruder beyond it reads their first value. With the three in agreement that changes
   nothing; a pair written without the list is replaced. A listed variant the pair lacks is a menu
   choice that reads variant index 0.
   - The pair without `extruder_variant_list` slices, but the sidebar offers no variant switch and
     the app cannot add variants to a list-less process: nothing is lost while every extruder
     has exactly one variant, and every further variant is unreachable.
   - A missing or one-value `printer_extruder_id` is extruder 1 at every index
     ([the id trap](#padding-truncation-and-composition)) unless the load-time rebuild above
     replaces the pair (`single_extruder_multi_material` off).
   - `check` reports against this rule ([variant arrays](validation.md#variant-arrays)). Errors: a
     pair that is not the flattening, an id array that does not give each entry its extruder, a list
     of more than one variant without the pair, and a pair without the list that puts several variants
     on one extruder. A pair without the list and one variant per extruder is a warning where the
     load-time rebuild would replace it (`single_extruder_multi_material` off, and a pair other than
     one `Direct Drive Standard` per extruder), and passes otherwise.
3. Every variant string in an entry must start with that extruder's `extruder_type`.
4. `default_nozzle_volume_type` has one value per extruder and must name a nozzle volume type that
   extruder's variants list; it seeds the sidebar. The live choice is `nozzle_volume_type` in the
   project config, never a preset key.
5. Size every array by the set its key belongs to (the three sets are listed in full in
   [The four key sets](#the-four-key-sets); the lengths are worked through in the
   [sizing equation](#sizing-equation)): exactly the length below in each selectable preset that
   writes it, or leave the key out and the preset takes what reaches it, the default or its base's
   array. One value is no
   shorthand for "the same for every variant"; write the value for every variant:

| Set | Length | Keys |
| --- | --- | --- |
| `printer_extruder_options` | extruders (`E`) | the 8 per-extruder keys outside the variant scheme (`extruder_type`, `nozzle_diameter`, `default_nozzle_volume_type`, …) |
| `printer_options_with_variant_1` | variant length (`S`) | the full list below; not guessable from names |
| `printer_options_with_variant_2` | 2 × variant length | the 16 `machine_max_*` limits, at [stride 2](#widths): a (normal, silent) pair per variant |

Put the variant layout on the shared base of all printers that share the hardware, and let the
nozzle-diameter siblings inherit it, restating only the variant arrays whose values change. `check`
does not judge a base on its own; its arrays count where they reach a selectable preset, under
`check --strict`. The loader stores a base at its **own** `printer_extruder_variant`, one variant
when it writes none whatever its extruder count, and cuts a wider array to its first values before
any child inherits it; so a multi-extruder base whose extruders need different values declares the
layout itself ([composition](#padding-truncation-and-composition)). When a variant array can move from
the presets to a base is in [shared-bases.md](shared-bases.md#variant-arrays-on-a-base).

## Process

1. `print_extruder_variant` + `print_extruder_id` list the (extruder id, variant string) pairs of the
   process's variants. A variant is found by that pair, never by position: what is **required** is
   that every pair a compatible printer (by list or condition) can select is present, in any order. A
   missing pair reads the process's variant index 0 for that extruder, an extra pair is a variant
   nothing selects, and neither is reported. The exception is a single-extruder printer whose
   `extruder_variant_list` offers one variant string: no pair is matched there and variant index 0
   is read, so a process shared with such a printer lists that printer's pair first. **Mirroring** the printer's `printer_extruder_variant` +
   `printer_extruder_id` entry for entry is the convention; follow it, so the arrays compare by eye,
   but a different order with every pair present is a nit, not a defect. A process shared by printers
   whose pairs differ falls back to variant index 0 on the pairs it lacks; give each layout its own base.
2. Every key in `print_options_with_variant` ([the full list](#the-four-key-sets), which is not
   "every speed") has exactly one value per variant, or is left out. `print_extruder_id` needs its
   value per variant too: one value pads to extruder 1 everywhere. `check` holds a
   `print_extruder_id` that reaches the preset, written or inherited, to one entry per variant on any
   printer, without `--strict`, and warns when it is absent and a variant repeats.
3. Only add process variants if speeds or accelerations really differ per nozzle volume type or
   extruder. Otherwise omit the variant keys: a one-variant process needs no list, because match and
   no-match both read variant index 0, and that variant is copied to every extruder at slice time.
4. Put the lists on the process base for that printer layout, so leaves stay small.

## Filament

```json
"filament_extruder_variant":     ["Direct Drive Standard", "Direct Drive High Flow"],
"filament_max_volumetric_speed": ["21", "29"],
"filament_flow_ratio":           ["0.98", "0.98"],
"filament_retraction_length":    ["nil", "0.4"]
```

1. `filament_extruder_variant` lists variants **without extruder ids**: a filament's High Flow variant
   is used on whichever extruder is in High Flow. Its entries must be distinct, since the lookup
   returns the first equal string and a repeated entry is a variant nothing selects. There is no
   filament id key: `filament_extruder_id` exists only as a G-code placeholder, not as a filament
   option (its option and its set entry are commented out in `PrintConfig.cpp`), so a filament file that writes it loses the key as unknown.
   `filament_extruder_compatibility` is unrelated to variants (it says which extruders the filament
   may be loaded into).
2. Every key in `filament_options_with_variant` ([the full list](#the-four-key-sets)) that reaches the
   preset, whether written, included or inherited, is resized to its variant count: write it at exactly
   that width, or leave it out. Keys outside the set
   (`filament_type`, plate temperatures, `fan_max_speed`, `slow_down_min_speed`, …) are never addressed by variant index; the preset contributes their first value however wide a
   file writes them.
3. Cover every variant the material is meant to print on across its `compatible_printers`. Leave out
   a variant deliberately when the material should not be tuned for it (e.g. a TPU High Flow variant
   only on TPU filaments); an extruder reporting that variant string then reads the first variant.
4. Order the variants like the printer's, Standard first, so the first-variant fallback is the
   conservative one.
5. Tune what really differs: `filament_max_volumetric_speed` is the usual difference between nozzle
   volume types, then flow ratio, temperature and retraction. Use measured values; never copy the
   Standard value into the High Flow variant and call it tuned.
6. `nil` is legal per variant in the nullable override keys, occupies one entry like any value, and
   keeps the printer's value for that variant only.
7. Declare a multi-variant list with the arrays it sizes: on the filament, in its own file or in a
   template it pulls in with [`include`](vendor-bundle.md#inherits-and-include). The filament may be
   compatible with printers that list fewer variants, or none: each printer takes the filament's variant
   for its own variant string, else the filament's first variant. A one-variant list on a shared base
   is harmless, because one variant is the width a list-less preset has anyway; and a list-less base is
   stored at that width, so a wider array on it reaches its children as its first value only.
   A key such a base writes reaches a multi-variant child as one value, which the loader spreads over
   the child's variants; `check --strict` reports it at the child, which restates it at its own width.

## Widths

Every variant key is a flat array addressed by **variant index**: index `n` belongs to entry `n` of
the preset's own variant list (`printer_extruder_variant`, `print_extruder_variant` or
`filament_extruder_variant`). The index is found by exact compare of the string
`"<extruder_type> <nozzle volume type>"` (plus the 1-based extruder id for printer and process lists),
and the value is read at `index × stride`:

| Stride | Keys | Layout |
| --- | --- | --- |
| 1 | `printer_options_with_variant_1`, `print_options_with_variant`, `filament_options_with_variant` | `[variant 0, variant 1, …]` |
| 2 | `printer_options_with_variant_2` (the `machine_max_*` limits) | `[variant 0 normal, variant 0 silent, variant 1 normal, variant 1 silent, …]` |

So a variant array is `variant length × stride` long. The **variant length** is the number of
entries in the preset's variant list, counted on the preset's config **after** `include` and
`inherits` are applied, dead variants included. An inherited array arrives already resized to the
base's own variant length, an included one at the width its file wrote
([composition](#padding-truncation-and-composition)):

| Preset | Variant length |
| --- | --- |
| `machine` | `len(printer_extruder_variant)`; without it, the variants `extruder_variant_list` offers; without both, **one per extruder** (`len(nozzle_diameter)`), since the list's default is one `Direct Drive Standard` per extruder. `extruders_count` is a printer-tab field, not a preset key. The loader honours that default only halfway for a system preset: it sizes the composed preset by the one-entry default `printer_extruder_variant`, cutting every variant array to its first value, and only then (with `single_extruder_multi_material` off) rebuilds the pair to one variant per extruder ([Printer rule 2](#printer-machine)) and pads the arrays with that value. The rule's width is still one per extruder; to give extruders different values, declare the layout. |
| `process` | `len(print_extruder_variant)`; without one, 1 |
| `filament` | `len(filament_extruder_variant)`; without one, 1 |

The per-extruder keys in `printer_extruder_options` ([listed with the sets](#the-four-key-sets)) are
outside this scheme and stay one value per extruder. Keys outside [the four sets](#the-four-key-sets)
are never variant-resized or addressed by variant index, however wide a shipped file writes them; a
per-extruder vector holds `len(nozzle_diameter)` values, and a shorter one acts as padded with its
first value ([machine-profiles.md](machine-profiles.md#multi-extruder-idex-and-tool-changers)).

### Sizing equation

For a key in one of the four variant sets:

```
E    = extruders                        = len(nozzle_diameter) = len(extruder_type)
                                        = len(default_nozzle_volume_type) = len(extruder_variant_list)
V_i  = variants listed for extruder i   = ","-separated entries of extruder_variant_list[i],
                                          each "<extruder_type[i]> <nozzle volume type>"
S    = variant length                   = V_1 + V_2 + … + V_E
                                        = len(printer_extruder_variant) = len(printer_extruder_id)
k    = stride                           = 2 for printer_options_with_variant_2, else 1

N    = values the key holds, one per variant:
       machine  (printer_options_with_variant_1, _2) = S × k
       process  (print_options_with_variant)         = len(print_extruder_variant) = len(print_extruder_id)
       filament (filament_options_with_variant)      = len(filament_extruder_variant)
values[s × k + m]                       = variant index s, mode m   (m = 0 normal, m = 1 silent; only m = 0 at stride 1)
variant index s                         = (printer_extruder_id[s], printer_extruder_variant[s])
```

`printer_extruder_variant` is not sized by the equation; it defines `S`: it is `extruder_variant_list`
flattened extruder by extruder, and `printer_extruder_id[s]` is the extruder that index `s` came from.
A process that mirrors the printer's pairs, as [Process rule 1](#process) asks, has `N = S`; a
filament lists each variant string it is tuned for once, with no extruder id, so its `N` is its own
and independent of any one printer: it serves every printer in its `compatible_printers`, and a
variant string no extruder of a printer reports is simply never read there (a two-extruder printer
with `S = 7` serves a filament whose `N` is 3, and the filament keeps its 3 on a printer that offers
two variant strings). The
pair is what the loader reads, so the equality with the sum holds when the three keys agree, as
[Printer rule 2](#printer-machine) requires.

A selectable preset that writes a variant key writes it at its own `N`; any other width, one value
included, is an error. A preset that leaves a key out takes what reaches it, the default or an array
it inherits or includes, which the loader resizes to the preset's `N`; `check --strict` holds that
array to the preset's `N` too. A base is not judged on its own: its arrays count only where they reach
a preset that does not override them. BBL is the model for strict: every printer-specific machine,
process and filament declares its layout and restates every variant key at its own `N`, even where all
the values are the same, keys Orca added to the sets included.
Without the lists, `extruder_variant_list` defaults to one
`Direct Drive Standard` per extruder, so a machine has `V_i = 1` and `S = E`, and a process or filament
has `N = 1`; the loader cuts a list-less machine to its first variant and, with
`single_extruder_multi_material` off, widens it again with that value ([variant length](#widths)). The per-extruder keys outside the sets
(`printer_extruder_options` plus `extruder_offset` and `extruder_colour`) and `extruder_variant_list`
itself hold `E` values.

### Sizing examples

**One extruder, two nozzle volume types**: `E = 1`, `V_1 = 2`, so `S = 2`; stride-1 keys hold 2
values, `machine_max_*` hold 4:

```json
"nozzle_diameter":            ["0.4"],
"extruder_type":              ["Direct Drive"],
"extruder_variant_list":      ["Direct Drive Standard,Direct Drive High Flow"],
"printer_extruder_id":        ["1", "1"],
"printer_extruder_variant":   ["Direct Drive Standard", "Direct Drive High Flow"],
"default_nozzle_volume_type": ["Standard"],
"retraction_length":          ["0.8", "1.0"],
"machine_max_speed_x":        ["500", "200", "600", "250"]
```

The matching process lists the same two pairs (`print_extruder_id` `["1", "1"]`,
`print_extruder_variant` as above) and holds 2 values per key, e.g. `outer_wall_speed`
`["200", "260"]`; a filament for it lists `["Direct Drive Standard", "Direct Drive High Flow"]` and
holds 2 values per key, e.g. `filament_max_volumetric_speed` `["16", "24"]`.

**Four extruders, one nozzle volume type each** (a tool changer): `E = 4`, every `V_i = 1`, so
`S = 4`; stride-1 keys hold 4 values, `machine_max_*` hold 8:

```json
"nozzle_diameter":            ["0.4", "0.4", "0.6", "0.4"],
"extruder_type":              ["Direct Drive", "Direct Drive", "Direct Drive", "Direct Drive"],
"extruder_variant_list":      ["Direct Drive Standard", "Direct Drive Standard",
                               "Direct Drive Standard", "Direct Drive Standard"],
"printer_extruder_id":        ["1", "2", "3", "4"],
"printer_extruder_variant":   ["Direct Drive Standard", "Direct Drive Standard",
                               "Direct Drive Standard", "Direct Drive Standard"],
"default_nozzle_volume_type": ["Standard", "Standard", "Standard", "Standard"],
"retraction_length":          ["0.8", "0.8", "1.2", "0.8"],
"machine_max_speed_x":        ["500", "200", "500", "200", "500", "200", "500", "200"]
```

The (normal, silent) pair is repeated per extruder: `["500", "200"]` would be width 2 against
`S × k = 8` (the loader pads it to `500, 200, 500, 500, …`) and `["500"]` width 1; both are errors. The
process mirrors the four pairs (`print_extruder_id` `["1", "2", "3", "4"]`) with 4 values per key, or
omits the variant keys altogether when nothing differs per extruder (then one value per key). A
filament for it lists only `["Direct Drive Standard"]`: one entry, so one value per key. Drop the
layout from this printer and the widths stay the same, since the default list gives `S = E = 4`; but
the loader then keeps only the first variant ([variant length](#widths)): `retraction_length`
becomes `0.8` on every extruder and extruder 3 loses its `1.2`.

### Adding a variant inserts its values at its variant index

Indexes run extruder-major: extruder 1's variants in `extruder_variant_list` order, then extruder
2's. A new variant's values go in at its index, not at the end. Giving extruder 1 of a two-extruder
printer a High Flow option, when only extruder 2 had one:

| Key | Before | After |
| --- | --- | --- |
| `extruder_variant_list` | `["Direct Drive Standard", "Direct Drive Standard,Direct Drive High Flow"]` | `["Direct Drive Standard,Direct Drive High Flow", "Direct Drive Standard,Direct Drive High Flow"]` |
| `printer_extruder_id` | `["1", "2", "2"]` | `["1", "1", "2", "2"]` |
| `printer_extruder_variant` | `[Standard, Standard, High Flow]` | `[Standard, High Flow, Standard, High Flow]` (full strings in the file) |
| `retraction_length` (stride 1) | `["0.8", "1.0", "1.2"]` | `["0.8", "?", "1.0", "1.2"]`: one value at index 1 |
| `machine_max_speed_x` (stride 2) | `["500", "200", "600", "250", "700", "300"]` | `["500", "200", "?", "?", "600", "250", "700", "300"]`: a (normal, silent) pair at position 2 |
| `print_extruder_id` / `print_extruder_variant` / `outer_wall_speed` | mirror the printer | the same insertion at index 1 |
| `filament_extruder_variant` `[Standard, High Flow]` | — | unchanged: filament variants carry no extruder id, so the existing High Flow variant now serves both extruders |

Every `?` is a measured value for that nozzle, on the machine limits as much as on retraction.
Removing or renaming a variant shifts the later values the same way in reverse; a variant string that
no longer matches the enum is simply a variant nothing selects.

### Padding, truncation and composition

**Every key in the set widens, whether or not a file restates it.** At load each variant key of the
composed config is resized to the length of the preset's own `*_extruder_variant` (the one-entry
default when it writes none) × stride: a short array is **padded by repeating its first value**, a
long one is truncated to its first values, without a word from the loader or the validator. The rule
does not follow from this padding: a file writes a variant key at **exactly `variant length × stride`**
or not at all. Any other length, one value included (which the loader spreads over every variant, at
stride 2 over normal *and* silent alike), is a mistake the loader hides and
`orca_profile_tool.py check` reports as an error in the selectable preset that writes it, even when
every value is the same; `check --strict` also reports an array that reaches a selectable preset at
another width
([variant arrays](validation.md#variant-arrays)).

The loader sizes a list-less preset of any type to one variant at this step, a list-less machine
included: a two-extruder machine without a layout that writes `retraction_length` `["0.8", "0.9"]`, the
width the equation asks for, stores `["0.8"]`, which the pair rebuild and the slice-time collapse hand
to both extruders. A 3-value array on a preset of variant length 2 keeps its first two; at length 4 it
becomes `[a, b, c, a]`.

Composition hands down widths in two ways:

- **`inherits` hands down the parent's resized arrays.** A base is stored after its own resize, at
  the length of its own `*_extruder_variant` (one variant for a base of any type that writes none,
  whatever its extruder count), so an array wider than the base's list is cut to its first values
  before any child sees it, and a child that adds variants gets those first values padded. Widen an
  array only on a preset whose own resolved list already has the entries. `check` judges selectable
  presets only, at the width each file wrote: an array a base's own resize cuts is not seen (a review
  item), and an inherited array of another width than a child's list is left to the loader's resize
  unless `check --strict`, which asks the child to restate it at its own width.
- **`include` hands down the template's diff at its pre-resize width.** The template contributes every
  key where its composed config differs from the built-in defaults, taken before its own resize, so the
  arrays it writes arrive at the width its file wrote, and the includer's own list sizes them. A key the
  template sets to the built-in default is not passed on
  ([`include`](vendor-bundle.md#inherits-and-include)).

The id keys are the trap in this padding: `printer_extruder_id` and `print_extruder_id` are members
with default `[1]`, so a missing or one-value id array beside a longer variant list is padded to
extruder 1 at every index. That is right on a single-extruder printer and wrong on a multi-extruder
one, where every variant is then addressed as extruder 1's. A machine escapes it only where the
load-time pair rebuild of [Printer rule 2](#printer-machine) runs (`single_extruder_multi_material`
off); a process's `print_extruder_id` is never rebuilt at load. `check`
holds an id array that reaches a selectable preset, written or inherited, to one entry per variant on
any printer; `fix-variant` never pads an id array or a variant list, since those address the
variants rather than fill them.

Consequences:

- A base that gains a variant silently pads every descendant that restates a variant array at the old
  width, and a short `machine_max_*` array copies variant 0's *normal* limit into the silent entries
  too; a descendant that restates nothing inherits the widened array and needs no edit. Extend, in one
  change: the printer base and each nozzle-diameter sibling that restates a variant array, the process
  bases that mirror the printer's variants, and the filaments that should cover the variant.
- A one-value override is reported, and the loader spreads it over **all** variants, overwriting the
  ones that should differ.
- A process or filament that omits the new variant is not padded; the variant resolves to its index 0.

### Slice time and existing users

**Slice time collapses variants to extruders.** When printer, process and filaments are combined on a
printer with several extruders or several variant strings, each printer and process variant key is
re-gathered to one entry per extruder (× stride) in extruder order, using each extruder's live nozzle
volume type, or to one entry per nozzle volume type for an extruder that holds several (Hybrid, or
`extruder_nozzle_stats` listing more types than there are extruders); on any other printer they are
not re-gathered and variant index 0 is read. Filament keys are re-gathered to one entry per filament,
on a printer with a single variant too once a filament has several variants, and under a dynamic
nozzle map to one entry per variant each filament prints through. Custom G-code and the
`machine_max_*` limits therefore index by extruder or filament, never by variant index; variant order
matters only inside the preset. An extruder with no matching variant reads variant index 0, extruder
1's first variant, whichever extruder it is; under layered nozzle grouping, `get_config_index_base`
reads the collapsed arrays and falls back to the same extruder's first entry instead.

**Existing user presets and projects follow the variant string, not the position.** A user preset
stores every variant array it changed (nullable keys as per-variant diffs, `nil` where equal to the
parent). On load each parent variant takes the child's value for the variant with the same extruder id
and variant string; variants the parent gained keep the parent's value, and a child whose arrays the
parent cannot map keeps its own. Per-object process overrides are remapped when a printer change
alters the extruder count or the length of `printer_extruder_variant`, by variant string alone (no
extruder id; of several matching values the smallest wins), and a single-value override applies to
every variant. Adding or reordering variants in a shipped preset is therefore safe for existing
users; renaming a variant, or moving it to another extruder id, loses their values for it.

**A new nozzle volume type changes no array.** Adding one to the code widens nothing until a profile
lists the new variant string; until then every existing array keeps its length and meaning.

## The four key sets

Membership is literal (four `std::set<std::string>` initializers in `src/libslic3r/PrintConfig.cpp`)
and **not guessable from names**: `ironing_speed`, `skirt_speed`, `wipe_speed`, `scarf_joint_speed`,
`small_support_perimeter_speed` and `wipe_tower_max_purge_speed` are process speeds outside the set,
while every process `*_acceleration` and `*_jerk` key is inside, and so are
`small_perimeter_threshold`, `top_solid_infill_flow_ratio` and `slowdown_for_curled_perimeters`;
`filament_flush_temp` is in and `filament_flush_temp_fast` out; `use_firmware_retraction` is out and
`travel_slope` and `retract_lift_enforce` in. The three variant-list keys and the two id
keys are members of their own set (the list sizes itself, a no-op; the id keys are padded like any
other member, [the id trap](#padding-truncation-and-composition)), while `extruder_variant_list` is in
no set: the variant-length resize leaves it alone, and only the pair rebuild of
[Printer rule 2](#printer-machine) pads it. The per-extruder `printer_extruder_options` is listed
last for contrast; it is not a variant set.

`check` and `fix-variant` read the four sets from `PrintConfig.cpp` on every run, so they follow the
engine. The lists below are from the 2026-09-29 checkout; regenerate them from the repository root
before relying on them (the recipe strips comments, since an initializer can carry a commented-out entry):

```bash
python3 - <<'EOF'
import re
src = open('src/libslic3r/PrintConfig.cpp', encoding='utf-8', errors='replace').read()
for name in ['printer_options_with_variant_1', 'printer_options_with_variant_2',
             'print_options_with_variant', 'filament_options_with_variant',
             'printer_extruder_options']:
    body = re.search(r'std::set<std::string>\s+' + name + r'\s*=\s*\{(.*?)\};', src, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    print(name, sorted(set(re.findall(r'"([^"]+)"', body))))
EOF
```

**`printer_options_with_variant_1`**, machine, stride 1 (27): `deretraction_speed`, `hotend_cooling_rate`, `hotend_heating_rate`, `long_retractions_when_cut`, `nozzle_flush_dataset`, `nozzle_type`, `nozzle_volume`, `printer_extruder_id`, `printer_extruder_variant`, `retract_after_wipe`, `retract_before_wipe`, `retract_length_toolchange`, `retract_lift_above`, `retract_lift_below`, `retract_lift_enforce`, `retract_restart_extra`, `retract_restart_extra_toolchange`, `retract_when_changing_layer`, `retraction_distances_when_cut`, `retraction_length`, `retraction_minimum_travel`, `retraction_speed`, `travel_slope`, `wipe`, `wipe_distance`, `z_hop`, `z_hop_types`

**`printer_options_with_variant_2`**, machine, stride 2 (16): `machine_max_acceleration_e`, `machine_max_acceleration_extruding`, `machine_max_acceleration_retracting`, `machine_max_acceleration_travel`, `machine_max_acceleration_x`, `machine_max_acceleration_y`, `machine_max_acceleration_z`, `machine_max_jerk_e`, `machine_max_jerk_x`, `machine_max_jerk_y`, `machine_max_jerk_z`, `machine_max_junction_deviation`, `machine_max_speed_e`, `machine_max_speed_x`, `machine_max_speed_y`, `machine_max_speed_z`

**`print_options_with_variant`**, process, stride 1 (45): `bridge_acceleration`, `bridge_speed`, `default_acceleration`, `default_jerk`, `default_junction_deviation`, `enable_overhang_speed`, `gap_infill_speed`, `infill_jerk`, `initial_layer_acceleration`, `initial_layer_infill_speed`, `initial_layer_jerk`, `initial_layer_speed`, `initial_layer_travel_acceleration`, `initial_layer_travel_jerk`, `initial_layer_travel_speed`, `inner_wall_acceleration`, `inner_wall_jerk`, `inner_wall_speed`, `internal_bridge_speed`, `internal_solid_infill_acceleration`, `internal_solid_infill_speed`, `outer_wall_acceleration`, `outer_wall_jerk`, `outer_wall_speed`, `overhang_1_4_speed`, `overhang_2_4_speed`, `overhang_3_4_speed`, `overhang_4_4_speed`, `print_extruder_id`, `print_extruder_variant`, `slowdown_for_curled_perimeters`, `small_perimeter_speed`, `small_perimeter_threshold`, `sparse_infill_acceleration`, `sparse_infill_speed`, `support_interface_speed`, `support_speed`, `top_solid_infill_flow_ratio`, `top_surface_acceleration`, `top_surface_jerk`, `top_surface_speed`, `travel_acceleration`, `travel_jerk`, `travel_speed`, `travel_speed_z`

**`filament_options_with_variant`**, filament, stride 1 (54): `activate_air_filtration`, `activate_air_filtration_during_print`, `activate_air_filtration_on_completion`, `adaptive_pressure_advance`, `adaptive_pressure_advance_bridges`, `adaptive_pressure_advance_model`, `adaptive_pressure_advance_overhangs`, `complete_print_exhaust_fan_speed`, `during_print_exhaust_fan_speed`, `enable_pressure_advance`, `filament_adaptive_volumetric_speed`, `filament_cooling_before_tower`, `filament_deretraction_speed`, `filament_extruder_variant`, `filament_flow_ratio`, `filament_flush_temp`, `filament_flush_volumetric_speed`, `filament_ironing_flow`, `filament_ironing_inset`, `filament_ironing_spacing`, `filament_ironing_speed`, `filament_long_retractions_when_cut`, `filament_max_volumetric_speed`, `filament_pre_cooling_temperature`, `filament_pre_cooling_temperature_nc`, `filament_preheat_temperature_delta`, `filament_ramming_travel_time`, `filament_ramming_travel_time_nc`, `filament_ramming_volumetric_speed`, `filament_ramming_volumetric_speed_nc`, `filament_retract_after_wipe`, `filament_retract_before_wipe`, `filament_retract_length_nc`, `filament_retract_length_toolchange`, `filament_retract_lift_above`, `filament_retract_lift_below`, `filament_retract_lift_enforce`, `filament_retract_restart_extra`, `filament_retract_restart_extra_toolchange`, `filament_retract_when_changing_layer`, `filament_retraction_distances_when_cut`, `filament_retraction_length`, `filament_retraction_minimum_travel`, `filament_retraction_speed`, `filament_wipe`, `filament_wipe_distance`, `filament_z_hop`, `filament_z_hop_types`, `long_retractions_when_ec`, `nozzle_temperature`, `nozzle_temperature_initial_layer`, `pressure_advance`, `retraction_distances_when_ec`, `volumetric_speed_coefficients`

**`printer_extruder_options`**, machine, one value per extruder, not a variant set (8):
`default_nozzle_volume_type`, `extruder_max_nozzle_count`, `extruder_printable_area`,
`extruder_printable_height`, `extruder_type`, `max_layer_height`, `min_layer_height`,
`nozzle_diameter`. These are never addressed by variant index; `extruder_offset`, `extruder_colour` and
`extruder_variant_list` are per extruder too. A shorter array acts as padded with its first value, and
entries beyond the extruder count are never read
([per-extruder vectors](machine-profiles.md#multi-extruder-idex-and-tool-changers)).

## Checking and testing

Checklist for a new variant profile set:

1. Decide the variants per extruder from the real hardware; pick legal strings only.
2. Printer base: `extruder_type`, `extruder_variant_list`, flattened `printer_extruder_variant` +
   `printer_extruder_id`, `default_nozzle_volume_type`; every variant array at variant length, every
   `machine_max_*` at 2 × variant length as (normal, silent) pairs, even where the values are the
   same, values in variant order.
3. A process base per variant layout, or no variant keys at all.
4. Filaments for the printer: a variant list covering the intended variants, every variant key at that
   width, measured values per nozzle volume type.
5. Run the usual authoring commands and full checks. `check` holds each array of steps 2–4 to the
   width of the selectable preset that writes it, `check --strict` also to every selectable preset it
   reaches, and
   the printer's layout keys through every selectable preset
   ([variant arrays](validation.md#variant-arrays)); `check_variant_names` holds every variant string,
   `extruder_type`, `nozzle_volume_type` and `default_nozzle_volume_type` to the enums
   ([variant names](validation.md#variant-names)). The choice of variants, the variant order and
   the measured values are not checked.
6. In the app, for each nozzle volume type in the sidebar combo: slice and confirm the G-code uses that
   variant's values (e.g. volumetric speed limit, retraction). `validate_slice` only slices the default
   nozzle volume type.

To review rather than author, run the same list against the diff. `check` catches a wrong array length,
a layout key out of step, and a variant string the enums cannot build. The failures no check catches: a
new variant appended instead of inserted at its index, an array widened on a base whose list is
shorter (cut before any child inherits it), a process lacking a pair its printer can select, and a High
Flow variant copied from Standard.

### UI facts to design around

- The single-extruder sidebar shows a **nozzle volume type combo** (tooltip `Flow`, in place of the
  nozzle-diameter selector) only when `extruder_variant_list` offers more than one distinct variant
  string (`support_different_extruders`); four extruders listing `Direct Drive Standard` each show
  none.
- The two-extruder sidebar's per-extruder nozzle volume type combos are shown for BBL printers only.
  Another vendor's multi-extruder printer falls back to the single-extruder layout: one combo (for the
  first extruder) when the variants differ, otherwise the nozzle-diameter selector, so the other
  extruders' nozzle volume type stays at `default_nozzle_volume_type`.
- High Flow is hidden from the combo when `printer_variant` is `0.2` or the printer model is
  `Bambu Lab X1E`, and E3D High Flow unless `printer_variant` is `0.4` or `0.6`. A variant listed on
  another nozzle diameter is never selectable there.
- The filament tab shows a variant switch built from the filament's own `filament_extruder_variant`;
  a multi-variant filament whose tab shows none did not resolve its variant list through `include` or
  `inherits`. The printer and process tabs show one entry per extruder instead, labelled with that
  extruder's live nozzle volume type (two for Hybrid), and only on two-extruder printers whose
  `extruder_variant_list` offers more than one variant string; elsewhere they edit the variant of the
  nozzle volume type selected in the sidebar (an X1C shows no switch).

### Worked examples in the tree

`BBL/machine/fdm_bbl_3dp_001_common.json` (one extruder), `fdm_bbl_3dp_002_common.json` (two
extruders), `Bambu Lab X1 Carbon 0.4 nozzle.json` (Standard + High Flow), `Bambu Lab H2D 0.4
nozzle.json` (extruders with different variant sets), `Bambu Lab X2D 0.4 nozzle.json` (Direct Drive +
Bowden), with their `@BBL` processes and filaments. The H2D and X2D examples also carry
`E3D High Flow` variants. BBL's multi-variant filament lists come from `fdm_filament_template_*`
presets that the printer-specific filaments pull in with `include`, not from their `inherits` chain.
