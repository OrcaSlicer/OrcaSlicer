# Max Volumetric Speed (FlowRate) Calibration

Each material profile includes a **maximum volumetric speed** setting, which limits your [print speed](speed_settings_other_layers_speed) to prevent issues like nozzle clogs, under-extrusion, or poor layer adhesion.

This value varies depending on your **material**, **machine**, **nozzle diameter**, and even your **extruder setup**, so it’s important to calibrate it for your specific printer and each filament you use.

> [!NOTE]
> Even for the same material type (e.g., PLA), the **brand** and **color** can significantly affect the maximum flow rate.

> [!TIP]
> If you're planning to increase speed or flow, it’s a good idea to **increase your nozzle temperature**, preferably toward the higher end of the recommended range for your filament. Use a [temperature tower calibration](temp-calib#nozzle-temp-tower) to find that range.

## Calibration Overview

You will be prompted to enter the settings for the test: start volumetric speed, end volumetric speed, and step. It is recommended to use the default values (5mm³/s start, 20mm³/s end, with a step of 0.5), unless you already have an idea of the lower or upper limit for your filament. Select "OK", slice the plate, and send it to the printer.

Once printed, take note of where the layers begin to fail and where the quality begins to suffer.

> [!TIP]
> A **change in surface sheen** (glossy vs. matte) is often a visual cue of material degradation or poor layer adhesion.

![mvf_measurement_point](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/MVF/mvf_measurement_point.jpg?raw=true)

Use calipers or a ruler to measure the **height** of the model just before the defects begin.  
![mvf_caliper_sample_mvf](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/MVF/mvf_caliper_sample_mvf.jpg?raw=true)

 Then you can:

- Use the following formula

  ```math
  Filament Max Volumetric Speed = start + (HeightMeasured * step)
  ```

  In this case (19mm), so the calculation would be: `5 + (19 * 0.5) = 14.5mm³/s`

- Use OrcaSlicer in the "Preview" tab, make sure the color scheme "flow" is selected. Scroll down to the layer height that you measured, and click on the toolhead slider. This will indicate the max flow level for your filament.  
![mvf_gui_flow](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/MVF/mvf_gui_flow.png?raw=true)

After you have determined the maximum volumetric speed, you can set it in the filament settings. This will ensure that the printer does not exceed the maximum flow rate for the filament.  
![mvf_material_settings](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/MVF/mvf_material_settings.png?raw=true)

> [!NOTE]
> This test is a best case scenario and doesn't take into account Retraction or other settings that can increase clogs or under-extrusion.  
> You may want to reduce the flow by 10%-20% (or even further) to ensure print quality/strength.  
> **Printing at high volumetric speed can lead to poor layer adhesion or even clogs in the nozzle.**

> [!TIP]
> @ItsDeidara has made a html to help with the calculation. Check it out if those equations give you a headache [here](https://github.com/ItsDeidara/Orca-Slicer-Assistant).
