---
name: orca-profiles
description: Use when adding support for a new printer, printer brand, nozzle size or filament material to OrcaSlicer, or when creating, modifying or reviewing system profiles under resources/profiles — the vendor bundle index, setting_id, filament_id or filament_id_snapshot.json, renaming or retiring a shipped preset. Also for diagnosing "my vendor/printer vanished from Orca", "this setting has no effect", "the preset does not show up", "the wrong filament matches in the AMS", or a failing profile check (check_profile.sh, orca_profile_tool.py, OrcaSlicer_profile_validator, the "Check profiles" job). FFF only.
---

# OrcaSlicer system profiles

Shipped profiles live in `resources/profiles/`. A **vendor bundle** is a `<Vendor>.json` index plus a
`<Vendor>/` folder, one per printer manufacturer, plus `OrcaFilamentLibrary` (the shared filament
bundle) and `blacklist.json` (no folder, deliberately). Four record kinds: `machine_model` (a printer
product), `machine` (a selectable printer variant), `process` (quality preset) and `filament`.

**The index is the loader's only entry point.** A JSON file in a vendor folder that no `*_list` names is
never read *as a preset* — no error, no warning, it simply does not exist. The only sanctioned
exceptions are `BBL/cli_config.json` and `BBL/filament/filaments_color_codes.json`, which the app loads
by path and the tooling knows are data, not presets. Every other unindexed file is a `check` error.

