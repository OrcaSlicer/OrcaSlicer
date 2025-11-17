# Adaptive Pressure Advance

This feature aims to dynamically adjust the printer’s pressure advance to better match the conditions the toolhead is facing during a print. Specifically, to more closely align to the ideal values as flow rate, acceleration, and bridges are encountered.  
This wiki page aims to explain how this feature works, the prerequisites required to get the most out of it as well as how to calibrate it and set it up.

## Settings Overview

This feature introduces the below options under the filament settings:

1. **Enable adaptive pressure advance:** This is the on/off setting switch for adaptive pressure advance.
2. **Enable adaptive pressure advance for overhangs:** Enable adaptive PA for overhangs as well as when flow changes within the same feature. This is an experimental option because if the PA profile is not set accurately, it will cause uniformity issues on the external surfaces before and after overhangs. It is recommended to start with this option switched off and enable it after the core adaptive pressure advance feature is calibrated correctly.
3. **Pressure advance for bridges:** Sets the desired pressure advance value for bridges. Set it to 0 to disable this feature. Experiments have shown that a lower PA value when printing bridges helps reduce the appearance of slight under extrusion immediately after a bridge, which is caused by the pressure drop in the nozzle when printing in the air. Therefore, a lower pressure advance value helps counteract this. A good starting point is approximately half your usual PA value.
4. **Adaptive pressure advance measurements:** This field contains the calibration values used to generate the pressure advance profile for the nozzle/printer. Input sets of pressure advance (PA) values and the corresponding volumetric flow speeds and accelerations they were measured at, separated by a comma. Add one set of values per line. More information on how to calibrate the model follows in the sections below.
5. **Pressure advance:** The old field is still needed and is required to be populated with a PA value. A “good enough” median PA value should be entered here, as this will act as a fallback value when performing tool changes, printing a purge/wipe tower for multi-color prints as well as a fallback in case the model fails to identify an appropriate value (unlikely but it’s the ultimate backstop).  
![apa-material-config](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-material-config.png?raw=true)

## Pre-Requisites

This feature has been tested with Klipper-based printers. While it may work with Marlin or Bambu lab printers, it is currently untested with them. It shouldn't adversely affect the machine; however, the quality results from enabling it are not validated.

