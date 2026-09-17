# Validating profiles

```bash
./scripts/check_profile.sh                       # everything CI runs
./scripts/check_profile.sh --vendor "<Vendor>"   # fast loop
./scripts/check_profile.sh profile_tool validate_slice   # named checks only
```

```bat
scripts\check_profile.bat                        :: the same three, on Windows
scripts\check_profile.bat -Vendor "<Vendor>"
scripts\check_profile.bat profile_tool validate_slice
```

`check_profile.bat` is a shim around `check_profile.ps1` — same checks, same order, same logs;
the flags take PowerShell spellings (`-Vendor`, `-ProfilesDir`, `-Validator`, `-Download`, `-Refresh`,
`-WorkDir`, `-LogLevel`) and positional check names are unchanged. `-p`, `-v` and `-l` are aliases, so
`-v Elegoo -l 2` reads the same on both platforms. It passes `-ExecutionPolicy Bypass` because a
default Windows client refuses to run a checked-out `.ps1` at all. The `.ps1` finds Python itself,
probing `py -3`, then `python`, then `python3`; run the tool by hand with `py -3` for the same reason.

Every check in the run happens even after an earlier one fails; the script exits non-zero if any did, and writes
`.test/check_profiles/logs/<check>.log` plus, on failure, `.test/check_profiles/pr_comment.md` — the same
report CI posts on the PR. A stale `.test/check_profiles/.lock` after a crash must be removed by hand.

## The five checks

| Check | Command it runs | Catches |
| --- | --- | --- |
| `profile_tool` | `python3 scripts/orca_profile_tool.py check` | index coverage **both ways**, preset-name collisions, files `normalize`/`update-index` would still rewrite, duplicate JSON keys, filament `compatible_printers`, `filament_type` array, conflict keys, id length, **all `setting_id` and `filament_id` rules** |
| `validate_system` | `validator -p resources/profiles -l 2` | load errors, missing filament `compatible_printers`, dangling `inherits`/`compatible_*`, duplicate `filament_id` per printer |
| `validate_slice` | `validator -p … -s -l 2` | custom G-code expansion, unresolvable printer defaults |
| `validate_filament_subtypes` | `validator -p … -l 2 -f` | nothing extra — see below |
| `validate_custom` | `validator -p <tree+fixture> -l 2` | a shipped preset name that a past release offered no longer resolving |

**`-f` is a no-op.** It is declared `po::bool_switch()->default_value(true)`, so the duplicate-`filament_id`
check runs whether or not you pass it — `validate_system` already fails on duplicates. The binary's own
`--help` ("Off unless this flag is present") does not reflect that default.

### `validate_custom` — the backward-compatibility gate

Downloads one fixture archive per past release (v1.9.0 onwards) of *generated mock* user presets —
a `<vendor>_<preset>_orca_test` copy of every system preset that
release shipped, cut with the validator's own `-g 1` mode — unpacks each over a copy of the current tree
and loads it. Each entry holds only `inherits` plus a canned diff, so the one failure it adds over
`validate_system` is a shipped preset name disappearing. (The whole current tree sits under each fixture,
so every `validate_system` error fails it too.) This is what makes a rename or an
`instantiation` flip a CI failure rather than just a user complaint, and the reason `renamed_from` is
mandatory.

### `validate_slice`

Slices a two-colour cube on every instantiable printer in the tree, sequentially, forcing the prime tower.
It selects `default_print_profile` and the first `default_filament_profile`, then updates compatibility;
that update can select a different compatible preset. Confirm the intended defaults yourself rather
than treating a passing sweep as proof that those exact presets were sliced.
A printer fails if it cannot be selected, falls back to a Default preset, throws, produces no g-code, or
emits no `CP TOOLCHANGE START`. It cannot be scoped to a filament-only vendor
(`No instantiable printer presets found for vendor OrcaFilamentLibrary`); `check_profile.sh` records it
as SKIP for a vendor with no `machine/` folder.

## `orca_profile_tool.py check`

`check` is one subcommand of the tool that also owns
`generate-id`, `normalize`, `trim`, `update-index` and `update-snapshot`; see [ids.md](ids.md) for the
writing half.

