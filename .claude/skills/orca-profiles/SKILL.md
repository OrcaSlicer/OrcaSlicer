---
name: orca-profiles
description: Use when creating, modifying, reviewing or debugging OrcaSlicer FFF system profiles under resources/profiles, including printer/vendor/nozzle/material additions, bundle indexes and versions, preset renames, setting_id and filament_id. Also use for missing presets or vendors, ignored profile settings, ambiguous AMS filament matches, and failures from orca_profile_tool.py, check_profile.sh/.bat, OrcaSlicer_profile_validator or the Check profiles CI job.
---

# OrcaSlicer system profiles

A bundle is `resources/profiles/<Vendor>.json` plus `<Vendor>/`. The vendor id is the
filename stem, not the index's display `name`. The index is the loader's only entry point:
unindexed presets never load. `OrcaFilamentLibrary` is the shared filament bundle;
`blacklist.json` is data, not a bundle.

## Choose the reference for the task

Read the relevant reference before editing; load others only when the task crosses those areas.
Paths below are relative to this skill. Commands run from the repository root.

| Task | Read |
| --- | --- |
| Add or tune a filament, brand or material; fix compatibility / alias shadowing | [filament-profiles.md](references/filament-profiles.md) |
| Add a printer or nozzle; change models, variants, assets or extruder vectors | [machine-profiles.md](references/machine-profiles.md) |
| Add a quality tier or tune a process | [process-profiles.md](references/process-profiles.md) |
| Create a vendor bundle; diagnose loading or inheritance; migrate preset names | [vendor-bundle.md](references/vendor-bundle.md) |
| Name a preset; check what a name must equal | [naming.md](references/naming.md) |
| Change ids; diagnose AMS identity | [ids.md](references/ids.md), then `docs/HLSD/filament_id.md` for identity changes |
| Review a profile diff | [review-checklist.md](references/review-checklist.md) |
| Run checks, interpret failures, test another tree or verify in the app | [validation.md](references/validation.md) |

## Golden rules

1. **Bump every changed bundle's `version`**, including `OrcaFilamentLibrary.json` when affected.
   Increment the last component; carry `.99` into the third component (`02.04.00.99` →
   `02.04.01.00`). The updater requires a strictly newer version. CI does not check this.
2. **Register every preset, bases included, parents before children.** `update-index` generates
   the four `*_list` arrays; `check` requires its output. Index names must equal file `name` fields.
3. **Generate ids; never invent or copy them.** Keep existing ids during ordinary tuning. New
   presets normally omit them until `generate-id`; bases must have no `setting_id`.
   BBL's authoritative `setting_id` and a wrongly inherited `filament_id` need the explicit
   handling in [ids.md](references/ids.md).
4. **Load failures can discard a whole vendor bundle.** Broken `inherits`, missing indexed files,
   duplicate names, invalid model/variant references and unresolved filament ids affect more than
   the edited preset. Inheritance stays within a bundle, except filaments may inherit the library.
