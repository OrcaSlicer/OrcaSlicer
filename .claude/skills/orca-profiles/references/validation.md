# Validating profiles

```bash
./scripts/check_profile.sh                               # everything CI runs
./scripts/check_profile.sh --vendor "<Vendor>"           # development loop
./scripts/check_profile.sh profile_tool validate_slice   # named checks only
```

```bat
scripts\check_profile.bat                        :: the same three, on Windows
scripts\check_profile.bat -Vendor "<Vendor>"
scripts\check_profile.bat profile_tool validate_slice
```

`check_profile.bat` is a shim around `check_profile.ps1`: same checks, same order, same logs. Its flags
take PowerShell spellings (`-Vendor`, `-ProfilesDir`, `-Validator`, `-Download`, `-Refresh`,
`-WorkDir`, `-LogLevel`), and positional check names are unchanged. `-p`, `-v` and `-l` are aliases,
so `-v Elegoo -l 2` reads the same on both platforms. It passes `-ExecutionPolicy Bypass` because a
default Windows client refuses to run a checked-out `.ps1` at all. The `.ps1` finds Python itself,
probing `py -3`, then `python`, then `python3`; run the tool by hand with `py -3` for the same reason.

Every check runs even after an earlier one fails; the script exits non-zero if any failed, and writes
`logs/<check>.log` plus, on failure, `pr_comment.md` (the same report CI posts on the PR) under a
per-user cache dir:

| Platform | Cache dir |
| --- | --- |
| macOS | `~/Library/Caches/orca-profile-check` |
| Linux | `${XDG_CACHE_HOME:-~/.cache}/orca-profile-check` |
| Windows | `%LOCALAPPDATA%\orca-profile-check` |

It is named apart from OrcaSlicer's own per-user dirs and sits outside the checkout, so every worktree
shares one copy and each run overwrites its `logs/`. `--work-dir` / `-WorkDir` overrides it.

When other worktrees or agents may run checks too, pass `--work-dir <a dir of your own>` from the start
and capture the console output yourself: the shared `logs/` can belong to another run by the time you
read them. With `--work-dir`, also pass `--validator` pointing at the cached nightly, so the new dir
does not download it again:

| Platform | Cached validator |
| --- | --- |
| Linux | `<cache dir>/validator/OrcaSlicer_profile_validator` |
| macOS | `<cache dir>/validator/OrcaSlicer_profile_validator.app/Contents/MacOS/OrcaSlicer_profile_validator` |
| Windows | `<cache dir>\validator\OrcaSlicer_profile_validator.exe` |

Copying the cached `profile-fixtures/` into the new dir reuses the fixture archives; the fixture
`manifest.json` is still downloaded on every run, so `validate_custom` needs the network either way.
`another run is using <dir>` means a live run holds `<dir>/.lock`: leave it and use your own
`--work-dir`. Only a `.lock` with no `check_profile` process alive is a crash leftover; delete it by
hand.

## The five checks

