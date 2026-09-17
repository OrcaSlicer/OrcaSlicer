# Reviewing a profile change

Ordered by how often each item actually goes wrong in this repo. The table is the short version — what
CI cannot see, which is where review earns its keep; the items are the how. What CI *does* run:
[validation.md](validation.md).

| Not checked by CI | Consequence |
| --- | --- |
| The `version` bump | The change never reaches an upgrading user |
| A misspelled setting key | Setting silently has no effect |
| A filename Windows cannot check out, or one that differs from its `sub_path` only in case | Works on the author's machine, breaks the bundle on another platform |
| `bed_model` / `bed_texture` / `hotend_model` pointing at a missing asset | Bed renders as Custom, hotend falls back to the generic model |
| A nozzle size in a model's list with no matching variant | The size is offered and resolves to nothing |
| A non-default process | `validate_slice` only slices each printer's `default_print_profile` |
| A dangling `compatible_printers` inside an `instantiation: "false"` base | A base never becomes a `Preset`, so the reference check never sees it (a bad `inherits` in a base *is* caught) |
| A `renamed_from` whose old name is still a live preset | The redirect is inert while a live preset carries that name |
| Per-extruder vector length on a multi-nozzle printer | Silently padded (with the **first** value) or truncated |

## 1. Was the vendor `version` bumped?

For **every** bundle whose folder the diff touches, `resources/profiles/<Vendor>.json` must have its
`version` incremented — last component only, never past 99. A library change means bumping
`OrcaFilamentLibrary.json`.

*Why:* nothing in CI checks it, and `PresetUpdater` reinstalls only when `vendor_ver < resource_ver` —
without a bump the change reaches neither an upgrading user nor the author's own running app.

## 2. Was the index rebuilt, and does the diff contain only this change?

`check` now fails on an unregistered file, on an index `update-index` would reorder, and on a file
`normalize` would rewrite — so a PR that skipped them arrives red, and you do not have to spot the
omission yourself. Three things are still yours:

- **The index diff belongs to this change.** `update-index` rewrites whole `*_list` sections. If the
  bundle had drifted, the author's PR now carries someone else's reordering; ask for it in a separate
  commit rather than reviewing it inline.
- **A deleted preset is a rename in disguise** unless item 4 is satisfied. `update-index` de-registers
  it silently and CI is happy.
- **`normalize` edits content, not just layout.** It drops `version` and `is_custom_defined` from preset
  files, deletes six print-speed keys from filament profiles, and resolves `extruder_clearance_radius`
  against `extruder_clearance_max_radius` by keeping the larger. Check that the keys it removed were
  meant to go.

*Why:* the index is the loader's only entry point. Out-of-order entries fail with `can not find inherits`
and take the whole vendor bundle down; an unindexed file gets reviewed, merged and never loads.

## 3. Are ids generated, not written?

No hand-typed or copied `setting_id` / `filament_id`. Instantiated presets have a `setting_id`; bases do
not. A filament id change comes with a `scripts/filament_id_snapshot.json` diff in the same commit.
`check` enforces all of that; what it cannot tell you is whether the identity *should* have moved.

Read the snapshot diff as the identity gate: a removed id or a changed triple means a product's identity
moved, and the old id is not forwarded anywhere. Confirm that was intended.

*Why:* a duplicate `filament_id` on one printer makes AMS spool matching a coin toss; a copied
`setting_id` breaks preset identity. See [ids.md](ids.md).

## 4. Does anything disappear for existing users?

A rename, a deletion, or a flip of `"instantiation": "true"` → `"false"` on a shipped preset removes the
name from the preset collection. It needs `renamed_from` on a successor — and only one preset may claim a
given old name. The claimed old name must **not** still be a live preset; the redirect is inert if it is.

*Why:* user presets inheriting it die with `can not find parent <name> for config <file>!`; 3MF-embedded
presets are dropped with no error at all. Commit `33923464ae` reverted exactly this for Cubicon;
`6943b6ddc3` redid it correctly. CI's `validate_custom` catches the shipped-name case — but not an inert
`renamed_from`.

## 5. Is `compatible_printers` right?

