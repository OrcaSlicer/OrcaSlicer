# Material compatibility — High Level Design

## Why it exists

Two filaments printed against each other either fuse or they do not. PLA laid on PETG
peels; ASA laid on ABS welds. Every part of the slicer that puts one filament next to
another has to know which case it is in: the support that must come off, the part that
must not delaminate, the wipe tower that must survive its own height, the ironing pass
smoothing a surface, the mixed-colour slot alternating two spools inside one bead.

Without a shared answer each of those decisions invents its own. String equality is the
only relation an opaque `filament_type` offers, so "same material" becomes the rule
everywhere — which is both too strict (PLA and PLA-CF fuse; ABS and ASA fuse) and too
loose (it says nothing about PLA and PETG, so a hard-coded pair list is bolted on beside
it).

This subsystem is that shared answer. [MaterialType](../../src/libslic3r/MaterialType.hpp)
resolves any two filament type strings to one of three verdicts, and every decision above
is routed through it.

## The material database

Two data files ship in `resources/info/`:

| File | Contents |
| --- | --- |
| `material_types.json` | one entry per filament type: nozzle and chamber temperature ranges, adhesion coefficient, yield strength, thermal length, and the material families the type belongs to |
| `base_compatibilities.json` | per base material, which other base materials it is known to bond with and which it is known not to |

Both are versioned like the vendor profiles and mirrored into `<data_dir>/info`. The
shipped copy replaces the user copy whenever its `version` is newer, which is what makes a
table update reach an existing installation — so a change to either file only ships if its
version is bumped. A user copy with no version reads as `0.0.0`, so a corrupted one is
refreshed as well. Tools that must not write to their data directory (the profile
validator, the test binaries) load with mirroring off and read the shipped copy directly.

Keeping the tables as data rather than code is what lets a material be added, or an
adhesion rule corrected, without a rebuild; the list also feeds the `filament_type` value
list, so the filament dropdown and the database cannot disagree.

`MaterialType` also carries built-in fallback tables covering the common materials. They
exist for two reasons. The print config definitions are built during static
initialisation, before `resources_dir()` and `data_dir()` are known, so the
`filament_type` value list has to come from somewhere before any file can be read; and a
broken installation must degrade to a smaller table rather than to an empty one. Every
entry in the fallback is checked against the shipped file by the unit tests, so the two
cannot drift.

Loading therefore happens in two steps, once, as soon as both directories are known:
`MaterialType::load()` reads the files, and `refresh_material_type_config_defs()` re-reads
the `filament_type` value list from the loaded table. The global `print_config_def` is a
const reference to a mutable static and that free function is its only `friend`, so this
is the sole path by which the definitions can be rewritten. Both calls are made in
`CLI::setup()`, which runs ahead of the GUI as well as of a command-line slice, in the
profile validator, and — through
[test_slic3r_bootstrap.cpp](../../tests/test_slic3r_bootstrap.cpp) — in every test binary
that links libslic3r, so nothing silently runs on the fallback table.

After loading, the database is read-only. That is what allows the lookups to be called
from the slicing threads without locking. `find()` is indexed rather than scanned, because
it sits under `compatibility()`, which runs per layer and per filament pair.

## The verdict

A type declares the **material families** it belongs to: `PLA-CF` → `{PLA}`,
`PC-ABS` → `{PC, ABS}`, `PETG` → `{PET}`. A type with no families declared is its own
family. This is what keeps the compatibility table small: rules are written once per base
material and every variant inherits them.

Rules are listed per base material, symmetric, and each pair appears once. `"*"` is a
wildcard matching every other base — how a soluble support material states that it bonds
with nothing.

`compatibility(a, b)` compares every pairing of their families and returns:

- **Compatible** — the two share a family, or a pairing is explicitly listed as bonding.
  A material always bonds with itself, so a shared family wins even against a `"*"`
  incompatibility; callers never special-case equal types.
- **Incompatible** — some pairing is explicitly listed as not bonding. This wins over any
  other evidence.
- **Unknown** — the table says nothing either way.

The third verdict is the load-bearing one. The table cannot be exhaustive — new materials
appear faster than adhesion data for them does — so *absence of data must never read as
incompatibility*. Unknown never blocks anything; it warns, and it ranks between the other
two wherever a preference order is needed. `bonds()` is the shorthand for Compatible, and
is what callers use when only "will these fuse?" matters.

