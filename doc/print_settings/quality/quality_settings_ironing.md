# Ironing

Ironing is a process used to improve the surface finish of 3D prints by smoothing out the top layers. This is achieved by printing a second time at the same height, but with a very [low flow rate](#flow) and a specific [pattern](#pattern). The result is a smoother surface that can enhance the aesthetic quality of the print increasing print time.

![ironing](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/ironing/ironing.png?raw=true)

> [!IMPORTANT]
> Ironing can cause filament to move very slowly through the hotend, which increases the risk of heat creep and potential clogging. Monitor your printer during ironing and ensure your hotend cooling is adequate to prevent jams.

## Type

This setting controls which layer being ironed.

- **Top Surfaces**: All [top surfaces](strength_settings_top_bottom_shells) will be ironed. This is the most common setting and is used to smooth out the top layers of the print.  
  ![ironing-top-surfaces](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/ironing/ironing-top-surfaces.png?raw=true)
- **Topmost Surface**: Only the last [top layer](strength_settings_top_bottom_shells) of the print will be ironed. This is useful for prints where only the last layer needs to be smoothed.  
  ![ironing-topmost-surface](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/ironing/ironing-topmost-surface.png?raw=true)
- **All solid layers**: All solid layers, including [internal solid infill](strength_settings_infill#internal-solid-infill) and [top layers](strength_settings_top_bottom_shells), will be ironed. This can be useful for prints that require a very smooth finish on all solid surfaces but may increase print time significantly.  
    ![ironing-all-solid-layers](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/ironing/ironing-all-solid-layers.png?raw=true)

## Pattern

The pattern that will be used when ironing. Usually, the best pattern is the one with the most efficient coverage of the surface.  

> [!TIP]
> See [Infill Patterns Wiki List](strength_settings_patterns) with **detailed specifications**, including their strengths and weaknesses.

 The ironing patterns are:

- **[Concentric](strength_settings_patterns#concentric)**
- **[Rectilinear](strength_settings_patterns#rectilinear)**

## Flow

The amount of material to extrude during ironing.  
This % is a percentage of the normal flow rate. A lower value will result in a smoother finish but may not cover the surface completely. A higher value may cover the surface better but can lead to over extrusion or rougher finish. 

A lower layer height may require higher flow due to less volumetric extrusion per distance.

## Line spacing

The distance between the lines of ironing.  
It's recommended to set this value to be equal to or less than the nozzle diameter for optimal coverage and surface finish.

## Inset

The distance to keep from the edges, which can help prevent over-extrusion at the edges of the surface being ironed.

![ironing-inset](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/ironing/ironing-inset.png?raw=true)

If this value is set to 0, the ironing toolpath will start directly at the perimeter edges without any inward offset. This means the [ironing pattern](#pattern) will extend all the way to the outer boundaries of the top surface being ironed.

## Angle Offset

The angle of ironing lines offset relative to the top surface solid infill direction. Commonly used ironing angle offsets are 0°, 45°, and 90° each producing a [different surface finish](https://github.com/SoftFever/OrcaSlicer/issues/10834#issuecomment-3322628589) which will depend on your printer nozzle.

## Speed

See [Speed settings for other layers](speed_settings_other_layers_speed#ironing-speed) for more information about ironing speed.
