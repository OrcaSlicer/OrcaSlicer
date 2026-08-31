# Filament IDs (`filament_id`)

`filament_id` identifies a **material family**: one commercial product line = one id, shared by
all of that material's per-printer / per-nozzle variants, in every profile bundle that ships it.
Devices use it to match a physical spool or tray to a filament preset. It is never per-color,
per-printer, per-nozzle, or per-preset (per-preset identity is `setting_id`), and it is never
per-bundle either — PolyLite PLA carries the same id whether the preset lives in the
OrcaFilamentLibrary (OFL), Qidi, or Snapmaker bundle.

**How it is generated:** an id is computed, never invented. `scripts/assign_filament_ids.py`
mints it as a deterministic hash of the product's identity — the triple
`(filament_vendor, filament_type, family name)`, where the family name is the preset name
with its `@...` variant suffix stripped — producing an 8-character `OF*` code that is the
same for that product in every bundle, in every PR, on every machine. For example, Polymaker's
PolyLite PLA presets (`PolyLite PLA @base`, `PolyLite PLA@Q2-Series`, …) resolve
`filament_vendor` `Polymaker`, `filament_type` `PLA`, and family name `PolyLite PLA`; hashing
`filament_product/Polymaker/PLA/PolyLite PLA` yields `OF5CgdDq`, and that is the id the
OrcaFilamentLibrary, OrcaArena, Qidi, and Snapmaker bundles all arrive at independently
(derivation details in the Minting section).

**How it is used:** at runtime the id is the join key between hardware and profiles.
When a printer reports what a tray holds (Bambu AMS, Qidi box, Creality CFS,
Klipper, Snapmaker), OrcaSlicer matches the reported id against the filament presets
compatible with that printer to select the right profile; other features — tray display
names, support-material detection, vitrification warnings, multi-nozzle filament grouping —
look up material properties by id alone. When an id has been retired, a shipped succession
ledger forwards it to its successor so old trays and records keep resolving.

This page is the rule for authoring `filament_id` in system profiles
(`resources/profiles/**`). CI enforces everything below; the short version is:

> [!IMPORTANT]
> **Never write a `filament_id` value by hand.** New families get their id from
> `python scripts/assign_filament_ids.py`; existing families already have one — inherit it.

## The design, in three pieces

Because several consumers match **globally by id alone, first hit wins** (see the next
section), any two materials sharing one id feed wrong data somewhere — a wrong tray name, a
wrong support-material flag, a wrong nozzle grouping — and inside one printer a duplicated id
makes AMS spool matching a coin toss. Hand-written ids produce such collisions constantly, so
the system is built to make them impossible and to make identity corrections safe:

1. **Deterministic minting.** An id is a pure hash of the product's identity — no registry to
   maintain, no next-free-number ceremony, no way for two concurrent PRs to race for the same
   number, and no way to get it wrong by hand, because you never write it by hand.
2. **A sanctioned snapshot.** The complete id landscape derived from the tree must equal
   `scripts/filament_id_snapshot.json` exactly, so every change to ids, claims (which bundles
   ship which id, and for which family), or product identity surfaces as a reviewable diff to
   one file — the maintainer gate.
3. **A shipped succession ledger.** Ids live outside the tree too — on device trays, in
   calibration records, in user presets, in project files. When an id is retired, the ledger
   forwards it to its successor so none of those references degrade to a generic fallback.

## Who consumes the id

The canonical consumer is tray-to-preset matching: a device reports a tray material id
(`tray_info_idx`), and the shared matching pipeline (`PresetBundle::sync_ams_list` and
friends) resolves it to a preset. The matcher is printer-scoped and first-match-wins:
scanning only compatible family roots — system roots plus user-made custom filaments, which
are user roots carrying their own `P*` ids; a preset derived from another resolves through
its root and never matches directly — it picks the first one whose `filament_id` equals the
tray's and retries once through the succession ledger on a miss. Only then does it fall back
by filament type: a system `Generic <type>` preset (matched by name, then by type
similarity), else the slot's previous selection, else any compatible system generic or,
failing that, any compatible system preset, else the slot is skipped — every fallback
selection surfaces a user-visible notice.

