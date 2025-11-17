# Wall Generator

The Wall Generator defines how the outer and inner walls (perimeters) of the model are printed.

- [Classic](#classic)
- [Arachne](#arachne)
  - [Wall transitioning threshhold angle](#wall-transitioning-threshhold-angle)
  - [Wall transitioning filter margin](#wall-transitioning-filter-margin)
  - [Wall transitioning length](#wall-transitioning-length)
  - [Wall distribution count](#wall-distribution-count)
  - [Minimum wall width](#minimum-wall-width)
    - [First layer minimum wall width](#first-layer-minimum-wall-width)
  - [Minimum feature size](#minimum-feature-size)
  - [Minimum wall length](#minimum-wall-length)

## Classic

The Classic wall generator is a simple and reliable method used in many slicers. It creates as many walls as possible (limited by [Wall Loops](strength_settings_walls#wall-loops)) by extruding along the model’s perimeter using the defined [Line Width](quality_settings_line_width).
This method does not vary extrusion width and is ideal for fast, predictable slicing.

![wallgenerator-classic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/WallGenerator/wallgenerator-classic.png?raw=true)

## Arachne

The Arachne wall generator dynamically adjusts extrusion width to follow the shape of the model more closely. This allows better handling of thin features and smooth transitions between wall counts.

![wallgenerator-arachne](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/WallGenerator/wallgenerator-arachne.png?raw=true)

> [!NOTE]
> [A Framework for Adaptive Width Control of Dense Contour-Parallel Toolpaths in Fused Deposition Modeling](https://www.sciencedirect.com/science/article/pii/S0010448520301007?via%3Dihub)

### Wall transitioning threshhold angle

Defines the minimum angle (in degrees) required for the algorithm to create a transition between an even and odd number of walls. If a wedge shape exceeds this angle, no extra center wall will be added. Lowering this value reduces center walls but may cause under- or over-extrusion in sharp corners.

### Wall transitioning filter margin

Prevents rapid switching between more or fewer walls by defining a tolerance range around the minimum wall width. The extrusion width will stay within the range:

```math
\left[ \text{Minimum Wall Width} - \text{Margin},\ 2 \times \text{Minimum Wall Width} + \text{Margin} \right]
```

Higher values reduce transitions, travel moves, and extrusion starts/stops, but may increase extrusion variability and introduce print quality issues. Expressed as a percentage of nozzle diameter.

### Wall transitioning length

Controls how far into the model the transition between wall counts extends. A lower value shortens or removes center walls, improving print time but potentially reducing coverage in tight areas.

### Wall distribution count

Sets how many walls (counted inward from the outer wall) are allowed to vary in width. Lower values constrain variation to inner walls, keeping outer walls consistent for best surface quality.

### Minimum wall width

Defines the narrowest wall that can be printed to represent thin features. If the feature is thinner than this value, the wall will match its width. Expressed as a percentage of nozzle diameter.

#### First layer minimum wall width

Specifies the minimum wall width for the first layer. It is recommended to match the nozzle diameter to improve adhesion and ensure stable base walls.

### Minimum feature size

Minimum width required for a model feature to be printed. Features below this value are skipped; features above it are widened to match the [Minimum Wall Width](#minimum-wall-width). Expressed as a percentage of nozzle diameter.

### Minimum wall length

Avoids very short or isolated wall segments that add unnecessary time.  
Increasing this value removes short unconnected walls, **improving efficiency**.

> [!NOTE]
> Top and bottom surfaces are not affected by this setting to avoid visual artifacts.
> Use the One Wall Threshold (in Advanced settings) to adjust how aggressively OrcaSlicer considers a region a top surface. This option only appears when this setting exceeds 0.5, or if single-wall top surfaces are enabled.
