# Skirt

A skirt is one or more additional perimeters printed around the model outline on the first layer(s). It helps prime the hotend, stabilise extrusion before the model starts, and can act as a basic wind/draft shield when built taller.

- [Loops](#loops)
- [Type](#type)
  - [Combined](#combined)
  - [Per object](#per-object)
- [Minimum extrusion Length](#minimum-extrusion-length)
- [Distance](#distance)
- [Start point](#start-point)
- [Speed](#speed)
- [Height](#height)
- [Shield](#shield)
- [Single loop after first layer](#single-loop-after-first-layer)

## Loops

Number of skirt loops to print. 
Usually 2 loops are recommended but increasing loops improve priming and give a larger buffer between the nozzle and the part, at the cost of extra filament and time.  
Set to 0 to disable the skirt.

![skirt](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/skirt/skirt.png?raw=true)

## Type

### Combined

A single skirt that surrounds all objects on the bed.
  Recommended for general use.

![skirt-combined](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/skirt/skirt-combined.png?raw=true)

### Per object

Each object gets its own skirt printed separately.
  Recommended when using [Print sequence by object](others_settings_special_mode#by-object).

![skirt-per-object](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/skirt/skirt-per-object.png?raw=true)

## Minimum extrusion Length

Minimum filament extrusion length in mm when printing the skirt. Zero means this feature is disabled.  
Using a non-zero value is useful if the printer is set up to print without a prime line.  
Final number of loops is not taking into account while arranging or validating objects distance. Increase loop number in such case.

## Distance

Distance from skirt to brim or object.  
Increasing this distance can help avoid collisions with brims or supports, but will increase the footprint of the skirt and filament usage.

## Start point

Start angle for the skirt relative to the object centre. 0° is the right-most position (along the +X axis), angles increase counter-clockwise.  
Use this to control where the skirt begins to better align with part features or prime locations.

## Speed

Printing speed for the skirt in mm/s. Set to 0 to use the default first-layer extrusion speed.  
Slower speeds give a more reliable prime; very fast skirt speeds may not adhere properly and come off, causing problems with the part.

## Height

Number of layers the skirt should be printed for. Usually 1 layer for priming. Increase the height if you want a taller draft shield effect.

## Shield

When enabled the skirt can be printed as a draft shield: a taller wall surrounding the part to help protect prints (especially ABS/ASA) from drafts and sudden temperature changes.  
This is most useful for open-frame printers without an enclosure.

- If set to follow the highest object, the shield will be as tall as the tallest printed model on the bed.
- Otherwise it will use the value specified in "Skirt height".

> [!NOTE]
> With the draft shield active, the skirt will be printed at [skirt distance](#distance) from the object. Therefore, if brims are active it may intersect with them. To avoid this, increase the skirt distance value.

## Single loop after first layer

When enabled, limits the draft shield to a single wall after the first layer (i.e. only one loop is printed on subsequent shield layers). This reduces filament and print time but makes the shield less robust and more prone to warping or cracking.
