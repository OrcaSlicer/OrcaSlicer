# G-code Match & Correctness Analyzer for H2C Printers

This tool is designed to analyze the slicing quality of G-code files generated for Bambu Lab H2C printers and compare the output of Bambu Studio and OrcaSlicer.

## Features

1. **Single File Analysis (Correctness Mode)**:
   - Verifies layer geometry (Z-height alignment).
   - Validates tool retraction values (expects `M620.11 O1 T18` for PLA and `T14` for PETG-CF).
   - Checks pre-cooling hotend addressing (verifies that `M104` cools the parked nozzle instead of the active one).
   - Detects duplicate/conflicting `M104` temperature commands in a single toolchange block.
   - Monitors the pre-cooling temperature floor (fails if pre-cooling drops below 150°C).
   - Tracks purging (flushing) volumes per tool based on `M620.10` commands.
   - Verifies extruder change long retractions (`M620.11 K1 R10` to avoid FTS sensor jams).
   - Validates temperature scheduler command pairing/balancing (`M632`/`M633`).
   - Assesses PETG interface tower oozing compensation configs (`filament_tower_interface_pre_extrusion_length`).

2. **Dual File Comparison (Match Mode)**:
   - Automatically detects logical tool swapping (e.g. BBS T0 $\leftrightarrow$ Orca T4).
   - Compares G-code command distributions (G1, G2, G3, M104, M204, M73, etc.).
   - Matches Z-heights and line counts layer-by-layer.
   - Compares filament consumption by printing feature type (Feature Extrusion Profile).
   - Compares flushing volumes across logically mapped tool channels.
   - Performs command-by-command analysis of logically aligned toolchange blocks.

3. **Print Strength Estimation (Add-on Mode)**:
   - Computes a qualitative and quantitative Print Strength Index (PSI, scale 0-100) per sliced component object.
   - Evaluates:
     - Plastic material profile mechanics (ABS, PETG, PLA, TPU, PA-CF).
     - Extrusion temperature ranges and nozzle limits.
     - Layer height to nozzle diameter ratios.
     - Infill pattern/density structure and perimeter wall loops.
     - Retraction frequencies (checks filament grinding risks).
     - Geometric aspect ratios (tall/slender structural penalties).
     - Preheating/precooling/scheduling sync issues penalty impact.

---

## Print Strength Index (PSI)

The **Print Strength Index (PSI)** is a heuristic engineering metric (scale 0-100) estimating the interlaminar bonding strength and structural durability of sliced components. It is calculated using a multiplicative factor model:

$$\text{PSI} = 100 \times F_{\text{material}} \times F_{\text{temperature}} \times F_{\text{layer}} \times F_{\text{structure}} \times F_{\text{retraction}} \times F_{\text{sync}} \times F_{\text{geometry}}$$

### Evaluation Factors:
- **Material ($F_{\text{material}}$)**: High-bonding plastics boost strength (TPU: `1.4`, PA-CF: `1.25`, PETG: `1.1`), while warp-prone materials (ABS/ASA) are penalized unless printed with an active heated chamber ($\ge 40^\circ\text{C}$).
- **Temperature ($F_{\text{temperature}}$)**: Compares print temperatures against low/high limits. Printing close to the material's maximum temperature improves interlayer fusion, yielding a higher index.
- **Layer Ratio ($F_{\text{layer}}$)**: Analyzes the layer height to nozzle diameter ratio. The mechanical sweet spot is near `0.35`. Tall layer heights (>0.6 ratio) suffer from weak contact area, while very thin layers (<0.2 ratio) increase shearing susceptibility.
- **Structure ($F_{\text{structure}}$)**: Evaluates perimeter wall count and sparse infill settings. Multidirectional patterns (Gyroid, Honeycomb, Cubic) distribute load much better than concentric/rectilinear patterns.
- **Retraction ($F_{\text{retraction}}$)**: High retraction frequencies can grind filament, causing micro-clogs, pressure drops, and weak layer starts.
- **Sync ($F_{\text{sync}}$)**: Applies a 10% penalty per preheating/precooling/scheduler sync error found in the toolchange sequence.
- **Geometry ($F_{\text{geometry}}$)**: Applies a structural penalty for tall and slender column geometries (high $H / W_{\text{min}}$ aspect ratio) due to higher moment bending sensitivity and faster heat dissipation stress.

### Qualitative Ratings:
- **PSI $\ge 90$**: Excellent (Очень высокая)
- **PSI $75 - 89$**: Good (Высокая / Стандартная)
- **PSI $55 - 74$**: Fair (Средняя / Требует осторожности)
- **PSI $< 55$**: Poor (Хрупкая / Высокий риск расслоения)

### Scientific Research & References:
1. **Sun, Q., Rizvi, G. M., Bellehumeur, C. T., & Gu, P. (2008).** *Effect of processing conditions on the bonding quality of FDM polymer filaments.* Rapid Prototyping Journal. [DOI: 10.1108/13552540810862028](https://doi.org/10.1108/13552540810862028) — Establishes the relationship between FDM parameter thermal profiles (cooling rate, nozzle temp) and bond neck-growth/fusion.
2. **Li, L., Sun, Q., Bellehumeur, C., & Gu, P. (2002).** *Investigation of bond formation in three-dimensional printing of polymer parts.* Polymer Engineering & Science. [DOI: 10.1002/pen.11036](https://doi.org/10.1002/pen.11036) — Explains the physical welding process (molecular chain diffusion and thermal sintering) occurring at the interfaces.
3. **Coogan, T. J., & Kazmer, D. O. (2017).** *Bond strength in material extrusion 3D printing.* Rapid Prototyping Journal. [DOI: 10.1108/RPJ-03-2016-0050](https://doi.org/10.1108/RPJ-03-2016-0050) — Analyzes the mechanical properties and anisotropic shear failures of the layers under tension.
4. **McIlroy, C., & Olmsted, P. D. (2017).** *Disentanglement effects on interlayer bonding in 3D printing.* Polymer. [DOI: 10.1016/j.polymer.2017.06.051](https://doi.org/10.1016/j.polymer.2017.06.051) — Details polymer chain disentanglement under high-shear extrusion and its critical impact on interdiffusion at the weld.

---

## Usage

The script is written in pure Python 3 and has no external dependencies.

### 1. Print Report to Console (Default)

If you don't specify the `--to-file` flag, the report is printed directly to the standard output (console):

```bash
# Analyze a single file for correctness
python3 analyzer.py -f1 /path/to/my_slice.gcode

# Compare two slices
python3 analyzer.py -f1 /path/to/10bbl.gcode -f2 /path/to/16orca.gcode
```

### 2. Save Reports to Files (`--to-file`)

When the `--to-file` flag is provided, the tool saves the human-readable text report (`.txt`) and structured statistics (`.json` for agent analysis):

```bash
# Save report for a single file
python3 analyzer.py -f1 /path/to/my_slice.gcode --to-file

# Save comparison report for two files
python3 analyzer.py -f1 /path/to/10bbl.gcode -f2 /path/to/16orca.gcode --to-file
```

### Command Line Arguments
* `-f1` / `--file1`: Path to the first G-code file (required).
* `-f2` / `--file2`: Path to the second G-code file for comparison (optional).
* `--to-file`: Flag to write output to files instead of console (optional).
* `--strength`: Enable print strength estimation for components (optional).
* `-o` / `--output`: Output file prefix (optional, active only when `--to-file` is set; defaults to Desktop or current directory).
