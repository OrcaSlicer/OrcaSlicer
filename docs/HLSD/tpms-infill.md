# TPMS infill — High Level Design

## Purpose and scope

The sparse infill patterns Gyroid, TPMS-D (Schwarz Diamond), TPMS-FK
(Fischer-Koch S), TPMS-P (Schwarz Primitive with cells 1.5 times taller and its
level set at -0.3), and the nodal approximations of Schoen's I-WP and F-RD,
Neovius, Lidinoid and Split P print triply periodic minimal surfaces.
`tpms_scale_x`, `tpms_scale_y` and `tpms_scale_z` stretch their cells along the
axes of the pattern, and `tpms_rotation_x` then turns them around its X axis.
`is_adaptive_infill_pattern()` lists these patterns; the scale and rotation only
reach their sparse infill, so they split no other fills.

Gyroid, TPMS-D, TPMS-FK and TPMS-P have their own `Fill` classes. The surfaces
with no parametric form and cells smaller than Fischer-Koch's share
`FillTpmsTraced`, which picks the equation and period factor of its pattern from
a table, and TPMS-P extends it with its rings.

## The equation

`TpmsEquation` evaluates a TPMS equation with a period of 2 PI at a point of the
fill frame: it turns the point back around X, then divides it by the scale.
The fill frame is the object frame turned by the infill angle and the pattern's
correction angle, so the axes of the pattern are the bed axes at the default
infill direction of 45 degrees and turn with it. The point comes in already
multiplied by the frequency of the pattern's density calibration; the optimized
Gyroid raises that frequency along Z before the turn, as its buckling argument is
about the build axis.

## Generating a layer

A layer comes from one of three generators:

- **Parametric lines.** Gyroid and TPMS-D solve their equation for the waves of a
  layer, and TPMS-P for its rings: a layer of TPMS-P is `cos x + cos y = level`, a
  grid of equal rings around `(0, 0)` or `(PI, PI)`, so one ring is traced on an
  eighth of it, mirrored, and copied to every cell. They are exact to their
  tolerance, cheap, and the same in every region, but only follow layers of the
  regular cells, stretched at most along Z: `TpmsEquation::regular_layers()`.
- **Marching squares** of `TpmsField`, in `make_tpms_polylines()`, for any
  equation, scale and turn. TPMS-FK and the surfaces of `FillTpmsTraced`, which
  have no parametric form, always use it; Gyroid with the Z-buckling optimization
  or when not `regular_layers()`, TPMS-D and TPMS-P when not `regular_layers()`.
  The grid cell is 0.4 mm, and for the surfaces of `FillTpmsTraced`, whose cells
  are 2 to 3 times smaller, a 32nd of their period at most. The lines are
  simplified within an eighth of the cell, at most the sparse infill resolution.
  All but TPMS-FK sample on a grid shared by every region of the layer
  (`tpms_sampling_bbox()`), so that they connect the same way at the saddles;
  TPMS-FK samples from the corner of each region.
- **Adaptive density**, `make_adaptive_tpms()`, with the same `TpmsEquation`; see
  [adaptive-infill.md](adaptive-infill.md).

## Constraints

- With the scales at 1 and no turn, every generator does the arithmetic it did
  before these options existed, so their output does not change. The optimized
  Gyroid is the exception: it moved from a field of its own to `TpmsField` and
  differs by float rounding, far below the 0.004 mm raster.
- The period factor of each pattern is calibrated from the contour length per
  area of its level set, so the same density prints the same filament. The
  factors of the surfaces added with `FillTpmsTraced` are TPMS-FK's 4.18 times
  their length per area over TPMS-FK's, averaged over a period of layers.
  Stretching the cells along X or Y changes that length (twice along both prints
  half of it); along Z it barely does.
- The layers of TPMS-P, I-WP, Neovius, F-RD and Split P are grids of separate
  closed rings, each printed from where its generator started it, so they travel
  more than Gyroid, TPMS-D, TPMS-FK and Lidinoid, whose layers are mostly long
  lines.
