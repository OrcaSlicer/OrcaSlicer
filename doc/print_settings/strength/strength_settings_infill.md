# Infill

Infill is the internal structure of a 3D print, providing strength and support. It can be adjusted to balance material usage, print time, and part strength.

- [Sparse infill density](#sparse-infill-density)
- [Fill Multiline](#fill-multiline)
  - [Use cases](#use-cases)
- [Direction and Rotation](#direction-and-rotation)
  - [Direction](#direction)
  - [Rotation](#rotation)
- [Infill Wall Overlap](#infill-wall-overlap)
- [Apply gap fill](#apply-gap-fill)
- [Filter out tiny gaps](#filter-out-tiny-gaps)
- [Anchor](#anchor)
- [Internal Solid Infill](#internal-solid-infill)
- [Extra Solid Infill](#extra-solid-infill)
  - [Interval Pattern](#interval-pattern)
  - [Explicit Layer List](#explicit-layer-list)
- [Sparse Infill Pattern](#sparse-infill-pattern)
- [Credits](#credits)

## Sparse infill density

Infill density determines the amount of material used to fill the interior of a 3D print. It is usually expressed as a percentage, with 100% being completely solid.

- Higher density increases
  - Strength
  - Material usage
  - Print time.

> [!NOTE]
> Density usually is calculated as a % of the total infill volume, not the total print volume.  
> Nevertheless, **not all patterns interpret density the same way**, so the actual material usage may vary.  
> You can see each pattern's material usage in the [Patterns section](strength_settings_patterns).

## Fill Multiline

This setting allows you to generate your selected [infill pattern](#sparse-infill-pattern) using multiple parallel lines while preserving both the defined [infill density](#sparse-infill-density) and the overall material usage.

![infill-multiline-1-5](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-multiline-1-5.gif?raw=true)

> [!NOTE]
> Orca's approach is different from other slicers that simply multiply the number of lines and material usage, generating a denser infill than expected.
>
>| Infill   Density % | Infill Lines | Orca Density | Other Slicers Density |
>|--------------------|--------------|--------------|-----------------------|
>| 10%                | 2            | 10%          | 20%                   |
>| 25%                | 2            | 25%          | 50%                   |
>| 40%                | 2            | 40%          | 80%                   |
>| 10%                | 3            | 10%          | 30%                   |
>| 25%                | 3            | 25%          | 75%                   |
>| 40%                | 3            | 40%          | 120% *                |
>| 10%                | 5            | 10%          | 50%                   |
>| 25%                | 5            | 25%          | 125% *                |
>| 40%                | 5            | 40%          | 200% *                |
>
> *Other slicers may limit the result to 100%.

### Use cases

- Increasing the number of lines (e.g., 2 or 3) can **improve part strength** and **print speed** without increasing material usage.
- **Fire-retardant applications:** Some flame-resistant materials (like PolyMax PC-FR) require a minimum printed wall/infill thickness—often 1.5–3 mm—to comply with standards. Since infill contributes to overall part thickness, using multiple lines helps achieve the necessary thickness without switching to a large nozzle or printing with 100% infill. This is especially useful for high-temperature materials like PC, which are prone to warping when fully solid.
- Creating **aesthetic** infill patterns (like [Grid](strength_settings_patterns#grid) or [Honeycomb](strength_settings_patterns#honeycomb)) with multiple line widths—without relying on CAD modeling or being limited to a single extrusion width.

![infill-multiline-aesthetic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-multiline-aesthetic.gif?raw=true)

> [!WARNING]
> For self intersecting infills (e.g. [Cubic](strength_settings_patterns#cubic), [Grid](strength_settings_patterns#grid)) multiline count greater than 3 may cause layer shift, extruder clog or other issues due to overlapping of lines on intersection points.
>
> ![infill-multiline-overlapping](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-multiline-overlapping.gif?raw=true)

## Direction and Rotation

> [!TIP]
> You can use [Template Metalanguage for infill rotation](strength_settings_infill_rotation_template_metalanguage) to create more complex patterns.

### Direction

Controls the direction of the infill lines to optimize or strengthen the print.

![fill-direction](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/fill-direction.png?raw=true)

### Rotation

This parameter adds a rotation to the sparse infill direction for each layer according to the specified template.  
The template is a comma-separated list of angles in degrees.

For example:

```c++
0,90
```

![fill-rotation](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/fill-rotation.png?raw=true)

The first layer uses 0°, the second uses 90°, and the pattern repeats for subsequent layers.

Other examples:

```c++
0,45,90
```

```c++
0,60,120,180
```

> [!NOTE]
> If there are more layers than angles, the sequence repeats.

> [!IMPORTANT]
> Not all sparse [patterns](strength_settings_patterns) support rotation.

## Infill Wall Overlap

Infill area is enlarged slightly to overlap with wall for better bonding. The percentage value is relative to line width of sparse infill. Set this value to ~10-15% to minimize potential over extrusion and accumulation of material resulting in rough surfaces.

- **Infill Wall Overlap Off**

![InfillWallOverlapOff](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/InfillWallOverlapOff.svg?raw=true)

- **Infill Wall Overlap On**

![InfillWallOverlapOn](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/InfillWallOverlapOn.svg?raw=true)

## Apply gap fill

Enables gap fill for the selected solid surfaces.  
The minimum gap length that will be filled can be controlled from the filter out tiny gaps option.

1. **Everywhere:** Applies gap fill to top, bottom and internal solid surfaces for maximum strength.
2. **Top and Bottom surfaces:** Applies gap fill to top and bottom surfaces only, balancing print speed, reducing potential over extrusion in the solid infill and making sure the top and bottom surfaces have no pinhole gaps.
3. **Nowhere:** Disables gap fill for all solid infill areas.

Note that if using the [classic perimeter generator](quality_settings_wall_generator#classic), gap fill may also be generated between perimeters, if a full width line cannot fit between them.
That perimeter gap fill is not controlled by this setting.

If you would like all gap fill, including the classic perimeter generated one, removed, set the filter out tiny gaps value to a large number, like 999999.

However this is not advised, as gap fill between perimeters is contributing to the model's strength. For models where excessive gap fill is generated between perimeters, a better option would be to switch to the [arachne wall generator](quality_settings_wall_generator#arachne) and use this option to control whether the cosmetic top and bottom surface gap fill is generated.

## Filter out tiny gaps

Don't print gap fill with a length is smaller than the threshold specified (in mm).  
This setting applies to top, bottom and solid infill and, if using the [classic perimeter generator](quality_settings_wall_generator#classic), to wall gap fill.

## Anchor

Connect an infill line to an internal perimeter with a short segment of an additional perimeter. If expressed as percentage (example: 15%) it is calculated over infill extrusion width.
OrcaSlicer tries to connect two close infill lines to a short perimeter segment. If no such perimeter segment shorter than this parameter is found, the infill line is connected to a perimeter segment at just one side and the length of the perimeter segment taken is limited to infill_anchor, but no longer than this parameter. If set to 0, the old algorithm for infill connection will be used, it should create the same result as with 1000 & 0.

- **Anchor Off**

![InfillAnchorOff](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/InfillAnchorOff.png?raw=true)

- **Anchor On**

![InfillAnchorOn](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/InfillAnchorOn.png?raw=true)
## Internal Solid Infill

Line pattern of internal solid infill. If the [detect narrow internal solid infill](strength_settings_advanced#detect-narrow-internal-solid-infill) be enabled, the [concentric pattern](strength_settings_patterns#concentric) will be used for the small area.


## Extra Solid Infill

Insert extra solid infills at specific layers to add strength at critical points in your print. This feature allows you to strategically reinforce your part without changing the overall sparse infill density.

![extra-solid-infill](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/extra-solid-infill.gif?raw=true)

The pattern supports two formats:

### Interval Pattern
- **Simple interval**: `N` - Insert 1 solid layer every N layers, equal to `N#1`
- **Multiple layers**: `N#K` - Insert K consecutive solid layers every N layers
- **Optional K**: `N#` - Shorthand for `N#1`

Examples:
```
5 or 5#1    # Insert 1 solid layer every 5 layers
5#          # Same as 5#1
10#2        # Insert 2 consecutive solid layers every 10 layers
```

### Explicit Layer List
Specify exact layer numbers (1-based) using comma-separated values. Each entry may be a single layer `N` or a range `N#K` to insert K consecutive solid layers starting at layer N:

```
1,7,9       # Insert solid layers at layers 1, 7, and 9
5,15,25     # Insert solid layers at layers 5, 15, and 25
5,9#2,18    # Insert at 5; at 9 and 10 (because #2); and at 18
```

> [!NOTE]
> - Layer numbers are 1-based (first layer is layer 1)
> - `#K` is optional in both interval and explicit list entries (`N#` equals `N#1`)
> - Solid layers are inserted in addition to the normal sparse infill pattern

> [!TIP]
> Use this feature to:
> - Add strength at stress concentration points
> - Reinforce mounting holes or attachment points
> - Create internal structure for functional parts
> - Add periodic reinforcement for tall prints
> - Insert a single solid layer at a specific height by using an explicit list with a leading 0, which will be ignored because layer indices are 1-based. Example: `0,15` inserts a solid layer only at layer 15.

> [!WARNING]
> Layers that include solid infill can take significantly longer than surrounding layers. This time differential may lead to z-banding-like bulges. Consider adjusting cooling or speeds if you observe artifacts.

## Sparse Infill Pattern

> [!TIP]
> See [Infill Patterns Wiki List](strength_settings_patterns) with **detailed specifications**, including their strengths and weaknesses.

## Credits

- **[Fill Multiline](#fill-multiline) implementation** - [@RF47](https://github.com/RF47)
- **Wiki page:** [IanAlexis](https://github.com/IanAlexis).