| Per vendor | Catches |
| --- | --- |
| `check_preset_name_uniqueness` | two files in one bundle claiming one type + name — indexed or not |
| `check_index_coverage` | a file on disk that no `*_list` references (**an error, not a warning**) |
| `check_name_consistency` | an index entry whose `name` disagrees with the file, or whose `sub_path` is missing |
| `check_normalized` | a file `normalize` would rewrite, and an index `update-index` would rebuild |
| `check_filament_compatible_printers` | an instantiated non-library filament with no `compatible_printers` of its own |
| `check_conflict_keys` | `extruder_clearance_radius` alongside `extruder_clearance_max_radius` |
| `check_vector_type_keys` | a vector option written as a scalar (`"filament_type": "PLA"`) |
| `check_filament_id_length` | a declared `filament_id` longer than 8 characters |
| `check_machine_default_materials` | every `default_materials` / `default_filament_profile` name resolves |
| `check_obsolete_keys` | per-key warnings for ignored options; **filament files only** |

Tree-wide, **ignoring `--vendor` entirely**: `check_setting_id_uniqueness` and `check_filament_ids`. So a
vendor-scoped run can and does fail on another vendor's files — and it saves seconds, not minutes.

Unscoped, the per-vendor pass covers every bundle. The only exclusion is the stray `user/` directory
(see below); `OrcaFilamentLibrary` is held to the same rules as any vendor, its sole exemption being
that a library filament may leave `compatible_printers` empty — exactly what
`check_filament_compatible_printers` allows. `check_normalized` covers every bundle with an index.

Notes that matter:

- Exit codes: **0** clean, **1** errors found, **2** argparse misuse. Warnings never change the exit code.
- A nonexistent `--vendor` is a hard error — `[ERROR] unknown vendor "<V>" in <dir>`, exit 1.
- `--vendor ""` means all vendors; `check_profile.sh` relies on that. `--vendor` is repeatable.
- A **stray directory** under `resources/profiles/` still gets counted as a vendor by the per-vendor pass
  and warned about (`No profiles found for vendor: <dir> at …/<dir>.json`, and the "Checked vendors" count
  goes up by one). The one exception is `user/`, the validator's data dir, which an unscoped `check`
  skips by name; `--vendor user` still checks and warns about it. Warnings never change the exit code.
  `normalize`, `trim` and `update-index` ignore strays too — they define a bundle as *a directory with a
  matching index file*.
- Each remedy is printed once for the whole run, not once per file, as a `[WARNING]` under the errors
  ("2 unreferenced file(s) above: delete them, or run … update-index"). Read those lines: they name the
  command that fixes the batch.
- The trailing summary always suggests `normalize`. That is right for the shape errors and misleading for
  everything else — an id error needs `generate-id`, a dangling `default_materials` needs a human.
- `resources/profiles/check_unused_setting_id.py` is a legacy BBL-only diagnostic, not part of
  profile CI. Use `orca_profile_tool.py check` for current id validation.

### Obsolete-key diagnostics

`check` always reports per-key warnings for obsolete options in filament profiles.
The normalization check also rejects obsolete keys across preset types; `normalize` removes them.

### Default-material references

The materials check finds `default_materials` / `default_filament_profile` entries naming a preset
that does not exist. The three authoring errors it surfaces are `,` instead of `;`, wrong case
(`@system`), and a whole `;`-joined string stuffed into one array element.

### `normalize` and `update-index` are part of the check

`check` fails when either command would still change something, so they are not optional polish — the
file that gets reviewed has to be the file that ships. What `normalize` changes is narrow and fixed:
adds a missing `type`, deletes a `version` or `is_custom_defined` key from a *preset* file, deletes six
print-speed keys from filament profiles (`initial_layer_print_speed`, `outer_wall_speed`,
`inner_wall_speed`, `infill_speed`, `top_surface_speed`, `travel_speed`), deletes the
obsolete keys in `PrintConfigDef::handle_legacy`'s `ignore` set across preset types, resolves the
`extruder_clearance_*` conflict pair by keeping the larger, arrayifies five filament options besides
`filament_type`, and hoists `type`, `name`, `renamed_from`, `inherits`, `from`, `setting_id`,
`filament_id`, `instantiation` to the front. A file it changes is then rewritten whole — tab-indented,
LF, one trailing newline, keys reordered.

**Set `type` explicitly when authoring.** For a file in `machine/` without it, normalization guesses
`machine` only if its name contains `nozzle`, otherwise `machine_model`. That heuristic cannot
reliably classify shared machine bases or unusually named variants.

