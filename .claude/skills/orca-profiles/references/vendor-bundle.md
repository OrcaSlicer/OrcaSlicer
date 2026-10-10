# Vendor bundles

A bundle is `resources/profiles/<Vendor>.json` (the index) plus `resources/profiles/<Vendor>/`. The
**vendor id is the filename stem**, not the `name` inside; the two may differ (`BBL.json` is named
"Bambulab"). Asset paths and the `setting_id` formula use the id; the `validate_custom` fixture prefix
uses the `name`.

## The index

```json
{
    "name": "Phrozen",
    "version": "02.04.00.03",
    "force_update": "0",
    "description": "Phrozen configurations",
    "machine_model_list": [ { "name": "Phrozen Arco", "sub_path": "machine/Phrozen Arco.json" } ],
    "machine_list":       [ … ],
    "process_list":       [ … ],
    "filament_list":      [ … ]
}
```

The loader reads `name`, `version`, `url` and the four `*_list` arrays. `description` is only logged;
`force_update` is read by the profile updater, never by the loader. `sub_path` is relative to the
**vendor folder**.

| List | Holds |
| --- | --- |
| `machine_model_list` | `machine_model` records (the printer product) |
| `machine_list` | printer variants **and** shared machine bases |
| `process_list` | selectable processes **and** shared process bases |
| `filament_list` | selectable filaments **and** shared filament bases |

### Three registration rules

1. **Everything is registered, bases included.** Every preset file on disk has exactly one entry in the
   matching list, and no unindexed preset file is left in the tree.
2. **Parents before children, includes before includers.** The lists load processes first, then
   filaments, then printers, each in index order, and `inherits` and `include` resolve only against
   presets of that type already loaded from it. A parent
   listed after its child produces `can not find inherits <parent> for <child>` and the bundle is
   discarded; an include listed after its includer is `can not find include`, a counted error that
   leaves the includer without those keys.
3. **The index entry's `name` equals the `name` inside the `sub_path` file.** `renamed_from` does not
   excuse a mismatch.

All three are `check` errors, and `update-index` writes an index that satisfies all three from the
files on disk, including the dependency ordering (parents and templates before the presets that use
them, then, in name order, entries that neither depend on nor are depended on by another entry of their own
list, and any entry on a dependency cycle). Hand-editing the index is
fine for a one-line addition, but the committed result must equal what `update-index` writes, because
`check` compares them.

The loader itself reports none of this: an unregistered file, or an entry with a misspelled key
(`"subpath"`), is silently dropped. (A misspelled `sub_path` value is a `check` error naming the entry.)

`BBL/cli_config.json` and, in `BBL/filament/`, `filaments_color_codes.json`, `filament_id_map.json`,
`filament_name_map.json` and `support_recommended_params.json` are auxiliary data files read by path,
not presets. The last three carry a `type` key and look like presets; the tool excludes all five from
preset maintenance.

## `version`

Four components, `MM.mm.pp.bb`, compared as a version number in which the fourth is folded into the
third (`patch × 100 + build`). Write all four components, zero-padded.

- **Bump the version for every bundle the change touches.** The app installs bundled profiles only
  when their version is newer than the installed one, and the `.opc` preset cache is also keyed on
  the version. Nothing in profile CI checks the bump.
- **Keep the last component ≤ 99.** `02.04.00.100` and `02.04.01.00` both read as
  `2.4.100`. A bundle that
  reaches `.99` carries into the third component (`02.03.02.99` → `02.03.03.00`).
- An **absent** version is worse than a stale one: it reads as `0.0.0`, which is not a valid version.
  `check` and the validator still pass, but the vendor is dropped from the setup wizard entirely and gets no preset cache. Confirm the key exists. An
  *unparseable* version is not silent: it discards the whole bundle (`vendor <V>'s config version: <s>
  invalid`).

## Common preset keys

