# Reviewing a profile change

Run `./scripts/check_profile.sh` on the applied diff first ([validation.md](validation.md) says what CI
runs), then work through the items below: delivery, identity and backward compatibility first, then the
affected preset types. The table lists the gaps CI cannot see, so only a reviewer catches them.

| Not checked by CI | Consequence |
| --- | --- |
| The `version` bump | The change never reaches an upgrading user; an absent `version` hides the vendor from the setup wizard |
| A misspelled setting key | The setting silently has no effect |
| A filename Windows cannot check out, or one that differs from its `sub_path` only in case | Works on the author's machine, breaks the bundle on another platform |
| `bed_model` / `bed_texture` / `hotend_model` / cover pointing at a missing asset | A missing bed model renders a generic custom bed and a missing texture renders none, the hotend falls back to the generic model, the cover shows a placeholder |
| A nozzle size in a model's list with no matching variant | The size is offered and resolves to nothing |
| A non-default process | `validate_slice` gives non-default quality tiers no dedicated coverage |
| Whether the intended default survived compatibility selection | The sweep can select a different compatible preset |
| A dangling `compatible_printers` inside a base whose children all override it (or that has no instantiated children) | The reference check walks resolved selectable presets, so it reports a base's list only through a child that inherits it unchanged (a bad `inherits` in a base *is* caught) |
| A base nothing inherits | Dead weight, usually the leftover of an unfinished nozzle addition |
| A `renamed_from` whose old name is still a live preset | The redirect is inert while a live preset carries that name |
| A preset differentiated only by colour, or an all-printer library preset without `@System` | Per-colour presets split one product across several ids and the selector fills with near-duplicates; CI stays green |
| Plate temperatures for plates the printer has | The user's plate reads an unset or inherited temperature |
| Per-extruder vector length on a multi-extruder printer | Silently padded (with the **first** value) or truncated |
| A new variant appended instead of inserted at its variant index, a variant array widened on a base with a shorter list, or a variant a filament/process lacks | Values shift onto the wrong extruder, are cut before any child inherits them, or resolve to the first variant: High Flow silently gets Standard values |
| A name that ignores its type's convention: a printer model in a `process` quality position, or an unrelated target label or base name left in a copied preset | The selector misrepresents the preset's quality or intended printer |
| Values: temperatures, speeds, widths, pressure advance | A wrong value prints wrong while CI stays green |

## 1. Was the vendor `version` bumped?

For **every** bundle whose folder the diff touches, `resources/profiles/<Vendor>.json` must have its
`version` incremented: last component, carrying `.99` into the third component. A library change means
bumping `OrcaFilamentLibrary.json`.

*Why:* nothing in CI checks it, and the app reinstalls a bundled profile set only when its version is
newer than the installed one: without a bump the change reaches neither an upgrading user nor the
author's own running app. Without any `version` the vendor vanishes from the setup wizard, and neither
`check` nor the validator reports it.

## 2. Was the index rebuilt, and does the diff contain only this change?

`check` fails on an unregistered file, on an index `update-index` would reorder, and on a file
`normalize` would rewrite, so a PR that skipped them arrives red and you do not have to spot the
omission yourself. Three things are still yours:

- **The index diff belongs to this change.** `update-index` rewrites whole `*_list` sections. If the
  bundle had drifted, the author's PR now carries someone else's reordering; ask for it in a separate
  commit rather than reviewing it inline.
- **A deleted selectable preset needs a successor** as in item 4. `update-index` removes its
  registration; `validate_custom` detects the break only for names covered by released fixtures.