The Python obsolete-key set is checked against the C++ source by a unit test. Active options
and legacy aliases that the loader migrates (such as `extruder_type` and
`extruder_clearance_max_radius`) are preserved.

Two things it therefore does **not** enforce:

- **Formatting and key order on their own.** A file with none of those problems is skipped entirely, so
  4-space indent, a missing trailing newline, and a file that leads with `compatible_printers` all pass
  `check`. They stay latent until something else trips `normalize` and the whole file reformats inside an
  unrelated diff. (`normalize --force` rewrites every file, which is not something to run on a shipped
  bundle.)
- **A misspelled setting key.** `inital_layer_height` and `sparse_infill_densiti` pass `check` cleanly.
  Verify new keys against `PrintConfig.cpp` and `PrintConfigDef::handle_legacy`.

## The validator binary

Built from `src/dev-utils/OrcaSlicer_profile_validator.cpp` (`-DORCA_TOOLS=ON`).
Both scripts find a local build under `build*/` — `check_profile.sh` tries Release, RelWithDebInfo, then
Debug, and `check_profile.ps1` adds MinSizeRel — else they download the nightly into
`.test/check_profiles/validator`. Pass `--download` / `-Download` to match CI exactly, since a stale
local build is used silently. Windows looks for `OrcaSlicer_profile_validator.exe`.

If your build lives somewhere else entirely, point at it with `--validator` / `-Validator`, or set
`ORCA_PROFILE_VALIDATOR` (`$env:ORCA_PROFILE_VALIDATOR` in PowerShell).

| Flag | Meaning |
| --- | --- |
| `-p <dir>` | profile tree (also becomes the data dir) |
| `-l <n>` | log level; CI uses 2 |
| `-v <Vendor>` | load only that vendor **plus** OrcaFilamentLibrary |
| `-s` | slice sweep |
| `-f` | no-op (see above) |
| `-g 1` | regenerate user-preset fixtures; takes a value, and wipes the user preset dir first |

On ARM64 Linux the nightly is x86-64 only — the script warns and downloads anyway, producing a binary
that will not run. Build it locally instead.

Running the validator directly uses the profile tree as its data directory and can create `user/`
there. Prefer the wrappers, which stash existing user presets and restore them afterward. After a
direct run, inspect `user/` and remove only empty directories created by that run; fixtures or
pre-existing user files may be present.

## Checking a copy of the tree

Use `--profiles DIR` on the Python tool and `-p DIR` on the validator.
`check` and `update-snapshot` describe a tree's sanctioned id state, so pointing them elsewhere also
needs `--snapshot PATH` for that tree — passing `--profiles` without it exits 2 rather than silently
judging the copy against `resources/profiles`'s snapshot.

**The wrappers' `--profiles` / `-ProfilesDir` redirects only their validator checks.** Their
`profile_tool` check still reads this checkout's `resources/profiles`. To validate a copy fully,
run the Python check separately with that tree's snapshot, then name only validator checks:

```bash
python3 scripts/orca_profile_tool.py check --profiles "<tree>" --snapshot "<snapshot.json>"
./scripts/check_profile.sh --profiles "<tree>" validate_system validate_slice validate_filament_subtypes validate_custom
```

On Windows use `py -3` and `scripts\check_profile.bat -ProfilesDir "<tree>"` with the same check names.

## Testing in the app

Editing this checkout's `resources/profiles` does not update a separately installed application.
Test with a build using the edited resources and a bumped bundle version; the updater installs newer
bundles under `<data_dir>/system/`, and the preset cache also depends on the bundle version.
Use Help ▸ Show Configuration Folder to locate the active data directory:

| Platform | Default data directory |
| --- | --- |
| macOS | `~/Library/Application Support/OrcaSlicer` |
| Linux | `$XDG_CONFIG_HOME/OrcaSlicer`, or `~/.config/OrcaSlicer` when unset |
| Windows | `%APPDATA%\OrcaSlicer` |

A portable `data_dir` next to the executable takes precedence. Use a separate test configuration
for a clean-install check; preserve the normal configuration and user presets.

## Cross-platform paths

