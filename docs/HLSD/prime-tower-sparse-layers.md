# Prime tower sparse layers — High Level Design

## Purpose and scope

A prime tower exists to absorb filament changes, but it is planned on every
object layer below the topmost change, not only on the layers that purge. The
layers in between carry no filament change and print nothing but a block of the
tower's own footprint to keep its top level. They are called sparse layers, and
on a print with few changes they are most of the tower: they cost time, filament
and a travel to the tower on every layer.

Two settings trade that cost against something else. `wipe_tower_no_sparse_layers`
drops them, which sinks the tower below the model. `wipe_tower_sparse_layers_combination`
merges runs of them into fewer, thicker layers, which keeps the tower level with
the model. Both are off by default, and with both off the tower prints one layer
per object layer as it always has.

The decisions belong to tower planning and G-code emission. They do not change
sliced object geometry, but they do change the emitted G-code, the filament and
time estimates, and — for the compacted case — whether a plate is printable at
all. Changing either setting invalidates the tower step.

## What a sparse layer is

`ToolOrdering::fill_wipe_tower_partitions` counts the filament changes per layer
and propagates that count downwards, so every layer below the topmost change is
marked as carrying a tower. It then fills any gap between two tower layers, so
the tower is continuous from the bed to its last purge. `wipe_tower_layer_height`
is the distance from the previous tower layer, which is the object's layer height
whenever the tower prints on every layer.

A tower layer thinner than the printer's minimum layer height (by more than
`EPSILON`, so layers sliced at exactly the minimum stay apart; never less than the
towers' own 1e-3 mm merge) is not printed on its own: it joins the tower layer
below. Precise Z height leaves such layers when it ends each object at its exact
top, moving one object's last layers off another's by anything up to a layer
height; a purge sized for a layer a few microns thick would run hundreds of mm.
A joined layer takes `wipe_tower_print_z` and `wipe_tower_layer_height` from the
tower layer below, and its purges print down at that tower layer's z: the emitter
travels over the tower first, descends, and climbs back to the object layer, the
same sequence it uses for a tower compacted below the objects. After a custom
`change_filament_gcode` the descent happens wherever the macro leaves the head, so
a macro has to end over the tower, as it already must for the compacted tower.
Layers from the minimum layer height up stay tower layers of their own, sized for
their height.

The emitter takes one tower result per toolchange, and a layer without one takes
a result only to finish the tower layer. So `wipe_tower_layer_ends` marks the last
of the tower layer and its joined layers that changes tools, or the last of them
when none does: a joined layer that changes nothing after the results are all
taken has nothing left to finish with. A joined layer also does not reset the
Type 1 tower's first print, so smooth timelapse prints each tower layer's wall
once.

`Print::_make_wipe_tower` plans every such object layer at its
`wipe_tower_print_z`: one tower layer per object layer, except that a joined
layer's toolchanges land in the tower layer below. A layer
whose only call keeps the current filament leaves no toolchange in the plan, and
the layer it generates is a single result whose initial and new tool are equal.
That is what `wipe_tower_layer_is_sparse` recognises, and it is the unit both
settings work on.

The plan stays one entry per tower layer in every case. The G-code emitter walks
`WipeTowerData::tool_changes` by layer index, advancing once per object layer
that carries a tower and does not join the one below, so a planner that removed
entries would silently shift every later layer onto the wrong tower geometry. Layers that print nothing are
therefore still planned and still generated; they are marked, and the emitter
drops them.

## Shared rules

Tower planning, G-code emission and the plate validation all have to agree about
which layers print and where. They ask one set of free functions, declared beside
the tower classes, rather than each re-deriving the answer from the raw options:

- `wipe_tower_sparse_layers_skipped` — whether sparse layers are really dropped.
  Smooth timelapse and clumping detection park the nozzle on the tower every
  layer, so with either of them on no layer is ever dropped and the option reads
  as off everywhere.
