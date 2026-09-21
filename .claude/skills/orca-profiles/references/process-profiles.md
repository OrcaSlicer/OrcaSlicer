# Process profiles

Processes live in `resources/profiles/<Vendor>/process/` — selectable leaves and shared bases alike, and
every one of them is registered in `process_list`. There are no global processes shared across vendors.

## Naming

`"<layer height>mm <quality> @<target>"` — near-universal, so match it.

Follow the bundle's existing quality vocabulary. BBL's common ladder relates the quality word to
the layer-height / nozzle ratio; it is a naming convention, not a loader constraint:

| Quality | Ratio | 0.2 nozzle | 0.4 | 0.6 | 0.8 |
| --- | --- | --- | --- | --- | --- |
| Extra Fine | 0.2× | — | 0.08 | — | — |
| Fine | 0.3× | 0.06 | 0.12 | 0.18 | 0.24 |
| Optimal | 0.4× | 0.08 | 0.16 | 0.24 | 0.32 |
| Standard | 0.5× | 0.10 | 0.20 | 0.30 | 0.40 |
| Draft | 0.6× | 0.12 | 0.24 | 0.36 | 0.48 |
| Extra Draft | 0.7× | 0.14 | 0.28 | 0.42 | 0.56 |

This is the `fdm_process_single_<lh>_nozzle_<n>` ladder; 0.4 is commonly the unsuffixed nozzle default.
Match neighbouring names rather than renaming shipped tiers to fit the table.

The `@target` is a human label, not a reference: most do not equal any real printer variant name.
Compatibility comes from the resolved list or condition, not this label.

## Shape

A selectable leaf's only truly universal keys are `type`, `setting_id`, `name` and `instantiation`;
`inherits` and `from` are near-universal — plus compatibility. No slicing key is universal; even
`layer_height` is more often inherited than restated. A base has `type`, `name`, `instantiation`, almost
always `from`, and **no** `setting_id`.

**Target shape: a 7-key leaf.** `OrcaArena` is the cleanest model —
`fdm_process_common` → `fdm_process_arena_common` → `fdm_process_arena_<lh>_nozzle_<n>` → leaf, where the
leaf carries only `type`, `name`, `inherits`, `from`, `setting_id`, `instantiation`,
`compatible_printers`, and the per-nozzle base holds the layer height and all eight line widths.

BBL, WonderMaker and Z-Bolt are uniform in *layering* — every leaf inherits a base, names its printers
directly and holds no layer height of its own — but not in key count. Imitate BBL's layering, not its
content: its leaves carry doubled `print_extruder_variant` arrays that no single-variant vendor needs.

Nearly every vendor ships its own `fdm_process_common` as the inherits-less root. Those files are not
identical; copying another vendor's version into a new bundle is normal.

Beware leaf-inherits-leaf: Prusa chains several levels deep through sibling leaves, and Elegoo and
Flashforge do it too, so editing one selectable process silently changes others. Check a leaf's children
before editing it.

## Compatibility

Most leaves set `compatible_printers` directly; some inherit it from a base, and Prusa's fall through to
`compatible_printers_condition`. After resolving `inherits`, **every selectable process has one or the
other** — that is the invariant to review against. Unlike filaments, inheriting `compatible_printers` is
legitimate for a process, and no check enforces its presence.

- A non-empty `compatible_printers` makes `compatible_printers_condition` **dead code**. Use one or
  the other.
- A condition that fails to parse means *compatible with everything* — a warning, not an error. A typo
  widens compatibility instead of narrowing it.
- Matching is `boost::regex` **`regex_match`** — a full-string match, which is why every shipped
  condition wraps its keyword in `.*`. Because it is boost rather than `std`, `.` also spans the newlines
  inside `printer_notes`.
- A `printer_notes` keyword that prefixes another model's keyword matches both. Prusa guards it:

  ```
  printer_notes=~/.*PRINTER_MODEL_COREONE[^_a-zA-Z0-9].*/ and nozzle_diameter[0]==0.4 and printer_notes=~/.*HF_NOZZLE.*/
  ```

  The `[^_a-zA-Z0-9]` exists because `PRINTER_MODEL_COREONE_L` also contains `PRINTER_MODEL_COREONE`.

`compatible_printers` is almost always one element. A leaf listing a whole model family is where a newly
added printer is usually forgotten.

