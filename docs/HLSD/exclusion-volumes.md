# Bed exclusions and toolhead collision volumes

## Purpose and configuration

Bed exclusions have two distinct meanings. A material keep-out prevents model
placement without prohibiting deliberate machine travel, such as entering a
filament cutter. A toolhead collision volume represents an obstacle that travel
and extrusion must avoid. Keeping separate settings preserves existing printer
profiles, project files and command-line usage without enabling new motion
constraints implicitly.

- `bed_exclude_area` remains a points-array polygon and a shared, bed-fixed
  material/placement keep-out.
- `bed_exclude_volumes` contains collision-volume polygons, with optional
  `ZMIN..ZMAX;` prefixes and `|` separators. A polygon without a Z prefix applies
  over the printable height.
- `bed_exclude_volume_mode` selects shared volumes, toolhead-relative volumes
  derived from nozzle XY offsets, or individually configured extruder volumes.
- `extruder_bed_exclude_volumes` stores the individual definitions. These are
  selected by physical extruder, not simply by filament slot.

Material keep-outs and collision volumes are additive. Nozzle offsets apply only
to collision volumes in toolhead-relative mode. The existing bed area never moves
or disappears when collision volumes are configured.

On belt printers, the region resolver projects material keep-outs onto the
tilted bed surface before placement and tower validation. Collision volumes
remain in model coordinates; the bed-surface projection is not applied to them.

The printer settings expose the bed area as a normal editable field with Orca's
standard reset controls. Collision volumes appear directly below it in Printable
space, as a separate field. A bed-area polygon can be manually copied into this
field to define a full-height collision volume. Pasted volume syntax in the
area field requires confirmation before it is added to the volume settings;
declining or invalid input leaves the stored area intact.

## Geometry and slicing

The region resolver retains the distinction between material keep-outs and
collision volumes, and supplies nozzle-specific regions to placement, arrangement,
generated geometry and motion consumers. An object is checked conservatively
against the collision volumes for every nozzle it uses, over its complete geometry
rather than only a painted portion. Shared support geometry likewise uses a
conservative union of the relevant nozzle and instance exclusions.

Collision-volume footprints are selected by their overlap with the applicable Z
slab. Brim and supported support-generation paths subtract these footprints before
extrusion generation. The travel router attempts to avoid active collision
volumes; the final G-code checker validates emitted motion independently.

The checker cannot infer movements inside opaque firmware macros. Homing leaves
the affected coordinates unknown until explicit coordinates establish them.
Skirts and draft shields are not rebuilt around obstacles, extrusion is not
rerouted, and the circular XY sweep of spiral Z-hop is not available to the travel
router when it plans the primary travel. The final checker can warn about these
violations; that warning is not an automatic repair or a guarantee of safe output.

## Preview

Ground footprints remain visible. Nozzle-specific collision volumes have thin
nozzle-coloured borders; shared volumes and single-nozzle configurations do not
imply ownership by a particular filament. Automatic 3D previews are limited to
nearby object height to avoid obscuring the workspace.

**View → Show all collision volumes** displays each volume over its complete
resolved Z range, including on an empty bed. Its default shortcut is Ctrl+Shift+K
(Command+Shift+K on macOS), configurable through Orca's shortcut settings. The
application remembers this display preference independently of printer presets;
it changes neither collision checking nor slicing results.
