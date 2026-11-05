# Decoupling filaments from tools

A multi-tool printer normally limits a project to one filament per tool: filament *n* is tool
*n*, and the filament list is resized to the nozzle count. With filaments decoupled the project
owns its filament list. It may hold more filaments than the printer has tools, and each plate
prints with whichever of them it uses, as long as it uses no more than the printer has tools.

## Where it applies

`decouples_filaments_from_tools(config)` is the one test. It is true when the printer option
`enable_filament_mapping` is on (the default) and the printer holds one filament per nozzle:
two or more nozzles and not `single_extruder_multi_material`. Bambu printers are excluded by
their `printer_model`, because they assign filaments to nozzles themselves through
`filament_map`. A single-nozzle or single-extruder multi-material printer is unaffected: its
filament count was never tied to a tool count.

A plate that fits the printer's tools slices to the same G-code with the option on or off. The
one exception is a `filament_map` set by hand, which only a Bambu printer's interface offers: a
decoupled printer derives the map and ignores it.

## The filament list belongs to the project

- Switching to a decoupled printer keeps the project's filaments when there are at least as
  many as the printer has nozzles, instead of replacing them with the printer's remembered list,
  and changing the printer's nozzle count does not delete them.
- Per-filament options are sized by the filament count, per-extruder options by the nozzle
  count (`Preset::normalize`).
- The filament-to-extruder map is the identity everywhere it is derived
  (`non_bbl_identity_filament_extruder_map`): filament *i* prints on extruder *i*, and a
  filament past the last extruder is placed on the master extruder until the plate is
  renumbered. `normalize_fdm_1`, `PresetBundle::full_fff_config` and
  `Print::update_filament_maps_to_config` must agree, or every `Print::apply` sees a config
  change.
- A 3mf's per-plate `filament_map` is padded and clamped against the loaded project's nozzle
  count, so a project saved for another printer still loads.

## One namespace rule

The printer's T namespace (`filament_namespace_size`) is its nozzle count.

- The plate's filaments are collected once per `Print::apply`, by the slicer's own resolution
  rules (`Print::collect_model_filaments`, shared with shrinkage compensation), and kept on the
  plate's `Print` (`FilamentCompaction::used_slots`). The numbering and the validation read
  that one set.
- A plate whose highest used filament number reaches past the namespace is renumbered densely
  before slicing (`FilamentCompaction`, applied at the top of `Print::apply`), so everything
  downstream of `apply` is dense by construction. Filaments are numbered by how sure it is that
  they print: part filaments, then per-feature filaments, the wipe tower, layer tool changes,
  and support last. One that is configured but never extrudes takes a number above every one
  that does.
- A plate is refused before slicing only when its part filaments alone outnumber the namespace,
  and after tool ordering only when the tools its G-code commands do: the ones it prints, plus,
  where export heats every configured filament (by object, or a start sequence other than
  Klipper's), the ones it heats. When they fit but one that never printed was numbered below
  one that did, the `Print` records what did not print and the caller (Plater, the CLI) applies
  and slices once more. Renumbering is a permutation, so the reslice prints the same filaments,
  numbered within the namespace.

Numbering therefore never limits a project: a four-tool printer prints any plate of up to four
filaments whatever their numbers.

## Project numbers in the interface

G-code and everything parsed from it is keyed by tool number; the project is keyed by filament
number. Under renumbering the two differ, so data read back from G-code goes through
`FilamentCompaction::project_slot_of_tool` before it indexes anything project-shaped. The
preview legend, the statistics, the per-plate filament lists and the exported slice info all
show project filament numbers.
