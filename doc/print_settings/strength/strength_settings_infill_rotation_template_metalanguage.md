# Infill rotation template metalanguage

This metalanguage provides a way to define the [direction and rotation](strength_settings_infill#direction-and-rotation) of [patterns](strength_settings_patterns) in 3D printing.

- [Basic instructions](#basic-instructions)
  - [Quick examples](#quick-examples)
  - [Defined angle](#defined-angle)
  - [Runtime instructions](#runtime-instructions)
  - [Joint sign](#joint-sign)
  - [Counting](#counting)
  - [Length modifier](#length-modifier)
- [Description of instructions and examples](#description-of-instructions-and-examples)
  - [Simple absolute instructions](#simple-absolute-instructions)
  - [Relative instructions](#relative-instructions)
  - [Repetitive, adjusting and one-time instructions](#repetitive-adjusting-and-one-time-instructions)
  - [Range instructions](#range-instructions)
  - [Constant layer number instructions](#constant-layer-number-instructions)
- [Complex template examples](#complex-template-examples)
- [Credits](#credits)

## Basic instructions

## Quick examples

- `0` - fixed direction at 0° (X-axis)
- `0, 90` - alternate 0° and 90° each layer
- `0, 15, 30` - alternate 0°, 15° and 30° each layer
- `+90` - rotate 90° each layer (sequence 90°, 180°, 270°, 0° ...)
- `+45` - rotate 45° each layer for higher dispersion
- `+30/50%` - linearly rotate by 30° over the next 50% of model height
- `+45/10#` - linearly rotate by 45° across 10 standard layers
- `+15#10` - keep the same angle for 10 layers, then rotate +15°; repeats every 10 layers
- `B!, +30` - skip the first bottom shell layers from rotation, then rotate 30° per layer
- `0, +30, +90` - use a repeating sequence of 0°, +30°, +90°

`[±]α[*ℤ or !][joint sign, or its combinations][-][ℕ, B or T][length modifier][* or !]` - full length template instruction for the **sparse** infill

`[±]α*` - just setting an initial rotation angle

 

> [!NOTE]
> `[...]` - values in square brackets are optional

### Defined angle

`[±]α` - command for setting rotation infill angle (for joint infills at some height range, this angle is finite):

- `α:β` - set the angle α value as a percentage of the full 360 degree rotation
- `+α` - set positive relative angle CCW
- `-α` - set negative relative angle CW

### Runtime instructions

`[*, *ℤ or !]` - runtime instructions:

- `*` - the mark of "dummy" instruction. It's needed for setting an initial angle. No further action will be taken
- `*ℤ` - repeat the instruction ℤ times
- `!` - the one-time running instruction

 

### Joint sign

`[joint sign]` - the symbol which determines the method of connection for turning of the infill:

- `/` - linear displacement of the infill. e.g. `+22.5/50%`  
  ![linear-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/linear-joint.png?raw=true)
- `#` - infill of multiple layers with vertical displacement at finish angle. e.g. `+22.5#50%`  
  ![multiple-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/multiple-joint.png?raw=true)
- `#-` - infill of multiple layers with vertical displacement at initial angle. e.g. `+22.5#-50%`  
  ![multiple-joint-initial-angle](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/multiple-joint-initial-angle.png?raw=true)
- `|` - infill of multiple layers with vertical displacement at middle angle. e.g. `+22.5|50%`  
  ![multiple-joint-middle-angle](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/multiple-joint-middle-angle.png?raw=true)
- `N` - infill formed by sinus function (vertical connection). e.g. `+22.5N50%`  
  ![v-sinus-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/v-sinus-joint.png?raw=true)
- `n` - infill formed by sinus function (vertical connection, lazy). e.g. `+22.5n50%`  
  ![v-sinus-joint-lazy](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/v-sinus-joint-lazy.png?raw=true)
- `Z` - infill formed by sinus function (horizontal connection). e.g. `+22.5Z50%`  
  ![z-h-sinus-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/z-h-sinus-joint.png?raw=true)
- `z` - infill formed by sinus function (horizontal connection, lazy). e.g. `+22.5z50%`  
  ![h-sinus-joint-lazy](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/h-sinus-joint-lazy.png?raw=true)
- `L` - infill formed by quarter of circle (horizontal to vertical connection). e.g. `+22.5L50%`  
  ![vh-quarter-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/vh-quarter-joint.png?raw=true)
- `l` - infill formed by quarter of circle (vertical to horizontal connection). e.g. `+22.5l50%`  
  ![hv-quarter-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/hv-quarter-joint.png?raw=true)
- `U` - infill formed by squared function. e.g. `+22.5U50%`  
  ![squared-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/squared-joint.png?raw=true)
- `u-` - infill formed by squared function (inverse). e.g. `+22.5u-50%`  
  ![squared-joint-inverse](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/squared-joint-inverse.png?raw=true)
- `Q` - infill formed by cubic function. e.g. `+22.5Q50%`  
  ![cubic-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/cubic-joint.png?raw=true)
- `q-` - infill formed by cubic function (inverse). e.g. `+22.5q-50%`  
  ![cubic-joint-inverse](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/cubic-joint-inverse.png?raw=true)
- `$` - infill formed by arcsinus method. e.g. `+22.5$50%`  
  ![arcsinus-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/arcsinus-joint.png?raw=true)
- `~` - infill formed with random angle. e.g. `+22.5~50%`  
  ![random-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/random-joint.png?raw=true)
- `^` - infill formed with pseudorandom angle. e.g. `+22.5^50%`  
  ![pseudorandom-joint](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/pseudorandom-joint.png?raw=true)

### Counting

`[-]ℕ` - counting the distance at which the turn will take place:

- `ℕ` - the count will take place by ℕ layers. e.g. `+22.5/50%`  
  ![infill-counting](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/infill-counting.png?raw=true)
- `-ℕ` - indicates that the joint form will be flipped upward. e.g. `+22.5/-50%`  
  ![infill-counting-flipped](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/infill-counting-flipped.png?raw=true)
- `B` - the count will take place over the next layers equal to the bottom_shell_layers parameter
- `T` - the count will take place over the next layers equal to the top_shell_layers parameter

### Length modifier

`ℕ[length modifier]` - the distance at which the specified turn will take place:

- `ℕmm` - the distance in millimeters
- `ℕcm` - the distance in centimeters
- `ℕm` - the distance in meters
- `ℕ'` - the distance in feet
- `ℕ"` - the distance in inches
- `ℕ#` - the distance in range of standard height of ℕ layers
- `ℕ%` - the distance as a percentage of model height

## Description of instructions and examples

Each instruction is written by a combination of symbols and numbers and separated by a comma or a space.
For more complex instructions, autoformatting is used to make the template easier to read.

> [!NOTE]
> All examples are shown with a 5% density rectilinear infill on a model of a cube 20x20x20mm which has 100 layers of 0.2mm thickness. Without walls and upper and lower shells. Initial angle is 0.

### Simple absolute instructions

They include a simple definition of the angle for each layer. Note that the initial setting of this angle is also affected by the value in the infill angle field.

- `0`, `15`, `45.5`, `256.5605`... - just fill at the existing angle. The initial direction starts at the X-axis, and the acceptable range of values is from 0 to 360  
  - `0` as well as `+0`, `-0` or just empty template  
  ![0](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/0.png?raw=true)
  - `45`  
  ![45](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/45.png?raw=true)
  - `0, 30` - is a simple alternation through each layer in the direction of 0 and 30 degrees.  
  ![0-30](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/0-30.png?raw=true)
- `0%`, `10%`, `25%`, `100%`... - infill angle determined from relative terms from a full turn of 360 degree rotation. Rotate by 0, 36, 90, and 0 degrees.  
  - `25%` - the equivalent of `90` instruction.  
  ![90](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/90.png?raw=true) 
- `30, 60, 90, 120, 150, 0` - a more complex command defines a turn every layer at 30 degrees. At the end of the template line, the next instruction is read first, and this process continues until the entire height of the model is filled.

### Relative instructions

- `+30` - this is a short instruction for counterclockwise rotation. The equivalent of `30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330, 0` or `30, 60, 90, 120, 150, 0` instruction.  
  ![+30](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+30.png?raw=true)
- `-30` - this is the same instruction, but with clockwise rotation. The equivalent of `330, 300, 270, 240, 210, 180, 150, 120, 90, 60, 30, 0` or `330, 300, 270, 240, 210, 0` instruction.  
- `+150` - you can specify a different multiple of the irrational angle for better fill dispersion = `150, 300, 90, 240, 30, 180, 330, 120, 270, 60, 210, 0` ...  
- `+45` - The equivalent of `45, 90, 135, 180, 225, 270, 315, 0` or `45, 90, 135, 0` instruction.  
  ![+45](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+45.png?raw=true)
- `+90` - The equivalent of `90, 180, 270, 0` or `90, 0` instruction.  
  ![+90](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+90.png?raw=true)
- `+15%` - useful for dividing angles on a decimal basis = `54, 108, 162, 270, 324, 18, 72, 126, 180, 234, 288, 342, 36, 90, 144, 196, 252, 306, 0` ...  
- `+30, +90` - a complex instruction setting the rotation of each layer in these positions = `30, 120, 150, 240, 270, 0` ...  
  ![+30+90](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+30+90.png?raw=true)
- `0, +30, +90` - a complex instruction setting the rotation of each layer in these positions = `0, 30, 120` ...  
  ![0+30+90](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/0+30+90.png?raw=true)

### Repetitive, adjusting and one-time instructions

- `5, 10, +20` - simple instructions without modifiers. Sets the angles in sequence = 5, 10, 30, 5, 10, 30, 5, 10 ...
- `5, 10*, +20` - the `*` sign sets the initial angle without quantitative designation and without layer processing. It is useful for setting the orientation of a group of layers, which will be described below. Sets the angles in sequence = 5, 30, 5, 30, 5, 30, 5 ...
- `5, 10!, +20` - the `!` sign indicates that the instruction will be executed only once for the entire height of the model. Sets the angles in sequence = 5, 10, 30, 5, 25, 5, 25, 5 ...
- `5, 10*!, +20` - the combination of `*` and `!` signs is also usable = 5, 30, 5, 25, 5, 25, 5, 25 ...
- `5, 10*3, +20` - if a number is written after the `*` sign, it indicates the number of repetitions of this instruction. Sets the angles in sequence = 5, 10, 10, 10, 30, 5, 10, 10, 10, 30, 5, 10 ...

### Range instructions

A combined set of layers will be organized, where the rotation of one layer relative to the other will also be predetermined.
You can specify how many layers will be rotated by a certain angle, and according to which mathematical law this rotation will be performed. This law is determined by writing a certain symbol and specifying a numeric value after it.
The following signs are available that determine the shape of the turn: `/` `#` `#-` `|` `N` `n` `Z` `z` `L` `l` `U` `u` `Q` `q` `$` `~` `^`. For their purpose, see [joint sign](#joint-sign).

Also, after the numeric value there is a range modifier, then this rotation will occur according to the described length.
The following modifiers are available that determine the range of turn: `mm` `cm` `m` `'` `"` `#` `%`. For their purpose, see [length modifier](#length-modifier).

If there is a `-` sign before the numeric value, then the initial fill angle changes with the final one. This is useful for joining the linear infills in some cases. Absolute values of the rotation angle using the range instructions have no effect.
It is important to know that this will not be the exact length, but will be tied to the nearest layer from below.

- `+45/100` - rotate the next 100 layers linearly at a 45 degree angle. For this model, this instruction is equivalent to `+45/100%` as it contains 100 layers.  
  ![+45-100](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+45-100.png?raw=true)
- When changing the height of the instruction `+45/50` or `+45/50%` - the final angle will be 90, as the turn will occur twice.  
  ![+45-50](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+45-50.png?raw=true)
- `-50%Z1cm` - rotate one centimeter of infill by sinus function at a 180 degree CW.

### Constant layer number instructions

There are 2 letter signs `T` and `B` that can determine the number of shell layers on the top and bottom of the model. It is useful for calculating skipping this amount to align the fill.

- `B!, +30` - skip the first shell layers from rotation, then fill with 30 degree turn each layer  
- `+30/1cm, T` - rotate one centimeter of infill linearly at a 30 degree angle, then skip the number of layers equal to the count of the upper shell layers without rotation.  

 

## Complex template examples

- `+10L25%, -10l25%, -10L25%, +10l25%` - fill the model with sine period with 10 degree amplitude  
  ![10period](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/10period.png?raw=true)
- `+30/-10#` - rotate the infill at height of 10 standard layers (or @ standard layer height is 0.2mm x 10 = 2mm) inverse linearly at a 30 degree angle.  
  ![+30-10](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+30-10.png?raw=true)
- `+360~100%` or `+100%~100%` - fill the model with infill with random direction at each layer.  
  ![+360-100p](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/Template-metalanguage/+360-100p.png?raw=true)

## Credits

- **Feature author:** [@pi-squared-studio](https://github.com/pi-squared-studio).
