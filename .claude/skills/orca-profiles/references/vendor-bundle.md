# The vendor bundle and the loader

A bundle is `resources/profiles/<Vendor>.json` (the index) plus `resources/profiles/<Vendor>/`.
The **vendor id is the filename stem**, not the `name` inside — several differ (`BBL.json` is named
"Bambulab"). Asset paths and the `setting_id` formula use the id; the `validate_custom` fixture prefix
uses the `name`.

## The index

```json
{
    "name": "Phrozen",
    "version": "02.04.00.03",
    "force_update": "0",
    "description": "Phrozen configurations",
    "machine_model_list": [ { "name": "...", "sub_path": "machine/....json" } ],
    "machine_list":       [ ... ],
    "process_list":       [ ... ],
    "filament_list":      [ ... ]
}
```

The loader reads `name`, `version`, `url` and the four `*_list` arrays.
`description` is only logged. `force_update` is read by `PresetUpdater`, never by the loader.
`sub_path` is relative to the **vendor folder**.

| List | Holds |
| --- | --- |
| `machine_model_list` | `machine_model` records (the printer product) |
| `machine_list` | printer variants **and** shared machine bases |
| `process_list` | selectable processes **and** shared process bases |
| `filament_list` | selectable filaments **and** shared filament bases |

### Three registration rules

1. **Everything is registered, bases included.** Every preset file on disk has exactly one entry in the
   matching list, and no unindexed preset file is left in the tree.
2. **Parents before children.** `inherits` resolves against a per-kind map filled as the list is walked
   (`configs.clear()` then process, filaments, printers). A parent listed after its child produces
   `can not find inherits <parent> for <child>` and the bundle is discarded.
3. **The index entry's `name` must equal the `name` inside the sub_path file.** `check_name_consistency`
   walks the index looking for the files; `check_index_coverage` walks the files looking for them in the
   index. The `renamed_from` escape hatch `check_name_consistency`'s docstring promises is commented out.

All three are `check` errors now, and `update-index` writes an index that satisfies all three from the
files on disk — including the parents-first ordering, by topological sort. Hand-editing the index is
fine for a one-line addition, but the committed result must equal what `update-index` writes, because
`check` compares them.