Exact printer **variant** names, non-empty on every filament outside OrcaFilamentLibrary and written in
the preset's own file — golden rule 6, with the flattened-vs-own-key trap in
[filament-profiles.md](filament-profiles.md#compatible_printers). Watch for a nozzle-specific variant that
inherited or copied the base's full printer list, and for two presets of one product with overlapping
lists — duplicate combobox entries and an ambiguous AMS match.

*Why:* real shipped bugs twice (`b7b3418baf` "showing up everywhere", `ff83aa41ef` duplicate Flashforge
entries).

## 6. Model ↔ variant ↔ process consistency

- New nozzle size → the model's `nozzle_diameter` list extended, a variant with a matching
  `printer_variant`, and at least one process listing that variant.
- `default_print_profile` is one exact name (not a `;` list), and that process's resolved
  `compatible_printers` includes this printer.
- `default_filament_profile` is an array of names that exist.

*Why:* the first two are hard-gated — an unlisted `printer_variant` fails the printer preset and takes
the whole bundle down. The rest is not: a variant no process lists still loads and passes
`validate_system`, surfacing only in `validate_slice` as a fallback to the default preset.

## 7. Types and spellings

Every value a string or an array of strings; `filament_type` an array; `instantiation` the string
`"true"`/`"false"` — golden rule 7, including the two keys whose non-string values abort the load for
**every** vendor.

The part only a reviewer can do: check new setting keys against `src/libslic3r/PrintConfig.cpp`. A
misspelled key is silently discarded (rule 8), the single most common way a profile edit does nothing
while CI stays green.

## 8. Blast radius of a base edit

A change to `fdm_*_common.json` reaches every child at once. Ask which presets it touches — several
reverts in this repo are exactly this (`41d1b0d3c8`, `dc491166a8`). Also check whether the edited leaf has
children of its own: Prusa, Flashforge and Elegoo all chain leaf-inherits-leaf several levels deep.

## 9. Do the numbers make sense for the nozzle?

Line widths at nozzle + 0.02; `layer_height` ≤ nozzle; MVS and pressure advance tracking nozzle size
(tables in [process-profiles.md](process-profiles.md) and [filament-profiles.md](filament-profiles.md)).
A 0.2-nozzle preset still carrying the 0.4 MVS is the classic copy-paste bug.

Settings tuned for real hardware cannot be verified by reading the diff. Say so rather than approving
numbers nobody measured.

## 10. Asset references (not checked anywhere)

`bed_model`, `bed_texture`, `hotend_model` and `<Model>_cover.png` exist under
`resources/profiles/<vendor folder>/`. Broken references already ship; nothing checks them.

## 11. `default_materials` (checked by CI)

`check` fails on a `default_materials` / `default_filament_profile` name that resolves to no system
filament, so a dangling entry no longer reaches review. Scope the run while working on one vendor:

```bash
python3 scripts/orca_profile_tool.py check --vendor "<Vendor>"   # py -3 on Windows
```

## 12. Per-extruder vector lengths (not checked)

One entry per extruder for the plain per-extruder vectors; the `printer_options_with_variant_1` keys are
sized to `printer_extruder_variant` instead. A wrong length is silently padded — repeating the **first**
value, not the last — or truncated. The two sizing families and the worked cases are in
[machine-profiles.md](machine-profiles.md#multi-extruder-idex-and-tool-changers).

## 13. Non-default processes get no slice coverage

`validate_slice` only slices each printer's `default_print_profile` and nothing else. A new quality tier
that is not the default was never sliced by CI.

## 14. Housekeeping worth a nit, not a block

`"from"` other than `"system"` (the preset-bundle loader ignores it, though the CLI's config-file loader
rejects anything but `system`/`user`/`User`), `printer_settings_id` copied from another
vendor, a filename that disagrees with the preset's `name` (common; the loader keys off `name`), and
obsolete keys (`check --obsolete-keys` warns tree-wide — read only this vendor's).

## 15. Cross-platform filenames and paths (not checked)

A filename with a trailing space or dot, or one of `< > : " | ? *`; a `sub_path` or asset path that
matches the file only case-insensitively. Both pass on the author's machine and break on another
platform — Windows cannot check the first out, Linux will not resolve the second.

---

## Reporting the review

A finding is: **one defect**, its file, what breaks at runtime or in CI, and the fix. Split independent
defects into separate findings even when they live in one file — five id problems in one bullet get one
fix and four survivors.

Severity discriminates only if it is earned:

| Severity | Means |
| --- | --- |
| blocker | the bundle fails to load, or a preset is unreachable at runtime |
| major | CI fails, or existing users lose a preset |
| minor | wrong-but-working: dead keys, `from`, naming, redundant overrides |

Compute every number and id (`orca_profile_tool.py`, a scripted count) or omit it — one invented count
makes a reader stop trusting the right ones. Report a command's result only if you ran it.