- `wipe_tower_sparse_layers_combined` — whether runs are really merged. The same
  two rule it out, and so does `wipe_tower_no_sparse_layers`: dropping the layers
  outright is the stronger answer to the same problem, so the two settings are
  exclusive and the GUI greys out the second while the first is on.
- `wipe_tower_layer_is_sparse`, `wipe_tower_layer_is_combined_away` — per-layer
  questions the emitter asks about generated results.
- `compute_compacted_wipe_tower_z` — the tower's print z per planned layer when
  it is compacted.
- `combine_sparse_wipe_tower_layers` and its `combine_sparse_wipe_tower_plan`
  wrapper — the merge rule, applied to either generator's plan.

Both tower generators are driven through these. `WipeTower` (Type 1, the block
tower) and `WipeTower2` (Type 2, the default) keep separate plans with the same
per-layer shape — print z, layer height, toolchanges, and a `combined_away` flag
— so one template covers both.

## Dropping sparse layers

With `wipe_tower_no_sparse_layers`, the tower only grows on layers that carry a
real change. It therefore falls one layer height behind the object for every
sparse layer, and by the top of a tall print it can sit far below the model. The
nozzle has to reach down to it at each purge.

`compute_compacted_wipe_tower_z` derives that z once, from the generated results,
so the emitter and the validator cannot disagree. Emission descends to it, but
only once the nozzle is parked over the tower: descending while still over the
model would drive the nozzle into the print, so a descent that would do that is
deferred until after the travel to the tower. Extrusions emitted without an
explicit z — the nozzle-change wipe in particular — are pulled down to the
compacted z for the same reason.

Reaching down is only safe if nothing tall stands near the tower. `Print.hpp`
carries the clearance rule: a keep-out zone grown from the tower's footprint by
the spiral z-hop envelope, and a per-object limit on how high an object may rise
near it, tiered by the nozzle cone, the head body, the rod and the lid. The
precise check applies all four to the real extrusions, measuring an object's
rise from the tower as printed by then. Before slicing that height is unknown,
so the pre-slice estimate that feeds the plater, and the outlines the plater
draws while an object is dragged, measure the rise from the plate and apply only
the nozzle cone and the head body. An object away from the toolhead, which only
the rod or the lid can reach, is left to the precise check. The ring the user
sees touches the object's outline exactly when the pre-slice check trips.

## Merging sparse layers

With `wipe_tower_sparse_layers_combination`, no layer is dropped and nothing is
compacted: the tower keeps following the object, and the nozzle never descends.
Instead a run of consecutive sparse layers prints once, on the run's last layer,
at the accumulated height of everything it covers — the same way infill
combination merges sparse infill. The layers below it in the run print nothing.

`combine_sparse_wipe_tower_plan` runs before the tower's depths are planned,
because the heights it rewrites feed the extrusion flow of every later pass. It
raises `height` in place on the layer that prints a run and sets `combined_away`
on the rest; generation then proceeds unchanged, and the flag is copied onto the
results so the emitter can drop them.

Four constraints shape the rule:

- **Whole layers only.** A tower layer is entered at the object's z, so a merged
  layer has to end on an object layer boundary. The merged height is therefore a
  sum of whole layer heights, never a clamped value.
- **The nozzle's maximum layer height.** A run stops growing as soon as one more
  layer would pass `max_layer_height` for the nozzle printing it — three quarters
  of the nozzle diameter when that is left at 0, as elsewhere in slicing. The cap
  is read through the filament-to-nozzle map, since `max_layer_height` is per
  nozzle while the tower indexes filaments. This is what makes the setting inert
  at common layer heights: two 0.2 mm layers are 0.4 mm and do not fit under a
  0.3 mm maximum, so nothing merges until the layer height is 0.15 mm or below,
  or the maximum is raised.
- **A filament change purges at its own z.** A layer with a real change can
  neither be merged away nor absorb the run below it, so a run always ends on its
  own last sparse layer and the change above it is untouched.
