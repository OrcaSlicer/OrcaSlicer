# Strength Advanced

- [Align infill direction to model](#align-infill-direction-to-model)
- [Bridge infill direction](#bridge-infill-direction)
- [Minimum sparse infill threshold](#minimum-sparse-infill-threshold)
- [Infill Combination](#infill-combination)
  - [Max layer height](#max-layer-height)
- [Detect narrow internal solid infill](#detect-narrow-internal-solid-infill)
- [Ensure vertical shell thickness](#ensure-vertical-shell-thickness)

## Align infill direction to model

Aligns infill and surface fill directions to follow the model's orientation on the build plate.  
When enabled, fill directions rotate with the model to maintain optimal characteristics.

![fill-direction-to-model](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/fill-direction-to-model.png?raw=true)

## Bridge infill direction

Bridging angle override.  
If left at zero, the bridging angle will be calculated automatically. Otherwise, the provided angle will be used for bridges.  
Use 180° to represent a zero angle.

## Minimum sparse infill threshold

Sparse infill areas smaller than the threshold value are replaced by [internal solid infill](strength_settings_infill#internal-solid-infill).
This setting helps to ensure that small areas of sparse infill do not compromise the strength of the print. It is particularly useful for models with intricate designs or small features where sparse infill may not provide sufficient support.

## Infill Combination

Automatically combine [sparse infill](strength_settings_infill) of several layers so they print together and reduce print time and while increasing strength. While walls are still printed with the original [layer height](quality_settings_layer_height).

![fill-combination](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/fill-combination.png?raw=true)

### Max layer height

Maximum layer height for the combined sparse infill.  
Set it to 0 or 100% to use the nozzle diameter (for maximum reduction in print time), or to a value of ~80% to maximize sparse infill strength.

The number of layers over which infill is combined is derived by dividing this value by the layer height and rounding down to the nearest decimal.

Use either absolute mm values (e.g., 0.32mm for a 0.4mm nozzle) or percentages (e.g., 80%). This value must not be larger than the nozzle diameter.

## Detect narrow internal solid infill

This option auto-detects narrow internal solid infill areas. If enabled, the [concentric pattern](strength_settings_patterns#concentric) will be used in those areas to speed up printing. Otherwise, the [rectilinear pattern](strength_settings_patterns#rectilinear) will be used by default.

## Ensure vertical shell thickness

Add solid infill near sloping surfaces to guarantee the vertical shell thickness (top and bottom solid layers).

- **None**: No solid infill will be added anywhere. **Caution:** Use this option carefully if your model has sloped surfaces.
- **Critical Only**: Avoid adding solid infill for walls.
- **Moderate**: Add solid infill for heavily sloping surfaces only.
- **All (default)**: Add solid infill for all suitable sloping surfaces.