## Mixing alerts

Two independent properties decide whether filaments can be printed together: their nozzle
temperature ranges must overlap, and their materials should bond.
`Print::check_multi_filaments_compatibility()` evaluates both and returns a single verdict
naming the combination, so a pair that fails both says both:

`Compatible`, `HighLowMixed`, `InvalidTemperatureRange`, `IncompatibleMaterials`,
`PossibleIncompatibleMaterials`, `HighLowMixedAndPossibleIncompatible`.

A known incompatibility short-circuits: nothing a temperature check can add changes the
outcome.

Two checks consume the verdict, and they exist for different physical reasons.

**The shared-hotend check** (`check_multi_filament_valid`) covers the whole plate, or each
object in turn under by-object print sequence. Known-incompatible materials block slicing
unless the user removes the restriction — the `enable_incompatible_material_mixed_printing`
preference, or `--allow-mix-incompatible-material` on the CLI — in which case the same text
is shown as a warning instead. Unknown bonding is always a warning. Each dimension is
bypassed by its own preference, and a blocked message ends with where to allow it anyway.

A filament that the plate uses *only* as a support base, interface or ironing filament is
exempt from the bonding rule. Not bonding to the object is the entire purpose of such a
filament, and it is exactly what "Auto" selects for; applying the rule to it would reject
every print whose support filament does its job. A filament that also prints object
geometry is not exempt. The temperature rules apply to both.

**The object check** (`check_object_materials_valid`) covers the filaments fused together
inside one object: its parts and the modifiers carving into them, the painted regions, the
per-feature picks. These must bond or the object delaminates along the seam. It runs on
every printer, because a second nozzle keeps two materials out of one hotend but not out
of one part, and it is always a warning — the pairing may well be deliberate, and unlike a
shared hotend it endangers no hardware. Support is left out of this check on purpose. When
several objects have findings the worst one is reported, which needs an explicit severity
ranking: the verdict enum is ordered by meaning, not by severity.

## Support filaments

Three object settings name a support filament: `support_filament` (base and raft),
`support_interface_filament`, and `support_ironing_filament`. Each accepts a concrete
filament, `0` for **Default**, or `SUPPORT_FILAMENT_AUTO` (`-1`) for **Auto**.

### Auto

Auto picks, per object, a filament that does not bond to what the object is made of, so
the support detaches cleanly. `PrintObject::resolve_auto_support_filament()` collects the
object's own materials — volume extruders plus per-face painted ones — and considers every
filament that bonds with none of them. Candidates are ranked lexicographically:

1. a support material (soluble, or flagged as a support filament) over a plain one,
2. known-incompatible over unknown bonding,
3. the colour closest to the object's primary colour,
4. failing all of that, the lowest filament id.

Colour only ever breaks ties between equally good candidates: among filaments that release
equally well, the one that looks most like the object is the least intrusive choice. If
every filament bonds to the object, the pick falls back to the object's own filament — at
least the colour does not change. Auto resolves to Default, doing nothing, when support is
off, on single-extruder-multi-material printers, or when there is only one filament.

The three settings resolve together, in dependency order:
`resolve_auto_support_filaments()` resolves the interface and the ironing pass first — both
unconstrained, so they share one ranking and land on the same filament — and the base last,
so that **Avoid interface filament for base** can exclude the interface's pick. A setting
that is not Auto passes through untouched and still constrains the base.

Resolution happens in `object_config_from_model_object()`, so everything downstream reads a
concrete filament and never sees the sentinel. That placement has one consequence worth
stating: the inputs an Auto pick depends on — filament type, soluble and support flags,
colour — are filament-scope, and none of them appear in the object diff that normally gates
re-deriving an object's config. Objects using Auto are therefore re-derived on every apply;
clearing a soluble flag would otherwise leave the support sitting on that filament forever.
An unchanged pick produces an empty diff and invalidates nothing.