Today only the Bambu AMS integration follows this pattern end to end — the device itself
reports the id, and the pipeline does all the matching. The other device integrations still
synthesize a preset id client-side in their agents (by type, brand, or color lookups against
the loaded presets) before the pipeline runs; they are intended to converge on the same
pattern, with the device-reported tray material id flowing through the shared matcher.

| Ecosystem | Where the tray id comes from today |
| --- | --- |
| Bambu AMS | the device itself (RFID / user tray setting) — the `GF*` catalog |
| Qidi box | composed at runtime as `QD_<series>_<vendor>_<typeidx>` — vendor and type indices from the device's per-slot saved variables, the series digit inferred client-side from the printer model/name — then translated through the succession ledger to the family's minted id (the `QD_*` values themselves no longer exist as preset ids — see the reserved-namespaces section) |
| Creality CFS | runtime brand/type scoring returns the winning preset's id |
| Klipper (AFC / Happy Hare) | runtime lookup by filament type |
| Snapmaker | runtime color/vendor/type match |

Tray-to-preset matching is printer-scoped, but **several consumers match globally by id alone,
first hit wins**: tray display names, `filament_is_support`, vitrification warnings, and
multi-nozzle filament grouping in the slicing pipeline (`FilamentGroup::try_merge_filaments`
merges plate slots sharing one `(filament_id, color)` pair, with matching
extruder-printability, onto one nozzle group; the engine is implemented but no grouping path
calls it yet).
Two *different* materials sharing one id
feed wrong data to those consumers even when the presets live in different vendors — so
cross-material id sharing is never safe. Within one printer, duplicate ids break AMS matching:
the matcher picks whichever preset loads first (it now logs an "Ambiguous AMS filament match"
warning, but the pick is still arbitrary) and the tray-edit dialog, which lists one entry per
id, hides the second preset entirely. The profile validator's `-f` check
(`check_filament_subtypes` → `PresetBundle::check_duplicate_filament_subtypes`) rejects this
per printer, and CI runs it tree-wide.

Two more consumer-side facts worth knowing:

- The machine-facing dialogs (AMS tray edit, AMS dry control, calibration history, extrusion
  calibration) offer the filaments a connected printer can use by the same compatibility rule
  the plater uses (an empty `compatible_printers` means *every* printer). Alias shadowing
  still applies: a vendor's same-name profile supersedes the library generic. That is what
  puts Orca Filament Library materials in those lists — deduplicated to one entry per
  `filament_id` in the AMS and calibration-history dialogs, while extrusion calibration
  deliberately lists every matching preset by full name.
- The id is load-bearing at startup: an instantiated system filament (one marked
  `"instantiation": "true"` — see the structure rules) that resolves **no**
  `filament_id` anywhere in its `inherits` chain is a hard load error in the C++ loader
  (`Can not find filament_id for <name>`) that discards the entire vendor bundle (for the
  OrcaFilamentLibrary itself the failure is messier: library presets loaded before the
  failing one survive, and every vendor bundle whose filaments inherit from the library is
  then discarded for want of a base). CI's structure check catches this before it ships.

## Do I need a new id? The one-question test

> **Would a user consider this a different spool product than anything already in the tree?**

Different polymer, different sub-brand (Basic / Matte / Silk / HF), fiber-filled sibling, or a
second selectable diameter → **new family, new id**. The same spool tuned for another printer
or nozzle → **join the existing family** (inherit its `@base`, write no id key). Tuning a
generic material → **join the OrcaFilamentLibrary family** (inherit `Generic X @System`, keep
the `Generic X` base name, write no id key).

| Situation | id |
| --- | --- |
| Per-printer / per-nozzle variant of an existing material | same id (inherit, never write the key) |
| Sub-brand or product line (PLA vs PLA Matte vs PLA Silk vs PLA HF) | new id each |
| Color | never a new id |
| Second diameter of the same product (1.75 + 2.85) | sibling family, new id |
| "High-speed" tuned for a *different printer model* | same id (it is a printer variant) |
| "High-speed" selectable *alongside* the normal preset on one printer | new id (it is a product line) |

