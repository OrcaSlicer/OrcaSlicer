# Initial layer speed

Printing the first layer slower than the rest of the print is a widely recommended practice. This helps ensure strong adhesion to the print bed, reduces the chances of warping or curling at the edges, and allows better compensation for minor leveling inconsistencies.

## Initial layer

This setting determines the printing speed for the first layer, excluding [solid infill](strength_settings_top_bottom_shells) regions.  It applies to the [outer/inner walls](strength_settings_walls), [sparse infill](strength_settings_infill) when [bottom layers](strength_settings_top_bottom_shells#shell-layers) is set to 0.  
Adjusting this speed helps ensure proper adhesion and print quality for the initial layer.

## Initial layer infill

Defines the speed used specifically for [solid infill](strength_settings_top_bottom_shells#shell-layers) regions on the first layer. These areas require more precise and consistent extrusion to create a flat and stable surface for subsequent layers. Printing this section too fast may result in high internal stresses (increased risk of warping), poor layer uniformity, or adhesion failures.

## Initial layer travel speed

Sets the travel (non-printing movement) speed for the first layer. This doesn't affect the printing quality and can be set to a percentage of the [travel speed](speed_settings_travel).  
Usually, this is set to 100% of the [travel speed](speed_settings_travel), but it can be reduced if you want to minimize vibrations or if your printer has issues with high-speed travel movements.

## Number of slow layers

Specifies how many of the first layers should be printed at a reduced speed. Instead of jumping straight to full speed after the first layer, the speed gradually increases in a linear fashion over this number of layers. This gradual ramp-up helps maintain adhesion and gives the print more stability in its early stages, especially on prints with a small contact area or materials prone to warping.

![number-of-slow-layers](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/speed/number-of-slow-layers.png?raw=true)
