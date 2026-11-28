# `setting_id` and `filament_id`

Orca-generated ids are deterministic hashes of identity. **Never invent an id or copy a sibling's
`setting_id`.**
Use `scripts/orca_profile_tool.py`; the two special cases are
[a wrongly inherited filament id](#what-generate-id-does-and-does-not-fix) and
[BBL's authoritative setting ids](#bbls-exception-precisely).

`docs/HLSD/filament_id.md` is the authoritative design document for `filament_id`: the id landscape,
the checks CI runs, and the Bambu catalog map. This page is the tooling half.

| | `setting_id` | `filament_id` |
| --- | --- | --- |
| Identifies | one selectable preset | one filament **product** |
| Key hashed | `<vendor folder>/<type>/<name>` | `filament_product/<filament_vendor>/<filament_type>/<alias>`, using the resolved (inherited) first values and the name up to the first `@`, right-trimmed |
| Shape | 16 base62 characters | `OF` + 6 base62 characters |
| Required on | every `instantiation: "true"` preset | every **instantiated** filament, own or inherited |
| Forbidden on | bases (`instantiation` not `"true"`) | — (a product's root base is exactly where it belongs) |
| Scope | unique across the whole tree | shared by every preset of the product, in every bundle |

`<type>` is `machine`, `process` or `filament`, and the vendor is the **folder** name (`BBL`), not the
display name (`Bambulab`). Renaming a preset changes its `setting_id`; renaming a filament's alias, or
editing its `filament_vendor` or `filament_type`, also changes its `filament_id`, and the old id is not
forwarded.

## The tool

`scripts/orca_profile_tool.py` takes a subcommand:

| Command | Does |
| --- | --- |
| `check` | everything CI's `profile_tool` step runs; see [validation.md](validation.md#orca_profile_toolpy-check) |
| `generate-id` | writes `setting_id` and `filament_id` |
| `normalize` | rewrites profile files into their canonical shape |
| `trim` | deletes profile files no `<Vendor>.json` list references |
| `update-index` | rebuilds the `*_list` sections from the files on disk |

The order after adding, renaming or deleting files is `normalize` → `update-index` → `generate-id` →
`check`. Each step feeds the next, so it is not interchangeable. The
[authoring workflow](../SKILL.md#creating-or-modifying-a-profile) has the commands.

> **`trim` deletes.** It removes every profile file the index does not list, including the one you just
> added and have not registered yet. Register first, or skip `trim` entirely: it is a cleanup sweep, not
> part of landing a profile. Preview with `--dry-run`.

**Register, then mint.** The `filament_id` pass reads `<Vendor>.json`'s `filament_list`, not the
filesystem (the `setting_id` pass walks the filesystem, so a bundle whose index has not landed yet is
still assignable). A new filament file is therefore invisible to `generate-id`'s `filament_id` pass
until it is registered; its `setting_id` is written regardless.

- `--dry-run` works on every writing command (`generate-id`, `normalize`, `trim`, `update-index`) and
  writes nothing.
- `--filament-id` / `--setting-id` narrow `generate-id` to one pass; they exclude each other, and
  passing neither writes both.
- `--vendor` is repeatable and narrows **only what is written**: an id is a function of its own key
  alone, so a narrowed run writes exactly what a full run would. An unknown vendor exits 1 before any
  write. `--vendor` on `check` narrows the per-vendor checks only; the `setting_id` and `filament_id`
  passes stay tree-wide.
- `--profiles DIR` points any command at another tree; see
  [Checking a copy of the tree](validation.md#checking-a-copy-of-the-tree).
- `--profile-type` narrows `normalize`, `trim` and `update-index` to `machine_model`, `process`,
  `filament` or `machine`.
- Exit codes: 0 clean, 1 errors found (`generate-id` still writes what it could), 2 argparse misuse.
- Output is ANSI-coloured; searching for the literal `[ERROR]` still works.

`generate-id` is **idempotent and byte-preserving**: BOM and CRLF are kept, and each pass touches only
its one key line. A legitimate `generate-id` diff is one or two changed lines per file: a new instantiated
filament gets both a `filament_id` and a `setting_id`. `normalize` is the opposite by design (it
rewrites whole files into canonical shape), which is why `check` demands it already be a no-op. A file
committed with CRLF line endings changes on every line under `normalize`; read the diff before
committing it.

Exit 1 from `generate-id` does not mean nothing was written: it writes every id it can and reports the
rest, so read the diff before rerunning. On a clean tree `check` and `generate-id --dry-run` both exit 0
with zero findings; that is the baseline to restore before opening a PR.

## What `generate-id` does and does not fix

Writes:

- a `setting_id` into any instantiated preset that lacks one, or whose value does not match the formula;
- strips a `setting_id` from a base;
- deletes the misspelled key `settings_id`, moving its value to `setting_id` only on an instantiated BBL
  preset that lacks one (everywhere else the old value is discarded and a fresh id minted);
- a `filament_id` into the id-less **root(s)** of an instantiated filament that resolves none;
- rewrites a **declared** `filament_id` that is not the mint of its own triple.

Refuses to write (reports only): a base62 collision between two products, an empty `filament_vendor` or
`filament_type`, a broken `inherits` chain, and roots of one filament resolving different
`(filament_vendor, filament_type)` pairs.

**Does not fix: a preset that *inherits* a wrong `filament_id`.** This is check 2b (the label `docs/HLSD/filament_id.md` and the tool use), and it is the trap most likely to bite.
It happens when a branded filament inherits a generic for its settings:

```jsonc
{ "name": "Phrozen Aura PETG @Phrozen Arco 0.4 nozzle",
  "inherits": "Generic PETG @System" }   // resolves the library generic's id: wrong product
```

The preset resolves *an* id, so `generate-id` neither inserts nor rewrites one, and `check` fails with
`inherits filament_id "X" but its own triple "V/T/N" mints "Y"`.

Two fixes, in order of preference:

1. **Give the product a `@base` root** inheriting a material base (`fdm_filament_pet`,
   `fdm_filament_pla`, …) with its own `filament_vendor` and `filament_type`. No `fdm_filament_*` base
   carries a `filament_id`, so the filament now resolves none and `generate-id` mints it on the root.
   This is the product-root shape ([the three-part shape](filament-profiles.md#the-three-part-shape)).
2. **Declare the tool-computed id on the preset itself.** Use the expected value `check` reports, or
   compute it with the function below; this is not a manually chosen id. First make sure the preset
   resolves the right `filament_vendor` and `filament_type`: with neither set, the triple resolves
   through the generic parent and the branded product is minted under vendor `Generic`. If you need the
   id before the file exists:

   ```bash
   python3 -c "import sys; sys.path.insert(0,'scripts'); from orca_profile_tool import generate_filament_id as g; print(g('Polymaker','PLA','PolyLite PLA'))"
   # -> OF5CgdDq
   ```

   The quoting works unchanged in cmd and PowerShell; only swap `python3` for `py -3`.
   `generate_preset_setting_id('<vendor folder>', '<type>', '<name>')` is the `setting_id` equivalent.

A vendor's tune of a generic that keeps the `Generic X` alias is not this case: inheriting the
generic's id is correct there, because the product really is the library generic
([filament-profiles.md](filament-profiles.md#the-two-most-common-contributions)).

## BBL's exception, precisely

The exception covers **`setting_id` assignment only**, keyed on the *folder* name `BBL`:

- The tool never mints or replaces a `setting_id` in `BBL/`: those are Bambu's own ids. A new
  instantiated BBL preset with no `setting_id` therefore **cannot be fixed by the tool**, yet the
  presence rule still applies to it: carry over Bambu's authoritative id by hand.
- BBL is not exempt from anything else: bases still get their `setting_id` stripped, ids must still be
  unique across the tree, and BBL `filament_id`s are minted like everyone else's, as `OF…` ids.

## Ids other systems compose

No id from another system is the mint of a triple, so `check` rejects one used as a `filament_id` like
any other bad id: same error, same remedy, whoever wrote it. Three such spaces exist near the tree;
recognise them so you do not copy one into a profile:

- **Bambu's `GF…` catalog**: external and opaque, correlated to Orca's ids by the generated
  `resources/printers/bambu_filament_ids.json`. `blacklist.json` and
  `BBL/filament/filaments_color_codes.json` reference Bambu catalog ids by design. The rule is about
  `filament_id` and nothing else: every BBL `setting_id` starts with `G`, and that is Bambu's own
  preset id, not a leaked catalog id.
- **Qidi's `QD_…`**: composed at runtime by the printer's filament box
  (`QD_<series>_<vendor>_<typeidx>`), not a preset id.
- **`P` + 7 hex digits, and `"null"`**: what the app gives a *user*-created filament.

## Tests

`python3 -m unittest discover -s scripts/tests -t scripts` (`py -3 -m …` on Windows) runs the tool's
unit tests. Note the `-t scripts` argument; without it the imports fail. CI runs them as the first,
non-`continue-on-error` step of the profile job; see [validation.md](validation.md#ci).