| Check | Command it runs | Catches |
| --- | --- | --- |
| `profile_tool` | `python3 scripts/orca_profile_tool.py check` | index coverage **both ways**, preset-name collisions, files `normalize` / `update-index` would still rewrite, duplicate JSON keys, filament `compatible_printers`, `filament_type` array, conflict keys, variant strings and array widths and layout keys, dangling `default_materials`, id length, **all `setting_id` and `filament_id` rules** ([below](#orca_profile_toolpy-check)) |
| `validate_system` | `validator -p resources/profiles -l 2` | load errors, missing filament `compatible_printers`, dangling `inherits` / `compatible_*`, duplicate `filament_id` per printer (`Ambiguous AMS filament match`), printer defaults that name no compatible system filament |
| `validate_slice` | `validator -p … -s -l 2` | custom G-code expansion and unresolvable printer defaults, by slicing |
| `validate_filament_subtypes` | `validator -p … -l 2 -f` | nothing extra; see below |
| `validate_custom` | `validator -p <tree + fixture> -l 2` | a shipped preset name that a past release offered no longer resolving |

**`-f` is a no-op.** It defaults to on, so the duplicate-`filament_id` check runs whether or not you
pass it, and `validate_system` already fails on duplicates. The binary's own `--help` ("Off unless this
flag is present") does not reflect that default.

**A `--vendor` run reads differently from CI.** The validator's `-v` loads that vendor plus
OrcaFilamentLibrary and nothing else, so every library tune whose `compatible_printers` names another
vendor's printers fails `validate_system`, `validate_filament_subtypes` and each `validate_custom`
fixture with thousands of `references unknown compatible_printers "Bambu Lab …"` lines. Under
`--vendor`, `profile_tool` and `validate_slice` are the meaningful results; for the other three, filter
the log for your vendor's files and treat only those lines as findings. The unscoped run is the CI
result; run it before the PR.

### `validate_custom`: the backward-compatibility gate

It downloads one fixture archive per past release (v1.9.0 onwards) of *generated mock* user presets: a
`<vendor>_<preset>_orca_test` copy of every system preset that release shipped, cut with the
validator's own `-g 1` mode. It unpacks each over a copy of the current tree and loads it. Each entry
holds only `inherits` plus a canned diff, so the one failure it adds over `validate_system` is a shipped
preset name disappearing. This is what makes a rename, a deletion or an `instantiation` flip a CI
failure rather than just a user complaint, and the reason `renamed_from` is mandatory.

The whole current tree sits under each fixture, so every `validate_system` error fails
`validate_custom` too: fix `validate_system` first. Under `--vendor` it copies only the top-level index
files, `<Vendor>/` and `OrcaFilamentLibrary/`, and picks fixtures by the index's display `name`
(`Bambulab` for `BBL`), not the file stem; fixture presets without that prefix are covered only by an
unscoped run, and it warns `validate_custom checked nothing` when none match.

### `validate_slice`

It slices a two-colour cube on every instantiable printer in the tree, sequentially, forcing the prime
tower. It selects `default_print_profile` and the first `default_filament_profile`, then updates
compatibility; that update can select a different compatible preset. Confirm the intended defaults
yourself rather than treating a passing sweep as proof that those exact presets were sliced.

A printer fails if it cannot be selected, falls back to a Default preset, throws, produces no G-code,
or emits no `CP TOOLCHANGE START` (`change_filament_gcode` never expanded). Non-default processes and
filaments get no dedicated coverage; [slice them on a copy](#checking-a-copy-of-the-tree). In the
default set, a bundle without a `machine/` folder is recorded as SKIP; naming `validate_slice`
explicitly for it fails (`No instantiable printer presets found for vendor OrcaFilamentLibrary`). The
validator logs `[error]` lines that do not fail a check (such as `could not found extruder_type`); only
each check's PASS or FAIL counts.

## `orca_profile_tool.py check`

`check` is one subcommand of the tool that also owns `fix-variant`, `generate-id`, `normalize`,
`trim` and `update-index`; [ids.md](ids.md#the-tool) has the writing half.

| Catches | Scope | Function in the tool |
| --- | --- | --- |
| two files in one bundle claiming one type + name, indexed or not | per vendor | `check_preset_name_uniqueness` |
| a file on disk that no `*_list` references (**an error, not a warning**) | per vendor | `check_index_coverage` |
| an index entry whose `name` disagrees with the file, or whose `sub_path` is missing | per vendor | `check_name_consistency` |
| a file `normalize` would rewrite, an index `update-index` would rebuild | per vendor | `check_normalized` |
| duplicate JSON keys in a file | every file read | the JSON loader |
| an instantiated non-library filament with no non-empty `compatible_printers` of its own | per vendor | `check_filament_compatible_printers` |
| `extruder_clearance_radius` alongside `extruder_clearance_max_radius` | per vendor | `check_conflict_keys` |
| a scalar `filament_type` (`"filament_type": "PLA"`); the five other filament vectors `normalize` arrayifies surface as `normalize would convert <field> to an array` | per vendor | `check_vector_type_keys`, `check_normalized` |
| a variant string the two enums cannot build (a dead variant, a legacy spelling included), a variant list naming one variant twice, an `extruder_type`, `nozzle_volume_type` or `default_nozzle_volume_type` that is not an enum name ([variant names](#variant-names)) | per vendor | `check_variant_names` |
| a variant array not exactly `variant length × stride` wide in a selectable preset that writes it (with `--strict`, also in one it reaches); machine variant layout keys that disagree; a process id array that does not pair each variant ([variant arrays](#variant-arrays)) | per vendor | `check_variant_arrays` |
| a declared `filament_id` longer than 8 characters | per vendor | `check_filament_id_length` |
| a `default_materials` name, or a `default_filament_profile` name, matching no filament file ([below](#default-material-references)) | per vendor | `check_machine_default_materials` |
| per-key warnings for ignored options, **filament files only** | per vendor | `check_obsolete_keys` |
| `setting_id` uniqueness, every `filament_id` rule, and `machine_model` names duplicated across bundles | **tree-wide, ignoring `--vendor` entirely** | `check_setting_id_uniqueness`, `check_filament_ids`, `check_machine_model_name_uniqueness` |

Because the id and model-name checks stay tree-wide, a vendor-scoped run can and does fail on another
vendor's files, and it saves seconds, not minutes.

Unscoped, the per-vendor pass covers every bundle. The only exclusion is the stray `user/` directory
(below); OrcaFilamentLibrary is held to the same rules as any vendor, its sole exemption being that a
library filament may leave `compatible_printers` empty. `check_normalized` covers every bundle with an
index.

Notes that matter:

- Exit codes: **0** clean, **1** errors found, **2** argparse misuse. Warnings never change the exit
  code.
- A nonexistent `--vendor` is a hard error: `[ERROR] unknown vendor "<V>" in <dir>`, exit 1.
- `--vendor ""` means all vendors; `check_profile.sh` relies on that. `--vendor` is repeatable
  (`check --vendor A --vendor OrcaFilamentLibrary`); `check_profile.sh` takes one.
- A **stray directory** under `resources/profiles/` still gets counted as a vendor by the per-vendor
  pass and warned about (`No profiles found for vendor: <dir> at …/<dir>.json`, and the "Checked
  vendors" count goes up by one): usually an emptied folder, or a `user/` left by a direct validator
  run. An unscoped `check` skips `user/` by name; `--vendor user` still checks and warns about it.
  `normalize`, `trim` and `update-index` ignore strays too: they define a bundle as *a directory with a
  matching index file*.
- Each remedy is printed once for the whole run, not once per file, as a `[WARNING]` under the errors
  (`2 unreferenced file(s) above: delete them, or run … update-index`). Read those lines: they name the
  command that fixes the batch.
- When there are errors or warnings, the trailing summary suggests `normalize`. That is right for the
  shape errors and misleading for everything else: an id error needs `generate-id`, a dangling
  `default_materials` needs a human.
- Other options: `--dry-run` on every writing command, `--profiles DIR` to point any command at another
  tree, `--profile-type` to narrow `normalize`, `trim` and `update-index` to one type
  ([ids.md](ids.md#the-tool)).
- `resources/profiles/check_unused_setting_id.py` is a legacy BBL-only diagnostic, not part of
  profile CI. Use `orca_profile_tool.py check` for current id validation.

### Obsolete keys

`check` always reports per-key warnings for obsolete options in filament profiles. Its normalization
check also rejects obsolete keys across all preset types (`normalize would remove <key>`); `normalize`
removes them.

### Default-material references

The materials check finds `default_materials` / `default_filament_profile` entries naming a preset that
does not exist. It reads each `machine/` file's own key (a model's `default_materials`, a variant's
`default_filament_profile`; a file that writes both is checked on `default_materials` only) and accepts
any `name` found in the vendor's or OrcaFilamentLibrary's `filament/` files, bases and unindexed files
included. The three authoring errors it surfaces are `,` instead of `;`, wrong case (`@system`), and a
whole `;`-joined string stuffed into one array element. Only the validator (`validate_system`) requires
an instantiated system filament that is compatible with each variant.

### Variant arrays

`check_variant_arrays` composes every selectable preset the loader's way (the parent, then each
`include` in order, then the file's own keys; a filament's `inherits` may fall through to
OrcaFilamentLibrary) and holds each key of [the four variant sets](extruder-variants.md#the-four-key-sets)
that the preset writes itself to exactly its `variant length × stride`
([widths](extruder-variants.md#widths)). The variant length is the length of the composed preset's
own `*_extruder_variant` list; without one, a machine's is the number of variants its
`extruder_variant_list` offers, else its extruder count (the list's default is one
`Direct Drive Standard` per extruder), and a process's or filament's is 1. Any other width is an
error, one value included and even when every value is the same. A key the preset does not write
takes what reaches it, the default or an array it inherits or includes, which the loader resizes; it
is not checked. A base is not judged on its own: what it writes counts only where it reaches a preset
that does not override it.

`check --strict` also holds every selectable preset to its own width for each key that reaches it,
so a preset whose variants differ from those of the file its array comes from restates the array
(the error names that file). That is BBL's practice and the target for new printer-specific presets;
CI runs `check` without `--strict`.

An id array that reaches a selectable preset, `printer_extruder_id` or `print_extruder_id`, written or
inherited, must have one entry per entry of its variant list on any printer. This rule and the
machine layout rules below judge the composed preset without `--strict`, whichever file writes the
keys. Beside a written variant list this rule reports it instead of the width
rule, so it is reported once. A process that
lists variants without `print_extruder_id` gets a **warning** when a variant repeats (every entry then
reads as extruder 1, so the repeated variant is unreachable), nothing otherwise.

On a machine the layout keys are held to [Printer rules 1–4](extruder-variants.md#printer-machine),
every failure an error unless marked:

- With `extruder_variant_list` written: the list has one entry per extruder, as many as
  `nozzle_diameter`; every variant starts with its extruder's `extruder_type`;
  `default_nozzle_volume_type` names a nozzle volume type that extruder lists;
  `printer_extruder_variant` is the list flattened extruder-major and `printer_extruder_id` gives each
  entry its 1-based extruder (an id array left out reads as extruder 1 everywhere, which passes when
  those are the flattening's ids). Without the pair, the list may offer one variant in total.
- With the pair written and no `extruder_variant_list`: one variant per extruder at most. A pair that
  `single_extruder_multi_material` off would replace with the default at load is a **warning**; a pair
  the rebuild would leave as it is passes.

It does **not** see a base's own resize: it composes at the width each file wrote, so an array wider
than a base's list, which the loader cuts before any child inherits it, passes even with `--strict`
([composition](extruder-variants.md#padding-truncation-and-composition)). Nor does it see per-extruder
vectors outside the sets (`extruder_offset`, `printer_extruder_options`, …), which no variant list
sizes. What a variant holds (a High Flow variant copied from Standard, a variant inserted at the wrong
index) is review work
([item 14](review-checklist.md#14-per-extruder-vectors-not-checked-and-variant-arrays-widths-and-layout-checked));
whether it is a name the engine can select at all is `check_variant_names`'.

`python3 scripts/orca_profile_tool.py fix-variant` resizes every array the width rule rejects in the
selectable preset that writes it, leaves bases alone and adds no key: extra values are dropped, missing ones
repeat the last value (the last normal/silent pair at stride 2; a lone value fills normal and silent
alike). It leaves the variant lists and id arrays to you, since they address the variants rather than
fill them. `fix-variant --strict` then also writes each key that reaches a selectable preset at
another width: into the most general file on the way down to the preset whose own width is the
preset's and whose selectable presets taking it all need that width, else into the preset itself.
Presets of every bundle count towards that agreement; `--vendor` limits the files written and
`--dry-run` previews. The loader pads with the first value where `fix-variant` repeats the last, so a
padded array need not load as before, and
trimming deletes values: when the extra values were meant as per-extruder or per-variant values,
declare the variant layout instead ([Printer rule 2](extruder-variants.md#printer-machine)) and keep
them. `fix-variant` moves no value, so a family whose presets all wrote one value repeats the widened
array in each; put it on the family's base afterwards ([shared bases](shared-bases.md)).

### Variant names

`check_variant_names` reads the four list keys plus `extruder_type`, `nozzle_volume_type` and
`default_nozzle_volume_type` of every preset the bundle's index references, bases included, and holds
each entry to the names the engine's two enum maps define (`s_keys_map_ExtruderType`,
`s_keys_map_NozzleVolumeType`, read from `PrintConfig.cpp` on every run). The bundle's own files are
judged, not the composed config: a bad string is the writing file's error, once. Every finding is an
error, and no bundle is exempt: BBL, whose bundle is imported from BambuStudio, is held to OrcaSlicer's
enums like any other.

- A variant string outside `<extruder type> <nozzle volume type>` is a **dead variant**: it still
  counts toward the variant length the arrays are sized by, so the values written for it silently never
  reach the G-code. `Hybrid` too, which is runtime-only, and an empty entry. A name BambuStudio's enum
  has and OrcaSlicer's lacks is dead here as well, and passes with no tool change once the engine gains
  that nozzle volume type.
- A legacy name the loader still rewrites in these keys (`Normal` → `Standard`, `Big Traffic` →
  `High Flow`) is an error that names the enum name to write; the profile has to spell the enum
  name. `DirectDrive` is only rewritten in `extruder_type`, so a variant string carrying it is dead.
- A variant list naming one variant twice is an error: the lookup returns the first equal string, so
  the repeat is unreachable and its value sits at an index no extruder reads. A filament list takes
  strings, `extruder_variant_list` takes them per extruder, and a process takes `(extruder id, variant)`
  pairs — one string on two extruders is two pairs, not a repeat.
- An `extruder_type`, `nozzle_volume_type` or `default_nozzle_volume_type` value that is not an enum
  name is an error: they are enum options, so an unknown value fails the validator's load of the
  whole bundle, while the app silently loads the option's default instead. A legacy spelling
  (`DirectDrive`, `Normal`, `Big Traffic`) and `Hybrid`, an enum value no profile writes, are errors
  too.

The variant *order*, the choice of variants, and the values themselves are not checked.

### `normalize` and `update-index` are part of the check

`check` fails when either command would still change something, so they are not optional polish: the
file that gets reviewed has to be the file that ships. What `normalize` changes is narrow and fixed:

- adds a missing `type`;
- deletes a `version` or `is_custom_defined` key from a *preset* file;
- deletes six print-speed keys from filament profiles (`initial_layer_print_speed`, `outer_wall_speed`,
  `inner_wall_speed`, `infill_speed`, `top_surface_speed`, `travel_speed`);
- deletes the obsolete keys the loader ignores (the `ignore` set in `PrintConfigDef::handle_legacy`),
  across preset types;
- resolves the `extruder_clearance_*` conflict pair by keeping the larger;
- arrayifies six filament options (`filament_type`, `filament_cost`, `filament_density`,
  `temperature_vitrification`, `filament_max_volumetric_speed`, `filament_vendor`);
- hoists `type`, `name`, `renamed_from`, `inherits`, `from`, `setting_id`, `filament_id`,
  `instantiation` to the front.

A file it changes is then rewritten whole: tab-indented, LF, one trailing newline, keys reordered. A
file committed with CRLF line endings therefore changes on every line; read the diff before committing
it.

**Set `type` explicitly when authoring.** For a file in `machine/` without it, normalization guesses
`machine` only if its name contains `nozzle` (case-insensitive), otherwise `machine_model`. That
heuristic cannot reliably classify shared machine bases or unusually named variants.

The tool's obsolete-key set is checked against the loader's ignore list by a unit test. Active options
are preserved, including live keys whose *values* the loader rewrites (`extruder_type`: `DirectDrive` →
`Direct Drive`; the variant-string keys: `Normal` / `Big Traffic` → `Standard` / `High Flow`), and so are
legacy key names the loader migrates (such as `extruder_clearance_max_radius`).

Two things it therefore does **not** enforce:

- **Formatting and key order on their own.** A file with none of those problems is skipped entirely, so
  4-space indent, a missing trailing newline, and a file that leads with `compatible_printers` all pass
  `check`. They stay latent until something else trips `normalize` and the whole file reformats inside
  an unrelated diff. (`normalize --force` rewrites every file; do not run it on a shipped bundle.)
- **A misspelled setting key.** `inital_layer_height` and `sparse_infill_densiti` pass `check` cleanly.
  Verify new keys against `src/libslic3r/PrintConfig.cpp` and the loader's legacy handling
  (`PrintConfigDef::handle_legacy`: renamed keys, rewritten values and ignored keys).

`check` does not catch a dangling `default_print_profile` either; check that name by hand.

## The validator binary

Built from `src/dev-utils/OrcaSlicer_profile_validator.cpp` with `-DORCA_TOOLS=ON`. Both scripts use a
local build under `build*/` when one exists, else they download the nightly into the `validator`
subdirectory of the cache dir. `check_profile.sh` searches Release, then RelWithDebInfo, then Debug
(each under `build*/src/<config>` and `build*/*/src/<config>`), then single-config `build*/src`, and
prefers a host-architecture build tree; `check_profile.ps1` tries Release, RelWithDebInfo, MinSizeRel,
then Debug. A stale local build is used silently; `--download` / `-Download` skips local builds and uses the
nightly. The download and the fixtures stay cached until `--refresh` / `-Refresh`, so
`--download --refresh` (`-Download -Refresh`) matches CI exactly. Windows looks for
`OrcaSlicer_profile_validator.exe`.

If your build lives somewhere else, point at it with `--validator` / `-Validator`, or set
`ORCA_PROFILE_VALIDATOR` (`$env:ORCA_PROFILE_VALIDATOR` in PowerShell).

| Flag | Meaning |
| --- | --- |
| `-p <dir>` | profile tree (also becomes the data dir) |
| `-l <n>` | log level; CI uses 2 |
| `-v <Vendor>` | load only that vendor **plus** OrcaFilamentLibrary |
| `-s` | slice sweep |
| `-o <dir>` | with `-s`, save each printer's G-code there |
| `-f` | no-op (see above) |
| `-g 1` | regenerate user-preset fixtures; takes a value, and wipes the user preset dir first |

On ARM64 Linux the nightly is x86-64 only: the script warns and downloads anyway, producing a binary
that will not run. Build it locally with `-DORCA_TOOLS=ON` and pass `--validator`.

Running the validator directly uses the profile tree as its data directory and can create `user/`
there. Prefer the wrappers, which stash existing user presets and restore them afterward. After a
direct run, inspect `user/` and remove only empty directories created by that run; fixtures or
pre-existing user files may be present.

## Checking a copy of the tree

Use `--profiles DIR` on the Python tool and `-p DIR` on the validator. The wrappers' `--profiles` /
`-ProfilesDir` passes the tree to both, so one run validates a copy fully:

```bash
./scripts/check_profile.sh --profiles "<tree>"
# Windows: scripts\check_profile.bat -ProfilesDir "<tree>"
```

To slice a process or filament that is not a printer's default, or to read the G-code, work on a copy:
copy `resources/profiles` and `resources/info` into one scratch dir (the validator reads `info/` next to
the tree), point the printer's `default_print_profile` or first `default_filament_profile` at the preset
in the copy, and run the validator with `-o`:

```bash
<validator> -p <scratch>/profiles -v "<Vendor>" -s -o <gcode dir>
```

Each printer's G-code is saved as `<vendor name>__<printer>.gcode`; its embedded config
(`; print_settings_id = …`, `; filament_settings_id = …`) shows what was actually sliced.

## Testing in the app

Editing this checkout's `resources/profiles` does not update a separately installed application. Test
with a build using the edited resources and a bumped bundle version: the updater installs newer bundles
under `<data_dir>/system/`, and the preset cache also depends on the bundle version. Use Help ▸ Show
Configuration Folder to locate the active data directory:

| Platform | Default data directory |
| --- | --- |
| macOS | `~/Library/Application Support/OrcaSlicer` |
| Linux | `$XDG_CONFIG_HOME/OrcaSlicer`, or `~/.config/OrcaSlicer` when unset |
| Windows | `%APPDATA%\OrcaSlicer` |

A portable `data_dir` next to the executable takes precedence. Use a separate test configuration for a
clean-install check; preserve the normal configuration and user presets.

## Error → remedy

| Message | Fix |
| --- | --- |
| `can not find inherits <parent> for <preset>` | parent missing, unregistered, misspelled, or listed **after** the child |
| `can not find include` | the template is misspelled, registered after the includer, or selectable |
| `can not find filament_id for <name>` | nothing in the chain declares one: run `generate-id` |
| `can not find parent <name> for config <user preset>!` | a shipped name disappeared: add `renamed_from` |
| `Failed loading configuration file <file>` | that file could not be loaded and the whole bundle was discarded: a JSON error, or a value its option cannot take, such as `nil` in a non-nullable key (`Invalid value provided for parameter <key>: nil`, `Deserializing nil into a non-nullable object`); the lines above it name the cause |
| `Missing instantiation attribute for <name>` | key absent **or** not the string `"true"` / `"false"` |
| `contains incorrect keys: <keys>, which were removed` | a key valid for a different preset type |
| `defines invalid printer variant "<v>"` | not a token of the model's `nozzle_diameter` list |
| `has printer_variant "<v>" that does not match its nozzle_diameter` | [the set comparison](machine-profiles.md#printer_model-and-printer_variant) |
| `references unknown compatible_printers "<p>"` | the printer was renamed or deleted, or a `machine_model` name was used instead of a variant name: fix the reference. Under `--vendor`, usually another vendor's printer ([why](#the-five-checks)) |
| `references renamed compatible_printers "<old>" (now "<new>")` | in-tree references must name the current preset; `renamed_from` does not excuse them |
| `Filament preset "<f>" is missing compatible_printers setting` | non-library filaments need a non-empty list in their **own** file; the resolved-vs-own-key trap is in [filament-profiles.md](filament-profiles.md#compatible_printers) |
| `Ambiguous AMS filament match: N filament presets share filament_id "X" and are all compatible with printer "Y"` | make the lists disjoint by [specificity](filament-profiles.md#overlapping-coverage-one-variant-one-profile-per-product): the specialized profile keeps the variant, the general ones drop it; prefer this over deleting a profile. Or fix an `inherits` pointing at another product's `@base`. `orca_profile_tool.py check` does not catch this; only `validate_system` does |
| `Layer height cannot exceed nozzle diameter.` / `Line width too small` / `Line width too large` | the [slicing limits](process-profiles.md#values-to-review-per-nozzle) |
| `[ERROR] … no <V>.json list references it, so it never loads` | `update-index`, or delete the file |
| `[ERROR] … no <V>.json list references it and it declares no profile type` | set the correct `type` explicitly, then `normalize` and `update-index` |
| `[ERROR] … normalize would <change>` / `<V>.json: update-index would rebuild <lists>` | run that command and commit the result |
| `[ERROR] <V> has N <type> profiles named "<name>"` | identify the intended preset and remove or rename the duplicate; use `trim --dry-run` only for deliberate unindexed-file cleanup |
| `Duplicate key error in <file>: Duplicate key detected: <key>` | a key written twice in one file; keep the intended one |
| `… must not have a setting_id` / `… is missing a setting_id` / `setting_id "X" in <file> does not match the expected "Y" …` | `generate-id --setting-id` (by hand in `BBL/`, see [ids.md](ids.md#bbls-exception-precisely)) |
| `filament_id "X" declared by … does not match the mint of its triple …` | `generate-id --filament-id`; if the id should not be declared here at all, remove it so the preset inherits its root's |
| `inherits filament_id "X" but its own triple "V/T/N" mints "Y"` | `generate-id` will **not** fix this; see [ids.md](ids.md#what-generate-id-does-and-does-not-fix) |
| `"<key>" has N values for variant length S at stride k, which takes M` (with ` (no <list key>, so …)` after `S`, naming where `S` came from, when the preset writes no list, and ` (it comes from <file>)` at the end under `--strict` for an array the preset does not write) | exactly `S × k` values in variant order, or leave the key out ([widths](extruder-variants.md#widths)); on a list-less multi-extruder printer whose extruders differ, declare the layout; `fix-variant` cuts or pads to that width ([variant arrays](#variant-arrays)); an `S` you did not expect means the variant list did not resolve through `include` or `inherits` |
| `printer_extruder_variant […] is not extruder_variant_list flattened extruder-major […]` / `printer_extruder_id […] does not give each entry of printer_extruder_variant its 1-based extruder […]` / `printer_extruder_id has N entries for the M entries of printer_extruder_variant` | write the pair as the flattening of the list, ids in step ([Printer rule 2](extruder-variants.md#printer-machine)) |
| `extruder_variant_list has N entries for M extruder(s)` / `extruder_variant_list entry i "…" holds a variant that does not start with extruder i's extruder_type` / `default_nozzle_volume_type "…" is not a nozzle volume type extruder i lists` | [Printer rules 1, 3 and 4](extruder-variants.md#printer-machine) |
| `extruder_variant_list offers N variants but the preset writes no printer_extruder_variant/printer_extruder_id` / `printer_extruder_variant lists several variants for extruder N but the preset writes no extruder_variant_list` / `[WARNING] … with single_extruder_multi_material off the loader replaces printer_extruder_variant …` | write all three layout keys ([Printer rule 2](extruder-variants.md#printer-machine)) |
| `print_extruder_id has N entries for the M entries of print_extruder_variant` / `[WARNING] … print_extruder_variant repeats a variant but print_extruder_id is absent` | one id per variant entry, mirroring the printer's pairs ([Process rule 1](extruder-variants.md#process)) |
| `<key> <where> holds "…", which no extruder can select: "…" is not a nozzle volume type the enum has (…)` / `… it does not start with an extruder type the enum has (…)` / `… is empty` / `… Hybrid names the sub-nozzles of one hybrid extruder at runtime` | write a legal variant string, `<extruder type> <nozzle volume type>` from the two enums ([variant names](#variant-names), [variant strings](extruder-variants.md#variant-strings)); in every bundle, BBL included |
| `… holds the legacy variant "…"` / `extruder_type spells the legacy name "…"` / `nozzle_volume_type spells the legacy name "…"` / `default_nozzle_volume_type spells the legacy name "…"` | write the enum name the message gives: the loader still rewrites the legacy one, but `check` rejects it |
| `<key> lists "…" N times` / `print_extruder_variant lists the pair (extruder N, "…") N times` | drop the repeat and its value from every variant array: the lookup returns the first equal string |
| `extruder_type "…" is not one of (…)` / `nozzle_volume_type "…" is not one of (…)` / `default_nozzle_volume_type "…" is not one of (…)` / `… names "Hybrid", which the engine computes for a hybrid extruder at runtime` | they are enum options, so an unknown value fails the validator's load of the whole bundle and silently becomes the default in the app; use `s_keys_map_ExtruderType` / `s_keys_map_NozzleVolumeType`, `Hybrid` excepted ([variant strings](extruder-variants.md#variant-strings)) |
| `[WARNING] No profiles found for vendor: <dir>` | a directory with no matching index (an emptied folder, or a `user/` left by a direct validator run); remove it |
| `… has no compatible system filament in its model's "default_materials"` | add a system filament preset compatible with that variant to the model's `default_materials` |
| `… names the unknown system filament "<n>" in its "default_materials"` / `… "default_filament_profile"` | name an existing system (not user, not base) filament exactly; `;` separators, exact case |
| `Missing filament profile: '<n>' referenced in <file>` | the same, caught by `check`; usual causes are `,` instead of `;`, wrong case (`@system`), or a `;`-joined list packed into one array element |
| `machine_model name "<n>" is declared by N bundles` | model names are unique across the tree; rename the new model |
| `vendor <V>'s config version: <s> invalid` | the `version` string does not parse; write `MM.mm.pp.bb` |
| `[json.exception.type_error.302] type must be string` | locate the non-string value in the index or a model; see [failure scopes](vendor-bundle.md#failure-scopes) |
| `Printer "<p>" fell back to a default preset` | the final process or filament selection is a generic Default preset: check the named defaults, their visibility and that compatible presets exist. An incompatible default may instead be replaced without this error |
| `Printer "<p>" sliced but the filament change never fired (no CP TOOLCHANGE START)` | `change_filament_gcode` never expanded |

## CI

`.github/workflows/check_profiles.yml`, job **"Check profiles"**, runs on `pull_request` into `main` or
`release/*` touching `resources/profiles/**`, `resources/printers/**`, `scripts/**`,
`src/libslic3r/PrintConfig.cpp` (where the tool reads the variant key sets) or the workflow itself.
There is no push trigger: a direct push to main runs no profile validation.

The job opens with `python3 -m unittest discover -s scripts/tests -t scripts`, the tool's own unit tests
(run them locally after changing `scripts/`, with `py -3` on Windows, and keep `-t scripts` or the
imports fail). That step is deliberately **not** `continue-on-error`: a broken tool makes everything it
then says about the profiles worthless. Every check after it is `continue-on-error` with a final gate,
so one run reports all five results. On failure a second workflow posts or replaces a single PR comment
marked `<!-- profile-validation-comment -->`, holding the start of each failing log (30 KB per check,
12 KB per `validate_custom` fixture; reproduce locally for the full list); it deletes the comment once
the run is green.

The job name is also the required check for the delegated-merge bot, which lets a vendor maintainer
self-merge a PR limited to their own `resources/profiles/<Vendor>/` folder with no human review, so
whatever CI does not check is what ships unreviewed. Its denied patterns refuse `^scripts/` and any
`.py`, so a PR that touches the tooling always needs a maintainer.
