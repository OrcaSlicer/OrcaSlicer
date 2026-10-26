---
name: orca-profiles
description: Use when creating, modifying, reviewing or debugging OrcaSlicer FFF system profiles under resources/profiles, including printer/vendor/nozzle/material additions, bundle indexes and versions, preset renames, setting_id and filament_id, and moving settings that sibling presets repeat onto shared bases after fix-variant or while drafting. Also use for missing presets or vendors, ignored profile settings, ambiguous AMS filament matches, and failures from orca_profile_tool.py, check_profile.sh/.bat, OrcaSlicer_profile_validator or the Check profiles CI job.
---

# OrcaSlicer system profiles

This skill describes how OrcaSlicer system profiles are drafted and shaped: the rules, equations and
patterns a profile follows. Use it to draft new profiles, modify existing ones, fix profile issues and
review profile changes.

A bundle is the index `resources/profiles/<Vendor>.json` plus the folder `<Vendor>/`. The vendor id is
the filename stem (`BBL`), not the index's display `name` (`Bambulab`). The index is the loader's only
entry point: an unindexed preset never loads. `OrcaFilamentLibrary` is the shared filament bundle,
loaded first; `blacklist.json` is data, not a bundle.

## References

Read the reference for the task before editing; load others only when the task crosses into them.
Paths below are relative to this skill. Commands run from the repository root; on Windows use `py -3`
for `python3`.

| Task | Read |
| --- | --- |
| Add or tune a filament, brand or material; fix compatibility, alias shadowing or overlapping coverage | [filament-profiles.md](references/filament-profiles.md) |
| Add a printer or nozzle; change models, variants, assets or per-extruder vectors | [machine-profiles.md](references/machine-profiles.md) |
| Add or tune extruder variants (`extruder_type` Direct Drive / Bowden × nozzle volume type Standard / High Flow / TPU High Flow / E3D High Flow / Extra High Flow variants) on a printer, process or filament | [extruder-variants.md](references/extruder-variants.md) |
| Add a quality tier or tune a process | [process-profiles.md](references/process-profiles.md) |
| Draft several presets, or clean up after `fix-variant`: which base each shared value belongs on, when a new base pays off, proving nothing loads differently | [shared-bases.md](references/shared-bases.md) |
| Name a preset; check what a name must equal; base names, uniqueness, filenames | [naming.md](references/naming.md) |
| Create a vendor bundle; index, `version`, `inherits`, `include`; migrate preset names; diagnose why a bundle fails to load | [vendor-bundle.md](references/vendor-bundle.md) |
| Change ids; diagnose AMS identity | [ids.md](references/ids.md), then `docs/HLSD/filament_id.md` for identity changes |
| Run checks, interpret failures, test another tree or verify in the app | [validation.md](references/validation.md) |
| Review a profile diff | [review-checklist.md](references/review-checklist.md) |

## Rules

1. **Bump the `version` of every bundle you change**, `OrcaFilamentLibrary.json` included when affected.
   Increment the last component and carry `.99` into the third (`02.04.00.99` → `02.04.01.00`). The
   updater installs only a strictly newer version, and CI does not check the bump.
2. **Register every preset, bases included, parents before children.** `update-index` writes the four
   `*_list` arrays from the files on disk; `check` fails unless the index equals its output. Each index
   entry's `name` must equal the file's `name`.
3. **Generate ids; never invent or copy them.** Keep existing ids during ordinary tuning. New presets
   normally omit them until `generate-id`; bases carry no `setting_id`. BBL's own `setting_id`s and a wrongly
   inherited `filament_id` need the explicit handling in [ids.md](references/ids.md).