Match the exact case of each `sub_path` and asset filename; Linux filesystems commonly distinguish
case even when a macOS or Windows checkout does not. Preset-name references are case-sensitive
on every platform. Avoid Windows-invalid characters (`< > : " | ? *`), reserved device names
such as `CON` / `NUL` (including with extensions), and trailing spaces or dots in path components.
Keep stems tidy too, but a space immediately before `.json` is not a trailing path-component space.

## Error → remedy

| Message | Fix |
| --- | --- |
| `can not find inherits <parent> for <preset>` | parent missing, unregistered, or listed **after** the child |
| `can not find filament_id for <name>` | nothing in the chain declares one — run `generate-id` |
| `can not find parent <name> for config <user preset>!` | a shipped name disappeared — add `renamed_from` |
| `Missing instantiation attribute for <name>` | key absent **or** not the string `"true"`/`"false"` |
| `contains incorrect keys: <keys>, which were removed` | a key valid for a different preset type |
| `defines invalid printer variant "<v>"` | not in the model's `nozzle_diameter` list |
| `has printer_variant "<v>" that does not match its nozzle_diameter` | the set comparison in [machine-profiles.md](machine-profiles.md) |
| `references unknown compatible_printers "<p>"` | the printer was renamed or deleted; fix the reference |
| `references renamed compatible_printers "<old>" (now "<new>")` | in-tree references must name the current preset; `renamed_from` does not excuse them |
| `Filament preset "<f>" is missing compatible_printers setting` | non-library filaments need a non-empty list in their **own** file — the flattened-vs-own-key trap is in [filament-profiles.md](filament-profiles.md#compatible_printers) |
| `Ambiguous AMS filament match: N presets share filament_id "X" … printer "Y"` | make the lists disjoint, or fix an `inherits` pointing at another material's `@base` |
| `Layer height cannot exceed nozzle diameter.` / `Line width too small` | `Print::validate()` flow rules |
| `[ERROR] … no <V>.json list references it, so it never loads` | `update-index`, or delete the file |
| `[ERROR] … references it and it declares no profile type` | set the correct `type` explicitly, then `normalize` and `update-index` |
| `[ERROR] … normalize would <change>` / `<V>.json: update-index would rebuild <lists>` | run that command and commit the result |
| `[ERROR] <V> has N <type> profiles named "<name>"` | identify the intended preset and remove or rename the duplicate; use `trim --dry-run` only for deliberate unindexed-file cleanup |
| `[ERROR] … must not have a setting_id` / `is missing a setting_id` | `generate-id --setting-id` |
| `[ERROR] filament_id "<id>" is not sanctioned by …snapshot.json` | `update-snapshot`, commit the diff |
| `inherits filament_id "X" but its own triple … mints "Y"` | `generate-id` will **not** fix this — see [ids.md](ids.md) |
| `vendor <V>'s config version: <s> invalid` | the `version` string is not Semver-parseable |
| `[json.exception.type_error.302] type must be string` | locate the non-string value in the index or model; see [failure scopes](vendor-bundle.md#failure-modes-ranked-by-blast-radius) |
| `Printer "<p>" fell back to a default preset` | final process or filament selection is a generic Default preset; check named defaults, visibility and available compatible presets. An incompatible default may instead be replaced without this error |
| `Printer "<p>" sliced but the filament change never fired` | `change_filament_gcode` never expanded |

## CI

`.github/workflows/check_profiles.yml`, job **"Check profiles"**, on `pull_request` into `main` or
`release/*`, paths `resources/profiles/**`, `resources/printers/**`, `scripts/**` and the workflow itself.
There is no push trigger — a direct push to main runs no profile validation.

The job opens with `python3 -m unittest discover -s scripts/tests -t scripts`, the tool's own unit
tests. That step is deliberately **not** `continue-on-error`: a broken tool makes everything it then says
about the profiles worthless. Every check after it is `continue-on-error` with a final gate, so one run
reports all five results. On failure a second workflow posts or replaces a single PR comment marked
`<!-- profile-validation-comment -->`, with each failing log truncated to 30 KB; it deletes the comment
once the run is green.

The job name is also the required check for the delegated-merge bot, which lets a vendor maintainer
self-merge a `resources/profiles/<Their vendor>/` PR with no human review — so whatever CI does not check
is what ships unreviewed. Its denied patterns refuse `^scripts/` and any `.py`, so a PR that must update
`scripts/filament_id_snapshot.json` always needs a maintainer.