- **`normalize` edits content, not just layout.** It drops `version` and `is_custom_defined` from preset
  files, removes obsolete keys, deletes six print-speed keys from filament profiles, and resolves
  `extruder_clearance_radius` against `extruder_clearance_max_radius` by keeping the larger
  ([what normalize changes](validation.md#normalize-and-update-index-are-part-of-the-check)). Check that
  the keys it removed were meant to go.

Index order is dependency order, not alphabetical: parents and include templates before the presets
that use them, then, in name order, the entries that neither depend on nor are depended on by another
entry of their own list (such as a leaf filament whose only parent is in the library), and any entry on
a dependency cycle. Judge a hand-placed entry only by
`update-index --dry-run`: if it reports nothing to rebuild, the position is not a finding.

*Why:* the index is the loader's only entry point. Out-of-order entries fail with `can not find inherits`
and take the whole vendor bundle down; an unindexed file gets reviewed, merged and never loads.

## 3. Are ids generated, not written?

No hand-typed or copied `setting_id` / `filament_id`. Instantiated presets have a `setting_id`; bases do
not. `check` enforces all of that; what it cannot tell you is whether the identity *should* have moved.

A rewritten or removed `filament_id` means a product's identity moved (a renamed alias, or an edited
`filament_vendor` / `filament_type`), and the old id is not forwarded anywhere. Confirm that was
intended, and that a new id is not a rename in disguise.

*Why:* a duplicate `filament_id` on one printer makes AMS spool matching a coin toss; a copied
`setting_id` breaks preset identity. See [ids.md](ids.md).

## 4. Does anything disappear for existing users?

A rename, a deletion, or a flip of `"instantiation": "true"` → `"false"` on a shipped selectable preset
removes the name from the preset collection. It needs `renamed_from` on a selectable successor
([renamed_from](vendor-bundle.md#renamed_from)), and only one preset may claim a given old name. The
claimed old name must **not** still be a live preset; the redirect is inert if it is.

*Why:* user presets inheriting it die with `can not find parent <name> for config <file>!`; 3MF-embedded
presets are dropped with no error at all. Commit `33923464ae` reverted exactly this for Cubicon;
`6943b6ddc3` redid it correctly with `renamed_from`. CI's `validate_custom` catches the shipped-name
case, but not an inert `renamed_from`.

## 5. Is `compatible_printers` right?

Exact printer **variant** names, non-empty on every instantiated filament outside OrcaFilamentLibrary,
and written in the preset's own file ([SKILL.md rule 9](../SKILL.md#rules); the resolved-vs-own-key trap
is in [filament-profiles.md](filament-profiles.md#compatible_printers)). A `machine_model` name instead
of a variant name is the usual mistake: `check` passes it, the validator reports
`references unknown compatible_printers`. Watch for a nozzle-specific tune that inherited or copied the
base's full printer list, and for two presets of one product with overlapping lists: duplicate combobox
entries and an ambiguous AMS match.

Two presets of one product (`filament_id`) must not share a variant. Resolve it by specificity: move the
variant to the most specific preset and remove it from the more general ones, which is preferred over
deleting a profile. Then repoint the machine's `default_filament_profile` and the model's
`default_materials` at the profile that now covers it. See
[one variant, one profile](filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product).

*Why:* real shipped bugs twice (`b7b3418baf` filaments "showing up everywhere", `ff83aa41ef` duplicate
Flashforge entries). The Python `check` passes on an overlap; only the full `check_profile.sh`
(`validate_system`) reports `Ambiguous AMS filament match`.

## 6. One product, one all-printer preset; colour is not a preset

No presets that differ only by colour: `filament_id` identifies a product, and the colour comes from the
spool at runtime. An all-printer library product is a `<Product> @System` shim with an empty
`compatible_printers` ([colour](filament-profiles.md#colour-is-a-runtime-property)).

*Why:* per-colour presets pass every check, so this is a review call.

## 7. Model ↔ variant ↔ process consistency

- A new nozzle size → the model's `nozzle_diameter` list extended, a variant with a matching
  `printer_variant`, and at least one process listing that variant. Every size in the model's list has a
  variant (unchecked).
- `default_print_profile` is one exact name (not a `;` list), and that process's resolved compatibility
  list or condition includes this printer.
- `default_filament_profile` is an array of names that exist.
- Each variant of the model has at least one compatible entry in the model's `default_materials`.

*Why:* an unlisted `printer_variant` is a hard bundle-load failure. Default process selection is weaker:
the sweep attempts the named default, then updates compatibility and rejects generic Default fallbacks,
so another compatible process can conceal a bad reference. Inspect it even after a pass.

## 8. Types and spellings

Every value a string or an array of strings; `filament_type` an array; `instantiation` the string
`"true"` / `"false"`; custom G-code one string, never an array of lines ([SKILL.md rule 5](../SKILL.md#rules)).
Check index metadata and model `nozzle_diameter` especially: wrong types there can abort loading for
**every** vendor.

The part only a reviewer can do: check new setting keys against `src/libslic3r/PrintConfig.cpp`. A
misspelled key is silently discarded ([rule 6](../SKILL.md#rules)), the single most common way a profile
edit does nothing while CI stays green.

## 9. Blast radius of a base edit

A change to `fdm_*_common.json` or any other base reaches every child at once. Ask which presets it
touches: several reverts in this repo are exactly this (`41d1b0d3c8`, `dc491166a8`). Also check whether
the edited leaf has children of its own: a bundle may chain leaf-inherits-leaf several levels deep. A
newly added base that nothing inherits is dead weight, and a dangling
`compatible_printers` inside a base is reported only through a child that inherits it unchanged. A diff
that only moves values between presets and bases must leave every selectable preset loading what it
loaded: ask for the `compare` result ([shared bases](shared-bases.md#nothing-loads-differently)), and
check each new base against the [balance rules](shared-bases.md#balance).

## 10. Do the numbers make sense for the nozzle and material?

Check resolved widths and layer heights against the nozzle, temperatures against the material (PLA
values under an ASA name print wrong), and flow limits / pressure advance against the actual hardware
and material. The patterns in [process-profiles.md](process-profiles.md#values-to-review-per-nozzle)
are examples, not mandatory values; [filament-profiles.md](filament-profiles.md#tuning-per-nozzle-and-per-variant)
explains what to revisit for a nozzle change. A cloned preset's unchanged volumetric speed needs
particular scrutiny.

Settings tuned for real hardware cannot be verified by reading the diff. Say so rather than approving
numbers nobody measured.

## 11. Plate temperatures

A filament sets the plate temperature for every plate the printer plausibly has, as its siblings do;
`textured_cool_plate_temp` is the one most often forgotten
([twelve keys](filament-profiles.md#bed-temperature-is-twelve-keys-not-one)).

## 12. Asset references (not checked anywhere)

`bed_model`, `bed_texture`, `hotend_model` and `<Model>_cover.png` exist under
`resources/profiles/<vendor folder>/`, in exact case. Nothing checks them.

## 13. `default_materials` and `default_filament_profile` (checked by CI)

`check` fails on a `default_materials` / `default_filament_profile` name that matches no filament file,
and `validate_system` on a variant with no compatible system filament in `default_materials`, so a
dangling entry no longer reaches review. When compatibility moves between profiles of a product, the
machine's `default_filament_profile` and the model's `default_materials` must be repointed at the most
specific profile that still covers the variant, dropping generic entries that no longer apply; the same
[specificity rule](filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product) applies
when adding or fixing defaults. Scope the run while working on one vendor:

```bash
python3 scripts/orca_profile_tool.py check --vendor "<Vendor>"   # py -3 on Windows
```

## 14. Per-extruder vectors (not checked) and variant arrays (widths and layout checked)

One entry per extruder for the plain per-extruder vectors; the variant sets are sized to the variant
length, `len(printer_extruder_variant)` or one per extruder when the resolved preset has no layout (a
per-extruder difference in those keys still needs the layout, since the loader keeps only the
first value of a list-less printer's arrays). A wrong length is silently padded, repeating the **first**
value, not the last, or truncated. The two sizing families and the worked cases are in
[machine-profiles.md](machine-profiles.md#multi-extruder-idex-and-tool-changers).

`check` reports a variant array of any type that is not exactly the `variant length × stride` width
of the selectable preset that writes it, one value included, as an error whatever the values;
`check --strict` also reports one that reaches a selectable preset at another width. A base is not
judged on its own. On a machine it
also reports layout keys that disagree: `printer_extruder_variant` / `printer_extruder_id` not the
flattening of `extruder_variant_list`, a variant without its extruder's `extruder_type` prefix, a
`default_nozzle_volume_type` the extruder does not list, a list of several variants without the pair, a
pair without the list that puts several variants on one extruder. On a machine or a process, an id
array, written or inherited, whose length differs from its variant list is an error; the id and
layout rules judge the composed preset without `--strict`. Two layout findings are warnings: a
pair without the list that `single_extruder_multi_material` off would replace at load, and a process
without `print_extruder_id` whose variants repeat. The full rules are in
[variant arrays](validation.md#variant-arrays). `check_variant_names` separately holds every variant
string, `extruder_type`, `nozzle_volume_type` and `default_nozzle_volume_type` to the engine's enums
in every bundle, BBL included, failing on a dead variant, a legacy spelling and a variant list that
names one variant twice ([variant names](validation.md#variant-names)).

On a multi-variant printer, still check by hand:

- that a new variant's values sit at its variant index (machine limits as a (normal, silent) pair at
  `2 × index`) in every file of the chain that restates the key, `include` templates included;
- that each process lists every (extruder id, variant) pair its printers can select;
- that no array was widened on a base with a shorter variant list: the loader cuts it before the
  children inherit it ([composition](extruder-variants.md#padding-truncation-and-composition)), and
  `check`, even `--strict`, composes without that cut, so it passes;
- that only keys in [the four sets](extruder-variants.md#the-four-key-sets) carry per-variant values;
- that the High Flow variants carry measured values rather than copies
  ([checking and testing](extruder-variants.md#checking-and-testing)).

## 15. Non-default processes get no slice coverage

`validate_slice` starts from printer defaults; it does not enumerate every process. Slice a new or
changed non-default tier explicitly with its intended printer
([on a copy of the tree](validation.md#checking-a-copy-of-the-tree)).

## 16. Do the preset names follow the conventions?

Check **every newly added profile and intentional name change**, including models and bases, against
[the naming conventions](naming.md#checking-names): no printer model in a process quality position, no
target label or base name left over from a copied preset, the bundle's established style. Preserve
shipped names during ordinary tuning; renaming a shipped selectable preset requires the migration in
item 4.

*Why:* CI checks name uniqueness, but does not enforce the naming conventions. Catch naming mistakes
before the names ship and existing projects depend on them.

## 17. Cross-platform filenames and paths (not checked)

Check for Windows-invalid characters, reserved device names, trailing path-component spaces or dots,
and case mismatches between `sub_path` or asset references and the files on disk. See
[filenames and paths](naming.md#filenames-and-paths).

## 18. Housekeeping worth a nit, not a block

`"from"` other than `"system"` (the bundle loader ignores it, though loading the file as a CLI config
rejects anything but `system` / `user` / `User`), `printer_settings_id` copied from another vendor,
redundant overrides that restate the parent's value, siblings that each repeat a value their base could
hold ([shared bases](shared-bases.md)), and a filename that disagrees with the preset's
`name` (the loader keys off `name`).

---

## Reporting the review

A finding is **one defect**: its file (or quoted lines), what breaks at runtime or in CI, and the fix.
Split independent defects into separate findings even when they live in one file: five id problems in
one bullet get one fix and four survivors. Say which findings `check` or the validator reports and which
only a reader catches: a missing version bump, a misspelled key and wrong temperatures pass CI, so a
contributor who only reruns the tools fixes what CI flags and resubmits the rest.

Severity discriminates only if it is earned:

| Severity | Means |
| --- | --- |
| blocker | the bundle fails to load, or a preset is unreachable at runtime |
| major | CI fails, existing users lose a preset, or a value prints wrong while CI stays green (PLA temperatures under an ASA name; a misspelled key whose intended value differs from the inherited one) |
| minor | wrong but working: redundant or dead keys that change nothing, `from`, naming |

Compute every number and id (`orca_profile_tool.py`, a scripted count) or omit it: one invented count
makes a reader stop trusting the right ones. Report a command's result only if you ran it. End with a
verdict: can it merge as it stands?