## What to review per nozzle

| Key group | Review |
| --- | --- |
| `line_width` and per-region widths | resolved widths suit the nozzle and layer height |
| `layer_height`, `initial_layer_print_height` | within the printer's limits |
| print speeds | consistent with flow limits and hardware tuning |
| shell layers, wall loops, accelerations, support Z distances | preserve the intended thickness, motion and support behavior |

**A common starting pattern is nozzle + 0.02 mm**: 0.22 / 0.42 / 0.62 / 0.82 / 1.02. In that pattern, at 0.4,
`inner_wall_line_width`, `sparse_infill_line_width`, `skin_infill_line_width` and
`skeleton_infill_line_width` widen to 0.45 and `initial_layer_line_width` to 0.5; at 0.2,
`initial_layer_line_width` widens to 0.25. Also derived, and easily missed:
`ironing_inset = line_width / 2` (0.11 / 0.21 / 0.31 / 0.41).
These are examples, not required values; preserve intentional vendor tuning and percentage/automatic
widths, and validate their resolved values.

`min_layer_height` and `max_layer_height` are machine keys — no process file sets them.

## Slice-time content checks

`Print::validate()` enforces four rules at slice time:

1. `initial_layer_print_height` ≤ min `nozzle_diameter`
2. `layer_height` ≤ min `nozzle_diameter` — *"Layer height cannot exceed nozzle diameter."*
3. `line_width` and the seven per-region widths (inner/outer wall, sparse infill, internal solid infill,
   top surface, skin, skeleton) > `layer_height` — *"Line width too small"*. `support_line_width` only
   when the object has support or a raft; `initial_layer_line_width` is never checked.
4. every width ≤ 5 × max `nozzle_diameter` — *"Line width too large"*

Two further rules cover `bridge_line_width` (≤ nozzle diameter; > `layer_height` unless `thick_bridges`
and `thick_internal_bridges` are both on). The sweep starts from printer defaults rather than
enumerating every process. **A new non-default process gets no dedicated slice coverage in CI.**

## What CI checks on a process

Structure, not content: `process_list` name consistency **and** index coverage the other way, two files
claiming one process name, the `extruder_clearance_radius` / `extruder_clearance_max_radius` conflict
pair, duplicate JSON keys, a file `normalize` would rewrite, and the five `setting_id` rules (the fifth
rejects the misspelled key `settings_id`). `compatible_printers` presence is checked for **filaments
only**.

Note the C++ loader derives a missing `setting_id` on the fly, so the validator will not fail a process
without one — only `orca_profile_tool.py check` catches it. Running the validator alone gives a false
all-clear.

## Silent failures specific to processes

- **Unknown or misspelled keys are discarded with no error and no warning.** They ship all over the
  process tree, both plain typos (`inital_layer_height`, `tree_support_bramch_diameter_angle`,
  `sparse_infill_patter`) and keys copied from other slicers that Orca never defined.
- Keys on the tool's `OBSOLETE_KEYS` list (`adaptive_layer_height`, `overhang_totally_speed`, …) are
  rejected by `check`'s normalization pass across preset types; `normalize` removes them.
  The additional per-key obsolete warnings read `filament/` only.
- A dangling `compatible_printers` inside an `instantiation: "false"` base is invisible to
  `check_preset_references`: a base never becomes a `Preset` at all (its config goes into `config_maps`
  and the loader returns early), so it is in no collection for the check to walk.
- Orphan bases that nothing inherits are scattered through the tree — usually the leftover of a
  half-finished nozzle addition.

## Adding a quality tier or a nozzle's processes

1. Choose the layer height and quality label using the vendor's existing ladder.
2. If the vendor has per-nozzle bases, add one (`fdm_process_<vendor>_<lh>_nozzle_<n>`) with the layer
   height, nozzle-appropriate line widths, `initial_layer_print_height` and `ironing_inset`.
3. Add the leaf: 7 keys, `compatible_printers` naming the exact printer variant(s).
4. Register both in `process_list`, parent first. Bump the version, run the id tool, validate.
5. Slice this process explicitly with its intended printer; the sweep gives non-default tiers no
   dedicated coverage. If it is a printer's `default_print_profile`, verify the exact name and
   resolved compatibility too — the sweep may fall back or select another compatible process.
