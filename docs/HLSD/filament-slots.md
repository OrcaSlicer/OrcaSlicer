# Filament slots

A project filament slot represents a material selection, not a physical
toolhead. A printer may have fewer filament slots than toolheads when some
tools are unused, or more slots than toolheads when several materials feed a
tool. The number of slots is independent of the printer's nozzle count.

The sidebar lets the user add and remove slots. An FFF project retains at least
one slot. Changing the printer's extruder count does not add or remove slots;
saved filament selections are restored without padding them to the nozzle
count. Mixed-filament slots are virtual slots in the same list.

The filament list and its per-slot settings, such as colours and filament maps,
must remain aligned as slots are added or removed. Purge volumes have a
filament-by-filament matrix for each nozzle, so their layout depends on both
the slot count and the nozzle count.

## Scope and constraints

Having a slot does not guarantee that a printer can route that material to the
desired tool. Tool assignment and device mapping are separate from slot count.
In particular, automatic mapping for non-Bambu multi-tool printers maps the
first slots to corresponding tools and defaults additional slots to the master
tool. Nozzle-dependent slicing settings do not yet consistently follow an
arbitrary filament-to-tool assignment for these printers.

Published 3MF material import has its own capacity and placeholder rules; slot
count independence does not change those rules.
