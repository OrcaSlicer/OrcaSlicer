# Resonance avoidance — High Level Design

## Purpose and scope

Many printers ring or show vertical fine artifacts (VFAs) on the surface when
the toolhead moves within a certain speed range. Resonance avoidance keeps outer
walls out of that range. Only outer walls are covered, because they form the
visible surface. Inner walls, infill and the other features keep their speeds.

The range is a property of the printer, so the settings are printer settings, in
Motion ability: `resonance_avoidance` (off by default),
`min_resonance_avoidance_speed` and `max_resonance_avoidance_speed`. The VFA
calibration measures the range. The calibrations that test speeds or motion turn
the option off, so it cannot hide what they measure: pressure advance, flow
rate, temperature, max volumetric speed, retraction, VFA, input shaping and
cornering.

An outer wall can get its speed in two places: when the G-code is generated, and
later in the layer cooling slowdown. The range is enforced in both.

## Speed of each path

`GCode::_extrude()` resolves the speed of an outer wall path first. This covers
the outer wall speed, the small perimeter speed, the scarf seam speed, the first
layer speeds and their ramp over the following layers, and the cap from the
filament's max volumetric speed. When the result falls inside the range, it
moves to the nearer half's end: a speed in the lower half goes down to the
minimum, and a speed in the upper half goes up to the maximum. Speeds above the
range, and speeds below it, are left alone.

## Layer cooling slowdown

When a layer prints faster than the filament's minimum layer time,
`CoolingBuffer` slows the layer down. It lowers the fastest moves first, so most
features end at one shared speed. Without resonance avoidance, that speed can
fall anywhere, including inside the range.

`CoolingBuffer::process_layer()` handles this in up to three passes over the
parsed layer:

1. The normal slowdown runs. If no outer wall ends inside the range, its result
   is used as is.
2. Otherwise the slowdown runs again from the unslowed layer. This time the outer
   walls whose speed was at or above the maximum are fixed at the maximum and
   are not adjustable. The other features must take the remaining time. This is
   the same mechanism that "Don't slow down outer walls" uses to exclude outer
   walls.
3. The second result is used if its layer time reaches the time of the first
   pass, within 0.1%. If not, the other features cannot slow down enough, and
   the outer walls are fixed at the minimum of the range instead.

Holding at the maximum comes first for two reasons. The path speeds of outer
walls are already outside the range, so the walls held here had a speed above
it, and the maximum is the allowed speed closest to their own. It also keeps the wall speed smooth from layer to layer: as
the layers get shorter, the slowed speed drops towards the maximum and then
stays there. The minimum layer time is a cooling requirement, so it is never
traded for the range. The minimum of the range only adds time, so the fallback
always keeps the layer as long as the first pass made it. The fallback makes a
step in wall speed, from the maximum to the minimum, at the first layer that
needs it.

Only the extruders whose outer walls landed inside the range are held. Holding
the walls of every extruder would slow filaments that the slowdown did not
touch, such as a filament with the slowdown turned off, or one whose own
minimum layer time the layer already meets.

The layer is only copied when resonance avoidance is on, and the extra passes
run only on layers where an outer wall landed inside the range. With "Don't
slow down outer walls" on, the walls are never slowed, so these passes never
run. The cooling stage needs a minimum above zero, because holding a wall at
zero speed has no meaning.

## Not covered

Overhang speeds are set for each point of a path, after the path speed above.
They can put parts of an outer wall inside the range. Those speeds are set by
the user for overhang quality, so resonance avoidance leaves them as they are.
The cooling stage does not hold them either: it only holds outer walls whose
speed was at or above the maximum.
