# Bone infill

The **Bone** sparse infill pattern (`sparse_infill_pattern = bone`) fills a part with the walls of a 3D foam graded like cancellous bone. Cells are small just inside the walls and grow towards the core, which can be left hollow like the cavity of a long bone. The code is in `src/libslic3r/Fill/FillBone.{hpp,cpp}`.

## What it guarantees

- **Graded by 3D depth.** The target density depends on the distance to the surface of the object in 3D. The band under a top surface, over a floor, or beside a horizontal hole is as dense as the band beside a vertical wall.
- **Braced in all directions.** Every layer is a cross-section of one 3D foam. Its walls tilt in every direction, and every cell has a top and a bottom, so the infill resists lateral load and torsion as well as vertical compression. There are no columns running straight up.
- **Printable.** Every wall rests on wall of the layer below, within the overhang allowance, or on the solid infill, walls or skins under it. The one exception is a short bridge between two supported points.
- **Reproducible.** The same object and settings give the same walls on every platform: the random numbers come straight from `std::mt19937_64`, whose sequence the standard fixes, and not from the standard distributions or `std::shuffle`, whose algorithms differ between libraries.

## Settings

| Key | Meaning |
|---|---|
| `sparse_infill_density` | Density just inside the walls. |
| `adaptive_infill_interior_density` | Density in the core. Shared with the adaptive infill of the Gyroid and TPMS patterns, where it is the density at the center: both grade from the sparse infill density at the surface to it. |
| `bone_falloff` | Depth below the walls over which the density goes from the one to the other. |
| `bone_falloff_curve` | Shape of that change: linear, S-curve (smoothstep), exponential (drops early), logarithmic (drops late) or custom. |
| `bone_falloff_custom` | The custom curve: how far the density has gone towards the interior density at 20%, 40%, 60% and 80% of the falloff, in percent, with a decimal point whatever the locale. A monotone cubic runs through these points and the two ends, so it doesn't overshoot. |
| `bone_hollow_depth` | Deeper than this below the walls the infill is left out. 0 fills the whole object, unless the interior density is below 3% (see step 5). |
| `bone_irregularity` | Variation of cell size and spacing. |
| `bone_isotropy` | 100% gives round cells; lower values give taller cells (stiffer in Z, fewer bridges). |
| `bone_max_bridge` | Longest span printed over nothing between two supported points. |

The Bone pattern grades in absolute depth, over `bone_falloff` millimetres from the walls, where the adaptive infill grades relative to the depth of the center of each part; so the two share the densities but not the gradient.

The walls of an object are generated once, with the settings of the first region using the pattern (`uses_bone_infill()`: Bone at a density above 0 and below 100%). The `bone_*` settings can therefore be overridden per object but not per part or modifier.

`BoneGradient` holds the density profile, from the four density settings and the curve, including the hollow core of step 5. All its depths are measured from the inner edge of the walls.

## Design

The walls of every layer are generated once per object, in `PrintObject::prepare_bone_infill()`. This runs in `bridge_over_infill()` next to the adaptive infill fields, so the anchors of internal bridges come from the same walls. It runs before the parallel infill step because pruning goes up through the layers. `FillBone` then clips the walls of its layer to each surface and chains them with `chain_or_connect_infill()`, like Lightning.

`generate_bone_walls()` builds the walls of any stack of layers in five steps; `BoneInfill` gives it the layers of the object.

1. **Distance field.** The outlines of the layers (`lslices`) are rasterized on a voxel grid of 0.4 mm or more, capped at about 12 M voxels. Two exact Euclidean distance transforms, one to the outside and one to the inside, give the signed distance to the surface. The rasterization and the transform are those of `src/libslic3r/DistanceTransform.hpp`, which the adaptive infill field uses too.
2. **Cell centres.** Centres are placed by dart throwing until saturated. Each candidate needs its own local spacing clear of the earlier centres, in a metric stretched along Z by the isotropy, so the spacing follows the target density. With line width `w` and density `rho`, the spacing is `2 w / rho`. The hollow core is tiled with small cells, whose walls are cut away later, so that its cells don't reach into the dense band around it.
3. **Cross-sections.** The cross-section of a 3D power diagram at height `z` is the 2D power diagram of the centres with weights `0.25 spacing² - (z - z_i)²`. Each cell is a polygon clipped by half-planes; the seeds near it are found by a ring search over a hash grid, bounded by the power distance. The size weights put the face between a big and a small cell in proportion to their sizes, which keeps the gradient sharp. The walls are clipped to the sparse infill surfaces of the layer (`stInternal`), and cut away in the hollow core. The layers are independent and are cut in parallel.
4. **Pruning.** Bottom-up, a wall is kept where it lies within `min(w / 2, layer height · tan 45°)` of a kept wall of the layer below, or over anything but the sparse infill of the layer below. Both are tested on a raster of the layer below. Unsupported pieces form a graph through the wall junctions. A patch of new wall is kept as a bridge when, after its dead-end branches are stripped, every point is within `bone_max_bridge / 2` along the walls of a supported anchor. This is what fills the band under a top surface: there the cells get smaller going up, and their new walls must span the larger cells below.
5. **Hollow core.** A point is hollow when it is deeper than `bone_hollow_depth` below the walls and the cavity can still retreat from it at 0.4 mm per mm of height before any point above must be dense. The cavity therefore closes with a pointed roof, and the cut ends of the walls around it grow back inwards layer by layer instead of starting in mid-air. Where the graded density (before the irregularity) falls below 3%, the core is hollow even when `bone_hollow_depth` is 0, and the shallower of the two depths applies. Cells that sparse are 30 mm or more across, wider than most cores. No seed fits there, so the cells of the band would reach across the core and meet in the middle as long spokes.

## Constraints

- The cost grows with the number of cells, and the cell spacing falls with the density just inside the walls, so a high `sparse_infill_density` on a large part makes many small cells, a long slice and a large G-code file. The pruning pass is the only sequential one; it costs one raster lookup per 0.1 mm of wall, and its dead-end stripping is linear in the walls of a layer.
- The voxel grid caps memory for large parts, at the cost of a coarser distance field: about half a voxel of error near the surface.
- The pattern doesn't support multiline infill. It has no consistent pattern between layers, since every layer is a different cut through the foam.
