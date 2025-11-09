# Special Mode

These settings control advanced slicing and printing behaviours, such as how layers are processed, object printing order, and special effects like spiral vase mode.

- [Slicing Mode](#slicing-mode)
  - [Regular](#regular)
  - [Close Holes](#close-holes)
  - [Even Odd](#even-odd)
- [Print Sequence](#print-sequence)
  - [By Layer](#by-layer)
    - [Intra-layer order](#intra-layer-order)
  - [By Object](#by-object)
- [Spiral vase](#spiral-vase)
  - [Smooth Spiral](#smooth-spiral)
    - [Max XY Smoothing](#max-xy-smoothing)
  - [Spiral starting flow ratio](#spiral-starting-flow-ratio)
  - [Spiral finishing flow ratio](#spiral-finishing-flow-ratio)
- [Timelapse](#timelapse)

## Slicing Mode

The slicing mode determines how the model is sliced into layers and how the G-code is generated. Different modes can be used to achieve various printing effects or to optimize the print process.

### Regular

This is the default slicing mode. It slices the model layer by layer, generating G-code for each layer.  
Use this for most prints where no special modifications are needed.

### Close Holes

Use "Close holes" to automatically close all holes in the model during slicing in the XY plane.  
This can help with models that have gaps or incomplete surfaces, ensuring a more solid print.

![close-holes](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/slicing-mode/close-holes.png?raw=true)

### Even Odd

Use "Even-odd" for specific models like [3DLabPrint](https://3dlabprint.com) airplane models. This mode applies a special slicing algorithm that may be required for certain proprietary or experimental prints.

## Print Sequence

This setting controls how multiple objects are printed in a single print job.

### By Layer

This option prints all objects layer by layer, one layer at a time. This is efficient for multi-part prints as it minimises travel time between objects and can improve overall print speed.

#### Intra-layer order

Determines the print order within a single layer.

- **Default**: Prints objects based on their position on the bed and travel distance to optimise movement.
- **As object list**: Prints objects in the order they appear in the object list, which can be useful for custom sequencing or debugging.

### By Object

This option prints each object completely before moving on to the next object. This is better for prints where objects need to cool separately or when using different materials per object, but it may increase total print time due to more travel moves.

This setting requires more models separation and may not be suitable for all print scenarios.

## Spiral vase

Spiral vase mode transforms a solid model into a single-walled print with solid bottom layers, eliminating seams by continuously spiralling the outer contour.  
This creates a smooth, vase-like appearance.

### Smooth Spiral

When enabled, Smooth Spiral smooths out X and Y moves as well, resulting in no visible seams even on non-vertical walls.  
This produces the smoothest possible spiral print.

> [!NOTE]
> If you are using absolute e distances, the smoothing may not work as expected.

#### Max XY Smoothing

Maximum distance to move points in XY to achieve a smooth spiral. If expressed as a percentage, it is calculated relative to the nozzle diameter.  
Higher values allow more smoothing but may distort the model slightly.

### Spiral starting flow ratio

Sets the starting flow ratio when transitioning from the last bottom layer to the spiral.  
Normally, the flow scales from 0% to 100% during the first loop, which can sometimes cause under-extrusion at the start.  
Adjust this to fine-tune the transition and prevent issues.

### Spiral finishing flow ratio

Sets the finishing flow ratio when ending the spiral. Normally, the flow scales from 100% to 0% during the last loop, which can lead to under-extrusion at the end.  
Use this to control the ending and ensure consistent extrusion.

## Timelapse

WIP...