For the human-facing tutorial see the
[profile development guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/how_to_create_profiles.md);
it is wrong or silent on several points — see [Where the wiki is wrong](#where-the-wiki-is-wrong).

## Golden rules

1. **Bump `version` in `resources/profiles/<Vendor>.json` for every bundle you touch.** Increment only
   the last component (`02.04.00.03` → `02.04.00.04`) and never past `99` — the 4th component is folded
   in as `patch*100 + value`, so `x.y.0.100` compares equal to `x.y.1.0`. Without a bump `PresetUpdater`
   never reinstalls the bundle (`vendor_ver < resource_ver`, strictly less) and the local `.opc` preset
   cache still "covers" the edit. **Nothing in CI checks this**, and it is the step merged profile PRs
   forget most often.
2. **Register every file in the index, bases included, parents before children.** The `*_list` arrays are
   **order-sensitive**: `inherits` resolves against a map filled in list order, so a parent listed after
   its child fails to resolve and takes the whole bundle down.
   `python3 scripts/orca_profile_tool.py update-index --vendor <Vendor>` writes the lists from the files
   on disk, parents first; `check` fails if you skipped it, if a file is unregistered, or if two files in
   the bundle claim one preset name.
3. **Never hand-write `setting_id` or `filament_id`.** Author without the key and run
   `python3 scripts/orca_profile_tool.py generate-id`. Both are deterministic hashes of the preset's identity;
   a copied id is a CI error and a wrong one breaks AMS spool matching. See [references/ids.md](references/ids.md).
4. **One bad preset discards the entire vendor bundle** — every printer, process and filament of that
   vendor disappears. A bad `inherits`, a missing `sub_path` file, a duplicate name, an unknown
   `printer_model`/`printer_variant` or a filament with no resolvable `filament_id` all do this.
5. **Never rename, delete, or flip a shipped `"instantiation": "true"` preset to `"false"` without
   `renamed_from` on a successor.** A non-instantiated preset is not in the preset collection at all, so
   every user preset inheriting it dies with `can not find parent <name> for config <file>!`. CI's
   `validate_custom` replays released user-preset archives (v1.9.0 onwards) and will fail.
6. **`compatible_printers` lists printer *variant* names** (`"Phrozen Arco 0.4 nozzle"`), never model
   names or filenames. Empty or absent means *compatible with every printer* — legitimate only for
   OrcaFilamentLibrary filaments; an error for any other vendor's filament.
7. **Every value is a string or an array of strings.** `"printable_height": "300"`, not `300`;
   `"instantiation": "false"`, not `false`; `"filament_type": ["PLA"]`, not `"PLA"`. A raw JSON number in
   a preset is dropped with only a log line, but two places throw an uncaught `nlohmann` type error that
   aborts the preset load for **every** vendor, leaving the user with no system profiles at all: a
   non-string `version`, `name` or `url` at the top level of a vendor index, and a non-string
   `nozzle_diameter` on a `machine_model`. **The two `nozzle_diameter` keys have different types** — the
   model's is a `;`-separated string (`"0.4;0.6"`), the variant's is an array (`["0.4"]`). (A non-string
   inside a `*_list` entry is caught and counted instead.)
8. **A misspelled setting key is silently discarded.** `handle_legacy` blanks any key
   `PrintConfigDef` does not know — no error at any log level. Only a key valid for a *different* preset
   type is reported. Check spellings against `src/libslic3r/PrintConfig.cpp`, and note that
   `handle_legacy` also has an explicit `ignore` set that drops keys which *do* appear there. Dead keys
   ship in bulk across the tree, so a neighbouring file carrying a key is no evidence the key is real.
9. **Validate with `./scripts/check_profile.sh`** (Windows: `.\scripts\check_profile.bat`) before you
   claim done. It is the exact local twin of the CI job.

## Working on macOS, Linux or Windows

The tree, the checks and every rule above are identical on all three. Only invocation differs.

| | macOS / Linux | Windows |
| --- | --- | --- |
| Full check | `./scripts/check_profile.sh` | `scripts\check_profile.bat` |
| …its flags | `--vendor "<V>"`, `--profiles`, `--download`, `--log-level` | `-Vendor "<V>"`, `-ProfilesDir`, `-Download`, `-LogLevel` |
| The tool | `python3 scripts/orca_profile_tool.py <cmd>` | `py -3 scripts\orca_profile_tool.py <cmd>` |
| Orca's data dir (step 7) | `~/Library/Application Support/OrcaSlicer`, or `${XDG_CONFIG_HOME:-~/.config}/OrcaSlicer` on Linux | `%APPDATA%\OrcaSlicer` |

- **Use `py -3` on Windows.** `python3` is rarely on PATH there and a bare `python` is often the
  Microsoft Store stub, which exits non-zero — see [validation.md](references/validation.md) for how the
  scripts probe for it.
- `orca_profile_tool.py` resolves `resources/profiles` relative to *its own* path, so invoking it by
  absolute path acts on that checkout from any working directory.
- **Name files so every platform can check them out.** No trailing space or dot before `.json`, and none
  of `< > : " | ? *` — Windows cannot represent those, so a `sub_path` naming one is unreachable there
  however correct the JSON is. Nothing checks it, and the tree already ships violations
  (`Eryone/machine/ER20_Klipper/Eryone ER20 Klipper 0.N nozzle .json` — trailing space in the filename
  *and* in `Eryone.json`).
- **Case: your checkout is probably case-insensitive, CI's Linux runner is not.** A `sub_path`, or a
  `bed_model` / `bed_texture` / `hotend_model` filename, that differs from the real file only in case
  resolves on macOS and Windows and fails on Linux. (`inherits` and `compatible_printers` are string
  matches in a map — case-sensitive on every platform, so those break for you too.)

## Which change am I making?

| Goal | What to touch | Reference |
| --- | --- | --- |
| New generic material for all printers | `OrcaFilamentLibrary/filament/`, its index, version | [filament-profiles.md](references/filament-profiles.md) |
| New filament brand or product | `OrcaFilamentLibrary/filament/<Brand>/` — `@base` root + `@System` | [filament-profiles.md](references/filament-profiles.md) |
| Brand's tune for one printer | `OrcaFilamentLibrary/filament/<Brand>/<PrinterVendor>/` preferred, or `<PrinterVendor>/filament/<Brand>/` | [filament-profiles.md](references/filament-profiles.md#where-a-filament-goes) |
| Printer vendor tuning a generic | `<Vendor>/filament/`, keep the `Generic X` base name, non-empty `compatible_printers` | [filament-profiles.md](references/filament-profiles.md) |
| New printer in an existing bundle | model + variants + processes + index entries + assets | [machine-profiles.md](references/machine-profiles.md#adding-a-printer-to-an-existing-bundle), [process-profiles.md](references/process-profiles.md) |
| A whole new vendor bundle | `<Vendor>.json` + `<Vendor>/{machine,process}/` + bases | [vendor-bundle.md](references/vendor-bundle.md#starting-a-whole-new-vendor-bundle) |
| New nozzle size on an existing printer | model's `nozzle_diameter` list, a variant, per-nozzle process(es), filament compatibility | [machine-profiles.md](references/machine-profiles.md#adding-a-nozzle-variant), [process-profiles.md](references/process-profiles.md) |
| New quality tier | one process leaf (+ a per-nozzle base if none exists) | [process-profiles.md](references/process-profiles.md#adding-a-quality-tier-or-a-nozzles-processes) |
| Rename / retire a preset | `renamed_from`, re-mint ids, index, version | [review-checklist.md](references/review-checklist.md) |

## Creating or modifying a profile

1. **Read the neighbours first.** Copy the shape of the bundle you are editing, not a different vendor's.
   Read the `name` field — plenty of preset files have a filename that disagrees with it, so never trust
   the path. Profile JSON is tab-indented, LF, one trailing newline — match it by hand. `normalize`
   writes that shape but **only rewrites a file it already has a reason to change**, so a space-indented file passes
   `check` today and then produces a whole-file diff the day something does trip `normalize`. Keep the
   filename checkout-safe on every platform — see [above](#working-on-macos-linux-or-windows).
2. **Write the profile with no `setting_id` and no `filament_id`.** Put shared values in a base
   (`"instantiation": "false"`, no `setting_id`) and only the deltas in the selectable leaf.
3. **Register it** in the right `*_list` of `<Vendor>.json` with a `sub_path` relative to the vendor
   folder, positioned after its parent — or let `update-index` do it in step 5. Either way the committed
   index has to equal what that command writes.
4. **Bump the bundle `version`** (rule 1) — including `OrcaFilamentLibrary.json` for a library change.
5. **Run the tool, in this order** — each step feeds the next:
   ```bash
   python3 scripts/orca_profile_tool.py normalize    --vendor <Vendor>   # canonical shape; writes the "type"
   python3 scripts/orca_profile_tool.py update-index --vendor <Vendor>   # rebuild the lists, parents first
   python3 scripts/orca_profile_tool.py generate-id  --vendor <Vendor>   # --dry-run to preview
   python3 scripts/orca_profile_tool.py update-snapshot                  # whenever an id OR a claim changed
   python3 scripts/orca_profile_tool.py check
   ```
   Any new filament preset needs the snapshot updated, even one that mints no new id: adding a vendor's
   `Generic PETG @…` adds the claim `<Vendor>/Generic PETG`, and `check` fails until you do. Commit
   `scripts/filament_id_snapshot.json` in the same commit as the profiles.
   **Do not run `trim`** as part of this: it deletes every file the index does not list, which includes
   the one you just added and have not registered.
6. **Validate:**
   ```bash
   ./scripts/check_profile.sh --vendor "<Vendor>"   # fast loop
   ./scripts/check_profile.sh                       # full tree, before opening the PR
   ```
   A vendor-scoped run is **not** a complete check: the id passes always run tree-wide, and
   `validate_slice` is skipped for a vendor with no `machine/` folder. Exit codes are 0 clean, 1 errors,
   2 argparse misuse; logs land in `.test/check_profiles/logs/<check>.log`. A failing message maps to its
   fix in [error → remedy](references/validation.md#error--remedy); every flag, every check, and working
   on a copy of the tree, is in [validation.md](references/validation.md).
7. **Try it in the app** for anything behavioural. Editing `resources/profiles` does not update an
   installed OrcaSlicer — it reads `<data_dir>/system/` (the data dirs in the table above), unless a
   `data_dir` folder sits next to the executable, which wins. Bump the version, or delete that folder (Help ▸ Show Configuration Folder).

## Reviewing a profile change

Work through [references/review-checklist.md](references/review-checklist.md) — ordered by how often
each item actually goes wrong in this repo, and it opens with the table of what CI cannot see: the
version bump, key spellings, asset paths, `default_materials`, non-default processes, cross-platform
filenames and more. What CI *does* run is in [validation.md](references/validation.md).

## Red flags — stop and re-read the rules

- "CI is green so the change is complete" → the version bump, key spellings and asset paths are not in CI.
- "`--vendor` passed, so we're done" → run the unscoped script before the PR.
- Emptying a filament's `compatible_printers` to make it apply everywhere → **rule 6**; it is an error
  outside the library and creates a duplicate-`filament_id` collision against the library generic.
- Hand-formatting a profile, or hand-sorting an index, to make it look like its neighbours → that is
  `normalize` and `update-index`'s job, and `check` compares against them, not against your judgement.

## Symptom → likely cause

| Symptom | Look at |
| --- | --- |
| A vendor's printers vanished from Orca, no dialog | The whole bundle was discarded (rule 4). The error is only in the log and in `validate_system` — see the failure table in [vendor-bundle.md](references/vendor-bundle.md#failure-modes-ranked-by-blast-radius) |
| A setting has no effect | Rule 8 — misspelled key, or a key on the `machine_model` record, or an obsolete name |
| A preset exists on disk but is not selectable | Unregistered in the index, or `instantiation` is not `"true"` |
| A filament is missing on one printer | Alias-shadowed by a vendor preset of the same base name |
| A filament appears twice, or the AMS picks the wrong spool | Duplicate `filament_id`, or overlapping `compatible_printers` |
| A bed temperature is ignored | The wrong plate key for this printer's `default_bed_type` |
| It works locally but not for users | The `version` was not bumped |

To identify a `filament_id` from an error message, grep it in `scripts/filament_id_snapshot.json` — each
entry carries the triple it was minted from and one `<vendor folder>/<name-before-@>` claim per bundle
shipping that product (so `BBL/Generic PLA` stands for every `Generic PLA @…` file in BBL, not one per
file). It is exhaustive: every id declared anywhere in the tree is in it.

## Where the wiki is wrong

The [wiki guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/how_to_create_profiles.md)
is the right place to start, but two of its statements are wrong:

- **`from`.** The vendor-bundle loader never reads it — a shipped preset can even say `"from": "User"`.
  Keep `"from": "system"` anyway: the CLI's `--load-settings` rejects anything else.
- **Quality words** are "Standard, Fine, Fast or Draft". `Fast` is barely used; the real ladder is
  Extra Fine / Fine / Optimal / Standard / Draft / Extra Draft, and the word encodes a
  layer-height/nozzle ratio.

It is silent on `instantiation: "false"` deleting a name, on `renamed_from` being a `;`-separated list,
and on the version bump having no CI check — all covered above. Its "The Profile Tool" section is
accurate and worth reading alongside [ids.md](references/ids.md).

## Reference files

| File | Contents |
| --- | --- |
| [vendor-bundle.md](references/vendor-bundle.md) | The index, `version` semantics, registration, every whole-bundle load failure |
| [machine-profiles.md](references/machine-profiles.md) | `machine_model` vs `machine`, `printer_variant`, assets, multi-extruder |
| [process-profiles.md](references/process-profiles.md) | Naming ladder, base layering, compatibility, per-nozzle numbers |
| [filament-profiles.md](references/filament-profiles.md) | OrcaFilamentLibrary, brands, alias shadowing, `nil`, nozzle scaling |
| [ids.md](references/ids.md) | `setting_id` and `filament_id` tooling, the snapshot, BBL's exception |
| [validation.md](references/validation.md) | Every check, every flag, error → remedy |
| [review-checklist.md](references/review-checklist.md) | Ordered reviewer checklist with the evidence for each item |

`docs/HLSD/filament_id.md` is the authoritative design document for `filament_id`; read it before
changing anything about filament identity.