4. **A name is an identity; preserve shipped selectable names.** Every reference (`inherits`,
   `compatible_printers`, `default_*`, `printer_model`) is the exact, case-sensitive `name`. Renaming or
   deleting a shipped selectable preset, or flipping its `instantiation` from `"true"` to `"false"`,
   needs `renamed_from` (a `;`-separated string) on a selectable successor
   ([migration rules](references/vendor-bundle.md#renamed_from)); update in-tree references too.
5. **Values are strings or arrays of strings.** `"instantiation": "false"`, never `false`. A
   `machine_model`'s `nozzle_diameter` is a `;`-separated string; a `machine`'s is an array. Custom
   G-code is one string. Wrong types can abort loading of the bundle or of every vendor
   ([failure scopes](references/vendor-bundle.md#failure-scopes)).
6. **Unknown keys are dropped silently.** Confirm every new key exists in
   `src/libslic3r/PrintConfig.cpp`; a key a neighbouring file writes is no evidence it exists. `check` rejects,
   and `normalize` removes, known obsolete keys, but neither detects an arbitrary misspelling. A key
   missing from the definitions may be a legacy name the loader still renames
   (`tool_change_gcode` → `change_filament_gcode`) or whose value it rewrites (`DirectDrive` →
   `Direct Drive`); check `PrintConfigDef::handle_legacy` before removing one, and write the current name
   in new edits.
7. **Write overrides only.** Inherit the bundle's bases and restate just what differs; follow the
   bundle's existing layering and style, except that a new filament prefers the library's bases. A
   value that every preset of a group shares goes on the group's base
   ([shared bases](references/shared-bases.md)).
8. **One load error can discard a whole vendor bundle**: an unresolved `inherits`, a missing indexed
   file, two selectable presets with one name, an unknown `printer_model` or `printer_variant`, a
   filament with no resolvable `filament_id`, `nil` in a non-nullable key. `inherits` and `include`
   resolve only inside the bundle, except that filaments may inherit from OrcaFilamentLibrary.
9. **Filament compatibility names exact printer variants.** Every instantiated filament outside the
   library writes a non-empty `compatible_printers` in its own file. Library fallbacks may omit it;
   library printer-specific tunes use a non-empty list. One variant may be claimed by only one preset
   per filament product (`filament_id`); an overlap is resolved by moving the variant to the most
   specific preset, which is preferred over deleting a preset
   ([one variant, one profile](references/filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product)).
10. **One all-printer preset per product; colour is a runtime property, never a preset.** Never ship
    presets that differ only by colour. CI accepts them, so this is a review call
    ([colour](references/filament-profiles.md#colour-is-a-runtime-property)).
11. **Write a variant key at full width or not at all.** A key in the four variant sets holds exactly
    `N` values in the selectable preset that writes it, `N = S × k` in the
    [sizing equation](references/extruder-variants.md#sizing-equation): one per variant, a (normal,
    silent) pair per variant for the `machine_max_*` limits; one value is not "the same for every
    variant". Profiles must be correct as written: `check` judges each selectable preset by the
    equation, never by what the loader pads or cuts, and `check --strict` also holds what a preset
    inherits to its own width, as BBL writes it. A base is never judged on its own: its array widths
    count, under `--strict`, where they reach a preset, while the id and layout rules judge the
    composed preset, inherited values included, without it. Declare the variant layout on a
    multi-extruder printer whose extruders need different values
    ([widths](references/extruder-variants.md#widths)).
12. **Run the full checks before reporting completion.** A `--vendor` run is only a development loop.
    Review also covers version bumps, assets, non-default processes and hardware tuning, which CI cannot
    establish.

## Names

| Type | Shape | What the loader uses |
| --- | --- | --- |
| `machine_model` | `<Model>` (`Bambu Lab X1 Carbon`) | the exact string, named by each variant's `printer_model`; also the `<Model>_cover.png` stem |
| `machine` | `<Model> <nozzle> nozzle` | the exact string, named by `compatible_printers`; `printer_variant` holds the nozzle token (`0.4`, [rules](references/machine-profiles.md#printer_model-and-printer_variant)) |
| `process` | `<lh>mm <Quality> @<target>` | the exact string when referenced or selected; `@<target>` is a label, compatibility comes from the preset's list or condition |
| `filament` | `<Product> @<target>` | text before the first `@` is the alias (shadowing, `filament_id`); the rest is a label, with reserved targets `@base` and `@System` |

The shapes are convention; `check` enforces only uniqueness. Per-type conventions, base names and
filename rules are in [naming.md](references/naming.md).

## Creating or modifying a profile

1. **Inspect the diff and the neighbouring presets.** Read their `name`, parent chain and children:
   edits to a base, or to a leaf that others inherit, propagate. Match the bundle's structure and write
   only overrides. New files use tab indentation, LF and a trailing newline; preserve unrelated
   formatting in existing files. Match filename case exactly and use
   [cross-platform names](references/naming.md#filenames-and-paths).
2. **Author explicit metadata.** Set `type` yourself (`machine` vs `machine_model` especially), and use
   `"from": "system"` and a string `instantiation` on config presets. Omit ids on new presets unless
   [ids.md](references/ids.md) requires special handling; retain them on existing ones. Complete
   compatibility, defaults, assets and any rename migration using the task reference.
3. **Put shared values on shared bases** when drafting several presets, and after `fix-variant`, which
   widens an array in every preset that writes it and moves nothing. Each value goes on the base of the
   level that determines it, a new base only where it pays for itself, and a restructure must leave
   every selectable preset loading what it loaded: `snapshot` before the edit, `compare` after it
   ([shared-bases.md](references/shared-bases.md)).
4. **Bump the version**, then run the authoring commands in order for each affected bundle, reading
   every diff and resolving every error before moving on:

   ```bash
   python3 scripts/orca_profile_tool.py normalize --vendor "<Vendor>"
   python3 scripts/orca_profile_tool.py update-index --vendor "<Vendor>"
   python3 scripts/orca_profile_tool.py generate-id --vendor "<Vendor>"
   python3 scripts/orca_profile_tool.py check
   ```

   Writing commands accept `--dry-run`. `normalize` changes content and can reformat entire files;
   rerun `update-index` after any change to `inherits` or `include`, since it orders by them.
   **Do not use `trim` in this workflow:** it deletes unindexed files, including one you just added. Do
   not use `normalize --force` for routine edits. An error in a bundle you did not touch predates your
   change: confirm it on a clean checkout and report it rather than fixing it in the same change.
5. **Validate:**

   ```bash
   ./scripts/check_profile.sh --vendor "<Vendor>"   # development loop
   ./scripts/check_profile.sh                       # full tree before the PR
   ```

   On Windows use `scripts\check_profile.bat -Vendor "<Vendor>"` / `scripts\check_profile.bat`. Logs
   land in a per-user cache dir ([validation.md](references/validation.md)). Id checks stay tree-wide
   under `--vendor`, and filament-only bundles skip the default slice check. Under `--vendor` read only
   `profile_tool` and `validate_slice`: the other three checks fail on library presets that name other
   vendors' printers ([why](references/validation.md#the-five-checks)).
6. **Verify the changed behaviour.** Slice newly added non-default processes and filaments
   [explicitly](references/validation.md#checking-a-copy-of-the-tree), and
   [test in the app](references/validation.md#testing-in-the-app) for selection or UI behaviour. Report
   the checks actually run, their failures and skips, and any hardware tuning still unverified.

## Symptom → first look

| Symptom | Start here |
| --- | --- |
| A vendor disappears | the app's log or the `validate_system` log; [failure scopes](references/vendor-bundle.md#failure-scopes) |
| A setting has no effect | key spelling or a legacy name (`PrintConfigDef::handle_legacy`), value type, or a config key placed on a `machine_model` |
| A preset exists but is not selectable | index registration, `instantiation`, whether it is installed (chosen in the setup wizard, or listed in the model's `default_materials`), compatibility |
| A filament is missing, duplicated, or matches the wrong spool | [compatibility and alias shadowing](references/filament-profiles.md#compatible_printers), [ids](references/ids.md) |
| Presets differ only by colour, or an all-printer library preset lacks `@System` | [colour is a runtime property](references/filament-profiles.md#colour-is-a-runtime-property) |
| High Flow (or a second extruder) slices with Standard (or extruder 1) values; a variant switch is missing; a variant's tuned values never arrive | [extruder variants](references/extruder-variants.md#variant-strings), [widths](references/extruder-variants.md#widths), [variant names](references/validation.md#variant-names) |
| Values land on the wrong extruder or mode after a variant was added | [inserting a variant](references/extruder-variants.md#adding-a-variant-inserts-its-values-at-its-variant-index), [padding and composition](references/extruder-variants.md#padding-truncation-and-composition) |
| A resolved value matches neither the file nor its `inherits` parent; an array is not the width the file wrote, or a child got only a base's first value | [composition order and `include`](references/vendor-bundle.md#inherits-and-include) |
| A bed temperature is ignored | [the twelve plate keys](references/filament-profiles.md#bed-temperature-is-twelve-keys-not-one) |
| A change is absent from the running app | version bump and [installed profile location](references/validation.md#testing-in-the-app) |
| A check fails | [error → remedy](references/validation.md#error--remedy) |

## Source of truth

When this skill and the checkout disagree, the checkout wins: `scripts/orca_profile_tool.py` for the
tool and its flags; `src/libslic3r/Preset*.cpp` for loading and compatibility;
`src/libslic3r/PrintConfig.cpp` for keys, types, nullable options, the variant key sets and legacy
handling; `src/dev-utils/OrcaSlicer_profile_validator.cpp` and `.github/workflows/check_profiles.yml`
for validation coverage; `docs/HLSD/filament_id.md` for filament identity. The wiki's
[profile guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/how_to_create_profiles.md)
is a tutorial; confirm loader and CLI details against these sources.

## Editing this skill

The skill describes what a profile must be, not what the shipped tree currently is. State rules,
equations, patterns and profile shapes; never inventories of shipped defects, counts, dated
measurements or lists of which vendors do what. Test each sentence: if editing the profiles alone,
with the engine and tool unchanged, could make it false, state the rule behind it or give a generic
example instead. Example files named as models to copy, and commit hashes cited as the reason for a
rule, are fine.