## Structure rules

1. **Only family roots carry the key.** Root presets (any preset *not* marked
   `"instantiation": "true"`, typically `<Family> @base` with `"instantiation": "false"`)
   declare `filament_id`; instantiated variants inherit a root and never write the key.
   A family may have several roots — Qidi's PolyLite PLA has four per-series roots
   (`PolyLite PLA@Q2-Series`, `@Q2C-Series`, `@X-Max 4-Series`, `@X-Plus 5-Series`) — and all
   of them must declare the *identical* id. This is the authoring rule for new work; a large
   grandfathered tail of older presets breaks it in two ways — keys written on instantiated
   presets (frozen in the snapshot's `instantiated_with_id` list) and keys that override the
   id the preset would inherit from its family (frozen in `id_overrides`) — and CI ratchets
   so no new preset joins either tail.
2. **The family name is the base name**: the preset name with everything from the first
   (optionally space-preceded) `@` stripped. `MyBrand PLA @Orca 3D Fuse1` and `MyBrand PLA@HS`
   both belong to family `MyBrand PLA`.
3. **Within a family, variants' `compatible_printers` are pairwise disjoint** — per printer,
   at most one compatible instantiated preset per id, or AMS matching turns ambiguous. The
   C++ validator's `-f` check enforces this.
4. **Generics belong to OrcaFilamentLibrary.** A vendor tuning a generic material inherits
   `Generic X @System`, keeps the `Generic X` base name (that alias is what hides the library
   preset on your printers), sets a non-empty `compatible_printers`, and writes no id key —
   e.g. `Generic PLA @Sovol SV08 MAX` inherits `Generic PLA @System` and lists three Sovol
   nozzles. A vendor-*branded* filament never rides a generic family id.
5. **Ids follow the product identity.** The id is a pure function of the product triple
   `(filament_vendor, filament_type, family name)` — correcting any of them re-mints the id
   **by design** (via `--remint <Vendor>`; the exact sequence is in the FAQ), and
   `--update-snapshot` records the old id in the shipped succession ledger with its successor
   so device trays, calibration records, and user presets keep resolving (`renamed_from`
   still gates preset-*name* compatibility as before). A shipped id is never recycled for a
   different material: retired ids are blocked forever.

## Minting — nobody invents ids

New ids are deterministic, computed exactly like the `setting_id` precedent
(`scripts/assign_vendor_setting_ids.py`):

```text
FILAMENT_ID_NAMESPACE = uuid5(setting-id NAMESPACE, "filament_id")
                      = c4d3ff49-4c32-5534-a3e3-00894157ab97
filament_id = "OF" + base62_6( uuid5(FILAMENT_ID_NAMESPACE,
                  "filament_product/<filament_vendor>/<filament_type>/<family_name>") )
```

`base62_6` is the low 6 base62 digits (alphabet `0-9A-Za-z`) of the UUID taken as a big-endian
integer, most-significant digit first; with the `OF` prefix the full id is 8 chars, within the
AMS length limit. The triple comes from the family root's *flattened* config:
`<filament_vendor>` is the filament
**manufacturer** (`"Polymaker"`, or `"Generic"` for generics — never the printer brand),
`<filament_type>` the material type, `<family_name>` the root's base name; the two config
values are inheritable list options and the first element counts.

Content-addressing on that triple is what makes the whole system converge. The key contains no
bundle name, so the same product mints the same id in every bundle — hoisting a family into
OrcaFilamentLibrary never changes its id, and two vendors independently shipping the same
product arrive at the same id without coordinating. `Polymaker/PLA/PolyLite PLA` mints
`OF5CgdDq`, and that one id is declared by the OrcaFilamentLibrary, OrcaArena, Qidi, and
Snapmaker bundles alike; the OFL generic `Generic/PLA/Generic PLA` mints `OFDSrzZ8`, claimed
by ten bundles — most by independent declarations converging on the same mint, the rest
purely through inheritance from the OFL family.

On the rare collision with any existing or retired id, the minter salts the input (`…/1`,
`…/2`, …) until free, and the result is frozen in the profile file. Salting is also used
deliberately: a *salt split* keeps two presets of one product on distinct ids where a single
id would be AMS-ambiguous on the same printer — the "selectable alongside" situation from the
table above, resolved without inventing a second family name. The tooling recognizes salt
iterations of a triple as conformant and preserves such splits across re-mints.

Workflow for a new family:

```bash
# 1. Author the family with NO filament_id key anywhere.
python scripts/assign_filament_ids.py                    # 2. mint + insert ids into the family root(s)
python scripts/assign_filament_ids.py --update-snapshot  # 3. record the new claims in the snapshot
python scripts/assign_filament_ids.py --check            # 4. verify — the same checks CI runs
# 5. Commit the profile edits together with scripts/filament_id_snapshot.json.
```

The default run is idempotent and never rewrites a valid existing id; it edits profile files
byte-preservingly (indentation, BOM, and line endings intact) and re-parses them to fail
loudly. `--mint "filament_vendor/filament_type/family_name"` prints the id a **new** mint of
that triple would get, without touching anything — note that for a triple whose id already
exists it prints the next *free* salt iteration, not the live id (asking for
`Polymaker/PLA/PolyLite PLA` today prints the salt-1 id, because `OF5CgdDq` is taken).

Maintenance modes (`--remint` is also the step for identity fixes — see the FAQ; the rest are
normally only used by id migrations):

- `--remint VENDOR` re-derives a vendor's declared ids from their triples. A declaration
  already equal to a salt iteration of its own triple is conformant and left alone, so
  deliberate salt splits survive; convergence onto an id another bundle already uses for the
  *same* triple is legal by design — that is the point.
- `--drop-redundant-ids VENDOR` deletes declarations that merely re-declare an inherited
  OrcaFilamentLibrary id.
- `--add-hint "OLD=NEW"` records a succession hint for an id a foreign catalog owns
  (`GF*` only — see the ledger section).
- `--retire "OLD=NEW"` records succession for a shipped id that vanished while another
  declarer kept it alive — lineage the automatic claim vote (see the succession-ledger
  section) can no longer see.
- `--forget-never-shipped FILE` (with `--update-snapshot`) — given a JSON list of ids that
  never shipped in any release, drops them from the lineage entirely (no retirement entry)
  and splices succession chains that pointed through them.
- `--profiles DIR` points the tooling at a different profile tree (default
  `resources/profiles`).

If you skip the tooling, CI fails and prints the remedy: the expected id for your family and
the instruction to run `python scripts/assign_filament_ids.py`; once the id is minted, the
snapshot checks likewise point at `--update-snapshot` and tell you to commit the resulting
diff.

## Reserved namespaces — never mint or hand-write into

An **island** is a frozen id namespace exempt from the mint rule because an external catalog
or device contract owns it.

| Space | Status | Rule |
| --- | --- | --- |
| `GF*` | Bambu AMS/RFID catalog — the one remaining *island* | BBL vendor only; a claim by anyone else is refused unless the snapshot sanctions that exact claim (via `--allow-shared-catalog` — see the CI section) |
| `QD_*` | Qidi device protocol — *dissolved island* | declarable by **nobody**, Qidi included; every shipped `QD_*` id is retired in the ledger with a live successor |
| `P` + 7 hex chars (case-insensitive), `"null"` | user-created custom filaments (`CreatePresetsDialog.cpp`) | never appears in system profiles |
| every already-shipped id | frozen in the snapshot (grandfathered) | frozen as-is; new claims need maintainer sign-off |
| every retired id | `resources/profiles/retired_filament_ids.json` | never used again, for anything |

The two device namespaces, in detail:

- **BBL (`GF*`).** Bambu's device/RFID/cloud catalog is external and opaque, so BBL
  declarations are frozen as-is and never re-minted. BBL also carries several dozen
  grandfathered legacy codes that are neither `GF*` nor `OF*` (the `BETA` family's `B*` ids,
  plus `Generic SBS`'s legacy `BFLSBS99`) — frozen the same way, via the snapshot.
- **Qidi (`QD_*`) — dissolved.** `QD_*` is a device-*protocol* namespace, not a preset id
  space: the Qidi box path composes `QD_<series>_<vendor>_<typeidx>` ids at runtime (slot
  vendor and type indices reported by the device, the series digit inferred client-side
  from the printer model/name), and the Qidi agent translates a composed id through the
  succession ledger to the family's minted id. Qidi presets themselves carry ordinary minted
  `OF*` ids (generics share the OFL ids), and all 204 `QD_*` ids that ever shipped sit in
  the ledger as retired entries with live successors — which is also what makes CI refuse
  any future `QD_*` occurrence permanently. The alternative — treating per-series protocol ids
  as preset ids — would put one product under five ids (`QIDI PLA Rapido` would be `QD_0_1_1`
  through `QD_4_1_1`), exactly the fragmentation the mint rule removes.

## The succession ledger

`resources/profiles/retired_filament_ids.json` ships with the app and has two maps:

- **`retired`** — ids Orca owned and withdrew. Each maps to
  `{"claims": [...], "successor": <id|null>}`; successor chains are followed to the live end.
  When `--update-snapshot` retires a vanished id, it picks the successor by an automatic
  *claim vote*: every old claim whose family still exists votes for that family's current id,
  most votes wins. Alongside the deliberate re-mints, this map carries the whole pre-rule
  legacy — old ad-hoc vendor codes, Creality's and Snapmaker's numeric ids, and the 204
  `QD_*` protocol ids.
- **`hints`** — the same forwarding for ids Orca *cannot* retire because a foreign island owns
  them and may legitimately keep shipping them. That means `GF*` only: e.g.
  `GFL99 → OFDSrzZ8` forwards Bambu's Generic PLA catalog id to the OFL generic family on
  installs where no live preset carries `GFL99`.

The client consults the ledger **only on resolution miss** — AMS tray sync and the sync-AMS
dialog's filament listing, the sidebar AMS dropdown, tray-id type lookup, calibration
history, the global filament-id preset lookup, and the Qidi box path. A live preset always
wins first, so BBL installs resolve `GF*` natively and behavior is unchanged wherever the
raw id still exists. This on-miss rule is what makes
identity-driven re-mints (structure rule 5) safe: the old id keeps resolving to the family's
current preset instead of degrading to a `Generic <type>` fallback. The file is append-only in
its keys — entries are never removed and a retired id never returns — and maintained
exclusively by `--update-snapshot` / `--add-hint` / `--retire` (`--forget-never-shipped`, an
`--update-snapshot` mode, may rewrite an existing entry's successor when splicing chains).

## How CI enforces this

Profile CI (`check_profiles.yml`) runs `check_filament_ids()` tree-wide via
`scripts/orca_extra_profile_check.py`. Its ground truth is
**`scripts/filament_id_snapshot.json` — the sanctioned state**: the id state derived from the
tree must equal the snapshot exactly, in both directions. Any change to the id landscape
therefore surfaces as a diff to that file, and **that snapshot diff is what maintainers review
and gate in a PR**. Never edit the snapshot by hand — `--update-snapshot` regenerates it
deterministically (running it twice changes nothing). Besides the live `ids` and `triples`
maps, the snapshot carries grandfather lists (`instantiated_with_id`, `id_overrides`,
`alias_exceptions`, `triple_exceptions`) that freeze pre-existing structure debt while the
checks ratchet all new profiles to the clean rules.

The checks, in brief:

- **Format** — every id is either in the snapshot, `OF` + 6 base62 chars, or BBL's.
- **Snapshot equality** — tree claims == snapshot claims **and** tree triples == snapshot
  triples, both directions: any `filament_vendor`/`filament_type`/family-name change surfaces
  as a snapshot diff.
- **Mint conformance** — a non-grandfathered `OF*` id must equal the mint (or a low salt
  iteration) of its declarer's product triple; the error prints the expected id to paste into
  the family root.
- **Retired reuse** — any tree id present in the `retired` map of
  `resources/profiles/retired_filament_ids.json` is an error, with no grandfathering (keys in
  the same file's `hints` map may legitimately stay live — BBL still ships them). Ids that
  fully vanish from the tree are appended to the `retired` map by `--update-snapshot`; ids
  are only ever added there, never removed or reused.
- **Alias hygiene** — a vendor preset riding an OFL family id must keep the library preset's
  base name (a rename re-exposes the library preset, since alias shadowing is name-based),
  a non-empty `compatible_printers` (an empty one shadows nothing), and no own id key
  (structure rule 4).
- **Triple integrity** — every declarer outside the BBL island must resolve a non-empty
  `filament_vendor` and `filament_type` (generics use `"Generic"`), and all declarers of one
  family within a bundle must agree on the triple.
- **Succession integrity** — retired successor chains terminate at a live id (or null) with
  no cycles; `hints` keys stay in the island namespace (`GF*`), are never also retired, and
  their chains end at a live tree id.
- **Reserved namespaces** — `GF*` outside BBL, `P<7-hex>` or `"null"` anywhere, unless that
  exact claim is grandfathered in the snapshot; `QD_*` anywhere, with no exception (all are
  retired).
- **Structure** — no `filament_id` key on newly instantiated presets; no new
  declared-vs-inherited id drift; every instantiated system filament must resolve an
  effective id through its `inherits` chain (recall: an id-less one is a hard load error in
  C++ that discards the whole vendor bundle).

Sharing a **reserved-catalog** id with a new family or vendor (e.g. shipping a Bambu-cataloged
product under another vendor with its authentic `GF*` id) is refused by `--update-snapshot`
unless you pass `--allow-shared-catalog` — and it still lands in the snapshot diff for
maintainer review. Any other new sharing via a *declared* id is caught by the mint-conformance
check; sharing through inheritance carries no declaration to check and surfaces only as a new
claim in the snapshot diff — which is exactly why that diff is the gate.

Complementing the Python checks, CI also runs the C++ profile validator with `-f`
(`check_filament_subtypes`): it loads the bundle exactly as the app does and flags any printer
for which two or more compatible filament presets share one `filament_id` — the runtime-shaped
ambiguity check behind structure rule 3.

## FAQ

- **A new color of an existing product?** Never a new id — colors are not families.
- **A second diameter (1.75 mm and 2.85 mm) of the same product?** A sibling family with its
  own id: two diameters are separately selectable spool products.
- **A high-speed tune of an existing material for another printer model?** Same family:
  inherit the family's root, write no id key.
- **A tuned generic ("our profile for Generic PLA")?** Inherit `Generic PLA @System`, keep the
  `Generic PLA` base name, set `compatible_printers`, write no id key.
- **I need to fix a family's `filament_vendor` or `filament_type`.** Fix the config, run
  `--remint <Vendor>` then `--update-snapshot`: the id re-derives from the corrected identity
  and the old id lands in the succession ledger pointing at the new one. Commit the profile,
  snapshot, and ledger diffs together.
- **I need to rename a family.** Renames need one extra step, because the succession vote
  follows family names (`renamed_from` gates preset-name compatibility but is not read by the
  id tooling): rename the presets (adding `renamed_from`), run `--update-snapshot` once to
  sanction the renamed claims on the old id, then `--remint <Vendor>`, then `--update-snapshot`
  again — the old id now lands in the ledger pointing at the new one. Re-minting first would
  retire the old id *heirless* (successor `null`), which cannot be repaired afterwards.
- **Can I reuse a `QD_*` id for a Qidi profile?** No — nobody can. The namespace is a
  dissolved island: the device still composes those ids at runtime, and the succession ledger
  translates them to the families' minted ids. Author Qidi filaments like any other vendor's.
- **CI says my family needs an id.** Run `python scripts/assign_filament_ids.py`, then
  `--update-snapshot`, and commit both diffs. Do not type an id by hand.

For general profile authoring, see the profile development guide on the
[OrcaSlicer wiki](https://www.orcaslicer.com/wiki).
