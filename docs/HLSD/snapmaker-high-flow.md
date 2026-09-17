# Snapmaker U1 high-flow profiles

The U1 provides separate High Flow printer presets for 0.2, 0.4, 0.6 and
0.8 mm nozzles. Each describes four tools fitted with the same diameter and
high-flow nozzle. They inherit the corresponding standard printer, retaining
its model identity, geometry, motion limits and machine G-code. Existing
standard and mixed-diameter presets retain their settings and names.

Enable the High Flow variants under Snapmaker U1 in the printer setup, select
the matching printer, and select a filament whose name includes that diameter
and `High Flow nozzle`. PLA SnapSpeed is the default material for these
printers. Dedicated HF process presets preserve diameter-specific extrusion
widths and layer heights while overriding the four print speeds below.
Separate HF filament presets keep higher ceilings out of standard-nozzle
material settings.

## Process speeds

All 36 HF process presets, across 0.2, 0.4, 0.6 and 0.8 mm nozzles,
use the same requested speeds from Snorca's 0.4 mm HF Standard process:

| Setting | Speed (mm/s) |
| --- | ---: |
| Inner walls | 600 |
| Outer walls | 500 |
| Internal solid infill | 600 |
| Sparse infill | 600 |

Each HF process inherits its standard diameter/layer-height counterpart,
so geometry, accelerations, first-layer speeds, bridges, overhangs and other
settings remain diameter-specific. Standard printers retain their original
process speeds. HF printers default to the corresponding HF process.
Volumetric ceilings and machine motion limits still cap actual speeds.

## Calibration sources

Except for the volumetric ceiling, the original 0.4 mm HF material overrides come from Snapmaker/OrcaSlicer `snorca/main`
commit `b1831e5dcb`, under `resources/profiles/Snapmaker/filament/`. That fork
uses `filament_flow_support: ["standard", "high_flow"]` to interpret paired
values. Here the high-flow column is translated into ordinary single-valued
filament overrides; the fork-specific keys and its profile restructuring are
not imported. Companion temperature and pressure-advance enable settings are
included. Machine and filament G-code remain inherited from this branch.

The 14 materials with upstream HF overrides are ABS, ASA, PETG HF, PETG Translucent, PETG-CF,
PLA Full Spectrum, PLA Glow, PLA Matte, PLA Silk, PLA SnapSpeed, PLA
Translucent, PLA Wood, PLA-CF and PVA. Upstream's renamed Support For PLA is
not assumed equivalent to the local Breakaway formulation.

## Volumetric ceiling

All 141 HF filament presets explicitly set maximum volumetric speed to three
times the fully inherited non-HF material value. Diameter-specific parents
are used where available; Generic HF presets inherit the active Orca filament
library's `@System` definitions, which are shared across diameters. The parent
is recorded in `inherits`. This replaces both the upstream 0.4 mm HF ceiling
and the earlier capped estimates.

The ceiling is a user-selected limit, not a measured hardware capability.
Other process and machine limits still apply. Future changes to a parent
preset's volumetric speed require updating its HF child; the multiplier is
materialized in JSON, not evaluated by the slicer.

## Other material settings

The 24 additional HF material presets are **estimated starting points**, not
manufacturer calibrations. They exist only where this branch already has a
matching material and diameter. In particular, this does not make filled
materials available to a 0.2 mm nozzle when no such standard profile exists.

For each material with a paired 0.4 mm standard/HF value:

- Flow ratio and pressure advance use the 0.4 mm HF/standard ratio,
  rounded to four decimal places. A zero standard value is not divided by.
- Nozzle and initial-layer temperatures use the 0.4 mm HF-minus-standard
  difference, added to the diameter's original temperature.
- Pressure advance follows the source's enable setting when its value is
  adjusted. Diameter-specific retraction and cooling settings are retained.
- Missing, single-valued or `nil` comparisons leave the diameter's original
  setting intact.

For example, PLA SnapSpeed has standard volumetric limits of 2 mm^3/s at
0.2 mm and 20 mm^3/s at 0.4, 0.6 and 0.8 mm. Its HF ceilings are therefore
6 and 60 mm^3/s respectively. The other 103 HF material presets retain their
standard parent's temperature, retraction, cooling and pressure-advance
settings, changing only the volumetric ceiling and profile metadata.
Each HF preset includes its baseline and ceiling in its filament notes.
Tune the material settings for the actual nozzle, filament and temperature.

The separate preset design requires no C++ changes or new profile keys. It
does not provide independent standard/HF selection per tool within one printer
preset; a mixed hardware setup needs its own calibrated user preset.
