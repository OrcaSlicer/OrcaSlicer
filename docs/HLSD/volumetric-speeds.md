# Volumetric speeds — High Level Design

## Purpose

A linear speed (mm/s) extrudes a different flow on every line whose cross section
differs: a wide gap fill line, an Arachne wall that widens, a thicker first layer
or an adaptive layer height all push more plastic at the same feed rate. Users who
tune a profile around the hotend's melting capacity think in mm³/s instead.

Volumetric speeds let a process profile set the speed of each feature as a flow.
Each line is printed at that flow divided by its own cross section, so every line
of a feature extrudes the same flow whatever its width or height.

## Settings

`enable_volumetric_speeds` switches every feature at once. It is a global
`PrintConfig` option and is off by default, so existing profiles and projects
slice exactly as before.

Each feature whose linear speed is an absolute value has a volumetric alternative,
`<feature>_volumetric_flow`: first layer, first layer infill, outer and inner
wall, sparse infill, internal solid infill, top surface, gap infill, support,
support interface, and external and internal bridges. Each one is a nullable
per-nozzle-variant vector in the same config class as its linear counterpart, so
the wall, infill, bridge and support flows can be overridden per object like
their speeds. A value is either absolute (mm³/s) or a percentage of the
`filament_max_volumetric_speed` of the filament printing the line. The filament
limit still caps the result, whichever form is used.

The keys end in `_volumetric_flow` because `outer_wall_volumetric_speed` is
already a custom G-code placeholder that vendor change-filament G-code reads.

Some speeds have no volumetric alternative and apply over the resolved speeds:

- Small perimeter, small support perimeter, overhang and scarf joint speeds are
  modifiers. A percentage is taken of the resolved feature speed, which is the
  same as a percentage of its flow; an absolute value stays in mm/s.
- Ironing extrudes a small fraction of a full line, so a flow target would give
  meaningless feed rates.
- Travel speeds and the skirt speed override do not extrude, or override the
  feature speed outright.

## Speed resolution

`GCode::_extrude` resolves every feature speed through `GCode::feature_speed`,
which returns the linear speed, or with volumetric speeds the configured flow
divided by the path's flow. That flow is `GCode::extrusion_mm3_per_mm`: the
geometric flow times the print, filament and role flow ratios and the mixed-colour
sub-layer ratio. The E values and the filament limit use the same flow, so the
configured flow is the flow the printer extrudes. A path that extrudes nothing,
such as the move inward before an outer wall seam, keeps the linear speed.

Speeds derived from a feature speed follow it: the first-layer speeds and the
interpolation over the slow-down layers, the overhang reference speeds, small
perimeters (resolved per path in `extrude_loop`), small support perimeters
(from the first path of each support entity) and the skirt and brim, which
`_extrude` prints at the support speed by role.

Calibration prints set linear speeds, some of them per layer through the
calibration config, so `feature_speed` ignores volumetric speeds whenever
`Print::calib_mode()` is set. The flow rate calibration sets no calibration mode,
so its setup turns the option off in the print preset instead.

## Max external volumetric speed

`filament_max_external_volumetric_speed` is a per-variant filament option, 0 by
default, that caps the flow of the visible features below the filament's max
volumetric speed, with or without volumetric speeds. It applies to outer walls,
to what prints at the external bridge speed (bridge infill and overhang walls),
and to top surfaces on a layer where their region is not ironed.

`GCode::volumetric_speed_limit` returns the flow limit of a path's role, and
`_extrude` uses it everywhere the filament limit used to cap a speed, including
the resonance-avoidance re-cap and the overhang reference speed. Whether a top
surface is ironed follows `Layer::choose_ironing_extruder`, the rule the ironing
generator uses, so an ironed top surface keeps the higher limit. Calibration prints
ignore the option, as they do volumetric speeds. A volumetric speed in percent is
still a share of the filament's max volumetric speed, not of this limit.

## Outside the G-code generator

- Fill grouping and perimeter merging: `SurfaceFillParams` compares the
  volumetric flow of the infill role next to its linear speed, and
  `Layer::is_perimeter_compatible` compares the wall and gap fill flows. Both
  compare them whether or not the option is on; that can only keep apart regions
  whose volumetric values were overridden per object.
- Wipe towers borrow feature speeds: `WipeTower2` the sparse infill, inner wall and
  first layer speeds, `WipeTower` the first layer speed. With volumetric speeds,
  `set_layer` derives them from the configured flows and the tower's own line cross
  section on that layer. A percentage uses the maximum volumetric speed of the
  initial tool, as the linear values use its nozzle.
- The `outer_wall_volumetric_speed` placeholder reports the configured outer wall
  flow, capped by the filament limit and the max external volumetric speed.
- Each key invalidates the same steps as its linear counterpart, except the gap
  fill flow, which invalidates perimeters because perimeter merging compares it.

## GUI

The Speed page opens with a "Speed definition" group holding the switch, and each
volumetric row sits under its linear row (the bridge rows as a pair).
`ConfigManipulation::toggle_print_fff_options` shows exactly one row of each pair
through `toggle_line`, so the choice holds on every page, in the object settings
and in the Object Table, whose Speed category lists both keys of each pair.
