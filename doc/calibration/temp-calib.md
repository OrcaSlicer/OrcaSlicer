# Temp Calibration

In FDM 3D printing, the temperature is a critical factor that affects the quality of the print.
There is no other calibration that can have such a big impact on the print quality as temperature calibration.

- [Standard Temperature Ranges](#standard-temperature-ranges)
- [Nozzle Temp tower](#nozzle-temp-tower)
- [Bed Temperature](#bed-temperature)
- [Chamber Temperature](#chamber-temperature)

## Standard Temperature Ranges

|   Material   | [Nozzle Temp (°C)](#nozzle-temp-tower) | [Bed Temp (°C)](#bed-temperature) | [Chamber Temp (°C)](#chamber-temperature) |
|:------------:|:--------------------------------------:|:---------------------------------:|:-----------------------------------------:|
| PLA          | 180-220                                | 50-60                             | Ambient                                   |
| ABS          | 230-250                                | 90-100                            | 50-70                                     |
| ASA          | 240-260                                | 90-100                            | 50-70                                     |
| Nylon 6      | 230-260                                | 90-110                            | 70-100                                    |
| Nylon 12     | 225-260                                | 90-110                            | 70-100                                    |
| TPU          | 220-245                                | 40-60                             | Ambient                                   |
| PC           | 270-310                                | 100-120                           | 80-100                                    |
| PC-ABS       | 260-280                                | 95-110                            | 60-80                                     |
| HIPS         | 220-250                                | 90-110                            | 50-70                                     |
| PP           | 220-270                                | 80-105                            | 40-70                                     |
| Acetal (POM) | 210-240                                | 100-130                           | 70-100                                    |

## Nozzle Temp tower

Nozzle temperature is one of the most important settings to calibrate for a successful print. The temperature of the nozzle affects the viscosity of the filament, which in turn affects how well it flows through the nozzle and adheres to the print bed. If the temperature is too low, the filament may not flow properly, leading to under-extrusion, poor layer adhesion and stringing. If the temperature is too high, the filament may degrade, over-extrude and produce stringing.

![temp-tower_test](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Temp-calib/temp-tower_test.gif?raw=true)

![temp-tower_test_menu](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Temp-calib/temp-tower_test_menu.png?raw=true)

Temp tower is a straightforward test. The temp tower is a vertical tower with multiple blocks, each printed at a different temperature.  
Once the print is complete, we can examine each block of the tower and determine the optimal temperature for the filament. The optimal temperature is the one that produces the highest quality print with the least amount of issues, such as stringing, layer adhesion, warping (overhang), and bridging.

![temp-tower](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Temp-calib/temp-tower.jpg?raw=true)

> [!NOTE]
> If a range of temperatures looks good, you may want to use the middle of that range as the optimal temperature.  
> But if you are planning to print at higher [speeds](speed_settings_other_layers_speed)/[flow rates](volumetric-speed-calib), you may want to use the higher end of that range as the optimal temperature.

## Bed Temperature

Bed temperature plays a crucial role in ensuring proper filament adhesion to the build surface, which directly impacts both print success and quality.  
Most materials have a relatively broad optimal range for bed temperature (typically +/-5°C).  
In general, following the manufacturer’s recommendations, maintaining a clean bed (free from oils or fingerprints), ensuring a stable [chamber temperature](#chamber-temperature), and having a properly leveled bed will produce reliable results.

- If the bed temperature is too low, the filament may fail to adhere properly, leading to warping, weak layer bonding, or complete detachment. In severe cases, the printed part may dislodge entirely and stick to the nozzle or other printer components, potentially causing mechanical damage.
- If the bed temperature is too high, the lower layers can overheat and soften excessively, resulting in deformation such as [elephant foot](quality_settings_precision#elephant-foot-compensation).

> [!TIP]
> As a general guideline, you can use the [glass transition temperature](https://en.wikipedia.org/wiki/Glass_transition) (Tg) of the material and subtract 5–10 °C to estimate a safe upper limit for bed temperature.  
> See [this article](https://magigoo.com/blog/prevent-warping-temperature-and-first-layer-adhesion-magigoo/) for a detailed explanation.

> [!NOTE]
> For challenging prints involving materials with **high shrinkage** (e.g., nylons or polycarbonate) or geometries prone to warping, dialed-in settings are critical.  
> In these cases, [chamber temperature](#chamber-temperature) becomes a **major factor** in preventing detachment and ensuring print success.

## Chamber Temperature

Chamber temperature can affect the print quality, especially for high-temperature filaments.  
A heated chamber can help to maintain a consistent temperature throughout the print, reducing the risk of warping and improving layer adhesion. However, it is important to monitor the chamber temperature to ensure that it does not exceed the filament's deformation temperature.

See: [Chamber temperature printer settings](Chamber-temperature)

> [!IMPORTANT]
> Low temperature Filaments like PLA can clog the nozzle if the chamber temperature is too high.
