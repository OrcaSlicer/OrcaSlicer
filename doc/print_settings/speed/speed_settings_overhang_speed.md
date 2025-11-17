# Overhang Speed

- [Slow down for overhang](#slow-down-for-overhang)
  - [Slow down for curled perimeters](#slow-down-for-curled-perimeters)
  - [Speed](#speed)
- [Bridge speed](#bridge-speed)

## Slow down for overhang

Enable this option to slow printing down for different overhang degree.
This can help improve print quality and reduce issues like stringing or sagging.

### Slow down for curled perimeters

Enable this option to slow down printing in areas where perimeters may have curled upwards. For example, additional slowdown will be applied when printing overhangs on sharp corners like the front of the Benchy hull, reducing curling which compounds over multiple layers.  

![slow-down-for-curled-perimeters](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/speed/slow-down-for-curled-perimeters.png?raw=true)

It is generally recommended to have this option switched on unless your printer cooling is powerful enough or the print speed slow enough that perimeter curling does not happen. If printing with a high external perimeter speed, this parameter may introduce slight artifacts when slowing down due to the large variance in print speeds. If you notice artifacts, ensure your pressure advance is tuned correctly.  

> [!NOTE]
> When this option is enabled, overhang perimeters are treated like overhangs, meaning the overhang speed is applied even if the overhanging perimeter is part of a bridge. For example, when the perimeters are 100% overhanging, with no wall supporting them from underneath, the 100% overhang speed will be applied.

### Speed

This is the speed for various overhang degrees. Overhang degrees are expressed as a percentage of [line width](quality_settings_line_width).  

> [!NOTE]
> 0 speed means no slowing down for the overhang degree range and wall speed is used.

## Bridge speed

Set speed for external and internal bridges.  
It's usually recommended to increase internal bridge speed to reduce print time, while external bridge speed should be reduced to improve print quality.
