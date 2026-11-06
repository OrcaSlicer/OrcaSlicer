# Sparse infill scale and rotation — High Level Design

## Purpose and scope

`sparse_infill_scale_x`, `sparse_infill_scale_y` and `sparse_infill_scale_z`
stretch the cells of the sparse infill along the axes of the pattern, and
`sparse_infill_rotation_x` then turns them around its X axis. The axes of the
pattern are the bed axes at the default infill direction of 45 degrees, and turn
with the infill direction (`Fill::pattern_axes_angle()`).

`is_xy_scaled_infill_pattern()`, `is_z_scaled_infill_pattern()` and
`is_x_rotated_infill_pattern()` list the patterns that take each option. `Fill.cpp`
passes an option only to the sparse infill of those patterns, so it splits no
other fills, and the GUI hides it for the others.

## How the patterns follow them

- **Gyroid and the TPMS patterns** evaluate their equation at the point turned
  back and divided by the scale, and take every option; see
  [tpms-infill.md](tpms-infill.md).
- **Line patterns**: Grid, Triangles, Tri-hexagon, Cubic, Quarter Cubic, 2D
  Lattice and 2D Honeycomb draw families of parallel lines in
  `FillRectilinear::fill_surface_by_multilines()`. Each family is the section of
  a family of planes, so stretched and turned cells still cut a layer in
  parallel lines: the planes transform by the inverse transpose of the
  transform of the points, which gives the angle, spacing and shift of the lines
  (`transform_sweep()`). The lines are exact, and connected in the layer as they
  print. The planes of Cubic rise with the height (`SweepParams::shift_per_z`),
  so Cubic takes the Z scale and the X rotation this way too; the other line
  patterns take the X and Y scales. Planes turned all but parallel to the layer
  cross it too far apart to print and are dropped. The trapezoids of multiline
  infill only follow the regular cells; stretched or turned cells print parallel
  lines instead.
- **Other cell patterns**: Honeycomb, 3D Honeycomb, Cross Hatch and Hilbert
  Curve draw the layer stretched back in `Fill::fill_surface()`, then stretch
  their lines. `FillParams::layer_stretch` carries the stretch to
  `multiline_fill()`, which spaces the lines of the layer as they print.
- **Z scale**: Quarter Cubic, Cross Hatch and 3D Honeycomb draw a layer at its
  height over the scale.

## Constraints

- At the defaults the transforms are skipped, so every pattern draws as it did
  without the options.
- The density is that of the regular cells: stretching the cells changes the
  line length per area, and twice along X and Y prints half of it.
- Rectilinear and the zig-zag patterns, where a stretch only changes the line
  spacing, Concentric, which follows the walls, and the octree and Lightning
  patterns, fitted to the model, ignore the options.
