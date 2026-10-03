# Volumetric speed limits — High Level Design

## Purpose and scope

A filament profile caps every extrusion by the volume of plastic it asks the
hotend to melt, in mm³/s. Feature speeds are linear, in mm/s, and belong to the
process profile. G-code emission converts the cap into a linear speed through
each path's own cross section, so one limit holds across line widths, layer
heights and flow ratios.

There are two limits. `filament_max_volumetric_speed` is the flow the hotend
can melt this material at reliably, and it bounds every extrusion.
`filament_max_outer_volumetric_speed` is optional and lower, and bounds only
the features that form a part's visible faces. Some materials change their
finish well below the flow they can melt: a glossy or silk filament turns
matte, and a surface printed at several flows shows bands where the flow
changes. With a single limit, a profile has to choose between printing
everything at the flow that looks right, printing the surface at the flow the
hotend sustains, or a compromise that is both slower and uneven. With two,
infill and inner walls run at the hotend's limit while the surface stays below
the flow at which its finish changes.

Both limits live in the filament profile because the flow at which a finish
changes is a property of the material on a given hotend, not of the process. A
process profile shared across materials keeps its linear speeds, and each
filament bounds its own surfaces.

## Settings

`filament_max_outer_volumetric_speed` defaults to 0, which disables it. A
positive value tightens the filament limit for the covered features and never
relaxes it: the effective limit is the lower of the two. It sits beside Max
volumetric speed under the filament's Volumetric speed limitation.

Like the filament limit, it holds one value per filament and per nozzle
variant (`filament_options_with_variant`), and G-code emission reads both
through the same index. A high-flow nozzle variant therefore carries its own
pair, and the two values always come from the same column.

A single value per variant is an approximation. The flow at which a finish
changes also depends on layer height and line width, so the value holds for
the setup it was measured on, as the filament limit does.

## Covered extrusions

The outer limit bounds three extrusion roles:

- Outer wall (`erExternalPerimeter`).
- Top surface (`erTopSolidInfill`).
- Bottom surface (`erBottomSurface`): bottom solid infill that is not printed
  as a bridge. That is an object's first layer, on the bed or on a raft, and a
  face resting on another region or on support with no gap.

Every other role keeps the filament limit alone. That includes inner walls,
sparse and internal solid infill, gap fill, bridges and overhang walls,
ironing, support, skirt and brim. The prime tower limits its own extrusions
by the filament limit.

## Applying the limit

`GCode::_extrude` resolves one volumetric limit per path before it settles the
path's speed. It starts from the filament limit, lowered by the fitted curve
when `filament_adaptive_volumetric_speed` is on. For a covered role with a
positive outer limit, it takes the lower of that and the outer limit. The
resolved limit then bounds the speed at every point where one is chosen:

- A feature speed of 0 means as fast as the limit allows, so a covered feature
  without a speed prints at exactly the outer limit.
- The cap is applied after the first-layer speeds and the `slow_down_layers`
  ramp. The first layer's outer walls and bottom surface are therefore bound
  by the outer limit, even though their speeds come from the first-layer
  settings.
- Resonance avoidance re-applies the cap to an outer wall before it moves the
  wall's speed out of the resonant band.
- Overhang slowdown on an outer wall scales percentage overhang speeds from
  the outer wall speed after bounding it by the lower of the two limits. No
  overhang segment prints faster than the path's capped speed.

Cooling slowdown and the other reductions applied later can only lower these
speeds further.

## Outer wall flow in custom G-code

`{outer_wall_volumetric_speed}` exposes the outer wall's flow to custom
G-code. It is the configured outer wall speed through the default outer wall
cross section, bounded by the filament limit and, when set, the outer limit.
It is set for the start G-code from the first non-support filament, and again
at every filament change for the incoming filament.

Stock start G-code paces its nozzle load and priming lines with it. The Bambu
Lab A1 and A1 mini also calibrate dynamic extrusion compensation at that flow,
at start (`M983`) and at every filament change (`M9833`). Such a calibration
is only valid at the flow the outer wall actually prints at, which is why the
outer limit bounds the placeholder as well.

## Validation

A positive outer limit above its own filament's limit is rejected by
`validate()`. Each filament is checked against its own limit, and an equal
value is accepted. The CLI refuses to slice such a configuration, and loading
a project that carries one raises a notification. The filament settings show
a warning whenever they are updated with a filament in that state, but do not
change the value.

G-code emission does not depend on validation. Because it always takes the
lower of the two limits, a configuration that skipped validation prints as if
the outer limit were off.

## Constraints

Resonance avoidance can lift an outer wall above either limit. When the capped
speed falls in the upper half of the avoidance band, the wall is raised to
`max_resonance_avoidance_speed`, and that raise is not capped again. With the
default band of 70–120 mm/s and a 0.42 × 0.2 mm outer wall, any limit between
about 7.2 and 9 mm³/s prints the wall at 120 mm/s, about 9 mm³/s. The filament
limit is exposed in the same way, but the outer limit is lower and so more
likely to land a wall in the band.

The placeholder uses the default region settings. Per-object overrides of the
outer wall speed or width, and the adaptive curve, do not change it.

## Implementation and verification

- [PrintConfig.cpp](../../src/libslic3r/PrintConfig.cpp) defines the setting,
  registers it as a per-variant filament option and validates it;
  [PrintConfig.hpp](../../src/libslic3r/PrintConfig.hpp) holds it in
  `FullPrintConfig`, and [Preset.cpp](../../src/libslic3r/Preset.cpp) lists it
  among the filament preset options.
- [GCode.cpp](../../src/libslic3r/GCode.cpp) resolves the limit per path in
  `_extrude`, bounds the overhang reference speed, and computes the placeholder
  in `get_outer_wall_volumetric_speed`.
- [Tab.cpp](../../src/slic3r/GUI/Tab.cpp) shows the setting, and
  [ConfigManipulation.cpp](../../src/slic3r/GUI/ConfigManipulation.cpp) warns
  when it exceeds the filament limit.
- [Slicing tests](../../tests/fff_print/test_volumetric_speed.cpp) ask every
  feature for more flow than either limit allows and read each role's flow back
  from the G-code. The covered roles print at the outer limit and the others at
  the filament limit. With the outer limit off or above the filament limit,
  every role prints at the filament limit, and the placeholder reports the
  limit the outer wall prints at.
- [Config tests](../../tests/libslic3r/test_config.cpp) cover the validation
  boundary and its per-filament pairing.
