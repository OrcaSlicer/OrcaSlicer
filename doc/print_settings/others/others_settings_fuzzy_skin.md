# Fuzzy Skin

Fuzzy skin randomly perturbs the wall path to produce a deliberately rough, matte appearance on the model surface.  
These settings control where the effect is applied, how the noise is generated, and how aggressive the displacement or extrusion modulation is.

Useful for creating a textures or hide surface imperfections but will increase print time and will affect dimensional accuracy.

- [Fuzzy Skin Mode](#fuzzy-skin-mode)
  - [Contour](#contour)
  - [Contour and Hole](#contour-and-hole)
  - [All Walls](#all-walls)
  - [Fuzzy Skin Generator Mode](#fuzzy-skin-generator-mode)
  - [Displacement](#displacement)
  - [Extrusion](#extrusion)
  - [Combined](#combined)
- [Noise Type](#noise-type)
  - [Classic](#classic)
  - [Perlin](#perlin)
  - [Billow](#billow)
  - [Ridged Multifractal](#ridged-multifractal)
  - [Voronoi](#voronoi)
- [Point distance](#point-distance)
- [Skin thickness](#skin-thickness)
- [Skin feature size](#skin-feature-size)
- [Skin Noise Octaves](#skin-noise-octaves)
- [Skin Noise Persistence](#skin-noise-persistence)
- [Apply fuzzy skin to first layer](#apply-fuzzy-skin-to-first-layer)
- [Credits](#credits)

## Fuzzy Skin Mode

Choose which parts of the model receive the fuzzy-skin effect.

### Contour

Apply fuzzy skin only to the outermost contour (external perimeter) of the model.  
Useful for creating a textured edge while keeping the inner surfaces smooth.

### Contour and Hole

Apply fuzzy skin to both the outer contour and interior holes. Useful when you want the rough texture to appear on negative features as well.

### All Walls

Apply fuzzy skin to every wall (external and internal). This gives the strongest overall textured appearance but will increase slicing and print time considerably.

### Fuzzy Skin Generator Mode

Select the underlying method used to produce the fuzzy effect. Each mode has different trade-offs for strength, speed and mechanical load.

### Displacement

![Fuzzy-skin-Displacement-mode](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-Displacement-mode.png?raw=true)

The classic method is when the pattern on the walls is achieved by shifting the printhead perpendicular to the wall.  
It gives a predictable result, but decreases the strength entire shells and open the pores inside the walls. It also increases the mechanical stress on the kinematics of the printer. The speed of general printing is slowing down.

### Extrusion

![Fuzzy-skin-Extrusion-mode](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-Extrusion-mode.png?raw=true)

The fuzzy skin condition is obtained by changing the amount of extruded plastic as the print head moves linearly. There is no extra load on the kinematics, there is no decrease in the printing speed, the pores do not open, but the drawing turns out to be smoother by a factor of 2. It is suitable for creating "loose" walls to reduce internal stress into extruded plastic, or masking printing defects on the side walls - a matte effect.

> [!CAUTION]
> The "Fuzzy skin thicknesses" parameter cannot be more than about 70%-125% (selected individually for different conditions) of the nozzle diameter! This is a complex condition that also depends on the height of the layer, and determines how thin the lines can be extruded.  
> [Arachne](quality_settings_wall_generator#arachne) wall generator mode should also be enabled.

### Combined

![Fuzzy-skin-Combined-mode](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-Combined-mode.png?raw=true)

This is a combination of Displacement and Extrusion modes. The clarity of the drawing is the same in the classic mode, but the walls remain strong and tight. The load on the kinematics is 2 times lower. The printing speed is faster than in Displacement mode, but the elapsed time will still be longer.

> [!WARNING]
> The limits on line thickness are the same as in the Extrusion mode.

## Noise Type

Select the noise algorithm used to generate the random offsets. Different noise types produce distinct visual textures.

### Classic

Simple uniform random noise. Produces a coarse, irregular texture.

![Fuzzy-skin-classic](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-classic.png?raw=true)

### Perlin

[Perlin noise](https://en.wikipedia.org/wiki/Perlin_noise) generates smooth, natural-looking variations with coherent structure.

![Fuzzy-skin-perlin](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-perlin.png?raw=true)

### Billow

Billow noise is similar to Perlin noise, but has a clumpier appearance. It can create more pronounced features and is often used for natural textures.

![Fuzzy-skin-billow](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-billow.png?raw=true)

### Ridged Multifractal

Creates sharp, jagged features and high-contrast detail. Useful for stone- or marble-like textures.

![Fuzzy-skin-ridged-multifractal](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-ridged-multifractal.png?raw=true)

### Voronoi

[Voronoi noise](https://en.wikipedia.org/wiki/Worley_noise) divides the surface into Voronoi cells and displaces each cell independently, creating a patchwork or cellular texture.

![Fuzzy-skin-voronoi](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/Fuzzy-skin/Fuzzy-skin-voronoi.png?raw=true)

## Point distance

Average distance between random sample points along each line segment.  
Smaller values add more detail and increase computation; larger values produce coarser, faster results.

## Skin thickness

Maximum lateral width (in mm) over which points can be displaced. This defines how far the wall can be jittered.  
Keep this below or near your outer wall line width and within nozzle/flow limits for reliable prints.

## Skin feature size

Base size of coherent noise features, in mm. Larger values yield bigger, more prominent structures; smaller values give fine-grained texture.

## Skin Noise Octaves

The number of octaves of coherent noise to use. Higher values increase the detail of the noise, but also increase computation time.

## Skin Noise Persistence

Controls how amplitude decays across octaves. Lower persistence results in smoother noise; higher persistence keeps finer-scale detail stronger.

## Apply fuzzy skin to first layer

Enable to apply fuzzy skin to the first layer.

> [!CAUTION]
> Can impact bed adhesion and surface contact.

## Credits

- **Generator Mode author:** [@pi-squared-studio](https://github.com/pi-squared-studio).
