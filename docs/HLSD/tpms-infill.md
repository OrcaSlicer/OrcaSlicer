# TPMS infill — High Level Design

## Purpose and scope

The sparse infill patterns Gyroid, TPMS-D (Schwarz Diamond), TPMS-FK
(Fischer-Koch S), TPMS-P (Schwarz Primitive with cells 1.5 times taller and its
level set at -0.3), and the nodal approximations of Schoen's I-WP and F-RD,
Neovius, Lidinoid and Split P print triply periodic minimal surfaces.
`sparse_infill_scale_x`, `sparse_infill_scale_y` and `sparse_infill_scale_z`
stretch their cells along the axes of the pattern, and `sparse_infill_rotation_x`
then turns them around its X axis, as for the other patterns that take them; see
[sparse-infill-scale.md](sparse-infill-scale.md). `is_tpms_infill_pattern()`
lists these patterns.

`FillTpms` fills them all. A table in `FillTpms.cpp` holds what sets each
pattern apart: its equation, its period factor and density cap, whether its cells
are small enough to need a finer grid, whether it chains its lines when the
anchors are off, and the parametric lines of its regular cells for Gyroid,
TPMS-D and TPMS-P. The rest, from the stepped shells of adaptive density to
connecting the lines, is the same for every pattern.

## The equation

`TpmsEquation` evaluates a TPMS equation with a period of 2 PI at a point of the
fill frame: it turns the point back around X, then divides it by the scale.
The fill frame is the object frame turned by the infill angle and the pattern's
correction angle, so the axes of the pattern are the bed axes at the default
infill direction of 45 degrees and turn with it. The point comes in already
multiplied by the frequency of the pattern, 2 PI over its period; the optimized
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
  equation, scale and turn. The patterns with no parametric form always use it;
  Gyroid with the Z-buckling optimization or when not `regular_layers()`, TPMS-D
  and TPMS-P when not `regular_layers()`. The grid cell is 0.4 mm, and for the
  patterns with no parametric form and TPMS-P a 32nd of their period at most, so
  their dense layers keep their length: the 0.4 mm grid draws the 2 mm cells of
  TPMS-FK at 90% in zigzags 6% too long. The lines are
  simplified within an eighth of the cell, at most the sparse infill resolution.
  Every pattern samples on a grid shared by every region of the layer
  (`tpms_sampling_bbox()`), so that the regions connect the same way at the
  saddles.
- **Adaptive density**, `make_adaptive_tpms()`, with the same `TpmsEquation`; see
  [adaptive-infill.md](adaptive-infill.md).

## Constraints

- With the scales at 1 and no turn, the waves of Gyroid and TPMS-D are the ones
  solved from their equations; the scale and the turn only send a layer to
  marching squares.
- Each pattern is calibrated by its own period factor: its period in line
  spacings at full density, which grows with the multiline count over the
  density. The factor of each pattern but Gyroid makes its sparse infill print
  the weight of its density: a 100 mm cube of infill alone, with no walls,
  shells, anchors or infill/wall overlap, weighs its density times the weight of
  the solid cube, within 0.4% on average and 1.5% at any density from 15% to 90%.
  Gyroid's factor is older and prints 5 to 8% less. Stretching the cells along X
  or Y changes the line length per area (twice along both prints half of it);
  along Z it barely does.
- With the anchors off, Gyroid and TPMS-D chain their lines; the other patterns
  connect them along the boundary even then, as chaining their many islands is
  slow.
- The layers of TPMS-P, I-WP, Neovius, F-RD and Split P are grids of separate
  closed rings, each printed from where its generator started it, so they travel
  more than Gyroid, TPMS-D, TPMS-FK and Lidinoid, whose layers are mostly long
  lines.
