# Cornering

Cornering is a critical aspect of 3D printing that affects print quality and accuracy. It's how the printer handles changes in direction during movement, particularly at corners and curves. Proper cornering settings can reduce artifacts such as ringing, ghosting, and overshooting, resulting in cleaner and more precise prints.

## Types of Cornering Settings

> [!TIP]
> Read more in [Jerk XY](speed_settings_jerk_xy) and check [Cornering Control Types](speed_settings_jerk_xy#cornering-control-types)

## Calibration

This test will be set detect automatically your printer firmware type and will adapt to the specific calibration process.

- Klipper: [square_corner_velocity](https://www.klipper3d.org/Config_Reference.html#printer)
- Marlin 2:
  - [Junction Deviation](https://marlinfw.org/docs/configuration/configuration.html#junction-deviation-) if `Maximum Junction Deviation` in Printer settings/Motion ability/Jerk limitations is bigger than `0`.
  - [Classic Jerk](https://marlinfw.org/docs/configuration/configuration.html#jerk-) if `Maximum Junction Deviation` is set to `0`.
- Marlin Legacy: [Classic Jerk](https://marlinfw.org/docs/configuration/configuration.html#jerk-).
- RepRap: [Maximum instantaneous speed changes](https://docs.duet3d.com/User_manual/Reference/Gcodes#m566-set-allowable-instantaneous-speed-change)

> [!NOTE]
> This calibration example uses Junction Deviation as an example. The process is similar for Jerk calibration; just read the Jerk values instead of JD values.  
> JD values are between `0.0` and `0.3` (in mm) while Jerk values are usually between `1` and `20` or higher (in mm/s).

1. Pre-requisites:
   1. If using Marlin 2 firmware, Check if your printer has Junction Deviation enabled. Look for `Junction deviation` in the printer's advanced settings.
   2. In OrcaSlicer, set:
      1. Acceleration high enough to trigger ringing or the speed you want to check out (e.g., 2000 mm/s²).
      2. Speed high enough to trigger ringing (e.g., 100 mm/s).
   3. Use an opaque, high-gloss filament to make ringing more visible.
2. Open the Cornering test.  
   ![jd_first_menu](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_first_menu.png?raw=true)
   1. In this first approximation, set a wide range of Start and End values.
      - If you don't see any loss of quality, increase the End value and retry.
      - If you do see a loss of quality, measure the maximum height when the corners start losing sharpness and read the Cornering/Jerk/JunctionDeviation value set at that point in OrcaSlicer.  
      ![jd_first_print_measure](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_first_print_measure.jpg?raw=true)  
      ![jd_first_slicer_measure](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_first_slicer_measure.png?raw=true)
   2. Print a new calibration tower with a maximum set near the point where corners start losing sharpness.  
      **RECOMMENDED:** Use the *Ringing Tower* test model to more easily visualize the jerk limit.
   3. Print the second Cornering test with the new maximum value.  
      ![jd_second_menu](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_second_menu.png?raw=true)
   4. Measure the maximum height when the corners start losing sharpness and read the Cornering/Jerk/JunctionDeviation value set at that point in OrcaSlicer.  
      ![jd_second_print_measure](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_second_print_measure.jpg?raw=true)  
      ![jd_second_slicer_measure](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_second_slicer_measure.png?raw=true)
3. Save the settings
   - Into your OrcaSlicer printer profile (**RECOMMENDED**):
     1. Go to Printer settings → Motion ability → Jerk limitation:
     2. Set your maximum Jerk X and Y or Junction Deviation values.  
        ![jd_printer_jerk_limitation](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/JunctionDeviation/jd_printer_jerk_limitation.png?raw=true)
   - Directly into your printer firmware:
     - Restore your 3D Printer settings to avoid keeping high acceleration and jerk values used for the test.

     - Klipper:
       - Skeleton

       ```gcode
       SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=#SquareCornerVelocity
       ```

       Example:

       ```gcode
       SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=5.0
       ```

       Note: You can also set `square_corner_velocity` persistently in your `printer.cfg` (restart required).

     - Marlin 2 (Junction Deviation enabled):
       - Skeleton

       ```gcode
       M205 J#JunctionDeviationValue
       M500
       ```

       Example:

       ```gcode
       M205 J0.012
       M500
       ```

       - To make the change permanent in firmware, set in `Configuration.h` and recompile:

       ```cpp
       #define JUNCTION_DEVIATION_MM 0.012  // (mm) Distance from real junction edge
       ```

       Also ensure classic jerk is disabled if using junction deviation:

       ```cpp
       //#define CLASSIC_JERK
       ```

     - Marlin Classic Jerk / Marlin Legacy:
       - Skeleton — set the per-axis jerk limits using `M205` (X/Y optional depending on firmware build):

       ```gcode
       M205 X#JerkX Y#JerkY
       M500
       ```

       Example:

       ```gcode
       M205 X10 Y10
       M500
       ```

     - RepRap (Duet / RepRapFirmware):
       **IMPORTANT:** Set in mm/min so convert from mm/s to mm/min multiply by 60.
       - Skeleton

       ```gcode
       M566 X#max_instantaneous_change Y#max_instantaneous_change
       M500  ; if supported by your board
       ```

       Example (Duet-style):

       ```gcode
       M566 X3000 Y3000
       ```

> [!NOTE]
> RepRapFirmware exposes `M566` to set allowable instantaneous speed changes; some boards may persist settings with `M500` or via their web/config files.

## Credits

- **Junction Deviation Machine Limit** [@RF47](https://github.com/RF47)
- **Cornering Calibration** [@IanAlexis](https://github.com/IanAlexis)
- **Fast tower model** [@RF47](https://github.com/RF47)
- **SCV-V2 model** [@chrisheib](https://www.thingiverse.com/chrisheib)
