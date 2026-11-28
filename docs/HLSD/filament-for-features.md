# Filament for features

## Purpose and scope

On a multi-material printer, each feature of an object can print with its own
filament: walls in a colour, infill in a cheaper filament, a brim in a soluble
one. The Multimaterial page groups these selectors under *Filament for
Features*. The support selectors work the same way but sit in their own group
on the Support page.

Every selector holds a 1-based filament number, and 0 shows as "Default". The
keys live in three config classes, according to the scope of the feature:

| Key | Config | Mixed slots |
| --- | --- | --- |
| `outer_wall_filament_id`, `inner_wall_filament_id`, `sparse_infill_filament_id`, `internal_solid_filament_id`, `top_surface_filament_id`, `bottom_surface_filament_id` | `PrintRegionConfig` | allowed |
| `brim_filament_id`, `support_filament`, `support_interface_filament` | `PrintObjectConfig` | physical only |
| `wipe_tower_filament` | `PrintConfig` | physical only |

The region keys can be set on the process, an object, a part, a modifier or a
height range, and painting overrides them. The object keys can be set on the
process or an object. A region key may name a mixed-colour slot because tool
ordering resolves those per layer (`resolve_mixed_filaments()`). The other keys
are consumed directly by the brim, support and wipe tower code, so the GUI
offers them only physical filaments and resets a mixed value
(`ConfigManipulation`, `physical_only_keys`), and `Print::validate()` rejects a
mixed brim or wipe tower filament that arrives from a project or the CLI.

## What "Default" means

Region keys never reach the engine as 0. `region_config_from_model_volume()`
applies each scope in turn: the process, the object, the part, its material and
the height range. An explicit filament at any scope sets that feature. A 0 marks
the feature as following the part's own filament (`extruder`), which every scope
that carries one applies to the features nobody set explicitly. What is still
unset becomes filament 1. A painted region then sets all six keys to the painted
filament (`PrintApply.cpp`). From there on every `PrintRegion` names a concrete
filament for every feature.

Object and print keys keep 0 into the engine, and each gives it its own meaning:

- Support and support interface: no tool change; the support prints with
  whichever filament is active.
- Brim: the brim prints in the pass of the first filament that reaches the
  object on the first layer, see [Brim](#brim).
- Wipe tower: any filament, preferring a non-soluble one.

## Which filament prints an extrusion

`region_filament()` in `ToolOrdering.cpp` maps an island's extrusion collection
to a region key. Tool ordering and G-code export both go through it
(`LayerTools::extruder()`), so the tools planned for a layer are exactly the
tools the export asks for.

- Sparse infill uses the sparse infill filament.
- Solid infill uses the top surface filament for top surfaces and ironing, the
  bottom surface filament for bottom surfaces, and the internal solid filament
  otherwise. Gap fill among infill follows the surface it fills.
- Gap fill between walls uses the inner wall filament when the region has more
  than one wall, and the outer wall filament otherwise.
- A wall collection made only of inner walls uses the inner wall filament;
  anything else uses the outer wall filament.

A per-layer tool change from custom G-code (`extruder_override`) replaces all
region filaments on its layers.

## Tool ordering

`ToolOrdering::collect_extruders()` builds each layer's filament set from what
is actually printed there: the filaments of the regions' walls and fills, the
support filaments of support layers, the chosen brim filament on the object's
first layer, and the brim filament on every layer that carries a belt brim band.
The brim filament is added even when nothing else uses it, so a filament can be
loaded only for the brim.

## Brim

**Planning.** `getObjectExtruderMap()` gives each object one brim filament: the
chosen `brim_filament_id`, otherwise the lowest outer wall filament of the
object's first layer. `make_brim()` generates the brims in passes over the
first-layer filaments, and each brim records that filament
(`SkirtBrimGroup::Brim::filament_id`) for the nozzle-specific bed exclusions.
The brim line width uses the nozzle of the first object's chosen brim filament,
or of the first region's outer wall filament.

**Combining.** *Combine brims* merges a brim with a chosen filament only with
brims that chose the same filament, so each merged brim still has a single
filament. Brims left on Default combine as before.

**Emission.** A Default brim prints at the first visit of any of its instances,
in whichever filament's pass gets there first. This keeps the established
order, in which the brim usually goes with the outer wall filament because tool
ordering starts the first layer with it. Only nozzle-specific bed exclusions
make a Default brim wait for its recorded filament. A brim with a chosen
filament prints at the start of that filament's pass on the first layer,
before the pass visits any object (`GCode::process_layer()`), and the first
visits of other passes skip it. That filament may print nothing else on the
object, so the brim cannot rely on an object visit in its pass.

**Belt printers.** They never print the plate brim. Belt brim bands print with
`PrintObject::belt_brim_filament()`: the chosen brim filament, or the lowest
outer wall filament of the object.

**Invalidation.** On a flat bed the brim filament affects only the brim, the
tool ordering and the export, so changing it reruns those steps and keeps the
sliced object and its supports. The belt brim is generated with the supports
and takes its line width from the brim filament's nozzle, so on a belt printer
the change reruns the supports as well.

## Predicting the used filaments

Several places need the set of filaments a print or plate will use before, or
without, building the tool ordering. Each one derives it from the config with
the same rules: a feature filament counts only while its feature prints, such
as walls or a brim for the wall filaments, support for the support filaments,
and a brim for the brim filament.

- `Print::extruders()`: the G-code writer's filament list for by-object export,
  and validation.
- `PrintObject::printing_extruders()`: bed exclusion collisions and automatic
  filament grouping.
- `Print::append_model_object_printing_extruders()`: one model object's
  filaments without a `Print`. It resolves each part and modifier through
  `region_config_from_model_volume()` per layer range, as `Print::apply()` does,
  and skips modifiers that overlap no part and layer ranges above the object.
- `PartPlate::get_extruders()` and `get_extruders_under_cli()`: the plate's
  filament list for AMS mapping and filament checks, from
  `append_model_object_printing_extruders()`.
- `PartPlate::get_extruders_without_support()`: the same list without support,
  raft and brim, for the preview's layer slider.
- `PartPlate::get_object_filament_extruders()`: the bed exclusion preview.
- `project_used_filament_slots()` in `PublishSettingsDialog.cpp`: the filament
  slots a published project uses.

A filament missing from these lists can still be printed by the tool ordering
but is unknown to the consumer; in by-object export that means the writer has
no extruder for it.

## Filament count changes

When the filament count shrinks, `PresetBundle::full_fff_config()` resets
out-of-range region and brim keys to Default and clamps the support and wipe
tower keys, and `PrintObject::object_config_from_model_object()` does the same
for object overrides. Deleting a filament renumbers the support and brim keys of
objects, parts and the plater config (`ObjectList`, `Plater`).

## Compatibility

Older keys are renamed on load in `handle_legacy()`: `infill_extruder` and
`sparse_infill_filament`, `solid_infill_extruder` and `solid_infill_filament`,
`top_solid_infill_filament`, `bottom_solid_infill_filament`,
`perimeter_extruder`, `wall_filament`, `wall_filament_id`,
`inner_wall_filament` and `outer_wall_filament`. Their old value 1 meant "the
active filament" and maps to 0, Default; other values stay explicit.
`wipe_tower_extruder` and `support_material_extruder` are renamed as they are.
A part's `extruder` fills the region keys that are still 0
(`DynamicPrintConfig::normalize_fdm()`). Every key defaults to 0, so a project
or profile without a key keeps its behaviour.
