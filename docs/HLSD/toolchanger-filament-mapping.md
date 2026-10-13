# Toolchanger filament mapping

How OrcaSlicer works with a printer that assigns filaments to tools itself: a Snapmaker U1, a
WonderMaker ZR Ultra S, or any Klipper printer with a filament changer (AFC, Happy Hare,
openACE). The project may hold more filaments than the printer has nozzles; a plate is sliced
in logical tool space; the filament-to-tool assignment is chosen at print time and delivered
with the job; the printer's loaded filaments are read, shown and (where the firmware allows)
written back.

## What the printer reports, and where it is kept

Everything the printer can report is read at sync time and cached, never configured by hand:

| Fact | Read from | Cached in |
|---|---|---|
| its physical slots (extruders, lanes, gates), each with a name, its unit, its position in the unit, the extruder it feeds and the virtual tool it currently maps to | the vendor agent (`SnapmakerPrinterAgent`, `WonderMakerPrinterAgent`, `MoonrakerPrinterAgent`) | the filament inventory (`FilamentInventory::slots`), one record per printer preset in the app config |
| which Klipper changer it runs (`afc`, `happy_hare`, `openace`) | Moonraker's `lane_data` namespace and objects list | the printer preset, hidden option `device_changer` |
| how many `T<n>` commands it registers | `/printer/gcode/help` | the printer preset, hidden option `device_tool_count` |

The two hidden options are develop-mode entries written by `seed_printer_from_report` as a
modification of the edited preset, which the user saves or discards like any other change.
Caching them in the profile is what lets slicing run offline against the last known printer;
the send path re-reads the printer and re-validates before starting.

A Moonraker lane's topology is read in AFC's schema: `lane` (the index, which is the lane's
virtual tool), `unit`, `extruder` or `extruder_index`, and `slot` once a changer publishes it.
openACE carries all of that in `lane_data` plus a `unit_name`, read as the unit's label only;
AFC's `lane_data` carries just the extruder index, so its `AFC_stepper <lane>` status objects
are queried once and joined by lane name. Units are grouped by id and titled by name, so
renaming a unit neither splits nor merges its lanes. Happy Hare's
gates, the U1's extruders and the stock ZR's boxes are the identity. The topology rides the
same hops as the filament data (agent tray → device tray → inventory slot) and is published for
empty slots too, since an empty slot still has a place.

## The protocol is the delivery format

`filament_mapping_protocol` is the vendor's declaration of how the map reaches the printer:
`none`, `snapmaker` (`extruder_map_table` in the start script) or `wondermaker` (`box_modify`
variables). A reported changer overrides it for delivery (`effective_map_delivery`): AFC takes
`RESET_AFC_MAPPING` and `SET_MAP LANE=… MAP=T<n>`, Happy Hare `MMU_TTG_MAP`, openACE one
`OPENACE_MAP="[[sliced,virtual],…]"` parameter on the SD start. The vendor's value is never
rewritten, so a ZR Ultra S running openACE keeps its own prelude (the timelapse and leveling
lines its screen offers) and hands only the map to openACE. Print-start options belong to the
vendor adapter (`device_print_spec`), not to the protocol.

## One namespace rule

The printer's T namespace (`filament_namespace_size`) is the probed count when a sync measured
it, else the vendor's constant (32 for the U1's table), else the nozzle count. Two consequences,
the same on every device-resolved printer:

- a plate whose highest used filament number reaches past the namespace is renumbered densely
  before slicing (`FilamentCompaction`, applied at the top of `Print::apply`), and the delivered
  map is expressed in those numbers;
- a plate is rejected by `Print::validate` only when it uses more distinct filaments than the
  namespace holds.

Numbering therefore never limits a project: a stock ZR (namespace 4) prints any plate of up to
four filaments whatever their numbers, and a 35-filament project prints on a U1 plate by plate.

## The dialogs

One panel, `SlotGridPanel`, draws the printer's slots where the printer has them: a titled box
per changer unit with its slots in slot order, a "Direct" row for slots without a unit, and on
a multi-extruder printer with a changer (MEMM) a row of extruder tiles that fades the slots on
other extruders. A tile shows the slot's current virtual tool, its material and, on MEMM, its
extruder; an empty slot is grey with a diagonal line. Everything shown to the user is
1-indexed.

- **Printer Material Settings** is the grid with a pencil per editable slot. The edit dialog
  writes back through the agent where the firmware takes writes (AFC's and openACE's
  `SET_COLOR`/`SET_MATERIAL`, Happy Hare's `MMU_GATE_MAP`, the U1's own API).
- **Synchronize Filament Information** picks project filaments from the grid.
- **Print mapping** picks each plate filament's slot from the grid: a popup on a
  single-extruder changer or the U1, a modal with the extruder row on MEMM, where two filaments
  landing on the same extruder are marked because the printer will swap and purge between them.

The three modes are derived, not configured: SEMM is one nozzle with a changer, ME several
nozzles without one, MEMM several nozzles with one.

## Flushing volumes

`flush_volumes_matrix` keeps upstream's one-block-per-extruder layout. The dialog lists a
toolchanger's physical extruders as `E<n>` (Bambu's dual head keeps Left/Right) and offers
"same flushing volumes for all extruders" (`flush_volumes_synced`, default on), which writes
identical blocks. The sidebar shows the button only where filament is swapped through a nozzle:
single-extruder multi-material, a Bambu head, or MEMM.

## Send-time checks

The device dialog re-reads the printer when it opens. Before a print starts, the plate is
re-validated against the fresh namespace, and the send is refused when the changer the printer
reports now differs from the one the plate was sliced with, or when the start script could not
be rendered (an AFC lane without a name). A device-resolved printer never auto-starts a print
whose map did not reach it.