5. **Preserve shipped selectable names.** Renaming, deleting or changing `instantiation` from
   `"true"` to `"false"` needs `renamed_from` on a selectable successor. It is a `;`-separated string;
   update in-tree references too. See [migration rules](references/vendor-bundle.md#renamed_from).
6. **Compatibility uses exact printer variant names.** Every instantiated non-library filament
   needs a non-empty `compatible_printers` in its own file. Library fallbacks may omit it;
   library printer-specific tunes use a non-empty list. One variant may be claimed by only one
   profile per filament product (`filament_id`); an overlap is resolved by moving the variant to the
   most specific preset, which is preferred over deleting a profile. See
   [one variant, one profile](references/filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product).
7. **Preset values are strings or arrays of strings.** Use `"instantiation": "false"`, not `false`.
   Model `nozzle_diameter` is a `;`-separated string; machine `nozzle_diameter` is an array.
   Wrong types can abort loading; see [failure scopes](references/vendor-bundle.md#failure-modes-ranked-by-blast-radius).
8. **Verify setting keys against the code.** Unknown keys are silently discarded. Check
   `PrintConfig.cpp` definitions and `PrintConfigDef::handle_legacy`; neighbours can contain dead
   keys. `normalize` removes known obsolete keys, but does not detect arbitrary misspellings.
9. **Run the full profile checks before reporting completion.** A vendor-scoped pass is only a
   development loop. Review also covers version bumps, assets, non-default processes and hardware
   tuning that CI cannot establish.

## Creating or modifying a profile

1. **Inspect the diff and neighbouring presets.** Read their `name`, parent chain and children;
   edits to a base or a leaf with descendants propagate. Match the bundle's structure and write
   only overrides. New files use tab indentation, LF and a trailing newline; preserve unrelated
   formatting in existing files. Match filename case exactly and use cross-platform names.
2. **Author explicit metadata.** Set `type` yourself, especially for `machine` vs `machine_model`.
   Use `"from": "system"` and string `instantiation` on config presets. Omit ids on new presets
   unless [ids.md](references/ids.md) requires special handling; retain them on existing ones.
   Complete compatibility, defaults, assets and any rename migration using the task reference.
3. **Bump the version**, then run the authoring commands in order for each affected bundle:

   ```bash
   python3 scripts/orca_profile_tool.py normalize --vendor "<Vendor>"
   python3 scripts/orca_profile_tool.py update-index --vendor "<Vendor>"
   python3 scripts/orca_profile_tool.py generate-id --vendor "<Vendor>"
   python3 scripts/orca_profile_tool.py check
   ```

   Writing commands support `--dry-run`. Inspect their diffs: `normalize` changes content and can
   reformat entire files. Stop and resolve command errors before proceeding.

   **Do not use `trim` in this workflow:** it can delete newly authored, unindexed profiles.
   Do not use `normalize --force` for routine edits.
4. **Validate:**

   ```bash
   ./scripts/check_profile.sh --vendor "<Vendor>"   # development loop
   ./scripts/check_profile.sh                       # full tree before the PR
   ```

   On Windows use `py -3` instead of `python3`, and `scripts\check_profile.bat -Vendor "<Vendor>"`
   / `scripts\check_profile.bat`. Logs land in a per-user cache dir (see
   [validation.md](references/validation.md)).
   Id checks remain tree-wide under `--vendor`; filament-only bundles skip the default slice check.
   See [validation.md](references/validation.md) for flags, coverage and error remedies.
5. **Verify the changed behavior.** Slice newly added non-default processes explicitly, and
   [test in the app](references/validation.md#testing-in-the-app) for selection or UI behavior.
   Report checks actually run, failures/skips and any hardware tuning still unverified.

## Symptom → first reference

| Symptom | Start here |
| --- | --- |
| A vendor disappears | Loader log / `validate_system`; [bundle failure scopes](references/vendor-bundle.md#failure-modes-ranked-by-blast-radius) |
| A setting has no effect | Key spelling/type, `handle_legacy`, or a config key placed on a `machine_model` |
| A preset exists but is not selectable | Index registration, `instantiation`, installation and compatibility |
| A filament is missing, duplicated, or matches the wrong spool | [Compatibility and alias shadowing](references/filament-profiles.md#compatible_printers); [ids](references/ids.md) |
| A bed temperature is ignored | [Plate-specific temperature keys](references/filament-profiles.md#bed-temperature-is-twelve-keys-not-one) |
| A change is absent from the running app | Version bump and [installed profile location](references/validation.md#testing-in-the-app) |
| A check fails | [Error → remedy](references/validation.md#error--remedy) |

## Source of truth

When guidance and behavior disagree, inspect the current checkout:
`scripts/orca_profile_tool.py` for tooling and flags; `src/libslic3r/Preset*.cpp` for loading and
compatibility; `src/libslic3r/PrintConfig.cpp` for setting types and legacy handling;
`src/dev-utils/OrcaSlicer_profile_validator.cpp` and `.github/workflows/check_profiles.yml` for
validation coverage. `docs/HLSD/filament_id.md` defines filament identity. The
[profile development guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/how_to_create_profiles.md)
is a tutorial; confirm loader and CLI details against these sources.
