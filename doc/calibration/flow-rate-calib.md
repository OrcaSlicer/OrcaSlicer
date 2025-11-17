
# Flow Rate Calibration

Flow ratio determines how much filament is extruded and is crucial for high-quality prints.  
A properly calibrated flow ratio ensures consistent layer adhesion and accurate dimensions.

- Too **low** flow ratio causes under-extrusion, which leads to gaps, weak layers, and poor structural integrity.
- Too **high** flow ratio causes over-extrusion, resulting in excess material, rough surfaces, and dimensional inaccuracies.

- [Calibration Types](#calibration-types)
  - [OrcaSlicer \> 2.3.0 Archimedean chords + YOLO (Recommended)](#orcaslicer--230-archimedean-chords--yolo-recommended)
  - [OrcaSlicer \<= 2.3.0 Monotonic Line + 2-Pass Calibration](#orcaslicer--230-monotonic-line--2-pass-calibration)
- [Credits](#credits)

> [!WARNING]
> **BambuLab Printers:** Make sure you do **not** select the 'Flow calibration' option.
> ![flowrate-Bambulab-uncheck](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/flowrate-Bambulab-uncheck.png?raw=true)

> [!NOTE]
> After v2.3.0, the [Top Pattern](strength_settings_top_bottom_shells#surface-pattern) changed to [Archimedean chords](strength_settings_patterns#archimedean-chords) from [Monotonic Line](strength_settings_patterns#monotonic-line).

## Calibration Types

- **YOLO:** A simplified method that adjusts the flow rate in a single pass using the formula `OldFlowRatio ± modifier`.
  - **Recommended:** calibration range `[-0.05, +0.05]`, flow rate step `0.01`.
  - **Perfectionist:** calibration range `[-0.04, +0.035]`, flow rate step `0.005`.
- **2-Pass Calibration:** the legacy method, using two passes to determine the optimal flow rate with the formula `OldFlowRatio * (100 + modifier) / 100`.

### OrcaSlicer > 2.3.0 Archimedean chords + YOLO (Recommended)

This method uses the [Archimedean Chords](strength_settings_patterns#archimedean-chords) pattern for flow rate calibration with the YOLO (Recommended) approach.

1. Select the printer and the filament you want to calibrate.
   This method is based on the filament's current flow ratio, so make sure you select the correct filament before proceeding.
2. In the `Calibration` menu, under the `Flow Rate` section, select `YOLO (Recommended)`.
3. A new project with eleven blocks will be created, each with a different flow rate modifier. Slice and print the project.
   ![flowcalibration-yolo](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/flowcalibration-yolo.gif?raw=true)
4. Examine the printed blocks and identify the one with the best surface quality. Look for:
   1. The smoothest top surface.
   2. No visible gaps between the pattern arcs.
   3. Minimal or no visible line between the Inner Spiral and the Outer Arcs.
   ![flowcalibration-guide](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/flowcalibration-guide.png?raw=true)  
   In this example, the block with a flow modifier of `+0.01` produced the best results, despite a visible line between the Inner Spiral and the Outer Arcs; reducing the flow further begins to show gaps between the lines.  
   ![flowcalibration-example](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/flowcalibration-example.png?raw=true)
5. Update the flow ratio in the filament settings using the equation: `OldFlowRatio ± modifier`.
   If your previous flow ratio was `0.98` and you selected the block with a flow rate modifier of `+0.01`, the new value would be: `0.98 + 0.01 = 0.99`.  
   **Remember** to save the filament profile.  
   ![flowcalibration_update_flowrate](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/flowcalibration_update_flowrate.png?raw=true)

> [!NOTE]
> The new Archimedean chords pattern uses a specific print order that prints the inner spiral last so you can check for material accumulation on the contact line at the end.

### OrcaSlicer <= 2.3.0 Monotonic Line + 2-Pass Calibration

This example uses the Monotonic Line pattern with the 2-Pass Calibration approach.

![flow-calibration-monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/monotonic-flow-rate/flow-calibration-monotonic.gif?raw=true)

1. Select the printer, filament, and process you want to use for the test.
2. In the `Calibration` menu, under the `Flow Rate` section, select `Pass 1`.
3. A new project with nine blocks will be created, each with a different flow rate modifier. Slice and print the project.
4. Examine the blocks and determine which one has the smoothest top surface.
   ![flowrate-pass1-monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/monotonic-flow-rate/flowrate-pass1-monotonic.jpg?raw=true)  
   ![flowrate-0-5-monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/monotonic-flow-rate/flowrate-0-5-monotonic.jpg?raw=true)
5. Update the flow ratio in the filament settings using the equation: `OldFlowRatio * (100 + modifier) / 100`.
   For example, if your previous flow ratio was `0.98` and you selected the block with a flow rate modifier of `+5`, the new value would be: `0.98 × (100 + 5) / 100 = 1.029`.
   **Remember** to save the filament profile.
6. Perform the `Pass 2` calibration. This process is similar to `Pass 1`, but a new project with ten blocks will be generated. The flow rate modifiers for this project will range from `-9` to `0`.
7. Repeat steps 4 and 5. For example, if your previous flow ratio was `1.029` and you selected the block with a flow rate modifier of `-6`, the new value would be: `1.029 × (100 - 6) / 100 = 0.96726`.
   **Remember** to save the filament profile.
   ![flowrate-pass2-monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/monotonic-flow-rate/flowrate-pass2-monotonic.jpg?raw=true)  
   ![flowrate-6-monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/monotonic-flow-rate/flowrate-6-monotonic.jpg?raw=true)  
   ![flowcalibration_update_flowrate_monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/monotonic-flow-rate/flowcalibration_update_flowrate_monotonic.png?raw=true)

> [!TIP]
> @ItsDeidara has created an HTML tool to help with these calculations. Check it out if you find the equations confusing: [Orca-Slicer-Assistant](https://github.com/ItsDeidara/Orca-Slicer-Assistant).

## Credits

- **[Archimedean Chords Idea](https://makerworld.com/es/models/189543-improved-flow-ratio-calibration-v3#profileId-209504)**: [Jimcorner](https://makerworld.com/es/@jimcorner)