| Key | Value |
| --- | --- |
| `type` | `machine_model`, `machine`, `process` or `filament` |
| `name` | the preset name, the identity every reference uses; the filename is *not* authoritative |
| `inherits` | the parent's exact `name`: no path, no `.json` |
| `include` | a template's exact `name`, or an array of them, layered under this preset's own keys ([below](#inherits-and-include)) |
| `instantiation` | the **string** `"true"` (selectable) or `"false"` (base) |
| `from` | `"system"` for shipped presets |
| `setting_id` | generated; required on instantiated presets, forbidden on bases |
| `renamed_from` | `;`-separated old names this preset supersedes ([below](#renamed_from)) |

These are config-preset keys; `machine_model` records have their own
[key set](machine-profiles.md#machine_model-a-record-not-a-config-preset). Keep `from` as `"system"`:
the bundle loader ignores it, but loading the file as a CLI config accepts only `system`, `user` or
`User` and handles their inheritance differently.

`instantiation` is the one metadata key the validator gates: a missing key or any value other than the
strings `"true"` / `"false"` is a counted error (`Missing instantiation attribute for <name>`) that fails
the validator, though the preset still loads and is treated as selectable. A file with no
`instantiation` whose name contains `gcode`, or that has no `name`, silently becomes an include-only
template. A JSON boolean `true` fails harder: it takes the **whole vendor bundle** down.

## `inherits` and `include`

`inherits` resolves by exact name **within the same bundle**, plus one exception: filaments may inherit
from OrcaFilamentLibrary, which is loaded first. Vendor-to-vendor inheritance always fails, and an
unresolved `inherits` discards the bundle. You can inherit from an instantiated preset as well as from a
base.

`"include": ["<name>", …]` (or one bare name) pulls in `instantiation: "false"` presets of the same type
from the same bundle (never the library), registered before the includer. It shares a block of keys
between presets that do not share a parent: a variant layout, a G-code template. Only `"false"` presets
can be included, so a name that resolves to nothing (misspelled, registered after the includer, or a
selectable preset) is a counted error (`can not find include`) and the preset loads without it.

**How a preset's config is composed:** start from the parent's stored config (a root starts from the
built-in defaults), apply each preset named in `include` in the order listed, then the preset's own
keys. Later layers win, so precedence is own keys > later includes > earlier includes > the `inherits`
chain. Only then is every variant key of the composed config resized to its variant length
([widths](extruder-variants.md#widths)), and keys of another preset type removed. The two routes hand
down different widths:

- **`inherits` hands down the resized config.** A base is stored *after* its own resize, at the length
  of its own `*_extruder_variant` (one variant for a base of any type that writes none, whatever its
  extruder count). A child therefore inherits the base's arrays at the base's width: an array wider than
  that is cut to its first values before any child sees it, and a child that adds variants gets those
  first values padded. So widen an array only on a preset whose own variant list already has the
  entries; a wide array on a narrow base is silently lost at load, and `check`, which composes at the
  width each file wrote and judges selectable presets only, misses that cut.
- **`include` hands down the template's diff, at its pre-resize width.** An included preset contributes
  every key where its own composed config (its parent, its own includes and its own keys) differs from
  the built-in defaults, taken *before* its resize. So keys the template inherits are passed on too,
  arrays it writes arrive at the width its file wrote, and a key it sets to the built-in default value
  is not passed on at all, so it cannot override what the includer inherited. Resizing happens on the
  includer, not on the template.

## `renamed_from`

One JSON string, `;`-separated for several old names.

- Write `"A;B"`, never `"A ; B"`: a space after a `;` is skipped, but a space before it stays part of
  the name (`"A "`), which can never match.
- When `renamed_from` is **absent** and the name contains `@`, the loader auto-adds the `@`-removed form
  (`X @Y` → `X Y`) as a rename alias. Declaring an explicit `renamed_from` **suppresses** that, so a
  preset that needs both the `@`-removed form and a real old name must list both; a preset that gains
  a `renamed_from` without it quietly loses its `X Y` alias.
- It rescues names stored **outside** the tree: user presets and 3MF projects. It does **not** rescue
  in-tree `inherits` (exact lookup), it does **not** satisfy the index-name rule, the validator reports
  an in-tree reference that only resolves through it (`references renamed compatible_printers "OLD"
  (now "NEW")`), and `machine_model` records never read it at all.
- Only one preset may claim a given old name; two that do is a counted error
  (`… was marked as renamed from "Y" … as well`). But the redirect is **inert while a live preset still
  carries that name**, and nothing checks *that*, so a neighbour's `renamed_from` is no model.

## Failure scopes

| Scope | Cause |
| --- | --- |
| **Every vendor except OrcaFilamentLibrary, and all user presets** | a non-string where the index or a `machine_model` expects a string (`"version": 2` at the top level of an index, a numeric `name` or `url`, a non-string `nozzle_diameter` or other model key): `[json.exception.type_error.302] type must be string`, and the validator reports `Validation failed` |
| **The whole vendor bundle** | index JSON parse error; unparseable `version`; a listed file missing or unparseable; a value its option cannot take, such as `nil` in a non-nullable key (`Failed loading configuration file`); unresolved `inherits`; two selectable presets with one name; empty or unknown `printer_model` / `printer_variant`; a filament resolving no `filament_id`; a JSON boolean `instantiation` |
| **A counted error; the preset still loads** | `instantiation` missing or not `"true"` / `"false"`; keys belonging to another preset type (`contains incorrect keys: …, which were removed`); a non-string inside a `*_list` entry (`invalid value type for <key>`); an `include` naming nothing usable (`can not find include`, loads without it) |
| **The rest of the file, logged only** | an array with a non-string element (`[0.4]`, `invalid json array`): that key and every key after it in the file are dropped, and no error is counted |
| **One value, logged only** | a raw JSON number in a preset (`invalid json type for <key>`): the value is dropped and the exit code stays 0 |
| **Nothing reported by the loader** | unregistered file; two bases with one name, or a base and a selectable preset with one name (the first in the index wins); misspelled setting key; missing bed, hotend or cover asset. `check` catches the first two; the others reach users |

Deleting a file the index still lists surfaces as a *parse error* on line 1 (`unexpected end of input`), not "file
not found".

Selectable preset names are a **single namespace across every vendor**: a duplicate within one vendor
is a hard bundle failure, and a duplicate across vendors is reported as `Found duplicated preset: <name>
in vendor: <vendor>` and still counts as an error. `check` catches the within-bundle case earlier and
more precisely, bases included, and including an *unindexed* twin, which is one `sub_path` edit away
from silently becoming the parent every child resolves to (the first registered preset of a name wins,
so index order decides). Base names, by contrast, repeat across bundles by design:
every bundle may have its own `fdm_process_common` ([uniqueness](naming.md#uniqueness)).

## Starting a whole new vendor bundle

Nothing generates one; copy the smallest bundle that resembles the hardware. **`Voxelab`** is the
minimal shape: a shared machine base, the model, one variant, a shared process base, two processes, and
an empty `filament_list`, so the printer takes the library generics. Do *not* start from a bundle that
carries local `fdm_filament_*` copies, which drift from the library, or filament presets that restate
most of their parent, the style this skill advises against.

Write the machine files **last**, so you only visit them once:

1. **Choose the names first**: model, variant(s), process(es). Everything else references them
   ([naming.md](naming.md)).
2. `resources/profiles/<Vendor>.json`: `name`, `version` (`01.00.00.00`), `force_update: "0"`,
   `description`, and all four `*_list` arrays (empty is fine: `update-index` fills them once the files
   exist, so this step only needs the bundle metadata to be right).
3. The shared bases: `<Vendor>/machine/fdm_machine_common.json` and
   `<Vendor>/process/fdm_process_common.json`, both `"instantiation": "false"` with no `setting_id`. For
   a Klipper printer add your own `<Vendor>/machine/fdm_klipper_common.json` inheriting the machine
   base; there is no shared one, because a `machine` preset can only inherit inside its own bundle.
4. One selectable process per variant, each naming its variant in `compatible_printers`.
5. Bed assets and `<Model>_cover.png`, all directly in `<Vendor>/`. None of them is needed for the
   bundle to load, and nothing in CI checks them; but the bed files are inert unless the
   `machine_model` names them in `bed_model` / `bed_texture`, and the cover is found by convention as
   `<the name you gave the model in machine_model_list>_cover.png`.
6. The `machine_model` record and the `machine` variants, now that every value they reference exists;
   the minimum key sets and the `default_*` shapes are in
   [machine-profiles.md](machine-profiles.md#machine-the-variant).
7. Run the tool and validate: follow
   [Creating or modifying a profile](../SKILL.md#creating-or-modifying-a-profile). `generate-id` is not
   optional for a new bundle: the validator loads presets that have no `setting_id`, but `check` fails
   every one of them.

## `resources/profiles_template/`

A separate tree (`Template.json` + `Template/`) holding filament and process templates. It is **not** a
scaffold for shipped profiles: the app's "create a custom printer / filament" dialog reads it, so
editing it changes what users get when they create a custom preset. `check_profile.sh`'s validator
checks default to `resources/profiles` (redirectable with `-p`), and so does `orca_profile_tool.py`
(redirectable with `--profiles`); neither covers this tree.
