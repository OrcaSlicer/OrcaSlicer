# Patterns

Patterns determine how material is distributed within a print. Different patterns can affect strength, flexibility and print speed using the same density setting.  
The infill pattern also impacts the uniformity of the layer times, since the patterns may be constant, or present significant variations between adjacent layers.

There is no one-size-fits-all solution, as the best pattern depends on the specific print and its requirements.

Many patterns may look similar and have similar overall specifications, but they can behave very differently in practice.  
As most settings in 3D printing, experience is the best way to determine which pattern works best for your specific needs.

## Analysis parameters

### Strength

- **X-Y Direction**: The strength of the print in the "Horizontal" X-Y plane. Affected by the pattern's connections between walls, contact between layers, and path.
- **Z Direction**: The strength of the print in the "Vertical" Z direction. Affected by contact between layers.

### Material Usage

Not all patterns use the same amount of material due to their **Density Calculations** and adjustments to the paths.  
This leads to patterns that do not use the specified percentage but rather variations of it.

### Print Time

Print time can vary significantly between patterns due to differences in their pathing and infill strategies.  
Some patterns may complete faster due to more efficient use of the print head's movement, while others may take longer due to more complex paths.

> [!NOTE]
> OrcaSlicer Time estimations are not always accurate, especially with complex patterns.  
> This analysis was estimated with [Klipper Estimator](https://github.com/Annex-Engineering/klipper_estimator).

### Layer Time Variability

Layer time variability refers to the differences in time it takes to print each layer of a pattern. Some patterns may have consistent layer times, while others may experience significant fluctuations. These variations can potentially impact the outer appearance of the print due to differences in cooling and material flow between layers.

![fill-layer-time-variability](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/fill-layer-time-variability.png?raw=true)

## Patterns Quick Reference

| - | Pattern | Strength | Material Usage | Print Time | Layer time Variability |
|---|---|---|---|---|---|
| <img alt="param_monotonic" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_monotonic.svg?raw=true" height="45"> | [Monotonic](#monotonic) | X-Y: ⚪️ Normal<br> Z: ⚪️ Normal | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_monotonicline" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_monotonicline.svg?raw=true" height="45"> | [Monotonic line](#monotonic-line) | X-Y: ⚪️ Normal<br> Z: ⚪️ Normal | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_rectilinear" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_rectilinear.svg?raw=true" height="45"> | [Rectilinear](#rectilinear) | X-Y: ⚪️ Normal-Low<br> Z: 🟡 Low | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_alignedrectilinear" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_alignedrectilinear.svg?raw=true" height="45"> | [Aligned Rectilinear](#aligned-rectilinear) | X-Y: ⚪️ Normal-Low<br> Z: ⚪️ Normal | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_zigzag" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_zigzag.svg?raw=true" height="45"> | [Zig Zag](#zig-zag) | X-Y: ⚪️ Normal-Low<br> Z: 🟡 Low | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_crosszag" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_crosszag.svg?raw=true" height="45"> | [Cross Zag](#cross-zag) | X-Y: ⚪️ Normal<br> Z: 🟡 Low | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_lockedzag" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_lockedzag.svg?raw=true" height="45"> | [Locked Zag](#locked-zag) | X-Y: ⚪️ Normal-Low<br> Z: ⚪️ Normal-Low | ⚪️ Normal-High | ⚪️ Normal-High | 🟢 None |
| <img alt="param_line" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_line.svg?raw=true" height="45"> | [Line](#line) | X-Y: 🟡 Low<br> Z: 🟡 Low | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_grid" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_grid.svg?raw=true" height="45"> | [Grid](#grid) | X-Y: 🟣 High<br> Z: 🟣 High | ⚪️ Normal | 🟣 Low | 🟢 None |
| <img alt="param_triangles" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_triangles.svg?raw=true" height="45"> | [Triangles](#triangles) | X-Y: 🟣 High<br> Z: ⚪️ Normal | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_tri-hexagon" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_tri-hexagon.svg?raw=true" height="45"> | [Tri-hexagon](#tri-hexagon) | X-Y: 🟣 High<br> Z: 🔘 Normal-High | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_cubic" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_cubic.svg?raw=true" height="45"> | [Cubic](#cubic) | X-Y: 🟣 High<br> Z: 🟣 High | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_adaptivecubic" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_adaptivecubic.svg?raw=true" height="45"> | [Adaptive Cubic](#adaptive-cubic) | X-Y: 🔘 Normal-High<br> Z: 🔘 Normal-High | 🟣 Low | 🟣 Low | 🔵 Unnoticeable |
| <img alt="param_quartercubic" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_quartercubic.svg?raw=true" height="45"> | [Quarter Cubic](#quarter-cubic) | X-Y: 🟣 High<br> Z: 🟣 High | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_supportcubic" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_supportcubic.svg?raw=true" height="45"> | [Support Cubic](#support-cubic) | X-Y: 🟡 Low<br> Z: 🟡 Low | 🔵 Extra-Low | 🔵 Extra-Low | 🔴 Likely Noticeable |
| <img alt="param_lightning" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_lightning.svg?raw=true" height="45"> | [Lightning](#lightning) | X-Y: 🟡 Low<br> Z: 🟡 Low | 🟢 Ultra-Low | 🟢 Ultra-Low | 🔴 Likely Noticeable |
| <img alt="param_honeycomb" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_honeycomb.svg?raw=true" height="45"> | [Honeycomb](#honeycomb) | X-Y: 🟣 High<br> Z: 🟣 High | 🟡 High | 🔴 Ultra-High | 🟢 None |
| <img alt="param_3dhoneycomb" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_3dhoneycomb.svg?raw=true" height="45"> | [3D Honeycomb](#3d-honeycomb) | X-Y: 🔘 Normal-High<br> Z: 🔘 Normal-High | 🔘 Normal-Low | 🟠 Extra-High | 🟡 Possibly Noticeable |
| <img alt="param_lateral-honeycomb" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_lateral-honeycomb.svg?raw=true" height="45"> | [Lateral Honeycomb](#lateral-honeycomb) | X-Y: ⚪️ Normal-Low<br> Z: ⚪️ Normal-Low | ⚪️ Normal | 🔘 Normal-Low | 🟡 Possibly Noticeable |
| <img alt="param_lateral-lattice" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_lateral-lattice.svg?raw=true" height="45"> | [Lateral Lattice](#lateral-lattice) | X-Y: ⚪️ Normal-Low<br> Z: 🟡 Low | ⚪️ Normal | 🔘 Normal-Low | 🔵 Unnoticeable |
| <img alt="param_crosshatch" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_crosshatch.svg?raw=true" height="45"> | [Cross Hatch](#cross-hatch) | X-Y: 🔘 Normal-High<br> Z: 🔘 Normal-High | ⚪️ Normal | 🟡 High | 🔴 Likely Noticeable |
| <img alt="param_tpmsd" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_tpmsd.svg?raw=true" height="45"> | [TPMS-D](#tpms-d) | X-Y: 🟣 High<br> Z: 🟣 High | ⚪️ Normal | 🟡 High | 🟡 Possibly Noticeable |
| <img alt="param_tpmsfk" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_tpmsfk.svg?raw=true" height="45"> | [TPMS-FK](#tpms-fk) | X-Y: 🔘 Normal-High<br> Z: 🔘 Normal-High | ⚪️ Normal | 🔴 Ultra-High | 🟡 Possibly Noticeable |
| <img alt="param_gyroid" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_gyroid.svg?raw=true" height="45"> | [Gyroid](#gyroid) | X-Y: 🟣 High<br> Z: 🟣 High | ⚪️ Normal | 🔴 Ultra-High | 🔵 Unnoticeable |
| <img alt="param_concentric" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_concentric.svg?raw=true" height="45"> | [Concentric](#concentric) | X-Y: 🟡 Low<br> Z: ⚪️ Normal | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_hilbertcurve" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_hilbertcurve.svg?raw=true" height="45"> | [Hilbert Curve](#hilbert-curve) | X-Y: 🟡 Low<br> Z: ⚪️ Normal | ⚪️ Normal | 🟠 Extra-High | 🟢 None |
| <img alt="param_archimedeanchords" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_archimedeanchords.svg?raw=true" height="45"> | [Archimedean Chords](#archimedean-chords) | X-Y: 🟡 Low<br> Z: ⚪️ Normal | ⚪️ Normal | 🔘 Normal-Low | 🟢 None |
| <img alt="param_octagramspiral" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/param_octagramspiral.svg?raw=true" height="45"> | [Octagram Spiral](#octagram-spiral) | X-Y: 🟡 Low<br> Z: ⚪️ Normal | ⚪️ Normal | ⚪️ Normal | 🟢 None |

> [!NOTE]
> This estimations are based in a Cube model to maintain consistency.  
> This **WILL NOT** be the same for all models and only serves as a standard guideline.

> [!TIP]
> You can see how this analysis was made in [infill-analysis](https://github.com/OrcaSlicer/OrcaSlicer/tree/main/doc/print_settings/strength/infill-analysis) folder:
>  - [Infill calculator Project](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/print_settings/strength/infill-analysis/infill_calculator.3mf?raw=true) to generate the gcode files and images.
>  - [infill_desc_calculator.xlsx](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/print_settings/strength/infill-analysis/infill_desc_calculator.xlsx?raw=true) used to calculate the values above.  
>  - Time, and material usage where simulated with the same [Klipper Estimator](https://github.com/Annex-Engineering/klipper_estimator) values to maintain consistency.

## Monotonic

[Rectilinear](#rectilinear) in a uniform direction for a smoother visual surface.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**

![infill-top-monotonic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-monotonic.png?raw=true)

## Monotonic line

[Monotonic](#monotonic) but avoids overlapping with the perimeter, reducing excess material at joints. May introduce visible seams and increase print time.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**

![infill-top-monotonic-line](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-monotonic-line.png?raw=true)

## Rectilinear

Parallel lines spaced according to infill density. Each layer is printed perpendicular to the previous, resulting in low vertical bonding. Consider using new [Zig Zag](#zig-zag) infill instead.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal-Low
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**
  - **[Ironing](quality_settings_ironing)**

![infill-top-rectilinear](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-rectilinear.png?raw=true)

## Aligned Rectilinear

Parallel lines spaced by the infill spacing, each layer printed in the same direction as the previous layer. Good horizontal strength perpendicular to the lines, but terrible in parallel direction.
Recommended with layer anchoring to improve not perpendicular strength.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal-Low
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**

![infill-top-aligned-rectilinear](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-aligned-rectilinear.png?raw=true)

## Zig Zag

Similar to [rectilinear](#rectilinear) with consistent pattern between layers. Allows you to add a Symmetric infill Y axis for models with two symmetric parts.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal-Low
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-zig-zag](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-zig-zag.png?raw=true)

## Cross Zag

Similar to [Zig Zag](#zig-zag) but displacing each layer with Infill shift step parameter.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-cross-zag](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-cross-zag.png?raw=true)

## Locked Zag

Version of [Zig Zag](#zig-zag) that adds extra skin.
When using this fill, you can individually modify the density of the skeleton and skin, as well as the size of the skin and how much interconnection there is between the skin and the skeleton (a lock depth of 50% of the skin depth is recommended).

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal-Low
  - **Vertical (Z):** ⚪️ Normal-Low
- **Density Calculation:** Similar to [Zig Zag](#zig-zag).
Skin density * ( Infill Area - Skin Area + lock depth area) + ( Skin density * Skin area).
  - **Material Usage:** ⚪️ Normal-High
  - **Print Time:** ⚪️ Normal-High
    - **Material/Time (Higher better):** ⚪️ Normal  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-locked-zag](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-locked-zag.png?raw=true)

## Line

Similar to [rectilinear](#rectilinear), but each line is slightly rotated to improve print speed.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-line](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-line.png?raw=true)

## Grid

Two-layer pattern of perpendicular lines, forming a grid. Overlapping points may cause noise or artifacts.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🟣 High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🟣 Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-grid](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-grid.png?raw=true)

## Triangles

Triangle-based grid, offering strong X-Y strength but with triple overlaps at intersections.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-triangles](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-triangles.png?raw=true)

## Tri-hexagon

Similar to the [triangles](#triangles) pattern but offset to prevent triple overlaps at intersections. This design combines triangles and hexagons, providing excellent X-Y strength.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🔘 Normal-High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-tri-hexagon](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-tri-hexagon.png?raw=true)

## Cubic

3D cube pattern with corners facing down, distributing force in all directions. Triangles in the horizontal plane provide good X-Y strength.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🟣 High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-cubic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-cubic.png?raw=true)

## Adaptive Cubic

[Cubic](#cubic) pattern with adaptive density: denser near walls, sparser in the center. Saves material and time while maintaining strength, ideal for large prints.

- **Strength**
  - **Horizontal (X-Y):** 🔘 Normal-High
  - **Vertical (Z):** 🔘 Normal-High
- **Density Calculation:** Same as [Cubic](#cubic) but reduced in the center
  - **Material Usage:** 🟣 Low
  - **Print Time:** 🟣 Low
    - **Material/Time (Higher better):** ⚪️ Normal  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-adaptive-cubic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-adaptive-cubic.png?raw=true)

## Quarter Cubic

[Cubic](#cubic) pattern with extra internal divisions, improving X-Y strength.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🟣 High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-quarter-cubic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-quarter-cubic.png?raw=true)

## Support Cubic

Support |Cubic is a variation of the [Cubic](#cubic) infill pattern that is specifically designed for support top layers. Will use more material than Lightning infill but will provide better strength. Nevertheless, it is still a low-density infill pattern.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:** % of layer before top shell layers
  - **Material Usage:** 🔵 Extra-Low
  - **Print Time:** 🔵 Extra-Low
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🔴 Likely Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-support-cubic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-support-cubic.png?raw=true)

## Lightning

Ultra-fast, ultra-low material infill. Designed for speed and efficiency, ideal for quick prints or non-structural prototypes.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:** % of layer before top shell layers
  - **Material Usage:** 🟢 Ultra-Low
  - **Print Time:** 🟢 Ultra-Low
    - **Material/Time (Higher better):** ⚪️ Normal-Low  
    - **Layer time Variability:** 🔴 Likely Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-lightning](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-lightning.png?raw=true)

## Honeycomb

Hexagonal pattern balancing strength and material use. Double walls in each hexagon increase material consumption.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🟣 High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** 🟡 High
  - **Print Time:** 🔴 Ultra-High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-honeycomb](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-honeycomb.png?raw=true)

## 3D Honeycomb

This infill tries to generate a printable honeycomb structure by printing squares and octagons maintaining a vertical angle high enough to maintain contact with the previous layer.

- **Strength**
  - **Horizontal (X-Y):** 🔘 Normal-High
  - **Vertical (Z):** 🔘 Normal-High
- **Density Calculation:** Unknown
  - **Material Usage:** 🔘 Normal-Low
  - **Print Time:** 🟠 Extra-High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🟡 Possibly Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-3d-honeycomb](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-3d-honeycomb.png?raw=true)

## Lateral Honeycomb

Vertical Honeycomb pattern. Acceptable torsional stiffness. Developed for low densities structures like wings. Improve over [Lateral Lattice](#lateral-lattice) offers same performance with lower densities.This infill includes a Overhang angle parameter to improve the point of contact between layers and reduce the risk of delamination.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal-Low
  - **Vertical (Z):** ⚪️ Normal-Low
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟡 Possibly Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-lateral-honeycomb](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-lateral-honeycomb.png?raw=true)

## Lateral Lattice

Low-strength pattern with good flexibility. You can adjust **Angle 1** and **Angle 2** to optimize the infill for your specific model. Each angle adjusts the plane of each layer generated by the pattern. 0° is vertical.

- **Strength**
  - **Horizontal (X-Y):** ⚪️ Normal-Low
  - **Vertical (Z):** 🟡 Low
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-lateral-lattice](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-lateral-lattice.png?raw=true)

## Cross Hatch

Similar to [Gyroid](#gyroid) but with linear patterns, creating weak points at internal corners.
Easier to slice but consider using [TPMS-D](#tpms-d) or [Gyroid](#gyroid) for better strength and flexibility.

- **Strength**
  - **Horizontal (X-Y):** 🔘 Normal-High
  - **Vertical (Z):** 🔘 Normal-High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🟡 High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🔴 Likely Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-cross-hatch](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-cross-hatch.png?raw=true)

## TPMS-D

Triply Periodic Minimal Surface (Schwarz Diamond). Hybrid between [Cross Hatch](#cross-hatch) and [Gyroid](#gyroid), combining rigidity and smooth transitions. Isotropic and strong in all directions. This geometry is faster to slice than Gyroid, but slower than Cross Hatch.

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🟣 High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🟡 High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🟡 Possibly Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-tpms-d](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-tpms-d.png?raw=true)

## TPMS-FK

Triply Periodic Minimal Surface (Fischer–Koch S) pattern. Its smooth, continuous geometry resembles trabecular bone microstructure, offering a balance between rigidity and energy absorption. Compared to [TPMS-D](#tpms-d), it has more complex curvature, which can improve load distribution and shock absorption in functional parts.

- **Strength**
  - **Horizontal (X-Y):** 🔘 Normal-High
  - **Vertical (Z):** 🔘 Normal-High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔴 Ultra-High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🟡 Possibly Noticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-tpms-fk](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-tpms-fk.png?raw=true)

## Gyroid

Mathematical, isotropic surface providing equal strength in all directions. Excellent for strong, flexible prints and resin filling due to its interconnected structure. This pattern may require more time to slice because of all the points needed to generate each curve. If your model has complex geometry, consider using a simpler infill pattern like [TPMS-D](#tpms-d) or [Cross Hatch](#cross-hatch).

- **Strength**
  - **Horizontal (X-Y):** 🟣 High
  - **Vertical (Z):** 🟣 High
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔴 Ultra-High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🔵 Unnoticeable
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**

![infill-top-gyroid](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-gyroid.png?raw=true)

## Concentric

Fills the area with progressively smaller versions of the outer contour, creating a concentric pattern. Ideal for 100% infill or flexible prints.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**
  - **[Ironing](quality_settings_ironing)**

![infill-top-concentric](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-concentric.png?raw=true)

## Hilbert Curve

Hilbert Curve is a space-filling curve that can be used to create a continuous infill pattern. It is known for its aesthetic appeal and ability to fill space efficiently.
Print speed is very low due to the complexity of the path, which can lead to longer print times. It is not recommended for structural parts but can be used for aesthetic purposes.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🟠 Extra-High
    - **Material/Time (Higher better):** 🟡 Low  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**

![infill-top-hilbert-curve](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-hilbert-curve.png?raw=true)

## Archimedean Chords

Spiral pattern that fills the area with concentric arcs, creating a smooth and continuous infill. Can be filled with resin thanks to its interconnected hollow structure, which allows the resin to flow through it and cure properly.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** 🔘 Normal-Low
    - **Material/Time (Higher better):** 🔘 Normal-High  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**

![infill-top-archimedean-chords](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-archimedean-chords.png?raw=true)

## Octagram Spiral

Aesthetic pattern with low strength and high print time.

- **Strength**
  - **Horizontal (X-Y):** 🟡 Low
  - **Vertical (Z):** ⚪️ Normal
- **Density Calculation:**  % of  total infill volume
  - **Material Usage:** ⚪️ Normal
  - **Print Time:** ⚪️ Normal
    - **Material/Time (Higher better):** ⚪️ Normal  
    - **Layer time Variability:** 🟢 None
- **Applies to:**
  - **[Sparse Infill](strength_settings_infill#sparse-infill-density)**
  - **[Solid Infill](strength_settings_infill#internal-solid-infill)**
  - **[Surface](strength_settings_top_bottom_shells)**

![infill-top-octagram-spiral](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/fill/infill-top-octagram-spiral.png?raw=true)
