# Jerk XY

**Jerk** is the rate of change of acceleration and how quickly your printer can change between different accelerations. It controls direction changes and velocity transitions during movement.

## Cornering Control Types

- **Jerk**: Traditional method, sets a maximum speed for direction changes.
  - Klipper: [Square corner velocity](https://www.klipper3d.org/Config_Reference.html#printer)
  - RepRapFirmware: [Maximum instantaneous speed changes](https://docs.duet3d.com/User_manual/Reference/Gcodes#m566-set-allowable-instantaneous-speed-change)
  - Marlin 2: [Classic Jerk](https://marlinfw.org/docs/configuration/configuration.html#jerk-) (deprecated in favor of [Junction Deviation](https://marlinfw.org/docs/configuration/configuration.html#junction-deviation-)) but can still be used.
  - Marlin Legacy: [Classic Jerk](https://marlinfw.org/docs/configuration/configuration.html#jerk-).
- **[Junction Deviation](#junction-deviation)**: Modern method, calculates cornering speed based on acceleration and speed.

> [!TIP]
> Calibrate your Cornering Values using the [Cornering Calibration guide](cornering-calib).

## Key Effects

- **Corner Control**: Lower values = smoother corners, better quality. Higher values = faster cornering, potential artifacts
- **Print Speed**: Higher jerk reduces deceleration at direction changes, increasing overall speed
- **Surface Quality**: Lower jerk minimizes vibrations and ringing, especially important for outer walls

This setting overrides firmware jerk values when different motion types need specific settings. Orca limits jerk to not exceed the Printer's Motion Ability settings.

> [!TIP]
> Jerk can work in conjunction with [Pressure Advance](pressure-advance-calib), [Adaptive Pressure Advance](adaptive-pressure-advance-calib), and [Input Shaping](input-shaping-calib) to optimize print quality and speed.  
> It's recommended to follow the [calibration guide](calibration) order for best results.

- [Cornering Control Types](#cornering-control-types)
- [Key Effects](#key-effects)
- [Default](#default)
  - [Outer wall](#outer-wall)
  - [Inner wall](#inner-wall)
  - [Infill](#infill)
  - [Top surface](#top-surface)
  - [Initial layer](#initial-layer)
  - [Travel](#travel)
- [Junction Deviation](#junction-deviation)
- [Useful links](#useful-links)

## Default

Default Jerk value.

> [!NOTE]
> If this value is set to 0, the jerk will be set to the printer's default jerk.

### Outer wall

Jerk for outer wall printing. This is usually set to a lower value than normal printing to ensure better quality.

### Inner wall

Jerk for inner wall printing. This is usually set to a higher but still reasonable value than outer wall printing to improve speed.

### Infill

Jerk for infill printing. This is usually set to a value higher than inner wall printing to improve speed.

### Top surface

Jerk for top surface printing. This is usually set to a lower value than infill to ensure better quality.

### Initial layer

Jerk for initial layer printing. This is usually set to a lower value than top surface to improve adhesion.

### Travel

Jerk for travel printing. This is usually set to a higher value than infill to reduce travel time.

## Junction Deviation

Alternative to Jerk, Junction Deviation is the default method for controlling cornering speed in Marlin 2 printers.  
Higher values result in more aggressive cornering speeds, while lower values produce smoother, more controlled cornering.

> [!NOTE]
> Classic Jerk can still be used in Marlin 2, but it is deprecated in favor of Junction Deviation.  
> If your printer uses Classic Jerk, you need to set your Junction Deviation to `0` to enable the use of Classic Jerk.

This value will **only be overwritten** if it is lower than the Junction Deviation value set in Printer settings > Motion ability. If it is higher, the value configured in Motion ability will be used.

```math
JD = 0,4 \cdot \frac{\text{Jerk}^2}{\text{Accel.}}
```

## Useful links

- [Klipper Kinematics](https://www.klipper3d.org/Kinematics.html?h=accelerat#acceleration)
- [Marlin Junction Deviation](https://marlinfw.org/docs/configuration/configuration.html#junction-deviation-)
- [JD Explained and Visualized, by Paul Wanamaker](https://reprap.org/forum/read.php?1,739819)
- [Computing JD for Marlin Firmware](https://blog.kyneticcnc.com/2018/10/computing-junction-deviation-for-marlin.html)
- [Improving GRBL: Cornering Algorithm](https://onehossshay.wordpress.com/2011/09/24/improving_grbl_cornering_algorithm/)
- [Pressure Advance Calibration](pressure-advance-calib)
- [Adaptive Pressure Advance](adaptive-pressure-advance-calib)
