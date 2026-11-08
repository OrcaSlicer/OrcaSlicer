# Decoupling filaments from tools

A multi-tool printer normally limits a project to one filament per tool: filament *n* is tool
*n*, and the filament list is resized to the nozzle count. With filaments decoupled the project
owns its filament list. It may hold more filaments than the printer has tools, and each plate
prints with whichever of them it uses, as long as it uses no more than the printer has tools.
Each plate also decides which tool, and so which nozzle, every one of its filaments prints from.

## Where it applies

`decouples_filaments_from_tools(config)` is the one test. It is true when the printer option
`enable_filament_mapping` is on (the default) and the printer holds one filament per nozzle:
two or more nozzles and not `single_extruder_multi_material`. Bambu printers are excluded by
their `printer_model`, because they assign filaments to nozzles themselves through
`filament_map`. A single-nozzle or single-extruder multi-material printer is unaffected: its
filament count was never tied to a tool count.

A plate whose filaments all sit on their own tools, with no assignment set by hand, slices to the
same G-code with the option on or off.

## The filament list belongs to the project

- Switching to a decoupled printer keeps the project's filaments when there are at least as
  many as the printer has nozzles, instead of replacing them with the printer's remembered list,
  and changing the printer's nozzle count does not delete them.
- Per-filament options are sized by the filament count, per-extruder options by the nozzle
  count (`Preset::normalize`).
- Once a plate's filaments are numbered to tools (below), the filament-to-extruder map the
  slicer works with is the identity: tool *i* is extruder *i* is nozzle *i*
  (`non_bbl_identity_filament_extruder_map`). `normalize_fdm_1`, `PresetBundle::full_fff_config`
  and `Print::update_filament_maps_to_config` must agree, or every `Print::apply` sees a config
  change.
- A 3mf's per-plate `filament_map` is padded and clamped against the loaded project's nozzle
  count, so a project saved for another printer still loads.

## Each plate assigns its filaments to tools

The printer's T namespace (`filament_namespace_size`) is its nozzle count. A plate's filaments are
numbered to tools by `FilamentCompaction`, built at the top of `Print::apply` from the plate's
filament set and its assignment, so everything downstream of `apply` sees tool numbers.

- The plate's filaments are collected once per `Print::apply`, by the slicer's own resolution
  rules (`Print::collect_model_filaments`, shared with shrinkage compensation), and kept on the
  plate's `Print` (`FilamentCompaction::used_slots`). The numbering and the validation read
  that one set.
- The assignment is the plate's `filament_map` (1-based extruder per project filament) when the
  plate's `filament_map_mode` is manual, and derived otherwise. Both are plate config, saved per
  plate in the 3mf, the same keys a Bambu plate uses. `BackgroundSlicingProcess::apply` lays the
  plate config over the full config, so `build_filament_compaction` reads them before
  normalization replaces them with the identity.
- `resolve_filament_tools` numbers the plate's filaments, surest to print first (part filaments,
  then per-feature filaments, the wipe tower, layer tool changes, support; filaments the last
  slice showed never print come last): a filament pinned by the manual map takes its tool; a
  filament whose own number is within the tools keeps it while it is free; the rest take the
  lowest free tool in order; those that find none are numbered past the last tool. A tool below
  the highest taken one that no filament takes holds a filament the plate does not use (its own
  number when possible), or a filler column copied from the plate's first filament; the printer
  is never told about such a tool. Used mixed slots are numbered after the physical tools.
- Two used filaments pinned to one tool is an error (`DuplicateToolAssignment`, reported by
  `Print::validate`). A filament whose `required_nozzle_HRC` exceeds the hardness of its tool's
  `nozzle_type` is a warning; whether the installed nozzle is hard enough is decided at print
  time, where the printer is known.
- A plate is refused before slicing only when its part filaments alone outnumber the namespace,
  and after tool ordering only when the tools its G-code commands do: the ones it prints, plus,
  where export heats every configured filament (by object, or a start sequence other than
  Klipper's), the ones it heats. When they fit but one that never printed was numbered below
  one that did, the `Print` records what did not print and the caller (Plater, the CLI) applies
  and slices once more. Renumbering is a permutation, so the reslice prints the same filaments,
  numbered within the namespace.

Numbering therefore never limits a project: a four-tool printer prints any plate of up to four
filaments whatever their numbers, and a project filament prints from whichever nozzle the plate
assigns it.

## Project numbers in the interface

G-code and everything parsed from it is keyed by tool number; the project is keyed by filament
number. Under renumbering the two differ, so data read back from G-code goes through
`FilamentCompaction::project_slot_of_tool` before it indexes anything project-shaped. The
preview legend, the statistics, the per-plate filament lists and the exported slice info all
show project filament numbers. The slice info's per-plate `filament_maps`, and each filament's
nozzle diameter derived from it, are the plate's resolved assignment
(`FilamentCompaction::project_filament_map`), in both modes.

Plate Settings shows a **Filament nozzles** group on decoupled printers: one row per filament the
plate uses, each with the tool, and so the nozzle, it prints from. The rows show the resolved
assignment; choosing a tool switches the plate to a manual map holding that assignment with the
change applied (two filaments never share a tool: choosing one another filament holds swaps them),
and "Reset to automatic" clears the map. A per-row automatic state is not stored: a 3mf map entry
below 1 is read as tool 1.
