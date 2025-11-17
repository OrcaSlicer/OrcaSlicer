# Speed Advanced - Extrusion Rate Smoothing

Extrusion Rate Smoothing (ERS) (pressure equalizer in PrusaSlicer) aims to **limit the rate of extrusion volume change to be below a user-set threshold (the ERS value)**.  
It aims to assist the printer firmware internal motion planners, pressure advance in achieving the desired nozzle flow and reducing deviations against the ideal flow.

This happens by reducing the stresses put on the extrusion system as well as reducing the absolute deviations from the ideal extrusion flow caused by pressure advance smooth time.

This feature is especially helpful when printing at high accelerations and large flow rates as the deviations are larger in these cases.

![ers-intro](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/ERS/ers-intro.png?raw=true)

- [Theory](#theory)
  - [Acceleration vs. Extrusion rate smoothing](#acceleration-vs-extrusion-rate-smoothing)
  - [Pressure advance vs extrusion rate smoothing](#pressure-advance-vs-extrusion-rate-smoothing)
- [Finding the ideal Extrusion Rate smoothing value](#finding-the-ideal-extrusion-rate-smoothing-value)
- [A note for Bowden printers using Marlin without pressure advance](#a-note-for-bowden-printers-using-marlin-without-pressure-advance)
- [A note on ERS Segment length](#a-note-on-ers-segment-length)
- [Limitations](#limitations)
- [Credits](#credits)

## Theory

Enabling this feature creates a small **speed "ramp"** by slowing down and ramping up print speeds prior to and after the features causing a sudden change in extrusion flow rate needs, such as overhangs and overhang perimeters.

This works by breaking down the printed line segments into smaller "chunks", proportional to the ERS segment length, and reduces the print speed of these segments so that the **requested extrusion volumetric flow rate change is less than or equal to the ERS threshold**.

In summary, **it takes the "edge" off rapid extrusion changes caused by acceleration/deceleration as these are now spread over a longer distance and time.** Therefore, it can reduce wall artifacts that show when the print speeds change suddenly. These artifacts are occurring because the extruder and firmware cannot perfectly adhere to the flow rates requested by the slicer, especially when the extrusion rate is changing rapidly.

**The example below shows the artifact that is mitigated by ERS.**

![ers-artefact](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/ERS/ers-artefact.jpg?raw=true)

The bulging visible above is due to the extruder not being able to respond fast enough to the required speed change when printing with high accelerations and high speeds and being requested to slow down for an overhang.

In the above scenario, the printer (Bambu Lab X1 Carbon) was requested to slow down from a 200mm/sec print speed to 40mm/sec at an acceleration of 5k/sec². **The extruder could not keep up with the pressure change, resulting in a slight bump ahead at the point of speed change.**

This parameter interacts with the below printer kinematic settings and physical limits:

1. **The limits of the extruder system** - how fast can it change pressure in the nozzle
2. **The configured pressure advance values** - that also affect pressure changes in the nozzle
3. **The acceleration profile of the printer** - higher accelerations mean higher pressure changes
4. **The pressure advance smooth time (Klipper)** - higher smooth time means higher deviation from ideal extrusion, hence more opportunity for this feature to be useful.

### Acceleration vs. Extrusion rate smoothing

A printer's motion system does not exactly follow the speed changes seen in the G-code preview screen of OrcaSlicer.

When a speed change is requested, the firmware look-ahead planner calculates the slowdown needed to achieve the target speed. The rate of slowdown is limited by the move's acceleration value.

**Let's consider an example.** Assume printing an overhang wall with **2k external wall acceleration**, where the printer is called to slow down from **200mm/sec to 40mm/sec**.

This deceleration move would happen over approximately 9.6mm. This is derived from the following equation:

```math
d = \frac{v_f^2 - v_i^2}{2a}
```

Where:

- vf = final speed.
- vi = initial speed.
- a = acceleration (in this case, it will be negative as it's a deceleration).
- d = distance.

The time taken to decelerate to this new speed would be approx. 0.08 seconds, derived from the following equation:

```math
t = \frac{v_f - v_i}{a}
```

A printer printing at 200mm/sec with a 0.42 line width and 0.16 layer height would be extruding plastic at approx. 12.16mm³/sec, as can also be seen from the below visual.

![ers-printspeed](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/ERS/ers-printspeed.png?raw=true)

When the printer is extruding at 40mm/sec with the same line width and layer height as above, the flow rate is 2.43mm³/sec.

So what we are asking the extruder to do in this example is **slow down from 12.16mm³/sec flow to 2.43mm³/sec flow in 0.08 seconds** or an extrusion change rate of 121mm³/sec².

**This value is proportional to the acceleration of the printer. At 4k this value doubles, at 1k it is half and is independent of the speed of movement or starting and ending speeds.**

**This value is also proportional to the line width - double the line width will result in double the extrusion rate change and vice versa. Same for layer height.**

So, continuing with the worked example, a 2k acceleration produces an extrusion rate change ramp of 121mm³/sec². **Therefore, setting a value higher than this would not bring any benefit to the print quality as the motion system would slow down less aggressively based on its acceleration settings.**

**Therefore, the acceleration values act as a meaningful upper limit to this setting.** An indicative set of values has been provided later in this page.

### Pressure advance vs extrusion rate smoothing

Then we need to consider pressure advance and smooth time as factors that influence extrusion rate.

**Pressure Advance** adjusts the extruder's speed to account for the pressure changes inside the hot end's melt zone. When the print head moves and extrudes filament, there's a delay between the movement of the extruder gear and the plastic being extruded due to the compressibility of the molten plastic in the hot end. This delay can cause too much plastic to be extruded when the print head starts moving or not enough plastic when the print head stops, leading to issues like blobbing or under-extrusion.

**Pressure Advance Smooth time** helps to mitigate potential negative effects on print quality due to the rapid changes in extruder flow rate, which are controlled by the Pressure Advance algorithm. This parameter essentially adds a smoothing effect to the adjustments made by Pressure Advance, aiming to prevent sharp or sudden changes in the extrusion rate.

When Pressure Advance adjusts the extruder speed to compensate for the pressure build-up or reduction in the hot end, it can lead to abrupt changes in the flow rate. These abrupt changes can potentially cause issues like:

1. Extruder motor skipping,
2. Increased wear on the extruder gear and filament,
3. Visible artifacts on the print surface due to non-uniform extrusion.

The smooth time setting introduces a controlled delay over which the Pressure Advance adjustments are spread out. This results in a more gradual application or reduction of extrusion pressure, leading to smoother transitions in filament flow.

The trade-off is extrusion accuracy. There is a deviation between the requested extrusion amount and the actual extrusion amount due to this smoothing.

**1. Increasing Smooth Time:** Leads to more gradual changes in extrusion pressure. While this can reduce artifacts and stress on the extruder system, setting it too high may diminish the effectiveness of Pressure Advance, as the compensation becomes too delayed to counteract the pressure dynamics accurately.

**2. Decreasing Smooth Time:** Makes the Pressure Advance adjustments more immediate, which can improve the responsiveness of pressure compensation but may also reintroduce abrupt changes in flow rate, potentially leading to the issues mentioned above.

In essence, **pressure advance smooth time creates an intentional deviation from the ideal extruder rotation** and, therefore, extrusion amount, to allow the printer's extruder to perform within its mechanical limits. Typically, this value is set to 0.04sec, which means that when Pressure Advance adjusts the extruder's flow rate to compensate for changes in pressure within the hot end, these adjustments are spread out over a period of 0.04 seconds.

There is a great example of pressure advance smooth time induced deviations in [this Klipper forum post](https://klipper.discourse.group/t/pressure-advance-smooth-time-skews-pressure-advance/13451) that is worth a read to get more insight in this trade-off.

In the worked example above, **we need to set an Extrusion Rate smoothing value enough to decrease the error introduced by pressure advance smooth time against the produced output flow.** The lower the extrusion rate smoothing value, the lower the changes in flow over time hence the lower the absolute deviation from the ideal extrusion caused by the smooth time algorithm. However, going too low will result in a material decrease in overall print speed, as the print speed will be materially reduced to achieve low extrusion deviations between features, for no real benefit after a point.

**The best way to find the lower beneficial limit is through experimentation.** Print an object with sharp overhangs that are slowed down because of the overhang print speed settings and observe for extrusion inconsistencies.

## Finding the ideal Extrusion Rate smoothing value

**Firstly, this value needs to be lower than the extrusion rate changes resulting from the acceleration profile of the printer.** As, generally, the greatest impact is in external wall finish, use your external perimeter acceleration as a point of reference.

**Below are some approximate ERS values for 0.42 line width and 0.16 layer height.**

1. 30mm³/sec for 0.5k acceleration
2. 60.5mm³/sec for 1k acceleration
3. 121mm³/sec² for 2k acceleration
4. 242mm³/sec² for 4k acceleration

**Below are some approximate ERS values for 0.42 line width and 0.20 layer height.**

1. 38mm³/sec for 0.5k acceleration
2. 76mm³/sec for 1k acceleration
3. 150mm³/sec² for 2k acceleration
4. 300mm³/sec² for 4k acceleration

**Below are some approximate ERS values for 0.45 line width and 0.16 layer height.**

1. 32mm³/sec for 0.5k acceleration
2. 65mm³/sec for 1k acceleration
3. 129mm³/sec² for 2k acceleration
4. 260mm³/sec² for 4k acceleration

**So, your tuning starting point needs to be an ERS value that is less than this.** A good point experiment with test prints would be **a value of 60-80%** of the above maximum values. This will give some meaningful assistance to pressure advance, reducing the deviation introduced by pressure advance smooth time. The greater the smooth time, the greater the quality benefit will be.

Therefore, for a **0.42 line width and 0.16 layer height**, the below are a recommended set of starting ERS values

1. 18-25mm³/sec for 0.5k acceleration
2. 35-50mm³/sec for 1k acceleration
3. 70-100mm³/sec² for 2k acceleration
4. 145-200mm³/sec² for 4k acceleration

If you are printing with a 0.2 layer height, you can increase these values by 25% and similarly reduce if printing with lower.

**The second factor is your extruder's mechanical abilities.** Direct drive extruders with a good grip on the filament typically are more responsive to extrusion rate changes. Similarly with stiff filaments. So, a Bowden printer or when printing softer material like TPU or soft PLAs like polyterra there is more opportunity for the extruder to slip or deviate from the desired extrusion amount due to mechanical grip or material deformation or just delay in propagating the pressure changes (in a Bowden setup).

**The final factor is the deviation introduced by pressure advance smooth time**, or equivalents in closed source firmware. The higher this value the larger the extrusion deviation from ideal. If you are using a direct drive extruder, reduce this value to 0.02 in your klipper firmware before tuning ERS, as a lower value results in lower deviations to mitigate. Then proceed to experimentally tune ERS.

**So where does that leave us?**

Perform a test print with the above ERS settings as a starting point and adjust to your liking! If you notice bulging on sharp overhangs where speed changes occur, like the hull of the Benchy, reduce this value by 10% and try again.

If you're not noticing any artifacts, increase by 10%, but don't go over the maximum values recommended above because then this feature would have no effect on your print.

## A note for Bowden printers using Marlin without pressure advance

If your printer is not equipped with pressure advance and, especially, if you are using a Bowden setup, you don't have the benefit of pressure advance dynamically adjusting your flow.

In this special case, ERS will be doing all the heavy lifting that pressure advance would typically perform. In this scenario, a low value of 8-10mm³/sec is usually recommended, irrespective of your acceleration settings, to smooth out pressure changes in the extrusion system as much as possible without impacting print speed too much.

## A note on ERS Segment length

Ideally, you want this value set to 1 to allow for the largest number of steps between each speed transition. However, this may result in G-code that is too large, with too many commands sent to your MCU per second and it may not be able to keep up. It will also slow down the OrcaSlicer front end as the sliced model is more complex to render.

For Klipper printers, a segment length of 1 works OK as the RPI or similar have enough computational power to handle the G-code command volume.

Similarly, for a Bambu Lab printer, a segment length of 1 works well. **However, if you do notice your printer stuttering or stalling** (which may be the case with the lower-powered P1 series printers) **or getting "Timer too close" errors** in Klipper, **increase this value to 2 or 3**. This would reduce the effectiveness of the setting but will present a more manageable load to your printer.

## Limitations

**This feature can only work where speed changes are induced by the slicer** - for example, when transitioning from fast to slow print moves when printing overhangs, bridges and from printing internal features to external features and vice versa.

However, it will not affect extruder behavior when the printer is slowing down due to firmware commands - for example, when turning around corners.

In this case, the printer slows down and then accelerates independently of what the slicer has requested. In this case, the slicer is commanding a consistent speed; however, the printer is adjusting this to operate within its printer kinematic limits (SCV/Jerk) and accelerations. As the slicer is not aware of this slowdown, it cannot apply pre-emptive extrusion rate smoothing to the feature and instead, the changes are governed by the printer firmware exclusively.

## Credits

- **Original feature authors and creators:** The PrusaSlicer team, including [@bubnikv](https://github.com/bubnikv), [@hejllukas](https://github.com/hejllukas).
- **Enhanced by:** [@MGunlogson](https://github.com/MGunlogson), introducing the feature to external perimeters, enhancing it by taking into account travel, retraction and implementing near-contiguous extrusions pressure equalizer adjustments.
- **Ported to Orca:** [@igiannakas](https://github.com/igiannakas).
- **Enhanced by:** [@noisyfox](https://github.com/Noisyfox), per object pressure equalization and fixing calculation logic bugs.
- **Wiki page:** [@igiannakas](https://github.com/igiannakas).
- **Overall Orca owner and assurance:** [@softfever](https://github.com/SoftFever).
- **Community testing and feedback:** [@HakunMatat4](https://github.com/HakunMatat4), [@psiberfunk](https://github.com/psiberfunk), [@u3dreal](https://github.com/u3dreal) and more.
