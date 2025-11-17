# VFA

Vertical Fine Artifacts (VFA) are small surface imperfections that appear on vertical walls, especially near sharp corners or sudden directional changes. These artifacts are typically caused by mechanical vibrations, motor resonance, or rapid directional shifts that impact print quality.

- **Mechanical adjustments**, such as tuning or replacing motors, belts, or pulleys.
- **MMR (Motor Resonance Rippling)** is a common subtype of VFA caused by stepper motors vibrating at resonant frequencies, leading to periodic ripples on the surface.
- **[Jerk/Junction Deviation](cornering-calib)** settings can also contribute to VFA, as they control how the printer handles rapid changes in direction.
- **[Input Shaping](input-shaping-calib)** can help mitigate VFA by reducing vibrations during printing.

## VFA Test

The VFA Speed Test in OrcaSlicer helps identify which print speeds trigger MRR artifacts. It prints a vertical tower with walls at various angles while progressively increasing the print speed.

![vfa_test_menu](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/vfa/vfa_test_menu.png?raw=true)

![vfa_test_print](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/vfa/vfa_test_print.jpg?raw=true)

After printing, inspect the tower for MRR artifacts. Look for speeds where the surface becomes visibly smoother or rougher. This allows you to pinpoint problematic speed ranges.

You can then configure the **Resonance Avoidance Speed Range** in the printer profile to skip speeds that cause visible artifacts.

![vfa_resonance_avoidance](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/vfa/vfa_resonance_avoidance.png?raw=true)