The loader itself reports none of this: an unregistered file, or an entry with a typo'd key
(`"subpath"`), is silently dropped. (A typo'd `sub_path` is a `check` error naming the entry.)

`BBL/cli_config.json` and `BBL/filament/filaments_color_codes.json` are auxiliary data loaded by path,
not presets. The tool's `NON_PROFILE_FILES` excludes these basenames from preset maintenance.

## `version`

Parsed by a four-component Semver where the 4th is folded in as `patch = patch*100 + value`. Write it
zero-padded, `MM.mm.pp.bb`; a couple of bundles drop a component or the padding, but do not imitate them.

- **Bump the version for every bundle the PR touches.** `PresetUpdater` installs bundled resources
  only when their version is newer than the installed version; the `.opc` preset cache is also
  versioned. Nothing in profile CI checks the bump.
- **Keep the last component ≤ 99.** `02.04.00.100` and `02.04.01.00` both parse to `2.4.100`. A bundle
  that reaches `.99` carries into the third component (`02.03.02.99` → `02.03.03.00`).
- An **absent** version is worse than a stale one: the validator still passes, but `Semver::valid()`
  excludes `0.0.0`, so the vendor is dropped from the configuration wizard entirely and the preset cache
  is disabled for it. An *unparseable* version is not silent — it throws and discards the whole bundle
  (see the failure table below).

## Common preset keys

| Key | Value |
| --- | --- |
| `type` | `machine_model` / `machine` / `process` / `filament` |
| `name` | the preset name; the filename is *not* authoritative |
| `inherits` | the parent's exact `name` — no path, no `.json` |
| `instantiation` | the **string** `"true"` (selectable) or `"false"` (base) |
| `from` | `"system"` by convention; the vendor loader never reads it |
| `setting_id` | required on instantiated presets, forbidden on bases — generated |
| `renamed_from` | `;`-separated list of old names this preset supersedes |

These are config-preset keys; `machine_model` records have their own
[schema](machine-profiles.md#a-machine_model-is-not-a-config-preset). Keep `from` as `"system"`
for shipped presets. The vendor loader ignores it, but the CLI config-file loader accepts only
`system`, `user` or `User` and handles their inheritance differently.

`instantiation` is the one metadata key that is hard-gated: a missing key or any value other than the
strings `"true"`/`"false"` is an error (`Missing instantiation attribute for <name>`). A JSON boolean
`true` fails harder — it throws inside `load_from_json` and takes the **whole vendor bundle** down.

### `inherits`

Resolution is an exact-name lookup **within the same bundle**, plus one exception: filaments may inherit
from `OrcaFilamentLibrary`, which is loaded first and becomes the base bundle. Vendor-to-vendor
inheritance always fails. You can inherit from an instantiated preset as well as from a base; it is
common.

### `renamed_from`

One JSON string, `;`-separated for several old names.

- Write `"A;B"`, never `"A ; B"` — an unquoted item keeps its trailing space and can never match.
- When `renamed_from` is **absent** and the name contains `@`, the loader auto-adds the `@`-removed form
  (`X @Y` → `X Y`) as a rename alias. Declaring an explicit `renamed_from` **suppresses** that, so a
  preset that needs both the `@`-removed form and a real old name must list both. No shipped profile
  currently does, which means any preset that gained a `renamed_from` quietly lost its `X Y` alias.
- It rescues names stored **outside** the tree: user presets and 3MF projects. It does **not** rescue
  in-tree `inherits` (exact lookup), it does **not** satisfy `check_name_consistency`, the validator
  reports an in-tree reference that only resolves through it (`references renamed compatible_printers
  "OLD" (now "NEW")`), and `machine_model` records never read it at all.
- Only one preset may claim a given old name — two that do is a counted error
  (`… was marked as renamed from "Y" … as well`). But the redirect is **inert while a live preset still
  carries that name**, and nothing checks *that*; Z-Bolt ships a folder of such dead entries.

## Failure modes, ranked by blast radius

| Scope | Cause |
| --- | --- |
| **All vendors, zero system profiles** | a non-string `version`, `name` or `url` at the top level of a vendor index (`"version": 2`), or non-string `nozzle_diameter` on a model — `nlohmann::type_error` escapes the per-vendor `std::runtime_error` catch |
| **The whole vendor bundle** | unparseable `version`; index JSON parse error; a `sub_path` file missing or unparseable; unresolvable `inherits`; duplicate preset name within the vendor; empty/unknown `printer_model` or `printer_variant`; a filament resolving no `filament_id` |
| **One preset** | `instantiation` missing or a wrong string; keys belonging to another preset type (`contains incorrect keys: …, which were removed`); a non-string inside a `*_list` entry (`invalid value type for <key>`) |
| **Logged, not counted** | a raw JSON number in a preset — `invalid json type for <key>`, the value is dropped and the exit code stays 0 |
| **Nothing reported by the loader** | unregistered file; misspelled setting key; missing bed/hotend asset. Only the first of those is a `check` error; the other two reach users |

Deleting a file the index still lists surfaces as a *parse error* on line 1, not "file not found" — the
loader `ifstream`s the missing path and nlohmann reports `unexpected end of input`.

Preset names are a **single global namespace across every vendor**: a duplicate within one vendor is a
hard bundle failure, a duplicate across vendors is reported as `Found duplicated preset: <name> in
vendor: <vendor>` and still counts as an error. `check_preset_name_uniqueness` catches the within-bundle
case earlier and more precisely — including an *unindexed* twin, which is one `sub_path` edit away from
silently becoming the parent every child resolves to (`std::map::emplace` keeps the first insertion, so
index order decides). Base names, by contrast, repeat across bundles by design: `fdm_process_common`
exists in nearly all of them.

## Starting a whole new vendor bundle

Nothing generates one; copy the smallest bundle that resembles the hardware. **`Voxelab` or `M3D`** are
the minimal shape — a shared machine base, the model, one variant, a shared process base, two
processes, and an empty `filament_list` that takes the library generics. Do *not* start from `Phrozen`:
it carries local `fdm_filament_*` copies that have drifted from the library, and a filament preset that
restates most of its parent — the style this skill advises against.

Write the machine files **last**, so you only visit them once:

1. **Choose the names first** — model, variant(s), process(es). Everything else references them.
2. `resources/profiles/<Vendor>.json`: `name`, `version` (`01.00.00.00`), `force_update: "0"`,
   `description`, and all four `*_list` arrays (an empty `filament_list` is fine).
3. The shared bases — `<Vendor>/machine/fdm_machine_common.json` and
   `<Vendor>/process/fdm_process_common.json`, both `"instantiation": "false"` with no `setting_id`.
   For a Klipper printer add your own `<Vendor>/machine/fdm_klipper_common.json` inheriting the machine
   base; there is no shared one, because a `machine` preset can only inherit inside its own bundle.
4. One selectable process per variant, each naming its variant in `compatible_printers`.
5. Bed assets and `<Model>_cover.png`, all directly in `<Vendor>/`. None of them is needed for the
   bundle to load, and nothing in CI checks them — but the bed files are inert unless the `machine_model`
   names them in `bed_model` / `bed_texture`, and the cover is found by convention as
   `<the name you gave the model in machine_model_list>_cover.png`.
6. The `machine_model` record and the `machine` variants, now that every value they reference exists —
   the minimum key sets and the `default_*` shapes are in
   [machine-profiles.md](machine-profiles.md#the-machine-variant).
7. Run the tool and validate — follow
   [Creating or modifying a profile](../SKILL.md#creating-or-modifying-a-profile). `generate-id` is not
   optional for a new bundle: the validator loads presets that have no `setting_id`, but `check` fails
   every one of them. `update-index` will fill the four `*_list` arrays for you once the files exist, so
   step 2 only needs the bundle metadata to be right.

## `resources/profiles_template/`

A separate tree (`Template.json` + `Template/`) holding filament and process templates. It is **not** a
scaffold for shipped profiles — `CreatePresetsDialog.cpp` reads it for the in-app "create a custom
printer/filament" wizard, so editing it changes what users get when they create a custom preset.
`check_profile.sh`'s validator checks default to `resources/profiles` (redirectable with `-p`), and so
does `orca_profile_tool.py` (redirectable with `--profiles`);
neither covers this tree.
