# Wall and surfaces

- [Walls printing order](#walls-printing-order)
  - [Inner/Outer](#innerouter)
  - [Inner/Outer/Inner](#innerouterinner)
  - [Outer/Inner](#outerinner)
  - [Print infill first](#print-infill-first)
- [Wall loop direction](#wall-loop-direction)
- [Surface flow ratio](#surface-flow-ratio)
- [Only one wall](#only-one-wall)
  - [Threshold](#threshold)
- [Avoid crossing walls](#avoid-crossing-walls)
  - [Max detour length](#max-detour-length)
- [Small area flow compensation](#small-area-flow-compensation)
  - [Flow Compensation Model](#flow-compensation-model)

## Walls printing order

Print sequence of the internal (inner) and external (outer) walls.  

### Inner/Outer

Use Inner/Outer for best overhangs. This is because the overhanging walls can adhere to a neighboring perimeter while printing. However, this option results in slightly reduced surface quality as the external perimeter is deformed by being squashed to the internal perimeter.

![inner-outer](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/inner-outer.gif?raw=true)

### Inner/Outer/Inner

Use Inner/Outer/Inner for the best external surface finish and dimensional accuracy as the external wall is printed undisturbed from an internal perimeter. However, overhang performance will reduce as there is no internal perimeter to print the external wall against. This option requires a minimum of 3 walls to be effective as it prints the internal walls from the 3rd perimeter onwards first, then the external perimeter and, finally, the first internal perimeter. This option is recommended against the Outer/Inner option in most cases.

![inner-outer-inner](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/inner-outer-inner.gif?raw=true)

### Outer/Inner

Use Outer/Inner for the same external wall quality and dimensional accuracy benefits of [Inner/Outer/Inner](#innerouterinner) option. However, the z seams will appear less consistent as the first extrusion of a new layer starts on a visible surface.

![outer-inner](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/outer-inner.gif?raw=true)

### Print infill first

When this option is enabled, the [infill](strength_settings_infill) and [top/bottom shells](strength_settings_top_bottom_shells) are printed first, followed by the walls. This can be useful for some overhangs where the infill can support the walls.

![infill-first](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/infill-first.gif?raw=true)

**However**, the infill will slightly push out the printed walls where it is attached to them, resulting in a worse external surface finish. It can also cause the infill to shine through the external surfaces of the part.

![infill-ghosting](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/infill-ghosting.png?raw=true)

When using this option is recommended to use the [Precise Wall](quality_settings_precision#precise-wall), [Inner/Outer/Inner](#innerouterinner) wall printing order or reduce [Infill/Wall Overlap](strength_settings_infill#infill-wall-overlap) to avoid the infill pushing out the external wall.

## Wall loop direction

The direction which the wall loops are extruded when looking down from the top.  
By default all walls are extruded in counter-clockwise, unless [Reverse on even](quality_settings_overhangs#reverse-on-even) is enabled.  
Set this to any option other than Auto will force the wall direction regardless of the [Reverse on even](quality_settings_overhangs#reverse-on-even).

> [!NOTE]
> This option will be disabled if spiral vase mode is enabled.

## Surface flow ratio

This factor affects the amount of material for [top or bottom solid infill](strength_settings_top_bottom_shells). You can decrease it slightly to have smooth surface finish.  
The actual top surface flow used is calculated by multiplying this value with the filament flow ratio, and if set, the object's flow ratio.

> [!TIP]
> Before using a value other than 1, it is recommended to [calibrate the flow ratio](flow-rate-calib) to ensure that the flow ratio is set correctly for your printer and filament.

## Only one wall

Use only one wall on flat surfaces, to give more space to the [top infill pattern](strength_settings_top_bottom_shells#surface-pattern).
Specially useful in small features, like letters, where the top surface is very small and [concentric pattern](strength_settings_patterns#concentric) from walls would not cover it properly.

![only-one-wall](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/only-one-wall.gif?raw=true)

### Threshold

If a top surface has to be printed and it's partially covered by another layer, it won't be considered at a top layer where its width is below this value. This can be useful to not let the 'one perimeter on top' trigger on surface that should be covered only by perimeters.  
This value can be a mm or a % of the perimeter extrusion width.

![only-one-wall-threshold](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/only-one-wall-threshold.png?raw=true)

> [!WARNING]
> If enabled, artifacts can be created if you have some thin features on the next layer, like letters. Set this setting to 0 to remove these artifacts.

## Avoid crossing walls

This option instructs the slicer to avoid crossing perimeters (walls) during travel moves.  
Instead of traveling directly through a wall, the print head will detour around it, which can significantly reduce surface defects and stringing.

While this increases print time slightly, the improvement in print quality—especially with materials prone to stringing like **PETG** or **TPU**, often justifies the tradeoff.  
Highly recommended for detailed or aesthetic prints.

![avoid-crossing-walls](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/avoid-crossing-walls.png?raw=true)

> [!NOTE]
> This feature is not compatible with Timelapse mode, as it can cause unexpected travel moves.

### Max detour length

Defines the maximum distance the printer is allowed to detour to avoid crossing a wall.
Can be set as:

- **Absolute value in millimeters:** exactly how far the detour can extend (e.g., `5mm`).
- **Percentage** of the direct travel path (e.g., `50%`).
- **0** disables the **limit** and allows detours of **any length**.

Use this setting to balance between print time and wall quality—longer detours mean fewer wall crossings but slower prints.

## Small area flow compensation

Enables adaptive flow control for small infill areas.
This feature helps address extrusion problems that often occur in small regions of solid infill, such as the tops of narrow letters or fine features.  
In these cases, standard extrusion flow may be too much for the available space, leading to over-extrusion or poor surface quality.

![flow-compensation-model](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/flow-compensation-model.png?raw=true)

It works by dynamically adjusting the extrusion flow based on the length of the extrusion path, ensuring more precise material deposition in small spaces.

This is a native implementation of @Alexander-T-Moss [Small Area Flow Compensation](https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp).

### Flow Compensation Model

The model uses a list of Extrusion Length and Flow Correction Factor value pairs. Each pair defines how much flow should be used for a specific Extrusion Length.  
For values between the listed points, the flow is calculated using linear interpolation.

![flow-compensation-model-graph](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/Wall-Order/flow-compensation-model-graph.png?raw=true)

For example for the following model:

| Extrusion Length | Flow Correction Factor |
|------------------|------------------------|
| 0                | 0                      |
| 0.2              | 0.4444                 |
| 0.4              | 0.6145                 |
| 0.6              | 0.7059                 |
| 0.8              | 0.7619                 |
| 1.5              | 0.8571                 |
| 2                | 0.8889                 |
| 3                | 0.9231                 |
| 5                | 0.952                  |
| 10               | 1                      |

You should write it as:

```c++
0,0;
0.2,0.4444;
0.4,0.6145;
0.6,0.7059;
0.8,0.7619;
1.5,0.8571;
2,0.8889;
3,0.9231;
5,0.9520;
10,1;
```
