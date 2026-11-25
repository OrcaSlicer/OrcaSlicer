# Filament slots

A project filament slot represents a material selection. The number of slots is
independent of the printer's nozzle count: a printer may run fewer materials
than it has nozzles when a tool is unused, or more materials than it has nozzles
when several materials feed the same path.

The sidebar lets the user add and remove slots. An FFF project retains at least
one slot. Changing the printer's extruder count does not add or remove slots,
and saved filament selections are restored at their saved length.
Mixed-filament slots are virtual slots in the same list.

The filament list and its per-slot settings, such as colours and filament maps,
must remain aligned as slots are added or removed. Purge volumes have a
filament-by-filament matrix for each nozzle, so their layout depends on both
the slot count and the nozzle count.

## Tool addressing

On a non-Bambu printer without `single_extruder_multi_material`, a filament
slot is addressed as the physical tool of the same number:
`GCodeWriter::toolchange()` writes `T<slot>`, and `ToolOrdering` maps filament
`i` to extruder `i`. A used slot past the configured tool count can therefore
name a tool the machine does not have.

`Print::validate()` adds a non-blocking warning for that case when the printer
has more than one configured nozzle. It uses `Print::extruders()`, so the
wipe-tower filament is included, and adds plate tool changes that match
`ToolOrdering`'s mode, sequence and index conditions. A change stored above the
final printed layer is not filtered at validation time and can still warn.
Mixed slots are expanded to their physical components first, so a virtual slot
does not warn on its own. The GUI shows the warning through the existing
validation notification, and the command-line slicer logs it; both continue
slicing.

A printer reduced to one nozzle is not identified as a multi-tool printer, and
neither is a project-embedded or custom one-nozzle preset. Those cases are
treated as an ordinary one-nozzle printer and do not produce the warning.
Single-extruder multi-material (MMU) printers and Bambu Lab printers are exempt.

On a Bambu Lab printer, G-code addresses a filament and a nozzle with
`M1020 S<filament> H<nozzle>`. Filament grouping assigns filaments to available
nozzles and falls back to the master tool when no better nozzle match exists. A
single-extruder multi-material printer feeds every filament through its shared
nozzle.

Published 3MF material import has its own capacity and placeholder rules; slot
count independence does not change those rules.