> [!IMPORTANT]
> Versions of Klipper older than July 11th, 2024 might in some cases produce artifacts when dynamic PA is used.  
> This has been fixed in Klipper stable version 0.13.0 (20250411), or in developer versions starting [from July 11th, 2024](https://github.com/Klipper3d/klipper/commit/c84d78f3f169bc5163d11b74837f9880b0b7dba4).  
> If you experience issues or print quality lower than expected you might want to ensure you have a Klipper firmware which contains the fix.

> [!TIP]
> **Advanced users**: the fix to ensure optimal quality when dynamic PA is used does not require flashing the toolhead MCU or the printer MCU, but only an update of the host Python code. If you are using a commercial printer which does not offer straightforward MCU flashing (for example, Qidi printers), you might be able to manually patch the host code yourself.

## Use case (what to expect)

Following experimentation, it has been noticed that the optimal pressure advance value is less:

1. The faster you print (hence the higher the volumetric flow rate requested from the toolhead).
2. The larger the layer height (hence the higher the volumetric flow rate requested from the toolhead).
3. The higher the print acceleration is.

What this means is that we never get ideal PA values for each print feature, especially when they vary drastically in speed and acceleration. We can tune PA for a faster print speed (flow) but compromise on corner sharpness for slower speeds or tune PA for corner sharpness and deal with slight corner-perimeter separation in faster speeds. The same goes for accelerations as well as different layer heights.

This compromise usually means that we settle for tuning an "in-between" PA value between slower external features and faster internal features so we don't get gaps, but also not get too much bulging in external perimeters.

**However, what this also means is that if you are printing with a single layer height, single speed, and acceleration, there is no need to enable this feature.**

Adaptive pressure advance aims to address this limitation by implementing a completely different method of setting pressure advance. **Following a set of PA calibration tests done at different flow rates (speeds and layer heights) and accelerations, a pressure advance model is calculated by the slicer.** Then that model is used to emit the best fit PA for any arbitrary feature flow rate (speed) and acceleration used in the print process.

In addition, it means that you only need to tune this feature once and print across different layer heights with good PA performance.

Finally, if during calibration you notice that there is little to no variance between the PA tests, this feature is redundant for you. **From experiments, high flow nozzles fitted on high-speed core XY printers appear to benefit the most from this feature as they print with a larger range of flow rates and at a larger range of accelerations.**

### Expected results

With this feature enabled there should be absolutely no bulge in the corners, just the smooth rounding caused by the square corner velocity of your printer.  
![apa-expected-results](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-expected-results.jpg?raw=true)

In addition, seams should appear smooth with no bulging or under extrusion.  
![apa-expected-seam](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-expected-seam.jpg?raw=true)

Solid infill should have no gaps, pinholes, or separation from the perimeters.  
![apa-expected-solid-infill](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-expected-solid-infill.jpg?raw=true)

Compared to with this feature disabled, where the internal solid infill and external-internal perimeters show signs of separation and under extrusion, when PA is tuned for optimal external perimeter performance as shown below.  
![apa-unexpected-solid-infill](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-unexpected-solid-infill.jpg?raw=true)

## How to calibrate the adaptive pressure advance model

### Defining the calibration sets

Firstly, it is important to understand your printer speed and acceleration limits in order to set meaningful boundaries for the calibrations:

1. **Upper acceleration range:** Do not attempt to calibrate adaptive PA for an acceleration that is larger than what the Klipper input shaper calibration tool recommends for your selected shaper. For example, if Klipper recommends an EI shaper with 4k maximum acceleration for your slowest axis (usually the Y axis), don’t calibrate adaptive PA beyond that value. This is because after 4k the input shaper smoothing is magnified and the perimeter separations that appear like PA issues are caused by the input shaper smoothing the shape of the corner. Basically, you’d be attempting to compensate for an input shaper artefact with PA.
2. **Upper print speed range:** The Ellis PA pattern test has been proven to be the most efficient and effective test to run to calibrate adaptive PA. It is fast and allows for a reasonably accurate and easy-to-read PA value. However, the size of the line segments is quite small, which means that for the faster print speeds and slower accelerations, the toolhead will not be able to reach the full flow rate that we are calibrating against. It is therefore generally not recommended to attempt calibration with a print speed of higher than ~200-250mm/sec and accelerations slower than 1k in the PA pattern test. If your lowest acceleration is higher than 1k, then proportionally higher maximum print speeds can be used.

**Remember:** With the calibration process, we aim to create a PA – Flow Rate – Acceleration profile for the toolhead. As we cannot directly control flow rate, we use print speed as a proxy (higher speed -> higher flow).

With the above in mind, let’s create a worked example to identify the optimal number of PA tests to calibrate the adaptive PA model.

**The below starting points are recommended for the majority of Core XY printers:**

1. **Accelerations:** 1k, 2k, 4k
2. **Print speeds:** 50mm/sec, 100mm/sec, 150mm/sec, 200mm/sec.

**That means we need to run 3x4 = 12 PA tests and identify the optimal PA for them.**

Finally, if the maximum acceleration given by input shaper is materially higher than 4k, run a set of tests with the higher accelerations. For example, if input shaper allows a 6k value, run PA tests as below:

1. **Accelerations:** 1k, 2k, 4k, 6k
2. **Print speeds:** 50mm/sec, 100mm/sec, 150mm/sec, 200mm/sec.

Similarly, if the maximum value recommended is 12k, run PA tests as below:

1. **Accelerations:** 1k, 2k, 4k, 8k, 12k
2. **Print speeds:** 50mm/sec, 100mm/sec, 150mm/sec, 200mm/sec.

So, at worst case you will need to run 5x4 = 20 PA tests if your printer acceleration is on the upper end! In essence, you want enough granularity of data points to create a meaningful model while also not overdoing it with the number of tests. So, doubling the speed and acceleration is a good compromise to arrive at the optimal number of tests.  
For this example, let’s assume that the baseline number of tests is adequate for your printer:

1. **Accelerations:** 1k, 2k, 4k
2. **Print speeds:** 50mm/sec, 100mm/sec, 150mm/sec, 200mm/sec.

We, therefore, need to run 12 PA tests as below:

| Speed | Acceleration |
|-------|--------------|
| 50    | 1k           |
| 100   | 1k           |
| 150   | 1k           |
| 200   | 1k           |
| 50    | 2k           |
| 100   | 2k           |
| 150   | 2k           |
| 200   | 2k           |
| 50    | 4k           |
| 100   | 4k           |
| 150   | 4k           |
| 200   | 4k           |

### Identifying the flow rates from the print speed

#### OrcaSlicer 2.2.0 and later

Test parameters needed to build adaptive PA table are printed on the test sample:

![apa-test](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-test.png?raw=true)

Test sample above was done with acceleration 12000 mm/s² and flow rate 27.13 mm³/s

#### OrcaSlicer 2.1.0 and older

As mentioned earlier, **the print speed is used as a proxy to vary the extrusion flow rate**. Once your PA test is set up, change the gcode preview to “flow” and move the horizontal slider over one of the herringbone patterns and take note of the flow rate for different speeds.

![apa-test210](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-test210.png?raw=true)

### Running the tests

#### General tips

It is recommended that the PA step is set to a small value, to allow you to make meaningful distinctions between the different tests – **therefore a PA step value of 0.001 is recommended**.

**Set the end PA to a value high enough to start showing perimeter separation for the lowest flow (print speed) and acceleration test.** For example, for a Voron 350 using Revo HF, the maximum value was set to 0.05 as that was sufficient to show perimeter separation even at the slowest flow rates and accelerations.

**If the test is too big to fit on the build plate, increase your starting PA value or the PA step value accordingly until the test can fit.** If the lowest value becomes too high and there is no ideal PA present in the test, focus on increasing the PA step value to reduce the number of herringbones printed (hence the size of the print).

![pa-pattern-general](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-pattern-general.png?raw=true)

#### OrcaSlicer 2.3.0 and newer

PA pattern calibration configuration window have been changed to simplify test setup. Now all is needed is to fill list of accelerations and speeds into relevant fields of the calibration window:

![pa-pattern-batch](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/pa-pattern-batch.png?raw=true)

Test patterns generated for each acceleration-speed pair and all parameters are set accordingly. No additional actions needed from user side. Just slice and print all plates generated.

Refer to [Calibration Guide](Calibration) for more details on batch mode calibration.

#### OrcaSlicer 2.2.0 and older

Setup your PA test as usual from the calibration menu in OrcaSlicer. Once setup, your PA test should look like the below:

![apa-setup-result-speed](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-setup-result-speed.png?raw=true)

![apa-setup-result-acceleration-jerk](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-setup-result-acceleration-jerk.png?raw=true)

Now input your identified print speeds and accelerations in the fields above and run the PA tests.

> [!IMPORTANT]
> Make sure your acceleration values are all the same in all text boxes. Same for the print speed values and Jerk (XY) values. Make sure your Jerk value is set to the external perimeter jerk used in your print profiles.

#### Test results processing

Now run the tests and note the optimal PA value, the flow, and the acceleration. You should produce a table like this:

| Speed | Flow  | Acceleration | PA    | Model values         |
|-------|-------|--------------|-------|----------------------|
|    50 |  3.84 |         1000 | 0.036 | 0.036 , 3.84 , 1000  |
|   100 |  7.68 |         1000 | 0.036 | 0.036 , 7.68 , 1000  |
|   150 | 11.51 |         1000 | 0.036 | 0.036 , 11.51 , 1000 |
|   200 | 15.35 |         1000 | 0.036 | 0.036 , 15.35 , 1000 |
|       |       |              |       |                      |
|    50 |  3.84 |         2000 | 0.036 | 0.036 , 3.84 , 2000  |
|   100 |  7.68 |         2000 |  0.03 | 0.03 , 7.68 , 2000   |
|   150 | 11.51 |         2000 | 0.029 | 0.029 , 11.51 , 2000 |
|   200 | 15.35 |         2000 | 0.028 | 0.028 , 15.35 , 2000 |
|       |       |              |       |                      |
|    50 |  3.84 |         4000 | 0.032 | 0.032 , 3.84 , 4000  |
|   100 |  7.68 |         4000 | 0.028 | 0.028 , 7.68 , 4000  |
|   150 | 11.51 |         4000 | 0.026 | 0.026 , 11.51 , 4000 |
|   200 | 15.35 |         4000 | 0.024 | 0.024 , 15.35 , 4000 |

Concatenate the PA value, the flow value, and the acceleration value into the final comma-separated sets to create the values entered in the model as shown above.

**You’re now done! The PA profile is created and calibrated!**

Remember to paste the values in the adaptive pressure advance measurements text box as shown below, and save your filament profile.

![apa-profile](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-profile.png?raw=true)

### Tips

#### Model input

The adaptive PA model built into the slicer is flexible enough to allow for as many or as few increments of flow and acceleration as you want. Ideally, you want at a minimum 3x data points for acceleration and flow in order to create a meaningful model.

However, if you don’t want to calibrate for flow, just run the acceleration tests and leave flow the same for each test (in which case you’ll input only 3 rows in the model text box). In this case, flow will be ignored when the model is used.

Similarly for acceleration – in the above example you’ll input only 4 rows in the model text box, in which case acceleration will be ignored when the model is used.

**However, make sure a triplet of values is always provided – PA value, Flow, Acceleration.**

#### Identifying the right PA

Higher acceleration and higher flow rate PA tests are easier to identify the optimal PA as the range of “good” values is much narrower. It’s evident where the PA is too large, as gaps start to appear in the corner and where PA is too low, as the corner starts bulging.

However, the lower the flow rate and accelerations are, the range of good values is much wider. Having examined the PA tests even under a microscope, what is evident, is that if you can’t distinguish a value as being evidently better than another (i.e. sharper corner with no gaps) with the naked eye, then both values are correct. In which case, if you can’t find any meaningful difference, simply use the optimal values from the higher flow rates.

- **Too high PA**
![apa-identify-too-high](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-identify-too-high.jpg?raw=true)

- **Too low PA**
![apa-identify-too-low](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-identify-too-low.jpg?raw=true)

- **Optimal PA**
![apa-identify-optimal](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/pa/apa-identify-optimal.jpg?raw=true)
