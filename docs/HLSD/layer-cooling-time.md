# Layer cooling time — High Level Design

## Purpose and scope

Two filament cooling settings depend on how long a layer takes. The slowdown for better layer cooling slows a
layer that is shorter than `slow_down_layer_time`. The part cooling fan interpolates its speed between
`fan_cooling_layer_time` and `slow_down_layer_time`. Both thresholds are in seconds, and users read the layer
times off the Preview, so `CoolingBuffer` measures a layer with the same estimate as the Preview.
`GCodeProcessor` provides it, with acceleration, cornering and the printer's motion limits.

The time at the feed rates, the length of each move over its speed, is much shorter on a printer whose moves
spend a large part of their time accelerating. On fast printers with moderate acceleration, the Preview time is
often 1.3 to 2 times the time at the feed rates. The slowdown itself still works in the time at the feed rates.
The estimate sets the targets it aims at, and the time the fan goes by.

## What is timed

A layer is timed whole, from its layer change to its end, as in the Preview. A layer whose first extrusion comes
after a filament change is timed from that extrusion instead: a long filament change before the printing would
otherwise hide a layer whose printing is quick. The time at the feed rates covers the same part of the layer.

The estimate leaves out the time the printer spends loading, unloading or changing a filament
(`machine_load_filament_time`, `machine_unload_filament_time`, `machine_tool_change_time`). The time at the feed
rates never counted it either. The moves of a filament change, such as the purge and the prime tower, count.

## The timer

The export runs its own `GCodeProcessor` on the G-code as it is written, in a later stage of the export pipeline
than the cooling. That processor has not seen a layer when the cooling decides on it, so `CoolingBuffer` owns a
second one. Before each estimate it resets that processor and applies the print config, with the filament change
times set to zero.

The layer is timed from a standstill. The G-code timed is preceded by lines that put the processor in the
machine state where the timed part starts:

- the extruder, as a `T` command;
- the extrusion mode, `M82` or `M83`;
- the last line of each kind that sets an acceleration or another motion limit, seen so far, in the order the
  lines came;
- the position, as a `G92`.

A line kind is its command and its parameter names, for example `M204 P T` or
`SET_VELOCITY_LIMIT ACCEL ACCEL_TO_DECEL`, so a later line overrides only what it sets. The commands followed are
`M201`, `M203`, `M204`, `M205`, `M221`, `M566` and `SET_VELOCITY_LIMIT`. Most of the G-code reaches the cooling
layer by layer. `GCode` passes it the G-code that users write and that goes straight to the file before the first
layer: the start G-code, the IDEX or IQEX mode G-code, and the first filament's start G-code on Bambu Lab
printers. The machine limits Orca writes at the top of the file come from the config, which the processor has
already. The first layer starts where the start G-code left the print head, which the export's processor knows by
then, as it reads everything written before the first layer as it is written. A `G4 P0` at the end makes the
planner finish the last moves.

## Slowdown

The slowdown lowers the fastest extrusions first, to a common speed cap, in the time at the feed rates. Its
targets, the minimum layer times, are multiplied by a scale, and the scale is searched for so that the estimate
of the slowed layer reaches the longest minimum layer time among its filaments:

1. A layer whose estimate already reaches the target is not slowed down.
2. The first scale is the ratio of the layer's time at the feed rates to its estimate.
3. Until a slowed layer reaches the target, the next scale comes from a model of the slowed layer. With the
   moves slowed to one speed, acceleration adds about `k / t` to a layer that takes `t` at its feed rates. Where
   that has no solution, because the moves never reach their speed, the time is taken as proportional to the
   scale.
4. Once a slowed layer reaches the target, false position between the last layer short of it and the shortest
   one past it.

A layer from 0.2% short of the target to 1% past it is accepted, as a slightly longer layer only cools more. The
search stops after six estimates. If it ends short of the target with a longer candidate found, the layer takes
that candidate. It also stops when the minimum print speed caps the slowdown, which the time at the feed rates
shows when it no longer grows.

## Fan

The fan interpolates on the estimate of the layer as it is printed, after the slowdown.

## Cost

Acceleration only makes a layer longer, so a layer that takes more than every fan and slowdown threshold at its
feed rates, by a 10% margin, is not timed: it gets the minimum fan and no slowdown either way. Those are the large
layers, which hold most of the G-code. Any other layer is timed once, and a slowed layer about three times more.
The cooling runs in its own stage of the export pipeline. On the 51 MB fan calibration cone of issue 11751, whose
65 s fan threshold has every layer timed, the slicing takes about 2% more processor time.
