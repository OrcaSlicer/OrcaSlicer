# Other layers speed

## Speed limitations

> [!IMPORTANT]
> Every speed setting is limited by several parameters like:
>
> - [Maximum Volumetric Speed](volumetric-speed-calib)
> - Machine / Motion ability
> - [Acceleration](speed_settings_acceleration)
> - [Jerk settings](speed_settings_jerk_xy)

- [Speed limitations](#speed-limitations)
- [Outer wall](#outer-wall)
- [Inner wall](#inner-wall)
- [Small perimeters](#small-perimeters)
  - [Small perimeters threshold](#small-perimeters-threshold)
- [Sparse infill](#sparse-infill)
- [Internal solid infill](#internal-solid-infill)
- [Top surface](#top-surface)
- [Gap infill](#gap-infill)
- [Ironing speed](#ironing-speed)
- [Support](#support)
- [Support interface](#support-interface)

## Outer wall

Speed of outer wall which is outermost and visible. It's used to be slower than [inner wall speed](#inner-wall) to get better quality and good layer adhesion.
This setting is also limited by [Machine / Motion ability / Resonance avoidance speed settings](vfa-calib).

## Inner wall

Speed of inner wall which is printed faster than outer wall to reduce print time but is still recommended to be slower than the [maximum volumetric speed](volumetric-speed-calib) to ensure good layer adhesion and reduce material internal stresses.

## Small perimeters

Speed of outer wall with theoretical radius <= [small perimeters threshold](#small-perimeters-threshold).
Any shape (not only circles) will be considered as a small perimeter.

If expressed as percentage (for example: 80%) it will be calculated on the [outer wall speed](#outer-wall).

> [!NOTE]
> Zero will use [50%](https://github.com/OrcaSlicer/OrcaSlicer/blob/7d2a12aa3cbf2e7ca5d0523446bf1d1d4717f8d1/src/libslic3r/GCode.cpp#L4698) of [outer wall speed](#outer-wall).

### Small perimeters threshold

**Radius** in millimeters below which the speed of perimeters will be reduced to the [small perimeters speed](#small-perimeters).  
To know the length of the perimeter, you can use the formula:

```math
\frac{\text{Perimeter Length}}{2\pi} \leq \text{Threshold}
```

For example, if the threshold is set to 5 mm, then the perimeter length must be less than or equal to 31.4 mm (2 * π * 5 mm) to be considered a small perimeter.

- A Circle with a diameter of 10 mm will have a perimeter length of approximately 31.4 mm, which is equal to the threshold, so it will be considered a small perimeter.
- A Cube of 10mm x 10mm will have a perimeter length of 40 mm, which is greater than the threshold, so it will not be considered a small perimeter.
- A Cube of 5mm x 5mm will have a perimeter length of 20 mm, which is less than the threshold, so it will be considered a small perimeter.

> [!NOTE]
> Zero will disable [small perimeters speed](#small-perimeters) and will use the [outer wall speed](#outer-wall).

## Sparse infill

Speed of [sparse infill](strength_settings_infill) which is printed faster than solid infill to reduce print time.  
In case you are using your [Infill Pattern](strength_settings_infill) as aesthetic feature, you may want to set it closer to the [outer wall speed](#outer-wall) to get better quality.

## Internal solid infill

Speed of internal solid infill, which fills the interior of the model with solid layers.  
This is typically set faster than the [top surface speed](#top-surface) to optimize print time, while still ensuring adequate strength and layer adhesion. Adjusting this speed can help balance print quality and efficiency, especially for models requiring strong internal structures.  
Solid infill is also considered when [infill % is set to 100%](strength_settings_infill#internal-solid-infill).

## Top surface

Speed of the [topmost solid layers](strength_settings_top_bottom_shells) of the print. This is usually set similar to the [outer wall speed](#outer-wall) to achieve a smoother and higher-quality finish on visible surfaces. Lower speeds help minimize surface defects and improve the appearance of the final printed object.

## Gap infill

Speed of [gap infill](strength_settings_infill#apply-gap-fill), which is used to fill small gaps or holes in the print.

## Ironing speed

[Ironing](quality_settings_ironing) and [Support Ironing](support_settings_ironing) speed, typically slower than the top surface speed to ensure a smooth finish.

## Support

Speed at which [support](support_settings_support) material is printed. Slower speeds help ensure that supports are stable and effective during the print process.

## Support interface

Speed for the support interface layers, which are the layers directly contacting the model. This is usually set even slower than the main [support speed](#support) to maximize surface quality where the support meets the model and to make support removal easier.