- **The first layer stays on the bed.** It carries the brim and is never merged.

A run holds one filament throughout — that is what makes it sparse — so the cap
is uniform across it, and the tower reserves depth only for the purges above a
layer, so a run has one footprint and the merged layer covers exactly the area
the layers it replaces would have.

## Emission and accounting

`WipeTowerIntegration` drops a layer whose results are marked, for both settings,
through the same `ignore_sparse` path in `tool_change` and
`is_empty_wipe_tower_gcode`. A dropped layer emits no travel to the tower and no
extrusion.

Filament used is accumulated by the generators while they write, so a layer that
will be dropped must not be charged. Type 1 asks `layer_is_printed` at each of
its accumulation points; Type 2 guards the equivalent block in `finish_layer`,
which also stops a merged-away layer from adding height of its own — the layer
that prints the run carries all of it.

A merged tower layer can be thicker than the object layer it sits on. Both
generators declare the tower's layer height in their own G-code blocks.
`Print::_make_wipe_tower` selects the printer's tag format before either
generator runs, so the G-code processor recognises the tower's height and
extrusion role. The object's height returns on the next object path, because
emission forces the processor role to the tower on any layer that carries one.

## Constraints

A layer that prints nothing prints nothing at all, including any interface work
the tower planner scheduled there. The Type 1 block planner marks a layer as a
contact layer when a filament category stops or starts being used relative to the
layer below, and a sparse layer immediately above a change qualifies. Merging a
run, like dropping its layers, replaces that interface with the run's single
layer. Both settings are off by default for this among other reasons.

Neither setting changes what the tower is for. A plate that needs a tower on
every layer — smooth timelapse, clumping detection — gets one, and the settings
read as off rather than compacting or merging in one place and not another.

## Implementation and verification

- [WipeTower.hpp](../../src/libslic3r/GCode/WipeTower.hpp) declares the shared
  rules and the plan-merging template;
  [WipeTower.cpp](../../src/libslic3r/GCode/WipeTower.cpp) implements them and
  the Type 1 tower, [WipeTower2.cpp](../../src/libslic3r/GCode/WipeTower2.cpp)
  the Type 2 tower.
- [ToolOrdering.cpp](../../src/libslic3r/GCode/ToolOrdering.cpp) decides which
  layers carry a tower at all, and
  [Print.cpp](../../src/libslic3r/Print.cpp) plans it and runs the clearance
  check whose rule lives in [Print.hpp](../../src/libslic3r/Print.hpp).
- [GCode.cpp](../../src/libslic3r/GCode.cpp) emits the tower, drops the layers
  that print nothing, and declares a merged layer's height;
  [PrintConfig.cpp](../../src/libslic3r/PrintConfig.cpp) defines the settings and
  [ConfigManipulation.cpp](../../src/slic3r/GUI/ConfigManipulation.cpp) their
  mutual exclusion.
- [GLCanvas3D.cpp](../../src/slic3r/GUI/GLCanvas3D.cpp) and
  [PartPlate.cpp](../../src/slic3r/GUI/PartPlate.cpp) draw the compacted tower's
  keep-out outlines live while the user drags.
- [Rule tests](../../tests/libslic3r/test_wipe_tower.cpp) cover the gating of
  both settings, the per-layer predicates, the compacted z, the merge rule's run
  flushing, height conservation, the nozzle cap and the first-layer exemption,
  and the clearance geometry the plater draws.
- [Slicing tests](../../tests/fff_print/test_wipe_tower.cpp) slice a real print
  and check that a run folds, that the tower still covers the object exactly
  once, that a run too thin for the cap is left alone, that a merged layer
  declares its height in the tag the printer's processor reads, and that layers
  precise Z height moves less than a minimum layer height off another object's
  join the tower layer below and print at its z, with and without their own
  toolchange and with smooth timelapse, while layers sliced at exactly the
  minimum height do not join.
