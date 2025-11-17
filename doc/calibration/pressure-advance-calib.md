# Pressure Advance

Pressure Advance is a feature that compensates for the lag in filament pressure within the nozzle during acceleration and deceleration. It helps improve print quality by reducing issues like blobs, oozing, and inconsistent extrusion, especially at corners or during fast movements.

OrcaSlicer includes three approaches for calibrating the Pressure Advance value. Each method has its own advantages and disadvantages. It is important to note that each method has two versions: one for a direct-drive extruder and one for a Bowden extruder. Make sure to select the appropriate version for your test.

> [!WARNING]
> **Marlin Printers:** Linear Advance must be enabled in firmware (M900).  
> **Not all printers have it enabled by default.**

> [!WARNING]
> **Bambulab Printers:** make sure you do not select the 'Flow calibration' option.
> ![flowrate-Bambulab-uncheck](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Flow-Rate/flowrate-Bambulab-uncheck.png?raw=true)

- [Calibration](#calibration)
  - [Tower method](#tower-method)
  - [Pattern method](#pattern-method)
  - [Line method](#line-method)

## Calibration

You can use different methods to calibrate the Pressure Advance value, each with its own advantages and disadvantages.

The results from these methods should be saved to the material profile.  
![pressure_advance_enable](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pressure_advance_enable.png?raw=true)

> [!TIP]
> Consider using the [Adaptive Pressure Advance](adaptive-pressure-advance-calib) method for more accurate results.  
> Especially for high-speed printers.

### Tower method

The tower method may take a bit more time to complete, but it does not rely on the quality of the first layer.

1. Select the printer, filament, and process you would like to use for the test.
2. Examine each corner of the print and mark the height that yields the best overall result.
3. In this example a height of 8 mm was selected, so the Pressure Advance value should be calculated as `PressureAdvanceStart + (PressureAdvanceStep x measured)`; example: `0 + (0.002 x 8) = 0.016`.
   ![pa-tower](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-tower.jpg?raw=true)
   ![pa-tower-measure](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-tower-measure.jpg?raw=true)

> [!TIP]
> @ItsDeidara has made an HTML tool to help with the calculation. Check it out if those equations give you a headache [here](https://github.com/ItsDeidara/Orca-Slicer-Assistant).

### Pattern method

The pattern method is adapted from [Andrew Ellis' pattern method generator](https://ellis3dp.com/Pressure_Linear_Advance_Tool/), which was itself derived from the [Marlin pattern method](https://marlinfw.org/tools/lin_advance/k-factor.html) developed by [Sineos](https://github.com/Sineos/k-factorjs).

[Instructions for using and reading the pattern method](https://ellis3dp.com/Print-Tuning-Guide/articles/pressure_linear_advance/pattern_method.html) are provided in [Ellis' Print Tuning Guide](https://ellis3dp.com/Print-Tuning-Guide/), with only a few OrcaSlicer differences to note.

The test configuration window allows the user to generate one or more tests in a single project. Multiple tests will be placed on the plate with extra plates added if needed.

1. Single test \
   ![pa-pattern-single](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-pattern-single.png?raw=true)
2. Batch mode testing (multiple tests on a single plate) \
   ![pa-pattern-batch](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-pattern-batch.png?raw=true)

Once a test is generated, one or more small rectangular prisms will be placed on the plate, one for each test case. The prism object serves a few purposes:

1. The test pattern itself is added in as custom G-Code at each layer, same as you could do by hand. The rectangular prism provides the layers in which to insert that G-Code. This also means that **you'll see the full test pattern when you move to the Preview pane:**

![pa-pattern-batch-plater](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-pattern-batch-plater.png?raw=true)

2. The prism acts as a handle, enabling you to move the test pattern wherever you'd like on the plate by moving the prism.
3. Each test object is pre-configured with target parameters which are reflected in the object's name. Test parameters may be adjusted for each prism individually via the object list pane:

![pa-pattern-batch-objects](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-pattern-batch-objects.png?raw=true)

Next, Ellis' generator provided the ability to adjust specific printer, filament, and print profile settings. You can make these same changes in OrcaSlicer by adjusting the settings in the Prepare pane as you would with any other print. When you initiate the calibration test, Ellis' default settings are applied. A few things to note about these settings:

1. Ellis specified line widths as a percent of filament diameter. The Orca pattern method does the same to provide its suggested defaults, making use of Ellis' percentages in combination with your specified nozzle diameter.
2. In terms of line width, the pattern only makes use of the `Default` and `First layer` widths.
3. In terms of speed, the pattern only uses the `First layer speed -> First layer` and `Other layers speed -> Outer wall` speeds.
4. The infill pattern beneath the numbers cannot be changed because it's not actually an infill pattern pulled from the settings. All of the pattern G-Code is custom written, so that "infill" is, effectively, hand-drawn and not processed through the usual channels that would enable Orca to recognize it as infill.

### Line method

The line method is quick and straightforward to test. However, its accuracy depends heavily on the quality of your first layer. It is suggested to turn on bed mesh leveling for this test.

Steps:

1. Select the printer, filament, and process you would like to use for the test.
2. Print the project and check the result. Choose the value corresponding to the most even line and update your Pressure Advance value in the filament settings.
3. In this test, a Pressure Advance value of `0.016` appears to be optimal.

   ![pa-line](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-line.gif?raw=true)

   ![pa-lines](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-lines.png?raw=true)

   ![pa-line-0-016](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-line-0-016.png?raw=true)
