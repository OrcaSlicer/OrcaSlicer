# Brim

Brim is a flat layer printed around a model's base to improve adhesion to the print bed. It is useful for models with small footprints or those prone to warping.

![brim](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/brim/brim.png?raw=true)

- [Type](#type)
  - [Auto](#auto)
  - [Painted](#painted)
  - [Outer](#outer)
  - [Inner](#inner)
  - [Outer and Inner](#outer-and-inner)
  - [Mouse Ears](#mouse-ears)
    - [Ear max angle](#ear-max-angle)
    - [Ear detection radius](#ear-detection-radius)
- [Width](#width)
- [Brim-Object Gap](#brim-object-gap)

## Type

Controls how the brim is generated on a model's outer and/or inner sides.

### Auto

The Auto brim feature computes an optimal brim width by evaluating material properties, part geometry, printing speed, and thermal characteristics.

- Model geometry
  - Uses the model's bounding box to determine dimensions.
  - Height-to-area ratio: `height/(width²*length)`.
- Printing speed
  - Higher maximum printing speeds generally increase the recommended brim width.
- Thermal length
  - Defined as the diagonal of the model's base.
  - Reference thermal lengths (material-specific):
    - ABS, PA-CF, PET-CF: 100
    - PC: 40
    - TPU: 1000
- Material adhesion coefficient
  - Default: 1
  - PETG/PCTG: 2
  - TPU: 0.5

The computed brim width is capped at 20 mm and at 1.5× the thermal length. If the final width is under 5 mm and also less than 1.5× the thermal length, no brim will be generated (width = 0).

### Painted

Generates a brim only on areas that have been painted ![toolbar_brimears_dark](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/toolbar_brimears_dark.svg?raw=true) in the Prepare tab .

### Outer

Creates a brim around the model's outer perimeter.  
Easier to remove than an inner brim, but may affect the model's appearance if not removed cleanly.

![brim-outer](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/brim/brim-outer.png?raw=true)

### Inner

Creates a brim around inner perimeters.  
More difficult to remove and less effective than an outer brim and may obscure fine inner details, but it can hide the brim removal seam.

![brim-inner](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/brim/brim-inner.png?raw=true)

### Outer and Inner

Creates a brim around both the outer and inner perimeters of the model.  
This approach combines the **disadvantages** of both brim types, making it more difficult to remove while potentially obscuring fine details but improving overall adhesion.

![brim-outer-inner](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/brim/brim-outer-inner.png?raw=true)

> [!TIP]
>> Consider using a [raft](support_settings_raft) on complex models/materials.

### Mouse Ears

Mouse ears are small, local brim extensions (typically placed near corners and sharp features) that improve bed adhesion and reduce warping while using less material than a full brim.  
The geometry analysis routine selects candidate locations based on the configured angle threshold and detection radius.

![brim-mouse-ears](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/brim/brim-mouse-ears.png?raw=true)

#### Ear max angle

Angle threshold (degrees) used to decide where mouse ears may be placed:

- 0° — disabled; no mouse ears are generated.
- Between 0° and 180° — ears are created at features with local angles sharper (smaller) than the threshold.
- 180° — ears are allowed on almost any non-straight feature.

#### Ear detection radius

The geometry will be decimated before detecting sharp angles.  
This parameter indicates the minimum length of the deviation for the decimation.  
0 to deactivate.

## Width

Distance between the model and the outermost brim line.  
Increasing this value widens the brim, which can improve adhesion but increases material usage.

## Brim-Object Gap

Gap between the innermost brim line and the object.  
Increasing the gap makes the brim easier to remove but reduces its adhesion benefit; very large gaps may eliminate contact and negate the brim's purpose.
