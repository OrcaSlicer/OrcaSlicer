# Walls

In 3D printing, "walls" refer to the outer layers of a printed object that provide its shape and structural integrity.  
Adjusting wall settings can significantly affect layer adhesion, strength, appearance and print time of your model.

![walls](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/walls/walls.png?raw=true)

- [Wall loops](#wall-loops)
- [Alternate extra wall](#alternate-extra-wall)
- [Detect thin wall](#detect-thin-wall)

## Wall loops

"Wall loops" refers to the number of times the outer wall is printed in a loop.  
Increasing the wall loops will:
- Enhance: 
  - Layer adhesion
  - Strength
  - Rigidity
- Reduce infill ghosting
- Increase print time

## Alternate extra wall

This setting adds an extra wall to every other layer. This way the infill gets wedged vertically between the walls, resulting in stronger prints.  
When this option is enabled, the ensure vertical shell thickness option needs to be disabled.  

> [!WARNING]
> It's not recommended to use this option with:
> - [Lightning infill](strength_settings_patterns#lightning) as there is limited infill to anchor the extra perimeters to.
> - **[Ensure vertical shell thickness: ALL](strength_settings_advanced#ensure-vertical-shell-thickness)**

## Detect thin wall

By default, walls are printed as closed loops. When a wall is too thin to contain two line widths, enabling "Detect thin walls" prints it as a single extrusion line.  
Thin walls printed this way may have reduced surface quality and strength because they are not closed loops.

> [!TIP]
> Usually, it is recommended to use [Arachne wall generator](quality_settings_wall_generator#arachne) which will disable "Detect thin walls" because it uses a different approach to wall generation.

- In small details it can generate details that wouldn't be possible with traditional wall generation methods.  
  ![walls-small-detect-thin-off](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/walls/walls-small-detect-thin-off.png?raw=true)
  ![walls-small-detect-thin-on](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/walls/walls-small-detect-thin-on.png?raw=true)
- In large prints, it can generate defects more easily due to the reduced wall thickness.
  ![walls-big-detect-thin-off-on](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/walls/walls-big-detect-thin-off-on.png?raw=true)