The GUI has to resolve Auto too, for two distinct reasons. Anything that enumerates the
filaments a plate prints with — the prime tower's existence, its arrange placement, the AMS
mapping — reads `PartPlate::get_extruders()`, and an auto-picked filament left out there
makes a single-material object look like a single-filament plate. And anything keyed on
"the user chose a support material" — the prompt offering the settings that make a soluble
interface work — would stay silent for Auto, leaving the support printing with the default
top Z distance, the very thing a support material is chosen to avoid.
`GUI::resolve_auto_support_filaments()` answers that second question: Auto resolves per
object while the two settings it reports on are global, so the objects on the current plate
are resolved in turn and the first one landing on a support material wins.

In the dropdowns, `DynamicSupportFilamentList` puts **Auto** ahead of **Default**, and
omits it on single-extruder-multi-material printers where it would be a no-op — a stored
Auto then reads back as Default. Because the list's entries shift when Auto appears or
disappears, the combos restore their selection by config value through the slot map they
were last built from, not by index.

### Default

Default means the supported object's own filament: support printed in the object's own
material, not in whatever happened to be loaded. When the object's filament is not printing
on the current layer, a filament already on that layer that bonds with it is used instead,
which avoids a tool change for no gain. The base additionally avoids the interface filament
and soluble filaments, so it stays distinct from the interface.

The choice is made in `GCode::process_layer()`, but tool ordering has to have planned for
it: support assigned to an extruder that was never planned onto the layer is dropped
silently, leaving a gap. `ToolOrdering::ensure_dontcare_support_extruders()` walks the
support layers of each object and adds the object's own filament wherever neither it nor
anything bonding with it is already planned. It runs after extruders have been collected
for every object and before "don't care" extruders are handled, and it is skipped entirely
when custom per-layer tool changes dictate the extruder — those fully determine the layer
and must not be fought.

## Ironing filaments

Ironing can print with a filament of its own: `ironing_filament` per region, and
`support_ironing_filament` per object. `0` means Default — the surface's own filament for
object ironing, the interface filament for support ironing. Selecting another filament lets
the pass use a smoother material for a cleaner finish, a non-bonding one so an ironed
support surface releases, or a zero-flow tool that only burnishes.

Because the ironing extruder is derived in several places, all of them consult the setting:
the region's ironing decision in [Fill.cpp](../../src/libslic3r/Fill/Fill.cpp), the
per-entity lookup and the per-layer collection in
[ToolOrdering.cpp](../../src/libslic3r/GCode/ToolOrdering.cpp), the region's printing
extruders in [PrintRegion.cpp](../../src/libslic3r/PrintRegion.cpp), the plate's filaments
in `PartPlate::get_extruders()`, and the G-code preview.

Support ironing needs one extra step in `process_layer()`. A support layer is split into
extruder groups: a base group and an interface group. The ironing pass joins whichever
group already uses its filament — the `prints_ironing` flag on that group — and only gets a
group of its own, carrying the `erIroning` role, when its filament is neither of the two.
A dedicated ironing group prints in the normal, non-wiping pass. Three places classify the
roles a support layer carries — extruder collection, the don't-care planning above, and
`process_layer()` — and all three have to agree on what a layer contains, which is why the
two inside tool ordering share one classifier.

Changing `support_ironing_filament` only changes which extruder prints the pass, not the
support geometry, so it re-runs tool ordering and G-code export without regenerating
supports.

## The wipe tower

The tower is a tall, thin structure built from whatever purges into it, so its walls are
the part most at risk of splitting: a layer of a material that does not bond to the ones
above and below it is a delamination plane through the whole tower.

There are two towers. The BBL tower ([WipeTower.cpp](../../src/libslic3r/GCode/WipeTower.cpp),
always used on BBL printers) organises purges into blocks by adhesiveness category. The
generic tower ([WipeTower2.cpp](../../src/libslic3r/GCode/WipeTower2.cpp)) does not, and
needs a filament chosen for it.

`wipe_tower_filament` names the filament that prints the tower's structure; `0` resolves it
automatically to **the print's most used filament**. "Most used" counts layers, not
extrusions, and weighs the material type before the individual filament — one material
split across two spools would otherwise lose to a single filament of a material the print
uses less. Soluble and support filaments never qualify; on the BBL tower they are counted
only if nothing else does.

Each tower then applies the same rule when finishing a layer, differing only in where the
decision lives:

- The **BBL tower** tracks, per block, the last filament changed into it that can build the
  structure — one that stays in the print and bonds with the tower's filament — and finishes
  the block with that rather than with whatever purged there last, which may be the soluble
  support. A layer that started on the tower filament and already printed its wall fills the
  rest with it too, merging into that wall. Its adhesiveness category remains the fallback
  for a layer with nothing compatible on it, since a shared category says nothing about
  bonding.
- The **generic tower** is planned in `ToolOrdering::insert_wipe_tower_extruder()`, which
  resolves the filament and forces a change to it only on layers printing nothing that bonds
  with it. `Print::_make_wipe_tower()` hands the resolved material type to the tower, which
  ranks the tools available on each layer — the resolved type, then a bonding one, then
  anything — and finishes with the best of them. Because tool ordering already planned a
  compatible filament onto every layer that lacked one, one is always available.

## Mixed filament slots

A mixed slot alternates its components inside a single printed body, so they have to bond.
`mixed_filament_types_compatible()` accepts identical types, and otherwise any pair the
database does not call incompatible — Unknown is allowed rather than blocked, as everywhere
else. The one addition is the support suffix: a support-flagged filament reads as `<type>-S`
here, a name the table does not carry, so the database returns Unknown for it against
everything — and Unknown is permission. Such a filament is made to peel off whatever it
touches, so it is rejected outright and blends only with itself.

Component sets are checked pairwise, never against the first component: compatibility is
not transitive, since A may bond with B and B with C while A and C are known not to. The
same predicate drives the consistency check, the recommended combinations, the dimming of
combo entries, and the message on a blocked confirmation, so the dialog cannot recommend a
combination it would then refuse.

## Constraints

- **Unknown never blocks.** Every consumer treats missing adhesion data as permission with
  a warning, not as refusal. The table is expected to be incomplete.
- **Support filaments are exempt from the bonding rule** wherever they are used only as
  support. Not bonding is their function.
- **The Auto sentinel is negative**, and the clamps applied when a full config is built
  accept it only for the three support keys. An older build clamps `-1` to `0`, so a project
  saved with Auto opens as Default rather than failing to load.
- **The data files are versioned**, and an installed copy is only refreshed by a version
  bump. A table change without one reaches new installations only.
- **The database is immutable after load**, which is what makes it safe to query from the
  slicing threads.

## Implementation and verification

- [MaterialType.hpp](../../src/libslic3r/MaterialType.hpp) /
  [MaterialType.cpp](../../src/libslic3r/MaterialType.cpp) — the database, the data-file
  loading and mirroring, and the verdict.
- [Print.cpp](../../src/libslic3r/Print.cpp) — the two mixing checks and their messages;
  [PrintObject.cpp](../../src/libslic3r/PrintObject.cpp) — the Auto resolver;
  [PrintConfig.cpp](../../src/libslic3r/PrintConfig.cpp) — the settings and the
  `filament_type` value list.
- [GCode.cpp](../../src/libslic3r/GCode.cpp) and
  [ToolOrdering.cpp](../../src/libslic3r/GCode/ToolOrdering.cpp) — Default support
  resolution, the support ironing groups, and the generic tower's filament.
- [Plater.cpp](../../src/slic3r/GUI/Plater.cpp) and
  [PartPlate.cpp](../../src/slic3r/GUI/PartPlate.cpp) — the Auto dropdown entry and the
  plate's filament enumeration; [GUI_App.cpp](../../src/slic3r/GUI/GUI_App.cpp) — the
  resolver the settings prompts read.
- [test_material_type.cpp](../../tests/libslic3r/test_material_type.cpp) covers the shipped
  files' loadability and versioning, the built-in fallback matching them, family resolution,
  and each verdict including the wildcard and symmetry.
- [test_print.cpp](../../tests/fff_print/test_print.cpp) covers the support-only exemption
  and the per-object warning;
  [test_printobject.cpp](../../tests/fff_print/test_printobject.cpp) the Auto ranking, the
  resolution order, re-resolution when filament properties change, and the effect on a
  plate's filament count;
  [test_wipe_tower.cpp](../../tests/fff_print/test_wipe_tower.cpp) that the tower is not
  built from a material it cannot bond to;
  [test_multifilament.cpp](../../tests/fff_print/test_multifilament.cpp) the ironing
  filament; and
  [test_filament_mixer.cpp](../../tests/libslic3r/test_filament_mixer.cpp) the mixed-slot
  predicate.
